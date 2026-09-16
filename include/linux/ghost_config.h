/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_CONFIG_H
#define _LINUX_GHOST_CONFIG_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/string.h>
#include <linux/netdevice.h>

/* Ghost Kernel Feature Matrix (Explicitly Configured) */
#ifndef GHOST_SENSOR_CLOAK
#define GHOST_SENSOR_CLOAK 0
#endif
#ifndef GHOST_GAID_CLOAK
#define GHOST_GAID_CLOAK 0
#endif
#ifndef GHOST_BINDER_CLOAK
#define GHOST_BINDER_CLOAK 1
#endif
#ifndef GHOST_STEALTH
#define GHOST_STEALTH 0
#endif
#ifndef GHOST_CELL_CLOAK
#define GHOST_CELL_CLOAK 0
#endif
#ifndef GHOST_WIFI_CLOAK
#define GHOST_WIFI_CLOAK 1
#endif
#ifndef GHOST_BT_CLOAK
#define GHOST_BT_CLOAK 1
#endif
#ifndef GHOST_UFS_CLOAK
#define GHOST_UFS_CLOAK 1
#endif
#ifndef GHOST_PANEL_CLOAK
#define GHOST_PANEL_CLOAK 1
#endif
#ifndef GHOST_BATTERY_CLOAK
#define GHOST_BATTERY_CLOAK 1
#endif
#ifndef GHOST_CHIPID_CLOAK
#define GHOST_CHIPID_CLOAK 1
#endif
#ifndef GHOST_HWPARAM_CLOAK
#define GHOST_HWPARAM_CLOAK 1
#endif
#ifndef GHOST_FSID_CLOAK
#define GHOST_FSID_CLOAK 1
#endif
#ifndef GHOST_CMDLINE_CLOAK
#define GHOST_CMDLINE_CLOAK 1
#endif
#ifndef GHOST_PROP_CLOAK
#define GHOST_PROP_CLOAK 1
#endif
#ifndef GHOST_TCP_ISN
#define GHOST_TCP_ISN 1
#endif
#ifndef GHOST_PRINTK_DROP
#define GHOST_PRINTK_DROP 0
#endif

#define GHOST_CONF_STR_LEN 128
#define GHOST_CONF_FP_LEN  256
#define GHOST_CONF_PATH_PRIMARY   "/data/adb/ghost.conf"
#define GHOST_CONF_PATH_SECONDARY "/efs/ghost.conf"
#define GHOST_CONF_PATH_FALLBACK  "/sdcard/ghost.conf"

struct ghost_profile {
	char model[GHOST_CONF_STR_LEN];
	char product[GHOST_CONF_STR_LEN];
	char device[GHOST_CONF_STR_LEN];
	char manufacturer[GHOST_CONF_STR_LEN];
	char brand[GHOST_CONF_STR_LEN];
	char soc_machine[GHOST_CONF_STR_LEN];
	char soc_family[GHOST_CONF_STR_LEN];
	char build_fingerprint[GHOST_CONF_FP_LEN];
	char build_desc[GHOST_CONF_FP_LEN];
	char build_id[GHOST_CONF_STR_LEN];
	char security_patch[GHOST_CONF_STR_LEN];
	char serialno[32];
	char ap_serial[32];
	char em_did[32];
	u64 unique_id;
	char imei[32];
	char imei2[32];
	char imsi[32];
	char imsi2[32];
	char iccid[32];
	char iccid2[32];
	u8 wifi_mac[ETH_ALEN];
	u8 bt_mac[6];
	char ufs_serial[32];
	char ufs_model[32];
	char boot_hash[65];
	char boot_key[65];
	char device_unique_id[65];
	u8 device_unique_id_bytes[32];
	u32 uptime_days;
	u32 boot_count;
	u32 battery_cycle;
	u32 battery_health;
	s16 sensor_bias[3];
	s32 baro_drift_hpa_x100;
	u32 tcp_isn_offset;
	u64 disk_sector_count;
	s32 ram_delta_pages;
	char battery_cell_id[48];
	char spoofed_kernel_version[65];
	struct rcu_head rcu;
	bool is_loaded;
	char loaded_from[64];
};

#if IS_ENABLED(CONFIG_GHOST_KERNEL)

typedef void (*ghost_chipid_sync_fn_t)(u64 uid);
extern ghost_chipid_sync_fn_t ghost_chipid_sync_fn;

extern struct ghost_profile __rcu *ghost_active_profile_ptr;
void ghost_get_active_serial_buf(char *buf, size_t len);
void ghost_get_active_ap_serial_buf(char *buf, size_t len);
void ghost_get_active_em_did_buf(char *buf, size_t len);
void ghost_get_imei_buf(char *buf, size_t len);
void ghost_get_imei2_buf(char *buf, size_t len);
void ghost_get_imsi_buf(char *buf, size_t len);
void ghost_get_imsi2_buf(char *buf, size_t len);
void ghost_get_iccid_buf(char *buf, size_t len);
void ghost_get_iccid2_buf(char *buf, size_t len);
bool ghost_select_cloaked_imei(const char *hw_imei, char *out, size_t len);
bool ghost_select_cloaked_imei_slot(const char *hw_imei, char *out, size_t len, int slot);
bool ghost_select_cloaked_imsi_slot(const char *hw_imsi, char *out, size_t len, int slot);
bool ghost_select_cloaked_iccid_slot(const char *hw_iccid, char *out, size_t len, int slot);
void ghost_get_boot_hash_buf(char *buf, size_t len);
void ghost_get_boot_key_buf(char *buf, size_t len);
void ghost_get_ufs_model_buf(char *buf, size_t len);
void ghost_get_ufs_serial_buf(char *buf, size_t len);
void ghost_get_ufs_wwid_buf(char *buf, size_t len);
void ghost_get_device_unique_id_buf(char *buf, size_t len);
void ghost_get_device_unique_id_bytes(u8 *buf, size_t len);
void ghost_get_active_wifi_mac_buf(char *buf, size_t len);
void ghost_get_active_bt_mac_buf(char *buf, size_t len);
void ghost_get_camera_moduleid_buf(char *buf, size_t len, int cam_index);
void ghost_get_panel_cellid_buf(char *buf, size_t len);
void ghost_get_panel_octaid_buf(char *buf, size_t len);
void ghost_get_panel_maid_date_buf(char *buf, size_t len);
void ghost_get_profile_snapshot(struct ghost_profile *out);
void ghost_cloak_vpd_page(int page, unsigned char *data, size_t len);
void ghost_mask_fsid(int fsid_val[2]);
int ghost_config_init(void);
int ghost_config_reload(void);
void ghost_on_f2fs_userdata_mount(const u8 *uuid);
u64 ghost_get_active_unique_id(void);
u32 ghost_get_battery_cycle(void);
u32 ghost_get_battery_health(void);
u32 ghost_get_uptime_days(void);
u32 ghost_get_boot_count(void);
u32 ghost_get_tcp_isn_offset(void);
s32 ghost_get_baro_drift_hpa_x100(void);
void ghost_get_sensor_bias(s16 bias[3]);
void ghost_apply_accel_bias(s16 *x, s16 *y, s16 *z);
void ghost_sanitize_bootargs(char *buf, size_t len);
void ghost_copy_wifi_mac(u8 *mac);
void ghost_copy_eth_addr_cloaked(u8 *dst, const u8 *src, unsigned int addr_len);
void ghost_get_panel_ddi_buf(char *buf, size_t len);
void ghost_get_panel_manufacture_date(int *year, int *month, int *day,
				      int *hour, int *min);
void ghost_get_chip_lot_buf(char *buf, size_t len);
void ghost_get_extra_info_id_buf(char *buf, size_t len);
void ghost_get_asv_values(int *asv);
void ghost_get_ids_values(int *ids);
void ghost_get_batt_qr_buf(char *buf, size_t len);
void ghost_get_pcb_buf(char *buf, size_t len);
void ghost_get_smd_date_buf(char *buf, size_t len);
void ghost_get_ufs_manf_date(u16 *out);
void ghost_get_ufs_health(u8 *eol, u8 *life_a, u8 *life_b);
void ghost_get_ufs_unique_number_buf(char *buf, size_t len);
u32 ghost_get_ufs_flt(void);
u32 ghost_get_cable_count(void);
u64 ghost_get_ufs_transferred_bytes(void);
bool ghost_get_cloaked_efs_payload(const char *dname, const char *pname,
				   char *out, size_t out_len, size_t *out_plen);
void ghost_sanitize_hwparam_blob(char *buf, size_t len);
void ghost_sanitize_efs_blob(const char *dname, const char *pname,
			    char *buf, size_t len);
void ghost_get_eid_buf(char *buf, size_t len);
void ghost_get_asb_psite(int *asb, int *psite);
void ghost_cloak_sensorid_exif(void *id, size_t len, int cam_index);
u64 ghost_get_disk_sector_count(void);
long ghost_get_ram_delta_pages(void);
void ghost_set_real_disk_sectors(u64 sectors);
u64 ghost_get_real_disk_sectors(void);

struct task_struct;
struct dentry;
struct path;

bool ghost_should_cloak_untrusted(struct task_struct *task);
bool ghost_path_is_root_leak(const char *path);
bool ghost_should_hide_user_path(const char __user *name);
void ghost_fill_drm_id(u8 *out, size_t len);
int ghost_cloak_drm_reply_bytes(u8 *pkt, size_t len);
bool ghost_selinux_relax_serialno_prop(const char *type_name);
void ghost_fill_gaid(char *out, size_t len);
int ghost_cloak_gaid_reply_bytes(u8 *pkt, size_t len);
void ghost_wifi_note_bss(const u8 *bssid, const u8 *ssid, u8 ssid_len);
void ghost_wifi_note_connected(const u8 *bssid, const u8 *ssid, u8 ssid_len);
int ghost_wifi_has_notes(void);
int ghost_wifi_cloak_bytes(u8 *pkt, size_t len);
int ghost_bt_cloak_bytes(u8 *pkt, size_t len);
int ghost_sensor_cloak_bytes(u8 *pkt, size_t len);
void ghost_cloak_sensor_text(char *buf, size_t len);
ssize_t ghost_sysfs_print_sensor_text(char *buf, const char *text);
bool ghost_is_cloaked_efs_dentry(const struct dentry *dentry);
bool ghost_is_cloaked_efs_path(const struct path *path);
bool ghost_is_oemcrypto_path(const char *name);
bool ghost_is_oemcrypto_user_path(const char __user *name);

#else /* !CONFIG_GHOST_KERNEL */

#define ghost_active_profile_ptr NULL
#define ghost_chipid_sync_fn NULL

static inline void ghost_get_active_serial_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_active_ap_serial_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_active_em_did_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_imei_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_imei2_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_imsi_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_imsi2_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_iccid_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_iccid2_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline bool ghost_select_cloaked_imei(const char *hw_imei, char *out, size_t len) { return false; }
static inline bool ghost_select_cloaked_imei_slot(const char *hw_imei, char *out, size_t len, int slot) { return false; }
static inline bool ghost_select_cloaked_imsi_slot(const char *hw_imsi, char *out, size_t len, int slot) { return false; }
static inline bool ghost_select_cloaked_iccid_slot(const char *hw_iccid, char *out, size_t len, int slot) { return false; }
static inline void ghost_get_boot_hash_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_boot_key_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_ufs_model_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_ufs_serial_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_ufs_wwid_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_device_unique_id_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_device_unique_id_bytes(u8 *buf, size_t len) { if (len) memset(buf, 0, len); }
static inline void ghost_get_active_wifi_mac_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_active_bt_mac_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_camera_moduleid_buf(char *buf, size_t len, int cam_index) { if (len) buf[0] = '\0'; }
static inline void ghost_get_panel_cellid_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_panel_octaid_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_panel_maid_date_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_profile_snapshot(struct ghost_profile *out) { if (out) memset(out, 0, sizeof(*out)); }
static inline void ghost_cloak_vpd_page(int page, unsigned char *data, size_t len) {}
static inline void ghost_mask_fsid(int fsid_val[2]) {}
static inline int ghost_config_init(void) { return 0; }
static inline int ghost_config_reload(void) { return 0; }
static inline void ghost_on_f2fs_userdata_mount(const u8 *uuid) {}
static inline u64 ghost_get_active_unique_id(void) { return 0; }
static inline u32 ghost_get_battery_cycle(void) { return 0; }
static inline u32 ghost_get_battery_health(void) { return 0; }
static inline u32 ghost_get_uptime_days(void) { return 0; }
static inline u32 ghost_get_boot_count(void) { return 0; }
static inline u32 ghost_get_tcp_isn_offset(void) { return 0; }
static inline s32 ghost_get_baro_drift_hpa_x100(void) { return 0; }
static inline void ghost_get_sensor_bias(s16 bias[3]) { if (bias) bias[0] = bias[1] = bias[2] = 0; }
static inline void ghost_apply_accel_bias(s16 *x, s16 *y, s16 *z) {}
static inline void ghost_sanitize_bootargs(char *buf, size_t len) {}
static inline void ghost_copy_wifi_mac(u8 *mac) {}
static inline void ghost_copy_eth_addr_cloaked(u8 *dst, const u8 *src, unsigned int addr_len) { memcpy(dst, src, addr_len); }
static inline void ghost_get_panel_ddi_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_panel_manufacture_date(int *y, int *m, int *d, int *h, int *min) {}
static inline void ghost_get_chip_lot_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_extra_info_id_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_asv_values(int *asv) {}
static inline void ghost_get_ids_values(int *ids) {}
static inline void ghost_get_batt_qr_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_pcb_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_smd_date_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_ufs_manf_date(u16 *out) {}
static inline void ghost_get_ufs_health(u8 *eol, u8 *life_a, u8 *life_b) {}
static inline void ghost_get_ufs_unique_number_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline u32 ghost_get_ufs_flt(void) { return 0; }
static inline u32 ghost_get_cable_count(void) { return 0; }
static inline u64 ghost_get_ufs_transferred_bytes(void) { return 0; }
static inline bool ghost_get_cloaked_efs_payload(const char *dname, const char *pname,
						 char *out, size_t out_len, size_t *out_plen) { return false; }
static inline void ghost_sanitize_hwparam_blob(char *buf, size_t len) {}
static inline void ghost_sanitize_efs_blob(const char *dname, const char *pname,
					  char *buf, size_t len) {}
static inline void ghost_get_eid_buf(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline void ghost_get_asb_psite(int *asb, int *psite) {}
static inline void ghost_cloak_sensorid_exif(void *id, size_t len, int cam_index) {}
static inline u64 ghost_get_disk_sector_count(void) { return 0; }
static inline long ghost_get_ram_delta_pages(void) { return 0; }
static inline void ghost_set_real_disk_sectors(u64 s) {}
static inline u64 ghost_get_real_disk_sectors(void) { return 0; }

struct task_struct;
struct dentry;
struct path;

static inline bool ghost_should_cloak_untrusted(struct task_struct *task) { return false; }
static inline bool ghost_path_is_root_leak(const char *path) { return false; }
static inline bool ghost_should_hide_user_path(const char __user *name) { return false; }
static inline void ghost_fill_drm_id(u8 *out, size_t len) {}
static inline int ghost_cloak_drm_reply_bytes(u8 *pkt, size_t len) { return 0; }
static inline bool ghost_selinux_relax_serialno_prop(const char *type_name) { return false; }
static inline void ghost_fill_gaid(char *out, size_t len) {}
static inline int ghost_cloak_gaid_reply_bytes(u8 *pkt, size_t len) { return 0; }
static inline void ghost_wifi_note_bss(const u8 *bssid, const u8 *ssid, u8 ssid_len) {}
static inline void ghost_wifi_note_connected(const u8 *bssid, const u8 *ssid, u8 ssid_len) {}
static inline int ghost_wifi_has_notes(void) { return 0; }
static inline int ghost_wifi_cloak_bytes(u8 *pkt, size_t len) { return 0; }
static inline int ghost_bt_cloak_bytes(u8 *pkt, size_t len) { return 0; }
static inline int ghost_sensor_cloak_bytes(u8 *pkt, size_t len) { return 0; }
static inline void ghost_cloak_sensor_text(char *buf, size_t len) {}
static inline ssize_t ghost_sysfs_print_sensor_text(char *buf, const char *text) { return 0; }
static inline bool ghost_is_cloaked_efs_dentry(const struct dentry *dentry) { return false; }
static inline bool ghost_is_cloaked_efs_path(const struct path *path) { return false; }
static inline bool ghost_is_oemcrypto_path(const char *name) { return false; }
static inline bool ghost_is_oemcrypto_user_path(const char __user *name) { return false; }

#endif /* CONFIG_GHOST_KERNEL */

#endif /* _LINUX_GHOST_CONFIG_H */
