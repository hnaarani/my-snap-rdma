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

#define _ISOC11_SOURCE //For aligned_alloc

#include <stdlib.h>
#include "snap_vq_adm.h"
#include "snap_vq_internal.h"
#include "snap_macros.h"
#include "snap_buf.h"
#include "snap_virtio_common_ctrl.h"

struct snap_vq_adm;

struct snap_vaq_cmd {
	struct snap_vq_adm *q;
	struct snap_vq_cmd common;
	struct snap_virtio_adm_cmd_layout *layout;
};

struct snap_vq_adm {
	struct snap_vaq_cmd *cmds;
	struct snap_virtio_adm_cmd_layout *cmd_layouts;
	struct snap_vq vq;
	snap_vq_process_fn_t adm_process_fn;
	struct ibv_pd *pd;
	uint16_t spec_version;
};

static struct snap_vaq_cmd *to_snap_vaq_cmd(struct snap_vq_cmd *cmd)
{
	return container_of(cmd, struct snap_vaq_cmd, common);
}

static struct snap_vq_adm *to_snap_vq_adm(struct snap_vq *vq)
{
	return container_of(vq, struct snap_vq_adm, vq);
}


static size_t snap_vaq_cmd_in_get_len_v1_2(struct snap_vaq_cmd *cmd)
{
	struct snap_virtio_adm_cmd_hdr_v1_2 hdr = cmd->layout->hdr.hdr_v1_2;
	size_t ret = 0;

	switch (hdr.cmd_class) {
	case SNAP_VQ_ADM_MIG_CTRL:
		switch (hdr.command) {
		case SNAP_VQ_ADM_MIG_GET_STATUS:
			ret = sizeof(struct snap_vq_adm_get_status_data);
			break;
		case SNAP_VQ_ADM_MIG_MODIFY_STATUS:
			ret = sizeof(struct snap_vq_adm_modify_status_data);
			break;
		case SNAP_VQ_ADM_MIG_GET_STATE_PENDING_BYTES:
			ret = sizeof(struct snap_vq_adm_get_pending_bytes_data);
			break;
		case SNAP_VQ_ADM_MIG_SAVE_STATE:
			ret = sizeof(struct snap_vq_adm_save_state_data);
			break;
		case SNAP_VQ_ADM_MIG_RESTORE_STATE:
			ret = sizeof(struct snap_vq_adm_restore_state_data);
			break;
		default:
			break;
		}
		break;
	case SNAP_VQ_ADM_DP_TRACK_CTRL:
		switch (hdr.command) {
		case SNAP_VQ_ADM_DP_START_TRACK:
			ret = sizeof(struct snap_vq_adm_dirty_page_track_start);
			break;
		case SNAP_VQ_ADM_DP_STOP_TRACK:
			ret = sizeof(struct snap_vq_adm_dirty_page_track_stop);
			break;
		case SNAP_VQ_ADM_DP_GET_MAP_PENDING_BYTES:
		case SNAP_VQ_ADM_DP_REPORT_MAP:
		default:
			break;
		}
		break;
	default:
		break;
	}

	return ret;
}

static size_t snap_vaq_cmd_in_get_len_v1_3(struct snap_vaq_cmd *cmd)
{
	struct snap_virtio_adm_cmd_hdr_v1_3 hdr = cmd->layout->hdr.hdr_v1_3;
	size_t ret = 0;

	switch (hdr.opcode) {
	case SNAP_VIRTIO_ADMIN_CMD_LIST_QUERY:
		switch (hdr.group_type) {
		case SNAP_VIRTIO_ADMIN_GROUP_TYPE_SRIOV:
		default:
			break;
		}
		break;
	case SNAP_VIRTIO_ADMIN_CMD_LIST_USE:
		switch (hdr.group_type) {
		case SNAP_VIRTIO_ADMIN_GROUP_TYPE_SRIOV:
			ret = sizeof(struct snap_virtio_admin_cmd_list);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return ret;
}

static size_t snap_vaq_cmd_in_get_len(struct snap_vaq_cmd *cmd)
{
	if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
		return snap_vaq_cmd_in_get_len_v1_3(cmd);
	else /* default VIRTIO_SPEC_VER_12 */
		return snap_vaq_cmd_in_get_len_v1_2(cmd);
}

static size_t snap_vaq_cmd_out_get_len_v1_2(struct snap_vaq_cmd *cmd)
{
	struct snap_virtio_adm_cmd_hdr_v1_2 hdr = cmd->layout->hdr.hdr_v1_2;
	size_t ret = 0;

	switch (hdr.cmd_class) {
	case SNAP_VQ_ADM_MIG_CTRL:
		switch (hdr.command) {
		case SNAP_VQ_ADM_MIG_GET_STATUS:
			ret = sizeof(struct snap_vq_adm_get_status_result);
			break;
		case SNAP_VQ_ADM_MIG_GET_STATE_PENDING_BYTES:
			ret = sizeof(struct snap_vq_adm_get_pending_bytes_result);
			break;
		default:
			break;
		}
		break;
	case SNAP_VQ_ADM_DP_TRACK_CTRL:
		switch (hdr.command) {
		case SNAP_VQ_ADM_DP_START_TRACK:
		case SNAP_VQ_ADM_DP_STOP_TRACK:
		case SNAP_VQ_ADM_DP_GET_MAP_PENDING_BYTES:
		case SNAP_VQ_ADM_DP_REPORT_MAP:
		default:
			break;
		}
		break;
	default:
		break;
	}
	return ret;
}

static size_t snap_vaq_cmd_out_get_len_v1_3(struct snap_vaq_cmd *cmd)
{
	struct snap_virtio_adm_cmd_hdr_v1_3 hdr = cmd->layout->hdr.hdr_v1_3;
	size_t ret = 0;

	switch (hdr.opcode) {
	case SNAP_VIRTIO_ADMIN_CMD_LIST_QUERY:
		switch (hdr.group_type) {
		case SNAP_VIRTIO_ADMIN_GROUP_TYPE_SRIOV:
			ret = sizeof(struct snap_virtio_admin_cmd_list);
			break;
		default:
			break;
		}
		break;
	case SNAP_VIRTIO_ADMIN_CMD_LIST_USE:
		switch (hdr.group_type) {
		case SNAP_VIRTIO_ADMIN_GROUP_TYPE_SRIOV:
		default:
			break;
		}
		break;
	default:
		break;
	}

	return ret;
}

static size_t snap_vaq_cmd_out_get_len(struct snap_vaq_cmd *cmd)
{
	if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
		return snap_vaq_cmd_out_get_len_v1_3(cmd);
	else
		return snap_vaq_cmd_out_get_len_v1_2(cmd);
}

size_t snap_vaq_cmd_get_total_len(struct snap_vq_cmd *cmd)
{
	size_t total_len = 0;
	struct snap_vq_cmd_desc *desc = NULL;

	TAILQ_FOREACH(desc, snap_vq_cmd_get_descs(cmd), entry) {
		total_len += desc->desc.len;
	}

	return total_len;
}

/**
 * snap_vaq_cmd_layout_data_read() - Read data from host memory.
 * @cmd: command context
 * @total_len: length to be read
 * @lbuf: local buffer to read data into
 * @lbuf_mkey: lkey to access local buffer
 * @done_fn: callback function to be called when finished.
 * @layout_offset: offset bytes from beginning of descs to start reading after
 *
 * The function asynchronously reads data from host memory into
 * a local buffer. When data is ready, done_fn() callback is called.
 *
 * Context: On virtio admin queue context, this function should be called
 *          by the command processor (typically virtio controller) to
 *          handle "restore state" API.
 *
 * Return: 0 on success, -errno otherwise.
 */
int snap_vaq_cmd_layout_data_read(struct snap_vq_cmd *cmd, size_t total_len,
				void *lbuf, uint32_t lbuf_mkey,
				snap_vq_cmd_done_cb_t done_fn,
				size_t layout_offset)
{
	struct snap_vq_cmd_desc *desc = NULL;

	/* Set desc to descriptor after offset bytes form start of descs */
	TAILQ_FOREACH(desc, snap_vq_cmd_get_descs(cmd), entry) {
		if (desc->desc.len > layout_offset)
			break;
		layout_offset -= desc->desc.len;
	}

	return snap_vq_cmd_descs_rw(cmd, desc, layout_offset, lbuf, total_len,
				lbuf_mkey, done_fn, false);
}

/**
 * snap_vaq_cmd_layout_data_write() - Write data to host memory.
 * @cmd: command context
 * @total_len: length to be read
 * @lbuf: local buffer to read data into
 * @lbuf_mkey: lkey to access local buffer
 * @done_fn: callback function to be called when finished.
 *
 * The function asynchronously writes data from local buffer
 * into host memory. When finished, done_fn() callback is called.
 *
 * Context: On virtio admin queue context, this function should be called
 *          by the command processor (typically virtio controller) to
 *          handle "save state" API.
 *
 * Return: 0 on success, -errno otherwise.
 */
int snap_vaq_cmd_layout_data_write(struct snap_vq_cmd *cmd, size_t total_len,
				void *lbuf, uint32_t lbuf_mkey,
				snap_vq_cmd_done_cb_t done_fn)
{
	struct snap_vq_cmd_desc *desc;

	/* Set desc to be first descriptor we can write to */
	TAILQ_FOREACH(desc, snap_vq_cmd_get_descs(cmd), entry) {
		if ((desc->desc.flags & VRING_DESC_F_WRITE))
			break;
	}
	/*
	 * Note first_offset is 0 because we are writing to the first writable desc
	 * and we dont need to write out more than once for any command.
	 */
	return snap_vq_cmd_descs_rw(cmd, desc, 0, lbuf, total_len, lbuf_mkey,
				 done_fn, true);
}


static inline
int snap_vaq_cmd_wb_cmd_out(struct snap_vaq_cmd *cmd)
{
	int out_len, ret, len;
	struct snap_vq_cmd_desc *desc;
	char *laddr;

	out_len = snap_vaq_cmd_out_get_len(cmd);
	if (!out_len)
		return 0;

	/* Use first desc with write flag */
	TAILQ_FOREACH(desc, &cmd->common.descs, entry) {
		if ((desc->desc.flags & VRING_DESC_F_WRITE))
			break;
	}

	if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
		desc = TAILQ_NEXT(desc, entry);

	laddr = (char *)&cmd->layout->out;
	while (out_len > 0 && desc) {
		len = snap_min(out_len, desc->desc.len);
		ret = snap_dma_q_write_short(cmd->common.vq->dma_q,
					    laddr, len, desc->desc.addr,
					    cmd->common.vq->xmkey);
		if (snap_unlikely(ret))
			return ret;

		cmd->common.len += len;
		laddr += len;
		out_len -= len;
		desc = TAILQ_NEXT(desc, entry);
	}

	return out_len;
}

static int snap_vaq_cmd_wb_ftr_v1_2(struct snap_vaq_cmd *cmd)
{
	const struct snap_vq_cmd_desc *last;
	int ret;
	struct snap_virtio_adm_cmd_ftr_v1_2 *ftr = &cmd->layout->ftr.ftr_v1_2;
	uint64_t raddr;

	/*
	 * Footer is in a single desc since it is 1 byte in size.
	 * The last byte in the last desc is the footer
	 */
	last = TAILQ_LAST(&cmd->common.descs, snap_vq_cmd_desc_list);
	raddr = last->desc.addr + last->desc.len - sizeof(*ftr);
	ret = snap_dma_q_write_short(cmd->common.vq->dma_q, ftr,
				sizeof(*ftr), raddr,
				cmd->common.vq->xmkey);
	if (snap_unlikely(ret))
		return ret;

	cmd->common.len += sizeof(*ftr);
	return 0;
}

static int snap_vaq_cmd_wb_ftr_v1_3(struct snap_vaq_cmd *cmd)
{
	struct snap_virtio_adm_cmd_ftr_v1_3 *ftr = &cmd->layout->ftr.ftr_v1_3;
	const struct snap_vq_cmd_desc *desc;
	uint64_t raddr;
	int ret;

	TAILQ_FOREACH(desc, &cmd->common.descs, entry) {
		if ((desc->desc.flags & VRING_DESC_F_WRITE))
			break;
	}

	raddr = desc->desc.addr + desc->desc.len - sizeof(*ftr);
	ret = snap_dma_q_write_short(cmd->common.vq->dma_q, ftr,
				sizeof(*ftr), raddr,
				cmd->common.vq->xmkey);
	if (snap_unlikely(ret))
		return ret;

	cmd->common.len += sizeof(*ftr);
	return 0;
}

static int snap_vaq_cmd_wb_ftr(struct snap_vaq_cmd *cmd)
{
	if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
		return snap_vaq_cmd_wb_ftr_v1_3(cmd);
	else
		return snap_vaq_cmd_wb_ftr_v1_2(cmd);
}

static int snap_vaq_cmd_read_hdr(struct snap_vaq_cmd *cmd,
				snap_vq_cmd_done_cb_t done_fn)
{
	struct snap_vq_cmd_desc *desc;
	size_t total_len;

	desc = TAILQ_FIRST(&cmd->common.descs);

	if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
		total_len = sizeof(struct snap_virtio_adm_cmd_hdr_v1_3);
	else
		total_len = sizeof(struct snap_virtio_adm_cmd_hdr_v1_2);

	return snap_vq_cmd_descs_rw(&cmd->common, desc, 0, &cmd->layout->hdr,
				    total_len,
				    snap_buf_get_mkey(cmd->q->cmd_layouts),
				    done_fn, false);
}

static int snap_vaq_cmd_read_cmd_in(struct snap_vaq_cmd *cmd,
					snap_vq_cmd_done_cb_t done_fn)
{
	size_t offset = 0, in_len;

	if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
		offset = sizeof(struct snap_virtio_adm_cmd_hdr_v1_3);
	else
		offset = sizeof(struct snap_virtio_adm_cmd_hdr_v1_2);

	in_len = snap_vaq_cmd_in_get_len(cmd);
	if (!in_len) {
		done_fn(&cmd->common, IBV_WC_SUCCESS);
		return 0;
	}

	return snap_vaq_cmd_layout_data_read(&cmd->common, in_len, &cmd->layout->in,
				  snap_buf_get_mkey(cmd->q->cmd_layouts),
				   done_fn, offset);
}

static void snap_vaq_cmd_create(struct snap_vq_adm *q,
				struct snap_vaq_cmd *cmd,
				struct snap_virtio_adm_cmd_layout *layout)
{
	cmd->q = q;
	cmd->layout = layout;
	snap_vq_cmd_create(&q->vq, &cmd->common);
}

static void snap_vaq_cmd_destroy(struct snap_vaq_cmd *cmd)
{
	snap_vq_cmd_destroy(&cmd->common);
}

static struct snap_vq_cmd *snap_vq_adm_create_cmd(struct snap_vq *vq, int index)
{
	struct snap_vq_adm *q = to_snap_vq_adm(vq);

	snap_vaq_cmd_create(q, &q->cmds[index], &q->cmd_layouts[index]);
	return &q->cmds[index].common;
}

static void snap_vq_adm_read_cmd_in_done(struct snap_vq_cmd *vcmd,
					enum ibv_wc_status status)
{
	struct snap_vaq_cmd *cmd = to_snap_vaq_cmd(vcmd);
	enum snap_virtio_adm_status adm_status;

	if (snap_unlikely(status != IBV_WC_SUCCESS)) {
		if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
			adm_status = SNAP_VIRTIO_ADMIN_STATUS_EAGAIN;
		else
			adm_status = SNAP_VIRTIO_ADM_STATUS_ERR;

		return snap_vaq_cmd_complete_v1_3(vcmd, adm_status,
						  SNAP_VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
	}
	/*
	 * Admin commands further processing should be done by the caller
	 * (AKA virtio controller).
	 */
	cmd->q->adm_process_fn(vcmd->vq->vctrl, vcmd);
}

static void snap_vq_adm_read_hdr_done(struct snap_vq_cmd *vcmd,
					enum ibv_wc_status status)
{
	struct snap_vaq_cmd *cmd = to_snap_vaq_cmd(vcmd);
	enum snap_virtio_adm_status adm_status;
	int ret;

	if (snap_unlikely(status != IBV_WC_SUCCESS)) {
		if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
			adm_status = SNAP_VIRTIO_ADMIN_STATUS_EAGAIN;
		else
			adm_status = SNAP_VIRTIO_ADM_STATUS_ERR;
		return snap_vaq_cmd_complete_v1_3(vcmd, adm_status,
						  SNAP_VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
	}

	ret = snap_vaq_cmd_read_cmd_in(cmd, snap_vq_adm_read_cmd_in_done);
	if (ret) {
		if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
			adm_status = SNAP_VIRTIO_ADMIN_STATUS_EAGAIN;
		else
			adm_status = SNAP_VIRTIO_ADM_STATUS_ERR;
		return snap_vaq_cmd_complete_v1_3(vcmd, adm_status,
						  SNAP_VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
	}
}

static void snap_vq_adm_handle_cmd(struct snap_vq_cmd *vcmd)
{
	struct snap_vaq_cmd *cmd = to_snap_vaq_cmd(vcmd);
	enum snap_virtio_adm_status adm_status;
	int ret;

	ret = snap_vaq_cmd_read_hdr(cmd, snap_vq_adm_read_hdr_done);
	if (ret) {
		if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3)
			adm_status = SNAP_VIRTIO_ADMIN_STATUS_EAGAIN;
		else
			adm_status = SNAP_VIRTIO_ADM_STATUS_ERR;

		return snap_vaq_cmd_complete_v1_3(vcmd, adm_status,
						  SNAP_VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
	}
}

static void snap_vq_adm_delete_cmd(struct snap_vq_cmd *vcmd)
{
	struct snap_vaq_cmd *cmd = to_snap_vaq_cmd(vcmd);

	snap_vaq_cmd_destroy(cmd);
}

static struct snap_vq_cmd_ops snap_vq_adm_cmd_ops = {
	.create = snap_vq_adm_create_cmd,
	.handle = snap_vq_adm_handle_cmd,
	.delete = snap_vq_adm_delete_cmd,
	.prefetch = NULL,
};

/**
 * snap_vq_adm_create() - Create new virtio admin queue instance
 * @attr: creation attributes.
 *
 * Creates a new instance of virtio admin queue.
 *
 * Return: virtqueue instance upon success, NULL otherwise.
 */
struct snap_vq *snap_vq_adm_create(struct snap_vq_adm_create_attr *attr)
{
	struct snap_vq_adm *q;
	struct snap_virtio_ctrl *vctrl = attr->common.vctrl;
	const size_t cmd_arr_sz = attr->common.size * sizeof(*q->cmds);

	q = calloc(1, sizeof(*q));
	if (!q)
		goto err;

	q->spec_version = attr->adm_spec_version;
	vctrl->spec_version = attr->adm_spec_version;

	q->cmds = aligned_alloc(SNAP_DCACHE_LINE, cmd_arr_sz);
	if (!q->cmds)
		goto free_q;
	memset(q->cmds, 0, cmd_arr_sz);

	q->cmd_layouts = snap_buf_alloc(attr->common.pd,
				attr->common.size * sizeof(*q->cmd_layouts));
	if (!q->cmd_layouts)
		goto free_cmds;

	if (q->spec_version == VIRTIO_SPEC_VER_1_3) {
		snap_vq_adm_cmd_ops.hdr_size =
			sizeof(struct snap_virtio_adm_cmd_hdr_v1_3);
		snap_vq_adm_cmd_ops.ftr_size =
			snap_max(sizeof(union snap_virtio_adm_cmd_out),
				 sizeof(struct snap_virtio_adm_cmd_ftr_v1_3));
	} else {
		snap_vq_adm_cmd_ops.hdr_size =
			sizeof(struct snap_virtio_adm_cmd_hdr_v1_2);
		snap_vq_adm_cmd_ops.ftr_size =
			snap_max(sizeof(union snap_virtio_adm_cmd_out),
				 sizeof(struct snap_virtio_adm_cmd_ftr_v1_2));
	}

	if (snap_vq_create(&q->vq, &attr->common,
					&snap_vq_adm_cmd_ops))
		goto free_layouts;

	q->pd = attr->common.pd;
	q->adm_process_fn = attr->adm_process_fn;
	return &q->vq;

free_layouts:
	snap_buf_free(q->cmd_layouts);
free_cmds:
	free(q->cmds);
free_q:
	free(q);
err:
	return NULL;
}

/**
 * snap_vq_adm_destroy() - Destroy a virtio admin queue instance.
 * @q: queue to destroy
 *
 * Destroys a previously created admin virtqueue instance
 */
void snap_vq_adm_destroy(struct snap_vq *vq)
{
	struct snap_vq_adm *q = to_snap_vq_adm(vq);

	snap_vq_destroy(vq);
	snap_buf_free(q->cmd_layouts);
	free(q->cmds);
	free(q);
}

static void snap_vaq_cmd_complete_int(struct snap_vq_cmd *vcmd,
				enum snap_virtio_adm_status status, bool dnr)
{
	int ret;
	struct snap_vaq_cmd *cmd = to_snap_vaq_cmd(vcmd);
	struct snap_virtio_adm_cmd_ftr_v1_2 *ftr = &cmd->layout->ftr.ftr_v1_2;

	if (status == SNAP_VIRTIO_ADM_STATUS_OK) {
		ret = snap_vaq_cmd_wb_cmd_out(cmd);
		if (snap_unlikely(ret))
			status = SNAP_VIRTIO_ADM_STATUS_ERR | SNAP_VIRTIO_ADM_STATUS_DNR;
	} else if (dnr) {
		status |= SNAP_VIRTIO_ADM_STATUS_DNR;
	}

	ftr->status = status;
	ret = snap_vaq_cmd_wb_ftr(cmd);
	if (snap_unlikely(ret)) {
		snap_vq_cmd_fatal(vcmd);
		return;
	}
	snap_vq_cmd_complete(vcmd);
}

/**
 * snap_vaq_cmd_complete() - complete virtio admin command
 * @cmd: Command to complete
 * @status: completion error code
 *
 * Complete virtio admin command. The function writes back to host memory
 * the response data and footer according to virtio admin command layout.
 * command processing stage to get the layout according to virtio spec.
 *
 * If status is not OK, the DNR bit will be set
 *
 * Context: After calling this function, command cannot be further processed,
 *          as command's struct may already be reused.
 */
void snap_vaq_cmd_complete(struct snap_vq_cmd *vcmd,
				enum snap_virtio_adm_status status)
{
	snap_vaq_cmd_complete_int(vcmd, status, true);
}

/**
 * snap_vaq_cmd_complete_no_dnr() - complete virtio admin command
 * @cmd: Command to complete
 * @status: completion error code
 *
 * Complete virtio admin command. The function writes back to host memory
 * the response data and footer according to virtio admin command layout.
 * command processing stage to get the layout according to virtio spec.
 *
 * If status is not OK, the DNR bit will NOT be set
 *
 * Context: After calling this function, command cannot be further processed,
 *          as command's struct may already be reused.
 */
void snap_vaq_cmd_complete_no_dnr(struct snap_vq_cmd *vcmd,
				enum snap_virtio_adm_status status)
{
	snap_vaq_cmd_complete_int(vcmd, status, false);
}

static void snap_vaq_cmd_compl_ftr(struct snap_vq_cmd *vcmd,
			enum snap_virtio_adm_status status,
			enum snap_virtio_adm_status_qualifier status_qualifier,
			bool dnr)
{
	int ret;
	struct snap_vaq_cmd *cmd = to_snap_vaq_cmd(vcmd);

	if (cmd->q->spec_version == VIRTIO_SPEC_VER_1_3) {
		cmd->layout->ftr.ftr_v1_3.status = status;
		cmd->layout->ftr.ftr_v1_3.status_qualifier = status_qualifier;
		ret = snap_vaq_cmd_wb_ftr(cmd);
		if (snap_unlikely(ret)) {
			snap_vq_cmd_fatal(vcmd);
			return;
		}

		if (status == SNAP_VIRTIO_ADM_STATUS_OK) {
			ret = snap_vaq_cmd_wb_cmd_out(cmd);
			if (snap_unlikely(ret))
				status = SNAP_VIRTIO_ADM_STATUS_ERR | SNAP_VIRTIO_ADM_STATUS_DNR;
		} else if (dnr)
			status |= SNAP_VIRTIO_ADM_STATUS_DNR;

		snap_vq_cmd_complete(vcmd);

	} else {
		if (status == SNAP_VIRTIO_ADM_STATUS_OK) {
			ret = snap_vaq_cmd_wb_cmd_out(cmd);
			if (snap_unlikely(ret))
				status = SNAP_VIRTIO_ADM_STATUS_ERR | SNAP_VIRTIO_ADM_STATUS_DNR;
		} else if (dnr) {
			status |= SNAP_VIRTIO_ADM_STATUS_DNR;
		}

		cmd->layout->ftr.ftr_v1_2.status = status;
		ret = snap_vaq_cmd_wb_ftr(cmd);
		if (snap_unlikely(ret)) {
			snap_vq_cmd_fatal(vcmd);
			return;
		}
		snap_vq_cmd_complete(vcmd);
	}
}

/**
 * snap_vaq_cmd_complete_v1_3() - complete virtio admin command,
 *				  compatible with virtio spec v1.2 and v1.3
 * @cmd: Command to complete
 * @status: completion error code
 * @status_qualifier: completion error reason
 *
 * Complete virtio admin command. The function writes back to host memory
 * the response data and footer according to virtio admin command layout.
 * command processing stage to get the layout according to virtio spec.
 *
 * If status is not OK, the DNR bit will be set
 *
 * Context: After calling this function, command cannot be further processed,
 *          as command's struct may already be reused.
 */
void snap_vaq_cmd_complete_v1_3(struct snap_vq_cmd *vcmd,
				enum snap_virtio_adm_status status,
				enum snap_virtio_adm_status_qualifier status_qualifier)
{
	snap_vaq_cmd_compl_ftr(vcmd, status, status_qualifier, true);
}

/**
 * snap_vaq_cmd_complete_no_dnr_v1_3() - complete virtio admin command
 *					 compatible with virtio spec v1.2 and v1.3
 * @cmd: Command to complete
 * @status: completion error code
 * @status_qualifier: completion error reason
 *
 * Complete virtio admin command. The function writes back to host memory
 * the response data and footer according to virtio admin command layout.
 * command processing stage to get the layout according to virtio spec.
 *
 * If status is not OK, the DNR bit will NOT be set
 *
 * Context: After calling this function, command cannot be further processed,
 *          as command's struct may already be reused.
 */
void snap_vaq_cmd_complete_no_dnr_v1_3(struct snap_vq_cmd *vcmd,
				       enum snap_virtio_adm_status status,
				       enum snap_virtio_adm_status_qualifier status_qualifier)
{
	snap_vaq_cmd_compl_ftr(vcmd, status, status_qualifier, false);
}

void **snap_vaq_cmd_priv(struct snap_vq_cmd *cmd)
{
	return &cmd->priv;
}

/**
 * snap_vaq_cmd_layout_get() - Get snap virtio admin command's layout
 * @cmd: command to get layout from
 *
 * Get snap virtio command's layout. Should be called during
 * command processing stage to get the layout according to virtio spec.
 *
 * Return: admin command's layout.
 */
inline struct snap_virtio_adm_cmd_layout *
snap_vaq_cmd_layout_get(struct snap_vq_cmd *vcmd)
{
	struct snap_vaq_cmd *cmd = to_snap_vaq_cmd(vcmd);

	return cmd->layout;
}

/**
 * snap_vaq_cmd_ctrl_get() - Get snap virtio admin command's ctrl
 * @cmd: command to get ctrl from
 *
 * Get snap virtio command's ctrl. Should be called during
 * command processing stage to get the ctrl.
 *
 * Return: admin command vq's ctrl .
 */
inline struct snap_virtio_ctrl *
snap_vaq_cmd_ctrl_get(struct snap_vq_cmd *vcmd)
{
	return vcmd->vq->vctrl;
}

/**
 * snap_vaq_cmd_dmaq_get() - Get snap virtio admin command's dma_q
 * @cmd: command to get ctrl from
 *
 * Get snap virtio command's dma_q. Should be called during
 * command processing stage to get the dma_q to process the completions.
 *
 * Return: admin command vq's dma_q.
 */
inline struct snap_dma_q *
snap_vaq_cmd_dmaq_get(struct snap_vq_cmd *cmd)
{
	return cmd->vq->dma_q;
}

int snap_vq_adm_get_debugstat(struct snap_vq *vq,
			  struct snap_virtio_queue_debugstat *q_debugstat)
{
	return snap_vq_get_debugstat(vq, q_debugstat);
}
