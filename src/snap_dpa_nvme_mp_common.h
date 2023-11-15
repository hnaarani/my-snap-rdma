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
#define SNAP_DPA_NVME_MP_MAX_NSID 32
#define NVME_MP_PACKED __attribute__((packed))

struct NVME_MP_PACKED nvme_cmd {
	uint8_t opc:8;
	uint8_t fuse:2;
	uint8_t:4;
	uint8_t psdt:2;
	uint16_t cid;
	uint32_t nsid;
	uint8_t rsv[56];
};

enum {
	DPA_NVME_MP_CQ_CREATE = SNAP_DPA_CMD_APP_FIRST,
	DPA_NVME_MP_CQ_DESTROY,
	DPA_NVME_MP_CQ_MODIFY,
	DPA_NVME_MP_CQ_QUERY,
	DPA_NVME_MP_SQ_CREATE,
	DPA_NVME_MP_SQ_DESTROY,
	DPA_NVME_MP_SQ_MODIFY,
	DPA_NVME_MP_SQ_QUERY,
	DPA_NVME_MP_RB_ATTACH,
	DPA_NVME_MP_RB_DETACH,
};

enum dpa_nvme_mp_state {
	DPA_NVME_MP_STATE_INIT = 0,
	DPA_NVME_MP_STATE_RDY,
	DPA_NVME_MP_STATE_ERR,
	DPA_NVME_MP_STATE_SUSPEND,
};

TAILQ_HEAD(dpa_nvme_mp_sq_list, dpa_nvme_mp_sq);

struct NVME_MP_PACKED dpa_nvme_mp_completion {
	uint32_t result;
	uint16_t cid;
	uint16_t status;
};

struct dpa_nvme_mp_cq {
	struct dpa_nvme_mp_sq_list sqs;
	enum dpa_nvme_mp_state state;
	struct snap_hw_cq cq_head_db_hw_cq;
	struct snap_dpa_p2p_q p2p_queues[SNAP_DPA_NVME_MP_MAX_NUM_QPS];
	uint32_t num_p2p_queues;
	uint32_t cq_head_duar_id;
	uint32_t msix_cqnum;
	uint16_t host_cq_head;
	uint16_t cq_tail;

	struct snap_dma_completion comp;
	struct nvme_cqe *shadow_cq;
	uint64_t host_cq_addr;
	uint32_t host_mkey;
	uint16_t queue_depth;
	bool msix_required;
	bool phase;
};

struct dpa_nvme_mp_rb {
	uint64_t arm_rb_addr;
	uint32_t arm_rb_mkey;
	uint16_t tail;
	uint8_t weight;
};

struct dpa_nvme_mp_ns {
	uint8_t active_rb;
	uint8_t credits;
	struct dpa_nvme_mp_rb rbs[0];
};

struct dpa_nvme_mp_sq {
	struct dpa_nvme_mp_ns *namespaces[SNAP_DPA_NVME_MP_MAX_NSID + 1];
	uint32_t sqid;
	uint64_t host_sq_addr;
	uint16_t queue_depth;
	uint32_t duar_id;
	uint32_t host_mkey;
	enum dpa_nvme_mp_state state;

	uint32_t sq_head;
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

struct dpa_nvme_mp_cmd_rb_attach {
	uint64_t arm_rb_addr;
	uint32_t arm_rb_mkey;
	uint32_t nsid;
	uint8_t qp_id;
	uint8_t weight;
};

struct dpa_nvme_mp_cmd_rb_detach {
	uint32_t nsid;
	uint8_t qp_id;
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
		struct dpa_nvme_mp_cmd_rb_attach cmd_rb_attach;
		struct dpa_nvme_mp_cmd_rb_detach cmd_rb_detach;
	};
};

struct dpa_nvme_mp_rsp {
	struct snap_dpa_rsp base;
	struct dpa_nvme_mp_rsp_query state;
};

#endif // _SNAP_DPA_NVME_MP_COMMON_H
