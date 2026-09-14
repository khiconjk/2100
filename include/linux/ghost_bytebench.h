/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_BYTEBENCH_H
#define _LINUX_GHOST_BYTEBENCH_H

#include <linux/types.h>
#include <linux/time64.h>
#include <linux/fs.h>

#if IS_ENABLED(CONFIG_GHOST_KERNEL)

extern int ghost_bytebench_enable_plan_b;
extern int ghost_bytebench_dilation_percent;
extern int ghost_bytebench_enable_plan_c;
extern u64 ghost_bytebench_max_window_ms;
extern char ghost_bytebench_target_pkg[64];

void ghost_apply_time_dilation(clockid_t which_clock, struct timespec64 *ts);
bool ghost_is_bytebench_io_test_file(struct file *file);

#else

static inline void ghost_apply_time_dilation(clockid_t which_clock,
					     struct timespec64 *ts)
{
}

static inline bool ghost_is_bytebench_io_test_file(struct file *file)
{
	return false;
}

#endif /* CONFIG_GHOST_KERNEL */

#endif /* _LINUX_GHOST_BYTEBENCH_H */
