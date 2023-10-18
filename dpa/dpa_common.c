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
#include <stdarg.h>

#include "dpa.h"
#include "snap_dma.h"
#include "snap_dma_internal.h"
#include "snap_dpa_rt.h"

/**
 * dpa_dma_ep_init() - initialize dma_q endpoint on DPA
 * @tcb: thread control block
 * @q: dpa endpoint address in dpa memory
 * @dummy_rq: enable a memory optimization if rq data isn't used
 *
 * The function takes endpoint that was created with snap_dma_ep_create()
 * and prepares it for use on DPA
 */
int dpa_dma_ep_init(struct snap_dma_q *q, bool dummy_rq)
{
	uint32_t sq_wqe_cnt = q->sw_qp.dv_qp.hw_qp.sq.wqe_cnt;
	uint32_t rq_wqe_cnt = q->sw_qp.dv_qp.hw_qp.rq.wqe_cnt;
	int i;

	/* This is an example how to use thread allocator. It is definitely
	 * possible to preinit buffer on DPU and copy to device memory as
	 * we are doing in snap_cq.
	 */
	/* If qp has non zero rx post receives */
	if (rq_wqe_cnt) {
		q->sw_qp.rx_buf = dpa_thread_alloc(dummy_rq ? q->rx_elem_size : 2 * rq_wqe_cnt * q->rx_elem_size);
		for (i = 0; i < 2 * rq_wqe_cnt; i++) {
			snap_dv_post_recv(&q->sw_qp.dv_qp,
					dummy_rq ? q->sw_qp.rx_buf : q->sw_qp.rx_buf + i * q->rx_elem_size,
					q->rx_elem_size, snap_dma_q_dpa_mkey(q));
			snap_dv_ring_rx_db(&q->sw_qp.dv_qp);
		}
	}

	/* setup completion memory */
	if (sq_wqe_cnt) {
		q->sw_qp.dv_qp.comps = dpa_thread_alloc(sq_wqe_cnt * sizeof(struct snap_dv_dma_completion));
		memset(q->sw_qp.dv_qp.comps, 0, sq_wqe_cnt * sizeof(struct snap_dv_dma_completion));
	}

	/* Setup a correct ops pointer */
	q->ops = &dv_ops;
	return 0;
}

struct snap_dma_q *dpa_dma_ep_cmd_copy(struct snap_dpa_cmd *cmd, bool dummy_rq)
{
	struct snap_dma_ep_copy_cmd *ep_cmd = (struct snap_dma_ep_copy_cmd *)cmd;
	struct snap_dma_q *q;

	q = dpa_thread_alloc(sizeof(*q));
	memcpy(q, &ep_cmd->q, sizeof(*q));
	dpa_dma_ep_init(q, dummy_rq);
	return q;
}

/**
 * dpa_thread_alloc() - allocate memory on thread heap
 * @size: amount of memory to allocate
 *
 * The function implements sbrk() like memory allocator. It reserves requested
 * amount of memory on per thread heap.
 *
 * The function never returns NULL
 *
 * TODO: error checking
 *
 * Return:
 * pointer to the reserved memory, fatal event on failure
 */
void *dpa_thread_alloc(size_t size)
{
	struct snap_dpa_tcb *tcb = dpa_tcb();
	void *data_add = (void *) tcb->data_address + tcb->data_used;

	//size is rounded up to cache line (64 bytes)
	if (size % DPA_CACHE_LINE_BYTES)
		tcb->data_used += size + (DPA_CACHE_LINE_BYTES - (size % DPA_CACHE_LINE_BYTES));
	else
		tcb->data_used += size;

	if (tcb->data_used > tcb->heap_size)
		dpa_fatal("thread alloc: OOM: want %d bytes, will use %d which is more than %d bytes\n",
				size, tcb->data_used, tcb->heap_size);

	dpa_debug("thread alloc: addr %p wanted %ld total used %ld\n", data_add, size, tcb->data_used);
	return data_add;
}

/**
 * dpa_thread_free() - free memory on the per thread heap
 * @addr: address of memory block to free
 *
 * The function does nothing at the moment. In the future it may
 * set heap top to the @addr.
 */
void dpa_thread_free(void *addr)
{
}

inline bool is_event_mode()
{
	return dpa_tcb()->user_flag == SNAP_DPA_RT_THR_EVENT;
}

inline int dpa_p2p_recv(struct snap_dpa_p2p_q *p2p_q)
{
	struct snap_dpa_p2p_msg *msgs[16];
	int n, msix_count;

	/* cq shall be armed before it is polled. See man ibv_get_cq_event */
	if (is_event_mode())
		snap_dv_arm_cq(&p2p_q->dma_q->sw_qp.dv_rx_cq);

	msix_count = 0;
	do {
		n = snap_dpa_p2p_recv_msg(p2p_q, msgs, 16);
		if (n)
			snap_debug("recv %d new messages", n);
		msix_count += n;
	} while (n != 0);

	if (msix_count == 0)
		return 0;

#if 0
	/* at the moment we are only getting msix messages for the one queue. no need to parse */
	for (msix_count = i = 0; i < n; i++) {
		rt_ctx->dpa_cmd_chan.credit_count += msgs[i]->base.credit_delta;
		if (msgs[i]->base.type == SNAP_DPA_P2P_MSG_CR_UPDATE) {
			//cr_update = 1;
			continue;
		}

		if (msgs[i]->base.type != SNAP_DPA_P2P_MSG_VQ_MSIX)
			continue;
		/* TODO: log bad messages */
		msix_count++;
	}
#endif

	return msix_count;
}

inline void dpa_msix_arm()
{
	/* TODO_Doron: use always armed in event mode */
	struct dpa_rt_context *rt_ctx = dpa_rt_ctx();
	struct mlx5_cqe64 *cqe;
	int n;

	/* cq shall be armed before it is polled. See man ibv_get_cq_event */
	snap_dv_arm_cq(&rt_ctx->msix_cq);

	for (n = 0; n < SNAP_DPA_RT_THR_MSIX_CQE_CNT; n++) {
		cqe = snap_dv_poll_cq(&rt_ctx->msix_cq, 64);
		if (!cqe)
			break;
	}
}

static void __attribute__((unused)) dpa_log_add(const char *msg)
{
	struct snap_dpa_log *log = dpa_mbox() + SNAP_DPA_THREAD_MBOX_LEN;
	struct snap_dpa_tcb *tcb = dpa_tcb();

	snap_dpa_log_add(log, 0, msg);

	if (tcb->mbox_lkey != tcb->active_lkey)
		dpa_window_set_mkey(tcb->active_lkey);
}

void dpa_rt_init(void)
{
	struct snap_dpa_tcb *tcb = dpa_tcb();
	struct dpa_rt_context *ctx;

	ctx = dpa_thread_alloc(sizeof(*ctx));
	if (ctx != (void *)tcb->data_address)
		dpa_fatal("oops, rt context is not at the beginning of the heap\n");
}

void dpa_rt_start(bool dummy_rq)
{
	struct snap_dpa_tcb *tcb = dpa_tcb();
	struct dpa_rt_context *ctx = dpa_rt_ctx();
	struct snap_dpa_cmd *cmd;

	cmd = snap_dpa_cmd_recv(dpa_mbox(), SNAP_DPA_CMD_DMA_EP_COPY);

	ctx->dpa_cmd_chan.dma_q = dpa_dma_ep_cmd_copy(cmd, dummy_rq);
	ctx->dpa_cmd_chan.q_size = SNAP_DPA_RT_QP_RX_SIZE;
	ctx->dpa_cmd_chan.credit_count = SNAP_DPA_RT_QP_RX_SIZE;

	/* drain command cq */
	snap_dv_poll_cq(&dpa_tcb()->cmd_cq, 64);

	snap_dpa_rsp_send(dpa_mbox(), SNAP_DPA_RSP_OK);
	dpa_debug("dma q at %p db_cq at %p\n", ctx->dpa_cmd_chan.dma_q, (void *)ctx->db_cq.cq_addr);
	tcb->cmd_last_sn = cmd->sn;
}

/* TODO: per dpa process/thread logging instead of dumping to simx */
static int do_print(const char *format, va_list ap)
{
	char str[SNAP_DPA_PRINT_BUF_LEN];
	int ret;

	ret = vsnprintf(str, sizeof(str), format, ap);
	dpa_print_string(str);
	dpa_log_add(str);
	return ret;
}

/* override builtin printfs */
int printf(const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = do_print(format, ap);
	va_end(ap);
	return ret;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = do_print(format, ap);
	va_end(ap);
	return ret;
}

void dpa_logger(const char *file_name, unsigned int line_num,
		int level, const char *level_c, const char *format, ...)
{
	va_list ap;
	char *file;

	file = strrchr(file_name, '/');
	if (!file)
		file = (char *)file_name;
	else
		file++;

	printf("[%s][%s:%d] ", level_c, file, line_num);
	va_start(ap, format);
	do_print(format, ap);
	va_end(ap);
}

void dpa_error_freeze()
{
	/* freeze calling thread so that we can debug it */
	while (1) {
		__asm("");
	}
	//flexio_os_event_wait();
}
