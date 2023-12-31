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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>

#include "config.h"
#include "snap_macros.h"

#if HAVE_FLEXIO
#include <libflexio/flexio.h>
#if HAVE_FLEXIO_SUBPROJECT
#include <libflexio/src/flexio_exp.h>
#else
#include <flexio_exp.h>
#endif
#endif

#include "snap_dpa.h"
#include "mlx5_ifc.h"
#include "snap_dma.h"
#include "snap_lib_log.h"

SNAP_LIB_LOG_REGISTER(DPA)

SNAP_STATIC_ASSERT(sizeof(struct snap_dpa_tcb) % SNAP_MLX5_L2_CACHE_SIZE == 0,
		"Thread control block must be padded to the cache line");

#if HAVE_FLEXIO

SNAP_STATIC_ASSERT(CPU_SETSIZE >= 256, "Static cpu set size must be greater than the max number of HARTS");
SNAP_STATIC_ASSERT(SNAP_DPA_HW_THREADS_COUNT > 0 && SNAP_DPA_HW_THREADS_COUNT <= CPU_SETSIZE,
		"Invalid max hw threads count");

/**
 * we need dummy dpa eq if we want to use dpa cq in the pure polling mode.
 * Use cases: polling mode for internal debug, in single queue/thread event
 * mode we can use polling mode cq for tx completions
 *
 * since user mode eq does not make sense on dpu side, we keep implementation here and not
 * in the snap_qp.c
 */
struct snap_dpa_eq {
	struct snap_devx_common devx;
};

static struct snap_dpa_eq *snap_dpa_eq_create(struct snap_dpa_ctx *ctx)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) +
		   DEVX_ST_SZ_BYTES(dpa_eq)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
	struct snap_dpa_eq *eq;
	uint8_t *eq_in;
	const int EQ_LOG_SIZE = 4;
	const int EQE_SIZE = 64;

	eq = calloc(1, sizeof(*eq));
	if (!eq)
		return NULL;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_DPA_EQ);

	eq_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);

	/* have some space, to handle improbably case when there will be an
	 * event and we want to inspect it
	 */
	eq->devx.dpa_mem = snap_dpa_mem_alloc(ctx, EQE_SIZE * (1<<EQ_LOG_SIZE));
	if (!eq->devx.dpa_mem)
		goto free_eq;

	/* TODO: set ownership bit on dpa mem*/
	DEVX_SET(dpa_eq, eq_in, log_umem_size, EQ_LOG_SIZE);
	DEVX_SET(dpa_eq, eq_in, oi, 1);
	DEVX_SET(dpa_eq, eq_in, uar_page, snap_dpa_process_uar_id(ctx));
	DEVX_SET(dpa_eq, eq_in, umem_id, snap_dpa_process_umem_id(ctx));
	DEVX_SET64(dpa_eq, eq_in, umem_offset,
			snap_dpa_process_umem_offset(ctx, snap_dpa_mem_addr(eq->devx.dpa_mem)));

	eq->devx.devx_obj = mlx5dv_devx_obj_create(ctx->pd->context, in, sizeof(in), out, sizeof(out));
	if (!eq->devx.devx_obj)
		goto free_mem;

	eq->devx.id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);
	return eq;

free_mem:
	snap_dpa_mem_free(eq->devx.dpa_mem);
free_eq:
	free(eq);
	return NULL;
}

static void snap_dpa_eq_destroy(struct snap_dpa_eq *eq)
{
	mlx5dv_devx_obj_destroy(eq->devx.devx_obj);
	snap_dpa_mem_free(eq->devx.dpa_mem);
	free(eq);
}

static uint32_t snap_dpa_eq_id(struct snap_dpa_eq *eq)
{
	return eq->devx.id;
}

/**
 * snap_dpa_mem_alloc() - allocate memory on DPA
 * @dctx: snap context
 * @size: amount of memory to allocate
 *
 * The function allocate DPA virtual memory and return it's memory handle
 *
 * Return: memory handle or NULL on error
 */
struct snap_dpa_memh *snap_dpa_mem_alloc(struct snap_dpa_ctx *dctx, size_t size)
{
	struct snap_dpa_memh *mem;
	flexio_status st;

	mem = calloc(1, sizeof(*mem));
	if (!mem) {
		SNAP_LIB_LOG_ERR("Failed to allocate dpa memory handle");
		return 0;
	}

	mem->dctx = dctx;
	mem->size = size;
	st = flexio_buf_dev_alloc(dctx->dpa_proc, size, &mem->va);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("Failed to allocate dpa memory with size 0x%lx", size);
		free(mem);
		return 0;
	}

	mem->dctx->stats.heap_memory += size;
	return mem;
}

/**
 * snap_dpa_mem_free() - free DPA memory
 * @mem: memory handle
 *
 * The function frees memory handle and its associated memory
 */
void snap_dpa_mem_free(struct snap_dpa_memh *mem)
{
	mem->dctx->stats.heap_memory -= mem->size;
	flexio_buf_dev_free(mem->dctx->dpa_proc, mem->va);
	free(mem);
}

/**
 * snap_dpa_mem_addr() - get DPA virtual address
 * @mem: memory handle
 *
 * Return: virtual address
 */
uint64_t snap_dpa_mem_addr(struct snap_dpa_memh *mem)
{
	return mem->va;
}

/**
 * snap_dpa_zalloc() - allocate memory on DPA and memzero it
 * @dpa_proc: dpa process
 * @size: amount of memory to allocate
 *
 * The function allocate DPA virtual memory and will do memzero to
 * this allocated memory.
 *
 * Return: memory handle or NULL on error
 */
struct snap_dpa_memh *snap_dpa_zalloc(struct snap_dpa_ctx *dpa_proc, size_t size)
{
	int ret;
	void *tmp_buf;
	struct snap_dpa_memh *dpa_mem;

	tmp_buf = calloc(1, size);
	if (!tmp_buf) {
		SNAP_LIB_LOG_ERR("failed to alloc tmp buf");
		errno = -ENOMEM;
		return NULL;
	}

	dpa_mem = snap_dpa_mem_alloc(dpa_proc, size);
	if (!dpa_mem) {
		SNAP_LIB_LOG_ERR("failed to alloc mem on DPA");
		errno = -ENOMEM;
		goto free_tmp_buf;
	}

	ret = snap_dpa_memcpy(dpa_proc, snap_dpa_mem_addr(dpa_mem), tmp_buf, size);
	if (ret) {
		SNAP_LIB_LOG_ERR("failed to init qp buffer on DPA");
		errno = ret;
		goto free_dpa_mem;
	}

	free(tmp_buf);

	return dpa_mem;

free_dpa_mem:
	snap_dpa_mem_free(dpa_mem);

free_tmp_buf:
	free(tmp_buf);

	return NULL;
}

/**
 * snap_dpa_mkey_alloc() - create dpa process memory key
 * @dctx: device context
 * @pd:   protection domain
 *
 * The function creates DPA memory key that can be used to access @dctx memory
 * by objects belonging to @pd.
 *
 * For example, a QP can use memory key to perform DMA or post_send operations
 * TODO: consider caching protection domains and sharing the key
 *
 * Return:
 * mkey handle or NULL
 */
struct snap_dpa_mkeyh *snap_dpa_mkey_alloc(struct snap_dpa_ctx *dctx, struct ibv_pd *pd)
{
	struct snap_dpa_mkeyh *h;
	struct flexio_mkey_attr fattr;
	flexio_status st;

	h = calloc(1, sizeof(*h));
	if (!h) {
		SNAP_LIB_LOG_ERR("Failed to allocate dpa memory key handle");
		return 0;
	}

	fattr.pd = pd;
	fattr.daddr = snap_dpa_process_umem_addr(dctx);
	fattr.len = snap_dpa_process_umem_size(dctx);
	fattr.access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

	st = flexio_device_mkey_create(dctx->dpa_proc, &fattr, &h->mkey);
	if (st != FLEXIO_STATUS_SUCCESS) {
		free(h);
		return NULL;
	}

	return h;
}

/**
 * snap_dpa_mkey_id() - get memory key
 * @h:  memory key handle created by snap_dpa_mkey_alloc()
 *
 * The function gets actual memory key value from the mkey handle @h
 *
 * Return:
 * memory key
 */
uint32_t snap_dpa_mkey_id(struct snap_dpa_mkeyh *h)
{
	return flexio_mkey_get_id(h->mkey);
}

/**
 * snap_dpa_mkey_free() - free memory key handle
 * @h: memory key handle
 *
 * The function frees memory key handle and memory key object
 */
void snap_dpa_mkey_free(struct snap_dpa_mkeyh *h)
{
	flexio_device_mkey_destroy(h->mkey);
	free(h);
}

static void dummy_rx_cb(struct snap_dma_q *q, const void *data, uint32_t data_len, uint32_t imm_data)
{
	SNAP_LIB_LOG_ERR("OOPS: rx cb called");
}

static int dma_q_create(struct snap_dpa_ctx *ctx)
{
	struct snap_dma_q_create_attr dma_q_attr = {0};
	int ret;

	dma_q_attr.rx_cb = dummy_rx_cb;
	dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	dma_q_attr.sw_use_devx = true;
	dma_q_attr.tx_qsize = 16;
	dma_q_attr.rx_qsize = 0;

	ctx->dma_q = snap_dma_ep_create(ctx->pd, &dma_q_attr);
	if (!ctx->dma_q)
		return -1;

	dma_q_attr.tx_qsize = 0;
	ctx->dummy_q = snap_dma_ep_create(ctx->pd, &dma_q_attr);
	if (!ctx->dummy_q)
		goto dma_q_destroy;

	ret = snap_dma_ep_connect(ctx->dma_q, ctx->dummy_q);
	if (ret)
		goto dummy_q_destroy;

	ctx->dma_mkeyh = snap_dpa_mkey_alloc(ctx, ctx->pd);
	if (!ctx->dma_mkeyh)
		goto dummy_q_destroy;

	return 0;

dma_q_destroy:
	snap_dma_q_destroy(ctx->dma_q);
dummy_q_destroy:
	snap_dma_q_destroy(ctx->dummy_q);
	return -1;
}

static void dma_q_destroy(struct snap_dpa_ctx *ctx)
{
	snap_dpa_mkey_free(ctx->dma_mkeyh);
	snap_dma_q_destroy(ctx->dma_q);
	snap_dma_q_destroy(ctx->dummy_q);
}

static int load_file(const char *file_name, void **buf, size_t *size)
{
	int ret;
	FILE *fp;
	struct stat file_st;
	size_t fbuf_size;
	void *fbuf;

	fp = fopen(file_name, "r");
	if (!fp) {
		SNAP_LIB_LOG_ERR("Failed to open %s: %m", file_name);
		return -1;
	}

	ret = fstat(fileno(fp), &file_st);
	if (ret < 0) {
		SNAP_LIB_LOG_ERR("Failed to stat %s: %m", file_name);
		goto close_file;
	}

	if (!S_ISREG(file_st.st_mode)) {
		SNAP_LIB_LOG_ERR("%s is not a regular file", file_name);
		goto close_file;
	}

	fbuf_size = file_st.st_size;
	ret = posix_memalign(&fbuf, 64, fbuf_size);
	if (ret) {
		SNAP_LIB_LOG_ERR("Failed to alloc memory for %s", file_name);
		goto close_file;
	}

	if (fread(fbuf, fbuf_size, 1, fp) != 1) {
		SNAP_LIB_LOG_ERR("Failed to load file %s: %m", file_name);
		goto free_buf;
	}

	*buf = fbuf;
	*size = fbuf_size;
	fclose(fp);
	return 0;

free_buf:
	free(fbuf);
close_file:
	fclose(fp);
	return -1;
}

/* first bytes of the flexio app, so we can load signature */
struct snap_flexio_app {
	CIRCLEQ_ENTRY(snap_flexio_app) node;
	char app_name[FLEXIO_MAX_NAME_LEN + 1];
	void *elf_buffer;
	size_t elf_size;
	uint32_t sig_exist;
	void *sig_buffer;
	size_t sig_size;
};

static int snap_dpa_load_app_sig(struct snap_dpa_ctx *dpa_ctx, const char *app_name)
{
	struct snap_flexio_app *app = (struct snap_flexio_app *)dpa_ctx->dpa_app;
	int ret;
	char *file_name;
	void *sig_buf;
	size_t sig_buf_size;
	struct stat st;

	if (getenv("LIBSNAP_DPA_DIR"))
		ret = asprintf(&file_name, "%s/%s.sig", getenv("LIBSNAP_DPA_DIR"),
			       app_name);
	else
		ret = asprintf(&file_name, "%s/%s.sig", DPA_DEFAULT_APP_DIR, app_name);

	if (ret < 0) {
		SNAP_LIB_LOG_ERR("Failed to allocate memory");
		return -ENOMEM;
	}

	if (stat(file_name, &st)) {
		SNAP_LIB_LOG_DBG("App has no signature");
		free(file_name);
		return 0;
	}

	SNAP_LIB_LOG_INFO("%s is signed DPA application", app_name);
	ret = load_file(file_name, &sig_buf, &sig_buf_size);
	if (ret)
		goto free_file;

	/* once set it will be freed internally by the flexio */
	app->sig_exist = 1;
	app->sig_buffer = sig_buf;
	app->sig_size = sig_buf_size;

	free(file_name);
	return 0;

free_file:
	free(file_name);
	return -EINVAL;
}

static int snap_dpa_load_app(struct snap_dpa_ctx *dpa_ctx, const char *app_name)
{
	int ret;
	char *file_name;
	struct flexio_app_attr fattr;
	flexio_status st;

	if (getenv("LIBSNAP_DPA_DIR"))
		ret = asprintf(&file_name, "%s/%s", getenv("LIBSNAP_DPA_DIR"),
			       app_name);
	else
		ret = asprintf(&file_name, "%s/%s", DPA_DEFAULT_APP_DIR, app_name);

	/* TODO: support dpa app code embedding */

	if (ret < 0) {
		SNAP_LIB_LOG_ERR("Failed to allocate memory");
		return -ENOMEM;
	}

	fattr.app_name = app_name;
	fattr.app_sig_sec_name = "";

	ret = load_file(file_name, &fattr.app_ptr, &fattr.app_bsize);
	if (ret)
		goto free_file;

	/* load elf buffer */
	st = flexio_app_create(&fattr, &dpa_ctx->dpa_app);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("Failed to create flexio app");
		goto free_buf;
	}

	/* lookup entry point */
	st = flexio_func_register(dpa_ctx->dpa_app, SNAP_DPA_THREAD_ENTRY_POINT, (flexio_func_t **)&dpa_ctx->dpa_app_entry_point);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("Failed to find entry point");
		goto free_app;
	}

	free(fattr.app_ptr);
	free(file_name);

	snap_dpa_load_app_sig(dpa_ctx, app_name);
	return 0;

free_app:
	flexio_app_destroy(dpa_ctx->dpa_app);
free_buf:
	free(fattr.app_ptr);
free_file:
	free(file_name);
	return -EINVAL;
}

static void snap_dpa_unload_app(struct snap_dpa_ctx *dpa_ctx)
{
	/* FLexio BUG: there is no flexio_func_unregister - memory leak -- must fix flexio */
	flexio_app_destroy(dpa_ctx->dpa_app);
}

static uint64_t snap_dpa_get_mem_size(struct ibv_context *ctx)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	int ret;
	uint32_t log_mem_blocks, block_size;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_SET_HCA_CAP_OP_MOD_DPA);

	ret = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (ret)
		return 0;

	block_size = DEVX_GET(query_hca_cap_out, out, capability.dpa_cap.dpa_mem_block_size);
	log_mem_blocks = DEVX_GET(query_hca_cap_out, out, capability.dpa_cap.log_max_num_dpa_mem_blocks);
	SNAP_LIB_LOG_DBG("block size %u log %u", block_size, log_mem_blocks);
	return block_size * (1ULL << log_mem_blocks);
}

static int hex_char_to_int(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	else if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;

	return -1; // Invalid character
}

static void hex_to_cpuset(cpu_set_t *cpu_set, int hex_digit, int order)
{
	int i;

	if (hex_digit == 0)
		return;

	for (i = 0; i < 4; i++) {
		if ((hex_digit & (1<<i)) == 0)
			continue;

		int core = i + (4 * order);

		SNAP_LIB_LOG_DBG("digit 0x%x adding core %d", hex_digit, core);
		CPU_SET(core, cpu_set);
	}
}

static int snap_dpa_cpu_set_init(struct snap_dpa_ctx *ctx, const char *app_name)
{
	int i, n;
	char *app_core_mask_env, *mask;
	int ret;

	ret = asprintf(&app_core_mask_env, "%s_core_mask", app_name);
	if (ret < 0) {
		SNAP_LIB_LOG_ERR("%s: failed to init dpa core mask", app_name);
		return -ENOMEM;
	}

	mask = getenv(app_core_mask_env);
	free(app_core_mask_env);

	CPU_ZERO(&ctx->dpa_cpu_set);

	if (!mask) {
		for (i = 0; i < SNAP_DPA_HW_THREADS_COUNT; i++)
			CPU_SET(i, &ctx->dpa_cpu_set);
		return 0;
	}

	SNAP_LIB_LOG_INFO("%s: core mask %s", app_name, mask);
	for (n = 0, i = strlen(mask) - 1; i >= 0; i--, n++) {
		char c = tolower(mask[i]);

		if (!isxdigit(c))
			break;

		hex_to_cpuset(&ctx->dpa_cpu_set, hex_char_to_int(c), n);
	}

	/* convert mask into allowed cores */
	if (CPU_COUNT(&ctx->dpa_cpu_set) == 0) {
		SNAP_LIB_LOG_ERR("%s: has no valid cores to run", app_name);
		return -EINVAL;
	}
	return 0;
}

/**
 * snap_dpa_process_create() - create DPA application process
 * @ctx:         snap context
 * @app_name:    application name
 *
 * The function creates DPA application process and performs common
 * initialization steps.
 *
 * Application image is loaded from the path given by LIBSNAP_DPA_DIR
 * or from the current working directory.
 *
 * Return:
 * dpa conxtext on success or NULL on failure
 */
struct snap_dpa_ctx *snap_dpa_process_create(struct ibv_context *ctx, const char *app_name)
{
	struct flexio_process_attr proc_attr = {0};
	struct flexio_outbox_attr outbox_attr = {0};
	flexio_status st;
	struct snap_dpa_ctx *dpa_ctx;
	int ret;

	dpa_ctx = calloc(1, sizeof(*dpa_ctx));
	if (!dpa_ctx) {
		SNAP_LIB_LOG_ERR("%s: Failed to allocate memory for DPA context", app_name);
		return NULL;
	}

	ret = snap_dpa_cpu_set_init(dpa_ctx, app_name);
	if (ret)
		goto free_dpa_ctx;

	dpa_ctx->dpa_mem_size = snap_dpa_get_mem_size(ctx);
	if (!dpa_ctx->dpa_mem_size)
		goto free_dpa_ctx;

	ret = snap_dpa_load_app(dpa_ctx, app_name);
	if (ret)
		goto free_dpa_ctx;

	dpa_ctx->pd = ibv_alloc_pd(ctx);
	if (!dpa_ctx->pd) {
		errno = -ENOMEM;
		SNAP_LIB_LOG_ERR("%s: Failed to allocate pd for DPA context", app_name);
		goto free_dpa_app;
	}

	proc_attr.pd = dpa_ctx->pd;
	st = flexio_process_create(ctx, dpa_ctx->dpa_app, &proc_attr, &dpa_ctx->dpa_proc);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("%s: Failed to create DPA process", app_name);
		goto free_dpa_pd;
	}

	dpa_ctx->flexio_uar = flexio_process_get_uar(dpa_ctx->dpa_proc);
	if (!dpa_ctx->flexio_uar)
		goto free_dpa_proc;

	outbox_attr.uar = dpa_ctx->flexio_uar;
	st = flexio_outbox_create(dpa_ctx->dpa_proc, &outbox_attr, &dpa_ctx->dpa_uar);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("%s: Failed to create DPA outbox (uar)", app_name);
		goto free_dpa_proc;
	}

	st = flexio_window_create(dpa_ctx->dpa_proc, dpa_ctx->pd, &dpa_ctx->dpa_window);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("Failed to create DPA thread mailbox window");
		goto free_dpa_outbox;
	}

	/* create a placeholder eq to attach cqs */
	dpa_ctx->dpa_eq = snap_dpa_eq_create(dpa_ctx);
	if (!dpa_ctx->dpa_eq) {
		SNAP_LIB_LOG_ERR("%s: Failed to create DPA event queue", app_name);
		goto free_dpa_window;
	}

	if (dma_q_create(dpa_ctx))
		goto free_dpa_eq;

	strncpy(dpa_ctx->app_name, app_name, sizeof(dpa_ctx->app_name) - 1);
	return dpa_ctx;

free_dpa_eq:
	snap_dpa_eq_destroy(dpa_ctx->dpa_eq);
free_dpa_window:
	flexio_window_destroy(dpa_ctx->dpa_window);
free_dpa_outbox:
	flexio_outbox_destroy(dpa_ctx->dpa_uar);
free_dpa_proc:
	flexio_process_destroy(dpa_ctx->dpa_proc);
free_dpa_pd:
	ibv_dealloc_pd(dpa_ctx->pd);
free_dpa_app:
	snap_dpa_unload_app(dpa_ctx);
free_dpa_ctx:
	free(dpa_ctx);
	return NULL;
}

/**
 * snap_dpa_process_destroy() - destroy snap DPA process
 * @ctx:  DPA context
 *
 * The function destroys DPA process and performs common cleanup tasks
 */
void snap_dpa_process_destroy(struct snap_dpa_ctx *ctx)
{
	dma_q_destroy(ctx);
	snap_dpa_eq_destroy(ctx->dpa_eq);
	flexio_window_destroy(ctx->dpa_window);
	flexio_outbox_destroy(ctx->dpa_uar);
	flexio_process_destroy(ctx->dpa_proc);
	ibv_dealloc_pd(ctx->pd);
	snap_dpa_unload_app(ctx);
	free(ctx);
}

/**
 * snap_dpa_process_umem_id() - get DPA process 'umem' id
 * @ctx: DPA context
 *
 * Return: DPA process umem id
 */
uint32_t snap_dpa_process_umem_id(struct snap_dpa_ctx *ctx)
{
	return ctx->dpa_proc->dumem.id;
}

/**
 * snap_dpa_process_umem_id() - get DPA process 'umem' address
 * @ctx: DPA context
 *
 * Return: DPA process umem address
 */
uint64_t snap_dpa_process_umem_addr(struct snap_dpa_ctx *ctx)
{
	return ctx->dpa_proc->heap_process_umem_base_daddr;
}

/**
 * snap_dpa_process_umem_size() - get DPA process 'umem' size
 * @ctx: DPA context
 *
 * Return: DPA process umem size
 */
uint64_t snap_dpa_process_umem_size(struct snap_dpa_ctx *ctx)
{
	return ctx->dpa_mem_size;
}

/**
 * snap_dpa_process_eq_id() - get DPA process event queue id
 * @ctx: DPA context
 *
 * Return: DPA process event queue id
 */
uint32_t snap_dpa_process_eq_id(struct snap_dpa_ctx *ctx)
{
	return snap_dpa_eq_id(ctx->dpa_eq);
}

/**
 * snap_dpa_process_cpu_set() - get DPA process allowed cores cpu set
 * @ctx: DPA context
 *
 * Return: DPA process allowed cpu set
 */
const cpu_set_t *snap_dpa_process_cpu_set(struct snap_dpa_ctx *ctx)
{
	return &ctx->dpa_cpu_set;
}

/**
 * snap_dpa_process_uar_id() - get DPA process uar id
 * @ctx: DPA context
 *
 * Return: DPA process uar id
 */
uint32_t snap_dpa_process_uar_id(struct snap_dpa_ctx *ctx)
{
	return flexio_uar_get_id(ctx->flexio_uar);
}

static void snap_dpa_thread_destroy_force(struct snap_dpa_thread *thr);

static int trigger_q_create(struct snap_dpa_thread *thr)
{
	struct snap_dma_q_create_attr dma_q_attr = {0};
	int ret;

	dma_q_attr.rx_cb = dummy_rx_cb;
	dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	dma_q_attr.sw_use_devx = true;
	dma_q_attr.tx_qsize = 16;
	dma_q_attr.rx_qsize = 0;
	dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_TRIGGER;
	dma_q_attr.dpa_thread = thr;

	thr->trigger_q = snap_dma_ep_create(thr->dctx->pd, &dma_q_attr);
	if (!thr->trigger_q)
		return -1;

	/* make sure that wakeup is not delayed */
	thr->trigger_q->sw_qp.dv_qp.db_flag = SNAP_DB_RING_IMM;

	dma_q_attr.tx_qsize = 0;
	dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_NONE;

	thr->dummy_q = snap_dma_ep_create(thr->dctx->pd, &dma_q_attr);
	if (!thr->dummy_q)
		goto trigger_q_destroy;

	ret = snap_dma_ep_connect(thr->trigger_q, thr->dummy_q);
	if (ret)
		goto dummy_q_destroy;
	return ret;

trigger_q_destroy:
	snap_dma_q_destroy(thr->trigger_q);
dummy_q_destroy:
	snap_dma_q_destroy(thr->dummy_q);
	return -1;
}

static void trigger_q_destroy(struct snap_dpa_thread *thr)
{
	snap_dma_q_destroy(thr->trigger_q);
	snap_dma_q_destroy(thr->dummy_q);
}

static int set_hart_mask(struct snap_dpa_ctx *dctx, struct snap_dpa_thread_attr *attr,
		struct flexio_event_handler_attr *f_thr_attr)
{
	int i;

	f_thr_attr->affinity.type = FLEXIO_AFFINITY_NONE;

	if (!attr->hart_set)
		return 0;

	/* convert cpu_set_t to strict affinity within allowed cpu cores */
	for (i = 0; i < SNAP_DPA_HW_THREADS_COUNT; i++) {
		if (!CPU_ISSET(i, attr->hart_set))
			continue;

		/* set hart mask, only strict core affinity is supported so
		 * we bind to the first cpu set
		 */
		if (!CPU_ISSET(i, &dctx->dpa_cpu_set))
			continue;

		SNAP_LIB_LOG_INFO("%s: scheduling thread on core %d", dctx->app_name, i);
		f_thr_attr->affinity.type = FLEXIO_AFFINITY_STRICT;
		f_thr_attr->affinity.id = i;
		return 0;
	}

	/* TODO: pretty print */
	SNAP_LIB_LOG_ERR("%s: required core is not available", dctx->app_name);
	return -1;
}

/**
 * snap_dpa_thread_create() - create DPA thread
 * @dctx:  DPA application context
 * @attr:  thread attributes
 *
 * The function creates a thread that runs on the DPA. On function return the
 * thread is running and ready to accept commands via its mailbox.
 *
 * Return:
 * dpa thread on success or NULL on failure
 */
struct snap_dpa_thread *snap_dpa_thread_create(struct snap_dpa_ctx *dctx,
		struct snap_dpa_thread_attr *attr)
{
	struct snap_dpa_tcb tcb = {0};
	struct snap_dpa_thread_attr default_attr = {0};
	struct flexio_event_handler_attr f_thr_attr = {0};
	struct snap_dpa_thread *thr;
	int ret;
	flexio_status st;
	uint64_t dpa_tcb_addr;
	struct snap_dpa_rsp *rsp;
	size_t mbox_size;
	struct snap_dpa_cmd_start *cmd_start;

	thr = calloc(1, sizeof(*thr));
	if (!thr) {
		SNAP_LIB_LOG_ERR("Failed to create DPA thread");
		return NULL;
	}

	thr->dctx = dctx;
	if (!attr)
		attr = &default_attr;

	ret = set_hart_mask(dctx, attr, &f_thr_attr);
	if (ret)
		goto free_thread;

	ret = pthread_mutex_init(&thr->cmd_lock, NULL);
	if (ret < 0) {
		SNAP_LIB_LOG_ERR("Failed to init DPA thread mailbox lock");
		goto free_thread;
	}

	/* window size must be a multiple of 64 bytes */
	mbox_size = SNAP_ALIGN_CEIL(SNAP_DPA_THREAD_MBOX_LEN +
			snap_dpa_log_size(SNAP_DPA_THREAD_N_LOG_ENTRIES), 64);
	ret = posix_memalign(&thr->cmd_mbox, SNAP_DPA_THREAD_MBOX_ALIGN, mbox_size);
	if (ret < 0) {
		SNAP_LIB_LOG_ERR("Failed to allocate DPA thread mailbox");
		goto free_mutex;
	}

	memset(thr->cmd_mbox, 0, mbox_size);
	/* log is written into memory owned by DPU. This way even if DPA thread
	 * crashes or becomes unresponsive, we can still read its log
	 */
	thr->dpa_log = thr->cmd_mbox + SNAP_DPA_THREAD_MBOX_LEN;
	snap_dpa_log_init(thr->dpa_log, SNAP_DPA_THREAD_N_LOG_ENTRIES);
	thr->cmd_mr = snap_reg_mr(thr->dctx->pd, thr->cmd_mbox, mbox_size);
	if (!thr->cmd_mr) {
		SNAP_LIB_LOG_ERR("Failed to allocate DPA thread mailbox mr");
		goto free_mbox;
	}

	tcb.heap_size = snap_max(attr->heap_size, SNAP_DPA_THREAD_MIN_HEAP_SIZE);
	thr->mem = snap_dpa_mem_alloc(dctx, sizeof(tcb) + tcb.heap_size);
	if (!thr->mem)
		goto free_mr;

	dpa_tcb_addr = snap_dpa_mem_addr(thr->mem);
	tcb.data_address = snap_dpa_thread_heap_base(thr);
	/* copy mailbox addr & lkey to the thread */
	tcb.mbox_address = (uint64_t)thr->cmd_mbox;
	tcb.mbox_lkey = thr->cmd_mr->lkey;
	tcb.active_lkey = tcb.mbox_lkey;
	tcb.user_flag = attr->user_flag;
	tcb.user_arg = attr->user_arg;
	SNAP_LIB_LOG_DBG("tcb 0x%lx tcb_size %ld mailbox lkey 0x%x addr %p size(mbox+log) %lu mem_base at 0x%lx",
			dpa_tcb_addr, sizeof(tcb), thr->cmd_mr->lkey, thr->cmd_mbox, mbox_size, tcb.data_address);

	ret = snap_dpa_memcpy(dctx, dpa_tcb_addr, &tcb, sizeof(tcb));
	if (ret) {
		SNAP_LIB_LOG_ERR("Failed to prepare DPA thread control block: %d", ret);
		goto free_mem;
	}

	f_thr_attr.arg = dpa_tcb_addr;
	f_thr_attr.thread_local_storage_daddr = dpa_tcb_addr;
	f_thr_attr.host_stub_func = dctx->dpa_app_entry_point;

	st = flexio_event_handler_create(thr->dctx->dpa_proc, &f_thr_attr, &thr->dpa_thread);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("Failed to create DPA thread: %d", st);
		goto free_mem;
	}

	/* w/a flexio bug */
	st = flexio_event_handler_run(thr->dpa_thread, 0 /*dpa_tcb_addr*/);
	if (st != FLEXIO_STATUS_SUCCESS) {
		SNAP_LIB_LOG_ERR("Failed to run DPA thread: %d", st);
		goto destroy_thread;
	}

	ret = trigger_q_create(thr);
	if (ret)
		goto destroy_thread;

	cmd_start = (struct snap_dpa_cmd_start *)thr->cmd_mbox;
	memcpy(&cmd_start->cmd_cq, &thr->trigger_q->sw_qp.dv_tx_cq, sizeof(cmd_start->cmd_cq));
	SNAP_LIB_LOG_DBG("Command cq  : 0x%x addr=0x%lx, cqe_cnt=%d cqe_size=%d",
			cmd_start->cmd_cq.cq_num, cmd_start->cmd_cq.cq_addr, cmd_start->cmd_cq.cqe_cnt, cmd_start->cmd_cq.cqe_size);
	snap_dpa_cmd_send(thr, thr->cmd_mbox, SNAP_DPA_CMD_START);

	/* wait for report back from the thread */
	rsp = snap_dpa_rsp_wait(thr->cmd_mbox);
	if (rsp->status != SNAP_DPA_RSP_OK) {
		SNAP_LIB_LOG_ERR("DPA thread failed to start");
		snap_dpa_log_print(thr->dpa_log);
		goto destroy_trigger;
	}

	return thr;

destroy_trigger:
	trigger_q_destroy(thr);
destroy_thread:
	flexio_event_handler_destroy(thr->dpa_thread);
free_mem:
	snap_dpa_mem_free(thr->mem);
free_mr:
	ibv_dereg_mr(thr->cmd_mr);
free_mbox:
	free(thr->cmd_mbox);
free_mutex:
	pthread_mutex_destroy(&thr->cmd_lock);
free_thread:
	free(thr);
	return NULL;
}

static void snap_dpa_thread_destroy_force(struct snap_dpa_thread *thr)
{
#if SIMX_BUILD
	SNAP_LIB_LOG_DBG("WA simx thread destroy bug: 1s sleep");
	sleep(1); /* WA over simx bug */
#endif
	trigger_q_destroy(thr);
	flexio_event_handler_destroy(thr->dpa_thread);
	snap_dpa_mem_free(thr->mem);
	ibv_dereg_mr(thr->cmd_mr);
	pthread_mutex_destroy(&thr->cmd_lock);
	free(thr->cmd_mbox);
	free(thr);
}

/**
 * snap_dpa_thread_destroy() - destroy DPA thread
 * @thr:  DPA thread
 *
 * The function stops execution and clears all resources taken by the
 * DPA thread.
 *
 * The function is blocking. It sends 'STOP' message to the DPA thread,
 * waits for the ack and only then destroys thread and resources.
 */
void snap_dpa_thread_destroy(struct snap_dpa_thread *thr)
{
	struct snap_dpa_rsp *rsp;

	snap_dpa_cmd_send(thr, thr->cmd_mbox, SNAP_DPA_CMD_STOP);
	rsp = snap_dpa_rsp_wait(thr->cmd_mbox);
	if (rsp->status != SNAP_DPA_RSP_OK) {
		SNAP_LIB_LOG_WARN("DPA thread was not properly stopped");
		snap_dpa_log_print(thr->dpa_log);
	}

	snap_dpa_thread_destroy_force(thr);
}

/**
 * snap_dpa_thread_id() - get DPA thread id
 * @thr: DPA thread
 *
 * Return: DPA thread id
 */
uint32_t snap_dpa_thread_id(struct snap_dpa_thread *thr)
{
	return thr->dpa_thread->thread->aliasable.id;
}

/**
 * snap_dpa_thread_heap_addr() - get thread heap address
 * @thr: dpa thread
 *
 * The function returns base address of the thread heap memory
 *
 * Return:
 * base heap address
 */
uint64_t snap_dpa_thread_heap_base(struct snap_dpa_thread *thr)
{
	return sizeof(struct snap_dpa_tcb) + snap_dpa_mem_addr(thr->mem);
}

/**
 * snap_dpa_thread_proc() - get thread process context
 * @thr: dpa thread
 *
 * The function returns thread process context
 *
 * Return:
 * thread process context
 */
struct snap_dpa_ctx *snap_dpa_thread_proc(struct snap_dpa_thread *thr)
{
	return thr->dctx;
}

/**
 *
 * Get DPA thread mailbox address in the MT safe way. The mailbox must be
 * released with the snap_dpa_thread_mbox_release()
 *
 * Return:
 * DPA thread mailbox address
 */
void *snap_dpa_thread_mbox_acquire(struct snap_dpa_thread *thr)
{
	pthread_mutex_lock(&thr->cmd_lock);
	return thr->cmd_mbox;
}

/**
 * snap_dpa_thread_mbox_release() - release thread mailbox
 * @thr: DPA thread
 *
 * The function releases mailbox lock acquired by calling
 * snap_dpa_thread_mbox_acquire()
 */
void snap_dpa_thread_mbox_release(struct snap_dpa_thread *thr)
{
	pthread_mutex_unlock(&thr->cmd_lock);
}

/**
 * snpa_dpa_thread_mr_copy_sync() - copy memory region to DPA thread
 * @thr: DPA thread
 * @va:  memory virtual or physical address
 * @len: memory region length
 * @mkey: memory region key
 *
 * The function copies memory region description (va, len, mkey) to the DPA
 * thread. The copy is sync and is done via the command channel.
 *
 * Only description is copied. Data are not touched.
 *
 * Return:
 * 0 on success or -errno
 */
int snap_dpa_thread_mr_copy_sync(struct snap_dpa_thread *thr, uint64_t va, uint64_t len, uint32_t mkey)
{
	int ret = 0;
	struct snap_dpa_cmd_mr *cmd;
	struct snap_dpa_rsp *rsp;
	void *mbox;

	mbox = snap_dpa_thread_mbox_acquire(thr);

	cmd = (struct snap_dpa_cmd_mr *)snap_dpa_mbox_to_cmd(mbox);
	cmd->va = va;
	cmd->mkey = mkey;
	cmd->len = len;
	snap_dpa_cmd_send(thr, &cmd->base, SNAP_DPA_CMD_MR);

	rsp = snap_dpa_rsp_wait(mbox);
	if (rsp->status != SNAP_DPA_RSP_OK) {
		SNAP_LIB_LOG_ERR("Failed to copy MR: %d", rsp->status);
		ret = -EINVAL;
	}

	snap_dpa_thread_mbox_release(thr);
	return ret;
}

/**
 * snap_dpa_thread_wakeup() - wake up dpa thread
 * @thr: thread to wake up
 *
 * The function sends and 'event' to the dpa thread. If the thread
 * is not running it will wake up and run. The behaviour is similar
 * to that of pthread_cond_signal().
 *
 * If the thread is already running the event will be ignored.
 */
int snap_dpa_thread_wakeup(struct snap_dpa_thread *thr)
{
	int ret;
	struct snap_dma_completion comp = {0};

	/* NOTE: flush_nowait always does zero length rdma write
	 * with completion. Actually this is a bug in the flush_nowait ;)
	 *
	 * Also note that the cq buffer of this qp is in the DPA memory.
	 * So we cannot call anything that will touch it. E.x. flush() or poll()
	 */
	ret = snap_dma_q_flush_nowait(thr->trigger_q, &comp);
	if (ret) {
		SNAP_LIB_LOG_ERR("thr %p: Failed to arm trigger", thr);
		return ret;
	}

	/* TODO: need a way to get number of wakeup from the thread,
	 * but for now it is safe to assume that it was processed.
	 * Our wake up usage is for the sync command channel only
	 */
	thr->trigger_q->tx_available++;

	SNAP_LIB_LOG_DBG("thr %p: wakeup sent", thr);
	return ret;
}

/**
 * snap_dpa_enabled() - check if DPA support is present
 *
 * Return: true if DPA is available
 */
bool snap_dpa_enabled(struct ibv_context *ctx)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	uint64_t general_obj_types = 0;
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);

	ret = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (ret)
		return false;

	general_obj_types = DEVX_GET64(query_hca_cap_out, out,
				       capability.cmd_hca_cap.general_obj_types);

	return (MLX5_OBJ_TYPE_APU_MEM & general_obj_types) &&
		(MLX5_OBJ_TYPE_APU_PROCESS & general_obj_types) &&
		(MLX5_OBJ_TYPE_APU_THREAD & general_obj_types);
}

/**
 * snap_dpa_memcpy() - sync copy from DPU to DPA memory
 * @ctx:    dpa process context
 * @dpa_va: destination address on dpa
 * @src:    source address
 * @n:      bytes to copy
 *
 * The function copies @n bytes from memory area @src to memory area @dpa_va
 *
 * Return: 0 or -EINVAL on failure
 */
int snap_dpa_memcpy(struct snap_dpa_ctx *ctx, uint64_t dpa_va, void *src, size_t n)
{
	/* use our rc qp to copy things because flexio transpose
	 * (flexio_host2dev_memcpy() is buggy
	 */
	int ret = -EINVAL;
	void *tmp_buf;
	struct ibv_mr *mr;

	if (n == 0)
		return 0;

	tmp_buf = malloc(n);
	if (!tmp_buf)
		return -EINVAL;

	mr = snap_reg_mr(ctx->pd, tmp_buf, n);
	if (!mr)
		goto free_buf;

	memcpy(tmp_buf, src, n);
	ret = snap_dma_q_write(ctx->dma_q, tmp_buf, n, mr->lkey, dpa_va,
			snap_dpa_mkey_id(ctx->dma_mkeyh), NULL);
	if (ret)
		goto free_mr;

	snap_dma_q_flush(ctx->dma_q);
free_mr:
	ibv_dereg_mr(mr);
free_buf:
	free(tmp_buf);
	return ret;
}

#else

struct snap_dpa_ctx *snap_dpa_process_create(struct ibv_context *ctx, const char *app_name)
{
	return NULL;
}

void snap_dpa_process_destroy(struct snap_dpa_ctx *ctx)
{
}

struct snap_dpa_thread *snap_dpa_thread_create(struct snap_dpa_ctx *dctx,
		struct snap_dpa_thread_attr *attr)
{
	return NULL;
}

void snap_dpa_thread_destroy(struct snap_dpa_thread *thr)
{
}

uint32_t snap_dpa_thread_id(struct snap_dpa_thread *thr)
{
	return 0;
}

struct snap_dpa_ctx *snap_dpa_thread_proc(struct snap_dpa_thread *thr)
{
	return NULL;
}

int snap_dpa_thread_mr_copy_sync(struct snap_dpa_thread *thr, uint64_t va, uint64_t len, uint32_t mkey)
{
	return -ENOTSUP;
}

struct snap_dpa_memh *snap_dpa_mem_alloc(struct snap_dpa_ctx *dctx, size_t size)
{
	return NULL;
}

void snap_dpa_mem_free(struct snap_dpa_memh *mem)
{
}

uint64_t snap_dpa_mem_addr(struct snap_dpa_memh *mem)
{
	return 0;
}

struct snap_dpa_memh *snap_dpa_zalloc(struct snap_dpa_ctx *dpa_proc, size_t size)
{
	return NULL;
}

uint32_t snap_dpa_process_umem_id(struct snap_dpa_ctx *ctx)
{
	return 0;
}

uint64_t snap_dpa_process_umem_addr(struct snap_dpa_ctx *ctx)
{
	return 0;
}

uint64_t snap_dpa_process_umem_size(struct snap_dpa_ctx *ctx)
{
	return 0;
}

uint32_t snap_dpa_process_eq_id(struct snap_dpa_ctx *ctx)
{
	return 0;
}

uint32_t snap_dpa_process_uar_id(struct snap_dpa_ctx *ctx)
{
	return 0;
}

bool snap_dpa_enabled(struct ibv_context *ctx)
{
	return false;
}

int snap_dpa_memcpy(struct snap_dpa_ctx *ctx, uint64_t dpa_va, void *src, size_t n)
{
	return -ENOTSUP;
}

struct snap_dpa_mkeyh *snap_dpa_mkey_alloc(struct snap_dpa_ctx *ctx, struct ibv_pd *pd)
{
	return NULL;
}

uint32_t snap_dpa_mkey_id(struct snap_dpa_mkeyh *h)
{
	return 0xFFFF;
}

void snap_dpa_mkey_free(struct snap_dpa_mkeyh *h)
{
}

void *snap_dpa_thread_mbox_acquire(struct snap_dpa_thread *thr)
{
	return NULL;
}

void snap_dpa_thread_mbox_release(struct snap_dpa_thread *thr)
{
}

uint64_t snap_dpa_thread_heap_base(struct snap_dpa_thread *thr)
{
	return 0;
}

int snap_dpa_thread_wakeup(struct snap_dpa_thread *thr)
{
	return -ENOTSUP;
}

const cpu_set_t *snap_dpa_process_cpu_set(struct snap_dpa_ctx *ctx)
{
	return NULL;
}

#endif

/**
 * snap_dpa_log_size() - return size of the cyclic log buffer
 * @n_entries: number of entries in the log buffer
 *
 * The function returns size of the log buffer in bytes
 */
size_t snap_dpa_log_size(int n_entries)
{
	return sizeof(struct snap_dpa_log) + n_entries * sizeof(struct snap_dpa_log_entry);
}

/**
 * snap_dpa_log_init() - initialize cyclic log buffer
 * @log:       log buffer to init
 * @n_entries: number of entries in the buffer
 *
 * The function initializes cyclic log buffer
 */
void snap_dpa_log_init(struct snap_dpa_log *log, int n_entries)
{
	memset(log, 0, snap_dpa_log_size(n_entries));

	log->n_entries = n_entries;
}

/**
 * snap_dpa_log_print() - pretty print log buffer
 * @log: log buffer to print
 *
 * The function prints log buffer to the stdout. It tries to detect newlines
 * and combine log entries.
 */
void snap_dpa_log_print(struct snap_dpa_log *log)
{
	bool newline = true;
	int n;
	size_t len;

	if (log->avail_idx - log->used_idx >= log->n_entries)
		log->used_idx = log->avail_idx - log->n_entries + 1;

	while (log->used_idx != log->avail_idx) {
		n = log->used_idx % log->n_entries;
		len = strnlen(log->entries[n].msg, SNAP_DPA_PRINT_BUF_LEN);

		if (newline)
			printf("[DPA] %s", log->entries[n].msg);
		else
			printf("%s", log->entries[n].msg);

		if (len && log->entries[n].msg[len - 1] == '\n')
			newline = true;
		else
			newline = false;

		log->used_idx++;
	}
	fflush(stdout);
}

struct snap_dpa_rsp *snap_dpa_rsp_wait(void *mbox)
{
	int n = 0;
	const int count = 100000;
	int i;
	struct snap_dpa_rsp *rsp;
	struct snap_dpa_cmd *cmd;

	cmd = snap_dpa_mbox_to_cmd(mbox);
	/* wait for report back from the thread */
	do {
		i = 0;
		do {
			rsp = snap_dpa_mbox_to_rsp(mbox);
			snap_memory_cpu_load_fence();
			if (rsp->sn == cmd->sn)
				goto done;
			i++;
		} while (i < count);

		usleep(1000 * SNAP_DPA_THREAD_MBOX_POLL_INTERVAL_MSEC);
		n += SNAP_DPA_THREAD_MBOX_POLL_INTERVAL_MSEC;
		if (n == SNAP_DPA_THREAD_MBOX_TIMEOUT_MSEC) {
			rsp->status = SNAP_DPA_RSP_TO;
			rsp->sn = cmd->sn;
			break;
		}
	} while (1);
done:
	if (SNAP_DEBUG && n)
		SNAP_LIB_LOG_DBG("slow wait... %d ms total", n);
	return rsp;
}

/**
 * snap_dpa_cmd_send() - send 'command' to the dpa thread
 *
 * The function send slow path 'command' to the thread. It will also send
 * a wakeup event to the thread. That way it is guaranteed that thread will
 * be scheduled and it will process the command.
 */
void snap_dpa_cmd_send(struct snap_dpa_thread *thr, struct snap_dpa_cmd *cmd, uint32_t type)
{
	cmd->cmd = type;
	/* TODO: check if weaker barriers can be used */
	snap_memory_cpu_fence();
	cmd->sn++;
	snap_memory_bus_fence();
	snap_dpa_thread_wakeup(thr);
}

static int snap_dpa_duar_query(struct snap_dpa_duar *duar, uint32_t obj_id)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)  +
		   DEVX_ST_SZ_BYTES(emulated_dev_db_cq_map)] = {0};
	uint8_t *cq_db_map_out;
	int ret;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode,
		 MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_DPA_DB_CQ_MAPPING);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_id, obj_id);

	ret = mlx5dv_devx_obj_query(duar->obj, in, sizeof(in), out, sizeof(out));
	if (ret)
		goto out;

	cq_db_map_out = out + DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr);
	duar->duar_id = DEVX_GET(emulated_dev_db_cq_map, cq_db_map_out, dbr_handle);

out:
	return ret;
}

/**
 * snap_dpa_duar_create() - create emulation doorbell mapping
 * @ctx:  ibv context
 * @attr: doorbell creating attributes
 *
 * The function creates a new doorbell context of (attr->dev_emu_id, attr->queue_id) and
 * attaches it CQ (attr->cq_num).
 *
 * Doing NVMe or virtio doorbell will put a new CQE on the CQ.
 *
 * The CQ must be created on the DPA. If the cq is attached to DPA thread it
 * must also be armed in order to trigger thread wakeup.
 *
 * Return:
 * New doorbell context or NULL on error.
 */
struct snap_dpa_duar *snap_dpa_duar_create(struct ibv_context *ctx, struct snap_dpa_duar_attr *attr)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) +
		   DEVX_ST_SZ_BYTES(emulated_dev_db_cq_map)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
	uint8_t *cq_db_map_in;
	struct snap_dpa_duar *duar;
	uint32_t obj_id;
	int ret;

	duar = calloc(1, sizeof(*duar));
	if (!duar)
		return NULL;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_DPA_DB_CQ_MAPPING);

	cq_db_map_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);
	DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, device_type, attr->dev_type);
	DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, map_state, attr->map_state);
	DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, device_emulation_id, attr->dev_emu_id);
	DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, queue_id, attr->queue_id);
	DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, cqn, attr->cq_num);
	DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, keep_db_value_on_create, attr->keep_db_value);
	DEVX_SET64(emulated_dev_db_cq_map, cq_db_map_in, db_value, attr->db_value);

	if ((attr->queue_type == MLX5_DEV_DB_NVME_SQ) || (attr->queue_type == MLX5_DEV_DB_NVME_CQ))
		DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, queue_type, attr->queue_type);

	duar->obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!duar->obj)
		goto free_duar;

	obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);

	ret = snap_dpa_duar_query(duar, obj_id);
	if (ret)
		goto destroy_duar;
	duar->obj_id = obj_id;
	return duar;
destroy_duar:
	mlx5dv_devx_obj_destroy(duar->obj);
free_duar:
	free(duar);
	return NULL;
}

int snap_dpa_duar_modify(struct snap_dpa_duar *duar, uint64_t mask,
			 struct snap_dpa_duar_attr *attr)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) +
		   DEVX_ST_SZ_BYTES(emulated_dev_db_cq_map)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
	uint8_t *cq_db_map_in;
	uint64_t modify_mask;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode,
		 MLX5_CMD_OP_MODIFY_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type,
		 MLX5_OBJ_TYPE_DPA_DB_CQ_MAPPING);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_id, duar->obj_id);

	cq_db_map_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);

	modify_mask = 0;
	if (mask & SNAP_DPA_DUAR_MAP_STATE) {
		modify_mask |= MLX5_DEV_DB_BIT_MASK_MAP_STATE;
		DEVX_SET(emulated_dev_db_cq_map, cq_db_map_in, map_state,
			 attr->map_state);
	}
	if (modify_mask)
		DEVX_SET64(emulated_dev_db_cq_map, cq_db_map_in,
			   modify_field_select, modify_mask);

	return mlx5dv_devx_obj_modify(duar->obj, in, sizeof(in), out,
				      sizeof(out));
}

/**
 * snap_dpa_duar_destroy() - destroy emulation doorbell mapping
 *
 */
void snap_dpa_duar_destroy(struct snap_dpa_duar *duar)
{
	mlx5dv_devx_obj_destroy(duar->obj);
	free(duar);
}

/**
 * snap_dpa_duar_id() - get doorbell id
 * @duar: doorbell mapping
 *
 * The function returns doorbell id. The id should be passed to the DPA along
 * with the mapping cq number. Then dpa_duar_arm() should be used to enable
 * doorbells
 */
uint32_t snap_dpa_duar_id(struct snap_dpa_duar *duar)
{
	return duar->duar_id;
}

struct snap_dpa_msix_eq *snap_dpa_msix_eq_create(struct ibv_context *ctx, uint32_t dev_emu_id, uint16_t msix_vector, uint8_t dev_type)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) +
		   DEVX_ST_SZ_BYTES(emulated_dev_eq)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
	uint8_t *msix_eq_in;
	struct snap_dpa_msix_eq *msix_eq;

	msix_eq = calloc(1, sizeof(*msix_eq));
	if (!msix_eq)
		return NULL;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_EMULATED_DEV_EQ);

	msix_eq_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);
	DEVX_SET(emulated_dev_eq, msix_eq_in, device_type, dev_type);
	DEVX_SET(emulated_dev_eq, msix_eq_in, device_emulation_id, dev_emu_id);
	DEVX_SET(emulated_dev_eq, msix_eq_in, intr, msix_vector);

	msix_eq->obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!msix_eq->obj)
		goto free_duar;

	msix_eq->eq_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);
	return msix_eq;

free_duar:
	free(msix_eq);
	return NULL;
}

void snap_dpa_msix_eq_destroy(struct snap_dpa_msix_eq *eq)
{
	mlx5dv_devx_obj_destroy(eq->obj);
	free(eq);
}

uint32_t snap_dpa_msix_eq_id(struct snap_dpa_msix_eq *eq)
{
	return eq->eq_id;
}

uint16_t snap_dpa_msix_vector(struct snap_dpa_msix_eq *eq)
{
	return eq->msix_vector;
}
