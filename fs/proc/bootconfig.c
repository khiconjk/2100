// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/bootconfig - Extra boot configuration
 */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/bootconfig.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ghost_net.h>

static char *saved_boot_config;

#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
extern int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m);
#endif

static void ghost_filter_bootconfig(struct seq_file *m, const char *src)
{
	char *buf, *p;
	size_t len;

	if (!src)
		return;

	len = strlen(src);
	buf = kmalloc(len + 256, GFP_KERNEL);
	if (!buf) {
		seq_puts(m, src);
		return;
	}

	strcpy(buf, src);

	while ((p = strstr(buf, "androidboot.verifiedbootstate = \"orange\"")))
		memcpy(p + 33, "\"green\" ", 8);
	while ((p = strstr(buf, "androidboot.verifiedbootstate = \"yellow\"")))
		memcpy(p + 33, "\"green\" ", 8);
	while ((p = strstr(buf, "androidboot.verifiedbootstate = \"red\"")))
		memcpy(p + 33, "\"green\" ", 8);

	while ((p = strstr(buf, "androidboot.flash.locked = \"0\"")))
		*(p + 28) = '1';

	while ((p = strstr(buf, "androidboot.warranty_bit = \"1\"")))
		*(p + 28) = '0';

	while ((p = strstr(buf, "androidboot.vbmeta.device_state = \"unlocked\"")))
		memcpy(p + 35, "\"locked\"  ", 10);

	if (!ghost_serialno_ready)
		ghost_init_serialno();
	p = strstr(buf, "androidboot.serialno = \"");
	if (p) {
		char *q = strchr(p + 24, '"');
		if (q) {
			size_t old_len = (size_t)(q - (p + 24));
			size_t new_len = strlen(ghost_serialno);
			memmove(p + 24 + new_len, q, strlen(q) + 1);
			memcpy(p + 24, ghost_serialno, new_len);
		}
	}

	seq_puts(m, buf);
	kfree(buf);
}

static int boot_config_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	if (saved_boot_config) {
		if (!susfs_spoof_cmdline_or_bootconfig(m)) {
			return 0;
		}
	}
#endif
	if (saved_boot_config)
		ghost_filter_bootconfig(m, saved_boot_config);
	return 0;
}

/* Rest size of buffer */
#define rest(dst, end) ((end) > (dst) ? (end) - (dst) : 0)

/* Return the needed total length if @size is 0 */
static int __init copy_xbc_key_value_list(char *dst, size_t size)
{
	struct xbc_node *leaf, *vnode;
	char *key, *end = dst + size;
	const char *val;
	char q;
	int ret = 0;

	key = kzalloc(XBC_KEYLEN_MAX, GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	xbc_for_each_key_value(leaf, val) {
		ret = xbc_node_compose_key(leaf, key, XBC_KEYLEN_MAX);
		if (ret < 0)
			break;
		ret = snprintf(dst, rest(dst, end), "%s = ", key);
		if (ret < 0)
			break;
		dst += ret;
		vnode = xbc_node_get_child(leaf);
		if (vnode) {
			xbc_array_for_each_value(vnode, val) {
				if (strchr(val, '"'))
					q = '\'';
				else
					q = '"';
				ret = snprintf(dst, rest(dst, end), "%c%s%c%s",
					q, val, q, xbc_node_is_array(vnode) ? ", " : "\n");
				if (ret < 0)
					goto out;
				dst += ret;
			}
		} else {
			ret = snprintf(dst, rest(dst, end), "\"\"\n");
			if (ret < 0)
				break;
			dst += ret;
		}
	}
out:
	kfree(key);

	return ret < 0 ? ret : dst - (end - size);
}

static int __init proc_boot_config_init(void)
{
	int len;

	len = copy_xbc_key_value_list(NULL, 0);
	if (len < 0)
		return len;

	if (len > 0) {
		saved_boot_config = kzalloc(len + 1, GFP_KERNEL);
		if (!saved_boot_config)
			return -ENOMEM;

		len = copy_xbc_key_value_list(saved_boot_config, len + 1);
		if (len < 0) {
			kfree(saved_boot_config);
			return len;
		}
	}

	proc_create_single("bootconfig", 0, NULL, boot_config_proc_show);

	return 0;
}
fs_initcall(proc_boot_config_init);
