/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GHOST_TEST_IDENTITY_H
#define _LINUX_GHOST_TEST_IDENTITY_H

#include <linux/types.h>

/**
 * struct ghost_test_identity - Pure mock/test telephony identity container
 * @imei1: 15-digit test IMEI with valid Luhn check digit + NUL byte
 * @imei2: 15-digit test IMEI with valid Luhn check digit + NUL byte
 * @meid:  14-character uppercase hex test MEID + NUL byte
 */
struct ghost_test_identity {
	char imei1[16];
	char imei2[16];
	char meid[15];
};

/**
 * ghost_test_compute_luhn() - Calculate Luhn check digit for 14-digit prefix
 * @digits: 14 ASCII decimal digits
 *
 * Return: ASCII char ('0'-'9') representing the 15th Luhn check digit
 */
char ghost_test_compute_luhn(const char digits[14]);

/**
 * ghost_test_imei_luhn_valid() - Validate 15-digit IMEI with Luhn algorithm
 * @imei: 15-digit ASCII string (+ optional NUL)
 *
 * Return: true if valid 15-digit decimal string with correct Luhn sum, false otherwise
 */
bool ghost_test_imei_luhn_valid(const char imei[16]);

/**
 * ghost_test_identity_generate() - Pure deterministic identity generator for test harness
 * @seed: Input test seed bytes
 * @seed_len: Length of test seed (must be > 0)
 * @out: Output structure to receive test identities
 *
 * Return: 0 on success, -EINVAL on invalid arguments or empty seed
 */
int ghost_test_identity_generate(const u8 *seed, size_t seed_len,
				 struct ghost_test_identity *out);

/* Mock telephony transport protocol opcodes */
#define GHOST_MOCK_OP_GET_IMEI   0x0101
#define GHOST_MOCK_OP_GET_MEID   0x0102

struct ghost_mock_telephony_hdr {
	__u16 opcode;
	__u8  slot;
	__u8  status;
	__u16 payload_len;
} __packed;

struct ghost_mock_telephony_packet {
	struct ghost_mock_telephony_hdr hdr;
	__u8 payload[64];
} __packed;

struct ghost_mock_transport_stats {
	unsigned long req_imei_ok;
	unsigned long req_meid_ok;
	unsigned long err_invalid_len;
	unsigned long err_bad_opcode;
	unsigned long err_bad_slot;
	unsigned long err_buffer_overflow;
};

int ghost_mock_transport_process(const u8 *in_frame, size_t in_len,
				 u8 *out_frame, size_t out_max, size_t *out_len,
				 const struct ghost_test_identity *id);
const struct ghost_mock_transport_stats *ghost_mock_transport_get_stats(void);
void ghost_mock_transport_reset_stats(void);

#endif /* _LINUX_GHOST_TEST_IDENTITY_H */
