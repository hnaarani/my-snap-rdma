#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/mman.h>

#include <infiniband/verbs.h>

extern "C" {
#include "snap.h"
#include "snap_env.h"
#include "snap_nvme.h"
#include "snap_dma.h"
#include "snap_umr.h"
#include "host_uio.h"
#include "snap_dpa.h"
};

#include "gtest/gtest.h"
#include "tests_common.h"
#include "test_snap_dma.h"

int SnapDmaTest::snap_dma_q_fw_send_imm(struct snap_dma_q *q, uint32_t imm)
{
	return -ENOTSUP;
}

int SnapDmaTest::snap_dma_q_fw_send(struct snap_dma_q *q, void *src_buf,
		size_t len, uint32_t lkey)
{
	struct ibv_qp *qp = snap_qp_to_verbs_qp(q->fw_qp->fw_qp.qp);
	struct ibv_send_wr *bad_wr;
	struct ibv_send_wr send_wr = {};
	struct ibv_sge send_sgl = {};
	int rc;

	send_sgl.addr = (uint64_t)src_buf;
	send_sgl.length = len;
	send_sgl.lkey = lkey;

	send_wr.wr_id = 0;
	send_wr.next = NULL;
	send_wr.opcode = IBV_WR_SEND;
	send_wr.send_flags = 0;
	send_wr.sg_list = &send_sgl;
	send_wr.num_sge = 1;
	send_wr.imm_data = 0;

	rc = ibv_post_send(qp, &send_wr, &bad_wr);
	if (rc) {
		snap_error("DMA queue %p: FW failed to post send: %m", q);
		return rc;
	}

	return 0;
}

void SnapDmaTest::alloc_bufs()
{
	m_bsize = 4096;
	m_bcount = 64;
	m_lbuf = (char *)malloc(m_bcount * m_bsize);
	m_rbuf = (char *)malloc(m_bcount * m_bsize);
	if (!m_lbuf || !m_rbuf)
		FAIL() << "buffer allocation";

	m_lmr = ibv_reg_mr(m_pd, m_lbuf, m_bcount * m_bsize,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!m_lmr)
		FAIL() << "local memory buffer";

	m_rmr = ibv_reg_mr(m_pd, m_rbuf, m_bcount * m_bsize,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
			IBV_ACCESS_REMOTE_READ);
	if (!m_rmr)
		FAIL() << "remote memory buffer";
}

void SnapDmaTest::free_bufs()
{
	ibv_dereg_mr(m_rmr);
	ibv_dereg_mr(m_lmr);
	free(m_lbuf);
	free(m_rbuf);
}

static int g_rx_count;
static char g_last_rx[128];

static void dma_rx_cb(struct snap_dma_q *q, const void *data, uint32_t data_len,
		uint32_t imm_data)
{
	g_rx_count++;
	memcpy(g_last_rx, data, data_len);
}

void SnapDmaTest::SetUp()
{
	struct mlx5dv_context_attr rdma_attr = {};
	bool init_ok = false;
	int i, n_dev;
	struct ibv_device **dev_list;
	struct ibv_context *ib_ctx;

	/* set to the nvme sqe/cqe size and fw max queue size */
	memset(&m_dma_q_attr, 0, sizeof(m_dma_q_attr));
	m_dma_q_attr.tx_qsize = m_dma_q_attr.rx_qsize = 64;
	m_dma_q_attr.tx_elem_size = 16;
	m_dma_q_attr.rx_elem_size = 64;
	m_dma_q_attr.rx_cb = dma_rx_cb;
	m_dma_q_attr.mode = snap_env_getenv(SNAP_DMA_Q_OPMODE);
	m_dma_q_attr.fw_use_devx = false;

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
			m_comp_channel = ibv_create_comp_channel(ib_ctx);
			if (!m_comp_channel)
				FAIL() << "Failed to created completion channel";
			m_dma_q_attr.comp_channel = m_comp_channel;
			alloc_bufs();
			init_ok = true;
			goto out;
		}
	}

out:
	ibv_free_device_list(dev_list);
	if (!init_ok)
		FAIL() << "Failed to setup " << get_dev_name();
}

void SnapDmaTest::TearDown()
{
	struct ibv_context *ib_ctx;

	if (!m_pd)
		return;
	ib_ctx = m_pd->context;
	free_bufs();
	ibv_destroy_comp_channel(m_comp_channel);
	ibv_dealloc_pd(m_pd);
	ibv_close_device(ib_ctx);
}

struct snap_dma_q *SnapDmaTest::create_queue()
{
	struct snap_dma_q *q;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	if (q) {
		EXPECT_TRUE(snap_qp_get_qpnum(q->fw_qp->fw_qp.qp) == snap_dma_q_get_fw_qp(q)->qp_num);
	}
	return q;
}

TEST_F(SnapDmaTest, ibv_create_ah_test) {
	int ret, attr_mask;
	struct ibv_qp_attr attr = {};
	struct ibv_qp_init_attr init_attr = {};
	struct ibv_ah *ah;
	struct snap_dma_q *q;
	union ibv_gid gid;
	struct ibv_qp *qp;

	q = create_queue();
	ASSERT_TRUE(q);
	qp = snap_qp_to_verbs_qp(q->fw_qp->fw_qp.qp);

	if (!ibv_query_gid(qp->context, 1, 0, &gid)) {
		attr_mask = IBV_QP_AV;
		ret = ibv_query_qp(qp, &attr, attr_mask, &init_attr);
		ASSERT_TRUE(!ret);

		ah = ibv_create_ah(qp->pd, &attr.ah_attr);
		ASSERT_TRUE(ah);

		ibv_destroy_ah(ah);
	}

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, create_destroy) {
	struct snap_dma_q *q;

	q = create_queue();
	ASSERT_TRUE(q);
	snap_dma_q_destroy(q);

	/* creating another queue after one was destroyed must work */
	q = create_queue();
	ASSERT_TRUE(q);
	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, create_destroy_n) {
	const int N = 16;
	int i;
	struct snap_dma_q *q[N];

	for (i = 0; i < N; i++) {
		q[i] = create_queue();
		ASSERT_TRUE(q[i]);
	}

	for (i = 0; i < N; i++)
		snap_dma_q_destroy(q[i]);
}

static int g_comp_count;
static int g_last_comp_status;

static void dma_completion(struct snap_dma_completion *comp, int status)
{
	g_comp_count++;
	g_last_comp_status = status;
}

void SnapDmaTest::dma_xfer_test(struct snap_dma_q *q, bool is_read, bool poll_mode,
		void *rvaddr, void *rpaddr, uint32_t rkey, int len)
{
	struct snap_dma_completion comp;
	int n, rc;

	comp.func = dma_completion;
	comp.count = 1;

	if (!poll_mode) {
		if (q->no_events)
			return;
		ASSERT_EQ(0, snap_dma_q_arm(q));
	}

	g_comp_count = 0;

	if (is_read) {
		memset(m_lbuf, 0, len);
		if (rvaddr)
			memset(rvaddr, 0xED, len);
		rc = snap_dma_q_read(q, m_lbuf, m_bsize, m_lmr->lkey,
				(uintptr_t)rpaddr, rkey, &comp);
	} else {
		if (rvaddr)
			memset(rvaddr, 0, len);
		memset(m_lbuf, 0xED, len);
		rc = snap_dma_q_write(q, m_lbuf, m_bsize, m_lmr->lkey,
				(uintptr_t)rpaddr, rkey, &comp);
	}
	ASSERT_EQ(0, rc);

	if (!poll_mode) {
		struct ibv_cq *cq;
		void *cq_context;
		struct pollfd pfd;

		pfd.fd = m_comp_channel->fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		ASSERT_EQ(1, poll(&pfd, 1, 1000));

		ASSERT_EQ(0, ibv_get_cq_event(m_comp_channel, &cq, &cq_context));
		ASSERT_TRUE(cq == snap_cq_to_verbs_cq(q->sw_qp.tx_cq));
		snap_dma_q_progress(q);
		ibv_ack_cq_events(cq, 1);
	} else {
		n = 0;
		while (n < 10000) {
			snap_dma_q_progress(q);
			if (g_comp_count == 1)
				break;
			n++;
		}
	}

	ASSERT_EQ(1, g_comp_count);
	ASSERT_EQ(0, g_last_comp_status);
	ASSERT_EQ(0, comp.count);
	if (rvaddr) {
		ASSERT_EQ(0, memcmp(m_lbuf, rvaddr, len));
	}
}

static void dma_read_short_test(struct ibv_pd *pd, struct snap_dma_q_create_attr *attr, void *lbuf, int len, void *rbuf, int rkey)
{
	int n, rc;
	struct snap_dma_q *q;
	struct snap_dma_completion comp;

	comp.func = dma_completion;
	comp.count = 1;
	g_comp_count = 0;

	q = snap_dma_q_create(pd, attr);
	ASSERT_TRUE(q);

	memset(rbuf, 'A', len);

	rc = snap_dma_q_read_short(q, lbuf, len, (uintptr_t)rbuf, rkey, &comp);
	ASSERT_EQ(0, rc);

	n = 0;
	while (n < 10000) {
		snap_dma_q_progress(q);
		if (g_comp_count == 1)
			break;
		n++;
	}

	ASSERT_EQ(1, g_comp_count);
	ASSERT_EQ(0, g_last_comp_status);
	ASSERT_EQ(0, comp.count);

	ASSERT_EQ(0, memcmp(lbuf, rbuf, len));

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, dma_read_short_dv_verbs) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = false;

	dma_read_short_test(m_pd, &m_dma_q_attr, m_lbuf, 32, m_rbuf,  m_rmr->rkey);
}

TEST_F(SnapDmaTest, dma_read_short_dv_devx) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;

	dma_read_short_test(m_pd, &m_dma_q_attr, m_lbuf, 32, m_rbuf,  m_rmr->rkey);
}

TEST_F(SnapDmaTest, dma_read_short_verbs) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_VERBS;

	dma_read_short_test(m_pd, &m_dma_q_attr, m_lbuf, 32, m_rbuf,  m_rmr->rkey);
}

TEST_F(SnapDmaTest, dma_read) {
	struct snap_dma_q *q;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	dma_xfer_test(q, true, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);
	dma_xfer_test(q, true, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, dma_read_inline_verbs) {
	struct snap_dma_q *q;

	m_bsize = 32;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	dma_xfer_test(q, true, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);
	dma_xfer_test(q, true, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, dma_read_inline_devx) {
	struct snap_dma_q *q;

	m_dma_q_attr.sw_use_devx = true;
	m_bsize = 32;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	dma_xfer_test(q, true, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);
	dma_xfer_test(q, true, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, dma_write) {
	struct snap_dma_q *q;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	dma_xfer_test(q, false, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);
	dma_xfer_test(q, false, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, dma_write_short) {
	struct snap_dma_q *q;
	char cqe[m_dma_q_attr.tx_elem_size];
	int rc;
	int n, saved_tx_available;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	saved_tx_available = q->tx_available;

	memset(m_rbuf, 0, sizeof(cqe));
	memset(cqe, 0xDA, sizeof(cqe));

	rc = snap_dma_q_write_short(q, cqe, sizeof(cqe), (uintptr_t)m_rbuf,
			            m_rmr->lkey);
	ASSERT_EQ(0, rc);
	n = 0;
	while (q->tx_available < saved_tx_available && n < 10000) {
		snap_dma_q_progress(q);
		n++;
	}

	ASSERT_EQ(0, memcmp(cqe, m_rbuf, sizeof(cqe)));
	snap_dma_q_flush(q);
	ASSERT_EQ(saved_tx_available, q->tx_available);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, flush_qp) {
	struct snap_dma_q *q;
	int rc;
	struct ibv_recv_wr rx_wr, *bad_wr;
	struct ibv_sge rx_sge;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	/* post one recv to the fw qp so that send can complete */
	rx_sge.addr = (uintptr_t)m_rbuf;
	rx_sge.length = m_dma_q_attr.tx_elem_size;
	rx_sge.lkey = m_rmr->lkey;
	rx_wr.next = NULL;
	rx_wr.sg_list = &rx_sge;
	rx_wr.num_sge = 1;

	rc = ibv_post_recv(snap_qp_to_verbs_qp(q->fw_qp->fw_qp.qp), &rx_wr, &bad_wr);
	ASSERT_EQ(0, rc);

	/* modify QP to error state */
	rc = snap_dma_q_modify_to_err_state(q);
	ASSERT_EQ(0, rc);

	/* Poll CQ as we expect 1 completion with flush error */
	struct ibv_wc wc;
	while (rc != 1)
		rc = ibv_poll_cq(snap_cq_to_verbs_cq(q->sw_qp.rx_cq), 1, &wc);

	ASSERT_EQ(1, rc);
	ASSERT_EQ(IBV_WC_WR_FLUSH_ERR, wc.status);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, flush_devx_qp) {
	int rc;
	struct snap_dma_q *q;
	struct snap_dma_worker *wk;

	snap_dma_worker_create_attr wk_attr;
	wk_attr.mode = SNAP_DMA_WORKER_MODE_SHARED_CQ;
	wk_attr.exp_queue_num = 4;
	wk_attr.exp_queue_rx_size = 128;

	wk = snap_dma_worker_create(m_pd, &wk_attr);
	ASSERT_TRUE(wk);

	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.wk = wk;
	m_dma_q_attr.tx_qsize = 16;
	m_dma_q_attr.rx_qsize = 16;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	rc = snap_dma_q_modify_to_err_state(q);
	ASSERT_EQ(0, rc);

	/* destroy_done will be marked true by progress function when the
	 * q is completely flushed
	 */
	while(!q->destroy_done)
	       snap_dma_worker_progress_rx(wk);

	ASSERT_EQ(q->flush_count, m_dma_q_attr.rx_qsize*SNAP_DMA_Q_POST_RECV_BUF_FACTOR);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, send_completion) {
	struct snap_dma_q *q;
	char cqe[m_dma_q_attr.tx_elem_size];
	int rc;
	int n, saved_tx_available;
	struct ibv_recv_wr rx_wr, *bad_wr;
	struct ibv_sge rx_sge;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	saved_tx_available = q->tx_available;

	/* post one recv to the fw qp so that send can complete */
	rx_sge.addr = (uintptr_t)m_rbuf;
	rx_sge.length = m_dma_q_attr.tx_elem_size;
	rx_sge.lkey = m_rmr->lkey;
	rx_wr.next = NULL;
	rx_wr.sg_list = &rx_sge;
	rx_wr.num_sge = 1;

	rc = ibv_post_recv(snap_qp_to_verbs_qp(q->fw_qp->fw_qp.qp), &rx_wr, &bad_wr);
	ASSERT_EQ(0, rc);

	memset(m_rbuf, 0, sizeof(cqe));
	memset(cqe, 0xDA, sizeof(cqe));

	rc = snap_dma_q_send_completion(q, cqe, sizeof(cqe));
	ASSERT_EQ(0, rc);
	n = 0;
	while (q->tx_available < saved_tx_available && n < 10000) {
		snap_dma_q_progress(q);
		n++;
	}
	/* check that send was actually received */
	struct ibv_wc wc;
	rc = ibv_poll_cq(snap_cq_to_verbs_cq(q->fw_qp->fw_qp.rx_cq), 1, &wc);

	ASSERT_EQ(0, memcmp(cqe, m_rbuf, sizeof(cqe)));
	ASSERT_EQ(1, rc);

	snap_dma_q_flush(q);
	ASSERT_EQ(saved_tx_available, q->tx_available);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, flush) {
	struct snap_dma_q *q;
	int rc;
	char cqe[m_dma_q_attr.tx_elem_size];
	struct ibv_recv_wr rx_wr, *bad_wr;
	struct ibv_sge rx_sge;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	rx_sge.addr = (uintptr_t)m_rbuf;
	rx_sge.length = m_dma_q_attr.tx_elem_size;
	rx_sge.lkey = m_rmr->lkey;
	rx_wr.next = NULL;
	rx_wr.sg_list = &rx_sge;
	rx_wr.num_sge = 1;
	rc = ibv_post_recv(snap_qp_to_verbs_qp(q->fw_qp->fw_qp.qp), &rx_wr, &bad_wr);
	ASSERT_EQ(0, rc);

	rc = snap_dma_q_flush(q);
	/* no outstanding requests */
	ASSERT_EQ(0, rc);

	rc = snap_dma_q_read(q, m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, m_rmr->lkey, NULL);
	ASSERT_EQ(0, rc);
	rc = snap_dma_q_write(q, m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, m_rmr->lkey, NULL);
	ASSERT_EQ(0, rc);
	rc = snap_dma_q_send_completion(q, cqe, sizeof(cqe));
	ASSERT_EQ(0, rc);
	rc = snap_dma_q_flush(q);
	/* 3 outstanding requests */
	ASSERT_EQ(3, rc);

	snap_dma_q_destroy(q);
}

void SnapDmaTest::empty(int mode)
{
        struct snap_dma_q *q;
        int rc;
        bool status;

        m_dma_q_attr.mode = mode;
        q = snap_dma_q_create(m_pd, &m_dma_q_attr);
        ASSERT_TRUE(q);

        status = snap_dma_q_empty(q);
        ASSERT_TRUE(status);

        rc = snap_dma_q_read(q, m_lbuf, m_bsize, m_lmr->lkey,
                        (uintptr_t)m_rbuf, m_rmr->lkey, NULL);
        ASSERT_EQ(0, rc);

        status = snap_dma_q_empty(q);
        ASSERT_FALSE(status);

        rc = snap_dma_q_flush(q);

        status = snap_dma_q_empty(q);
        ASSERT_TRUE(status);

        snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, empty_verbs) {
        empty(SNAP_DMA_Q_MODE_VERBS);
}

TEST_F(SnapDmaTest, empty_dv) {
        empty(SNAP_DMA_Q_MODE_DV);
}

void SnapDmaTest::flush_async(int mode)
{
        bool status;
        int rc, n;
        struct snap_dma_q *q;
        struct snap_dma_completion comp;

        m_dma_q_attr.mode = mode;
        q = snap_dma_q_create(m_pd, &m_dma_q_attr);
        ASSERT_TRUE(q);

        rc = snap_dma_q_read(q, m_lbuf, m_bsize, m_lmr->lkey,
                        (uintptr_t)m_rbuf, m_rmr->lkey, NULL);
        ASSERT_EQ(0, rc);

        status = snap_dma_q_empty(q);
        ASSERT_FALSE(status);

        g_comp_count = 0;

        comp.count = 1;
        comp.func = dma_completion;
        rc = snap_dma_q_flush_nowait(q, &comp);
        ASSERT_EQ(0, rc);

        ASSERT_EQ(0, g_comp_count);
        status = snap_dma_q_empty(q);
        ASSERT_FALSE(status);

        n = 0;
        while (n < 10000) {
                snap_dma_q_progress(q);
                if (g_comp_count == 1)
                        break;
                n++;
        }

        ASSERT_EQ(1, g_comp_count);
        status = snap_dma_q_empty(q);
        ASSERT_TRUE(status);

        snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, flush_async_verbs) {
	flush_async(SNAP_DMA_Q_MODE_VERBS);
}

TEST_F(SnapDmaTest, flush_async_dv) {
	flush_async(SNAP_DMA_Q_MODE_DV);
}

TEST_F(SnapDmaTest, rx_callback) {
	struct snap_dma_q *q;
	char *sqe = m_rbuf;
	int rc;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	memset(sqe, 0xDA, m_dma_q_attr.rx_elem_size);

	g_rx_count = 0;
	rc = snap_dma_q_fw_send(q, sqe, m_dma_q_attr.rx_elem_size, m_rmr->lkey);
	ASSERT_EQ(0, rc);

	sleep(1);
	snap_dma_q_progress(q);

	ASSERT_EQ(1, g_rx_count);
	ASSERT_EQ(0, memcmp(g_last_rx, sqe, m_dma_q_attr.rx_elem_size));
	snap_dma_q_destroy(q);
}

void SnapDmaTest::poll_rx(int mode)
{
	struct snap_dma_q *q;
	char *sqe = m_rbuf;
	int rc, i;
	int rx_reqs = 16;
	struct snap_rx_completion read_comp[rx_reqs];
	int n;
	m_dma_q_attr.mode = mode;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	memset(sqe, 0xDA, m_dma_q_attr.rx_elem_size);

	g_rx_count = 0;
	for(i = 0; i < rx_reqs; i++) {
		rc = snap_dma_q_fw_send(q, sqe, m_dma_q_attr.rx_elem_size, m_rmr->lkey);
		ASSERT_EQ(0, rc);
	}
	sleep(1);
	n = snap_dma_q_poll_rx(q, read_comp, rx_reqs);
	for(i = 0; i < n; i++) {
		q->rx_cb(q, read_comp[i].data, read_comp[i].byte_len, read_comp[i].imm_data);
	}
	ASSERT_EQ(rx_reqs, g_rx_count);
	ASSERT_EQ(0, memcmp(g_last_rx, sqe, m_dma_q_attr.rx_elem_size));
	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, poll_rx_verbs)
{
	poll_rx(SNAP_DMA_Q_MODE_VERBS);
}

TEST_F(SnapDmaTest, poll_rx_dv)
{
	poll_rx(SNAP_DMA_Q_MODE_DV);
}

void SnapDmaTest::poll_tx(int mode)
{
	struct snap_dma_q *q;
	int rc, i;
	int tx_reqs = 64;
	struct snap_dma_completion *write_comp[tx_reqs];
	struct snap_dma_completion *read_comp[tx_reqs];
	int n = 0;
	m_dma_q_attr.mode = mode;

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	g_comp_count = 0;
	for(i = 0; i < tx_reqs; i++) {
		write_comp[i] =(struct snap_dma_completion *) malloc(sizeof(struct snap_dma_completion));
		write_comp[i]->count = 1;
		write_comp[i]->func = dma_completion;
		memset(m_rbuf, 0, m_bsize);
		memset(m_lbuf, 0xED, m_bsize);
		rc = snap_dma_q_write(q, m_lbuf, m_bsize, m_lmr->lkey,
				(uintptr_t)m_rbuf, m_rmr->lkey, write_comp[i]);
		ASSERT_EQ(0, rc);
	}

	int k = 0;
	int retry = 1;
again:
	while ((n = snap_dma_q_poll_tx(q, read_comp, tx_reqs)) > 0) {
		for(i = 0; i < n; i++, k++) {
			read_comp[i]->func(read_comp[i], 0);
			free(write_comp[k]);
		}
	}
	/* if batching enabled the op will only go out at first poll, so
	 * give additional time for things to complete
	 */
	if (retry-- && k == 0) {
		sleep(1);
		goto again;
	}

	ASSERT_EQ(tx_reqs, g_comp_count);
	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, poll_tx_verbs)
{
	poll_tx(SNAP_DMA_Q_MODE_VERBS);
}

TEST_F(SnapDmaTest, poll_tx_dv)
{
	poll_tx(SNAP_DMA_Q_MODE_DV);
}

TEST_F(SnapDmaTest, inline_data_test) {
	struct snap_dma_q *q;
	struct ibv_sge rx_sge;
	struct ibv_recv_wr rx_wr, *bad_wr;
	char cqe[32];
	char rem_data[32];
	int rc;
	int n, saved_tx_available;
	struct ibv_wc wc;

	m_dma_q_attr.tx_elem_size = 64;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	struct ibv_mr *rem_mr = snap_reg_mr(m_pd, rem_data, 32);
	saved_tx_available = q->tx_available;
	rx_sge.addr = (uintptr_t)m_rbuf;
	rx_sge.length =  m_dma_q_attr.tx_elem_size;
	rx_sge.lkey = m_rmr->lkey;
	rx_wr.next = NULL;
	rx_wr.sg_list = &rx_sge;
	rx_wr.num_sge = 1;

	rc = ibv_post_recv(q->fw_qp->fw_qp.qp->verbs_qp, &rx_wr, &bad_wr);

	ASSERT_EQ(0, rc);
	memset(m_rbuf, 0, 64);
	memset(cqe, 0xDA, sizeof(cqe));
	memset(rem_data, 0xAD, 32);
	rc = snap_dma_q_send(q, cqe, sizeof(cqe),(uint64_t) rem_data, 32, rem_mr->rkey, NULL);

	ASSERT_EQ(0, rc);
	n = 0;
	while (q->tx_available < saved_tx_available && n < 10000) {
		snap_dma_q_progress(q);
		n++;
	}

	rc = ibv_poll_cq(snap_cq_to_verbs_cq(q->fw_qp->fw_qp.rx_cq), 1, &wc);
	ASSERT_EQ(1, rc);
	ASSERT_EQ(0, memcmp(cqe, m_rbuf, sizeof(cqe)));
	ASSERT_EQ(0, memcmp(rem_data, &m_rbuf[sizeof(cqe)], sizeof(rem_data)));
	snap_dma_q_flush(q);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, error_checks) {
	char data[4096];
	struct snap_dma_q *q;
	int rc;

	/* we should not be able to create q if
	 * tx cannot be done inline */
	m_dma_q_attr.tx_elem_size = 4096;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_FALSE(q);

	/**
	 * Should get an error when trying to send something
	 * that cannot be sent inline
	 */
	m_dma_q_attr.tx_elem_size = 64;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);
	rc = snap_dma_q_send_completion(q, data, sizeof(data));
	ASSERT_NE(0, rc);
	snap_dma_q_destroy(q);
}

/* Test only works on CX6 DX because it assumes that our local
 * memory is accessible via cross function mkey
 */
TEST_F(SnapDmaTest, xgvmi_mkey) {
	struct snap_context *sctx = NULL;
	struct snap_device_attr attr = {};
	struct snap_nvme_cq_attr cq_attr = {};
	struct snap_nvme_sq_attr sq_attr = {};
	struct snap_nvme_cq *cq;
	struct snap_nvme_sq *sq;
	struct ibv_device **dev_list;
	int i, n_dev;
	struct snap_device *sdev;
	struct snap_dma_q *q;
	uint32_t xgvmi_rkey;
	struct snap_cross_mkey *mkey;
	struct snap_nvme_device_attr nvme_device_attr = {};
	struct snap_cross_mkey_attr cm_attr = {};

	/* TODO: check that test is actually working and limit it to
	 * the CX6 DX
	 */
	SKIP_TEST_R("Under construction");
	ASSERT_EQ(0, host_uio_dma_init());
	/* open snap ctx */
	dev_list = ibv_get_device_list(&n_dev);
	ASSERT_TRUE(dev_list);

	for (i = 0; i < n_dev; i++) {
		sctx = snap_open(dev_list[i]);
		if (sctx)
			break;
	}

	ASSERT_TRUE(sctx);
	printf("snap manager is %s\n", dev_list[i]->name);
	ibv_free_device_list(dev_list);

	/* create cq/sq */
	attr.type = SNAP_NVME_PF;
	attr.pf_id = 0;
	sdev = snap_open_device(sctx, &attr);
	ASSERT_TRUE(sdev);
	ASSERT_EQ(0, snap_nvme_init_device(sdev));

	cq_attr.type = SNAP_NVME_RAW_MODE;
	cq_attr.id = 0;
	cq_attr.msix = 0;
	cq_attr.queue_depth = 16;
	cq_attr.base_addr = 0xdeadbeef;
	cq = snap_nvme_create_cq(sdev, &cq_attr);
	ASSERT_TRUE(cq);

	sq_attr.type = SNAP_NVME_RAW_MODE;
	sq_attr.id = 0;
	sq_attr.queue_depth = 16;
	sq_attr.base_addr = 0xbeefdead;
	sq_attr.cq = cq;
	sq = snap_nvme_create_sq(sdev, &sq_attr);
	ASSERT_TRUE(sq);

	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	memset(&sq_attr, 0, sizeof(sq_attr));
	sq_attr.qp = snap_dma_q_get_fw_qp(q);
	sq_attr.state = SNAP_NVME_SQ_STATE_RDY;

	// not working yet
	ASSERT_EQ(0, snap_nvme_modify_sq(sq,
				SNAP_NVME_SQ_MOD_STATE |
				SNAP_NVME_SQ_MOD_QPN,
				&sq_attr));

	ASSERT_EQ(0, snap_nvme_query_device(sdev, &nvme_device_attr));

	cm_attr.vtunnel = sdev->mdev.vtunnel;
	cm_attr.dma_rkey = sdev->dma_rkey;
	cm_attr.vhca_id = snap_get_vhca_id(sdev);
	cm_attr.crossed_vhca_mkey = sdev->crossed_vhca_mkey;

	mkey = snap_create_cross_mkey_by_attr(m_pd, &cm_attr);
	ASSERT_TRUE(mkey);

	xgvmi_rkey = mkey->mkey;
	ASSERT_NE(0U, xgvmi_rkey);

	void *va;
	uintptr_t pa;

	va = host_uio_dma_alloc(m_bsize, &pa);
	ASSERT_TRUE(va);

	/* read data. */
	dma_xfer_test(q, true, true, va, (void *)pa, xgvmi_rkey, m_bsize);
	dma_xfer_test(q, false, true, va, (void *)pa, xgvmi_rkey, m_bsize);

	host_uio_dma_free(va);
	snap_destroy_cross_mkey(mkey);
	snap_dma_q_destroy(q);
	snap_nvme_destroy_sq(sq);
	snap_nvme_destroy_cq(cq);
	snap_nvme_teardown_device(sdev);
	snap_close_device(sdev);
	snap_close(sctx);
	host_uio_dma_destroy();
}

TEST_F(SnapDmaTest, create_destory_indirect_mkey_0) {
	struct snap_indirect_mkey *klm_mkey;
	struct mlx5_devx_mkey_attr mkey_attr = {};

	mkey_attr.addr = 0;
	mkey_attr.size = 0;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = 0;
	mkey_attr.relaxed_ordering_read = 0;
	mkey_attr.klm_array = NULL;
	mkey_attr.klm_num = 0;

	klm_mkey = snap_create_indirect_mkey(m_pd, &mkey_attr);
	ASSERT_TRUE(klm_mkey);

	snap_destroy_indirect_mkey(klm_mkey);
}

TEST_F(SnapDmaTest, create_destory_indirect_mkey_1) {
	struct snap_indirect_mkey *klm_mkey;
	struct mlx5_devx_mkey_attr mkey_attr = {};
	struct mlx5_klm *klm;
	char *buf;
	struct ibv_mr *mr;

	buf = (char *)malloc(4096);
	if (!buf)
		FAIL() << "buffer allocation";

	mr = ibv_reg_mr(m_pd, buf, 4096, IBV_ACCESS_LOCAL_WRITE);
	if (!mr)
		FAIL() << "memory register";

	klm = (struct mlx5_klm *)malloc(sizeof(*klm));
	if (!klm)
		FAIL() << "klm buffer allocation";

	klm->byte_count = mr->length;
	klm->mkey = mr->lkey;
	klm->address = (uintptr_t)mr->addr;

	mkey_attr.addr = klm->address;
	mkey_attr.size = klm->byte_count;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = 0;
	mkey_attr.relaxed_ordering_read = 0;
	mkey_attr.klm_array = klm;
	mkey_attr.klm_num = 1;

	klm_mkey = snap_create_indirect_mkey(m_pd, &mkey_attr);
	ASSERT_TRUE(klm_mkey);

	snap_destroy_indirect_mkey(klm_mkey);
	ibv_dereg_mr(mr);
	free(buf);
}

static int g_umr_wqe_comp;

static void post_umr_completion(struct snap_dma_completion *comp, int status)
{
	g_umr_wqe_comp++;
	g_last_comp_status = status;
}

static void post_umr_wqe(struct snap_dma_q *q,
		struct ibv_pd *pd, int bsize, bool wait_umr_complete)
{
#define MTT_ENTRIES  5
	struct snap_indirect_mkey *klm_mkey;
	struct mlx5_devx_mkey_attr mkey_attr = {};
	int i, j, n, ret, n_bb = 0;
	char *lbuf[MTT_ENTRIES], *rbuf;
	struct ibv_mr *lmr[MTT_ENTRIES], *rmr;
	struct mlx5_klm *klm_mtt;
	struct snap_dma_completion comp;
	struct snap_post_umr_attr umr_attr = {};

	for (i = 0; i < MTT_ENTRIES; i++) {
		lbuf[i] = (char *)malloc(bsize);
		if (!lbuf[i])
			FAIL() << "local buffer allocation";

		lmr[i] = ibv_reg_mr(pd, lbuf[i], bsize, IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (!lmr[i])
			FAIL() << "local memory register";
	}

	rbuf = (char *)malloc(MTT_ENTRIES * bsize);
	if (!rbuf)
		FAIL() << "remote buffer allocation";

	rmr = ibv_reg_mr(pd, rbuf, MTT_ENTRIES * bsize, IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!rmr)
		FAIL() << "remote memory register";

	mkey_attr.addr = 0;
	mkey_attr.size = 0;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = 0;
	mkey_attr.relaxed_ordering_read = 0;
	mkey_attr.klm_array = NULL;
	mkey_attr.klm_num = 0;

	klm_mkey = snap_create_indirect_mkey(pd, &mkey_attr);
	ASSERT_TRUE(klm_mkey);

	klm_mtt = (struct mlx5_klm *)malloc(MTT_ENTRIES * sizeof(*klm_mtt));
	if (!klm_mtt)
		FAIL() << "klm mtt allocation";

	for (i = 0; i < MTT_ENTRIES; i++) {
		klm_mtt[i].byte_count = lmr[i]->length;
		klm_mtt[i].mkey = lmr[i]->lkey;
		klm_mtt[i].address = (uintptr_t)lmr[i]->addr;
	}

	umr_attr.purpose = SNAP_UMR_MKEY_MODIFY_ATTACH_MTT;
	umr_attr.klm_mkey = klm_mkey;
	umr_attr.klm_mtt = klm_mtt;
	umr_attr.klm_entries = MTT_ENTRIES;

	if (wait_umr_complete) {
		comp.func = post_umr_completion;
		comp.count = 1;
		g_umr_wqe_comp = 0;
		ret = snap_umr_post_wqe(q, &umr_attr, &comp, &n_bb);
		ASSERT_EQ(ret, 0);

		n = 0;
		while (n < 10000) {
			ret = snap_dma_q_progress(q);
			if (ret == 1)
				break;
			n++;
		}
		ASSERT_EQ(1, g_umr_wqe_comp);
		ASSERT_EQ(0, g_last_comp_status);
	} else {
		ret = snap_umr_post_wqe(q, &umr_attr, NULL, &n_bb);
		ASSERT_EQ(ret, 0);
	}
	q->tx_available -= n_bb;

	for (j = 0; j < 2; j++) {
		comp.func = dma_completion;
		comp.count = 1;
		g_comp_count = 0;

		if (j == 0) { /* read test */
			for (i = 0; i < MTT_ENTRIES; i++) {
				memset(rbuf + i * bsize, 'A' + i, bsize);
				memset(lbuf[i], 0, bsize);
			}

			ret = snap_dma_q_read(q, (void *)klm_mkey->addr,
					MTT_ENTRIES * bsize, klm_mkey->mkey, (uintptr_t)rbuf, rmr->lkey, &comp);
		} else { /* write test */
			memset(rbuf, 0,  MTT_ENTRIES * bsize);
			for (i = 0; i < MTT_ENTRIES; i++)
				memset(lbuf[i], 'a' + i, bsize);

			ret = snap_dma_q_write(q, (void *)klm_mkey->addr,
					MTT_ENTRIES * bsize, klm_mkey->mkey, (uintptr_t)rbuf, rmr->lkey, &comp);
		}
		ASSERT_EQ(ret, 0);

		n = 0;
		while (n < 10000) {
			ret = snap_dma_q_progress(q);
			if (g_comp_count == 1)
				break;
			n++;
		}

		ASSERT_EQ(1, g_comp_count);
		ASSERT_EQ(0, g_last_comp_status);
		ASSERT_EQ(0, comp.count);
		for (i = 0; i < MTT_ENTRIES; i++)
			ASSERT_EQ(0, memcmp(rbuf + i * bsize, lbuf[i], bsize));
	}

	free(klm_mtt);
	ibv_dereg_mr(rmr);
	free(rbuf);
	for (i = 0; i < MTT_ENTRIES; i++) {
		ibv_dereg_mr(lmr[i]);
		free(lbuf[i]);
	}
	snap_destroy_indirect_mkey(klm_mkey);
}

TEST_F(SnapDmaTest, post_umr_wqe_no_wait_complete) {
	struct snap_dma_q *q;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	post_umr_wqe(q, m_pd, m_bsize, false);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, post_umr_wqe_wait_complete) {
	struct snap_dma_q *q;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	post_umr_wqe(q, m_pd, m_bsize, true);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, post_umr_wqe_warp_around_no_wait_complete) {
	struct snap_dma_q *q;
	struct snap_dv_qp *dv_qp;
	unsigned i;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	/*
	 * post enough RDMA READ wqe to make SQ only left 3 WQE BB to the end.
	 * Because the UMR WQE used below need consume 4 WQE BB, so it will warp around.
	 */
	dv_qp = &q->sw_qp.dv_qp;
	for (i = 0; i < dv_qp->hw_qp.sq.wqe_cnt - 3; i++)
		dma_xfer_test(q, true, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	post_umr_wqe(q, m_pd, m_bsize, false);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, post_umr_wqe_warp_around_wait_complete) {
	struct snap_dma_q *q;
	struct snap_dv_qp *dv_qp;
	unsigned i;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	/*
	 * post enough RDMA READ wqe to make SQ only left 3 WQE BB to the end.
	 * Because the UMR WQE used below need consume 4 WQE BB, so it will warp around.
	 */
	dv_qp = &q->sw_qp.dv_qp;
	for (i = 0; i < dv_qp->hw_qp.sq.wqe_cnt - 3; i++)
		dma_xfer_test(q, true, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	post_umr_wqe(q, m_pd, m_bsize, true);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, post_umr_wqe_reuse_wait_complete) {
	struct snap_dma_q *q;
	struct snap_dv_qp *dv_qp;
	unsigned i;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	/*
	 * first post wqe_cnt RDMA READ wqe to occupied the whole SQ buffer,
	 * then post umr wqe to let it reuse the WQE BBs which hold dirty data.
	 */
	dv_qp = &q->sw_qp.dv_qp;
	for (i = 0; i < dv_qp->hw_qp.sq.wqe_cnt; i++)
		dma_xfer_test(q, true, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	post_umr_wqe(q, m_pd, m_bsize, true);

	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, post_umr_wqe_reuse_no_wait_complete) {
	struct snap_dma_q *q;
	struct snap_dv_qp *dv_qp;
	int i;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	/*
	 * first post wqe_cnt RDMA READ wqe to occupied the whole SQ buffer,
	 * then post umr wqe to let it reuse the WQE BBs which hold dirty data.
	 */
	dv_qp = &q->sw_qp.dv_qp;
	for (i = 0; i < (int)dv_qp->hw_qp.sq.wqe_cnt; i++)
		dma_xfer_test(q, true, false, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	post_umr_wqe(q, m_pd, m_bsize, false);

	snap_dma_q_destroy(q);
}

static void snap_dma_q_rw_iov(struct snap_dma_q_create_attr *dma_q_attr,
		struct ibv_pd *pd, int bsize)
{
#define IOV_CNT 3
	struct snap_dma_q *q;
	int i, j, n, ret;
	char *lbuf, *rbuf;
	struct ibv_mr *lmr, *rmr;
	struct iovec l_iov;
	struct iovec iov[2][IOV_CNT];
	struct snap_dma_completion comp;

	q = snap_dma_q_create(pd, dma_q_attr);
	ASSERT_TRUE(q);
	ASSERT_TRUE(q->iov_support);

	lbuf = (char *)malloc(bsize * IOV_CNT * 2);
	if (!lbuf)
		FAIL() << "local buffer allocation";

	lmr = ibv_reg_mr(pd, lbuf, bsize * IOV_CNT * 2, IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!lmr)
		FAIL() << "local memory register";

	rbuf = (char *)malloc(IOV_CNT * bsize);
	if (!rbuf)
		FAIL() << "remote buffer allocation";

	rmr = ibv_reg_mr(pd, rbuf, IOV_CNT * bsize, IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!rmr)
		FAIL() << "remote memory register";

	/*
	 * iov[0] ----->  --------
	 *               |iov_base|--> lbuf[0 * bsize]
	 *               |iov_len |
	 *                --------
	 *               |iov_base|--> lbuf[2 * bsize]
	 *               |iov_len |
	 *                --------
	 *               |iov_base|--> lbuf[4 * bsize]
	 *               |iov_len |
	 *                --------
	 *               |    .   |
	 *               |    .   |
	 *
	 * iov[1] ----->  --------
	 *               |iov_base|--> lbuf[1 * bsize]
	 *               |iov_len |
	 *                --------
	 *               |iov_base|--> lbuf[3 * bsize]
	 *               |iov_len |
	 *                --------
	 *               |iov_base|--> lbuf[5 * bsize]
	 *               |iov_len |
	 *                --------
	 *               |    .   |
	 *               |    .   |
	 */
	for (j = 0; j < IOV_CNT; j++) {
		iov[0][j].iov_base = lbuf + (2 * j) * bsize;
		iov[0][j].iov_len =  bsize;
		iov[1][j].iov_base = lbuf + (2 * j + 1) * bsize;
		iov[1][j].iov_len =  bsize;
	}

	memset(rbuf, 0, bsize * IOV_CNT);
	for (i = 0; i < IOV_CNT; i++) {
		memset(lbuf + 2 * i * bsize, 'A' + i, bsize);
		memset(lbuf + (2 * i + 1) * bsize, 'a' + i, bsize);
	}

	l_iov.iov_base = rbuf;
	l_iov.iov_len = IOV_CNT * bsize;

	for (j = 0; j < 2; j++) {
		comp.func = dma_completion;
		comp.count = 1;
		g_comp_count = 0;

		if (j == 0) { /* readv: iov[0] --> rbuf */
			ret = snap_dma_q_readv2v(q, &rmr->lkey, &l_iov, 1,
					&lmr->lkey, &iov[0][0], IOV_CNT, true, true, &comp);
		} else { /* writev: rbuf --> iov[1] */
			ret = snap_dma_q_writev2v(q, &rmr->lkey, &l_iov, 1,
					&lmr->lkey, &iov[1][0], IOV_CNT, true, true, &comp);
		}
		ASSERT_EQ(ret, 0);

		n = 0;
		while (n < 10) {
			ret = snap_dma_q_progress(q);
			if (ret == 1)
				break;
			sleep(1);
			n++;
		}

		ASSERT_EQ(1, g_comp_count);
		ASSERT_EQ(0, g_last_comp_status);
		ASSERT_EQ(0, comp.count);
	}

	for (i = 0; i < IOV_CNT; i++)
		ASSERT_EQ(0, memcmp(lbuf + 2 * i * bsize, lbuf + (2 * i + 1) * bsize, bsize));

	ibv_dereg_mr(rmr);
	free(rbuf);
	ibv_dereg_mr(lmr);
	free(lbuf);
	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, rdma_iov_rw_verbs) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_VERBS;
	m_dma_q_attr.iov_enable = true;
	snap_dma_q_rw_iov(&m_dma_q_attr, m_pd, m_bsize);
}

TEST_F(SnapDmaTest, rdma_iov_rw_dv) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.iov_enable = true;
	snap_dma_q_rw_iov(&m_dma_q_attr, m_pd, m_bsize);
}

TEST_F(SnapDmaTest, rdma_iov_rw_gga) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_GGA;
	m_dma_q_attr.iov_enable = true;
	snap_dma_q_rw_iov(&m_dma_q_attr, m_pd, m_bsize);
}

static void snap_dma_q_rw_iov2v(struct snap_dma_q_create_attr *dma_q_attr,
		struct ibv_pd *pd, int bsize)
{
#define IOV_CNT 3
	int i, j, n, ret;
	struct snap_dma_q *q;
	struct ibv_mr *mr0[IOV_CNT * 4], *mr1[IOV_CNT * 2], *mr2[IOV_CNT];
	struct iovec iov0[IOV_CNT * 4], iov1[IOV_CNT * 2], iov2[IOV_CNT];
	uint32_t mkey0[IOV_CNT * 4], mkey1[IOV_CNT * 2], mkey2[IOV_CNT];
	struct snap_dma_completion comp;

	q = snap_dma_q_create(pd, dma_q_attr);
	ASSERT_TRUE(q);

	for (i = 0; i < IOV_CNT * 4; i++) {
		iov0[i].iov_len = bsize / 2;
		iov0[i].iov_base = calloc(1, iov0[i].iov_len);
		if (!iov0[i].iov_base)
			FAIL() << "iov0 allocation";

		mr0[i] = ibv_reg_mr(pd, iov0[i].iov_base, iov0[i].iov_len,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (!mr0[i])
			FAIL() << "iov0 register";

		mkey0[i] = mr0[i]->lkey;
	}

	for (i = 0; i < IOV_CNT * 2; i++) {
		iov1[i].iov_len = bsize;
		iov1[i].iov_base = malloc(iov1[i].iov_len);
		if (!iov1[i].iov_base)
			FAIL() << "iov1 allocation";

		mr1[i] = ibv_reg_mr(pd, iov1[i].iov_base, iov1[i].iov_len,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (!mr1[i])
			FAIL() << "iov1 register";

		mkey1[i] = mr1[i]->lkey;

		memset(iov1[i].iov_base, 'A' + i, iov1[i].iov_len);
	}

	for (i = 0; i < IOV_CNT; i++) {
		iov2[i].iov_len = bsize * 2;
		iov2[i].iov_base = calloc(1, iov2[i].iov_len);
		if (!iov2[i].iov_base)
			FAIL() << "iov2 allocation";

		mr2[i] = ibv_reg_mr(pd, iov2[i].iov_base, iov2[i].iov_len,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		if (!mr2[i])
			FAIL() << "iov2 register";

		mkey2[i] = mr2[i]->lkey;
	}

	for (j = 0; j < 2; j++) {
		comp.func = dma_completion;
		comp.count = 1;
		g_comp_count = 0;

		if (j == 0) { /* readv2v: iov1 -> iov0 */
			ret = snap_dma_q_readv2v(q, mkey0, iov0, IOV_CNT * 4,
						mkey1, iov1, IOV_CNT * 2, false, false, &comp);
		} else { /* writev2v: iov0 -> iov2 */
			ret = snap_dma_q_writev2v(q, mkey0, iov0, IOV_CNT * 4,
						mkey2, iov2, IOV_CNT, false, false, &comp);
		}
		ASSERT_EQ(ret, 0);

		n = 0;
		while (n < 10) {
			ret = snap_dma_q_progress(q);
			if (ret == 1)
				break;
			sleep(1);
			n++;
		}

		ASSERT_EQ(1, g_comp_count);
		ASSERT_EQ(0, g_last_comp_status);
		ASSERT_EQ(0, comp.count);
	}

	for (i = 0; i < IOV_CNT; i++) {
		ASSERT_EQ(0, memcmp(iov2[i].iov_base, iov1[2 * i].iov_base, bsize));
		ASSERT_EQ(0, memcmp((void *)((char *)iov2[i].iov_base + bsize), iov1[2 * i + 1].iov_base, bsize));
	}

	for (i = 0; i < IOV_CNT * 4; i++) {
		ibv_dereg_mr(mr0[i]);
		free(iov0[i].iov_base);
	}
	for (i = 0; i < IOV_CNT * 2; i++) {
		ibv_dereg_mr(mr1[i]);
		free(iov1[i].iov_base);
	}
	for (i = 0; i < IOV_CNT; i++) {
		ibv_dereg_mr(mr2[i]);
		free(iov2[i].iov_base);
	}
	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, rdma_iov2v_rw_verbs) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_VERBS;
	m_dma_q_attr.iov_enable = true;
	snap_dma_q_rw_iov2v(&m_dma_q_attr, m_pd, m_bsize);
}

TEST_F(SnapDmaTest, rdma_iov2v_rw_dv) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.iov_enable = true;
	snap_dma_q_rw_iov2v(&m_dma_q_attr, m_pd, m_bsize);
}

TEST_F(SnapDmaTest, rdma_iov2v_rw_gga) {
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_GGA;
	m_dma_q_attr.iov_enable = true;
	snap_dma_q_rw_iov2v(&m_dma_q_attr, m_pd, m_bsize);
}

static void post_umr_modify_mkey(struct ibv_pd *pd,
		struct snap_dma_q_create_attr *dma_q_attr,
		bool attach_bsf, bool attach_mtt, bool wait_completion)
{
#define MTT_ENTRIES  5
	int i, ret, n, n_bb = 0;
	struct snap_dma_q *q;
	struct snap_indirect_mkey *klm_mkey;
	struct mlx5_devx_mkey_attr mkey_attr = {};
	struct snap_post_umr_attr attr = {};
	struct snap_dma_completion comp;
	char buf[MTT_ENTRIES][32];
	struct ibv_mr *mr[MTT_ENTRIES];
	struct mlx5_klm klm_mtt[MTT_ENTRIES];

	dma_q_attr->mode = SNAP_DMA_Q_MODE_DV;
	q = snap_dma_q_create(pd, dma_q_attr);
	ASSERT_TRUE(q);

	mkey_attr.addr = 0;
	mkey_attr.size = 0;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = 0;
	mkey_attr.relaxed_ordering_read = 0;
	mkey_attr.klm_array = NULL;
	mkey_attr.klm_num = 0;
	if (attach_bsf) {
		mkey_attr.bsf_en = true;
		mkey_attr.crypto_en = true;
	}

	klm_mkey = snap_create_indirect_mkey(pd, &mkey_attr);
	ASSERT_TRUE(klm_mkey);

	attr.klm_mkey = klm_mkey;
	if (attach_mtt) {
		for (i = 0; i < MTT_ENTRIES; i++) {
			mr[i] = ibv_reg_mr(pd, buf[i], 32, IBV_ACCESS_LOCAL_WRITE |
					IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
			if (!mr[i])
				FAIL() << "local memory register";
		}

		for (i = 0; i < MTT_ENTRIES; i++) {
			klm_mtt[i].byte_count = mr[i]->length;
			klm_mtt[i].mkey = mr[i]->lkey;
			klm_mtt[i].address = (uintptr_t)mr[i]->addr;
		}

		attr.purpose |= SNAP_UMR_MKEY_MODIFY_ATTACH_MTT;
		attr.klm_mtt = klm_mtt;
		attr.klm_entries = MTT_ENTRIES;
	}

	if (attach_bsf) {
		attr.purpose |= SNAP_UMR_MKEY_MODIFY_ATTACH_CRYPTO_BSF;
		attr.encryption_order = SNAP_CRYPTO_BSF_ENCRYPTION_ORDER_ENCRYPTED_MEMORY_SIGNATURE;
		attr.encryption_standard = SNAP_CRYPTO_BSF_ENCRYPTION_STANDARD_AES_XTS;
		attr.raw_data_size = 0;
		attr.crypto_block_size_pointer = SNAP_CRYPTO_BSF_CRYPTO_BLOCK_SIZE_POINTER_512;
		attr.dek_pointer = 0;
		memset(attr.keytag, 0, SNAP_CRYPTO_KEYTAG_SIZE);
		attr.xts_initial_tweak = 0;
	}

	if (wait_completion) {
		comp.func = post_umr_completion;
		comp.count = 1;
		g_umr_wqe_comp = 0;
		ret = snap_umr_post_wqe(q, &attr, &comp, &n_bb);
		ASSERT_EQ(ret, 0);

		q->tx_available -= n_bb;
		n = 0;
		while (n < 10) {
			ret = snap_dma_q_progress(q);
			if (ret == 1)
				break;
			sleep(1);
			n++;
		}
		ASSERT_EQ(1, g_umr_wqe_comp);
		ASSERT_EQ(0, g_last_comp_status);
	} else {
		ret = snap_umr_post_wqe(q, &attr, NULL, &n_bb);
		ASSERT_EQ(ret, 0);
	}

	if (attach_mtt) {
		for (i = 0; i < MTT_ENTRIES; i++) {
			ibv_dereg_mr(mr[i]);
		}
	}
	snap_destroy_indirect_mkey(klm_mkey);
	snap_dma_q_destroy(q);
}

TEST_F(SnapDmaTest, attach_crypto_bsf_to_mkey_wait_complete) {
	post_umr_modify_mkey(m_pd, &m_dma_q_attr, true, false, true);
}

TEST_F(SnapDmaTest, attach_crypto_bsf_to_mkey_no_wait_complete) {
	post_umr_modify_mkey(m_pd, &m_dma_q_attr, true, false, false);
}

TEST_F(SnapDmaTest, attach_mtt_to_mkey_wait_complete) {
	post_umr_modify_mkey(m_pd, &m_dma_q_attr, false, true, true);
}

TEST_F(SnapDmaTest, attach_mtt_to_mkey_no_wait_complete) {
	post_umr_modify_mkey(m_pd, &m_dma_q_attr, false, true, false);
}

TEST_F(SnapDmaTest, attach_crypto_bsf_and_mtt_to_mkey_wait_complete) {
	post_umr_modify_mkey(m_pd, &m_dma_q_attr, true, true, true);
}

TEST_F(SnapDmaTest, attach_crypto_bsf_and_mtt_to_mkey_no_wait_complete) {
	post_umr_modify_mkey(m_pd, &m_dma_q_attr, true, true, false);
}

TEST_F(SnapDmaTest, devx_dma_only_q) {
	struct snap_dma_q *dma_q;
	struct snap_dma_q *dummy_q;
	int ret;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.tx_qsize = 16;
	m_dma_q_attr.tx_elem_size = 0;
	m_dma_q_attr.rx_qsize = 0;

	dma_q = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dma_q);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.tx_qsize = 0;
	m_dma_q_attr.rx_qsize = 0;

	dummy_q = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dummy_q);

	ret = snap_dma_ep_connect(dma_q, dummy_q);
	EXPECT_EQ(0, ret);

	dma_xfer_test(dma_q, true, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);
	dma_xfer_test(dma_q, false, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	snap_dma_ep_destroy(dma_q);
	snap_dma_ep_destroy(dummy_q);
}

TEST_F(SnapDmaTest, devx_dma_q_connect_via_qpn) {
	struct snap_dma_q *dma_q;
	struct snap_dma_q *dummy_q;
	int ret;
	int dma_q_qpn;
	int dummy_q_qpn;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.tx_qsize = 16;
	m_dma_q_attr.tx_elem_size = 0;
	m_dma_q_attr.rx_qsize = 0;

	dma_q = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dma_q);
	dma_q_qpn = snap_qp_get_qpnum(dma_q->sw_qp.qp);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.tx_qsize = 0;
	m_dma_q_attr.rx_qsize = 0;

	dummy_q = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dummy_q);
	dummy_q_qpn = snap_qp_get_qpnum(dummy_q->sw_qp.qp);

	ret = snap_dma_ep_connect_remote_qpn(dummy_q, dma_q_qpn);
	EXPECT_EQ(0, ret);

	ret = snap_dma_ep_connect_remote_qpn(dma_q, dummy_q_qpn);
	EXPECT_EQ(0, ret);

	dma_xfer_test(dma_q, true, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);
	dma_xfer_test(dma_q, false, true, m_rbuf, m_rbuf, m_rmr->lkey, m_bsize);

	snap_dma_ep_destroy(dma_q);
	snap_dma_ep_destroy(dummy_q);
}

TEST_F(SnapDmaTest, dma_pd_mismatch) {
	struct snap_dma_q *dma_q;
	struct snap_dma_q *dummy_q;
	struct ibv_pd *dummy_pd;
	struct ibv_mr *rmr;
	int ret;

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.tx_qsize = 16;
	m_dma_q_attr.tx_elem_size = 0;
	m_dma_q_attr.rx_qsize = 0;

	dma_q = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dma_q);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.tx_qsize = 0;
	m_dma_q_attr.rx_qsize = 0;

	dummy_pd = ibv_alloc_pd(m_pd->context);
	ASSERT_TRUE(dummy_pd);
	rmr = ibv_reg_mr(dummy_pd, m_rbuf, m_bcount * m_bsize,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
			IBV_ACCESS_REMOTE_READ);
	ASSERT_TRUE(rmr);
	dummy_q = snap_dma_ep_create(dummy_pd, &m_dma_q_attr);
	ASSERT_TRUE(dummy_q);

	ret = snap_dma_ep_connect(dma_q, dummy_q);
	EXPECT_EQ(0, ret);

	dma_xfer_test(dma_q, true, true, m_rbuf, m_rbuf, rmr->lkey, m_bsize);
	dma_xfer_test(dma_q, false, true, m_rbuf, m_rbuf, rmr->lkey, m_bsize);

	snap_dma_ep_destroy(dma_q);
	snap_dma_ep_destroy(dummy_q);
	ibv_dereg_mr(rmr);
	ibv_dealloc_pd(dummy_pd);
}

/* DPA section */
TEST_F(SnapDmaTest, dpa_ep_create_polling) {
	struct snap_dma_q *dpu_qp;
	struct snap_dma_q *dpa_qp;
	struct snap_dpa_ctx *dpa_ctx;
	int ret;

	if (!snap_dpa_enabled(m_pd->context))
		SKIP_TEST_R("DPA is not available");

	dpa_ctx = snap_dpa_process_create(m_pd->context, "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpu_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpu_qp);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_POLLING;
	m_dma_q_attr.dpa_proc = dpa_ctx;

	dpa_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpa_qp);

	ret = snap_dma_ep_connect(dpu_qp, dpa_qp);
	EXPECT_EQ(0, ret);

	snap_dma_ep_destroy(dpa_qp);
	snap_dma_ep_destroy(dpu_qp);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDmaTest, dpa_ep_create_event) {
	struct snap_dma_q *dpu_qp;
	struct snap_dma_q *dpa_qp;
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr;
	int ret;

	if (!snap_dpa_enabled(m_pd->context))
		SKIP_TEST_R("DPA is not available");

	dpa_ctx = snap_dpa_process_create(m_pd->context, "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpa_thr = snap_dpa_thread_create(dpa_ctx, 0);
	ASSERT_TRUE(dpa_thr);

	dpu_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpu_qp);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_EVENT;
	m_dma_q_attr.dpa_thread = dpa_thr;

	dpa_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpa_qp);

	ret = snap_dma_ep_connect(dpu_qp, dpa_qp);
	EXPECT_EQ(0, ret);

	snap_dma_ep_destroy(dpa_qp);
	snap_dma_ep_destroy(dpu_qp);
	snap_dpa_thread_destroy(dpa_thr);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDmaTest, dpa_ep_create_trigger) {
	struct snap_dma_q *dpu_qp;
	struct snap_dma_q *dpa_qp;
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr;
	int ret;

	if (!snap_dpa_enabled(m_pd->context))
		SKIP_TEST_R("DPA is not available");

	dpa_ctx = snap_dpa_process_create(m_pd->context, "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpa_thr = snap_dpa_thread_create(dpa_ctx, 0);
	ASSERT_TRUE(dpa_thr);

	dpu_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpu_qp);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_TRIGGER;
	m_dma_q_attr.dpa_thread = dpa_thr;

	dpa_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpa_qp);

	ret = snap_dma_ep_connect(dpu_qp, dpa_qp);
	EXPECT_EQ(0, ret);

	snap_dma_ep_destroy(dpa_qp);
	snap_dma_ep_destroy(dpu_qp);
	snap_dpa_thread_destroy(dpa_thr);
	snap_dpa_process_destroy(dpa_ctx);
}

TEST_F(SnapDmaTest, dpa_rdma_from_dpu) {
	struct snap_dma_q *dpu_qp;
	struct snap_dma_q *dpa_qp;
	struct snap_dpa_ctx *dpa_ctx;
	int ret;
	struct snap_dpa_memh *dpa_mem;
	struct snap_dpa_mkeyh *dpa_mkey;
	char tmp_buf[m_bsize];

	if (!snap_dpa_enabled(m_pd->context))
		SKIP_TEST_R("DPA is not available");

	dpa_ctx = snap_dpa_process_create(m_pd->context, "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpu_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpu_qp);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_POLLING;
	m_dma_q_attr.dpa_proc = dpa_ctx;

	dpa_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpa_qp);

	ret = snap_dma_ep_connect(dpu_qp, dpa_qp);
	EXPECT_EQ(0, ret);

	dpa_mem = snap_dpa_mem_alloc(dpa_ctx, m_bsize);
	ASSERT_TRUE(dpa_mem);

	dpa_mkey = snap_dpa_mkey_alloc(dpa_ctx, m_pd);

	/* TODO: alloc dpa mem, use it to test rdma write and read */
	memset(tmp_buf, 0xED, m_bsize);
	/* this will write buffer filled with 0xED to dpa */
	dma_xfer_test(dpu_qp, false, true, 0, (void *)snap_dpa_mem_addr(dpa_mem), snap_dpa_mkey_id(dpa_mkey), m_bsize);
	/* read the same buffer back to m_lbuf */
	dma_xfer_test(dpu_qp, true, true, 0, (void *)snap_dpa_mem_addr(dpa_mem), snap_dpa_mkey_id(dpa_mkey), m_bsize);
	ASSERT_EQ(0, memcmp(m_lbuf, tmp_buf, m_bsize));

	snap_dpa_mkey_free(dpa_mkey);
	snap_dpa_mem_free(dpa_mem);
	snap_dma_ep_destroy(dpa_qp);
	snap_dma_ep_destroy(dpu_qp);
	snap_dpa_process_destroy(dpa_ctx);
}

static void fill_pattern(char *buf, int n)
{
	int i;

	/* todo: random pattern */
	for (i = 0; i < n; i++)
		buf[i] = (uint8_t)((uint64_t)buf + n) & 0xff;
}

TEST_F(SnapDmaTest, dpa_memcpy_stress) {
	struct snap_dma_q *dpu_qp;
	struct snap_dma_q *dpa_qp;
	struct snap_dpa_ctx *dpa_ctx;
	int ret, i;
	struct snap_dpa_memh *dpa_mem;
	struct snap_dpa_mkeyh *dpa_mkey;
	char tmp_buf[m_bsize];

	if (!snap_dpa_enabled(m_pd->context))
		SKIP_TEST_R("DPA is not available");

	dpa_ctx = snap_dpa_process_create(m_pd->context, "dpa_hello");
	ASSERT_TRUE(dpa_ctx);

	dpu_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpu_qp);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_POLLING;
	m_dma_q_attr.dpa_proc = dpa_ctx;

	dpa_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpa_qp);

	ret = snap_dma_ep_connect(dpu_qp, dpa_qp);
	ASSERT_EQ(0, ret);

	dpa_mem = snap_dpa_mem_alloc(dpa_ctx, m_bsize);
	ASSERT_TRUE(dpa_mem);

	dpa_mkey = snap_dpa_mkey_alloc(dpa_ctx, m_pd);
	ASSERT_TRUE(dpa_mkey);

	for (i = 1; i < m_bsize; i++) {
		memset(m_lbuf, 0, i);
		fill_pattern(tmp_buf, i);
		ret = snap_dpa_memcpy(dpa_ctx, snap_dpa_mem_addr(dpa_mem), tmp_buf, i);
		if (ret)
			printf("Failed to copy %d bytes\n", i);
		ASSERT_EQ(0, ret);
		ret = snap_dma_q_read(dpu_qp, m_lbuf, i, m_lmr->lkey,
				snap_dpa_mem_addr(dpa_mem), snap_dpa_mkey_id(dpa_mkey), NULL);
		ASSERT_EQ(0, ret);
		snap_dma_q_flush(dpu_qp);
		ASSERT_EQ(0, memcmp(m_lbuf, tmp_buf, i));
	}

	snap_dpa_mkey_free(dpa_mkey);
	snap_dpa_mem_free(dpa_mem);
	snap_dma_ep_destroy(dpa_qp);
	snap_dma_ep_destroy(dpu_qp);
	snap_dpa_process_destroy(dpa_ctx);
}

/*
 * Check that DPA can do basic dma operations
 * See dpa/dpa_dma_test.c
 */
TEST_F(SnapDmaTest, dpa_rdma_from_dpa) {
	int poll_cycles = 120;
	struct snap_dma_q *dpu_qp;
	struct snap_dma_q *dpa_qp;
	struct snap_dpa_ctx *dpa_ctx;
	struct snap_dpa_thread *dpa_thr;
	int ret, n;
	struct snap_rx_completion comp;

	if (!snap_dpa_enabled(m_pd->context))
		SKIP_TEST_R("DPA is not available");

	dpa_ctx = snap_dpa_process_create(m_pd->context, "dpa_dma_test");
	ASSERT_TRUE(dpa_ctx);

	dpu_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpu_qp);

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.dpa_mode = SNAP_DMA_Q_DPA_MODE_POLLING;
	m_dma_q_attr.dpa_proc = dpa_ctx;

	dpa_qp = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(dpa_qp);

	ret = snap_dma_ep_connect(dpu_qp, dpa_qp);
	ASSERT_EQ(0, ret);

	dpa_thr = snap_dpa_thread_create(dpa_ctx, 0);
	ASSERT_TRUE(dpa_thr);

	ret = snap_dma_ep_dpa_copy_sync(dpa_thr, dpa_qp);
	ASSERT_EQ(0, ret);

	/* rbuf because mr has required access permissions and lbuf does not */
	strcpy(m_rbuf, "I am a DPU buffer\n");
	ret = snap_dpa_thread_mr_copy_sync(dpa_thr, (uint64_t)m_rbuf, m_bsize, m_rmr->lkey);
	ASSERT_EQ(0, ret);
	/* at this point dpa thread is running dma tests */

	/* Sync on ping pong test */
	printf("Waiting for DPA PING\n");
	do {
		n = snap_dma_q_poll_rx(dpu_qp, &comp, 1);
		if (n)
			break;
		sleep(1);
		printf(".\n");
	} while (poll_cycles-- > 0);
	printf("Got ping\n");

	EXPECT_EQ(1, n);
	if (n == 1) {
		char msg[16] = "PONG";

		EXPECT_EQ(0, memcmp(comp.data, "PING", 4));
		ret = snap_dma_q_send_completion(dpu_qp, msg, sizeof(msg));
		EXPECT_EQ(0, ret);
		snap_dma_q_flush(dpu_qp);
	}

	sleep(1);
	snap_dpa_log_print(dpa_thr->dpa_log);
	/* dpa thread destroy is a sync point */
	snap_dpa_thread_destroy(dpa_thr);
	printf("done, r_buf: %s status %s\n", m_rbuf, m_rbuf + m_bsize - 16);
	/* at this point rbuf data a written by the DPA */
	EXPECT_EQ(0, strcmp(m_rbuf, "I am a DPA buffer\n"));

	snap_dma_ep_destroy(dpa_qp);
	snap_dma_ep_destroy(dpu_qp);
	snap_dpa_process_destroy(dpa_ctx);
}

void SnapDmaTest::worker_poll_rx()
{
	struct snap_dma_worker *wk;
	char *sqe = m_rbuf;
	int rc, i;
	int rx_reqs = 16;
	snap_dma_worker_create_attr wk_attr;
	struct snap_dma_q *q[2];

	wk_attr.mode = SNAP_DMA_WORKER_MODE_SHARED_CQ;
	wk_attr.exp_queue_num = 2;
	wk_attr.exp_queue_rx_size = 64;

	wk = snap_dma_worker_create(m_pd, &wk_attr);
	ASSERT_TRUE(wk);

	m_dma_q_attr.wk = wk;
	m_dma_q_attr.sw_use_devx = true;
	for(i = 0; i < 2; i++) {
		q[i] = snap_dma_q_create(m_pd, &m_dma_q_attr);
		ASSERT_TRUE(q[i]);
	}

	memset(sqe, 0xDA, wk_attr.exp_queue_rx_size);

	g_rx_count = 0;
	for(i = 0; i < rx_reqs; i++) {
		rc = snap_dma_q_fw_send(wk->queues[0], sqe, wk_attr.exp_queue_rx_size,
				m_rmr->lkey);
		rc = snap_dma_q_fw_send(wk->queues[1], sqe, wk_attr.exp_queue_rx_size,
				m_rmr->lkey);
		ASSERT_EQ(0, rc);
	}
	sleep(1);

	snap_dma_worker_progress_rx(wk);

	ASSERT_EQ(rx_reqs * 2, g_rx_count);
	ASSERT_EQ(0, memcmp(g_last_rx, sqe, m_dma_q_attr.rx_elem_size));
	snap_dma_q_destroy(q[0]);
	snap_dma_q_destroy(q[1]);
	snap_dma_worker_destroy(wk);
}

TEST_F(SnapDmaTest, poll_rx_worker)
{
	worker_poll_rx();
}

void SnapDmaTest::worker_poll_tx()
{
	struct snap_dma_worker *wk;
	int rc, i;
	int tx_reqs = 64;
	struct snap_dma_completion *write_comp[tx_reqs];
	int n = 0;
	struct snap_dma_q *q;
	snap_dma_worker_create_attr wk_attr;

	wk_attr.mode = SNAP_DMA_WORKER_MODE_SHARED_CQ;
	wk_attr.exp_queue_num = 1;
	wk_attr.exp_queue_rx_size = 64;

	wk = snap_dma_worker_create(m_pd, &wk_attr);
	ASSERT_TRUE(wk);

	m_dma_q_attr.wk = wk;
	m_dma_q_attr.sw_use_devx = true;
	q = snap_dma_q_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(q);

	g_comp_count = 0;
	for(i = 0; i < tx_reqs; i++) {
		write_comp[i] =(struct snap_dma_completion *) malloc(sizeof(struct snap_dma_completion));
		write_comp[i]->count = 1;
		write_comp[i]->func = dma_completion;
		memset(m_rbuf, 0, m_bsize);
		memset(m_lbuf, 0xED, m_bsize);
		rc = snap_dma_q_write(wk->queues[0], m_lbuf, m_bsize, m_lmr->lkey,
				(uintptr_t)m_rbuf, m_rmr->lkey, write_comp[i]);
		ASSERT_EQ(0, rc);
	}

	int k = 0;
	int retry = 1;
again:
	while ((n = snap_dma_worker_progress_tx(wk)) > 0) {
	}
	/* if batching enabled the op will only go out at first poll, so
	 * give additional time for things to complete
	 */
	if (retry-- && k == 0) {
		sleep(1);
		goto again;
	}

	ASSERT_EQ(tx_reqs, g_comp_count);
	snap_dma_q_destroy(q);
	snap_dma_worker_destroy(wk);
}

TEST_F(SnapDmaTest, poll_tx_worker)
{
	worker_poll_tx();
}

/* CRYPTO test case */
#define NVMF_CRYPTO_KEY1 "12345678123456781234567812345678"
#define NVMF_CRYPTO_KEY2 "deadbeefdeadbeefdeadbeefdeadbeef"
/* initial tweak value for tests */
#define NVMF_LBA 42
static inline int __c2v(char c)
{
	if ((c >= '0') && (c <= '9')) {
		return c - '0';
	}
	if ((c >= 'a') && (c <= 'f')) {
		return c - 'a' + 10;
	}
	if ((c >= 'A') && (c <= 'F')) {
		return c - 'A' + 10;
	}
	return -1;
}

static nvmf_crypto_key_load(const char *key1, const char *key2, uint8_t *out, size_t *out_size)
{
	size_t i;
	const size_t key1_len = strlen(key1);
	const size_t key2_len = strlen(key2);
	const size_t key_len = (key1_len + key2_len) / 2;

	if (key1_len & 1) {
		printf("Invalid key1 length, can't be odd. Key: %s, len: %zu", key1, key1_len);
		return -1;
	}

	if (key2_len & 1) {
		printf("Invalid key2 length, can't be odd. Key: %s, len: %zu", key2, key2_len);
		return -1;
	}

	if (key_len > *out_size) {
		printf("Key buffer is too small, required: %zu, provided: %zu", key_len, *out_size);
		return -1;
	}

	if (key_len != 32 && key_len != 64) {
		printf("Unsupported key length");
		return -1;
	}

	for (i = 0; i < key1_len; i += 2) {
		const int v0 = __c2v(key1[i]);
		const int v1 = __c2v(key1[i + 1]);
		if (v0 < 0 || v1 < 0) {
			printf("Invalid key1");
			return -1;
		}
		*out++ = (v0 << 4) + v1;
	}

	for (i = 0; i < key2_len; i += 2) {
		const int v0 = __c2v(key2[i]);
		const int v1 = __c2v(key2[i + 1]);
		if (v0 < 0 || v1 < 0) {
			printf("Invalid key2");
			return -1;
		}
		*out++ = (v0 << 4) + v1;
	}

	*out_size = key_len;
	return 0;
}

extern "C" {

enum mlx5_devx_obj_type {
	MLX5_DEVX_FLOW_TABLE		= 1,
	MLX5_DEVX_FLOW_COUNTER		= 2,
	MLX5_DEVX_FLOW_METER		= 3,
	MLX5_DEVX_QP			= 4,
	MLX5_DEVX_PKT_REFORMAT_CTX	= 5,
	MLX5_DEVX_TIR			= 6,
	MLX5_DEVX_FLOW_GROUP		= 7,
	MLX5_DEVX_FLOW_TABLE_ENTRY	= 8,
	MLX5_DEVX_FLOW_SAMPLER		= 9,
	MLX5_DEVX_ASO_FIRST_HIT		= 10,
	MLX5_DEVX_ASO_FLOW_METER	= 11,
	MLX5_DEVX_ASO_CT		= 12,
};

struct mlx5dv_devx_obj {
	struct ibv_context *context;
	uint32_t handle;
	enum mlx5_devx_obj_type type;
	uint32_t object_id;
	uint64_t rx_icm_addr;
	uint8_t log_obj_range;
	void *priv;
};

struct mlx5dv_dek {
    struct mlx5dv_devx_obj *devx_obj;
};

};

class SnapCryptoTest : public SnapDmaTest, public ::testing::WithParamInterface<int> {

	virtual void SetUp() {
		int ret;
		uint8_t crypto_key[128];
		size_t crypto_key_len = sizeof(crypto_key);

		SnapDmaTest::SetUp();

		ret = nvmf_crypto_key_load(NVMF_CRYPTO_KEY1, NVMF_CRYPTO_KEY2, crypto_key, &crypto_key_len);
		ASSERT_EQ(0, ret);

		/* create DEK */
		struct mlx5dv_dek_init_attr dek_attr = {};
		struct mlx5dv_dek_attr dek_query_attr = {};

		dek_attr.pd = m_pd;
		dek_attr.key_purpose = MLX5DV_CRYPTO_KEY_PURPOSE_AES_XTS;
		dek_attr.comp_mask = MLX5DV_DEK_INIT_ATTR_CRYPTO_LOGIN;
		dek_attr.crypto_login = NULL;

		memcpy(dek_attr.key, crypto_key, crypto_key_len);

		m_dek = mlx5dv_dek_create(m_pd->context, &dek_attr);
		ASSERT_TRUE(m_dek);

		ret = mlx5dv_dek_query(m_dek, &dek_query_attr);
		ASSERT_EQ(0, ret);
		ASSERT_EQ(dek_query_attr.state, MLX5DV_DEK_STATE_READY);

		/* create dummy qp */
		m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
		m_dma_q_attr.sw_use_devx = true;
		m_dma_q_attr.tx_qsize = 0;
		m_dma_q_attr.rx_qsize = 0;

		m_dummy_q = snap_dma_ep_create(m_pd, &m_dma_q_attr);
		ASSERT_TRUE(m_dummy_q);
	}
	virtual void TearDown() {
		mlx5dv_dek_destroy(m_dek);
		snap_dma_ep_destroy(m_dummy_q);
		SnapDmaTest::TearDown();
	}

	public:
	struct mlx5dv_dek *m_dek;
	struct snap_dma_q *m_dummy_q;
	struct snap_dma_q *m_dma_q;

	void crypto_qp_create();
	void crypto_qp_destroy();

	void dump_bufs(char *src, char *dst);
};

void SnapCryptoTest::dump_bufs(char *src, char *dst)
{
	printf("From: ");
	for (int i = 0; i < 32; i++) {
		printf("0x%02x ", src[i]);
	}
	printf("\n");

	printf("To: ");
	for (int i = 0; i < 32; i++) {
		if(i % 64 == 0) printf("\n");
		printf("0x%02x ", dst[i]);
	}
	printf("\n");
}

void SnapCryptoTest::crypto_qp_create()
{
	int ret;

	/* create endpoint */
	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
	m_dma_q_attr.sw_use_devx = true;
	m_dma_q_attr.tx_qsize = 1024;
	m_dma_q_attr.tx_elem_size = 0;
	m_dma_q_attr.rx_qsize = 0;
	m_dma_q_attr.crypto_enable = true;
	m_dma_q_attr.crypto_attr.crypto_place = GetParam();

	m_dma_q = snap_dma_ep_create(m_pd, &m_dma_q_attr);
	ASSERT_TRUE(m_dma_q);

	ret = snap_dma_ep_connect(m_dma_q, m_dummy_q);
	ASSERT_EQ(0, ret);
}

void SnapCryptoTest::crypto_qp_destroy()
{
	snap_dma_ep_destroy(m_dma_q);
}

TEST_P(SnapCryptoTest, crypto_writec_readc)
{
	int ret;
	struct iovec iov;

	crypto_qp_create();

	/* writec in one direction */
	memset(m_rbuf, 0, 4096);
	memset(m_lbuf, 0xAB, 4096);
	iov.iov_base = m_rbuf;
	iov.iov_len = 4096;

	ret = snap_dma_q_writec(m_dma_q, m_lbuf, m_lmr->lkey,
			&iov, 1, m_rmr->lkey, m_dek->devx_obj->object_id, NVMF_LBA, NULL);
	ASSERT_EQ(0, ret);
	snap_dma_q_flush(m_dma_q);

	dump_bufs(m_lbuf, m_rbuf);

	memset(m_lbuf, 0, 4096);
	iov.iov_base = m_rbuf;
	iov.iov_len = 4096;

	/* readc should decrypt */
	ret = snap_dma_q_readc(m_dma_q, m_lbuf, m_lmr->lkey,
			&iov, 1, m_rmr->lkey, m_dek->devx_obj->object_id, NVMF_LBA, NULL);
	ASSERT_EQ(0, ret);
	snap_dma_q_flush(m_dma_q);

	dump_bufs(m_rbuf, m_lbuf);

	memset(m_rbuf, 0xAB, 4096);
	printf("known bug, validation disabled!!!\n");
	/* There is a bug with 512b encrypt/descrtyp. Only even numbered blocks
	 * are decrypted correctly. Need to check
	 */
	//EXPECT_EQ(0, memcmp(m_lbuf, m_rbuf, 4096));

	crypto_qp_destroy();
}

TEST_P(SnapCryptoTest, crypto_writev2c_iov1)
{
	int ret;

	crypto_qp_create();

	/* writec in one direction */
	struct iovec riov, liov, iov;

	memset(m_rbuf, 0xAB, 4096);
	memset(m_lbuf, 0, 4096);

	riov.iov_base = m_rbuf;
	riov.iov_len = 4096;

	liov.iov_base = m_lbuf;
	liov.iov_len = 4096;

	iov.iov_base = m_lbuf;
	iov.iov_len = 4096;

	ret = snap_dma_q_writec(m_dma_q, m_rbuf, m_rmr->lkey,
			&iov, 1, m_lmr->lkey, m_dek->devx_obj->object_id, NVMF_LBA, NULL);
	ASSERT_EQ(0, ret);
	snap_dma_q_flush(m_dma_q);

	dump_bufs(m_rbuf, m_lbuf);

	memset(m_rbuf, 0x0, 4096);
	ret = snap_dma_q_writev2vc(m_dma_q, &m_lmr->lkey, &liov, 1,
			m_rmr->lkey, &riov, 1, m_dek->devx_obj->object_id, NVMF_LBA, NULL);
	ASSERT_EQ(0, ret);
	snap_dma_q_flush(m_dma_q);

	dump_bufs(m_lbuf, m_rbuf);

	memset(m_lbuf, 0xAB, 4096);
	EXPECT_EQ(0, memcmp(m_lbuf, m_rbuf, 4096));

	crypto_qp_destroy();
}

TEST_P(SnapCryptoTest, crypto_writev2c_iov2)
{
	int ret;

	/* writec in one direction */
	struct iovec riov, liov[2];

	crypto_qp_create();

	memset(m_rbuf, 0, 4096);
	memset(m_lbuf, 0xAB, 4096);

	riov.iov_base = m_rbuf;
	riov.iov_len = 4096;

	liov[0].iov_base = m_lbuf;
	liov[0].iov_len = 696;

	liov[1].iov_base = (char *)m_lbuf + 696;
	liov[1].iov_len = 3400;

	uint32_t lkeys[2] = { m_lmr->lkey, m_lmr->lkey };

	ret = snap_dma_q_writev2vc(m_dma_q, lkeys, liov, 2,
			m_rmr->lkey, &riov, 1, m_dek->devx_obj->object_id, NVMF_LBA, NULL);
	ASSERT_EQ(0, ret);
	snap_dma_q_flush(m_dma_q);

	dump_bufs(m_lbuf, m_rbuf);

	crypto_qp_destroy();
}

TEST_P(SnapCryptoTest, crypto_writev2c_iov2_riov2)
{
	int ret;

	struct iovec riov[2], liov[2];

	crypto_qp_create();

	memset(m_rbuf, 0, 4096);
	memset(m_lbuf, 0xAB, 4096);

	riov[0].iov_base = m_rbuf;
	riov[0].iov_len = 2048;
	riov[1].iov_base = (char *)m_rbuf + 2048;
	riov[1].iov_len = 2048;

	liov[0].iov_base = m_lbuf;
	liov[0].iov_len = 696;

	liov[1].iov_base = (char *)m_lbuf + 696;
	liov[1].iov_len = 3400;

	uint32_t lkeys[2] = { m_lmr->lkey, m_lmr->lkey };

	ret = snap_dma_q_writev2vc(m_dma_q, lkeys, liov, 2,
			m_rmr->lkey, riov, 2, m_dek->devx_obj->object_id, NVMF_LBA, NULL);
	ASSERT_EQ(0, ret);
	snap_dma_q_flush(m_dma_q);

	dump_bufs(m_lbuf, m_rbuf);

	crypto_qp_destroy();
}

INSTANTIATE_TEST_CASE_P(
		Crypto,
		SnapCryptoTest,
		::testing::Values(0, 1));

class SnapQpRecoveryTest : public SnapDmaTest {

	virtual void SetUp();
	virtual void TearDown();

	public:
	const static int N_EPS = 2;
	struct snap_dma_q *m_dummy_q[N_EPS];
	struct snap_dma_q *m_dma_q[N_EPS];

	uint32_t get_fixed_rkey() { return m_rmr->lkey; }
};

void SnapQpRecoveryTest::SetUp() {
	int ret;

	SnapDmaTest::SetUp();

	for (int i = 0; i < N_EPS; i++) {
		/* create endpoint */
		m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
		m_dma_q_attr.sw_use_devx = true;
		m_dma_q_attr.tx_qsize = 1024;
		m_dma_q_attr.tx_elem_size = 64;
		m_dma_q_attr.rx_qsize = 0;

		m_dma_q[i] = snap_dma_ep_create(m_pd, &m_dma_q_attr);
		ASSERT_TRUE(m_dma_q[i]);

		/* create dummy qp */
		m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;
		m_dma_q_attr.sw_use_devx = true;
		m_dma_q_attr.tx_qsize = 0;
		m_dma_q_attr.rx_qsize = 0;

		m_dummy_q[i] = snap_dma_ep_create(m_pd, &m_dma_q_attr);
		ASSERT_TRUE(m_dummy_q[i]);

		ret = snap_dma_ep_connect(m_dma_q[i], m_dummy_q[i]);
		ASSERT_EQ(0, ret);
	}
}

void SnapQpRecoveryTest::TearDown() {

	for (int i = 0; i < N_EPS; i++) {
		snap_dma_ep_destroy(m_dma_q[i]);
		snap_dma_ep_destroy(m_dummy_q[i]);
	}
	SnapDmaTest::TearDown();
}

static int g_err_count;
static int g_err_flush_count;
int dv_err_cb_normal(struct snap_dma_q *q, struct mlx5_cqe64 *cqe)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR)
		g_err_flush_count++;
	else {
		EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, ecqe->syndrome);
		printf("Error detected!!!\n");
		g_err_count++;
	}
	return SNAP_DMA_Q_ERR_HANDLED;
}

TEST_F(SnapQpRecoveryTest, err_cb_handled_flush) {
	/* post bad wqe */
	int rc;
	struct snap_dma_completion comp;

	comp.func = dma_completion;
	comp.count = 1;

	g_err_count = g_err_flush_count = g_comp_count = 0;
	snap_dma_q_dv_err_cb_set(m_dma_q[0], dv_err_cb_normal);

	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, 0xdeadbeef /*m_rmr->lkey*/, &comp);
	ASSERT_EQ(0, rc);
	/* post good wqe */
	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, m_rmr->lkey, NULL);
	ASSERT_EQ(0, rc);

	snap_dma_q_flush(m_dma_q[0]);

	/* poll, check that we have 1 err + 2 flush */
	EXPECT_EQ(1, g_err_count);
	EXPECT_EQ(2, g_err_flush_count);
	EXPECT_EQ(0, g_comp_count);
	printf("g_last_comp_status is %d\n", g_last_comp_status);
}

TEST_F(SnapQpRecoveryTest, err_to_rts) {
	/* post bad wqe */
	int rc;
	struct snap_dma_completion comp;

	comp.func = dma_completion;
	comp.count = 1;

	printf("qpn1 0x%x, qpn2 0x%x\n",
			snap_qp_get_qpnum(m_dma_q[0]->sw_qp.qp),
			snap_qp_get_qpnum(m_dummy_q[1]->sw_qp.qp));

	g_err_count = g_err_flush_count = g_comp_count = 0;
	snap_dma_q_dv_err_cb_set(m_dma_q[0], dv_err_cb_normal);

	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, 0xdeadbeef /*m_rmr->lkey*/, &comp);
	ASSERT_EQ(0, rc);
	snap_dma_q_flush(m_dma_q[0]);
	rc = snap_dma_ep_reconnect(m_dma_q[0], m_dummy_q[0]);
	ASSERT_EQ(0, rc);
	printf("reconnect done\n");
	/* post good wqe */
	comp.count = 1;
	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, m_rmr->lkey, &comp);
	ASSERT_EQ(0, rc);
	printf("posting read\n");
	snap_dma_q_flush(m_dma_q[0]);
	printf("flush done\n");

	/* poll, check that we have 1 err + 1 good completion */
	EXPECT_EQ(1, g_err_count);
	EXPECT_EQ(0, g_err_flush_count);
	EXPECT_EQ(1, g_comp_count);
	EXPECT_EQ(0, g_last_comp_status);
}

TEST_F(SnapQpRecoveryTest, reconnect_bench) {
	/* post bad wqe */
	const int N = 100;
	int rc;
	struct timeval t_s, t_e, t_r;
	double t;

	gettimeofday(&t_s, 0);
	for (int i = 0; i < N; i++) {
		rc = snap_dma_ep_reconnect(m_dma_q[0], m_dummy_q[0]);
		ASSERT_EQ(0, rc);
	}
	gettimeofday(&t_e, 0);
	timersub(&t_e, &t_s, &t_r);
	t = t_r.tv_sec + t_r.tv_usec/1000000.0;
	printf("Reconnect latency %1.9lf seconds, %d iters\n", t/N, N);
}

TEST_F(SnapQpRecoveryTest, err_cb_handled_comp) {
	/* post bad wqe */
	int rc;
	struct snap_dma_completion comp, comp2;

	comp.func = dma_completion;
	comp.count = 1;
	comp2 = comp;

	g_err_count = g_err_flush_count = g_comp_count = 0;
	snap_dma_q_dv_err_cb_set(m_dma_q[0], dv_err_cb_normal);

	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, 0xdeadbeef /*m_rmr->lkey*/, &comp);
	ASSERT_EQ(0, rc);
	/* post good wqe */
	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, m_rmr->lkey, &comp2);
	ASSERT_EQ(0, rc);

	snap_dma_q_flush(m_dma_q[0]);

	/* poll, check that we have 1 err + 1 flush because after comp2 there will
	 * be no outstanding - no need to flush */
	EXPECT_EQ(1, g_err_count);
	EXPECT_EQ(1, g_err_flush_count);
	/* comp2 should not be completed */
	EXPECT_EQ(0, g_comp_count);
	printf("g_last_comp_status is %d\n", g_last_comp_status);
}

static int dv_err_cb_migrate(struct snap_dma_q *q, struct mlx5_cqe64 *cqe)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR)
		g_err_flush_count++;
	else {
		EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, ecqe->syndrome);
		printf("Access error detected, migration started\n");
		g_err_count++;
		SnapQpRecoveryTest *t = (SnapQpRecoveryTest *)q->uctx;
		const snap_dma_q_migrate_attr attr = {
			.start_pi = be16toh(cqe->wqe_counter),
			.rkey_policy = SNAP_DMA_Q_MIGR_RKEY_DISCARD,
		};
		int ret = snap_dma_q_migrate(q, t->m_dma_q[1], &attr);
		EXPECT_EQ(0, ret);
	}
	return SNAP_DMA_Q_ERR_HANDLED;
}

static void good_dma_completion(struct snap_dma_completion *comp, int status)
{
	printf("good DMA completion\n");
	g_comp_count++;
	g_last_comp_status = status;
	EXPECT_EQ(0, status);
}

static void bad_dma_completion(struct snap_dma_completion *comp, int status)
{
	printf("bad DMA (REMOTE_ACCESS_ERR) completion \n");
	g_comp_count++;
	g_last_comp_status = status;
	EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, status);
}

TEST_F(SnapQpRecoveryTest, err_cb_handled_migrate_rdma_read) {
	/* post bad wqe */
	int rc;
	struct snap_dma_completion comp, comp2;

	comp.func = bad_dma_completion;
	comp.count = 1;
	comp2.func = good_dma_completion;
	comp2.count = 1;

	g_err_count = g_err_flush_count = g_comp_count = 0;
	snap_dma_q_dv_err_cb_set(m_dma_q[0], dv_err_cb_migrate);
	m_dma_q[0]->uctx = this;

	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, 0xdeadbeef /*m_rmr->lkey*/, &comp);
	ASSERT_EQ(0, rc);
	/* post good wqe */
	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, m_rmr->lkey, &comp2);
	ASSERT_EQ(0, rc);

	snap_dma_q_flush(m_dma_q[0]);

	/* poll, check that we have 1 err + 0 flush, second comp is migrated */
	EXPECT_EQ(1, g_err_count);
	EXPECT_EQ(0, g_err_flush_count);
	/* request with the bad rkey should be completed with error */
	EXPECT_EQ(1, g_comp_count);
	EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, g_last_comp_status);
	printf("g_last_comp_status is %d\n", g_last_comp_status);
	snap_dma_q_flush(m_dma_q[1]);
	/* request with the good rkey is migrated to the new qp and completed normally */
	EXPECT_EQ(2, g_comp_count);
	EXPECT_EQ(0, g_last_comp_status);
}

TEST_F(SnapQpRecoveryTest, err_cb_handled_migrate_write_short) {
	/* post bad wqe */
	int rc;
	struct snap_dma_completion comp;

	comp.func = bad_dma_completion;
	comp.count = 1;

	g_err_count = g_err_flush_count = g_comp_count = 0;
	snap_dma_q_dv_err_cb_set(m_dma_q[0], dv_err_cb_migrate);
	m_dma_q[0]->uctx = this;

	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, 0xdeadbeef /*m_rmr->lkey*/, &comp);
	ASSERT_EQ(0, rc);
	/* post good inline wqe that will spawn 2 wqes */
	rc = snap_dma_q_write_short(m_dma_q[0], m_lbuf, 64, (uintptr_t)m_rbuf, m_rmr->lkey);
	ASSERT_EQ(0, rc);

	snap_dma_q_flush(m_dma_q[0]);

	/* poll, check that we have 1 err + 0 flush, second comp is migrated */
	EXPECT_EQ(1, g_err_count);
	EXPECT_EQ(0, g_err_flush_count);
	/* request with the bad rkey should be completed with error */
	EXPECT_EQ(1, g_comp_count);
	EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, g_last_comp_status);
	printf("g_last_comp_status is %d\n", g_last_comp_status);

	snap_dma_q_flush(m_dma_q[1]);
	/* request with the good rkey is migrated to the new qp and completed normally */
	EXPECT_EQ(1, g_comp_count);
	EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, g_last_comp_status);
}

static int dv_err_cb_migrate_recover(struct snap_dma_q *q, struct mlx5_cqe64 *cqe)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR)
		g_err_flush_count++;
	else {
		EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, ecqe->syndrome);
		printf("Access error detected, migration started, fix rkey\n");
		g_err_count++;
		SnapQpRecoveryTest *t = (SnapQpRecoveryTest *)q->uctx;
		const snap_dma_q_migrate_attr attr = {
			.start_pi = be16toh(cqe->wqe_counter),
			.rkey_policy = SNAP_DMA_Q_MIGR_RKEY_FIX,
			.fixed_rkey =  t->get_fixed_rkey(),
		};
		int ret = snap_dma_q_migrate(q, t->m_dma_q[1], &attr);
		EXPECT_EQ(0, ret);
	}
	return SNAP_DMA_Q_ERR_HANDLED;
}

TEST_F(SnapQpRecoveryTest, err_cb_handled_comp_migrate_fix_rkey) {
	/* post bad wqe */
	int rc;
	struct snap_dma_completion comp, comp2;

	comp.func = good_dma_completion;
	comp.count = 1;
	comp2.func = good_dma_completion;
	comp2.count = 1;

	g_err_count = g_err_flush_count = g_comp_count = 0;
	snap_dma_q_dv_err_cb_set(m_dma_q[0], dv_err_cb_migrate_recover);
	m_dma_q[0]->uctx = this;

	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, 0xdeadbeef /*m_rmr->lkey*/, &comp);
	ASSERT_EQ(0, rc);
	/* post good wqe */
	rc = snap_dma_q_read(m_dma_q[0], m_lbuf, m_bsize, m_lmr->lkey,
			(uintptr_t)m_rbuf, m_rmr->lkey, &comp2);
	ASSERT_EQ(0, rc);

	snap_dma_q_flush(m_dma_q[0]);

	/* poll, check that we have 1 err + 1 flush because after comp2 there will
	 * be no outstanding - no need to flush */
	EXPECT_EQ(1, g_err_count);
	EXPECT_EQ(0, g_err_flush_count);
	/* request with the bad rkey should NOT be completed with error */
	EXPECT_EQ(0, g_comp_count);
	EXPECT_EQ(MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR, g_last_comp_status);
	printf("g_last_comp_status is %d\n", g_last_comp_status);
	snap_dma_q_flush(m_dma_q[1]);
	/* request with the good rkey is migrated to the new qp and completed normally */
	EXPECT_EQ(2, g_comp_count);
	EXPECT_EQ(0, g_last_comp_status);
}

