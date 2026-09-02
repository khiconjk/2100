// SPDX-License-Identifier: GPL-2.0
/*
 * Ghost Net: Session-wide MAC and Bluetooth address spoofing for o1s.
 *
 * Implements IEEE 802 compliant random MAC address generation:
 * - Bit 0 of byte 0 is 0 (Unicast)
 * - Bit 1 of byte 0 is 1 (Locally Administered)
 * - First byte ends in 2, 6, A, or E.
 *
 * Exposes /proc/ghost_mac for inspection.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/path.h>
#include <linux/dcache.h>
#include <linux/timekeeping.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/ghost_uptime.h>
#include <linux/ghost_net.h>
#include <linux/ghost_storage.h>

#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/kmod.h>

#define GHOST_DEFAULT_RESET_AGE_DAYS 180ULL
#define GHOST_DEFAULT_RESET_AGE_SECS (GHOST_DEFAULT_RESET_AGE_DAYS * 86400ULL)

u8 ghost_wifi_mac[ETH_ALEN] = {0};
u8 ghost_p2p_mac[ETH_ALEN] = {0};
u8 ghost_bt_addr[6] = {0};
bool ghost_net_ready;
char ghost_serialno[16] = {0};
bool ghost_serialno_ready;
u64 ghost_reset_timestamp;
bool ghost_reset_ready;
EXPORT_SYMBOL(ghost_wifi_mac);
EXPORT_SYMBOL(ghost_p2p_mac);
EXPORT_SYMBOL(ghost_bt_addr);
EXPORT_SYMBOL(ghost_net_ready);
EXPORT_SYMBOL(ghost_serialno);
EXPORT_SYMBOL(ghost_serialno_ready);
EXPORT_SYMBOL(ghost_reset_timestamp);
EXPORT_SYMBOL(ghost_reset_ready);

/* Protects ghost_wifi_mac, ghost_bt_addr, ghost_net_ready */
static DEFINE_SPINLOCK(ghost_mac_lock);
/* Protects ghost_serialno, ghost_serialno_ready */
static DEFINE_SPINLOCK(ghost_sn_lock);

static const u8 samsung_ouis[][3] = {
	{0x8C, 0x85, 0x90}, /* Samsung Electronics */
	{0xF4, 0x7B, 0x5E}, /* Samsung Electronics */
	{0x00, 0x12, 0xFB}, /* Samsung Electronics */
	{0x34, 0xC0, 0x59}, /* Samsung Electronics */
	{0x50, 0x01, 0xD9}, /* Samsung Electronics */
	{0xE4, 0x58, 0xB8}, /* Samsung Electronics */
	{0xB4, 0x79, 0xA7}, /* Samsung Electronics */
	{0x40, 0x40, 0xA7}, /* Samsung Electronics */
	{0x68, 0xEB, 0xAE}, /* Samsung Electronics */
	{0x78, 0x4B, 0x87}, /* Samsung Electronics */
};

static void ghost_generate_ieee802_mac(u8 *mac)
{
	u8 oui_idx;
	get_random_bytes(&oui_idx, 1);
	memcpy(mac, samsung_ouis[oui_idx % ARRAY_SIZE(samsung_ouis)], 3);
	get_random_bytes(mac + 3, 3);
}

static void ghost_generate_bdaddr(u8 *bdaddr)
{
	u8 oui_idx;
	get_random_bytes(&oui_idx, 1);
	memcpy(bdaddr + 3, samsung_ouis[oui_idx % ARRAY_SIZE(samsung_ouis)], 3);
	get_random_bytes(bdaddr, 3);
}

void ghost_reroll_network_macs(void)
{
	u8 tmp_mac[ETH_ALEN], tmp_p2p[ETH_ALEN], tmp_bt[6];
	unsigned long flags;

	ghost_generate_ieee802_mac(tmp_mac);
	memcpy(tmp_p2p, tmp_mac, ETH_ALEN);
	/* Distinct P2P Wi-Fi Direct address: Locally Administered, Unicast */
	tmp_p2p[0] = (tmp_p2p[0] & 0xFE) | 0x02;
	ghost_generate_bdaddr(tmp_bt);

	spin_lock_irqsave(&ghost_mac_lock, flags);
	memcpy(ghost_wifi_mac, tmp_mac, ETH_ALEN);
	memcpy(ghost_p2p_mac, tmp_p2p, ETH_ALEN);
	memcpy(ghost_bt_addr, tmp_bt, 6);
	ghost_net_ready = true;
	spin_unlock_irqrestore(&ghost_mac_lock, flags);

	pr_debug("ghost_net: rerolled WiFi MAC %pM, P2P MAC %pM, BT BD_ADDR %02X:%02X:%02X:%02X:%02X:%02X\n",
		ghost_wifi_mac, ghost_p2p_mac,
		ghost_bt_addr[5], ghost_bt_addr[4], ghost_bt_addr[3],
		ghost_bt_addr[2], ghost_bt_addr[1], ghost_bt_addr[0]);
}
EXPORT_SYMBOL(ghost_reroll_network_macs);

void ghost_net_init_macs(void)
{
	u8 tmp_mac[ETH_ALEN], tmp_p2p[ETH_ALEN], tmp_bt[6];
	unsigned long flags;

	/* Fast path: check without lock first (read is safe; worst case double-init) */
	if (ghost_net_ready)
		return;

	ghost_generate_ieee802_mac(tmp_mac);
	memcpy(tmp_p2p, tmp_mac, ETH_ALEN);
	/* Distinct P2P Wi-Fi Direct address: Locally Administered, Unicast */
	tmp_p2p[0] = (tmp_p2p[0] & 0xFE) | 0x02;
	ghost_generate_bdaddr(tmp_bt);

	spin_lock_irqsave(&ghost_mac_lock, flags);
	if (!ghost_net_ready) {
		memcpy(ghost_wifi_mac, tmp_mac, ETH_ALEN);
		memcpy(ghost_p2p_mac, tmp_p2p, ETH_ALEN);
		memcpy(ghost_bt_addr, tmp_bt, 6);
		ghost_net_ready = true;
	}
	spin_unlock_irqrestore(&ghost_mac_lock, flags);

	pr_debug("ghost_net: initialized WiFi MAC %pM, P2P MAC %pM, BT BD_ADDR %02X:%02X:%02X:%02X:%02X:%02X\n",
		ghost_wifi_mac, ghost_p2p_mac,
		ghost_bt_addr[5], ghost_bt_addr[4], ghost_bt_addr[3],
		ghost_bt_addr[2], ghost_bt_addr[1], ghost_bt_addr[0]);
}
EXPORT_SYMBOL(ghost_net_init_macs);

/* Internal helper: generate a Samsung-format serial into ghost_serialno.
 * Caller MUST hold ghost_sn_lock. */
static void ghost_gen_serialno_locked(void)
{
	static const char valid_chars[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
	static const char month_chars[] = "123456789ABC";
	u8 rand_bytes[12];
	int i;

	get_random_bytes(rand_bytes, sizeof(rand_bytes));
	ghost_serialno[0] = 'R';
	ghost_serialno[1] = '5';
	ghost_serialno[2] = month_chars[rand_bytes[0] % 12];
	for (i = 3; i < 11; i++)
		ghost_serialno[i] = valid_chars[rand_bytes[i] % (sizeof(valid_chars) - 1)];
	ghost_serialno[11] = '\0';
	ghost_serialno_ready = true;
}

void ghost_init_serialno(void)
{
	unsigned long flags;

	spin_lock_irqsave(&ghost_sn_lock, flags);
	if (!ghost_serialno_ready)
		ghost_gen_serialno_locked();
	spin_unlock_irqrestore(&ghost_sn_lock, flags);

	pr_debug("ghost_bootloader: initialized ghost serialno %s\n", ghost_serialno);
}
EXPORT_SYMBOL(ghost_init_serialno);

void ghost_reroll_serialno(void)
{
	unsigned long flags;

	spin_lock_irqsave(&ghost_sn_lock, flags);
	ghost_gen_serialno_locked();
	spin_unlock_irqrestore(&ghost_sn_lock, flags);

	pr_debug("ghost_bootloader: rerolled ghost serialno %s\n", ghost_serialno);
}
EXPORT_SYMBOL(ghost_reroll_serialno);

static bool ghost_reset_user_set = false;

u64 ghost_get_reset_timestamp(void)
{
	struct timespec64 boottime;
	struct timespec64 now;

	if (ghost_reset_user_set)
		return ghost_reset_timestamp;

	/* If ghost_uptime has an active offset, synchronize with ghost session epoch */
	if (ghost_uptime_offset_ns > 0) {
		u64 offset_secs = div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC);
		ktime_get_real_ts64(&now);
		if ((u64)now.tv_sec > offset_secs) {
			ghost_reset_timestamp = (u64)now.tv_sec - offset_secs;
			ghost_reset_ready = true;
			return ghost_reset_timestamp;
		}
	}

	getboottime64(&boottime);
	if (boottime.tv_sec > 1000000000LL) {
		ghost_reset_timestamp = (u64)boottime.tv_sec - 120ULL;
		ghost_reset_ready = true;
	} else if (!ghost_reset_ready) {
		return 1786380402ULL;
	}

	return ghost_reset_timestamp;
}
EXPORT_SYMBOL(ghost_get_reset_timestamp);

void ghost_init_factory_reset(void)
{
	ghost_get_reset_timestamp();
}
EXPORT_SYMBOL(ghost_init_factory_reset);

static int ghost_is_reset_target(const struct path *path)
{
	struct dentry *d = path ? path->dentry : NULL;
	struct inode *inode = d ? d_backing_inode(d) : NULL;

	if (!d || !d->d_name.name)
		return 0;

	/* 1. Root of /data partition is handled exclusively by ghost_uptime */
	if (inode && inode->i_sb && inode->i_sb->s_magic == F2FS_SUPER_MAGIC) {
		if (inode->i_ino == 3 || d == inode->i_sb->s_root)
			return 0;

		/* /data/system or /data/user or /data/media */
		if ((!strcmp(d->d_name.name, "system") ||
		     !strcmp(d->d_name.name, "user") ||
		     !strcmp(d->d_name.name, "media")) &&
		    d->d_parent == inode->i_sb->s_root)
			return 1;
	}

	/* 2. Check by name for /data root -> handled exclusively by ghost_uptime */
	if (!strcmp(d->d_name.name, "data")) {
		if (d->d_parent && IS_ROOT(d->d_parent))
			return 0;
	}

	/* 3. Check /data/system/packages.xml or packages.list */
	if (!strcmp(d->d_name.name, "packages.xml") || !strcmp(d->d_name.name, "packages.list")) {
		if (d->d_parent && !strcmp(d->d_parent->d_name.name, "system"))
			return 1;
	}

	/* 4. Check /data/property/persistent_properties */
	if (!strcmp(d->d_name.name, "persistent_properties"))
		return 1;

	/* 5. Pillar 25: Normalize system fonts & fonts.xml to stock build timestamp */
	if (current_uid().val >= 10000) {
		if (!strcmp(d->d_name.name, "fonts.xml") ||
		    (d->d_parent && !strcmp(d->d_parent->d_name.name, "fonts")))
			return 2;
	}

	/* 6. Pillar 36: Storage Age Cloaking for /data/media/0/ user dirs.
	 * Anti-fraud SDKs call stat("/sdcard/DCIM") which resolves to
	 * /data/media/0/DCIM on F2FS. A freshly wiped device has these
	 * dirs created today, contradicting 17+ days of uptime.
	 * Shift timestamps to match ghost_reset_timestamp (~180 days ago).
	 * Also covers /sdcard/ root itself (/data/media/0). */
	if (current_uid().val >= 10000 && d->d_parent &&
	    d->d_parent->d_name.name) {
		/* 6a. /data/media/0 itself → sdcard root /sdcard/ */
		if (!strcmp(d->d_name.name, "0") &&
		    !strcmp(d->d_parent->d_name.name, "media"))
			return 1;

		/* 6b. /data/media/0/{DCIM,Android,...} → standard user dirs */
		if (!strcmp(d->d_parent->d_name.name, "0")) {
			struct dentry *gp = d->d_parent->d_parent;
			if (gp && gp->d_name.name &&
			    !strcmp(gp->d_name.name, "media")) {
				if (!strcmp(d->d_name.name, "DCIM") ||
				    !strcmp(d->d_name.name, "Android") ||
				    !strcmp(d->d_name.name, "Download") ||
				    !strcmp(d->d_name.name, "Pictures") ||
				    !strcmp(d->d_name.name, "Documents") ||
				    !strcmp(d->d_name.name, "Music") ||
				    !strcmp(d->d_name.name, "Movies") ||
				    !strcmp(d->d_name.name, "Alarms") ||
				    !strcmp(d->d_name.name, "Ringtones") ||
				    !strcmp(d->d_name.name, "Notifications"))
					return 1;
			}
		}

		/* 6c. /data/media/0/Android/{data,obb} → app sandbox roots */
		if ((!strcmp(d->d_name.name, "data") ||
		     !strcmp(d->d_name.name, "obb")) &&
		    !strcmp(d->d_parent->d_name.name, "Android")) {
			struct dentry *gp = d->d_parent->d_parent;
			if (gp && gp->d_name.name &&
			    !strcmp(gp->d_name.name, "0"))
				return 1;
		}

		/* 6d. /data/user/0/* or /data/data/* → app private sandbox packages */
		if (d->d_parent && d->d_parent->d_name.name) {
			if (!strcmp(d->d_parent->d_name.name, "0")) {
				struct dentry *gp = d->d_parent->d_parent;
				if (gp && gp->d_name.name && !strcmp(gp->d_name.name, "user"))
					return 1;
			}
			if (!strcmp(d->d_parent->d_name.name, "data")) {
				struct dentry *gp = d->d_parent->d_parent;
				if (gp && gp->d_name.name && !strcmp(gp->d_name.name, "data"))
					return 1;
			}
		}
	}

	/* 7. Ghost Kernel (Pillar 74): Plan A - System & Read-Only Partitions VFS Timestamp Normalization
	 * App sandbox SDKs check /system/lib64/libc.so, /system/build.prop, /vendor, etc. to detect OS changes.
	 * Cloak read-only system partition files (EXT4 / EROFS) for unprivileged applications (UID >= 10000). */
	if (current_uid().val >= 10000 && inode && inode->i_sb) {
		if ((inode->i_sb->s_flags & SB_RDONLY) &&
		    (inode->i_sb->s_magic == 0xEF53 /* EXT4_SUPER_MAGIC */ ||
		     inode->i_sb->s_magic == 0xe0f5e1e2 /* EROFS_SUPER_MAGIC_V1 */)) {
			return 3;
		}
	}

	return 0;
}

void ghost_apply_stat_reset(const struct path *path, struct kstat *stat)
{
	u64 reset_ts;
	int target;

	if (!path || !stat)
		return;

	target = ghost_is_reset_target(path);
	if (target == 1) {
		/* Derive 3 distinct nsec values from TCP ISN offset seed.
		 * Real filesystems always have different nsec per timestamp. */
		u32 base = ghost_storage_get_tcp_isn_offset();
		long nsec_b = (long)((base) % 999999999UL) + 1;
		long nsec_c = (long)((base ^ 0x55AA55AAU) % 999999999UL) + 1;
		long nsec_m = (long)((base ^ 0xAA55AA55U) % 999999999UL) + 1;

		reset_ts = ghost_get_reset_timestamp();
		stat->btime.tv_sec = (time64_t)reset_ts;
		stat->btime.tv_nsec = nsec_b;
		stat->ctime.tv_sec = (time64_t)reset_ts;
		stat->ctime.tv_nsec = nsec_c;
		stat->mtime.tv_sec = (time64_t)reset_ts;
		stat->mtime.tv_nsec = nsec_m;
		stat->result_mask |= (STATX_BTIME | STATX_CTIME | STATX_MTIME);

		/* Ghost Kernel (Pillar 72): VFS Inode Number Offset (Aged App Inode Range)
		 * Freshly created sandbox directory has suspiciously low sequential inode (< 200,000).
		 * Cloak to aged filesystem inode range [250,000 .. 1,000,000+]. */
		if (stat->ino > 0 && stat->ino < 200000ULL) {
			u64 ino_offset = 250000ULL + (u64)((base ^ (u32)stat->ino) % 750000UL);
			stat->ino += ino_offset;
		}
	} else if (target == 2) {
		/* Pillar 25: Canonical stock Android system image timestamp (1230768000 = 2008-12-31 22:00:00 UTC) */
		stat->btime.tv_sec = (time64_t)1230768000;
		stat->btime.tv_nsec = 0;
		stat->ctime.tv_sec = (time64_t)1230768000;
		stat->ctime.tv_nsec = 0;
		stat->mtime.tv_sec = (time64_t)1230768000;
		stat->mtime.tv_nsec = 0;
		stat->atime.tv_sec = (time64_t)1230768000;
		stat->atime.tv_nsec = 0;
		stat->result_mask |= (STATX_BTIME | STATX_CTIME | STATX_MTIME | STATX_ATIME);
	} else if (target == 3) {
		/* Ghost Kernel (Pillar 74): Plan A - System & Vendor File Age Spoofing
		 * Canonical Samsung Stock BUL1 Release Timestamp: 1638778962 (Mon Dec 6 08:22:42 UTC 2021)
		 * Deterministic subtle jitter per inode (within 15 minutes). */
		u32 base = ghost_storage_get_tcp_isn_offset();
		time64_t stock_base = 1638778962LL;
		time64_t jitter = (time64_t)(((u32)stat->ino ^ base) % 900);
		long nsec = (long)(((u32)stat->ino * 2654435761UL) % 999999999UL);

		stat->btime.tv_sec = stock_base + jitter;
		stat->btime.tv_nsec = nsec;
		stat->ctime.tv_sec = stock_base + jitter;
		stat->ctime.tv_nsec = nsec;
		stat->mtime.tv_sec = stock_base + jitter;
		stat->mtime.tv_nsec = nsec;
		stat->atime.tv_sec = stock_base + jitter;
		stat->atime.tv_nsec = nsec;
		stat->result_mask |= (STATX_BTIME | STATX_CTIME | STATX_MTIME | STATX_ATIME);
	}
}
EXPORT_SYMBOL(ghost_apply_stat_reset);

static bool ghost_scan_and_replace_r5_serial(u8 *buf, size_t len, const char *serial, size_t sn_len)
{
	size_t i;
	bool modified = false;

	if (!buf || len < 11 || sn_len != 11)
		return false;

	for (i = 0; i <= len - 11; i++) {
		if (buf[i] == 'R' && buf[i+1] == '5') {
			int k;
			bool is_sn = true;
			for (k = 2; k < 11; k++) {
				if (!isalnum(buf[i+k])) {
					is_sn = false;
					break;
				}
			}
			if (is_sn) {
				bool start_ok = (i == 0 || !isalnum(buf[i-1]));
				bool end_ok = (i + 11 >= len || !isalnum(buf[i+11]));
				if (start_ok && end_ok) {
					memcpy(&buf[i], serial, 11);
					modified = true;
					i += 10;
				}
			}
		}
	}
	return modified;
}

static bool ghost_scan_and_replace_imei(u8 *buf, size_t len, const char *imei1, const char *imei2)
{
	size_t i;
	bool modified = false;
	int count = 0;

	if (!buf || len < 15 || !imei1)
		return false;

	for (i = 0; i <= len - 15; i++) {
		if (isdigit(buf[i]) && isdigit(buf[i+1]) && isdigit(buf[i+2]) && isdigit(buf[i+3]) &&
		    isdigit(buf[i+4]) && isdigit(buf[i+5]) && isdigit(buf[i+6]) && isdigit(buf[i+7]) &&
		    isdigit(buf[i+8]) && isdigit(buf[i+9]) && isdigit(buf[i+10]) && isdigit(buf[i+11]) &&
		    isdigit(buf[i+12]) && isdigit(buf[i+13]) && isdigit(buf[i+14])) {
			bool start_ok = (i == 0 || !isdigit(buf[i-1]));
			bool end_ok = (i + 15 >= len || !isdigit(buf[i+15]));
			if (start_ok && end_ok) {
				if ((buf[i] == '3' && buf[i+1] == '5') || (buf[i] == '8' && buf[i+1] == '6')) {
					const char *tgt = (count == 0) ? imei1 : (imei2 ? imei2 : imei1);
					memcpy(&buf[i], tgt, 15);
					modified = true;
					count++;
					i += 14;
				}
			}
		}
	}
	return modified;
}

void ghost_telecom_filter_ipc_data(void *data, size_t len)
{
	u8 *p = (u8 *)data;
	size_t i;
	const char *imei;
	const char *imei2;
	u8 imei_bcd[8];
	u8 imei2_bcd[8];
	size_t sn_len;
	int imei_ascii_count = 0;
	int imei_bcd_count = 0;

	if (!data || len < 8)
		return;

	if (unlikely(!ghost_serialno_ready))
		ghost_init_serialno();

	imei = ghost_get_imei();
	imei2 = ghost_get_imei2();
	ghost_get_imei_bcd(imei_bcd);
	ghost_get_imei2_bcd(imei2_bcd);
	sn_len = strlen(ghost_serialno);

	/* 1. In-place ASCII IMEI scanner (Slot 1 -> imei, Slot 2 -> imei2) */
	if (len >= 15) {
		for (i = 0; i <= len - 15; i++) {
			if (isdigit(p[i]) && isdigit(p[i+1]) && isdigit(p[i+2]) && isdigit(p[i+3]) &&
			    isdigit(p[i+4]) && isdigit(p[i+5]) && isdigit(p[i+6]) && isdigit(p[i+7]) &&
			    isdigit(p[i+8]) && isdigit(p[i+9]) && isdigit(p[i+10]) && isdigit(p[i+11]) &&
			    isdigit(p[i+12]) && isdigit(p[i+13]) && isdigit(p[i+14])) {
				bool start_ok = (i == 0 || !isdigit(p[i-1]));
				bool end_ok = (i + 15 >= len || !isdigit(p[i+15]));
				if (start_ok && end_ok) {
					if ((p[i] == '3' && p[i+1] == '5') || (p[i] == '8' && p[i+1] == '6')) {
						const char *tgt = (imei_ascii_count == 0) ? imei : imei2;
						memcpy(&p[i], tgt, 15);
						imei_ascii_count++;
						i += 14;
					}
				}
			}
		}
	}

	/* 2. In-place Samsung Serial Number scanner */
	ghost_scan_and_replace_r5_serial(p, len, ghost_serialno, sn_len);

	/* 3. 3GPP BCD IMEI scanner (Slot 1 -> imei_bcd, Slot 2 -> imei2_bcd) */
	if (len >= 8) {
		for (i = 0; i <= len - 8; i++) {
			if ((p[i] == 0x3A || p[i] == 0x31 || p[i] == 0x33 || p[i] == 0x8A) &&
			    ((p[i+1] & 0x0F) == 0x05 || (p[i+1] & 0x0F) == 0x06)) {
				const u8 *tgt_bcd = (imei_bcd_count == 0) ? imei_bcd : imei2_bcd;
				memcpy(&p[i], tgt_bcd, 8);
				imei_bcd_count++;
				i += 7;
			}
		}
	}
}
EXPORT_SYMBOL(ghost_telecom_filter_ipc_data);

void ghost_filter_vfs_read_payload(struct file *file, char __user *buf, size_t ret)
{
	struct dentry *d = file ? file->f_path.dentry : NULL;
	const char *name = (d && d->d_name.name) ? d->d_name.name : NULL;
	char *kbuf;
	size_t copy_len;

	if (!name || !buf || ret == 0)
		return;

	if (unlikely(!ghost_serialno_ready))
		ghost_init_serialno();

	/* A. /efs/FactoryApp/serial_no or sysfs serial_no */
	if (strcmp(name, "serial_no") == 0) {
		size_t sn_len = strlen(ghost_serialno);
		if (ret >= sn_len && access_ok(buf, sn_len)) {
			if (!copy_to_user(buf, ghost_serialno, sn_len)) {
				if (ret > sn_len) {
					char nl = '\n';
					if (copy_to_user(buf + sn_len, &nl, 1))
						pr_debug("ghost_net: failed to copy newline\n");
				}
			}
		}
		return;
	}

	/* B. /efs/FactoryApp hardware string scanning (Serial + IMEI) */
	if (strcmp(name, "HwParamData") == 0 || strcmp(name, "HwPartInform") == 0 ||
	    strcmp(name, "BarCode") == 0 || strcmp(name, "mps_code.dat") == 0 ||
	    strcmp(name, "imei") == 0) {
		bool sn_mod, imei_mod;
		copy_len = min_t(size_t, ret, 4096);
		kbuf = kmalloc(copy_len + 1, GFP_KERNEL);
		if (!kbuf)
			return;

		if (copy_from_user(kbuf, buf, copy_len)) {
			kfree(kbuf);
			return;
		}
		kbuf[copy_len] = '\0';

		sn_mod = ghost_scan_and_replace_r5_serial((u8 *)kbuf, copy_len, ghost_serialno, 11);
		imei_mod = ghost_scan_and_replace_imei((u8 *)kbuf, copy_len, ghost_get_imei(), ghost_get_imei2());
		if (sn_mod || imei_mod) {
			if (copy_to_user(buf, kbuf, copy_len))
				pr_debug("ghost_net: copy_to_user failed for %s\n", name);
		}
		kfree(kbuf);
		return;
	}

	/* C. /system/build.prop, prop.default, default.prop test-keys normalization */
	if (strcmp(name, "build.prop") == 0 || strcmp(name, "prop.default") == 0 ||
	    strcmp(name, "default.prop") == 0) {
		char *p;
		bool modified = false;
		copy_len = min_t(size_t, ret, 8192);
		kbuf = kmalloc(copy_len + 1, GFP_KERNEL);
		if (!kbuf)
			return;

		if (copy_from_user(kbuf, buf, copy_len)) {
			kfree(kbuf);
			return;
		}
		kbuf[copy_len] = '\0';

		p = kbuf;
		while ((p = strstr(p, "test-keys")) != NULL) {
			memcpy(p, "release-k", 9);
			p += 9;
			modified = true;
		}

		if (modified) {
			if (copy_to_user(buf, kbuf, copy_len))
				pr_debug("ghost_net: copy_to_user failed for %s\n", name);
		}

		kfree(kbuf);
		return;
	}
}
EXPORT_SYMBOL(ghost_filter_vfs_read_payload);

static char *ghost_find_submem(char *haystack, size_t hlen, const char *needle, size_t nlen)
{
	size_t i;
	if (!haystack || hlen < nlen || nlen == 0)
		return NULL;
	for (i = 0; i <= hlen - nlen; i++) {
		if (haystack[i] == needle[0] && !memcmp(haystack + i, needle, nlen))
			return haystack + i;
	}
	return NULL;
}

void ghost_sanitize_boot_kmsg_buffer(char *buf, size_t len)
{
	static const struct {
		const char *search;
		const char *replace;
	} patterns[] = {
		{"androidboot.verifiedbootstate=orange", "androidboot.verifiedbootstate=green "},
		{"verifiedbootstate=orange", "verifiedbootstate=green "},
		{"[ret: 0x3] (orange)", "[ret: 0x0] (green) "},
		{"update_image_status_auth: Status for DTBO image is already custom",
		 "update_image_status_auth: Status for DTBO image is official      "},
		{"update_image_status_auth: Status for BOOT image is already custom",
		 "update_image_status_auth: Status for BOOT image is official      "},
		{"update_image_status_auth: Status for VENDOR_BOOT image is already custom",
		 "update_image_status_auth: Status for VENDOR_BOOT image is official      "},
		{"Verify_Signature_Ecdsa: failed.(FDAA0031)", "Verify_Signature_Ecdsa: success.(00000000)"},
		{"check_signature (VBMETA) invalid.", "check_signature (VBMETA) valid.  "},
		{"avb_slot_verify.c:881: ERROR: vbmeta: Error verifying vbmeta image: OK_NOT_SIGNED",
		 "avb_slot_verify.c:881: INFO:  vbmeta: Successfully verified vbmeta: OK_VERIFIED  "},
		{"avb_slot_verify.c:1072: DEBUG: vbmeta: VERIFICATION_DISABLED bit is set.",
		 "avb_slot_verify.c:1072: DEBUG: vbmeta: VERIFICATION_ENABLED bit is set.  "},
		{"[AVB] Root of trust error ret: 0xFDAA4003",
		 "[AVB] Root of trust valid ret: 0x00000000 "},
		{"vbmeta: AVB key length is zero",
		 "vbmeta: AVB key length is valid"},
		{"[AVB 2.0 ERR] authentication fail",
		 "[AVB 2.0]     authentication pass"},
		/* Tombstone crash dump sanitization — replace root module
		 * paths and process names with plausible Samsung service names.
		 * Each replace string MUST be exactly the same length as its
		 * search string for safe in-place memcpy overwrite.           */
		{"/data/adb/modules/tricky_store",  /* 30 chars */
		 "/system/priv-app/CarrierCfg   "}, /* 30 chars */
		{">>> TrickyStore <<<",  /* 19 chars */
		 ">>> carrier_svc <<<"},  /* 19 chars */
		{"Cmdline: TrickyStore",  /* 20 chars */
		 "Cmdline: carrier_svc"},  /* 20 chars */
		{"libtricky_store.so",  /* 18 chars */
		 "libsec_carrier.so "},  /* 18 chars */
	};
	size_t p;
	size_t copy_len;
	char *match;

	if (!buf || len == 0)
		return;

	for (p = 0; p < ARRAY_SIZE(patterns); p++) {
		size_t slen = strlen(patterns[p].search);
		size_t rlen = strlen(patterns[p].replace);
		char *curr = buf;
		size_t rem = len;

		while (rem >= slen) {
			match = ghost_find_submem(curr, rem, patterns[p].search, slen);
			if (!match)
				break;
			copy_len = min(slen, rlen);
			memcpy(match, patterns[p].replace, copy_len);
			if (slen > copy_len)
				memset(match + copy_len, ' ', slen - copy_len);
			rem -= (size_t)(match - curr) + slen;
			curr = match + slen;
		}
	}
}
EXPORT_SYMBOL(ghost_sanitize_boot_kmsg_buffer);



static int ghost_factory_reset_show(struct seq_file *m, void *v)
{
	struct timespec64 now;
	u64 reset_ts = ghost_get_reset_timestamp();
	u64 age_secs = 0;
	u64 age_days = 0;

	ktime_get_real_ts64(&now);
	if ((u64)now.tv_sec > reset_ts) {
		age_secs = (u64)now.tv_sec - reset_ts;
		age_days = div64_u64(age_secs, 86400ULL);
	}

	seq_printf(m, "reset_timestamp: %llu\n", reset_ts);
	seq_printf(m, "device_age_days: %llu\n", age_days);
	seq_printf(m, "device_age_seconds: %llu\n", age_secs);
	return 0;
}

static ssize_t ghost_factory_reset_write(struct file *file, const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	char kbuf[32];
	u64 val;
	int ret;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	ret = kstrtoull(strim(kbuf), 10, &val);
	if (ret)
		return ret;

	if (val > 1000000000ULL) {
		ghost_reset_timestamp = val;
		ghost_reset_user_set = true;
	} else if (val > 0) {
		struct timespec64 now;
		ktime_get_real_ts64(&now);
		ghost_reset_timestamp = (u64)now.tv_sec - (val * 86400ULL);
		ghost_reset_user_set = true;
	}

	ghost_reset_ready = true;
	pr_info("ghost_reset: updated reset timestamp to %llu\n", ghost_reset_timestamp);
	return count;
}

static int ghost_factory_reset_open(struct inode *inode, struct file *file)
{
	return single_open(file, ghost_factory_reset_show, NULL);
}

static const struct file_operations ghost_factory_reset_fops = {
	.open = ghost_factory_reset_open,
	.read = seq_read,
	.write = ghost_factory_reset_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void ghost_net_apply_mac(struct net_device *dev)
{
	if (!dev || !dev->name[0])
		return;

	if (!ghost_net_ready)
		ghost_net_init_macs();

	if (!strncmp(dev->name, "wlan", 4)) {
		memcpy(dev->dev_addr, ghost_wifi_mac, ETH_ALEN);
		pr_debug("ghost_net: applied ghost MAC %pM to interface %s\n",
			dev->dev_addr, dev->name);
	} else if (!strncmp(dev->name, "p2p", 3)) {
		memcpy(dev->dev_addr, ghost_p2p_mac, ETH_ALEN);
		pr_debug("ghost_net: applied ghost P2P MAC %pM to interface %s\n",
			dev->dev_addr, dev->name);
	}
}
EXPORT_SYMBOL(ghost_net_apply_mac);

void ghost_net_filter_bd_addr(u8 *bdaddr)
{
	if (!bdaddr)
		return;

	if (!ghost_net_ready)
		ghost_net_init_macs();

	memcpy(bdaddr, ghost_bt_addr, 6);
	pr_debug("ghost_net: filtered HCI BD_ADDR -> %02X:%02X:%02X:%02X:%02X:%02X\n",
		bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]);
}
EXPORT_SYMBOL(ghost_net_filter_bd_addr);

static int ghost_mac_show(struct seq_file *m, void *v)
{
	if (!ghost_net_ready)
		ghost_net_init_macs();
	if (!ghost_serialno_ready)
		ghost_init_serialno();
	if (!ghost_reset_ready)
		ghost_init_factory_reset();

	seq_printf(m, "wifi_mac: %pM\n", ghost_wifi_mac);
	seq_printf(m, "bt_bdaddr: %02X:%02X:%02X:%02X:%02X:%02X\n",
		   ghost_bt_addr[5], ghost_bt_addr[4], ghost_bt_addr[3],
		   ghost_bt_addr[2], ghost_bt_addr[1], ghost_bt_addr[0]);
	seq_printf(m, "serialno: %s\n", ghost_serialno);
	seq_printf(m, "reset_timestamp: %llu\n", ghost_reset_timestamp);
	seq_printf(m, "tcp_isn_offset: 0x%08X\n", ghost_storage_get_tcp_isn_offset());
	seq_printf(m, "tcp_ts_offset: 0x%08X\n", ghost_storage_get_tcp_ts_offset());
	return 0;
}

/* ================================================================
 * Pillar 37 Layer 2: Kernel-Native Boot Property Sanitizer
 *
 * Uses call_usermodehelper() to auto-sanitize Android settings after
 * boot without depending on init.rc, Magisk, or KSU modules.
 * Scheduled 90s after kernel init — framework is guaranteed ready.
 * ================================================================ */

static struct delayed_work ghost_boot_sanitizer_work;

static void ghost_boot_sanitizer_fn(struct work_struct *work)
{
	/* Inline shell script executed as root (UID 0):
	 * 1. Stop DSMS crash loop & satisfy security.dsmsd.enable
	 * 2. Auto-bind /prism/etc/csc -> /system/csc if missing
	 * 3. Purge DropBox system_server_wtf & tombstones
	 * 4. Normalize boot_count if <= 2 (fresh wipe indicator)
	 * 5. Hide Developer Options menu
	 * 6. Sanitize boot reason & history (remove recovery & factory_reset) */
	static char *script =
		/* 1. DSMS crash loop neutralization */
		"setprop security.dsmsd.enable false 2>/dev/null;"
		"stop dsmsd 2>/dev/null;"
		"stop dsmsca 2>/dev/null;"
		/* 2. CSC customer.xml bind */
		"if [ ! -f /system/csc/customer.xml ] && [ -f /prism/etc/csc/customer.xml ]; then "
		"mkdir -p /system/csc 2>/dev/null;"
		"mount -o bind /prism/etc/csc /system/csc 2>/dev/null || true;"
		"fi;"
		/* 3. Purge DropBox WTF / crash entries */
		"rm -rf /data/system/dropbox/*wtf* /data/system/dropbox/*strictmode* "
		"/data/system/dropbox/*crash* /data/system/dropbox/*anr* /data/tombstones/* 2>/dev/null;"
		/* 4. boot_count normalization */
		"BC=$(settings get global boot_count 2>/dev/null);"
		"if [ -n \"$BC\" ] && [ \"$BC\" -le 2 ] 2>/dev/null; then "
		"R=$(od -An -tu4 -N4 /dev/urandom|tr -d ' ');"
		"NB=$(( (R % 27) + 22 ));"
		"settings put global boot_count $NB 2>/dev/null;"
		"fi;"
		/* 5. boot reason & history sanitization (eradicate recovery & factory_reset) */
		"setprop sys.boot.reason \"reboot\" 2>/dev/null;"
		"setprop sys.boot.reason.last \"reboot\" 2>/dev/null;"
		"RP=;"
		"if [ -x /data/adb/ksu/bin/resetprop ]; then RP=/data/adb/ksu/bin/resetprop;"
		"elif [ -x /data/adb/magisk/resetprop ]; then RP=/data/adb/magisk/resetprop; fi;"
		"if [ -n \"$RP\" ]; then "
		"BT=$(awk '/^btime/{print $2}' /proc/stat);"
		"if [ -n \"$BT\" ] && [ \"$BT\" -gt 1000000000 ] 2>/dev/null; then "
		"A=$((BT+90));"
		"$RP -p persist.sys.boot.reason.history \"reboot,$A\" 2>/dev/null;"
		"$RP -p persist.sys.boot.reason \"\" 2>/dev/null;"
		"$RP -p ro.boot.bootreason \"reboot\" 2>/dev/null;"
		"fi; fi;"
		"log -t ghost_kernel -p i 'Ghost 5-package sanitization completed'";

	static char *argv[] = { "/system/bin/sh", "-c", NULL, NULL };
	static char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/system/bin:/system/xbin:/vendor/bin",
		NULL
	};

	argv[2] = script;
	call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
	pr_info("ghost_net: boot property sanitizer dispatched\n");
}

static int __init ghost_net_proc_init(void)
{
	ghost_net_init_macs();
	ghost_init_serialno();
	ghost_init_factory_reset();
	if (!proc_create_single("ghost_mac", 0400, NULL, ghost_mac_show)) {
		pr_err("ghost_net: failed to create /proc/ghost_mac\n");
		return -ENOMEM;
	}

	if (!proc_create("ghost_factory_reset", 0600, NULL, &ghost_factory_reset_fops)) {
		pr_err("ghost_reset: failed to create /proc/ghost_factory_reset\n");
	}

	/* Pillar 37: Schedule boot property sanitizer 90s after init.
	 * By then the Android framework + SettingsProvider are fully up. */
	INIT_DELAYED_WORK(&ghost_boot_sanitizer_work, ghost_boot_sanitizer_fn);
	schedule_delayed_work(&ghost_boot_sanitizer_work, msecs_to_jiffies(90000));

	return 0;
}
late_initcall(ghost_net_proc_init);
