#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <infiniband/verbs.h>

extern "C" {
#include "snap_env.h"
#include "snap_dma.h"
#include "snap_umr.h"
};

#include "gtest/gtest.h"
#include "tests_common.h"
#include "test_snap_dma.h"

#include <sstream>
#include <thread>
#include <mutex>

typedef ::std::vector<std::string> str_vector_t;

#define AUTO_VARIABLE_TYPE auto
#define IS_CONST_PTR(ptr) (std::is_const<decltype(ptr)>::value)

#define container_of_ex(ptr, type, member) \
        ({ \
                AUTO_VARIABLE_TYPE __mptr = (ptr); \
                IS_CONST_PTR(ptr) ? \
                        ((type *)(((char const *)__mptr) - offsetof(type, member))) : \
                        ((type *)(((char *)__mptr) - offsetof(type, member))); \
        })

class SnapDmaUmrPerfTest : public ::testing::Test {
private:	
	virtual void SetUp();
	virtual void TearDown();

protected:
	void setup_dma_q_attributes();
	static void dma_q_rx_cb(struct snap_dma_q *q, const void *data,
			uint32_t data_len, uint32_t imm_data);

protected:
	struct ibv_pd *m_pd;
	struct snap_dma_q_create_attr m_dma_q_attr;
	struct ibv_comp_channel *m_comp_channel;
};

void SnapDmaUmrPerfTest::SetUp()
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
			m_comp_channel = ibv_create_comp_channel(ib_ctx);
			if (!m_comp_channel)
				FAIL() << "Failed to created completion channel";
			init_ok = true;
			break;
		}
	}

	ibv_free_device_list(dev_list);
	if (!init_ok)
		FAIL() << "Failed to setup " << get_dev_name();

	setup_dma_q_attributes();
}

void SnapDmaUmrPerfTest::TearDown()
{
	if (m_pd) {
		struct ibv_context *ib_ctx = m_pd->context;

		if (m_comp_channel)
			ibv_destroy_comp_channel(m_comp_channel);
		
		ibv_dealloc_pd(m_pd);
		ibv_close_device(ib_ctx);
	}
}

void SnapDmaUmrPerfTest::dma_q_rx_cb(struct snap_dma_q *q, const void *data,
		uint32_t data_len, uint32_t imm_data)
{
	printf("\t dma_q_rx_cb was called: q: %p data_len: %d, imm_data: 0x%x\n",
			q, data_len, imm_data);
}

void SnapDmaUmrPerfTest::setup_dma_q_attributes()
{
	memset(&m_dma_q_attr, 0, sizeof(m_dma_q_attr));
	m_dma_q_attr.tx_qsize = m_dma_q_attr.rx_qsize = 64;
	m_dma_q_attr.tx_elem_size = 16;
	m_dma_q_attr.rx_elem_size = 64;
	m_dma_q_attr.rx_cb = dma_q_rx_cb;
	m_dma_q_attr.mode = snap_env_getenv(SNAP_DMA_Q_OPMODE);
	m_dma_q_attr.fw_use_devx = false;
	m_dma_q_attr.comp_channel = m_comp_channel;
}

bool find_arg(const str_vector_t &argvs, const char *name)
{
	str_vector_t::const_iterator it = std::find(argvs.begin(), argvs.end(), name);
	return it != argvs.end();
}

template<typename T>
void setup_param(const str_vector_t &argvs, const char *name, T* param)
{
	str_vector_t::const_iterator it = std::find(argvs.begin(), argvs.end(), name);
	if (it != argvs.end()) {
		if (++it != argvs.end()) {
			if ((*it)[1] =='x' || (*it)[1] =='X')
				std::istringstream(*it) >> std::hex >> *param;
			else
				std::istringstream(*it) >> std::dec >> *param;
		}
	}
}

enum post_umr_test_type {
	POST_UMR_TEST_TYPE_UMR_ONLY,
	POST_UMR_TEST_TYPE_UMR_AND_READ,
	POST_UMR_TEST_TYPE_UMR_AND_WRITE,
};

const char* post_umr_test_type_to_str(int type)
{
	if (type == POST_UMR_TEST_TYPE_UMR_ONLY)
		return "UMR_ONLY";
	else if (type == POST_UMR_TEST_TYPE_UMR_AND_READ)
		return "UMR_AND_READ";
	else if (type == POST_UMR_TEST_TYPE_UMR_AND_WRITE)
		return "UMR_AND_WRITE";
	
	return "UKNOWN_TEST";
}

#define MIN_BUFF_SIZE (4096)
#define MAX_BUFF_SIZE (64*1024)

#define MAX_QS_PER_CORE (1)
#define MAX_MTT (5)
#define MAX_BATCH (64)

struct post_umr_params {	
	/* the buffer will be populated accross mtt entries*/
	uint32_t buf_size; 
	enum post_umr_test_type type;
	/* a single thread will be bound per core */
	uint32_t cpu_mask;	
	uint16_t qs_per_cores;
	uint16_t mtt;

	/* how match requests should be sent in one 'shot'.
	 * the next 'shot' will be sent after receiving the 'batch' completions
	 */
	uint16_t batch;

	post_umr_params() 
	{
		buf_size = MIN_BUFF_SIZE;
		type = POST_UMR_TEST_TYPE_UMR_ONLY;
		cpu_mask = 0x1;
		qs_per_cores = 1;
		mtt = 1;
		batch = 1;
	}

	bool validate()
	{
		if (!(buf_size >= MIN_BUFF_SIZE && buf_size <= MAX_BUFF_SIZE)) {
			std::cerr << "Unsupported buf_size: " << buf_size;
			return false;
		}

		if (buf_size % 1024 != 0) {
			std::cerr << "Unsupported buf_size: " << buf_size;
			return false;
		}

		if (!(type >= POST_UMR_TEST_TYPE_UMR_ONLY && type <= POST_UMR_TEST_TYPE_UMR_AND_WRITE)) {
			std::cerr << "Unsupported type: " << type;
			return false;
		}

		if (!(cpu_mask >= 0x1 && cpu_mask <= 0xFFFF)) {
			std::cerr << "Unsupported cpu_mask: " << cpu_mask;
			return false;
		}

		if (!(qs_per_cores >= 1 && qs_per_cores <= MAX_QS_PER_CORE)) {
			std::cerr << "Unsupported qs_per_cores: " << qs_per_cores;
			return false;
		}

		if (!(mtt >= 1 && mtt <= MAX_MTT)) {
			std::cerr << "Unsupported mtt: " << mtt;
			return false;
		}

		if (buf_size * mtt > MAX_BUFF_SIZE) {
			std::cerr << "Unsupported buf_size: " << buf_size << ", mtt: " << mtt;
			return false;
		}

		if (!(batch >= 1 && batch <= MAX_BATCH)) {
			std::cerr << "Unsupported batch: " << batch;
			return false;
		}
		return true;
	}

	bool parse_args(const str_vector_t &argvs)
	{
		if (find_arg(argvs, "--h") || find_arg(argvs, "help")) {
			usage();
			exit(0);
		}

		setup_param<decltype(buf_size)>(argvs, "--size", &buf_size);
		setup_param<int>(argvs, "--type", (int*)&type);
		setup_param<decltype(cpu_mask)>(argvs, "--mask", &cpu_mask);
		setup_param<decltype(mtt)>(argvs, "--mtt", &mtt);
		setup_param<decltype(batch)>(argvs, "--batch", &batch);

		return validate();
	}

	void dump()
	{
		printf("buffer size:   %d ( x mtt: %d = %d)\n", buf_size, mtt, mtt * buf_size);
		printf("operation:     %s\n", post_umr_test_type_to_str(type));
		printf("core bit mask: 0x%x (CPUs: %d)\n", cpu_mask, __builtin_popcount(cpu_mask));
		printf("qs per cores:  %d\n", qs_per_cores);
		printf("mtt:           %d\n", mtt);
		printf("batch:         %d\n", batch);
	}

	void usage()
	{
		std::cout << "\n";
		std::cout << "Usage:\n";
		std::cout << "--size <" << MIN_BUFF_SIZE << ".." << MAX_BUFF_SIZE << ">\n";
		std::cout << "--type <0: UMR | 1: UMR + READ | 2: UMR + WRITE>\n";
		std::cout << "--mask <cpu_mask>\n";
		std::cout << "--mtt <1.." << MAX_MTT << ">\n";
		std::cout << "--batch <1.." << MAX_BATCH << ">\n";
		std::cout << "\n\n";
	}
};

struct umr_dma_buffer {

	/* size for the remote buffer will be populated into 1 flat entry */
	uint8_t *rbuf;
	struct ibv_mr *rmr;
	uint32_t rsize;

	/* size for the local buffer will be populated into 'mtt' entries */
	uint8_t *lbuf_arr[MAX_MTT];
	struct ibv_mr *lmr_arr[MAX_MTT];

	/* size of one mtt entry*/
	uint32_t lsize;

	uint16_t mtt;

	umr_dma_buffer()
	{
		clear();
	}

	~umr_dma_buffer()
	{
		if (rmr)
			ibv_dereg_mr(rmr);

		for (int i = 0; i < MAX_MTT; ++i) {
			if (lmr_arr[i])
				ibv_dereg_mr(lmr_arr[i]);
		}

		if(rbuf)
			free(rbuf);

		if (lbuf_arr[0])
			free(lbuf_arr[0]);

	}
	
	umr_dma_buffer& operator=(const umr_dma_buffer&) = delete;
	umr_dma_buffer(const umr_dma_buffer &) = delete;
	umr_dma_buffer(umr_dma_buffer &&req)
	{
		if (req.rbuf != NULL)
			std::cerr << "umr_dma_buffer move ctor unhandled case \n";

		clear();
	} 

	void clear()
	{
		memset(lbuf_arr, 0, sizeof(lbuf_arr));
		memset(lmr_arr, 0, sizeof(lmr_arr));

		rbuf = NULL;
		rsize = 0;
		lsize = 0;
		rmr = NULL;
	}

	bool init(struct ibv_pd *pd, uint16_t mtt, uint32_t buf_size)
	{
		uint32_t access = IBV_ACCESS_LOCAL_WRITE |IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
		uint8_t *lbuf;
		
		rsize = buf_size * mtt;
		lsize = buf_size;
		this->mtt = mtt;

		lbuf = (uint8_t*)calloc(1, rsize);
		rbuf = (uint8_t*)calloc(1, rsize);

		if (!(lbuf && rbuf)) {
			std::cerr << "Failed to allocate memory\n";
			return false;
		}

		rmr = ibv_reg_mr(pd, rbuf, rsize, access);
		if (!rmr) {
			std::cerr << "Failed to regiter remote memory\n";
			return false;
		}

		lmr_arr[0] = ibv_reg_mr(pd, lbuf, rsize, access);
		if (!lmr_arr[0]) {
			std::cerr << "Failed to regiter local memory\n";
			return false;
		}

		for (int i = 0; i < mtt; ++i)
			lbuf_arr[i] = lbuf + lsize * i;

		return true;
	}

	void setup_klm(struct mlx5_klm *klm_mtt)
	{
		for (int i = 0; i < mtt; ++i)
		{
			klm_mtt[i].byte_count = lsize;
			klm_mtt[i].mkey = lmr_arr[0]->lkey;
			klm_mtt[i].address = (uintptr_t)lbuf_arr[i];
		}
	}
};

enum dma_cmd_operation {
	DMA_OP_NONE,
	DMA_OP_POST_UMR,
	DMA_OP_READ,
	DMA_OP_WRITE,
};

struct umr_dma_cmd;
typedef void (*umr_dma_cmd_done_cb)(void *arg, umr_dma_cmd *cmd, int status);

struct umr_dma_cmd {
	struct snap_dma_q *q;
	struct snap_indirect_mkey *klm_mkey;
	struct mlx5_klm klm_mtt[MAX_MTT];

	umr_dma_buffer buffer;
	struct snap_dma_completion comp;
	bool used;
	dma_cmd_operation cur_op;

	umr_dma_cmd_done_cb cb;
	void *arg;

	STAILQ_ENTRY(umr_dma_cmd) entry;

	umr_dma_cmd()
	{
		q = NULL;
		klm_mkey = NULL;
		memset(klm_mtt, 0, sizeof(klm_mtt));
		used = false;
		cur_op = DMA_OP_NONE;
	}

	~umr_dma_cmd()
	{
		if (klm_mkey)
			snap_destroy_indirect_mkey(klm_mkey);
	}


	void set_q(struct snap_dma_q *q)
	{
		if (!this->q)
			this->q = q;
	}

	bool init_klm_key(struct ibv_pd *pd, struct mlx5_devx_mkey_attr *mkey_attr)
	{
		if (!klm_mkey) {
			klm_mkey = snap_create_indirect_mkey(pd, mkey_attr);
			if (!klm_mkey) {
				std::cerr << "Failed to create indirect mkey\n";
				return false;
			}
		}

		return true;
	}

	void init_cb(umr_dma_cmd_done_cb cb, void *arg)
	{
		this->cb = cb;
		this->arg = arg;
	}

	bool init_buffer(struct ibv_pd *pd, uint16_t mtt, uint32_t buf_size)
	{
		if (buffer.rmr)
			return false;

		return buffer.init(pd, mtt, buf_size);
	}
	
	bool post_umr_wqe()
	{
		struct snap_post_umr_attr umr_attr = { 0 };
		int ret;
		int n_bb = 0;

		if (!used) {

			comp.func = cmd_completed;
			comp.count = 1;

			umr_attr.klm_mkey = klm_mkey;
			umr_attr.purpose |= SNAP_UMR_MKEY_MODIFY_ATTACH_MTT;
			umr_attr.klm_mtt = klm_mtt;
			umr_attr.klm_entries = buffer.mtt;
			//printf("\t post_umr_wqe: buffer.mtt: %d\n", buffer.mtt);

			buffer.setup_klm(klm_mtt);

			ret = snap_umr_post_wqe(q, &umr_attr, &comp, &n_bb);

			if (ret) {
				std::cerr << "Failed to post umr wqe, err: " << ret << "\n";
				return false;
			}

			used = true;
			cur_op = DMA_OP_POST_UMR;
			return true;
		}

		std::cerr << "Failed to send cmd " << this << " - the command is busy\n";
		return false;
	}

	bool send_rdma_read()
	{
		int ret;

		if (!used) {

			comp.func = cmd_completed;
			comp.count = 1;
		
			//printf("\t send_rdma_read: cmd: %p \n", this);

			ret = snap_dma_q_read(q, (void *)klm_mkey->addr,
				buffer.mtt * buffer.lsize, klm_mkey->mkey, (uintptr_t)buffer.rbuf, buffer.rmr->lkey, &comp);

			if (ret) {
				std::cerr << "Failed to send 'RDMA READ', err: " << ret << "\n";
				return false;
			}

			used = true;
			cur_op = DMA_OP_READ;
			return true;
		}

		std::cerr << "Failed to send cmd " << this << " - the command is busy\n";
		return false;
	}

	bool send_rdma_write()
	{
		int ret;

		if (!used) {

			comp.func = cmd_completed;
			comp.count = 1;
		
			//printf("\t send_rdma_write: cmd: %p \n", this);

			ret = snap_dma_q_write(q, (void *)klm_mkey->addr,
						buffer.mtt * buffer.lsize, klm_mkey->mkey, (uintptr_t)buffer.rbuf, buffer.rmr->lkey, &comp);

			if (ret) {
				std::cerr << "Failed to send 'RDMA WRITE', err: " << ret << "\n";
				return false;
			}

			used = true;
			cur_op = DMA_OP_WRITE;
			return true;
		}

		std::cerr << "Failed to send cmd " << this << " - the command is busy\n";
		return false;
	}

	bool progress(enum post_umr_test_type test_type)
	{
		bool ret = false;

		switch (cur_op) {
		case DMA_OP_NONE:
			ret = post_umr_wqe();
			break;
		case DMA_OP_POST_UMR:
			if (test_type == POST_UMR_TEST_TYPE_UMR_ONLY)
				ret = post_umr_wqe();
			else if (test_type == POST_UMR_TEST_TYPE_UMR_AND_READ)
				ret = send_rdma_read();
			else if (test_type == POST_UMR_TEST_TYPE_UMR_AND_WRITE)
				ret = send_rdma_write();
			break;
		case DMA_OP_READ:
		case DMA_OP_WRITE:
			ret = post_umr_wqe();
			break;
		default:
			std::cerr << "Wrong cmd operation: "  << cur_op <<"\n";
			break;
		}

		return ret;
	}

	static void cmd_completed(struct snap_dma_completion *self, int status)
	{
		struct umr_dma_cmd *cmd = container_of_ex(self, struct umr_dma_cmd, comp);

		cmd->cb(cmd->arg, cmd, status);
	}
};

struct umr_thread_data {
	/* Perhaps change to array of queues in the future */
	struct snap_dma_q *q;

	/* batch */
	STAILQ_HEAD(, umr_dma_cmd) free_cmds;

	uint64_t umr_completed;
	uint64_t read_completed;
	uint64_t write_completed;

	/* how match requests shoul be sent in each 'shot'.
	 * the next 'shot' will be sent after receiving the batch completions
	 */
	uint16_t batch;

	enum post_umr_test_type test_type;

	bool is_progress;
	uint16_t outstanding;

	umr_thread_data()
	{
		clear();
	}

	~umr_thread_data()
	{
		umr_dma_cmd * cmd = get_cmd();

		while(cmd) {
			delete cmd;
			cmd = get_cmd();
		}

		if (q)
			snap_dma_q_destroy(q);
	
	}

	umr_thread_data(const umr_thread_data &thr_data) = delete;
	umr_thread_data& operator=(const umr_thread_data &thr_data) = delete;

	umr_thread_data(const umr_thread_data &&thr_data)
	{
		if (thr_data.q != NULL)
			std::cerr << "umr_thread_data move ctor unhandled case \n";

		clear();
	}

	void clear()
	{
		q = NULL;
		batch = 0;

		umr_completed = read_completed = write_completed = 0;
		is_progress = false;
		outstanding = 0;

		STAILQ_INIT(&free_cmds);
	}

	bool init_q(struct ibv_pd *pd, struct snap_dma_q_create_attr *dma_q_attr)
	{
		if (!q) {
			q = snap_dma_q_create(pd, dma_q_attr);
			if (!q) {
				std::cerr << "Failed to create dma_q\n";
				return false;
			}
		}

		return true;
	}

	bool init_cmds(struct ibv_pd *pd, struct mlx5_devx_mkey_attr *key_attr, uint16_t batch, uint16_t mtt, uint32_t buf_size)
	{
		if (STAILQ_EMPTY(&free_cmds)) {
			
			umr_dma_cmd *dma_cmd;

			for (int i = 0; i < batch; ++i) {
				dma_cmd = new umr_dma_cmd;
				if(!dma_cmd) {
					std::cerr << "Failed to allocate the dma_cmd #" << i << "\n";
					return false;
				}

				if (!dma_cmd->init_buffer(pd, mtt, buf_size)) {
					std::cerr << "Failed to create the dma buffer #" << i << "\n";
					return false;
				}

				if (!dma_cmd->init_klm_key(pd, key_attr)) {
					std::cerr << "Failed to create the mkey #" << i << "\n";
					return false;
				}

				dma_cmd->set_q(q);
				dma_cmd->init_cb(rdma_op_completed, this);

				STAILQ_INSERT_HEAD(&free_cmds, dma_cmd, entry);
			}
		}

		return true;
	}

	void free_cmd(umr_dma_cmd *cmd)
	{
		STAILQ_INSERT_TAIL(&free_cmds, cmd, entry);
		--outstanding;
	}

	umr_dma_cmd * get_cmd()
	{
		umr_dma_cmd *cmd = NULL;

		cmd = STAILQ_FIRST(&free_cmds);
		if (cmd) {
			STAILQ_REMOVE_HEAD(&free_cmds, entry);
			++outstanding;
		}

		return cmd;
	}

	bool start_work()
	{
		umr_dma_cmd *cmd;
		int i;

		is_progress = true;

    	/* 1. prepare umr wqe 'batch'
		 * 2. post umr wqe
		 * 		when completed
		 * 		do read or write dma operation
		 */

		for (i = 0; i < batch; ++i) {
			cmd = get_cmd();
			if (cmd) {
				if (!progress_cmd(cmd))
					break;
			}
		}

		return i == batch;
	}

	bool progress_cmd(umr_dma_cmd *cmd)
	{
		if (is_progress)
			return cmd->progress(test_type);
		else
			free_cmd(cmd);
		
		return true;
	}

	static void rdma_op_completed(void *arg, umr_dma_cmd *cmd, int status)
	{
		struct umr_thread_data *td = (struct umr_thread_data*) arg;

		cmd->used = false;

		//printf("\t rdma_op_completed: cmd: %p cur_op: %d\n", cmd, cmd->cur_op);
		//std::this_thread::sleep_for (std::chrono::milliseconds(500));

		if (cmd->cur_op == DMA_OP_POST_UMR)			
			++td->umr_completed;
		else if (cmd->cur_op == DMA_OP_READ)
			++td->read_completed;
		else if (cmd->cur_op == DMA_OP_WRITE)
			++td->write_completed;

		td->progress_cmd(cmd);
	}
};

int set_thread_affinity(pthread_t thread, uint8_t core_id)
{
	cpu_set_t cpuset;
	int rc;

	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);
	rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);		
	if (rc != 0)
		std::cerr << "Failed to boud thread to core_id #" << core_id << ", error: " << rc <<"\n";

	return rc;
}

struct umr_thread {
private:	
	std::thread *thr;
	umr_thread_data thr_data;

	/* will be boud to this core id */
	uint16_t core_id;

public:
	umr_thread()
	{
		thr = NULL;
		core_id = 0;
	}

	~umr_thread()
	{
		/* TODO */

		delete thr;
	}

	umr_thread(const umr_thread&) = delete;
	umr_thread& operator=(const umr_thread&) = delete;

	umr_thread(const umr_thread &&thr)
	{
		if (thr.thr_data.q != NULL)
			std::cerr << "umr_thread move ctor unhandled case \n";

		this->thr = NULL;
		this->core_id = 0;
		this->thr_data.clear();
	}

	bool init(struct ibv_pd *pd, struct snap_dma_q_create_attr *q_attr,
			struct mlx5_devx_mkey_attr *key_attr, const post_umr_params &params)
	{
		if (!thr_data.init_q(pd, q_attr))
			return false;

		if (!thr_data.init_cmds(pd, key_attr, params.batch, params.mtt, params.buf_size))
			return false;
		
		thr_data.test_type = params.type;
		thr_data.batch = params.batch;

		return true;	
	}

	bool start(uint16_t core_id)
	{
		if (thr_data.q == NULL) {
			std::cerr << "Failed to start thread - call init(...) first\n";
			return false;
		}

		this->core_id = core_id;
		thr = new std::thread(umr_thread::worker, this);
		if (!thr) {
			std::cerr << "Failed to start thread - no memory\n";
			return false;
		}

		return true;
	}

	void wait()
	{
		if (!thr)
			return;

		thr->join();
	}

	uint16_t thr_core_id() const
	{ 
		return core_id; 
	}

	static uint64_t iops_per_sec(uint32_t millis, uint64_t val)
	{
		uint64_t res = (val * 1000) / millis;

		return res;
	}

	void complete()
	{
		thr_data.is_progress = false;
		while(thr_data.outstanding > 0)
			snap_dma_q_progress(thr_data.q);
	}

	static void worker(umr_thread *args) 
	{
		umr_thread_data &thr_data = args->thr_data;
		std::thread *thr = args->thr; 
		struct snap_dma_q *q = thr_data.q;
		static std::mutex iomutex;		

		if (set_thread_affinity(thr->native_handle(), args->thr_core_id()))
			return;

		const uint16_t test_duration_sec = 5;
		const auto start_time = std::chrono::high_resolution_clock::now();

		if (!thr_data.start_work()) {
			std::cout << "thread #" << std::hex << thr->get_id() << " failed to start\n\n";
			return;
		}

		while (true) {
			snap_dma_q_progress(q);

			if (thr_data.umr_completed && ((thr_data.umr_completed % 100000) == 0)) {
				const auto end_time = std::chrono::high_resolution_clock::now();
				auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

				if (elapsed_sec.count() >= test_duration_sec) {

					{
						/* dump statistics */

						std::lock_guard<std::mutex> iolock(iomutex);
						auto elapsed_mill = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

						std::cout << "thread #" << std::hex << thr->get_id() << " --> CPU # " << std::dec << sched_getcpu() << "\n";

						//setlocale(LC_NUMERIC, "en_US.utf8");
						//printf("elapsed:    %'ld\n", elapsed_mill.count());

						std::cout.imbue(std::locale("en_US.utf8"));
						std::cout << std::fixed << std::showpoint << std::setprecision(3);

						std::cout << "elapsed:    " << elapsed_mill.count() << " milliseconds \n";
						std::cout << "UMRs:       " << thr_data.umr_completed   << " / per sec: " << iops_per_sec(elapsed_mill.count(), thr_data.umr_completed)   <<"\n";
						std::cout << "RDMA READ:  " << thr_data.read_completed  << " / per sec: " << iops_per_sec(elapsed_mill.count(), thr_data.read_completed)  <<"\n";
						std::cout << "WRITE READ: " << thr_data.write_completed << " / per sec: " << iops_per_sec(elapsed_mill.count(), thr_data.write_completed) <<"\n";
						std::cout << "\n\n";
					}

					args->complete();					
					break;
				}
			}
		}
	}
};

void post_umr_mkey_attr_init(struct mlx5_devx_mkey_attr &mkey_attr, bool attach_bsf)
{
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
}

bool post_umr_mkey_is_core_enable(uint32_t cpu_mask, uint8_t core_index)
{
	/* Note: core_index is zero based */

	return (cpu_mask & ((uint32_t)1 << core_index)) ? true : false;
}

TEST_F(SnapDmaUmrPerfTest, post_umr_mkey_modify_perf) {

	struct mlx5_devx_mkey_attr mkey_attr = { 0 };
	struct post_umr_params params;
	const str_vector_t &argvs = testing::internal::GetArgvs();

	bool ret = params.parse_args(argvs);
	uint8_t cores = 0, i;
	std::vector<umr_thread> thread_vec;
	/* BF3 - up to 16 cores */
	uint8_t max_core_num = 0xf; 

	if (!ret)
		FAIL() << "Failed to parse arguments";

	m_dma_q_attr.mode = SNAP_DMA_Q_MODE_DV;

	post_umr_mkey_attr_init(mkey_attr, false);

	std::cout << "\n";
	params.dump();
	std::cout << "\n";

	cores = __builtin_popcount(params.cpu_mask);

	thread_vec.reserve(cores);
	for (i = 0; i <= max_core_num; ++i) {
		if (post_umr_mkey_is_core_enable(params.cpu_mask, i)) {
			thread_vec.emplace_back(umr_thread());
			if (!thread_vec.back().init(m_pd, &m_dma_q_attr, &mkey_attr, params))
				break;

			thread_vec.back().start(i);
		}
	}

	for (int i = 0; i < cores; ++i)
		thread_vec[i].wait();

}
