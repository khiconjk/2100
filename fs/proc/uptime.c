// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>
#include <linux/time64.h>
#include <linux/math64.h>
#include <linux/ghost_uptime.h>

static int uptime_proc_show(struct seq_file *m, void *v)
{
	struct timespec64 uptime;
	u64 total_uptime_nsec;
	u64 idle_nsec;
	u64 idle_pct;

	ktime_get_boottime_ts64(&uptime);

	total_uptime_nsec = (u64)uptime.tv_sec * NSEC_PER_SEC + uptime.tv_nsec;

	/*
	 * Derive idle percentage from actual sleep/total ratio.
	 * On a real phone, idle time closely tracks deep sleep time
	 * plus some screen-off idle. Use the ghost sleep ratio if
	 * available, otherwise fall back to a reasonable default.
	 */
	if (ghost_uptime_offset_ns > 0)
		idle_pct = div64_u64(ghost_uptime_sleep_offset_ns * 100ULL,
				     ghost_uptime_offset_ns);
	else
		idle_pct = 73;

	idle_nsec = div_u64(total_uptime_nsec * idle_pct, 100);

	seq_printf(m, "%lu.%02lu %lu.%02lu\n",
		   (unsigned long)uptime.tv_sec,
		   (unsigned long)(uptime.tv_nsec / (NSEC_PER_SEC / 100)),
		   (unsigned long)(idle_nsec / NSEC_PER_SEC),
		   (unsigned long)((idle_nsec % NSEC_PER_SEC) / (NSEC_PER_SEC / 100)));

	return 0;
}

static int __init proc_uptime_init(void)
{
	proc_create_single("uptime", 0, NULL, uptime_proc_show);
	return 0;
}
fs_initcall(proc_uptime_init);
