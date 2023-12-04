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

#include <stddef.h>
#include <string.h>

#include "dpa.h"
#include "snap_macros.h"
#include "snap_dma.h"
#include "snap_dma_internal.h"
#include "snap_dpa_rt.h"
#include "snap_dpa_nvme_mp_common.h"
#include "dpa_dma_utils.h"

#define COMMAND_DELAY 100000
#define NVME_MP_MAX_COMPS_POLL 64

/* In many cases, using DPA compiler's copy implementation
 * gives poor performance in comparison to this copy */
#define U64_ALIGNED_COPY(dst, src, size) do { \
    uint64_t *dst_ptr = (uint64_t*)(dst); \
    uint64_t *src_ptr = (uint64_t*)(src); \
    size_t size_u64 = size / 8; \
    for (size_t i = 0; i < size_u64; i++) { \
        dst_ptr[i] = src_ptr[i]; \
    } \
} while (0)

enum NVME_MP_PACKED nvme_status_code {
	NVME_SC_CMD_ABORT_MISSING_FUSE = 0x000a,
	NVME_SC_INVALID_NSID = 0x000b,
};

struct NVME_MP_PACKED nvme_cqe {
	uint32_t	result;
	uint32_t	rsvd;
	uint16_t	sq_head;
	uint16_t	sq_id;
	uint16_t	cid;
	uint16_t	status;
};

/**
 * Single nvme queue per thread implementation. The thread can be
 * either in polling or in event mode
 */
static inline struct dpa_nvme_mp_cq *get_nvme_cq()
{
	/* will redo to be mt safe */
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();

	/* cq is always allocated after rt context */
	return (struct dpa_nvme_mp_cq *)SNAP_ALIGN_CEIL((uint64_t)(rt_ctx + 1), DPA_CACHE_LINE_BYTES);
}

static void dpa_write_rsp(enum dpa_nvme_mp_state state, uint32_t db_value)
{
	struct dpa_nvme_mp_rsp *rsp;

	rsp = (struct dpa_nvme_mp_rsp *)snap_dpa_mbox_to_rsp(dpa_mbox());

	rsp->state.state = state;
	rsp->state.db_value = db_value;
}

static inline void
nvme_mp_completion_msg_handle(struct dpa_nvme_mp_cq *cq, struct dpa_nvme_mp_sq *sq, void *data)
{
	/* Copying the mp completion message (heap) to stack, constructing the
	 * NVMe CQE on stack and then copying it to the shadow cq (heap) proved to
	 * be 30% faster (on NVMe local single queue) than direct heap->heap copies.
	 */
	struct dpa_nvme_mp_completion mp_comp;

	U64_ALIGNED_COPY(&mp_comp, data, sizeof(struct dpa_nvme_mp_completion));
	/* Assumes that our CQ has 1 SQ */
	sq->sq_head = (sq->sq_head + 1) % sq->queue_depth;

	struct nvme_cqe cqe  = {
		.result = mp_comp.result,
		.sq_head = sq->sq_head,
		.sq_id = sq->sqid,
		.cid = mp_comp.cid,
		.status = mp_comp.status | (cq->phase & 1),
	};

	U64_ALIGNED_COPY(&cq->shadow_cq[cq->cq_tail], &cqe, sizeof(struct nvme_cqe));

	cq->cq_tail = (cq->cq_tail + 1) % cq->queue_depth;
	cq->phase = cq->phase ^ (!cq->cq_tail);
}

static inline void
dpa_nvme_mp_error_cqe_send(struct dpa_nvme_mp_cq *cq, struct dpa_nvme_mp_sq *sq,
		uint32_t sqe_idx, enum nvme_status_code sc)
{
	struct dpa_nvme_mp_completion mp_comp = {
		.cid = sq->sqe_buffer[sqe_idx].cid,
		.status = sc,
		.result = 0,
	};

	nvme_mp_completion_msg_handle(cq, sq, &mp_comp);
}

static inline void
dpa_nvme_mp_ns_next_rb_get(struct dpa_nvme_mp_ns *ns, uint32_t num_p2p_queues)
{
	do
		ns->active_rb = (ns->active_rb + 1) % num_p2p_queues;
	while (!ns->rbs[ns->active_rb].weight);

	ns->credits = ns->rbs[ns->active_rb].weight;
}

static inline void
dpa_nvme_mp_sqes_send(struct dpa_nvme_mp_cq *cq, struct dpa_nvme_mp_sq *sq,
		uint32_t nsid, uint32_t start_idx, uint32_t end_idx)
{
	struct snap_dpa_p2p_msg_rb_tail msg;
	struct dpa_nvme_mp_ns *ns = sq->namespaces[nsid];
	struct snap_dpa_p2p_q *p2p_q;
	struct dpa_nvme_mp_rb *rb;
	uint16_t num_elements;

	if (snap_unlikely(!ns || ns->active_rb >= cq->num_p2p_queues)) {
		for (size_t i = start_idx; i != end_idx; i = (i + 1) % sq->queue_depth)
			dpa_nvme_mp_error_cqe_send(cq, sq, i, NVME_SC_INVALID_NSID);
		return;
	}

	/* TODO_Itay: check perf against an array of only active rbs */
	if (!ns->credits)
		dpa_nvme_mp_ns_next_rb_get(ns, cq->num_p2p_queues);

	p2p_q = &cq->p2p_queues[ns->active_rb];
	rb = &ns->rbs[ns->active_rb];
	ns->credits--;

	num_elements = (end_idx - start_idx) % sq->queue_depth;
	snap_dpa_dma_cyclic_buffer_write(p2p_q->dma_q, sq->sqe_buffer, snap_dma_q_dpa_mkey(p2p_q->dma_q),
			rb->arm_rb_addr, rb->arm_rb_mkey, start_idx, rb->tail,
			num_elements, sizeof(struct nvme_cmd), sq->queue_depth, NULL);
	rb->tail = (rb->tail + num_elements) % sq->queue_depth;

	msg = (struct snap_dpa_p2p_msg_rb_tail) {
		.base.type = SNAP_DPA_P2P_MSG_NVME_MP_RB_TAIL,
		.base.qid = sq->sqid,
		.rb_addr = (void *) rb->arm_rb_addr,
		.rb_tail = rb->tail,
	};

	snap_dpa_p2p_send_msg(p2p_q, (struct snap_dpa_p2p_msg *) &msg);

	p2p_q->dma_q->ops->progress_tx(p2p_q->dma_q, -1);
}

static void
dpa_nvme_mp_io_host2dpa_done(struct snap_dma_completion *comp, int status)
{
	struct dpa_nvme_mp_sq *sq = container_of(comp, struct dpa_nvme_mp_sq, host2dpa_comp);
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	uint32_t prev_nsid, curr_nsid, i;

	/* Split the sqe batch into sub-batches, where all sqes within a sub-batch share the same nsid. */
	prev_nsid = sq->sqe_buffer[sq->arm_sq_tail].nsid;
	for (i = sq->arm_sq_tail; i != sq->last_read_sq_tail; i = (i + 1) % sq->queue_depth) {
		/* Recovery mark should go here */
		curr_nsid = sq->sqe_buffer[i].nsid;
		if (prev_nsid != curr_nsid) {
			dpa_nvme_mp_sqes_send(cq, sq, prev_nsid, sq->arm_sq_tail, i);
			sq->arm_sq_tail = i;
			prev_nsid = curr_nsid;
		}
	}

	dpa_nvme_mp_sqes_send(cq, sq, curr_nsid, sq->arm_sq_tail, sq->last_read_sq_tail);
	sq->arm_sq_tail = sq->last_read_sq_tail;
}

static void
dpa_nvme_mp_admin_host2dpa_done(struct snap_dma_completion *comp, int status)
{
	struct dpa_nvme_mp_sq *sq = container_of(comp, struct dpa_nvme_mp_sq, host2dpa_comp);
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();

	dpa_nvme_mp_sqes_send(cq, sq, 0, sq->arm_sq_tail, sq->last_read_sq_tail);
	sq->arm_sq_tail = sq->last_read_sq_tail;
}

static int dpa_nvme_mp_queues_init(struct dpa_nvme_mp_cq *cq)
{
	int err, i;

	for (i = 0; i < cq->num_p2p_queues; i++) {
		err = dpa_dma_ep_init(cq->p2p_queues[i].dma_q, true);
		if (err) {
			dpa_error("Failed to init p2p queue %u\n", i);
			return -1;
		}
	}

	return 0;
}

static inline void dpa_msix_raise()
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();

	dpa_msix_arm();
	dpa_msix_send(cq->msix_cqnum);
}

static void
dpa_nvme_mp_dpa2host_done(struct snap_dma_completion *comp, int status)
{
	dpa_msix_raise();
}

static int dpa_nvme_mp_cq_create(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cmd *nvme_cmd = (struct dpa_nvme_mp_cmd *)cmd;
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();

	memcpy(cq, &nvme_cmd->cmd_cq_create.cq, sizeof(nvme_cmd->cmd_cq_create.cq));

	TAILQ_INIT(&cq->sqs);

	if (dpa_nvme_mp_queues_init(cq))
		return SNAP_DPA_RSP_ERR;

	/* Set count to 1 if msix isn't required, so callback won't be called */
	cq->comp.count = !cq->msix_required;
	cq->comp.func = dpa_nvme_mp_dpa2host_done;
	cq->phase = 1;
	cq->shadow_cq = dpa_thread_alloc(cq->queue_depth * sizeof(struct nvme_cqe));

	/* TODO_Doron: input validation/sanity check */
	dpa_debug("nvme cq create\n");

	return SNAP_DPA_RSP_OK;
}

static int
dpa_nvme_mp_rb_attach(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_sq *sq = TAILQ_FIRST(&cq->sqs);
	struct dpa_nvme_mp_cmd *ncmd = (struct dpa_nvme_mp_cmd *)cmd;
	struct dpa_nvme_mp_cmd_rb_attach *rb_cmd = &ncmd->cmd_rb_attach;
	struct dpa_nvme_mp_ns *ns;
	size_t ns_size;

	if (!sq->namespaces[rb_cmd->nsid]) {
		ns_size = sizeof(struct dpa_nvme_mp_ns) + cq->num_p2p_queues * sizeof(struct dpa_nvme_mp_rb);
		sq->namespaces[rb_cmd->nsid] = dpa_thread_alloc(ns_size);
		memset(sq->namespaces[rb_cmd->nsid], 0, ns_size);
	}

	ns = sq->namespaces[rb_cmd->nsid];
	ns->rbs[rb_cmd->qp_id] = (struct dpa_nvme_mp_rb) {
		.arm_rb_addr = rb_cmd->arm_rb_addr,
		.arm_rb_mkey = rb_cmd->arm_rb_mkey,
		.weight = rb_cmd->weight,
		.tail = 0,
	};

	ns->active_rb = rb_cmd->qp_id;
	ns->credits = rb_cmd->weight;

	return SNAP_DPA_RSP_OK;
}

static int
dpa_nvme_mp_rb_detach(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_sq *sq = TAILQ_FIRST(&cq->sqs);
	struct dpa_nvme_mp_cmd *ncmd = (struct dpa_nvme_mp_cmd *)cmd;
	struct dpa_nvme_mp_cmd_rb_detach *rb_cmd = &ncmd->cmd_rb_detach;
	struct dpa_nvme_mp_ns *ns = sq->namespaces[rb_cmd->nsid];
	size_t i;

	struct snap_dpa_p2p_msg_rb_detached msg = {
		.rb_addr = (void *) ns->rbs[rb_cmd->qp_id].arm_rb_addr,
		.base.type = SNAP_DPA_P2P_MSG_NVME_MP_RB_DETACHED,
		.base.qid = sq->sqid,
	};

	ns->rbs[rb_cmd->qp_id] = (struct dpa_nvme_mp_rb) { 0 };

	for (i = 0; i < cq->num_p2p_queues; i++) {
		if (ns->rbs[i].arm_rb_addr) {
			if (ns->active_rb == rb_cmd->qp_id) {
				ns->active_rb = i;
				ns->credits = ns->rbs[i].weight;
			}
			break;
		}
	}

	/* If there are no rbs left, mark ns as invalid */
	if (i == cq->num_p2p_queues)
		ns->active_rb = cq->num_p2p_queues;

	snap_dpa_p2p_send_msg(&cq->p2p_queues[rb_cmd->qp_id], (struct snap_dpa_p2p_msg *) &msg);
	cq->p2p_queues[rb_cmd->qp_id].dma_q->ops->progress_tx(cq->p2p_queues[rb_cmd->qp_id].dma_q, -1);

	return SNAP_DPA_RSP_OK;
}

static int
dpa_nvme_mp_rb_modify(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_sq *sq = TAILQ_FIRST(&cq->sqs);
	struct dpa_nvme_mp_cmd *ncmd = (struct dpa_nvme_mp_cmd *)cmd;
	struct dpa_nvme_mp_cmd_rb_modify *rb_cmd = &ncmd->cmd_rb_modify;
	struct dpa_nvme_mp_rb *rb = &sq->namespaces[rb_cmd->nsid]->rbs[rb_cmd->qp_id];

	if (!rb) {
		dpa_error("SQ %u: RB of nsid %u, qp_id %u could not be found\n", sq->sqid, rb_cmd->nsid, rb_cmd->qp_id);
		return SNAP_DPA_RSP_ERR;
	}

	if (rb_cmd->mask.weight)
		rb->weight = rb_cmd->weight;

	return SNAP_DPA_RSP_OK;
}

static int dpa_nvme_mp_sq_create(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cmd *nvme_cmd = (struct dpa_nvme_mp_cmd *)cmd;
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_sq *sq;

	sq = dpa_thread_alloc(sizeof(*sq));

	memcpy(sq, &nvme_cmd->cmd_sq_create.sq, sizeof(nvme_cmd->cmd_sq_create.sq));

	memset(sq->namespaces, 0, sizeof(*sq->namespaces));
	sq->sqe_buffer = (struct nvme_cmd *) dpa_thread_alloc(sq->queue_depth * SNAP_DPA_NVME_SQE_SIZE);
	sq->host2dpa_comp = (struct snap_dma_completion) {
		.func = sq->sqid ? dpa_nvme_mp_io_host2dpa_done : dpa_nvme_mp_admin_host2dpa_done,
		.count = 0,
	};
	sq->sq_head = 0;
	sq->last_read_sq_tail = 0;

	/* TODO_Doron: input validation/sanity check */
	TAILQ_INSERT_TAIL(&cq->sqs, sq, entry);

	snap_debug("sq create: id %d, state:%d", sq->sqid, sq->state);

	return SNAP_DPA_RSP_OK;
}

static int dpa_nvme_mp_sq_destroy(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_sq *sq;

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		sq->state = DPA_NVME_MP_STATE_ERR;
	}

	return SNAP_DPA_RSP_OK;
}

int dpa_nvme_mp_sq_modify(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_sq *sq;
	struct dpa_nvme_mp_cmd *ncmd = (struct dpa_nvme_mp_cmd *)cmd;
	enum dpa_nvme_mp_state next_state = ncmd->cmd_sq_modify.state;
	struct dpa_sq_modify_mask mask = ncmd->cmd_sq_modify.mask;
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		if (sq->sqid == ncmd->cmd_sq_modify.sqid) {
			sq->state = next_state;
			if (mask.state)
				sq->state = next_state;
			if (mask.host_sq_tail)
				sq->arm_sq_tail =
					ncmd->cmd_sq_modify.host_sq_tail;

			if (mask.state && sq->state == DPA_NVME_MP_STATE_RDY)
				dpa_duar_arm(sq->duar_id, rt_ctx->db_cq.cq_num);

			return SNAP_DPA_RSP_OK;
		}
	}

	dpa_debug("error modify: SQ id %d not found\n", ncmd->cmd_sq_modify.sqid);
	return SNAP_DPA_RSP_OK;
}

static int dpa_nvme_mp_sq_query(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_sq *sq;
	struct dpa_nvme_mp_cmd *ncmd = (struct dpa_nvme_mp_cmd *)cmd;
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();
	volatile uint64_t host_sq_tail;

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		if (sq->sqid == ncmd->cmd_sq_query.sqid) {
			dpa_duar_arm(sq->duar_id, rt_ctx->db_cq.cq_num);
			if (sq->state == DPA_NVME_MP_STATE_RDY)
			dpa_duar_arm(sq->duar_id, rt_ctx->db_cq.cq_num);
			host_sq_tail = dpa_ctx_read(sq->duar_id);
			dpa_write_rsp(sq->state, host_sq_tail);
			return SNAP_DPA_RSP_OK;
		}
	}

	dpa_debug("error query: SQ id %d not found\n", ncmd->cmd_sq_query.sqid);
	return SNAP_DPA_RSP_ERR;
}

static int dpa_nvme_mp_cq_query(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	volatile uint64_t host_cq_head;

	host_cq_head = dpa_ctx_read(cq->cq_head_duar_id);
	dpa_write_rsp(cq->state, host_cq_head);

	return SNAP_DPA_RSP_OK;
}

static int dpa_nvme_mp_cq_destroy(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();

	cq->state = DPA_NVME_MP_STATE_ERR;
	return SNAP_DPA_RSP_OK;
}

static int dpa_nvme_cq_modify(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct dpa_nvme_mp_cmd *ncmd = (struct dpa_nvme_mp_cmd *)cmd;
	struct dpa_cq_modify_mask mask = ncmd->cmd_cq_modify.mask;

	if (mask.state)
		cq->state = ncmd->cmd_cq_modify.state;

	return SNAP_DPA_RSP_OK;
}

static int do_command(int *done)
{
	struct snap_dpa_tcb *tcb = dpa_tcb();
	struct mlx5_cqe64 *cqe;
	struct snap_dpa_cmd *cmd;
	uint32_t rsp_status;

	cqe = snap_dv_poll_cq(&tcb->cmd_cq, 64);
	if (!cqe)
		return 0;

	/**
	 * Set mbox key as active because logger macros will restore
	 * current active key. It will lead to the crash if cmd is
	 * accessed after the dpa_debug and friends
	 */
	dpa_window_set_active_mkey(tcb->mbox_lkey);
	cmd = snap_dpa_mbox_to_cmd(dpa_mbox());

	if (snap_likely(cmd->sn == tcb->cmd_last_sn))
		goto cmd_done;

	dpa_debug("sn %d: new command 0x%x\n", cmd->sn, cmd->cmd);

	tcb->cmd_last_sn = cmd->sn;
	rsp_status = SNAP_DPA_RSP_OK;

	switch (cmd->cmd) {
		case SNAP_DPA_CMD_STOP:
			*done = 1;
			break;
		case DPA_NVME_MP_CQ_CREATE:
			rsp_status = dpa_nvme_mp_cq_create(cmd);
			break;
		case DPA_NVME_MP_CQ_DESTROY:
			rsp_status = dpa_nvme_mp_cq_destroy(cmd);
			break;
		case DPA_NVME_MP_CQ_MODIFY:
			rsp_status = dpa_nvme_cq_modify(cmd);
			break;
		case DPA_NVME_MP_CQ_QUERY:
			rsp_status = dpa_nvme_mp_cq_query(cmd);
			break;
		case DPA_NVME_MP_SQ_CREATE:
			rsp_status = dpa_nvme_mp_sq_create(cmd);
			break;
		case DPA_NVME_MP_SQ_DESTROY:
			rsp_status = dpa_nvme_mp_sq_destroy(cmd);
			break;
		case DPA_NVME_MP_SQ_MODIFY:
			rsp_status = dpa_nvme_mp_sq_modify(cmd);
			break;
		case DPA_NVME_MP_SQ_QUERY:
			rsp_status = dpa_nvme_mp_sq_query(cmd);
			break;
		case DPA_NVME_MP_RB_ATTACH:
			rsp_status = dpa_nvme_mp_rb_attach(cmd);
			break;
		case DPA_NVME_MP_RB_DETACH:
			rsp_status = dpa_nvme_mp_rb_detach(cmd);
			break;
		case DPA_NVME_MP_RB_MODIFY:
			rsp_status = dpa_nvme_mp_rb_modify(cmd);
			break;
		default:
			dpa_warn("unsupported command %d\n", cmd->cmd);
	}

	dpa_debug("sn %d: done command 0x%x status %d\n", cmd->sn, cmd->cmd, rsp_status);
	snap_dpa_rsp_send(dpa_mbox(), rsp_status);
cmd_done:
	return 0;
}

static inline int process_commands(int *done)
{
	if (snap_likely(dpa_tcb()->counter++ % COMMAND_DELAY)) {
		return 0;
	}

	return do_command(done);
}

static inline int
nvme_mp_completions_poll(struct dpa_nvme_mp_cq *cq, struct dpa_nvme_mp_sq *sq, struct snap_dma_q *q)
{
	struct snap_rx_completion comps[NVME_MP_MAX_COMPS_POLL];
	uint16_t cq_avail;
	int n, i, count = 0;

	do {
		cq_avail = (cq->host_cq_head - cq->cq_tail - 1) % cq->queue_depth;
		snap_dv_arm_cq(&q->sw_qp.dv_rx_cq);
		n = snap_dma_q_poll_rx(q, comps, MIN(cq_avail, NVME_MP_MAX_COMPS_POLL));
		count += n;

		for (i = 0; i < n; i++)
			nvme_mp_completion_msg_handle(cq, sq, comps[i].data);

	} while (n == NVME_MP_MAX_COMPS_POLL);

	return count;
}

static inline void
nvme_mp_cqes_handle(struct dpa_rt_context *rt_ctx, struct dpa_nvme_mp_cq *cq,
		struct dpa_nvme_mp_sq *sq, struct snap_dma_q *dma_q)
{
	int num_comps = 0;
	uint16_t old_cq_tail;

	dpa_duar_arm(cq->cq_head_duar_id, rt_ctx->db_cq.cq_num);
	cq->host_cq_head = dpa_ctx_read(cq->cq_head_duar_id);
	old_cq_tail = cq->cq_tail;

	/* TODO_Itay: to be replaced by shared rx cq */
	for (int i = 0; i < cq->num_p2p_queues; i++)
		num_comps += nvme_mp_completions_poll(cq, sq, cq->p2p_queues[i].dma_q);

	if (num_comps) {
		cq->comp.count += snap_dpa_dma_cyclic_buffer_write(dma_q, cq->shadow_cq,
				snap_dma_q_dpa_mkey(dma_q), cq->host_cq_addr, cq->host_mkey,
				old_cq_tail, old_cq_tail, num_comps, sizeof(struct nvme_cqe),
				cq->queue_depth, &cq->comp);
		dma_q->ops->complete_tx(dma_q);
	}
}

static inline void
nvme_mp_sqes_handle(struct dpa_rt_context *rt_ctx, struct dpa_nvme_mp_cq *cq,
		struct dpa_nvme_mp_sq *sq, struct snap_dma_q *dma_q)
{
	uint64_t sq_tail;

	dpa_duar_arm(sq->duar_id, rt_ctx->db_cq.cq_num);
	sq_tail = dpa_ctx_read(sq->duar_id);

	if (sq_tail != sq->last_read_sq_tail) {
		sq->host2dpa_comp.count += snap_dpa_dma_cyclic_buffer_write(dma_q,
				(void *) sq->host_sq_addr, sq->host_mkey, (uint64_t) sq->sqe_buffer,
				snap_dma_q_dpa_mkey(dma_q), sq->last_read_sq_tail, sq->last_read_sq_tail,
				(sq_tail - sq->last_read_sq_tail) % sq->queue_depth,
				SNAP_DPA_NVME_SQE_SIZE, sq->queue_depth, &sq->host2dpa_comp);
		sq->last_read_sq_tail = sq_tail;
		dma_q->ops->complete_tx(dma_q);
	}
}

static inline void nvme_mp_progress()
{
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();
	struct dpa_nvme_mp_cq *cq = get_nvme_cq();
	struct snap_dma_q *dma_q = cq->p2p_queues[0].dma_q;
	struct dpa_nvme_mp_sq *sq;

	if (snap_unlikely(cq->state != DPA_NVME_MP_STATE_RDY))
		return;

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		if (snap_unlikely(sq->state != DPA_NVME_MP_STATE_RDY))
			continue;

		dma_q->ops->progress_tx(dma_q, -1);
		nvme_mp_cqes_handle(rt_ctx, cq, sq, dma_q);
		nvme_mp_sqes_handle(rt_ctx, cq, sq, dma_q);
	}
}

int dpa_init()
{
	struct dpa_nvme_mp_cq *cq;

	dpa_rt_init();
	cq = dpa_thread_alloc(sizeof(*cq));
	if (cq != get_nvme_cq())
		dpa_fatal("cq must follow rt context: cq@%p expected@%p\n", cq, get_nvme_cq());

	cq->state = DPA_NVME_MP_STATE_INIT;

	dpa_debug("DPA nvme init done\n");
	return 0;
}

static void dpa_run_polling()
{
	int done = 0;

	dpa_rt_start(true);

	do {
		process_commands(&done);
		nvme_mp_progress();
	} while (!done);
}

static inline void dpa_run_event()
{
	struct snap_dpa_tcb *tcb = dpa_tcb();
	int done;

	/* may be add post init ?? */
	if (snap_unlikely(tcb->init_done == 1)) {
		dpa_rt_start(true);
		tcb->init_done = 2;
	}

	do_command(&done);
	nvme_mp_progress();
}

int dpa_run()
{
	if (snap_likely(is_event_mode())) {
		dpa_run_event();
	} else {
		dpa_run_polling();
	}
	return 0;
}
