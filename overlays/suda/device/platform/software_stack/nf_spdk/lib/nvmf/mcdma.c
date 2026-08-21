#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/tree.h"
#include "spdk/util.h"
#include "spdk/barrier.h"

#include "spdk_internal/assert.h"
#include "spdk/log.h"

#include "nvmf_internal.h"
#include "transport.h"
#include "blowfish.h"

#include "spdk_internal/trace_defs.h"

#include "spdk/axi_dma.h"
#include "spdk/qdma.h"

#include "spdk/hlsacccompute.h"
#include "spdk/endian.h"
#include "spdk/hash_table.h"

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_mcdma;
SPDK_LOG_REGISTER_COMPONENT(nvmq);
#define SPDK_NVMF_MCDMA_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_MCDMA_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_MCDMA_DEFAULT_SRQ_DEPTH 4096
#define SPDK_NVMF_MCDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR 8
#define SPDK_NVMF_MCDMA_MAX_CTRLRS_PER_DISK 4
#define SPDK_NVMF_MCDMA_MAX_QPAIRS 4
#define SPDK_NVMF_MCDMA_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_MCDMA_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_MCDMA_MIN_IO_BUFFER_SIZE (SPDK_NVMF_MCDMA_DEFAULT_MAX_IO_SIZE / SPDK_NVMF_MAX_SGL_ENTRIES)
#define SPDK_NVMF_MCDMA_DEFAULT_NUM_SHARED_BUFFERS 4095
#define SPDK_NVMF_MCDMA_DEFAULT_BUFFER_CACHE_SIZE 32
#define SPDK_NVMF_MCDMA_DEFAULT_NO_SRQ false
#define SPDK_NVMF_MCDMA_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_MCDMA_ACCEPTOR_BACKLOG 100
#define SPDK_NVMF_MCDMA_DEFAULT_ABORT_TIMEOUT_SEC 1
#define SPDK_NVMF_MCDMA_DEFAULT_NO_WR_BATCHING false

#define MCDMA_NUM_IO_DEVS ((SPDK_NVMF_MCDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR + 15) / 16)
#define MCDMA_NVMF_RSP_SIZE 64
#define MCDMA_NVMF_PAD_SIZE (MCDMA_NVMF_RSP_SIZE - sizeof(union nvmf_c2h_msg))

#define MCDMA_RX_BUF_SZ 64
#define MCDMA_TASK_FINISH_BEAT_SIZE MCDMA_RX_BUF_SZ
#define MCDMA_TASK_INPUT_FINISH_BEAT_SIZE 16
#define MCDMA_COMPUTE_RX_BATCH_PAGES 48

#define MEMORY_RANGE_SET_ENTRY_NUM 16
#define CP SPDK_DEBUGLOG(nvmf, "Checkpoint\n");;

const char *g_mcdma_dev = "b0000000.dma";

uint8_t poll_group_cnt = 0;

static int
mcdma_compute_rx_batch_pages(void)
{
	static int batch_pages;
	const char *value;
	char *end = NULL;
	long parsed;

	if (spdk_likely(batch_pages != 0)) {
		return batch_pages;
	}

	batch_pages = MCDMA_COMPUTE_RX_BATCH_PAGES;
	value = getenv("SUDA_MCDMA_RX_BATCH_PAGES");
	if (value != NULL && value[0] != '\0') {
		parsed = strtol(value, &end, 10);
		if (end != value && *end == '\0' && parsed >= 1 &&
		    parsed <= MCDMA_COMPUTE_RX_BATCH_PAGES) {
			batch_pages = (int)parsed;
		} else {
			SPDK_WARNLOG("Ignoring invalid SUDA_MCDMA_RX_BATCH_PAGES=%s; "
				     "expected 1..%d\n",
				     value, MCDMA_COMPUTE_RX_BATCH_PAGES);
		}
	}

	SPDK_NOTICELOG("Compute RX batch size: %d page(s)\n", batch_pages);
	return batch_pages;
}

enum spdk_nvmf_mcdma_request_state {
	/* The request is not currently in use */
	MCDMA_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	MCDMA_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	// MCDMA_REQUEST_STATE_NEED_BUFFER,

	MCDMA_REQUEST_STATE_GET_SGL,

	MCDMA_REQUEST_STATE_GET_DATA,

	/* The request is waiting on MCDMA queue depth availability
	 * to transfer data from the host to the controller.
	 */
	// MCDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,

	/* The request is currently transferring data from the host to the controller. */
	MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	MCDMA_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	MCDMA_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	MCDMA_REQUEST_STATE_EXECUTED,

	/*The request(kernel) is processing*/
	MCDMA_REQUEST_STATE_KERNEL_PROCESSING,

	/* The request is waiting on MCDMA queue depth availability
	 * to transfer data from the controller to the host.
	 */
	// MCDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,

	/* The request is ready to send a completion */
	MCDMA_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request is currently transferring data from the controller to the host. */
	// MCDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,

	/* The request currently has an outstanding completion without an
	 * associated data transfer.
	 */
	MCDMA_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	MCDMA_REQUEST_STATE_COMPLETED,

	/* Terminator */
	MCDMA_REQUEST_NUM_STATES,
};

SPDK_TRACE_REGISTER_FN(nvmf_trace, "nvmf_mcdma", TRACE_GROUP_NVMF_MCDMA)
{
	spdk_trace_register_object(OBJECT_NVMF_RDMA_IO, 'r');
	spdk_trace_register_description("MCDMA_REQ_NEW", TRACE_MCDMA_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 1,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_NEED_BUFFER", TRACE_MCDMA_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_TX_PEND_C2H",
					TRACE_MCDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_TX_PEND_H2C",
					TRACE_MCDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_TX_H2C",
					TRACE_MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_RDY_TO_EXEC",
					TRACE_MCDMA_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_EXECUTING",
					TRACE_MCDMA_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_EXECUTED",
					TRACE_MCDMA_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_RDY_TO_CMPL",
					TRACE_MCDMA_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_CMPLNG_C2H",
					TRACE_MCDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_COMPLETING",
					TRACE_MCDMA_REQUEST_STATE_COMPLETING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_REQ_COMPLETED",
					TRACE_MCDMA_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("MCDMA_KERNEL_EXEC",
					TRACE_MCDMA_REQUEST_STATE_KERNEL_EXEC,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "cid");
	spdk_trace_register_description("MCDMA_HLS_EXEC",
					TRACE_MCDMA_REQUEST_STATE_HLS_EXEC,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_STR, "function");
	
}

struct byp_io {
	struct spdk_axi_dma_iovec iov;
	void *buf;
	struct spdk_axi_dma_io *cur_io;
	void *req;
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_mcdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_mcdma_recv {
	// struct ibv_recv_wr			wr;
	struct byp_io *byp_io;

	uint64_t				receive_tsc;

	STAILQ_ENTRY(spdk_nvmf_mcdma_recv)	link;
};

struct spdk_nvmf_mcdma_request {
	struct spdk_nvmf_request		req;

	enum spdk_nvmf_mcdma_request_state	state;

	/* Data offset in req.iov */
	uint32_t				offset;

	struct spdk_nvmf_mcdma_qpair		*qpair;

	struct spdk_nvmf_mcdma_recv		*recv;

	void		*admin_data_buf;

	struct {
		struct spdk_axi_dma_iovec			*iovs;
		int iovcnt;
	} rsp;

	uint32_t				iovpos;

	uint32_t				num_outstanding_data_wr;
	uint64_t				receive_tsc;

	bool					fused_failed;
	struct spdk_nvmf_mcdma_request		*fused_pair;

	struct spdk_nvme_sgl_descriptor 	*sgl_buf;
	void 					*data_buf;

	STAILQ_ENTRY(spdk_nvmf_mcdma_request)	state_link;
	STAILQ_HEAD(, spdk_nvmf_mcdma_recv)	recv_queue;
	struct spdk_thread* impl_thread;
};

struct spdk_mcdma_qp {
	struct spdk_axi_dma_ch *tx_ch;
	struct spdk_axi_dma_ch *rx_ch;
	struct byp_io *rx_ios;
	struct spdk_ring *data_rx_io_ring;
	//struct spdk_compute_qpair compute_ch;
	struct spdk_nvmf_mcdma_poller *poller;
	//struct spdk_poller *compute_poller;
	uint32_t chid;
};

struct spdk_nvmf_mcdma_resource_opts {
	struct spdk_nvmf_mcdma_qpair	*qpair;
	/* qp points either to an ibv_qp object or an ibv_srq object depending on the value of shared. */
	// void				*qp;
	struct spdk_mcdma_qp *qp;
	struct ibv_pd			*pd;
	uint32_t			max_queue_depth;
	uint32_t			in_capsule_data_size;
	// bool				shared;
};

struct nvmf_cpl_padded {
	union nvmf_c2h_msg			msg;
	uint8_t padding[MCDMA_NVMF_PAD_SIZE];
};

struct spdk_nvmf_mcdma_resources {
	/* Array of size "max_queue_depth" containing RDMA requests. */
	struct spdk_nvmf_mcdma_request		*reqs;

	struct spdk_nvmf_mcdma_request		*cur_req;

	/* Array of size "max_queue_depth" containing RDMA recvs. */
	struct spdk_simple_pool			*recv_pool;
	/* Array of size "max_queue_depth" containing 64 byte capsules
	 * used for receive.
	 */
	// union nvmf_h2c_msg			*cmds;
	// struct ibv_mr				*cmds_mr;

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	struct nvmf_cpl_padded			*cpls;
	// struct ibv_mr				*cpls_mr;

	/* Array of size "max_queue_depth * InCapsuleDataSize" containing
	 * buffers to be used for in capsule data.
	 */
	void					*bufs;
	void					*cpl_data_bufs;
	// struct ibv_mr				*bufs_mr;

	/* Receives that are waiting for a request object */
	STAILQ_HEAD(, spdk_nvmf_mcdma_recv)	incoming_queue;

	/* Queue to track free requests */
	STAILQ_HEAD(, spdk_nvmf_mcdma_request)	free_queue;
};

struct spdk_nvmf_mcdma_qpair {
	struct spdk_nvmf_qpair			qpair;

	struct spdk_nvmf_mcdma_device		*device;
	struct spdk_nvmf_mcdma_poller		*poller;

	struct spdk_mcdma_qp *mcdma_qp;

	/* Cache the QP and Controller number to improve QP search by RB tree. */
	uint32_t				qp_num;
	uint16_t        ctrlr_num;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	uint16_t				running_compute_jobs;

	/* The maximum number of RDMA SEND operations at one time */
	uint32_t				max_send_depth;

	/* The current number of active RDMA READ operations */
	uint16_t				current_read_depth;

	/* The current number of posted WRs from this qpair's
	 * send queue. Should not exceed max_send_depth.
	 */
	uint32_t				current_send_depth;

	/* The maximum number of SGEs per WR on the send queue */
	uint32_t				max_send_sge;

	/* The maximum number of SGEs per WR on the recv queue */
	uint32_t				max_recv_sge;

	struct spdk_nvmf_mcdma_resources		*resources;

	STAILQ_HEAD(, spdk_nvmf_mcdma_request)	pending_mcdma_read_queue;

	STAILQ_HEAD(, spdk_nvmf_mcdma_request)	pending_mcdma_write_queue;

	/* Number of requests not in the free state */
	uint32_t				qd;

	RB_ENTRY(spdk_nvmf_mcdma_qpair)		node;

	STAILQ_ENTRY(spdk_nvmf_mcdma_qpair)	recv_link;

	STAILQ_ENTRY(spdk_nvmf_mcdma_qpair)	send_link;

	/* Points to the a request that has fuse bits set to
	 * SPDK_NVME_CMD_FUSE_FIRST, when the qpair is waiting
	 * for the request that has SPDK_NVME_CMD_FUSE_SECOND.
	 */
	struct spdk_nvmf_mcdma_request		*fused_first;

	/*
	 * io_channel which is used to destroy qpair when it is removed from poll group
	 */
	struct spdk_io_channel		*destruct_channel;

	/* List of ibv async events */
	STAILQ_HEAD(, spdk_nvmf_mcdma_ibv_event_ctx)	ibv_events;

	/* Lets us know that we have received the last_wqe event. */
	bool					last_wqe_reached;

	/* Indicate that nvmf_mcdma_close_qpair is called */
	bool					to_close;

	bool					started;

};

struct spdk_nvmf_mcdma_poller_stat {
	uint64_t				completions;
	uint64_t				polls;
	uint64_t				idle_polls;
	uint64_t				requests;
	uint64_t				request_latency;
	uint64_t				pending_free_request;
	uint64_t				pending_mcdma_read;
	uint64_t				pending_mcdma_write;
	struct spdk_axi_dma_ch_stat		qp_stats;
};

struct spdk_nvmf_mcdma_poller {
	struct spdk_nvmf_mcdma_device		*device;
	struct spdk_nvmf_mcdma_poll_group	*group;

	int					num_cqe;
	int					required_num_wr;

	uint64_t last_poll_ticks;
	uint64_t poll_unstarted_interval_ticks;

	struct spdk_nvmf_mcdma_resources		*resources;
	struct spdk_nvmf_mcdma_poller_stat	stat;

	struct spdk_mcdma_qp *mcdma_qps;

	int num_mcdma_qp;

	RB_HEAD(qpairs_tree, spdk_nvmf_mcdma_qpair) qpairs;

	STAILQ_HEAD(, spdk_nvmf_mcdma_qpair)	qpairs_pending_recv;

	STAILQ_HEAD(, spdk_nvmf_mcdma_qpair)	qpairs_pending_send;

	TAILQ_ENTRY(spdk_nvmf_mcdma_poller)	link;
};

struct spdk_nvmf_mcdma_poll_group_stat {
	uint64_t				pending_data_buffer;
};

struct spdk_nvmf_mcdma_poll_group {
	struct spdk_nvmf_transport_poll_group		group;
	struct spdk_nvmf_mcdma_poll_group_stat		stat;
	TAILQ_HEAD(, spdk_nvmf_mcdma_poller)		pollers;
	TAILQ_ENTRY(spdk_nvmf_mcdma_poll_group)		link;
};

struct spdk_nvmf_mcdma_conn_sched {
	struct spdk_nvmf_mcdma_poll_group *next_admin_pg;
	struct spdk_nvmf_mcdma_poll_group *next_io_pg;
};

/* Assuming mcdma_cm uses just one protection domain per ibv_context. */
struct spdk_nvmf_mcdma_device {
	// struct ibv_device_attr			attr;
	// struct ibv_context			*context;

	// struct spdk_mcdma_mem_map		*map;
	// struct ibv_pd				*pd;

	// int					num_srq;
	struct spdk_axi_dma_dev *mcdma;

	//**provided for operators scheduler */
	struct spdk_hlsacccompute_dev compute;
	struct spdk_ring* compute_req_ring_list[SPDK_NVMF_MCDMA_MAX_QPAIRS];//equal to qpair num
	
	/** -------------END---------------- */

	char *name;

	pthread_mutex_t pfch_tag_lock;

	union {
		uint8_t byte[8];
		struct {
			uint32_t lsb;
			uint32_t msb;
		} dword;
	} pfch_tag;

	TAILQ_ENTRY(spdk_nvmf_mcdma_device)	link;

	/**Use for memory namespace ranges set function */
	//Support Up to 4 virtual devices
	void* memrangeset_hash_tables;
	atomic_uint memrangeset_next_id;

	/**Used for computational storage */
	struct spdk_thread* compute_thread,*handc_thread;
	pthread_mutex_t compute_mutex;
	struct spdk_axi_dma_ch* compute_tx_channel,*compute_rx_channel;
	/**/

/**debug*/
	uint64_t one_kernel_start_execute_time;
	uint64_t all_kernel_executed_time;
	//uint64_t one_step_executed_start_time;
	uint64_t kernel_num;
	//uint64_t all_kernel_executed_time_buf[3000];

};

struct spdk_nvmf_mcdma_port {
	const struct spdk_nvme_transport_id	*trid;
	// struct mcdma_cm_id			*id;
	struct spdk_nvmf_mcdma_device		*device;
	TAILQ_ENTRY(spdk_nvmf_mcdma_port)	link;
};

struct mcdma_transport_opts {
	int		num_cqe;
	// uint32_t	max_srq_depth;
	// bool		no_srq;
	// bool		no_wr_batching;
	// int		acceptor_backlog;
};

struct spdk_nvmf_mcdma_transport {
	struct spdk_nvmf_transport	transport;
	struct mcdma_transport_opts	mcdma_opts;

	struct spdk_nvmf_mcdma_conn_sched conn_sched;

	pthread_mutex_t			lock;

	struct spdk_nvmf_mcdma_poll_group	*poll_groups[SPDK_NVMF_MCDMA_MAX_QPAIRS];
	int num_poll_groups;

	TAILQ_HEAD(, spdk_nvmf_mcdma_device)	devices;
	TAILQ_HEAD(, spdk_nvmf_mcdma_port)	ports;
};



struct memory_range_sets{
	union spdk_nvme_memory_range_set_decriptor raw_data[MEMORY_RANGE_SET_ENTRY_NUM];
	struct spdk_hlsacccompute_virtual_object quick_cache[MEMORY_RANGE_SET_ENTRY_NUM];
	union spdk_nvme_memory_range_set_decriptor header;
};
bool
nvmf_mcdma_request_process(struct spdk_nvmf_mcdma_transport *rtransport,
			  struct spdk_nvmf_mcdma_request *mcdma_req);

static void rx_data_cmpl_cb(struct spdk_axi_dma_io *io, int status);
static void mcdma_qp_rx_cmpl_cb(struct spdk_axi_dma_io *io, int status);
static struct spdk_nvmf_mcdma_device *nvmf_mcdma_get_device(void);


struct axi_dma_channel_info
{
	struct spdk_axi_dma_dev *dev;
	struct spdk_axi_dma_ch *ch;
	struct spdk_axi_dma_iovec iovs[64];
	unsigned char
		data_ptr;  // 指示当前iovs的指针，如果任务被暂停过，指针非0，否则为0（用于后续优化，未启用）
	unsigned char
		iovcnt;  // 指示当前使用了多少个iov--------------------------------（用于后续优化，未启用）
	bool is_working;
	bool is_last;  // 仅rx
				   // channel有用，表示收到了tuser=0xff的信号表示，数据接受完成
	bool reg_for_next_send;//确定下一次轮询是否要进行发送了
};



int tx_channel_send(struct spdk_hlsacccompute_channel *ch) {
	struct axi_dma_channel_info *axi_dma_info =
		(struct axi_dma_channel_info *)ch->channel_info;
	if (axi_dma_info == NULL || axi_dma_info->ch == NULL ||
		axi_dma_info->dev == NULL) {
	  SPDK_ERRLOG("Failed To Use Channel, Pointer is NULL\n");
	  return -1;
	}
	if (spdk_unlikely(axi_dma_info->ch->cmpl_poller == NULL)) {
	  axi_dma_info->ch->cmpl_poller =
		  spdk_poller_register(ch->channel_poller, ch, 0);
	}else{
	  spdk_poller_resume(axi_dma_info->ch->cmpl_poller);
	}
	struct spdk_axi_dma_ctrl ctrl;
	ctrl.tdest = ch->channel_id;
	ctrl.tid = ch->dest_id;
	ctrl.tuser = 0;
	axi_dma_info->is_working = true;
	if (spdk_unlikely(TAILQ_EMPTY(&ch->req->tx_vos[ch->virtual_channel_id]))) {
	  axi_dma_info->is_working = false;
	  ch->channel_done(ch);
	  return 0;
	}
	struct spdk_hlsacccompute_virtual_object *ob =
		TAILQ_FIRST(&ch->req->tx_vos[ch->virtual_channel_id]);
	if (spdk_unlikely(ob == NULL)) {  // 检查分配的ob地址是否为空，为空需要报错
	  SPDK_ERRLOG("VIRTUAL OBJECT PTR IS NULL\n");
	  return -1;
	}
	int size = ob->iov_len - ob->cur_used;
	int iovcnt = (size / PAGE_SIZE) + (size % PAGE_SIZE != 0);
	bool last_data = false;
	if (iovcnt > 48) {
	  iovcnt = 48;
	} else {
	  // 判断一下是否数据到达了Virtual Object链条的末尾
	  // 如果virtual
	  // object描述的数据大小超过了最大限制，就代表下一次还需要传输一部分数据，因此无需设置last_data
	  // 如果在最大限制内，而且到达末尾，代表数据last_data需要拉高，表示数据已经处理完成
	  if (TAILQ_NEXT(ob, link) == NULL) {
		last_data = true;
		SPDK_DEBUGLOG(nvmf,"SEND LAST DATA ID%d\n",ch->req->request_id);
	  }
	}
	for (int i = 0; i < iovcnt; i++) {
	  int offset = PAGE_SIZE * i;
	  int chunk_size = size - offset;
	  if (chunk_size > PAGE_SIZE) {
		chunk_size = PAGE_SIZE;
	  }
	  axi_dma_info->iovs[i].iov_base =
		  ob->iov_base + ob->cur_used + offset;
	  axi_dma_info->iovs[i].iov_len = chunk_size;
	  axi_dma_info->iovs[i].paddr =
		  spdk_vtophys(axi_dma_info->iovs[i].iov_base, NULL);
	}
	uint64_t tx_first_qword = 0;
	if (ob->iov_base != 0 && size >= (int)sizeof(tx_first_qword)) {
	  memcpy(&tx_first_qword, (void *)(uintptr_t)(ob->iov_base + ob->cur_used), sizeof(tx_first_qword));
	}
	SPDK_NOTICELOG("LWE_TX_SEND 提交输入流 req=%p request_id=%u channel=%d dest=0x%x size=%d ob_len=%zu cur_used=%zu iovcnt=%d last_data=%d first_qword=0x%llx\n",
		ch->req,
		ch->req->request_id,
		ch->channel_id,
		ch->dest_id,
		size,
		ob->iov_len,
		ob->cur_used,
		iovcnt,
		last_data ? 1 : 0,
		(unsigned long long)tx_first_qword);
	SPDK_DEBUGLOG(hlsacc,"CHANNEL SEND IOVCNT%d\n",iovcnt);
	axi_dma_info->iovcnt = iovcnt;
	spdk_axi_dma_tx_channel_send_seg(axi_dma_info->ch, axi_dma_info->iovs, iovcnt,
									 ch->channel_done, ch, &ctrl, last_data);
	return size;
  }
  int tx_channel_recv(struct spdk_hlsacccompute_channel *ch, void *dst, int size,
					  int align) {
	return -1;
  }
  int tx_channel_done(struct spdk_hlsacccompute_channel *ch) {
	if (!TAILQ_EMPTY(&(ch->req->tx_vos[ch->virtual_channel_id]))) {
	  // 如果virtual object通道不为空，那就继续发送
	  struct spdk_hlsacccompute_virtual_object *ob =
		  TAILQ_FIRST(&(ch->req->tx_vos[ch->virtual_channel_id]));
	  int data_left = ob->iov_len - ob->cur_used;
	  if (data_left == 0) {
		struct spdk_hlsacccompute_virtual_object *swap_ob = TAILQ_NEXT(ob, link);
		TAILQ_REMOVE(&(ch->req->tx_vos[ch->virtual_channel_id]), ob, link);
		//TODO Fix this bug in the future :(
		TAILQ_INSERT_HEAD(&(ch->req->dev->vo_pool),ob,link);
		//Get size
		struct spdk_hlsacccompute_virtual_object* obb;
		int ele = 0;
		TAILQ_FOREACH(obb,&(ch->req->dev->vo_pool),link){
		  ele++;
		}
		SPDK_DEBUGLOG(hlsacc,"DUMP VO_POOL SIZE%d\n",ele);
		ob = swap_ob;
		if (ob != NULL) data_left = ob->iov_len;
	  }
	  if (ob != NULL && data_left > 0 &&
		  ((struct axi_dma_channel_info *)(ch->channel_info))->is_working) {
		int next_send_data =
			data_left > 64 * PAGE_SIZE ? 64 * PAGE_SIZE : data_left;
		// data_left -= next_send_data;
		// 发送数据并不移动cur_used指针，只有数据发送完成，才更新cur_used指针
		((struct axi_dma_channel_info *)(ch->channel_info))->reg_for_next_send = true;
		//ch->channel_send(ch);
		return 0;
	  }else ((struct axi_dma_channel_info *)(ch->channel_info))->reg_for_next_send = false;
	}
	{
	  // 如果virtual object全部发送完成，此处可以进行最后的收尾工作
	  // 倘若request的类型是长服务类型，也就是数据源会随着时间进行动态更新
	  // 那就不通知释放，但是可以适度降低request的优先级
	  // 如果request不是上述类型，代表输入的数据已经全部完成，那可以选择释放通道资源
	  // 只有在rx_channel接收全部完成之后，才释放request资源
	  // tx_channel发送完毕只释放发送资源
	  // 此处偷懒，先选择不释放:)
	}
	return 0;
  }
  
  int tx_rx_channel_pause(struct spdk_hlsacccompute_channel *ch) {
	
	((struct axi_dma_channel_info *)(ch->channel_info))->is_working = false;
	if(((struct axi_dma_channel_info *)(ch->channel_info))->ch->cmpl_poller!=NULL)
	  spdk_poller_pause(((struct axi_dma_channel_info *)(ch->channel_info))->ch->cmpl_poller);
	  struct spdk_hlsacccompute_virtual_object *ob =
	  TAILQ_FIRST(&(ch->req->tx_vos[ch->virtual_channel_id]));
	bool wait_for_time = false;
	int data_left = 0;
	if(ob){
	  data_left = ob->iov_len - ob->cur_used;
	  if(data_left <= 64*PAGE_SIZE){
		wait_for_time = true;
	  }
	}
	/*
	if(wait_for_time){
	  SPDK_DEBUGLOG(hlsacc,"WAITTIMES\n");
	  //usleep(500);
	  usleep(data_left/PAGE_SIZE);
	}*/
	spdk_axi_dma_stop_channel(
		((struct axi_dma_channel_info *)(ch->channel_info))->ch);
	return 0;
  }
  
  int tx_rx_channel_release(struct spdk_hlsacccompute_channel *ch,
							bool context_save) {
	// 先暂停通道
	// spdk_axi_dma_stop_channel(((struct axi_dma_channel_info
	// *)(ch->channel_info))->ch); 然后统计一下当前发送出多少数据
	// 因为在poller里面，发送数据的信息已经被确认过了，所以可以直接保存资源并释放，但是为了保险起见
	// 还是需要重新统计一下
	((struct axi_dma_channel_info *)(ch->channel_info))->is_working = false;
	ch->channel_poller((void *)ch);
	
	spdk_simple_pool_reset(&(((struct axi_dma_channel_info *)(ch->channel_info))->ch->io_pool));							
	if(((struct axi_dma_channel_info *)(ch->channel_info))->ch->cmpl_poller!=NULL)
	  spdk_poller_pause(((struct axi_dma_channel_info *)(ch->channel_info))->ch->cmpl_poller);
	else{
		  assert(false);
	}
	
	//((struct axi_dma_channel_info *)(ch->channel_info))->ch->cmpl_poller = NULL;
	
	// 暂停Poller
	// 因为关键数据已经保存在context_save中，所以忽略上下文保存
	// SPDK_DEBUGLOG(hlsacc,"DATA PROCESSED%d\n",((struct axi_dma_channel_info
	// *)(ch->channel_info))->fin_used_data);
	int used_data = -1;
	if (ch->is_tx && !TAILQ_EMPTY(&(ch->req->tx_vos[ch->virtual_channel_id]))) {
	  used_data =
		  TAILQ_FIRST(&(ch->req->tx_vos[ch->virtual_channel_id]))->cur_used;
	} else if (!ch->is_tx &&
			   !TAILQ_EMPTY(&(ch->req->rx_vos[ch->virtual_channel_id]))) {
	  used_data =
		  TAILQ_FIRST(&(ch->req->rx_vos[ch->virtual_channel_id]))->cur_used;
	}
	if (used_data != -1) {
	  SPDK_DEBUGLOG(hlsacc,"DATA PROCESSED%d is_tx%d\n", used_data, ch->is_tx);
	} else {
	  SPDK_DEBUGLOG(hlsacc,"ALL DATA HAS BEEN SEND OUT\n");
	}
	((struct axi_dma_channel_info *)(ch->channel_info))->is_last = false;
	((struct axi_dma_channel_info *)(ch->channel_info))->iovcnt = 0;
	// 释放最新的io
  
	return 0;
  }
  
  int tx_rx_channel_poller(void *ctx)
  {
	struct spdk_hlsacccompute_channel *ch = (struct spdk_hlsacccompute_channel *)ctx;
	struct axi_dma_channel_info *axi_dma_info = (struct axi_dma_channel_info *)ch->channel_info;
	int transfered_length;
	struct spdk_axi_dma_io *io = spdk_axi_dma_poller_for_compute((((struct axi_dma_channel_info *)ch->channel_info))->ch);
	struct spdk_axi_dma_io *last_io = NULL;
	transfered_length = 0;
	if (io == NULL)
	  return 0;
	
	while (io)
	{
	  SPDK_DEBUGLOG(hlsacc,"LWE_CH_CMPL %s完成 req=%p request_id=%u channel=%d bytes=%u tuser=0x%x tid=0x%x iovcnt_before=%d result_before=%llu is_last_before=%d used_iovcnt=%d\n",
		  ch->is_tx ? "TX" : "RX",
		  ch->req,
		  ch->req->request_id,
		  ch->channel_id,
		  io->status.transfered_bytes,
		  (unsigned int)io->ctrl.tuser,
		  (unsigned int)io->ctrl.tid,
		  axi_dma_info->iovcnt,
		  ch->req->result,
		  axi_dma_info->is_last ? 1 : 0,
		  io->used_iovcnt);
		  if (io->status.transfered_bytes == 0 || ((unsigned char)(io->ctrl.tuser)>>4) == 0xf)
		  {
			// 收到了tuser=0xff的信号，表示数据完全处理完成
			if (((unsigned char)(io->ctrl.tuser)>>4) == 0xf)
			{
			  uint32_t finish_completion_bytes = io->status.transfered_bytes;
			  uint32_t finish_beat_bytes = ch->is_tx ?
				  MCDMA_TASK_INPUT_FINISH_BEAT_SIZE : MCDMA_TASK_FINISH_BEAT_SIZE;
			  uint32_t payload_bytes = 0;
			  if (finish_completion_bytes >= finish_beat_bytes) {
				payload_bytes = finish_completion_bytes - finish_beat_bytes;
			  } else if (finish_completion_bytes != 0) {
				SPDK_ERRLOG(
					"%s结束completion长度异常: bytes=%u，小于结束beat长度%u\n",
					ch->is_tx ? "TX" : "RX",
					finish_completion_bytes,
					finish_beat_bytes);
			  }
			  /*
			   * AXI DMA aggregates every descriptor in one software submission.
			   * The final completion therefore contains payload plus SUDA's
			   * TUSER=0xff marker: 16 bytes on TX and 64 bytes on RX.
			   */
			  transfered_length += payload_bytes;
			  SPDK_NOTICELOG(
				  "LWE_FINISH_ACCOUNT %s结束completion记账 req=%p request_id=%u bytes=%u payload_bytes=%u finish_bytes=%u result_before=%llu\n",
				  ch->is_tx ? "TX" : "RX",
				  ch->req,
				  ch->req->request_id,
				  finish_completion_bytes,
				  payload_bytes,
				  finish_beat_bytes,
				  (unsigned long long)ch->req->result);
		  SPDK_DEBUGLOG(hlsacc,"RECV REAL LAST SIGNAL\n");
		  if (!ch->is_tx) {
			axi_dma_info->is_last = true;
		  } else {
			axi_dma_info->iovcnt = 0;
		  }
		  last_io = io;
		 
		  spdk_axi_dma_io_free(io);	
		}
		break;
	  }
	  transfered_length += io->status.transfered_bytes;
	  last_io = io;
		  if (io->iovcnt >= axi_dma_info->iovcnt) {
			axi_dma_info->iovcnt = 0;
		  } else {
			axi_dma_info->iovcnt -= io->iovcnt;
		  }
	  io = spdk_axi_dma_poller_for_compute((((struct axi_dma_channel_info *)ch->channel_info))->ch);

	  //这里axi_dma_io的分配需要重新设计和优化！！！
	  if(((last_io->used_iovcnt))<=0){
		spdk_axi_dma_io_free(last_io);
	  }
	}
	//注意！！！当存在多个通道的时候，这个设计会带来BUG！
	//TODO 后续修复这个BUG！！！
	if(!(ch->is_tx)){
		ch->req->result += transfered_length;
	}
	if (transfered_length == 0&&!(!ch->is_tx&&axi_dma_info->is_last)){
	  return 0;
	}
  
	if (ch->is_tx)
	{
  
	  struct spdk_hlsacccompute_virtual_object *ob = TAILQ_FIRST(&(ch->req->tx_vos[ch->virtual_channel_id]));
	  ob->cur_used += transfered_length;
	  SPDK_DEBUGLOG(hlsacc,"POLLED TX GET DATA%d CHANNEL ID%d\n", transfered_length,ch->channel_id);
	  SPDK_DEBUGLOG(hlsacc,"CUR USED%d\n",ob->cur_used);
	  if (ob->cur_used == ob->iov_len)
	  {
		TAILQ_REMOVE(&(ch->req->tx_vos[ch->virtual_channel_id]), ob, link);
		TAILQ_INSERT_HEAD(&(ch->req->dev->vo_pool),ob,link);
	  }
	  else if (spdk_unlikely(ob->cur_used > ob->iov_len))
	  {
		SPDK_ERRLOG("ERROR! DATA USED IS BIGGER THAN NEEDED!\n");
		return -1;
	  }
	}
	else
	{
	  struct spdk_hlsacccompute_virtual_object *ob = TAILQ_FIRST(&(ch->req->rx_vos[ch->virtual_channel_id]));
	  ob->cur_used += transfered_length;
	  SPDK_DEBUGLOG(nvmf,"POLLED RX GET DATA%d CHANNEL ID%d\n", transfered_length,ch->channel_id);
	 
	  if (ob->cur_used >= ob->iov_len)
	  {
  
		TAILQ_REMOVE(&(ch->req->rx_vos[ch->virtual_channel_id]), ob, link);
		TAILQ_INSERT_HEAD(&(ch->req->dev->vo_pool),ob,link);
	  }
	  else if (spdk_unlikely(ob->cur_used > ob->iov_len))
	  {
		SPDK_ERRLOG("ERROR! DATA USED IS BIGGER THAN NEEDED!\n");
		return -1;
	  }
	}
	
  
	// 注意，如果传输完成所有数据，或者调用release函数触发了is_working变更为false，均会导致函数axi_dma_io结构体释放！
	

	if(((!ch->is_tx)&&axi_dma_info->iovcnt<=0)||axi_dma_info->is_working == false || axi_dma_info->is_last == true || ((ch->is_tx&&axi_dma_info->iovcnt==0))){
	  ch->channel_done(ch);
	}
	if(axi_dma_info->reg_for_next_send==true&&ch->is_tx&&axi_dma_info->is_working!=false){
	  ch->channel_send(ch);
	  axi_dma_info->reg_for_next_send = false;
	}
	return 0;
  }
  int tx_rx_channel_apply(struct spdk_hlsacccompute_channel *ch) {
	struct axi_dma_channel_info *axi_dma_info =
		(struct axi_dma_channel_info *)ch->channel_info;
	axi_dma_info->iovcnt = 0;
	spdk_axi_dma_enable_channel(axi_dma_info->ch);
	return 0;
  }
  int rx_channel_send(struct spdk_hlsacccompute_channel *ch, void *src, int size,
					  int align) {
	return -1;
  }
  int rx_channel_recv(struct spdk_hlsacccompute_channel *ch) {
	struct axi_dma_channel_info *axi_dma_info =
		(struct axi_dma_channel_info *)ch->channel_info;
	if (axi_dma_info == NULL || axi_dma_info->ch == NULL ||
		axi_dma_info->dev == NULL) {
	  SPDK_ERRLOG("Failed To Use Channel, Pointer is NULL\n");
	  return -1;
	}
	if (spdk_unlikely(axi_dma_info->ch->cmpl_poller == NULL)) {
	  axi_dma_info->ch->cmpl_poller =
		  spdk_poller_register(ch->channel_poller, ch, 0);
	  
	}else{
	  spdk_poller_resume(axi_dma_info->ch->cmpl_poller);
	}
	axi_dma_info->is_working = true;
	struct spdk_hlsacccompute_virtual_object *ob =
		TAILQ_FIRST(&ch->req->rx_vos[ch->virtual_channel_id]);
	if (spdk_unlikely(ob == NULL)) {  // 检查分配的ob地址是否为空，为空需要报错
	  SPDK_ERRLOG("VIRTUAL OBJECT PTR IS NULL\n");
	  return -1;
	}
	int size = ob->iov_len - ob->cur_used;
	int iovcnt = (size / PAGE_SIZE) + (size % PAGE_SIZE != 0);
	/* One software completion now represents the whole multi-BD submission. */
	int max_cnt = mcdma_compute_rx_batch_pages();
	if (iovcnt > max_cnt) {
	  iovcnt = max_cnt;
	  // SPDK_ERRLOG("Failed To Use Channel, Send Data Size Too Big\n");
	  // return -1;
	}
	if(iovcnt==0) return 0;
	for (int i = 0; i < iovcnt; i++) {
	  int chunk_size = size - PAGE_SIZE * i;
	  if (chunk_size > PAGE_SIZE) {
		chunk_size = PAGE_SIZE;
	  }
	  axi_dma_info->iovs[i].iov_base =
		  ob->iov_base + ob->cur_used + PAGE_SIZE * i;
	  axi_dma_info->iovs[i].iov_len = chunk_size;
	  axi_dma_info->iovs[i].paddr =
		  spdk_vtophys(axi_dma_info->iovs[i].iov_base, NULL);
	}
	SPDK_DEBUGLOG(hlsacc,"LWE_RX_RECV_POST 准备接收输出流 req=%p request_id=%u channel=%d size_left=%d ob_len=%zu cur_used=%zu iovcnt=%d first_paddr=0x%llx first_len=%zu\n",
		ch->req,
		ch->req->request_id,
		ch->channel_id,
		size,
		ob->iov_len,
		ob->cur_used,
		iovcnt,
		(unsigned long long)axi_dma_info->iovs[0].paddr,
		axi_dma_info->iovs[0].iov_len);
	int ret = spdk_axi_dma_rx_channel_recv(axi_dma_info->ch, axi_dma_info->iovs, iovcnt,
								 ch->channel_done, (void *)ch);
	if (ret != 0) {
	  axi_dma_info->iovcnt = 0;
	  return ret;
	}
	axi_dma_info->iovcnt = iovcnt;
	SPDK_DEBUGLOG(hlsacc,"CHANNEL RECV\n");
	return 0;
  }
  int rx_channel_done(struct spdk_hlsacccompute_channel *ch) {
	if (((struct axi_dma_channel_info *)(ch->channel_info))->is_last&&
	(ch->req->req_cb_fns!=NULL)) {
	  ch->req->req_cb_fns(ch->req, ch->req->req_cb_args);
	  return 0;
	}
	if (!TAILQ_EMPTY(&(ch->req->rx_vos[ch->virtual_channel_id]))) {
	  // 如果virtual object通道不为空，那就继续发送
	  struct spdk_hlsacccompute_virtual_object *ob =
		  TAILQ_FIRST(&(ch->req->rx_vos[ch->virtual_channel_id]));
	  int data_left = ob->iov_len - ob->cur_used;
	  if (data_left == 0) {
		struct spdk_hlsacccompute_virtual_object *swap_ob = TAILQ_NEXT(ob, link);
		TAILQ_REMOVE(&(ch->req->rx_vos[ch->virtual_channel_id]), ob, link);
		//TODO Fix this bug in the future :(
		TAILQ_INSERT_TAIL(&(ch->req->dev->vo_pool),ob,link);
		ob = swap_ob;
		if (ob != NULL) data_left = ob->iov_len;
	  }
	  //TODO FIX!!!!
	  if (ob != NULL && data_left > 0 &&
		  ((struct axi_dma_channel_info *)(ch->channel_info))->is_working&&!(((struct axi_dma_channel_info *)(ch->channel_info))->is_last)) {
		//int next_send_data =
		//	data_left > 64 * PAGE_SIZE ? 64 * PAGE_SIZE : data_left;
		// data_left -= next_send_data;
		// 发送数据并不移动cur_used指针，只有数据发送完成，才更新cur_used指针
		ch->channel_recv(ch);
		return 0;
	  }
	}
	{
	  //TODO FIx!!
	  // 如果virtual object全部发送完成，此处可以进行最后的收尾工作
	  // 倘若request的类型是长服务类型，也就是数据源会随着时间进行动态更新
	  // 那就不通知释放，但是可以适度降低request的优先级
	  // 如果request不是上述类型，代表输入的数据已经全部完成，那可以选择释放通道资源
	  // 只有在rx_channel接收全部完成之后，才释放request资源
	  // tx_channel发送完毕只释放发送资源
	  if (ch->req->req_cb_fns != NULL&&((struct axi_dma_channel_info *)(ch->channel_info))->is_working)
		ch->req->req_cb_fns(ch->req, ch->req->req_cb_args);
	}
	return 0;
  }
  


static void create_axi_dma_channel(int count, struct spdk_hlsacccompute_dev *dev, int phy_id_begin,struct spdk_axi_dma_dev* mcdma_dev)
{
  // TAILQ_INIT(&(dev->rx_channel_pool));
  // TAILQ_INIT(&(dev->tx_channel_pool));
  //static struct spdk_axi_dma_dev* mcdma_dev = NULL;
  struct spdk_hlsacccompute_channel *ch = spdk_malloc(sizeof(struct spdk_hlsacccompute_channel) * count * 2, 2, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
  struct axi_dma_channel_info *info = spdk_malloc(sizeof(struct axi_dma_channel_info) * count * 2, 2, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
  struct spdk_axi_dma_ch *axi_dma_ch = spdk_malloc(sizeof(struct spdk_axi_dma_ch) * count * 2, 2, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
  const char *g_mcdma_dev = "b0000000.dma";
  if(mcdma_dev==NULL)
    mcdma_dev = spdk_axi_dma_get_device(g_mcdma_dev);
  int i = 0;
  for (; i < count * 2; i++)
  {
    ch[i].channel_info = (void *)&(info[i]);
    info[i].ch = &(axi_dma_ch[i]);
    info[i].dev = mcdma_dev;
    info[i].iovcnt = 0;
    info[i].is_last = false;
    axi_dma_ch[i].cmpl_poller = NULL;
    axi_dma_ch[i].thread = spdk_get_thread();
    ch[i].channel_pause = tx_rx_channel_pause;
    ch[i].channel_poller = tx_rx_channel_poller;
    ch[i].channel_release = tx_rx_channel_release;
    ch[i].channel_apply = tx_rx_channel_apply;
    if (i >= count)
    {
      ch[i].is_tx = true;
      axi_dma_ch[i].env_ch = spdk_env_axi_dma_create_tx_channel(mcdma_dev->env_dev, 65, phy_id_begin + i - count, phy_id_begin + i - count);
      if (!axi_dma_ch[i].env_ch)
      {
        SPDK_ERRLOG("Failed To Create TX Channels\n");
        goto release_channel;
      }
      axi_dma_ch[i].id = phy_id_begin + i - count;
      ch[i].channel_id = phy_id_begin + i - count;
      // 创建iopool
      if ((spdk_simple_pool_init(&((axi_dma_ch[i]).io_pool), 4096, sizeof(struct spdk_axi_dma_io))) != 0)
      {
        SPDK_ERRLOG("Failed To Allocate AXI DMA IO POOL FOR CHANNEL %d\n", i);
        goto release_channel;
      }
      ch[i].channel_done = tx_channel_done;
      ch[i].channel_send = tx_channel_send;
      ch[i].channel_recv = tx_channel_recv;
      spdk_hlsacccompute_register_channel(dev, &ch[i]);
    }
    else
    {
      ch[i].is_tx = false;
      axi_dma_ch[i].env_ch = spdk_env_axi_dma_create_rx_channel(mcdma_dev->env_dev, 65, phy_id_begin + i, phy_id_begin + i);
      if (!axi_dma_ch[i].env_ch)
      {
        SPDK_ERRLOG("Failed To Create RX Channels\n");
        goto release_channel;
      }
      axi_dma_ch[i].id = phy_id_begin + i;
      ch[i].channel_id = phy_id_begin + i;
      ch[i].channel_done = rx_channel_done;
      ch[i].channel_send = rx_channel_send;
      ch[i].channel_recv = rx_channel_recv;
      // 创建iopool
      if ((spdk_simple_pool_init(&((axi_dma_ch[i]).io_pool), 4096, sizeof(struct spdk_axi_dma_io)) != 0))
      {
        SPDK_ERRLOG("Failed To Allocate AXI DMA IO POOL FOR CHANNEL %d\n", i);
        goto release_channel;
      }
      spdk_hlsacccompute_register_channel(dev, &ch[i]);
    }
  }
operator_done:
  return;
release_channel:
  return;
}

/**
 * Context Used To Operate Data Between Host and Device
 * Max To Handle 0.5GB File
 */
struct handc_ctx{
	struct spdk_axi_dma_iovec from_iovecs[64];
	struct spdk_axi_dma_iovec to_iovecs[64];
	int from_size;							//4B
	int to_size;							//4B
	struct spdk_nvmf_mcdma_device* device;  //8B
	struct spdk_thread* impl_thread;		//8B
	struct spdk_nvmf_mcdma_transport *rtransport;//8B
	struct spdk_nvmf_mcdma_request *mcdma_req;//8B
	enum {									//2B
		FETCH_PRP,
		END_FETCH_PRP,
		FETCH_DATA,
		END_FETCH_DATA,
		OPERATOR_SOURCE_RANGES,
		NON_OP
	} fsm_state;
	unsigned long long cur_bytes;
	unsigned long long prp_buf;
	unsigned long long total_rx_bytes;
	unsigned long long cur_rx_bytes;
	void* hls_request;
};

static_assert(sizeof(struct handc_ctx)<4096);

static const char *handc_fsm_name(int state)
{
	switch (state) {
	case FETCH_PRP:
		return "FETCH_PRP";
	case END_FETCH_PRP:
		return "END_FETCH_PRP";
	case FETCH_DATA:
		return "FETCH_DATA";
	case END_FETCH_DATA:
		return "END_FETCH_DATA";
	case OPERATOR_SOURCE_RANGES:
		return "OPERATOR_SOURCE_RANGES";
	case NON_OP:
		return "NON_OP";
	default:
		return "UNKNOWN";
	}
}

void compute_handc_impl(void* ctx){
	struct handc_ctx *hc = (struct handc_ctx*) ctx;
	SPDK_NOTICELOG(
		"HANDC_IMPL enter req=%p opc=0x%x fsm=%s cur_rx=%llu total_rx=%llu\n",
		hc->mcdma_req,
		hc->mcdma_req ? hc->mcdma_req->req.cmd->nvme_cmd.opc : 0,
		handc_fsm_name(hc->fsm_state),
		hc->cur_rx_bytes,
		hc->total_rx_bytes);
	if(hc->fsm_state==FETCH_PRP){
		hc->fsm_state = FETCH_DATA;
	}else{
		hc->fsm_state = END_FETCH_DATA;
	}
	SPDK_NOTICELOG(
		"HANDC_IMPL request_process req=%p next_fsm=%s\n",
		hc->mcdma_req,
		handc_fsm_name(hc->fsm_state));
	hc->mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
	nvmf_mcdma_request_process(hc->rtransport,hc->mcdma_req);
	return;
}
void compute_handc_op_tx_impl(struct spdk_axi_dma_io *io, int status){
	struct handc_ctx *hc = (struct handc_ctx*) io->ctx;
	SPDK_NOTICELOG(
		"HANDC_TX_CMPL req=%p opc=0x%x status=%d bytes=%u fsm=%s\n",
		hc->mcdma_req,
		hc->mcdma_req ? hc->mcdma_req->req.cmd->nvme_cmd.opc : 0,
		status,
		io->status.transfered_bytes,
		handc_fsm_name(hc->fsm_state));
	spdk_axi_dma_io_free(io);
	return;
}

void compute_handc_op_rx_impl(struct spdk_axi_dma_io *io, int status){
	
	struct handc_ctx *hc = (struct handc_ctx*) io->ctx;
	hc->cur_rx_bytes += io->status.transfered_bytes;
	SPDK_NOTICELOG(
		"HANDC_RX_CMPL req=%p opc=0x%x status=%d bytes=%u cur_rx=%llu total_rx=%llu fsm=%s\n",
		hc->mcdma_req,
		hc->mcdma_req ? hc->mcdma_req->req.cmd->nvme_cmd.opc : 0,
		status,
		io->status.transfered_bytes,
		hc->cur_rx_bytes,
		hc->total_rx_bytes,
		handc_fsm_name(hc->fsm_state));
	SPDK_DEBUGLOG(nvmf,"CUR RX BYTES GET %d TOTAL BYTES%d\n",hc->cur_rx_bytes,hc->total_rx_bytes);
	//if(hc->cur_rx_bytes>=hc->total_rx_bytes){
	//Might Not So useful
	//SPDK_NOTICELOG("TRANSFERED BYTES%d\n",io->status.transfered_bytes);
	if(hc->cur_rx_bytes>=hc->total_rx_bytes){
		spdk_axi_dma_io_free(io);
		if(hc->impl_thread!=NULL){
			SPDK_NOTICELOG(
				"HANDC_RX_DONE req=%p opc=0x%x posting back to impl thread\n",
				hc->mcdma_req,
				hc->mcdma_req ? hc->mcdma_req->req.cmd->nvme_cmd.opc : 0);
			SPDK_DEBUGLOG(nvmf,"HC CURRXBYTES%d TOTALRXBYTES%d\n",hc->cur_rx_bytes,hc->total_rx_bytes);
			if(hc->to_iovecs[0].iov_base!=NULL){
				unsigned int* data = (unsigned int*)(hc->to_iovecs[0].iov_base);
				SPDK_DEBUGLOG(nvmf,"DATA DUMP %lx %lx %lx %lx\n",data[0],data[1],data[2],data[3]);
			}
			spdk_thread_send_msg(hc->impl_thread,compute_handc_impl,hc);
		}else{
			SPDK_ERRLOG("UNDEFINED OPERATION!\n");
		}
	}
	
	return;
}
void compute_handc_op(void* ctx){
	struct handc_ctx *hc = (struct handc_ctx*) ctx;
	struct spdk_nvmf_mcdma_device* dev = hc->device;
	struct spdk_axi_dma_ctrl ctrl;
	ctrl.tdest = dev->compute_tx_channel->id;
	ctrl.tid = dev->compute_tx_channel->id;
	ctrl.tuser = 0;
	struct spdk_axi_dma_ch* ch;
	int iovcnt;
	SPDK_NOTICELOG(
		"HANDC_OP start req=%p opc=0x%x fsm=%s from_size=%d to_size=%d\n",
		hc->mcdma_req,
		hc->mcdma_req ? hc->mcdma_req->req.cmd->nvme_cmd.opc : 0,
		handc_fsm_name(hc->fsm_state),
		hc->from_size,
		hc->to_size);
	//RX RECV
	ch = dev->compute_rx_channel;
	iovcnt = hc->to_size;
	struct spdk_axi_dma_io *io = spdk_simple_pool_get(&ch->io_pool);
    if (!io) {
        SPDK_ERRLOG("Failed to allocate spdk_axi_dma_io\n");
        return;
    }
    io->ch = ch;
    io->iovs = hc->to_iovecs;
    io->iovcnt = iovcnt;
    io->cb = compute_handc_op_rx_impl;
    io->ctx = ctx;
    io->transfered_length = 0;
	
	
	SPDK_DEBUGLOG(nvmf,"RX IOVCNT%d\n",iovcnt);
    for (int i = 0; i < iovcnt; i++) {
        uint64_t len = io->iovs[i].iov_len;
        io->transfered_length += len;
		SPDK_NOTICELOG(
			"HANDC_OP RX_IOV[%d] base=%p paddr=0x%llx len=%llu\n",
			i,
			io->iovs[i].iov_base,
			(unsigned long long)io->iovs[i].paddr,
			(unsigned long long)len);
    }
	hc->total_rx_bytes = io->transfered_length;
	hc->cur_rx_bytes = 0;


    int ret = spdk_env_axi_dma_rx_channel_recv(ch->env_ch, hc->to_iovecs, iovcnt, io);
	SPDK_NOTICELOG(
		"HANDC_OP RX_SUBMIT req=%p ret=%d total_rx=%llu\n",
		hc->mcdma_req,
		ret,
		hc->total_rx_bytes);
	if (ret != 0) {
		SPDK_ERRLOG("HANDC_OP RX submit failed ret=%d req=%p\n", ret, hc->mcdma_req);
		spdk_axi_dma_io_free(io);
		return;
	}
	iovcnt = hc->from_size;
	//TX SEND
	for(int i=0;i<iovcnt;i++)
	{
		ch = dev->compute_tx_channel;
		io = spdk_simple_pool_get(&ch->io_pool);
		if (!io) {
			SPDK_ERRLOG("Failed to allocate spdk_axi_dma_io\n");
			return;
		}
		io->ch = ch;
		io->iovs = &(hc->from_iovecs[i]);
		
		io->iovcnt = 1;
		io->cb = compute_handc_op_tx_impl;
		io->ctx = ctx;
		io->transfered_length = 0;
		memcpy(&io->ctrl, &ctrl, sizeof(struct spdk_axi_dma_ctrl));
		io->transfered_length = hc->from_iovecs[i].iov_len;
		SPDK_NOTICELOG(
			"HANDC_OP TX_IOV[%d] base=%p paddr=0x%llx len=%llu\n",
			i,
			hc->from_iovecs[i].iov_base,
			(unsigned long long)hc->from_iovecs[i].paddr,
			(unsigned long long)hc->from_iovecs[i].iov_len);
		ret = spdk_env_axi_dma_tx_channel_send(ch->env_ch, &(hc->from_iovecs[i]), 1, io);
		SPDK_NOTICELOG(
			"HANDC_OP TX_SUBMIT[%d] req=%p ret=%d\n",
			i,
			hc->mcdma_req,
			ret);
		if (ret != 0) {
			SPDK_ERRLOG("HANDC_OP TX submit failed ret=%d req=%p idx=%d\n", ret, hc->mcdma_req, i);
			spdk_axi_dma_io_free(io);
			return;
		}
	}
	return;
}

void compute_request_impl(void* ctx){
	
	struct spdk_nvmf_mcdma_request* mcdma_req = ctx;
	mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_COMPLETE;
	struct spdk_nvmf_qpair *qpair = mcdma_req->req.qpair;
	struct spdk_nvmf_mcdma_transport	*rtransport = SPDK_CONTAINEROF(qpair->transport,
	struct spdk_nvmf_mcdma_transport, transport);
	//SPDK_DEBUGLOG(nvmf,"COMPUTE REQUEST IMPL ctx address%llx\n",mcdma_req->data_buf);
	nvmf_mcdma_request_process(rtransport,mcdma_req);
	spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
		(uintptr_t)mcdma_req, "compute_request_impl");
	return;
}

void hlsacccompute_req_callback(struct spdk_hlsacccompute_request *request, void *cb_arg)
{
	struct spdk_nvmf_mcdma_request* mcdma_req = cb_arg;
	mcdma_req->req.rsp->nvme_cpl.cdw0 = request->result;
	mcdma_req->req.rsp->nvme_cpl.cdw1 = request->result >> 32;
	SPDK_NOTICELOG(
		"HLSACC_REQ_CB req=%p mcdma_req=%p request_id=%d result=%llu\n",
		request,
		mcdma_req,
		request->request_id,
		request->result);
	SPDK_DEBUGLOG(nvmf,"GET RESULT%d request %llx id %d\n",request->result,request,request->request_id);
	spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
		(uintptr_t)mcdma_req, "hlsacccompute_req_callback");
	spdk_hlsacccompute_free_request(request->dev,request,true);

	//Send Msg Back To Poller Thread
	spdk_thread_send_msg(mcdma_req->impl_thread,compute_request_impl,cb_arg);
}

void hlsacccompute_run_request_preempt(void* ctx){
	
	struct spdk_hlsacccompute_request* request = ctx;
	
	SPDK_DEBUGLOG(nvmf,"PREEMPT\n");
	spdk_hlsacccompute_run_request(request->dev,request,true);
}

void hlsacccompute_run_request_unpreempt(void* ctx){
	struct spdk_hlsacccompute_request* request = ctx;
	struct spdk_nvmf_mcdma_request* mcdma_req = request->req_cb_args;
	spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
		(uintptr_t)mcdma_req, "run request ");
	
	SPDK_DEBUGLOG(nvmf,"UNPREEMPT\n");
	spdk_hlsacccompute_run_request(request->dev,request,false);
}

static inline void nvmf_mcdma_initialize_compute_dev(struct spdk_nvmf_mcdma_device* device){
//TODO 初始化hlsacccompute设备
	SPDK_DEBUGLOG(nvmf,"DEV INITING\n");
	if (spdk_hlsacccompute_dev_init(&(device->compute),SPDK_HLSACCCOMPUTE_BAR_PHYS_ADDR)) {
		SPDK_ERRLOG("Failed to initialize compute device\n");
		//return NULL;
	}
	
	//注册compute_thread
	
	device->compute_thread = spdk_get_thread();
	pthread_mutexattr_t		attr;
	if (pthread_mutexattr_init(&attr)) {
		SPDK_ERRLOG("COMPUTE DEV pthread_mutexattr_init() failed\n");
		return NULL;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		SPDK_ERRLOG("COMPUTE DEV pthread_mutexattr_settype() failed\n");
		pthread_mutexattr_destroy(&attr);
		return NULL;
	}
	int phy_id_begin = 4;
	pthread_mutex_init(&(device->compute_mutex),&attr);
	create_axi_dma_channel(3,&(device->compute),phy_id_begin+1,device->mcdma);
}


static int
nvmf_mcdma_qpair_compare(struct spdk_nvmf_mcdma_qpair *rqpair1, struct spdk_nvmf_mcdma_qpair *rqpair2)
{
	if (rqpair1->ctrlr_num < rqpair2->ctrlr_num)
		return -1;
	else if (rqpair1->ctrlr_num > rqpair2->ctrlr_num)
		return 1;
	else 
		return rqpair1->qp_num < rqpair2->qp_num ? -1 : rqpair1->qp_num > rqpair2->qp_num;
}

static inline uint8_t nvme_qid_to_qdma_qid(struct spdk_nvmf_mcdma_qpair *rqpair, bool is_data)
{
	/* 
	 * QDMA queue ID mapping to x86 host-end NVMe Queues
	 * Doc: https://serve.yuque.com/mora7e/qpqkrm/lt7y1tmp3r0tagfe
	 * Admin queue (QE and Data): 0
	 * IO QE: 1-8
	 * Data QE: 9-16
	 */ 
	SPDK_DEBUGLOG(nvmf, "nvme qid: %u\n", rqpair->qp_num);
	SPDK_DEBUGLOG(nvmf, "qdma data qid: %u\n", rqpair->qp_num + rqpair->ctrlr_num * 32 + 8);
	SPDK_DEBUGLOG(nvmf, "qdma qe qid: %u\n", rqpair->qp_num + rqpair->ctrlr_num * 32);
	SPDK_DEBUGLOG(nvmf, "pf %u\n", rqpair->ctrlr_num);
	if (is_data) {
		return rqpair->qp_num + rqpair->ctrlr_num * 32 + 8; // IO qid starts from 8
	} else {
		return rqpair->qp_num + rqpair->ctrlr_num * 32;
	}
}

static inline uint8_t qdma_qid_to_nvme_qid(uint32_t qid, struct spdk_nvmf_mcdma_qpair *rqpair)
{
	// always qe queue
	SPDK_DEBUGLOG(nvmf, "qdma qe qid: %u\n", qid);
	SPDK_DEBUGLOG(nvmf, "nvme qid: %u\n", qid % 32);
	rqpair->ctrlr_num = qid / 32;
	return qid % 32;
}

RB_GENERATE_STATIC(qpairs_tree, spdk_nvmf_mcdma_qpair, node, nvmf_mcdma_qpair_compare);

static inline enum spdk_nvme_media_error_status_code
nvmf_mcdma_dif_error_to_compl_status(uint8_t err_type) {
	enum spdk_nvme_media_error_status_code result;
	switch (err_type)
	{
	case SPDK_DIF_REFTAG_ERROR:
		result = SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_APPTAG_ERROR:
		result = SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_GUARD_ERROR:
		result = SPDK_NVME_SC_GUARD_CHECK_ERROR;
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return result;
}

static int
nvmf_mcdma_write_pfch_tag(struct spdk_nvmf_mcdma_qpair *rqpair, uint8_t tag)
{
	struct spdk_nvmf_mcdma_device *ndev = rqpair->device;
	uint64_t qid = rqpair->qp_num - 1;
	uint16_t ctrlr_id = rqpair->ctrlr_num;
	int ret;
	
	pthread_mutex_lock(&ndev->pfch_tag_lock);

	ndev->pfch_tag.byte[qid] = tag;

	SPDK_DEBUGLOG(nvmf, "Update prefetch tag of qid %u ctrl %u to %X\n", rqpair->qp_num, ctrlr_id, tag);

	if (qid > 4) {
		ret = spdk_env_write_physmem(PFCH_TAG_MSB_ADDR + ctrlr_id * 0x10000, sizeof(uint32_t), ndev->pfch_tag.dword.msb);
	} else {
		ret = spdk_env_write_physmem(PFCH_TAG_LSB_ADDR + ctrlr_id * 0x10000, sizeof(uint32_t), ndev->pfch_tag.dword.lsb);
	}

	pthread_mutex_unlock(&ndev->pfch_tag_lock);

	return ret;
}

static int
nvmf_mcdma_qpair_update_pfch_tag(struct spdk_nvmf_qpair *qpair, uint8_t tag)
{
	struct spdk_nvmf_mcdma_qpair *rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);
	return nvmf_mcdma_write_pfch_tag(rqpair, tag);
}

static void
nvmf_mcdma_request_free_data(struct spdk_nvmf_mcdma_request *mcdma_req,
			    struct spdk_nvmf_mcdma_transport *rtransport)
{
	mcdma_req->num_outstanding_data_wr = 0;
	
	if (mcdma_req->admin_data_buf) {
		spdk_dma_free(mcdma_req->admin_data_buf);
		mcdma_req->admin_data_buf = NULL;
	}
}

static void
nvmf_mcdma_dump_request(struct spdk_nvmf_mcdma_request *req)
{
	SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", req->req.data_from_pool);
	if (req->req.cmd) {
		SPDK_ERRLOG("\t\tRequest opcode: %d\n", req->req.cmd->nvmf_cmd.opcode);
	}
	// if (req->recv) {
	// 	SPDK_ERRLOG("\t\tRequest recv wr_id%lu\n", req->recv->wr.wr_id);
	// }
}

static void
nvmf_mcdma_dump_qpair_contents(struct spdk_nvmf_mcdma_qpair *rqpair)
{
	int i;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", rqpair->qpair.qid);
	for (i = 0; i < rqpair->max_queue_depth; i++) {
		if (rqpair->resources->reqs[i].state != MCDMA_REQUEST_STATE_FREE) {
			nvmf_mcdma_dump_request(&rqpair->resources->reqs[i]);
		}
	}
}

static void
nvmf_mcdma_resources_destroy(struct spdk_nvmf_mcdma_resources *resources)
{
	spdk_free(resources->cpls);
	spdk_free(resources->bufs);
	spdk_free(resources->reqs);
	free(resources);
}

static inline void print_sqe(void *data)
{
	uint64_t *p = data;
	SPDK_DEBUGLOG(nvmf, "SQE %p\n", p);
	for (int i = 0; i < 4; i++) {
		SPDK_DEBUGLOG(nvmf, "%d %016lX %016lX\n", i, p[2 * i], p[2 * i + 1]);
	}
}

static inline void print_csqe(void *data)
{
	uint64_t *p = data;
	printf("CSQE %p\n", p);
	for (int i = 0; i < 4; i++) {
		printf("%d %016lX %016lX\n", i, p[2 * i], p[2 * i + 1]);
	}
}

static inline void print_cqe(void *data)
{
	uint64_t *p = data;
	SPDK_DEBUGLOG(nvmf, "CQE %p\n", p);
	for (int i = 0; i < 1; i++) {
		SPDK_DEBUGLOG(nvmf, "%d %016lX %016lX\n", i, p[2 * i], p[2 * i + 1]);
	}
}

static void mcdma_qe_recv_cmpl(struct spdk_axi_dma_io *io, int status)
{
	struct byp_io	*byp_io = io->ctx;
	struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;
	struct spdk_nvmf_mcdma_poller *rpoller = mcdma_qp->poller;
	struct spdk_nvmf_mcdma_qpair target;
	target.qp_num = qdma_qid_to_nvme_qid(io->ctrl.tid, &target);
	struct spdk_nvmf_mcdma_qpair *rqpair = qpairs_tree_RB_FIND(&rpoller->qpairs, &target);
	struct spdk_nvmf_mcdma_recv *mcdma_recv;
	if (target.qp_num == 9 && target.ctrlr_num == 1)
		return;
	if (!rqpair) {
		SPDK_ERRLOG("Non-existing qid %u\n", target.qp_num);
		return;
	}
	SPDK_DEBUGLOG(nvmf, "pf %u\n", rqpair->ctrlr_num);

	byp_io->cur_io = io;
	
	

	SPDK_DEBUGLOG(nvmf, "MCDMA recv packet from q %u, paddr %lX\n", rqpair->qp_num, io->iovs[0].paddr);
	print_sqe(io->iovs[0].iov_base);
	// print_sqe(io->iovs[1].iov_base);

	assert(rqpair != NULL);

	mcdma_recv = spdk_simple_pool_get(rqpair->resources->recv_pool);
	if (!mcdma_recv) {
		SPDK_ERRLOG("Failed to get mcdma_recv\n");
		return;
	}

	mcdma_recv->byp_io = byp_io;

	mcdma_recv->receive_tsc = spdk_get_ticks();
	rpoller->stat.requests++;
	STAILQ_INSERT_TAIL(&rqpair->resources->incoming_queue, mcdma_recv, link);

	spdk_axi_dma_io_free(io);
}

static void mcdma_qe_send_cmpl(struct spdk_axi_dma_io *io, int status)
{
	struct spdk_nvmf_mcdma_request	*mcdma_req = io->ctx;
	struct spdk_nvmf_qpair *qpair = mcdma_req->req.qpair;
	struct spdk_nvmf_mcdma_transport	*rtransport = SPDK_CONTAINEROF(qpair->transport,
			struct spdk_nvmf_mcdma_transport, transport);
	struct spdk_nvmf_mcdma_qpair *rqpair;
	
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);

	mcdma_req->state = MCDMA_REQUEST_STATE_COMPLETED;

	rqpair->current_send_depth -= mcdma_req->num_outstanding_data_wr + 1;
	mcdma_req->num_outstanding_data_wr = 0;

	nvmf_mcdma_request_process(rtransport, mcdma_req);

	// TODO: Send waiting requests
	
	spdk_axi_dma_io_free(io);
}

static struct spdk_nvmf_mcdma_resources *
nvmf_mcdma_resources_create(struct spdk_nvmf_mcdma_resource_opts *opts)
{
	struct spdk_nvmf_mcdma_resources	*resources;
	struct spdk_nvmf_mcdma_request	*mcdma_req;
	uint32_t			i;
	int				rc = 0;
	char pool_name[15];
	//
	resources = calloc(1, sizeof(struct spdk_nvmf_mcdma_resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
		return NULL;
	}

	sprintf(pool_name, "RecvPoolq%uc%u", opts->qpair->qp_num, opts->qpair->ctrlr_num);

	resources->reqs = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->reqs),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->recv_pool = calloc(1, sizeof(struct spdk_simple_pool));
	spdk_simple_pool_init(resources->recv_pool, opts->max_queue_depth, sizeof(struct spdk_nvmf_mcdma_recv));
	
	resources->cpls = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	resources->bufs = spdk_zmalloc(opts->max_queue_depth * opts->in_capsule_data_size,
					       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
	resources->cpl_data_bufs = spdk_zmalloc(opts->max_queue_depth * opts->in_capsule_data_size,
					       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
		if (!resources->reqs || !resources->recv_pool ||
			!resources->cpls || (opts->in_capsule_data_size && !resources->bufs)) {
			SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
			goto cleanup;
		}

	SPDK_DEBUGLOG(nvmf, "Command Array Length: %u\n", opts->max_queue_depth);
	SPDK_DEBUGLOG(nvmf, "Completion Array: %p Length: %lx\n",
		      resources->cpls, opts->max_queue_depth * sizeof(*resources->cpls));
	if (resources->bufs) {
		SPDK_DEBUGLOG(nvmf, "In Capsule Data Array: %p Length: %x\n",
			      resources->bufs, opts->max_queue_depth *
			      opts->in_capsule_data_size);
	}
	/* Initialize queues */
	STAILQ_INIT(&resources->incoming_queue);
	STAILQ_INIT(&resources->free_queue);

	for (i = 0; i < opts->max_queue_depth; i++) {
		mcdma_req = &resources->reqs[i];

		if (opts->qpair != NULL) {
			mcdma_req->qpair = opts->qpair;
			mcdma_req->req.qpair = &opts->qpair->qpair;
		} else {
			mcdma_req->qpair = NULL;
			mcdma_req->req.qpair = NULL;
		}
		mcdma_req->req.cmd = NULL;
		mcdma_req->req.iovcnt = 0;
		mcdma_req->req.stripped_data = NULL;

		mcdma_req->sgl_buf = spdk_zmalloc(PAGE_SIZE, PAGE_SIZE, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		if (!mcdma_req->sgl_buf) {
			SPDK_ERRLOG("Failed to allocate request sgl buffer");
		}

		mcdma_req->data_buf = spdk_zmalloc(PAGE_SIZE, PAGE_SIZE, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		if (!mcdma_req->data_buf) {
			SPDK_ERRLOG("Failed to allocate request sgl buffer");
		}

		/* Set up memory to send responses */
		mcdma_req->req.rsp = &resources->cpls[i].msg;

		if (opts->qpair && opts->qpair->qpair.qid == 0) {
			mcdma_req->rsp.iovs = calloc(2, sizeof(struct spdk_axi_dma_iovec));

			mcdma_req->rsp.iovs[0].iov_base = &resources->cpls[i];
			mcdma_req->rsp.iovs[0].iov_len = sizeof(resources->cpls[i]);
			mcdma_req->rsp.iovs[1].iov_base = (void *)((uintptr_t)resources->cpl_data_bufs +
													(i * opts->in_capsule_data_size));
			mcdma_req->rsp.iovs[1].iov_len = opts->in_capsule_data_size;
			
			mcdma_req->rsp.iovcnt = 2;
		} else {
			mcdma_req->rsp.iovs = calloc(1, sizeof(struct spdk_axi_dma_iovec));

			mcdma_req->rsp.iovs[0].iov_base = &resources->cpls[i].msg;
			mcdma_req->rsp.iovs[0].iov_len = sizeof(resources->cpls[i].msg);
		

			mcdma_req->rsp.iovcnt = 1;

		}
		

		/* Initialize request state to FREE */
		mcdma_req->state = MCDMA_REQUEST_STATE_FREE;
		STAILQ_INSERT_TAIL(&resources->free_queue, mcdma_req, state_link);
	}



	if (rc) {
		goto cleanup;
	}

	return resources;

cleanup:
	nvmf_mcdma_resources_destroy(resources);
	return NULL;
}

static void
nvmf_mcdma_qpair_destroy(struct spdk_nvmf_mcdma_qpair *rqpair)
{

	if (rqpair->qd != 0) {
		struct spdk_nvmf_qpair *qpair = &rqpair->qpair;
		struct spdk_nvmf_mcdma_transport	*rtransport = SPDK_CONTAINEROF(qpair->transport,
				struct spdk_nvmf_mcdma_transport, transport);
		struct spdk_nvmf_mcdma_request *req;
		uint32_t i, max_req_count = 0;

		SPDK_WARNLOG("Destroying qpair when queue depth is %d\n", rqpair->qd);

		// if (rqpair->srq == NULL) {
			nvmf_mcdma_dump_qpair_contents(rqpair);
			max_req_count = rqpair->max_queue_depth;
		// } else if (rqpair->poller && rqpair->resources) {
		// 	max_req_count = rqpair->poller->max_srq_depth;
		// }

		SPDK_DEBUGLOG(nvmf, "Release incomplete requests\n");
		for (i = 0; i < max_req_count; i++) {
			req = &rqpair->resources->reqs[i];
			if (req->req.qpair == qpair && req->state != MCDMA_REQUEST_STATE_FREE) {
				/* nvmf_mcdma_request_process checks qpair ibv and internal state
				 * and completes a request */
				nvmf_mcdma_request_process(rtransport, req);
			}
		}
		assert(rqpair->qd == 0);
	}


	if (rqpair->poller) {
	
		RB_REMOVE(qpairs_tree, &rqpair->poller->qpairs, rqpair);

	
	}



		if (rqpair->poller != NULL) {
			rqpair->poller->required_num_wr -= rqpair->max_queue_depth;
		}


	if (rqpair->resources != NULL) {
		nvmf_mcdma_resources_destroy(rqpair->resources);
	}

	// nvmf_mcdma_qpair_clean_ibv_events(rqpair);

	if (rqpair->destruct_channel) {
		spdk_put_io_channel(rqpair->destruct_channel);
		rqpair->destruct_channel = NULL;
	}

	free(rqpair);
}

// static int
// nvmf_mcdma_resize_cq(struct spdk_nvmf_mcdma_qpair *rqpair, struct spdk_nvmf_mcdma_device *device)
// {
// 	struct spdk_nvmf_mcdma_poller	*rpoller;
// 	int				rc, num_cqe, required_num_wr;

// 	/* Enlarge CQ size dynamically */
// 	rpoller = rqpair->poller;
// 	required_num_wr = rpoller->required_num_wr + MAX_WR_PER_QP(rqpair->max_queue_depth);
// 	num_cqe = rpoller->num_cqe;
// 	if (num_cqe < required_num_wr) {
// 		num_cqe = spdk_max(num_cqe * 2, required_num_wr);
// 		num_cqe = spdk_min(num_cqe, device->attr.max_cqe);
// 	}

// 	if (rpoller->num_cqe != num_cqe) {
// 		if (device->context->device->transport_type == IBV_TRANSPORT_IWARP) {
// 			SPDK_ERRLOG("iWARP doesn't support CQ resize. Current capacity %u, required %u\n"
// 				    "Using CQ of insufficient size may lead to CQ overrun\n", rpoller->num_cqe, num_cqe);
// 			return -1;
// 		}
// 		if (required_num_wr > device->attr.max_cqe) {
// 			SPDK_ERRLOG("RDMA CQE requirement (%d) exceeds device max_cqe limitation (%d)\n",
// 				    required_num_wr, device->attr.max_cqe);
// 			return -1;
// 		}

// 		SPDK_DEBUGLOG(nvmf, "Resize RDMA CQ from %d to %d\n", rpoller->num_cqe, num_cqe);
// 		rc = ibv_resize_cq(rpoller->cq, num_cqe);
// 		if (rc) {
// 			SPDK_ERRLOG("RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
// 			return -1;
// 		}

// 		rpoller->num_cqe = num_cqe;
// 	}

// 	rpoller->required_num_wr = required_num_wr;
// 	return 0;
// }

struct spdk_mcdma_qp_init_attr {
	struct spdk_axi_dma_dev *mcdma_dev;
	struct spdk_hlsacccompute_dev *compute_dev;
	uint32_t max_queue_depth;
	uint32_t qid;
};

static void desc_byp_in_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	struct qdma_h2c_byp_in *byp_in = (struct qdma_h2c_byp_in *)io->iovs[0].iov_base;
	SPDK_DEBUGLOG(nvmf, "Got %s byp: Addr %lX len %d port_id %u qid %u cidx %u cur_cidx %u\n",
				  io->ctrl.tid == MCDMA_QDMA_H2C_BYP_OUT_RX_TID ? "H2C" : "C2H", byp_in->dsc.addr,
				  byp_in->dsc.len, byp_in->port_id, byp_in->qid, byp_in->cidx, byp_in->cur_cidx_lsb + (byp_in->cur_cidx_msb << 8));
	//
	io->iovs->iov_len = MCDMA_RX_BUF_SZ;
	spdk_axi_dma_rx_channel_recv(io->ch, io->iovs, io->iovcnt, mcdma_qp_rx_cmpl_cb, io->ctx);

	spdk_axi_dma_io_free(io);
}

static int mcdma_resubmit_byp_io(struct spdk_mcdma_qp *mcdma_qp, struct byp_io *byp_io)
{
	//
	byp_io->iov.iov_len = MCDMA_RX_BUF_SZ;
	return spdk_axi_dma_rx_channel_recv(mcdma_qp->rx_ch, &byp_io->iov, 1, mcdma_qp_rx_cmpl_cb, byp_io);
}

static void mcdma_pass_rx_data_cmpl(struct spdk_axi_dma_io *io, int status)
{
  struct byp_io *byp_io = io->ctx;
	// struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;
	
  SPDK_DEBUGLOG(nvmf, "Send r_data cmpl for arid %u len %d\n", io->ctrl.tuser, byp_io->iov.iov_len);

	byp_io->iov.iov_len = MCDMA_RX_BUF_SZ;
	spdk_axi_dma_rx_channel_recv(io->ch, io->iovs, io->iovcnt, mcdma_qp_rx_cmpl_cb, io->ctx);

  spdk_axi_dma_io_free(io);
}

static int
mcdma_pass_rx_data(struct spdk_mcdma_qp *mcdma_qp, struct byp_io *byp_io, uint32_t arid, uint8_t tid, bool is_r)
{
	struct spdk_axi_dma_ctrl ctrl;
	
	ctrl.tdest = ctrl.tid = tid;
	ctrl.tuser = arid;

	SPDK_DEBUGLOG(nvmf, "Send %c_data: arid %u len %lu\n", is_r ? 'r' : 'w', arid, byp_io->iov.iov_len);

	return spdk_axi_dma_tx_channel_send(mcdma_qp->tx_ch, &byp_io->iov, 1, mcdma_pass_rx_data_cmpl, byp_io, &ctrl);
}

static void rx_data_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	struct byp_io *byp_io = io->ctx;
	struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;
	int rc;
	
	SPDK_DEBUGLOG(nvmf, "Got r_data: arid %u\n", io->ctrl.tuser);
	print_cqe(io->iovs[0].iov_base);

	// spdk_axi_dma_rx_channel_recv(io->ch, io->iovs, io->iovcnt, ar_req_in_cmpl_cb, NULL);

	byp_io->iov.iov_len = io->transfered_length;

	rc = mcdma_pass_rx_data(mcdma_qp, byp_io, io->ctrl.tuser, MCDMA_R_BRIDGE_TX_TID, true);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to send r data: %d\n", rc);
	}

	spdk_axi_dma_io_free(io);
}

static void ar_req_in_cmpl_cb(struct spdk_axi_dma_io *io, int status);

static void ar_req_send_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	struct byp_io *byp_io = io->ctx;
	// struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;

	SPDK_DEBUGLOG(nvmf, "ar_req send cmpl for %u\n", io->ctrl.tid);

	byp_io->iov.iov_len = MCDMA_RX_BUF_SZ;
	spdk_axi_dma_rx_channel_recv(io->ch, io->iovs, io->iovcnt, mcdma_qp_rx_cmpl_cb, io->ctx);

	spdk_axi_dma_io_free(io);
}

static int ar_req_send(struct spdk_mcdma_qp *mcdma_qp, struct byp_io *byp_io, bool is_ar)
{
	struct qdma_ar_req *ar_req = byp_io->buf;
	struct spdk_axi_dma_ctrl ctrl;

	SPDK_DEBUGLOG(nvmf, "Send a%c_req: addr %lX cid %u qid %u arid %u burst_size %u burst_len %u\n",
			is_ar ? 'r' : 'w', ar_req->addr, ar_req->cid, ar_req->qid, ar_req->arid, ar_req->burst_size, ar_req->burst_len);

	ctrl.tid = is_ar ? MCDMA_AR_BRIDGE_TX_TID : MCDMA_AW_BRIDGE_TX_TID;

	return spdk_axi_dma_tx_channel_send(mcdma_qp->tx_ch, &byp_io->iov, 1, ar_req_send_cmpl_cb, byp_io, &ctrl);
}

static void ar_req_in_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	struct byp_io *byp_io = io->ctx;
	struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;
	struct qdma_ar_req *ar_req = (struct qdma_ar_req *)io->iovs[0].iov_base;
	int rc;

	print_cqe(io->iovs[0].iov_base);

	SPDK_DEBUGLOG(nvmf, "Got ar_req: addr %lX cid %u qid %u arid %u burst_size %u burst_len %u\n",
			ar_req->addr, ar_req->cid, ar_req->qid, ar_req->arid, ar_req->burst_size, ar_req->burst_len);

	// rc = mcdma_send_dummy_r_data(device, ar_req);
	// if (spdk_unlikely(rc)) {
	// 	SPDK_ERRLOG("Failed to send r data: %d\n", rc);
	// } 

	rc = ar_req_send(mcdma_qp, byp_io, true);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to send QDMA ar_req: %d\n", rc);
	} 

	spdk_axi_dma_io_free(io);
}

static void aw_req_in_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	struct byp_io *byp_io = io->ctx;
	struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;
	struct qdma_ar_req *ar_req = (struct qdma_ar_req *)io->iovs[0].iov_base;
	int rc;

	print_cqe(io->iovs[0].iov_base);

	SPDK_INFOLOG(nvmq, "Got aw_req: addr %lX cid %u qid %u arid %u burst_size %u burst_len %u\n",
			ar_req->addr, ar_req->cid, ar_req->qid, ar_req->arid, ar_req->burst_size, ar_req->burst_len);

	rc = ar_req_send(mcdma_qp, byp_io, false);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to send QDMA aw_req: %d\n", rc);
	} 

	spdk_axi_dma_io_free(io);
}

static void crdt_in_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	struct byp_io *byp_io = io->ctx;
	struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;
	struct qdma_crdt *crdt = (struct qdma_crdt *)io->iovs[0].iov_base;
	int rc;

	print_cqe(io->iovs[0].iov_base);

	SPDK_INFOLOG(nvmq, "Got crdt: qid %u avl %u pidx %u port_id %u byp %u dir %u mm %u qinv %u qen %u irq_arm %u error %u\n",
			crdt->qid, crdt->avl, crdt->pidx, crdt->port_id, crdt->byp, 
			crdt->dir, crdt->mm, crdt->qinv, crdt->qen, crdt->irq_arm, crdt->error);

	spdk_axi_dma_io_free(io);
}

static void w_in_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	struct byp_io *byp_io = io->ctx;
	struct spdk_mcdma_qp *mcdma_qp = (struct spdk_mcdma_qp *)byp_io->req;
	int rc;

	SPDK_INFOLOG(nvmq, "Got w data len %u\n", io->transfered_length);
	print_cqe(io->iovs[0].iov_base);

	// spdk_axi_dma_rx_channel_recv(io->ch, io->iovs, io->iovcnt, ar_req_in_cmpl_cb, NULL);

	byp_io->iov.iov_len = io->transfered_length;

	rc = mcdma_pass_rx_data(mcdma_qp, byp_io, io->ctrl.tuser, io->ctrl.tid, false);
	if (spdk_unlikely(rc)) {
		SPDK_ERRLOG("Failed to send w data: %d\n", rc);
	}

	spdk_axi_dma_io_free(io);
}

static void mcdma_qp_rx_cmpl_cb(struct spdk_axi_dma_io *io, int status)
{
	uint8_t tid = io->ctrl.tid;

	// if (spdk_unlikely(tid == MCDMA_QDMA_H2C_BYP_OUT_RX_TID)) {
	// 	desc_byp_in_cmpl_cb(io, status);
	// } else if (spdk_unlikely(tid == MCDMA_QDMA_C2H_BYP_OUT_RX_TID)) {
	// 	desc_byp_in_cmpl_cb(io, status);
	// } else if (spdk_unlikely(tid == MCDMA_AR_BRIDGE_RX_TID)) {
	// 	ar_req_in_cmpl_cb(io, status);
	// } else if (spdk_unlikely(tid == MCDMA_R_BRIDGE_RX_TID)) {
	// 	rx_data_cmpl_cb(io, status);
	// } else if (spdk_unlikely(tid >= MCDMA_W_BRIDGE_RX_MIN_TID && tid <= MCDMA_W_BRIDGE_RX_MAX_TID)) {
	// 	w_in_cmpl_cb(io, status);
	// } else if (spdk_unlikely(tid == MCDMA_AW_BRIDGE_RX_TID)) {
	// 	aw_req_in_cmpl_cb(io, status);
	// } else 
	if (spdk_unlikely(tid == MCDMA_QDMA_CRDT_RX_TID)) {
		crdt_in_cmpl_cb(io, status);
	} else
	mcdma_qe_recv_cmpl(io, status);
}

//static int compute_cq_poller_fn(void *arg) {
//    struct spdk_compute_qpair *qpair = arg;
//    return spdk_compute_poll_cq(qpair);
//}

static int
mcdma_qpair_create(struct spdk_mcdma_qp *mcdma_qp, struct spdk_mcdma_qp_init_attr *attr)
{
	//uint8_t tid = nvme_qid_to_qdma_qid(attr->qid, false);
	uint8_t tid = attr->qid;
	uint32_t max_queue_depth = 256;
	//
	mcdma_qp->chid = attr->qid;

	SPDK_INFOLOG(nvmf, "Create QP %u on core %u\n", mcdma_qp->chid, spdk_env_get_current_core());

	mcdma_qp->tx_ch = spdk_axi_dma_create_tx_channel(attr->mcdma_dev, max_queue_depth, mcdma_qp->chid, tid);
	if (!mcdma_qp->tx_ch) {
		SPDK_ERRLOG("Failed to create mcdma TX ch for q %u\n", mcdma_qp->chid);
		return -1;
	}

	mcdma_qp->rx_ch = spdk_axi_dma_create_rx_channel(attr->mcdma_dev, max_queue_depth, mcdma_qp->chid, tid);
	if (!mcdma_qp->rx_ch) {
		SPDK_ERRLOG("Failed to create mcdma RX ch for q %u\n", mcdma_qp->chid);
		return -1;
	}

	mcdma_qp->rx_ios = calloc(max_queue_depth, sizeof(struct byp_io));
	for (uint32_t i = 0; i < max_queue_depth; i++) {
		struct byp_io *byp_io = &mcdma_qp->rx_ios[i];
		byp_io->buf = spdk_dma_zmalloc(MCDMA_RX_BUF_SZ, 4096, NULL);
		if (!byp_io->buf) {
			SPDK_ERRLOG("Failed to allocate desc byp input buffer\n");
			return -ENOMEM;
		}
		byp_io->iov.iov_base = byp_io->buf;
		byp_io->iov.iov_len = MCDMA_RX_BUF_SZ;
		byp_io->req = mcdma_qp;
		if (spdk_axi_dma_rx_channel_recv(mcdma_qp->rx_ch, &byp_io->iov, 1, mcdma_qp_rx_cmpl_cb, byp_io) != 0) {
			SPDK_ERRLOG("Failed to submit bypass rx io\n");
		}
	}
	/*
	if (spdk_compute_qpair_create(attr->compute_dev, &mcdma_qp->compute_ch, attr->qid) != 0) {
		SPDK_ERRLOG("Failed to create compute qpair\n");
	}

	mcdma_qp->compute_poller = spdk_poller_register(compute_cq_poller_fn, &mcdma_qp->compute_ch, 0);
	if (!mcdma_qp->compute_poller) {
		SPDK_ERRLOG("Failed to register compute poller\n");
	}
	*/
	return 0;
}

static int
nvmf_mcdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_mcdma_qpair		*rqpair;
	struct spdk_nvmf_mcdma_transport		*rtransport;
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_mcdma_resource_opts	opts;
	// struct spdk_nvmf_mcdma_device		*device;
	struct spdk_mcdma_qp_init_attr		qp_init_attr = {};
	//
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);
	// device = rqpair->device;

	// qp_init_attr.qp_context	= rqpair;
	// qp_init_attr.pd		= device->pd;
	// qp_init_attr.send_cq	= rqpair->poller->cq;
	// qp_init_attr.recv_cq	= rqpair->poller->cq;

	// if (rqpair->srq) {
	// 	qp_init_attr.srq		= rqpair->srq->srq;
	// } else {
	// 	qp_init_attr.cap.max_recv_wr	= rqpair->max_queue_depth;
	// }

	/* SEND, READ, and WRITE operations */
	qp_init_attr.max_queue_depth	= (uint32_t)rqpair->max_queue_depth * 2;
	// qp_init_attr.cap.max_send_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_TX_SGE);
	// qp_init_attr.cap.max_recv_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
	// qp_init_attr.stats		= &rqpair->poller->stat.qp_stats;

	// if (rqpair->srq == NULL && nvmf_mcdma_resize_cq(rqpair, device) < 0) {
	// 	SPDK_ERRLOG("Failed to resize the completion queue. Cannot initialize qpair.\n");
	// 	goto error;
	// }

	rqpair->qp_num = rqpair->qpair.qid;
	SPDK_DEBUGLOG(nvmf, "qpair qid = %u, ctrlr id = %u\n", rqpair->qp_num, rqpair->ctrlr_num);

	rqpair->max_send_depth = spdk_min((uint32_t)(rqpair->max_queue_depth * 2),
					  qp_init_attr.max_queue_depth);
	// rqpair->max_send_sge = spdk_min(NVMF_DEFAULT_TX_SGE, qp_init_attr.cap.max_send_sge);
	// rqpair->max_recv_sge = spdk_min(NVMF_DEFAULT_RX_SGE, qp_init_attr.cap.max_recv_sge);
	// spdk_trace_record(TRACE_MCDMA_QP_CREATE, 0, 0, (uintptr_t)rqpair);
	SPDK_DEBUGLOG(nvmf, "New RDMA Connection: %p\n", qpair);

	// if (rqpair->poller->srq == NULL) {
		rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_mcdma_transport, transport);
		transport = &rtransport->transport;

		opts.qp = rqpair->mcdma_qp;
		// opts.pd = rqpair->cm_id->pd;
		opts.qpair = rqpair;
		// opts.shared = false;
		opts.max_queue_depth = rqpair->max_queue_depth;
		opts.in_capsule_data_size = transport->opts.in_capsule_data_size;

		SPDK_INFOLOG(nvmf, "Create resources %u\n", rqpair->qp_num);
		rqpair->resources = nvmf_mcdma_resources_create(&opts);

		if (!rqpair->resources) {
			SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
			// mcdma_destroy_qp(rqpair->cm_id);
			goto error;
		}
	// } else {
	// 	rqpair->resources = rqpair->poller->resources;
	// }

	STAILQ_INIT(&rqpair->pending_mcdma_read_queue);
	STAILQ_INIT(&rqpair->pending_mcdma_write_queue);

	

	return 0;

error:
	// mcdma_destroy_id(rqpair->cm_id);
	// rqpair->cm_id = NULL;
	return -1;
}

static inline bool nvmf_is_io_request_with_sgl(struct spdk_nvmf_request *req)
{
	return !nvmf_qpair_is_admin_queue(req->qpair) &&
		   req->cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC &&
		   !spdk_nvme_opc_is_kernel(req->cmd->nvme_cmd.opc) &&
		   req->cmd->nvme_cmd.dptr.prp.prp1 > 0;
}

static void
nvmf_mcdma_recv_free(struct spdk_nvmf_mcdma_qpair *rqpair,
					 struct spdk_nvmf_mcdma_recv *recv)
{
	spdk_simple_pool_put(rqpair->resources->recv_pool, recv);

	mcdma_resubmit_byp_io(rqpair->mcdma_qp, recv->byp_io);
}

static void
nvmf_mcdma_qpair_process_pending(struct spdk_nvmf_mcdma_transport *rtransport,
				struct spdk_nvmf_mcdma_qpair *rqpair, bool drain)
{
	// struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_mcdma_request	*mcdma_req;
	struct spdk_nvmf_mcdma_resources *resources = rqpair->resources;
	bool is_kernel;
	bool is_new;

	

	while (!STAILQ_EMPTY(&resources->free_queue) && !STAILQ_EMPTY(&resources->incoming_queue)) {
		if (resources->cur_req && resources->cur_req->state == MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER) {
			mcdma_req = resources->cur_req;
			is_kernel = spdk_nvme_opc_is_kernel(mcdma_req->req.cmd->nvme_cmd.opc);

			//Receives that are waiting for a request object
			struct spdk_nvmf_mcdma_recv *data_recv = STAILQ_FIRST(&resources->incoming_queue);
			struct byp_io *byp_io = data_recv->byp_io;
			bool should_stop;

			if (is_kernel) {
				struct spdk_nvme_kernel *kernel = mcdma_req->req.kernel;
				struct spdk_nvme_cmd *cmd = byp_io->iov.iov_base;
				//SPDK_DEBUGLOG(nvmf,"KERNEL CMD GET,PENDING CID%u\n",cmd->cid);

				kernel->cmds[kernel->num_cmds++] = cmd;

				should_stop = cmd->opc == SPDK_NVME_OPC_KERNEL_END;

				SPDK_DEBUGLOG(nvmf, "Append one command, current len %u\n", kernel->num_cmds);
				if(should_stop){
					SPDK_DEBUGLOG(nvmf,"NeedToStop\n");
				}
				STAILQ_REMOVE_HEAD(&resources->incoming_queue, link);
				STAILQ_INSERT_TAIL(&mcdma_req->recv_queue, data_recv, link);
			} else {
				void *dst = mcdma_req->req.data + mcdma_req->req.length;
				void *src = byp_io->iov.iov_base;
				size_t len = byp_io->cur_io->transfered_length;

				memcpy(dst, src, len);
				mcdma_req->req.length += len;
				mcdma_req->req.data_from_pool = false;

				should_stop = byp_io->cur_io->status.rxeof;

				SPDK_DEBUGLOG(nvmf, "Append req data, current len %u\n", mcdma_req->req.length);

				STAILQ_REMOVE_HEAD(&resources->incoming_queue, link);
				nvmf_mcdma_recv_free(rqpair, data_recv);
			}

			if (should_stop) {
				if (nvmf_is_io_request_with_sgl(&mcdma_req->req)) {
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
				} else {
					mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_EXECUTE;
				}
			}
			is_new = false;
		} else {
			mcdma_req = STAILQ_FIRST(&resources->free_queue);
			STAILQ_REMOVE_HEAD(&resources->free_queue, state_link);
			mcdma_req->recv = STAILQ_FIRST(&resources->incoming_queue);
			STAILQ_REMOVE_HEAD(&resources->incoming_queue, link);
			is_new = true;
		}

		resources->cur_req = mcdma_req;

		rqpair->qd++;

		if (is_new) {
			mcdma_req->receive_tsc = mcdma_req->recv->receive_tsc;
			mcdma_req->state = MCDMA_REQUEST_STATE_NEW;
		}
		if (nvmf_mcdma_request_process(rtransport, mcdma_req) == false) {
			break;
		}
	}
	if (!STAILQ_EMPTY(&resources->incoming_queue) && STAILQ_EMPTY(&resources->free_queue)) {
		rqpair->poller->stat.pending_free_request++;
	}
}

static int
request_transfer_out(struct spdk_nvmf_request *req, int *data_posted)
{
	int				num_outstanding_data_wr = 0;
	struct spdk_nvmf_mcdma_request	*mcdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_mcdma_qpair	*rqpair;
	struct spdk_nvme_cpl		*rsp;
	struct spdk_axi_dma_ctrl ctrl;
	int ret;
	// struct ibv_send_wr		*first = NULL;
	// struct spdk_nvmf_mcdma_transport *rtransport;
	
	*data_posted = 0;
	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	mcdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_mcdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);
	// rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
	// 			      struct spdk_nvmf_mcdma_transport, transport);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else if (spdk_nvme_opc_is_kernel(req->cmd->nvme_cmd.opc)) {
		qpair->sq_head += req->kernel->num_cmds;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	/* queue the capsule for the recv buffer */
	assert(mcdma_req->recv != NULL);

	print_cqe(mcdma_req->rsp.iovs[0].iov_base);
	ctrl.tid = nvme_qid_to_qdma_qid(rqpair, false);
	//ctrl.tuser = 0;
	ret = spdk_axi_dma_tx_channel_send(rqpair->mcdma_qp->tx_ch, mcdma_req->rsp.iovs, mcdma_req->rsp.iovcnt, mcdma_qe_send_cmpl, mcdma_req, &ctrl);
	if (ret) {
		SPDK_ERRLOG("Failed to submit response: %d\n", ret);
	}
	// 	STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	// }
	// if (rtransport->mcdma_opts.no_wr_batching) {
		// _poller_submit_sends(rtransport, rqpair->poller);
	// }

	/* +1 for the rsp wr */
	rqpair->current_send_depth += num_outstanding_data_wr + 1;

	return 0;
}

// static int
// nvmf_mcdma_event_accept(struct mcdma_cm_id *id, struct spdk_nvmf_mcdma_qpair *rqpair)
// {
// 	struct spdk_nvmf_mcdma_accept_private_data	accept_data;
// 	struct mcdma_conn_param				ctrlr_event_data = {};
// 	int						rc;

// 	accept_data.recfmt = 0;
// 	accept_data.crqsize = rqpair->max_queue_depth;

// 	ctrlr_event_data.private_data = &accept_data;
// 	ctrlr_event_data.private_data_len = sizeof(accept_data);
// 	if (id->ps == RDMA_PS_TCP) {
// 		ctrlr_event_data.responder_resources = 0; /* We accept 0 reads from the host */
// 		ctrlr_event_data.initiator_depth = rqpair->max_read_depth;
// 	}

// 	/* Configure infinite retries for the initiator side qpair.
// 	 * We need to pass this value to the initiator to prevent the
// 	 * initiator side NIC from completing SEND requests back to the
// 	 * initiator with status rnr_retry_count_exceeded. */
// 	ctrlr_event_data.rnr_retry_count = 0x7;

// 	/* When qpair is created without use of mcdma cm API, an additional
// 	 * information must be provided to initiator in the connection response:
// 	 * whether qpair is using SRQ and its qp_num
// 	 * Fields below are ignored by mcdma cm if qpair has been
// 	 * created using mcdma cm API. */
// 	ctrlr_event_data.srq = rqpair->srq ? 1 : 0;
// 	ctrlr_event_data.qp_num = rqpair->qp_num;

// 	rc = spdk_mcdma_qp_accept(rqpair->mcdma_qp, &ctrlr_event_data);
// 	if (rc) {
// 		SPDK_ERRLOG("Error %d on spdk_mcdma_qp_accept\n", errno);
// 	} else {
// 		SPDK_DEBUGLOG(nvmf, "Sent back the accept\n");
// 	}

// 	return rc;
// }

// static void
// nvmf_mcdma_event_reject(struct mcdma_cm_id *id, enum spdk_nvmf_mcdma_transport_error error)
// {
// 	struct spdk_nvmf_mcdma_reject_private_data	rej_data;

// 	rej_data.recfmt = 0;
// 	rej_data.sts = error;

// 	mcdma_reject(id, &rej_data, sizeof(rej_data));
// }

static int
nvmf_mcdma_add_rqpair(struct spdk_nvmf_transport *transport, struct spdk_nvmf_mcdma_device *mdev, uint16_t max_queue_depth, uint16_t qid, uint16_t ctrlr_id)
{
	struct spdk_nvmf_mcdma_qpair	*rqpair;
	// struct spdk_nvmf_mcdma_transport *rtransport;

	// rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_mcdma_transport, transport);
	//
	rqpair = calloc(1, sizeof(struct spdk_nvmf_mcdma_qpair));
	if (rqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		return -1;
	}

	rqpair->device = mdev;
	rqpair->max_queue_depth = max_queue_depth;
	// rqpair->max_read_depth = max_read_depth;
	// rqpair->cm_id = event->id;
	// rqpair->listen_id = event->listen_id;
	rqpair->qpair.transport = transport;
	// STAILQ_INIT(&rqpair->ibv_events);
	/* use qid from the private data to determine the qpair type
	   qid will be set to the appropriate value when the controller is created */
	rqpair->qpair.qid = qid;
	rqpair->ctrlr_num = ctrlr_id;

	// event->id->context = &rqpair->qpair;

	spdk_nvmf_tgt_new_qpair(transport->tgt, &rqpair->qpair);

	return 0;
}

static void
_nvmf_mcdma_request_free(struct spdk_nvmf_mcdma_request *mcdma_req,
			struct spdk_nvmf_mcdma_transport	*rtransport)
{
	struct spdk_nvmf_mcdma_qpair		*rqpair;
	struct spdk_nvmf_mcdma_recv	*recv;

	rqpair = SPDK_CONTAINEROF(mcdma_req->req.qpair, struct spdk_nvmf_mcdma_qpair, qpair);
	// if (mcdma_req->req.data_from_pool) {
	// 	rgroup = rqpair->poller->group;

	// 	spdk_nvmf_request_free_buffers(&mcdma_req->req, &rgroup->group, &rtransport->transport);
	// }
	if (mcdma_req->req.stripped_data) {
		nvmf_request_free_stripped_buffers(&mcdma_req->req,
						   &rqpair->poller->group->group,
						   &rtransport->transport);
	}
	nvmf_mcdma_request_free_data(mcdma_req, rtransport);
	mcdma_req->req.length = 0;
	mcdma_req->req.iovcnt = 0;
	mcdma_req->req.data = NULL;
	// mcdma_req->rsp.wr.next = NULL;
	// mcdma_req->data.wr.next = NULL;
	mcdma_req->offset = 0;
	mcdma_req->req.dif_enabled = false;
	mcdma_req->fused_failed = false;
	if (mcdma_req->fused_pair) {
		/* This req was part of a valid fused pair, but failed before it got to
		 * READ_TO_EXECUTE state.  This means we need to fail the other request
		 * in the pair, because it is no longer part of a valid pair.  If the pair
		 * already reached READY_TO_EXECUTE state, we need to kick it.
		 */
		mcdma_req->fused_pair->fused_failed = true;
		if (mcdma_req->fused_pair->state == MCDMA_REQUEST_STATE_READY_TO_EXECUTE) {
			nvmf_mcdma_request_process(rtransport, mcdma_req->fused_pair);
		}
		mcdma_req->fused_pair = NULL;
	}
	memset(&mcdma_req->req.dif, 0, sizeof(mcdma_req->req.dif));
	rqpair->qd--;

	nvmf_mcdma_recv_free(rqpair, mcdma_req->recv);

//I don not know whether it is really work...
	if (mcdma_req->req.kernel) {

		//debug
		struct spdk_nvmf_mcdma_device* mdev = mcdma_req->qpair->device;
		// mdev->all_kernel_executed_time += spdk_get_ticks() -  mdev->one_kernel_start_execute_time;
		mdev->kernel_num++;
		//if (mdev->kernel_num % 1000 == 0) {
		//	printf("%lu kernel executed time: %luus\n", mdev->kernel_num, mdev->all_kernel_executed_time * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
		//}
		/*
		mdev->all_kernel_executed_time_buf[mdev->kernel_num] = mdev->all_kernel_executed_time; 
		mdev->kernel_num++;
		if(spdk_unlikely(mdev->kernel_num > 999)){
			for(int i=0;i<mdev->kernel_num;i++){
				SPDK_DEBUGLOG(nvmf,"Finish Execute One Kernel, Execute Time%lf s, Kernel Num %ld\n",(double)mdev->all_kernel_executed_time_buf[i]/spdk_get_ticks_hz(),i);
			}
			for(int i=0;i<mdev->command_time_buf_index;i++){
				SPDK_DEBUGLOG(nvmf,"Finish Execute One Command, Execute Time%lf s,index i%ld\n",(double)mdev->all_command_executed_time_buf[i]/spdk_get_ticks_hz(),i);
			}
		}*/
		struct spdk_nvmf_kernel_mem* mem_entry;
		for(int i=0;i<mcdma_req->req.kernel->num_phys_mem;i++){
			mem_entry = mcdma_req->req.kernel->mem_buf[i];
			mem_entry->databuf_first_page_paddr = NULL;
			for(int i = 0;i<NVMF_MAX_PRP_LIST_ENTRIES+1;i++){
				if(spdk_unlikely(mem_entry->databuf_vaddr_vector[i]==NULL)){
					break;
				} else {
					spdk_simple_pool_put(mcdma_req->req.qpair->kernel_inner_buf_pool,mem_entry->databuf_vaddr_vector[i]);
					mem_entry->databuf_vaddr_vector[i] = NULL;
				}
			}
			memset(mem_entry->prp,0,sizeof(uint64_t)*NVMF_MAX_PRP_LIST_ENTRIES);
			mem_entry->bufsize = 0;
			STAILQ_INSERT_TAIL(&(mcdma_req->req.qpair->mem_buf_head),mem_entry,link);
		}
		free(mcdma_req->req.kernel);
		mcdma_req->req.kernel = NULL;
		STAILQ_FOREACH(recv, &mcdma_req->recv_queue, link) {
			nvmf_mcdma_recv_free(rqpair, recv);
		}
		
	}

	STAILQ_INSERT_HEAD(&rqpair->resources->free_queue, mcdma_req, state_link);
	mcdma_req->state = MCDMA_REQUEST_STATE_FREE;
}

static void
nvmf_mcdma_check_fused_ordering(struct spdk_nvmf_mcdma_transport *rtransport,
			       struct spdk_nvmf_mcdma_qpair *rqpair,
			       struct spdk_nvmf_mcdma_request *mcdma_req)
{
	enum spdk_nvme_cmd_fuse last, next;

	last = rqpair->fused_first ? rqpair->fused_first->req.cmd->nvme_cmd.fuse : SPDK_NVME_CMD_FUSE_NONE;
	next = mcdma_req->req.cmd->nvme_cmd.fuse;

	assert(last != SPDK_NVME_CMD_FUSE_SECOND);

	if (spdk_likely(last == SPDK_NVME_CMD_FUSE_NONE && next == SPDK_NVME_CMD_FUSE_NONE)) {
		return;
	}

	if (last == SPDK_NVME_CMD_FUSE_FIRST) {
		if (next == SPDK_NVME_CMD_FUSE_SECOND) {
			/* This is a valid pair of fused commands.  Point them at each other
			 * so they can be submitted consecutively once ready to be executed.
			 */
			rqpair->fused_first->fused_pair = mcdma_req;
			mcdma_req->fused_pair = rqpair->fused_first;
			rqpair->fused_first = NULL;
			return;
		} else {
			/* Mark the last req as failed since it wasn't followed by a SECOND. */
			rqpair->fused_first->fused_failed = true;

			/* If the last req is in READY_TO_EXECUTE state, then call
			 * nvmf_mcdma_request_process(), otherwise nothing else will kick it.
			 */
			if (rqpair->fused_first->state == MCDMA_REQUEST_STATE_READY_TO_EXECUTE) {
				nvmf_mcdma_request_process(rtransport, rqpair->fused_first);
			}

			rqpair->fused_first = NULL;
		}
	}

	if (next == SPDK_NVME_CMD_FUSE_FIRST) {
		/* Set rqpair->fused_first here so that we know to check that the next request
		 * is a SECOND (and to fail this one if it isn't).
		 */
		rqpair->fused_first = mcdma_req;
	} else if (next == SPDK_NVME_CMD_FUSE_SECOND) {
		/* Mark this req failed since it ia SECOND and the last one was not a FIRST. */
		mcdma_req->fused_failed = true;
	}
}

static void nvmf_mcdma_memcopy_cmpl(struct spdk_compute_sqe *sqe,
									uint32_t status, void *cb_arg)
{
	struct spdk_nvmf_mcdma_request *mcdma_req = cb_arg;
	struct spdk_nvmf_qpair *qpair = mcdma_req->req.qpair;
	struct spdk_nvmf_mcdma_transport *rtransport = SPDK_CONTAINEROF(qpair->transport,
																	struct spdk_nvmf_mcdma_transport, transport);

	mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;

	nvmf_mcdma_request_process(rtransport, mcdma_req);

}



static inline uint64_t mem_handle_to_physaddr(struct spdk_nvmf_mcdma_request *req, uint64_t handle)
{
	if (handle < 512) {
		return req->req.kernel->mem_buf[handle]->databuf_first_page_paddr;
	} else {
		return handle;
	}
}

bool
nvmf_mcdma_request_process(struct spdk_nvmf_mcdma_transport *rtransport,
			  struct spdk_nvmf_mcdma_request *mcdma_req)
{
	struct spdk_nvmf_mcdma_qpair	*rqpair;
	struct spdk_nvme_cpl		*rsp = &mcdma_req->req.rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_mcdma_recv	*mcdma_recv;
	enum spdk_nvmf_mcdma_request_state prev_state;
	bool				progress = false;
	int				data_posted;
	uint32_t			num_blocks;
	
	rqpair = SPDK_CONTAINEROF(mcdma_req->req.qpair, struct spdk_nvmf_mcdma_qpair, qpair);

	assert(mcdma_req->state != MCDMA_REQUEST_STATE_FREE);
	// Controller Number
	mcdma_req->req.ctrlr_id = rqpair->ctrlr_num;


	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = mcdma_req->state;

		SPDK_DEBUGLOG(nvmf, "Request %p ctrlr %u entering state %d\n", mcdma_req, mcdma_req->qpair->ctrlr_num, prev_state);

		switch (mcdma_req->state) {
		case MCDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into MCDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case MCDMA_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_NEW, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);
			mcdma_recv = mcdma_req->recv;

			/* The first element of the SGL is the NVMe command */
			mcdma_req->req.cmd = (union nvmf_h2c_msg *)mcdma_recv->byp_io->iov.iov_base;
			memset(mcdma_req->req.rsp, 0, sizeof(*mcdma_req->req.rsp));

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&mcdma_req->req, &mcdma_req->req.dif.dif_ctx))) {
				mcdma_req->req.dif_enabled = true;
			}

			nvmf_mcdma_check_fused_ordering(rtransport, rqpair, mcdma_req);

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
			mcdma_req->rsp.wr.opcode = IBV_WR_SEND;
			mcdma_req->rsp.wr.imm_data = 0;
#endif

			/* The next state transition depends on the data transfer needs of this request. */
			mcdma_req->req.xfer = spdk_nvmf_req_get_xfer(&mcdma_req->req);
			/** Opcode does not transfer data */
			//SPDK_NVME_DATA_NONE				= 0,
			/** Opcode transfers data from host to controller (e.g. Write) */
			//SPDK_NVME_DATA_HOST_TO_CONTROLLER		= 1,
			/** Opcode transfers data from controller to host (e.g. Read) */
			//SPDK_NVME_DATA_CONTROLLER_TO_HOST		= 2,
			/** Opcode transfers data both directions */
			//SPDK_NVME_DATA_BIDIRECTIONAL			= 3

			if (spdk_unlikely(mcdma_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
				mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_COMPLETE;
				SPDK_DEBUGLOG(nvmf, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", mcdma_req);
				break;
			}

			SPDK_DEBUGLOG(nvmf, "Got cmd opc %X fctype %X xfer %d\n", mcdma_req->req.cmd->nvme_cmd.opc, mcdma_req->req.cmd->nvmf_cmd.fctype, mcdma_req->req.xfer);

			mcdma_req->admin_data_buf = NULL;

			STAILQ_INIT(&mcdma_req->recv_queue);


			//判断队列是否是admin queue还是io queue
			if (nvmf_is_io_request_with_sgl(&mcdma_req->req)) {
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;

				SPDK_DEBUGLOG(nvmf, "PRP 1 is 0x%lX, PRP 2 is 0x%lX\n",
							  mcdma_req->req.cmd->nvme_cmd.dptr.prp.prp1,
							  mcdma_req->req.cmd->nvme_cmd.dptr.prp.prp2);

					mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_EXECUTE;
				if (spdk_unlikely(!rqpair->started)) {
					rqpair->started = true;
				}
			} else {
			/* If no data to transfer, ready to execute. */
				if (spdk_nvme_opc_is_kernel(mcdma_req->req.cmd->nvme_cmd.opc)) {
				//如果是内核，代码都会被连续传输到kernels结构里>>>
				
					mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
					mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
					
					// printf("Got kernel command\n");
					mcdma_req->req.kernel = calloc(1, sizeof(*mcdma_req->req.kernel));
					if (!mcdma_req->req.kernel) {
						SPDK_ERRLOG("Unable to allocate memory for kernel.\n");
						mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_COMPLETE;
						break;
					}
					mcdma_req->req.kernel->num_cmds = 1;
					mcdma_req->req.kernel->cmds[0] = mcdma_req->req.cmd;
					mcdma_req->state = MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;

					//debug
					//struct spdk_nvmf_mcdma_device* mdev = mcdma_req->qpair->device;
					//mdev->one_kernel_start_transfer_time = spdk_get_ticks();
				} else if (mcdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER || mcdma_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
					mcdma_req->state = MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;
					//Allocate a pinned memory buffer with the given size and alignment. The buffer will be zeroed.
					mcdma_req->admin_data_buf = spdk_dma_zmalloc(PAGE_SIZE, PAGE_SIZE, NULL);
					mcdma_req->req.data = mcdma_req->admin_data_buf;
					mcdma_req->req.length = 0;
					//TODO Test OPC CODE
					if(mcdma_req->req.cmd->nvme_cmd.opc == 0x0d){
						SPDK_DEBUGLOG(nvmf,"GET NAMESPACE CREATE COMMAND!\n");
					}
				} else {
					//Admin Queue QID = 0
					if (mcdma_req->req.qpair->qid == 0 && mcdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
						//arm上mcdma请求的地址
						//mcdma请求需要对应一个arm的buffer
						//iovs[0]是请求本身
						//iovs[1]是请求数据（arm侧）
						mcdma_req->req.data = mcdma_req->rsp.iovs[1].iov_base;
						mcdma_req->req.length = mcdma_req->rsp.iovs[1].iov_len;
						mcdma_req->req.data_from_pool = false;
					}
					mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_EXECUTE;
				}
			}

			break;
		case MCDMA_REQUEST_STATE_GET_SGL:
			break;
		case MCDMA_REQUEST_STATE_GET_DATA:
			break;
		case MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		//当数据从主机到控制器，且是对控制队列进行读写时进入此状态
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into MCDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case MCDMA_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_READY_TO_EXECUTE, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);
			//TODO: 后续封装成单独的函数？
			if(nvmf_qpair_is_admin_queue(mcdma_req->req.qpair)&&mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_NS_MANAGEMENT){
				SPDK_DEBUGLOG(nvmf,"OK I GOT DATA! size %d CSI%d cdw10%x\n",mcdma_req->req.length,(mcdma_req->req.cmd->nvme_cmd.cdw11)>>24,mcdma_req->req.cmd->nvme_cmd.cdw10);
				if(mcdma_req->req.cmd->nvme_cmd.cdw11>>24 == 0x03)	
				{
					struct spdk_nvme_ns_data* ns_data = (struct spdk_nvme_ns_data*)mcdma_req->admin_data_buf;
					//1<<31为memory namespace mask
					if(mcdma_req->req.cmd->nvme_cmd.cdw10 == 0) //Create Namespace
					{
						struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
						
						int size = ns_data->nsze;
						SPDK_DEBUGLOG(nvmf,"NS SIZE%d\n",size);
						int ret = spdk_hlsacccompute_devmem_malloc(dev,
																		size,
																		(int)(PAGE_SIZE),
																		NULL,
																		NULL);
						if(ret>=0){
							mcdma_req->req.rsp->nvme_cpl.cdw0 = (1 << 31) | ret;
						}else{
							mcdma_req->req.rsp->nvme_cpl.cdw0 = 0;
							mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY;
						}
						
						
					}else if(mcdma_req->req.cmd->nvme_cmd.cdw10==1){ //Delete
						int mid = mcdma_req->req.cmd->nvme_cmd.nsid & (~(1 << 31));
						struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
						SPDK_DEBUGLOG(nvmf,"DELETE NAMSEPACE MID%d\n",mid);
						int ret = spdk_hlsacccompute_devmem_free(dev,NULL,mid);
						if(ret<0){
							mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE;
						}
					}
					mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
					mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
					continue;
				}
			}else if(nvmf_qpair_is_admin_queue(mcdma_req->req.qpair)&&mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_MEMORY_RANGE_SET_MGMT){
				//当前每个控制器仅有一个compute namespace，不可修改！
				SPDK_DEBUGLOG(nvmf,"ENTER XX!\n");
				struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
				unsigned short op = cmd->cdw10;
				unsigned short rsid = (cmd->cdw10 >> 16);
				unsigned char numr = (cmd->cdw11);
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
				mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
				if(mcdma_req->req.cmd->nvme_cmd.nsid != 2){
					mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_MEMORY_NAMESPACE;
				}
				if(op==0)//create
				{
					struct spdk_nvme_memory_range_descriptor* desc = (struct spdk_nvme_memory_range_descriptor*)mcdma_req->admin_data_buf;
					struct memory_range_sets se,*entry_ptr;
					memcpy(&se.raw_data,desc,sizeof(struct spdk_nvme_memory_range_descriptor)*numr);
					se.header.header.nmr = numr;
					unsigned int key = atomic_fetch_add(&(rqpair->device->memrangeset_next_id),1);
					se.header.header.rsid = key;
					struct spdk_hlsacccompute_dev *dev = &(rqpair->device->compute);
					for(int k=0;k<numr;k++){
						//Get Quick Cache
						void* ns_vaddr,*ns_paddr;
						if(se.raw_data[k].payload.flag==MEM_RANGE_DEVICE_MEM){
							int ret = spdk_hlsacccompute_devmem_lookup(dev,(se.raw_data[k].payload.mnsid)&(~SLM_MASK),&(ns_vaddr),&(ns_paddr));
							if(ret == -1){
								mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_MEMORY_NAMESPACE;
								continue;
							}
							se.quick_cache[k].iov_base = ns_vaddr + se.raw_data[k].payload.starting_byte;
							se.quick_cache[k].cur_used = 0;
						}else if(se.raw_data[k].payload.flag==MEM_RANGE_HOST_MEM){
							se.quick_cache[k].iov_base = se.raw_data[k].payload.starting_byte;
							se.quick_cache[k].cur_used = 0;
						}
						se.quick_cache[k].is_mem = true;
						se.quick_cache[k].iov_len = se.raw_data[k].payload.length;
						SPDK_DEBUGLOG(nvmf,"INDEX%d,STARTINGBYTES%llx,ISMEM%d,LEN%x",k,
						se.raw_data[k].payload.starting_byte,
						se.quick_cache[k].is_mem,
						se.raw_data[k].payload.length);
					}
					// Error Handler ???????
					void* table = rqpair->device->memrangeset_hash_tables; 
					int ret = spdk_cuckoo_table_entry_add(table,key,&se,&entry_ptr);
					mcdma_req->req.rsp->nvme_cpl.cdw0 = key | (rqpair->ctrlr_num<<16);
					if(ret < 0){
						mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_MEMORY_RANGE_SET;
					}
				}else if(op==1)//delete
				{
					void* table = rqpair->device->memrangeset_hash_tables; 
					void* entry;
					int ret = spdk_cuckoo_table_lookup(table,rsid,&entry);
					if(ret!=0){
						mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_MEMORY_RANGE_SET;
					}else{
						ret = spdk_cuckoo_table_entry_delete(table,rsid,entry);
						if(ret!=0)
						mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_MEMORY_RANGE_SET;
					}
				}
				continue;
			}else if(nvmf_qpair_is_admin_queue(mcdma_req->req.qpair)&&mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_PROGRAM_LOAD){
				
				struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
				mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
				unsigned short op = (cmd->cdw10 >> 24) & 0x1;
				unsigned short pind = (cmd->cdw10);
				unsigned int psize = (cmd->cdw11);
				unsigned int numb = (cmd->cdw14);
				unsigned int load_offset = (cmd->cdw15);
				SPDK_DEBUGLOG(nvmf,"ENTER XX! LOAD PROGRAM op %x pind%xpsize%x\n",op,pind,psize);
				struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
				if(op==0)//Load Program
				{
					SPDK_DEBUGLOG(nvmf,"ENTER XX! PROGRAM LOAD OPERATE\n");
					//Will Support Software Program In the Future :D
					if(load_offset!=0){
						struct spdk_hlsacccompute_program* p;
						
						if(!spdk_hlsacccompute_lookup_program(dev,&p,pind)){
							//Program does not load
							if(p->software_data==NULL)
								spdk_hlsacccompute_add_program_sw_data(&p,psize-PAGE_SIZE);
							if(load_offset > psize){
								mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_MAX_PROGRAM_BYTES_EXCEE;
							}
							else
								memcpy(p->software_data+(load_offset-PAGE_SIZE),mcdma_req->admin_data_buf,numb);
						}else{
							mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_NO_PROGRAM;
						}
						
					}
					else{
						struct spdk_hlsacccompute_program* fromprogram = (struct spdk_hlsacccompute_program*)mcdma_req->admin_data_buf;
						spdk_hlsacccompute_dump_program_data(fromprogram);
						struct spdk_hlsacccompute_program* toprogram;
						int ret = spdk_hlsacccompute_get_program_container(&toprogram);
						if(ret < 0){
							mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INSUFF_PROGRAM_RESOURCES;
						}else{
							memcpy(toprogram,fromprogram,sizeof(struct spdk_hlsacccompute_program));
							toprogram->software_data = NULL;
							ret = spdk_hlsacccompute_add_program_with_id(dev,toprogram,pind);
							if(ret==-1)
							mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INSUFF_PROGRAM_RESOURCES;

						}
					}

				}else if(op==1){
					SPDK_DEBUGLOG(nvmf,"ENTER XX! UNLOAD PROGRAM\n");
					//Program in use is invalid in this design
					if(pind<64&&dev->program_list[pind]!=NULL)
						spdk_free(dev->program_list[pind]);
					int ret = spdk_hlsacccompute_del_program(dev,pind);
					if(ret == -1){
						mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_PROGRAM_INDEX;
					}
				}
				continue;
			}else if(nvmf_qpair_is_admin_queue(mcdma_req->req.qpair)&&(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_PROGRAM_ACTIVATION_MGMT)){
				SPDK_DEBUGLOG(nvmf,"ENTER XX!\n");
				struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
				mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
				unsigned short op = (cmd->cdw10 >> 16) & 0x1;
				unsigned short pind = (cmd->cdw10);
				struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
				{
					if(dev->program_list[pind]!=NULL){
						dev->program_list[pind]->activated = (op == 0);
						if(dev->program_list[pind]->software_data!=NULL){
							spdk_hlsacccompute_add_program_sw_data(&(dev->program_list[pind]),0);
						}
					}
					else
					{
						mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_PROGRAM_INDEX;
					}
				}
				continue;
			}else if((!nvmf_qpair_is_admin_queue(mcdma_req->req.qpair))&&((mcdma_req->req.cmd->nvme_cmd.nsid & SLM_MASK) != 0)//MEMORY NAMESPACE NSID
			&&(mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_READ||
				mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_WRITE||
				mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_FILL||
				mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_COPY)){
				SPDK_DEBUGLOG(nvmf,"ENTER XX! OPC%d\n",mcdma_req->req.cmd->nvme_cmd.opc);
				struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
				struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
				mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
				unsigned long long starting_bytes = cmd->cdw11 << 32 | cmd->cdw10;
				unsigned int read_or_write_length = cmd->cdw12;
				//struct spdk_nvmf_ctrlr *ctrlr = mcdma_req->req.qpair->ctrlr;
				//TODO Current Namespace is locked to 1,
				//Does not support multiple stroage namespace!!!!!!
				//struct spdk_nvmf_ns *ns = _nvmf_subsystem_get_ns(ctrlr->subsys, 1);
				//TODO Dynamic set more block size in the future
				unsigned int bsize = PAGE_SIZE;//= spdk_bdev_get_block_size(ns->bdev);
				void* ns_vaddr,*ns_paddr;
				int ret = spdk_hlsacccompute_devmem_lookup(dev,cmd->nsid&(~SLM_MASK),&(ns_vaddr),&(ns_paddr));
				SPDK_DEBUGLOG(nvmf,"VADDR %llx,PADDR%llx,starting_bytes%llx\n",ns_vaddr,ns_paddr,starting_bytes);
				struct  handc_ctx* ctx = (struct handc_ctx*) mcdma_req->data_buf;
				//SPDK_DEBUGLOG(nvmf,"READ OR WRITE LENGTH%d\n",read_or_write_length);
				ctx->impl_thread = spdk_get_thread();
				if(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_SLM_WRITE){
					//First，Try to Get Prp Pages
					//SPDK_DEBUGLOG(nvmf,"SLM WRITE\n");
					ctx->device = rqpair->device;
					
					if(read_or_write_length<=bsize){
						ctx->fsm_state = FETCH_DATA;
						ctx->from_size = 1;
						ctx->to_size = 1;
						ctx->from_iovecs[0].iov_base = NULL;
						ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp1;
						ctx->from_iovecs[0].iov_len = read_or_write_length;
						ctx->to_iovecs[0].iov_base = ns_vaddr+starting_bytes;
						ctx->to_iovecs[0].paddr = spdk_vtophys(ctx->to_iovecs[0].iov_base,NULL);
						ctx->to_iovecs[0].iov_len = read_or_write_length;
						SPDK_DEBUGLOG(nvmf,"Get PRP ADDRESS%llx DATA ELM%llx ADDRESS%llx ELM%c\n",cmd->dptr.prp.prp1,*((uint64_t*)ctx->to_iovecs[0].iov_base),ctx->to_iovecs[0].iov_base,((char*)(ctx->to_iovecs[0].iov_base))[0]);
					}else if(read_or_write_length>bsize&&read_or_write_length<=2*bsize){
						ctx->fsm_state = FETCH_DATA;
						ctx->from_size = 2;
						ctx->to_size = 2;
						ctx->from_iovecs[0].iov_base = NULL;
						ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp1;
						ctx->from_iovecs[0].iov_len = bsize;
						ctx->to_iovecs[0].iov_base = ns_vaddr+starting_bytes;
						ctx->to_iovecs[0].paddr = spdk_vtophys(ctx->to_iovecs[0].iov_base,NULL);
						ctx->to_iovecs[0].iov_len = bsize;
						ctx->from_iovecs[1].iov_base = NULL;
						ctx->from_iovecs[1].paddr = cmd->dptr.prp.prp2;
						ctx->from_iovecs[1].iov_len = read_or_write_length-bsize;
						ctx->to_iovecs[1].iov_base = ns_vaddr+starting_bytes+bsize;
						ctx->to_iovecs[1].paddr = spdk_vtophys(ctx->to_iovecs[1].iov_base,NULL);
						ctx->to_iovecs[1].iov_len = read_or_write_length-bsize;
						SPDK_DEBUGLOG(nvmf,"Get PRP ADDRESS%llx DATA ELM%llx ADDRESS%llx ELM%c\n",cmd->dptr.prp.prp1,*((uint64_t*)ctx->to_iovecs[0].iov_base),ctx->to_iovecs[0].iov_base,((char*)(ctx->to_iovecs[0].iov_base))[0]);
					}
					else{
						ctx->fsm_state = FETCH_PRP;
						ctx->from_size = 1;
						ctx->to_size = 1;
						ctx->from_iovecs[0].iov_base = NULL;
						ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp2;
						ctx->to_iovecs[0].iov_base = mcdma_req->sgl_buf;
						ctx->to_iovecs[0].paddr = spdk_vtophys(mcdma_req->sgl_buf,NULL);
						ctx->from_iovecs[0].iov_len = PAGE_SIZE;
						ctx->to_iovecs[0].iov_len = PAGE_SIZE;
						SPDK_DEBUGLOG(nvmf,"Get PRP ADDRESS%llx %llx\n",cmd->dptr.prp.prp1,cmd->dptr.prp.prp2);
					}
					ctx->mcdma_req = mcdma_req;
					ctx->rtransport = rtransport;
					spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
					continue;

				}else if(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_SLM_READ){
					SPDK_NOTICELOG(
						"SLM_READ enter req=%p cid=%u mem_nsid=0x%x mem_id=%u start=%llu len=%u prp1=0x%llx prp2=0x%llx lookup_ret=%d ns_vaddr=%p ns_paddr=%p\n",
						mcdma_req,
						mcdma_req->req.cmd->nvme_cmd.cid,
						cmd->nsid,
						cmd->nsid&(~SLM_MASK),
						starting_bytes,
						read_or_write_length,
						(unsigned long long)cmd->dptr.prp.prp1,
						(unsigned long long)cmd->dptr.prp.prp2,
						ret,
						ns_vaddr,
						ns_paddr);
					spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
						(uintptr_t)mcdma_req,"srfetpp");
					//SPDK_DEBUGLOG(nvmf,"REQUEST%llx CTX%llx SGL BUF%llx\n",mcdma_req,ctx,mcdma_req->sgl_buf);
					//First，Try to Get Prp Pages
					//SPDK_DEBUGLOG(nvmf,"RECV SLM READ COMMAND\n");
					ctx->device = rqpair->device;
					
					//SPDK_DEBUGLOG(nvmf,"CUR CORE%u\n",spdk_env_get_current_core());
					if(read_or_write_length<=bsize){
						ctx->fsm_state = FETCH_DATA;
						ctx->from_size = 1;
						ctx->to_size = 1;
						ctx->to_iovecs[0].iov_base = NULL;
						ctx->to_iovecs[0].paddr = cmd->dptr.prp.prp1;
						ctx->to_iovecs[0].iov_len = read_or_write_length;
						ctx->from_iovecs[0].iov_base = ns_vaddr+starting_bytes;
						ctx->from_iovecs[0].paddr = spdk_vtophys(ctx->from_iovecs[0].iov_base,NULL);
						ctx->from_iovecs[0].iov_len = read_or_write_length;
						SPDK_DEBUGLOG(nvmf,"Get PRP ADDRESS%llx DATA ELM%llx ADDRESS%llx ELM%c\n",cmd->dptr.prp.prp1,*((uint64_t*)ctx->from_iovecs[0].iov_base),ctx->from_iovecs[0].iov_base,((char*)(ctx->from_iovecs[0].iov_base))[0]);
					}else if(read_or_write_length>bsize&&read_or_write_length<=2*bsize){
						ctx->fsm_state = FETCH_DATA;
						ctx->from_size = 2;
						ctx->to_size = 2;
						ctx->to_iovecs[0].iov_base = NULL;
						ctx->to_iovecs[0].paddr = cmd->dptr.prp.prp1;
						ctx->to_iovecs[0].iov_len = bsize;
						ctx->from_iovecs[0].iov_base = ns_vaddr+starting_bytes;
						ctx->from_iovecs[0].paddr = spdk_vtophys(ctx->from_iovecs[0].iov_base,NULL);
						ctx->from_iovecs[0].iov_len = bsize;
						ctx->to_iovecs[1].iov_base = NULL;
						ctx->to_iovecs[1].paddr = cmd->dptr.prp.prp2;
						ctx->to_iovecs[1].iov_len = read_or_write_length-bsize;
						ctx->from_iovecs[1].iov_base = ns_vaddr+starting_bytes+bsize;
						ctx->from_iovecs[1].paddr = spdk_vtophys(ctx->from_iovecs[1].iov_base,NULL);
						ctx->from_iovecs[1].iov_len = read_or_write_length-bsize;
						SPDK_DEBUGLOG(nvmf,"Get PRP ADDRESS%llx DATA ELM%llx ADDRESS%llx ELM%c\n",cmd->dptr.prp.prp1,*((uint64_t*)ctx->from_iovecs[0].iov_base),ctx->from_iovecs[0].iov_base,((char*)(ctx->from_iovecs[0].iov_base))[0]);
					}
					else{
						ctx->fsm_state = FETCH_PRP;
						ctx->from_size = 1;
						ctx->to_size = 1;
						ctx->from_iovecs[0].iov_base = NULL;
						ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp2;
						ctx->to_iovecs[0].iov_base = mcdma_req->sgl_buf;
						ctx->to_iovecs[0].paddr = spdk_vtophys(mcdma_req->sgl_buf,NULL);
						ctx->from_iovecs[0].iov_len = PAGE_SIZE;
						ctx->to_iovecs[0].iov_len = PAGE_SIZE;
						//SPDK_DEBUGLOG(nvmf,"Get PRP ADDRESS%llx %llx\n",cmd->dptr.prp.prp1,cmd->dptr.prp.prp2);
					}
					ctx->mcdma_req = mcdma_req;
					ctx->rtransport = rtransport;
					SPDK_NOTICELOG(
						"SLM_READ handc_submit req=%p fsm=%s from_size=%d to_size=%d from0=%p from0_paddr=0x%llx from0_len=%llu to0_paddr=0x%llx to0_len=%llu\n",
						mcdma_req,
						handc_fsm_name(ctx->fsm_state),
						ctx->from_size,
						ctx->to_size,
						ctx->from_iovecs[0].iov_base,
						(unsigned long long)ctx->from_iovecs[0].paddr,
						(unsigned long long)ctx->from_iovecs[0].iov_len,
						(unsigned long long)ctx->to_iovecs[0].paddr,
						(unsigned long long)ctx->to_iovecs[0].iov_len);
					
					spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
					continue;

				}else if(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_SLM_FILL){
					//May has BUG!!!!
					if(ns_vaddr!=NULL)
						memset(ns_vaddr+starting_bytes,0,read_or_write_length);
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
					continue;
				}else if(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_SLM_COPY){
					//Only Support Storage Device to Local Memory
					//The Memory Copy command is used by the host to 
					//copy data from one or more source ranges in one or more source namespaces 
					//to a single consecutive address range in a destination memory namespace. 
					//Each source range may be in the same namespace or a different namespace with 
					//respect to any other source range and with respect to the destination memory namespace
					//(i.e., the NSID in the Submission Queue Entry for the Memory Copy command) address range.
					//First，Try to Get Prp Pages
					spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
						(uintptr_t)mcdma_req,"readyslmcopy");
					unsigned char nr = mcdma_req->req.cmd->nvme_cmd.cdw12 + 1;
					ctx->device = rqpair->device;
					ctx->fsm_state = FETCH_DATA;
					ctx->from_size = 1;
					ctx->to_size = 1;
					ctx->from_iovecs[0].iov_base = NULL;
					ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp1;
					ctx->to_iovecs[0].iov_base = mcdma_req->sgl_buf;
					ctx->to_iovecs[0].paddr = spdk_vtophys(mcdma_req->sgl_buf,NULL);
					ctx->from_iovecs[0].iov_len = PAGE_SIZE;
					ctx->to_iovecs[0].iov_len = PAGE_SIZE;	
					ctx->mcdma_req = mcdma_req;
					ctx->rtransport = rtransport;
					spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
					continue;
				}
			}else if(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_COPY&&(
				((uint8_t)(mcdma_req->req.cmd->nvme_cmd.cdw12 >> 8)==0x4)
			)){
				unsigned char nr = mcdma_req->req.cmd->nvme_cmd.cdw12 + 1;
				struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
				struct  handc_ctx* ctx = (struct handc_ctx*) mcdma_req->data_buf;
				ctx->impl_thread = spdk_get_thread();
				ctx->device = rqpair->device;
				ctx->fsm_state = FETCH_DATA;
				ctx->from_size = 1;
				ctx->to_size = 1;
				ctx->from_iovecs[0].iov_base = NULL;
				ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp1;
				ctx->to_iovecs[0].iov_base = mcdma_req->sgl_buf;
				ctx->to_iovecs[0].paddr = spdk_vtophys(mcdma_req->sgl_buf,NULL);
				ctx->from_iovecs[0].iov_len = 32*nr;
				ctx->to_iovecs[0].iov_len = PAGE_SIZE;	
				ctx->mcdma_req = mcdma_req;
				ctx->rtransport = rtransport;
				spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
				continue;
			}else if((!nvmf_qpair_is_admin_queue(mcdma_req->req.qpair))&&mcdma_req->req.cmd->nvme_cmd.nsid==2&&
			mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_PROGRAM_EXECUTE){
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
				mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
				SPDK_DEBUGLOG(nvmf,"BEGIN TO EXECUTE PROGRAM!\n");
				spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
					(uintptr_t)mcdma_req, "readyexecprog");
				//根据当前的需求，分配virtual object
				//请求运行程序，注册返回回调函数
				//首先，获取一个memory range set
				struct spdk_nvme_cmd cmd = mcdma_req->req.cmd->nvme_cmd;
				unsigned short rsid = cmd.rsvd2 >> 16;
				unsigned short pind = cmd.rsvd2;
				unsigned int numr = cmd.rsvd3;
				unsigned int dlen = cmd.mptr;
				//cparam1 ctx initialize sel
				unsigned long long cparam1 = cmd.cdw10 | (cmd.cdw11 << 32);
				//cparam2 request priority (7:0) request type(sw,hw,fusion)(15:8)
				unsigned long long cparam2 = cmd.cdw12 | (cmd.cdw13 << 32);
				unsigned long long cparam3 = cmd.cdw14 | (cmd.cdw15 << 32);

				unsigned char need_init_ctx[8] = {0};
				unsigned char priority = cparam2;

				unsigned long long temp_val = cparam1;
				unsigned char need_init_num = 0;
			

				struct spdk_nvmf_mcdma_device* mdev = mcdma_req->qpair->device;
				void* table = mdev->memrangeset_hash_tables;
				struct memory_range_sets* entry = NULL;
				int ret = spdk_cuckoo_table_lookup(table,rsid,&entry);
				if(ret!=0){
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
					mcdma_req->req.rsp->nvme_cpl.status.sc = 0x8B;//Invalid Memory Namespaces
					continue;
				}
				struct spdk_hlsacccompute_program* program = (mdev->compute).program_list[pind];
				if(program==NULL){
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
					mcdma_req->req.rsp->nvme_cpl.status.sc = 0x8F;//Invalid Program Pind
					continue;
				}
				struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
				pthread_mutex_lock(&(mdev->compute_mutex));

				struct spdk_hlsacccompute_request* request = spdk_hlsacccompute_create_request(&(mdev->compute),program);
				request->req_cb_args = mcdma_req;
				request->req_cb_fns = hlsacccompute_req_callback;
				request->priority = priority;
				request->dev = dev;
				//SPDK_NOTICELOG("MCDMAREQ %llx rsp%llx\n",mcdma_req,mcdma_req->req.rsp);
				request->run_way = cparam3;
				//SPDK_NOTICELOG("GET REQUEST RUN WAY%d\n",request->run_way);
				int m=0;
				for(int i=0;i<program->apply_operators_num;i++){
					need_init_ctx[i] = temp_val;
					temp_val = temp_val >> 8;
					if(need_init_ctx[i]==1){
						need_init_num++;
					}
				}
				//Lock?? MemPool!
				for(int k=0;k<program->input_channum;k++){
					struct spdk_hlsacccompute_virtual_object* ob = TAILQ_FIRST(&((mdev->compute).vo_pool));
					TAILQ_REMOVE(&((mdev->compute).vo_pool),ob,link);
					memcpy(ob,&(entry->quick_cache[m]),sizeof(struct spdk_hlsacccompute_virtual_object));
					++m;
					SPDK_DEBUGLOG(nvmf,"OB LEN %x OB VADDR%llx CUR USED%d\n",ob->iov_len,ob->iov_base,ob->cur_used);
					TAILQ_INSERT_HEAD(&(request->tx_vos[k]),ob,link);
				}
				for(int k=0;k<program->output_channum;k++){
					struct spdk_hlsacccompute_virtual_object* ob = TAILQ_FIRST(&((mdev->compute).vo_pool));
					TAILQ_REMOVE(&((mdev->compute).vo_pool),ob,link);
					memcpy(ob,&(entry->quick_cache[m]),sizeof(struct spdk_hlsacccompute_virtual_object));
					++m;
					SPDK_DEBUGLOG(nvmf,"OB LEN %x OB VADDR%llx\n",ob->iov_len,ob->iov_base);
					TAILQ_INSERT_HEAD(&(request->rx_vos[k]),ob,link);
				}
				struct  handc_ctx* ctx = (struct handc_ctx*) mcdma_req->data_buf;
				ctx->device = mcdma_req->qpair->device;
				ctx->mcdma_req = mcdma_req;
				ctx->rtransport = rtransport;
				pthread_mutex_unlock(&(mdev->compute_mutex));
				ctx->fsm_state = NON_OP;
				ctx->hls_request = request;
				ctx->impl_thread = spdk_get_thread();
				SPDK_DEBUGLOG(nvmf,"BLOCK CONTEXT NUM%d\n",need_init_num);
				if(request->program->apply_operators_num==0||(cparam1==0)){
					SPDK_DEBUGLOG(hlsacc,"RUN REQUEST PREEMPT%d\n",request->priority);
					mcdma_req->impl_thread = spdk_get_thread();
					if(request->priority<120)//Preempt if priority is high
						spdk_thread_send_msg(mdev->compute_thread,hlsacccompute_run_request_unpreempt,request);
					else
						spdk_thread_send_msg(mdev->compute_thread,hlsacccompute_run_request_preempt,request);
				}else{
					struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
					//SPDK_NOTICELOG("WAITING FOR ACCCONTEXT %llx\n",request->acccontext[0]);	
					if(request->acccontext[1]!=NULL){
						SPDK_DEBUGLOG(nvmf,"WAITING FOR ACCCONTEXT %llx\n",request->acccontext[1]);	
					}
						if(need_init_num==1){	
							ctx->fsm_state = FETCH_DATA;
							ctx->to_size = 1;
							ctx->from_size = 1;
							ctx->from_iovecs[0].iov_base = NULL;
							ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp1;
							ctx->from_iovecs[0].iov_len = 4096;
							int i=0;
							for(i=0;i<8;i++){
								if(need_init_ctx[i]==1) break;
							}
					
							
							ctx->to_iovecs[0].iov_base = (unsigned long long)(request->acccontext[i])+4096;
							ctx->to_iovecs[0].iov_len =  4096;
							ctx->to_iovecs[0].paddr = spdk_vtophys(ctx->to_iovecs[0].iov_base,NULL);
							SPDK_NOTICELOG("LWE_CTX_FETCH 准备搬运context req=%p op_index=%d host_prp=0x%llx dst_vaddr=0x%llx dst_paddr=0x%llx len=%llu cparam1=0x%llx need_init_num=%u\n",
								request,
								i,
								(unsigned long long)ctx->from_iovecs[0].paddr,
								(unsigned long long)ctx->to_iovecs[0].iov_base,
								(unsigned long long)ctx->to_iovecs[0].paddr,
								(unsigned long long)ctx->to_iovecs[0].iov_len,
								(unsigned long long)cparam1,
								(unsigned int)need_init_num);
							//SPDK_NOTICELOG("VADDR%llx PADDR%llx\n",ctx->to_iovecs[0].iov_base,ctx->to_iovecs[0].paddr);
							assert(cmd->dptr.prp.prp1!=NULL);
							if(cmd->dptr.prp.prp1==NULL){
								SPDK_ERRLOG("Failed to Fetch Block Context!\n");

						}else{
							SPDK_DEBUGLOG(nvmf,"PRP ADDRESS%llx\n",cmd->dptr.prp.prp1);
						}
						//SPDK_NOTICELOG("FETCH ONE BLOCK CONTEXT");
						spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
					}else if(need_init_num==2){
						ctx->fsm_state = FETCH_DATA;
						ctx->to_size = 2;
						ctx->from_size = 2;
						int i=0;
						for(i=0;i<8;i++){
							if(need_init_ctx[i]==1) break;
						}
						ctx->from_iovecs[0].iov_base = NULL;
						ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp1;
						ctx->from_iovecs[0].iov_len =  4096;
						ctx->to_iovecs[0].iov_base = request->acccontext[i]->context.static_data;
						ctx->to_iovecs[0].iov_len =  4096;
						ctx->to_iovecs[0].paddr = spdk_vtophys(ctx->to_iovecs[0].iov_base,NULL);
						for(;i<8;i++){
							if(need_init_ctx[i]==1) break;
						}
						ctx->from_iovecs[1].iov_base = NULL;
						ctx->from_iovecs[1].paddr = cmd->dptr.prp.prp2;
						ctx->from_iovecs[1].iov_len =  4096;
						ctx->to_iovecs[1].iov_base = request->acccontext[i]->context.static_data;
						ctx->to_iovecs[1].iov_len =  4096;
						ctx->to_iovecs[1].paddr = spdk_vtophys(ctx->to_iovecs[1].iov_base,NULL);
						SPDK_DEBUGLOG(nvmf,"FETCH TWO BLOCK CONTEXT");
						spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
					}
					else if(need_init_num>2){
						SPDK_DEBUGLOG(nvmf,"FETCH MULTI BLOCK CONTEXT");
						ctx->fsm_state = FETCH_PRP;
						ctx->to_size = 1;
						ctx->from_size = 1;
						ctx->from_iovecs[0].iov_base = NULL;
						ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp2;
						ctx->from_iovecs[0].iov_len = PAGE_SIZE;
						ctx->to_iovecs[0].iov_base = mcdma_req->sgl_buf;
						ctx->to_iovecs[0].iov_len = PAGE_SIZE;
						ctx->to_iovecs[0].paddr = spdk_vtophys(ctx->to_iovecs[0].iov_base,NULL);
						spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
					}
				}
				continue;
			}else if(nvmf_qpair_is_admin_queue(mcdma_req->qpair)&&mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_GET_LOG_PAGE
			){
				unsigned char lid = mcdma_req->req.cmd->nvme_cmd.cdw10;
				if(lid == SPDK_NVME_LOG_DOWNLOAD_PROGRAM_TYPE||lid == SPDK_NVME_LOG_PROGRAM_LIST||
				lid == SPDK_NVME_LOG_MEM_RANGE_SET){
					struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
					mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
					mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
					struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
					switch(lid){
						case SPDK_NVME_LOG_DOWNLOAD_PROGRAM_TYPE:
							if(mcdma_req->rsp.iovs[1].iov_base!=NULL){
								union spdk_nvme_downable_prog_type_list* data = mcdma_req->rsp.iovs[1].iov_base;
								data[0].header.numd = 4;
								data[1].data.ptype = xilinx_soc_only_progran;
								data[1].data.version = 0;
								data[2].data.ptype = xilinx_fpga_only_program;
								data[2].data.version = 0;
								data[3].data.ptype = fusion_program;
								data[3].data.version = 0;
								data[4].data.ptype = operator_library;
								data[4].data.version = 0;
							}
							break;
						case SPDK_NVME_LOG_PROGRAM_LIST:
							if(mcdma_req->rsp.iovs[1].iov_base!=NULL){
								union spdk_nvme_prog_desc_list* data = mcdma_req->rsp.iovs[1].iov_base;
								data[0].header.numd = 48;					
								for(int i=0;i<16;i++){
									data[i+1].data.peocc = 0;
									data[i+1].data.pit = 0;
									data[i+1].data.pid = 0;
									if(dev->program_list[i]==NULL){
										data[i+1].data.activation = 0;
									}else{
										data[i+1].data.activation = dev->program_list[i]->activated;
										data[i+1].data.peocc = 0x1;
										data[i+1].data.program_type = fusion_program;
									}
								}
								//Library Mode
								for(int i=0;i<32;i++){
									data[i+17].data.peocc = 0;
									data[i+17].data.pit = 0;
									data[i+17].data.pid = 0;
									if(((dev->opconfigs[i]))==NULL){
										data[i+17].data.activation = 0;
									}else{
										data[i+17].data.peocc = 0x2;//device defined program
										data[i+17].data.program_type = operator_library;
										data[i+17].data.pit = 0x1;
										data[i+17].data.pid = 0;
										SPDK_DEBUGLOG(nvmf,"ADD OPERATOR NAME%s\n",((dev->opconfigs[i]))->operator_type_name);
										for(int j=0;j<8;j++){
											data[i+17].data.pid |= (((unsigned long long)(((dev->opconfigs[i]))->operator_type_name[j]))<<(j*8));
										}
									}
								}
								
							}
							break;
						case SPDK_NVME_LOG_MEM_RANGE_SET:
							if(mcdma_req->rsp.iovs[1].iov_base!=NULL){
								union spdk_nvme_memory_range_set_decriptor* data = mcdma_req->rsp.iovs[1].iov_base;
							}
							mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
							break;
					}
					continue;
				}
			}
			/*
			if (spdk_nvme_opc_is_kernel(mcdma_req->req.cmd->nvme_cmd.opc)) {
				//debug
				struct spdk_nvmf_mcdma_device* mdev = mcdma_req->qpair->device;
				mdev->one_kernel_start_execute_time = spdk_get_ticks();
			//此处kernel传输完毕，开始执行
			//需要添加一个spdk_nvme_kernel_exec函数执行kernel内的命令
			//如果遇到特殊类型，譬如MEM_ALLOC，则还需要特殊处理
				spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_KERNEL_EXEC, 0, 0,
						(uintptr_t)mcdma_req, (uintptr_t)mcdma_req->req.kernel->num_add_to_que);
				
				if(SPDK_DEBUGLOG_FLAG_ENABLED("nvmf"))
					{spdk_nvme_print_kernel(rqpair->qp_num, mcdma_req->req.kernel);}
				mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;	
	
				uint16_t opc = mcdma_req->req.kernel->cmds[mcdma_req->req.kernel->num_add_to_que]->opc;
			
				spdk_nvmf_request_exec(&mcdma_req->req);
				
				
				break;
			}*/
			if (spdk_unlikely(mcdma_req->req.dif_enabled)) {
				if (mcdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
					/* generate DIF for write operation */
					num_blocks = SPDK_CEIL_DIV(mcdma_req->req.dif.elba_length, mcdma_req->req.dif.dif_ctx.block_size);
					//DIF (Data Integrity Field)
					assert(num_blocks > 0);
					/**
					 * Generate DIF for extended LBA payload.
					参数:
					iovs – iovec array describing the extended LBA payload.
					iovcnt – Number of elements in the iovec array.
					num_blocks – Number of blocks of the payload.
					ctx – DIF context.
					返回:
					0 on success and negated errno otherwise.
					*/
					rc = spdk_dif_generate(mcdma_req->req.iov, mcdma_req->req.iovcnt,
							       num_blocks, &mcdma_req->req.dif.dif_ctx);
					if (rc != 0) {
						SPDK_ERRLOG("DIF generation failed\n");
						mcdma_req->state = MCDMA_REQUEST_STATE_COMPLETED;
						spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
						break;
					}
				}

				assert(mcdma_req->req.dif.elba_length >= mcdma_req->req.length);
				/* set extended length before IO operation */
				mcdma_req->req.length = mcdma_req->req.dif.elba_length;
			}

			if (mcdma_req->req.cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
				if (mcdma_req->fused_failed) {
					/* This request failed FUSED semantics.  Fail it immediately, without
					 * even sending it to the target layer.
					 */
					rsp->status.sct = SPDK_NVME_SCT_GENERIC;
					rsp->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
					mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_COMPLETE;
					break;
				}

				if (mcdma_req->fused_pair == NULL ||
				    mcdma_req->fused_pair->state != MCDMA_REQUEST_STATE_READY_TO_EXECUTE) {
					/* This request is ready to execute, but either we don't know yet if it's
					 * valid - i.e. this is a FIRST but we haven't received the next
					 * request yet or the other request of this fused pair isn't ready to
					 * execute.  So break here and this request will get processed later either
					 * when the other request is ready or we find that this request isn't valid.
					 */
					break;
				}
			}

			/* If we get to this point, and this request is a fused command, we know that
			 * it is part of valid sequence (FIRST followed by a SECOND) and that both
			 * requests are READY_TO_EXECUTE. So call spdk_nvmf_request_exec() both on this
			 * request, and the other request of the fused pair, in the correct order.
			 * Also clear the ->fused_pair pointers on both requests, since after this point
			 * we no longer need to maintain the relationship between these two requests.
			 */
			if (mcdma_req->req.cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_SECOND) {
				assert(mcdma_req->fused_pair != NULL);
				assert(mcdma_req->fused_pair->fused_pair != NULL);
				mcdma_req->fused_pair->state = MCDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&mcdma_req->fused_pair->req);
				mcdma_req->fused_pair->fused_pair = NULL;
				mcdma_req->fused_pair = NULL;
			}
			mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(&mcdma_req->req);
			
			//Do not consider below
			if (mcdma_req->req.cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_FIRST) 
			{
				assert(mcdma_req->fused_pair != NULL);
				assert(mcdma_req->fused_pair->fused_pair != NULL);
				mcdma_req->fused_pair->state = MCDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&mcdma_req->fused_pair->req);
				mcdma_req->fused_pair->fused_pair = NULL;
				mcdma_req->fused_pair = NULL;
			}
			break;
		//case MCDMA_REQUEST_STATE_KERNEL_PROCESSING:
		//	mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
		//	spdk_nvmf_request_exec(&mcdma_req->req);
		//	break;
		case MCDMA_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_EXECUTING, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into MCDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			//SPDK_DEBUGLOG(nvmf,"Time Trace5 %lf s\n",(double)spdk_get_ticks()/spdk_get_ticks_hz());
		
			break;
		case MCDMA_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_EXECUTED, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);
			mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_COMPLETE;
			
			//if kernel has not finished, continue to execute
			//ignore kernel begin and kernel end
			//SPDK_DEBUGLOG(nvmf,"Time Trace6 %lf s\n",(double)spdk_get_ticks()/spdk_get_ticks_hz());
			if((!nvmf_qpair_is_admin_queue(mcdma_req->req.qpair))&&((mcdma_req->req.cmd->nvme_cmd.nsid & SLM_MASK) != 0)//MEMORY NAMESPACE NSID
			&&(mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_READ||
				mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_WRITE||
				mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_FILL||
				mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_COPY)){
				SPDK_DEBUGLOG(nvmf,"ENTER XX!\n");
				struct spdk_nvme_cmd* cmd = &(mcdma_req->req.cmd->nvme_cmd);
				struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
				unsigned long long starting_bytes = cmd->cdw11 << 32 | cmd->cdw10;
				int read_or_write_length = cmd->cdw12;
				unsigned long long len = (mcdma_req->req.cmd->nvme_cmd.rsvd2) | 
											(mcdma_req->req.cmd->nvme_cmd.rsvd3 << 32);
				unsigned char copy_format = mcdma_req->req.cmd->nvme_cmd.cdw12 >> 8;
				unsigned char nr = mcdma_req->req.cmd->nvme_cmd.cdw12+1;//0 based number of ranges
				//struct spdk_nvmf_ctrlr *ctrlr = mcdma_req->req.qpair->ctrlr;
				//TODO Current Namespace is locked to 1,
				//Does not support multiple stroage namespace!!!!!!
				//struct spdk_nvmf_ns *ns = _nvmf_subsystem_get_ns(ctrlr->subsys, 1);
				//TODO Set More bsize in the future！
				unsigned int bsize = PAGE_SIZE;//spdk_bdev_get_block_size(ns->bdev);
				void* ns_vaddr,*ns_paddr;
				int ret = spdk_hlsacccompute_devmem_lookup(dev,cmd->nsid&(~SLM_MASK),&(ns_vaddr),&(ns_paddr));
				struct  handc_ctx* ctx = (struct handc_ctx*) mcdma_req->data_buf;
				if(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_SLM_WRITE){
					//If Need To Fetch Data
					if(ctx->fsm_state == FETCH_DATA){
						mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
						unsigned long long* prp_list = (unsigned long long*)mcdma_req->sgl_buf;
						for(int i=0;i<16;i++){
							SPDK_DEBUGLOG(nvmf,"GET PRP2 ADDRESS%llx\n",prp_list[i]);
						}
						ctx->from_size = 1;
						ctx->to_size = 1;
						ctx->from_iovecs[0].iov_base = NULL;
						ctx->from_iovecs[0].paddr = cmd->dptr.prp.prp1;
						ctx->from_iovecs[0].iov_len = PAGE_SIZE;
						ctx->to_iovecs[0].iov_base = ns_vaddr+starting_bytes;
						ctx->to_iovecs[0].iov_len = PAGE_SIZE;
						ctx->to_iovecs[0].paddr = spdk_vtophys(ctx->to_iovecs[0].iov_base,NULL);
						starting_bytes += PAGE_SIZE;
						read_or_write_length -= PAGE_SIZE;
						for(int i=1;i<=128&&(read_or_write_length>0);i++){
							++(ctx->from_size);
							++(ctx->to_size);
							ctx->from_iovecs[i].iov_base = NULL;
							ctx->from_iovecs[i].paddr = prp_list[i-1];
							unsigned int incr_bytes = PAGE_SIZE<read_or_write_length?PAGE_SIZE:read_or_write_length;
							ctx->from_iovecs[i].iov_len = incr_bytes;
							ctx->to_iovecs[i].iov_base = ns_vaddr+starting_bytes;
							ctx->to_iovecs[i].iov_len = incr_bytes;
							starting_bytes += incr_bytes;
							ctx->to_iovecs[i].paddr = spdk_vtophys(ctx->to_iovecs[i].iov_base,NULL);
							read_or_write_length -= incr_bytes;
						}
						spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
						continue;
					}else if(ctx->fsm_state==END_FETCH_DATA){
						printf("GET DATA ns_vaddr%llx nsid%d\n ",ns_vaddr,cmd->nsid&(~SLM_MASK));
						if(ns_vaddr!=NULL){
							char* dumpdata = (char*)(ns_vaddr);
							printf("%d %d %d %c\n",dumpdata[0],dumpdata[1],dumpdata[2],dumpdata[3]);
						}
					}

				}else if(mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_SLM_READ){
					//First，Try to Get Prp Pages
					//SPDK_DEBUGLOG(nvmf,"ENTER FETCH DATA\n");
					if(ctx->fsm_state == FETCH_DATA){
						SPDK_NOTICELOG(
							"SLM_READ FETCH_DATA req=%p cid=%u mem_id=%u start=%llu len=%d ns_vaddr=%p\n",
							mcdma_req,
							mcdma_req->req.cmd->nvme_cmd.cid,
							cmd->nsid&(~SLM_MASK),
							starting_bytes,
							read_or_write_length,
							ns_vaddr);
						spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
							(uintptr_t)mcdma_req,"srfetda");
						
						mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
						//SPDK_DEBUGLOG(nvmf,"FETCH_DATA REQUEST%llx CTX%llx\n",mcdma_req,ctx);
						unsigned long long* prp_list = (unsigned long long*)mcdma_req->sgl_buf;
						ctx->from_size = 1;
						ctx->to_size = 1;
						ctx->to_iovecs[0].iov_base = NULL;
						ctx->to_iovecs[0].paddr = cmd->dptr.prp.prp1;
						ctx->to_iovecs[0].iov_len = PAGE_SIZE;
						ctx->from_iovecs[0].iov_base = ns_vaddr+starting_bytes;
						ctx->from_iovecs[0].iov_len = PAGE_SIZE;
						ctx->from_iovecs[0].paddr = spdk_vtophys(ctx->from_iovecs[0].iov_base,NULL);
						read_or_write_length -= PAGE_SIZE;
						starting_bytes += PAGE_SIZE;
						for(int i=1;i<64&&(read_or_write_length>0);i++){
							ctx->from_size++;
							ctx->to_size++;
							ctx->to_iovecs[i].iov_base = NULL;
							ctx->to_iovecs[i].paddr = prp_list[i-1];
							unsigned int incr_bytes = PAGE_SIZE<read_or_write_length?PAGE_SIZE:read_or_write_length;
							ctx->to_iovecs[i].iov_len = incr_bytes;
							ctx->from_iovecs[i].iov_base = ns_vaddr+starting_bytes;
							ctx->from_iovecs[i].iov_len = incr_bytes;
							starting_bytes += incr_bytes;
							ctx->from_iovecs[i].paddr = spdk_vtophys(ctx->from_iovecs[i].iov_base,NULL);
							read_or_write_length -= incr_bytes;
						}
						SPDK_DEBUGLOG(nvmf,"GET FROM SIZE%d,TO SIZE%d\n",ctx->from_size,ctx->to_size);
						SPDK_NOTICELOG(
							"SLM_READ FETCH_DATA handc_submit req=%p from_size=%d to_size=%d from0=%p from0_paddr=0x%llx to0_paddr=0x%llx\n",
							mcdma_req,
							ctx->from_size,
							ctx->to_size,
							ctx->from_iovecs[0].iov_base,
							(unsigned long long)ctx->from_iovecs[0].paddr,
							(unsigned long long)ctx->to_iovecs[0].paddr);
						spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
						continue;
					}else{
						SPDK_NOTICELOG(
							"SLM_READ END_FETCH_DATA req=%p cid=%u fsm=%s ready_to_complete\n",
							mcdma_req,
							mcdma_req->req.cmd->nvme_cmd.cid,
							handc_fsm_name(ctx->fsm_state));
						//SPDK_DEBUGLOG(nvmf,"END_FETCH_DATA REQUEST%llx CTX%llx\n",mcdma_req,ctx);
					}
				}else if(mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_SLM_COPY){
					union spdk_nvme_source_range* range = (union spdk_nvme_source_range*)mcdma_req->sgl_buf;
					struct spdk_nvme_kernel* kernel = mcdma_req->req.kernel;
					
					if(copy_format == 3){
						//Simple Copy
						struct spdk_nvme_scc_source_range* scc_range = range;
						//SPDK_NOTICELOG("COPY!\n");
						if(ctx->fsm_state == END_FETCH_DATA){
							spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
								(uintptr_t)mcdma_req, "slmcopyfirstkernel");
							if(nr > 64){
								mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_RESERVATION_CONFLICT;
								continue;
							}
							SPDK_DEBUGLOG(nvmf,"Get NR%d\n",nr);
							mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
							//IF YOU HAS KERNEL YOU WILL MEET BUGS!!!
							ctx->fsm_state = OPERATOR_SOURCE_RANGES;
							if(kernel==NULL){
								mcdma_req->req.kernel = spdk_simple_pool_get(mcdma_req->req.qpair->kernel_inner_buf_pool);
								
								kernel = mcdma_req->req.kernel;
							}
							//memset(mcdma_req->req.kernel,0,4096);
							char* begin_address = ((char*)kernel)+1024;
							for(int k=0;k<nr;k++){
								//Reallocate Space
								kernel->cmds[k] = (struct spdk_nvme_cmd*)(begin_address + k*(sizeof(struct spdk_nvme_cmd)));
							}
							kernel->num_add_to_que = 0;
							kernel->num_finished = 0;
							kernel->num_cmds = 0;
							void* paddr_buf = spdk_simple_pool_get(mcdma_req->req.qpair->kernel_inner_buf_pool);
							unsigned long long paddr = spdk_vtophys(paddr_buf,NULL);
							for(int k=0;k<nr;k++){
								assert(kernel!=NULL);
								SPDK_DEBUGLOG(nvmf,"INDEX%d,SLBA%llx,NLB%llx\n",k,scc_range[k].slba,scc_range[k].nlb);
								kernel->cmds[k]->opc = SPDK_NVME_OPC_READ;
								kernel->cmds[k]->nsid = 1;
								kernel->cmds[k]->cdw10 = scc_range[k].slba;
								kernel->cmds[k]->cdw11 = (scc_range[k].slba >> 32);
								kernel->cmds[k]->cdw12 = scc_range[k].nlb;
								kernel->cmds[k]->dptr.prp.prp2 = paddr;
								kernel->cmds[k]->fuse = SPDK_NVME_CMD_FUSE_NONE;
								
								
							}
							kernel->num_cmds = nr;
							kernel->cmds[0]->dptr.prp.prp1 = spdk_vtophys(ns_vaddr+starting_bytes,NULL);
							//Need To Get Real Block Size!!!!!
							ctx->cur_bytes = starting_bytes + (1+scc_range[0].nlb)*PAGE_SIZE;
							
							if(scc_range[0].nlb>0){
								for(int k=0;k<scc_range[0].nlb;k++){
									((unsigned long long*)paddr_buf)[k] = spdk_vtophys(ns_vaddr+starting_bytes+PAGE_SIZE*(1+k),NULL);
								}
							}
							if(scc_range[0].nlb==1)  kernel->cmds[0]->dptr.prp.prp2 = ((unsigned long long*)paddr_buf)[0];
							ctx->prp_buf = paddr_buf;
							
							spdk_nvmf_request_exec(&(mcdma_req->req));
							continue;
						}else if(ctx->fsm_state == OPERATOR_SOURCE_RANGES){
							spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_HLS_EXEC, 0, 0,
								(uintptr_t)mcdma_req, "slmcopysecondkernel");
							kernel->num_finished++;
							int sel = kernel->num_finished;
							if(kernel->num_finished<kernel->num_cmds){
								mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
								kernel->cmds[sel]->dptr.prp.prp1 = spdk_vtophys(ns_vaddr+ctx->cur_bytes,NULL);
								void* paddr_buf = ctx->prp_buf;
								if(scc_range[sel].nlb>0){
									for(int k=0;k<scc_range[sel].nlb;k++){
										((unsigned long long*)paddr_buf)[k] = spdk_vtophys(ns_vaddr+ctx->cur_bytes+PAGE_SIZE*(1+k),NULL);
									}
								}
								if(scc_range[sel].nlb==1)  kernel->cmds[sel]->dptr.prp.prp2 = ((unsigned long long*)paddr_buf)[sel];
								ctx->cur_bytes += (scc_range[sel].nlb+1)*PAGE_SIZE;
								spdk_nvmf_request_exec(&(mcdma_req->req));
								
							}else{
								if(ctx->prp_buf!=NULL)
									spdk_simple_pool_put(mcdma_req->req.qpair->kernel_inner_buf_pool,ctx->prp_buf);
									ctx->prp_buf = NULL;
								if(mcdma_req->req.kernel!=NULL){
									spdk_simple_pool_put(mcdma_req->req.qpair->kernel_inner_buf_pool,mcdma_req->req.kernel);
									mcdma_req->req.kernel = NULL;
								}
							}
							continue;
						}

					}else if(copy_format == 4){
						//SLM Copy
						struct spdk_nvme_mc_source_range* mc_range = range;
						for(int k=0;k<nr;k++){
							void* sns_vaddr,*sns_paddr;
							int snsid = mc_range[k].snsid;
							int nb = mc_range[k].nbyte;
							int saddr = mc_range[k].saddr;
							int ret = spdk_hlsacccompute_devmem_lookup(dev,(snsid)&(~SLM_MASK),&(sns_vaddr),&(sns_paddr));
							if(ret>=0&&sns_vaddr!=NULL){
								//May Has BUG ! Failed to lookup slm memory size!!!
								
								memcpy(ns_vaddr+starting_bytes,sns_vaddr+saddr,nb);
								
							}
							starting_bytes += nb;
						}
					}else{
						mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
					}
					continue;
				}
			}else if(mcdma_req->req.cmd->nvme_cmd.opc==SPDK_NVME_OPC_COPY&&
				((uint8_t)(mcdma_req->req.cmd->nvme_cmd.cdw12 >> 8)==0x4)){
				struct spdk_nvme_cmd cmd = mcdma_req->req.cmd->nvme_cmd;
				uint64_t sdlba = cmd.cdw10 | (cmd.cdw11 << 32);
				uint8_t nr = cmd.cdw12+1;
				struct  handc_ctx* ctx = (struct handc_ctx*) mcdma_req->data_buf;
				//Simple Copy
				struct spdk_nvme_kernel* kernel = mcdma_req->req.kernel;
				if(kernel==NULL){
					mcdma_req->req.kernel = spdk_simple_pool_get(mcdma_req->req.qpair->kernel_inner_buf_pool);
					kernel = mcdma_req->req.kernel;
				}
				char* begin_address = ((char*)kernel)+1024;
				for(int k=0;k<nr;k++){
					//Reallocate Space
					kernel->cmds[k] = (struct spdk_nvme_cmd*)(begin_address+k*(sizeof(struct spdk_nvme_cmd)));
				}
				struct spdk_nvme_mc_source_range* mc_range = mcdma_req->sgl_buf;
				if(ctx->fsm_state == END_FETCH_DATA){
					if(nr > 64){
						mcdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_RESERVATION_CONFLICT;
						continue;
					}
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
					//IF YOU HAS KERNEL YOU WILL MEET BUGS!!!
					ctx->fsm_state = OPERATOR_SOURCE_RANGES;
					kernel->num_add_to_que = 0;
					kernel->num_finished = 0;
					kernel->num_cmds = 0;
					void* paddr_buf = spdk_simple_pool_get(mcdma_req->req.qpair->kernel_inner_buf_pool);
					void* ns_vaddr,*ns_paddr; 
				    unsigned long long starting_bytes = sdlba;
					for(int k=0;k<nr;k++){
						assert(kernel!=NULL);
						kernel->cmds[k]->opc = SPDK_NVME_OPC_WRITE;
						kernel->cmds[k]->nsid = 1;
						kernel->cmds[k]->cdw10 = cmd.cdw10;//slba
						kernel->cmds[k]->cdw11 = cmd.cdw11;//slba
						//Need To Change to Block size in the future
						kernel->cmds[k]->cdw12 = (mc_range[k].nbyte/PAGE_SIZE)-1;
						kernel->cmds[k]->dptr.prp.prp2 = spdk_vtophys(paddr_buf,NULL);
					}
					kernel->num_cmds = nr;
					struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
					spdk_hlsacccompute_devmem_lookup(dev,(mc_range[0].snsid)&(~SLM_MASK),&ns_vaddr,&ns_paddr);
					ns_vaddr += mc_range[0].saddr;
					kernel->cmds[0]->dptr.prp.prp1 = spdk_vtophys(ns_vaddr,NULL);
					//Need To Get Real Block Size!!!!!
					ctx->cur_bytes = sdlba + (mc_range[0].nbyte);
					
					if(mc_range[0].nbyte>PAGE_SIZE){
						for(int k=0;k<(mc_range[0].nbyte/PAGE_SIZE);k++){
							((unsigned long long*)paddr_buf)[k] = spdk_vtophys(ns_vaddr+mc_range[0].saddr+PAGE_SIZE*(1+k),NULL);
						}
					}
					ctx->prp_buf = paddr_buf;
					
					spdk_nvmf_request_exec(&(mcdma_req->req));
					continue;
				}else if(ctx->fsm_state == OPERATOR_SOURCE_RANGES){
					kernel->num_finished++;
					int sel = kernel->num_finished;
					if((kernel->num_finished)<(kernel->num_cmds)){
						mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
						void* ns_vaddr,*ns_paddr;
						struct spdk_hlsacccompute_dev* dev = &(mcdma_req->qpair->device->compute);
						spdk_hlsacccompute_devmem_lookup(dev,(mc_range[0].snsid)&(~SLM_MASK),&ns_vaddr,&ns_paddr);
						//SPDK_ERRLOG("CUrrent Doesnot SUpport too much source ranges!\n");
						kernel->cmds[sel]->dptr.prp.prp1 = spdk_vtophys(ns_vaddr+mc_range[sel].saddr,NULL);
						kernel->cmds[sel]->cdw10 = ctx->cur_bytes;
						kernel->cmds[sel]->cdw11 = (ctx->cur_bytes>>32);
						void* paddr_buf = ctx->prp_buf;
						if(mc_range[sel].nbyte>PAGE_SIZE){
							for(int k=0;k<(mc_range[sel].nbyte/PAGE_SIZE);k++){
								((unsigned long long*)paddr_buf)[k] = spdk_vtophys(ns_vaddr+mc_range[sel].saddr+PAGE_SIZE*(1+k),NULL);
							}
						}
						ctx->cur_bytes += (mc_range[sel].nbyte);
						spdk_nvmf_request_exec(&(mcdma_req->req));
						continue;
					}else{
						if(ctx->prp_buf!=NULL){
							spdk_simple_pool_put(mcdma_req->req.qpair->kernel_inner_buf_pool,ctx->prp_buf);
							ctx->prp_buf = NULL;
						}
						if(mcdma_req->req.kernel!=NULL){
							spdk_simple_pool_put(mcdma_req->req.qpair->kernel_inner_buf_pool,mcdma_req->req.kernel);
							mcdma_req->req.kernel = NULL;
						}
						continue;
					}
				
				}
			}else if((!nvmf_qpair_is_admin_queue(mcdma_req->req.qpair))&&mcdma_req->req.cmd->nvme_cmd.nsid==2&&
			mcdma_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_PROGRAM_EXECUTE){
				mcdma_req->req.rsp->nvme_cpl.sqid = rqpair->qpair.qid;
				mcdma_req->req.rsp->nvme_cpl.cid = mcdma_req->req.cmd->nvme_cmd.cid;
				SPDK_DEBUGLOG(nvmf,"BEGIN TO FETCH CONTEXT!\n");
				//根据当前的需求，分配virtual object
				//请求运行程序，注册返回回调函数
				//首先，获取一个memory range set
				struct spdk_nvme_cmd cmd = mcdma_req->req.cmd->nvme_cmd;
				unsigned short rsid = cmd.rsvd2 >> 16;
				unsigned short pind = cmd.rsvd2;
				unsigned int numr = cmd.rsvd3;
				unsigned int dlen = cmd.mptr;
				//cparam1 ctx initialize sel
				unsigned long long cparam1 = cmd.cdw10 | (cmd.cdw11 << 32);
				//cparam2 request priority (7:0) request type(sw,hw,fusion)(15:8)
				unsigned long long cparam2 = cmd.cdw12 | (cmd.cdw13 << 32);
				unsigned char need_init_ctx[8] = {0};
				unsigned char priority = cparam2;

				unsigned long long temp_val = cparam1;
				unsigned char need_init_num = 0;

				struct spdk_nvmf_mcdma_device* mdev = mcdma_req->qpair->device;
				struct handc_ctx* ctx = (struct handc_ctx*) mcdma_req->data_buf;
				struct spdk_hlsacccompute_request* request = ctx->hls_request;
				struct spdk_hlsacccompute_program* program = request->program;
				request->priority = priority;
				for(int i=0;i<program->apply_operators_num;i++){
					need_init_ctx[i] = temp_val;
					temp_val = temp_val >> 8;
					if(need_init_ctx[i]==1){
						need_init_num++;
					}
				}
				if(ctx->fsm_state == NON_OP){
					SPDK_DEBUGLOG(nvmf,"NONOPERATOR\n");
					continue;
				}else if(ctx->fsm_state == FETCH_DATA){
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
					SPDK_DEBUGLOG(nvmf,"APPLY OPERATORS NUM%d NEED INIT NUM%d\n",program->apply_operators_num,need_init_num);
					int m=0;
					ctx->from_size = need_init_num;
					ctx->to_size = need_init_num;
					for(int k=0;k<program->apply_operators_num;k++){
						if(need_init_ctx[k]==1){
							ctx->from_iovecs[m].iov_base = NULL;
							ctx->from_iovecs[m].iov_len = 4096;
							if(k==0) ctx->from_iovecs[m].paddr = cmd.dptr.prp.prp1;
							else if(need_init_ctx[0]==1) ctx->from_iovecs[m].paddr = ((unsigned long long*)(mcdma_req->sgl_buf))[m-1];
							else ctx->from_iovecs[m].paddr = ((unsigned long long*)(mcdma_req->sgl_buf))[m];
							ctx->to_iovecs[m].iov_base = (unsigned long long)(request->acccontext[m])+4096;
							ctx->to_iovecs[m].iov_len =  4096;
							ctx->to_iovecs[m].paddr = spdk_vtophys(ctx->to_iovecs[m].iov_base,NULL);
							SPDK_DEBUGLOG(nvmf,"FETCH CONTEXT ADDRESS INDEX m %d VADDR%llx PADDR%llx\n",m,ctx->to_iovecs[m].iov_base,ctx->to_iovecs[m].paddr);
							++m;
						}
					}
					spdk_thread_send_msg(rqpair->device->handc_thread,compute_handc_op,ctx);
					continue;
				}else if(ctx->fsm_state == END_FETCH_DATA){
					//SPDK_NOTICELOG("END FETCH DATA EXECUTING REQUEST ADDRESS%llx\n",request);
					ctx->fsm_state = NON_OP;
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
						mcdma_req->impl_thread = spdk_get_thread();
						//DUMP CONTEXT
						unsigned int *staged_da = (unsigned int *)((unsigned long long)(request->acccontext[0])+4096);
						memcpy(request->acccontext[0]->context.static_data,(void*)((unsigned long long)(request->acccontext[0])+4096),2048);
						unsigned int *da = (unsigned int*)(request->acccontext[0]->context.static_data);
						SPDK_NOTICELOG("LWE_CTX_VERIFY context已搬入 req=%p accctx=%p context_phy=0x%llx staged_u32=[%u,%u,%u,%u,%u] static_u32=[%u,%u,%u,%u,%u]\n",
							request,
							request->acccontext[0],
							(unsigned long long)request->acccontext[0]->context_phy,
							staged_da[0],
							staged_da[1],
							staged_da[2],
							staged_da[3],
							staged_da[4],
							da[0],
							da[1],
							da[2],
							da[3],
							da[4]);
						//SPDK_NOTICELOG("CONTEXT DUMP%x %x %x %x\n",da[0],da[1],da[2],da[3]);
						//usleep(10);
						if(request->priority<120)//Preempt if priority is high
							spdk_thread_send_msg(mdev->compute_thread,hlsacccompute_run_request_unpreempt,request);
					else
						spdk_thread_send_msg(mdev->compute_thread,hlsacccompute_run_request_preempt,request);
					continue;
				}
			}
			if(spdk_nvme_opc_is_kernel(mcdma_req->req.cmd->nvme_cmd.opc)){
				if(spdk_unlikely(mcdma_req->req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INTERNAL_DEVICE_ERROR)){
					//如果不是顺序一条一条执行，这个错误处理会出错
					//如果后一条指令先于前一条报错
					break;
				}
				if(spdk_likely((++(mcdma_req->req.kernel->num_finished)) < mcdma_req->req.kernel->num_cmds)){
					SPDK_DEBUGLOG(nvmf,"Completly finish one command of kernel, finished command number %u\n",mcdma_req->req.kernel->num_finished);
					spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_KERNEL_EXEC, 0, 0,
							(uintptr_t)mcdma_req, (uintptr_t)mcdma_req->req.kernel->num_add_to_que);
				
					mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTING;
					uint16_t opc = mcdma_req->req.kernel->cmds[mcdma_req->req.kernel->num_add_to_que]->opc;

					spdk_nvmf_request_exec(&mcdma_req->req);
					
					break;
				} else {
					struct spdk_nvmf_mcdma_device* mdev = mcdma_req->qpair->device;
					mdev->all_kernel_executed_time += spdk_get_ticks() -  mdev->one_kernel_start_execute_time;
				}

				
			}
			if (spdk_unlikely(mcdma_req->req.dif_enabled)) {
				/* restore the original length */
				mcdma_req->req.length = mcdma_req->req.dif.orig_length;

				if (mcdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
					struct spdk_dif_error error_blk;

					num_blocks = SPDK_CEIL_DIV(mcdma_req->req.dif.elba_length, mcdma_req->req.dif.dif_ctx.block_size);
					if (!mcdma_req->req.stripped_data) {
						rc = spdk_dif_verify(mcdma_req->req.iov, mcdma_req->req.iovcnt, num_blocks,
								     &mcdma_req->req.dif.dif_ctx, &error_blk);
					} else {
						rc = spdk_dif_verify_copy(mcdma_req->req.stripped_data->iov,
									  mcdma_req->req.stripped_data->iovcnt,
									  mcdma_req->req.iov, mcdma_req->req.iovcnt, num_blocks,
									  &mcdma_req->req.dif.dif_ctx, &error_blk);
					}
					if (rc) {
						struct spdk_nvme_cpl *rsp = &mcdma_req->req.rsp->nvme_cpl;
						SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n", error_blk.err_type,
							    error_blk.err_offset);
						rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
						rsp->status.sc = nvmf_mcdma_dif_error_to_compl_status(error_blk.err_type);
						mcdma_req->state = MCDMA_REQUEST_STATE_READY_TO_COMPLETE;
						STAILQ_REMOVE(&rqpair->pending_mcdma_write_queue, mcdma_req, spdk_nvmf_mcdma_request, state_link);
					}
				}
			}
			break;
		case MCDMA_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_READY_TO_COMPLETE, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);
			rc = request_transfer_out(&mcdma_req->req, &data_posted);
			assert(rc == 0); /* No good way to handle this currently */
			if (rc) {
				mcdma_req->state = MCDMA_REQUEST_STATE_COMPLETED;
			} else {
				mcdma_req->state = MCDMA_REQUEST_STATE_COMPLETING;
			}
			break;
		case MCDMA_REQUEST_STATE_COMPLETING:
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_COMPLETING, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into MCDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case MCDMA_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_MCDMA_REQUEST_STATE_COMPLETED, 0, 0,
					  (uintptr_t)mcdma_req, (uintptr_t)rqpair);

			rqpair->poller->stat.request_latency += spdk_get_ticks() - mcdma_req->receive_tsc;
			_nvmf_mcdma_request_free(mcdma_req, rtransport);
			break;
		case MCDMA_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		if (mcdma_req->state != prev_state) {
			progress = true;
		}
	} while (mcdma_req->state != prev_state);

	return progress;
}

static void
nvmf_mcdma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_MCDMA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_MCDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_MCDMA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_MCDMA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_MCDMA_MIN_IO_BUFFER_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_MCDMA_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_MCDMA_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_MCDMA_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_MCDMA_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_MCDMA_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->transport_specific =      NULL;
}




static struct spdk_nvmf_mcdma_device *nvmf_mcdma_get_device(void)
{
	struct spdk_nvmf_mcdma_device *mdev = calloc(1, sizeof(struct spdk_nvmf_mcdma_device));
	mdev->mcdma = spdk_axi_dma_get_device(g_mcdma_dev);

	mdev->name = "qdma01000";
	
	return mdev;
}

static int
nvmf_mcdma_destroy(struct spdk_nvmf_transport *transport,
		  spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_mcdma_transport	*rtransport;
	struct spdk_nvmf_mcdma_port	*port, *port_tmp;
	struct spdk_nvmf_mcdma_device	*device, *device_tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_mcdma_transport, transport);

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->ports, port, link);
		// mcdma_destroy_id(port->id);
		free(port);
	}

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		TAILQ_REMOVE(&rtransport->devices, device, link);
		// spdk_mcdma_free_mem_map(&device->map);
		// if (device->pd) {
		// 	if (!g_nvmf_hooks.get_ibv_pd) {
		// 		ibv_dealloc_pd(device->pd);
		// 	}
		// }
		free(device);
	}

	pthread_mutex_destroy(&rtransport->lock);
	free(rtransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
	return 0;
}

static struct spdk_nvmf_transport *
nvmf_mcdma_create(struct spdk_nvmf_transport_opts *opts)
{
	// int rc;
	struct spdk_nvmf_mcdma_transport *qtransport;
	struct spdk_nvmf_mcdma_device	*device;
	// struct ibv_context		**contexts;
	// uint32_t			i;
	// int				flag;
	// uint32_t			sge_count;
	//
	uint32_t			min_shared_buffers;
	uint32_t			min_in_capsule_data_size;
	int				max_device_sge = SPDK_NVMF_MAX_SGL_ENTRIES;
	pthread_mutexattr_t		attr;

	qtransport = calloc(1, sizeof(*qtransport));
	if (!qtransport) {
		return NULL;
	}

	if (pthread_mutexattr_init(&attr)) {
		SPDK_ERRLOG("pthread_mutexattr_init() failed\n");
		free(qtransport);
		return NULL;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		SPDK_ERRLOG("pthread_mutexattr_settype() failed\n");
		pthread_mutexattr_destroy(&attr);
		free(qtransport);
		return NULL;
	}

	if (pthread_mutex_init(&qtransport->lock, &attr)) {
		SPDK_ERRLOG("pthread_mutex_init() failed\n");
		pthread_mutexattr_destroy(&attr);
		free(qtransport);
		return NULL;
	}

	
	pthread_mutexattr_destroy(&attr);

	TAILQ_INIT(&qtransport->devices);
	TAILQ_INIT(&qtransport->ports);

	qtransport->transport.ops = &spdk_nvmf_transport_mcdma;
	qtransport->mcdma_opts.num_cqe = SPDK_NVMF_MCDMA_DEFAULT_MAX_QUEUE_DEPTH;
	// qtransport->mcdma_opts.max_srq_depth = SPDK_NVMF_MCDMA_DEFAULT_SRQ_DEPTH;
	// qtransport->mcdma_opts.no_srq = SPDK_NVMF_MCDMA_DEFAULT_NO_SRQ;
	// qtransport->mcdma_opts.acceptor_backlog = SPDK_NVMF_MCDMA_ACCEPTOR_BACKLOG;
	// qtransport->mcdma_opts.no_wr_batching = SPDK_NVMF_MCDMA_DEFAULT_NO_WR_BATCHING;
	// if (opts->transport_specific != NULL &&
	//     spdk_json_decode_object_relaxed(opts->transport_specific, mcdma_transport_opts_decoder,
	// 				    SPDK_COUNTOF(mcdma_transport_opts_decoder),
	// 				    &qtransport->mcdma_opts)) {
	// 	SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
	// 	nvmf_mcdma_destroy(&qtransport->transport, NULL, NULL);
	// 	return NULL;
	// }

	SPDK_INFOLOG(nvmf, "*** MCDMA Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, num_cqe=%d abort_timeout_sec=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     qtransport->mcdma_opts.num_cqe,
		     opts->abort_timeout_sec);

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	if (opts->num_shared_buffers < (SPDK_NVMF_MAX_SGL_ENTRIES * 2)) {
		SPDK_ERRLOG("The number of shared data buffers (%d) is less than"
			    "the minimum number required to guarantee that forward progress can be made (%d)\n",
			    opts->num_shared_buffers, (SPDK_NVMF_MAX_SGL_ENTRIES * 2));
		nvmf_mcdma_destroy(&qtransport->transport, NULL, NULL);
		return NULL;
	}

	min_shared_buffers = spdk_env_get_core_count() * opts->buf_cache_size;
	if (min_shared_buffers > opts->num_shared_buffers) {
		SPDK_ERRLOG("There are not enough buffers to satisfy"
			    "per-poll group caches for each thread. (%" PRIu32 ")"
			    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
		SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
		nvmf_mcdma_destroy(&qtransport->transport, NULL, NULL);
		return NULL;
	}

	// sge_count = opts->max_io_size / opts->io_unit_size;
	// if (sge_count > NVMF_DEFAULT_TX_SGE) {
	// 	SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
	// 	nvmf_mcdma_destroy(&qtransport->transport, NULL, NULL);
	// 	return NULL;
	// }

	device = nvmf_mcdma_get_device();

	TAILQ_INSERT_TAIL(&qtransport->devices, device, link);

	min_in_capsule_data_size = sizeof(struct spdk_nvme_sgl_descriptor) * SPDK_NVMF_MAX_SGL_ENTRIES;
	if (opts->in_capsule_data_size < min_in_capsule_data_size) {
		SPDK_WARNLOG("In capsule data size is set to %u, this is minimum size required to support msdbd=16\n",
			     min_in_capsule_data_size);
		opts->in_capsule_data_size = min_in_capsule_data_size;
	}

	if (opts->io_unit_size * max_device_sge < opts->max_io_size) {
		/* divide and round up. */
		opts->io_unit_size = (opts->max_io_size + max_device_sge - 1) / max_device_sge;

		/* round up to the nearest 4k. */
		opts->io_unit_size = (opts->io_unit_size + NVMF_DATA_BUFFER_ALIGNMENT - 1) & ~NVMF_DATA_BUFFER_MASK;

		opts->io_unit_size = spdk_max(opts->io_unit_size, SPDK_NVMF_MCDMA_MIN_IO_BUFFER_SIZE);
		SPDK_DEBUGLOG(nvmf, "Adjusting the io unit size to fit the device's maximum I/O size. New I/O unit size %u\n",
			       opts->io_unit_size);
	}

	//Support For memory range sets
	//HLSACCCOMPUTE TAG
	
	if(device->memrangeset_hash_tables==NULL){
		device->memrangeset_hash_tables = spdk_cuckoo_table_create(spdk_env_get_socket_id(spdk_env_get_current_core())
																			,sizeof(struct memory_range_sets));
		
		if(device->memrangeset_hash_tables==NULL){
			SPDK_ERRLOG("Unable to allocate memory_range_set\n");
		}
		device->memrangeset_next_id = 0;
	}
	device->compute_thread = NULL;
	
	// if (rc < 0) {
	// 	nvmf_mcdma_destroy(&qtransport->transport, NULL, NULL);
	// 	return NULL;
	// }

	/* Set up poll descriptor array to monitor events from MCDMA and IB
	 * in a single poll syscall
	 */
	// qtransport->npoll_fds = i + 1;
	// i = 0;
	// qtransport->poll_fds = calloc(qtransport->npoll_fds, sizeof(struct pollfd));
	// if (qtransport->poll_fds == NULL) {
	// 	SPDK_ERRLOG("poll_fds allocation failed\n");
	// 	nvmf_mcdma_destroy(&qtransport->transport, NULL, NULL);
	// 	return NULL;
	// }

	// qtransport->poll_fds[i].fd = qtransport->event_channel->fd;
	// qtransport->poll_fds[i++].events = POLLIN;

	// TAILQ_FOREACH_SAFE(device, &qtransport->devices, link, tmp) {
	// 	qtransport->poll_fds[i].fd = device->context->async_fd;
	// 	qtransport->poll_fds[i++].events = POLLIN;
	// }

	// qtransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_mcdma_accept, &qtransport->transport,
	// 			    opts->acceptor_poll_rate);
	// if (!qtransport->accept_poller) {
	// 	nvmf_mcdma_destroy(&qtransport->transport, NULL, NULL);
	// 	return NULL;
	// }

	return &qtransport->transport;
}

static void
nvmf_mcdma_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_mcdma_transport	*qtransport;
	assert(w != NULL);

	qtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_mcdma_transport, transport);
	spdk_json_write_named_int32(w, "num_cqe", qtransport->mcdma_opts.num_cqe);
}

static int
nvmf_mcdma_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvmf_listen_opts *listen_opts)
{
	struct spdk_nvmf_mcdma_transport	*rtransport;
	struct spdk_nvmf_mcdma_device	*device;
	struct spdk_nvmf_mcdma_port	*port;
	
	//
	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_mcdma_transport, transport);
	// assert(rtransport->event_channel != NULL);

	pthread_mutex_lock(&rtransport->lock);
	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		pthread_mutex_unlock(&rtransport->lock);
		return -ENOMEM;
	}

	port->trid = trid;

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (!strcmp(device->name, port->trid->traddr)) {
			port->device = device;
			break;
		}
	}
	if (!port->device) {
		SPDK_ERRLOG("Accepted a connection with traddr %s, but unable to find a corresponding device.\n",
			    port->trid->traddr);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return -EINVAL;
	}

	for (int k = 0; k < SPDK_NVMF_MCDMA_MAX_CTRLRS_PER_DISK; k++) {
		for (int i = 0; i <= SPDK_NVMF_MCDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR; i++) {
			int ret = nvmf_mcdma_add_rqpair(&rtransport->transport, port->device, SPDK_NVMF_MCDMA_DEFAULT_MAX_QUEUE_DEPTH, i, k);
			if (ret) {
				SPDK_ERRLOG("Failed to add rqpair: %d\n", ret);
				return -1;
			}
		}
	}
	SPDK_DEBUGLOG(nvmf,"*** NVMe/RDMA Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);
	pthread_mutex_unlock(&rtransport->lock);
	return 0;
}

static void
nvmf_mcdma_stop_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_mcdma_transport *rtransport;
	struct spdk_nvmf_mcdma_port *port, *tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_mcdma_transport, transport);

	pthread_mutex_lock(&rtransport->lock);
	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
			TAILQ_REMOVE(&rtransport->ports, port, link);
			// mcdma_destroy_id(port->id);
			free(port);
			break;
		}
	}

	pthread_mutex_unlock(&rtransport->lock);
}

static void
nvmf_mcdma_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		     struct spdk_nvmf_ctrlr_data *cdata)
{
	
	cdata->nvmf_specific.msdbd = SPDK_NVMF_MAX_SGL_ENTRIES;

	/* Disable in-capsule data transfer for RDMA controller when dif_insert_or_strip is enabled
	since in-capsule data only works with NVME drives that support SGL memory layout */
	if (transport->opts.dif_insert_or_strip) {
		cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	}

	if (cdata->nvmf_specific.ioccsz > ((sizeof(struct spdk_nvme_cmd) + 0x1000) / 16)) {
		SPDK_WARNLOG("RDMA is configured to support up to 16 SGL entries while in capsule"
			     " data is greater than 4KiB.\n");
		SPDK_WARNLOG("When used in conjunction with the NVMe-oF initiator from the Linux "
			     "kernel between versions 5.4 and 5.12 data corruption may occur for "
			     "writes that are not a multiple of 4KiB in size.\n");
	}
}

static void
nvmf_mcdma_discover(struct spdk_nvmf_transport *transport,
		   struct spdk_nvme_transport_id *trid,
		   struct spdk_nvmf_discovery_log_page_entry *entry)
{
	
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	// entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	// entry->tsas.mcdma.mcdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	// entry->tsas.mcdma.mcdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	// entry->tsas.mcdma.mcdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static void
nvmf_mcdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);


static int hac_axi_dma_poller_group(void* ctx){
	struct  spdk_nvmf_mcdma_device* dev = (struct spdk_nvmf_mcdma_device*)ctx;
	int r2 = spdk_axi_dma_poller(dev->compute_rx_channel);
	int r1 = spdk_axi_dma_poller(dev->compute_tx_channel);
	
	return r1|r2;
}

//Host and Device
static inline void nvmf_mcdma_initialize_compute_hac_channel(struct spdk_nvmf_mcdma_device* device){
	int phy_id_begin = 4;
	struct spdk_axi_dma_ch *axi_dma_ch = spdk_malloc(sizeof(struct spdk_axi_dma_ch) * 2, 2, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	int i = 0;
	for (; i < 2; i++)
	{
		axi_dma_ch[i].cmpl_poller = NULL;
		axi_dma_ch[i].thread = spdk_get_thread();
		axi_dma_ch[i].id = phy_id_begin;
		if (i == 0)
		{
		axi_dma_ch[i].env_ch = spdk_env_axi_dma_create_tx_channel(device->mcdma->env_dev, 1024, phy_id_begin, phy_id_begin);
		}
		else
		{
		axi_dma_ch[i].env_ch = spdk_env_axi_dma_create_rx_channel(device->mcdma->env_dev, 1024, phy_id_begin, phy_id_begin);
		}
		if (!axi_dma_ch[i].env_ch)
		{
			SPDK_ERRLOG("Failed To Create TX Channels\n");
		}
		// 创建iopool
		if ((spdk_simple_pool_init(&((axi_dma_ch[i]).io_pool), 2048, sizeof(struct spdk_axi_dma_io))) != 0)
		{
			SPDK_ERRLOG("Failed To Allocate AXI DMA IO POOL FOR CHANNEL %d\n", i);
		}
	}
	device->compute_tx_channel = axi_dma_ch;
	device->compute_rx_channel = axi_dma_ch + 1;
	//Only Register Poller on cmpl poller
	(axi_dma_ch)->cmpl_poller = spdk_poller_register(hac_axi_dma_poller_group,device,0);
	
	device->handc_thread = spdk_get_thread();
}



static struct spdk_nvmf_transport_poll_group *
nvmf_mcdma_poll_group_create(struct spdk_nvmf_transport *transport,
			    struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_mcdma_transport		*rtransport;
	struct spdk_nvmf_mcdma_poll_group	*rgroup;
	struct spdk_nvmf_mcdma_poller		*poller;
	struct spdk_nvmf_mcdma_device		*device;
	// struct spdk_nvmf_mcdma_resource_opts	opts;
	int					num_cqe;
	int					ret;
	struct spdk_mcdma_qp_init_attr qp_init_attr = {};
	//
	poll_group_cnt += 1;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_mcdma_transport, transport);

	rgroup = calloc(1, sizeof(*rgroup));
	if (!rgroup) {
		return NULL;
	}

	TAILQ_INIT(&rgroup->pollers);

	pthread_mutex_lock(&rtransport->lock);
	//if (rtransport->compute_job_ring == NULL) {
	//	rtransport->compute_job_ring = spdk_ring_create(SPDK_RING_TYPE_MP_MC, 16, SPDK_ENV_SOCKET_ID_ANY);
	//	rtransport->compute_job_cmpl_ring = spdk_ring_create(SPDK_RING_TYPE_MP_MC, 16, SPDK_ENV_SOCKET_ID_ANY);
	//}
	TAILQ_FOREACH(device, &rtransport->devices, link) {
		poller = calloc(1, sizeof(*poller));
		if (!poller) {
			SPDK_ERRLOG("Unable to allocate memory for new RDMA poller\n");
			nvmf_mcdma_poll_group_destroy(&rgroup->group);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}

		poller->device = device;
		poller->group = rgroup;

		qp_init_attr.mcdma_dev = device->mcdma;
		qp_init_attr.max_queue_depth = SPDK_NVMF_MCDMA_DEFAULT_MAX_QUEUE_DEPTH;

		poller->last_poll_ticks = spdk_get_ticks();
		poller->poll_unstarted_interval_ticks = 10 * spdk_get_ticks_hz() / 1000; // 10ms

		RB_INIT(&poller->qpairs);
		STAILQ_INIT(&poller->qpairs_pending_send);
		STAILQ_INIT(&poller->qpairs_pending_recv);

		TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);

		int i = spdk_env_get_current_core();
		int num_cores = spdk_env_get_core_count();
		int cnt = 0;

		poller->num_mcdma_qp = (SPDK_NVMF_MCDMA_MAX_QPAIRS + num_cores - 1 - i) / num_cores;

		SPDK_DEBUGLOG(nvmf, "Create %d MCDMA qp on core %u\n", poller->num_mcdma_qp, spdk_env_get_current_core());
		poller->mcdma_qps = calloc(poller->num_mcdma_qp, sizeof(struct spdk_mcdma_qp));

		while (i < SPDK_NVMF_MCDMA_MAX_QPAIRS) {
			struct spdk_mcdma_qp *mcdma_qp = &poller->mcdma_qps[cnt++];
			qp_init_attr.qid = i;
			qp_init_attr.compute_dev = &device->compute;
			ret = mcdma_qpair_create(mcdma_qp, &qp_init_attr);
			if (ret) {
				SPDK_ERRLOG("Failed to ccreate mcdma_qp: %d\n", ret);
			}
			mcdma_qp->poller = poller;
			i += num_cores;
		}

		num_cqe = rtransport->mcdma_opts.num_cqe;

		poller->num_cqe = num_cqe;
		

		//Auto Allocate on last Core, Which may has lower utilization
		if(spdk_env_get_current_core()==(num_cores-1)%(num_cores)){
			//Register Compute Poller On Other 
			//if(device->compute.poller!=NULL){
			//	spdk_poller_unregister(device->compute.poller);
			//}
			device->compute_thread = spdk_get_thread();
			//device->compute.poller = spdk_poller_register(spdk_hlsacccompute_poller,&(device->compute),0);
			nvmf_mcdma_initialize_compute_dev(device);
		}
		//Auto Allocate on second last core
		if(spdk_env_get_current_core()==(num_cores-2)%(num_cores)){
			nvmf_mcdma_initialize_compute_hac_channel(device);
		}
	}

	rtransport->poll_groups[spdk_env_get_current_core()] = rgroup;
	rtransport->num_poll_groups++;

	pthread_mutex_unlock(&rtransport->lock);
	return &rgroup->group;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_mcdma_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_mcdma_transport *rtransport;
	struct spdk_nvmf_mcdma_poll_group *pg;
	struct spdk_nvmf_transport_poll_group *result;

	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_mcdma_transport, transport);

	pthread_mutex_lock(&rtransport->lock);

	if (rtransport->num_poll_groups == 0) {
		pthread_mutex_unlock(&rtransport->lock);
		return NULL;
	}

	pg = rtransport->poll_groups[qpair->qid % rtransport->num_poll_groups];
	result = &pg->group;

	pthread_mutex_unlock(&rtransport->lock);

	return result;
}

static void
nvmf_mcdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_mcdma_poll_group	*rgroup, *next_rgroup;
	struct spdk_nvmf_mcdma_poller		*poller, *tmp;
	struct spdk_nvmf_mcdma_qpair		*qpair, *tmp_qpair;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_mcdma_poll_group, group);
	if (!rgroup) {
		return;
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		TAILQ_REMOVE(&rgroup->pollers, poller, link);

		RB_FOREACH_SAFE(qpair, qpairs_tree, &poller->qpairs, tmp_qpair) {
			nvmf_mcdma_qpair_destroy(qpair);
		}

		// if (poller->srq) {
		// 	if (poller->resources) {
		// 		nvmf_mcdma_resources_destroy(poller->resources);
		// 	}
		// 	spdk_mcdma_srq_destroy(poller->srq);
		// 	SPDK_DEBUGLOG(nvmf, "Destroyed RDMA shared queue %p\n", poller->srq);
		// }

		// if (poller->cq) {
		// 	ibv_destroy_cq(poller->cq);
		// }

		free(poller);
	}

	if (rgroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_mcdma_poll_group_create()
		 * calls this function directly in a failure path. */
		free(rgroup);
		return;
	}

	free(rgroup);
}

// static void
// nvmf_mcdma_qpair_reject_connection(struct spdk_nvmf_mcdma_qpair *rqpair)
// {
// 	if (rqpair->cm_id != NULL) {
// 		nvmf_mcdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
// 	}
// }

static int
nvmf_mcdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_mcdma_poll_group	*rgroup;
	struct spdk_nvmf_mcdma_qpair		*rqpair;
	struct spdk_nvmf_mcdma_device		*device;
	struct spdk_nvmf_mcdma_poller		*poller;
	int					rc;
	//
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_mcdma_poll_group, group);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);

	device = rqpair->device;

	TAILQ_FOREACH(poller, &rgroup->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		SPDK_ERRLOG("No poller found for device.\n");
		return -1;
	}

	rqpair->poller = poller;
	for (int i = 0; i < poller->num_mcdma_qp; i++) {
		if (rqpair->qpair.qid % SPDK_NVMF_MCDMA_MAX_QPAIRS == poller->mcdma_qps[i].chid) {
			rqpair->mcdma_qp = &poller->mcdma_qps[i];
			break;
		}
	}
	
	if (rqpair->mcdma_qp == NULL) {
		SPDK_ERRLOG("Unable to find MCDMA qp for q %u on core %u\n", rqpair->qpair.qid, spdk_env_get_current_core());
	}
	// rqpair->srq = rqpair->poller->srq;

	rc = nvmf_mcdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_mcdma_qpair with qpair=%p\n", qpair);
		rqpair->poller = NULL;
		// rqpair->srq = NULL;
		return -1;
	}

	SPDK_INFOLOG(nvmf, "Add qpair %u ctrlr %u on core %u\n", rqpair->qp_num, rqpair->ctrlr_num, spdk_env_get_current_core());

	//SPDK_DEBUGLOG(nvmf,"RQPAIR ADDRESS %llx\n",poller->qpairs);
	RB_INSERT(qpairs_tree, &poller->qpairs, rqpair);

	// rc = nvmf_mcdma_event_accept(rqpair->cm_id, rqpair);
	// if (rc) {
	// 	/* Try to reject, but we probably can't */
	// 	nvmf_mcdma_qpair_reject_connection(rqpair);
	// 	return -1;
	// }

	// nvmf_mcdma_update_ibv_state(rqpair);

	return 0;
}

static int
nvmf_mcdma_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_mcdma_qpair		*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);
	assert(group->transport->tgt != NULL);

	rqpair->destruct_channel = spdk_get_io_channel(group->transport->tgt);

	if (!rqpair->destruct_channel) {
		SPDK_WARNLOG("failed to get io_channel, qpair %p\n", qpair);
		return 0;
	}

	/* Sanity check that we get io_channel on the correct thread */
	if (qpair->group) {
		assert(qpair->group->thread == spdk_io_channel_get_thread(rqpair->destruct_channel));
	}

	return 0;
}

static int
nvmf_mcdma_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_mcdma_request	*mcdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_mcdma_request, req);
	struct spdk_nvmf_mcdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_mcdma_transport, transport);
	// struct spdk_nvmf_mcdma_qpair *rqpair = SPDK_CONTAINEROF(mcdma_req->req.qpair,
	// 				      struct spdk_nvmf_mcdma_qpair, qpair);

	/*
	 * AER requests are freed when a qpair is destroyed. The recv corresponding to that request
	 * needs to be returned to the shared receive queue or the poll group will eventually be
	 * starved of RECV structures.
	 */
	// if (rqpair->srq && mcdma_req->recv) {
	// 	int rc;
	// 	struct ibv_recv_wr *bad_recv_wr;

	// 	spdk_mcdma_srq_queue_recv_wrs(rqpair->srq, &mcdma_req->recv->wr);
	// 	rc = spdk_mcdma_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
	// 	if (rc) {
	// 		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
	// 	}
	// }

	_nvmf_mcdma_request_free(mcdma_req, rtransport);
	return 0;
}

static int
nvmf_mcdma_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_mcdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_mcdma_transport, transport);
	struct spdk_nvmf_mcdma_request	*mcdma_req = SPDK_CONTAINEROF(req,
			struct spdk_nvmf_mcdma_request, req);
	// struct spdk_nvmf_mcdma_qpair     *rqpair = SPDK_CONTAINEROF(mcdma_req->req.qpair,
	// 		struct spdk_nvmf_mcdma_qpair, qpair);

	// if (rqpair->ibv_state != IBV_QPS_ERR) {
		/* The connection is alive, so process the request as normal */
		mcdma_req->state = MCDMA_REQUEST_STATE_EXECUTED;
	// } else {
		/* The connection is dead. Move the request directly to the completed state. */
		// mcdma_req->state = MCDMA_REQUEST_STATE_COMPLETED;
	// }

	nvmf_mcdma_request_process(rtransport, mcdma_req);
	

	return 0;
}

static void
nvmf_mcdma_close_qpair(struct spdk_nvmf_qpair *qpair,
		      spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_mcdma_qpair *rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);
	//
	rqpair->to_close = true;

	/* This happens only when the qpair is disconnected before
	 * it is added to the poll group. Since there is no poll group,
	 * the RDMA qp has not been initialized yet and the RDMA CM
	 * event has not yet been acknowledged, so we need to reject it.
	 */
	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		// nvmf_mcdma_qpair_reject_connection(rqpair);
		nvmf_mcdma_qpair_destroy(rqpair);
		return;
	}

	// if (rqpair->mcdma_qp) {
	// 	spdk_mcdma_qp_disconnect(rqpair->mcdma_qp);
	// }

	// nvmf_mcdma_destroy_drained_qpair(rqpair);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static int
nvmf_mcdma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	
	trid->trtype = SPDK_NVME_TRANSPORT_MCDMA;
	snprintf(trid->traddr, 10, "qdma01000");
	trid->adrfam = SPDK_NVMF_ADRFAM_QBDF;
	return 0;
}

static int
nvmf_mcdma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	
	snprintf(trid->trstring, 10, "QDMA");
	trid->trtype = SPDK_NVME_TRANSPORT_MCDMA;
	trid->adrfam = SPDK_NVMF_ADRFAM_QBDF;
	snprintf(trid->traddr, 10, "qdma01000");
	return 0;
}

static int
nvmf_mcdma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	snprintf(trid->trstring, 10, "QDMA");
	trid->trtype = SPDK_NVME_TRANSPORT_MCDMA;
	trid->adrfam = SPDK_NVMF_ADRFAM_QBDF;
	snprintf(trid->traddr, 10, "qdma01000");
	return 0;
}

static int
_nvmf_mcdma_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_mcdma_request *mcdma_req_to_abort = SPDK_CONTAINEROF(
				req->req_to_abort, struct spdk_nvmf_mcdma_request, req);
	// struct spdk_nvmf_mcdma_qpair *rqpair = SPDK_CONTAINEROF(req->req_to_abort->qpair,
	// 				      struct spdk_nvmf_mcdma_qpair, qpair);
	int rc;

	spdk_poller_unregister(&req->poller);

	switch (mcdma_req_to_abort->state) {
	case MCDMA_REQUEST_STATE_EXECUTING:
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	// case MCDMA_REQUEST_STATE_NEED_BUFFER:
	// 	STAILQ_REMOVE(&rqpair->poller->group->group.pending_buf_queue,
	// 		      &mcdma_req_to_abort->req, spdk_nvmf_request, buf_link);

	// 	nvmf_mcdma_request_set_abort_status(req, mcdma_req_to_abort);
	// 	break;

	// case MCDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
	// 	STAILQ_REMOVE(&rqpair->pending_mcdma_read_queue, mcdma_req_to_abort,
	// 		      spdk_nvmf_mcdma_request, state_link);

	// 	nvmf_mcdma_request_set_abort_status(req, mcdma_req_to_abort);
	// 	break;

	// case MCDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
	// 	STAILQ_REMOVE(&rqpair->pending_mcdma_write_queue, mcdma_req_to_abort,
	// 		      spdk_nvmf_mcdma_request, state_link);

	// 	nvmf_mcdma_request_set_abort_status(req, mcdma_req_to_abort);
	// 	break;

	// case MCDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
	// 	if (spdk_get_ticks() < req->timeout_tsc) {
	// 		req->poller = SPDK_POLLER_REGISTER(_nvmf_mcdma_qpair_abort_request, req, 0);
	// 		return SPDK_POLLER_BUSY;
	// 	}
	// 	break;

	default:
		break;
	}

	spdk_nvmf_request_complete(req);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_mcdma_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_mcdma_qpair *rqpair;
	struct spdk_nvmf_mcdma_transport *rtransport;
	struct spdk_nvmf_transport *transport;
	uint16_t cid;
	uint32_t i, max_req_count;
	struct spdk_nvmf_mcdma_request *mcdma_req_to_abort = NULL, *mcdma_req;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_mcdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_mcdma_transport, transport);
	transport = &rtransport->transport;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;
	max_req_count = rqpair->max_queue_depth;

	for (i = 0; i < max_req_count; i++) {
		mcdma_req = &rqpair->resources->reqs[i];
		/* When SRQ == NULL, rqpair has its own requests and req.qpair pointer always points to the qpair
		 * When SRQ != NULL all rqpairs share common requests and qpair pointer is assigned when we start to
		 * process a request. So in both cases all requests which are not in FREE state have valid qpair ptr */
		if (mcdma_req->state != MCDMA_REQUEST_STATE_FREE && mcdma_req->req.cmd->nvme_cmd.cid == cid &&
		    mcdma_req->req.qpair == qpair) {
			mcdma_req_to_abort = mcdma_req;
			break;
		}
	}

	if (mcdma_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &mcdma_req_to_abort->req;
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	req->poller = NULL;

	_nvmf_mcdma_qpair_abort_request(req);
}

static void
nvmf_mcdma_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_mcdma_poll_group *rgroup;
	struct spdk_nvmf_mcdma_poller *rpoller;

	assert(w != NULL);

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_mcdma_poll_group, group);

	spdk_json_write_named_uint64(w, "pending_data_buffer", rgroup->stat.pending_data_buffer);

	spdk_json_write_named_array_begin(w, "devices");

	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name",
					     "qdma01000");
		spdk_json_write_named_uint64(w, "polls",
					     rpoller->stat.polls);
		spdk_json_write_named_uint64(w, "idle_polls",
					     rpoller->stat.idle_polls);
		spdk_json_write_named_uint64(w, "completions",
					     rpoller->stat.completions);
		spdk_json_write_named_uint64(w, "requests",
					     rpoller->stat.requests);
		spdk_json_write_named_uint64(w, "request_latency",
					     rpoller->stat.request_latency);
		spdk_json_write_named_uint64(w, "pending_free_request",
					     rpoller->stat.pending_free_request);
		spdk_json_write_named_uint64(w, "pending_mcdma_read",
					     rpoller->stat.pending_mcdma_read);
		spdk_json_write_named_uint64(w, "pending_mcdma_write",
					     rpoller->stat.pending_mcdma_write);
		// spdk_json_write_named_uint64(w, "total_send_wrs",
		// 			     rpoller->stat.qp_stats.send.num_submitted_wrs);
		// spdk_json_write_named_uint64(w, "send_doorbell_updates",
		// 			     rpoller->stat.qp_stats.send.doorbell_updates);
		// spdk_json_write_named_uint64(w, "total_recv_wrs",
		// 			     rpoller->stat.qp_stats.recv.num_submitted_wrs);
		// spdk_json_write_named_uint64(w, "recv_doorbell_updates",
		// 			     rpoller->stat.qp_stats.recv.doorbell_updates);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

static int
nvmf_mcdma_poller_poll(struct spdk_nvmf_mcdma_transport *rtransport,
		      struct spdk_nvmf_mcdma_poller *rpoller)
{
	int count = 0;
	struct spdk_nvmf_mcdma_qpair *rqpair;
	uint64_t ticks = spdk_get_ticks();
	bool should_poll_unstarted = false;
	bool has_nvme_queues = !RB_EMPTY(&rpoller->qpairs);
	//
	if (spdk_unlikely(ticks > rpoller->last_poll_ticks + rpoller->poll_unstarted_interval_ticks)) {
		should_poll_unstarted = true;
		rpoller->last_poll_ticks = ticks;
	}

	for(int i = 0; i < rpoller->num_mcdma_qp; i++) {
		if (has_nvme_queues || should_poll_unstarted) {
			struct spdk_mcdma_qp *mcdma_qp = &rpoller->mcdma_qps[i];
			// SPDK_DEBUGLOG(nvmf, "Poll mcdma_qp %u\n", mcdma_qp->chid);
			count += spdk_axi_dma_poller(mcdma_qp->tx_ch);
			count += spdk_axi_dma_poller(mcdma_qp->rx_ch);
			}
			}
	
	/*
	if (spdk_ring_count(rtransport->compute_job_ring)) {
		struct blowfish_ctx *ctx;
		size_t cnt = spdk_ring_dequeue(rtransport->compute_job_ring, &ctx, 1);
		while (cnt == 0) {
			cnt = spdk_ring_dequeue(rtransport->compute_job_ring, &ctx, 1);
		}
		// printf("Thread %u got compute job %p %u to %u\n", spdk_env_get_current_core(), ctx->buf, ctx->start, ctx->end);
		blowfish_encrypt_range(ctx->buf, ctx->start, ctx->end);

		spdk_ring_enqueue(rtransport->compute_job_cmpl_ring, &ctx, 1, NULL);
	}

	
	RB_FOREACH(rqpair, qpairs_tree, &rpoller->qpairs) {
		if (rqpair->running_compute_jobs > 0) {
			struct blowfish_ctx *ctx;
			struct spdk_nvmf_mcdma_request *mcdma_req;
			size_t cnt = spdk_ring_dequeue(rtransport->compute_job_cmpl_ring, &ctx, 1);
			while (cnt > 0) {
				rqpair->running_compute_jobs -= cnt;
				mcdma_req = ctx->mcdma_req;


				// printf("Got compute cmpl %p %u to %u\n", ctx->buf, ctx->start, ctx->end);
				free(ctx);
				cnt = spdk_ring_dequeue(rtransport->compute_job_cmpl_ring, &ctx, 1);
			}
			if (rqpair->running_compute_jobs == 0) {
				nvmf_mcdma_memcopy_cmpl(NULL, 0, mcdma_req);
			}
		}
			nvmf_mcdma_qpair_process_pending(rtransport, rqpair, false);
	}*/

	RB_FOREACH(rqpair, qpairs_tree, &rpoller->qpairs) {
			nvmf_mcdma_qpair_process_pending(rtransport, rqpair, false);
	}
	return count;
}

static int
nvmf_mcdma_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_mcdma_transport *rtransport;
	struct spdk_nvmf_mcdma_poll_group *rgroup;
	struct spdk_nvmf_mcdma_poller	*rpoller;
	int				count, rc;
	//
	rtransport = SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_mcdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_mcdma_poll_group, group);

	count = 0;
	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		rc = nvmf_mcdma_poller_poll(rtransport, rpoller);
		if (rc < 0) {
			return rc;
		}
		count += rc;
	}

	return count;
}


const struct spdk_nvmf_transport_ops spdk_nvmf_transport_mcdma = {
	.name = "QDMA",
	.type = SPDK_NVME_TRANSPORT_MCDMA,
	.opts_init = nvmf_mcdma_opts_init,
	.create = nvmf_mcdma_create,
	.dump_opts = nvmf_mcdma_dump_opts,
	.destroy = nvmf_mcdma_destroy,

	.listen = nvmf_mcdma_listen,
	.stop_listen = nvmf_mcdma_stop_listen,
	.cdata_init = nvmf_mcdma_cdata_init,

	.listener_discover = nvmf_mcdma_discover,

	.poll_group_create = nvmf_mcdma_poll_group_create,
	.get_optimal_poll_group = nvmf_mcdma_get_optimal_poll_group,
	.poll_group_destroy = nvmf_mcdma_poll_group_destroy,
	.poll_group_add = nvmf_mcdma_poll_group_add,
	.poll_group_remove = nvmf_mcdma_poll_group_remove,
	.poll_group_poll = nvmf_mcdma_poll_group_poll,

	.req_free = nvmf_mcdma_request_free,
	.req_complete = nvmf_mcdma_request_complete,

	.qpair_fini = nvmf_mcdma_close_qpair,
	.qpair_update_pfch_tag = nvmf_mcdma_qpair_update_pfch_tag,
	.qpair_get_peer_trid = nvmf_mcdma_qpair_get_peer_trid,
	.qpair_get_local_trid = nvmf_mcdma_qpair_get_local_trid,
	.qpair_get_listen_trid = nvmf_mcdma_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_mcdma_qpair_abort_request,

	.poll_group_dump_stat = nvmf_mcdma_poll_group_dump_stat,
};

SPDK_NVMF_TRANSPORT_REGISTER(mcdma, &spdk_nvmf_transport_mcdma);
