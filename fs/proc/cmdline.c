// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#if __has_include(<linux/ghost_config.h>)
#include <linux/ghost_config.h>
#endif

#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
extern int susfs_spoof_cmdline_or_bootconfig(struct seq_file *m);
#endif

static void sanitize_cmdline_output(char *buf, const struct ghost_profile *prof)
{
	char *p;
	if (!buf)
		return;

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
	while ((p = strstr(buf, "sec_debug.warranty_bit=1")))
		*(p + 23) = '0';

	/* 3. flash.locked: 0 -> 1 */
	while ((p = strstr(buf, "androidboot.flash.locked=0")))
		*(p + 25) = '1';

	/* 4. kg & kg.state */
	while ((p = strstr(buf, "androidboot.kg=0x6"))) {
		memcpy(p + 15, "0x0", 3);
	}
	while ((p = strstr(buf, "sec_debug.kg=0x6"))) {
		memcpy(p + 13, "0x0", 3);
	}
	while ((p = strstr(buf, "kg=0x6"))) {
		memcpy(p + 3, "0x0", 3);
	}
	/* 4b. kg.state: Prenormal -> Completed */
	while ((p = strstr(buf, "androidboot.kg.state=Prenormal"))) {
		memcpy(p + 21, "Completed", 9);
	}
	while ((p = strstr(buf, "sec_debug.kg.state=Prenormal"))) {
		memcpy(p + 19, "Completed", 9);
	}
	while ((p = strstr(buf, "knox.kg.state=Prenormal"))) {
		memcpy(p + 14, "Completed", 9);
	}
	while ((p = strstr(buf, "kg.state=Prenormal"))) {
		memcpy(p + 9, "Completed", 9);
	}

		/* 6. ulcnt: unlock count -> 0 */
	while ((p = strstr(buf, "androidboot.ulcnt="))) {
		char *val = p + 18;
		int digits = 0;
		while (val[digits] >= '0' && val[digits] <= '9')
			digits++;
		if (digits > 0) {
			*val = '0';
			if (digits > 1)
				memmove(val + 1, val + digits, strlen(val + digits) + 1);
		}
		break;
	}

	/* 5. vbmeta.device_state: unlocked -> locked */
	while ((p = strstr(buf, "androidboot.vbmeta.device_state=unlocked"))) {
		memcpy(p + 32, "locked", 6);
		memmove(p + 38, p + 40, strlen(p + 40) + 1);
	}

	/* 6. Wipe ALL occurrences of factory_reset anywhere in cmdline */
	while ((p = strstr(buf, "factory_reset"))) {
		memcpy(p, "reboot       ", 13);
	}

	/* 7. Force ALL bootreason= to reboot */
	p = buf;
	while ((p = strstr(p, "bootreason="))) {
		char *val = p + 11;
		char *space = strchr(val, ' ');
		if (space) {
			size_t old_len = space - val;
			if (old_len >= 6) {
				memcpy(val, "reboot", 6);
				memset(val + 6, ' ', old_len - 6);
			}
			p = space;
		} else {
			size_t old_len = strlen(val);
			if (old_len >= 6) {
				memcpy(val, "reboot", 6);
				memset(val + 6, ' ', old_len - 6);
			}
			break;
		}
	}

	/* 8. Align VBMeta digest, boot_hash, bootkey and verifiedbootkey */
	{
		const char *target_boot_hash = (prof && prof->boot_hash[0]) ?
			prof->boot_hash : "22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781";
		const char *target_boot_key = (prof && prof->boot_key[0]) ?
			prof->boot_key : "78d88bcb03734bebc53a14658b315a500f7c7488357dafe490d283cc726bff95";

		p = buf;
		while ((p = strstr(p, "androidboot.vbmeta.digest="))) {
			memcpy(p + 26, target_boot_hash, 64);
			p += 90;
		}
		p = buf;
		while ((p = strstr(p, "androidboot.boot_hash="))) {
			memcpy(p + 22, target_boot_hash, 64);
			p += 86;
		}
		p = buf;
		while ((p = strstr(p, "androidboot.bootkey="))) {
			memcpy(p + 20, target_boot_key, 64);
			p += 84;
		}
		p = buf;
		while ((p = strstr(p, "androidboot.verifiedbootkey="))) {
			memcpy(p + 28, target_boot_key, 64);
			p += 92;
		}
		p = buf;
		while ((p = strstr(p, "androidboot.vbmeta.public_key_digest="))) {
			memcpy(p + 37, target_boot_key, 64);
			p += 101;
		}
	}

	/* 9. Align serialno */
	{
		const char *target_sn = (prof && prof->serialno[0]) ?
			prof->serialno : "R5CR0000000";
		size_t sn_len = strlen(target_sn);
		p = buf;
		while ((p = strstr(p, "androidboot.serialno="))) {
			char *val = p + 21;
			char *space = strchr(val, ' ');
			size_t old_len = space ? (size_t)(space - val) : strlen(val);
			if (old_len == sn_len) {
				memcpy(val, target_sn, sn_len);
			} else {
				memmove(val + sn_len, val + old_len, strlen(val + old_len) + 1);
				memcpy(val, target_sn, sn_len);
			}
			p = val + sn_len;
		}
	}

	/* 10. Align ap_serial */
	{
		const char *target_ap = (prof && prof->ap_serial[0]) ?
			prof->ap_serial : "0x979BE420021A";
		size_t ap_len = strlen(target_ap);
		p = buf;
		while ((p = strstr(p, "androidboot.ap_serial="))) {
			char *val = p + 22;
			char *space = strchr(val, ' ');
			size_t old_len = space ? (size_t)(space - val) : strlen(val);
			if (old_len == ap_len) {
				memcpy(val, target_ap, ap_len);
			} else {
				memmove(val + ap_len, val + old_len, strlen(val + old_len) + 1);
				memcpy(val, target_ap, ap_len);
			}
			p = val + ap_len;
		}
		p = buf;
		while ((p = strstr(p, "ap_serial="))) {
			char *val;
			char *space;
			size_t old_len;
			if (p > buf && *(p - 1) == '.') {
				p += 10;
				continue;
			}
			val = p + 10;
			space = strchr(val, ' ');
			old_len = space ? (size_t)(space - val) : strlen(val);
			if (old_len == ap_len) {
				memcpy(val, target_ap, ap_len);
			} else {
				memmove(val + ap_len, val + old_len, strlen(val + old_len) + 1);
				memcpy(val, target_ap, ap_len);
			}
			p = val + ap_len;
		}
	}

	/* 11. Align em.did */
	{
		const char *target_did = (prof && prof->em_did[0]) ?
			prof->em_did : "20979be420021a11";
		size_t did_len = strlen(target_did);
		p = buf;
		while ((p = strstr(p, "androidboot.em.did="))) {
			char *val = p + 19;
			char *space = strchr(val, ' ');
			size_t old_len = space ? (size_t)(space - val) : strlen(val);
			if (old_len == did_len) {
				memcpy(val, target_did, did_len);
			} else {
				memmove(val + did_len, val + old_len, strlen(val + old_len) + 1);
				memcpy(val, target_did, did_len);
			}
			p = val + did_len;
		}
		p = buf;
		while ((p = strstr(p, "em.did="))) {
			char *val;
			char *space;
			size_t old_len;
			if (p > buf && *(p - 1) == '.') {
				p += 7;
				continue;
			}
			val = p + 7;
			space = strchr(val, ' ');
			old_len = space ? (size_t)(space - val) : strlen(val);
			if (old_len == did_len) {
				memcpy(val, target_did, did_len);
			} else {
				memmove(val + did_len, val + old_len, strlen(val + old_len) + 1);
				memcpy(val, target_did, did_len);
			}
			p = val + did_len;
		}
	}

	/* 12. Align bore_cnt (boot count) */
	p = buf;
	while ((p = strstr(p, "androidboot.bore_cnt="))) {
		char *val = p + 21;
		char *space = strchr(val, ' ');
		size_t old_len = space ? (size_t)(space - val) : strlen(val);
		if (old_len >= 1) {
			*val = '3';
			if (old_len > 1)
				memmove(val + 1, val + old_len, strlen(val + old_len) + 1);
		}
		p = val + 1;
	}
}

static int cmdline_proc_show(struct seq_file *m, void *v)
{
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	if (!susfs_spoof_cmdline_or_bootconfig(m)) {
		seq_putc(m, '\n');
		return 0;
	}
#endif
	if (saved_command_line) {
		char *buf = kstrdup(saved_command_line, GFP_KERNEL);
		if (buf) {
			struct ghost_profile snap;
			const char *target_boot_hash;
			const char *target_boot_key;
			const char *target_serialno;
			const char *target_ap_serial;
			const char *target_em_did;

			memset(&snap, 0, sizeof(snap));
#if __has_include(<linux/ghost_config.h>)
			ghost_get_profile_snapshot(&snap);
#endif
			target_boot_hash = snap.boot_hash[0] ? snap.boot_hash : "22defff599279ee456bbae21e65c2623cf87660f8eb8cb50d91d5879d703a781";
			target_boot_key = snap.boot_key[0] ? snap.boot_key : "78d88bcb03734bebc53a14658b315a500f7c7488357dafe490d283cc726bff95";
			target_serialno = snap.serialno[0] ? snap.serialno : "R5CR0000000";
			target_ap_serial = snap.ap_serial[0] ? snap.ap_serial : "0x979BE420021A";
			target_em_did = snap.em_did[0] ? snap.em_did : "20979be420021a11";

			sanitize_cmdline_output(buf, &snap);
			seq_printf(m, "%s", buf);
			if (!strstr(buf, "androidboot.serialno="))
				seq_printf(m, " androidboot.serialno=%s", target_serialno);
			if (!strstr(buf, "androidboot.ap_serial="))
				seq_printf(m, " androidboot.ap_serial=%s", target_ap_serial);
			if (!strstr(buf, "androidboot.em.did="))
				seq_printf(m, " androidboot.em.did=%s", target_em_did);
			if (!strstr(buf, "androidboot.bore_cnt="))
				seq_puts(m, " androidboot.bore_cnt=3");
			if (!strstr(buf, "androidboot.bootreason="))
				seq_puts(m, " androidboot.bootreason=reboot");
			if (!strstr(buf, "androidboot.vbmeta.device_state="))
				seq_puts(m, " androidboot.vbmeta.device_state=locked");
			if (!strstr(buf, "androidboot.vbmeta.size="))
				seq_puts(m, " androidboot.vbmeta.size=4096");
			if (!strstr(buf, "androidboot.vbmeta.digest="))
				seq_printf(m, " androidboot.vbmeta.digest=%s", target_boot_hash);
			if (!strstr(buf, "androidboot.boot_hash="))
				seq_printf(m, " androidboot.boot_hash=%s", target_boot_hash);
			if (!strstr(buf, "androidboot.bootkey="))
				seq_printf(m, " androidboot.bootkey=%s", target_boot_key);
			if (!strstr(buf, "androidboot.verifiedbootkey="))
				seq_printf(m, " androidboot.verifiedbootkey=%s", target_boot_key);
			if (!strstr(buf, "androidboot.vbmeta.public_key_digest="))
				seq_printf(m, " androidboot.vbmeta.public_key_digest=%s", target_boot_key);
			if (!strstr(buf, "androidboot.vbmeta.avb_version="))
				seq_puts(m, " androidboot.vbmeta.avb_version=1.2");
			if (!strstr(buf, "androidboot.vbmeta.hash_alg="))
				seq_puts(m, " androidboot.vbmeta.hash_alg=sha256");
			seq_putc(m, '\n');
			kfree(buf);
			return 0;
		}
	}
	seq_printf(m, "%s\n", saved_command_line ? saved_command_line : "");
	return 0;
}

static int __init proc_cmdline_init(void)
{
	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);
