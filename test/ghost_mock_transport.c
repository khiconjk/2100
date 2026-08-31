// SPDX-License-Identifier: GPL-2.0
/*
 * Mock Telephony Transport Adapter for Unit Testing.
 * Strictly operates on structured mock protocol frames.
 * Does not touch production modem hardware or generic VFS buffers.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/ghost_test_identity.h>

static struct ghost_mock_transport_stats mock_stats;

const struct ghost_mock_transport_stats *ghost_mock_transport_get_stats(void)
{
	return &mock_stats;
}
EXPORT_SYMBOL_GPL(ghost_mock_transport_get_stats);

void ghost_mock_transport_reset_stats(void)
{
	memset(&mock_stats, 0, sizeof(mock_stats));
}
EXPORT_SYMBOL_GPL(ghost_mock_transport_reset_stats);

int ghost_mock_transport_process(const u8 *in_frame, size_t in_len,
				 u8 *out_frame, size_t out_max, size_t *out_len,
				 const struct ghost_test_identity *id)
{
	const struct ghost_mock_telephony_hdr *in_hdr;
	struct ghost_mock_telephony_hdr *out_hdr;
	size_t req_total_len;
	const char *src_identity = NULL;
	size_t id_len = 0;

	if (!in_frame || !out_frame || !out_len || !id) {
		mock_stats.err_invalid_len++;
		return -EINVAL;
	}

	/* 1. Header boundary check */
	if (in_len < sizeof(struct ghost_mock_telephony_hdr)) {
		mock_stats.err_invalid_len++;
		return -EINVAL;
	}

	in_hdr = (const struct ghost_mock_telephony_hdr *)in_frame;
	req_total_len = sizeof(struct ghost_mock_telephony_hdr) + in_hdr->payload_len;

	/* Verify frame size matches payload_len in header */
	if (in_len < req_total_len || req_total_len > sizeof(struct ghost_mock_telephony_packet)) {
		mock_stats.err_invalid_len++;
		return -EINVAL;
	}

	/* 2. Opcode and Slot validation */
	switch (in_hdr->opcode) {
	case GHOST_MOCK_OP_GET_IMEI:
		if (in_hdr->slot == 0) {
			src_identity = id->imei1;
			id_len = 15;
		} else if (in_hdr->slot == 1) {
			src_identity = id->imei2;
			id_len = 15;
		} else {
			mock_stats.err_bad_slot++;
			return -EOPNOTSUPP;
		}
		break;

	case GHOST_MOCK_OP_GET_MEID:
		src_identity = id->meid;
		id_len = 14;
		break;

	default:
		mock_stats.err_bad_opcode++;
		return -EOPNOTSUPP;
	}

	/* 3. Output buffer overflow protection */
	if (out_max < sizeof(struct ghost_mock_telephony_hdr) + id_len) {
		mock_stats.err_buffer_overflow++;
		return -ENOBUFS;
	}

	/* 4. Construct Response Packet */
	out_hdr = (struct ghost_mock_telephony_hdr *)out_frame;
	out_hdr->opcode = in_hdr->opcode;
	out_hdr->slot = in_hdr->slot;
	out_hdr->status = 0; /* SUCCESS */
	out_hdr->payload_len = id_len;

	memcpy(out_frame + sizeof(struct ghost_mock_telephony_hdr), src_identity, id_len);
	*out_len = sizeof(struct ghost_mock_telephony_hdr) + id_len;

	if (in_hdr->opcode == GHOST_MOCK_OP_GET_IMEI)
		mock_stats.req_imei_ok++;
	else
		mock_stats.req_meid_ok++;

	return 0;
}
EXPORT_SYMBOL_GPL(ghost_mock_transport_process);

MODULE_DESCRIPTION("Mock Telephony Transport Adapter for Unit Testing");
MODULE_AUTHOR("Antigravity Engineering");
MODULE_LICENSE("GPL v2");
