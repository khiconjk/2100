// SPDX-License-Identifier: GPL-2.0
/*
 * Pure deterministic test identity generator for mock telephony test harness.
 * Strictly for engineering/mock testing. Uses GSMA test allocations only.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/errno.h>
#include <crypto/sha.h>
#include <linux/ghost_test_identity.h>

/* GSMA allocated non-routable test TAC prefix (3GPP TS 23.003) */
static const char GHOST_TEST_TAC_PREFIX[] = "00100100";

char ghost_test_compute_luhn(const char digits[14])
{
	int sum = 0;
	int i;

	for (i = 0; i < 14; i++) {
		int d = digits[i] - '0';
		if (d < 0 || d > 9)
			d = 0;

		/* Odd-indexed positions (1-based even positions 2, 4, 6... 14) are doubled */
		if (i % 2 == 1) {
			d *= 2;
			if (d >= 10)
				d -= 9;
		}
		sum += d;
	}

	return (char)('0' + ((10 - (sum % 10)) % 10));
}
EXPORT_SYMBOL_GPL(ghost_test_compute_luhn);

bool ghost_test_imei_luhn_valid(const char imei[16])
{
	int sum = 0;
	int i;

	if (!imei)
		return false;

	for (i = 0; i < 15; i++) {
		int d;
		if (!isdigit(imei[i]))
			return false;

		d = imei[i] - '0';
		if (i % 2 == 1) {
			d *= 2;
			if (d >= 10)
				d -= 9;
		}
		sum += d;
	}

	return (sum % 10 == 0);
}
EXPORT_SYMBOL_GPL(ghost_test_imei_luhn_valid);

static void ghost_derive_single_imei(const char *label, size_t label_len,
				     const u8 *seed, size_t seed_len,
				     char out_imei[16])
{
	u8 digest[SHA256_DIGEST_SIZE];
	u8 input_buf[128];
	size_t copy_seed;
	int i;

	if (label_len > sizeof(input_buf))
		label_len = sizeof(input_buf);
	memcpy(input_buf, label, label_len);

	copy_seed = seed_len;
	if (copy_seed > (sizeof(input_buf) - label_len))
		copy_seed = sizeof(input_buf) - label_len;
	memcpy(input_buf + label_len, seed, copy_seed);

	sha256(input_buf, label_len + copy_seed, digest);

	/* 1. First 8 digits: Non-routable GSMA Test TAC */
	memcpy(out_imei, GHOST_TEST_TAC_PREFIX, 8);

	/* 2. Next 6 digits: Derived from digest */
	for (i = 0; i < 6; i++)
		out_imei[8 + i] = (char)('0' + (digest[i] % 10));

	/* 3. 15th digit: Luhn check digit */
	out_imei[14] = ghost_test_compute_luhn(out_imei);
	out_imei[15] = '\0';

	memzero_explicit(digest, sizeof(digest));
	memzero_explicit(input_buf, sizeof(input_buf));
}

static void ghost_derive_single_meid(const char *label, size_t label_len,
				     const u8 *seed, size_t seed_len,
				     char out_meid[15])
{
	u8 digest[SHA256_DIGEST_SIZE];
	u8 input_buf[128];
	size_t copy_seed;

	if (label_len > sizeof(input_buf))
		label_len = sizeof(input_buf);
	memcpy(input_buf, label, label_len);

	copy_seed = seed_len;
	if (copy_seed > (sizeof(input_buf) - label_len))
		copy_seed = sizeof(input_buf) - label_len;
	memcpy(input_buf + label_len, seed, copy_seed);

	sha256(input_buf, label_len + copy_seed, digest);

	/* 14 uppercase hex characters */
	snprintf(out_meid, 15, "%02X%02X%02X%02X%02X%02X%02X",
		 digest[0], digest[1], digest[2], digest[3],
		 digest[4], digest[5], digest[6]);

	memzero_explicit(digest, sizeof(digest));
	memzero_explicit(input_buf, sizeof(input_buf));
}

int ghost_test_identity_generate(const u8 *seed, size_t seed_len,
				 struct ghost_test_identity *out)
{
	static const char label_imei1[] = "TEST_GHOST_TEL_V1_IMEI1";
	static const char label_imei2[] = "TEST_GHOST_TEL_V1_IMEI2";
	static const char label_meid[]  = "TEST_GHOST_TEL_V1_MEID";

	if (!seed || seed_len == 0 || !out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));

	ghost_derive_single_imei(label_imei1, sizeof(label_imei1) - 1,
				 seed, seed_len, out->imei1);
	ghost_derive_single_imei(label_imei2, sizeof(label_imei2) - 1,
				 seed, seed_len, out->imei2);
	ghost_derive_single_meid(label_meid, sizeof(label_meid) - 1,
				 seed, seed_len, out->meid);

	return 0;
}
EXPORT_SYMBOL_GPL(ghost_test_identity_generate);

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static struct dentry *ghost_debugfs_root;
static struct ghost_test_identity active_debugfs_id;
static bool active_debugfs_id_valid = false;

static int ghost_test_debugfs_show(struct seq_file *m, void *v)
{
	const struct ghost_mock_transport_stats *st = ghost_mock_transport_get_stats();

	seq_puts(m, "=== Ghost Telephony Identity Test Harness ===\n");
	seq_puts(m, "status: test_mock_active\n");

	if (active_debugfs_id_valid) {
		seq_printf(m, "imei1_masked: ***********%s\n", active_debugfs_id.imei1 + 11);
		seq_printf(m, "imei2_masked: ***********%s\n", active_debugfs_id.imei2 + 11);
		seq_printf(m, "meid_masked:  **********%s\n", active_debugfs_id.meid + 10);
	} else {
		seq_puts(m, "identity: not_configured\n");
	}

	if (st) {
		seq_printf(m, "stats_req_imei_ok: %lu\n", st->req_imei_ok);
		seq_printf(m, "stats_req_meid_ok: %lu\n", st->req_meid_ok);
		seq_printf(m, "stats_err_invalid_len: %lu\n", st->err_invalid_len);
		seq_printf(m, "stats_err_bad_opcode: %lu\n", st->err_bad_opcode);
		seq_printf(m, "stats_err_bad_slot: %lu\n", st->err_bad_slot);
	}

	return 0;
}

static int ghost_test_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, ghost_test_debugfs_show, NULL);
}

static const struct file_operations ghost_test_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = ghost_test_debugfs_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int __init ghost_test_identity_init(void)
{
	static const u8 default_test_seed[] = { 0x55, 0xAA, 0x12, 0x34, 0x56, 0x78 };

	ghost_test_identity_generate(default_test_seed, sizeof(default_test_seed),
				     &active_debugfs_id);
	active_debugfs_id_valid = true;

	ghost_debugfs_root = debugfs_create_dir("ghost_telephony_test", NULL);
	if (ghost_debugfs_root) {
		debugfs_create_file("status", 0440, ghost_debugfs_root, NULL,
				    &ghost_test_debugfs_fops);
	}

	pr_info("ghost_test_identity: initialized mock test harness (debugfs ready)\n");
	return 0;
}

static void __exit ghost_test_identity_exit(void)
{
	debugfs_remove_recursive(ghost_debugfs_root);
	ghost_debugfs_root = NULL;
	active_debugfs_id_valid = false;
	memzero_explicit(&active_debugfs_id, sizeof(active_debugfs_id));
	pr_info("ghost_test_identity: cleaned up mock test harness\n");
}

module_init(ghost_test_identity_init);
module_exit(ghost_test_identity_exit);

MODULE_DESCRIPTION("Mock Telephony Identity Generator for Unit Testing");
MODULE_AUTHOR("Antigravity Engineering");
MODULE_LICENSE("GPL v2");
