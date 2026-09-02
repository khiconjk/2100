// SPDX-License-Identifier: GPL-2.0
/*
 * Ghost Storage: UFS Hardware ID and Storage Partition UUID spoofing.
 *
 * Supports two operational modes:
 * - Per-Format: Master seed is derived from /data F2FS Superblock UUID.
 *   Remains identical across reboots, but completely randomizes upon factory reset / data wipe.
 * - Per-Reboot: Master seed is generated freshly in RAM on every boot.
 *
 * Exposes /proc/ghost_storage for status inspection and mode switching.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <crypto/sha.h>
#include <asm/unaligned.h>
#include <linux/jiffies.h>
#include <linux/sched/clock.h>
#include <linux/statfs.h>
#include <uapi/linux/magic.h>
#include <linux/ghost_storage.h>
#include <linux/ghost_net.h>
#include <linux/ghost_thermal.h>
#include <linux/spinlock.h>
#include <linux/rwlock.h>

/* Protects all derived ghost state during reroll/mode-switch operations.
 * Readers take read-lock, reroll/derive takes write-lock. */
DEFINE_RWLOCK(ghost_storage_rwlock);
EXPORT_SYMBOL(ghost_storage_rwlock);

bool ghost_storage_ready = false;
enum ghost_storage_mode ghost_storage_current_mode = GHOST_STORAGE_PER_FORMAT;

static u8 ghost_storage_master_seed[32] = {0};
static char ghost_ufs_serial[GHOST_UFS_SN_LEN + 1] = {0};
static char ghost_ufs_unique_num[GHOST_UFS_UN_LEN + 1] = {0};
static char ghost_ufs_cid[GHOST_UFS_CID_LEN + 1] = {0};
static char ghost_ufs_manfid[16] = "0x0001ce";
static char ghost_ufs_rev[8] = "0800";
static char ghost_ufs_model[32] = "KLUEG8UHDB-C2D1";
static u16 ghost_ufs_manfid_val = 0x01CE;
static u16 ghost_ufs_date_val = 0x0121;
static u8 ghost_f2fs_uuid[16] = {0};
static bool ghost_f2fs_uuid_recorded = false;
static u64 ghost_soc_unique_id = 0;
static u32 ghost_soc_lot_id = 0;
static char ghost_soc_lot_id2[8] = "S0000";
static u8 ghost_panel_date[7] = {0};
static u8 ghost_panel_coord[4] = {0};
static char ghost_panel_octa_id[17] = {0};
static u8 ghost_panel_manf_code[5] = {0};
static s16 ghost_sensor_bias[3] = {0};
static s16 ghost_gyro_bias[3] = {0};
static s16 ghost_baro_drift_hpa = 0;
static char ghost_scsi_wwid[32] = {0};
static u32 ghost_tcp_isn_offset = 0;
static u32 ghost_tcp_ts_offset = 0;
static int ghost_battery_cycle = 185;
static int ghost_battery_asoc = 96;
static long ghost_ram_carveout_delta_pages = 0;

static u8 ghost_widevine_device_id[GHOST_WIDEVINE_ID_LEN] = {0};
static char ghost_widevine_device_id_hex[GHOST_WIDEVINE_ID_LEN * 2 + 1] = {0};
static char ghost_camera_rear_moduleid[GHOST_CAMERA_MOD_LEN] = "SVOGA0123000E51";
static char ghost_camera_rear2_moduleid[GHOST_CAMERA_MOD_LEN] = "AVOGS0FAB0002B5";
static char ghost_camera_rear3_moduleid[GHOST_CAMERA_MOD_LEN] = "SVOGA0123000E51";
static char ghost_camera_front_moduleid[GHOST_CAMERA_MOD_LEN] = "CVOGK1FE9002002";
static u8 ghost_camera_rear_sensorid[16] = {0};
static u8 ghost_camera_rear2_sensorid[16] = {0};
static u8 ghost_camera_rear3_sensorid[16] = {0};
static u8 ghost_camera_front_sensorid[16] = {0};

EXPORT_SYMBOL(ghost_storage_ready);
EXPORT_SYMBOL(ghost_storage_current_mode);

static void ghost_storage_derive_ids(void)
{
	static const u8 valid_months[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12 };
	static const u8 valid_years[] = { 0x21, 0x22, 0x23 };
	u8 digest[SHA256_DIGEST_SIZE];
	u8 month, year;

	/* Derive 32-byte digest from master seed */
	sha256(ghost_storage_master_seed, sizeof(ghost_storage_master_seed), digest);

	/* 1. Generate Samsung UFS Serial Number: SEC_KLUEG8UHDB_XXXXXXXX */
	snprintf(ghost_ufs_serial, sizeof(ghost_ufs_serial),
		 "SEC_KLUEG8UHDB_%02X%02X%02X%02X",
		 digest[8], digest[9], digest[10], digest[11]);

	/* 2. Generate Samsung Unique Number (20 hex characters):
	 * [ManfID: 0x15 Samsung (2 hex)][Month (2 hex)][Year (2 hex)][7 random serial bytes (14 hex)]
	 */
	month = valid_months[digest[12] % ARRAY_SIZE(valid_months)];
	year = valid_years[digest[13] % ARRAY_SIZE(valid_years)];
	ghost_ufs_date_val = ((u16)month << 8) | year;

	snprintf(ghost_ufs_unique_num, sizeof(ghost_ufs_unique_num),
		 "15%02X%02X%02X%02X%02X%02X%02X%02X%02X",
		 month, year,
		 digest[14], digest[15], digest[16],
		 digest[17], digest[18], digest[19], digest[20]);

	/* 3. Generate Samsung UFS CID (32 hex characters):
	 * 15 (Samsung) + 0100 (OEM ID) + 4B4C5545473855484442 (KLUEG8UHDB) + 08 (rev) + 8 hex serial + date + crc
	 */
	snprintf(ghost_ufs_cid, sizeof(ghost_ufs_cid),
		 "1501004B4C554547385548444208%02X%02X%02X%02X%02X%02X",
		 digest[21], digest[22], digest[23], digest[24],
		 month, digest[25]);

	/* 4. Generate Exynos 2100 SoC Unique Silicon ID, Lot ID & Lot ID2 */
	ghost_soc_unique_id = ((u64)digest[0] << 32) |
			      ((u64)digest[1] << 24) |
			      ((u64)digest[2] << 16) |
			      ((u64)digest[3] << 8)  |
			      (u64)digest[4];
	ghost_soc_unique_id |= 0x1000000000ULL;
	ghost_soc_lot_id = (u32)(ghost_soc_unique_id & 0x001FFFFF);

	{
		static const char b36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		static const char pfx[] = { 'S', 'A', 'N' };
		int i;
		ghost_soc_lot_id2[0] = pfx[digest[5] % ARRAY_SIZE(pfx)];
		ghost_soc_lot_id2[1] = b36[digest[6] % 36];
		ghost_soc_lot_id2[2] = b36[digest[7] % 36];
		ghost_soc_lot_id2[3] = b36[digest[8] % 36];
		ghost_soc_lot_id2[4] = b36[digest[9] % 36];
		ghost_soc_lot_id2[5] = '\0';

		/* 5. Generate Samsung Display OLED Panel Cell ID, OCTA ID & Manf Code */
		ghost_panel_date[0] = (0xA << 4) | (1 + (digest[26] % 12));
		ghost_panel_date[1] = 1 + (digest[27] % 28);
		ghost_panel_date[2] = digest[28] % 24;
		ghost_panel_date[3] = digest[29] % 60;
		ghost_panel_date[4] = digest[30] % 60;
		ghost_panel_date[5] = digest[31];
		ghost_panel_date[6] = digest[0];

		ghost_panel_coord[0] = (digest[1] % 20);
		ghost_panel_coord[1] = (digest[2] % 30);
		ghost_panel_coord[2] = (digest[3] % 10);
		ghost_panel_coord[3] = (digest[4] % 10);

		for (i = 0; i < 16; i++)
			ghost_panel_octa_id[i] = b36[(digest[(i + 5) % 32] + i) % 36];
		ghost_panel_octa_id[16] = '\0';

		ghost_panel_manf_code[0] = 0xA0 | (digest[10] & 0x0F);
		ghost_panel_manf_code[1] = digest[11];
		ghost_panel_manf_code[2] = digest[12];
		ghost_panel_manf_code[3] = digest[13];
		ghost_panel_manf_code[4] = digest[14];

		/* 6. MEMS Sensor Fixed Bias Drift & Barometer baseline (derived from KDF seed) */
		ghost_sensor_bias[0] = (s16)((digest[15] % 17) - 8);
		ghost_sensor_bias[1] = (s16)((digest[16] % 17) - 8);
		ghost_sensor_bias[2] = (s16)((digest[17] % 17) - 8);

		ghost_gyro_bias[0] = (s16)((digest[23] % 11) - 5);
		ghost_gyro_bias[1] = (s16)((digest[24] % 11) - 5);
		ghost_gyro_bias[2] = (s16)((digest[25] % 11) - 5);

		ghost_baro_drift_hpa = (s16)((digest[26] % 31) - 15); /* -1.5 .. +1.5 hPa */

		/* 6b. SCSI WWID in IEEE NAA format (Samsung OUI 0x0001ce) */
		snprintf(ghost_scsi_wwid, sizeof(ghost_scsi_wwid),
			 "naa.5001ce%02x%02x%02x%02x%02x",
			 digest[0], digest[1], digest[2], digest[3], digest[4]);

		/* 7. TCP/IP Stack ISN and Timestamp Offsets */
		ghost_tcp_isn_offset = get_unaligned_le32(&digest[18]);
		ghost_tcp_ts_offset = get_unaligned_le32(&digest[22]);

		/* 8. Battery Health, Cycle Count & ASoC */
		ghost_battery_cycle = 180 + (((u16)digest[19] << 8 | digest[20]) % 371);  /* 180..550 cycles */
		ghost_battery_asoc = 92 + (digest[27] % 7);  /* 92..98% */

		/* 9. Widevine DRM Device Unique ID (32 bytes / 256-bit) */
		{
			u8 wv_in[64];
			size_t wv_label_len = sizeof("GHOST_WIDEVINE_DEVICE_ID_V1") - 1;
			memcpy(wv_in, "GHOST_WIDEVINE_DEVICE_ID_V1", wv_label_len);
			memcpy(wv_in + wv_label_len, ghost_storage_master_seed, 16);
			sha256(wv_in, wv_label_len + 16, ghost_widevine_device_id);

			for (i = 0; i < GHOST_WIDEVINE_ID_LEN; i++)
				snprintf(&ghost_widevine_device_id_hex[i * 2], 3, "%02x", ghost_widevine_device_id[i]);
			ghost_widevine_device_id_hex[GHOST_WIDEVINE_ID_LEN * 2] = '\0';
		}

		/* 10. Camera Module & Sensor OTP IDs */
		{
			u8 cam_in[64];
			u8 cam_digest[SHA256_DIGEST_SIZE];
			size_t label_len;

			/* Rear Main (CAM_INFO_REAR) — label 23 bytes */
			label_len = sizeof("GHOST_CAMERA_REAR_ID_V1") - 1;
			memcpy(cam_in, "GHOST_CAMERA_REAR_ID_V1", label_len);
			memcpy(cam_in + label_len, ghost_storage_master_seed, 16);
			sha256(cam_in, label_len + 16, cam_digest);
			snprintf(ghost_camera_rear_moduleid, sizeof(ghost_camera_rear_moduleid),
				 "SVOGA%02X%02X%02X%02X%02X",
				 cam_digest[0], cam_digest[1], cam_digest[2], cam_digest[3], cam_digest[4]);
			memcpy(ghost_camera_rear_sensorid, cam_digest + 5, 16);

			/* Rear Tele (CAM_INFO_REAR2) — label 24 bytes */
			label_len = sizeof("GHOST_CAMERA_REAR2_ID_V1") - 1;
			memcpy(cam_in, "GHOST_CAMERA_REAR2_ID_V1", label_len);
			memcpy(cam_in + label_len, ghost_storage_master_seed, 16);
			sha256(cam_in, label_len + 16, cam_digest);
			snprintf(ghost_camera_rear2_moduleid, sizeof(ghost_camera_rear2_moduleid),
				 "AVOGS%02X%02X%02X%02X%02X",
				 cam_digest[0], cam_digest[1], cam_digest[2], cam_digest[3], cam_digest[4]);
			memcpy(ghost_camera_rear2_sensorid, cam_digest + 5, 16);

			/* Rear Ultra-Wide (CAM_INFO_REAR3) — label 24 bytes */
			label_len = sizeof("GHOST_CAMERA_REAR3_ID_V1") - 1;
			memcpy(cam_in, "GHOST_CAMERA_REAR3_ID_V1", label_len);
			memcpy(cam_in + label_len, ghost_storage_master_seed, 16);
			sha256(cam_in, label_len + 16, cam_digest);
			snprintf(ghost_camera_rear3_moduleid, sizeof(ghost_camera_rear3_moduleid),
				 "SVOGA%02X%02X%02X%02X%02X",
				 cam_digest[0], cam_digest[1], cam_digest[2], cam_digest[3], cam_digest[4]);
			memcpy(ghost_camera_rear3_sensorid, cam_digest + 5, 16);

			/* Front (CAM_INFO_FRONT) — label 24 bytes */
			label_len = sizeof("GHOST_CAMERA_FRONT_ID_V1") - 1;
			memcpy(cam_in, "GHOST_CAMERA_FRONT_ID_V1", label_len);
			memcpy(cam_in + label_len, ghost_storage_master_seed, 16);
			sha256(cam_in, label_len + 16, cam_digest);
			snprintf(ghost_camera_front_moduleid, sizeof(ghost_camera_front_moduleid),
				 "CVOGK%02X%02X%02X%02X%02X",
				 cam_digest[0], cam_digest[1], cam_digest[2], cam_digest[3], cam_digest[4]);
			memcpy(ghost_camera_front_sensorid, cam_digest + 5, 16);
		}

		/* 11. Randomize Static RAM Carveout Delta (+/- 16MB in 4K pages) */
		ghost_ram_carveout_delta_pages = ((long)(digest[31] % 4096)) - 2048;
	}

	smp_store_release(&ghost_storage_ready, true);

	pr_debug("ghost_storage: UFS SN='%s', UN='%s', CID='%s', SoC UID='%010llX' (mode=%s)\n",
		ghost_ufs_serial, ghost_ufs_unique_num, ghost_ufs_cid, ghost_soc_unique_id,
		ghost_storage_current_mode == GHOST_STORAGE_PER_FORMAT ? "per-format" : "per-reboot");
}

void ghost_storage_init(void)
{
	if (ghost_storage_ready && ghost_storage_current_mode == GHOST_STORAGE_PER_FORMAT && ghost_f2fs_uuid_recorded)
		return;

	if (ghost_storage_current_mode == GHOST_STORAGE_PER_REBOOT || !ghost_f2fs_uuid_recorded) {
		get_random_bytes(ghost_storage_master_seed, sizeof(ghost_storage_master_seed));
		ghost_storage_derive_ids();
	}
}
EXPORT_SYMBOL(ghost_storage_init);

void ghost_storage_on_f2fs_mount(const u8 *uuid)
{
	if (!uuid)
		return;

	memcpy(ghost_f2fs_uuid, uuid, 16);
	ghost_f2fs_uuid_recorded = true;

	if (ghost_storage_current_mode == GHOST_STORAGE_PER_FORMAT) {
		/* Salt with constant string to derive storage-specific master seed */
		u8 kdf_in[48];
		memcpy(kdf_in, "GHOST_STORAGE_F2FS_KDF_SALT_V1", 30);
		memcpy(kdf_in + 30, uuid, 16);
		sha256(kdf_in, 46, ghost_storage_master_seed);
		ghost_storage_derive_ids();
		pr_debug("ghost_storage: updated master seed from F2FS UUID %pUb\n", uuid);
	}
}
EXPORT_SYMBOL(ghost_storage_on_f2fs_mount);

void ghost_storage_get_ufs_sn(char *buf, size_t max_len)
{
	ghost_ensure_ready();

	if (!buf || max_len == 0)
		return;

	strncpy(buf, ghost_ufs_serial, max_len - 1);
	buf[max_len - 1] = '\0';
}
EXPORT_SYMBOL(ghost_storage_get_ufs_sn);

void ghost_storage_get_ufs_un(char *buf, size_t max_len)
{
	ghost_ensure_ready();

	if (!buf || max_len == 0)
		return;

	strncpy(buf, ghost_ufs_unique_num, max_len - 1);
	buf[max_len - 1] = '\0';
}
EXPORT_SYMBOL(ghost_storage_get_ufs_un);

void ghost_storage_get_ufs_cid(char *buf, size_t max_len)
{
	ghost_ensure_ready();

	if (!buf || max_len == 0)
		return;

	strncpy(buf, ghost_ufs_cid, max_len - 1);
	buf[max_len - 1] = '\0';
}
EXPORT_SYMBOL(ghost_storage_get_ufs_cid);

void ghost_storage_get_ufs_manfid(char *buf, size_t max_len)
{
	ghost_ensure_ready();

	if (!buf || max_len == 0)
		return;

	strncpy(buf, ghost_ufs_manfid, max_len - 1);
	buf[max_len - 1] = '\0';
}
EXPORT_SYMBOL(ghost_storage_get_ufs_manfid);

void ghost_storage_get_ufs_rev(char *buf, size_t max_len)
{
	ghost_ensure_ready();

	if (!buf || max_len == 0)
		return;

	strncpy(buf, ghost_ufs_rev, max_len - 1);
	buf[max_len - 1] = '\0';
}
EXPORT_SYMBOL(ghost_storage_get_ufs_rev);

void ghost_storage_get_ufs_model(char *buf, size_t max_len)
{
	ghost_ensure_ready();

	if (!buf || max_len == 0)
		return;

	strncpy(buf, ghost_ufs_model, max_len - 1);
	buf[max_len - 1] = '\0';
}
EXPORT_SYMBOL(ghost_storage_get_ufs_model);

u16 ghost_storage_get_ufs_manfid_u16(void)
{
	return ghost_ufs_manfid_val;
}
EXPORT_SYMBOL(ghost_storage_get_ufs_manfid_u16);

u16 ghost_storage_get_ufs_date_u16(void)
{
	return ghost_ufs_date_val;
}
EXPORT_SYMBOL(ghost_storage_get_ufs_date_u16);

void ghost_storage_filter_vpd_pg80(u8 *buf, size_t len)
{
	size_t sn_len;

	if (!buf || len <= 4)
		return;

	/* VPD Page 0x80: byte 1 must be 0x80 */
	if (buf[1] != 0x80)
		return;

	ghost_ensure_ready();

	sn_len = strlen(ghost_ufs_serial);
	if (len >= 4 + sn_len) {
		memcpy(buf + 4, ghost_ufs_serial, sn_len);
		buf[3] = (u8)sn_len; /* update page length */
	}
}
EXPORT_SYMBOL(ghost_storage_filter_vpd_pg80);

void ghost_storage_filter_part_uuid(const char *orig_uuid, char *out_uuid)
{
	u8 hash_in[64];
	u8 digest[SHA256_DIGEST_SIZE];
	size_t in_len;

	if (!orig_uuid || !out_uuid)
		return;

	ghost_ensure_ready();

	in_len = strnlen(orig_uuid, 36);
	if (in_len < 36)
		return;

	memcpy(hash_in, orig_uuid, in_len);
	memcpy(hash_in + in_len, ghost_storage_master_seed, 16);
	sha256(hash_in, in_len + 16, digest);

	snprintf(out_uuid, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 digest[0], digest[1], digest[2], digest[3],
		 digest[4], digest[5],
		 (digest[6] & 0x0f) | 0x40,
		 digest[7],
		 (digest[8] & 0x3f) | 0x80,
		 digest[9],
		 digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]);
}
EXPORT_SYMBOL(ghost_storage_filter_part_uuid);

u64 ghost_storage_get_soc_unique_id(void)
{
	ghost_ensure_ready();
	return ghost_soc_unique_id;
}
EXPORT_SYMBOL(ghost_storage_get_soc_unique_id);

u32 ghost_storage_get_soc_lot_id(void)
{
	ghost_ensure_ready();
	return ghost_soc_lot_id;
}
EXPORT_SYMBOL(ghost_storage_get_soc_lot_id);

const char *ghost_storage_get_soc_lot_id2(void)
{
	ghost_ensure_ready();
	return ghost_soc_lot_id2;
}
EXPORT_SYMBOL(ghost_storage_get_soc_lot_id2);

void ghost_storage_get_panel_cell_id(u8 *date, u8 *coord)
{
	ghost_ensure_ready();
	if (date)
		memcpy(date, ghost_panel_date, sizeof(ghost_panel_date));
	if (coord)
		memcpy(coord, ghost_panel_coord, sizeof(ghost_panel_coord));
}
EXPORT_SYMBOL(ghost_storage_get_panel_cell_id);

void ghost_storage_get_panel_octa_id(char *buf, size_t max_len)
{
	ghost_ensure_ready();
	if (buf && max_len > 0) {
		strncpy(buf, ghost_panel_octa_id, max_len - 1);
		buf[max_len - 1] = '\0';
	}
}
EXPORT_SYMBOL(ghost_storage_get_panel_octa_id);

void ghost_storage_get_panel_manf_code(u8 *code)
{
	ghost_ensure_ready();
	if (code)
		memcpy(code, ghost_panel_manf_code, sizeof(ghost_panel_manf_code));
}
EXPORT_SYMBOL(ghost_storage_get_panel_manf_code);

/* ghost_tremor_sine removed — use shared ghost_sine_table from ghost_thermal.h */

s16 ghost_storage_apply_sensor_jitter(s16 sample, int axis)
{
	u64 now_ns;
	s16 bias;
	int tremor1, tremor2, noise;
	int axis_phase = 0;

	ghost_ensure_ready();

	if (axis < 0 || axis >= 3)
		axis = 0;

	bias = ghost_sensor_bias[axis];

	/* Spatial phase shift: X: 0, Y: 21, Z: 42 (120 deg spatial separation) */
	if (axis == 1)
		axis_phase = 21;
	else if (axis == 2)
		axis_phase = 42;

	now_ns = sched_clock();

	/* Primary harmonic wave: ~9.5 Hz physiological hand tremor */
	tremor1 = (int)ghost_sine_table[((now_ns >> 21) + axis_phase) & 63] / 10; /* approx -12 .. +12 LSB */

	/* Secondary wave: ~14 Hz micro-vibration */
	tremor2 = (int)ghost_sine_table[((now_ns >> 19) + (axis_phase * 2)) & 63] / 24; /* approx -5 .. +5 LSB */

	/* Microscopic stochastic jitter */
	noise = (int)((now_ns ^ (now_ns >> 9)) & 0x7) - 4; /* -4 .. +3 LSB */

	return sample + bias + (s16)(tremor1 + tremor2 + noise);
}
EXPORT_SYMBOL(ghost_storage_apply_sensor_jitter);

void ghost_storage_get_gyro_bias(s16 *gyro_bias)
{
	ghost_ensure_ready();
	if (gyro_bias)
		memcpy(gyro_bias, ghost_gyro_bias, sizeof(ghost_gyro_bias));
}
EXPORT_SYMBOL(ghost_storage_get_gyro_bias);

s16 ghost_storage_get_baro_drift(void)
{
	ghost_ensure_ready();
	return ghost_baro_drift_hpa;
}
EXPORT_SYMBOL(ghost_storage_get_baro_drift);

void ghost_storage_get_scsi_wwid(char *buf, size_t max_len)
{
	ghost_ensure_ready();
	if (buf && max_len > 0) {
		strncpy(buf, ghost_scsi_wwid, max_len - 1);
		buf[max_len - 1] = '\0';
	}
}
EXPORT_SYMBOL(ghost_storage_get_scsi_wwid);

u32 ghost_storage_get_tcp_isn_offset(void)
{
	ghost_ensure_ready();
	return ghost_tcp_isn_offset;
}
EXPORT_SYMBOL(ghost_storage_get_tcp_isn_offset);

u32 ghost_storage_get_tcp_ts_offset(void)
{
	ghost_ensure_ready();
	return ghost_tcp_ts_offset;
}
EXPORT_SYMBOL(ghost_storage_get_tcp_ts_offset);

int ghost_storage_get_battery_cycle(void)
{
	ghost_ensure_ready();
	return ghost_battery_cycle;
}
EXPORT_SYMBOL(ghost_storage_get_battery_cycle);

int ghost_storage_get_battery_asoc(void)
{
	ghost_ensure_ready();
	return ghost_battery_asoc;
}
EXPORT_SYMBOL(ghost_storage_get_battery_asoc);

long ghost_storage_get_ram_delta_pages(void)
{
	ghost_ensure_ready();
	return ghost_ram_carveout_delta_pages;
}
EXPORT_SYMBOL(ghost_storage_get_ram_delta_pages);

void ghost_storage_apply_statfs_geometry(const struct path *path, struct kstatfs *buf)
{
	u64 stock_total_blocks, base_blocks;
	u64 orig_blocks, orig_free, orig_avail;
	u32 block_jitter;

	if (!buf || !ghost_storage_ready)
		return;

	/* Only apply to untrusted apps (UID >= 10000) so vold and system daemons see real geometry */
	if (current_uid().val < 10000)
		return;

	/* Only apply to large data filesystems (F2FS / Ext4 data partition >= 16GB) */
	if (buf->f_type != F2FS_SUPER_MAGIC && buf->f_type != 0xEF53)
		return;

	if (buf->f_bsize != 4096 || buf->f_blocks < (16ULL * 1024 * 1024 * 1024 / 4096))
		return;

	/*
	 * Dynamically detect actual storage capacity:
	 * 128GB: ~28M blocks (107.5 GiB /data)
	 * 256GB: ~56M blocks (220 GiB /data)
	 * 512GB: ~112M blocks (450 GiB /data)
	 */
	orig_blocks = buf->f_blocks;
	if (orig_blocks >= 80000000ULL)
		base_blocks = 112721920ULL; /* 512GB model */
	else if (orig_blocks >= 40000000ULL)
		base_blocks = 56360960ULL;  /* 256GB model */
	else
		base_blocks = 28180480ULL;  /* 128GB model */

	block_jitter = ((u32)ghost_storage_master_seed[14] << 8 | ghost_storage_master_seed[15]) % 32768;
	stock_total_blocks = base_blocks + (u64)block_jitter - 16384ULL;

	orig_free = buf->f_bfree;
	orig_avail = buf->f_bavail;

	if (orig_blocks > 0) {
		u64 simulated_free, simulated_avail;
		u32 fake_used_pct;

		/* Ghost Kernel (Pillar 47): Realistic Storage Usage Jitter (64% - 82% used)
		 * Fresh device/wipe typically has > 90% free space which is a fraud red-flag. */
		fake_used_pct = 64 + (((u32)ghost_storage_master_seed[16] << 4 | (ghost_storage_master_seed[17] & 0x0F)) % 19);
		simulated_free = (stock_total_blocks * (100 - fake_used_pct)) / 100;
		simulated_avail = (simulated_free > (stock_total_blocks / 20)) ?
				  simulated_free - (stock_total_blocks / 20) : simulated_free;

		buf->f_blocks = stock_total_blocks;

		/* If actual free space is suspiciously high (> 80% free), simulate aged device usage */
		if ((orig_free * 100 / orig_blocks) > 80) {
			buf->f_bfree = simulated_free;
			buf->f_bavail = simulated_avail;
		} else {
			buf->f_bfree = (orig_free * stock_total_blocks) / orig_blocks;
			buf->f_bavail = (orig_avail * stock_total_blocks) / orig_blocks;
		}

		/* Standardize inode geometry (1 inode per 4 blocks) */
		buf->f_files = stock_total_blocks / 4;
		if (buf->f_ffree > 0) {
			buf->f_ffree = (buf->f_files * buf->f_bavail) / stock_total_blocks;
		}
	}
}
EXPORT_SYMBOL(ghost_storage_apply_statfs_geometry);

void ghost_storage_get_widevine_device_id(u8 out_id[GHOST_WIDEVINE_ID_LEN])
{
	ghost_ensure_ready();
	if (out_id)
		memcpy(out_id, ghost_widevine_device_id, GHOST_WIDEVINE_ID_LEN);
}
EXPORT_SYMBOL(ghost_storage_get_widevine_device_id);

const char *ghost_storage_get_widevine_device_id_hex(void)
{
	ghost_ensure_ready();
	return ghost_widevine_device_id_hex;
}
EXPORT_SYMBOL(ghost_storage_get_widevine_device_id_hex);

const char *ghost_storage_get_camera_moduleid(int cam_index, const char *orig_prefix)
{
	/* Per-camera static buffers — no lock needed, each camera index
	 * maps to its own dedicated buffer */
	static char buf_rear[GHOST_CAMERA_MOD_LEN];
	static char buf_rear2[GHOST_CAMERA_MOD_LEN];
	static char buf_rear3[GHOST_CAMERA_MOD_LEN];
	static char buf_front[GHOST_CAMERA_MOD_LEN];
	char *target_buf;
	const char *src_moduleid;

	ghost_ensure_ready();

	switch (cam_index) {
	case 1: /* CAM_INFO_FRONT */
	case 3: /* CAM_INFO_FRONT2 */
		target_buf = buf_front;
		src_moduleid = ghost_camera_front_moduleid;
		break;
	case 2: /* CAM_INFO_REAR2 */
		target_buf = buf_rear2;
		src_moduleid = ghost_camera_rear2_moduleid;
		break;
	case 4: /* CAM_INFO_REAR3 */
		target_buf = buf_rear3;
		src_moduleid = ghost_camera_rear3_moduleid;
		break;
	case 0: /* CAM_INFO_REAR */
	default:
		target_buf = buf_rear;
		src_moduleid = ghost_camera_rear_moduleid;
		break;
	}

	if (orig_prefix && strlen(orig_prefix) >= 5)
		snprintf(target_buf, GHOST_CAMERA_MOD_LEN, "%.5s%s", orig_prefix, &src_moduleid[5]);
	else
		strncpy(target_buf, src_moduleid, GHOST_CAMERA_MOD_LEN - 1);
	target_buf[GHOST_CAMERA_MOD_LEN - 1] = '\0';

	return target_buf;
}
EXPORT_SYMBOL(ghost_storage_get_camera_moduleid);

void ghost_storage_get_camera_sensorid(int cam_index, u8 *buf, size_t len)
{
	const u8 *src;
	size_t copy_len;

	if (!buf || len == 0)
		return;

	ghost_ensure_ready();

	switch (cam_index) {
	case 1: /* CAM_INFO_FRONT */
	case 3: /* CAM_INFO_FRONT2 */
		src = ghost_camera_front_sensorid;
		break;
	case 2: /* CAM_INFO_REAR2 */
		src = ghost_camera_rear2_sensorid;
		break;
	case 4: /* CAM_INFO_REAR3 */
		src = ghost_camera_rear3_sensorid;
		break;
	case 0: /* CAM_INFO_REAR */
	default:
		src = ghost_camera_rear_sensorid;
		break;
	}

	copy_len = min_t(size_t, len, sizeof(ghost_camera_rear_sensorid));
	memcpy(buf, src, copy_len);
	if (len > copy_len)
		memset(buf + copy_len, 0, len - copy_len);
}
EXPORT_SYMBOL(ghost_storage_get_camera_sensorid);

static int ghost_storage_proc_show(struct seq_file *m, void *v)
{
	ghost_ensure_ready();

	seq_printf(m, "mode: %s\n",
		   ghost_storage_current_mode == GHOST_STORAGE_PER_FORMAT ? "per-format" : "per-reboot");
	seq_printf(m, "ufs_serial: %s\n", ghost_ufs_serial);
	seq_printf(m, "unique_number: %s\n", ghost_ufs_unique_num);
	seq_printf(m, "cid: %s\n", ghost_ufs_cid);
	seq_printf(m, "manfid: %s\n", ghost_ufs_manfid);
	seq_printf(m, "model: %s\n", ghost_ufs_model);
	seq_printf(m, "rev: %s\n", ghost_ufs_rev);
	seq_printf(m, "soc_unique_id: %010llX\n", ghost_soc_unique_id);
	seq_printf(m, "soc_lot_id: %08X\n", ghost_soc_lot_id);
	seq_printf(m, "soc_lot_id2: %s\n", ghost_soc_lot_id2);
	seq_printf(m, "panel_cell_id: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
		   ghost_panel_date[0], ghost_panel_date[1], ghost_panel_date[2],
		   ghost_panel_date[3], ghost_panel_date[4], ghost_panel_date[5],
		   ghost_panel_date[6], ghost_panel_coord[0], ghost_panel_coord[1],
		   ghost_panel_coord[2], ghost_panel_coord[3]);
	seq_printf(m, "panel_octa_id: %s\n", ghost_panel_octa_id);
	seq_printf(m, "scsi_wwid: %s\n", ghost_scsi_wwid);
	seq_printf(m, "sensor_bias: X=%d Y=%d Z=%d\n",
		   ghost_sensor_bias[0], ghost_sensor_bias[1], ghost_sensor_bias[2]);
	seq_printf(m, "gyro_bias: X=%d Y=%d Z=%d\n",
		   ghost_gyro_bias[0], ghost_gyro_bias[1], ghost_gyro_bias[2]);
	seq_printf(m, "baro_drift_hpa: %d.%d\n",
		   ghost_baro_drift_hpa / 10, abs(ghost_baro_drift_hpa % 10));
	seq_printf(m, "tcp_isn_offset: 0x%08X\n", ghost_tcp_isn_offset);
	seq_printf(m, "tcp_ts_offset: 0x%08X\n", ghost_tcp_ts_offset);
	seq_printf(m, "battery_cycle: %d\n", ghost_battery_cycle);
	seq_printf(m, "battery_asoc: %d%%\n", ghost_battery_asoc);
	seq_printf(m, "camera_rear_moduleid: %s\n", ghost_camera_rear_moduleid);
	seq_printf(m, "camera_rear2_moduleid: %s\n", ghost_camera_rear2_moduleid);
	seq_printf(m, "camera_rear3_moduleid: %s\n", ghost_camera_rear3_moduleid);
	seq_printf(m, "camera_front_moduleid: %s\n", ghost_camera_front_moduleid);
	seq_printf(m, "widevine_device_id: %s\n", ghost_widevine_device_id_hex);
	if (ghost_f2fs_uuid_recorded)
		seq_printf(m, "f2fs_uuid: %pUb\n", ghost_f2fs_uuid);
	else
		seq_puts(m, "f2fs_uuid: not_mounted_yet\n");
	seq_printf(m, "seed_source: %s\n",
		   (ghost_storage_current_mode == GHOST_STORAGE_PER_FORMAT && ghost_f2fs_uuid_recorded)
		   ? "f2fs_superblock_uuid" : "kernel_csprng_ram");

	return 0;
}

void ghost_storage_reroll_all(void)
{
	unsigned long flags;

	write_lock_irqsave(&ghost_storage_rwlock, flags);
	get_random_bytes(ghost_storage_master_seed, sizeof(ghost_storage_master_seed));
	ghost_storage_derive_ids();
	write_unlock_irqrestore(&ghost_storage_rwlock, flags);

	ghost_reroll_serialno();
	ghost_reroll_imei();
	ghost_reroll_network_macs();
	pr_debug("ghost_storage: full dynamic identity reroll completed\n");
}
EXPORT_SYMBOL(ghost_storage_reroll_all);

static ssize_t ghost_storage_proc_write(struct file *file, const char __user *ubuf,
					size_t count, loff_t *ppos)
{
	char kbuf[32];

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (strstr(kbuf, "per-reboot")) {
		ghost_storage_current_mode = GHOST_STORAGE_PER_REBOOT;
		ghost_storage_reroll_all();
		pr_info("ghost_storage: switched to per-reboot mode\n");
	} else if (strstr(kbuf, "per-format")) {
		ghost_storage_current_mode = GHOST_STORAGE_PER_FORMAT;
		if (ghost_f2fs_uuid_recorded) {
			u8 kdf_in[48];
			memcpy(kdf_in, "GHOST_STORAGE_F2FS_KDF_SALT_V1", 30);
			memcpy(kdf_in + 30, ghost_f2fs_uuid, 16);
			sha256(kdf_in, 46, ghost_storage_master_seed);
		} else {
			get_random_bytes(ghost_storage_master_seed, sizeof(ghost_storage_master_seed));
		}
		ghost_storage_derive_ids();
		pr_info("ghost_storage: switched to per-format mode\n");
	} else if (strstr(kbuf, "reroll")) {
		ghost_storage_reroll_all();
	} else {
		return -EINVAL;
	}

	return count;
}

static int ghost_storage_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ghost_storage_proc_show, NULL);
}

static const struct file_operations ghost_storage_proc_fops = {
	.open = ghost_storage_proc_open,
	.read = seq_read,
	.write = ghost_storage_proc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ghost_widevine_proc_show(struct seq_file *m, void *v)
{
	ghost_ensure_ready();

	seq_printf(m, "widevine_device_id: %s\n", ghost_widevine_device_id_hex);
	seq_printf(m, "length: %d\n", GHOST_WIDEVINE_ID_LEN);
	seq_printf(m, "mode: %s\n",
		   ghost_storage_current_mode == GHOST_STORAGE_PER_FORMAT ? "per-format" : "per-reboot");
	return 0;
}

static int ghost_widevine_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ghost_widevine_proc_show, NULL);
}

static const struct file_operations ghost_widevine_proc_fops = {
	.open = ghost_widevine_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t ghost_widevine_raw_proc_read(struct file *file, char __user *buf,
					    size_t count, loff_t *ppos)
{
	ghost_ensure_ready();

	return simple_read_from_buffer(buf, count, ppos,
				       ghost_widevine_device_id,
				       GHOST_WIDEVINE_ID_LEN);
}

static const struct file_operations ghost_widevine_raw_proc_fops = {
	.owner = THIS_MODULE,
	.read = ghost_widevine_raw_proc_read,
	.llseek = default_llseek,
};

static int __init ghost_storage_module_init(void)
{
	ghost_storage_init();
	if (!proc_create("ghost_storage", 0600, NULL, &ghost_storage_proc_fops)) {
		pr_err("ghost_storage: failed to create /proc/ghost_storage\n");
		return -ENOMEM;
	}
	if (!proc_create("ghost_widevine", 0400, NULL, &ghost_widevine_proc_fops)) {
		pr_err("ghost_storage: failed to create /proc/ghost_widevine\n");
		return -ENOMEM;
	}
	if (!proc_create("ghost_widevine_raw", 0400, NULL, &ghost_widevine_raw_proc_fops)) {
		pr_err("ghost_storage: failed to create /proc/ghost_widevine_raw\n");
		return -ENOMEM;
	}
	return 0;
}
late_initcall(ghost_storage_module_init);
