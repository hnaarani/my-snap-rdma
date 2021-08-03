#include <limits.h>
#include "gtest/gtest.h"

extern "C" {
#include "snap.h"
#include "snap_dpa.h"
}

#include "tests_common.h"

class SnapDpaTest : public ::testing::Test {
	virtual void SetUp();
	virtual void TearDown();

	protected:
	struct ibv_pd *m_pd;
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

TEST_F(SnapDpaTest, hello) {

	struct snap_context *ctx;

	ctx = snap_open(get_ib_ctx()->device);
	ASSERT_TRUE(ctx);
	snap_close(ctx);
}

TEST_F(SnapDpaTest, app_load_unload) {
	struct snap_context *ctx;
	struct snap_dpa_ctx *dpa_ctx;

	ctx = snap_open(get_ib_ctx()->device);
	ASSERT_TRUE(ctx);

	dpa_ctx = snap_dpa_app_create(ctx, "dpa_hello");
	ASSERT_TRUE(dpa_ctx);
	snap_dpa_app_destroy(dpa_ctx);

	snap_close(ctx);
}

TEST_F(SnapDpaTest, create_thread) {
	struct snap_context *ctx;
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr;

	ctx = snap_open(get_ib_ctx()->device);
	ASSERT_TRUE(ctx);

	dpa_ctx = snap_dpa_app_create(ctx, "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpa_thr = snap_dpa_thread_create(dpa_ctx, 0);
	ASSERT_TRUE(dpa_thr);
	printf("thread is running now...\n");
	//getchar();

	snap_dpa_thread_destroy(dpa_thr);
	snap_dpa_app_destroy(dpa_ctx);
	snap_close(ctx);
}