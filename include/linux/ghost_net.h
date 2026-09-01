/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_NET_H
#define _LINUX_GHOST_NET_H

#include <linux/types.h>
#include <linux/netdevice.h>

struct path;
struct kstat;
struct file;

extern u8 ghost_wifi_mac[ETH_ALEN];
extern u8 ghost_bt_addr[6];
extern bool ghost_net_ready;
extern char ghost_serialno[16];
extern bool ghost_serialno_ready;
extern u64 ghost_reset_timestamp;
extern bool ghost_reset_ready;

void ghost_net_init_macs(void);
void ghost_net_apply_mac(struct net_device *dev);
void ghost_net_filter_bd_addr(u8 *bdaddr);
void ghost_init_serialno(void);
void ghost_init_factory_reset(void);
u64 ghost_get_reset_timestamp(void);
void ghost_apply_stat_reset(const struct path *path, struct kstat *stat);


/* Pillar 3: Telecom & Modem Baseband IPC Stealth */
const char *ghost_get_imei(void);
const char *ghost_get_imei2(void);
void ghost_get_imei_bcd(u8 out_bcd[8]);
void ghost_get_imei2_bcd(u8 out_bcd[8]);
void ghost_telecom_filter_ipc_data(void *data, size_t len);
void ghost_filter_vfs_read_payload(struct file *file, char __user *buf, size_t ret);

void ghost_sanitize_boot_kmsg_buffer(char *buf, size_t len);

/* Pillar 7: Instant Profile Reset Engine */
void ghost_reroll_serialno(void);
void ghost_reroll_imei(void);
void ghost_reroll_network_macs(void);

#include <linux/dcache.h>
#include <linux/cred.h>

static inline bool ghost_is_stealth_denied_dentry(struct dentry *d)
{
	int depth = 0;

	if (!d)
		return false;

	/* Whitelist pass: walk ancestors first to allow legitimate
	 * Android system paths before any blacklist can trigger.
	 * e.g. /data/misc/adb/adb_keys, /dev/usb-ffs/adb/ep0 */
	{
		struct dentry *p = d;
		int i = 0;
		while (p && i < 8) {
			if (p->d_name.name &&
			    (!strcmp(p->d_name.name, "misc") ||
			     !strcmp(p->d_name.name, "usb-ffs")))
				return false;
			if (IS_ROOT(p) || !p->d_parent || p->d_parent == p)
				break;
			p = p->d_parent;
			i++;
		}
	}

	/* Blacklist: check the target dentry name for stealth identifiers */
	if (d->d_name.name) {
		const char *name = d->d_name.name;
		if (!strncmp(name, "ghost_", 6))
			return true;
		if (!strcmp(name, "su") || !strcmp(name, "daemonsu") ||
		    !strcmp(name, "ksud") || !strcmp(name, "busybox") ||
		    !strcmp(name, "magisk") || !strcmp(name, "zygisk"))
			return true;
		if (!strcmp(name, "last_kmsg") || !strcmp(name, "first_kmsg") ||
		    !strcmp(name, "secdbg_logbuf") || !strcmp(name, "reset_summary") ||
		    !strcmp(name, "pstore"))
			return true;
	}

	/* Blacklist: walk ancestors for module/root/crash-dump directories */
	do {
		if (d->d_name.name) {
			const char *n = d->d_name.name;
			if (!strcmp(n, "adb") || !strcmp(n, "ksu") ||
			    !strcmp(n, "kernelsu"))
				return true;
			if (!strcmp(n, "tricky_store") ||
			    !strcmp(n, "sec_carrier_config") ||
			    !strcmp(n, "sec_media_enhancer"))
				return true;
			if (!strcmp(n, "tombstones"))
				return true;
		}
		if (IS_ROOT(d) || !d->d_parent || d->d_parent == d)
			break;
		d = d->d_parent;
	} while (++depth < 8);

	return false;
}

#endif /* _LINUX_GHOST_NET_H */
