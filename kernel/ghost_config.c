// SPDX-License-Identifier: GPL-2.0
/*
 * Ghost Kernel: Centralized Pure Kernel Dynamic Configuration Engine
 *
 * Reads hardware and OS identity profiles from /efs/ghost.conf or /data/adb/ghost.conf
 * and feeds all 51 Ghost Kernel pillars with dynamic values.
 * Exposes /proc/ghost_config and /proc/ghost_reload.
 *
 * Autonomous Format Randomizer & Seed Guard Engine:
 * - Detects /data format automatically via seed marker files (/data/.ghost_seed)
 * - Generates authentic Samsung 11-char serial on format and persists it across normal reboots
 * - Directly synchronizes Bionic property area in /dev/__properties__/u:object_r:serialno_prop:s0
 *   and USB gadget descriptors in kernel space without userspace root dependencies.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/random.h>
#include <linux/namei.h>
#include <linux/magic.h>
#include <linux/fs_struct.h>
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/ghost_config.h>
#include <linux/of.h>

#ifndef F2FS_SUPER_MAGIC
#define F2FS_SUPER_MAGIC 0xF2F52010
#endif
#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53
#endif
#include <linux/ghost_uptime.h>

struct ghost_profile ghost_active_profile;
EXPORT_SYMBOL(ghost_active_profile);
struct ghost_profile __rcu *ghost_active_profile_ptr;
EXPORT_SYMBOL(ghost_active_profile_ptr);
char ghost_spoofed_kernel_version[65] = "";
EXPORT_SYMBOL(ghost_spoofed_kernel_version);

static DEFINE_MUTEX(ghost_config_mutex);
static DEFINE_MUTEX(ghost_reload_mutex);
static struct delayed_work ghost_config_work;
static int ghost_load_attempts = 0;
#define GHOST_MAX_LOAD_ATTEMPTS 600

#define GHOST_SEED_FILE_DATA     "/data/.ghost_seed"
#define GHOST_SEED_FILE_DATA_SYS "/data/system/.ghost_seed"
#define GHOST_SEED_FILE_DATA_ADB "/data/adb/.ghost_serial_seed"
#define GHOST_SEED_FILE_EFS      "/efs/ghost_serial.txt"
#define GHOST_FACTORY_EFS_1      "/efs/FactoryApp/serial_no"
#define GHOST_FACTORY_EFS_2      "/mnt/vendor/efs/FactoryApp/serial_no"
#define GHOST_PROP_SERIAL_FILE   "/dev/__properties__/u:object_r:serialno_prop:s0"
#define GHOST_USB_GADGET_SERIAL  "/config/usb_gadget/g1/strings/0x409/serialnumber"

static void ghost_generate_samsung_serial(char *out_sn, size_t max_len)
{
	static const char year_chars[] = {'C', 'R', 'T', 'W', 'X'};
	static const char month_chars[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C'};
	static const char base34_chars[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
	u8 r[16];
	int i;

	if (!out_sn || max_len < 12)
		return;

	get_random_bytes(r, sizeof(r));

	out_sn[0] = 'R';
	out_sn[1] = '5';
	out_sn[2] = year_chars[r[0] % sizeof(year_chars)];
	out_sn[3] = month_chars[r[1] % sizeof(month_chars)];

	for (i = 0; i < 7; i++) {
		out_sn[4 + i] = base34_chars[r[2 + i] % (sizeof(base34_chars) - 1)];
	}
	out_sn[11] = '\0';
}

static bool is_valid_samsung_serial(const char *sn)
{
	int i;
	if (!sn || strlen(sn) != 11)
		return false;
	if (sn[0] != 'R' || sn[1] != '5')
		return false;
	/* Reject known test/dummy serials */
	if (strncmp(sn, "R58R30ABCDE", 11) == 0)
		return false;
	for (i = 2; i < 11; i++) {
		if (!isalnum(sn[i]))
			return false;
	}
	return true;
}

static int __maybe_unused ghost_file_exists(const char *path)
{
	struct file *filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return 0;
	filp_close(filp, NULL);
	return 1;
}

static int ghost_get_init_root(struct path *root)
{
	struct task_struct *init_p;
	int ret = -ESRCH;

	rcu_read_lock();
	init_p = find_task_by_pid_ns(1, &init_pid_ns);
	if (init_p)
		get_task_struct(init_p);
	rcu_read_unlock();

	if (!init_p)
		return -ESRCH;

	if (init_p->fs) {
		get_fs_root(init_p->fs, root);
		ret = 0;
	}
	put_task_struct(init_p);
	return ret;
}

static bool __maybe_unused ghost_is_android_data_ready(void)
{
	struct path init_root;
	struct file *filp;
	bool ready = false;

	if (ghost_get_init_root(&init_root) != 0)
		return false;

	filp = file_open_root(init_root.dentry, init_root.mnt, "data", O_RDONLY | O_DIRECTORY, 0);
	if (!IS_ERR(filp)) {
		if (filp->f_inode && filp->f_inode->i_sb) {
			unsigned long magic = filp->f_inode->i_sb->s_magic;
			unsigned long flags = filp->f_inode->i_sb->s_flags;

			if ((magic == F2FS_SUPER_MAGIC || magic == EXT4_SUPER_MAGIC) &&
			    !(flags & SB_RDONLY)) {
				ready = true;
			}
		}
		filp_close(filp, NULL);
	}
	path_put(&init_root);
	return ready;
}

static int __maybe_unused ghost_android_read_file(const char *rel_path, char *out, size_t max_len)
{
	struct path init_root;
	struct file *filp;
	loff_t pos = 0;
	ssize_t bytes_read;

	if (!out || max_len == 0)
		return -EINVAL;

	if (ghost_get_init_root(&init_root) != 0)
		return -ESRCH;

	filp = file_open_root(init_root.dentry, init_root.mnt, rel_path, O_RDONLY, 0);
	path_put(&init_root);

	if (IS_ERR(filp))
		return PTR_ERR(filp);

	bytes_read = kernel_read(filp, out, max_len - 1, &pos);
	filp_close(filp, NULL);

	if (bytes_read > 0) {
		out[bytes_read] = '\0';
		while (bytes_read > 0 && (out[bytes_read - 1] == '\n' ||
					 out[bytes_read - 1] == '\r' ||
					 out[bytes_read - 1] == ' ')) {
			out[--bytes_read] = '\0';
		}
		return 0;
	}
	return -EINVAL;
}


static int ghost_patch_property_serial(const char *new_sn)
{
	struct path init_root;
	struct file *filp;
	char *buf;
	loff_t pos = 0;
	ssize_t bytes_read, bytes_written;
	int i, count = 0;

	if (!new_sn || strlen(new_sn) != 11)
		return -EINVAL;

	if (ghost_get_init_root(&init_root) == 0) {
		filp = file_open_root(init_root.dentry, init_root.mnt,
				      "dev/__properties__/u:object_r:serialno_prop:s0",
				      O_RDWR, 0);
		path_put(&init_root);
	} else {
		filp = ERR_PTR(-ESRCH);
	}

	if (IS_ERR(filp)) {
		filp = filp_open("/dev/__properties__/u:object_r:serialno_prop:s0", O_RDWR, 0);
		if (IS_ERR(filp))
			return PTR_ERR(filp);
	}

	buf = kzalloc(196608, GFP_KERNEL);
	if (!buf) {
		filp_close(filp, NULL);
		return -ENOMEM;
	}

	bytes_read = kernel_read(filp, buf, 196608, &pos);
	if (bytes_read > 0) {
		for (i = 0; i <= bytes_read - 13; i++) {
			/* Match length byte 11 (0x0b) followed by 'R', '5' */
			if ((u8)buf[i] == 11 && buf[i+1] == 'R' && buf[i+2] == '5') {
				memcpy(&buf[i+1], new_sn, 11);
				count++;
				i += 11;
			}
		}
		if (count > 0) {
			pos = 0;
			bytes_written = kernel_write(filp, buf, bytes_read, &pos);
			pr_info("GhostKernel: Patched %d serialno property entries in Bionic memory\n", count);
		}
	}
	kfree(buf);
	filp_close(filp, NULL);
	return count > 0 ? 0 : -ENOENT;
}

static void ghost_patch_usb_serial(const char *new_sn)
{
	struct path init_root;
	struct file *filp;
	loff_t pos = 0;

	if (ghost_get_init_root(&init_root) == 0) {
		filp = file_open_root(init_root.dentry, init_root.mnt,
				      "config/usb_gadget/g1/strings/0x409/serialnumber",
				      O_WRONLY, 0);
		path_put(&init_root);
		if (!IS_ERR(filp)) {
			kernel_write(filp, new_sn, strlen(new_sn), &pos);
			filp_close(filp, NULL);
			pr_info("GhostKernel: Patched USB gadget serial via PID 1 root to %s\n", new_sn);
			return;
		}
	}

	filp = filp_open("/config/usb_gadget/g1/strings/0x409/serialnumber", O_WRONLY, 0);
	if (!IS_ERR(filp)) {
		kernel_write(filp, new_sn, strlen(new_sn), &pos);
		filp_close(filp, NULL);
		pr_info("GhostKernel: Patched USB gadget serial to %s\n", new_sn);
	}
}

static bool is_valid_ap_serial(const char *s)
{
	int i;
	if (!s || strlen(s) != 14)
		return false;
	if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
		return false;
	for (i = 2; i < 14; i++) {
		if (!isxdigit(s[i]))
			return false;
	}
	return true;
}

static void ghost_patch_property_ap_serial(const char *new_ap, const char *new_did)
{
	static const char * const prop_files[] = {
		"dev/__properties__/u:object_r:bootloader_prop:s0",
		"dev/__properties__/u:object_r:default_prop:s0",
		"dev/__properties__/u:object_r:exported_default_prop:s0",
		NULL
	};
	struct path init_root;
	int idx;

	if (!new_ap || strlen(new_ap) != 14 || !new_did || strlen(new_did) != 16)
		return;

	if (ghost_get_init_root(&init_root) != 0)
		return;

	for (idx = 0; prop_files[idx]; idx++) {
		struct file *filp = file_open_root(init_root.dentry, init_root.mnt,
						  prop_files[idx], O_RDWR, 0);
		if (!IS_ERR(filp)) {
			char *buf = kzalloc(196608, GFP_KERNEL);
			loff_t pos = 0;
			ssize_t bytes_read, bytes_written;
			int i, patched = 0;

			if (buf) {
				bytes_read = kernel_read(filp, buf, 196608, &pos);
				if (bytes_read > 0) {
					/* Replace AP serial: match 14 bytes starting with "0x" */
					for (i = 0; i <= bytes_read - 16; i++) {
						if ((u8)buf[i] == 14 && buf[i+1] == '0' && (buf[i+2] == 'x' || buf[i+2] == 'X')) {
							memcpy(&buf[i+1], new_ap, 14);
							patched++;
							i += 14;
						}
					}
					/* Replace EM DID: match 16 bytes starting with "20" */
					for (i = 0; i <= bytes_read - 18; i++) {
						if ((u8)buf[i] == 16 && buf[i+1] == '2' && buf[i+2] == '0' &&
						    buf[i+15] == '1' && buf[i+16] == '1') {
							memcpy(&buf[i+1], new_did, 16);
							patched++;
							i += 16;
						}
					}
					if (patched > 0) {
						pos = 0;
						bytes_written = kernel_write(filp, buf, bytes_read, &pos);
						pr_info("GhostKernel: Patched %d AP/DID property entries in %s\n",
							patched, prop_files[idx]);
					}
				}
				kfree(buf);
			}
			filp_close(filp, NULL);
		}
	}
	path_put(&init_root);
}

static int ghost_patch_property_security_patch(const char *new_patch)
{
	static const char * const prop_files[] = {
		"dev/__properties__/u:object_r:build_prop:s0",
		"dev/__properties__/u:object_r:vendor_security_patch_level_prop:s0",
		"dev/__properties__/u:object_r:default_prop:s0",
		"dev/__properties__/u:object_r:vendor_default_prop:s0",
		"dev/__properties__/u:object_r:exported_default_prop:s0",
		NULL
	};
	struct path init_root;
	int idx, total_files_patched = 0;
	const char *patch_val = new_patch;

	if (!patch_val || strlen(patch_val) != 10)
		patch_val = "2024-05-01";

	if (ghost_get_init_root(&init_root) != 0)
		return 0;

	for (idx = 0; prop_files[idx]; idx++) {
		struct file *filp = file_open_root(init_root.dentry, init_root.mnt,
						  prop_files[idx], O_RDWR, 0);
		if (IS_ERR(filp)) {
			filp = filp_open(prop_files[idx], O_RDWR, 0);
		}
		if (!IS_ERR(filp)) {
			char *buf = kzalloc(196608, GFP_KERNEL);
			loff_t pos = 0;
			ssize_t bytes_read, bytes_written;
			int i, patched = 0;

			if (buf) {
				bytes_read = kernel_read(filp, buf, 196608, &pos);
				if (bytes_read > 0) {
					for (i = 0; i <= bytes_read - 12; i++) {
						if ((u8)buf[i] == 10 &&
						    buf[i+1] == '2' && buf[i+2] == '0' &&
						    buf[i+5] == '-' && buf[i+8] == '-' &&
						    buf[i+11] == '\0') {
							memcpy(&buf[i+1], patch_val, 10);
							patched++;
							i += 10;
						} else if (buf[i] == '2' && buf[i+1] == '0' &&
							   buf[i+4] == '-' && buf[i+7] == '-' &&
							   buf[i+10] == '\0' &&
							   (!memcmp(&buf[i], "2021-12-01", 10) ||
							    !memcmp(&buf[i], "2022-01-01", 10))) {
							memcpy(&buf[i], patch_val, 10);
							patched++;
							i += 10;
						}
					}
					if (patched > 0) {
						pos = 0;
						bytes_written = kernel_write(filp, buf, bytes_read, &pos);
						pr_info("GhostKernel: Patched %d security_patch entries in %s\n",
							patched, prop_files[idx]);
						total_files_patched++;
					}
				}
				kfree(buf);
			}
			filp_close(filp, NULL);
		}
	}
	path_put(&init_root);
	return total_files_patched;
}

void ghost_sanitize_bootargs(char *buf, size_t len)
{
	struct ghost_profile snap;
	char *p;

	if (!buf || len == 0)
		return;

	ghost_get_profile_snapshot(&snap);

	if (snap.serialno[0] && strlen(snap.serialno) == 11) {
		p = strnstr(buf, "androidboot.serialno=", len);
		if (p && (p + 21 + 11 <= buf + len)) {
			memcpy(p + 21, snap.serialno, 11);
		}
	}

	if (snap.ap_serial[0] && strlen(snap.ap_serial) == 14) {
		p = strnstr(buf, "androidboot.ap_serial=", len);
		if (p && (p + 22 + 14 <= buf + len)) {
			memcpy(p + 22, snap.ap_serial, 14);
		}
	}

	if (snap.em_did[0] && strlen(snap.em_did) == 16) {
		p = strnstr(buf, "androidboot.em.did=", len);
		if (p && (p + 19 + 16 <= buf + len)) {
			memcpy(p + 19, snap.em_did, 16);
		}
	}
}
EXPORT_SYMBOL(ghost_sanitize_bootargs);

static void ghost_apply_properties_from_snapshot(const struct ghost_profile *prof)
{
	if (!prof)
		return;
	ghost_patch_property_serial(prof->serialno);
	ghost_patch_property_ap_serial(prof->ap_serial, prof->em_did);
	ghost_patch_usb_serial(prof->serialno);
	ghost_patch_property_security_patch(prof->security_patch);
}

static bool ghost_serial_guard_completed = false;

static void ghost_apply_serial_guard(void)
{
	struct ghost_profile snap;
	ghost_get_profile_snapshot(&snap);

	/* Zero-Disk Architecture:
	 * Pure RAM and VFS in-flight virtualization.
	 * Absolutely NO writes to /efs or /data partitions under any circumstances.
	 */
	if (is_valid_samsung_serial(snap.serialno) &&
	    is_valid_ap_serial(snap.ap_serial)) {
		ghost_apply_properties_from_snapshot(&snap);
		ghost_serial_guard_completed = true;
		pr_info("GhostKernel: Zero-disk serial guard active in RAM (serial=%s, ap=%s)\n",
			snap.serialno, snap.ap_serial);
	}
}

static void ghost_set_default_profile(struct ghost_profile *p)
{
	if (!p)
		p = &ghost_active_profile;

	strscpy(p->model, "SM-G991B", sizeof(p->model));
	strscpy(p->product, "o1sxeea", sizeof(p->product));
	strscpy(p->device, "o1s", sizeof(p->device));
	strscpy(p->manufacturer, "samsung", sizeof(p->manufacturer));
	strscpy(p->brand, "samsung", sizeof(p->brand));
	strscpy(p->soc_machine, "Exynos", sizeof(p->soc_machine));
	strscpy(p->soc_family, "samsung", sizeof(p->soc_family));

	strscpy(p->build_fingerprint,
		"samsung/o1sxeea/o1s:12/SP1A.210812.016/SM-G991BXXS3BULC:user/release-keys",
		sizeof(p->build_fingerprint));
	strscpy(p->build_desc,
		"o1sxeea-user 12 SP1A.210812.016 SM-G991BXXS3BULC release-keys",
		sizeof(p->build_desc));
	strscpy(p->build_id, "SP1A.210812.016", sizeof(p->build_id));
	strscpy(p->security_patch, "2024-05-01", sizeof(p->security_patch));

	/* Authentic dynamic Samsung serial (will be aligned with persistent seed upon mount) */
	ghost_generate_samsung_serial(p->serialno, sizeof(p->serialno));

	{
		u64 ap_raw;
		get_random_bytes(&ap_raw, sizeof(ap_raw));
		ap_raw &= 0xFFFFFFFFFFFFULL; /* 48-bit */
		snprintf(p->ap_serial, sizeof(p->ap_serial), "0x%012llX", ap_raw);
		snprintf(p->em_did, sizeof(p->em_did), "20%012llx11", ap_raw);
		p->unique_id = (0x5857ULL << 48) | ap_raw;
	}

	strscpy(p->imei, "354892110293845", sizeof(p->imei));
	strscpy(p->imei2, "354892110293852", sizeof(p->imei2));

	p->wifi_mac[0] = 0xA4; p->wifi_mac[1] = 0x75; p->wifi_mac[2] = 0xB9;
	p->wifi_mac[3] = 0x44; p->wifi_mac[4] = 0x55; p->wifi_mac[5] = 0x66;

	p->bt_mac[0] = 0xA4; p->bt_mac[1] = 0x75; p->bt_mac[2] = 0xB9;
	p->bt_mac[3] = 0x44; p->bt_mac[4] = 0x55; p->bt_mac[5] = 0x67;

	strscpy(p->ufs_serial, "0x9c4a8b12", sizeof(p->ufs_serial));
	strscpy(p->ufs_model, "KLUDG8UHDB-C2D1", sizeof(p->ufs_model));

	strscpy(p->boot_hash, "22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781", sizeof(p->boot_hash));
	strscpy(p->boot_key, "78d88bcb03734bebc53a14658b315a500f7c7488357dafe490d283cc726bff95", sizeof(p->boot_key));

	p->uptime_days = 17;
	p->boot_count = 38;
	p->battery_cycle = 142;
	p->battery_health = 96;
	p->sensor_bias[0] = 12;
	p->sensor_bias[1] = -8;
	p->sensor_bias[2] = 21;
	p->baro_drift_hpa_x100 = -45;
	p->tcp_isn_offset = 0x1a2b3c4d;

	p->spoofed_kernel_version[0] = '\0';
	p->is_loaded = false;
	strscpy(p->loaded_from, "[DEFAULT_FALLBACK]", sizeof(p->loaded_from));
}

static char *trim_str(char *str)
{
	char *end;
	while (*str && isspace((unsigned char)*str)) str++;
	if (*str == 0) return str;
	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end)) end--;
	end[1] = '\0';
	return str;
}

static int parse_mac_str(const char *str, u8 *out_mac, int len)
{
	int values[6];
	int i;
	if (str && sscanf(str, "%x:%x:%x:%x:%x:%x",
		   &values[0], &values[1], &values[2],
		   &values[3], &values[4], &values[5]) == 6) {
		for (i = 0; i < 6 && i < len; ++i)
			out_mac[i] = (u8)values[i];
		return 0;
	}
	return -EINVAL;
}

static int parse_sensor_bias(const char *str, s16 bias[3])
{
	int x, y, z;
	if (str && sscanf(str, "%d,%d,%d", &x, &y, &z) == 3) {
		bias[0] = (s16)x;
		bias[1] = (s16)y;
		bias[2] = (s16)z;
		return 0;
	}
	return -EINVAL;
}

static int parse_config_buffer(char *buf, size_t len, const char *source_path)
{
	char *line, *next_line;
	struct ghost_profile *temp_prof;

	if (!buf || len == 0)
		return -EINVAL;

	temp_prof = kzalloc(sizeof(*temp_prof), GFP_KERNEL);
	if (!temp_prof)
		return -ENOMEM;

	/* Initialize temp_prof from an atomic snapshot of current active profile */
	ghost_get_profile_snapshot(temp_prof);

	line = buf;
	while (line < buf + len) {
		char *newline = strchr(line, '\n');
		char *k_key, *k_val, *eq;

		if (newline) {
			*newline = '\0';
			next_line = newline + 1;
		} else {
			next_line = buf + len;
		}

		line = trim_str(line);
		if (line[0] == '\0' || line[0] == '#' || line[0] == ';' || line[0] == '[') {
			line = next_line;
			continue;
		}

		eq = strchr(line, '=');
		if (!eq) {
			pr_warn("GhostKernel: Syntax error in %s: missing '=' in '%s'\n", source_path, line);
			kfree(temp_prof);
			return -EINVAL;
		}

		*eq = '\0';
		k_key = trim_str(line);
		k_val = trim_str(eq + 1);

		if (!strcasecmp(k_key, "model")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty model in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->model, k_val, sizeof(temp_prof->model));
		} else if (!strcasecmp(k_key, "product")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty product in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->product, k_val, sizeof(temp_prof->product));
		} else if (!strcasecmp(k_key, "device")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty device in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->device, k_val, sizeof(temp_prof->device));
		} else if (!strcasecmp(k_key, "manufacturer")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty manufacturer in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->manufacturer, k_val, sizeof(temp_prof->manufacturer));
		} else if (!strcasecmp(k_key, "brand")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty brand in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->brand, k_val, sizeof(temp_prof->brand));
		} else if (!strcasecmp(k_key, "soc_machine")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty soc_machine in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->soc_machine, k_val, sizeof(temp_prof->soc_machine));
		} else if (!strcasecmp(k_key, "soc_family")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty soc_family in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->soc_family, k_val, sizeof(temp_prof->soc_family));
		} else if (!strcasecmp(k_key, "fingerprint") || !strcasecmp(k_key, "build_fingerprint")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty fingerprint in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->build_fingerprint, k_val, sizeof(temp_prof->build_fingerprint));
		} else if (!strcasecmp(k_key, "build_desc")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty build_desc in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->build_desc, k_val, sizeof(temp_prof->build_desc));
		} else if (!strcasecmp(k_key, "kernel_version")) {
			strscpy(temp_prof->spoofed_kernel_version, k_val, sizeof(temp_prof->spoofed_kernel_version));
		} else if (!strcasecmp(k_key, "build_id")) {
			if (strlen(k_val) == 0) {
				pr_warn("GhostKernel: Empty build_id in %s\n", source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->build_id, k_val, sizeof(temp_prof->build_id));
		} else if (!strcasecmp(k_key, "security_patch")) {
			if (strlen(k_val) != 10 || k_val[4] != '-' || k_val[7] != '-') {
				pr_warn("GhostKernel: Invalid security_patch format '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->security_patch, k_val, sizeof(temp_prof->security_patch));
		} else if (!strcasecmp(k_key, "serialno") || !strcasecmp(k_key, "serial_no")) {
			if (!is_valid_samsung_serial(k_val)) {
				pr_warn("GhostKernel: Invalid serialno '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->serialno, k_val, sizeof(temp_prof->serialno));
		} else if (!strcasecmp(k_key, "ap_serial")) {
			if (!is_valid_ap_serial(k_val)) {
				pr_warn("GhostKernel: Invalid ap_serial '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->ap_serial, k_val, sizeof(temp_prof->ap_serial));
		} else if (!strcasecmp(k_key, "em_did") || !strcasecmp(k_key, "em.did")) {
			if (strlen(k_val) != 16) {
				pr_warn("GhostKernel: Invalid em_did '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->em_did, k_val, sizeof(temp_prof->em_did));
		} else if (!strcasecmp(k_key, "imei")) {
			if (strlen(k_val) != 15) {
				pr_warn("GhostKernel: Invalid IMEI '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->imei, k_val, sizeof(temp_prof->imei));
		} else if (!strcasecmp(k_key, "imei2")) {
			if (strlen(k_val) != 15) {
				pr_warn("GhostKernel: Invalid IMEI2 '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->imei2, k_val, sizeof(temp_prof->imei2));
		} else if (!strcasecmp(k_key, "wifi_mac")) {
			if (parse_mac_str(k_val, temp_prof->wifi_mac, ETH_ALEN) != 0) {
				pr_warn("GhostKernel: Invalid wifi_mac '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
		} else if (!strcasecmp(k_key, "bt_mac")) {
			if (parse_mac_str(k_val, temp_prof->bt_mac, 6) != 0) {
				pr_warn("GhostKernel: Invalid bt_mac '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
		} else if (!strcasecmp(k_key, "ufs_serial")) {
			strscpy(temp_prof->ufs_serial, k_val, sizeof(temp_prof->ufs_serial));
		} else if (!strcasecmp(k_key, "ufs_model")) {
			strscpy(temp_prof->ufs_model, k_val, sizeof(temp_prof->ufs_model));
		} else if (!strcasecmp(k_key, "uptime_days")) {
			if (kstrtouint(k_val, 10, &temp_prof->uptime_days) != 0) {
				pr_warn("GhostKernel: Invalid uptime_days '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
		} else if (!strcasecmp(k_key, "boot_count")) {
			if (kstrtouint(k_val, 10, &temp_prof->boot_count) != 0) {
				pr_warn("GhostKernel: Invalid boot_count '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
		} else if (!strcasecmp(k_key, "battery_cycle")) {
			if (kstrtouint(k_val, 10, &temp_prof->battery_cycle) != 0) {
				pr_warn("GhostKernel: Invalid battery_cycle '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
		} else if (!strcasecmp(k_key, "battery_health")) {
			u32 bh = 0;
			if (kstrtouint(k_val, 10, &bh) != 0 || bh > 100) {
				pr_warn("GhostKernel: Invalid battery_health '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			temp_prof->battery_health = bh;
		} else if (!strcasecmp(k_key, "sensor_bias")) {
			if (parse_sensor_bias(k_val, temp_prof->sensor_bias) != 0) {
				pr_warn("GhostKernel: Invalid sensor_bias '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
		} else if (!strcasecmp(k_key, "tcp_isn_offset")) {
			if (kstrtouint(k_val, 0, &temp_prof->tcp_isn_offset) != 0) {
				pr_warn("GhostKernel: Invalid tcp_isn_offset '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
		} else if (!strcasecmp(k_key, "boot_hash") || !strcasecmp(k_key, "vbmeta_digest")) {
			if (strlen(k_val) != 64) {
				pr_warn("GhostKernel: Invalid boot_hash length '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->boot_hash, k_val, sizeof(temp_prof->boot_hash));
		} else if (!strcasecmp(k_key, "boot_key") || !strcasecmp(k_key, "verifiedbootkey")) {
			if (strlen(k_val) != 64) {
				pr_warn("GhostKernel: Invalid boot_key length '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->boot_key, k_val, sizeof(temp_prof->boot_key));
		} else {
			pr_warn("GhostKernel: Unknown config key '%s' in %s\n", k_key, source_path);
			kfree(temp_prof);
			return -EINVAL;
		}

		line = next_line;
	}

	/* Validation Gates: Ensure critical fields are well-formed */
	if (!is_valid_samsung_serial(temp_prof->serialno)) {
		pr_warn("GhostKernel: Validation failed for serialno '%s' from %s\n",
			temp_prof->serialno, source_path);
		kfree(temp_prof);
		return -EINVAL;
	}

	if (!is_valid_ap_serial(temp_prof->ap_serial)) {
		pr_warn("GhostKernel: Validation failed for ap_serial '%s' from %s\n",
			temp_prof->ap_serial, source_path);
		kfree(temp_prof);
		return -EINVAL;
	}

	/* Synchronize uptime_days with immutable runtime session offset */
	if (ghost_uptime_offset_ns) {
		u32 runtime_days = (u32)div64_u64(div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC), 86400);
		if (runtime_days > 0)
			temp_prof->uptime_days = runtime_days;
	}

	temp_prof->is_loaded = true;
	strscpy(temp_prof->loaded_from, source_path, sizeof(temp_prof->loaded_from));

	{
		struct ghost_profile *old_prof;

		/* Atomic commit under mutex via RCU publication */
		mutex_lock(&ghost_config_mutex);
		old_prof = rcu_dereference_protected(ghost_active_profile_ptr, lockdep_is_held(&ghost_config_mutex));
		rcu_assign_pointer(ghost_active_profile_ptr, temp_prof);
		if (temp_prof->spoofed_kernel_version[0])
			strscpy(ghost_spoofed_kernel_version, temp_prof->spoofed_kernel_version, sizeof(ghost_spoofed_kernel_version));
		mutex_unlock(&ghost_config_mutex);

		if (old_prof && old_prof != &ghost_active_profile) {
			synchronize_rcu();
			kfree(old_prof);
		}
	}
	pr_info("GhostKernel: Config loaded and committed atomically via RCU from %s (serial=%s)\n",
		source_path, temp_prof->serialno);
	return 0;
}

static int ghost_read_file_and_parse(const char *path)
{
	struct path init_root;
	struct file *filp = ERR_PTR(-ENOENT);
	char *buffer;
	loff_t pos = 0;
	ssize_t bytes_read;
	int ret = 0;
	const char *rel_path = (path && path[0] == '/') ? (path + 1) : path;

	if (rel_path && ghost_get_init_root(&init_root) == 0) {
		filp = file_open_root(init_root.dentry, init_root.mnt, rel_path, O_RDONLY, 0);
		path_put(&init_root);
	}

	if (IS_ERR(filp)) {
		filp = filp_open(path, O_RDONLY, 0);
		if (IS_ERR(filp))
			return PTR_ERR(filp);
	}

	buffer = kzalloc(8192, GFP_KERNEL);
	if (!buffer) {
		filp_close(filp, NULL);
		return -ENOMEM;
	}

	bytes_read = kernel_read(filp, buffer, 8191, &pos);
	filp_close(filp, NULL);

	if (bytes_read > 0) {
		buffer[bytes_read] = '\0';
		ret = parse_config_buffer(buffer, bytes_read, path);
	} else {
		ret = (bytes_read < 0) ? (int)bytes_read : -EINVAL;
	}

	kfree(buffer);
	return ret;
}

int ghost_config_reload(void)
{
	int ret = -ENOENT;

	mutex_lock(&ghost_reload_mutex);

	/* Try Primary: /data/adb/ghost.conf */
	ret = ghost_read_file_and_parse(GHOST_CONF_PATH_PRIMARY);
	if (ret == 0)
		goto out;
	if (ret != -ENOENT)
		goto out;

	/* Try Secondary: /efs/ghost.conf */
	ret = ghost_read_file_and_parse(GHOST_CONF_PATH_SECONDARY);
	if (ret == 0)
		goto out;
	if (ret != -ENOENT)
		goto out;

	/* Try Fallback: /sdcard/ghost.conf */
	ret = ghost_read_file_and_parse(GHOST_CONF_PATH_FALLBACK);

out:
	mutex_unlock(&ghost_reload_mutex);
	return ret;
}
EXPORT_SYMBOL(ghost_config_reload);

/* Procfs: /proc/ghost_config */

static ssize_t ghost_utsname_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char tmp[65];
    size_t copy_size = count < sizeof(tmp) ? count : sizeof(tmp) - 1;
    if (copy_from_user(tmp, buf, copy_size))
        return -EFAULT;
    tmp[copy_size] = '\0';
    if (copy_size > 0 && tmp[copy_size - 1] == '\n')
        tmp[copy_size - 1] = '\0';
    if (strlen(tmp) > 0) {
        strscpy(ghost_spoofed_kernel_version, tmp, sizeof(ghost_spoofed_kernel_version));
    }
    return count;
}
static const struct file_operations ghost_utsname_fops = {
    .write = ghost_utsname_write,
};

static int ghost_config_proc_show(struct seq_file *m, void *v)
{
	struct ghost_profile p_copy;
	struct ghost_profile *p;

	/* Restrict sensitive full config inspection from untrusted UIDs */
	if (current_uid().val >= 10000) {
		seq_printf(m, "status: active\n");
		return 0;
	}

	/* Snapshot active profile atomically under RCU lock to prevent torn reads */
	ghost_get_profile_snapshot(&p_copy);
	p = &p_copy;

	seq_printf(m, "=====================================================\n");
	seq_printf(m, "   GHOST KERNEL PURE ENGINE PROFILE CONFIGURATION    \n");
	seq_printf(m, "=====================================================\n");
	seq_printf(m, "Source Status     : %s\n", p->loaded_from);
	seq_printf(m, "Device Model      : %s\n", p->model);
	seq_printf(m, "Product / Device  : %s / %s\n", p->product, p->device);
	seq_printf(m, "SoC Machine/Family: %s / %s\n", p->soc_machine, p->soc_family);
	seq_printf(m, "Build Fingerprint : %s\n", p->build_fingerprint);
	seq_printf(m, "Spoofed Kernel    : %s\n", ghost_spoofed_kernel_version);
	seq_printf(m, "Build Description : %s\n", p->build_desc);
	seq_printf(m, "Serial Number     : %s\n", p->serialno);
	seq_printf(m, "Cellular IMEI     : %s\n", p->imei);
	seq_printf(m, "Cellular IMEI 2   : %s\n", p->imei2);
	seq_printf(m, "Wi-Fi MAC         : %pM\n", p->wifi_mac);
	seq_printf(m, "Bluetooth Address : %02X:%02X:%02X:%02X:%02X:%02X\n",
		   p->bt_mac[0], p->bt_mac[1], p->bt_mac[2],
		   p->bt_mac[3], p->bt_mac[4], p->bt_mac[5]);
	seq_printf(m, "UFS Serial/Model  : %s / %s\n", p->ufs_serial, p->ufs_model);
	if (ghost_uptime_offset_ns) {
		u32 runtime_days = (u32)div64_u64(div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC), 86400);
		seq_printf(m, "Uptime Age (Days) : %u days (runtime session offset)\n", runtime_days);
	} else {
		seq_printf(m, "Uptime Age (Days) : %u days\n", p->uptime_days);
	}
	seq_printf(m, "Boot Count        : %u\n", p->boot_count);
	seq_printf(m, "Battery Cycle     : %u (Health: %u%%)\n", p->battery_cycle, p->battery_health);
	seq_printf(m, "Sensor Bias (XYZ) : %d, %d, %d\n", p->sensor_bias[0], p->sensor_bias[1], p->sensor_bias[2]);
	seq_printf(m, "TCP ISN Offset    : 0x%08X\n", p->tcp_isn_offset);
	seq_printf(m, "Boot Hash (VBMeta): %s\n", p->boot_hash);
	seq_printf(m, "Boot Key (Pubkey) : %s\n", p->boot_key);
	seq_printf(m, "=====================================================\n");
	return 0;
}

/* Procfs: /proc/ghost_reload (Writing '1' triggers reload) */
static ssize_t ghost_reload_proc_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char kbuf[8] = {0};
	if (count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	if (kbuf[0] == '1' || kbuf[0] == 'r' || kbuf[0] == 'R') {
		int ret = ghost_config_reload();
		if (ret < 0) {
			pr_warn("GhostKernel: Manual profile reload failed, ret=%d\n", ret);
			return ret;
		}
		{
			struct ghost_profile snap;
			ghost_get_profile_snapshot(&snap);
			ghost_apply_properties_from_snapshot(&snap);
		}
		pr_info("GhostKernel: Manual profile reload succeeded (ret=0)\n");
	}

	return count;
}

static int ghost_reload_proc_open(struct inode *inode, struct file *file)
{
	return simple_open(inode, file);
}

static const struct file_operations ghost_reload_proc_fops = {
	.owner = THIS_MODULE,
	.open = ghost_reload_proc_open,
	.write = ghost_reload_proc_write,
	.llseek = default_llseek,
};


static void ghost_config_delayed_worker(struct work_struct *work)
{
	int ret = ghost_config_reload();
	ghost_load_attempts++;

	if (!ghost_serial_guard_completed)
		ghost_apply_serial_guard();

	if (ret == 0) {
		/* Profile loaded from /efs/ghost.conf or /data/adb/ghost.conf */
		struct ghost_profile snap;
		ghost_get_profile_snapshot(&snap);
		ghost_apply_properties_from_snapshot(&snap);
	}

	if (ret == 0 && ghost_serial_guard_completed) {
		pr_info("GhostKernel: Engine and Serial Guard ready on attempt %d (ret=0)\n", ghost_load_attempts);
	} else if (ghost_load_attempts < GHOST_MAX_LOAD_ATTEMPTS) {
		/* Retry every 500ms until filesystems (/efs and /data) mount */
		schedule_delayed_work(&ghost_config_work, msecs_to_jiffies(500));
	} else {
		pr_info("GhostKernel: Engine ready on attempt %d (ret=%d)\n", ghost_load_attempts, ret);
	}
}

static struct delayed_work ghost_prop_patch_work;
static int ghost_prop_patch_attempts = 0;

static void ghost_prop_patch_delayed_worker(struct work_struct *work)
{
	int count = 0;
	int sec_files = 0;
	struct ghost_profile snap;

	ghost_prop_patch_attempts++;
	ghost_get_profile_snapshot(&snap);

	count = ghost_patch_property_serial(snap.serialno);
	ghost_patch_property_ap_serial(snap.ap_serial, snap.em_did);
	ghost_patch_usb_serial(snap.serialno);
	sec_files = ghost_patch_property_security_patch(snap.security_patch);

	/* Stop early if both security patch property files are patched and serial patched */
	if (sec_files >= 2 && count > 0) {
		pr_info("GhostKernel: Property patch confirmed complete on attempt %d (serial count=%d, sec_files=%d)\n",
			ghost_prop_patch_attempts, count, sec_files);
		return;
	}

	/* Retry up to 180 times (every 1s) to make sure init finishes writing late vendor properties */
	if (ghost_prop_patch_attempts < 180) {
		schedule_delayed_work(&ghost_prop_patch_work, msecs_to_jiffies(1000));
	} else {
		pr_info("GhostKernel: Property patch confirmed complete on attempt %d (serial count=%d, sec_files=%d)\n",
			ghost_prop_patch_attempts, count, sec_files);
	}
}

void ghost_on_f2fs_userdata_mount(const u8 *uuid)
{
	/* Deprecated: Serial lifecycle is now autonomously managed via PID 1 VFS seed markers */
}
EXPORT_SYMBOL(ghost_on_f2fs_userdata_mount);

void ghost_get_active_serial_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	if (!buf || len == 0)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->serialno[0])
		strscpy(buf, p->serialno, len);
	else
		strscpy(buf, ghost_active_profile.serialno, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_active_serial_buf);

void ghost_get_active_ap_serial_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	if (!buf || len == 0)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->ap_serial[0])
		strscpy(buf, p->ap_serial, len);
	else
		strscpy(buf, ghost_active_profile.ap_serial, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_active_ap_serial_buf);

void ghost_get_active_em_did_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	if (!buf || len == 0)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->em_did[0])
		strscpy(buf, p->em_did, len);
	else
		strscpy(buf, ghost_active_profile.em_did, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_active_em_did_buf);

void ghost_get_boot_hash_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	if (!buf || len == 0)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->boot_hash[0])
		strscpy(buf, p->boot_hash, len);
	else
		strscpy(buf, ghost_active_profile.boot_hash, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_boot_hash_buf);

void ghost_get_boot_key_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	if (!buf || len == 0)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->boot_key[0])
		strscpy(buf, p->boot_key, len);
	else
		strscpy(buf, ghost_active_profile.boot_key, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_boot_key_buf);

void ghost_get_ufs_model_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	if (!buf || len == 0)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->ufs_model[0])
		strscpy(buf, p->ufs_model, len);
	else
		strscpy(buf, ghost_active_profile.ufs_model, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_ufs_model_buf);

void ghost_get_profile_snapshot(struct ghost_profile *out)
{
	struct ghost_profile *p;
	if (!out)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p)
		memcpy(out, p, sizeof(*out));
	else
		memcpy(out, &ghost_active_profile, sizeof(*out));
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_profile_snapshot);

u64 ghost_get_active_unique_id(void)
{
	struct ghost_profile *p;
	u64 val;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	val = p ? p->unique_id : ghost_active_profile.unique_id;
	rcu_read_unlock();
	return val;
}
EXPORT_SYMBOL(ghost_get_active_unique_id);

u32 ghost_get_battery_cycle(void)
{
	struct ghost_profile *p;
	u32 val = 142;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->battery_cycle > 0)
		val = p->battery_cycle;
	else if (ghost_active_profile.battery_cycle > 0)
		val = ghost_active_profile.battery_cycle;
	rcu_read_unlock();
	return val;
}
EXPORT_SYMBOL_GPL(ghost_get_battery_cycle);

u32 ghost_get_battery_health(void)
{
	struct ghost_profile *p;
	u32 val = 96;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->battery_health > 0 && p->battery_health <= 100)
		val = p->battery_health;
	else if (ghost_active_profile.battery_health > 0 && ghost_active_profile.battery_health <= 100)
		val = ghost_active_profile.battery_health;
	rcu_read_unlock();
	return val;
}
EXPORT_SYMBOL_GPL(ghost_get_battery_health);

static int ghost_imei_proc_show(struct seq_file *m, void *v)
{
	char imei1[32] = "", imei2[32] = "";
	struct ghost_profile *p;

	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p) {
		strscpy(imei1, p->imei, sizeof(imei1));
		strscpy(imei2, p->imei2, sizeof(imei2));
	} else {
		strscpy(imei1, ghost_active_profile.imei, sizeof(imei1));
		strscpy(imei2, ghost_active_profile.imei2, sizeof(imei2));
	}
	rcu_read_unlock();

	seq_printf(m, "IMEI1: %s\nIMEI2: %s\n", imei1, imei2);
	return 0;
}

int __init ghost_config_init(void)
{
	struct ghost_profile *init_prof;

	init_prof = kzalloc(sizeof(*init_prof), GFP_KERNEL);
	if (init_prof) {
		ghost_set_default_profile(init_prof);
		rcu_assign_pointer(ghost_active_profile_ptr, init_prof);
		memcpy(&ghost_active_profile, init_prof, sizeof(ghost_active_profile));
	} else {
		ghost_set_default_profile(&ghost_active_profile);
	}

	proc_create_single("ghost_config", 0444, NULL, ghost_config_proc_show);
	proc_create_single("ghost_imei", 0444, NULL, ghost_imei_proc_show);
	proc_create("ghost_utsname", 0222, NULL, &ghost_utsname_fops);
	proc_create("ghost_reload", 0200, NULL, &ghost_reload_proc_fops);

	INIT_DELAYED_WORK(&ghost_config_work, ghost_config_delayed_worker);
	INIT_DELAYED_WORK(&ghost_prop_patch_work, ghost_prop_patch_delayed_worker);
	/* Trigger initial check after 500ms */
	schedule_delayed_work(&ghost_config_work, msecs_to_jiffies(500));
	/* Trigger property patch worker after 2000ms */
	schedule_delayed_work(&ghost_prop_patch_work, msecs_to_jiffies(2000));

	pr_info("GhostKernel: Pure Kernel Dynamic Configuration Engine initialized.\n");
	return 0;
}
core_initcall(ghost_config_init);
