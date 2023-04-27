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
#include "snap_dpa_nvme_common.h"

#define COMMAND_DELAY 100000

/**
 * Single nvme queue per thread implementation. The thread can be
 * either in polling or in event mode
 */
static inline struct dpa_nvme_cq *get_nvme_cq()
{
	/* will redo to be mt safe */
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();

	/* cq is always allocated after rt context */
	return (struct dpa_nvme_cq *)SNAP_ALIGN_CEIL((uint64_t)(rt_ctx + 1), DPA_CACHE_LINE_BYTES);
}

static void dpa_write_rsp(enum dpa_nvme_state state, uint32_t db_value)
{
	struct dpa_nvme_rsp *rsp;

	rsp = (struct dpa_nvme_rsp *)snap_dpa_mbox_to_rsp(dpa_mbox());

	rsp->state.state = state;
	rsp->state.db_value = db_value;
}

int dpa_nvme_cq_create(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_cmd *nvme_cmd = (struct dpa_nvme_cmd *)cmd;
	struct dpa_nvme_cq *cq = get_nvme_cq();

	memcpy(cq, &nvme_cmd->cmd_cq_create.cq, sizeof(nvme_cmd->cmd_cq_create.cq));

	/* TODO_Doron: input validation/sanity check */
	dpa_debug("nvme cq create\n");

	return SNAP_DPA_RSP_OK;
}

int dpa_nvme_sq_create(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_cmd *nvme_cmd = (struct dpa_nvme_cmd *)cmd;
	struct dpa_nvme_cq *cq = get_nvme_cq();
	struct dpa_nvme_sq *sq;
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();

	sq = dpa_thread_alloc(sizeof(*sq));

	memcpy(sq, &nvme_cmd->cmd_sq_create.sq, sizeof(nvme_cmd->cmd_sq_create.sq));

	/* TODO_Doron: add support for multiple sqs per cq */
	TAILQ_INIT(&cq->sqs);

	/* TODO_Doron: input validation/sanity check */
	TAILQ_INSERT_TAIL(&cq->sqs, sq, entry);

	snap_dv_arm_cq(&rt_ctx->db_cq);
	dpa_duar_arm(sq->duar_id, rt_ctx->db_cq.cq_num);
	snap_debug("sq create: id %d, state:%d\n", sq->sqid, sq->state);

	return SNAP_DPA_RSP_OK;
}

int dpa_nvme_sq_destroy(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_cq *cq = get_nvme_cq();
	struct dpa_nvme_sq *sq;

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		sq->state = DPA_NVME_STATE_ERR;
	}

	return SNAP_DPA_RSP_OK;
}

int dpa_nvme_sq_modify(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_cq *cq = get_nvme_cq();
	struct dpa_nvme_sq *sq;
	struct dpa_nvme_cmd *ncmd = (struct dpa_nvme_cmd *)cmd;
	enum dpa_nvme_state next_state = ncmd->cmd_sq_modify.state;

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		if (sq->sqid == ncmd->cmd_sq_modify.sqid) {
			sq->state = next_state;
			return SNAP_DPA_RSP_OK;
		}
	}

	dpa_debug("error modify: SQ id %d not found\n", ncmd->cmd_sq_modify.sqid);
	return SNAP_DPA_RSP_OK;
}

int dpa_nvme_sq_query(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_cq *cq = get_nvme_cq();
	struct dpa_nvme_sq *sq;
	struct dpa_nvme_cmd *ncmd = (struct dpa_nvme_cmd *)cmd;
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();
	volatile uint64_t host_sq_tail;

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		if (sq->sqid == ncmd->cmd_sq_query.sqid) {
			snap_dv_arm_cq(&rt_ctx->db_cq);
			dpa_duar_arm(sq->duar_id, rt_ctx->db_cq.cq_num);
			host_sq_tail = dpa_ctx_read(sq->duar_id);
			dpa_write_rsp(sq->state, host_sq_tail);
			return SNAP_DPA_RSP_OK;
		}
	}

	dpa_debug("error query: SQ id %d not found\n", ncmd->cmd_sq_query.sqid);
	return SNAP_DPA_RSP_ERR;
}


int dpa_nvme_cq_query(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_cq *cq = get_nvme_cq();
	volatile uint64_t host_cq_head;
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();

	snap_dv_arm_cq(&rt_ctx->db_cq);
	host_cq_head = dpa_ctx_read(cq->cq_head_duar_id);
	dpa_write_rsp(cq->state, host_cq_head);

	return SNAP_DPA_RSP_OK;
}

int dpa_nvme_cq_destroy(struct snap_dpa_cmd *cmd)
{
	struct dpa_nvme_cq *cq = get_nvme_cq();

	cq->state = DPA_NVME_STATE_ERR;
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
		case DPA_NVME_CQ_CREATE:
			rsp_status = dpa_nvme_cq_create(cmd);
			break;
		case DPA_NVME_CQ_DESTROY:
			rsp_status = dpa_nvme_cq_destroy(cmd);
			break;
		case DPA_NVME_CQ_QUERY:
			rsp_status = dpa_nvme_cq_query(cmd);
			break;
		case DPA_NVME_SQ_CREATE:
			rsp_status = dpa_nvme_sq_create(cmd);
			break;
		case DPA_NVME_SQ_DESTROY:
			rsp_status = dpa_nvme_sq_destroy(cmd);
			break;
		case DPA_NVME_SQ_MODIFY:
			rsp_status = dpa_nvme_sq_modify(cmd);
			break;
		case DPA_NVME_SQ_QUERY:
			rsp_status = dpa_nvme_sq_query(cmd);
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

static inline void dpa_msix_raise()
{
	struct dpa_nvme_cq *cq = get_nvme_cq();

	dpa_msix_arm();
	dpa_msix_send(cq->msix_cqnum);
}

static inline void nvme_progress()
{
	struct dpa_nvme_cq *cq = get_nvme_cq();
	uint64_t sq_tail;
	uint64_t cq_head;
	int msix_count;
	struct dpa_nvme_sq *sq;
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();

	if (snap_unlikely(cq->state != DPA_NVME_STATE_RDY))
		return;

	msix_count = dpa_p2p_recv();
	if (msix_count)
		dpa_msix_raise();

	TAILQ_FOREACH(sq, &cq->sqs, entry) {
		if (snap_unlikely(sq->state != DPA_NVME_STATE_RDY))
			continue;
		snap_dv_arm_cq(&rt_ctx->db_cq);
		dpa_duar_arm(sq->duar_id, rt_ctx->db_cq.cq_num);

		sq_tail = dpa_ctx_read(sq->duar_id);
		if (sq_tail != sq->host_sq_tail) {
			snap_dpa_p2p_send_sq_tail(&rt_ctx->dpa_cmd_chan, sq->sqid, sq_tail,
			sq->base_addr,
			sq->dpu_mkey, sq->dpu_sqe_shadow_addr, sq->dpu_sqe_shadow_mkey, sq->host_sq_tail, sq->queue_depth);
			sq->host_sq_tail = (uint32_t) sq_tail;
		}
		/* kick off sq tail message fast */
		rt_ctx->dpa_cmd_chan.dma_q->ops->progress_tx(rt_ctx->dpa_cmd_chan.dma_q);
	}
	dpa_duar_arm(cq->cq_head_duar_id, rt_ctx->db_cq.cq_num);
	snap_dv_arm_cq(&cq->cq_head_db_hw_cq);
	cq_head = dpa_ctx_read(cq->cq_head_duar_id);
	if (cq->host_cq_head != cq_head) {
		snap_dpa_p2p_send_cq_head(&rt_ctx->dpa_cmd_chan, cq_head);
		cq->host_cq_head = cq_head;
	}

	/* kick off cq head message */
	rt_ctx->dpa_cmd_chan.dma_q->ops->progress_tx(rt_ctx->dpa_cmd_chan.dma_q);
}

int dpa_init()
{
	struct dpa_nvme_cq *cq;

	dpa_rt_init();
	cq = dpa_thread_alloc(sizeof(*cq));
	if (cq != get_nvme_cq())
		dpa_fatal("cq must follow rt context: cq@%p expected@%p\n", cq, get_nvme_cq());

	cq->state = DPA_NVME_STATE_INIT;

	dpa_debug("DPA nvme init done\n");
	return 0;
}

static void dpa_run_polling()
{
	int done = 0;

	dpa_rt_start();

	do {
		process_commands(&done);
		nvme_progress();
	} while (!done);
}

static inline void dpa_run_event()
{
	struct snap_dpa_tcb *tcb = dpa_tcb();
	int done;

	/* may be add post init ?? */
	if (snap_unlikely(tcb->init_done == 1)) {
		dpa_rt_start();
		tcb->init_done = 2;
	}

	do_command(&done);
	nvme_progress();
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
