#include <limits.h>
#include <sys/time.h>

#include "gtest/gtest.h"

extern "C" {
//#include "snap.h"
#include "snap_dpa.h"
#include "snap_qp.h"
#include "snap_dpa_rt.h"
}

#include "tests_common.h"

class SnapDpaTest : public ::testing::Test {
	virtual void SetUp();
	virtual void TearDown();

	protected:
	struct ibv_pd *m_pd;
	void run_cmd_lat_bench(int how);
	public:
	struct ibv_context *get_ib_ctx() { return m_pd->context; }
};

void SnapDpaTest::SetUp()
{
	struct mlx5dv_context_attr rdma_attr = {};
	bool init_ok = false;
	int i, n_dev;
	struct ibv_device **dev_list;
	struct ibv_context *ib_ctx;

	m_pd = NULL;
	dev_list = ibv_get_device_list(&n_dev);
	if (!dev_list)
		FAIL() << "Failed to open device list";

	for (i = 0; i < n_dev; i++) {
		if (strcmp(ibv_get_device_name(dev_list[i]),
					get_dev_name()) == 0) {
			rdma_attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;
			ib_ctx = mlx5dv_open_device(dev_list[i], &rdma_attr);
			if (!ib_ctx)
				FAIL() << "Failed to open " << dev_list[i];
			m_pd = ibv_alloc_pd(ib_ctx);
			if (!m_pd)
				FAIL() << "Failed to create PD";
			init_ok = true;
			goto out;
		}
	}
out:
	ibv_free_device_list(dev_list);
	if (!init_ok)
		FAIL() << "Failed to setup " << get_dev_name();
}

void SnapDpaTest::TearDown()
{
	struct ibv_context *ib_ctx;

	if (!m_pd)
		return;
	ib_ctx = get_ib_ctx();
	ibv_dealloc_pd(m_pd);
	ibv_close_device(ib_ctx);
}

/* check that we have emulation caps working on simx */
TEST_F(SnapDpaTest, hello_simx) {
/* this one can only work with snap */
#if 0
	struct snap_context *ctx;

	ctx = snap_open(get_ib_ctx()->device);
	ASSERT_TRUE(ctx);
	snap_close(ctx);
#endif
}

TEST_F(SnapDpaTest, app_load_unload) {
	struct snap_dpa_ctx *dpa_ctx;

	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_hello");
	ASSERT_TRUE(dpa_ctx);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDpaTest, create_thread) {
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr;

	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpa_thr = snap_dpa_thread_create(dpa_ctx, 0);
	ASSERT_TRUE(dpa_thr);
	printf("thread is running now...\n");
	//getchar();
	sleep(1);
	snap_dpa_log_print(dpa_thr->dpa_log);
	snap_dpa_thread_destroy(dpa_thr);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDpaTest, create_thread_event) {
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr;

	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_hello_event");
	ASSERT_TRUE(dpa_ctx);

	dpa_thr = snap_dpa_thread_create(dpa_ctx, 0);
	ASSERT_TRUE(dpa_thr);
	printf("thread is running now...\n");
	sleep(1);
	snap_dpa_log_print(dpa_thr->dpa_log);
	snap_dpa_thread_destroy(dpa_thr);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDpaTest, create_thread_two) {
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr1, *dpa_thr2;
	struct snap_dpa_thread_attr attr = {0};
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	attr.hart_set = &cpu_mask;

	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	/* polling threads should run on 2 harts */
	CPU_SET(0, &cpu_mask);
	dpa_thr1 = snap_dpa_thread_create(dpa_ctx, &attr);
	ASSERT_TRUE(dpa_thr1);
	snap_dpa_log_print(dpa_thr1->dpa_log);
	printf("thread1 is running now...\n");

	CPU_CLR(0, &cpu_mask);
	CPU_SET(1, &cpu_mask);
	printf("cpu isset %d\n", CPU_ISSET(1, &cpu_mask));
	dpa_thr2 = snap_dpa_thread_create(dpa_ctx, &attr);
	ASSERT_TRUE(dpa_thr2);
	snap_dpa_log_print(dpa_thr2->dpa_log);
	printf("thread2 is running now...\n");
	sleep(1);

	snap_dpa_log_print(dpa_thr2->dpa_log);
	snap_dpa_thread_destroy(dpa_thr2);
	printf("thr2 done\n");

	snap_dpa_log_print(dpa_thr1->dpa_log);
	snap_dpa_thread_destroy(dpa_thr1);
	printf("thr1 done\n");

	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDpaTest, create_thread_two_event) {
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr1, *dpa_thr2;
	struct snap_dpa_thread_attr attr = {0};
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	attr.hart_set = &cpu_mask;

	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_hello_event");
	ASSERT_TRUE(dpa_ctx);

	/* polling threads should run on 2 harts */
	CPU_SET(0, &cpu_mask);
	dpa_thr1 = snap_dpa_thread_create(dpa_ctx, &attr);
	ASSERT_TRUE(dpa_thr1);
	snap_dpa_log_print(dpa_thr1->dpa_log);
	printf("thread1 is running now...\n");

	CPU_CLR(0, &cpu_mask);
	CPU_SET(1, &cpu_mask);
	dpa_thr2 = snap_dpa_thread_create(dpa_ctx, &attr);
	ASSERT_TRUE(dpa_thr2);
	snap_dpa_log_print(dpa_thr2->dpa_log);
	printf("thread2 is running now...\n");
	sleep(1);

	snap_dpa_log_print(dpa_thr2->dpa_log);
	snap_dpa_thread_destroy(dpa_thr2);
	printf("thr2 done\n");

	snap_dpa_log_print(dpa_thr1->dpa_log);
	snap_dpa_thread_destroy(dpa_thr1);
	printf("thr1 done\n");

	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDpaTest, create_thread_n_event) {
	const int N = 1024;
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr[N];
	int i;
	uint64_t total_memory;

	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_hello_event");
	ASSERT_TRUE(dpa_ctx);

	for (i = 0; i < N; i++) {
		dpa_thr[i] = snap_dpa_thread_create(dpa_ctx, NULL);
		if (!dpa_thr[i]) {
			printf("Failed to create thread %d mem_used %ld bytes\n", i, dpa_ctx->stats.heap_memory);
			ASSERT_TRUE(dpa_thr[i]);
		}
	}
	total_memory = dpa_ctx->stats.heap_memory;
	sleep(1);
	for (i = 0; i < N; i++) {
		snap_dpa_log_print(dpa_thr[i]->dpa_log);
		snap_dpa_thread_destroy(dpa_thr[i]);
	}
	snap_dpa_process_destroy(dpa_ctx);
	printf("total heap memory used %ld bytes\n", total_memory);
}

TEST_F(SnapDpaTest, dpa_memcpy) {
	struct snap_dpa_ctx *dpa_ctx;
	size_t i;
	int ret;
	char buf[4096];
	struct snap_dpa_memh *dpa_mem;

	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpa_mem = snap_dpa_mem_alloc(dpa_ctx, sizeof(buf));
	ASSERT_TRUE(dpa_mem);

	for (i = 0; i < sizeof(buf); i++) {
		ret = snap_dpa_memcpy(dpa_ctx, snap_dpa_mem_addr(dpa_mem), buf, i);
		if (ret)
			printf("Failed to copy %lu bytes\n", i);
		ASSERT_EQ(0, ret);
	};

	snap_dpa_mem_free(dpa_mem);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDpaTest, create_rt)
{
	struct snap_dpa_rt_attr attr = {};
	struct snap_dpa_rt *rt1, *rt2;

	rt1 = snap_dpa_rt_get(get_ib_ctx(), "dpa_hello", &attr);
	ASSERT_TRUE(rt1);

	rt2 = snap_dpa_rt_get(get_ib_ctx(), "dpa_hello", &attr);
	ASSERT_TRUE(rt2);
	ASSERT_TRUE(rt1 == rt2);
	EXPECT_EQ(rt1->refcount, rt2->refcount);

	snap_dpa_rt_put(rt2);
	EXPECT_EQ(1, rt1->refcount);
	snap_dpa_rt_put(rt1);
}

TEST_F(SnapDpaTest, create_rt_thread_single_polling)
{
	struct snap_dpa_rt_attr attr = {};
	struct snap_dpa_rt *rt;
	struct snap_dpa_rt_thread *thr;
	struct snap_dpa_rt_filter f;

	rt = snap_dpa_rt_get(get_ib_ctx(), "dpa_rt_test_polling", &attr);
	ASSERT_TRUE(rt);

	f.mode = SNAP_DPA_RT_THR_POLLING;
	f.queue_mux_mode = SNAP_DPA_RT_THR_SINGLE;
	f.pd = NULL;
	thr = snap_dpa_rt_thread_get(rt, &f, NULL);
	ASSERT_TRUE(thr);
	sleep(1);
	snap_dpa_log_print(thr->thread->dpa_log);
	snap_dpa_rt_thread_put(thr);
	snap_dpa_rt_put(rt);
}

TEST_F(SnapDpaTest, create_rt_thread_single_event)
{
	struct snap_dpa_rt_attr attr = {};
	struct snap_dpa_rt *rt;
	struct snap_dpa_rt_thread *thr;
	struct snap_dpa_rt_filter f;

	rt = snap_dpa_rt_get(get_ib_ctx(), "dpa_rt_test_event", &attr);
	ASSERT_TRUE(rt);

	f.mode = SNAP_DPA_RT_THR_EVENT;
	f.queue_mux_mode = SNAP_DPA_RT_THR_SINGLE;
	f.pd = NULL;
	thr = snap_dpa_rt_thread_get(rt, &f, NULL);
	ASSERT_TRUE(thr);
	sleep(1);
	snap_dpa_log_print(thr->thread->dpa_log);
	snap_dpa_rt_thread_put(thr);
	snap_dpa_rt_put(rt);
}

TEST_F(SnapDpaTest, create_rt_thread_single_n_event)
{
	const int N = 1024;
	int i;
	struct snap_dpa_rt_attr attr = {};
	struct snap_dpa_rt *rt;
	struct snap_dpa_rt_thread *thr[N];
	struct snap_dpa_rt_filter f;
	uint64_t total_memory;

	rt = snap_dpa_rt_get(get_ib_ctx(), "dpa_rt_test_event", &attr);
	ASSERT_TRUE(rt);

	f.mode = SNAP_DPA_RT_THR_EVENT;
	f.queue_mux_mode = SNAP_DPA_RT_THR_SINGLE;
	f.pd = NULL;

	for (i = 0; i < N; i++) {
		thr[i] = snap_dpa_rt_thread_get(rt, &f, NULL);
		if (!thr[i]) {
			printf("Failed to create thread %d mem_used %ld bytes\n", i, rt->dpa_proc->stats.heap_memory);
			ASSERT_TRUE(thr[i]);
		}
	}
	total_memory = rt->dpa_proc->stats.heap_memory;
	sleep(1);
	for (i = 0; i < N; i++) {
		snap_dpa_log_print(thr[i]->thread->dpa_log);
		snap_dpa_rt_thread_put(thr[i]);
	}
	snap_dpa_rt_put(rt);
	printf("total heap memory used %ld bytes\n", total_memory);
}

void SnapDpaTest::run_cmd_lat_bench(int how)
{
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr;
	struct snap_dpa_thread_attr attr = {0};
	void *mbox;
	struct snap_dpa_cmd *cmd;
	struct snap_dpa_rsp *rsp;
	int i;
	struct timeval t_s, t_e, t_r;
	double t;
	int N;

	N = SNAP_DEBUG ? 10 : 1000000;
	dpa_ctx = snap_dpa_process_create(get_ib_ctx(), "dpa_cmd_lat_bench");
	ASSERT_TRUE(dpa_ctx);

	attr.user_arg = how;
	dpa_thr = snap_dpa_thread_create(dpa_ctx, &attr);
	ASSERT_TRUE(dpa_thr);
	printf("benchmark is running now...\n");

	mbox = snap_dpa_thread_mbox_acquire(dpa_thr);
	cmd = snap_dpa_mbox_to_cmd(mbox);

	gettimeofday(&t_s, 0);
	for (i = 0; i < N; i++) {
		snap_dpa_cmd_send(dpa_thr, cmd, SNAP_DPA_CMD_APP_FIRST);
		rsp = snap_dpa_rsp_wait(mbox);
		if (rsp->status != SNAP_DPA_RSP_OK) {
			printf("%d: Failed to copy DMA queue: %d\n", i, rsp->status);
			break;
		}
	}
	gettimeofday(&t_e, 0);
	timersub(&t_e, &t_s, &t_r);
	t = t_r.tv_sec + t_r.tv_usec/1000000.0;
	printf("CMD latency %1.9lf seconds, %d iters\n", t/N, N);

	snap_dpa_thread_mbox_release(dpa_thr);
	snap_dpa_log_print(dpa_thr->dpa_log);
	snap_dpa_thread_destroy(dpa_thr);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDpaTest, cmd_lat_bench_event_on_cq) {
	run_cmd_lat_bench(0);
}

TEST_F(SnapDpaTest, cmd_lat_bench_event_on_window) {
	run_cmd_lat_bench(1);
}

TEST_F(SnapDpaTest, cmd_lat_bench_poll_on_cq) {
	run_cmd_lat_bench(2);
}

TEST_F(SnapDpaTest, cmd_lat_bench_poll_on_window) {
	run_cmd_lat_bench(3);
}

#if 0
extern "C" {
#include "snap_virtio_common.h"
#include "snap_dpa_common.h"
#include "snap_dpa_virtq.h"
struct snap_virtio_queue *virtq_blk_dpa_create(struct snap_device *sdev,
		struct snap_virtio_common_queue_attr *attr);
int virtq_blk_dpa_destroy(struct snap_virtio_queue *vbq);
int virtq_blk_dpa_query(struct snap_virtio_queue *vbq,
		struct snap_virtio_common_queue_attr *attr);

#include <linux/virtio_ring.h>
};

TEST_F(SnapDpaTest, dpa_virtq) {
	struct snap_context *ctx;
	struct snap_virtio_queue *vq;
	struct snap_device sdev;
	struct snap_virtio_common_queue_attr vattr;

	ctx = snap_open(get_ib_ctx()->device);
	ASSERT_TRUE(ctx);
	/* hack to allow working on simx */
	sdev.sctx = ctx;

	vq = virtq_blk_dpa_create(&sdev, &vattr);
	ASSERT_TRUE(vq);
	printf("VQ is running\n"); getchar();
	virtq_blk_dpa_destroy(vq);

	snap_close(ctx);
}

/* Basic test for the DPA window copy machine */
TEST_F(SnapDpaTest, dpa_virtq_copy_avail) {
	struct snap_context *ctx;
	struct snap_virtio_queue *vq;
	struct snap_device sdev;
	struct snap_virtio_common_queue_attr attr = {0};
	struct vring_avail *avail;
	char page[4096];

	/* TODO:
	 * make this test CX7 specific, use phys memory for
	 * the virtio rings.
	 */
	ctx = snap_open(get_ib_ctx()->device);
	ASSERT_TRUE(ctx);
	/* hack to allow working on simx */
	sdev.sctx = ctx;

	/* create our dummy virtio device (only avail ring)
	 * and virtq implementation
	 * modify avail index, and wait till dpu reads it and
	 * updates our hw_avail_index
	 */
	avail = (struct vring_avail *)page;
	attr.vattr.idx = 0;
	attr.vattr.device = (uintptr_t)avail;

	vq = virtq_blk_dpa_create(&sdev, &attr);
	ASSERT_TRUE(vq);
	avail->idx = 42;
	sleep(10);
	//printf("VQ is running\n"); getchar();
	virtq_blk_dpa_query(vq, &attr);
	EXPECT_EQ(42, attr.hw_available_index);

	virtq_blk_dpa_destroy(vq);

	snap_close(ctx);
}

/* Basic test for the DPA window copy machine */
TEST_F(SnapDpaTest, dpa_two_virtq_create) {
#if 0
	struct snap_context *ctx;
	struct snap_virtio_queue *vq;
	struct snap_dpa_virtq *dvq;
	struct snap_device sdev;
	struct snap_virtio_common_queue_attr attr = {0};
	struct vring_avail *avail;
	char page[4096];
	void *mbox;
	struct dpa_virtq_cmd *cmd;
	struct snap_dpa_rsp *rsp;
	/* TODO:
	 * make this test CX7 specific, use phys memory for
	 * the virtio rings.
	 */
	ctx = snap_open(get_ib_ctx()->device);
	ASSERT_TRUE(ctx);
	/* hack to allow working on simx */
	sdev.sctx = ctx;

	/* create our dummy virtio device (only avail ring)
	 * and virtq implementation
	 * modify avail index, and wait till dpu reads it and
	 * updates our hw_avail_index
	 */
	avail = (struct vring_avail *)page;
	attr.vattr.idx = 0;
	attr.vattr.device = (uintptr_t)avail;

	vq = virtq_blk_dpa_create(&sdev, &attr);
	ASSERT_TRUE(vq);
	virtq_blk_dpa_query(vq, &attr);
	dvq = (snap_dpa_virtq *)vq;
	mbox = snap_dpa_thread_mbox_acquire(dvq->dpa_worker);
	cmd = (struct dpa_virtq_cmd *)snap_dpa_mbox_to_cmd(mbox);
	/* convert vq_attr to the create command */
	memset(&cmd->cmd_create, 0, sizeof(cmd->cmd_create));
	/* at the momemnt pass only avail/used/descr addresses */
	cmd->cmd_create.idx = 1;
	cmd->cmd_create.size = attr.vattr.size;
	cmd->cmd_create.desc = attr.vattr.desc;
	cmd->cmd_create.driver = attr.vattr.driver;
	cmd->cmd_create.device = attr.vattr.device;
	cmd->cmd_create.dpu_avail_mkey = dvq->dpa_window_mr->lkey;
	cmd->cmd_create.dpu_avail_ring_addr = (uint64_t)dvq->dpa_window;

	cmd->cmd_create.host_mkey = dvq->host_driver_mr->lkey;
	snap_dpa_cmd_send(&cmd->base, DPA_VIRTQ_CMD_CREATE);
	rsp = snap_dpa_rsp_wait(mbox);

	snap_dpa_cmd_send(&cmd->base, DPA_VIRTQ_CMD_DESTROY);
	rsp = snap_dpa_rsp_wait(mbox);
	snap_dpa_thread_mbox_release(dvq->dpa_worker);
	EXPECT_EQ(rsp->status ,SNAP_DPA_RSP_OK);

	virtq_blk_dpa_destroy(vq);

	snap_close(ctx);
#endif
}
#endif
