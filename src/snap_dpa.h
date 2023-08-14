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

#ifndef _SNAP_DPA_H
#define _SNAP_DPA_H

#include <stdint.h>
#include <stdbool.h>
/* for cpu_set_t */
#include <sched.h>
#if HAVE_FLEXIO
#include <libflexio/flexio.h>
#endif

#if !__DPA
#include <infiniband/verbs.h>
#endif

#include "snap_dpa_common.h"

bool snap_dpa_enabled(struct ibv_context *ctx);

struct snap_dpa_ctx {
	struct flexio_process  *dpa_proc;
	struct flexio_outbox   *dpa_uar;
	struct ibv_pd          *pd;
	struct snap_uar        *uar;
	struct snap_dma_q      *dma_q;
	struct snap_dma_q      *dummy_q;
	struct snap_dpa_mkeyh  *dma_mkeyh;
	struct flexio_uar      *flexio_uar;
	struct flexio_app      *dpa_app;
	void                   *dpa_app_entry_point;
	struct flexio_window   *dpa_window;
	struct snap_dpa_eq     *dpa_eq;
	uint64_t                dpa_mem_size;
	struct {
		uint64_t heap_memory;
	} stats;
};

struct snap_dpa_memh {
	struct snap_dpa_ctx *dctx;
	uint64_t va;
	size_t size;
};

struct snap_dpa_memh *snap_dpa_mem_alloc(struct snap_dpa_ctx *dctx, size_t size);
uint64_t snap_dpa_mem_addr(struct snap_dpa_memh *mem);
void snap_dpa_mem_free(struct snap_dpa_memh *mem);
struct snap_dpa_memh *snap_dpa_zalloc(struct snap_dpa_ctx *dpa_proc, size_t size);

struct snap_dpa_mkeyh {
	struct flexio_mkey *mkey;
};

struct snap_dpa_mkeyh *snap_dpa_mkey_alloc(struct snap_dpa_ctx *ctx, struct ibv_pd *pd);
uint32_t snap_dpa_mkey_id(struct snap_dpa_mkeyh *h);
void snap_dpa_mkey_free(struct snap_dpa_mkeyh *h);

struct snap_dpa_ctx *snap_dpa_process_create(struct ibv_context *ctx, const char *app_name);
void snap_dpa_process_destroy(struct snap_dpa_ctx *app);
uint32_t snap_dpa_process_umem_id(struct snap_dpa_ctx *ctx);
uint64_t snap_dpa_process_umem_addr(struct snap_dpa_ctx *ctx);
uint64_t snap_dpa_process_umem_size(struct snap_dpa_ctx *ctx);
uint32_t snap_dpa_process_eq_id(struct snap_dpa_ctx *ctx);

/**
 * snap_dpa_umem_offset() - get virtual address umem offset
 * @proc:   dpa process context
 * @dpa_va: dpa virtual address
 *
 * Return: offset relative to the dpa process heap umem
 */
static inline uint64_t snap_dpa_process_umem_offset(struct snap_dpa_ctx *proc, uint64_t dpa_va)
{
	return dpa_va - snap_dpa_process_umem_addr(proc);
}

enum {
	SNAP_DPA_THREAD_ATTR_POLLING = 0x1
};

int snap_dpa_memcpy(struct snap_dpa_ctx *ctx, uint64_t dpa_va, void *src, size_t n);

/**
 * struct snap_dpa_thread_attr - DPA thread attributes
 *
 * At the moment attributes are not used yet
 */
struct snap_dpa_thread_attr {
	size_t heap_size;
	/* pointer to a static cpu set. CPU_ALLOC may not work */
	cpu_set_t *hart_set;
	/* arbitrary user defined values */
	uint64_t user_arg;
	uint8_t user_flag;
};

struct snap_dpa_thread {
	struct snap_dpa_ctx   *dctx;
	struct flexio_event_handler *dpa_thread;
	void                  *cmd_mbox;
	pthread_mutex_t       cmd_lock;
	struct ibv_mr         *cmd_mr;
	struct snap_dpa_memh  *mem;
	struct snap_dpa_log   *dpa_log;
	struct snap_dma_q     *trigger_q;
	struct snap_dma_q     *dummy_q;
};

struct snap_dpa_thread *snap_dpa_thread_create(struct snap_dpa_ctx *dctx,
		struct snap_dpa_thread_attr *attr);
void snap_dpa_thread_destroy(struct snap_dpa_thread *thr);
uint32_t snap_dpa_thread_id(struct snap_dpa_thread *thr);
uint64_t snap_dpa_thread_heap_base(struct snap_dpa_thread *thr);
struct snap_dpa_ctx *snap_dpa_thread_proc(struct snap_dpa_thread *thr);

void *snap_dpa_thread_mbox_acquire(struct snap_dpa_thread *thr);
void snap_dpa_thread_mbox_release(struct snap_dpa_thread *thr);

void snap_dpa_cmd_send(struct snap_dpa_thread *thr, struct snap_dpa_cmd *cmd, uint32_t type);
struct snap_dpa_rsp *snap_dpa_rsp_wait(void *mbox);

int snap_dpa_thread_wakeup(struct snap_dpa_thread *thr);

int snap_dpa_thread_mr_copy_sync(struct snap_dpa_thread *thr, uint64_t va, uint64_t len, uint32_t mkey);

struct snap_dpa_duar {
	struct mlx5dv_devx_obj *obj;
	uint32_t duar_id;
	uint32_t obj_id;
};

enum snap_dpa_duar_modify {
	SNAP_DPA_DUAR_MAP_STATE = 1 << 0,
};

/*
 * @dev_emu_id:   emuation object id
 * @queue_id:  queue number (virtio/nvme)
 * @cq_num:    completion queue (cq) number to use
 * @dev_type: device type (virtio/nvme)
 * @queue_type: queue type owning the db to be mapped (Only for dev_type nvme)
 * @map_state: Mapping state of the db
 * @keep_db_value: get db value from host (1) or set new value (0)
 * @db_value: if keep_db_value is 0, set new db value
 * @modifiable_fields: mask of snap_dpa_duar_modify
 */
struct snap_dpa_duar_attr {
	uint32_t dev_emu_id;
	uint32_t queue_id;
	uint32_t cq_num;
	uint8_t dev_type;
	uint8_t queue_type;
	uint8_t map_state;
	uint8_t keep_db_value;
	uint64_t db_value;
	uint64_t modifiable_fields;
};

struct snap_dpa_duar *snap_dpa_duar_create(struct ibv_context *ctx, struct snap_dpa_duar_attr *attr);
int snap_dpa_duar_modify(struct snap_dpa_duar *duar, uint64_t mask, struct snap_dpa_duar_attr *attr);
void snap_dpa_duar_destroy(struct snap_dpa_duar *duar);
uint32_t snap_dpa_duar_id(struct snap_dpa_duar *duar);

struct snap_dpa_msix_eq {
	struct mlx5dv_devx_obj *obj;
	uint32_t eq_id;
	uint16_t msix_vector;
};

struct snap_dpa_msix_eq *snap_dpa_msix_eq_create(struct ibv_context *ctx, uint32_t dev_emu_id,
	uint16_t msix_vector, uint8_t dev_type);
void snap_dpa_msix_eq_destroy(struct snap_dpa_msix_eq *eq);
uint32_t snap_dpa_msix_eq_id(struct snap_dpa_msix_eq *eq);
uint16_t snap_dpa_msix_vector(struct snap_dpa_msix_eq *eq);

#endif
