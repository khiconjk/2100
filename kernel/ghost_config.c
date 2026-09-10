// SPDX-License-Identifier: GPL-2.0
/*
 * Ghost Kernel: Centralized Pure Kernel Dynamic Configuration Engine
 *
 * Reads hardware and OS identity profiles from /efs/ghost.conf or /data/adb/ghost.conf
 * and feeds all 51 Ghost Kernel pillars with dynamic values.
 * Exposes /proc/ghost_config and /proc/ghost_reload.
 *
 * Reset-epoch identity engine:
 * - Unique IDs derive from F2FS userdata UUID mixed with a compile salt.
 * - Factory reset formats userdata (new UUID) => new device; normal reboot keeps UUID.
 * - ghost.conf is SKU-only in default epoch mode; unique keys apply only if identity_mode=pinned
 *   and the file is /data/adb/ghost.conf. /efs/ghost.conf never pins unique IDs.
 * - Zero disk writes: no /data/.ghost_seed and no Ghost writes to EFS.
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
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <asm/cacheflush.h>
#include <linux/cred.h>
#include <linux/etherdevice.h>
#include <linux/mm.h>

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

/* Fixed salt: identity must change only with userdata UUID, not with every kernel flash. */
#define GHOST_COMPILE_SALT        0x47524F5354313031ULL
#define GHOST_IDENTITY_EPOCH      0
#define GHOST_IDENTITY_PINNED     1

static int ghost_identity_mode = GHOST_IDENTITY_EPOCH;
static u8 ghost_userdata_uuid[16];
static bool ghost_userdata_uuid_valid;

static u64 ghost_mix64(u64 x, u64 salt)
{
	x ^= salt + 0x9E3779B97F4A7C15ULL;
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

static u64 ghost_seed_from_uuid(const u8 *uuid)
{
	u64 s = GHOST_COMPILE_SALT;
	int i;

	if (!uuid)
		return s ? s : 1ULL;
	for (i = 0; i < 16; i++)
		s = ghost_mix64(s ^ ((u64)uuid[i] << ((i & 7) * 8)), 0x55ULL + (u64)i);
	if (!s)
		s = 1ULL;
	return s;
}

static void ghost_generate_samsung_serial(char *out_sn, size_t max_len, u64 seed)
{
	static const char year_chars[] = {'C', 'R', 'T', 'W', 'X'};
	static const char month_chars[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C'};
	static const char base34_chars[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
	u8 r[16];
	u64 s1, s2;
	int i;

	if (!out_sn || max_len < 12)
		return;

	s1 = ghost_mix64(seed, 0x534E000000000001ULL);
	s2 = ghost_mix64(seed, 0x534E000000000002ULL);
	for (i = 0; i < 8; i++)
		r[i] = (u8)(s1 >> (i * 8));
	for (i = 0; i < 8; i++)
		r[8 + i] = (u8)(s2 >> (i * 8));

	out_sn[0] = 'R';
	out_sn[1] = '5';
	out_sn[2] = year_chars[r[0] % sizeof(year_chars)];
	out_sn[3] = month_chars[r[1] % sizeof(month_chars)];
	for (i = 0; i < 7; i++)
		out_sn[4 + i] = base34_chars[r[2 + i] % (sizeof(base34_chars) - 1)];
	out_sn[11] = '\0';
}

static char ghost_imei_luhn_digit(const char *d14)
{
	int sum = 0;
	int i;
	int d;

	for (i = 0; i < 14; i++) {
		d = d14[i] - '0';
		if (d < 0 || d > 9)
			d = 0;
		if (i & 1) {
			d *= 2;
			if (d > 9)
				d -= 9;
		}
		sum += d;
	}
	return (char)('0' + ((10 - (sum % 10)) % 10));
}

static void ghost_format_imei(char *out, size_t len, const char *tac6, u64 src)
{
	char body[16];
	u32 serial8;

	if (!out || len < 16 || !tac6)
		return;
	serial8 = (u32)(src % 100000000ULL);
	memset(body, 0, sizeof(body));
	snprintf(body, 15, "%s%08u", tac6, serial8);
	body[14] = ghost_imei_luhn_digit(body);
	body[15] = '\0';
	strscpy(out, body, len);
}

static char ghost_luhn_from_right(const char *payload, int len)
{
	int sum = 0;
	int i;
	int d;

	if (!payload || len <= 0)
		return '0';
	for (i = 0; i < len; i++) {
		d = payload[len - 1 - i] - '0';
		if (d < 0 || d > 9)
			d = 0;
		if ((i & 1) == 0) {
			d *= 2;
			if (d > 9)
				d -= 9;
		}
		sum += d;
	}
	return (char)('0' + ((10 - (sum % 10)) % 10));
}

static void ghost_put_dec(char *dst, int n, u64 v)
{
	int i;

	if (!dst || n <= 0)
		return;
	for (i = n - 1; i >= 0; i--) {
		dst[i] = (char)('0' + (int)(v % 10ULL));
		v /= 10ULL;
	}
}

static void ghost_format_imsi(char *out, size_t len, const char *mccmnc5, u64 src)
{
	char body[16];

	if (!out || len < 16 || !mccmnc5)
		return;
	memset(body, 0, sizeof(body));
	memcpy(body, mccmnc5, 5);
	ghost_put_dec(body + 5, 10, src % 10000000000ULL);
	body[15] = '\0';
	strscpy(out, body, len);
}

static void ghost_format_iccid(char *out, size_t len, const char *prefix6, u64 src)
{
	char body[24];
	u64 mid;

	if (!out || len < 21 || !prefix6)
		return;
	mid = src % 10000000000000ULL;
	if (!mid)
		mid = 1;
	memset(body, 0, sizeof(body));
	memcpy(body, prefix6, 6);
	ghost_put_dec(body + 6, 13, mid);
	body[19] = ghost_luhn_from_right(body, 19);
	body[20] = '\0';
	strscpy(out, body, len);
}

static void ghost_fit_iccid_len(char *out, size_t olen, const char *src, int dstlen)
{
	char body[24];
	u64 mid;
	int i;

	if (!out || !src || dstlen < 19 || dstlen > 20 || olen < (size_t)(dstlen + 1))
		return;
	if ((int)strlen(src) == dstlen) {
		strscpy(out, src, olen);
		return;
	}
	memset(body, '0', sizeof(body));
	for (i = 0; i < 6 && src[i]; i++)
		body[i] = src[i];
	mid = 0;
	for (i = 6; src[i] >= '0' && src[i] <= '9'; i++)
		mid = mid * 10ULL + (u64)(src[i] - '0');
	if (!mid)
		mid = 1;
	ghost_put_dec(body + 6, dstlen - 7, mid);
	body[dstlen - 1] = ghost_luhn_from_right(body, dstlen - 1);
	body[dstlen] = '\0';
	strscpy(out, body, olen);
}

static void ghost_fill_unique_from_seed(struct ghost_profile *p, u64 seed)
{
	u64 ap, macmix, ufsmix, batmix, agemix, senmix;
	int i;

	if (!p)
		return;

	ghost_generate_samsung_serial(p->serialno, sizeof(p->serialno), seed);

	ap = ghost_mix64(seed, 0xA15EULL) & 0xFFFFFFFFFFFFULL;
	if (!ap)
		ap = 1ULL;
	snprintf(p->ap_serial, sizeof(p->ap_serial), "0x%012llX", ap);
	snprintf(p->em_did, sizeof(p->em_did), "20%012llx11", ap);
	p->unique_id = (0x5857ULL << 48) | ap;

	ghost_format_imei(p->imei, sizeof(p->imei), "358446",
			  ghost_mix64(seed, 0x494D4531ULL));
	ghost_format_imei(p->imei2, sizeof(p->imei2), "359067",
			  ghost_mix64(seed, 0x494D4532ULL));
	ghost_format_imsi(p->imsi, sizeof(p->imsi), "45204",
			  ghost_mix64(seed, 0x494D5331ULL));
	ghost_format_imsi(p->imsi2, sizeof(p->imsi2), "45204",
			  ghost_mix64(seed, 0x494D5332ULL));
	ghost_format_iccid(p->iccid, sizeof(p->iccid), "898404",
			   ghost_mix64(seed, 0x49434331ULL));
	ghost_format_iccid(p->iccid2, sizeof(p->iccid2), "898404",
			   ghost_mix64(seed, 0x49434332ULL));

	macmix = ghost_mix64(seed, 0x4D4143ULL);
	p->wifi_mac[0] = 0x24;
	p->wifi_mac[1] = 0x4B;
	p->wifi_mac[2] = 0xFE;
	p->wifi_mac[3] = (u8)(macmix >> 16);
	p->wifi_mac[4] = (u8)(macmix >> 8);
	p->wifi_mac[5] = (u8)macmix;
	if (!p->wifi_mac[3] && !p->wifi_mac[4] && !p->wifi_mac[5])
		p->wifi_mac[5] = 1;
	memcpy(p->bt_mac, p->wifi_mac, 6);
	for (i = 5; i >= 3; i--) {
		p->bt_mac[i]++;
		if (p->bt_mac[i] != 0)
			break;
	}

	ufsmix = ghost_mix64(seed, 0x554653ULL) & 0xFFFFFFFFULL;
	if (!ufsmix)
		ufsmix = 1ULL;
	snprintf(p->ufs_serial, sizeof(p->ufs_serial), "0x%08llx", ufsmix);

	batmix = ghost_mix64(seed, 0xBA77000000000001ULL);
	p->battery_cycle = 80 + (u32)(batmix % 141ULL);
	p->battery_health = 94 + (u32)(ghost_mix64(seed, 0x4EA1ULL) % 5ULL);

	agemix = ghost_mix64(seed, 0xA6E0000000000001ULL);
	p->uptime_days = 12 + (u32)(agemix % 49ULL);
	p->boot_count = 20 + (u32)(ghost_mix64(seed, 0xB007ULL) % 61ULL);

	senmix = ghost_mix64(seed, 0x51B0ULL);
	p->sensor_bias[0] = (s16)((s32)(senmix % 41ULL) - 20);
	p->sensor_bias[1] = (s16)((s32)((senmix >> 8) % 41ULL) - 20);
	p->sensor_bias[2] = (s16)((s32)((senmix >> 16) % 41ULL) - 20);
	p->baro_drift_hpa_x100 = (s32)((ghost_mix64(seed, 0xBA20ULL) % 81ULL) - 40);
	p->tcp_isn_offset = (u32)ghost_mix64(seed, 0x15E0ULL);
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


#define GHOST_PROP_AREA_SIZE     196608
#define GHOST_PROP_AREA_HEADER   128
#define GHOST_PROP_VALUE_MAX     92
#define GHOST_PROP_AREA_SERIAL   4

static DEFINE_SPINLOCK(ghost_imei_seen_lock);
static char ghost_seen_hw_imei1[16];
static char ghost_seen_hw_imei2[16];
static char ghost_seen_hw_imsi1[16];
static char ghost_seen_hw_imsi2[16];
static char ghost_seen_hw_iccid1[24];
static char ghost_seen_hw_iccid2[24];

static bool ghost_prop_name_is(const char *name, size_t nmax, const char *want)
{
	size_t i;

	if (!name || !want || nmax == 0)
		return false;
	for (i = 0; i < nmax; i++) {
		if (want[i] == '\0')
			return name[i] == '\0';
		if (name[i] != want[i])
			return false;
	}
	return false;
}

static int ghost_mapping_write(struct file *filp, loff_t offset,
			       const void *src, size_t len)
{
	struct address_space *mapping;
	struct page *page;
	char *kaddr;
	size_t page_off;
	size_t chunk;
	const char *p = src;

	if (!filp || !src || !len)
		return -EINVAL;
	mapping = filp->f_mapping;
	if (!mapping)
		return -ENODEV;

	while (len) {
		page_off = (size_t)(offset & (PAGE_SIZE - 1));
		chunk = PAGE_SIZE - page_off;
		if (chunk > len)
			chunk = len;
		page = read_mapping_page(mapping, (pgoff_t)(offset >> PAGE_SHIFT),
					 NULL);
		if (IS_ERR(page))
			return PTR_ERR(page);
		lock_page(page);
		kaddr = kmap(page);
		memcpy(kaddr + page_off, p, chunk);
		kunmap(page);
		set_page_dirty(page);
		flush_dcache_page(page);
		unlock_page(page);
		put_page(page);
		offset += chunk;
		p += chunk;
		len -= chunk;
	}
	return 0;
}

static struct file *ghost_open_android_rel(const char *rel_path)
{
	struct path init_root;
	struct file *filp = ERR_PTR(-ENOENT);
	struct task_struct *init_p = NULL;
	const struct cred *old_cred = NULL;
	const struct cred *init_cred = NULL;
	char abs[160];

	if (!rel_path || !rel_path[0])
		return ERR_PTR(-EINVAL);

	rcu_read_lock();
	init_p = find_task_by_pid_ns(1, &init_pid_ns);
	if (init_p) {
		get_task_struct(init_p);
		init_cred = get_cred(__task_cred(init_p));
	}
	rcu_read_unlock();
	if (init_cred)
		old_cred = override_creds(init_cred);

	if (ghost_get_init_root(&init_root) == 0) {
		filp = file_open_root(init_root.dentry, init_root.mnt,
				      rel_path, O_RDONLY, 0);
		path_put(&init_root);
	}
	if (IS_ERR(filp)) {
		if (rel_path[0] == '/')
			filp = filp_open(rel_path, O_RDONLY, 0);
		else {
			snprintf(abs, sizeof(abs), "/%s", rel_path);
			filp = filp_open(abs, O_RDONLY, 0);
		}
	}

	if (old_cred)
		revert_creds(old_cred);
	if (init_cred)
		put_cred(init_cred);
	if (init_p)
		put_task_struct(init_p);
	return filp;
}

static int ghost_prop_bump_area_serial(struct file *filp, char *buf, ssize_t bytes)
{
	u32 area_serial;

	if (!filp || !buf || bytes < 8)
		return -EINVAL;
	memcpy(&area_serial, buf + GHOST_PROP_AREA_SERIAL, 4);
	area_serial += 1;
	memcpy(buf + GHOST_PROP_AREA_SERIAL, &area_serial, 4);
	return ghost_mapping_write(filp, GHOST_PROP_AREA_SERIAL, &area_serial, 4);
}

static int ghost_prop_patch_one(struct file *filp, char *buf, ssize_t bytes,
				loff_t info_off, size_t vlen, const char *new_val)
{
	u32 serial;
	u32 dirty;
	u32 done;

	if (!filp || !buf || !new_val || vlen == 0 || vlen >= GHOST_PROP_VALUE_MAX)
		return 0;
	if (info_off < GHOST_PROP_AREA_HEADER ||
	    info_off + 4 + (loff_t)vlen >= bytes)
		return 0;
	memcpy(&serial, buf + info_off, 4);
	if ((serial >> 24) != vlen)
		return 0;
	if (buf[info_off + 4 + vlen] != '\0')
		return 0;
	if (!memcmp(buf + info_off + 4, new_val, vlen))
		return 1;
	dirty = serial | 1u;
	if (ghost_mapping_write(filp, info_off, &dirty, 4))
		return -EIO;
	smp_wmb();
	if (ghost_mapping_write(filp, info_off + 4, new_val, vlen))
		return -EIO;
	smp_wmb();
	done = ((u32)vlen << 24) | (((serial | 1u) + 1u) & 0xffffffu);
	if (ghost_mapping_write(filp, info_off, &done, 4))
		return -EIO;
	memcpy(buf + info_off, &done, 4);
	memcpy(buf + info_off + 4, new_val, vlen);
	return 2;
}

static int ghost_patch_prop_file_by_names(const char *rel_path, size_t vlen,
					  const char *new_val,
					  const char *const *names,
					  const char *tag)
{
	struct file *filp;
	char *buf;
	loff_t pos = 0;
	ssize_t bytes;
	int i, n, r, hit;
	int patched = 0;
	int already = 0;
	int bump = 0;
	u32 serial;
	const char *pname;

	if (!rel_path || !new_val || !names || strlen(new_val) != vlen)
		return 0;

	filp = ghost_open_android_rel(rel_path);
	if (IS_ERR(filp)) {
		pr_info_ratelimited("GhostKernel: prop %s open %s err=%ld\n",
				    tag, rel_path, PTR_ERR(filp));
		return 0;
	}

	buf = kzalloc(GHOST_PROP_AREA_SIZE, GFP_KERNEL);
	if (!buf) {
		filp_close(filp, NULL);
		return 0;
	}

	bytes = kernel_read(filp, buf, GHOST_PROP_AREA_SIZE, &pos);
	if (bytes >= (ssize_t)(GHOST_PROP_AREA_HEADER + GHOST_PROP_VALUE_MAX + 8)) {
		for (i = GHOST_PROP_AREA_HEADER;
		     i + 96 + 8 < bytes;
		     i += 4) {
			memcpy(&serial, buf + i, 4);
			if ((serial >> 24) != vlen)
				continue;
			if (buf[i + 4 + vlen] != '\0')
				continue;
			pname = buf + i + 96;
			hit = 0;
			for (n = 0; names[n]; n++) {
				if (!ghost_prop_name_is(pname, bytes - (i + 96),
							names[n]))
					continue;
				hit = 1;
				r = ghost_prop_patch_one(filp, buf, bytes, i,
							 vlen, new_val);
				if (r == 1)
					already++;
				else if (r == 2) {
					patched++;
					bump = 1;
				}
				break;
			}
			if (hit)
				continue;
			/* Fallback: typed same-length value if name layout differs. */
			if (vlen == 11 && buf[i + 4] == 'R' && buf[i + 5] == '5')
				r = 1;
			else if (vlen == 14 && buf[i + 4] == '0' &&
				 (buf[i + 5] == 'x' || buf[i + 5] == 'X'))
				r = 1;
			else if (vlen == 16 && buf[i + 4] == '2' &&
				 buf[i + 5] == '0' && buf[i + 18] == '1' &&
				 buf[i + 19] == '1')
				r = 1;
			else
				r = 0;
			if (r) {
				r = ghost_prop_patch_one(filp, buf, bytes, i,
							 vlen, new_val);
				if (r == 1)
					already++;
				else if (r == 2) {
					patched++;
					bump = 1;
				}
			}
		}
		if (bump)
			ghost_prop_bump_area_serial(filp, buf, bytes);
		if (patched || already)
			pr_info("GhostKernel: prop %s in %s patched=%d already=%d\n",
				tag, rel_path, patched, already);
		else
			pr_info_ratelimited("GhostKernel: prop %s miss %s bytes=%zd\n",
					    tag, rel_path, bytes);
	} else {
		pr_info_ratelimited("GhostKernel: prop %s short read %s bytes=%zd\n",
				    tag, rel_path, bytes);
	}
	kfree(buf);
	filp_close(filp, NULL);
	return patched + already;
}

static int ghost_patch_property_serial(const char *new_sn)
{
	static const char *const names[] = {
		"ro.serialno",
		"ro.boot.serialno",
		NULL
	};
	static const char *const files[] = {
		"dev/__properties__/u:object_r:serialno_prop:s0",
		"dev/__properties__/u:object_r:default_prop:s0",
		NULL
	};
	int i, total = 0;

	if (!new_sn || strlen(new_sn) != 11)
		return 0;
	for (i = 0; files[i]; i++)
		total += ghost_patch_prop_file_by_names(files[i], 11, new_sn,
							names, "serialno");
	return total;
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

static int ghost_patch_property_ap_serial(const char *new_ap, const char *new_did)
{
	static const char *const ap_names[] = {
		"ro.boot.ap_serial",
		NULL
	};
	static const char *const did_names[] = {
		"ro.boot.em.did",
		NULL
	};
	static const char *const ap_files[] = {
		"dev/__properties__/u:object_r:ap_serial_prop:s0",
		"dev/__properties__/u:object_r:bootloader_prop:s0",
		NULL
	};
	static const char *const did_files[] = {
		"dev/__properties__/u:object_r:boot_em_did_prop:s0",
		"dev/__properties__/u:object_r:bootloader_prop:s0",
		NULL
	};
	int i, total = 0;

	if (!is_valid_ap_serial(new_ap) || !new_did || strlen(new_did) != 16)
		return 0;
	for (i = 0; ap_files[i]; i++)
		total += ghost_patch_prop_file_by_names(ap_files[i], 14, new_ap,
							ap_names, "ap_serial");
	for (i = 0; did_files[i]; i++)
		total += ghost_patch_prop_file_by_names(did_files[i], 16, new_did,
							did_names, "em.did");
	return total;
}

static int ghost_patch_property_security_patch(const char *new_patch)
{
	static const char *const names[] = {
		"ro.build.version.security_patch",
		"ro.vendor.build.security_patch",
		"ro.bootimage.build.security_patch",
		"ro.system.build.version.security_patch",
		"ro.system_ext.build.version.security_patch",
		"ro.product.build.version.security_patch",
		"ro.odm.build.version.security_patch",
		NULL
	};
	static const char *const files[] = {
		"dev/__properties__/u:object_r:build_prop:s0",
		"dev/__properties__/u:object_r:vendor_security_patch_level_prop:s0",
		"dev/__properties__/u:object_r:default_prop:s0",
		"dev/__properties__/u:object_r:vendor_default_prop:s0",
		"dev/__properties__/u:object_r:exported_default_prop:s0",
		NULL
	};
	const char *patch_val = new_patch;
	int i, total = 0;

	if (!patch_val || strlen(patch_val) != 10)
		patch_val = "2024-05-01";
	for (i = 0; files[i]; i++) {
		if (ghost_patch_prop_file_by_names(files[i], 10, patch_val, names,
						   "security_patch") > 0)
			total++;
	}
	return total;
}


static char *ghost_memstr(char *buf, size_t len, const char *needle)
{
	size_t nlen;
	size_t i;

	if (!buf || !needle || len == 0)
		return NULL;
	nlen = strlen(needle);
	if (nlen == 0 || len < nlen)
		return NULL;
	for (i = 0; i + nlen <= len; i++) {
		if (!memcmp(buf + i, needle, nlen))
			return buf + i;
	}
	return NULL;
}

static void ghost_inplace_copy(char *buf, size_t len, const char *key,
			       const char *val, size_t vlen)
{
	char *p;

	if (!buf || !key || !val || vlen == 0)
		return;
	p = ghost_memstr(buf, len, key);
	if (!p)
		return;
	p += strlen(key);
	if (p + vlen > buf + len)
		return;
	memcpy(p, val, vlen);
}

static void ghost_rewrite_digits(char *buf, size_t len, const char *key,
				 u64 derived, int force_zero)
{
	char *p;
	char *val;
	int digits = 0;
	int i;
	u64 mod = 1;
	u64 n;
	size_t klen;

	if (!buf || !key)
		return;
	klen = strlen(key);
	p = ghost_memstr(buf, len, key);
	if (!p)
		return;
	val = p + klen;
	while (val + digits < buf + len && val[digits] >= '0' && val[digits] <= '9')
		digits++;
	if (digits <= 0)
		return;
	if (force_zero) {
		for (i = 0; i < digits; i++)
			val[i] = '0';
		return;
	}
	for (i = 0; i < digits && i < 9; i++)
		mod *= 10ULL;
	n = derived % mod;
	for (i = digits - 1; i >= 0; i--) {
		val[i] = (char)('0' + (n % 10ULL));
		n /= 10ULL;
	}
}

void ghost_sanitize_bootargs(char *buf, size_t len)
{
	struct ghost_profile snap;
	char *p;
	u64 panel_id;
	u64 snapqb;
	u64 bore;
	int asb;
	int psite;

	if (!buf || len == 0)
		return;

	ghost_get_profile_snapshot(&snap);

	if (snap.serialno[0] && strlen(snap.serialno) == 11)
		ghost_inplace_copy(buf, len, "androidboot.serialno=", snap.serialno, 11);
	if (snap.ap_serial[0] && strlen(snap.ap_serial) == 14)
		ghost_inplace_copy(buf, len, "androidboot.ap_serial=", snap.ap_serial, 14);
	if (snap.em_did[0] && strlen(snap.em_did) == 16)
		ghost_inplace_copy(buf, len, "androidboot.em.did=", snap.em_did, 16);

	p = ghost_memstr(buf, len, "factory_reset");
	if (p && (p + 13) <= (buf + len))
		memcpy(p, "reboot,kernel", 13);

	p = ghost_memstr(buf, len, "androidboot.warranty_bit=1");
	if (p)
		p[25] = '0';
	p = ghost_memstr(buf, len, "sec_debug.warranty_bit=1");
	if (p)
		p[23] = '0';

	p = ghost_memstr(buf, len, "androidboot.verifiedbootstate=orange");
	if (p)
		memcpy(p + 30, "green ", 6);
	p = ghost_memstr(buf, len, "androidboot.verifiedbootstate=yellow");
	if (p)
		memcpy(p + 30, "green ", 6);

	p = ghost_memstr(buf, len, "androidboot.selinux=permissive");
	if (p)
		memcpy(p + 20, "enforcing ", 10);
	p = ghost_memstr(buf, len, "androidboot.flash.locked=0");
	if (p)
		p[25] = '1';

	p = ghost_memstr(buf, len, "androidboot.vbmeta.device_state=unlocked");
	if (p)
		memcpy(p + 32, "locked  ", 8);

	p = ghost_memstr(buf, len, "androidboot.kg=0x6");
	if (p)
		memcpy(p + 15, "0x0", 3);

	if (snap.boot_hash[0] && strlen(snap.boot_hash) == 64) {
		ghost_inplace_copy(buf, len, "androidboot.vbmeta.digest=", snap.boot_hash, 64);
		ghost_inplace_copy(buf, len, "androidboot.boot_hash=", snap.boot_hash, 64);
	}
	if (snap.boot_key[0] && strlen(snap.boot_key) == 64) {
		ghost_inplace_copy(buf, len, "androidboot.bootkey=", snap.boot_key, 64);
		ghost_inplace_copy(buf, len, "androidboot.verifiedbootkey=", snap.boot_key, 64);
		ghost_inplace_copy(buf, len, "androidboot.vbmeta.public_key_digest=", snap.boot_key, 64);
	}

	ghost_rewrite_digits(buf, len, "androidboot.ulcnt=", 0, 1);
	bore = snap.boot_count ? snap.boot_count : 20;
	ghost_rewrite_digits(buf, len, "androidboot.bore_cnt=", bore, 0);

	panel_id = ghost_mix64(snap.unique_id, 0x1CD7ULL) % 100000000ULL;
	if (panel_id < 10000000ULL)
		panel_id += 10000000ULL;
	ghost_rewrite_digits(buf, len, "lcdtype=", panel_id, 0);
	ghost_rewrite_digits(buf, len, "mcd-panel.boot_panel_id=", panel_id, 0);

	snapqb = ghost_mix64(snap.unique_id, 0x5A9BULL) % 100000000ULL;
	if (snapqb < 10000000ULL)
		snapqb += 10000000ULL;
	ghost_rewrite_digits(buf, len, "androidboot.wb.snapQB=", snapqb, 0);

	ghost_get_asb_psite(&asb, &psite);
	ghost_rewrite_digits(buf, len, "androidboot.asb=", asb, 0);
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
	strscpy(p->security_patch, "2022-01-01", sizeof(p->security_patch));

	strscpy(p->serialno, "R5Y51V5FVU5", sizeof(p->serialno));
	strscpy(p->ap_serial, "0x9F80C16900A1", sizeof(p->ap_serial));
	strscpy(p->em_did, "209f80c16900a111", sizeof(p->em_did));
	p->unique_id = (0x5857ULL << 48) | 0x9F80C16900A1ULL;

	strscpy(p->imei, "358446927832645", sizeof(p->imei));
	strscpy(p->imei2, "359067849061094", sizeof(p->imei2));
	strscpy(p->imsi, "452041938274650", sizeof(p->imsi));
	strscpy(p->imsi2, "452048173625941", sizeof(p->imsi2));
	strscpy(p->iccid, "89840410293847561055", sizeof(p->iccid));
	strscpy(p->iccid2, "89840421938475610288", sizeof(p->iccid2));

	p->wifi_mac[0] = 0x24; p->wifi_mac[1] = 0x4B; p->wifi_mac[2] = 0xFE;
	p->wifi_mac[3] = 0x20; p->wifi_mac[4] = 0x35; p->wifi_mac[5] = 0x25;

	p->bt_mac[0] = 0x24; p->bt_mac[1] = 0x4B; p->bt_mac[2] = 0xFE;
	p->bt_mac[3] = 0x20; p->bt_mac[4] = 0x35; p->bt_mac[5] = 0x26;

	strscpy(p->ufs_serial, "0x9f80c169", sizeof(p->ufs_serial));
	strscpy(p->ufs_model, "KLUDG8UHDB-C2D1", sizeof(p->ufs_model));

	strscpy(p->boot_hash, "7207368a4caca12d62f0382e67932c38f78c6d0b3f9bd7f5967825461b4172c1", sizeof(p->boot_hash));
	strscpy(p->boot_key, "22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781", sizeof(p->boot_key));

	p->uptime_days = 44;
	p->boot_count = 55;
	p->battery_cycle = 175;
	p->battery_health = 96;
	p->sensor_bias[0] = -19;
	p->sensor_bias[1] = 7;
	p->sensor_bias[2] = 9;
	p->baro_drift_hpa_x100 = -45;
	p->tcp_isn_offset = 0x49614cb1;

	p->spoofed_kernel_version[0] = '\0';
	p->is_loaded = false;
	strscpy(p->loaded_from, "[DEFAULT_FALLBACK]", sizeof(p->loaded_from));
}

static char *trim_str(char *str)
{
	char *end;
	while (*str && (isspace((unsigned char)*str) || *str == '\r'))
		str++;
	if (*str == 0)
		return str;
	end = str + strlen(str) - 1;
	while (end > str && (isspace((unsigned char)*end) || *end == '\r'))
		end--;
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

static bool ghost_is_unique_conf_key(const char *k)
{
	if (!k)
		return false;
	return !strcasecmp(k, "serialno") || !strcasecmp(k, "serial_no") ||
	       !strcasecmp(k, "ap_serial") || !strcasecmp(k, "unique_id") ||
	       !strcasecmp(k, "em_did") || !strcasecmp(k, "em.did") ||
	       !strcasecmp(k, "imei") || !strcasecmp(k, "imei2") ||
	       !strcasecmp(k, "imsi") || !strcasecmp(k, "imsi2") ||
	       !strcasecmp(k, "iccid") || !strcasecmp(k, "iccid2") ||
	       !strcasecmp(k, "wifi_mac") || !strcasecmp(k, "bt_mac") ||
	       !strcasecmp(k, "ufs_serial") ||
	       !strcasecmp(k, "uptime_days") || !strcasecmp(k, "boot_count") ||
	       !strcasecmp(k, "battery_cycle") || !strcasecmp(k, "battery_health") ||
	       !strcasecmp(k, "sensor_bias") || !strcasecmp(k, "tcp_isn_offset");
}

static void ghost_parse_identity_mode_prepass(char *buf, size_t len, const char *source_path)
{
	char *copy;
	char *line;
	char *next_line;
	char *eq;
	char *k_key;
	char *k_val;

	if (!buf || !source_path || strcmp(source_path, GHOST_CONF_PATH_PRIMARY) != 0)
		return;

	copy = kmalloc(len + 1, GFP_KERNEL);
	if (!copy)
		return;
	memcpy(copy, buf, len);
	copy[len] = '\0';

	line = copy;
	while (line < copy + len) {
		char *newline = strchr(line, '\n');

		if (newline) {
			*newline = '\0';
			next_line = newline + 1;
		} else {
			next_line = copy + len;
		}
		line = trim_str(line);
		if (line[0] && line[0] != '#' && line[0] != ';' && line[0] != '[') {
			eq = strchr(line, '=');
			if (eq) {
				*eq = '\0';
				k_key = trim_str(line);
				k_val = trim_str(eq + 1);
				if (!strcasecmp(k_key, "identity_mode")) {
					if (!strcasecmp(k_val, "pinned"))
						ghost_identity_mode = GHOST_IDENTITY_PINNED;
					else
						ghost_identity_mode = GHOST_IDENTITY_EPOCH;
				}
			}
		}
		line = next_line;
	}
	kfree(copy);
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
	ghost_parse_identity_mode_prepass(buf, len, source_path);

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

		if (ghost_is_unique_conf_key(k_key) &&
		    (ghost_identity_mode != GHOST_IDENTITY_PINNED ||
		     !source_path || strcmp(source_path, GHOST_CONF_PATH_PRIMARY) != 0)) {
			line = next_line;
			continue;
		}

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
			u64 raw_ap = 0;
			if (!is_valid_ap_serial(k_val)) {
				pr_warn("GhostKernel: Invalid ap_serial '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->ap_serial, k_val, sizeof(temp_prof->ap_serial));
			if (kstrtoull(k_val + 2, 16, &raw_ap) == 0) {
				raw_ap &= 0xFFFFFFFFFFFFULL;
				temp_prof->unique_id = (0x5857ULL << 48) | raw_ap;
			}
		} else if (!strcasecmp(k_key, "unique_id")) {
			u64 uid_val = 0;
			if (kstrtoull(k_val, 16, &uid_val) == 0 ||
			    (k_val[0] == '0' && (k_val[1] == 'x' || k_val[1] == 'X') && kstrtoull(k_val + 2, 16, &uid_val) == 0)) {
				temp_prof->unique_id = uid_val;
			}
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
		} else if (!strcasecmp(k_key, "imsi")) {
			if (strlen(k_val) != 15) {
				pr_warn("GhostKernel: Invalid IMSI '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->imsi, k_val, sizeof(temp_prof->imsi));
		} else if (!strcasecmp(k_key, "imsi2")) {
			if (strlen(k_val) != 15) {
				pr_warn("GhostKernel: Invalid IMSI2 '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->imsi2, k_val, sizeof(temp_prof->imsi2));
		} else if (!strcasecmp(k_key, "iccid")) {
			if (strlen(k_val) < 19 || strlen(k_val) > 20 ||
			    k_val[0] != '8' || k_val[1] != '9') {
				pr_warn("GhostKernel: Invalid ICCID '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->iccid, k_val, sizeof(temp_prof->iccid));
		} else if (!strcasecmp(k_key, "iccid2")) {
			if (strlen(k_val) < 19 || strlen(k_val) > 20 ||
			    k_val[0] != '8' || k_val[1] != '9') {
				pr_warn("GhostKernel: Invalid ICCID2 '%s' in %s\n", k_val, source_path);
				kfree(temp_prof);
				return -EINVAL;
			}
			strscpy(temp_prof->iccid2, k_val, sizeof(temp_prof->iccid2));
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
		} else if (!strcasecmp(k_key, "identity_mode")) {
			/* Honored in prepass from /data/adb only. */
		} else {
			pr_warn("GhostKernel: Unknown config key '%s' in %s (skipped)\n", k_key, source_path);
		}

		line = next_line;
	}

	/* Validation Gates: serial is required; silicon IDs may be derived. */
	if (!is_valid_samsung_serial(temp_prof->serialno)) {
		pr_warn("GhostKernel: Validation failed for serialno '%s' from %s\n",
			temp_prof->serialno, source_path);
		kfree(temp_prof);
		return -EINVAL;
	}

	if (!is_valid_ap_serial(temp_prof->ap_serial)) {
		u64 raw = 0;
		int si;
		for (si = 0; si < 11 && temp_prof->serialno[si]; si++)
			raw = (raw * 131u) + (u8)temp_prof->serialno[si];
		raw = ghost_mix64(raw, 0xA15EULL) & 0xFFFFFFFFFFFFULL;
		if (!raw)
			raw = 0x9F80C16900A1ULL;
		snprintf(temp_prof->ap_serial, sizeof(temp_prof->ap_serial), "0x%012llX", raw);
		pr_info("GhostKernel: Derived ap_serial %s from serialno in %s\n",
			temp_prof->ap_serial, source_path);
	}
	{
		u64 raw_ap = 0;
		if (kstrtoull(temp_prof->ap_serial + 2, 16, &raw_ap) == 0) {
			raw_ap &= 0xFFFFFFFFFFFFULL;
			temp_prof->unique_id = (0x5857ULL << 48) | raw_ap;
			if (strlen(temp_prof->em_did) != 16)
				snprintf(temp_prof->em_did, sizeof(temp_prof->em_did),
					 "20%012llx11", raw_ap);
		}
	}

	if (ghost_identity_mode != GHOST_IDENTITY_PINNED && ghost_userdata_uuid_valid)
		ghost_fill_unique_from_seed(temp_prof,
					    ghost_seed_from_uuid(ghost_userdata_uuid));

	temp_prof->is_loaded = true;
	if (ghost_identity_mode != GHOST_IDENTITY_PINNED && ghost_userdata_uuid_valid)
		snprintf(temp_prof->loaded_from, sizeof(temp_prof->loaded_from),
			 "epoch:%s", source_path);
	else
		strscpy(temp_prof->loaded_from, source_path, sizeof(temp_prof->loaded_from));

	{
		struct ghost_profile *old_prof;

		/* Atomic commit under mutex via RCU publication */
		mutex_lock(&ghost_config_mutex);
		old_prof = rcu_dereference_protected(ghost_active_profile_ptr, lockdep_is_held(&ghost_config_mutex));
		rcu_assign_pointer(ghost_active_profile_ptr, temp_prof);
		memcpy(&ghost_active_profile, temp_prof, sizeof(*temp_prof));
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
	static const char * const paths[] = {
		GHOST_CONF_PATH_PRIMARY,
		GHOST_CONF_PATH_SECONDARY,
		GHOST_CONF_PATH_FALLBACK,
	};
	int i, ret, last = -ENOENT;

	mutex_lock(&ghost_reload_mutex);
	ghost_identity_mode = GHOST_IDENTITY_EPOCH;
	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		ret = ghost_read_file_and_parse(paths[i]);
		if (ret == 0) {
			mutex_unlock(&ghost_reload_mutex);
			return 0;
		}
		pr_info("GhostKernel: profile %s ret=%d, trying next\n", paths[i], ret);
		if (ret != -ENOENT)
			last = ret;
	}
	mutex_unlock(&ghost_reload_mutex);
	if (ghost_userdata_uuid_valid && ghost_identity_mode != GHOST_IDENTITY_PINNED)
		ghost_on_f2fs_userdata_mount(ghost_userdata_uuid);
	return last;
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
	u8 drm[32];
	char drmhex[65];
	unsigned di;

	/* Restrict sensitive full config inspection from untrusted UIDs */
	if (current_uid().val >= 2000) {
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
	seq_printf(m, "Identity Mode     : %s\n",
		   ghost_identity_mode == GHOST_IDENTITY_PINNED ? "pinned" : "epoch");
	seq_printf(m, "Device Model      : %s\n", p->model);
	seq_printf(m, "Product / Device  : %s / %s\n", p->product, p->device);
	seq_printf(m, "SoC Machine/Family: %s / %s\n", p->soc_machine, p->soc_family);
	seq_printf(m, "Build Fingerprint : %s\n", p->build_fingerprint);
	seq_printf(m, "Spoofed Kernel    : %s\n", ghost_spoofed_kernel_version);
	seq_printf(m, "Build Description : %s\n", p->build_desc);
	seq_printf(m, "Serial Number     : %s\n", p->serialno);
	seq_printf(m, "Cellular IMEI     : %s\n", p->imei);
	seq_printf(m, "Cellular IMEI 2   : %s\n", p->imei2);
	seq_printf(m, "Cellular IMSI     : %s\n", p->imsi);
	seq_printf(m, "Cellular IMSI 2   : %s\n", p->imsi2);
	seq_printf(m, "SIM ICCID         : %s\n", p->iccid);
	seq_printf(m, "SIM ICCID 2       : %s\n", p->iccid2);
	seq_printf(m, "Wi-Fi MAC         : %pM\n", p->wifi_mac);
	seq_printf(m, "Bluetooth Address : %02X:%02X:%02X:%02X:%02X:%02X\n",
		   p->bt_mac[0], p->bt_mac[1], p->bt_mac[2],
		   p->bt_mac[3], p->bt_mac[4], p->bt_mac[5]);
	seq_printf(m, "UFS Serial/Model  : %s / %s\n", p->ufs_serial, p->ufs_model);
	seq_printf(m, "Uptime Age (Days) : %u days\n", p->uptime_days);
	seq_printf(m, "Boot Count        : %u\n", p->boot_count);
	seq_printf(m, "Battery Cycle     : %u (Health: %u%%)\n", p->battery_cycle, p->battery_health);
	seq_printf(m, "Sensor Bias (XYZ) : %d, %d, %d\n", p->sensor_bias[0], p->sensor_bias[1], p->sensor_bias[2]);
	seq_printf(m, "TCP ISN Offset    : 0x%08X\n", p->tcp_isn_offset);
	seq_printf(m, "Unique ID         : 0x%016llX\n",
		   (unsigned long long)p->unique_id);
	memset(drm, 0, sizeof(drm));
	ghost_fill_drm_id(drm, 32);
	for (di = 0; di < 32; di++)
		sprintf(drmhex + di * 2, "%02x", drm[di]);
	drmhex[64] = 0;
	seq_printf(m, "DRM ID (32)       : %s\n", drmhex);
	seq_printf(m, "Wi-Fi BSS Notes   : %d\n", ghost_wifi_has_notes());
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
	int ap = 0;
	int sec_files = 0;
	struct ghost_profile snap;

	ghost_prop_patch_attempts++;
	ghost_get_profile_snapshot(&snap);

	count = ghost_patch_property_serial(snap.serialno);
	ap = ghost_patch_property_ap_serial(snap.ap_serial, snap.em_did);
	ghost_patch_usb_serial(snap.serialno);
	sec_files = ghost_patch_property_security_patch(snap.security_patch);

	/* Serial/AP/DID trie hits are the identity leak; stop once those land. */
	if (count > 0 && ap > 0) {
		pr_info("GhostKernel: Property patch confirmed complete on attempt %d (serial count=%d, ap=%d, sec_files=%d)\n",
			ghost_prop_patch_attempts, count, ap, sec_files);
		return;
	}

	/* Retry up to 180 times (every 1s) to make sure init finishes writing late vendor properties */
	if (ghost_prop_patch_attempts < 180) {
		schedule_delayed_work(&ghost_prop_patch_work, msecs_to_jiffies(1000));
	} else {
		pr_info("GhostKernel: Property patch confirmed complete on attempt %d (serial count=%d, ap=%d, sec_files=%d)\n",
			ghost_prop_patch_attempts, count, ap, sec_files);
	}
}

static void ghost_apply_epoch(const u8 *uuid)
{
	struct ghost_profile *new_prof;
	struct ghost_profile *old_prof;
	u64 seed;

	if (!uuid)
		return;
	if (ghost_identity_mode == GHOST_IDENTITY_PINNED)
		return;

	seed = ghost_seed_from_uuid(uuid);
	new_prof = kzalloc(sizeof(*new_prof), GFP_KERNEL);
	if (!new_prof)
		return;

	ghost_get_profile_snapshot(new_prof);
	ghost_fill_unique_from_seed(new_prof, seed);
	new_prof->is_loaded = true;
	strscpy(new_prof->loaded_from, "[EPOCH_UUID]", sizeof(new_prof->loaded_from));

	mutex_lock(&ghost_config_mutex);
	old_prof = rcu_dereference_protected(ghost_active_profile_ptr,
			lockdep_is_held(&ghost_config_mutex));
	rcu_assign_pointer(ghost_active_profile_ptr, new_prof);
	memcpy(&ghost_active_profile, new_prof, sizeof(*new_prof));
	mutex_unlock(&ghost_config_mutex);

	if (old_prof && old_prof != &ghost_active_profile) {
		synchronize_rcu();
		kfree(old_prof);
	}

	ghost_apply_properties_from_snapshot(new_prof);
	ghost_serial_guard_completed = true;
	pr_info("GhostKernel: epoch seed applied uuid=%02x%02x%02x%02x%02x%02x%02x%02x serial=%s\n",
		uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
		new_prof->serialno);
}

void ghost_on_f2fs_userdata_mount(const u8 *uuid)
{
	if (!uuid)
		return;

	if (ghost_userdata_uuid_valid && !memcmp(ghost_userdata_uuid, uuid, 16)) {
		if (ghost_identity_mode == GHOST_IDENTITY_PINNED)
			return;
		/* Same userdata UUID already mixed into the active profile. */
		if (ghost_active_profile.loaded_from[0] &&
		    (strncmp(ghost_active_profile.loaded_from, "[EPOCH_UUID]", 12) == 0 ||
		     strncmp(ghost_active_profile.loaded_from, "epoch:", 6) == 0))
			return;
	}

	memcpy(ghost_userdata_uuid, uuid, 16);
	ghost_userdata_uuid_valid = true;
	ghost_apply_epoch(uuid);
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

void ghost_get_imei_buf(char *buf, size_t len)
{
	struct ghost_profile *p;

	if (!buf || len == 0)
		return;
	buf[0] = '\0';
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->imei[0])
		strscpy(buf, p->imei, len);
	else
		strscpy(buf, ghost_active_profile.imei, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_imei_buf);

void ghost_get_imei2_buf(char *buf, size_t len)
{
	struct ghost_profile *p;

	if (!buf || len == 0)
		return;
	buf[0] = '\0';
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->imei2[0])
		strscpy(buf, p->imei2, len);
	else
		strscpy(buf, ghost_active_profile.imei2, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_imei2_buf);

bool ghost_select_cloaked_imei_slot(const char *hw_imei, char *out, size_t len,
				    int slot)
{
	char e1[16];
	char e2[16];
	int i;
	bool ok = false;
	bool hw_is_epoch;

	if (!hw_imei || !out || len < 16)
		return false;
	out[0] = '\0';
	for (i = 0; i < 15; i++) {
		if (hw_imei[i] < '0' || hw_imei[i] > '9')
			return false;
	}
	if (hw_imei[14] != ghost_imei_luhn_digit(hw_imei))
		return false;
	if (!((hw_imei[0] == '3' && hw_imei[1] == '5') ||
	      (hw_imei[0] == '8' && hw_imei[1] == '6')))
		return false;

	memset(e1, 0, sizeof(e1));
	memset(e2, 0, sizeof(e2));
	ghost_get_imei_buf(e1, sizeof(e1));
	ghost_get_imei2_buf(e2, sizeof(e2));
	if (strlen(e1) != 15 || strlen(e2) != 15)
		return false;

	hw_is_epoch = !memcmp(hw_imei, e1, 15) || !memcmp(hw_imei, e2, 15);
	if (slot == 0) {
		if (!memcmp(hw_imei, e1, 15))
			return false;
		strscpy(out, e1, len);
		return true;
	}
	if (slot == 1) {
		if (!memcmp(hw_imei, e2, 15))
			return false;
		strscpy(out, e2, len);
		return true;
	}
	if (hw_is_epoch)
		return false;

	spin_lock(&ghost_imei_seen_lock);
	if (!ghost_seen_hw_imei1[0] || !memcmp(ghost_seen_hw_imei1, hw_imei, 15)) {
		memcpy(ghost_seen_hw_imei1, hw_imei, 15);
		ghost_seen_hw_imei1[15] = '\0';
		strscpy(out, e1, len);
		ok = true;
	} else if (!ghost_seen_hw_imei2[0] || !memcmp(ghost_seen_hw_imei2, hw_imei, 15)) {
		memcpy(ghost_seen_hw_imei2, hw_imei, 15);
		ghost_seen_hw_imei2[15] = '\0';
		strscpy(out, e2, len);
		ok = true;
	} else {
		strscpy(out, e1, len);
		ok = true;
	}
	spin_unlock(&ghost_imei_seen_lock);
	return ok;
}
EXPORT_SYMBOL(ghost_select_cloaked_imei_slot);

bool ghost_select_cloaked_imei(const char *hw_imei, char *out, size_t len)
{
	return ghost_select_cloaked_imei_slot(hw_imei, out, len, -1);
}
EXPORT_SYMBOL(ghost_select_cloaked_imei);

void ghost_get_imsi_buf(char *buf, size_t len)
{
	struct ghost_profile *p;

	if (!buf || len == 0)
		return;
	buf[0] = '\0';
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->imsi[0])
		strscpy(buf, p->imsi, len);
	else
		strscpy(buf, ghost_active_profile.imsi, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_imsi_buf);

void ghost_get_imsi2_buf(char *buf, size_t len)
{
	struct ghost_profile *p;

	if (!buf || len == 0)
		return;
	buf[0] = '\0';
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->imsi2[0])
		strscpy(buf, p->imsi2, len);
	else
		strscpy(buf, ghost_active_profile.imsi2, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_imsi2_buf);

void ghost_get_iccid_buf(char *buf, size_t len)
{
	struct ghost_profile *p;

	if (!buf || len == 0)
		return;
	buf[0] = '\0';
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->iccid[0])
		strscpy(buf, p->iccid, len);
	else
		strscpy(buf, ghost_active_profile.iccid, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_iccid_buf);

void ghost_get_iccid2_buf(char *buf, size_t len)
{
	struct ghost_profile *p;

	if (!buf || len == 0)
		return;
	buf[0] = '\0';
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->iccid2[0])
		strscpy(buf, p->iccid2, len);
	else
		strscpy(buf, ghost_active_profile.iccid2, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_iccid2_buf);

bool ghost_select_cloaked_imsi_slot(const char *hw_imsi, char *out, size_t len,
				    int slot)
{
	char e1[16];
	char e2[16];
	int i;
	int mcc;
	bool ok = false;
	bool hw_is_epoch;

	if (!hw_imsi || !out || len < 16)
		return false;
	out[0] = '\0';
	for (i = 0; i < 15; i++) {
		if (hw_imsi[i] < '0' || hw_imsi[i] > '9')
			return false;
	}
	if ((hw_imsi[0] == '3' && hw_imsi[1] == '5') ||
	    (hw_imsi[0] == '8' && hw_imsi[1] == '6')) {
		if (hw_imsi[14] == ghost_imei_luhn_digit(hw_imsi))
			return false;
	}
	mcc = (hw_imsi[0] - '0') * 100 + (hw_imsi[1] - '0') * 10 +
	      (hw_imsi[2] - '0');
	if (mcc < 200 || mcc > 799)
		return false;

	memset(e1, 0, sizeof(e1));
	memset(e2, 0, sizeof(e2));
	ghost_get_imsi_buf(e1, sizeof(e1));
	ghost_get_imsi2_buf(e2, sizeof(e2));
	if (strlen(e1) != 15 || strlen(e2) != 15)
		return false;

	hw_is_epoch = !memcmp(hw_imsi, e1, 15) || !memcmp(hw_imsi, e2, 15);
	if (slot == 0) {
		if (!memcmp(hw_imsi, e1, 15))
			return false;
		strscpy(out, e1, len);
		return true;
	}
	if (slot == 1) {
		if (!memcmp(hw_imsi, e2, 15))
			return false;
		strscpy(out, e2, len);
		return true;
	}
	if (hw_is_epoch)
		return false;

	spin_lock(&ghost_imei_seen_lock);
	if (!ghost_seen_hw_imsi1[0] || !memcmp(ghost_seen_hw_imsi1, hw_imsi, 15)) {
		memcpy(ghost_seen_hw_imsi1, hw_imsi, 15);
		ghost_seen_hw_imsi1[15] = '\0';
		strscpy(out, e1, len);
		ok = true;
	} else if (!ghost_seen_hw_imsi2[0] || !memcmp(ghost_seen_hw_imsi2, hw_imsi, 15)) {
		memcpy(ghost_seen_hw_imsi2, hw_imsi, 15);
		ghost_seen_hw_imsi2[15] = '\0';
		strscpy(out, e2, len);
		ok = true;
	} else {
		strscpy(out, e1, len);
		ok = true;
	}
	spin_unlock(&ghost_imei_seen_lock);
	return ok;
}
EXPORT_SYMBOL(ghost_select_cloaked_imsi_slot);

bool ghost_select_cloaked_iccid_slot(const char *hw_iccid, char *out, size_t len,
				     int slot)
{
	char e1[32];
	char e2[32];
	char c1[32];
	char c2[32];
	int n;
	bool ok = false;
	bool hw_is_epoch;

	if (!hw_iccid || !out || len < 21)
		return false;
	out[0] = '\0';
	n = 0;
	while (n < 20 && hw_iccid[n]) {
		if (hw_iccid[n] < '0' || hw_iccid[n] > '9')
			return false;
		n++;
	}
	if (n < 19 || n > 20)
		return false;
	if (hw_iccid[0] != '8' || hw_iccid[1] != '9')
		return false;

	memset(e1, 0, sizeof(e1));
	memset(e2, 0, sizeof(e2));
	memset(c1, 0, sizeof(c1));
	memset(c2, 0, sizeof(c2));
	ghost_get_iccid_buf(e1, sizeof(e1));
	ghost_get_iccid2_buf(e2, sizeof(e2));
	if (strlen(e1) < 19 || strlen(e2) < 19)
		return false;
	ghost_fit_iccid_len(c1, sizeof(c1), e1, n);
	ghost_fit_iccid_len(c2, sizeof(c2), e2, n);
	if ((int)strlen(c1) != n || (int)strlen(c2) != n)
		return false;

	hw_is_epoch = !memcmp(hw_iccid, c1, n) || !memcmp(hw_iccid, c2, n);
	if (slot == 0) {
		if (!memcmp(hw_iccid, c1, n))
			return false;
		strscpy(out, c1, len);
		return true;
	}
	if (slot == 1) {
		if (!memcmp(hw_iccid, c2, n))
			return false;
		strscpy(out, c2, len);
		return true;
	}
	if (hw_is_epoch)
		return false;

	spin_lock(&ghost_imei_seen_lock);
	if (!ghost_seen_hw_iccid1[0] || !memcmp(ghost_seen_hw_iccid1, hw_iccid, n)) {
		memcpy(ghost_seen_hw_iccid1, hw_iccid, n);
		ghost_seen_hw_iccid1[n] = '\0';
		strscpy(out, c1, len);
		ok = true;
	} else if (!ghost_seen_hw_iccid2[0] || !memcmp(ghost_seen_hw_iccid2, hw_iccid, n)) {
		memcpy(ghost_seen_hw_iccid2, hw_iccid, n);
		ghost_seen_hw_iccid2[n] = '\0';
		strscpy(out, c2, len);
		ok = true;
	} else {
		strscpy(out, c1, len);
		ok = true;
	}
	spin_unlock(&ghost_imei_seen_lock);
	return ok;
}
EXPORT_SYMBOL(ghost_select_cloaked_iccid_slot);

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

void ghost_get_ufs_serial_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	if (!buf || len == 0)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->ufs_serial[0])
		strscpy(buf, p->ufs_serial, len);
	else
		strscpy(buf, ghost_active_profile.ufs_serial, len);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_ufs_serial_buf);

void ghost_copy_wifi_mac(u8 *mac)
{
	struct ghost_profile *p;

	if (!mac)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p)
		memcpy(mac, p->wifi_mac, ETH_ALEN);
	else
		memcpy(mac, ghost_active_profile.wifi_mac, ETH_ALEN);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_copy_wifi_mac);

void ghost_copy_eth_addr_cloaked(u8 *dst, const u8 *src, unsigned int addr_len)
{
	if (!dst)
		return;
	if (src && addr_len)
		memcpy(dst, src, addr_len);
	if (addr_len == ETH_ALEN && ghost_should_cloak_untrusted(current))
		ghost_copy_wifi_mac(dst);
}
EXPORT_SYMBOL(ghost_copy_eth_addr_cloaked);

void ghost_get_active_wifi_mac_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	const u8 *m;
	if (!buf || len < 18)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	m = p ? p->wifi_mac : ghost_active_profile.wifi_mac;
	snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 m[0], m[1], m[2], m[3], m[4], m[5]);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_active_wifi_mac_buf);

void ghost_get_active_bt_mac_buf(char *buf, size_t len)
{
	struct ghost_profile *p;
	const u8 *m;
	if (!buf || len < 18)
		return;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	m = p ? p->bt_mac : ghost_active_profile.bt_mac;
	snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
		 m[0], m[1], m[2], m[3], m[4], m[5]);
	rcu_read_unlock();
}
EXPORT_SYMBOL(ghost_get_active_bt_mac_buf);

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

u32 ghost_get_uptime_days(void)
{
	struct ghost_profile *p;
	u32 val = 30;

	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->uptime_days > 0)
		val = p->uptime_days;
	else if (ghost_active_profile.uptime_days > 0)
		val = ghost_active_profile.uptime_days;
	rcu_read_unlock();
	return val;
}
EXPORT_SYMBOL_GPL(ghost_get_uptime_days);

u32 ghost_get_boot_count(void)
{
	struct ghost_profile *p;
	u32 val = 40;

	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->boot_count > 0)
		val = p->boot_count;
	else if (ghost_active_profile.boot_count > 0)
		val = ghost_active_profile.boot_count;
	rcu_read_unlock();
	return val;
}
EXPORT_SYMBOL_GPL(ghost_get_boot_count);

u32 ghost_get_tcp_isn_offset(void)
{
	struct ghost_profile *p;
	u32 val = 0x49614cb1;

	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p && p->tcp_isn_offset)
		val = p->tcp_isn_offset;
	else if (ghost_active_profile.tcp_isn_offset)
		val = ghost_active_profile.tcp_isn_offset;
	rcu_read_unlock();
	return val;
}
EXPORT_SYMBOL_GPL(ghost_get_tcp_isn_offset);

s32 ghost_get_baro_drift_hpa_x100(void)
{
	struct ghost_profile *p;
	s32 val = 0;

	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p)
		val = p->baro_drift_hpa_x100;
	else
		val = ghost_active_profile.baro_drift_hpa_x100;
	rcu_read_unlock();
	return val;
}
EXPORT_SYMBOL_GPL(ghost_get_baro_drift_hpa_x100);

void ghost_get_sensor_bias(s16 bias[3])
{
	struct ghost_profile *p;
	s16 src[3];

	if (!bias)
		return;
	src[0] = 0;
	src[1] = 0;
	src[2] = 0;
	rcu_read_lock();
	p = rcu_dereference(ghost_active_profile_ptr);
	if (p) {
		src[0] = p->sensor_bias[0];
		src[1] = p->sensor_bias[1];
		src[2] = p->sensor_bias[2];
	} else {
		src[0] = ghost_active_profile.sensor_bias[0];
		src[1] = ghost_active_profile.sensor_bias[1];
		src[2] = ghost_active_profile.sensor_bias[2];
	}
	rcu_read_unlock();
	bias[0] = src[0];
	bias[1] = src[1];
	bias[2] = src[2];
}
EXPORT_SYMBOL_GPL(ghost_get_sensor_bias);

void ghost_apply_accel_bias(s16 *x, s16 *y, s16 *z)
{
	s16 b[3];
	s32 t;

	if (!x || !y || !z)
		return;
	ghost_get_sensor_bias(b);
	t = (s32)(*x) + (s32)b[0];
	if (t > 32767)
		t = 32767;
	if (t < -32768)
		t = -32768;
	*x = (s16)t;
	t = (s32)(*y) + (s32)b[1];
	if (t > 32767)
		t = 32767;
	if (t < -32768)
		t = -32768;
	*y = (s16)t;
	t = (s32)(*z) + (s32)b[2];
	if (t > 32767)
		t = 32767;
	if (t < -32768)
		t = -32768;
	*z = (s16)t;
}
EXPORT_SYMBOL_GPL(ghost_apply_accel_bias);


void ghost_get_ufs_wwid_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u64 eui;

	if (!buf || len < 21)
		return;
	eui = ghost_mix64(uid, 0xEE10000000000083ULL);
	snprintf(buf, len, "eui.%016llx", eui);
}
EXPORT_SYMBOL(ghost_get_ufs_wwid_buf);

void ghost_get_camera_moduleid_buf(char *buf, size_t len, int cam_index)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix;
	u8 b[5];
	const char *pfx = "SVOGA";
	int i;

	if (!buf || len < 16)
		return;
	if (cam_index == 1 || cam_index == 3 || cam_index == 5 || cam_index == 7)
		pfx = "CVOGK";
	mix = ghost_mix64(uid, 0xCA000000ULL + (u64)(cam_index + 1) * 0x9E3779B185EBCA87ULL);
	for (i = 0; i < 5; i++)
		b[i] = (u8)(mix >> (i * 8));
	snprintf(buf, len, "%s%02X%02X%02X%02X%02X",
		 pfx, b[0], b[1], b[2], b[3], b[4]);
}
EXPORT_SYMBOL(ghost_get_camera_moduleid_buf);

void ghost_get_panel_cellid_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u64 m1 = ghost_mix64(uid, 0xCE11000B11610271ULL);
	u64 m2 = ghost_mix64(uid, 0xCE120008090BE90CULL);

	if (!buf || len < 23)
		return;
	snprintf(buf, len, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
		 (u8)m1, (u8)(m1 >> 8), (u8)(m1 >> 16), (u8)(m1 >> 24),
		 (u8)(m1 >> 32), (u8)(m1 >> 40), (u8)m2, (u8)(m2 >> 8),
		 (u8)(m2 >> 16), (u8)(m2 >> 24), (u8)(m2 >> 32));
}
EXPORT_SYMBOL(ghost_get_panel_cellid_buf);

void ghost_get_panel_octaid_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0x0C7A000000000001ULL);
	u8 site, rework, poc, x2, x3;
	char tail[17];
	int i;
	static const char hex[] = "0123456789ABCDEF";

	if (!buf || len < 24)
		return;
	site = (u8)(mix % 4);
	rework = (u8)((mix >> 8) % 2);
	poc = 1;
	x2 = (u8)(mix >> 16);
	x3 = (u8)(mix >> 24);
	for (i = 0; i < 16; i++)
		tail[i] = hex[(mix >> (i * 4)) & 0xF];
	tail[16] = '\0';
	snprintf(buf, len, "%u%u%u%02x%02x%s", site, rework, poc, x2, x3, tail);
}
EXPORT_SYMBOL(ghost_get_panel_octaid_buf);

void ghost_get_panel_manufacture_date(int *year, int *month, int *day,
				      int *hour, int *min)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0xDA7EULL);

	if (year)
		*year = 2022;
	if (month)
		*month = (int)(mix % 6ULL) + 1;
	if (day)
		*day = (int)((mix >> 8) % 28ULL) + 1;
	if (hour)
		*hour = (int)((mix >> 16) % 24ULL);
	if (min)
		*min = (int)((mix >> 24) % 60ULL);
}
EXPORT_SYMBOL(ghost_get_panel_manufacture_date);

void ghost_get_panel_ddi_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0xDD1C011DULL);

	if (!buf || len < 11)
		return;
	snprintf(buf, len, "%02X%02X%02X%02X%02X",
		 (u8)mix, (u8)(mix >> 8), (u8)(mix >> 16),
		 (u8)(mix >> 24), (u8)(mix >> 32));
}
EXPORT_SYMBOL(ghost_get_panel_ddi_buf);

void ghost_get_chip_lot_buf(char *buf, size_t len)
{
	static const char b36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0x1071DULL);
	char tmp[6];
	int i;

	if (!buf || len < 6)
		return;
	tmp[0] = 'N';
	for (i = 4; i >= 1; i--) {
		tmp[i] = b36[mix % 36ULL];
		mix /= 36ULL;
	}
	tmp[5] = '\0';
	strscpy(buf, tmp, len);
}
EXPORT_SYMBOL(ghost_get_chip_lot_buf);

void ghost_get_extra_info_id_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u32 n;

	if (!buf || len < 14)
		return;
	n = (u32)(ghost_mix64(uid, 0x1D000000ULL) % 1000000000ULL);
	snprintf(buf, len, "%09uRI25", n);
}
EXPORT_SYMBOL(ghost_get_extra_info_id_buf);

void ghost_get_asv_values(int *asv)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0xA5B1C2D3ULL);
	int i;

	if (!asv)
		return;
	for (i = 0; i < 5; i++)
		asv[i] = 1 + (int)((mix >> (i * 8)) % 5ULL);
	if (asv[3] > 3)
		asv[3] = 3;
}
EXPORT_SYMBOL(ghost_get_asv_values);

void ghost_get_ids_values(int *ids)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0x1D51D51DULL);
	int i;

	if (!ids)
		return;
	for (i = 0; i < 4; i++)
		ids[i] = 40 + (int)((mix >> (i * 12)) % 160ULL);
}
EXPORT_SYMBOL(ghost_get_ids_values);

void ghost_get_batt_qr_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0xBA770001ULL);
	u32 a, b;

	if (!buf || len < 29)
		return;
	a = (u32)(mix % 1000000ULL);
	b = (u32)((mix >> 20) % 1000000ULL);
	snprintf(buf, len, "GH43-05054A+SEC%06u+%06u", a, b);
}
EXPORT_SYMBOL(ghost_get_batt_qr_buf);

void ghost_get_pcb_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0x0CB00001ULL);

	if (!buf || len < 14)
		return;
	snprintf(buf, len, "A%07XA0P%02u",
		 (u32)(mix % 0x10000000ULL),
		 (u32)((mix >> 32) % 100ULL));
}
EXPORT_SYMBOL(ghost_get_pcb_buf);

void ghost_get_smd_date_buf(char *buf, size_t len)
{
	int year = 2022, month = 1, day = 1, hour = 12, min = 0;

	if (!buf || len < 9)
		return;
	ghost_get_panel_manufacture_date(&year, &month, &day, &hour, &min);
	snprintf(buf, len, "%04d%02d%02d", year, month, day);
}
EXPORT_SYMBOL(ghost_get_smd_date_buf);

void ghost_get_ufs_manf_date(u16 *out)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0xDA7E0F50ULL);
	u16 yy, ww;

	if (!out)
		return;
	yy = (u16)(0x16 + (mix % 7ULL));
	ww = (u16)(1 + ((mix >> 8) % 52ULL));
	*out = (u16)((yy << 8) | ww);
}
EXPORT_SYMBOL(ghost_get_ufs_manf_date);

void ghost_get_ufs_health(u8 *eol, u8 *life_a, u8 *life_b)
{
	u64 mix = ghost_mix64(ghost_get_active_unique_id(), 0x484C5448ULL);

	if (eol)
		*eol = 0;
	if (life_a)
		*life_a = 1 + (u8)(mix % 2ULL);
	if (life_b)
		*life_b = 1;
}
EXPORT_SYMBOL(ghost_get_ufs_health);

void ghost_get_ufs_unique_number_buf(char *buf, size_t len)
{
	char sn[32];
	const char *p;
	u16 md = 0;
	u64 mix;
	u32 serial = 0;
	int i;
	char c;
	u8 d;

	if (!buf || len < 21)
		return;
	memset(sn, 0, sizeof(sn));
	memset(buf, 0, len);
	ghost_get_ufs_serial_buf(sn, sizeof(sn));
	ghost_get_ufs_manf_date(&md);
	p = sn;
	if (sn[0] == '0' && (sn[1] == 'x' || sn[1] == 'X'))
		p = sn + 2;
	for (i = 0; i < 8 && p[i]; i++) {
		c = p[i];
		d = 0;
		if (c >= '0' && c <= '9')
			d = (u8)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (u8)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (u8)(c - 'A' + 10);
		serial = (serial << 4) | d;
	}
	mix = ghost_mix64(ghost_get_active_unique_id(), 0x5546534EULL);
	snprintf(buf, len, "CE%02X%02X%08X%02X%02X%02X",
		 (u8)(md >> 8), (u8)md, serial,
		 (u8)mix, (u8)(mix >> 8), (u8)(mix >> 16));
}
EXPORT_SYMBOL(ghost_get_ufs_unique_number_buf);

u32 ghost_get_ufs_flt(void)
{
	u64 mix = ghost_mix64(ghost_get_active_unique_id(), 0x464C5401ULL);

	return (u32)(mix % 3ULL);
}
EXPORT_SYMBOL(ghost_get_ufs_flt);

u32 ghost_get_cable_count(void)
{
	u64 mix = ghost_mix64(ghost_get_active_unique_id(), 0xCAB1E001ULL);

	return 40 + (u32)(mix % 180ULL);
}
EXPORT_SYMBOL(ghost_get_cable_count);

u64 ghost_get_ufs_transferred_bytes(void)
{
	u64 mix = ghost_mix64(ghost_get_active_unique_id(), 0x54524E53ULL);
	u32 days = ghost_get_uptime_days();
	u64 per_day;

	per_day = (8ULL * 1024ULL * 1024ULL) + (mix % (24ULL * 1024ULL * 1024ULL));
	if (days < 12)
		days = 12;
	return (u64)days * per_day;
}
EXPORT_SYMBOL(ghost_get_ufs_transferred_bytes);

bool ghost_get_cloaked_efs_payload(const char *dname, const char *pname,
				   char *out, size_t out_len, size_t *out_plen)
{
	char tmp[40];
	size_t n = 0;
	u32 num;

	if (!dname || !out || out_len < 4 || !out_plen)
		return false;
	*out_plen = 0;
	memset(tmp, 0, sizeof(tmp));
	memset(out, 0, out_len);

	if (!strcmp(dname, "ghost_serial.txt") ||
	    (pname && !strcmp(pname, "FactoryApp") && !strcmp(dname, "serial_no"))) {
		ghost_get_active_serial_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 11)
			return false;
	} else if (pname && !strcmp(pname, "bluetooth") && !strcmp(dname, "bt_addr")) {
		ghost_get_active_bt_mac_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 17)
			return false;
	} else if (pname && !strcmp(pname, "wifi") && !strcmp(dname, ".mac.info")) {
		ghost_get_active_wifi_mac_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 17)
			return false;
	} else if (pname && (!strcmp(pname, "imei") || !strcmp(pname, "FactoryApp")) &&
		   (!strcmp(dname, "imei") || !strcmp(dname, "imei1") ||
		    !strcmp(dname, ".imei"))) {
		ghost_get_imei_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 15)
			return false;
	} else if (pname && (!strcmp(pname, "imei") || !strcmp(pname, "FactoryApp")) &&
		   !strcmp(dname, "imei2")) {
		ghost_get_imei2_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 15)
			return false;
	} else if (pname && !strcmp(pname, "FactoryApp") && !strcmp(dname, "HwParamBattQR")) {
		ghost_get_batt_qr_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 28)
			return false;
	} else if (pname && !strcmp(pname, "FactoryApp") && !strcmp(dname, "control_no")) {
		ghost_get_pcb_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 13)
			return false;
	} else if (pname && !strcmp(pname, "FactoryApp") && !strcmp(dname, "HwPartSMDDate")) {
		ghost_get_smd_date_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 8)
			return false;
	} else if (pname && !strcmp(pname, "FactoryApp") && !strcmp(dname, "asoc")) {
		num = ghost_get_battery_health();
		if (num < 10 || num > 99)
			num = 95;
		snprintf(tmp, sizeof(tmp), "%u\n", num);
		n = strlen(tmp);
		if (n != 3)
			return false;
	} else if (pname && !strcmp(pname, "FactoryApp") &&
		   !strcmp(dname, "batt_after_manufactured")) {
		num = ghost_get_battery_cycle();
		if (num > 999)
			num = 999;
		snprintf(tmp, sizeof(tmp), "%03u", num);
		n = strlen(tmp);
		if (n != 3)
			return false;
	} else if (pname && !strcmp(pname, "FactoryApp") && !strcmp(dname, "eID")) {
		ghost_get_eid_buf(tmp, sizeof(tmp));
		n = strlen(tmp);
		if (n != 32)
			return false;
	} else {
		return false;
	}

	if (n == 0 || n >= out_len)
		return false;
	memcpy(out, tmp, n);
	*out_plen = n;
	return true;
}
EXPORT_SYMBOL(ghost_get_cloaked_efs_payload);

static void ghost_replace_quoted_field(char *buf, size_t len,
				      const char *key, const char *newval)
{
	char needle[40];
	char *p;
	char *q;
	size_t keylen, nlen, vlen, oldlen;

	if (!buf || !key || !newval || len == 0)
		return;
	keylen = strlen(key);
	if (keylen == 0 || keylen > 24)
		return;
	needle[0] = '"';
	memcpy(needle + 1, key, keylen);
	needle[1 + keylen] = '"';
	needle[2 + keylen] = ':';
	needle[3 + keylen] = '"';
	needle[4 + keylen] = '\0';
	nlen = keylen + 4;
	vlen = strlen(newval);
	if (nlen >= sizeof(needle) || vlen == 0)
		return;
	p = buf;
	while (p + nlen < buf + len) {
		if (!memcmp(p, needle, nlen)) {
			p += nlen;
			q = p;
			while (q < buf + len && *q != '"')
				q++;
			if (q >= buf + len)
				return;
			oldlen = (size_t)(q - p);
			if (oldlen == vlen)
				memcpy(p, newval, vlen);
			p = q + 1;
			continue;
		}
		p++;
	}
}

static void ghost_sanitize_identity_json(char *buf, size_t len)
{
	char serial[16];
	char imei1[16];
	char imei2[16];
	char uniq[20];
	char ufsun[24];
	char rear[20];
	char rear2[20];
	char rear3[20];
	char front[20];
	char cell[24];
	char octa[28];
	char ddi[16];
	char *p;

	if (!buf || len == 0)
		return;
	memset(serial, 0, sizeof(serial));
	memset(imei1, 0, sizeof(imei1));
	memset(imei2, 0, sizeof(imei2));
	memset(uniq, 0, sizeof(uniq));
	memset(ufsun, 0, sizeof(ufsun));
	memset(rear, 0, sizeof(rear));
	memset(rear2, 0, sizeof(rear2));
	memset(rear3, 0, sizeof(rear3));
	memset(front, 0, sizeof(front));
	memset(cell, 0, sizeof(cell));
	memset(octa, 0, sizeof(octa));
	memset(ddi, 0, sizeof(ddi));
	ghost_get_active_serial_buf(serial, sizeof(serial));
	ghost_get_imei_buf(imei1, sizeof(imei1));
	ghost_get_imei2_buf(imei2, sizeof(imei2));
	ghost_get_ufs_unique_number_buf(ufsun, sizeof(ufsun));
	ghost_get_camera_moduleid_buf(rear, sizeof(rear), 0);
	ghost_get_camera_moduleid_buf(front, sizeof(front), 1);
	ghost_get_camera_moduleid_buf(rear2, sizeof(rear2), 2);
	ghost_get_camera_moduleid_buf(rear3, sizeof(rear3), 4);
	ghost_get_panel_cellid_buf(cell, sizeof(cell));
	ghost_get_panel_octaid_buf(octa, sizeof(octa));
	ghost_get_panel_ddi_buf(ddi, sizeof(ddi));
	snprintf(uniq, sizeof(uniq), "%016llX",
		 (unsigned long long)ghost_get_active_unique_id());
	ghost_replace_quoted_field(buf, len, "serialNumber", serial);
	ghost_replace_quoted_field(buf, len, "deviceID", imei2);
	ghost_replace_quoted_field(buf, len, "uniqueNumber", ufsun);
	ghost_replace_quoted_field(buf, len, "rootingFlag", "N");
	ghost_replace_quoted_field(buf, len, "SVC_AP", uniq);
	ghost_replace_quoted_field(buf, len, "SVC_front_module", front);
	ghost_replace_quoted_field(buf, len, "SVC_rear_module", rear);
	ghost_replace_quoted_field(buf, len, "SVC_rear_module2", rear2);
	ghost_replace_quoted_field(buf, len, "SVC_rear_module3", rear3);
	ghost_replace_quoted_field(buf, len, "SVC_OCTA", cell);
	ghost_replace_quoted_field(buf, len, "SVC_OCTA_CHIPID", octa);
	ghost_replace_quoted_field(buf, len, "SVC_OCTA_DDI_CHIPID", ddi);
	p = buf;
	while (p + 18 <= buf + len) {
		if (!memcmp(p, "\"changeList\":\"fail", 18)) {
			memcpy(p + 14, "pass", 4);
			break;
		}
		p++;
	}
}

void ghost_sanitize_hwparam_blob(char *buf, size_t len)
{
	char pcb[16];
	char smd[12];
	char qr[32];
	char qr29[32];
	char cell[24];
	char model[32];
	char padded[20];
	size_t mlen;
	u64 uid;
	u64 mix;
	u32 a;

	if (!buf || len == 0)
		return;
	memset(pcb, 0, sizeof(pcb));
	memset(smd, 0, sizeof(smd));
	memset(qr, 0, sizeof(qr));
	memset(qr29, 0, sizeof(qr29));
	memset(cell, 0, sizeof(cell));
	memset(model, 0, sizeof(model));
	memset(padded, 0, sizeof(padded));
	ghost_get_pcb_buf(pcb, sizeof(pcb));
	ghost_get_smd_date_buf(smd, sizeof(smd));
	ghost_get_batt_qr_buf(qr, sizeof(qr));
	ghost_get_panel_cellid_buf(cell, sizeof(cell));
	ghost_get_ufs_model_buf(model, sizeof(model));
	uid = ghost_get_active_unique_id();
	mix = ghost_mix64(uid, 0xBA770001ULL);
	a = (u32)(mix % 1000000ULL);
	snprintf(qr29, sizeof(qr29), "GH43-05054A+SEC%06u+XXXXXXX", a);
	ghost_replace_quoted_field(buf, len, "PCB", pcb);
	ghost_replace_quoted_field(buf, len, "SMD", smd);
	ghost_replace_quoted_field(buf, len, "SMD_DATE", smd);
	ghost_replace_quoted_field(buf, len, "BATTQR", qr);
	ghost_replace_quoted_field(buf, len, "BATTQR", qr29);
	ghost_replace_quoted_field(buf, len, "OCTA_CELL_ID", cell);
	ghost_replace_quoted_field(buf, len, "REV", "0100");
	mlen = strlen(model);
	if (mlen > 0 && mlen <= 16) {
		memset(padded, ' ', 16);
		memcpy(padded, model, mlen);
		padded[16] = '\0';
		ghost_replace_quoted_field(buf, len, "PNM", padded);
		ghost_replace_quoted_field(buf, len, "PNM", model);
	}
	ghost_sanitize_identity_json(buf, len);
}
EXPORT_SYMBOL(ghost_sanitize_hwparam_blob);

void ghost_get_eid_buf(char *buf, size_t len)
{
	u64 uid = ghost_get_active_unique_id();
	u64 m1 = ghost_mix64(uid, 0xE1D00001ULL);
	u64 m2 = ghost_mix64(uid, 0xE1D00002ULL);
	char tmp[36];

	if (!buf || len < 33)
		return;
	memset(tmp, 0, sizeof(tmp));
	snprintf(tmp, sizeof(tmp), "8904%014llu%014llu",
		 (unsigned long long)(m1 % 100000000000000ULL),
		 (unsigned long long)(m2 % 100000000000000ULL));
	tmp[32] = '\0';
	memcpy(buf, tmp, 33);
}
EXPORT_SYMBOL(ghost_get_eid_buf);

void ghost_get_asb_psite(int *asb, int *psite)
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix = ghost_mix64(uid, 0xA5B0517EULL);

	if (asb)
		*asb = 16 + (int)(mix % 5ULL);
	if (psite)
		*psite = 1 + (int)((mix >> 8) % 4ULL);
}
EXPORT_SYMBOL(ghost_get_asb_psite);

void ghost_cloak_sensorid_exif(void *id, size_t len, int cam_index)
{
	u8 *p = id;
	u64 uid;
	u64 mix;
	int i;
	u8 keep0, keep1, keep8;

	if (!p || len < 16)
		return;
	keep0 = p[0];
	keep1 = p[1];
	keep8 = p[8];
	uid = ghost_get_active_unique_id();
	mix = ghost_mix64(uid, 0x5E05001DULL + (u64)cam_index * 0x10008ULL);
	for (i = 0; i < 16; i++) {
		if (i == 0 || i == 1 || i == 8)
			continue;
		if (p[i] == 0 || p[i] == 0xff)
			continue;
		p[i] = (u8)(mix >> ((i & 7) * 8));
		mix = ghost_mix64(mix, (u64)i + 1ULL);
	}
	p[0] = keep0;
	p[1] = keep1;
	p[8] = keep8;
}
EXPORT_SYMBOL(ghost_cloak_sensorid_exif);

static void ghost_sanitize_jhist(char *buf, size_t len)
{
	char date[12];
	int year = 2022, month = 1, day = 1, hour = 12, min = 0;
	char *p;

	if (!buf || len < 10)
		return;
	ghost_get_panel_manufacture_date(&year, &month, &day, &hour, &min);
	snprintf(date, sizeof(date), "%04d-%02d-%02d", year, month, day);
	p = buf;
	while (p + 10 <= buf + len) {
		if (p[0] == '2' && p[1] == '0' && p[4] == '-' && p[7] == '-' &&
		    p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9') {
			memcpy(p, date, 10);
			p += 10;
			continue;
		}
		p++;
	}
}

static void ghost_sanitize_gyro_cal(char *buf, size_t len)
{
	char cell[24];
	char *p;
	int i;
	int hex;

	if (!buf || len < 22)
		return;
	memset(cell, 0, sizeof(cell));
	ghost_get_panel_cellid_buf(cell, sizeof(cell));
	if (strlen(cell) != 22)
		return;
	p = buf + len - 22;
	hex = 1;
	for (i = 0; i < 22; i++) {
		if (!((p[i] >= '0' && p[i] <= '9') ||
		      (p[i] >= 'A' && p[i] <= 'F') ||
		      (p[i] >= 'a' && p[i] <= 'f'))) {
			hex = 0;
			break;
		}
	}
	if (hex)
		memcpy(p, cell, 22);
}

static void ghost_overwrite_after_key(char *buf, size_t len,
				     const char *key, const char *val, int nth)
{
	size_t klen, vlen;
	char *p;
	int hit = 0;

	if (!buf || !key || !val || len == 0)
		return;
	klen = strlen(key);
	vlen = strlen(val);
	if (klen == 0 || vlen == 0 || klen + vlen > len)
		return;
	p = buf;
	while (p + klen + vlen <= buf + len) {
		if (!memcmp(p, key, klen)) {
			if (!strcmp(key, "SVCm:") && (p[klen] == '2' || p[klen] == '3')) {
				p++;
				continue;
			}
			if (nth < 0 || hit == nth) {
				memcpy(p + klen, val, vlen);
				if (nth >= 0)
					return;
			}
			hit++;
			p += klen + vlen;
			continue;
		}
		p++;
	}
}

static void ghost_sanitize_svc_blob(char *buf, size_t len)
{
	char serial[16];
	char imei1[16];
	char imei2[16];
	char uniq[20];
	char rear[20];
	char rear2[20];
	char rear3[20];
	char front[20];
	char cell[24];
	char octa[28];
	char ddi[16];
	char manf[8];
	u16 md = 0;
	u64 uid;

	if (!buf || len == 0)
		return;
	memset(serial, 0, sizeof(serial));
	memset(imei1, 0, sizeof(imei1));
	memset(imei2, 0, sizeof(imei2));
	memset(uniq, 0, sizeof(uniq));
	memset(rear, 0, sizeof(rear));
	memset(rear2, 0, sizeof(rear2));
	memset(rear3, 0, sizeof(rear3));
	memset(front, 0, sizeof(front));
	memset(cell, 0, sizeof(cell));
	memset(octa, 0, sizeof(octa));
	memset(ddi, 0, sizeof(ddi));
	memset(manf, 0, sizeof(manf));
	ghost_get_active_serial_buf(serial, sizeof(serial));
	ghost_get_imei_buf(imei1, sizeof(imei1));
	ghost_get_imei2_buf(imei2, sizeof(imei2));
	ghost_get_camera_moduleid_buf(rear, sizeof(rear), 0);
	ghost_get_camera_moduleid_buf(front, sizeof(front), 1);
	ghost_get_camera_moduleid_buf(rear2, sizeof(rear2), 2);
	ghost_get_camera_moduleid_buf(rear3, sizeof(rear3), 4);
	ghost_get_panel_cellid_buf(cell, sizeof(cell));
	ghost_get_panel_octaid_buf(octa, sizeof(octa));
	ghost_get_panel_ddi_buf(ddi, sizeof(ddi));
	uid = ghost_get_active_unique_id();
	snprintf(uniq, sizeof(uniq), "%016llX", (unsigned long long)uid);
	ghost_get_ufs_manf_date(&md);
	snprintf(manf, sizeof(manf), "%04X", md);
	ghost_overwrite_after_key(buf, len, "ID:", imei1, 0);
	ghost_overwrite_after_key(buf, len, "ID:", imei2, 1);
	if (serial[0] == 'R')
		ghost_overwrite_after_key(buf, len, "Nm:R", serial + 1, -1);
	ghost_overwrite_after_key(buf, len, "SVCAP:", uniq, -1);
	ghost_overwrite_after_key(buf, len, "SVCOCTADDICHIPID:", ddi, -1);
	ghost_overwrite_after_key(buf, len, "SVCOCTACHIPID:", octa, -1);
	ghost_overwrite_after_key(buf, len, "SVCOCTA:", cell, -1);
	ghost_overwrite_after_key(buf, len, "SVCm3:", rear3, -1);
	ghost_overwrite_after_key(buf, len, "SVCm2:", rear2, -1);
	ghost_overwrite_after_key(buf, len, "SVCm:", front, 0);
	ghost_overwrite_after_key(buf, len, "SVCm:", rear, 1);
	{
		char *p = buf;
		size_t nlen = 4;

		while (p + nlen <= buf + len) {
			if (!memcmp(p, "965D", 4) || !memcmp(p, "965d", 4)) {
				memcpy(p, manf, 4);
				p += 4;
				continue;
			}
			p++;
		}
	}
}

void ghost_sanitize_efs_blob(const char *dname, const char *pname,
			    char *buf, size_t len)
{
	if (!dname || !buf || len == 0)
		return;
	if (pname && !strcmp(pname, "FactoryApp")) {
		if (!strcmp(dname, "HwParamData") || !strcmp(dname, "HwPartInform"))
			ghost_sanitize_hwparam_blob(buf, len);
		else if (!strcmp(dname, "jhist_nv"))
			ghost_sanitize_jhist(buf, len);
		else if (!strcmp(dname, "gyro_cal_data"))
			ghost_sanitize_gyro_cal(buf, len);
	} else if (pname && !strcmp(pname, "sec_efs") &&
		   (!strcmp(dname, "SVC") || !strcmp(dname, "!SVC"))) {
		ghost_sanitize_svc_blob(buf, len);
		ghost_sanitize_identity_json(buf, len);
	} else if (pname && !strcmp(pname, "sec_efs") &&
		   !strcmp(dname, "SettingsBackup.json")) {
		ghost_sanitize_identity_json(buf, len);
	}
}
EXPORT_SYMBOL(ghost_sanitize_efs_blob);


void ghost_get_panel_maid_date_buf(char *buf, size_t len)
{
	int year = 2022, month = 1, day = 1, hour = 12, min = 0;

	if (!buf || len < 16)
		return;
	ghost_get_panel_manufacture_date(&year, &month, &day, &hour, &min);
	snprintf(buf, len, "%04d%02d%02d %02d%02d00", year, month, day, hour, min);
}
EXPORT_SYMBOL(ghost_get_panel_maid_date_buf);

void ghost_cloak_vpd_page(int page, unsigned char *data, size_t len)
{
	char sn[32] = {0};
	u64 eui;
	size_t i, n;
	const char *payload;

	if (!data || len < 4)
		return;

	if (page == 0x80) {
		ghost_get_ufs_serial_buf(sn, sizeof(sn));
		payload = sn;
		if (payload[0] == '0' && (payload[1] == 'x' || payload[1] == 'X'))
			payload += 2;
		n = strlen(payload);
		if (n > len - 4)
			n = len - 4;
		if (len >= 4) {
			data[0] = 0x00;
			data[1] = 0x80;
			data[2] = 0x00;
			data[3] = (u8)n;
		}
		for (i = 0; i < n; i++)
			data[4 + i] = (unsigned char)payload[i];
		for (i = 4 + n; i < len; i++)
			data[i] = ' ';
	} else if (page == 0x83) {
		eui = ghost_mix64(ghost_get_active_unique_id(), 0xEE10000000000083ULL);
		if (len >= 16) {
			data[0] = 0x00;
			data[1] = 0x83;
			data[2] = 0x00;
			data[3] = 0x0C;
			data[4] = 0x01;
			data[5] = 0x02;
			data[6] = 0x00;
			data[7] = 0x08;
			data[8] = (eui >> 56) & 0xFF;
			data[9] = (eui >> 48) & 0xFF;
			data[10] = (eui >> 40) & 0xFF;
			data[11] = (eui >> 32) & 0xFF;
			data[12] = (eui >> 24) & 0xFF;
			data[13] = (eui >> 16) & 0xFF;
			data[14] = (eui >> 8) & 0xFF;
			data[15] = eui & 0xFF;
		}
	}
}
EXPORT_SYMBOL(ghost_cloak_vpd_page);

void ghost_mask_fsid(int fsid_val[2])
{
	u64 uid = ghost_get_active_unique_id();
	u64 mix;

	if (!fsid_val || !uid)
		return;
	mix = ghost_mix64(uid, 0xF51DF51DF51DF51DULL);
	fsid_val[0] ^= (int)mix;
	fsid_val[1] ^= (int)(mix >> 32);
}
EXPORT_SYMBOL(ghost_mask_fsid);

static int ghost_imei_proc_show(struct seq_file *m, void *v)
{
	char imei1[32] = "", imei2[32] = "";
	struct ghost_profile *p;

	if (current_uid().val >= 2000) {
		seq_printf(m, "status: active\n");
		return 0;
	}

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

	proc_create_single("ghost_config", 0440, NULL, ghost_config_proc_show);
	proc_create_single("ghost_imei", 0440, NULL, ghost_imei_proc_show);
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

#define GHOST_WIFI_BSS_MAX 64

struct ghost_wifi_bss_ent {
	u8 bssid[ETH_ALEN];
	u8 ssid[32];
	u8 ssid_len;
	u8 has_bssid;
};

static DEFINE_SPINLOCK(ghost_wifi_bss_lock);
static struct ghost_wifi_bss_ent ghost_wifi_bss[GHOST_WIFI_BSS_MAX];
static struct ghost_wifi_bss_ent ghost_wifi_connected;
static unsigned int ghost_wifi_bss_pos;

bool ghost_should_cloak_untrusted(struct task_struct *task)
{
	char cmd[192];
	int n;
	char *cut;

	if (!task)
		return false;
	if (task_uid(task).val < 10000)
		return false;
	memset(cmd, 0, sizeof(cmd));
	n = get_cmdline(task, cmd, (int)sizeof(cmd) - 1);
	if (n < 0)
		n = 0;
	if (n >= (int)sizeof(cmd))
		n = (int)sizeof(cmd) - 1;
	cmd[n] = 0;
	cut = cmd;
	while (*cut && *cut != ' ' && *cut != '\t')
		cut++;
	*cut = 0;
	if (!strncmp(cmd, "com.google.", 11))
		return false;
	if (!strncmp(cmd, "com.netflix.", 12))
		return false;
	if (!strncmp(cmd, "com.android.vending", 19))
		return false;
	if (!strncmp(cmd, "com.android.systemui", 20))
		return false;
	if (!strncmp(cmd, "com.android.chrome", 18))
		return false;
	if (!strncmp(cmd, "com.android.webview", 19))
		return false;
	return true;
}
EXPORT_SYMBOL(ghost_should_cloak_untrusted);

static bool ghost_task_is_feniks(struct task_struct *task)
{
	char cmd[192];
	int n;
	char *cut;

	if (!task)
		return false;
	if (task_uid(task).val < 10000)
		return false;
	memset(cmd, 0, sizeof(cmd));
	n = get_cmdline(task, cmd, (int)sizeof(cmd) - 1);
	if (n < 0)
		n = 0;
	if (n >= (int)sizeof(cmd))
		n = (int)sizeof(cmd) - 1;
	cmd[n] = 0;
	cut = cmd;
	while (*cut && *cut != ' ' && *cut != '\t')
		cut++;
	*cut = 0;
	if (!strncmp(cmd, "com.shopee.", 11))
		return true;
	if (strstr(cmd, "feniks"))
		return true;
	return false;
}

bool ghost_path_is_root_leak(const char *path)
{
	static const char *const exact[] = {
		"/system/bin/su",
		"/system/xbin/su",
		"/sbin/su",
		"/vendor/bin/su",
		"/system/bin/ksu",
		"/system/bin/ksud",
		"/data/adb",
		"/data/adb/ksu",
		"/data/adb/ksud",
		"/data/adb/modules",
		"/data/adb/magisk",
		"/debug_ramdisk",
		"/dev/ksu",
		"/proc/ghost_config",
		"/proc/ghost_imei",
		"/proc/ghost_reload",
		"/proc/ghost_utsname",
		"/proc/kallsyms",
		"su",
		NULL
	};
	static const char *const pref[] = {
		"/data/adb/",
		"/debug_ramdisk/",
		"/sbin/.magisk",
		NULL
	};
	int i;
	size_t n;

	if (!path || !path[0])
		return false;
	for (i = 0; exact[i]; i++) {
		if (!strcmp(path, exact[i]))
			return true;
	}
	for (i = 0; pref[i]; i++) {
		n = strlen(pref[i]);
		if (!strncmp(path, pref[i], n))
			return true;
	}
	return false;
}
EXPORT_SYMBOL(ghost_path_is_root_leak);

bool ghost_should_hide_user_path(const char __user *name)
{
	char path[160];
	long n;

	if (!name || !ghost_task_is_feniks(current))
		return false;
	memset(path, 0, sizeof(path));
	n = strncpy_from_user(path, name, sizeof(path) - 1);
	if (n <= 0)
		return false;
	path[sizeof(path) - 1] = 0;
	return ghost_path_is_root_leak(path);
}
EXPORT_SYMBOL(ghost_should_hide_user_path);



bool ghost_selinux_relax_serialno_prop(const char *type_name)
{
	return type_name && !strcmp(type_name, "serialno_prop");
}
EXPORT_SYMBOL(ghost_selinux_relax_serialno_prop);


void ghost_fill_drm_id(u8 *out, size_t len)
{
	u64 a, b, uid;
	size_t i;

	if (!out || !len)
		return;
	uid = ghost_get_active_unique_id();
	a = ghost_mix64(uid, 0x44524D4944310001ULL);
	b = ghost_mix64(uid, 0x44524D4944310002ULL);
	for (i = 0; i < len; i++) {
		if ((i & 7) == 0)
			a = ghost_mix64(a, b + i);
		out[i] = (u8)(a >> ((i & 7) * 8));
	}
}
EXPORT_SYMBOL(ghost_fill_drm_id);

static char ghost_hex_digit(u8 v)
{
	v &= 0xf;
	return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

void ghost_fill_gaid(char *out, size_t len)
{
	u8 raw[16];
	u64 uid, a;
	size_t i;
	int o;

	if (!out || len < 37)
		return;
	uid = ghost_get_active_unique_id();
	for (i = 0; i < 16; i++) {
		a = ghost_mix64(uid, 0x474149441D000001ULL + (u64)i);
		raw[i] = (u8)a;
	}
	raw[6] = (raw[6] & 0x0f) | 0x40;
	raw[8] = (raw[8] & 0x3f) | 0x80;
	o = 0;
	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[o++] = '-';
		out[o++] = ghost_hex_digit(raw[i] >> 4);
		out[o++] = ghost_hex_digit(raw[i]);
	}
	out[o] = 0;
}
EXPORT_SYMBOL(ghost_fill_gaid);


int ghost_cloak_drm_reply_bytes(u8 *pkt, size_t len)
{
	u32 exc;
	u32 alen;
	u32 maybe;
	size_t off;
	size_t data_off;

	if (!pkt || !len)
		return 0;
	if (len < 12 || len > 8192)
		return 0;
	memcpy(&exc, pkt, 4);
	if (exc != 0)
		return 0;
	for (off = 4; off + 8 <= len && off <= 16; off += 4) {
		memcpy(&alen, pkt + off, 4);
		if (alen != 16 && alen != 32 && alen != 64)
			continue;
		data_off = off + 4;
		if (data_off + alen > len)
			continue;
		if (data_off + 4 + alen <= len &&
		    (data_off + alen) < len) {
			memcpy(&maybe, pkt + data_off, 4);
			if (maybe == 0 || maybe == 1)
				data_off += 4;
		}
		if (data_off + alen > len)
			continue;
		ghost_fill_drm_id(pkt + data_off, alen);
		return (int)alen;
	}
	return 0;
}
EXPORT_SYMBOL(ghost_cloak_drm_reply_bytes);

static void ghost_map_ssid_same_len(const u8 *in, size_t n, u8 *out)
{
	static const char alphabet[] =
	    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
	u64 h;
	size_t i;
	int alen = (int)(sizeof(alphabet) - 1);

	if (!in || !out || !n)
		return;
	h = ghost_mix64(ghost_get_active_unique_id(), 0x535349445F4D4150ULL);
	for (i = 0; i < n; i++) {
		h = ghost_mix64(h, (u64)in[i] + 0x9e + i);
		if (in[i] == ' ' || in[i] == '-' || in[i] == '_' ||
		    in[i] == '.' || in[i] == '\'')
			out[i] = in[i];
		else if (in[i] >= 32 && in[i] < 127)
			out[i] = (u8)alphabet[h % (u64)alen];
		else
			out[i] = in[i];
	}
}

static void ghost_map_bssid_bytes(const u8 *in, u8 *out)
{
	u64 h, x;
	int i;

	if (!in || !out)
		return;
	x = 0;
	for (i = 0; i < ETH_ALEN; i++)
		x = (x << 8) | in[i];
	h = ghost_mix64(ghost_get_active_unique_id() ^ x, 0x42535349445F4DULL);
	out[0] = (u8)((h >> 40) & 0xfe) | 0x02;
	out[1] = (u8)(h >> 32);
	out[2] = (u8)(h >> 24);
	out[3] = (u8)(h >> 16);
	out[4] = (u8)(h >> 8);
	out[5] = (u8)h;
}

static int ghost_hexval_c(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void ghost_format_bssid_str(const u8 *mac, char *out, int upper)
{
	static const char *L = "0123456789abcdef";
	static const char *U = "0123456789ABCDEF";
	const char *h;
	int i;

	h = upper ? U : L;
	for (i = 0; i < 6; i++) {
		out[i * 3] = h[mac[i] >> 4];
		out[i * 3 + 1] = h[mac[i] & 0xf];
		if (i < 5)
			out[i * 3 + 2] = ':';
	}
	out[17] = 0;
}

static void ghost_format_bssid_compact(const u8 *mac, char *out, int upper)
{
	static const char *L = "0123456789abcdef";
	static const char *U = "0123456789ABCDEF";
	const char *h;
	int i;

	h = upper ? U : L;
	for (i = 0; i < 6; i++) {
		out[i * 2] = h[mac[i] >> 4];
		out[i * 2 + 1] = h[mac[i] & 0xf];
	}
	out[12] = 0;
}

static void ghost_format_bssid_dash(const u8 *mac, char *out, int upper)
{
	ghost_format_bssid_str(mac, out, upper);
	out[2] = out[5] = out[8] = out[11] = out[14] = '-';
}

static int ghost_replace_mem(u8 *pkt, int len, const u8 *oldv, const u8 *neu, int n)
{
	int i, c;

	c = 0;
	if (!pkt || !oldv || !neu || n <= 0 || n > len)
		return 0;
	if (!memcmp(oldv, neu, n))
		return 0;
	for (i = 0; i + n <= len; i++) {
		if (!memcmp(pkt + i, oldv, n)) {
			memcpy(pkt + i, neu, n);
			c++;
			i += n - 1;
		}
	}
	return c;
}

static void ghost_expand_u16(const u8 *s, int n, u8 *out)
{
	int i;

	for (i = 0; i < n; i++) {
		out[i * 2] = s[i];
		out[i * 2 + 1] = 0;
	}
}

static int ghost_replace_utf8_utf16(u8 *pkt, int len, const u8 *s, int n, const u8 *d)
{
	u8 old16[64];
	u8 new16[64];
	int c;

	c = 0;
	if (n <= 0 || n > 32)
		return 0;
	c += ghost_replace_mem(pkt, len, s, d, n);
	ghost_expand_u16(s, n, old16);
	ghost_expand_u16(d, n, new16);
	c += ghost_replace_mem(pkt, len, old16, new16, n * 2);
	return c;
}

static int ghost_is_uuid_str(const u8 *s)
{
	int i;
	char c;

	if (!s)
		return 0;
	for (i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (s[i] != '-')
				return 0;
			continue;
		}
		c = (char)s[i];
		if (!isxdigit(c))
			return 0;
	}
	return 1;
}

int ghost_cloak_gaid_reply_bytes(u8 *pkt, size_t len)
{
	char gaid[40];
	u8 g16[72];
	u8 tmp[36];
	int i, j, c;

	if (!pkt || len < 36 || len > 4096)
		return 0;
	ghost_fill_gaid(gaid, sizeof(gaid));
	c = 0;
	for (i = 0; i + 36 <= (int)len; i++) {
		if (ghost_is_uuid_str(pkt + i) && memcmp(pkt + i, gaid, 36)) {
			memcpy(pkt + i, gaid, 36);
			c++;
			i += 35;
			break;
		}
	}
	ghost_expand_u16((const u8 *)gaid, 36, g16);
	for (i = 0; i + 72 <= (int)len; i++) {
		for (j = 0; j < 36; j++) {
			if (pkt[i + j * 2 + 1])
				break;
			tmp[j] = pkt[i + j * 2];
		}
		if (j != 36)
			continue;
		if (ghost_is_uuid_str(tmp) && memcmp(pkt + i, g16, 72)) {
			memcpy(pkt + i, g16, 72);
			c++;
			i += 71;
			break;
		}
	}
	return c;
}
EXPORT_SYMBOL(ghost_cloak_gaid_reply_bytes);


void ghost_wifi_note_bss(const u8 *bssid, const u8 *ssid, u8 ssid_len)
{
	unsigned long flags;
	int i, slot;
	struct ghost_wifi_bss_ent *e;

	if (ssid_len > 32)
		ssid_len = 32;
	if (bssid && (is_broadcast_ether_addr(bssid) ||
		      is_multicast_ether_addr(bssid) ||
		      is_zero_ether_addr(bssid)))
		bssid = NULL;
	if (!bssid && (!ssid || !ssid_len))
		return;

	spin_lock_irqsave(&ghost_wifi_bss_lock, flags);
	slot = -1;
	for (i = 0; i < GHOST_WIFI_BSS_MAX; i++) {
		e = &ghost_wifi_bss[i];
		if (bssid && e->has_bssid &&
		    !memcmp(e->bssid, bssid, ETH_ALEN)) {
			slot = i;
			break;
		}
		if (!bssid && ssid && ssid_len && e->ssid_len == ssid_len &&
		    !memcmp(e->ssid, ssid, ssid_len) && !e->has_bssid) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = (int)(ghost_wifi_bss_pos % GHOST_WIFI_BSS_MAX);
		ghost_wifi_bss_pos++;
		memset(&ghost_wifi_bss[slot], 0, sizeof(*e));
	}
	e = &ghost_wifi_bss[slot];
	if (bssid) {
		memcpy(e->bssid, bssid, ETH_ALEN);
		e->has_bssid = 1;
	}
	if (ssid && ssid_len) {
		memcpy(e->ssid, ssid, ssid_len);
		e->ssid_len = ssid_len;
	}
	spin_unlock_irqrestore(&ghost_wifi_bss_lock, flags);
}
EXPORT_SYMBOL(ghost_wifi_note_bss);

void ghost_wifi_note_connected(const u8 *bssid, const u8 *ssid, u8 ssid_len)
{
	unsigned long flags;

	ghost_wifi_note_bss(bssid, ssid, ssid_len);
	if (ssid_len > 32)
		ssid_len = 32;
	if (bssid && (is_broadcast_ether_addr(bssid) ||
		      is_multicast_ether_addr(bssid) ||
		      is_zero_ether_addr(bssid)))
		bssid = NULL;
	if (!bssid && (!ssid || !ssid_len))
		return;

	spin_lock_irqsave(&ghost_wifi_bss_lock, flags);
	memset(&ghost_wifi_connected, 0, sizeof(ghost_wifi_connected));
	if (bssid) {
		memcpy(ghost_wifi_connected.bssid, bssid, ETH_ALEN);
		ghost_wifi_connected.has_bssid = 1;
	}
	if (ssid && ssid_len) {
		memcpy(ghost_wifi_connected.ssid, ssid, ssid_len);
		ghost_wifi_connected.ssid_len = ssid_len;
	}
	spin_unlock_irqrestore(&ghost_wifi_bss_lock, flags);
}
EXPORT_SYMBOL(ghost_wifi_note_connected);

int ghost_wifi_has_notes(void)
{
	int i, n;
	unsigned long flags;

	n = 0;
	spin_lock_irqsave(&ghost_wifi_bss_lock, flags);
	if (ghost_wifi_connected.ssid_len || ghost_wifi_connected.has_bssid)
		n++;
	for (i = 0; i < GHOST_WIFI_BSS_MAX; i++) {
		if (ghost_wifi_bss[i].ssid_len || ghost_wifi_bss[i].has_bssid)
			n++;
	}
	spin_unlock_irqrestore(&ghost_wifi_bss_lock, flags);
	return n;
}
EXPORT_SYMBOL(ghost_wifi_has_notes);

int ghost_wifi_cloak_bytes(u8 *pkt, size_t len)
{
	struct ghost_wifi_bss_ent copy[GHOST_WIFI_BSS_MAX];
	struct ghost_wifi_bss_ent connected;
	struct ghost_wifi_bss_ent *e;
	unsigned long flags;
	int i, changed, n;
	u8 fake_ssid[32];
	u8 fake_mac[ETH_ALEN];
	char real_str[18];
	char fake_str[18];
	u8 qold[34];
	u8 qnew[34];

	if (!pkt || len < 4 || len > 65536)
		return 0;
	spin_lock_irqsave(&ghost_wifi_bss_lock, flags);
	memcpy(copy, ghost_wifi_bss, sizeof(copy));
	connected = ghost_wifi_connected;
	spin_unlock_irqrestore(&ghost_wifi_bss_lock, flags);

	changed = 0;
	for (i = -1; i < GHOST_WIFI_BSS_MAX; i++) {
		e = (i < 0) ? &connected : &copy[i];
		if (e->ssid_len >= 1 && e->ssid_len <= 32) {
			ghost_map_ssid_same_len(e->ssid, e->ssid_len, fake_ssid);
			changed += ghost_replace_utf8_utf16(pkt, (int)len,
							    e->ssid, e->ssid_len,
							    fake_ssid);
			if (e->ssid_len <= 30) {
				qold[0] = '"';
				memcpy(qold + 1, e->ssid, e->ssid_len);
				qold[1 + e->ssid_len] = '"';
				qnew[0] = '"';
				memcpy(qnew + 1, fake_ssid, e->ssid_len);
				qnew[1 + e->ssid_len] = '"';
				changed += ghost_replace_utf8_utf16(pkt, (int)len,
								    qold,
								    e->ssid_len + 2,
								    qnew);
			}
		}
		if (e->has_bssid) {
			ghost_map_bssid_bytes(e->bssid, fake_mac);
			ghost_format_bssid_str(e->bssid, real_str, 0);
			ghost_format_bssid_str(fake_mac, fake_str, 0);
			changed += ghost_replace_utf8_utf16(pkt, (int)len,
							    (u8 *)real_str, 17,
							    (u8 *)fake_str);
			ghost_format_bssid_str(e->bssid, real_str, 1);
			ghost_format_bssid_str(fake_mac, fake_str, 1);
			changed += ghost_replace_utf8_utf16(pkt, (int)len,
							    (u8 *)real_str, 17,
							    (u8 *)fake_str);
			ghost_format_bssid_compact(e->bssid, real_str, 0);
			ghost_format_bssid_compact(fake_mac, fake_str, 0);
			changed += ghost_replace_utf8_utf16(pkt, (int)len,
							    (u8 *)real_str, 12,
							    (u8 *)fake_str);
			ghost_format_bssid_compact(e->bssid, real_str, 1);
			ghost_format_bssid_compact(fake_mac, fake_str, 1);
			changed += ghost_replace_utf8_utf16(pkt, (int)len,
							    (u8 *)real_str, 12,
							    (u8 *)fake_str);
			ghost_format_bssid_dash(e->bssid, real_str, 0);
			ghost_format_bssid_dash(fake_mac, fake_str, 0);
			changed += ghost_replace_utf8_utf16(pkt, (int)len,
							    (u8 *)real_str, 17,
							    (u8 *)fake_str);
			ghost_format_bssid_dash(e->bssid, real_str, 1);
			ghost_format_bssid_dash(fake_mac, fake_str, 1);
			changed += ghost_replace_utf8_utf16(pkt, (int)len,
							    (u8 *)real_str, 17,
							    (u8 *)fake_str);
		}
	}
	n = changed;
	return n;
}
EXPORT_SYMBOL(ghost_wifi_cloak_bytes);

static char ghost_twist_char(char c, u64 h)
{
	unsigned k;

	if (c >= 'A' && c <= 'Z') {
		k = (unsigned)(h % 25u) + 1u;
		return (char)('A' + ((unsigned)(c - 'A') + k) % 26u);
	}
	if (c >= 'a' && c <= 'z') {
		k = (unsigned)(h % 25u) + 1u;
		return (char)('a' + ((unsigned)(c - 'a') + k) % 26u);
	}
	if (c >= '0' && c <= '9') {
		k = (unsigned)(h % 9u) + 1u;
		return (char)('0' + ((unsigned)(c - '0') + k) % 10u);
	}
	return c;
}

static void ghost_epoch_twist_token(char *dst, const char *src, int n, u64 salt)
{
	u64 h;
	int i;

	if (!dst || !src || n <= 0)
		return;
	h = ghost_mix64(ghost_get_active_unique_id() ^ salt, 0x53454E535F4D4150ULL);
	for (i = 0; i < n; i++) {
		h = ghost_mix64(h, 0x100ULL + (u64)i);
		dst[i] = ghost_twist_char(src[i], h);
	}
	dst[n] = 0;
}

int ghost_sensor_cloak_bytes(u8 *pkt, size_t len)
{
	static const char *const tokens[] = {
		"LPS22HHTR", "AK09918C", "LSM6DSO", "TMD4912", "ISG5320A",
		"SITRON", "BOSCH", "STM", "AKM", "AMS", "TDK", "IMAGIS",
		"Palm Proximity Sensor version 2",
	};
	char to[64];
	unsigned i;
	int c, n;

	c = 0;
	if (!pkt || !len)
		return 0;
	for (i = 0; i < ARRAY_SIZE(tokens); i++) {
		n = (int)strlen(tokens[i]);
		if (n <= 0 || n >= (int)sizeof(to))
			continue;
		ghost_epoch_twist_token(to, tokens[i], n, 0x53454E0001ULL + i);
		if (!memcmp(to, tokens[i], n))
			continue;
		c += ghost_replace_utf8_utf16(pkt, (int)len,
					      (const u8 *)tokens[i], n,
					      (const u8 *)to);
	}
	return c;
}
EXPORT_SYMBOL(ghost_sensor_cloak_bytes);

void ghost_cloak_sensor_text(char *buf, size_t len)
{
	if (!buf || !len)
		return;
	if (!ghost_should_cloak_untrusted(current))
		return;
	ghost_sensor_cloak_bytes((u8 *)buf, len);
}
EXPORT_SYMBOL(ghost_cloak_sensor_text);

ssize_t ghost_sysfs_print_sensor_text(char *buf, const char *text)
{
	ssize_t n;

	if (!buf)
		return 0;
	if (!text)
		text = "";
	n = sprintf(buf, "%s\n", text);
	if (n > 0)
		ghost_cloak_sensor_text(buf, (size_t)n);
	return n;
}
EXPORT_SYMBOL(ghost_sysfs_print_sensor_text);
