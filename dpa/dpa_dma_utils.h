/*
 * Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include "snap_dma.h"

static inline int
snap_dpa_dma_rb_write(struct snap_dma_q *q, void *src_buffer,
		uint32_t src_mkey, uint64_t dst_buffer, uint32_t dst_mkey,
		struct snap_dma_completion *comp, uint16_t new_elem_idx,
		uint32_t old_elem_idx, const uint32_t element_size, uint32_t q_depth)
{
	uint32_t start_offset = old_elem_idx * element_size;
	uint32_t elements_reg, elements_wrapped;
	int rc;

	if (snap_unlikely(new_elem_idx < old_elem_idx)) {
		elements_reg = q_depth - old_elem_idx;
		elements_wrapped = new_elem_idx;
	} else {
		elements_reg = new_elem_idx - old_elem_idx;
		elements_wrapped = 0;
	}

	comp->count++;
	rc = snap_dma_q_write(q, src_buffer + start_offset,
			element_size * elements_reg, src_mkey,
			dst_buffer + start_offset, dst_mkey, comp);
	if (snap_unlikely(rc)) {
		snap_debug("Ring buffer dma write failed with err %d", rc);
		return rc;
	}

	if (snap_unlikely(elements_wrapped)) {
		comp->count++;
		rc = snap_dma_q_write(q, src_buffer,
				element_size * elements_wrapped,
				src_mkey, dst_buffer, dst_mkey, comp);
		if (snap_unlikely(rc)) {
			snap_debug("Ring buffer dma write failed with err %d", rc);
			return rc;
		}
	}

	return rc;
}
