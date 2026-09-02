/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_THERMAL_H
#define _LINUX_GHOST_THERMAL_H

#include <linux/types.h>

/*
 * Shared 64-entry sine approximation table (0..2π mapped to 0..63).
 * Range: -127..+127. Used by ghost_thermal.c and ghost_storage.c.
 */
extern const s8 ghost_sine_table[64];

int ghost_apply_thermal_entropy(const char *zone_name, int raw_temp);
int ghost_apply_battery_temp_entropy(int raw_temp);
int ghost_apply_battery_voltage_physics(int raw_mv);
int ghost_apply_battery_current_entropy(int raw_ua);

#endif /* _LINUX_GHOST_THERMAL_H */
