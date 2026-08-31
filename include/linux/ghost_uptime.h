/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_UPTIME_H
#define _LINUX_GHOST_UPTIME_H

#include <linux/time64.h>
#include <linux/types.h>

struct inode;
struct kstat;
struct task_struct;

extern u64 ghost_uptime_offset_ns;

void ghost_uptime_apply_boot_offset(struct timespec64 *boot_offset,
				    time64_t wall_sec);
void ghost_uptime_apply_realtime(struct timespec64 *wall_time);
void ghost_uptime_apply_realtime_for_settimeofday(struct timespec64 *wall_time);
void ghost_uptime_restore_realtime_for_rtc(struct timespec64 *wall_time);
void ghost_uptime_audit_adjtimex(unsigned int modes);
void ghost_uptime_audit_timekeeping_inject_offset(int result);
void ghost_uptime_apply_stat(struct inode *inode, struct kstat *stat);
unsigned long long ghost_uptime_apply_proc_start_time(
	struct task_struct *task, unsigned long long start_time);

#endif /* _LINUX_GHOST_UPTIME_H */
