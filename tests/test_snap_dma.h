#ifndef TEST_SNAP_DMA_H
#define TEST_SNAP_DMA_H

class SnapDmaTest : public ::testing::Test {

	private:
	void alloc_bufs();
	void free_bufs();

	protected:
	struct ibv_pd *m_pd;
	struct ibv_comp_channel *m_comp_channel;
	struct ibv_mr *m_lmr;
	struct ibv_mr *m_rmr;
	char *m_lbuf;
	char *m_rbuf;
	int   m_bsize;
	int   m_bcount;
	struct snap_dma_q_create_attr m_dma_q_attr;

	void dma_xfer_test(struct snap_dma_q *q, bool is_read, bool poll_mode,
			void *rvaddr, void *rpaddr, uint32_t rkey, int len);
	void poll_rx(int mode);
	void poll_tx(int mode);
	void worker_poll_rx();
	void worker_poll_tx();
	void empty(int mode);
	void flush_async(int mode);
	struct snap_dma_q *create_queue();

	virtual void SetUp();
	virtual void TearDown();
	public:
	/* Send data to sw qp */
	static int snap_dma_q_fw_send(struct snap_dma_q *q, void *src_buf,
			size_t len, uint32_t lkey);
	/* For testing only. Send with imm */
	static int snap_dma_q_fw_send_imm(struct snap_dma_q *q, uint32_t imm);
};

#endif
