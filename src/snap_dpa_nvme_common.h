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

#ifndef _SNAP_DPA_NVME_COMMON_H
#define _SNAP_DPA_NVME_COMMON_H

#include "snap_dpa_common.h"

#define SNAP_DPA_NVME_SQE_SHADOW_ALIGN 64

enum {
	DPA_NVME_CQ_CREATE = SNAP_DPA_CMD_APP_FIRST,
	DPA_NVME_CQ_DESTROY,
	DPA_NVME_CQ_MODIFY,
	DPA_NVME_CQ_QUERY,
	DPA_NVME_SQ_CREATE,
	DPA_NVME_SQ_DESTROY,
	DPA_NVME_SQ_MODIFY,
	DPA_NVME_SQ_QUERY,
};

enum dpa_nvme_state {
	DPA_NVME_STATE_INIT = 0,
	DPA_NVME_STATE_RDY,
	DPA_NVME_STATE_ERR,
	DPA_NVME_STATE_SUSPEND,
};

TAILQ_HEAD(dpa_nvme_sq_list, dpa_nvme_sq);

struct dpa_nvme_cq {
	struct dpa_nvme_sq_list sqs;
	enum dpa_nvme_state state;
	struct snap_hw_cq cq_head_db_hw_cq;
	uint32_t cq_head_duar_id;
	uint32_t host_cq_head;
	uint32_t msix_cqnum;
};

struct dpa_nvme_sq {
	uint32_t sq_tail;
	uint32_t sqid;
	uint32_t dpu_sqe_shadow_mkey;
	uint64_t dpu_sqe_shadow_addr;
	uint64_t base_addr;
	uint16_t queue_depth;
	uint32_t duar_id;
	uint32_t dpu_mkey;
	uint32_t host_sq_tail;
	enum dpa_nvme_state state;

	TAILQ_ENTRY(dpa_nvme_sq) entry;

};

struct dpa_sq_modify_mask {
	uint8_t state:1;
	uint8_t host_sq_tail:1;
};

struct dpa_cq_modify_mask {
	uint8_t state:1;
};

struct __attribute__((packed)) dpa_nvme_cmd_cq_create {
	struct dpa_nvme_cq cq;
};

struct dpa_nvme_cmd_cq_modify {
	enum dpa_nvme_state state;
	struct dpa_cq_modify_mask mask;
};

struct __attribute__((packed)) dpa_nvme_cmd_sq_create {
	struct dpa_nvme_sq sq;
};

struct dpa_nvme_cmd_sq_modify {
	uint32_t sqid;
	enum dpa_nvme_state state;
	uint16_t host_sq_tail;
	struct dpa_sq_modify_mask mask;
};

struct dpa_nvme_cmd_sq_query {
	uint32_t sqid;
};

struct dpa_nvme_rsp_query {
	enum dpa_nvme_state state;
	uint32_t db_value;
};

struct dpa_nvme_cmd {
	struct snap_dpa_cmd base;
	union {
		struct dpa_nvme_cmd_cq_create cmd_cq_create;
		struct dpa_nvme_cmd_sq_create cmd_sq_create;
		struct dpa_nvme_cmd_cq_modify cmd_cq_modify;
		struct dpa_nvme_cmd_sq_modify cmd_sq_modify;
		struct dpa_nvme_cmd_sq_query cmd_sq_query;
	};
};

struct dpa_nvme_rsp {
	struct snap_dpa_rsp base;
	struct dpa_nvme_rsp_query state;
};

#define SNAP_DPA_NVME_APP "dpa_nvme"

#endif
