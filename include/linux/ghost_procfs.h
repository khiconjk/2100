/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_PROCFS_H
#define _LINUX_GHOST_PROCFS_H

#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/uidgid.h>
#include <linux/string.h>

#ifndef GHOST_STEALTH
#define GHOST_STEALTH 0
#endif

#ifdef CONFIG_KSU_SUSFS
extern bool susfs_is_current_ksu_domain(void);
#endif

static inline bool ghost_is_untrusted_app(void)
{
#if !GHOST_STEALTH
	return false;
#else
	return (current_uid().val >= 10000);
#endif
}

static inline bool ghost_is_sensitive_task_name(const char *comm)
{
	if (!comm)
		return false;

	if (strstr(comm, "busybox") ||
	    strstr(comm, "TrickyStore") ||
	    strstr(comm, "tricky_store") ||
	    strstr(comm, "sec_carrier_svc") ||
	    strstr(comm, "mazoku") ||
	    strstr(comm, "machikado") ||
	    strstr(comm, "service.apk") ||
	    !strcmp(comm, "daemon") ||
	    !strcmp(comm, "ksud") ||
	    !strcmp(comm, "daemonsu") ||
	    !strcmp(comm, "magisk") ||
	    !strcmp(comm, "su") ||
	    !strncmp(comm, "ghost_", 6))
		return true;

	return false;
}

static inline bool ghost_is_sensitive_task(struct task_struct *task)
{
	if (!task)
		return false;

#ifdef CONFIG_KSU_SUSFS
	if (susfs_is_current_ksu_domain())
		return false;
#endif
	if (current_uid().val == 0)
		return false;

	return ghost_is_sensitive_task_name(task->comm);
}

static inline bool ghost_can_see_pid(struct task_struct *target)
{
#if !GHOST_STEALTH
	return true;
#else
	kuid_t cur_uid = current_uid();

	/* System UIDs (root, system, adb shell, etc.) have full visibility */
	if (cur_uid.val < 10000)
		return true;

#ifdef CONFIG_KSU_SUSFS
	if (susfs_is_current_ksu_domain())
		return true;
#endif

	if (!target)
		return false;

	/*
	 * Only hide sensitive root/KSU daemon tasks from untrusted apps.
	 * All other PIDs remain visible — this matches stock Android behavior
	 * where apps can enumerate all PIDs in /proc via readdir.
	 */
	if (ghost_is_sensitive_task_name(target->comm))
		return false;

	return true;
#endif
}

#endif /* _LINUX_GHOST_PROCFS_H */
