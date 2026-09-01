// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ghost_net.h>

#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
extern int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m);
#endif

static void ghost_filter_cmdline(struct seq_file *m, const char *src)
{
	char *buf, *p;
	size_t len;

	if (!src)
		return;

	len = strlen(src);
	buf = kmalloc(len + 256, GFP_KERNEL);
	if (!buf) {
		seq_printf(m, "%s\n", src);
		return;
	}

	strcpy(buf, src);

	/* 1. verifiedbootstate: orange/yellow/red -> green */
	while ((p = strstr(buf, "androidboot.verifiedbootstate=orange"))) {
		memcpy(p + 30, "green", 5);
		memmove(p + 35, p + 36, strlen(p + 36) + 1);
	}
	while ((p = strstr(buf, "androidboot.verifiedbootstate=yellow"))) {
		memcpy(p + 30, "green", 5);
		memmove(p + 35, p + 36, strlen(p + 36) + 1);
	}
	while ((p = strstr(buf, "androidboot.verifiedbootstate=red"))) {
		memcpy(p + 30, "green", 5);
		memmove(p + 35, p + 33, strlen(p + 33) + 1);
	}

	/* 2. warranty_bit: 1 -> 0 */
	while ((p = strstr(buf, "androidboot.warranty_bit=1")))
		*(p + 25) = '0';

	/* 3. flash.locked: 0 -> 1 */
	while ((p = strstr(buf, "androidboot.flash.locked=0")))
		*(p + 25) = '1';

	/* 4. vbmeta.device_state: unlocked -> locked */
	while ((p = strstr(buf, "androidboot.vbmeta.device_state=unlocked"))) {
		memcpy(p + 32, "locked", 6);
		memmove(p + 38, p + 40, strlen(p + 40) + 1);
	}

	/* 5. serialno: replace androidboot.serialno=<old> with ghost_serialno */
	if (!ghost_serialno_ready)
		ghost_init_serialno();
	p = strstr(buf, "androidboot.serialno=");
	if (p) {
		char *end = strchr(p + 21, ' ');
		size_t old_len = end ? (size_t)(end - (p + 21)) : strlen(p + 21);
		size_t new_len = strlen(ghost_serialno);
		if (old_len == new_len) {
			memcpy(p + 21, ghost_serialno, new_len);
		} else {
			memmove(p + 21 + new_len, p + 21 + old_len, strlen(p + 21 + old_len) + 1);
			memcpy(p + 21, ghost_serialno, new_len);
		}
	}

	/* 6. bootreason: recovery/factory_reset -> reboot */
	while ((p = strstr(buf, "androidboot.bootreason=reboot,factory_reset"))) {
		memcpy(p + 23, "reboot", 6);
		memmove(p + 29, p + 43, strlen(p + 43) + 1);
	}
	while ((p = strstr(buf, "androidboot.bootreason=reboot,recovery"))) {
		memcpy(p + 23, "reboot", 6);
		memmove(p + 29, p + 38, strlen(p + 38) + 1);
	}
	while ((p = strstr(buf, "androidboot.bootreason=recovery"))) {
		memcpy(p + 23, "reboot", 6);
		memmove(p + 29, p + 31, strlen(p + 31) + 1);
	}
	while ((p = strstr(buf, "androidboot.bootreason=factory_reset"))) {
		memcpy(p + 23, "reboot", 6);
		memmove(p + 29, p + 36, strlen(p + 36) + 1);
	}

	seq_printf(m, "%s", buf);

	/* 6. Append locked & dsms flags if not present */
	if (!strstr(buf, "androidboot.flash.locked="))
		seq_puts(m, " androidboot.flash.locked=1");
	if (!strstr(buf, "androidboot.vbmeta.device_state="))
		seq_puts(m, " androidboot.vbmeta.device_state=locked");
	if (!strstr(buf, "androidboot.dsms="))
		seq_puts(m, " androidboot.dsms=0 androidboot.dsmsd=0");

	seq_putc(m, '\n');
	kfree(buf);
}

static int cmdline_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	if (!susfs_spoof_cmdline_or_bootconfig(m)) {
		seq_putc(m, '\n');
		return 0;
	}
#endif
	ghost_filter_cmdline(m, saved_command_line);
	return 0;
}

static int ghost_bootloader_proc_show(struct seq_file *m, void *v)
{
	if (!ghost_serialno_ready)
		ghost_init_serialno();

	seq_puts(m, "verifiedbootstate: green\n");
	seq_puts(m, "flash_locked: 1\n");
	seq_puts(m, "vbmeta_device_state: locked\n");
	seq_puts(m, "warranty_bit: 0\n");
	seq_printf(m, "serialno: %s\n", ghost_serialno);
	return 0;
}

static int __init proc_cmdline_init(void)
{
	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	proc_create_single("ghost_bootloader", 0400, NULL, ghost_bootloader_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);