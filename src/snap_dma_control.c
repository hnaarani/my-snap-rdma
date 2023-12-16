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
#include <arpa/inet.h>

#include "snap_dma_internal.h"
#include "snap_env.h"
#include "mlx5_ifc.h"
#include "snap_umr.h"
#include "snap_dpa.h"

#include "config.h"

SNAP_ENV_REG_ENV_VARIABLE(SNAP_DMA_Q_OPMODE, 0);
SNAP_ENV_REG_ENV_VARIABLE(SNAP_DMA_Q_IOV_SUPP, 0);
SNAP_ENV_REG_ENV_VARIABLE(SNAP_DMA_Q_CRYPTO_SUPP, 0);
SNAP_ENV_REG_ENV_VARIABLE(SNAP_DMA_Q_DBMODE, SNAP_DB_RING_BATCH);

struct snap_roce_caps {
	bool resources_on_nvme_emulation_manager;
	bool roce_enabled;
	uint8_t roce_version;
	bool fl_when_roce_disabled;
	bool fl_when_roce_enabled;
	uint16_t r_roce_max_src_udp_port;
	uint16_t r_roce_min_src_udp_port;
};

static int fill_roce_caps(struct ibv_context *context,
			  struct snap_roce_caps *roce_caps)
{

	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret)
		return ret;

	roce_caps->resources_on_nvme_emulation_manager =
		 DEVX_GET(query_hca_cap_out, out,
		 capability.cmd_hca_cap.resources_on_nvme_emulation_manager);
	roce_caps->fl_when_roce_disabled = DEVX_GET(query_hca_cap_out, out,
		 capability.cmd_hca_cap.fl_rc_qp_when_roce_disabled);
	roce_caps->roce_enabled = DEVX_GET(query_hca_cap_out, out,
						capability.cmd_hca_cap.roce);
	if (!roce_caps->roce_enabled)
		goto out;

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_SET_HCA_CAP_OP_MOD_ROCE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret)
		return ret;

	roce_caps->roce_version = DEVX_GET(query_hca_cap_out, out,
					   capability.roce_cap.roce_version);
	roce_caps->fl_when_roce_enabled = DEVX_GET(query_hca_cap_out,
			out, capability.roce_cap.fl_rc_qp_when_roce_enabled);
	roce_caps->r_roce_max_src_udp_port = DEVX_GET(query_hca_cap_out,
			out, capability.roce_cap.r_roce_max_src_udp_port);
	roce_caps->r_roce_min_src_udp_port = DEVX_GET(query_hca_cap_out,
			out, capability.roce_cap.r_roce_min_src_udp_port);
out:
	SNAP_LIB_LOG_DBG("RoCE Caps: enabled %d ver %d fl allowed %d",
		   roce_caps->roce_enabled, roce_caps->roce_version,
		   roce_caps->roce_enabled ? roce_caps->fl_when_roce_enabled :
		   roce_caps->fl_when_roce_disabled);
	return 0;
}

static int check_port(struct ibv_context *ctx, int port_num, bool *roce_en,
		      bool *ib_en, enum ibv_mtu *mtu)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_nic_vport_context_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_nic_vport_context_out)] = {0};
	uint8_t devx_v;
	struct ibv_port_attr port_attr = {};
	int ret;

	*roce_en = false;
	*ib_en = false;

	ret = ibv_query_port(ctx, port_num, &port_attr);
	if (ret)
		return ret;

	if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
		/* we only support local IB addressing for now */
		if (port_attr.flags & IBV_QPF_GRH_REQUIRED) {
			SNAP_LIB_LOG_ERR("IB enabled and GRH addressing is required but only local addressing is supported");
			return -1;
		}
		*mtu = port_attr.active_mtu;
		*ib_en = true;
		return 0;
	}

	if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET)
		return -1;

	/* port may be ethernet but still have roce disabled */
	DEVX_SET(query_nic_vport_context_in, in, opcode,
		 MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
	ret = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out,
				      sizeof(out));
	if (ret) {
		SNAP_LIB_LOG_ERR("Failed to get VPORT context - assuming ROCE is disabled");
		return ret;
	}
	devx_v = DEVX_GET(query_nic_vport_context_out, out,
			  nic_vport_context.roce_en);
	if (devx_v)
		*roce_en = true;

	/* When active mtu is invalid, default to 1K MTU. */
	*mtu = port_attr.active_mtu ? port_attr.active_mtu : IBV_MTU_1024;
	return 0;
}

static void snap_destroy_qp_helper(struct snap_dma_ibv_qp *qp, bool destroy_cqs)
{
	if (!snap_qp_on_dpa(qp->qp)) {
		if (qp->dv_qp.comps)
			free(qp->dv_qp.comps);

		if (qp->dv_qp.opaque_buf) {
			ibv_dereg_mr(qp->dv_qp.opaque_mr);
			free(qp->dv_qp.opaque_buf);
		}
	} else {
		snap_dpa_mkey_free(qp->dpa.mkey);
	}

	snap_qp_destroy(qp->qp);
	if (destroy_cqs) {
		if (qp->rx_cq)
			snap_cq_destroy(qp->rx_cq);
		if (qp->tx_cq)
			snap_cq_destroy(qp->tx_cq);
	}
}

static void snap_free_rx_wqes(struct snap_dma_ibv_qp *qp)
{
	if (!qp->rx_buf)
		return;
	ibv_dereg_mr(qp->rx_mr);
	free(qp->rx_buf);
}

static int snap_alloc_rx_wqes(struct ibv_pd *pd, struct snap_dma_ibv_qp *qp, size_t rx_qsize,
		size_t rx_elem_size)
{
	int rc;

	if (rx_qsize == 0) {
		qp->rx_buf = 0;
		return 0;
	}

	rc = posix_memalign((void **)&qp->rx_buf, SNAP_DMA_RX_BUF_ALIGN,
			rx_qsize * rx_elem_size);
	if (rc)
		return rc;

	qp->rx_mr = ibv_reg_mr(pd, qp->rx_buf, rx_qsize * rx_elem_size,
			IBV_ACCESS_LOCAL_WRITE);
	if (!qp->rx_mr) {
		free(qp->rx_buf);
		return -ENOMEM;
	}

	return 0;
}

static int dummy_progress(struct snap_dma_q *q)
{
	return 0;
}
static int dummy_progress_tx(struct snap_dma_q *q, int max_tx_comp)
{
	return 0;
}

/* NOTE: we cannot take mode from the dma_q_attr because it can be 'autoselect'
 * and then it is replaced by the real mode in dma_q->ops
 *
 * This is done to keep creation attribute 'const'
 */
static int snap_create_qp_helper(struct ibv_pd *pd, const struct snap_dma_q_create_attr *dma_q_attr,
		struct snap_qp_attr *qp_init_attr, struct snap_dma_ibv_qp *qp, int mode, bool use_devx)
{
	struct snap_cq_attr cq_attr = {
		.cq_context = dma_q_attr->comp_context,
		.comp_channel = dma_q_attr->comp_channel,
		.comp_vector = dma_q_attr->comp_vector,
		.cqe_cnt = qp_init_attr->sq_size,
		.cqe_size = SNAP_DMA_Q_TX_CQE_SIZE
	};
	int rc;

	qp->mode = mode;

	/* TODO: add attribute to choose how snap_qp/cq are created */
	if (mode == SNAP_DMA_Q_MODE_VERBS)
		cq_attr.cq_type = SNAP_OBJ_VERBS;
	else
		cq_attr.cq_type = use_devx ? SNAP_OBJ_DEVX : SNAP_OBJ_DV;

	switch (dma_q_attr->dpa_mode) {
	case SNAP_DMA_Q_DPA_MODE_POLLING:
		qp_init_attr->qp_on_dpa = true;
		qp_init_attr->dpa_proc = dma_q_attr->dpa_proc;

		cq_attr.cq_type = SNAP_OBJ_DEVX;
		cq_attr.cq_on_dpa = true;
		cq_attr.dpa_element_type = MLX5_APU_ELEMENT_TYPE_EQ;
		cq_attr.dpa_proc = dma_q_attr->dpa_proc;
		break;

	case SNAP_DMA_Q_DPA_MODE_EVENT:
		qp_init_attr->qp_on_dpa = true;
		qp_init_attr->dpa_proc = snap_dpa_thread_proc(dma_q_attr->dpa_thread);

		cq_attr.cq_type = SNAP_OBJ_DEVX;
		cq_attr.cq_on_dpa = true;
		cq_attr.dpa_element_type = MLX5_APU_ELEMENT_TYPE_THREAD;
		cq_attr.dpa_thread = dma_q_attr->dpa_thread;
		break;

	case SNAP_DMA_Q_DPA_MODE_TRIGGER:
		qp_init_attr->qp_on_dpa = false;

		cq_attr.cq_type = SNAP_OBJ_DEVX;
		cq_attr.cq_on_dpa = true;
		cq_attr.dpa_element_type = MLX5_APU_ELEMENT_TYPE_THREAD;
		cq_attr.dpa_thread = dma_q_attr->dpa_thread;
		break;

	case SNAP_DMA_Q_DPA_MODE_MSIX_TRIGGER:
		qp_init_attr->qp_on_dpa = false;

		cq_attr.cq_type = SNAP_OBJ_DEVX;
		cq_attr.cq_on_dpa = true;
		cq_attr.dpa_element_type = MLX5_APU_ELEMENT_TYPE_EMULATED_DEV_EQ;
		cq_attr.dpa_proc = dma_q_attr->dpa_proc;
		cq_attr.use_eqn = true;
		cq_attr.eqn = dma_q_attr->emu_dev_eqn;
		break;

	case SNAP_DMA_Q_DPA_MODE_NONE:
		if (dma_q_attr->use_emu_dev_eqn) {
			cq_attr.use_eqn = true;
			cq_attr.eqn = dma_q_attr->emu_dev_eqn;
		}
		break;
	default:
		SNAP_LIB_LOG_ERR("unsupported dpa mode %d", dma_q_attr->dpa_mode);
		return -EINVAL;
	}

	if (qp_init_attr->sq_size) {
		qp->tx_cq = snap_cq_create(pd->context, &cq_attr);
		if (!qp->tx_cq)
			return -EINVAL;
	} else
		qp->tx_cq = NULL;

	if (qp_init_attr->rq_size && !qp_init_attr->rq_cq) {
		cq_attr.cqe_cnt = qp_init_attr->rq_size;
		/* Use 128 bytes cqes in order to allow scatter to cqe on receive
		 * This is relevant for NVMe sqe and for virtio queues when number of
		 * tunneled descr is less then three.
		 */
		cq_attr.cqe_size = SNAP_DMA_Q_RX_CQE_SIZE;
		qp->rx_cq = snap_cq_create(pd->context, &cq_attr);
		if (!qp->rx_cq)
			goto free_tx_cq;
	} else if (qp_init_attr->rq_size && qp_init_attr->rq_cq) {
		qp->rx_cq = qp_init_attr->rq_cq;
	} else
		qp->rx_cq = NULL;

	qp_init_attr->qp_type = cq_attr.cq_type;
	qp_init_attr->sq_cq = qp->tx_cq;
	qp_init_attr->rq_cq = qp->rx_cq;

	qp->qp = snap_qp_create(pd, qp_init_attr);
	if (!qp->qp)
		goto free_rx_cq;

	rc = snap_qp_to_hw_qp(qp->qp, &qp->dv_qp.hw_qp);
	if (rc)
		goto free_qp;

	if (mode == SNAP_DMA_Q_MODE_VERBS)
		return 0;

	if (qp->tx_cq) {
		rc = snap_cq_to_hw_cq(qp->tx_cq, &qp->dv_tx_cq);
		if (rc)
			goto free_qp;
	}

	if (qp->rx_cq) {
		rc = snap_cq_to_hw_cq(qp->rx_cq, &qp->dv_rx_cq);
		if (rc)
			goto free_qp;
	}

	if (!qp_init_attr->qp_on_dpa) {
		rc = posix_memalign((void **)&qp->dv_qp.comps, SNAP_DMA_BUF_ALIGN,
				qp->dv_qp.hw_qp.sq.wqe_cnt * sizeof(struct snap_dv_dma_completion));
		if (rc)
			goto free_qp;

		memset(qp->dv_qp.comps, 0,
				qp->dv_qp.hw_qp.sq.wqe_cnt * sizeof(struct snap_dv_dma_completion));
	} else {
		qp->dpa.mkey = snap_dpa_mkey_alloc(qp_init_attr->dpa_proc, pd);
		if (!qp->dpa.mkey)
			goto free_qp;
		qp->dv_qp.dpa_mkey = snap_dpa_mkey_id(qp->dpa.mkey);
	}

	if (!qp->dv_qp.hw_qp.sq.tx_db_nc) {
#if defined(__aarch64__)
		SNAP_LIB_LOG_ERR("DB record must be in the non-cacheable memory on BF");
		goto free_comps;
#else
		SNAP_LIB_LOG_WARN("DB record is not in the non-cacheable memory. Performance may be reduced\n"
			  "Try setting MLX5_SHUT_UP_BF environment variable");
#endif
	}

	if (mode == SNAP_DMA_Q_MODE_DV)
		return 0;

	/* TODO: gga on dpa */
	rc = posix_memalign((void **)&qp->dv_qp.opaque_buf,
			    sizeof(struct mlx5_dma_opaque),
			    sizeof(struct mlx5_dma_opaque));
	if (rc)
		goto free_comps;

	qp->dv_qp.opaque_mr = ibv_reg_mr(pd, qp->dv_qp.opaque_buf,
					 sizeof(struct mlx5_dma_opaque),
					 IBV_ACCESS_LOCAL_WRITE);
	if (!qp->dv_qp.opaque_mr)
		goto free_opaque;

	qp->dv_qp.opaque_lkey = htobe32(qp->dv_qp.opaque_mr->lkey);
	return 0;

free_opaque:
	free(qp->dv_qp.opaque_buf);
free_comps:
	if (!qp_init_attr->qp_on_dpa)
		free(qp->dv_qp.comps);
	else
		snap_dpa_mkey_free(qp->dpa.mkey);
free_qp:
	snap_qp_destroy(qp->qp);
free_rx_cq:
	if (qp->rx_cq)
		snap_cq_destroy(qp->rx_cq);
free_tx_cq:
	if (qp->tx_cq)
		snap_cq_destroy(qp->tx_cq);
	return -EINVAL;
}

static void snap_destroy_sw_qp(struct snap_dma_q *q)
{
	bool destroy_cqs = true;

	if (!snap_qp_on_dpa(q->sw_qp.qp))
		snap_free_rx_wqes(&q->sw_qp);

	if (q->custom_ops) {
		free(q->custom_ops);
		q->custom_ops = NULL;
	}

	if (q->worker)
		destroy_cqs = false;
	snap_destroy_qp_helper(&q->sw_qp, destroy_cqs);
}

static int clone_ops(struct snap_dma_q *q)
{
	struct snap_dma_q_ops *new_ops;

	if (q->custom_ops)
		return 0;

	new_ops = malloc(sizeof(*new_ops));
	if (!new_ops)
		return -1;

	memcpy(new_ops, q->ops, sizeof(*new_ops));
	q->custom_ops = new_ops;
	q->ops = q->custom_ops;

	return 0;
}

struct snap_mmo_caps_dma {
	bool qp_support;
	bool sq_support;
	uint8_t log_max_size : 5;
};

struct snap_mmo_caps_regexp {
	bool qp_support;
	bool sq_support;
	uint8_t log_sg_size : 5;
};

struct snap_mmo_caps_compress {
	bool qp_support;
	bool sq_support;
	uint8_t log_max_size : 5;
	uint8_t min_block_size : 4;
};

struct snap_mmo_caps_decompress {
	bool qp_support;
	bool sq_support;
	uint8_t log_max_size : 5;
	bool snappy;
	bool lz4_data_only;
	bool lz4_no_checksum;
	bool lz4_checksum;
};

/*
 * struct snap_mmo_caps - compression and HW acceleration capabilities
 * @dma: GGA engine support
 * @regexp: regexp support
 * @compress: compression support
 * @decompress: decompression support
 */
struct snap_mmo_caps {
	struct snap_mmo_caps_dma dma;
	struct snap_mmo_caps_regexp regexp;
	struct snap_mmo_caps_compress compress;
	struct snap_mmo_caps_decompress decompress;
};

static int query_mmo_caps(struct ibv_context *context,
				struct snap_mmo_caps *caps)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret)
		return ret;

	caps->dma.qp_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.dma_mmo_qp);
	caps->dma.sq_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.dma_mmo_sq);
	caps->dma.log_max_size = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.log_dma_mmo_max_size);

	caps->regexp.qp_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.regexp_mmo_qp);
	caps->regexp.sq_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.regexp_mmo_sq);
	caps->regexp.log_sg_size = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.log_regexp_scatter_gather_size);

	caps->compress.qp_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.compress_mmo_qp);
	caps->compress.sq_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.compress_mmo_sq);
	caps->compress.log_max_size = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.log_compress_max_size);
	caps->compress.min_block_size = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.compress_min_block_size);

	caps->decompress.qp_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.decompress_mmo_qp);
	caps->decompress.sq_support = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.decompress_mmo_sq);
	caps->decompress.log_max_size = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.log_decompress_max_size);
	caps->decompress.snappy = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.decompress_snappy);
	caps->decompress.lz4_data_only = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.decompress_lz4_data_only);
	caps->decompress.lz4_no_checksum = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.decompress_lz4_no_checksum);
	caps->decompress.lz4_checksum = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.decompress_lz4_checksum);

	return 0;
}

int snap_dma_q_post_recv(struct snap_dma_q *q)
{
	int i;
	int rc;
	struct ibv_recv_wr rx_wr, *bad_wr;
	struct ibv_sge rx_sge;

	if (snap_qp_on_dpa(q->sw_qp.qp))
		return 0;

	for (i = 0; i < SNAP_DMA_Q_POST_RECV_BUF_FACTOR * q->rx_qsize; i++) {
		if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS) {
			rx_sge.addr = (uint64_t)(q->sw_qp.rx_buf +
					i * q->rx_elem_size);
			rx_sge.length = q->rx_elem_size;
			rx_sge.lkey = q->sw_qp.rx_mr->lkey;

			rx_wr.wr_id = rx_sge.addr;
			rx_wr.next = NULL;
			rx_wr.sg_list = &rx_sge;
			rx_wr.num_sge = 1;

			rc = ibv_post_recv(snap_qp_to_verbs_qp(q->sw_qp.qp), &rx_wr, &bad_wr);
			if (rc)
				return rc;
		} else {
			snap_dv_post_recv(&q->sw_qp.dv_qp,
					  q->sw_qp.rx_buf + i * q->rx_elem_size,
					  q->rx_elem_size,
					  q->sw_qp.rx_mr->lkey);
			snap_dv_ring_rx_db(&q->sw_qp.dv_qp);
		}
	}

	return 0;
}

static int snap_dma_worker_queue_idx_get(struct snap_dma_worker *wk, struct snap_dma_q *q)
{
	int i;

	for (i = 0; i < wk->max_queues; i++)
		if (wk->queues[i] == q)
			return i;

	return -1;
}

static int snap_qp_attr_helper(struct snap_dma_q *q, struct ibv_pd *pd,
		const struct snap_dma_q_create_attr *attr, struct snap_qp_attr *qp_init_attr)
{
	struct snap_mmo_caps mmo_caps = {{0}};
	int rc;

	switch (attr->mode) {
	case SNAP_DMA_Q_MODE_AUTOSELECT:
		rc = query_mmo_caps(pd->context, &mmo_caps);
		if (rc)
			return rc;

		if (mmo_caps.dma.qp_support)
			q->ops = &gga_ops;
		else
			q->ops = &dv_ops;
		break;
	case SNAP_DMA_Q_MODE_VERBS:
		q->ops = &verb_ops;
		break;
	case SNAP_DMA_Q_MODE_DV:
		q->ops = &dv_ops;
		break;
	case SNAP_DMA_Q_MODE_GGA:
		q->ops = &gga_ops;
		break;
	default:
		SNAP_LIB_LOG_ERR("Invalid SNAP_DMA_Q_OPMODE %d", attr->mode);
		return -EINVAL;
	}

	/* our ops point to the global struct, we need to clone it before
	 * making a private change
	 *
	 * it is better to have a dummy progress than another if statement
	 */
	if (attr->rx_qsize == 0) {
		if (clone_ops(q))
			return -EINVAL;
		q->custom_ops->progress_rx = dummy_progress;
	}

	if (attr->tx_qsize == 0) {
		if (clone_ops(q))
			return -EINVAL;
		q->custom_ops->progress_tx = dummy_progress_tx;
	}

	SNAP_LIB_LOG_DBG("Opening dma_q of type %d dpa_mode %d", attr->mode, attr->dpa_mode);
	if (attr->dpa_mode != SNAP_DMA_Q_DPA_MODE_NONE) {
		if (attr->use_aliases || snap_dpa_enabled(pd->context)) {
			if (q->ops->mode != SNAP_DMA_Q_MODE_DV)
				return -EINVAL;
			q->no_events = true;
		} else
			return -ENOTSUP;
	}

	/*
	 * disable event mode if OBJ_DEVX are used to create qp and cq
	 * snap_dma_q_arm() will still work but event channel cannot be used
	 * to pick up events.
	 * At the moment this is only relevant for the unit tests
	 */
	if (attr->sw_use_devx)
		q->no_events = true;

	/* make sure that the completion is requested at least once */
	if (q->ops->mode != SNAP_DMA_Q_MODE_VERBS
			&& attr->tx_qsize <= SNAP_DMA_Q_TX_MOD_COUNT && attr->tx_qsize > 0)
		q->tx_qsize = SNAP_DMA_Q_TX_MOD_COUNT + 8;
	else
		q->tx_qsize = attr->tx_qsize;

	q->rx_elem_size = attr->rx_elem_size;
	q->tx_elem_size = attr->tx_elem_size;

	qp_init_attr->sq_size = q->tx_qsize;
	/* Need more space in rx queue in order to avoid memcpy() on rx data */
	qp_init_attr->rq_size = 2 * attr->rx_qsize;
	/* we must be able to send CQEs inline */
	qp_init_attr->sq_max_inline_size = attr->tx_elem_size;

	if (q->ops->mode == SNAP_DMA_Q_MODE_VERBS)
		qp_init_attr->sq_max_sge = SNAP_DMA_Q_MAX_SGE_NUM;
	else
		qp_init_attr->sq_max_sge = 1;
	qp_init_attr->rq_max_sge = 1;

	if (attr->wk) {
		qp_init_attr->rq_cq = attr->wk->rx_cq;
		qp_init_attr->sq_cq = attr->wk->tx_cq;
		/* TODO add worker support for DV, VERBS */
		qp_init_attr->qp_type = SNAP_OBJ_DEVX;
		qp_init_attr->uidx = snap_dma_worker_queue_idx_get(attr->wk, q);
		qp_init_attr->qp_on_dpa = false;
		q->no_events = true;
	}

	return 0;
}

static int snap_sw_qp_rx_wqe_helper(struct snap_dma_q *q, struct ibv_pd *pd,
								const struct snap_dma_q_create_attr *attr)
{
	int rc;
	bool destroy_cqs = true;

	if (q->ops->mode == SNAP_DMA_Q_MODE_DV || q->ops->mode == SNAP_DMA_Q_MODE_GGA) {
		q->sw_qp.dv_qp.db_flag = (enum snap_db_ring_flag)snap_env_getenv(SNAP_DMA_Q_DBMODE);
		q->tx_available = snap_dma_q_dv_get_tx_avail_max(q);
	} else
		/**
		 * in verbs mode every operation takes exactly one place in the queue
		 * wqe management is done by the rdma-core
		 */
		q->tx_available = q->tx_qsize;

	if (attr->dpa_mode)
		return 0;

	rc = snap_alloc_rx_wqes(pd, &q->sw_qp, 2 * attr->rx_qsize, attr->rx_elem_size);
	if (rc)
		goto free_qp;

	q->rx_qsize = attr->rx_qsize;

	return 0;

free_qp:
	if (q->worker)
		destroy_cqs = false;
	snap_destroy_qp_helper(&q->sw_qp, destroy_cqs);
	return rc;
}

static int snap_create_sw_qp(struct snap_dma_q *q, struct ibv_pd *pd,
		const struct snap_dma_q_create_attr *attr)
{
	struct snap_qp_attr qp_init_attr = {0};
	int rc;

	rc = snap_qp_attr_helper(q, pd, attr, &qp_init_attr);
	if (rc)
		return rc;
	rc = snap_create_qp_helper(pd, attr, &qp_init_attr, &q->sw_qp, q->ops->mode, attr->sw_use_devx);
	if (rc)
		return rc;

	return snap_sw_qp_rx_wqe_helper(q, pd, attr);
}

static void snap_destroy_fw_qp(struct snap_dma_q *q)
{
	if (q->fw_qp && q->fw_qp->fw_qp.qp)
		snap_destroy_qp_helper(&q->fw_qp->fw_qp, true);

	free(q->fw_qp);
}

/*
 * Since SNAP need to set isolate_vl_tc bit for fw_qp,
 * devx type qp is the only option to do it, this is why
 * change fw qp from verbs qp to devx qp. BUT, we have
 * already use fw qp as verbs qp in many other place,
 * in order to minimize the impact of this change, fake
 * a verbs type fw qp for it.
 **/
static void snap_fill_fw_verbs_qp(struct snap_dma_ibv_qp *devx_qp,
				struct ibv_qp *verbs_qp)
{
	verbs_qp->pd = devx_qp->qp->devx_qp.devx.pd;
	verbs_qp->context = devx_qp->qp->devx_qp.devx.pd->context;
	verbs_qp->qp_num = devx_qp->qp->devx_qp.devx.id;
	/* should also init verbs_qp->handle, but handle cannot get from devx_qp. */
}

static int snap_create_fw_qp(struct snap_dma_q *q, struct ibv_pd *pd,
			     const struct snap_dma_q_create_attr *attr)
{
	struct snap_qp_attr qp_init_attr = {};
	struct snap_dma_q_create_attr fw_dma_q_attr;
	int rc;

	q->fw_qp = calloc(1, sizeof(*q->fw_qp));
	if (!q->fw_qp) {
		SNAP_LIB_LOG_ERR("allocate fw_qp for dma_q failed.");
		return -ENOMEM;
	}

	/* refactor fw qp creation code */
	memcpy(&fw_dma_q_attr, attr, sizeof(*attr));
	if (attr->fw_use_devx)
		fw_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	else
		fw_dma_q_attr.mode = SNAP_DMA_Q_MODE_VERBS;
	fw_dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_NONE;
	fw_dma_q_attr.comp_channel = NULL;
	fw_dma_q_attr.comp_context = NULL;
	fw_dma_q_attr.comp_context = NULL;

	/* cannot create empty cq or a qp without one */
	qp_init_attr.sq_size = snap_max(attr->tx_qsize / 4, SNAP_DMA_FW_QP_MIN_SEND_WR);
	qp_init_attr.rq_size = attr->rx_qsize;
	/* give one sge so that we can post which is useful for testing */
	qp_init_attr.sq_max_sge = 1;

	/* the qp 'resources' are going to be replaced by the fw. */
	rc = snap_create_qp_helper(pd, &fw_dma_q_attr, &qp_init_attr, &q->fw_qp->fw_qp,
			fw_dma_q_attr.mode, attr->fw_use_devx);
	if (rc) {
		SNAP_LIB_LOG_ERR("create fw_qp failed, rc:%d", rc);
		goto free_fw_qp;
	}

	q->fw_qp->use_devx = attr->fw_use_devx;
	if (q->fw_qp->use_devx)
		snap_fill_fw_verbs_qp(&q->fw_qp->fw_qp, &q->fw_qp->fake_verbs_qp);

	return 0;

free_fw_qp:
	free(q->fw_qp);

	return rc;
}

static int snap_modify_lb_qp_init2init(struct snap_qp *qp)
{
	uint8_t in[DEVX_ST_SZ_BYTES(init2init_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(init2init_qp_out)] = {0};
	void *qpc_ext;
	int ret;

	DEVX_SET(init2init_qp_in, in, opcode, MLX5_CMD_OP_INIT2INIT_QP);
	DEVX_SET(init2init_qp_in, in, qpn, snap_qp_get_qpnum(qp));

	DEVX_SET(init2init_qp_in, in, opt_param_mask, 0);

	/* Set mmo parameter in qpc_ext */
	DEVX_SET(init2init_qp_in, in, qpc_ext, 1);
	DEVX_SET64(init2init_qp_in, in, opt_param_mask_95_32, 1ULL << 3);
	qpc_ext = DEVX_ADDR_OF(init2init_qp_in, in, qpc_data_extension);
	DEVX_SET(qpc_ext, qpc_ext, mmo, 1);

	ret = snap_qp_modify(qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		SNAP_LIB_LOG_ERR("failed to modify qp to init with errno = %d", ret);

	return ret;
}

static int snap_modify_lb_qp_rst2init(struct snap_qp *qp,
				     struct ibv_qp_attr *qp_attr, int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rst2init_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rst2init_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);
	int ret;

	DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
	DEVX_SET(rst2init_qp_in, in, qpn, snap_qp_get_qpnum(qp));
	DEVX_SET(qpc, qpc, pm_state, MLX5_QP_PM_MIGRATED);

	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);

	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);

	if (attr_mask & IBV_QP_ACCESS_FLAGS) {
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_READ)
			DEVX_SET(qpc, qpc, rre, 1);
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_WRITE)
			DEVX_SET(qpc, qpc, rwe, 1);
	}

	ret = snap_qp_modify(qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		SNAP_LIB_LOG_ERR("failed to modify qp to init with errno = %d", ret);
	return ret;
}

static int snap_modify_lb_qp_init2rtr(struct snap_qp *qp,
				    struct ibv_qp_attr *qp_attr, int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(init2rtr_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(init2rtr_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);
	int ret;

	DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
	DEVX_SET(init2rtr_qp_in, in, qpn, snap_qp_get_qpnum(qp));

	/* 30 is the maximum value for Infiniband QPs*/
	DEVX_SET(qpc, qpc, log_msg_max, 30);

	/* TODO: add more attributes */
	if (attr_mask & IBV_QP_PATH_MTU)
		DEVX_SET(qpc, qpc, mtu, qp_attr->path_mtu);
	if (attr_mask & IBV_QP_DEST_QPN)
		DEVX_SET(qpc, qpc, remote_qpn, qp_attr->dest_qp_num);
	if (attr_mask & IBV_QP_RQ_PSN)
		DEVX_SET(qpc, qpc, next_rcv_psn, qp_attr->rq_psn & 0xffffff);
	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);
	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);
	if (attr_mask & IBV_QP_MAX_DEST_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_rra_max,
			 snap_u32log2(qp_attr->max_dest_rd_atomic));
	if (attr_mask & IBV_QP_MIN_RNR_TIMER)
		DEVX_SET(qpc, qpc, min_rnr_nak, qp_attr->min_rnr_timer);
	if (attr_mask & IBV_QP_AV)
		DEVX_SET(qpc, qpc, primary_address_path.fl, 1);

	ret = snap_qp_modify(qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		SNAP_LIB_LOG_ERR("failed to modify qp to rtr with errno = %d", ret);
	return ret;
}

static int snap_modify_lb_qp_rtr2rts(struct snap_qp *qp,
				    struct ibv_qp_attr *qp_attr, int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rtr2rts_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rtr2rts_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);
	int ret;

	DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
	DEVX_SET(rtr2rts_qp_in, in, qpn, snap_qp_get_qpnum(qp));

	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_RETRY_CNT)
		DEVX_SET(qpc, qpc, retry_count, qp_attr->retry_cnt);
	if (attr_mask & IBV_QP_SQ_PSN)
		DEVX_SET(qpc, qpc, next_send_psn, qp_attr->sq_psn & 0xffffff);
	if (attr_mask & IBV_QP_RNR_RETRY)
		DEVX_SET(qpc, qpc, rnr_retry, qp_attr->rnr_retry);
	if (attr_mask & IBV_QP_MAX_QP_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_sra_max,
			 snap_u32log2(qp_attr->max_rd_atomic));

	ret = snap_qp_modify(qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		SNAP_LIB_LOG_ERR("failed to modify qp to rts with errno = %d", ret);
	return ret;
}

#if !HAVE_DECL_IBV_QUERY_GID_EX
enum ibv_gid_type {
	IBV_GID_TYPE_IB,
	IBV_GID_TYPE_ROCE_V1,
	IBV_GID_TYPE_ROCE_V2,
};

struct ibv_gid_entry {
	union ibv_gid gid;
	uint32_t gid_index;
	uint32_t port_num;
	uint32_t gid_type; /* enum ibv_gid_type */
	uint32_t ndev_ifindex;
};

static int ibv_query_gid_ex(struct ibv_context *context, uint32_t port_num,
			    uint32_t gid_index, struct ibv_gid_entry *entry,
			    uint32_t flags)
{
	SNAP_LIB_LOG_ERR("%s is not implemented", __func__);
	return -1;
}
#endif

static int snap_activate_loop_2_qp(struct snap_dma_ibv_qp *qp1, struct snap_dma_ibv_qp *qp2,
				enum ibv_mtu mtu, bool force_loopback)
{
	struct ibv_qp_attr attr;
	int rc, flags_mask;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = SNAP_DMA_QP_PKEY_INDEX;
	attr.port_num = SNAP_DMA_QP_PORT_NUM;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
	flags_mask = IBV_QP_STATE |
		     IBV_QP_PKEY_INDEX |
		     IBV_QP_PORT |
		     IBV_QP_ACCESS_FLAGS;

	rc = snap_modify_lb_qp_rst2init(qp1->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify SW QP to INIT errno=%d", rc);
		return rc;
	} else if (qp1->mode == SNAP_DMA_Q_MODE_GGA) {
		rc = snap_modify_lb_qp_init2init(qp1->qp);
		if (rc) {
			SNAP_LIB_LOG_ERR("failed to modify SW QP in INIT2INIT errno=%d",
				   rc);
			return rc;
		}
	}

	rc = snap_modify_lb_qp_rst2init(qp2->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify FW QP to INIT errno=%d", rc);
		return rc;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu;
	attr.rq_psn = SNAP_DMA_QP_RQ_PSN;
	attr.max_dest_rd_atomic = SNAP_DMA_QP_MAX_DEST_RD_ATOMIC;
	attr.min_rnr_timer = SNAP_DMA_QP_RNR_TIMER;
	attr.ah_attr.port_num = SNAP_DMA_QP_PORT_NUM;
	attr.ah_attr.grh.hop_limit = SNAP_DMA_QP_HOP_LIMIT;

	flags_mask = IBV_QP_STATE              |
		     IBV_QP_AV                 |
		     IBV_QP_PATH_MTU           |
		     IBV_QP_DEST_QPN           |
		     IBV_QP_RQ_PSN             |
		     IBV_QP_MAX_DEST_RD_ATOMIC |
		     IBV_QP_MIN_RNR_TIMER;

	attr.dest_qp_num = snap_qp_get_qpnum(qp2->qp);
	rc = snap_modify_lb_qp_init2rtr(qp1->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify SW QP to RTR errno=%d", rc);
		return rc;
	}

	attr.dest_qp_num = snap_qp_get_qpnum(qp1->qp);
	rc = snap_modify_lb_qp_init2rtr(qp2->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify FW QP to RTR errno=%d", rc);
		return rc;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = SNAP_DMA_QP_TIMEOUT;
	attr.retry_cnt = SNAP_DMA_QP_RETRY_COUNT;
	attr.sq_psn = SNAP_DMA_QP_SQ_PSN;
	attr.rnr_retry = SNAP_DMA_QP_RNR_RETRY;
	attr.max_rd_atomic = SNAP_DMA_QP_MAX_RD_ATOMIC;
	flags_mask = IBV_QP_STATE              |
		     IBV_QP_TIMEOUT            |
		     IBV_QP_RETRY_CNT          |
		     IBV_QP_RNR_RETRY          |
		     IBV_QP_SQ_PSN             |
		     IBV_QP_MAX_QP_RD_ATOMIC;

	/* once QPs were moved to RTR using devx, they must also move to RTS
	 * using devx since kernel doesn't know QPs are on RTR state
	 **/
	rc = snap_modify_lb_qp_rtr2rts(qp1->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify SW QP to RTS errno=%d", rc);
		return rc;
	}

	rc = snap_modify_lb_qp_rtr2rts(qp2->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify FW QP to RTS errno=%d", rc);
		return rc;
	}

	return 0;
}

static int snap_activate_qp_to_remote_qpn(struct snap_dma_ibv_qp *qp1,
					  int remote_qpn, enum ibv_mtu mtu, bool force_loopback)
{
	struct ibv_qp_attr attr;
	int rc, flags_mask;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = SNAP_DMA_QP_PKEY_INDEX;
	attr.port_num = SNAP_DMA_QP_PORT_NUM;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
	flags_mask = IBV_QP_STATE |
		IBV_QP_PKEY_INDEX |
		IBV_QP_PORT |
		IBV_QP_ACCESS_FLAGS;

	rc = snap_modify_lb_qp_rst2init(qp1->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify SW QP to INIT errno=%d", rc);
		return rc;
	} else if (qp1->mode == SNAP_DMA_Q_MODE_GGA) {
		rc = snap_modify_lb_qp_init2init(qp1->qp);
		if (rc) {
			SNAP_LIB_LOG_ERR("failed to modify SW QP in INIT2INIT errno=%d",
					rc);
			return rc;
		}
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu;
	attr.rq_psn = SNAP_DMA_QP_RQ_PSN;
	attr.max_dest_rd_atomic = SNAP_DMA_QP_MAX_DEST_RD_ATOMIC;
	attr.min_rnr_timer = SNAP_DMA_QP_RNR_TIMER;
	attr.ah_attr.port_num = SNAP_DMA_QP_PORT_NUM;
	attr.ah_attr.grh.hop_limit = SNAP_DMA_QP_HOP_LIMIT;
	attr.ah_attr.is_global = 1;

	attr.dest_qp_num = remote_qpn;
	flags_mask = IBV_QP_STATE              |
		IBV_QP_AV                 |
		IBV_QP_PATH_MTU           |
		IBV_QP_DEST_QPN           |
		IBV_QP_RQ_PSN             |
		IBV_QP_MAX_DEST_RD_ATOMIC |
		IBV_QP_MIN_RNR_TIMER;

	rc = snap_modify_lb_qp_init2rtr(qp1->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify SW QP to RTR errno=%d", rc);
		return rc;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = SNAP_DMA_QP_TIMEOUT;
	attr.retry_cnt = SNAP_DMA_QP_RETRY_COUNT;
	attr.sq_psn = SNAP_DMA_QP_SQ_PSN;
	attr.rnr_retry = SNAP_DMA_QP_RNR_RETRY;
	attr.max_rd_atomic = SNAP_DMA_QP_MAX_RD_ATOMIC;
	flags_mask = IBV_QP_STATE              |
		IBV_QP_TIMEOUT            |
		IBV_QP_RETRY_CNT          |
		IBV_QP_RNR_RETRY          |
		IBV_QP_SQ_PSN             |
		IBV_QP_MAX_QP_RD_ATOMIC;

	/* once QPs were moved to RTR using devx, they must also move to RTS
	 * using devx since kernel doesn't know QPs are on RTR state
	 */
	rc = snap_modify_lb_qp_rtr2rts(qp1->qp, &attr, flags_mask);
	if (rc) {
		SNAP_LIB_LOG_ERR("failed to modify SW QP to RTS errno=%d", rc);
		return rc;
	}

	return 0;
}

static int snap_dma_ep_connect_helper(struct snap_dma_ibv_qp *qp1,
		struct snap_dma_ibv_qp *qp2, struct ibv_pd *pd)
{
	int rc;
	bool roce_en = false, ib_en = false;
	enum ibv_mtu mtu = IBV_MTU_1024;
	bool force_loopback = false;
	struct snap_roce_caps roce_caps = {0};

	rc = check_port(pd->context, SNAP_DMA_QP_PORT_NUM, &roce_en,
			&ib_en, &mtu);
	if (rc)
		return rc;

	rc = fill_roce_caps(pd->context, &roce_caps);
	if (rc)
		return rc;

	/* Check if force-loopback is supported */
	if (ib_en || (roce_caps.resources_on_nvme_emulation_manager &&
	    ((roce_caps.roce_enabled && roce_caps.fl_when_roce_enabled) ||
	     (!roce_caps.roce_enabled && roce_caps.fl_when_roce_disabled)))) {
		force_loopback = true;
		/*
		 * If force loopback is set it seems that we can ignore port
		 * mtu settings.
		 * Using 4k mtu gives better RDMA performance
		 * And it rises crypto limit from 600Kiops to 2.2Miops
		 */
		mtu = IBV_MTU_4096;
	} else {
		SNAP_LIB_LOG_ERR("Force-loopback QP is not supported. Cannot create queue.");
		return -ENOTSUP;
	}

	return snap_activate_loop_2_qp(qp1, qp2, mtu, force_loopback);
}

static int snap_dma_ep_connect_qpn_helper(struct snap_dma_ibv_qp *qp1,
					  int remote_qpn, struct ibv_pd *pd)
{
	struct snap_roce_caps roce_caps = {0};
	bool roce_en = false, ib_en = false;
	enum ibv_mtu mtu = IBV_MTU_1024;
	bool force_loopback = false;
	int rc;

	rc = check_port(pd->context, SNAP_DMA_QP_PORT_NUM, &roce_en,
			&ib_en, &mtu);

	if (rc)
		return rc;

	if (ib_en) {
		SNAP_LIB_LOG_ERR("IB mode not supported. Cannot create queue");
		return -ENOTSUP;
	}

	rc = fill_roce_caps(pd->context, &roce_caps);
	if (rc)
		return rc;

	/* Check if force-loopback is supported based on roce caps */
	if (roce_caps.resources_on_nvme_emulation_manager &&
			((roce_caps.roce_enabled && roce_caps.fl_when_roce_enabled) ||
			 (!roce_caps.roce_enabled && roce_caps.fl_when_roce_disabled))) {
		force_loopback = true;
	} else {
		SNAP_LIB_LOG_ERR("Force-loopback option is not supported. Cannot create queue");
		return -ENOTSUP;
	}

	return snap_activate_qp_to_remote_qpn(qp1, remote_qpn, mtu, force_loopback);
}

static int snap_alloc_iov_ctx(struct snap_dma_q *q)
{
	int i, ret, io_ctx_cnt;
	struct snap_dma_q_iov_ctx *iov_ctx;

	io_ctx_cnt = q->sw_qp.dv_qp.hw_qp.sq.wqe_cnt;

	ret = posix_memalign((void **)&iov_ctx, SNAP_DMA_BUF_ALIGN,
			io_ctx_cnt * sizeof(struct snap_dma_q_iov_ctx));
	if (ret) {
		SNAP_LIB_LOG_ERR("alloc dma_q iov_ctx array failed");
		return -ENOMEM;
	}

	memset(iov_ctx, 0, io_ctx_cnt * sizeof(struct snap_dma_q_iov_ctx));

	TAILQ_INIT(&q->free_iov_ctx);

	for (i = 0; i < io_ctx_cnt; i++) {
		iov_ctx[i].q = q;
		TAILQ_INSERT_TAIL(&q->free_iov_ctx, &iov_ctx[i], entry);
	}

	q->iov_ctx = iov_ctx;

	return 0;
}

static void snap_free_iov_ctx(struct snap_dma_q *q)
{
	int i, io_ctx_cnt;
	struct snap_dma_q_iov_ctx *iov_ctx = q->iov_ctx;

	io_ctx_cnt = q->sw_qp.dv_qp.hw_qp.sq.wqe_cnt;

	for (i = 0; i < io_ctx_cnt; i++)
		TAILQ_REMOVE(&q->free_iov_ctx, &iov_ctx[i], entry);

	free(iov_ctx);
	q->iov_ctx = NULL;
}

static int snap_alloc_crypto_ctx(struct snap_dma_q *q, struct ibv_pd *pd, const struct snap_dma_q_crypto_attr *attr)
{
	int i, ret, io_ctx_cnt;
	struct snap_dma_q_crypto_ctx *crypto_ctx;
	struct mlx5_devx_mkey_attr mkey_attr = {};
	struct snap_relaxed_ordering_caps caps = {};

	if (attr->crypto_place != SNAP_DMA_Q_CRYPTO_ON_DEST &&
	    attr->crypto_place != SNAP_DMA_Q_CRYPTO_ON_SRC)
		return -EINVAL;

	io_ctx_cnt = attr->crypto_ctx_max;
	if (io_ctx_cnt == 0)
		io_ctx_cnt = SNAP_DMA_Q_CRYPTO_CTX_MAX;

	ret = posix_memalign((void **)&crypto_ctx, SNAP_DMA_BUF_ALIGN,
			io_ctx_cnt * sizeof(struct snap_dma_q_crypto_ctx));
	if (ret) {
		SNAP_LIB_LOG_ERR("alloc dma_q crypto_ctx array failed");
		return -ENOMEM;
	}

	ret = snap_query_relaxed_ordering_caps(pd->context, &caps);
	if (ret) {
		SNAP_LIB_LOG_ERR("query relaxed_ordering_caps failed, ret:%d", ret);
		goto free_crypto_ctx;
	}

	memset(crypto_ctx, 0, io_ctx_cnt * sizeof(struct snap_dma_q_crypto_ctx));

	TAILQ_INIT(&q->free_crypto_ctx);

	mkey_attr.addr = 0;
	mkey_attr.size = 0;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = caps.relaxed_ordering_write;
	mkey_attr.relaxed_ordering_read = caps.relaxed_ordering_read;
	mkey_attr.klm_num = 0;
	mkey_attr.klm_array = NULL;

	for (i = 0; i < io_ctx_cnt; i++) {
		if (attr->crypto_place == SNAP_DMA_Q_CRYPTO_ON_DEST) {
			mkey_attr.crypto_en = true;
			mkey_attr.bsf_en = true;
		} else {
			mkey_attr.crypto_en = false;
			mkey_attr.bsf_en = false;
		}
		crypto_ctx[i].r_klm_mkey = snap_create_indirect_mkey(pd, &mkey_attr);
		if (!crypto_ctx[i].r_klm_mkey) {
			SNAP_LIB_LOG_ERR("create remote klm mkey for crypto_ctx[%d] failed", i);
			goto destroy_mkeys;
		}

		/* local key is used to linearize */
		if (attr->crypto_place == SNAP_DMA_Q_CRYPTO_ON_DEST) {
			mkey_attr.crypto_en = false;
			mkey_attr.bsf_en = false;
		} else {
			mkey_attr.crypto_en = true;
			mkey_attr.bsf_en = true;
		}
		crypto_ctx[i].l_klm_mkey = snap_create_indirect_mkey(pd, &mkey_attr);
		if (!crypto_ctx[i].l_klm_mkey) {
			SNAP_LIB_LOG_ERR("create local klm mkey for crypto_ctx[%d] failed", i);
			snap_destroy_indirect_mkey(crypto_ctx[i].r_klm_mkey);
			goto destroy_mkeys;
		}
	}

	for (i = 0; i < io_ctx_cnt; i++) {
		crypto_ctx[i].q = q;
		TAILQ_INSERT_TAIL(&q->free_crypto_ctx, &crypto_ctx[i], entry);
	}

	q->crypto_ctx = crypto_ctx;
	q->n_crypto_ctx = io_ctx_cnt;
	q->crypto_place = attr->crypto_place;

	return 0;

destroy_mkeys:
	for (i--; i >= 0; i--) {
		snap_destroy_indirect_mkey(crypto_ctx[i].l_klm_mkey);
		snap_destroy_indirect_mkey(crypto_ctx[i].r_klm_mkey);
	}

free_crypto_ctx:
	free(crypto_ctx);

	return 1;
}

static void snap_free_crypto_ctx(struct snap_dma_q *q)
{
	int i, io_ctx_cnt;
	struct snap_dma_q_crypto_ctx *crypto_ctx = q->crypto_ctx;

	io_ctx_cnt = q->n_crypto_ctx;

	for (i = 0; i < io_ctx_cnt; i++) {
		TAILQ_REMOVE(&q->free_crypto_ctx, &crypto_ctx[i], entry);
		snap_destroy_indirect_mkey(crypto_ctx[i].l_klm_mkey);
		snap_destroy_indirect_mkey(crypto_ctx[i].r_klm_mkey);
	}

	free(crypto_ctx);
	q->crypto_ctx = NULL;
	q->n_crypto_ctx = 0;
}

static int snap_alloc_ir_ctx(struct snap_dma_q *q, struct ibv_pd *pd)
{
	int i, ret, io_ctx_cnt;
	struct snap_dma_q_ir_ctx *ir_ctx;

	io_ctx_cnt = q->sw_qp.dv_qp.hw_qp.sq.wqe_cnt;

	ret = posix_memalign((void **)&ir_ctx, SNAP_DMA_BUF_ALIGN,
			io_ctx_cnt * sizeof(struct snap_dma_q_ir_ctx));
	if (ret) {
		SNAP_LIB_LOG_ERR("alloc dma_q ir_ctx array failed");
		return -ENOMEM;
	}

	q->ir_ctx = ir_ctx;

	ret = posix_memalign((void **)&q->ir_buf, SNAP_DMA_BUF_ALIGN,
			io_ctx_cnt * 32);
	if (ret) {
		SNAP_LIB_LOG_ERR("alloc dma_q inline recv buffer failed");
		goto free_ir_ctx;
	}

	q->ir_mr = ibv_reg_mr(pd, q->ir_buf, io_ctx_cnt * 32,
					IBV_ACCESS_LOCAL_WRITE |
					IBV_ACCESS_REMOTE_WRITE |
					IBV_ACCESS_REMOTE_READ);
	if (!q->ir_mr) {
		SNAP_LIB_LOG_ERR("register ir_mr failed");
		ret = -errno;
		goto free_ir_buf;
	}

	memset(ir_ctx, 0, io_ctx_cnt * sizeof(struct snap_dma_q_ir_ctx));
	TAILQ_INIT(&q->free_ir_ctx);
	memset(q->ir_buf, 0, io_ctx_cnt * 32);

	for (i = 0; i < io_ctx_cnt; i++) {
		ir_ctx[i].q = q;
		ir_ctx[i].buf = (char *)q->ir_buf + i * 32;
		ir_ctx[i].mkey = q->ir_mr->lkey;

		TAILQ_INSERT_TAIL(&q->free_ir_ctx, &ir_ctx[i], entry);
	}

	return 0;

free_ir_buf:
	free(q->ir_buf);

free_ir_ctx:
	free(q->ir_ctx);

	return ret;
}

static void snap_free_ir_ctx(struct snap_dma_q *q)
{
	int i, io_ctx_cnt;
	struct snap_dma_q_ir_ctx *ir_ctx = q->ir_ctx;

	io_ctx_cnt = q->sw_qp.dv_qp.hw_qp.sq.wqe_cnt;

	for (i = 0; i < io_ctx_cnt; i++)
		TAILQ_REMOVE(&q->free_ir_ctx, &ir_ctx[i], entry);

	ibv_dereg_mr(q->ir_mr);
	free(q->ir_buf);
	free(ir_ctx);
	q->ir_ctx = NULL;
}

static int snap_create_io_ctx(struct snap_dma_q *q, struct ibv_pd *pd,
			const struct snap_dma_q_create_attr *attr)
{
	int ret;

	q->iov_support = false;
	q->crypto_support = false;

	if (attr->iov_enable) {
		/* DV/GGA mode do not need iov_ctx */
		if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS) {
			ret = snap_alloc_iov_ctx(q);
			if (ret) {
				SNAP_LIB_LOG_ERR("Allocate iov_ctx failed");
				goto out;
			}
		}
		q->iov_support = true;
	}

	if (attr->crypto_enable) {

		if (q->sw_qp.mode != SNAP_DMA_Q_MODE_DV) {
			SNAP_LIB_LOG_ERR("failed to enable crypto: dma q must be in DV mode");
			goto out;
		}

		ret = snap_alloc_crypto_ctx(q, pd, &attr->crypto_attr);
		if (ret) {
			SNAP_LIB_LOG_ERR("Allocate crypto_ctx failed");
			goto free_iov_ctx;
		}

		q->crypto_support = true;
	}

	if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS) {
		ret = snap_alloc_ir_ctx(q, pd);
		if (ret) {
			SNAP_LIB_LOG_ERR("Allocate ir_ctx failed");
			goto free_iov_ctx;
		}
	}

	return 0;

free_iov_ctx:
	if (attr->iov_enable) {
		if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS)
			snap_free_iov_ctx(q);
		q->iov_support = false;
	}

out:
	return 1;
}

static void snap_destroy_io_ctx(struct snap_dma_q *q)
{
	if (q->iov_support) {
		if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS)
			snap_free_iov_ctx(q);
		q->iov_support = false;
	}

	if (q->crypto_support) {
		snap_free_crypto_ctx(q);
		q->crypto_support = false;
	}

	if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS)
		snap_free_ir_ctx(q);
}

/**
 * snap_dma_ep_connect() - Connect 2 Qps
 * @q1:  first queue to connect
 * @q2:  second queue to connect
 *
 * connect software qps of 2 separate snap_dma_q's
 *
 * Return: 0 on success, -errno on failure.
 */
int snap_dma_ep_connect(struct snap_dma_q *q1, struct snap_dma_q *q2)
{
	int ret;
	struct ibv_pd *pd;

	if (!q1 || !q2)
		return -1;

	pd = snap_qp_get_pd(q1->sw_qp.qp);
	if (!pd)
		return -1;

	ret = snap_dma_ep_connect_helper(&q1->sw_qp, &q2->sw_qp, pd);
	if (ret)
		return ret;

	/* In general one must post recvs before qp is moved to the RTR.
	 * However in our case we control both sides and there is no traffic
	 * until this function completes.
	 */
	ret = snap_dma_q_post_recv(q1);
	if (ret)
		return ret;

	ret = snap_dma_q_post_recv(q2);
	if (ret)
		return ret;

	return 0;
}

/**
 * snap_dma_ep_connect_remote_qpn() - Connect 2 Qps
 * @q1:  first queue
 * @q2:  qpn of remote queue
 *
 * connect software qps of 2 separate snap_dma_q's
 *
 * Return: 0 on success, -errno on failure.
 */
int snap_dma_ep_connect_remote_qpn(struct snap_dma_q *q1, int remote_qpn)
{
	struct ibv_pd *pd;
	int ret;

	if (!q1 || !remote_qpn)
		return -1;

	pd = snap_qp_get_pd(q1->sw_qp.qp);
	if (!pd)
		return -1;

	ret = snap_dma_ep_connect_qpn_helper(&q1->sw_qp, remote_qpn, pd);
	if (ret)
		return ret;

	return 0;
}

static struct snap_dma_q *snap_dma_worker_queue_get(struct snap_dma_worker *wk)
{
	int idx;
	struct snap_dma_q *q;

	q = calloc(1, sizeof(*q));
	if (!q)
		return NULL;
	for (idx = 0; idx < wk->max_queues; idx++)
		if (!wk->queues[idx]) {
			wk->queues[idx] = q;
			return q;
		}

	free(q);
	return NULL;
}

static void snap_dma_worker_queue_put(struct snap_dma_q *q)
{
	int i;

	for (i = 0; i < q->worker->max_queues; i++)
		if (q->worker->queues[i] == q)
			q->worker->queues[i] = NULL;
	free(q);
}

/**
 * snap_dma_ep_create() - Create sw only DMA queue
 * @pd:    protection domain to create qp
 * @attr:  dma queue creation attributes
 *
 * The function creates only a sw qp.
 * The use is to create 2 separate sw only snap_dma_q's and connect them
 *
 * If the endpoint is created on DPA, dpa_dma_ep_init() must be called by a
 * DPA thread to complete initialization.
 *
 * Return: dma queue or NULL on error.
 */
struct snap_dma_q *snap_dma_ep_create(struct ibv_pd *pd,
		const struct snap_dma_q_create_attr *attr)
{
	int rc;
	struct snap_dma_q *q;

	if (!pd)
		return NULL;

	if (!attr->rx_cb)
		return NULL;

	if (!attr->wk)
		q = calloc(1, sizeof(*q));
	else
		q = snap_dma_worker_queue_get(attr->wk);
	if (!q)
		return NULL;

	q->worker = attr->wk;

	rc = snap_create_sw_qp(q, pd, attr);
	if (rc)
		goto free_q;

	rc = snap_create_io_ctx(q, pd, attr);
	if (rc)
		goto destroy_sw_qp;

	q->uctx = attr->uctx;
	q->rx_cb = attr->rx_cb;
	return q;

destroy_sw_qp:
	snap_destroy_sw_qp(q);

free_q:
	if (!attr->wk)
		free(q);
	else
		snap_dma_worker_queue_put(q);
	return NULL;
}

/**
 * snap_dma_q_create() - Create DMA queue
 * @pd:    protection domain to create qps
 * @attr:  dma queue creation attributes
 *
 * Create and connect both software and fw qps
 *
 * The function creates a pair of QPs and connects them.
 * snap_dma_q_get_fw_qp() should be used to obtain qp number that
 * can be given to firmware emulation objects.
 *
 * Note that on Blufield1 extra steps are required:
 *  - an on behalf QP with the same number as
 *    returned by the snap_dma_q_get_fw_qp() must be created
 *  - a fw qp state must be copied to the on behalf qp
 *  - steering rules must be set
 *
 * All these steps must be done by the application.
 *
 * Return: dma queue or NULL on error.
 */
struct snap_dma_q *snap_dma_q_create(struct ibv_pd *pd,
		const struct snap_dma_q_create_attr *attr)
{
	struct snap_dma_q *q;
	int rc;

	q = snap_dma_ep_create(pd, attr);
	if (!q)
		return NULL;

	rc = snap_create_fw_qp(q, pd, attr);
	if (rc)
		goto free_sw_qp;

	rc = snap_dma_ep_connect_helper(&q->sw_qp, &q->fw_qp->fw_qp, pd);
	if (rc)
		goto free_fw_qp;

	/* In general one must post recvs before qp is moved to the RTR.
	 * However in our case we control both sides and there is no traffic
	 * until fw qp is passed to the FW
	 */
	rc = snap_dma_q_post_recv(q);
	if (rc)
		goto free_fw_qp;

	return q;

free_fw_qp:
	snap_destroy_fw_qp(q);
free_sw_qp:
	snap_dma_ep_destroy(q);
	return NULL;
}

int snap_dma_q_modify_to_err_state(struct snap_dma_q *q)
{
	int ret;
	uint8_t in[DEVX_ST_SZ_BYTES(2err_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(2err_qp_out)] = {0};

	DEVX_SET(2err_qp_in, in, opcode, MLX5_CMD_OP_2ERR_QP);
	DEVX_SET(2err_qp_in, in, qpn, snap_qp_get_qpnum(q->sw_qp.qp));

	ret = snap_qp_modify(q->sw_qp.qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		SNAP_LIB_LOG_ERR("failed to modify qp to err with errno = %d", ret);

	return ret;
}

/**
 * snap_dma_ep_destroy() - Destroy DMA ep queue
 *
 * @q: dma queue
 */
void snap_dma_ep_destroy(struct snap_dma_q *q)
{
	snap_destroy_io_ctx(q);
	snap_destroy_sw_qp(q);
	if (q->worker)
		snap_dma_worker_queue_put(q);
	else
		free(q);
}

/**
 * snap_dma_q_destroy() - Destroy DMA queue
 *
 * @q: dma queue
 */
void snap_dma_q_destroy(struct snap_dma_q *q)
{
	snap_destroy_fw_qp(q);
	snap_dma_ep_destroy(q);
}

SNAP_STATIC_ASSERT(sizeof(struct snap_dma_q) < SNAP_DMA_THREAD_MBOX_CMD_SIZE,
		"struct snap_dma_q is too big for the DPA thread mbox");
/**
 * snap_dma_ep_dpa_copy_sync() - Copy DMA endpoint to DPA
 * @thr:  thread to copy endpoint to
 * @q:    dma endpoint
 *
 * The function copies DMA endpoint to the DPA thread. The copy is done
 * synchronously. After the function return endpoint is initialized by the
 * DPA thread and is ready to use.
 *
 * Return:
 * 0 on success or -errno
 */
int snap_dma_ep_dpa_copy_sync(struct snap_dpa_thread *thr, struct snap_dma_q *q)
{
	int ret = 0;
	struct snap_dma_ep_copy_cmd *cmd;
	struct snap_dpa_rsp *rsp;
	void *mbox;

	mbox = snap_dpa_thread_mbox_acquire(thr);

	cmd = (struct snap_dma_ep_copy_cmd *)snap_dpa_mbox_to_cmd(mbox);
	if (snap_unlikely(!cmd)) {
		ret = -EINVAL;
		goto out;
	}
	memcpy(&cmd->q, q, sizeof(*q));
	snap_dpa_cmd_send(thr, &cmd->base, SNAP_DPA_CMD_DMA_EP_COPY);

	rsp = snap_dpa_rsp_wait(mbox);
	if (rsp->status != SNAP_DPA_RSP_OK) {
		SNAP_LIB_LOG_ERR("Failed to copy DMA queue: %d", rsp->status);
		ret = -EIO;
	}

out:
	snap_dpa_thread_mbox_release(thr);
	return ret;
}

static int snap_create_worker_cqs_helper(struct snap_dma_worker *wk,
		struct ibv_pd *pd, struct snap_cq_attr *cq_attr)
{
	int rc;

	if (wk->mode == SNAP_DMA_WORKER_MODE_SHARED_CQ_RX_ONLY ||
			wk->mode == SNAP_DMA_WORKER_MODE_SHARED_CQ) {
		/* Use 128 bytes cqes in order to allow scatter to cqe on receive
		 * This is relevant for NVMe sqe and for virtio queues when number of
		 * tunneled descr is less then three.
		 */
		cq_attr->cqe_size = SNAP_DMA_Q_RX_CQE_SIZE;
		wk->rx_cq = snap_cq_create(pd->context, cq_attr);
		if (!wk->rx_cq)
			return -EINVAL;

		rc = snap_cq_to_hw_cq(wk->rx_cq, &wk->dv_rx_cq);
		if (rc)
			goto free_rx_cq;
	}

	return 0;

free_rx_cq:
	snap_cq_destroy(wk->rx_cq);
	return -EINVAL;
}

struct snap_dma_worker *snap_dma_worker_create(struct ibv_pd *pd,
	const struct snap_dma_worker_create_attr *attr)
{
	struct snap_dma_worker *wk = calloc(1, sizeof(*wk) + attr->exp_queue_num * sizeof(struct snap_dma_q *));
	struct snap_cq_attr cq_attr = {
		.cq_context = NULL,
		.comp_channel = NULL,
		.comp_vector = 0,
		.cqe_cnt = attr->exp_queue_num * attr->exp_queue_rx_size,
		.cqe_size = SNAP_DMA_Q_TX_CQE_SIZE,
		.cq_type = SNAP_OBJ_DEVX
	};

	if (!wk)
		return NULL;

	wk->max_queues = attr->exp_queue_num;
	wk->mode = attr->mode;
	SLIST_INIT(&wk->pending_dbs);

	snap_create_worker_cqs_helper(wk, pd, &cq_attr);

	return wk;
}

void snap_dma_worker_destroy(struct snap_dma_worker *wk)
{
	if (!wk)
		return;

	if (wk->rx_cq)
		snap_cq_destroy(wk->rx_cq);

	free(wk);
}

int snap_dma_worker_flush(struct snap_dma_worker *wk)
{
	return dv_worker_flush(wk);
}

int snap_dma_worker_progress_rx(struct snap_dma_worker *wk)
{
	return dv_worker_progress_rx(wk);
}

int snap_dma_worker_progress_tx(struct snap_dma_worker *wk)
{
	return dv_worker_progress_tx(wk);
}

void snap_dma_q_dv_err_cb_set(struct snap_dma_q *q, snap_dma_dv_err_cb_t cb)
{
	q->dv_err_cb = cb;
}

/**
 * snap_dma_q_migrate() - Resubmit WQEs from the old qp to the new one
 * @orig_q:       original qp
 * @new_q:        new qp to post WQEs
 * @attr:         various attributes that control how WQEs are resubmitted
 *
 * The purpose of the function is to allow fast error recovery when one of the submitted
 * WQEs executed with error because of a bad remote memory key. For example QP is used
 * to multiplex requests to several VMs and one VM is suddenly killed.
 *
 * However the function tries to be as generic as possible and can be used for other
 * purposes. E.x. duplicate WQEs to the backup dma q.
 *
 * The function will copy all WQEs starting from the @attr.start_pi index to the @new_q.
 *
 * If @attr.rkey_policy is SNAP_DMA_Q_MIGR_RKEY_DISCARD the function will assume
 * that the first WQE caused remote access error and it will not post WQEs with
 * the same bad rkey. If WQE with the 'bad rkey' has user completion it will be
 * invoked with the 'remote access' error.
 *
 * If @attr.rkey_policy is SNAP_DMA_Q_MIGR_RKEY_FIX the function will repost
 * everything. Rkey in the first WQE considered 'bad' and it will be replaced
 * with the @attr.fixed_rkey.
 *
 * If @attr.rkey_policy is SNAP_DMA_Q_MIGR_RKEY_RETRY the function will repost
 * everything.
 *
 * All user completions will be also copied to the @new_qp.
 *
 * The function will use doorbell trigger mode of the @new_qp.
 *
 * There is a number of limitations:
 * - VERBs are not supported
 * - only one sided operations are supported:
 *   RDMA read, write and write short
 *   GGA read, write
 *   UMR operations
 *   compound operations like v2v, writec and v2vc
 *
 * Return:
 * 0 on success
 * -1 on error
 */
int snap_dma_q_migrate(struct snap_dma_q *orig_q, struct snap_dma_q *new_q, const struct snap_dma_q_migrate_attr *attr)
{
	/* TODO: this function can also run on DPA, check */
	uint32_t bad_rkey = 0xffff;
	struct snap_dv_qp *dvq_orig = &orig_q->sw_qp.dv_qp;
	struct snap_dv_qp *dvq_new = &new_q->sw_qp.dv_qp;
	uint16_t end_pi = dvq_orig->hw_qp.sq.pi;
	uint16_t pi;
	bool rkey_found;

	SNAP_LIB_LOG_DBG("QPN:0x%x -> 0x%x copy from pi_idx=%d to pi_idx=%d",
			dvq_orig->hw_qp.qp_num,
			dvq_new->hw_qp.qp_num,
			attr->start_pi & (dvq_orig->hw_qp.sq.wqe_cnt - 1),
			end_pi & (dvq_orig->hw_qp.sq.wqe_cnt - 1));
	SNAP_LIB_LOG_DBG("Dest idx=%d", dvq_new->hw_qp.sq.pi & (dvq_new->hw_qp.sq.wqe_cnt - 1));

	for (pi = attr->start_pi; pi != end_pi;) {
		struct mlx5_wqe_ctrl_seg *ctrl = snap_dv_get_wqe_bb_by_pi(dvq_orig, pi);
		uint8_t opcode = be32toh(ctrl->opmod_idx_opcode) & 0xff;
		uint8_t opmod = (be32toh(ctrl->opmod_idx_opcode) >> 24) & 0xff;
		uint8_t ds = be32toh(ctrl->qpn_ds) & 0xff;
		int n_bb = round_up(16*ds, MLX5_SEND_WQE_BB);
		struct mlx5_wqe_raddr_seg *rseg = NULL;
		uint32_t rkey;
		int i;

		SNAP_LIB_LOG_DBG("ctrl_seg: opmod_idx_opcode 0x%x qpn_dps 0x%x", be32toh(ctrl->opmod_idx_opcode), be32toh(ctrl->qpn_ds));
		SNAP_LIB_LOG_DBG("opcode: %d wqe_size: %d n_bb: %d", opcode, 16*ds, n_bb);

		if (!qp_can_tx(new_q, n_bb)) {
			/* TODO: partial migration */
			SNAP_LIB_LOG_ERR("dest qp 0x%x is full", dvq_new->hw_qp.qp_num);
			return -EAGAIN;
		}

		switch (opcode) {
		case MLX5_OPCODE_RDMA_WRITE:
		case MLX5_OPCODE_RDMA_READ:
			rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
			rkey = be32toh(rseg->rkey);

			if (pi == attr->start_pi) {
				bad_rkey = rkey;
				SNAP_LIB_LOG_DBG("First rkey is 0x%x", rkey);
				rkey_found = true;
			}
			break;
		default:
			SNAP_LIB_LOG_ERR("qp 0x%x cannot migrate opcode %d", dvq_orig->hw_qp.qp_num, opcode);
			return -ENOTSUP;
		}

		/* get completion */
		uint16_t comp_idx = pi & (dvq_orig->hw_qp.sq.wqe_cnt - 1);
		struct snap_dma_completion *comp = dvq_orig->comps[comp_idx].comp;

		orig_q->tx_available += n_bb;

		if (attr->rkey_policy == SNAP_DMA_Q_MIGR_RKEY_DISCARD) {
			if (rkey_found && bad_rkey == rkey) {
				SNAP_LIB_LOG_DBG("Skipping rkey 0x%x", rkey);
				pi += n_bb;
				if (comp && --comp->count == 0) {
					comp->func(comp, MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR);
					/* completion callback may post more to this qp, and it will use
					 * same 'bad_rkey'
					 */
					end_pi = dvq_orig->hw_qp.sq.pi;
					SNAP_LIB_LOG_DBG("CB called, new end_pi is %d", end_pi & (dvq_orig->hw_qp.sq.wqe_cnt - 1));
					dvq_orig->tx_need_ring_db = false;
				}
				continue;
			}
		} else if (attr->rkey_policy == SNAP_DMA_Q_MIGR_RKEY_FIX) {
			if (rkey_found && bad_rkey == rkey) {
				SNAP_LIB_LOG_DBG("Fixing 0x%x -> 0x%x", rkey, attr->fixed_rkey);
				rseg->rkey = htobe32(attr->fixed_rkey);
			}
		}

		/* repost to the new qp */
		for (i = 0; i < n_bb; i++) {
			void *src, *dst;

			src = snap_dv_get_wqe_bb_by_pi(dvq_orig, pi + i);
			dst = snap_dv_get_wqe_bb_by_pi(dvq_new, dvq_new->hw_qp.sq.pi + i);
			memcpy(dst, src, MLX5_SEND_WQE_BB);
		}

		/* adjust control segment */
		struct mlx5_wqe_ctrl_seg *dst_ctrl = snap_dv_get_wqe_bb(dvq_new);

		dst_ctrl->opmod_idx_opcode = htobe32(((uint32_t)opmod << 24) |
				((uint32_t)dvq_new->hw_qp.sq.pi << 8) | opcode);
		dst_ctrl->qpn_ds = htobe32((dvq_new->hw_qp.qp_num << 8) | ds);

		comp_idx = dvq_new->hw_qp.sq.pi & (dvq_new->hw_qp.sq.wqe_cnt - 1);

		/* submit to the qp */
		dvq_new->hw_qp.sq.pi += n_bb - 1;
		snap_dv_wqe_submit(dvq_new, dst_ctrl);

		snap_dv_set_comp(dvq_new, comp_idx, comp, ctrl->fm_ce_se, n_bb);
		new_q->tx_available -= n_bb;
		pi += n_bb;
	}

	dvq_orig->tx_need_ring_db = false;
	return 0;
}

/**
 * snap_dma_ep_reconnect() - Recycle endpoints in the error state
 * @q1: snap_dma_q endpoint
 * @q2: snap_dma_q endpoint
 *
 * The function will move @q1 and @q2 from the error state back to the
 * connected (RTS) state. This is going to be faster then creating two
 * new endpoint and connecting them.
 *
 * Return:
 * 0 on success
 * -errno on error
 */
int snap_dma_ep_reconnect(struct snap_dma_q *q1, struct snap_dma_q *q2)
{
	uint8_t in[DEVX_ST_SZ_BYTES(2rst_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(2rst_qp_out)] = {0};
	struct snap_dv_qp *dvq1 = &q1->sw_qp.dv_qp;
	struct snap_dv_qp *dvq2 = &q2->sw_qp.dv_qp;
	int ret;

	/* reset qp */
	if (dvq1->hw_qp.rq.wqe_cnt || dvq2->hw_qp.rq.wqe_cnt) {
		SNAP_LIB_LOG_ERR("Qps with non zero RQ cannot be reconnected");
		return -ENOTSUP;
	}

	dvq1->hw_qp.sq.pi = 0;
	dvq2->hw_qp.sq.pi = 0;
	/* TODO: rq ??? */
	q1->tx_available = snap_dma_q_dv_get_tx_avail_max(q1);
	q2->tx_available = snap_dma_q_dv_get_tx_avail_max(q2);
	dvq1->tx_need_ring_db = dvq2->tx_need_ring_db = false;

	/* It looks like RC qp drops directly into the error state,
	 * skipping SQERR state. It means we cannot move back to RTS with
	 * the one syscall
	 */
	DEVX_SET(2rst_qp_in, in, opcode, MLX5_CMD_OP_2RST_QP);
	DEVX_SET(2rst_qp_in, in, qpn, snap_qp_get_qpnum(q1->sw_qp.qp));

	ret = snap_qp_modify(q1->sw_qp.qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		SNAP_LIB_LOG_ERR("failed to modify qp 0x%x to RST with errno = %d",  snap_qp_get_qpnum(q1->sw_qp.qp), ret);

	DEVX_SET(2rst_qp_in, in, qpn, snap_qp_get_qpnum(q2->sw_qp.qp));
	ret = snap_qp_modify(q2->sw_qp.qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		SNAP_LIB_LOG_ERR("failed to modify qp 0x%x to rst with errno = %d",  snap_qp_get_qpnum(q2->sw_qp.qp), ret);

	/* can skip port check because we already know that 4k mtu and force loopback are ok */
	return snap_activate_loop_2_qp(&q1->sw_qp, &q2->sw_qp, IBV_MTU_4096, true);
}
