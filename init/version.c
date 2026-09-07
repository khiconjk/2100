// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/init/version.c
 *
 *  Copyright (C) 1992  Theodore Ts'o
 *
 *  May be freely distributed as part of Linux.
 */

#include <generated/compile.h>
#include <linux/build-salt.h>
#include <linux/export.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <generated/utsrelease.h>
#include <linux/version.h>
#include <linux/proc_ns.h>

#ifndef CONFIG_KALLSYMS
#define version(a) Version_ ## a
#define version_string(a) version(a)

extern int version_string(LINUX_VERSION_CODE);
int version_string(LINUX_VERSION_CODE);
#endif

#define STOCK_UTS_RELEASE "5.4.129-22936777-abG991BXXS3BULC"
#define STOCK_COMPILE_BY "dpi"
#define STOCK_COMPILE_HOST "21DJ6C20"
#define STOCK_COMPILER "Android (7284624, based on r416183b) Clang version 12.0.5"
#define STOCK_UTS_VERSION "#1 SMP PREEMPT Tue Dec 21 19:10:34 KST 2021"

struct uts_namespace init_uts_ns = {
	.kref = KREF_INIT(2),
	.name = {
		.sysname	= UTS_SYSNAME,
		.nodename	= UTS_NODENAME,
		.release	= STOCK_UTS_RELEASE,
		.version	= STOCK_UTS_VERSION,
		.machine	= UTS_MACHINE,
		.domainname	= UTS_DOMAINNAME,
	},
	.user_ns = &init_user_ns,
	.ns.inum = PROC_UTS_INIT_INO,
#ifdef CONFIG_UTS_NS
	.ns.ops = &utsns_operations,
#endif
};
EXPORT_SYMBOL_GPL(init_uts_ns);

/* FIXED STRINGS! Don't touch! */
const char linux_banner[] =
	"Linux version " STOCK_UTS_RELEASE " (" STOCK_COMPILE_BY "@"
	STOCK_COMPILE_HOST ") (" STOCK_COMPILER ") " STOCK_UTS_VERSION "\n";

const char linux_proc_banner[] =
	"%s version %s (" STOCK_COMPILE_BY "@" STOCK_COMPILE_HOST ") (" STOCK_COMPILER ") %s\n";

BUILD_SALT;
