// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>
#include <linux/string.h>

static int version_proc_show(struct seq_file *m, void *v)
{
	/* Ghost Kernel (Pillar 30): Normalize /proc/version to stock Samsung format */
	seq_printf(m, "Linux version %s (%s) (%s) %s\n",
		   utsname()->release,
		   "dpi@SWDG4608",  /* Stock Samsung build user@host */
		   "Android (8186898, based on r416183b) clang version 12.0.5",
		   "#1 SMP PREEMPT Mon Dec 06 17:22:42 KST 2021");
	return 0;
}

static int __init proc_version_init(void)
{
	proc_create_single("version", 0, NULL, version_proc_show);
	return 0;
}
fs_initcall(proc_version_init);
