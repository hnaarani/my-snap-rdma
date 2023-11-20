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

/**
 * @brief Transfer data using DMA from one cyclic buffer to another, handling wraparounds.
 *
 * This function copies data from one cyclic buffer to another using DMA, taking into account
 * potential wraparounds in both cyclic buffers.
 *
 * @return The number of DMA operations initiated.
 *
 * @note The 'comp->count' field is not updated by this function.
 */
static inline int
snap_dpa_dma_cyclic_buffer_write(struct snap_dma_q *q, void *src_buffer,
		uint32_t src_mkey, uint64_t dst_buffer, uint32_t dst_mkey,
		uint32_t src_idx, uint32_t dst_idx, uint16_t num_elements,
		uint32_t element_size, uint32_t q_depth, struct snap_dma_completion *comp)
{
	uint32_t src_wa = q_depth - src_idx, dst_wa = q_depth - dst_idx;
	uint32_t first_wa = MIN(src_wa, dst_wa);
	uint32_t second_wa = src_wa + dst_wa - (2 * first_wa);
	int dma_count = 1;

	if (snap_unlikely(num_elements > first_wa)) {
		snap_dma_q_write(q, src_buffer + (src_idx * element_size), first_wa * element_size,
				src_mkey, dst_buffer + (dst_idx * element_size), dst_mkey, comp);
		num_elements -= first_wa;
		src_idx = (src_idx + first_wa) % q_depth;
		dst_idx = (dst_idx + first_wa) % q_depth;
		dma_count++;
	}

	if (snap_unlikely(second_wa && num_elements > second_wa)) {
		snap_dma_q_write(q, src_buffer + (src_idx * element_size), second_wa * element_size,
				src_mkey, dst_buffer + (dst_idx * element_size), dst_mkey, comp);
		num_elements -= second_wa;
		src_idx = (src_idx + second_wa) % q_depth;
		dst_idx = (dst_idx + second_wa) % q_depth;
		dma_count++;
	}

	snap_dma_q_write(q, src_buffer + (src_idx * element_size), num_elements * element_size,
			src_mkey, dst_buffer + (dst_idx * element_size), dst_mkey, comp);

	return dma_count;
}
