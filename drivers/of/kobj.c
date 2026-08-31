// SPDX-License-Identifier: GPL-2.0
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ghost_net.h>

#include "of_private.h"

/* true when node is initialized */
static int of_node_is_initialized(struct device_node *node)
{
	return node && node->kobj.state_initialized;
}

/* true when node is attached (i.e. present on sysfs) */
int of_node_is_attached(struct device_node *node)
{
	return node && node->kobj.state_in_sysfs;
}


#ifndef CONFIG_OF_DYNAMIC
static void of_node_release(struct kobject *kobj)
{
	/* Without CONFIG_OF_DYNAMIC, no nodes gets freed */
}
#endif /* CONFIG_OF_DYNAMIC */

struct kobj_type of_node_ktype = {
	.release = of_node_release,
};

static ssize_t of_node_property_read(struct file *filp, struct kobject *kobj,
				struct bin_attribute *bin_attr, char *buf,
				loff_t offset, size_t count)
{
	struct property *pp = container_of(bin_attr, struct property, attr);
	const void *val = pp->value;
	size_t len = pp->length;
	ssize_t ret;

	if (pp->name) {
		if (!strcmp(pp->name, "verifiedbootstate")) {
			val = "green\0";
			len = 6;
		} else if (!strcmp(pp->name, "device_state")) {
			val = "locked\0";
			len = 7;
		} else if (!strcmp(pp->name, "warranty_bit")) {
			val = "0\0";
			len = 2;
		} else if (!strcmp(pp->name, "serial-number") || !strcmp(pp->name, "serialno")) {
			if (!ghost_serialno_ready)
				ghost_init_serialno();
			val = ghost_serialno;
			len = strlen(ghost_serialno) + 1;
		}
	}
	ret = memory_read_from_buffer(buf, count, &offset, val, len);

	if (ret > 0 && pp->name && (!strcmp(pp->name, "bootargs") || !strcmp(pp->name, "bootargs_ext"))) {
		char *p;
		char *end = buf + ret;

		/* 1. verifiedbootstate: orange/yellow -> green  */
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.verifiedbootstate=orange"))) {
			if (p + 36 <= end)
				memcpy(p + 30, "green ", 6);
			p += 36;
		}
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.verifiedbootstate=yellow"))) {
			if (p + 36 <= end)
				memcpy(p + 30, "green ", 6);
			p += 36;
		}
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.verifiedbootstate=red"))) {
			if (p + 33 <= end)
				memcpy(p + 30, "grn", 3);
			p += 33;
		}

		/* 2. warranty_bit: 1 -> 0 */
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.warranty_bit=1"))) {
			*(p + 25) = '0';
			p += 26;
		}

		/* 3. flash.locked: 0 -> 1 */
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.flash.locked=0"))) {
			*(p + 25) = '1';
			p += 26;
		}

		/* 4. vbmeta.device_state: unlocked -> locked   */
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.vbmeta.device_state=unlocked"))) {
			if (p + 40 <= end)
				memcpy(p + 32, "locked  ", 8);
			p += 40;
		}

		/* 5. serialno */
		if (!ghost_serialno_ready)
			ghost_init_serialno();
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.serialno="))) {
			if (p + 21 + 11 <= end) {
				memcpy(p + 21, ghost_serialno, 11);
			}
			p += 32;
		}

		/* 6. ap_serial */
		p = buf;
		while (p < end && (p = strstr(p, "androidboot.ap_serial=0x8BF5CC45CC10"))) {
			if (p + 36 <= end)
				memcpy(p + 22, "0x100000000000", 14);
			p += 36;
		}
	}

	return ret;
}

/* always return newly allocated name, caller must free after use */
static const char *safe_name(struct kobject *kobj, const char *orig_name)
{
	const char *name = orig_name;
	struct kernfs_node *kn;
	int i = 0;

	/* don't be a hero. After 16 tries give up */
	while (i < 16 && (kn = sysfs_get_dirent(kobj->sd, name))) {
		sysfs_put(kn);
		if (name != orig_name)
			kfree(name);
		name = kasprintf(GFP_KERNEL, "%s#%i", orig_name, ++i);
	}

	if (name == orig_name) {
		name = kstrdup(orig_name, GFP_KERNEL);
	} else {
		pr_warn("Duplicate name in %s, renamed to \"%s\"\n",
			kobject_name(kobj), name);
	}
	return name;
}

int __of_add_property_sysfs(struct device_node *np, struct property *pp)
{
	int rc;

	/* Important: Don't leak passwords */
	bool secure = strncmp(pp->name, "security-", 9) == 0;

	if (!IS_ENABLED(CONFIG_SYSFS))
		return 0;

	if (!of_kset || !of_node_is_attached(np))
		return 0;

	sysfs_bin_attr_init(&pp->attr);
	pp->attr.attr.name = safe_name(&np->kobj, pp->name);
	pp->attr.attr.mode = secure ? 0400 : 0444;
	pp->attr.size = secure ? 0 : pp->length;
	pp->attr.read = of_node_property_read;

	rc = sysfs_create_bin_file(&np->kobj, &pp->attr);
	WARN(rc, "error adding attribute %s to node %pOF\n", pp->name, np);
	return rc;
}

void __of_sysfs_remove_bin_file(struct device_node *np, struct property *prop)
{
	if (!IS_ENABLED(CONFIG_SYSFS))
		return;

	sysfs_remove_bin_file(&np->kobj, &prop->attr);
	kfree(prop->attr.attr.name);
}

void __of_remove_property_sysfs(struct device_node *np, struct property *prop)
{
	/* at early boot, bail here and defer setup to of_init() */
	if (of_kset && of_node_is_attached(np))
		__of_sysfs_remove_bin_file(np, prop);
}

void __of_update_property_sysfs(struct device_node *np, struct property *newprop,
		struct property *oldprop)
{
	/* At early boot, bail out and defer setup to of_init() */
	if (!of_kset)
		return;

	if (oldprop)
		__of_sysfs_remove_bin_file(np, oldprop);
	__of_add_property_sysfs(np, newprop);
}

int __of_attach_node_sysfs(struct device_node *np)
{
	const char *name;
	struct kobject *parent;
	struct property *pp;
	int rc;

	if (!IS_ENABLED(CONFIG_SYSFS) || !of_kset)
		return 0;

	np->kobj.kset = of_kset;
	if (!np->parent) {
		/* Nodes without parents are new top level trees */
		name = safe_name(&of_kset->kobj, "base");
		parent = NULL;
	} else {
		name = safe_name(&np->parent->kobj, kbasename(np->full_name));
		parent = &np->parent->kobj;
	}
	if (!name)
		return -ENOMEM;

	rc = kobject_add(&np->kobj, parent, "%s", name);
	kfree(name);
	if (rc)
		return rc;

	for_each_property_of_node(np, pp)
		__of_add_property_sysfs(np, pp);

	of_node_get(np);
	return 0;
}

void __of_detach_node_sysfs(struct device_node *np)
{
	struct property *pp;

	BUG_ON(!of_node_is_initialized(np));
	if (!of_kset)
		return;

	/* only remove properties if on sysfs */
	if (of_node_is_attached(np)) {
		for_each_property_of_node(np, pp)
			__of_sysfs_remove_bin_file(np, pp);
		kobject_del(&np->kobj);
	}

	of_node_put(np);
}
