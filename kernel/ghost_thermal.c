// SPDX-License-Identifier: GPL-2.0
/*
 * Ghost Thermal: Dynamic Thermal Entropy & Microscopic Thermodynamic Physical Jitter
 *
 * Simulates natural thermoelectric noise and continuous ADC quantization jitter
 * on thermal zones (CPU clusters, GPU, NPU, ISP, CP) and battery subsystem.
 *
 * Prevents detection by Device Fingerprinting and Anti-Fraud SDKs that look for
 * artificial, static or stepped thermal curves.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/sched/clock.h>
#include <linux/string.h>
#include <linux/ghost_thermal.h>

/*
 * Shared 64-entry sine approximation table (0..2π mapped to 0..63, -127..+127).
 * Exported via ghost_thermal.h for use by ghost_storage.c sensor jitter.
 */
const s8 ghost_sine_table[64] = {
	0,   12,  25,  37,  49,  60,  71,  81,
	90,  98,  106, 112, 117, 122, 125, 126,
	127, 126, 125, 122, 117, 112, 106, 98,
	90,  81,  71,  60,  49,  37,  25,  12,
	0,   -12, -25, -37, -49, -60, -71, -81,
	-90, -98, -106,-112,-117,-122,-125,-126,
	-127,-126,-125,-122,-117,-112,-106,-98,
	-90, -81, -71, -60, -49, -37, -25, -12
};
EXPORT_SYMBOL(ghost_sine_table);

int ghost_apply_thermal_entropy(const char *zone_name, int raw_temp)
{
	u64 now_ns;
	u32 jif;
	int wave1, wave2, noise;
	int total_offset;
	int zone_bias = 0;

	/* Only apply to positive valid temperatures (e.g. 10C to 105C) */
	if (raw_temp < 10000 || raw_temp > 105000)
		return raw_temp;

	now_ns = sched_clock();
	jif = (u32)jiffies;

	/* Differentiate phase by zone name */
	if (zone_name) {
		if (!strcmp(zone_name, "BIG"))
			zone_bias = 17;
		else if (!strcmp(zone_name, "MID"))
			zone_bias = 33;
		else if (!strcmp(zone_name, "LITTLE"))
			zone_bias = 49;
		else if (!strcmp(zone_name, "G3D"))
			zone_bias = 11;
		else if (!strcmp(zone_name, "NPU"))
			zone_bias = 27;
		else if (!strcmp(zone_name, "ISP"))
			zone_bias = 41;
		else if (!strcmp(zone_name, "CP"))
			zone_bias = 53;
	}

	/* Harmonic Wave 1: Slow thermal inertia oscillation (~0.2 Hz) */
	wave1 = (int)ghost_sine_table[((jif >> 1) + zone_bias) & 63] * 2; /* -254 .. +254 mC */

	/* Harmonic Wave 2: Faster ADC clock fluctuation */
	wave2 = (int)ghost_sine_table[((now_ns >> 24) + (zone_bias * 3)) & 63] / 2; /* -63 .. +63 mC */

	/* Microscopic Gaussian noise: -40 .. +40 mC */
	noise = (int)((now_ns ^ (now_ns >> 8)) & 0x7F) - 64;

	total_offset = (wave1 + wave2 + noise) / 2; /* approx -200 .. +200 mC */

	return raw_temp + total_offset;
}
EXPORT_SYMBOL(ghost_apply_thermal_entropy);

int ghost_apply_battery_temp_entropy(int raw_temp)
{
	u32 jif = (u32)jiffies;
	int wave;

	/* Battery temp is in 0.1 deg C (e.g. 350 for 35.0 C) */
	if (raw_temp < 100 || raw_temp > 650)
		return raw_temp;

	/* Battery has large thermal mass, only gentle +/- 0.1C to 0.2C jitter */
	wave = (int)ghost_sine_table[(jif >> 3) & 63];
	if (wave > 75)
		return raw_temp + 1;
	else if (wave < -75)
		return raw_temp - 1;

	return raw_temp;
}
EXPORT_SYMBOL(ghost_apply_battery_temp_entropy);

int ghost_apply_battery_voltage_physics(int raw_mv)
{
	u64 now_ns;
	u32 jif;
	int wave, noise;
	int delta_mv;

	/* Typical phone battery voltage is 3000 mV to 4500 mV */
	if (raw_mv < 3000 || raw_mv > 4500)
		return raw_mv;

	now_ns = sched_clock();
	jif = (u32)jiffies;

	/* Dynamic electrochemical cell voltage fluctuations (-6 mV to +6 mV) */
	wave = (int)ghost_sine_table[(jif >> 4) & 63] / 20;

	/* Microscopic ADC quantization noise (-2 mV to +2 mV) */
	noise = (int)((now_ns ^ (now_ns >> 7)) & 0x7) - 3;

	delta_mv = wave + (noise / 2);

	return raw_mv + delta_mv;
}
EXPORT_SYMBOL(ghost_apply_battery_voltage_physics);

int ghost_apply_battery_current_entropy(int raw_ua)
{
	u64 now_ns;
	u32 jif;
	int wave, noise;
	int delta_ua;

	/* Only apply when device is not in deep shutdown or extreme bounds */
	if (raw_ua < -5000000 || raw_ua > 5000000)
		return raw_ua;

	now_ns = sched_clock();
	jif = (u32)jiffies;

	/* Dynamic CPU/display current oscillation (-15000 uA to +15000 uA) */
	wave = (int)ghost_sine_table[(jif >> 2) & 63] * 120;

	/* High frequency switching regulator noise (-4000 uA to +4000 uA) */
	noise = (int)((now_ns ^ (now_ns >> 6)) & 0x1F) * 250 - 4000;

	delta_ua = wave + noise;

	return raw_ua + delta_ua;
}
EXPORT_SYMBOL(ghost_apply_battery_current_entropy);
