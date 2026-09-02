/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_STORAGE_H
#define _LINUX_GHOST_STORAGE_H

#include <linux/types.h>

#define GHOST_UFS_SN_LEN 32
#define GHOST_UFS_UN_LEN 21  /* 20 hex digits + null terminator */
#define GHOST_UFS_CID_LEN 33 /* 32 hex digits + null terminator */

enum ghost_storage_mode {
	GHOST_STORAGE_PER_FORMAT = 0,
	GHOST_STORAGE_PER_REBOOT = 1,
};

extern bool ghost_storage_ready;
extern enum ghost_storage_mode ghost_storage_current_mode;

void ghost_storage_init(void);

static inline void ghost_ensure_ready(void)
{
	if (unlikely(!smp_load_acquire(&ghost_storage_ready)))
		ghost_storage_init();
}
void ghost_storage_on_f2fs_mount(const u8 *uuid);
void ghost_storage_get_ufs_sn(char *buf, size_t max_len);
void ghost_storage_get_ufs_un(char *buf, size_t max_len);
void ghost_storage_get_ufs_cid(char *buf, size_t max_len);
void ghost_storage_get_ufs_manfid(char *buf, size_t max_len);
void ghost_storage_get_ufs_rev(char *buf, size_t max_len);
void ghost_storage_get_ufs_model(char *buf, size_t max_len);
u16 ghost_storage_get_ufs_manfid_u16(void);
u16 ghost_storage_get_ufs_date_u16(void);
void ghost_storage_filter_vpd_pg80(u8 *buf, size_t len);
void ghost_storage_filter_part_uuid(const char *orig_uuid, char *out_uuid);
u64 ghost_storage_get_soc_unique_id(void);
u32 ghost_storage_get_soc_lot_id(void);
const char *ghost_storage_get_soc_lot_id2(void);

void ghost_storage_get_panel_cell_id(u8 *date, u8 *coord);
void ghost_storage_get_panel_octa_id(char *buf, size_t max_len);
void ghost_storage_get_panel_manf_code(u8 *code);
s16 ghost_storage_apply_sensor_jitter(s16 sample, int axis);
void ghost_storage_get_gyro_bias(s16 *gyro_bias);
s16 ghost_storage_get_baro_drift(void);
void ghost_storage_get_scsi_wwid(char *buf, size_t max_len);

u32 ghost_storage_get_tcp_isn_offset(void);
u32 ghost_storage_get_tcp_ts_offset(void);

int ghost_storage_get_battery_cycle(void);
int ghost_storage_get_battery_asoc(void);
long ghost_storage_get_ram_delta_pages(void);

#define GHOST_WIDEVINE_ID_LEN 32
#define GHOST_CAMERA_MOD_LEN  16

void ghost_storage_get_widevine_device_id(u8 out_id[GHOST_WIDEVINE_ID_LEN]);
const char *ghost_storage_get_widevine_device_id_hex(void);

const char *ghost_storage_get_camera_moduleid(int cam_index, const char *orig_prefix);
void ghost_storage_get_camera_sensorid(int cam_index, u8 *buf, size_t len);

/* Pillar 7: Instant Profile Reset & Identity Synchronization Engine */
void ghost_storage_reroll_all(void);

/* Pillar 10: Storage Partition Geometry & Inode Spoofing */
struct path;
struct kstatfs;
void ghost_storage_apply_statfs_geometry(const struct path *path, struct kstatfs *buf);

#endif /* _LINUX_GHOST_STORAGE_H */
