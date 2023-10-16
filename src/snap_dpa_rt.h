/*
 * Copyright © 202 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#ifndef _SNAP_DPA_RT_H
#define _SNAP_DPA_RT_H

#include "snap_dpa.h"
#include "snap_dpa_p2p.h"
#include "snap_dma.h"

/**
 * Regardless of the storage emulation logic we always have following
 * components:
 * - reference counted application
 * - thread type: event or polling
 * - queues per thread: one or multiple
 * - workers. A worker represents a group of queues handled by the
 * same DPU 'polling context'.
 * - basic DPA thread structure. For example RC QP, Doorbell CQ,
 *   MSIX vectors, trigger CQ etc...
 * - assigning queue to the DPA thread
 */

struct snap_dpa_rt_attr {
};

#define SNAP_DPA_RT_NAME_LEN 32

struct  snap_dma_worker;

/**
 * struct snap_dma_q_init_attr DMA queue init attributes
 *
 * @cq:	CQ for this DMA queue
 * @wk:	Worker where the queue is attached
 */
struct snap_dma_q_init_attr {
	void *cq;
	struct snap_dma_worker *wk;
	snap_dma_rx_cb_t rx_cb;
	bool crypto_enable;
	struct snap_dma_q_crypto_attr crypto_attr;
	struct ibv_comp_channel *comp_channel;
	void *comp_context;
};

struct snap_dpa_rt_thread_init_attr {
	struct snap_dma_q_init_attr *q_init_attr;
	size_t heap_size;
};

struct snap_dpa_rt {
	struct snap_dpa_ctx *dpa_proc;
	/* TODO: keep list of current threads. At first we are going to
	 * support on polling/sing combo so it can wait
	 */
	int refcount;
	pthread_mutex_t lock;
	char name[SNAP_DPA_RT_NAME_LEN];

	LIST_ENTRY(snap_dpa_rt) entry;

	cpu_set_t polling_core_set;
	cpu_set_t polling_cores;
	int next_polling_core;

	int next_event_core;
};

int snap_dpa_rt_polling_core_get(struct snap_dpa_rt *rt);
void snap_dpa_rt_polling_core_put(struct snap_dpa_rt *rt, int i);

int snap_dpa_rt_event_core_get(struct snap_dpa_rt *rt);
void snap_dpa_rt_event_core_put(struct snap_dpa_rt *rt, int i);

struct snap_dpa_rt *snap_dpa_rt_get(struct ibv_context *ctx, const char *name,
		struct snap_dpa_rt_attr *attr);
void snap_dpa_rt_put(struct snap_dpa_rt *rt);

struct snap_dpa_rt_worker {
};

struct snap_dpa_rt_worker *snap_dpa_rt_worker_create(struct snap_dpa_rt *rt);
void snap_dpa_rt_worker_destroy(struct snap_dpa_rt_worker *w);

/* allocate single thread */

enum snap_dpa_rt_thr_mode {
	SNAP_DPA_RT_THR_POLLING,
	SNAP_DPA_RT_THR_EVENT,
};

enum snap_dpa_rt_thr_nqs {
	SNAP_DPA_RT_THR_SINGLE,
	SNAP_DPA_RT_THR_MULTI,
};

struct snap_dpa_rt_filter {
	struct ibv_pd *pd; // create p2p qps on this pd
	enum snap_dpa_rt_thr_mode mode;
	enum snap_dpa_rt_thr_nqs queue_mux_mode;
	struct snap_dpa_rt_worker *w;
	size_t heap_size;
};

struct snap_dpa_rt_thread {
	struct snap_dpa_rt *rt;
	struct snap_dpa_worker *wk;
	enum snap_dpa_rt_thr_mode mode;
	enum snap_dpa_rt_thr_nqs queue_mux_mode;
	int refcount;

	/* DPA specific things */
	struct snap_dpa_thread *thread;
	struct snap_dpa_p2p_q dpa_cmd_chan;
	struct snap_dpa_p2p_q dpu_cmd_chan;
	struct snap_cq *db_cq;
	/* in manyq per thread, we should have array of msix_cqs, for now use
	 * one msix vector per thread
	 */
	struct snap_cq *msix_cq;
	int n_msix;
	int hart;
};

struct dpa_rt_context {
	struct snap_hw_cq db_cq;
	struct snap_dpa_p2p_q dpa_cmd_chan;
	struct snap_hw_cq msix_cq;
};

#define SNAP_DPA_RT_QP_TX_SIZE 2048
#define SNAP_DPA_RT_QP_RX_SIZE 1024
#define SNAP_DPA_RT_QP_TX_ELEM_SIZE 64
#define SNAP_DPA_RT_QP_RX_ELEM_SIZE 64

#define SNAP_DPA_RT_THR_SINGLE_DB_CQE_SIZE 64
#define SNAP_DPA_RT_THR_SINGLE_DB_CQE_CNT 2

#define SNAP_DPA_RT_THR_MSIX_CQE_SIZE 64
#define SNAP_DPA_RT_THR_MSIX_CQE_CNT 2

struct snap_dpa_rt_thread *snap_dpa_rt_thread_get(struct snap_dpa_rt *rt,
			struct snap_dpa_rt_filter *filter,
			struct snap_dpa_rt_thread_init_attr *rtt_attr);
void snap_dpa_rt_thread_put(struct snap_dpa_rt_thread *rt);

int snap_dpa_rt_p2p_queue_create(struct snap_dpa_rt_thread *rt_thr,
		struct ibv_pd *pd, struct snap_dma_q_init_attr *q_init_attr,
		struct snap_dpa_p2p_q *dpu_cmd_chan, struct snap_dpa_p2p_q *dpa_cmd_chan);
int snap_dpa_rt_thread_msix_add(struct snap_dpa_rt_thread *rt_thr, struct snap_dpa_msix_eq *msix_eq, uint32_t *msix_cqnum);
void snap_dpa_rt_thread_msix_remove(struct snap_dpa_rt_thread *rt_thr, struct snap_dpa_msix_eq *msix_eq);

/* TODO: add a worker to support 1w:Ndpa threads model */
#endif
