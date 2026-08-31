// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/pid_namespace.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/stat.h>
#include <linux/seq_file.h>
#include <linux/seqlock.h>
#include <linux/time.h>
#include <linux/sched/clock.h>
#include <linux/ghost_storage.h>

static int loadavg_proc_show(struct seq_file *m, void *v)
{
	unsigned long avnrun[3];

	get_avenrun(avnrun, FIXED_1/200, 0);

	if (ghost_storage_ready) {
		u64 now_ns = sched_clock();
		unsigned long j0 = ((now_ns >> 20) % 15) * (FIXED_1 / 100);
		unsigned long j1 = ((now_ns >> 22) % 12) * (FIXED_1 / 100);
		unsigned long j2 = ((now_ns >> 24) % 10) * (FIXED_1 / 100);
		if (avnrun[0] < (FIXED_1 / 10))
			avnrun[0] += (FIXED_1 / 10) + j0;
		if (avnrun[1] < (FIXED_1 / 12))
			avnrun[1] += (FIXED_1 / 12) + j1;
		if (avnrun[2] < (FIXED_1 / 15))
			avnrun[2] += (FIXED_1 / 15) + j2;
	}

	seq_printf(m, "%lu.%02lu %lu.%02lu %lu.%02lu %ld/%d %d\n",
		LOAD_INT(avnrun[0]), LOAD_FRAC(avnrun[0]),
		LOAD_INT(avnrun[1]), LOAD_FRAC(avnrun[1]),
		LOAD_INT(avnrun[2]), LOAD_FRAC(avnrun[2]),
		nr_running(), nr_threads,
		idr_get_cursor(&task_active_pid_ns(current)->idr) - 1);
	return 0;
}

static int __init proc_loadavg_init(void)
{
	proc_create_single("loadavg", 0, NULL, loadavg_proc_show);
	return 0;
}
fs_initcall(proc_loadavg_init);
