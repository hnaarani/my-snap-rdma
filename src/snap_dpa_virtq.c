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
#include <stdlib.h>
#include <linux/virtio_pci.h>

#include "snap_macros.h"
#include "config.h"
#include "snap_virtio_blk.h"
#include "snap_dpa_p2p.h"
#include "snap_dpa_virtq.h"
#include "snap_dpa_rt.h"

#if HAVE_FLEXIO
#include "snap_dpa.h"
#include "snap_dpa_virtq.h"

#include "mlx5_ifc.h"

#include "snap_lib_log.h"

SNAP_LIB_LOG_REGISTER(DPA_VIRTQ);

#define SNAP_DPA_VIRTQ_BBUF_ALIGN 4096

/* make sure our virtq commands are fit into mailbox */
SNAP_STATIC_ASSERT(sizeof(struct dpa_virtq_cmd) < SNAP_DMA_THREAD_MBOX_CMD_SIZE,
		"Ooops, struct dpa_virtq_cmd is too big");

struct snap_dpa_virtq_attr {
	// hack to fit into 'standard' type
	size_t type_size;
};

static uint32_t snap_get_dev_emu_id(struct snap_device *sdev)
{
	return sdev->mdev.device_emulation->obj_id;
}

static struct snap_dpa_virtq *snap_dpa_virtq_create(struct snap_device *sdev,
		struct snap_dpa_virtq_attr *dpa_vq_attr, struct snap_virtio_common_queue_attr *vq_attr)
{
	/* TODO: should get these from the upper layer */
	struct snap_dpa_rt_filter f = {
		//.mode = SNAP_DPA_RT_THR_POLLING,
		.mode = SNAP_DPA_RT_THR_EVENT,
		.queue_mux_mode = SNAP_DPA_RT_THR_SINGLE
	};
	struct snap_dpa_rt_attr attr = {};

	struct snap_dpa_virtq *vq;
	void *mbox;
	struct dpa_virtq_cmd *cmd;
	struct snap_dpa_rsp *rsp;
	size_t desc_shadow_size;
	struct snap_hw_cq db_hw_cq;
	struct snap_dpa_duar_attr duar_attr;
	int ret;

	snap_debug("create dpa virtq\n");

	vq = calloc(1, sizeof(*vq));
	if (!vq)
		return NULL;

	vq->rt = snap_dpa_rt_get(sdev->sctx->context, SNAP_DPA_VIRTQ_APP, &attr);
	if (!vq->rt)
		goto free_vq;

	vq->rt_thr = snap_dpa_rt_thread_get(vq->rt, &f, NULL);
	if (!vq->rt_thr)
		goto put_rt;

	/* pass queue data to the worker */
	mbox = snap_dpa_thread_mbox_acquire(vq->rt_thr->thread);

	cmd = (struct dpa_virtq_cmd *)snap_dpa_mbox_to_cmd(mbox);
	//sleep(1);
	snap_dpa_log_print(vq->rt_thr->thread->dpa_log);
	//printf("wait1...\n"); getchar();
	/* at the momemnt pass only avail/used/descr addresses */
	vq->common.idx = vq_attr->vattr.idx;
	vq->common.size = vq_attr->vattr.size;
	vq->common.desc = vq_attr->vattr.desc;
	vq->common.driver = vq_attr->vattr.driver;
	vq->common.device = vq_attr->vattr.device;
	vq->common.msix_vector = vq_attr->vattr.event_qpn_or_msix;
	vq->common.dev_emu_id = snap_get_dev_emu_id(sdev);
	snap_info("Got ev_mode 0x%x msix 0x%x\n", vq_attr->vattr.ev_mode, vq_attr->vattr.event_qpn_or_msix);

	/* register mr for the avail staging buffer */
	desc_shadow_size = vq->common.size * sizeof(struct virtq_desc);

	ret = posix_memalign((void **)&vq->desc_shadow, SNAP_DPA_VIRTQ_DESC_SHADOW_ALIGN, desc_shadow_size);
	if (ret) {
		snap_error("Failed to allocate virtq dpa window buffer: %d\n", ret);
		goto release_mbox;
	}

	memset(vq->desc_shadow, 0, desc_shadow_size);

	/* TODO: rename to descr staging buffer */
	vq->desc_shadow_mr = snap_reg_mr(vq->rt->dpa_proc->pd, vq->desc_shadow, desc_shadow_size);
	if (!vq->desc_shadow_mr) {
		snap_error("Failed to allocate virtq dpa window mr: %m\n");
		goto free_dpa_window;
	}

	memset(&cmd->cmd_create, 0, sizeof(cmd->cmd_create));
	memcpy(&cmd->cmd_create.vq.common, &vq->common, sizeof(vq->common));
	cmd->cmd_create.vq.hw_available_index = vq_attr->hw_available_index;
	cmd->cmd_create.vq.hw_used_index = vq_attr->hw_used_index;
	cmd->cmd_create.vq.dpu_desc_shadow_mkey = vq->desc_shadow_mr->lkey;
	cmd->cmd_create.vq.dpu_desc_shadow_addr = (uint64_t)vq->desc_shadow;

	ret = snap_cq_to_hw_cq(vq->rt_thr->db_cq, &db_hw_cq);
	if (ret)
		goto free_dpa_window_mr;

	duar_attr.dev_emu_id = virtio_emu_dev_emu_id(dev);
	duar_attr.queue_id = vq_attr->vattr.idx;
	duar_attr.cq_num = db_hw_cq.cq_num;
	duar_attr.dev_type = MLX5_HOTPLUG_DEVICE_TYPE_VIRTIO_BLK;
	duar_attr.queue_type = MLX5_DEV_DB_RESERVED;
	duar_attr.map_state = MLX5_DEV_DB_MAPPED;
	vq->duar = snap_dpa_duar_create(dev->dev_emu.context, &duar_attr);

	if (!vq->duar) {
		snap_error("Failed to create virt duar mapping: dev_emu_id %d queue_id %d cq_num 0x%x\n",
				snap_get_dev_emu_id(sdev), vq_attr->vattr.idx, db_hw_cq.cq_num);
		goto free_dpa_window_mr;
	}

	snap_debug("virtq duar 0x%x mapping: dev_emu_id %d queue_id %d cq_num 0x%x\n",
			snap_dpa_duar_id(vq->duar), snap_get_dev_emu_id(sdev),
			vq_attr->vattr.idx, db_hw_cq.cq_num);
	//printf("duar mapping created\n");getchar();

	if (vq->common.msix_vector != VIRTIO_MSI_NO_VECTOR) {
		/* EQ should sit on the sdev level, but this is going to be
		 * implemented only on 'devemu' branch
		 */
		vq->msix_eq = snap_dpa_msix_eq_create(sdev->sctx->context, snap_get_dev_emu_id(sdev),
				vq->common.msix_vector, MLX5_HOTPLUG_DEVICE_TYPE_VIRTIO_BLK);
		if (!vq->msix_eq) {
			snap_error("Failed to create MSIX_EQ\n");
			goto free_dpa_duar;
		}

		ret = snap_dpa_rt_thread_msix_add(vq->rt_thr, vq->msix_eq, &cmd->cmd_create.vq.msix_cqnum);
		if (ret)
			goto free_msix_eq;

		SNAP_LIB_LOG_INFO("MSIX eqn 0x%x cqn 0x%x msix_vector %d",
				snap_dpa_msix_eq_id(vq->msix_eq),
				cmd->cmd_create.vq.msix_cqnum,
				vq->common.msix_vector);
	}

	vq->cross_mkey = snap_create_cross_mkey(vq->rt->dpa_proc->pd, sdev);
	if (!vq->cross_mkey) {
		snap_error("Failed to create virtq cross mkey\n");
		goto remove_msix_vector;
	}

	//sleep(1);
	//snap_dpa_log_print(vq->rt_thr->thread->dpa_log);
	//printf("wait... xmkey 0x%x\n", vq->cross_mkey->mkey); //getchar();

	cmd->cmd_create.vq.dpu_xmkey = vq->cross_mkey->mkey;
	cmd->cmd_create.vq.dpa_xmkey = vq->cross_mkey->mkey;
	cmd->cmd_create.vq.duar_id = snap_dpa_duar_id(vq->duar);
	snap_dpa_cmd_send(vq->rt_thr->thread, &cmd->base, DPA_VIRTQ_CMD_CREATE);

	rsp = snap_dpa_rsp_wait(mbox);
	if (rsp->status != SNAP_DPA_RSP_OK) {
		snap_dpa_log_print(vq->rt_thr->thread->dpa_log);
		snap_error("Failed to create DPA virtio queue: %d\n", rsp->status);
		goto free_cross_mkey;
	}

	snap_dpa_thread_mbox_release(vq->rt_thr->thread);
	//printf("virtq mapping created\n");getchar();
	return vq;

free_cross_mkey:
	snap_destroy_cross_mkey(vq->cross_mkey);
remove_msix_vector:
	if (vq->msix_eq)
		snap_dpa_rt_thread_msix_remove(vq->rt_thr, vq->msix_eq);
free_msix_eq:
	if (vq->msix_eq)
		snap_dpa_msix_eq_destroy(vq->msix_eq);
free_dpa_duar:
	snap_dpa_duar_destroy(vq->duar);
free_dpa_window_mr:
	ibv_dereg_mr(vq->desc_shadow_mr);
free_dpa_window:
	free(vq->desc_shadow);
release_mbox:
	snap_dpa_thread_mbox_release(vq->rt_thr->thread);
	snap_dpa_rt_thread_put(vq->rt_thr);
put_rt:
	snap_dpa_rt_put(vq->rt);
free_vq:
	free(vq);
	return NULL;
}

static void snap_dpa_virtq_destroy(struct snap_dpa_virtq *vq)
{
	void *mbox;
	struct dpa_virtq_cmd *cmd;
	struct snap_dpa_rsp *rsp;

	snap_info("destroy dpa virtq: 0x%x:%d io_completed: %d comp_updates: %d used_updates: %d\n",
			vq->common.dev_emu_id, vq->common.idx,
			vq->stats.n_io_completed, vq->stats.n_compl_updates, vq->stats.n_used_updates);
	snap_dpa_log_print(vq->rt_thr->thread->dpa_log);
	mbox = snap_dpa_thread_mbox_acquire(vq->rt_thr->thread);

	cmd = (struct dpa_virtq_cmd *)snap_dpa_mbox_to_cmd(mbox);
	//printf("wait... b4 destroy command\n");getchar();
	snap_dpa_cmd_send(vq->rt_thr->thread, &cmd->base, DPA_VIRTQ_CMD_DESTROY);

	rsp = snap_dpa_rsp_wait(mbox);
	if (rsp->status != SNAP_DPA_RSP_OK)
		snap_error("Failed to destroy DPA virtio queue: %d\n", rsp->status);

	snap_dpa_thread_mbox_release(vq->rt_thr->thread);
	snap_dpa_log_print(vq->rt_thr->thread->dpa_log);
	//printf("wait... a4 destroy command\n");getchar();
	if (vq->msix_eq) {
		snap_dpa_rt_thread_msix_remove(vq->rt_thr, vq->msix_eq);
		snap_dpa_msix_eq_destroy(vq->msix_eq);
	}
	snap_dpa_duar_destroy(vq->duar);
	snap_dpa_rt_thread_put(vq->rt_thr);
	snap_dpa_rt_put(vq->rt);
	snap_destroy_cross_mkey(vq->cross_mkey);
	ibv_dereg_mr(vq->desc_shadow_mr);
	free(vq->desc_shadow);
	free(vq);
}

static enum dpa_virtq_state to_dpa_virtq_state(enum snap_virtq_state state)
{
	switch (state) {
	case SNAP_VIRTQ_STATE_INIT:
		return DPA_VIRTQ_STATE_INIT;
	case SNAP_VIRTQ_STATE_RDY:
		return DPA_VIRTQ_STATE_RDY;
	case SNAP_VIRTQ_STATE_SUSPEND:
		return DPA_VIRTQ_STATE_SUSPEND;
	case SNAP_VIRTQ_STATE_ERR:
		return DPA_VIRTQ_STATE_ERR;
	}

	return DPA_VIRTQ_STATE_ERR;
}

static enum snap_virtq_state to_snap_virtq_state(enum dpa_virtq_state state)
{
	switch (state) {
	case DPA_VIRTQ_STATE_INIT:
		return SNAP_VIRTQ_STATE_INIT;
	case DPA_VIRTQ_STATE_RDY:
		return SNAP_VIRTQ_STATE_RDY;
	case DPA_VIRTQ_STATE_SUSPEND:
		return SNAP_VIRTQ_STATE_SUSPEND;
	case DPA_VIRTQ_STATE_ERR:
		return SNAP_VIRTQ_STATE_ERR;
	}

	return SNAP_VIRTQ_STATE_ERR;
}

static int snap_dpa_virtq_query(struct snap_dpa_virtq *vq,
		struct snap_virtio_common_queue_attr *attr)
{
	void *mbox;
	struct dpa_virtq_cmd *cmd;
	struct dpa_virtq_rsp *rsp;

	mbox = snap_dpa_thread_mbox_acquire(vq->rt_thr->thread);

	cmd = (struct dpa_virtq_cmd *)snap_dpa_mbox_to_cmd(mbox);
	snap_dpa_cmd_send(vq->rt_thr->thread, &cmd->base, DPA_VIRTQ_CMD_QUERY);

	rsp = (struct dpa_virtq_rsp *)snap_dpa_rsp_wait(mbox);
	if (rsp->base.status != SNAP_DPA_RSP_OK) {
		snap_error("Failed to query DPA virtio queue: %d\n", rsp->base.status);
		snap_dpa_log_print(vq->rt_thr->thread->dpa_log);
	}

	snap_info("DPA query: vq_state %d avail %d used %d\n", rsp->vq_state.state,
			rsp->vq_state.hw_available_index,
			vq->hw_used_index);

	attr->vattr.state = to_snap_virtq_state(rsp->vq_state.state);
	attr->hw_available_index = rsp->vq_state.hw_available_index;
	attr->hw_used_index = vq->hw_used_index;
	snap_dpa_thread_mbox_release(vq->rt_thr->thread);
	return 0;
}

static int snap_dpa_virtq_modify(struct snap_dpa_virtq *vq,
		struct snap_virtio_common_queue_attr *attr)
{
	void *mbox;
	struct dpa_virtq_cmd *cmd;
	struct snap_dpa_rsp *rsp;

	snap_info("DPA modify to state %d\n", attr->vattr.state);
	mbox = snap_dpa_thread_mbox_acquire(vq->rt_thr->thread);

	cmd = (struct dpa_virtq_cmd *)snap_dpa_mbox_to_cmd(mbox);
	cmd->cmd_modify.state = to_dpa_virtq_state(attr->vattr.state);
	snap_dpa_cmd_send(vq->rt_thr->thread, &cmd->base, DPA_VIRTQ_CMD_MODIFY);

	rsp = snap_dpa_rsp_wait(mbox);
	if (rsp->status != SNAP_DPA_RSP_OK) {
		snap_error("Failed to modify DPA virtio queue: %d\n", rsp->status);
		snap_dpa_log_print(vq->rt_thr->thread->dpa_log);
	}

	snap_dpa_thread_mbox_release(vq->rt_thr->thread);
	return 0;
}

struct snap_virtio_queue *virtq_blk_dpa_create(struct snap_device *sdev,
		struct snap_virtio_common_queue_attr *attr)
{
	struct snap_dpa_virtq *vq;
	struct snap_dpa_virtq_attr dpa_attr = {};

	dpa_attr.type_size = sizeof(struct snap_virtio_queue);
	vq = snap_dpa_virtq_create(sdev, &dpa_attr, attr);
	attr->q_provider = SNAP_DPA_Q_PROVIDER;
	return (struct snap_virtio_queue *)vq;
}

int virtq_blk_dpa_destroy(struct snap_virtio_queue *vbq)
{
	struct snap_dpa_virtq *vq;

	vq = (struct snap_dpa_virtq *)vbq;
	snap_dpa_virtq_destroy(vq);
	return 0;
}

int virtq_blk_dpa_query(struct snap_virtio_queue *vbq,
		struct snap_virtio_common_queue_attr *attr)
{
	struct snap_dpa_virtq *vq;

	vq = (struct snap_dpa_virtq *)vbq;
	return snap_dpa_virtq_query(vq, attr);
}

int virtq_blk_dpa_modify(struct snap_virtio_queue *vbq,
		uint64_t mask, struct snap_virtio_common_queue_attr *attr)
{
	struct snap_dpa_virtq *vq;

	if (mask != SNAP_VIRTIO_BLK_QUEUE_MOD_STATE)
		return -EINVAL;

	vq = (struct snap_dpa_virtq *)vbq;
	return snap_dpa_virtq_modify(vq, attr);
}

#else

static struct snap_virtio_queue *virtq_blk_dpa_create(struct snap_device *sdev,
		struct snap_virtio_common_queue_attr *attr)
{
	return NULL;
}

static int virtq_blk_dpa_destroy(struct snap_virtio_queue *vbq)
{
	return -1;
}

static int virtq_blk_dpa_query(struct snap_virtio_queue *vq,
		struct snap_virtio_common_queue_attr *attr)
{
	return -1;
}

static int virtq_blk_dpa_modify(struct snap_virtio_queue *vq,
		uint64_t mask, struct snap_virtio_common_queue_attr *attr)
{
	return -1;
}

#endif

struct snap_dpa_virtq *to_dpa_queue(struct snap_virtio_queue *vq)
{
	return container_of(vq, struct snap_dpa_virtq, vq);
}

static int virtq_blk_dpa_poll(struct snap_virtio_queue *vq, struct virtq_split_tunnel_req *reqs, int num_reqs)
{
	struct snap_dpa_virtq *dpa_q = to_dpa_queue(vq);
	int n, i;
	struct snap_dpa_p2p_msg_vq_update *msg;

	/* TODO: use virtio specific recv msg, save one loop on translation,
	 * since max virtq heads is known we can pick several messages
	 */
	n = snap_dpa_p2p_recv_msg(&dpa_q->rt_thr->dpu_cmd_chan, (struct snap_dpa_p2p_msg **)&msg, 1);
	if (n <= 0)
		return n;

	if (msg->descr_head_count >= num_reqs) {
		snap_error("oops, too many requests (%d > %d)\n", n, num_reqs);
		return -ENOMEM;
	}

	if (dpa_q->debug_count++ % 1000 == 0)
		snap_dpa_log_print(dpa_q->rt_thr->thread->dpa_log);

	if (msg->base.type == SNAP_DPA_P2P_MSG_VQ_HEADS) {
		snap_debug("vq heads message %d heads\n", msg->descr_head_count);
		for (i = 0; i < msg->descr_head_count; i++) {
			reqs[i].hdr.num_desc = 0;
			reqs[i].hdr.descr_head_idx = msg->descr_heads[i];
			reqs[i].hdr.dpa_vq_table_flag = 0;
			snap_debug("vq head idx: %d\n", reqs[i].hdr.descr_head_idx);
		}
	} else if (msg->base.type == SNAP_DPA_P2P_MSG_VQ_TABLE || msg->base.type == SNAP_DPA_P2P_MSG_VQ_TABLE_CONT) {
		snap_debug("vq table message %d heads\n", msg->descr_head_count);
		for (i = 0; i < msg->descr_head_count; i++) {
			reqs[i].hdr.num_desc = 0;
			reqs[i].hdr.descr_head_idx = msg->descr_heads[i];
			reqs[i].hdr.dpa_vq_table_flag = VQ_TABLE_REC;
			reqs[i].tunnel_descs = dpa_q->desc_shadow;
			snap_debug("vq head idx: %d\n", reqs[i].hdr.descr_head_idx);
		}
	} else {
		snap_error("oops unknown p2p msg type\n");
		return -ENOTSUP;
	}

	return msg->descr_head_count;
}

static inline int flush_completions(struct snap_dpa_virtq *dpa_q)
{
	uint64_t used_elem_addr;
	int ret;

	/* TODO: remove fatal on write */
	used_elem_addr = dpa_q->common.device +
			offsetof(struct vring_used, ring[dpa_q->host_used_index & (dpa_q->common.size - 1)]);

	ret = snap_dma_q_write_short(dpa_q->rt_thr->dpu_cmd_chan.dma_q, dpa_q->pending_comps,
			sizeof(struct vring_used_elem) * dpa_q->num_pending_comps, used_elem_addr,
			dpa_q->cross_mkey->mkey);
	if (snap_unlikely(ret)) {
		snap_info("failed to send completion - %d\n", ret);
		return ret;
	}

	dpa_q->host_used_index += (uint16_t)dpa_q->num_pending_comps;
	dpa_q->num_pending_comps = 0;
	dpa_q->stats.n_compl_updates++;
	return ret;
}

int virtq_blk_dpa_complete(struct snap_virtio_queue *vq, struct vring_used_elem *comp)
{
	struct snap_dpa_virtq *dpa_q = to_dpa_queue(vq);
	int ret;

	dpa_q->pending_comps[dpa_q->num_pending_comps].id = comp->id;
	dpa_q->pending_comps[dpa_q->num_pending_comps].len = comp->len;

	dpa_q->num_pending_comps++;
	dpa_q->hw_used_index++;
	dpa_q->stats.n_io_completed++;

	/* after 8 gains are marginal. TODO: virtq param */
	if (dpa_q->num_pending_comps >= 8)
		goto flush_comps;

	if ((dpa_q->hw_used_index & (dpa_q->common.size - 1)) == 0)
		goto flush_comps;

	return 0;
flush_comps:
	/* TODO: rollback on error */
	return flush_completions(dpa_q);

	return ret;
}

int virtq_blk_dpa_send_completions(struct snap_virtio_queue *vq)
{
	struct snap_dpa_virtq *dpa_q = to_dpa_queue(vq);
	uint64_t used_idx_addr;
	int ret;

	if (dpa_q->num_pending_comps) {
		ret = flush_completions(dpa_q);
		if (ret)
			return ret;
	}

	if (dpa_q->host_used_index != dpa_q->hw_used_index)
		snap_error("Missing completions!!!\n");

	if (dpa_q->last_hw_used_index == dpa_q->hw_used_index)
		return 0;

	used_idx_addr = dpa_q->common.device + offsetof(struct vring_used, idx);
	ret = snap_dma_q_write_short(dpa_q->rt_thr->dpu_cmd_chan.dma_q, &dpa_q->hw_used_index, sizeof(uint16_t),
			used_idx_addr, dpa_q->cross_mkey->mkey);
	if (ret) {
		snap_info("failed to send hw_used - %d\n", ret);
		return ret;
	}
	/* if msix enabled, send also msix message */
	dpa_q->last_hw_used_index = dpa_q->hw_used_index;
	dpa_q->stats.n_used_updates++;

	if (dpa_q->msix_eq) {
		ret = snap_dpa_p2p_send_msix(&dpa_q->rt_thr->dpu_cmd_chan, 0);
		if (ret)
			snap_info("failed to send msix msg at used %d ret %d\n", dpa_q->last_hw_used_index, ret);
	}

	/* kick off completions */
	dpa_q->rt_thr->dpu_cmd_chan.dma_q->ops->progress_tx(dpa_q->rt_thr->dpu_cmd_chan.dma_q, -1);
	return ret;
}

int virtq_blk_dpa_send_status(struct snap_virtio_queue *vq, void *data, int size, uint64_t raddr)
{
	struct snap_dpa_virtq *dpa_q = to_dpa_queue(vq);
	int ret;

	ret = snap_dma_q_write_short(dpa_q->rt_thr->dpu_cmd_chan.dma_q, data, size,
			raddr, dpa_q->cross_mkey->mkey);
	return ret;
}

struct virtq_q_ops snap_virtq_blk_dpa_ops = {
	.create = virtq_blk_dpa_create,
	.destroy = virtq_blk_dpa_destroy,
	.query = virtq_blk_dpa_query,
	.modify = virtq_blk_dpa_modify,
	.poll = virtq_blk_dpa_poll,
	.complete = virtq_blk_dpa_complete,
	.send_completions = virtq_blk_dpa_send_completions
};

struct virtq_q_ops *get_dpa_queue_ops(void)
{
	return &snap_virtq_blk_dpa_ops;
}
