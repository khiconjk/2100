// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit Test Suite for Pure Mock Telephony Identity Generator.
 */

#include <kunit/test.h>
#include <linux/ctype.h>
#include <linux/ghost_test_identity.h>

static const u8 SEED_A[] = { 0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD };
static const u8 SEED_B[] = { 0xFE, 0xDC, 0xBA, 0x98, 0x11, 0x22, 0x33, 0x44 };

static void test_ghost_identity_determinism(struct test *test)
{
	struct ghost_test_identity id1, id2;
	int ret;

	ret = ghost_test_identity_generate(SEED_A, sizeof(SEED_A), &id1);
	EXPECT_EQ(test, 0, ret);

	ret = ghost_test_identity_generate(SEED_A, sizeof(SEED_A), &id2);
	EXPECT_EQ(test, 0, ret);

	EXPECT_STREQ(test, id1.imei1, id2.imei1);
	EXPECT_STREQ(test, id1.imei2, id2.imei2);
	EXPECT_STREQ(test, id1.meid, id2.meid);
}

static void test_ghost_identity_distinct_seeds(struct test *test)
{
	struct ghost_test_identity idA, idB;
	int ret;

	ret = ghost_test_identity_generate(SEED_A, sizeof(SEED_A), &idA);
	EXPECT_EQ(test, 0, ret);

	ret = ghost_test_identity_generate(SEED_B, sizeof(SEED_B), &idB);
	EXPECT_EQ(test, 0, ret);

	EXPECT_STRNEQ(test, idA.imei1, idB.imei1);
	EXPECT_STRNEQ(test, idA.imei2, idB.imei2);
	EXPECT_STRNEQ(test, idA.meid, idB.meid);
}

static void test_ghost_identity_slot_separation(struct test *test)
{
	struct ghost_test_identity id;
	int ret;

	ret = ghost_test_identity_generate(SEED_A, sizeof(SEED_A), &id);
	EXPECT_EQ(test, 0, ret);

	/* IMEI1 and IMEI2 must differ even with the same seed */
	EXPECT_STRNEQ(test, id.imei1, id.imei2);
}

static void test_ghost_identity_format_and_luhn(struct test *test)
{
	struct ghost_test_identity id;
	int ret, i;

	ret = ghost_test_identity_generate(SEED_A, sizeof(SEED_A), &id);
	EXPECT_EQ(test, 0, ret);

	/* 1. Validate IMEI1: length 15, all digits, valid Luhn */
	EXPECT_EQ(test, 15, (int)strlen(id.imei1));
	for (i = 0; i < 15; i++)
		EXPECT_TRUE(test, isdigit(id.imei1[i]));
	EXPECT_TRUE(test, ghost_test_imei_luhn_valid(id.imei1));

	/* 2. Validate IMEI2: length 15, all digits, valid Luhn */
	EXPECT_EQ(test, 15, (int)strlen(id.imei2));
	for (i = 0; i < 15; i++)
		EXPECT_TRUE(test, isdigit(id.imei2[i]));
	EXPECT_TRUE(test, ghost_test_imei_luhn_valid(id.imei2));

	/* 3. Validate MEID: length 14, uppercase hex */
	EXPECT_EQ(test, 14, (int)strlen(id.meid));
	for (i = 0; i < 14; i++)
		EXPECT_TRUE(test, isxdigit(id.meid[i]) && !islower(id.meid[i]));
}

static void test_ghost_identity_invalid_inputs(struct test *test)
{
	struct ghost_test_identity id;

	/* NULL seed */
	EXPECT_EQ(test, -EINVAL, ghost_test_identity_generate(NULL, 8, &id));

	/* Zero seed_len */
	EXPECT_EQ(test, -EINVAL, ghost_test_identity_generate(SEED_A, 0, &id));

	/* NULL out */
	EXPECT_EQ(test, -EINVAL, ghost_test_identity_generate(SEED_A, sizeof(SEED_A), NULL));

	/* Invalid IMEI strings for Luhn validator */
	EXPECT_FALSE(test, ghost_test_imei_luhn_valid(NULL));
	EXPECT_FALSE(test, ghost_test_imei_luhn_valid("1234"));
	EXPECT_FALSE(test, ghost_test_imei_luhn_valid("00100100123456789")); /* 17 chars */
	EXPECT_FALSE(test, ghost_test_imei_luhn_valid("00100100ABCD567"));   /* Non-digits */
}

static void test_ghost_mock_transport_imei_and_meid(struct test *test)
{
	struct ghost_test_identity id;
	struct ghost_mock_telephony_packet req, resp;
	size_t resp_len = 0;
	int ret;

	ghost_mock_transport_reset_stats();
	ret = ghost_test_identity_generate(SEED_A, sizeof(SEED_A), &id);
	EXPECT_EQ(test, 0, ret);

	/* 1. Request IMEI Slot 0 */
	memset(&req, 0, sizeof(req));
	req.hdr.opcode = GHOST_MOCK_OP_GET_IMEI;
	req.hdr.slot = 0;
	req.hdr.payload_len = 0;

	ret = ghost_mock_transport_process((u8 *)&req, sizeof(req.hdr),
					   (u8 *)&resp, sizeof(resp), &resp_len, &id);
	EXPECT_EQ(test, 0, ret);
	EXPECT_EQ(test, sizeof(struct ghost_mock_telephony_hdr) + 15, resp_len);
	EXPECT_EQ(test, 0, memcmp(resp.payload, id.imei1, 15));

	/* 2. Request IMEI Slot 1 */
	req.hdr.slot = 1;
	ret = ghost_mock_transport_process((u8 *)&req, sizeof(req.hdr),
					   (u8 *)&resp, sizeof(resp), &resp_len, &id);
	EXPECT_EQ(test, 0, ret);
	EXPECT_EQ(test, sizeof(struct ghost_mock_telephony_hdr) + 15, resp_len);
	EXPECT_EQ(test, 0, memcmp(resp.payload, id.imei2, 15));

	/* 3. Request MEID */
	req.hdr.opcode = GHOST_MOCK_OP_GET_MEID;
	req.hdr.slot = 0;
	ret = ghost_mock_transport_process((u8 *)&req, sizeof(req.hdr),
					   (u8 *)&resp, sizeof(resp), &resp_len, &id);
	EXPECT_EQ(test, 0, ret);
	EXPECT_EQ(test, sizeof(struct ghost_mock_telephony_hdr) + 14, resp_len);
	EXPECT_EQ(test, 0, memcmp(resp.payload, id.meid, 14));

	EXPECT_EQ(test, 2, (int)ghost_mock_transport_get_stats()->req_imei_ok);
	EXPECT_EQ(test, 1, (int)ghost_mock_transport_get_stats()->req_meid_ok);
}

static void test_ghost_mock_transport_boundary_rejection(struct test *test)
{
	struct ghost_test_identity id;
	struct ghost_mock_telephony_packet req, resp;
	size_t resp_len = 0;
	int ret;

	ghost_mock_transport_reset_stats();
	ret = ghost_test_identity_generate(SEED_A, sizeof(SEED_A), &id);
	EXPECT_EQ(test, 0, ret);

	/* 1. Short frame (less than header) */
	ret = ghost_mock_transport_process((u8 *)&req, 2,
					   (u8 *)&resp, sizeof(resp), &resp_len, &id);
	EXPECT_EQ(test, -EINVAL, ret);

	/* 2. Invalid opcode */
	req.hdr.opcode = 0x9999;
	req.hdr.slot = 0;
	req.hdr.payload_len = 0;
	ret = ghost_mock_transport_process((u8 *)&req, sizeof(req.hdr),
					   (u8 *)&resp, sizeof(resp), &resp_len, &id);
	EXPECT_EQ(test, -EOPNOTSUPP, ret);

	/* 3. Invalid slot (slot 2) */
	req.hdr.opcode = GHOST_MOCK_OP_GET_IMEI;
	req.hdr.slot = 2;
	ret = ghost_mock_transport_process((u8 *)&req, sizeof(req.hdr),
					   (u8 *)&resp, sizeof(resp), &resp_len, &id);
	EXPECT_EQ(test, -EOPNOTSUPP, ret);

	/* 4. Output buffer too small */
	req.hdr.slot = 0;
	ret = ghost_mock_transport_process((u8 *)&req, sizeof(req.hdr),
					   (u8 *)&resp, 5, &resp_len, &id);
	EXPECT_EQ(test, -ENOBUFS, ret);

	EXPECT_TRUE(test, ghost_mock_transport_get_stats()->err_invalid_len > 0);
	EXPECT_TRUE(test, ghost_mock_transport_get_stats()->err_bad_opcode > 0);
	EXPECT_TRUE(test, ghost_mock_transport_get_stats()->err_bad_slot > 0);
	EXPECT_TRUE(test, ghost_mock_transport_get_stats()->err_buffer_overflow > 0);
}

static struct test_case ghost_test_identity_test_cases[] = {
	TEST_CASE(test_ghost_identity_determinism),
	TEST_CASE(test_ghost_identity_distinct_seeds),
	TEST_CASE(test_ghost_identity_slot_separation),
	TEST_CASE(test_ghost_identity_format_and_luhn),
	TEST_CASE(test_ghost_identity_invalid_inputs),
	TEST_CASE(test_ghost_mock_transport_imei_and_meid),
	TEST_CASE(test_ghost_mock_transport_boundary_rejection),
	{},
};

static struct test_module ghost_test_identity_test_module = {
	.name = "ghost_test_identity",
	.test_cases = ghost_test_identity_test_cases,
};
module_test(ghost_test_identity_test_module);
