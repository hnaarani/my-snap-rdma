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

#ifndef _SNAP_DPA_NVME_MP_COMMON_H
#define _SNAP_DPA_NVME_MP_COMMON_H

#include "snap_dpa_common.h"

#define SNAP_DPA_NVME_MP_APP "dpa_nvme_mp"
#define SNAP_DPA_NVME_MP_SQE_SHADOW_ALIGN 64
#define SNAP_DPA_NVME_MP_MAX_NUM_QPS 8

enum {
	DPA_NVME_MP_CQ_CREATE = SNAP_DPA_CMD_APP_FIRST,
	DPA_NVME_MP_CQ_DESTROY,
	DPA_NVME_MP_CQ_MODIFY,
	DPA_NVME_MP_CQ_QUERY,
	DPA_NVME_MP_SQ_CREATE,
	DPA_NVME_MP_SQ_DESTROY,
	DPA_NVME_MP_SQ_MODIFY,
	DPA_NVME_MP_SQ_QUERY,
};

enum dpa_nvme_mp_state {
	DPA_NVME_MP_STATE_INIT = 0,
	DPA_NVME_MP_STATE_RDY,
	DPA_NVME_MP_STATE_ERR,
	DPA_NVME_MP_STATE_SUSPEND,
};

TAILQ_HEAD(dpa_nvme_mp_sq_list, dpa_nvme_mp_sq);

struct dpa_nvme_mp_cq {
	struct dpa_nvme_mp_sq_list sqs;
	enum dpa_nvme_mp_state state;
	struct snap_hw_cq cq_head_db_hw_cq;
	struct snap_dpa_p2p_q p2p_queues[SNAP_DPA_NVME_MP_MAX_NUM_QPS];
	uint32_t num_p2p_queues;
	uint32_t cq_head_duar_id;
	uint32_t host_cq_head;
	uint32_t msix_cqnum;
};

struct dpa_nvme_mp_sq {
	uint32_t sqid;
	uint32_t dpu_sqe_shadow_mkey;
	uint64_t dpu_sqe_shadow_addr;
	uint64_t host_sq_addr;
	uint16_t queue_depth;
	uint32_t duar_id;
	uint32_t dpu_mkey;
	enum dpa_nvme_mp_state state;

	uint32_t arm_sq_tail;
	uint32_t last_read_sq_tail;
	struct nvme_cmd *sqe_buffer;
	struct snap_dma_completion host2dpa_comp;

	TAILQ_ENTRY(dpa_nvme_mp_sq) entry;
};

struct dpa_sq_modify_mask {
	uint8_t state:1;
	uint8_t host_sq_tail:1;
};

struct dpa_cq_modify_mask {
	uint8_t state:1;
};

struct __attribute__((packed)) dpa_nvme_mp_cmd_cq_create {
	struct dpa_nvme_mp_cq cq;
};

struct dpa_nvme_mp_cmd_cq_modify {
	enum dpa_nvme_mp_state state;
	struct dpa_cq_modify_mask mask;
};

struct __attribute__((packed)) dpa_nvme_mp_cmd_sq_create {
	struct dpa_nvme_mp_sq sq;
};

struct dpa_nvme_mp_cmd_sq_modify {
	uint32_t sqid;
	enum dpa_nvme_mp_state state;
	uint16_t host_sq_tail;
	struct dpa_sq_modify_mask mask;
};

struct dpa_nvme_mp_cmd_sq_query {
	uint32_t sqid;
};

struct dpa_nvme_mp_rsp_query {
	enum dpa_nvme_mp_state state;
	uint32_t db_value;
};

struct dpa_nvme_mp_cmd {
	struct snap_dpa_cmd base;
	union {
		struct dpa_nvme_mp_cmd_cq_create cmd_cq_create;
		struct dpa_nvme_mp_cmd_sq_create cmd_sq_create;
		struct dpa_nvme_mp_cmd_cq_modify cmd_cq_modify;
		struct dpa_nvme_mp_cmd_sq_modify cmd_sq_modify;
		struct dpa_nvme_mp_cmd_sq_query cmd_sq_query;
	};
};
struct dpa_nvme_mp_rsp {
	struct snap_dpa_rsp base;
	struct dpa_nvme_mp_rsp_query state;
};

#endif // _SNAP_DPA_NVME_MP_COMMON_H
