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

#include <stdint.h>
#include <errno.h>

#if !defined(__DPA)
#include <arpa/inet.h>
#endif

#include "snap_dma_internal.h"
#include "snap_umr.h"

#include "config.h"

struct snap_dma_xfer_ctx {
	void *lbuf;
	uint32_t lkey;
	uint32_t rkey;
	uint64_t raddr;
	size_t len;

	bool use_fence;
	struct snap_dma_completion *comp;
};

static inline void snap_dv_set_comp_payload(struct snap_dv_qp *dv_qp,
			uint16_t pi, void *buf)
{
	dv_qp->comps[pi].read_payload = buf;
}

static inline int do_dv_dma_xfer(struct snap_dma_q *q, void *buf, size_t len,
		uint32_t lkey, uint64_t raddr, uint32_t rkey, int op, int flags,
		struct snap_dma_completion *comp, bool use_fence)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint16_t comp_idx;
	uint8_t fm_ce_se = 0;

	fm_ce_se |= flags;
	fm_ce_se |= snap_dv_get_cq_update(dv_qp, comp);
	if (use_fence)
		fm_ce_se |= MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;

	ctrl = (struct mlx5_wqe_ctrl_seg *)snap_dv_get_wqe_bb(dv_qp);
	snap_set_ctrl_seg(ctrl, dv_qp->hw_qp.sq.pi, op, 0, dv_qp->hw_qp.qp_num,
			    fm_ce_se, 3, 0, 0);

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	rseg->raddr = htobe64((uintptr_t)raddr);
	rseg->rkey  = htobe32(rkey);

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	mlx5dv_set_data_seg(dseg, len, lkey, (intptr_t)buf);

	snap_dv_wqe_submit(dv_qp, ctrl);

	/* it is better to start dma as soon as possible and do
	 * bookkeeping later
	 **/
	comp_idx = (dv_qp->hw_qp.sq.pi - 1) & (dv_qp->hw_qp.sq.wqe_cnt - 1);
	snap_dv_set_comp(dv_qp, comp_idx, comp, fm_ce_se, 1);

	if (flags)
		snap_dv_set_comp_payload(dv_qp, comp_idx, buf);

	return 0;
}

static int dv_dma_q_write(struct snap_dma_q *q, void *src_buf, size_t len,
			  uint32_t lkey, uint64_t dstaddr, uint32_t rmkey,
			  struct snap_dma_completion *comp)
{
	return do_dv_dma_xfer(q, src_buf, len, lkey, dstaddr, rmkey,
			MLX5_OPCODE_RDMA_WRITE, 0, comp, false);
}

static int dv_dma_q_read(struct snap_dma_q *q, void *dst_buf, size_t len,
			 uint32_t lkey, uint64_t srcaddr, uint32_t rmkey,
			 struct snap_dma_completion *comp)
{
	int flags;

	flags = (len <= 32) ? MLX5_WQE_CTRL_CQ_UPDATE : 0;

	return do_dv_dma_xfer(q, dst_buf, len, lkey, srcaddr, rmkey,
			MLX5_OPCODE_RDMA_READ, flags, comp, false);
}

/* UMRs are not supported on the DPA yet */
__attribute__((unused))
static inline struct snap_dma_q_crypto_ctx *snap_dma_q_crypto_ctx_alloc(struct snap_dma_q *q)
{
	struct snap_dma_q_crypto_ctx *crypto_ctx;

	crypto_ctx = TAILQ_FIRST(&q->free_crypto_ctx);
	if (snap_unlikely(!crypto_ctx))
		return NULL;

	TAILQ_REMOVE(&q->free_crypto_ctx, crypto_ctx, entry);
	return crypto_ctx;
}

__attribute__((unused))
static inline void snap_dma_q_crypto_ctx_free(struct snap_dma_q_crypto_ctx *ctx)
{
	TAILQ_INSERT_HEAD(&ctx->q->free_crypto_ctx, ctx, entry);
}

__attribute__((unused)) static void
snap_use_klm_mkey_done(struct snap_dma_completion *comp, int status)
{
	struct snap_dma_q_crypto_ctx *crypto_ctx;
	struct snap_dma_completion *orig_comp;

	crypto_ctx = container_of(comp, struct snap_dma_q_crypto_ctx, comp);

	orig_comp = (struct snap_dma_completion *)crypto_ctx->uctx;

	snap_dma_q_crypto_ctx_free(crypto_ctx);

	if (orig_comp && --orig_comp->count == 0)
		orig_comp->func(orig_comp, status);
}

__attribute__((unused)) static inline int snap_iov_to_klm_mtt(struct iovec *iov, int iov_cnt,
			uint32_t *mkey, struct mlx5_klm *klm_mtt, size_t *len)
{
	int i;

	/*TODO: dynamically expand klm_mtt array */
	if (snap_unlikely(iov_cnt > SNAP_DMA_Q_MAX_IOV_CNT)) {
		SNAP_LIB_LOG_ERR("iov_cnt:%d is larger than max supported(%d)",
			iov_cnt, SNAP_DMA_Q_MAX_IOV_CNT);
		return -EINVAL;
	}

	*len = 0;
	for (i = 0; i < iov_cnt; i++) {
		klm_mtt[i].byte_count = iov[i].iov_len;
		klm_mtt[i].mkey = mkey[i];
		klm_mtt[i].address = (uintptr_t)iov[i].iov_base;

		*len += iov[i].iov_len;
	}

	return 0;
}

__attribute__((unused))
static inline int snap_dma_q_iov2umr(struct snap_dma_q *q,
		struct iovec *iov, uint32_t *iov_mkeys, int iov_cnt,
		struct snap_indirect_mkey *klm_mkey, bool crypto,
		struct snap_dma_q_io_attr *io_attr,
		int *n_bb, size_t *len)
{
	struct snap_post_umr_attr umr_attr;
	struct mlx5_klm klm_mtt[iov_cnt];
	int ret;

	/* post UMR WQE for local IOV memory */
	ret = snap_iov_to_klm_mtt(iov, iov_cnt, iov_mkeys, klm_mtt, len);
	if (snap_unlikely(ret))
		return ret;

	umr_attr.purpose = SNAP_UMR_MKEY_MODIFY_ATTACH_MTT;
	umr_attr.klm_mkey = klm_mkey;
	umr_attr.klm_mtt = klm_mtt;
	umr_attr.klm_entries = iov_cnt;

	if (crypto) {
		umr_attr.purpose |= SNAP_UMR_MKEY_MODIFY_ATTACH_CRYPTO_BSF;
		umr_attr.encryption_order = io_attr->enc_order;
		umr_attr.encryption_standard =
			SNAP_CRYPTO_BSF_ENCRYPTION_STANDARD_AES_XTS;
		umr_attr.raw_data_size = *len;
		umr_attr.crypto_block_size_pointer =
			SNAP_CRYPTO_BSF_CRYPTO_BLOCK_SIZE_POINTER_512;
		umr_attr.dek_pointer = io_attr->dek_obj_id;
		umr_attr.xts_initial_tweak = io_attr->xts_initial_tweak;
	}

	ret = snap_umr_post_wqe(q, &umr_attr, NULL, n_bb);
	if (snap_unlikely(ret))
		SNAP_LIB_LOG_ERR("dma_q:%p post umr wqe for local mkey failed, ret:%d", q, ret);

	return ret;
}

/* return NULL if prepare crypto_ctx failed in any reason,
 * and use 'errno' to pass the actually failure reason.
 * TODO:
 * - crypto attr setup to sep function
 */
static inline int
snap_prepare_crypto_ctx(struct snap_dma_q_crypto_ctx *crypto_ctx,
				int crypto_place,
				struct snap_dma_q_io_attr *io_attr,
				struct snap_dma_completion *comp, int *n_bb)
{
#if !defined(__DPA)
	int ret;
	size_t len;
	*n_bb = 0;
	struct snap_dma_q *q = crypto_ctx->q;

	crypto_ctx->uctx = comp;
	crypto_ctx->comp.func = snap_use_klm_mkey_done;
	crypto_ctx->comp.count = 1;

	if (crypto_place == SNAP_DMA_Q_CRYPTO_ON_DEST) {
		if (io_attr->liov_cnt > 1) {
			/* post UMR WQE for local IOV memory */
			ret = snap_dma_q_iov2umr(q, io_attr->liov, io_attr->lkey, io_attr->liov_cnt,
					crypto_ctx->l_klm_mkey, false, NULL, n_bb, &len);
			if (snap_unlikely(ret)) {
				SNAP_LIB_LOG_ERR("dma_q:%p post umr wqe for local mkey failed, ret:%d", q, ret);
				return ret;
			}
		}

		ret = snap_dma_q_iov2umr(q, io_attr->riov, io_attr->rkey, io_attr->riov_cnt,
				crypto_ctx->r_klm_mkey, true, io_attr, n_bb, &io_attr->len);
		if (snap_unlikely(ret)) {
			SNAP_LIB_LOG_ERR("dma_q:%p post umr wqe for remote mkey failed, ret:%d", q, ret);
			return ret;
		}

	} else {
		ret = snap_dma_q_iov2umr(q, io_attr->liov, io_attr->lkey, io_attr->liov_cnt,
				crypto_ctx->l_klm_mkey, true, io_attr, n_bb, &io_attr->len);
		if (ret) {
			SNAP_LIB_LOG_ERR("dma_q:%p post umr wqe for local mkey failed, ret:%d", q, ret);
			return ret;
		}

		if (io_attr->riov_cnt > 1) {
			ret = snap_dma_q_iov2umr(q, io_attr->riov, io_attr->rkey, io_attr->riov_cnt,
					crypto_ctx->r_klm_mkey, false, io_attr, n_bb, &len);
			if (ret) {
				SNAP_LIB_LOG_ERR("dma_q:%p post umr wqe for remote mkey failed, ret:%d", q, ret);
				return ret;
			}
		}
	}

	*n_bb += 1; /* +1 for DMA WQE */

	return 0;
#else
	return -1;
#endif
}

static int snap_prepare_dma_crypto_xfer_ctx(struct snap_dma_q *q,
				struct snap_dma_q_io_attr *io_attr,
				struct snap_dma_completion *comp,
				int *n_bb, struct snap_dma_xfer_ctx *dx_ctx)
{
	struct snap_dma_q_crypto_ctx *crypto_ctx;
	int ret;

	if (snap_unlikely(!(io_attr->io_type & SNAP_DMA_Q_IO_TYPE_ENCRYPTO) &&
			  !(io_attr->io_type & SNAP_DMA_Q_IO_TYPE_IOV))) {
		SNAP_LIB_LOG_ERR("expected iov+crypto combination");
		return -EINVAL;
	}

	crypto_ctx = snap_dma_q_crypto_ctx_alloc(q);
	if (snap_unlikely(!crypto_ctx)) {
		SNAP_LIB_LOG_DBG("dma_q:%p Out of crypto_ctx from pool", q);
		return -EAGAIN;
	}

	if (q->crypto_place == SNAP_DMA_Q_CRYPTO_ON_DEST) {
		ret = snap_prepare_crypto_ctx(crypto_ctx, SNAP_DMA_Q_CRYPTO_ON_DEST, io_attr, comp, n_bb);
		if (snap_unlikely(ret))
			goto free_ctx;

		if (io_attr->liov_cnt == 1) {
			dx_ctx->lbuf = io_attr->liov[0].iov_base;
			dx_ctx->lkey = io_attr->lkey[0];
		} else {
			dx_ctx->lbuf = (void *)crypto_ctx->l_klm_mkey->addr;
			dx_ctx->lkey = crypto_ctx->l_klm_mkey->mkey;
		}

		dx_ctx->raddr = 0; /* use zero based rdma if use bsf enabled mkey */
		dx_ctx->rkey = crypto_ctx->r_klm_mkey->mkey;
	} else {
		ret = snap_prepare_crypto_ctx(crypto_ctx, SNAP_DMA_Q_CRYPTO_ON_SRC, io_attr, comp, n_bb);
		if (snap_unlikely(ret))
			goto free_ctx;

		dx_ctx->lbuf = 0;
		dx_ctx->lkey = crypto_ctx->l_klm_mkey->mkey;

		if (io_attr->riov_cnt == 1) {
			dx_ctx->raddr = (uint64_t)io_attr->riov[0].iov_base;
			dx_ctx->rkey = io_attr->rkey[0];
		} else {
			dx_ctx->raddr = (uint64_t)crypto_ctx->r_klm_mkey->addr;
			dx_ctx->rkey = crypto_ctx->r_klm_mkey->mkey;
		}
	}

	dx_ctx->len = io_attr->len;
	dx_ctx->comp = &crypto_ctx->comp;
	dx_ctx->use_fence = true;

	return 0;

free_ctx:
	snap_dma_q_crypto_ctx_free(crypto_ctx);
	return ret;
}

static int dv_dma_q_writec(struct snap_dma_q *q,
				struct snap_dma_q_io_attr *io_attr,
				struct snap_dma_completion *comp, int *n_bb)
{
	int ret;
	struct snap_dma_xfer_ctx dx_ctx = {0};

	ret = snap_prepare_dma_crypto_xfer_ctx(q, io_attr, comp, n_bb, &dx_ctx);
	if (ret)
		return ret;

	return do_dv_dma_xfer(q, dx_ctx.lbuf, dx_ctx.len, dx_ctx.lkey,
			dx_ctx.raddr, dx_ctx.rkey, MLX5_OPCODE_RDMA_WRITE, 0,
			dx_ctx.comp, dx_ctx.use_fence);
}

static int dv_dma_q_readc(struct snap_dma_q *q,
				struct snap_dma_q_io_attr *io_attr,
				struct snap_dma_completion *comp, int *n_bb)
{
	int ret;
	struct snap_dma_xfer_ctx dx_ctx = {0};

	ret = snap_prepare_dma_crypto_xfer_ctx(q, io_attr, comp, n_bb, &dx_ctx);
	if (ret)
		return ret;

	return do_dv_dma_xfer(q, dx_ctx.lbuf, dx_ctx.len, dx_ctx.lkey,
			dx_ctx.raddr, dx_ctx.rkey, MLX5_OPCODE_RDMA_READ, 0,
			dx_ctx.comp, dx_ctx.use_fence);
}

static int dv_dma_q_read_short(struct snap_dma_q *q, void *dst_buf,
			 size_t len, uint64_t srcaddr, uint32_t rmkey,
			 struct snap_dma_completion *comp)
{
	return do_dv_dma_xfer(q, dst_buf, len, 0, srcaddr, rmkey,
			MLX5_OPCODE_RDMA_READ, MLX5_WQE_CTRL_CQ_UPDATE, comp, false);
}

static int do_dv_dma_xfer_v2v(struct snap_dma_q *q,
				int wqe_cnt, int op, int *num_sge,
				struct ibv_sge (*l_sgl)[SNAP_DMA_Q_MAX_SGE_NUM],
				struct ibv_sge *r_sgl,
				struct snap_dma_completion *comp, int *n_bb)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint8_t fm_ce_se = 0;
	uint32_t pi, to_end, wqe_bb;
	int i, j;
	struct snap_dma_completion *c_comp;

	for (i = 0; i < wqe_cnt; i++) {
		if (i < wqe_cnt - 1)
			c_comp = NULL;
		else
			c_comp = comp;

		fm_ce_se = snap_dv_get_cq_update(dv_qp, c_comp);

		pi = dv_qp->hw_qp.sq.pi & (dv_qp->hw_qp.sq.wqe_cnt - 1);
		to_end = (dv_qp->hw_qp.sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB;

		ctrl = (struct mlx5_wqe_ctrl_seg *)snap_dv_get_wqe_bb(dv_qp);
		snap_set_ctrl_seg(ctrl, dv_qp->hw_qp.sq.pi, op, 0, dv_qp->hw_qp.qp_num,
				    fm_ce_se, 2 + num_sge[i], 0, 0);
		to_end -= sizeof(struct mlx5_wqe_ctrl_seg);

		rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
		rseg->raddr = htobe64((uintptr_t)r_sgl[i].addr);
		rseg->rkey  = htobe32(r_sgl[i].lkey);
		rseg->reserved = 0;
		to_end -= sizeof(struct mlx5_wqe_raddr_seg);

		dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
		for (j = 0; j < num_sge[i]; j++) {
			dseg->byte_count = htobe32(l_sgl[i][j].length);
			dseg->lkey = htobe32(l_sgl[i][j].lkey);
			dseg->addr = htobe64((uintptr_t)l_sgl[i][j].addr);

			to_end -= sizeof(struct mlx5_wqe_data_seg);
			if (to_end != 0) {
				dseg = dseg + 1;
			} else {
				dseg = (struct mlx5_wqe_data_seg *)(dv_qp->hw_qp.sq.addr);
				to_end = dv_qp->hw_qp.sq.wqe_cnt * MLX5_SEND_WQE_BB;
			}
		}

		wqe_bb = (num_sge[i] <= 2) ? 1 : 1 + round_up((num_sge[i] - 2), 4);

		dv_qp->hw_qp.sq.pi += (wqe_bb - 1);
		snap_dv_wqe_submit(dv_qp, ctrl);

		snap_dv_set_comp(dv_qp, pi, c_comp, fm_ce_se, wqe_bb);
	}

	return 0;
}

static int dv_dma_q_readv2v(struct snap_dma_q *q,
			    struct snap_dma_q_io_attr *io_attr,
			    struct snap_dma_completion *comp, int *n_bb)
{
	int wr_cnt;
	int num_sge[SNAP_DMA_Q_MAX_WR_CNT];
	struct ibv_sge r_sgl[SNAP_DMA_Q_MAX_WR_CNT];
	struct ibv_sge l_sgl[SNAP_DMA_Q_MAX_WR_CNT][SNAP_DMA_Q_MAX_SGE_NUM];

	if (snap_dma_build_sgl(io_attr, &wr_cnt, n_bb, num_sge, l_sgl, r_sgl))
		return -EINVAL;

	if (snap_unlikely(!qp_can_tx(q, *n_bb))) {
		SNAP_LIB_LOG_DBG("%s: qp out of tx_available resource", __func__);
		return -EAGAIN;
	}

	/*
	 * Post RDMA read with V2V is broken when TX Scatter-to-CQE is enabled,
	 * so as a WA post RDMA write and replace at the caller between source
	 * and destination.
	 */
	return do_dv_dma_xfer_v2v(q, wr_cnt,
				MLX5_OPCODE_RDMA_WRITE, num_sge,
				l_sgl, r_sgl, comp, n_bb);
}

static int dv_dma_q_writev2v(struct snap_dma_q *q,
				struct snap_dma_q_io_attr *io_attr,
				struct snap_dma_completion *comp, int *n_bb)
{
	int wr_cnt;
	int num_sge[SNAP_DMA_Q_MAX_WR_CNT];
	struct ibv_sge r_sgl[SNAP_DMA_Q_MAX_WR_CNT];
	struct ibv_sge l_sgl[SNAP_DMA_Q_MAX_WR_CNT][SNAP_DMA_Q_MAX_SGE_NUM];

	if (snap_dma_build_sgl(io_attr, &wr_cnt, n_bb, num_sge, l_sgl, r_sgl))
		return -EINVAL;

	if (snap_unlikely(!qp_can_tx(q, *n_bb))) {
		SNAP_LIB_LOG_DBG("%s: qp out of tx_available resource", __func__);
		return -EAGAIN;
	}

	return do_dv_dma_xfer_v2v(q, wr_cnt,
				MLX5_OPCODE_RDMA_WRITE, num_sge,
				l_sgl, r_sgl, comp, n_bb);
}

static inline int do_dv_xfer_inline(struct snap_dma_q *q, void *src_buf, size_t len,
				    int op, uint64_t raddr, uint32_t rkey,
				    struct snap_dma_completion *flush_comp, int *n_bb, int is_flush)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	uint8_t fm_ce_se = 0;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_inl_data_seg *dseg;
	struct mlx5_wqe_raddr_seg *rseg;
	uint16_t pi, wqe_size;
	size_t to_end;
	void *pdata;

	wqe_size = sizeof(*ctrl) + sizeof(*dseg) + len;
	if (op == MLX5_OPCODE_RDMA_WRITE)
		wqe_size += sizeof(*rseg);

	/* if is_flush is set it means that we are dealing with the zero
	 * length rdma_write op. Check is_flush instead of length to allow
	 * optimization in the fast path where the is_flush is always zero.
	 */
	if (is_flush)
		wqe_size -= sizeof(*dseg);

	*n_bb = round_up(wqe_size, MLX5_SEND_WQE_BB);
	if (snap_unlikely(!qp_can_tx(q, *n_bb)))
		return -EAGAIN;

	if (is_flush)
		fm_ce_se |= MLX5_WQE_CTRL_CQ_UPDATE;
	else
		fm_ce_se |= snap_dv_get_cq_update(dv_qp, flush_comp);

	ctrl = (struct mlx5_wqe_ctrl_seg *)snap_dv_get_wqe_bb(dv_qp);
	snap_set_ctrl_seg(ctrl, dv_qp->hw_qp.sq.pi, op, 0,
			    dv_qp->hw_qp.qp_num, fm_ce_se,
			    round_up(wqe_size, 16), 0, 0);

	if (op == MLX5_OPCODE_RDMA_WRITE) {
		rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
		rseg->raddr = htobe64((uintptr_t)raddr);
		rseg->rkey = htobe32(rkey);
		dseg = (struct mlx5_wqe_inl_data_seg *)(rseg + 1);
	} else
		dseg = (struct mlx5_wqe_inl_data_seg *)(ctrl + 1);

	dseg->byte_count = htobe32(len | MLX5_INLINE_SEG);
	pdata = dseg + 1;

	/* handle wrap around, where inline data needs several building blocks */
	pi = dv_qp->hw_qp.sq.pi & (dv_qp->hw_qp.sq.wqe_cnt - 1);
	to_end = (dv_qp->hw_qp.sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB -
		 sizeof(*ctrl) - sizeof(*dseg);
	if (op == MLX5_OPCODE_RDMA_WRITE)
		to_end -= sizeof(*rseg);

	/*
	 * is_flush can be tested in compilation time, while src_buf isn't,
	 * so we better use it in fastpath. We rely on the fact that we always
	 * use this function either with src_buf or with is_flush, but never
	 * with both.
	 */
#ifdef __COVERITY__
	if (src_buf) {
#else
	if (!is_flush) {
#endif
		if (snap_unlikely(len > to_end)) {
			memcpy(pdata, src_buf, to_end);
			memcpy((void *)dv_qp->hw_qp.sq.addr, src_buf + to_end, len - to_end);
		} else {
			memcpy(pdata, src_buf, len);
		}
	}

	dv_qp->hw_qp.sq.pi += (*n_bb - 1);

	snap_dv_wqe_submit(dv_qp, ctrl);

	snap_dv_set_comp(dv_qp, pi, flush_comp, fm_ce_se, *n_bb);
	return 0;
}

static int dv_dma_q_send_completion(struct snap_dma_q *q, void *src_buf,
				    size_t len, int *n_bb)
{
	return do_dv_xfer_inline(q, src_buf, len, MLX5_OPCODE_SEND, 0, 0, NULL, n_bb, 0);
}

static inline int do_dv_send(struct snap_dma_q *q, void *in_buf, size_t in_len,
				    uint64_t addr, int len, uint32_t key,
				    int *n_bb, int op, uint32_t imm)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	uint16_t complement = 0;
	uint8_t fm_ce_se = 0;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_inl_data_seg *in_seg;
	struct mlx5_wqe_data_seg *data_seg;
	uint16_t pi, wqe_size;
	size_t to_end;
	void *pdata;

	/* Every inline data segment occupies one or more octowords */
	if ((sizeof(*in_seg) + in_len) % 16)
		complement = 16 - ((sizeof(*in_seg) + in_len) % 16);
	wqe_size = sizeof(*ctrl) + sizeof(*data_seg) + sizeof(*in_seg) + in_len	+
			complement;

	*n_bb = round_up(wqe_size, MLX5_SEND_WQE_BB);

	if (snap_unlikely(!qp_can_tx(q, *n_bb)))
		return -EAGAIN;

	fm_ce_se |= snap_dv_get_cq_update(dv_qp, NULL);

	ctrl = (struct mlx5_wqe_ctrl_seg *)snap_dv_get_wqe_bb(dv_qp);
	snap_set_ctrl_seg(ctrl, dv_qp->hw_qp.sq.pi, op, 0,
			dv_qp->hw_qp.qp_num, fm_ce_se,
			    round_up(wqe_size, 16), 0, imm);

	in_seg = (struct mlx5_wqe_inl_data_seg *)(ctrl + 1);
	in_seg->byte_count = htobe32(in_len | MLX5_INLINE_SEG);
	pdata = in_seg + 1;

	/* handle wrap around, where inline data needs several building blocks */
	pi = dv_qp->hw_qp.sq.pi & (dv_qp->hw_qp.sq.wqe_cnt - 1);
	to_end = (dv_qp->hw_qp.sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB -
		 sizeof(*ctrl) - sizeof(*in_seg);

	if (snap_unlikely((in_len + complement + sizeof(*data_seg)) > to_end)) {
		memcpy(pdata, in_buf, to_end);
		memcpy((void *)dv_qp->hw_qp.sq.addr, in_buf + to_end, in_len - to_end);
		data_seg = (struct mlx5_wqe_data_seg *)
				((void *)dv_qp->hw_qp.sq.addr + in_len - to_end + complement);
		mlx5dv_set_data_seg(data_seg, len, key, (uintptr_t)addr);
	} else {
		memcpy(pdata, in_buf, in_len);
		data_seg = (struct mlx5_wqe_data_seg *)(pdata + in_len + complement);
		mlx5dv_set_data_seg(data_seg, len, key, (uintptr_t)addr);
	}

	dv_qp->hw_qp.sq.pi += (*n_bb - 1);

	snap_dv_wqe_submit(dv_qp, ctrl);

	snap_dv_set_comp(dv_qp, pi, NULL, fm_ce_se, *n_bb);
	return 0;
}

static inline int dv_dma_q_send(struct snap_dma_q *q, void *in_buf, size_t in_len,
				    uint64_t addr, int len, uint32_t key,
				    int *n_bb, uint32_t *imm)
{
	if (imm)
		return do_dv_send(q, in_buf, in_len, addr, len, key, n_bb, MLX5_OPCODE_SEND_IMM, *imm);
	else
		return do_dv_send(q, in_buf, in_len, addr, len, key, n_bb, MLX5_OPCODE_SEND, 0);
}

static int dv_dma_q_write_short(struct snap_dma_q *q, void *src_buf, size_t len,
				uint64_t dstaddr, uint32_t rmkey, int *n_bb)
{
	return do_dv_xfer_inline(q, src_buf, len, MLX5_OPCODE_RDMA_WRITE,
			dstaddr, rmkey, NULL, n_bb, 0);
}

static const char *snap_dv_cqe_err_opcode(struct mlx5_err_cqe *ecqe)
{
	uint8_t wqe_err_opcode = be32toh(ecqe->s_wqe_opcode_qpn) >> 24;

	switch (ecqe->op_own >> 4) {
	case MLX5_CQE_REQ_ERR:
		switch (wqe_err_opcode) {
		case MLX5_OPCODE_RDMA_WRITE_IMM:
		case MLX5_OPCODE_RDMA_WRITE:
			return "RDMA_WRITE";
		case MLX5_OPCODE_SEND_IMM:
		case MLX5_OPCODE_SEND:
		case MLX5_OPCODE_SEND_INVAL:
			return "SEND";
		case MLX5_OPCODE_RDMA_READ:
			return "RDMA_READ";
		case MLX5_OPCODE_ATOMIC_CS:
			return "COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_FA:
			return "FETCH_ADD";
		case MLX5_OPCODE_ATOMIC_MASKED_CS:
			return "MASKED_COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_MASKED_FA:
			return "MASKED_FETCH_ADD";
		case MLX5_OPCODE_MMO:
			return "GGA_DMA";
		default:
			return "";
			}
	case MLX5_CQE_RESP_ERR:
		return "RECV";
	default:
		return "";
	}
}

static void snap_dv_cqe_err(struct mlx5_cqe64 *cqe)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;
	uint16_t wqe_counter;
	uint32_t qp_num = 0;
	char info[200] = {0};

	wqe_counter = be16toh(ecqe->wqe_counter);
	qp_num = be32toh(ecqe->s_wqe_opcode_qpn) & ((1<<24)-1);

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR) {
		SNAP_LIB_LOG_DBG("QP 0x%x wqe[%d] is flushed", qp_num, wqe_counter);
		return;
	}

	switch (ecqe->syndrome) {
	case MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		snprintf(info, sizeof(info), "Local length");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		snprintf(info, sizeof(info), "Local QP operation");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_PROT_ERR:
		snprintf(info, sizeof(info), "Local protection");
		break;
	case MLX5_CQE_SYNDROME_WR_FLUSH_ERR:
		snprintf(info, sizeof(info), "WR flushed because QP in error state");
		break;
	case MLX5_CQE_SYNDROME_MW_BIND_ERR:
		snprintf(info, sizeof(info), "Memory window bind");
		break;
	case MLX5_CQE_SYNDROME_BAD_RESP_ERR:
		snprintf(info, sizeof(info), "Bad response");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		snprintf(info, sizeof(info), "Local access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		snprintf(info, sizeof(info), "Invalid request");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		snprintf(info, sizeof(info), "Remote access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_OP_ERR:
		snprintf(info, sizeof(info), "Remote QP");
		break;
	case MLX5_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Transport retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Receive-no-ready retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ABORTED_ERR:
		snprintf(info, sizeof(info), "Remote side aborted");
		break;
	default:
		snprintf(info, sizeof(info), "Generic");
		break;
	}
	SNAP_LIB_LOG_ERR("Error on QP 0x%x wqe[%03d]: %s (synd 0x%x vend 0x%x) opcode %s",
		   qp_num, wqe_counter, info, ecqe->syndrome, ecqe->vendor_err_synd,
		   snap_dv_cqe_err_opcode(ecqe));
}

static inline struct snap_dma_completion *dv_dma_q_get_comp(struct snap_dma_q *q, struct mlx5_cqe64 *cqe)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	uint16_t comp_idx;
	uint32_t sq_mask;

	sq_mask = dv_qp->hw_qp.sq.wqe_cnt - 1;
	comp_idx = be16toh(cqe->wqe_counter) & sq_mask;
	q->tx_available += dv_qp->comps[comp_idx].n_outstanding;

	if ((cqe->op_own & MLX5_INLINE_SCATTER_32) && dv_qp->comps[comp_idx].read_payload) {
		memcpy(dv_qp->comps[comp_idx].read_payload, (void *)cqe, be32toh(cqe->byte_cnt));
		dv_qp->comps[comp_idx].read_payload = NULL;
	}

	return dv_qp->comps[comp_idx].comp;
}

static inline int dv_dma_q_progress_tx(struct snap_dma_q *q, int max_tx_comp)
{
	uint16_t max_tx_comp_value = max_tx_comp == -1 ? SNAP_DMA_MAX_TX_COMPLETIONS : max_tx_comp;
	struct mlx5_cqe64 *cqe[max_tx_comp_value];
	struct snap_dma_completion *comp[max_tx_comp_value];
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	int n, i;
	uint8_t opcode;

	n = 0;
	if (max_tx_comp_value == 0)
		goto out;

	do {
		cqe[n] = snap_dv_poll_cq(&q->sw_qp.dv_tx_cq, SNAP_DMA_Q_TX_CQE_SIZE);
		if (!cqe[n])
			break;

		if (snap_unlikely(mlx5dv_get_cqe_opcode(cqe[n]) != MLX5_CQE_REQ)) {

			if (q->dv_err_cb) {
				int ret = q->dv_err_cb(q, cqe[n]);

				if (ret == SNAP_DMA_Q_ERR_HANDLED) {
					/* only need to update available, completion
					 * itself is processed by the error handler
					 */
					(void)dv_dma_q_get_comp(q, cqe[n]);
					continue;
				} else
					snap_dv_cqe_err(cqe[n]);
			} else
				snap_dv_cqe_err(cqe[n]);
		}

		comp[n] = dv_dma_q_get_comp(q, cqe[n]);
		n++;
	} while (n < max_tx_comp_value);

	for (i = 0; i < n; i++) {
		if (comp[i] && --comp[i]->count == 0) {
			if (snap_unlikely(mlx5dv_get_cqe_opcode(cqe[i]) != MLX5_CQE_REQ)) {
				struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe[i];

				opcode = ecqe->syndrome;
			} else
				opcode = 0;

			comp[i]->func(comp[i], opcode);
		}
	}

out:
	snap_dv_tx_complete(dv_qp);
	dv_qp->stat.tx.total_completed += n;
	return n;
}

static inline void dv_dma_q_complete_tx(struct snap_dma_q *q)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;

	snap_dv_tx_complete(dv_qp);
}

static inline void dv_dma_q_get_rx_comp(struct snap_dma_q *q, struct mlx5_cqe64 *cqe, struct snap_rx_completion *rx_comp)
{
	int ri;
	uint32_t rq_mask;

	/* optimize for NVMe where SQE is 64 bytes and will always
	 * be scattered
	 **/
	rq_mask = q->sw_qp.dv_qp.hw_qp.rq.wqe_cnt - 1;
	if (snap_likely(cqe->op_own & MLX5_INLINE_SCATTER_64)) {
		rx_comp->data = cqe - 1;
	} else if (cqe->op_own & MLX5_INLINE_SCATTER_32) {
		rx_comp->data = cqe;
	} else {
		ri = be16toh(cqe->wqe_counter) & rq_mask;
		__builtin_prefetch(q->sw_qp.rx_buf + ri * q->rx_elem_size);
		rx_comp->data = q->sw_qp.rx_buf + ri * q->rx_elem_size;
	}
	rx_comp->q = q;
	rx_comp->byte_len = be32toh(cqe->byte_cnt);
	rx_comp->imm_data = cqe->imm_inval_pkey;
}

static inline int dv_dma_q_progress_rx(struct snap_dma_q *q)
{
	struct mlx5_cqe64 *cqe[SNAP_DMA_MAX_RX_COMPLETIONS];
	int n, i;
	int op;
	struct snap_rx_completion rx_comp[SNAP_DMA_MAX_RX_COMPLETIONS];

	n = 0;
	do {
		cqe[n] = snap_dv_poll_cq(&q->sw_qp.dv_rx_cq, SNAP_DMA_Q_RX_CQE_SIZE);
		if (!cqe[n])
			break;

		op = mlx5dv_get_cqe_opcode(cqe[n]);
		if (snap_unlikely(op != MLX5_CQE_RESP_SEND &&
				  op != MLX5_CQE_RESP_SEND_IMM)) {
			snap_dv_cqe_err(cqe[n]);
			return n;
		}


		dv_dma_q_get_rx_comp(q, cqe[n], &rx_comp[n]);
		n++;
	} while (n < SNAP_DMA_MAX_RX_COMPLETIONS);

	snap_memory_cpu_load_fence();

	for (i = 0; i < n; i++)
		q->rx_cb(q, rx_comp[i].data, rx_comp[i].byte_len, rx_comp[i].imm_data);

	if (n == 0)
		return 0;

	q->sw_qp.dv_qp.hw_qp.rq.ci += n;
	snap_dv_ring_rx_db(&q->sw_qp.dv_qp);
	q->sw_qp.dv_qp.stat.rx.total_completed += n;
	return n;
}

static inline int dv_dma_q_poll_rx(struct snap_dma_q *q,
		struct snap_rx_completion *rx_completions, int max_completions)
{
	struct mlx5_cqe64 *cqe;
	int n;
	int op;

	n = 0;
	do {
		cqe = snap_dv_poll_cq(&q->sw_qp.dv_rx_cq, SNAP_DMA_Q_RX_CQE_SIZE);
		if (!cqe)
			break;

		op = mlx5dv_get_cqe_opcode(cqe);
		if (snap_unlikely(op != MLX5_CQE_RESP_SEND &&
				  op != MLX5_CQE_RESP_SEND_IMM)) {
			snap_dv_cqe_err(cqe);
			return n;
		}

		dv_dma_q_get_rx_comp(q, cqe, &rx_completions[n]);
		n++;
	} while (n < max_completions);

	if (n == 0)
		return 0;

	snap_memory_cpu_load_fence();

	q->sw_qp.dv_qp.hw_qp.rq.ci += n;
	snap_dv_ring_rx_db(&q->sw_qp.dv_qp);
	return n;
}

static inline int dv_dma_q_poll_tx(struct snap_dma_q *q, struct snap_dma_completion **comp, int max_completions)
{
	struct mlx5_cqe64 *cqe;
	int n;
	struct snap_dma_completion *dma_comp;
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;

	snap_dv_tx_complete(dv_qp);

	n = 0;
	do {
		cqe = snap_dv_poll_cq(&q->sw_qp.dv_tx_cq, SNAP_DMA_Q_TX_CQE_SIZE);
		if (!cqe)
			break;
		if (snap_unlikely(mlx5dv_get_cqe_opcode(cqe) != MLX5_CQE_REQ))
			snap_dv_cqe_err(cqe);

		dma_comp = dv_dma_q_get_comp(q, cqe);
		if (dma_comp && --dma_comp->count == 0) {
			comp[n] = dma_comp;
			n++;
		}

	} while (n < max_completions);

	return n;
}

static int dv_dma_q_arm(struct snap_dma_q *q)
{
	/* ring doorbells, disable batch mode.
	 * TODO: better interaction of batch and event modes
	 */
	snap_dv_tx_complete(&q->sw_qp.dv_qp);
	q->sw_qp.dv_qp.db_flag = SNAP_DB_RING_IMM;
	if (q->sw_qp.dv_tx_cq.cqe_cnt)
		snap_dv_arm_cq(&q->sw_qp.dv_tx_cq);
	if (q->sw_qp.dv_rx_cq.cqe_cnt)
		snap_dv_arm_cq(&q->sw_qp.dv_rx_cq);
	return 0;
}

static int dv_dma_q_flush(struct snap_dma_q *q)
{
	int n, n_out, n_bb;
	int tx_available;

	n = 0;
	/* in case we have tx moderation we need at least one
	 * available to be able to send a flush command
	 */
	while (!qp_can_tx(q, 1))
		n += dv_dma_q_progress_tx(q, -1);

	/* flush all outstanding ops by issuing a zero length inline rdma write */
	n_out = q->sw_qp.dv_qp.n_outstanding;
	if (n_out) {
		do_dv_xfer_inline(q, NULL, 0, MLX5_OPCODE_RDMA_WRITE, 0, 0, NULL, &n_bb, 1);
		q->tx_available -= n_bb;
		n--;
	}

	tx_available = snap_dma_q_dv_get_tx_avail_max(q);
	while (q->tx_available < tx_available)
		n += dv_dma_q_progress_tx(q, -1);

	return n_out + n;
}

static int dv_dma_q_flush_nowait(struct snap_dma_q *q, struct snap_dma_completion *comp, int *n_bb)
{
	return do_dv_xfer_inline(q, NULL, 0, MLX5_OPCODE_RDMA_WRITE, 0, 0, comp, n_bb, 1);
}

static bool dv_dma_q_empty(struct snap_dma_q *q)
{
	return q->tx_available == snap_dma_q_dv_get_tx_avail_max(q);
}

static const struct snap_dv_qp_stat *dv_dma_q_stat(const struct snap_dma_q *q)
{
	return &q->sw_qp.dv_qp.stat;
}

const struct snap_dma_q_ops dv_ops = {
	.mode            = SNAP_DMA_Q_MODE_DV,
	.write           = dv_dma_q_write,
	.writev2v        = dv_dma_q_writev2v,
	.writec          = dv_dma_q_writec,
	.write_short     = dv_dma_q_write_short,
	.read            = dv_dma_q_read,
	.readv2v         = dv_dma_q_readv2v,
	.readc           = dv_dma_q_readc,
	.read_short      = dv_dma_q_read_short,
	.send_completion = dv_dma_q_send_completion,
	.send            = dv_dma_q_send,
	.progress_tx     = dv_dma_q_progress_tx,
	.complete_tx     = dv_dma_q_complete_tx,
	.progress_rx     = dv_dma_q_progress_rx,
	.poll_rx         = dv_dma_q_poll_rx,
	.poll_tx         = dv_dma_q_poll_tx,
	.arm             = dv_dma_q_arm,
	.flush           = dv_dma_q_flush,
	.flush_nowait    = dv_dma_q_flush_nowait,
	.empty           = dv_dma_q_empty,
	.stat            = dv_dma_q_stat,
};

/* GGA */
__attribute__((unused)) static void dump_gga_wqe(int op, uint32_t *wqe)
{
	int i;

	printf("%s op %d wqe:\n", __func__, op);

	for (i = 0; i < 16; i += 4)
		printf("%08X %08X %08X %08X\n",
			be32toh(wqe[i]), be32toh(wqe[i + 1]),
			be32toh(wqe[i + 2]), be32toh(wqe[i + 3]));
}

static inline int do_gga_dma_xfer(struct snap_dma_q *q, uint64_t saddr, size_t len,
			      uint32_t s_lkey, uint64_t daddr, uint32_t d_lkey,
			      struct snap_dma_completion *comp, bool use_fence)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	struct mlx5_dma_wqe *gga_wqe;
	/* struct mlx5_wqe_ctrl_seg changed to packed(4),
	 * and struct mlx5_dma_wqe is use default packed attribute, which is 8.
	 * in order to fix the compile issue on UB OS, make `ctrl` to void*,
	 * and convert it to struct mlx5_wqe_ctrl_seg * when it is needed.
	 */
	void *ctrl;
	uint16_t comp_idx;
	uint8_t fm_ce_se = 0;

	comp_idx = dv_qp->hw_qp.sq.pi & (dv_qp->hw_qp.sq.wqe_cnt - 1);

	fm_ce_se |= snap_dv_get_cq_update(dv_qp, comp);
	if (use_fence)
		fm_ce_se |= MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;

	ctrl = snap_dv_get_wqe_bb(dv_qp);
	snap_set_ctrl_seg((struct mlx5_wqe_ctrl_seg *)ctrl, dv_qp->hw_qp.sq.pi,
			    MLX5_OPCODE_MMO, MLX5_OPC_MOD_MMO_DMA,
			    dv_qp->hw_qp.qp_num, fm_ce_se,
			    4, 0, 0);

	gga_wqe = (struct mlx5_dma_wqe *)ctrl;
	gga_wqe->gga_ctrl2 = 0;
	gga_wqe->opaque_lkey = dv_qp->opaque_lkey;
	gga_wqe->opaque_vaddr = htobe64((uint64_t)dv_qp->opaque_buf);

	mlx5dv_set_data_seg(&gga_wqe->gather, len, s_lkey, saddr);
	mlx5dv_set_data_seg(&gga_wqe->scatter, len, d_lkey, daddr);

	snap_dv_wqe_submit(dv_qp, ctrl);

	snap_dv_set_comp(dv_qp, comp_idx, comp, fm_ce_se, 1);
	return 0;
}

static int gga_dma_q_write(struct snap_dma_q *q, void *src_buf, size_t len,
			  uint32_t lkey, uint64_t dstaddr, uint32_t rmkey,
			  struct snap_dma_completion *comp)
{
	return do_gga_dma_xfer(q, (uint64_t)src_buf, len, lkey,
			dstaddr, rmkey, comp, false);
}

static int gga_dma_q_read(struct snap_dma_q *q, void *dst_buf, size_t len,
			 uint32_t lkey, uint64_t srcaddr, uint32_t rmkey,
			 struct snap_dma_completion *comp)
{
	return do_gga_dma_xfer(q, srcaddr, len, rmkey,
			(uint64_t)dst_buf, lkey, comp, false);
}

static int gga_dma_q_writec(struct snap_dma_q *q,
				struct snap_dma_q_io_attr *io_attr,
				struct snap_dma_completion *comp, int *n_bb)
{
	return -ENOTSUP;
}

static int gga_dma_q_readc(struct snap_dma_q *q,
				struct snap_dma_q_io_attr *io_attr,
				struct snap_dma_completion *comp, int *n_bb)
{
	return -ENOTSUP;
}

const struct snap_dma_q_ops gga_ops = {
	.mode            = SNAP_DMA_Q_MODE_GGA,
	.write           = gga_dma_q_write,
	.writev2v        = dv_dma_q_writev2v,
	.writec          = gga_dma_q_writec,
	.write_short     = dv_dma_q_write_short,
	.read            = gga_dma_q_read,
	.readv2v         = dv_dma_q_readv2v,
	.readc           = gga_dma_q_readc,
	.read_short      = dv_dma_q_read_short,
	.send_completion = dv_dma_q_send_completion,
	.send            = dv_dma_q_send,
	.progress_tx     = dv_dma_q_progress_tx,
	.complete_tx     = dv_dma_q_complete_tx,
	.progress_rx     = dv_dma_q_progress_rx,
	.poll_rx         = dv_dma_q_poll_rx,
	.poll_tx         = dv_dma_q_poll_tx,
	.arm             = dv_dma_q_arm,
	.flush           = dv_dma_q_flush,
	.flush_nowait    = dv_dma_q_flush_nowait,
	.empty           = dv_dma_q_empty,
	.stat            = dv_dma_q_stat,
};

int dv_worker_progress_rx(struct snap_dma_worker *wk)
{
	int n, i;
	int op, qp_id;
	struct snap_dma_q *q;
	struct mlx5_cqe64 *cqe;
	struct snap_rx_completion rx_comp[SNAP_DMA_MAX_RX_COMPLETIONS];

	n = 0;
	do {
		cqe = snap_dv_poll_cq(&wk->dv_rx_cq, SNAP_DMA_Q_RX_CQE_SIZE);
		if (!cqe)
			break;

		qp_id = be32toh(cqe->srqn_uidx);
		q = wk->queues[qp_id];

		/* Should not happen as long as we flush dma queues before destroying them */
		if (snap_unlikely(!q)) {
			snap_debug("%s: Queue %d is not valid, dropping CQE\n", __func__, qp_id);
			continue;
		}

		op = mlx5dv_get_cqe_opcode(cqe);
		if (snap_unlikely(op != MLX5_CQE_RESP_SEND && op != MLX5_CQE_RESP_SEND_IMM)) {
			snap_dv_cqe_err(cqe);

			if (++q->flush_count == q->rx_qsize*SNAP_DMA_Q_POST_RECV_BUF_FACTOR)
				q->destroy_done = true;

			break;
		}

		dv_dma_q_get_rx_comp(q, cqe, &rx_comp[n]);
		n++;
	} while (n < SNAP_DMA_MAX_RX_COMPLETIONS);

	/* Return from here as it will save from unnecessary load / store fence */
	if (n == 0)
		return n;

	snap_memory_cpu_load_fence();

	for (i = 0; i < n; i++) {
		q = rx_comp[i].q;
		q->rx_cb(q, rx_comp[i].data, rx_comp[i].byte_len, rx_comp[i].imm_data);
		q->sw_qp.dv_qp.hw_qp.rq.ci++;
		snap_dv_update_rx_db(&q->sw_qp.dv_qp);
	}
	snap_memory_bus_store_fence();

	return n;
}

static inline void dv_worker_ring_all_doorbells(struct snap_dma_worker *wk)
{
	struct snap_dma_q *q;

	SLIST_FOREACH(q, &wk->pending_dbs, entry)
		snap_dv_update_tx_db(&q->sw_qp.dv_qp);

	snap_memory_bus_store_fence();

	SLIST_FOREACH(q, &wk->pending_dbs, entry) {
		struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;

		dv_qp->tx_need_ring_db = false;
		snap_dv_flush_tx_db(dv_qp, dv_qp->ctrl);
#if !defined(__aarch64__)
	if (!dv_qp->hw_qp.sq.tx_db_nc)
		snap_memory_bus_store_fence();
#endif
	}

	SLIST_INIT(&wk->pending_dbs);
}

int dv_worker_progress_tx(struct snap_dma_worker *wk)
{
	struct mlx5_cqe64 *cqe[SNAP_DMA_MAX_TX_COMPLETIONS];
	struct snap_dma_completion *comp[SNAP_DMA_MAX_TX_COMPLETIONS];
	struct snap_dma_q *q;
	int n, i, cqe_id;

	n = 0;

	do {
		cqe[n] = snap_dv_poll_cq(&wk->dv_tx_cq, SNAP_DMA_Q_TX_CQE_SIZE);
		if (!cqe[n])
			break;

		if (snap_unlikely(mlx5dv_get_cqe_opcode(cqe[n]) != MLX5_CQE_REQ))
			snap_dv_cqe_err(cqe[n]);

		cqe_id = be32toh(cqe[n]->srqn_uidx);

		q = wk->queues[cqe_id];
		comp[n] = dv_dma_q_get_comp(q, cqe[n]);
		n++;
	} while (n < SNAP_DMA_MAX_TX_COMPLETIONS);

	for (i = 0; i < n; i++) {
		if (comp[i] && --comp[i]->count == 0)
			comp[i]->func(comp[i], mlx5dv_get_cqe_opcode(cqe[i]));
	}

	dv_worker_ring_all_doorbells(wk);

	return n;
}

static int worker_flush_helper(struct snap_dma_q *q)
{
	int n_bb, n_out;

	/* flush all outstanding ops by issuing a zero length inline rdma write */
	n_out = q->sw_qp.dv_qp.n_outstanding;
	if (n_out) {
		do_dv_xfer_inline(q, NULL, 0, MLX5_OPCODE_RDMA_WRITE, 0, 0, NULL, &n_bb, 1);
		q->tx_available -= n_bb;
		n_out--;
	}

	return n_out;
}

int dv_worker_flush(struct snap_dma_worker *wk)
{
	int n, tx_available, i;
	struct snap_dma_q *q;

	n = 0;
	/* in case we have tx moderation we need at least one
	 * available to be able to send a flush command
	 */
	while (!worker_qps_can_tx(wk, 1))
		n += dv_worker_progress_tx(wk);

	for (i = 0 ; i < wk->max_queues; i++) {
		if (snap_unlikely(!wk->queues[i]))
			continue;
		q = wk->queues[i];
		n += worker_flush_helper(q);
	}

	for (i = 0 ; i < wk->max_queues; i++) {
		if (snap_unlikely(wk->queues[i]))
			continue;
		/* api is highly experimental and not in use at the moment */
		SNAP_LIB_LOG_ERR("WORKER FLUSH NEEDS TO BE FIXED");
		tx_available = wk->queues[i]->sw_qp.dv_qp.hw_qp.sq.wqe_cnt;
		while (wk->queues[i]->tx_available < tx_available)
			n += dv_worker_progress_tx(wk);
	}

	return n;
}
