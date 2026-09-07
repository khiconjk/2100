/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_CONFIG_H
#define _LINUX_GHOST_CONFIG_H

#include <linux/types.h>
#include <linux/netdevice.h>

#define GHOST_CONF_STR_LEN 128
#define GHOST_CONF_FP_LEN  256
#define GHOST_CONF_PATH_PRIMARY   "/data/adb/ghost.conf"
#define GHOST_CONF_PATH_SECONDARY "/efs/ghost.conf"
#define GHOST_CONF_PATH_FALLBACK  "/sdcard/ghost.conf"

struct ghost_profile {
	/* Device Identification */
	char model[GHOST_CONF_STR_LEN];
	char product[GHOST_CONF_STR_LEN];
	char device[GHOST_CONF_STR_LEN];
	char manufacturer[GHOST_CONF_STR_LEN];
	char brand[GHOST_CONF_STR_LEN];
	char soc_machine[GHOST_CONF_STR_LEN];
	char soc_family[GHOST_CONF_STR_LEN];

	/* Android OS Fingerprint */
	char build_fingerprint[GHOST_CONF_FP_LEN];
	char build_desc[GHOST_CONF_FP_LEN];
	char build_id[GHOST_CONF_STR_LEN];
	char security_patch[GHOST_CONF_STR_LEN];

	/* Hardware IDs */
	char serialno[32];
	char ap_serial[32];
	char em_did[32];
	u64 unique_id;
	char imei[32];
	char imei2[32];
	u8 wifi_mac[ETH_ALEN];
	u8 bt_mac[6];
	char ufs_serial[32];
	char ufs_model[32];

	/* Verified Boot */
	char boot_hash[65];
	char boot_key[65];

	/* Environment & Sensor Metrics */
	u32 uptime_days;
	u32 boot_count;
	u32 battery_cycle;
	u32 battery_health;
	s16 sensor_bias[3];
	s32 baro_drift_hpa_x100;
	u32 tcp_isn_offset;

	/* Spoofed Kernel & RCU Lifetime */
	char spoofed_kernel_version[65];
	struct rcu_head rcu;

	/* State */
	bool is_loaded;
	char loaded_from[64];
};

extern struct ghost_profile __rcu *ghost_active_profile_ptr;
void ghost_get_active_serial_buf(char *buf, size_t len);
void ghost_get_active_ap_serial_buf(char *buf, size_t len);
void ghost_get_active_em_did_buf(char *buf, size_t len);
void ghost_get_boot_hash_buf(char *buf, size_t len);
void ghost_get_boot_key_buf(char *buf, size_t len);
void ghost_get_ufs_model_buf(char *buf, size_t len);
void ghost_get_profile_snapshot(struct ghost_profile *out);

/* Core Engine APIs */
int ghost_config_init(void);
int ghost_config_reload(void);
void ghost_on_f2fs_userdata_mount(const u8 *uuid);
u64 ghost_get_active_unique_id(void);
u32 ghost_get_battery_cycle(void);
u32 ghost_get_battery_health(void);
void ghost_sanitize_bootargs(char *buf, size_t len);

#endif /* _LINUX_GHOST_CONFIG_H */
