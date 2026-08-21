// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics RDMA host code.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <rdma/mr_pool.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/blk-mq.h>
#include <linux/blk-mq-rdma.h>
#include <linux/ctype.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/nvme.h>
#include <asm/unaligned.h>

#include <qdma/libqdma_export.h>
#include <qdma/qdma.h>
#include <qdma/xdev.h>

#include "nvme.h"
#include "fabrics.h"


#define NVME_QDMA_CONNECT_TIMEOUT_MS	3000		/* 3 second */

#define NVME_QDMA_MAX_SEGMENTS		256

#define NVME_QDMA_MAX_INLINE_SEGMENTS	4

#define NVME_QDMA_MAX_IO_QUEUES 8

#define NVME_QDMA_RSP_SIZE 64

#define NVME_QDMA_KERNEL_MAX_COMMANDS 64

/*
 * These can be higher, but we need to ensure that any command doesn't
 * require an sg allocation that needs more than a page of data.
 */
#define NVMQ_MAX_KB_SZ	4096
#define NVMQ_MAX_SEGS	127

#define QDMA_BYP_RING_SZ 16384

#define QDMA_C2H_PFCH_BYP_QID 0x1408
#define QDMA_C2H_PFCH_BYP_TAG 0x140C

#define QDMA_PRP_FETCH_QID 17

struct qdma_c2h_pfch_byp_qid_data {
	uint32_t byp_qid : 12;
	uint32_t rsvd : 20;
};

struct qdma_c2h_pfch_byp_tag_data {
	uint32_t byp_tag : 7;
	uint32_t rsvd1 : 1;
	uint32_t byp_qid : 12;
	uint32_t rsvd2 : 12;
};
struct nvme_qdma_device {
	// struct ib_device	*dev;
	// struct ib_pd		*pd;
	struct xlnx_pci_dev *xpdev;
	struct kref		ref;
	struct list_head	entry;
	struct mutex	pfch_reg_mutex;
	unsigned int		num_inline_segments;
};

struct nvme_qdma_cqe {
	// struct ib_cqe		cqe;
	struct qdma_request qdma_req;
	void			*qe_data;
	// u64			dma;
};

struct nvme_qdma_sqe {
	void			*qe_data;
	struct scatterlist	*sg;
	struct bio	*bio;
	int npages;		/* In the PRP list. 0 means small pool in use */
	int nents;		/* Used in scatterlist */
	void		*sgl_buf;
	struct list_head	entry;
};

struct qdma_desc_byp_ctrl {
	uint16_t pidx;
	uint16_t num_outstanding;
	
	struct qdma_request *qdma_reqs;
	void *dummy_pg;
	struct qdma_sw_sg sgl;
};

struct nvme_qdma_queue;
struct nvme_qdma_request;

struct nvme_qdma_rsp {
	struct nvme_qdma_cqe qe;
	struct nvme_qdma_queue *queue;
	struct nvme_qdma_request *req;
	void *data;
};
struct nvme_qdma_request {
	union {
		struct nvmq_request	req;
		struct nvmq_kernel	knl;
	} u;
	struct qdma_request qdma_req;
	// struct nvme_qdma_sqe first_sqe;
	struct list_head	sqe_list;
	union nvme_result	result;
	__le16			status;
	refcount_t		ref;
	u32			num_sge;
	struct nvme_qdma_queue  *queue;
};

enum nvme_qdma_queue_flags {
	nvme_qdma_Q_ALLOCATED		= 0,
	nvme_qdma_Q_LIVE		= 1,
	nvme_qdma_Q_TR_READY		= 2,
};

struct nvme_qdma_qp {
	unsigned long h2c_qhndl;
	unsigned long c2h_qhndl;
};

struct nvme_qdma_queue {
	struct nvme_qdma_rsp	*rsp_ring;
	int			queue_size;
	size_t			cmnd_capsule_len;
	struct nvme_qdma_ctrl	*ctrl;
	struct nvme_qdma_device	*device;
	// struct ib_cq		*ib_cq;
	// struct ib_qp		*qp;
	struct nvme_qdma_qp qe_qp;
	struct nvme_qdma_qp data_qp;
	struct qdma_desc_byp_ctrl data_h2c_byp;
	// unsigned long qe_qhndl;
	// unsigned long data_qhndl;
	uint32_t rsp_idx;
	uint16_t qid;
	uint8_t  data_c2h_pfch_tag;

	unsigned long		flags;
	// struct rdma_cm_id	*cm_id;
	int			cm_error;
	struct completion	cm_done;
};

struct nvme_qdma_ctrl {
	/* read only in the hot path */
	struct nvme_qdma_queue	*queues;

	/* other member variables */
	struct blk_mq_tag_set	tag_set;
	struct work_struct	err_work;

	struct nvme_qdma_sqe	async_event_sqe;
	struct qdma_request		async_event_qdma_req;

	struct delayed_work	reconnect_work;

	struct list_head	list;

	struct blk_mq_tag_set	admin_tag_set;
	struct nvme_qdma_device	*device;

	u32			max_fr_pages;

	mempool_t *iod_mempool;

	mempool_t *sqe_mempool;

	struct nvmq_ctrl	ctrl;
	bool			use_inline_data;
	u32			io_queues[HCTX_MAX_TYPES];
};

struct _nvmq_sgl_desc {
	__le64	addr : 48;
	__le64	cid : 10;
	__le64	qid : 5;
	__le64	one : 1;
	__le32	length;
	__u8	rsvd[3];
	__u8	type;
};

enum nvmq_map_option {
	QE,
	DATA,
	QE_DATA
};

static inline struct nvme_qdma_ctrl *to_qdma_ctrl(struct nvmq_ctrl *ctrl)
{
	return container_of(ctrl, struct nvme_qdma_ctrl, ctrl);
}

static inline bool nvme_opcode_is_compute_cmd(uint8_t opcode)
{
	return opcode >= 0x22 && opcode <= 0x30;
}

static inline struct nvme_command *nvmq_diag_req_cmd(struct nvme_qdma_request *req)
{
	if (!req)
		return NULL;
	if (req->u.req.flags & NVME_REQ_USERKERNEL)
		return req->u.knl.cmds;
	return req->u.req.cmd;
}

static inline bool nvmq_diag_interesting_cmd(struct nvme_command *cmd)
{
	if (!cmd)
		return false;
	return nvme_is_slm_set(cmd) ||
	       nvme_opcode_is_compute_cmd(cmd->common.opcode) ||
	       cmd->common.opcode == 0x84 ||
	       cmd->common.opcode == 0x85 ||
	       cmd->common.opcode == 0x88 ||
	       cmd->common.opcode == 0x89;
}

static void nvmq_diag_log_cmd(const char *stage, struct nvme_qdma_queue *queue,
			      struct request *rq, struct nvme_command *cmd, int ret)
{
	if (!nvmq_diag_interesting_cmd(cmd))
		return;

	pr_warn("NVMQ_DIAG %s qid=%u tag=%d opc=0x%02x nsid=0x%x cid=%u cdw10=0x%x cdw11=0x%x cdw12=0x%x prp1=0x%llx prp2=0x%llx bytes=%u segs=%u ret=%d\n",
		stage,
		queue ? queue->qid : 0xffff,
		rq ? rq->tag : -1,
		cmd->common.opcode,
		le32_to_cpu(cmd->common.nsid),
		le16_to_cpu(cmd->common.command_id),
		le32_to_cpu(cmd->common.cdw10),
		le32_to_cpu(cmd->common.cdw11),
		le32_to_cpu(cmd->common.cdw12),
		(unsigned long long)le64_to_cpu(cmd->common.dptr.prp1),
		(unsigned long long)le64_to_cpu(cmd->common.dptr.prp2),
		rq ? blk_rq_bytes(rq) : 0,
		rq ? blk_rq_nr_phys_segments(rq) : 0,
		ret);
}

static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_mutex);

static LIST_HEAD(nvme_qdma_ctrl_list);
static DEFINE_MUTEX(nvme_qdma_ctrl_mutex);
static char g_msg_buf[4096 * 5];

/*
 * Disabling this option makes small I/O goes faster, but is fundamentally
 * unsafe.  With it turned off we will have to register a global rkey that
 * allows read and write access to all physical memory.
 */
static bool register_always = true;
module_param(register_always, bool, 0444);
MODULE_PARM_DESC(register_always,
	 "Use memory registration even for contiguous memory regions");

// static int nvme_qdma_cm_handler(struct rdma_cm_id *cm_id,
// 		struct rdma_cm_event *event);
static int nvme_qdma_post_rsp(struct nvme_qdma_queue *queue,
							   struct nvme_qdma_rsp *rsp, struct request_queue *q, struct request *rq);
static int nvme_qdma_qe_recv_done(struct qdma_request *req, unsigned int bytes_done, int err);
static unsigned int qdma_map_page(struct pci_dev *pdev, void *buf, void *sg, bool qdma_format);

static const struct blk_mq_ops nvme_qdma_mq_ops;
static const struct blk_mq_ops nvme_qdma_admin_mq_ops;

static inline uint32_t nvme_qid_to_qdma_qid(uint32_t qid, bool is_data, uint8_t func_id)
{
	/* 
	 * QDMA queue ID mapping to x86 host-end NVMe Queues
	 * Doc: https://serve.yuque.com/mora7e/qpqkrm/lt7y1tmp3r0tagfe
	 * Admin queue (QE and Data): 32 * func_id
	 * IO QE: qid + 32 * func_id
	 * Data QE: qid + 32 * func_id + 8
	 */ 
	pr_debug("is data = %u\n", is_data);
	if (is_data) {
		BUG_ON(qid == 0); // No data queue for admin queue
		return qid + 32 * func_id + 8; // IO qid starts from 8
	} else {
		if (qid == 0)
			return 32 * func_id;
		else
			return qid + 32 * func_id;
	}
}

/* XXX: really should move to a generic header sooner or later.. */
static inline void put_unaligned_le24(u32 val, u8 *p)
{
	*p++ = val;
	*p++ = val >> 8;
	*p++ = val >> 16;
}

static inline int nvme_qdma_queue_idx(struct nvme_qdma_queue *queue)
{
	return queue - queue->ctrl->queues;
}

static bool nvme_qdma_poll_queue(struct nvme_qdma_queue *queue)
{
	return nvme_qdma_queue_idx(queue) >
		queue->ctrl->io_queues[HCTX_TYPE_DEFAULT] +
		queue->ctrl->io_queues[HCTX_TYPE_READ];
}

static inline size_t nvme_qdma_inline_data_size(struct nvme_qdma_queue *queue)
{
	return queue->cmnd_capsule_len - sizeof(struct nvme_command);
}

static void nvme_qdma_free_cqe(unsigned long dev_hndl, struct nvme_qdma_cqe *qe,
		size_t capsule_size, enum dma_data_direction dir)
{
	kfree(qe->qe_data);
	kfree(qe->qdma_req.sgl);
	qe->qe_data = NULL;
	qe->qdma_req.sgl = NULL;
}

static void nvme_qdma_free_rsp(unsigned long dev_hndl, struct nvme_qdma_rsp *rsp,
		size_t capsule_size, enum dma_data_direction dir)
{
	// ib_dma_unmap_single(ibdev, qe->dma, capsule_size, dir);
	if (rsp->queue->qid == 0) {
	kfree(rsp->data);
	}
	nvme_qdma_free_cqe(dev_hndl, &rsp->qe, capsule_size, dir);
}

static int nvme_qdma_alloc_cqe(unsigned long dev_hndl, struct nvme_qdma_cqe *qe, size_t capsule_size)
{
	qe->qe_data = kzalloc(capsule_size, GFP_KERNEL);
	if (!qe->qe_data)
		return -ENOMEM;
	
	qe->qdma_req.sgl = kzalloc(3 * sizeof(struct qdma_sw_sg), GFP_KERNEL);
	if (!qe->qdma_req.sgl)
		return -ENOMEM;

	return 0;
}

static int nvme_qdma_alloc_rsp(unsigned long dev_hndl, uint16_t qid, struct nvme_qdma_rsp *rsp,
		size_t capsule_size, enum dma_data_direction dir)
{
	if (qid == 0) {
		pr_debug("rsp->data size is %u\n", capsule_size);
		rsp->data = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!rsp->data){
		pr_debug("ERR! rsp data null\n");
		
		return -ENOMEM;
	}
	}

	rsp->qe.qdma_req.write = 0;
	rsp->qe.qdma_req.dma_mapped = false;
	rsp->qe.qdma_req.udd_len = 0;
	rsp->qe.qdma_req.ep_addr = 0;
	rsp->qe.qdma_req.no_memcpy = 0;
	rsp->qe.qdma_req.timeout_ms = nvmq_io_timeout * 1000;
	rsp->qe.qdma_req.fp_done = nvme_qdma_qe_recv_done;

	return nvme_qdma_alloc_cqe(dev_hndl, &rsp->qe, NVME_QDMA_RSP_SIZE);
}

static int sqe_member_init(struct nvme_qdma_sqe *sqe, gfp_t gfp_mask)
{
	sqe->qe_data = kzalloc(sizeof(struct nvme_command), GFP_KERNEL);
	if (!sqe->qe_data)
		return -1;

	sqe->sgl_buf = kzalloc(PAGE_SIZE, GFP_KERNEL | GFP_DMA);
	BUG_ON(((uintptr_t)sqe->sgl_buf) & (PAGE_SIZE - 1)); // SGL buf isn't 4KB aligned
	if (unlikely(!sqe->sgl_buf)) {
		pr_err("failed to allocate sgl buffer for req %p\n", sqe);
		return -1;
	}

	sqe->bio = NULL;
        //pr_warning("get pointer qe_data%p\n",sqe->qe_data);
	//pr_warning("get pointer sgl_buf%p\n",sqe->sgl_buf);
	return 0;
}

static void *sqe_kmalloc(gfp_t gfp_mask, void *pool_data)
{
	struct nvme_qdma_sqe *sqe = mempool_kmalloc(gfp_mask, pool_data);

	if (sqe_member_init(sqe, gfp_mask)) {
		mempool_free(sqe, pool_data);
		return NULL;
	}

	return sqe;
}

static void sqe_member_free(struct nvme_qdma_sqe *sqe)
{
	if (sqe->bio) {
		blk_rq_unmap_user(sqe->bio);
	}
	//pr_warning("free pointer qe_data%p\n",sqe->qe_data);
	//pr_warning("free pointer sgl_buf%p\n",sqe->sgl_buf);
	kfree(sqe->qe_data);
	kfree(sqe->sgl_buf);
	sqe->qe_data = NULL;
	sqe->sgl_buf = NULL;
}

static void sqe_kfree(void *element, void *pool_data)
{
	struct nvme_qdma_sqe *sqe = element;

	sqe_member_free(sqe);

	mempool_kfree(sqe, pool_data);
}

static void nvme_qdma_free_ring(unsigned long dev_hndl,
		struct nvme_qdma_rsp *ring, size_t qdma_ring_size,
		size_t capsule_size, enum dma_data_direction dir)
{
	int i;

	for (i = 0; i < qdma_ring_size; i++) {
		nvme_qdma_free_rsp(dev_hndl, &ring[i], capsule_size, dir);
	}
	kfree(ring);
}

static struct nvme_qdma_rsp *nvme_qdma_alloc_ring(unsigned long dev_hndl,
		uint16_t qid, size_t qdma_ring_size, size_t capsule_size,
		enum dma_data_direction dir)
{
	struct nvme_qdma_rsp *ring;
	int i;

	ring = kcalloc(qdma_ring_size, sizeof(struct nvme_qdma_rsp), GFP_KERNEL);
	if (!ring)
		return NULL;

	/*
	 * Bind the CQEs (post recv buffers) DMA mapping to the RDMA queue
	 * lifetime. It's safe, since any chage in the underlying RDMA device
	 * will issue error recovery and queue re-creation.
	 */
	for (i = 0; i < qdma_ring_size; i++) {
		if (nvme_qdma_alloc_rsp(dev_hndl, qid, &ring[i], capsule_size, dir))
			goto out_free_ring;
	}

	return ring;

out_free_ring:
	nvme_qdma_free_ring(dev_hndl, ring, i, capsule_size, dir);
	return NULL;
}

// static void nvme_qdma_qp_event(struct ib_event *event, void *context)
// {
// 	pr_debug("QP event %s (%d)\n",
// 		 ib_event_msg(event->event), event->event);

// }

// static int nvme_qdma_wait_for_cm(struct nvme_qdma_queue *queue)
// {
// 	int ret;

// 	ret = wait_for_completion_interruptible_timeout(&queue->cm_done,
// 			msecs_to_jiffies(NVME_QDMA_CONNECT_TIMEOUT_MS) + 1);
// 	if (ret < 0)
// 		return ret;
// 	if (ret == 0)
// 		return -ETIMEDOUT;
// 	WARN_ON_ONCE(queue->cm_error > 0);
// 	return queue->cm_error;
// }

static int nvme_qdma_get_c2h_pfch_tag(struct nvme_qdma_queue *queue)
{
	struct nvme_qdma_device *dev = queue->device;
	unsigned long xdev_hndl = dev->xpdev->dev_hndl;
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)xdev_hndl;
	uint32_t qid = nvme_qid_to_qdma_qid(nvme_qdma_queue_idx(queue), true, xdev->func_id);

	struct qdma_c2h_pfch_byp_qid_data byp_qid_data;
	struct qdma_c2h_pfch_byp_tag_data byp_tag_data;
	unsigned int *p = (unsigned int *)&byp_qid_data;

	int ret;

	byp_qid_data.byp_qid = qid;

	mutex_lock(&dev->pfch_reg_mutex);

	ret = qdma_device_write_config_register(dev->xpdev->dev_hndl, QDMA_C2H_PFCH_BYP_QID, *p);
	
	if (ret) {
		pr_err("Failed to write QDMA_C2H_PFCH_BYP_QID: %d\n", ret);
		return ret;
	}

	p = (unsigned int *)&byp_tag_data;

	do {
		ret = qdma_device_read_config_register(dev->xpdev->dev_hndl, QDMA_C2H_PFCH_BYP_TAG, p);
		
		if (ret) {
			pr_err("Failed to write QDMA_C2H_PFCH_BYP_TAG: %d\n", ret);
			return ret;
		}
	} while (byp_tag_data.byp_qid != qid);

	mutex_unlock(&dev->pfch_reg_mutex);

	queue->data_c2h_pfch_tag = byp_tag_data.byp_tag;

	pr_debug("C2H pfch tag for queue %u is 0x%X\n", queue->qid, queue->data_c2h_pfch_tag);

	return ret;
}

static int nvme_qdma_create_h2c_data_qp(struct nvme_qdma_device *dev, int qid)
{
	char msg_buf[32];
	struct qdma_queue_conf qconf;
	unsigned long xdev_hndl = dev->xpdev->dev_hndl;
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)xdev_hndl;
	struct nvme_qdma_qp data_qp;
	int ret;
	
	memset(&qconf, 0, sizeof(qconf));

	/* find pci dev info */
	pr_debug("device id = %x\n", dev->xpdev->pdev->device);
	pr_debug("PF id = %u\n", xdev->func_id);

	/* Data H2C queue*/
	qconf.qidx = nvme_qid_to_qdma_qid(qid, true, xdev->func_id);
	qconf.st = 1;
	qconf.q_type = Q_H2C;
	qconf.desc_rng_sz_idx = 15;
	qconf.wb_status_en = 1;
	qconf.irq_en = 1;
	qconf.cmpl_status_acc_en = 1;
	qconf.cmpl_status_pend_chk = 1;
	qconf.cmpl_rng_sz_idx = 6;
	qconf.cmpl_stat_en = 1;
	qconf.cmpl_trig_mode = TRIG_MODE_ANY;
	qconf.fetch_credit = 0;
	// qconf.cmpl_timer_idx = 9;
	// qconf.cmpl_trig_mode = 
	qconf.rngsz = 256;
	// qconf.quld = (unsigned long)queue;
	qconf.desc_bypass = 1;

	pr_debug("Data H2C queue, qid %d, qdma qid %u\n", qid, qconf.qidx);

	ret = qdma_queue_add(dev->xpdev->dev_hndl, &qconf, &data_qp.h2c_qhndl, msg_buf, 32);
	dev->xpdev->qdata[qconf.qidx].qhndl = data_qp.h2c_qhndl;
	
	if (ret) {
		pr_err("Failed to add qdma h2c queue %u: %s\n", qconf.qidx, msg_buf);
		return ret;
	}

	ret = qdma_queue_start(dev->xpdev->dev_hndl, data_qp.h2c_qhndl, msg_buf, 32);
	
	if (ret) {
		pr_err("Failed to start qdma queue %lu: %s\n", data_qp.h2c_qhndl, msg_buf);
		return ret;
	}

	return ret;
}

static int nvme_qdma_create_qp(struct nvme_qdma_queue *queue)
{
	struct nvme_qdma_device *dev = queue->device;
	int qid = nvme_qdma_queue_idx(queue);
	char msg_buf[32];
	struct qdma_queue_conf qconf;
	unsigned long xdev_hndl = dev->xpdev->dev_hndl;
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)xdev_hndl;
	int ret;
	
	memset(&qconf, 0, sizeof(qconf));

	/* find pci dev info */
	pr_debug("device id = %x\n", dev->xpdev->pdev->device);
	pr_debug("PF id = %u\n", xdev->func_id);
  
	/* QE H2C queue*/
	qconf.qidx = nvme_qid_to_qdma_qid(qid, false, xdev->func_id);
	qconf.st = 1;
	qconf.q_type = Q_H2C;
	qconf.desc_rng_sz_idx = 8;
	qconf.wb_status_en = 1;
	qconf.irq_en = 0;
	qconf.cmpl_status_acc_en = 1;
	qconf.cmpl_status_pend_chk = 1;
	qconf.cmpl_rng_sz_idx = 6;
	qconf.cmpl_stat_en = 1;
	qconf.cmpl_trig_mode = TRIG_MODE_ANY;
	// qconf.cmpl_timer_idx = 9;
	// qconf.cmpl_trig_mode = 
	qconf.rngsz = queue->queue_size;
	qconf.quld = (unsigned long)queue;

	pr_debug("nvme qid = %d, qdma qid = %d\n", qid, qconf.qidx);
	pr_debug("QE H2C queue %p, qid %d, qdma qid %u\n", queue, qid, qconf.qidx);

	ret = qdma_queue_add(dev->xpdev->dev_hndl, &qconf, &queue->qe_qp.h2c_qhndl, msg_buf, 32);
	dev->xpdev->qdata[qconf.qidx].qhndl = queue->qe_qp.h2c_qhndl;
	
	if (ret) {
		pr_err("Failed to add qdma h2c queue %u: %s\n", qconf.qidx, msg_buf);
		return ret;
	}

	ret = qdma_queue_start(dev->xpdev->dev_hndl, queue->qe_qp.h2c_qhndl, msg_buf, 32);
	
	if (ret) {
		pr_err("Failed to start qdma queue %lu: %s\n", queue->qe_qp.h2c_qhndl, msg_buf);
		return ret;
	}

	/* QE C2H queue*/
	qconf.q_type = Q_C2H;
	qconf.desc_rng_sz_idx = 8;
	qconf.wb_status_en = 1;
	qconf.irq_en = 1;
	qconf.cmpl_status_acc_en = 1;
	qconf.cmpl_status_pend_chk = 1;
	qconf.fetch_credit = 1;
	qconf.cmpl_rng_sz_idx = 9;
	qconf.cmpl_stat_en = 1;
#ifdef NVME_QDMA_REDUCE_IRQ
	qconf.cmpl_timer_idx = 15;
	qconf.cmpl_trig_mode = qid > 0 ? TRIG_MODE_COMBO : TRIG_MODE_ANY;
	qconf.cmpl_cnt_th_idx = 2;
#else
	qconf.cmpl_timer_idx = 9;
	qconf.cmpl_trig_mode = TRIG_MODE_ANY;
#endif
	qconf.cmpl_en_intr = 1;
	// qconf.fp_descq_c2h_packet = nvme_qdma_recv_done;

	pr_debug("QE C2H queue %p, qid %d, qdma qid %u\n", queue, qid, qconf.qidx);

	ret = qdma_queue_add(dev->xpdev->dev_hndl, &qconf, &queue->qe_qp.c2h_qhndl, msg_buf, 32);
	dev->xpdev->qdata[qconf.qidx + 128].qhndl = queue->qe_qp.c2h_qhndl;
	dev->xpdev->qdata[qconf.qidx + 256].qhndl = queue->qe_qp.c2h_qhndl + 128;
	
	if (ret) {
		pr_err("Failed to add qdma c2h queue %u: %s\n", qconf.qidx, msg_buf);
		return ret;
	}

	ret = qdma_queue_start(dev->xpdev->dev_hndl, queue->qe_qp.c2h_qhndl, msg_buf, 32);
	
	if (ret) {
		pr_err("Failed to start qdma queue %lu: %s\n", queue->qe_qp.c2h_qhndl, msg_buf);
		return ret;
	}

	// No data queue for admin queues
	if (qid == 0) {
		return 0;
	}

	/* Data C2H queue*/
	qconf.qidx = nvme_qid_to_qdma_qid(qid, true, xdev->func_id);
	qconf.q_type = Q_C2H;
	qconf.desc_rng_sz_idx = 15;
	qconf.wb_status_en = 1;
	qconf.irq_en = 0;
	qconf.cmpl_status_acc_en = 1;
	qconf.cmpl_status_pend_chk = 1;
	qconf.cmpl_rng_sz_idx = 15;
	qconf.cmpl_stat_en = 1;
	qconf.cmpl_timer_idx = 9;
	qconf.cmpl_cnt_th_idx = 7;
	qconf.cmpl_trig_mode = TRIG_MODE_USER;
	qconf.cmpl_en_intr = 1;
	qconf.pfetch_bypass = 1;
	qconf.desc_bypass = 1;
	qconf.fetch_credit = 1;
	// qconf.fp_descq_c2h_packet = nvme_qdma_recv_done;

	pr_debug("Data C2H queue %p, qid %d, qdma qid %u\n", queue, qid, qconf.qidx);

	ret = qdma_queue_add(dev->xpdev->dev_hndl, &qconf, &queue->data_qp.c2h_qhndl, msg_buf, 32);
	dev->xpdev->qdata[qconf.qidx + 128].qhndl = queue->data_qp.c2h_qhndl;
	dev->xpdev->qdata[qconf.qidx + 256].qhndl = queue->data_qp.c2h_qhndl + 128;
	
	if (ret) {
		pr_err("Failed to add qdma c2h queue %u: %s\n", qconf.qidx, msg_buf);
		return ret;
	}

	ret = qdma_queue_start(dev->xpdev->dev_hndl, queue->data_qp.c2h_qhndl, msg_buf, 32);
	
	if (ret) {
		pr_err("Failed to start qdma queue %lu: %s\n", queue->data_qp.c2h_qhndl, msg_buf);
		return ret;
	}

	ret = nvme_qdma_get_c2h_pfch_tag(queue);
	
	if (ret) {
		pr_err("Failed to get C2H pfch tag: %d\n", ret);
		return ret;
	}

	return ret;
}

static void nvme_qdma_exit_request(struct blk_mq_tag_set *set,
		struct request *rq, unsigned int hctx_idx)
{
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);

	// kfree(req->sqe.qe_data);
	kfree(req->qdma_req.sgl);
        req->qdma_req.sgl = NULL;
	// if (req->sgl_buf) {
	// 	kfree(req->sgl_buf);
	// }
}

static int nvme_qdma_init_request(struct blk_mq_tag_set *set,
		struct request *rq, unsigned int hctx_idx,
		unsigned int numa_node)
{
	struct nvme_qdma_ctrl *ctrl = set->driver_data;
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	int queue_idx = (set == &ctrl->tag_set) ? hctx_idx + 1 : 0;
	struct nvme_qdma_queue *queue = &ctrl->queues[queue_idx];

	nvmq_req(rq)->ctrl = &ctrl->ctrl;

	req->qdma_req.write = 1;
	req->qdma_req.dma_mapped = false;
	req->qdma_req.udd_len = 0;
	req->qdma_req.ep_addr = 0;
	req->qdma_req.no_memcpy = 1;
	req->qdma_req.timeout_ms = nvmq_io_timeout * 1000;
	req->qdma_req.sgl = kzalloc(NVME_QDMA_KERNEL_MAX_COMMANDS * sizeof(struct qdma_sw_sg), GFP_KERNEL);
	if (!req->qdma_req.sgl)
		return -ENOMEM;

	req->queue = queue;

	return 0;
}

static int nvme_qdma_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
		unsigned int hctx_idx)
{
	struct nvme_qdma_ctrl *ctrl = data;
	struct nvme_qdma_queue *queue = &ctrl->queues[hctx_idx + 1];

	BUG_ON(hctx_idx >= ctrl->ctrl.queue_count);

	hctx->driver_data = queue;
	return 0;
}

static int nvme_qdma_init_admin_hctx(struct blk_mq_hw_ctx *hctx, void *data,
		unsigned int hctx_idx)
{
	struct nvme_qdma_ctrl *ctrl = data;
	struct nvme_qdma_queue *queue = &ctrl->queues[0];

	BUG_ON(hctx_idx != 0);

	hctx->driver_data = queue;
	return 0;
}

static void nvme_qdma_free_dev(struct kref *ref)
{
	struct nvme_qdma_device *ndev =
		container_of(ref, struct nvme_qdma_device, ref);

	mutex_lock(&device_list_mutex);
	list_del(&ndev->entry);
	mutex_unlock(&device_list_mutex);

	// qdma_device_close()
	kfree(ndev);
}

static void nvme_qdma_dev_put(struct nvme_qdma_device *dev)
{
	kref_put(&dev->ref, nvme_qdma_free_dev);
}

static int nvme_qdma_dev_get(struct nvme_qdma_device *dev)
{
	return kref_get_unless_zero(&dev->ref);
}

// static struct nvme_qd *
// nvme_qdma_find_get_device(struct rdma_cm_id *cm_id)
// {
// 	struct nvme_qdma_device *ndev;

// 	mutex_lock(&device_list_mutex);
// 	list_for_each_entry(ndev, &device_list, entry) {
// 		if (ndev->dev->node_guid == cm_id->device->node_guid &&
// 		    nvme_qdma_dev_get(ndev))
// 			goto out_unlock;
// 	}

// 	ndev = kzalloc(sizeof(*ndev), GFP_KERNEL);
// 	if (!ndev)
// 		goto out_err;

// 	ndev->dev = cm_id->device;
// 	kref_init(&ndev->ref);

// 	ndev->pd = ib_alloc_pd(ndev->dev,
// 		register_always ? 0 : IB_PD_UNSAFE_GLOBAL_RKEY);
// 	if (IS_ERR(ndev->pd))
// 		goto out_free_dev;

// 	if (!(ndev->dev->attrs.device_cap_flags &
// 	      IB_DEVICE_MEM_MGT_EXTENSIONS)) {
// 		dev_err(&ndev->dev->dev,
// 			"Memory registrations not supported.\n");
// 		goto out_free_pd;
// 	}

// 	ndev->num_inline_segments = min(NVME_QDMA_MAX_INLINE_SEGMENTS,
// 					ndev->dev->attrs.max_send_sge - 1);
// 	list_add(&ndev->entry, &device_list);
// out_unlock:
// 	mutex_unlock(&device_list_mutex);
// 	return ndev;

// out_free_pd:
// 	ib_dealloc_pd(ndev->pd);
// out_free_dev:
// 	kfree(ndev);
// out_err:
// 	mutex_unlock(&device_list_mutex);
// 	return NULL;
// }

static int qdma_destroy_qp(unsigned long dev_hndl, unsigned int qhndl)
{
	char msg_buf[32];
	int ret;

	ret = qdma_queue_stop(dev_hndl, qhndl, msg_buf, 32);
	if (ret) {
		pr_err("Failed to stop queue %u: %s\n", qhndl, msg_buf);
		return ret;
	}

	ret = qdma_queue_remove(dev_hndl, qhndl, msg_buf, 32);
	if (ret) {
		pr_err("Failed to remove queue %u: %s\n", qhndl, msg_buf);
		return ret;
	}

	return 0;
}

static void nvme_qdma_destroy_queue_ib(struct nvme_qdma_queue *queue)
{
	struct nvme_qdma_device *dev;
	unsigned long dev_hndl;

	if (!test_and_clear_bit(nvme_qdma_Q_TR_READY, &queue->flags))
		return;

	dev = queue->device;
	dev_hndl = dev->xpdev->dev_hndl;

	// ib_mr_pool_destroy(queue->qp, &queue->qp->rdma_mrs);

	/*
	 * The cm_id object might have been destroyed during RDMA connection
	 * establishment error flow to avoid getting other cma events, thus
	 * the destruction of the QP shouldn't use rdma_cm API.
	 */
	// ib_destroy_qp(queue->qp);
	// ib_free_cq(queue->ib_cq);

	qdma_destroy_qp(dev_hndl, queue->qe_qp.h2c_qhndl);
	qdma_destroy_qp(dev_hndl, queue->qe_qp.c2h_qhndl);

	nvme_qdma_free_ring(dev_hndl, queue->rsp_ring, queue->queue_size,
			sizeof(struct nvme_completion), DMA_FROM_DEVICE);

	nvme_qdma_dev_put(dev);
}

// static int nvme_qdma_get_max_fr_pages(unsigned long dev_hndl)
// {
// 	return min_t(u32, NVME_QDMA_MAX_SEGMENTS,
// 		     ibdev->attrs.max_fast_reg_page_list_len - 1);
// }

static void nvmq_print_sgl(struct scatterlist *sgl, int nents)
{
	int i;
	struct scatterlist *sg;

	for_each_sg(sgl, sg, nents, i) {
		dma_addr_t phys = sg_phys(sg);
		pr_debug("sg[%d] phys_addr:%pad offset:%d length:%d "
			"dma_address:%pad dma_length:%d\n",
			i, &phys, sg->offset, sg->length, &sg_dma_address(sg),
			sg_dma_len(sg));
	}
}

static inline u64 nvme_qdma_req_get_addr_prefix(u8 func_id, u16 qid, u16 cid)
{
	/* Addr layout:
	 * __le64	addr   : 48;
	 * __le64	cid    : 10;
	 * __le64	qid    : 3 ;
	 * __le64   fun_cid: 2 ;
	 * __le64	one    : 1 ;
	 */
	return (1ULL << 63) | (((u64)func_id) << 61) | (((u64)qid) << 58) | (((u64)cid) << 48);
}

static blk_status_t nvme_qdma_setup_prps(struct nvme_qdma_ctrl *ctrl,
										 struct nvme_qdma_request *iod, struct nvme_qdma_sqe *sqe,
										 struct nvme_rw_command *cmnd, int length, int dma_dir,
										 bool fill_lba)
{
	struct scatterlist *sg = sqe->sg;
	int dma_len = sg_dma_len(sg);
	u64 dma_addr = sg_dma_address(sg);
	u32 page_size = ctrl->ctrl.page_size;
	int offset = dma_addr & (page_size - 1);
	__le64 *prp_list;
	dma_addr_t first_dma;
	dma_addr_t prp_dma = pci_map_page(ctrl->device->xpdev->pdev, virt_to_page(sqe->sgl_buf),
									  0, PAGE_SIZE, dma_dir);
	int i;

	/* get PF id */
	unsigned long xdev_hndl = ctrl->device->xpdev->dev_hndl;
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)xdev_hndl;
	//pr_debug("setup prps: PF id = %u\n", xdev->func_id);

	u64 addr_prefix = nvme_qdma_req_get_addr_prefix(xdev->func_id, iod->queue->qid, cmnd->command_id);

	length -= (page_size - offset);
	if (length <= 0) {
		first_dma = 0;
		goto done;
	}

	dma_len -= (page_size - offset);
	if (dma_len) {
		dma_addr += (page_size - offset);
	} else {
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}

	if (length <= page_size) {
		first_dma = dma_addr;
		goto done;
	}

	prp_list = sqe->sgl_buf;
	if (!prp_list) {
		first_dma = dma_addr;
		sqe->npages = -1;
		return BLK_STS_RESOURCE;
	}

	first_dma = prp_dma;
	i = 0;
	for (;;) {
		BUG_ON(i == page_size >> 3);
		prp_list[i] = cpu_to_le64((dma_addr) | addr_prefix);
		i++;
		dma_len -= page_size;
		dma_addr += page_size;
		length -= page_size;
		if (length <= 0)
			break;
		if (dma_len > 0)
			continue;
		if (unlikely(dma_len < 0))
			goto bad_sgl;
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}
done:
	if (unlikely(fill_lba)) {
		u64 *lba1 = (u64 *)&cmnd->slba; // cdw10 and cdw11
		u64 *lba2 = (u64 *)&cmnd->reftag; // cdw14 and cdw15
		*lba1 = cpu_to_le64(sg_dma_address(sqe->sg) | addr_prefix);
		*lba2 = cpu_to_le64(first_dma) | addr_prefix;
		pr_debug("LBA 1 is 0x%llX, LBA 2 is 0x%llX\n", *lba1, *lba2);
	} else {
		cmnd->dptr.prp1 = cpu_to_le64(sg_dma_address(sqe->sg) | addr_prefix);
		cmnd->dptr.prp2 = cpu_to_le64(first_dma) | addr_prefix;
		pr_debug("PRP 1 is 0x%llX, PRP 2 is 0x%llX\n", cmnd->dptr.prp1, cmnd->dptr.prp2);
	}
	return BLK_STS_OK;
bad_sgl:
	WARN(DO_ONCE(nvmq_print_sgl, sqe->sg, sqe->nents),
			"Invalid SGL for payload:%d nents:%d\n",
			length, sqe->nents);
	return BLK_STS_IOERR;
}

static int nvme_qdma_create_queue_ib(struct nvme_qdma_queue *queue)
{
	unsigned long dev_hndl;
	// const int send_wr_factor = 3;			/* MR, SEND, INV */
	// const int cq_factor = send_wr_factor + 1;	/* + RECV */
	// int comp_vector, idx = nvme_qdma_queue_idx(queue);
	// enum ib_poll_context poll_ctx;
	int ret, i;

	dev_hndl = queue->device->xpdev->dev_hndl;

	pr_debug("dev_hndl: %lu\n", dev_hndl);

	// queue->device = nvme_qdma_find_get_device(queue->cm_id);
	// if (!queue->device) {
	// 	dev_err(queue->cm_id->device->dev.parent,
	// 		"no client data found!\n");
	// 	return -ECONNREFUSED;
	// }
	// ibdev = queue->device->dev;

	/*
	 * Spread I/O queues completion vectors according their queue index.
	 * Admin queues can always go on completion vector 0.
	 */
	// comp_vector = (idx == 0 ? idx : idx - 1) % ibdev->num_comp_vectors;

	/* Polling queues need direct cq polling context */
	// if (nvme_qdma_poll_queue(queue))
	// 	poll_ctx = IB_POLL_DIRECT;
	// else
	// 	poll_ctx = IB_POLL_SOFTIRQ;

	/* +1 for ib_stop_cq */
	// queue->ib_cq = ib_alloc_cq(ibdev, queue,
	// 			cq_factor * queue->queue_size + 1,
	// 			comp_vector, poll_ctx);
	// if (IS_ERR(queue->ib_cq)) {
	// 	ret = PTR_ERR(queue->ib_cq);
	// 	goto out_put_dev;
	// }

	ret = nvme_qdma_create_qp(queue);
	if (ret)
		goto out_destroy_ib_cq;

	queue->rsp_ring = nvme_qdma_alloc_ring(dev_hndl, queue->qid, queue->queue_size,
			sizeof(struct nvme_completion), DMA_FROM_DEVICE);
	if (!queue->rsp_ring) {
		ret = -ENOMEM;
		goto out_destroy_qp;
	}

	for (i = 0; i < queue->queue_size; i++) {
		queue->rsp_ring[i].queue = queue;
	}

	/*
	 * Currently we don't use SG_GAPS MR's so if the first entry is
	 * misaligned we'll end up using two entries for a single data page,
	 * so one additional entry is required.
	 */
	// pages_per_mr = nvme_qdma_get_max_fr_pages(ibdev) + 1;
	// ret = ib_mr_pool_init(queue->qp, &queue->qp->rdma_mrs,
	// 		      queue->queue_size,
	// 		      IB_MR_TYPE_MEM_REG,
	// 		      pages_per_mr, 0);
	// if (ret) {
	// 	dev_err(queue->ctrl->ctrl.device,
	// 		"failed to initialize MR pool sized %d for QID %d\n",
	// 		queue->queue_size, idx);
	// 	goto out_destroy_ring;
	// }

	set_bit(nvme_qdma_Q_TR_READY, &queue->flags);

	return 0;

out_destroy_qp:
	qdma_destroy_qp(dev_hndl, queue->qe_qp.h2c_qhndl);
	qdma_destroy_qp(dev_hndl, queue->qe_qp.c2h_qhndl);
out_destroy_ib_cq:
	// ib_free_cq(queue->ib_cq);
// out_put_dev:
	nvme_qdma_dev_put(queue->device);
	return ret;
}

static int nvme_qdma_fill_desc_byp(struct nvme_qdma_queue *queue)
{
	struct qdma_desc_byp_ctrl *byp = &queue->data_h2c_byp;
	unsigned long dev_hndl = queue->device->xpdev->dev_hndl;
	int ret;

	while (byp->num_outstanding < QDMA_BYP_RING_SZ - 1) {
		ret = qdma_request_submit(dev_hndl, queue->data_qp.h2c_qhndl, &byp->qdma_reqs[byp->pidx]);
		if (unlikely(ret < 0)) {
			dev_err(queue->ctrl->ctrl.device,
					"qdma_request_submit failed with error code %d\n", ret);
			return ret;
		}
		byp->pidx = (byp->pidx + 1) % (QDMA_BYP_RING_SZ - 1);
		byp->num_outstanding++;
	}

	pr_debug("data h2c queue outstanding %u\n", byp->num_outstanding);

	return 0;
}

static int nvme_qdma_data_h2c_done(struct qdma_request *qdma_req, unsigned int bytes_done, int err)
{
	struct qdma_desc_byp_ctrl *byp = (struct qdma_desc_byp_ctrl *)qdma_req->uld_data;
	struct nvme_qdma_queue *queue = container_of(byp, struct nvme_qdma_queue, data_h2c_byp);
	unsigned long xdev_hndl = queue->device->xpdev->dev_hndl;
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)xdev_hndl;

	pr_debug("byp is 0x%p\n", byp);
	// pr_debug("data h2c queue cidx: %u\n", byp->cidx);

	if (unlikely(err)) {
		pr_err("Failed to do data qdma request: %d\n", err);
	}
	byp->num_outstanding--;

	pr_debug("Queue %u outstanding %u", nvme_qid_to_qdma_qid(queue->qid, true, xdev->func_id), byp->num_outstanding);

	pr_debug("Return\n");
	
	return 0;
}

// static int nvme_qdma_bypass_desc_fill(void *q_hndl, enum qdma_q_mode q_mode,
// 						  enum qdma_q_dir, struct qdma_request *req)
// {
// 	return 0;
// }

static int nvme_qdma_alloc_desc_byp(struct nvme_qdma_queue *queue)
{
	struct qdma_desc_byp_ctrl *byp = &queue->data_h2c_byp;
	struct pci_dev *pdev = queue->device->xpdev->pdev;
	struct qdma_sw_sg *sg = &byp->sgl;
	int i, sgcnt;

	byp->qdma_reqs = kzalloc(sizeof(struct qdma_request) * QDMA_BYP_RING_SZ, GFP_KERNEL);
	byp->dummy_pg = kzalloc(PAGE_SIZE, GFP_KERNEL | GFP_DMA);

	if (!byp->qdma_reqs || !byp->dummy_pg) {
		return -ENOMEM;
	}

	sgcnt = qdma_map_page(NULL, byp->dummy_pg, sg, true);
	if (unlikely(sgcnt != 1)) {
		pr_err("Failed to allocate aligned page!\n");
		return -ENOMEM;
	}
	sg->dma_addr = pci_map_page(pdev, sg->pg, 0, PAGE_SIZE, DMA_TO_DEVICE);
	if (unlikely(pci_dma_mapping_error(pdev, sg->dma_addr))) {
		pr_err("map sgl failed, sg %d, %u.\n", i, sg->len);
		return -EIO;
	}

	pr_debug("Dummy sg %p pg %p paddr %llX\n", sg, sg->pg, sg->dma_addr);

	for (i = 0; i < QDMA_BYP_RING_SZ; i++) {
		byp->qdma_reqs[i].write = 1;
		byp->qdma_reqs[i].dma_mapped = true;
		byp->qdma_reqs[i].udd_len = 0;
		byp->qdma_reqs[i].ep_addr = 0;
		byp->qdma_reqs[i].no_memcpy = 1;
		byp->qdma_reqs[i].timeout_ms = nvmq_io_timeout * 1000;
		byp->qdma_reqs[i].uld_data = (unsigned long)byp;
		byp->qdma_reqs[i].fp_done = nvme_qdma_data_h2c_done;
		byp->qdma_reqs[i].sgl = sg;
		byp->qdma_reqs[i].sgcnt = 1;
		byp->qdma_reqs[i].count = PAGE_SIZE; // Dummy value
	}

	return 0;
}

static int nvme_qdma_alloc_queue(struct nvme_qdma_ctrl *ctrl,
		int idx, size_t queue_size)
{
	struct nvme_qdma_queue *queue;
	// struct sockaddr *src_addr = NULL;
	int ret;

	pr_debug("idx %u, xpdev %p\n", idx, ctrl->device->xpdev);

	queue = &ctrl->queues[idx];
	queue->ctrl = ctrl;
	queue->qid = (uint16_t)idx;
	// init_completion(&queue->cm_done);

	pr_debug("queue %p, idx %u, xpdev %p\n", queue, idx, ctrl->device->xpdev);

	if (idx > 0)
		queue->cmnd_capsule_len = ctrl->ctrl.ioccsz * 16;
	else
		queue->cmnd_capsule_len = sizeof(struct nvme_command);

	queue->queue_size = queue_size;

	// if (ctrl->ctrl.opts->mask & NVMQF_OPT_HOST_TRADDR)
	// 	src_addr = (struct sockaddr *)&ctrl->src_addr;
	
	queue->device = ctrl->device;

	ret = nvme_qdma_create_queue_ib(queue);
	if (ret)
		return ret;
	
	// if (idx != 0) {
	// 	ret = nvme_qdma_alloc_desc_byp(queue);
	// 	if (ret)
	// 		return ret;
	// }

	set_bit(nvme_qdma_Q_ALLOCATED, &queue->flags);

	return 0;

// out_destroy_cm_id:
// 	nvme_qdma_destroy_queue_ib(queue);
	return ret;
}

static void __nvme_qdma_stop_queue(struct nvme_qdma_queue *queue)
{
	// rdma_disconnect(queue->cm_id);
	// ib_drain_qp(queue->qp);
	char msg_buf[32];
	unsigned long dev_hndl = queue->device->xpdev->dev_hndl;
	qdma_queue_stop(dev_hndl, queue->qe_qp.h2c_qhndl, msg_buf, 32);
	qdma_queue_stop(dev_hndl, queue->qe_qp.c2h_qhndl, msg_buf, 32);
}

static void nvme_qdma_stop_queue(struct nvme_qdma_queue *queue)
{
	if (!test_and_clear_bit(nvme_qdma_Q_LIVE, &queue->flags))
		return;
	__nvme_qdma_stop_queue(queue);
}

static void nvme_qdma_free_queue(struct nvme_qdma_queue *queue)
{
	if (!test_and_clear_bit(nvme_qdma_Q_ALLOCATED, &queue->flags))
		return;

	nvme_qdma_destroy_queue_ib(queue);
	// rdma_destroy_id(queue->cm_id);
}

static void nvme_qdma_free_io_queues(struct nvme_qdma_ctrl *ctrl)
{
	int i;

	for (i = 1; i < ctrl->ctrl.queue_count; i++)
		nvme_qdma_free_queue(&ctrl->queues[i]);
}

static void nvme_qdma_stop_io_queues(struct nvme_qdma_ctrl *ctrl)
{
	int i;

	for (i = 1; i < ctrl->ctrl.queue_count; i++)
		nvme_qdma_stop_queue(&ctrl->queues[i]);
}

static int nvme_qdma_start_queue(struct nvme_qdma_ctrl *ctrl, int idx)
{
	struct nvme_qdma_queue *queue = &ctrl->queues[idx];
	bool poll = nvme_qdma_poll_queue(queue);
	int ret;

	// for (i = 0; i < queue->queue_size; i++) {
	// 	ret = nvme_qdma_post_rsp(queue, &queue->rsp_ring[i]);
	// 	if (ret) {
	// 		pr_err("failed to post qdma recv: %d\n", ret);
	// 		return ret;
	// 	}
	// }
	
	// if (idx > 0) {
	// 	ret = nvme_qdma_fill_desc_byp(queue);
	// 	if (ret)
	// 		return ret;
	// }

	if (idx)
		ret = nvmqf_connect_io_queue(&ctrl->ctrl, idx, queue->data_c2h_pfch_tag, poll);
	else
		ret = nvmqf_connect_admin_queue(&ctrl->ctrl);

	if (!ret) {
		set_bit(nvme_qdma_Q_LIVE, &queue->flags);
	} else {
		if (test_bit(nvme_qdma_Q_ALLOCATED, &queue->flags))
			__nvme_qdma_stop_queue(queue);
		dev_info(ctrl->ctrl.device,
			"failed to connect queue: %d ret=%d\n", idx, ret);
	}
	return ret;
}

static int nvme_qdma_start_io_queues(struct nvme_qdma_ctrl *ctrl)
{
	int i, ret = 0;

	for (i = 1; i < ctrl->ctrl.queue_count; i++) {
		ret = nvme_qdma_start_queue(ctrl, i);
		if (ret)
			goto out_stop_queues;
	}

	return 0;

out_stop_queues:
	for (i--; i >= 1; i--)
		nvme_qdma_stop_queue(&ctrl->queues[i]);
	return ret;
}

static int nvme_qdma_alloc_io_queues(struct nvme_qdma_ctrl *ctrl)
{
	struct nvmqf_ctrl_options *opts = ctrl->ctrl.opts;
	// unsigned long dev_hndl = ctrl->device->xpdev->dev_hndl;
	unsigned int nr_io_queues, nr_default_queues;
	unsigned int nr_read_queues, nr_poll_queues;
	int i, ret;

	nr_read_queues = min_t(unsigned int, NVME_QDMA_MAX_IO_QUEUES + 1,
				min(opts->nr_io_queues, num_online_cpus()));
	nr_default_queues =  min_t(unsigned int, NVME_QDMA_MAX_IO_QUEUES + 1,
				min(opts->nr_write_queues, num_online_cpus()));
	nr_poll_queues = min(opts->nr_poll_queues, num_online_cpus());
	pr_warn("nr_read_queues %u, nr_default_queues %u, nr_poll_queues %u, num_online_cpus %u\n", nr_read_queues, nr_default_queues, nr_poll_queues, num_online_cpus());
	nr_io_queues = nr_read_queues + nr_default_queues + nr_poll_queues;

	ret = nvmq_set_queue_count(&ctrl->ctrl, &nr_io_queues);
	if (ret)
		return ret;

	if (nr_io_queues == 0) {
		dev_err(ctrl->ctrl.device,
			"unable to set any I/O queues\n");
		return -ENOMEM;
	}

	ctrl->ctrl.queue_count = nr_io_queues + 1;
	dev_info(ctrl->ctrl.device,
		"creating %d I/O queues.\n", nr_io_queues);

	if (opts->nr_write_queues && nr_read_queues < nr_io_queues) {
		/*
		 * separate read/write queues
		 * hand out dedicated default queues only after we have
		 * sufficient read queues.
		 */
		ctrl->io_queues[HCTX_TYPE_READ] = nr_read_queues;
		nr_io_queues -= ctrl->io_queues[HCTX_TYPE_READ];
		ctrl->io_queues[HCTX_TYPE_DEFAULT] =
			min(nr_default_queues, nr_io_queues);
		nr_io_queues -= ctrl->io_queues[HCTX_TYPE_DEFAULT];
	} else {
		/*
		 * shared read/write queues
		 * either no write queues were requested, or we don't have
		 * sufficient queue count to have dedicated default queues.
		 */
		ctrl->io_queues[HCTX_TYPE_DEFAULT] =
			min(nr_read_queues, nr_io_queues);
		nr_io_queues -= ctrl->io_queues[HCTX_TYPE_DEFAULT];
	}

	if (opts->nr_poll_queues && nr_io_queues) {
		/* map dedicated poll queues only if we have queues left */
		ctrl->io_queues[HCTX_TYPE_POLL] =
			min(nr_poll_queues, nr_io_queues);
	}

	for (i = 1; i < ctrl->ctrl.queue_count; i++) {
		ret = nvme_qdma_alloc_queue(ctrl, i,
				ctrl->ctrl.sqsize + 1);
		if (ret)
			goto out_free_queues;
	}

	for (i = 1; i <= NVME_QDMA_MAX_IO_QUEUES; i++) {
		ret = nvme_qdma_create_h2c_data_qp(ctrl->device, i);
		if (ret)
			goto out_free_queues;
	}

	// Create PRP fetch queue
	ret = nvme_qdma_create_h2c_data_qp(ctrl->device, QDMA_PRP_FETCH_QID - 8);
	if (ret) {
		pr_err("Failed to create PRP fetch queue: %d\n", ret);
		goto out_free_queues;
	}

	return 0;

out_free_queues:
	for (i--; i >= 1; i--)
		nvme_qdma_free_queue(&ctrl->queues[i]);

	return ret;
}

static struct blk_mq_tag_set *nvme_qdma_alloc_tagset(struct nvmq_ctrl *nctrl,
		bool admin)
{
	struct nvme_qdma_ctrl *ctrl = to_qdma_ctrl(nctrl);
	struct blk_mq_tag_set *set;
	int ret;

	if (admin) {
		set = &ctrl->admin_tag_set;
		memset(set, 0, sizeof(*set));
		set->ops = &nvme_qdma_admin_mq_ops;
		set->queue_depth = NVME_AQ_MQ_TAG_DEPTH;
		set->reserved_tags = 2; /* connect + keep-alive */
		set->numa_node = nctrl->numa_node;
		set->cmd_size = sizeof(struct nvme_qdma_request) +
			SG_CHUNK_SIZE * sizeof(struct scatterlist);
		set->driver_data = ctrl;
		set->nr_hw_queues = 1;
		set->timeout = ADMIN_TIMEOUT;
		set->flags = BLK_MQ_F_NO_SCHED;
	} else {
		set = &ctrl->tag_set;
		memset(set, 0, sizeof(*set));
		set->ops = &nvme_qdma_mq_ops;
		set->queue_depth = nctrl->sqsize + 1;
		set->reserved_tags = 1; /* fabric connect */
		set->numa_node = nctrl->numa_node;
		set->flags = BLK_MQ_F_SHOULD_MERGE;
		set->cmd_size = sizeof(struct nvme_qdma_request) +
			SG_CHUNK_SIZE * sizeof(struct scatterlist);
		set->driver_data = ctrl;
		set->nr_hw_queues = nctrl->queue_count - 1;
		set->timeout = NVMQ_IO_TIMEOUT;
		set->nr_maps = nctrl->opts->nr_poll_queues ? HCTX_MAX_TYPES : 2;
	}

	ret = blk_mq_alloc_tag_set(set);
	if (ret)
		return ERR_PTR(ret);

	return set;
}

static void nvme_qdma_destroy_admin_queue(struct nvme_qdma_ctrl *ctrl,
		bool remove)
{
	if (remove) {
		blk_cleanup_queue(ctrl->ctrl.admin_q);
		blk_cleanup_queue(ctrl->ctrl.fabrics_q);
		blk_mq_free_tag_set(ctrl->ctrl.admin_tagset);
	}
	if (ctrl->async_event_sqe.qe_data) {
		cancel_work_sync(&ctrl->ctrl.async_event_work);
		kfree(ctrl->async_event_sqe.qe_data);
		ctrl->async_event_sqe.qe_data = NULL;
	}
	nvme_qdma_free_queue(&ctrl->queues[0]);
}

static int nvme_qdma_configure_admin_queue(struct nvme_qdma_ctrl *ctrl,
		bool new)
{
	int error;

	error = nvme_qdma_alloc_queue(ctrl, 0, NVME_AQ_DEPTH);
	if (error)
		return error;

	// ctrl->device = ctrl->queues[0].device;
	ctrl->ctrl.numa_node = dev_to_node(&ctrl->device->xpdev->pdev->dev);

	ctrl->max_fr_pages = NVME_QDMA_MAX_SEGMENTS;

	/*
	 * Bind the async event SQE DMA mapping to the admin queue lifetime.
	 * It's safe, since any chage in the underlying RDMA device will issue
	 * error recovery and queue re-creation.
	 */
	ctrl->async_event_sqe.qe_data = kzalloc(sizeof(struct nvme_command), GFP_KERNEL);
	if (!ctrl->async_event_sqe.qe_data)
		goto out_free_queue;

	if (new) {
		ctrl->ctrl.admin_tagset = nvme_qdma_alloc_tagset(&ctrl->ctrl, true);
		if (IS_ERR(ctrl->ctrl.admin_tagset)) {
			error = PTR_ERR(ctrl->ctrl.admin_tagset);
			goto out_free_async_qe;
		}

		ctrl->ctrl.fabrics_q = blk_mq_init_queue(&ctrl->admin_tag_set);
		if (IS_ERR(ctrl->ctrl.fabrics_q)) {
			error = PTR_ERR(ctrl->ctrl.fabrics_q);
			goto out_free_tagset;
		}

		ctrl->ctrl.admin_q = blk_mq_init_queue(&ctrl->admin_tag_set);
		if (IS_ERR(ctrl->ctrl.admin_q)) {
			error = PTR_ERR(ctrl->ctrl.admin_q);
			goto out_cleanup_fabrics_q;
		}
	}

	error = nvme_qdma_start_queue(ctrl, 0);
	if (error)
		goto out_cleanup_queue;

	error = nvmq_enable_ctrl(&ctrl->ctrl);
	if (error)
		goto out_stop_queue;

	ctrl->ctrl.max_segments = ctrl->max_fr_pages;
	ctrl->ctrl.max_hw_sectors = ctrl->max_fr_pages << (ilog2(SZ_4K) - 9);

	blk_mq_unquiesce_queue(ctrl->ctrl.admin_q);

	error = nvmq_init_identify(&ctrl->ctrl);
	if (error)
		goto out_quiesce_queue;

	return 0;

out_quiesce_queue:
	blk_mq_quiesce_queue(ctrl->ctrl.admin_q);
	blk_sync_queue(ctrl->ctrl.admin_q);
out_stop_queue:
	nvme_qdma_stop_queue(&ctrl->queues[0]);
	nvmq_cancel_admin_tagset(&ctrl->ctrl);
out_cleanup_queue:
	if (new)
		blk_cleanup_queue(ctrl->ctrl.admin_q);
out_cleanup_fabrics_q:
	if (new)
		blk_cleanup_queue(ctrl->ctrl.fabrics_q);
out_free_tagset:
	if (new)
		blk_mq_free_tag_set(ctrl->ctrl.admin_tagset);
out_free_async_qe:
	if (ctrl->async_event_sqe.qe_data) {
		kfree(ctrl->async_event_sqe.qe_data);
		ctrl->async_event_sqe.qe_data = NULL;
	}
out_free_queue:
	nvme_qdma_free_queue(&ctrl->queues[0]);
	return error;
}

static void nvme_qdma_destroy_io_queues(struct nvme_qdma_ctrl *ctrl,
		bool remove)
{
	if (remove) {
		blk_cleanup_queue(ctrl->ctrl.connect_q);
		blk_mq_free_tag_set(ctrl->ctrl.tagset);
	}
	nvme_qdma_free_io_queues(ctrl);
}

static int nvme_qdma_configure_io_queues(struct nvme_qdma_ctrl *ctrl, bool new)
{
	int ret;

	ret = nvme_qdma_alloc_io_queues(ctrl);
	if (ret)
		return ret;

	if (new) {
		ctrl->ctrl.tagset = nvme_qdma_alloc_tagset(&ctrl->ctrl, false);
		if (IS_ERR(ctrl->ctrl.tagset)) {
			ret = PTR_ERR(ctrl->ctrl.tagset);
			goto out_free_io_queues;
		}

		ctrl->ctrl.connect_q = blk_mq_init_queue(&ctrl->tag_set);
		if (IS_ERR(ctrl->ctrl.connect_q)) {
			ret = PTR_ERR(ctrl->ctrl.connect_q);
			goto out_free_tag_set;
		}
	}

	ret = nvme_qdma_start_io_queues(ctrl);
	if (ret)
		goto out_cleanup_connect_q;

	if (!new) {
		nvmq_start_queues(&ctrl->ctrl);
		if (!nvmq_wait_freeze_timeout(&ctrl->ctrl, NVMQ_IO_TIMEOUT)) {
			/*
			 * If we timed out waiting for freeze we are likely to
			 * be stuck.  Fail the controller initialization just
			 * to be safe.
			 */
			ret = -ENODEV;
			goto out_wait_freeze_timed_out;
		}
		blk_mq_update_nr_hw_queues(ctrl->ctrl.tagset,
			ctrl->ctrl.queue_count - 1);
		nvmq_unfreeze(&ctrl->ctrl);
	}

	return 0;

out_wait_freeze_timed_out:
	nvmq_stop_queues(&ctrl->ctrl);
	nvmq_sync_io_queues(&ctrl->ctrl);
	nvme_qdma_stop_io_queues(ctrl);
out_cleanup_connect_q:
	nvmq_cancel_tagset(&ctrl->ctrl);
	if (new)
		blk_cleanup_queue(ctrl->ctrl.connect_q);
out_free_tag_set:
	if (new)
		blk_mq_free_tag_set(ctrl->ctrl.tagset);
out_free_io_queues:
	nvme_qdma_free_io_queues(ctrl);
	return ret;
}

static void nvme_qdma_teardown_admin_queue(struct nvme_qdma_ctrl *ctrl,
		bool remove)
{
	blk_mq_quiesce_queue(ctrl->ctrl.admin_q);
	blk_sync_queue(ctrl->ctrl.admin_q);
	nvme_qdma_stop_queue(&ctrl->queues[0]);
	if (ctrl->ctrl.admin_tagset) {
		blk_mq_tagset_busy_iter(ctrl->ctrl.admin_tagset,
			nvmq_cancel_request, &ctrl->ctrl);
		blk_mq_tagset_wait_completed_request(ctrl->ctrl.admin_tagset);
	}
	if (remove)
		blk_mq_unquiesce_queue(ctrl->ctrl.admin_q);
	nvme_qdma_destroy_admin_queue(ctrl, remove);
}

static void nvme_qdma_teardown_io_queues(struct nvme_qdma_ctrl *ctrl,
		bool remove)
{
	if (ctrl->ctrl.queue_count > 1) {
		nvmq_start_freeze(&ctrl->ctrl);
		nvmq_stop_queues(&ctrl->ctrl);
		nvmq_sync_io_queues(&ctrl->ctrl);
		nvme_qdma_stop_io_queues(ctrl);
		if (ctrl->ctrl.tagset) {
			blk_mq_tagset_busy_iter(ctrl->ctrl.tagset,
				nvmq_cancel_request, &ctrl->ctrl);
			blk_mq_tagset_wait_completed_request(ctrl->ctrl.tagset);
		}
		if (remove)
			nvmq_start_queues(&ctrl->ctrl);
		nvme_qdma_destroy_io_queues(ctrl, remove);
	}
}

static void nvme_qdma_stop_ctrl(struct nvmq_ctrl *nctrl)
{
	struct nvme_qdma_ctrl *ctrl = to_qdma_ctrl(nctrl);

	cancel_work_sync(&ctrl->err_work);
	cancel_delayed_work_sync(&ctrl->reconnect_work);
}

static void nvme_qdma_free_ctrl(struct nvmq_ctrl *nctrl)
{
	struct nvme_qdma_ctrl *ctrl = to_qdma_ctrl(nctrl);

	if (list_empty(&ctrl->list))
		goto free_ctrl;

	mutex_lock(&nvme_qdma_ctrl_mutex);
	list_del(&ctrl->list);
	mutex_unlock(&nvme_qdma_ctrl_mutex);

	mempool_destroy(ctrl->iod_mempool);

	mempool_destroy(ctrl->sqe_mempool);

	nvmqf_free_options(nctrl->opts);
free_ctrl:
	kfree(ctrl->queues);
	kfree(ctrl);
	ctrl = NULL;
}

static void nvme_qdma_reconnect_or_remove(struct nvme_qdma_ctrl *ctrl)
{
	/* If we are resetting/deleting then do nothing */
	if (ctrl->ctrl.state != NVME_CTRL_CONNECTING) {
		WARN_ON_ONCE(ctrl->ctrl.state == NVME_CTRL_NEW ||
			ctrl->ctrl.state == NVME_CTRL_LIVE);
		return;
	}

	if (nvmqf_should_reconnect(&ctrl->ctrl)) {
		dev_info(ctrl->ctrl.device, "Reconnecting in %d seconds...\n",
			ctrl->ctrl.opts->reconnect_delay);
		queue_delayed_work(nvmq_wq, &ctrl->reconnect_work,
				ctrl->ctrl.opts->reconnect_delay * HZ);
	} else {
		nvmq_delete_ctrl(&ctrl->ctrl);
	}
}

static int nvme_qdma_setup_ctrl(struct nvme_qdma_ctrl *ctrl, bool new)
{
	int ret = -EINVAL;
	bool changed;

	pr_debug("ctrl %p xpdev %p\n", ctrl, ctrl->device->xpdev);

	ret = nvme_qdma_configure_admin_queue(ctrl, new);
	if (ret)
		return ret;

	if (ctrl->ctrl.icdoff) {
		ret = -EOPNOTSUPP;
		dev_err(ctrl->ctrl.device, "icdoff is not supported!\n");
		goto destroy_admin;
	}

	if (!(ctrl->ctrl.sgls & (1 << 2))) {
		ret = -EOPNOTSUPP;
		dev_err(ctrl->ctrl.device,
			"Mandatory keyed sgls are not supported!\n");
		goto destroy_admin;
	}

	if (ctrl->ctrl.opts->queue_size > ctrl->ctrl.sqsize + 1) {
		dev_warn(ctrl->ctrl.device,
			"queue_size %zu > ctrl sqsize %u, clamping down\n",
			ctrl->ctrl.opts->queue_size, ctrl->ctrl.sqsize + 1);
	}

	if (ctrl->ctrl.sqsize + 1 > ctrl->ctrl.maxcmd) {
		dev_warn(ctrl->ctrl.device,
			"sqsize %u > ctrl maxcmd %u, clamping down\n",
			ctrl->ctrl.sqsize + 1, ctrl->ctrl.maxcmd);
		ctrl->ctrl.sqsize = ctrl->ctrl.maxcmd - 1;
	}

	if (ctrl->ctrl.sgls & (1 << 20))
		ctrl->use_inline_data = true;

	if (ctrl->ctrl.queue_count > 1) {
		ret = nvme_qdma_configure_io_queues(ctrl, new);
		if (ret)
			goto destroy_admin;
	}

	changed = nvmq_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_LIVE);
	if (!changed) {
		/* state change failure is ok if we're in DELETING state */
		WARN_ON_ONCE(ctrl->ctrl.state != NVME_CTRL_DELETING);
		ret = -EINVAL;
		goto destroy_io;
	}

	nvmq_start_ctrl(&ctrl->ctrl);
	return 0;

destroy_io:
	if (ctrl->ctrl.queue_count > 1) {
		nvmq_stop_queues(&ctrl->ctrl);
		nvmq_sync_io_queues(&ctrl->ctrl);
		nvme_qdma_stop_io_queues(ctrl);
		nvmq_cancel_tagset(&ctrl->ctrl);
		nvme_qdma_destroy_io_queues(ctrl, new);
	}
destroy_admin:
	blk_mq_quiesce_queue(ctrl->ctrl.admin_q);
	blk_sync_queue(ctrl->ctrl.admin_q);
	nvme_qdma_stop_queue(&ctrl->queues[0]);
	nvmq_cancel_admin_tagset(&ctrl->ctrl);
	nvme_qdma_destroy_admin_queue(ctrl, new);
	return ret;
}

static void nvme_qdma_reconnect_ctrl_work(struct work_struct *work)
{
	struct nvme_qdma_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvme_qdma_ctrl, reconnect_work);

	++ctrl->ctrl.nr_reconnects;

	if (nvme_qdma_setup_ctrl(ctrl, false))
		goto requeue;

	dev_info(ctrl->ctrl.device, "Successfully reconnected (%d attempts)\n",
			ctrl->ctrl.nr_reconnects);

	ctrl->ctrl.nr_reconnects = 0;

	return;

requeue:
	dev_info(ctrl->ctrl.device, "Failed reconnect attempt %d\n",
			ctrl->ctrl.nr_reconnects);
	nvme_qdma_reconnect_or_remove(ctrl);
}

static void nvme_qdma_error_recovery_work(struct work_struct *work)
{
	struct nvme_qdma_ctrl *ctrl = container_of(work,
			struct nvme_qdma_ctrl, err_work);

	nvmq_stop_keep_alive(&ctrl->ctrl);
	flush_work(&ctrl->ctrl.async_event_work);
	nvme_qdma_teardown_io_queues(ctrl, false);
	nvmq_start_queues(&ctrl->ctrl);
	nvme_qdma_teardown_admin_queue(ctrl, false);
	blk_mq_unquiesce_queue(ctrl->ctrl.admin_q);

	if (!nvmq_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {
		/* state change failure is ok if we're in DELETING state */
		WARN_ON_ONCE(ctrl->ctrl.state != NVME_CTRL_DELETING);
		return;
	}

	nvme_qdma_reconnect_or_remove(ctrl);
}

static void nvme_qdma_error_recovery(struct nvme_qdma_ctrl *ctrl)
{
	if (!nvmq_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_RESETTING))
		return;

	dev_warn(ctrl->ctrl.device, "starting error recovery\n");
	queue_work(nvmq_reset_wq, &ctrl->err_work);
}

// static void nvme_qdma_wr_error(struct ib_cq *cq, struct ib_wc *wc,
// 		const char *op)
// {
// 	struct nvme_qdma_queue *queue = cq->cq_context;
// 	struct nvme_qdma_ctrl *ctrl = queue->ctrl;

// 	if (ctrl->ctrl.state == NVME_CTRL_LIVE)
// 		dev_info(ctrl->ctrl.device,
// 			     "%s for CQE 0x%p failed with status %s (%d)\n",
// 			     op, wc->wr_cqe,
// 			     ib_wc_status_msg(wc->status), wc->status);
// 	nvme_qdma_error_recovery(ctrl);
// }

// static void nvme_qdma_memreg_done(struct ib_cq *cq, struct ib_wc *wc)
// {
// 	if (unlikely(wc->status != IB_WC_SUCCESS))
// 		nvme_qdma_wr_error(cq, wc, "MEMREG");
// }

// static void nvme_qdma_inv_rkey_done(struct ib_cq *cq, struct ib_wc *wc)
// {
// 	struct nvme_qdma_request *req =
// 		container_of(wc->wr_cqe, struct nvme_qdma_request, reg_cqe);
// 	struct request *rq = blk_mq_rq_from_pdu(req);

// 	if (unlikely(wc->status != IB_WC_SUCCESS)) {
// 		nvme_qdma_wr_error(cq, wc, "LOCAL_INV");
// 		return;
// 	}

// 	if (refcount_dec_and_test(&req->ref))
// 		nvmq_end_request(rq, req->status, req->result);

// }

// static int nvme_qdma_inv_rkey(struct nvme_qdma_queue *queue,
// 		struct nvme_qdma_request *req)
// {
// 	struct ib_send_wr wr = {
// 		.opcode		    = IB_WR_LOCAL_INV,
// 		.next		    = NULL,
// 		.num_sge	    = 0,
// 		.send_flags	    = IB_SEND_SIGNALED,
// 		.ex.invalidate_rkey = req->mr->rkey,
// 	};

// 	req->reg_cqe.done = nvme_qdma_inv_rkey_done;
// 	wr.wr_cqe = &req->reg_cqe;

// 	return ib_post_send(queue->qp, &wr, NULL);
// }

static void nvme_qdma_unmap_data(struct nvme_qdma_queue *queue,
		struct request *rq)
{
	struct nvme_qdma_request *iod = blk_mq_rq_to_pdu(rq);
	struct nvme_qdma_sqe *sqe, *next_sqe;
	struct nvme_qdma_ctrl *ctrl = queue->ctrl;
	struct nvme_command* c = iod->u.req.cmd;
	bool is_slm_rw = false;
	if(c!=NULL)
		is_slm_rw = nvme_is_slm_rw(c);
	pr_debug("SLM RW%c\n",is_slm_rw);
    
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	struct qdma_sw_sg* sg = req->qdma_req.sgl;
	while(sg){
		if(sg->pg&&(sg!=(req->qdma_req.sgl)))
			sg->pg = NULL;
		sg = sg->next;
	}
	nvmq_cleanup_cmd(rq);

	list_for_each_entry_safe(sqe, next_sqe, &iod->sqe_list, entry) {
		if (sqe->sg) {
			//if(nvme_is_slm_rw(c)){
			//	pr_debug("kfree sqe->sg why??\n");
			//	kfree(sqe->sg);
			//}
			//else
				mempool_free(sqe->sg, queue->ctrl->iod_mempool);
			sqe->sg = NULL;
		}
		list_del(&sqe->entry);
		if (unlikely(queue->qid == 0 || ctrl->sqe_mempool == NULL)) {
			sqe_member_free(sqe);
			kfree(sqe);
			sqe = NULL;
		} else {
                        //pr_warning("mempool_free sqe\n");
			mempool_free(sqe, ctrl->sqe_mempool);
		}
	}
}

static int nvme_qdma_set_sg_null(struct nvme_command *c)
{
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;

	sg->addr = 0;
	sg->length = 0;
	// put_unaligned_le32(0, sg->key);
	sg->type = NVME_KEY_SGL_FMT_DATA_DESC << 4;
	return 0;
}

static void *qdma_next_sg(void *ptr, bool qdma_format)
{
	struct qdma_sw_sg *sg;
	struct nvme_sgl_desc *sgl;
	if (qdma_format) {
		sg = ptr;
		if (sg->pg) {
			//pr_debug("YES HAS CURR PAGES\n");
			struct qdma_sw_sg *next = sg + 1;
			sg->next = next;
			sg = next;
		}
		return sg;
	} else {
		sgl = ptr;
		if (sgl->addr) {
			return sgl + 1;
		} else {
			return sgl;
		}
	}
}

static inline void qdma_sg_set_page(struct pci_dev *pdev, void *ptr, struct page *page, unsigned int len, unsigned int offset, bool qdma_format, enum dma_data_direction dma_dir)
{
	struct qdma_sw_sg *sg;
	struct nvme_sgl_desc *sgl;
	// pr_debug("ptr is %p\n", ptr);
	if (qdma_format) {
		sg = ptr;
		sg->pg = page;
		sg->offset = offset;
		sg->len = len;
		sg->next = NULL;
	} else {
		sgl = ptr;
		sgl->addr = pci_map_page(pdev, page, 0, PAGE_SIZE, dma_dir);
		// pr_debug("sgl %p DMA address: %llX\n", sgl, sgl->addr);
		if (unlikely(pci_dma_mapping_error(pdev, sgl->addr))) {
			pr_err("map sgl failed, page %p, offset %u.\n", page, sg->len);
		}
		sgl->addr += offset;
		sgl->length = len;
		sgl->type = NVME_SGL_FMT_SEG_DESC << 4;
	}
}

static inline int __blk_bvec_map_qdma_sg(struct pci_dev *pdev, struct bio_vec bv, void *ptr, bool qdma_format, enum dma_data_direction dma_dir)
{
	if (bv.bv_len > 64) {
		uint64_t *p = page_to_virt(bv.bv_page) + bv.bv_offset;
		int i;
		pr_debug("First 64B of data %p qdma_format%d", p,qdma_format);
		for (i = 0; i < 4; i++) {
			pr_debug("%d %016llX %016llX", i, p[2 * i], p[2 * i + 1]);
		}
	}
	
	ptr = qdma_next_sg(ptr, qdma_format);
	qdma_sg_set_page(pdev, ptr, bv.bv_page, bv.bv_len, bv.bv_offset, qdma_format, dma_dir);
	return 1;
}

static inline unsigned get_max_segment_size(const struct request_queue *q,
					    struct page *start_page,
					    unsigned long offset)
{
	unsigned long mask = queue_segment_boundary(q);

	offset = mask & (page_to_phys(start_page) + offset);

	/*
	 * overflow may be triggered in case of zero page physical address
	 * on 32bit arch, use queue's max segment size when that happens.
	 */
	return min_not_zero(mask - offset + 1,
			(unsigned long)queue_max_segment_size(q));
}

static unsigned blk_bvec_map_qdma_sg(struct pci_dev *pdev, struct request_queue *q,
		struct bio_vec *bvec, void *ptr, bool qdma_format, enum dma_data_direction dma_dir)
{
	unsigned nbytes = bvec->bv_len;
	unsigned nsegs = 0, total = 0;
	
	if (bvec->bv_len > 64) {
		uint64_t *p = page_to_virt(bvec->bv_page) + bvec->bv_offset;
		int i;
		pr_debug("First 64B of data %p", p);
		for (i = 0; i < 4; i++) {
			pr_debug("%d %016llX %016llX", i, p[2 * i], p[2 * i + 1]);
		}
	}

	while (nbytes > 0) {
		unsigned offset = bvec->bv_offset + total;
		unsigned len = min(get_max_segment_size(q, bvec->bv_page,
					offset), nbytes);
		struct page *page = bvec->bv_page;

		/*
		 * Unfortunately a fair number of drivers barf on scatterlists
		 * that have an offset larger than PAGE_SIZE, despite other
		 * subsystems dealing with that invariant just fine.  For now
		 * stick to the legacy format where we never present those from
		 * the block layer, but the code below should be removed once
		 * these offenders (mostly MMC/SD drivers) are fixed.
		 */
		page += (offset >> PAGE_SHIFT);
		offset &= ~PAGE_MASK;

		ptr = qdma_next_sg(ptr, qdma_format);
		qdma_sg_set_page(pdev, ptr, page, len, offset, qdma_format, dma_dir);

		total += len;
		nbytes -= len;
		nsegs++;
	}

	return nsegs;
}

static inline bool biovec_phys_mergeable(struct request_queue *q,
		struct bio_vec *vec1, struct bio_vec *vec2)
{
	unsigned long mask = queue_segment_boundary(q);
	phys_addr_t addr1 = page_to_phys(vec1->bv_page) + vec1->bv_offset;
	phys_addr_t addr2 = page_to_phys(vec2->bv_page) + vec2->bv_offset;

	if (addr1 + vec1->bv_len != addr2)
		return false;
	// if (xen_domain() && !xen_biovec_phys_mergeable(vec1, vec2->bv_page))
	// 	return false;
	if ((addr1 | mask) != ((addr2 + vec2->bv_len - 1) | mask))
		return false;
	return true;
}

/* only try to merge bvecs into one sg if they are from two bios */
static inline bool
__blk_segment_map_qdma_sg_merge(struct request_queue *q, struct bio_vec *bvec,
			   struct bio_vec *bvprv, void *ptr, bool qdma_format)
{

	int nbytes = bvec->bv_len;
	struct qdma_sw_sg *sg;
	struct nvme_sgl_desc *sgl;

	if (!ptr)
		return false;

	if (!biovec_phys_mergeable(q, bvprv, bvec))
		return false;

	if (qdma_format) {
		sg = ptr;
		if (sg->len + nbytes > queue_max_segment_size(q))
			return false;

		sg->len += nbytes;
	} else {
		sgl = ptr;
		if (sgl->length + nbytes > queue_max_segment_size(q))
			return false;
		
		sgl->length += nbytes;
	}

	return true;
}

static int __blk_bios_map_qdma_sg(struct pci_dev *pdev, struct request_queue *q, struct bio *bio,
			     void *ptr, bool qdma_format, enum dma_data_direction dma_dir)
{
	struct bio_vec uninitialized_var(bvec), bvprv = { NULL };
	struct bvec_iter iter;
	int nsegs = 0;
	bool new_bio = false;

	for_each_bio(bio) {
		bio_for_each_bvec(bvec, bio, iter) {
			/*
			 * Only try to merge bvecs from two bios given we
			 * have done bio internal merge when adding pages
			 * to bio
			 */
			if (new_bio &&
			    __blk_segment_map_qdma_sg_merge(q, &bvec, &bvprv, ptr, qdma_format))
				goto next_bvec;

			if (bvec.bv_offset + bvec.bv_len <= PAGE_SIZE){
				//pr_debug("ENTER A BVOFFSET%d,LEN%d\n",bvec.bv_offset,bvec.bv_len);
				nsegs += __blk_bvec_map_qdma_sg(pdev, bvec, ptr, qdma_format, dma_dir);
			}
			else{
				nsegs += blk_bvec_map_qdma_sg(pdev, q, &bvec, ptr, qdma_format, dma_dir);
				//pr_debug("ENTER B BVOFFSET%d,LEN%d\n",bvec.bv_offset,bvec.bv_len);
			}
 next_bvec:
			new_bio = false;
		}
		if (likely(bio->bi_iter.bi_size)) {
			bvprv = bvec;
			new_bio = true;
		}
	}

	return nsegs;
}

static inline void qdma_sg_len_add(void *ptr, unsigned int len, bool qdma_format)
{
	struct qdma_sw_sg *sg;
	struct nvme_sgl_desc *sgl;

	if (qdma_format) {
		sg = ptr;
		sg->len += len;
	} else {
		sgl = ptr;
		sgl->length += len;
	}
}

/*
 * map a request to qdma_sw_sg, return number of sg entries setup. Caller
 * must make sure sg can hold rq->nr_phys_segments entries
 */
static int blk_qd_map_qdma_sg(struct pci_dev *pdev, struct request_queue *q, struct request *rq,
		  void *ptr, bool qdma_format, enum dma_data_direction dma_dir)
{
	int nsegs = 0;

	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD){
		nsegs = __blk_bvec_map_qdma_sg(pdev, rq->special_vec, ptr, qdma_format, dma_dir);
		//pr_debug("ENTER A\n");
	}
	else if (rq->bio && bio_op(rq->bio) == REQ_OP_WRITE_SAME){
		nsegs = __blk_bvec_map_qdma_sg(pdev, bio_iovec(rq->bio), ptr, qdma_format, dma_dir);
		//pr_debug("ENTER B\n");
	}
	else if (rq->bio){
		nsegs = __blk_bios_map_qdma_sg(pdev, q, rq->bio, ptr, qdma_format, dma_dir);
		//pr_debug("ENTER C\n");
	}

	if (unlikely(rq->rq_flags & RQF_COPY_USER) &&
	    (blk_rq_bytes(rq) & q->dma_pad_mask)) {
		unsigned int pad_len =
			(q->dma_pad_mask & ~blk_rq_bytes(rq)) + 1;
		qdma_sg_len_add(ptr, pad_len, qdma_format);
		rq->extra_len += pad_len;
	}

	if (q->dma_drain_size && q->dma_drain_needed(rq)) {
		if (op_is_write(req_op(rq)))
			memset(q->dma_drain_buffer, 0, q->dma_drain_size);

		// sg_unmark_end(sg);
		ptr = qdma_next_sg(ptr, qdma_format);
		qdma_sg_set_page(pdev, ptr, virt_to_page(q->dma_drain_buffer),
			    q->dma_drain_size,
			    ((unsigned long)q->dma_drain_buffer) &
			    (PAGE_SIZE - 1), qdma_format, dma_dir);
		nsegs++;
		rq->extra_len += q->dma_drain_size;
	}

	// if (sg)
	// 	sg_mark_end(sg);

	/*
	 * Something must have been wrong if the figured number of
	 * segment is bigger than number of req's physical segments
	 */
	WARN_ON(nsegs > blk_rq_nr_phys_segments(rq));

	return nsegs;
}

static unsigned int nvme_qdma_qe_to_sg(void *qe, struct qdma_sw_sg *sg, bool sqe)
{
	if (sqe) {
		struct nvme_qdma_sqe *sqe = qe;
		sg->pg = virt_to_page(sqe->qe_data);
		sg->len = sizeof(struct nvme_command);
		sg->offset = (uintptr_t)sqe->qe_data & (PAGE_SIZE - 1);
	} else {
		struct nvme_qdma_cqe *cqe = qe;
		sg->pg = virt_to_page(cqe->qe_data);
		sg->len = NVME_QDMA_RSP_SIZE;
		sg->offset = (uintptr_t)cqe->qe_data & (PAGE_SIZE - 1);
	}
	sg->next = NULL;
	return 1;
}

static int map_nvmq_req_to_sg(struct request_queue *q, struct request *rq,
		  struct qdma_sw_sg *sg, enum nvmq_map_option option, bool sqe)
{
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	struct nvme_qdma_sqe *qe;
	int cnt = 0;
	bool first_qe = true;
	switch (option) {
		case QE:
			list_for_each_entry(qe, &req->sqe_list, entry) {
				if (!first_qe) {
					sg = qdma_next_sg(sg, true);
				} else {
					first_qe = false;
				}
				cnt += nvme_qdma_qe_to_sg(qe, sg, sqe);
			}
			break;
		case DATA:
			cnt = blk_qd_map_qdma_sg(NULL, q, rq, sg, true, DMA_TO_DEVICE);
			break;
		case QE_DATA:
			list_for_each_entry(qe, &req->sqe_list, entry) {
				if (!first_qe) {
					sg = qdma_next_sg(sg, true);
				} else {
					first_qe = false;
				}
				cnt += nvme_qdma_qe_to_sg(qe, sg, sqe);
			}
			if (blk_rq_nr_phys_segments(rq) > 0) {
				sg = qdma_next_sg(sg, true);
				//pr_debug("SGL MAP LEN BEFORE%d",sg->len);
				cnt += blk_qd_map_qdma_sg(NULL, q, rq, sg, true, DMA_TO_DEVICE);
				//pr_debug("SGL MAP LEN AFTER%d",sg->len);
			}
			break;
		default:
			pr_err("Invalid nvmq_map_option: %d\n", option);
	}
	return cnt;
}

static int nvme_qdma_map_sg_inline(struct nvme_qdma_queue *queue,
		struct nvme_qdma_request *req, struct nvme_command *c)
{
	struct nvme_sgl_desc *sg = &c->common.dptr.sgl;
	struct qdma_request *qdma_req = &req->qdma_req;
	int count = qdma_req->sgcnt;
	struct qdma_sw_sg *sgl = &qdma_req->sgl[1];
	u32 len = 0;
	int i;

	for (i = 1; i < count; i++, sgl++) {
		// sge->addr = sg_dma_address(sgl);
		// sge->length = sg_dma_len(sgl);
		// sge->lkey = queue->device->pd->local_dma_lkey;
		len += sgl->len;
		//pr_debug("current sgl len%d\n",sgl->len);
	}

	sg->addr = cpu_to_le64(queue->ctrl->ctrl.icdoff);
	sg->length = cpu_to_le32(len);
	sg->type = (NVME_SGL_FMT_DATA_DESC << 4) | NVME_SGL_FMT_OFFSET;

	req->num_sge += count-1;
	return 0;
}

static inline void nvme_qdma_sgl_set_prefix(struct nvme_sgl_desc *sgl, int nents, u16 qid, u16 cid)
{
	struct _nvmq_sgl_desc *qsgl = (struct _nvmq_sgl_desc *)sgl;
	int i;

	for (i = 0; i < nents; i++) {
		qsgl[i].one = 1;
		qsgl[i].qid = qid;
		qsgl[i].cid = cid;
		pr_debug("SG %d: (%llX, %X)\n", i, sgl[i].addr, sgl[i].length);
	}
}

static int nvme_qdma_map_data(struct nvme_qdma_queue *queue,
		struct request *rq, struct nvme_command *c)
{
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	struct nvme_qdma_sqe *sqe = list_first_entry_or_null(&req->sqe_list, struct nvme_qdma_sqe, entry);

	// dev->xpdev->pdev->dev;
	// unsigned long dev_hndl = dev->dev;
	int nr_mapped;

	req->num_sge = 1;
	// refcount_set(&req->ref, 2); /* send and recv completions */

	pr_debug("blk_rq_nr_phys_segments is %u\n", blk_rq_nr_phys_segments(rq));

	if (!blk_rq_nr_phys_segments(rq)) {
		if(sqe->sgl_buf != NULL)
			kfree(sqe->sgl_buf);
		sqe->sgl_buf = NULL;
		sqe->sg = NULL;
		return nvme_qdma_set_sg_null(c);
	}
	// req->sg_table.sgl = req->first_sgl;
	// ret = sg_alloc_table_chained(&req->sg_table,
	// 		blk_rq_nr_phys_segments(rq), req->sg_table.sgl,
	// 		SG_CHUNK_SIZE);

	// count = dma_map_sg(ctrl->ctrl.dev, req->sg_table.sgl, req->nents, rq_dma_dir(rq));
	// if (unlikely(count <= 0)) {
	// 	ret = -EIO;
	// 	goto out_free_table;
	// }

	if (queue->qid == 0 || nvme_is_fabrics(c)) {
		// Admin and fabrics command use inlined data, postpone setting sgl
		if ((c->common.opcode & 0x3) != 2) {
			c->common.flags |= NVME_CMD_SGL_METABUF;
		}
		if(sqe->sgl_buf != NULL){
			kfree(sqe->sgl_buf);
		}
		sqe->sgl_buf = NULL;
		return 0;
	} else {
		if(nvme_is_slm_rw(c)){
			//only slm read write could work only prepare for operate host data bigger than 128KB
			sqe->sg = kmalloc(sizeof(struct scatterlist) * 256, GFP_KERNEL);
			pr_debug("map special data\n");
			if(sqe->sg){
				sg_init_table(sqe->sg,256);
			}
		}
		else{
			sqe->sg = mempool_alloc(queue->ctrl->iod_mempool, GFP_ATOMIC);
			if(sqe->sg){
				sg_init_table(sqe->sg, blk_rq_nr_phys_segments(rq));
			}
		}
		if (!sqe->sg)
			goto out_free_sg;
		sqe->nents = blk_rq_map_sg(rq->q, rq, sqe->sg);
		if (!sqe->nents)
			goto out_free_sg;
		nr_mapped = dma_map_sg_attrs(&queue->device->xpdev->pdev->dev, sqe->sg, sqe->nents,
									 rq_dma_dir(rq), DMA_ATTR_NO_WARN);
		if (!nr_mapped)
			goto out_free_sg;
		nvmq_print_sgl(sqe->sg, sqe->nents);
		nvme_qdma_setup_prps(queue->ctrl, req, sqe, &c->rw, blk_rq_payload_bytes(rq), rq_dma_dir(rq), false);
	/*	
		if(queue->qid != 0 && c->common.opcode == 1 && c->rw.slba == 0 ){
		
			pr_warning("Write SLBA length%d\n",c->rw.length);
			struct scatterlist *sg = sqe->sg;
			unsigned int nents = sqe->nents;
			unsigned int i;
			for_each_sg(sqe->sg,sg,nents,i){
				 dma_addr_t phys = sg_phys(sg);
                		 pr_warning("sg[%d] phys_addr:%lxad offset:%d length:%d "
                        	"dma_address:%lxad dma_length:%d\n",
                        	i, phys, sg->offset, sg->length, sg_dma_address(sg),
                        	sg_dma_len(sg));
				 if(i==1 && sg->length/4096 > 1){
				 	void* virt_addr = sg_virt(sg) + PAGE_SIZE;
					if(!virt_addr){
						pr_warning("Failed to get virt address\n");		
					}else {
						int k=0;
						for(k=0;k<PAGE_SIZE/4;k++){
							if((k%8)==0) printk("\n");
							printk("%u \0",((unsigned int*)virt_addr)[k]);
					
						}
				 	}
				 }
			}
		
		}*/
		// ret = nvme_qdma_map_sgls(queue, req, c, req->nents);
		// if (ret) {
		// 	pr_err("map data to sgl failed: %d\n", ret);
		// }
		return 0;
out_free_sg:
		pr_warning("failed at out_free_sg");
		mempool_free(sqe->sg, queue->ctrl->iod_mempool);
		return BLK_STS_RESOURCE;
	}
}

static int nvme_qdma_qe_send_done(struct qdma_request *qdma_req, unsigned int bytes_done, int err)
{
	struct nvme_qdma_request *req = container_of(qdma_req, struct nvme_qdma_request, qdma_req);
	struct request *rq = blk_mq_rq_from_pdu(req);

	if (unlikely(err)) {
		pr_err("Failed to send qdma request: %d\n", err);
	}
	nvmq_diag_log_cmd("H2C_DONE", req->queue, rq, nvmq_diag_req_cmd(req), err);

	pr_debug("DEC %p: %u\n", req, refcount_read(&req->ref));
	if (refcount_dec_and_test(&req->ref)) {
		nvmq_end_request(rq, req->status, req->result);
	}
	
	return 0;
}

static int nvme_qdma_async_done(struct qdma_request *req, unsigned int bytes_done, int err)
{
	if (unlikely(err))
		pr_err("Failed to submit qdma async request: %d\n", err);
	
	kfree(req->sgl);
	
	return 0;
}

static inline void print_sqe(void *data)
{
	uint64_t *p = data;
	int i;
	pr_debug("SQE %p", p);
	for (i = 0; i < 4; i++) {
		pr_debug("%d %016llX %016llX", i, p[2 * i], p[2 * i + 1]);
	}
}

static inline void print_cqe(void *data)
{
	uint64_t *p = data;
	int i;
	pr_debug("CQE %p", p);
	for (i = 0; i < 1; i++) {
		pr_debug("%d %016llX %016llX", i, p[2 * i], p[2 * i + 1]);
	}
}

static inline void print_qdma_req(struct qdma_request *qdma_req)
{
	char str[256];
	struct qdma_sw_sg *p = qdma_req->sgl;
	snprintf(str, 256, "submit %c: qdma_req %p, sgl %p sgcnt %d, ", qdma_req->write ? 'W' : 'R', qdma_req, qdma_req->sgl, qdma_req->sgcnt);
	snprintf(str + strlen(str), 256, "pg %p %lx+%u, ", p->pg, (uintptr_t)page_to_virt(p->pg) + p->offset, p->len);

	p = p->next;
	while (p) {
		snprintf(str + strlen(str), 256, "pg %p %lx+%u, ", p->pg, (uintptr_t)page_to_virt(p->pg) + p->offset, p->len);
		p = p->next;
	}

	pr_debug("%s", str);
}

static inline void qdma_req_update_count(struct qdma_request *qdma_req)
{
	struct qdma_sw_sg *p = qdma_req->sgl;
	qdma_req->count = 0;
	int i=0;
	while (p) {
		qdma_req->count += p->len;
		p = p->next;
		i++;
	}
	if(i>qdma_req->sgcnt){
		p = qdma_req->sgl;
		while(p){
			pr_debug("ERR QDMA_REQ LEN%d\n",p->len);
			p = p->next;
		}
	}
	
}

static int nvme_qdma_post_send(struct nvme_qdma_queue *queue,
		struct qdma_request *qdma_req, bool is_aer)
{
	unsigned long dev_hndl = queue->device->xpdev->dev_hndl;
	// struct qdma_request *qdma_req = &qe->qdma_req;
	int ret;

	qdma_req->fp_done = is_aer ? nvme_qdma_async_done : nvme_qdma_qe_send_done;

	// print_qdma_req(qdma_req);

	// pr_debug("submit: dev_hndl %lu, h2c qhndl %lu, \n", dev_hndl, queue->qe_qp.h2c_qhndl);
	// print_sqe((uint8_t *)page_to_virt(qdma_req->sgl[0].pg) + qdma_req->sgl[0].offset);
	if (!is_aer) {
		struct nvme_qdma_request *req =
			container_of(qdma_req, struct nvme_qdma_request, qdma_req);
		struct request *rq = blk_mq_rq_from_pdu(req);
		nvmq_diag_log_cmd("H2C_SUBMIT", queue, rq, nvmq_diag_req_cmd(req), 0);
	}

	ret = qdma_request_submit(dev_hndl, queue->qe_qp.h2c_qhndl, qdma_req);
	if (unlikely(ret)) {
		dev_err(queue->ctrl->ctrl.device,
			     "%s failed with error code %d\n", __func__, ret);
		if (!is_aer) {
			struct nvme_qdma_request *req =
				container_of(qdma_req, struct nvme_qdma_request, qdma_req);
			struct request *rq = blk_mq_rq_from_pdu(req);
			nvmq_diag_log_cmd("H2C_SUBMIT_FAIL", queue, rq, nvmq_diag_req_cmd(req), ret);
		}
	}
	return ret;
}

static struct blk_mq_tags *nvme_qdma_tagset(struct nvme_qdma_queue *queue)
{
	u32 queue_idx = nvme_qdma_queue_idx(queue);

	if (queue_idx == 0)
		return queue->ctrl->admin_tag_set.tags[queue_idx];
	return queue->ctrl->tag_set.tags[queue_idx - 1];
}

static void nvme_qdma_process_nvme_rsp(struct nvme_qdma_queue *queue,
		struct nvme_completion *cqe)
{
	struct request *rq;
	struct nvme_qdma_request *req;

	rq = blk_mq_tag_to_rq(nvme_qdma_tagset(queue), cqe->command_id);
	if (!rq) {
		dev_err(queue->ctrl->ctrl.device,
			"tag 0x%x on QP (%lx, %lx) not found\n",
			cqe->command_id, queue->qe_qp.h2c_qhndl, queue->data_qp.h2c_qhndl);
		nvme_qdma_error_recovery(queue->ctrl);
		return;
	}
	req = blk_mq_rq_to_pdu(rq);

	req->status = cqe->status;
	req->result = cqe->result;
	nvmq_diag_log_cmd("C2H_CQE", queue, rq, nvmq_diag_req_cmd(req), cqe->status);

	// if (wc->wc_flags & IB_WC_WITH_INVALIDATE) {
	// 	if (unlikely(wc->ex.invalidate_rkey != req->mr->rkey)) {
	// 		dev_err(queue->ctrl->ctrl.device,
	// 			"Bogus remote invalidation for rkey %#x\n",
	// 			req->mr->rkey);
	// 		nvme_qdma_error_recovery(queue->ctrl);
	// 	}
	// } else if (req->mr) {
	// 	int ret;

	// 	ret = nvme_qdma_inv_rkey(queue, req);
	// 	if (unlikely(ret < 0)) {
	// 		dev_err(queue->ctrl->ctrl.device,
	// 			"Queueing INV WR for rkey %#x failed (%d)\n",
	// 			req->mr->rkey, ret);
	// 		nvme_qdma_error_recovery(queue->ctrl);
	// 	}
	// 	/* the local invalidation completion will end the request */
	// 	return;
	// }
	pr_debug("DEC %p: %u\n", req, refcount_read(&req->ref));
        if (refcount_dec_and_test(&req->ref)) {
		nvmq_end_request(rq, req->status, req->result);
	}
}

static inline void copy_page_to_req_buf(void *data, struct nvme_qdma_request *req)
{
	struct request *rq = blk_mq_rq_from_pdu(req);
	struct bio *bio = rq->bio;
	struct bio_vec uninitialized_var(bvec);
	struct bvec_iter iter;
	unsigned int src_offset = 0;
	uintptr_t dst, src;

	for_each_bio(bio) {
		bio_for_each_bvec(bvec, bio, iter) {
			if (unlikely(src_offset >= PAGE_SIZE)) {
				pr_err("More bvecs than page size\n");
				return;
			}
			dst = (uintptr_t)page_to_virt(bvec.bv_page) + bvec.bv_offset;
			src = (uintptr_t)data + src_offset;
			pr_debug("memcpy: %lu(%u) -> %lu\n", src, bvec.bv_len, dst);
			memcpy((void *)dst, (void *)src, bvec.bv_len);
			src_offset += bvec.bv_len;
		}
	}
}

static int nvme_qdma_qe_recv_done(struct qdma_request *req, unsigned int bytes_done, int err)
{
	struct nvme_qdma_cqe *qe = container_of(req, struct nvme_qdma_cqe, qdma_req);
	struct nvme_qdma_rsp *rsp = container_of(qe, struct nvme_qdma_rsp, qe);
	struct nvme_qdma_queue *queue = rsp->queue;
	// unsigned long dev_hndl = queue->device->xpdev->dev_hndl;
	struct nvme_completion *cqe = (struct nvme_completion *)((uintptr_t)page_to_virt(req->sgl->pg) + req->sgl->offset);

	if (unlikely(err)) {
		pr_err("QDMA recv error: %d\n", err);
		return err;
	}
	
	// pr_debug("Got QDMA recv: %u\n", bytes_done);
	// print_cqe(cqe);

	pr_debug("Cmpl q %u cmd %u\n", queue->qid, cqe->command_id);

	if (queue->qid == 0) {
		copy_page_to_req_buf(rsp->data, rsp->req);
	}

	/* sanity checking for received data length */
	// if (unlikely(bytes_done < cqe_len)) {
	// 	dev_err(queue->ctrl->ctrl.device,
	// 		"Unexpected nvme completion length(%d)\n", bytes_done);
	// 	nvme_qdma_error_recovery(queue->ctrl);
	// 	return -EINVAL;
	// }

	// ib_dma_sync_single_for_cpu(ibdev, qe->dma, len, DMA_FROM_DEVICE);
	/*
	 * AEN requests are special as they don't time out and can
	 * survive any kind of queue freeze and often don't respond to
	 * aborts.  We don't even bother to allocate a struct request
	 * for them but rather special case them here.
	 */
	if (unlikely(nvme_qdma_queue_idx(queue) == 0 &&
			cqe->command_id >= NVME_AQ_BLK_MQ_DEPTH))
		nvmq_complete_async_event(&queue->ctrl->ctrl, cqe->status,
				&cqe->result);
	else
		nvme_qdma_process_nvme_rsp(queue, cqe);
	// ib_dma_sync_single_for_device(ibdev, qe->dma, len, DMA_FROM_DEVICE);

	pr_debug("rsp process succeeded\n");

	// ret = nvme_qdma_post_rsp(queue, rsp);
	// if (ret) {
	// 	pr_err("Failed to post qdma recv: %d\n", ret);
	// }
	return 0;
}

static inline unsigned int qdma_map_page(struct pci_dev *pdev, void *buf, void *sg, bool qdma_format)
{
	struct page *pg = virt_to_page(buf);
	unsigned int offset = (uintptr_t)buf & (PAGE_SIZE - 1);
	sg = qdma_next_sg(sg, qdma_format);
	if (offset > 0) {
		qdma_sg_set_page(pdev, sg, pg, PAGE_SIZE - offset, offset, qdma_format, DMA_FROM_DEVICE);
		pg = virt_to_page((uintptr_t)buf + PAGE_SIZE);
		sg = qdma_next_sg(sg, qdma_format);
		qdma_sg_set_page(pdev, sg, pg, offset, 0, qdma_format, DMA_FROM_DEVICE);
		return 2;
	} else {
		// pr_debug("sg is %p\n", sg);
		qdma_sg_set_page(pdev, sg, pg, PAGE_SIZE, offset, qdma_format, DMA_FROM_DEVICE);
		return 1;
	}
}

static int nvme_qdma_post_rsp(struct nvme_qdma_queue *queue,
							   struct nvme_qdma_rsp *rsp, struct request_queue *q, struct request *rq)
{
	struct qdma_request *qdma_req = &rsp->qe.qdma_req;
	unsigned long dev_hndl = queue->device->xpdev->dev_hndl;
	// enum nvmq_map_option option = queue->qid ? QE : QE_DATA;
	int ret;

	rsp->req = blk_mq_rq_to_pdu(rq);

	if (queue->qid == 0) {
		nvme_qdma_qe_to_sg(&rsp->qe, qdma_req->sgl, false);
		qdma_req->sgcnt = 1 + qdma_map_page(NULL, rsp->data, qdma_req->sgl, true);
	} else {
		nvme_qdma_qe_to_sg(&rsp->qe, qdma_req->sgl, false);
		qdma_req->sgcnt = 1;
		qdma_req->sgl->len = sizeof(struct nvme_completion);
	}

	qdma_req_update_count(qdma_req);

	
	//print_qdma_req(qdma_req);

	pr_debug("submit: dev_hndl %lu, c2h qhndl %lu, sg cnt%d\n", dev_hndl, queue->qe_qp.c2h_qhndl,qdma_req->sgcnt);
	// print_cqe((uint8_t *)page_to_virt(qdma_req->sgl[0].pg) + qdma_req->sgl[0].offset);
	nvmq_diag_log_cmd("C2H_POST", queue, rq, nvmq_diag_req_cmd(rsp->req), 0);

	ret = qdma_request_submit(dev_hndl, queue->qe_qp.c2h_qhndl, qdma_req);
	if (unlikely(ret)) {
		dev_err(queue->ctrl->ctrl.device,
			     "qdma_request_submit failed with error code %d\n", ret);
		nvmq_diag_log_cmd("C2H_POST_FAIL", queue, rq, nvmq_diag_req_cmd(rsp->req), ret);
	}
	return ret;
}

static int nvme_qdma_post_next_rsp(struct nvme_qdma_queue *queue, struct request_queue *q, struct request *rq)
{
	uint32_t rsp_idx = queue->rsp_idx;
	queue->rsp_idx = (queue->rsp_idx + 1) % queue->queue_size;
	return nvme_qdma_post_rsp(queue, &queue->rsp_ring[rsp_idx], q, rq);
}

static void nvme_qdma_submit_async_event(struct nvmq_ctrl *arg)
{
	struct nvme_qdma_ctrl *ctrl = to_qdma_ctrl(arg);
	struct nvme_qdma_queue *queue = &ctrl->queues[0];
	struct nvme_qdma_sqe *sqe = &ctrl->async_event_sqe;
	struct qdma_request *qdma_req = &ctrl->async_event_qdma_req;
	struct nvme_command *cmd = sqe->qe_data;
	int ret;

	// pr_err("submit_async_event is not implemented!\n");

	memset(cmd, 0, sizeof(*cmd));
	cmd->common.opcode = nvme_admin_async_event;
	cmd->common.command_id = NVME_AQ_BLK_MQ_DEPTH;
	cmd->common.flags |= NVME_CMD_SGL_METABUF;
	nvme_qdma_set_sg_null(cmd);

	qdma_req->write = 1;
	qdma_req->dma_mapped = false;
	qdma_req->udd_len = 0;
	qdma_req->ep_addr = 0;
	qdma_req->no_memcpy = 1;
	qdma_req->timeout_ms = nvmq_io_timeout * 1000;
	qdma_req->sgl = kzalloc(sizeof(struct qdma_sw_sg), GFP_KERNEL);
	qdma_req->sgcnt = nvme_qdma_qe_to_sg(sqe, qdma_req->sgl, true);
	qdma_req_update_count(qdma_req);

	// print_qdma_req(qdma_req);

	ret = nvme_qdma_post_send(queue, qdma_req, true);
	WARN_ON_ONCE(ret);
}

// static int nvme_qdma_conn_established(struct nvme_qdma_queue *queue)
// {
// 	int ret, i;

// 	for (i = 0; i < queue->queue_size; i++) {
// 		ret = nvme_qdma_post_rsp(queue, &queue->rsp_ring[i]);
// 		if (ret)
// 			goto out_destroy_queue_ib;
// 	}

// 	return 0;

// out_destroy_queue_ib:
// 	nvme_qdma_destroy_queue_ib(queue);
// 	return ret;
// }

// static int nvme_qdma_conn_rejected(struct nvme_qdma_queue *queue,
// 		struct rdma_cm_event *ev)
// {
// 	struct rdma_cm_id *cm_id = queue->cm_id;
// 	int status = ev->status;
// 	const char *rej_msg;
// 	const struct nvme_qdma_cm_rej *rej_data;
// 	u8 rej_data_len;

// 	rej_msg = rdma_reject_msg(cm_id, status);
// 	rej_data = rdma_consumer_reject_data(cm_id, ev, &rej_data_len);

// 	if (rej_data && rej_data_len >= sizeof(u16)) {
// 		u16 sts = le16_to_cpu(rej_data->sts);

// 		dev_err(queue->ctrl->ctrl.device,
// 		      "Connect rejected: status %d (%s) nvme status %d (%s).\n",
// 		      status, rej_msg, sts, nvme_qdma_cm_msg(sts));
// 	} else {
// 		dev_err(queue->ctrl->ctrl.device,
// 			"Connect rejected: status %d (%s).\n", status, rej_msg);
// 	}

// 	return -ECONNRESET;
// }

// static int nvme_qdma_addr_resolved(struct nvme_qdma_queue *queue)
// {
// 	struct nvmq_ctrl *ctrl = &queue->ctrl->ctrl;
// 	int ret;

// 	ret = nvme_qdma_create_queue_ib(queue);
// 	if (ret)
// 		return ret;

// 	if (ctrl->opts->tos >= 0)
// 		rdma_set_service_type(queue->cm_id, ctrl->opts->tos);
// 	ret = rdma_resolve_route(queue->cm_id, NVME_QDMA_CONNECT_TIMEOUT_MS);
// 	if (ret) {
// 		dev_err(ctrl->device, "rdma_resolve_route failed (%d).\n",
// 			queue->cm_error);
// 		goto out_destroy_queue;
// 	}

// 	return 0;

// out_destroy_queue:
// 	nvme_qdma_destroy_queue_ib(queue);
// 	return ret;
// }

// static int nvme_qdma_route_resolved(struct nvme_qdma_queue *queue)
// {
// 	struct nvme_qdma_ctrl *ctrl = queue->ctrl;
// 	struct rdma_conn_param param = { };
// 	struct nvme_qdma_cm_req priv = { };
// 	int ret;

// 	param.qp_num = queue->qp->qp_num;
// 	param.flow_control = 1;

// 	param.responder_resources = queue->device->dev->attrs.max_qp_rd_atom;
// 	/* maximum retry count */
// 	param.retry_count = 7;
// 	param.rnr_retry_count = 7;
// 	param.private_data = &priv;
// 	param.private_data_len = sizeof(priv);

// 	priv.recfmt = cpu_to_le16(nvme_qdma_CM_FMT_1_0);
// 	priv.qid = cpu_to_le16(nvme_qdma_queue_idx(queue));
// 	/*
// 	 * set the admin queue depth to the minimum size
// 	 * specified by the Fabrics standard.
// 	 */
// 	if (priv.qid == 0) {
// 		priv.hrqsize = cpu_to_le16(NVME_AQ_DEPTH);
// 		priv.hsqsize = cpu_to_le16(NVME_AQ_DEPTH - 1);
// 	} else {
// 		/*
// 		 * current interpretation of the fabrics spec
// 		 * is at minimum you make hrqsize sqsize+1, or a
// 		 * 1's based representation of sqsize.
// 		 */
// 		priv.hrqsize = cpu_to_le16(queue->queue_size);
// 		priv.hsqsize = cpu_to_le16(queue->ctrl->ctrl.sqsize);
// 	}

// 	ret = rdma_connect(queue->cm_id, &param);
// 	if (ret) {
// 		dev_err(ctrl->ctrl.device,
// 			"rdma_connect failed (%d).\n", ret);
// 		goto out_destroy_queue_ib;
// 	}

// 	return 0;

// out_destroy_queue_ib:
// 	nvme_qdma_destroy_queue_ib(queue);
// 	return ret;
// }

// static int nvme_qdma_cm_handler(struct rdma_cm_id *cm_id,
// 		struct rdma_cm_event *ev)
// {
// 	struct nvme_qdma_queue *queue = cm_id->context;
// 	int cm_error = 0;

// 	dev_dbg(queue->ctrl->ctrl.device, "%s (%d): status %d id %p\n",
// 		rdma_event_msg(ev->event), ev->event,
// 		ev->status, cm_id);

// 	switch (ev->event) {
// 	case RDMA_CM_EVENT_ADDR_RESOLVED:
// 		cm_error = nvme_qdma_addr_resolved(queue);
// 		break;
// 	case RDMA_CM_EVENT_ROUTE_RESOLVED:
// 		cm_error = nvme_qdma_route_resolved(queue);
// 		break;
// 	case RDMA_CM_EVENT_ESTABLISHED:
// 		queue->cm_error = nvme_qdma_conn_established(queue);
// 		/* complete cm_done regardless of success/failure */
// 		complete(&queue->cm_done);
// 		return 0;
// 	case RDMA_CM_EVENT_REJECTED:
// 		cm_error = nvme_qdma_conn_rejected(queue, ev);
// 		break;
// 	case RDMA_CM_EVENT_ROUTE_ERROR:
// 	case RDMA_CM_EVENT_CONNECT_ERROR:
// 	case RDMA_CM_EVENT_UNREACHABLE:
// 		nvme_qdma_destroy_queue_ib(queue);
// 		/* fall through */
// 	case RDMA_CM_EVENT_ADDR_ERROR:
// 		dev_dbg(queue->ctrl->ctrl.device,
// 			"CM error event %d\n", ev->event);
// 		cm_error = -ECONNRESET;
// 		break;
// 	case RDMA_CM_EVENT_DISCONNECTED:
// 	case RDMA_CM_EVENT_ADDR_CHANGE:
// 	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
// 		dev_dbg(queue->ctrl->ctrl.device,
// 			"disconnect received - connection closed\n");
// 		nvme_qdma_error_recovery(queue->ctrl);
// 		break;
// 	case RDMA_CM_EVENT_DEVICE_REMOVAL:
// 		/* device removal is handled via the ib_client API */
// 		break;
// 	default:
// 		dev_err(queue->ctrl->ctrl.device,
// 			"Unexpected RDMA CM event (%d)\n", ev->event);
// 		nvme_qdma_error_recovery(queue->ctrl);
// 		break;
// 	}

// 	if (cm_error) {
// 		queue->cm_error = cm_error;
// 		complete(&queue->cm_done);
// 	}

// 	return 0;
// }

static void nvme_qdma_complete_timed_out(struct request *rq)
{
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	struct nvme_qdma_queue *queue = req->queue;

	nvme_qdma_stop_queue(queue);
	if (blk_mq_request_started(rq) && !blk_mq_request_completed(rq)) {
		nvmq_req(rq)->status = NVME_SC_HOST_ABORTED_CMD;
		blk_mq_complete_request(rq);
	}
}

static enum blk_eh_timer_return
nvme_qdma_timeout(struct request *rq, bool reserved)
{
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	struct nvme_qdma_queue *queue = req->queue;
	struct nvme_qdma_ctrl *ctrl = queue->ctrl;

	dev_warn(ctrl->ctrl.device, "I/O %d QID %d timeout\n",
		 rq->tag, nvme_qdma_queue_idx(queue));
	nvmq_diag_log_cmd("TIMEOUT", queue, rq, nvmq_diag_req_cmd(req), 0);
		
	qdma_queue_dump(ctrl->device->xpdev->dev_hndl, queue->qe_qp.c2h_qhndl, g_msg_buf, 4096 * 5);
	// pr_debug("%s\n", g_msg_buf);

	if (ctrl->ctrl.state != NVME_CTRL_LIVE) {
		/*
		 * If we are resetting, connecting or deleting we should
		 * complete immediately because we may block controller
		 * teardown or setup sequence
		 * - ctrl disable/shutdown fabrics requests
		 * - connect requests
		 * - initialization admin requests
		 * - I/O requests that entered after unquiescing and
		 *   the controller stopped responding
		 *
		 * All other requests should be cancelled by the error
		 * recovery work, so it's fine that we fail it here.
		 */
		nvme_qdma_complete_timed_out(rq);
		return BLK_EH_DONE;
	}

	/*
	 * LIVE state should trigger the normal error recovery which will
	 * handle completing this request.
	 */
	nvme_qdma_error_recovery(ctrl);
	return BLK_EH_RESET_TIMER;
}

static inline struct scatterlist *nvme_qdma_blk_next_sg(struct scatterlist **sg,
		struct scatterlist *sglist)
{
	if (!*sg)
		return sglist;

	/*
	 * If the driver previously mapped a shorter list, we could see a
	 * termination bit prematurely unless it fully inits the sg table
	 * on each mapping. We KNOW that there must be more entries here
	 * or the driver would be buggy, so force clear the termination bit
	 * to avoid doing a full sg_init_table() in drivers for each command.
	 */
	sg_unmark_end(*sg);
	return sg_next(*sg);
}

static unsigned nvme_qdma_blk_bvec_map_sg(struct request_queue *q,
		struct bio_vec *bvec, struct scatterlist *sglist,
		struct scatterlist **sg)
{
	unsigned nbytes = bvec->bv_len;
	unsigned nsegs = 0, total = 0;

	while (nbytes > 0) {
		unsigned offset = bvec->bv_offset + total;
		unsigned len = min(get_max_segment_size(q, bvec->bv_page,
					offset), nbytes);
		struct page *page = bvec->bv_page;

		/*
		 * Unfortunately a fair number of drivers barf on scatterlists
		 * that have an offset larger than PAGE_SIZE, despite other
		 * subsystems dealing with that invariant just fine.  For now
		 * stick to the legacy format where we never present those from
		 * the block layer, but the code below should be removed once
		 * these offenders (mostly MMC/SD drivers) are fixed.
		 */
		page += (offset >> PAGE_SHIFT);
		offset &= ~PAGE_MASK;

		*sg = nvme_qdma_blk_next_sg(sg, sglist);
		sg_set_page(*sg, page, len, offset);

		total += len;
		nbytes -= len;
		nsegs++;
	}

	return nsegs;
}

static inline int __nvme_qdma_blk_bvec_map_sg(struct bio_vec bv,
		struct scatterlist *sglist, struct scatterlist **sg)
{
	*sg = nvme_qdma_blk_next_sg(sg, sglist);
	sg_set_page(*sg, bv.bv_page, bv.bv_len, bv.bv_offset);
	return 1;
}

static int nvme_qdma_blk_bios_map_sg(struct request_queue *q, struct bio *bio,
			     struct scatterlist *sglist,
			     struct scatterlist **sg)
{
	struct bio_vec uninitialized_var(bvec), bvprv = { NULL };
	struct bvec_iter iter;
	int nsegs = 0;
	bool new_bio = false;

	for_each_bio(bio) {
		bio_for_each_bvec(bvec, bio, iter) {
			/*
			 * Only try to merge bvecs from two bios given we
			 * have done bio internal merge when adding pages
			 * to bio
			 */
			// if (new_bio &&
			//     __blk_segment_map_sg_merge(q, &bvec, &bvprv, sg))
			// 	goto next_bvec;

			if (bvec.bv_offset + bvec.bv_len <= PAGE_SIZE)
				nsegs += __nvme_qdma_blk_bvec_map_sg(bvec, sglist, sg);
			else
				nsegs += nvme_qdma_blk_bvec_map_sg(q, &bvec, sglist, sg);

			new_bio = false;
		}
		if (likely(bio->bi_iter.bi_size)) {
			bvprv = bvec;
			new_bio = true;
		}
	}

	return nsegs;
}

static int nvme_qdma_rq_map_sg(struct request_queue *q, struct bio *bio,
		  struct scatterlist *sglist)
{
	struct scatterlist *sg = NULL;
	int nsegs = 0;

	nsegs = nvme_qdma_blk_bios_map_sg(q, bio, sglist, &sg);

	if (sg)
		sg_mark_end(sg);

	return nsegs;
}

static int __blk_rq_map_user_iov(struct request_queue *q, struct nvme_qdma_sqe *sqe,
		struct rq_map_data *map_data, struct iov_iter *iter,
		gfp_t gfp_mask, bool copy)
{
	// struct bio *bio, *orig_bio;
	struct bio *bio;

	// if (copy)
	// 	bio = bio_copy_user_iov(q, map_data, iter, gfp_mask);
	// else
	bio = bio_map_user_iov(q, iter, gfp_mask);

	if (IS_ERR(bio))
		return PTR_ERR(bio);

	bio->bi_opf &= ~REQ_OP_MASK;
	// bio->bi_opf |= req_op(rq);

	// orig_bio = bio;

	/*
	 * We link the bounce buffer in and could have to traverse it
	 * later so we have to get a ref to prevent it from being freed
	 */
	// ret = blk_rq_append_bio(rq, &bio);
	// if (ret) {
	// 	__blk_rq_unmap_user(orig_bio);
	// 	return ret;
	// }

	sqe->bio = bio;

	bio_get(bio);

	return 0;
}

static int nvme_qdma_rq_map_user_iov(struct request_queue *q,
			struct rq_map_data *map_data, struct nvme_qdma_sqe *sqe,
			const struct iov_iter *iter, gfp_t gfp_mask)
{
	bool copy = false;
	unsigned long align = q->dma_pad_mask | queue_dma_alignment(q);
	struct bio *bio = NULL;
	struct iov_iter i;
	int ret = -EINVAL;

	if (!iter_is_iovec(iter))
		goto fail;

	if (map_data)
		copy = true;
	else if (iov_iter_alignment(iter) & align)
		copy = true;
	else if (queue_virt_boundary(q))
		copy = queue_virt_boundary(q) & iov_iter_gap_alignment(iter);
	
	BUG_ON(copy); // We currently do not support copy iovs

	i = *iter;

	ret =__blk_rq_map_user_iov(q, sqe, map_data, &i, gfp_mask, copy);
	if (ret)
		goto unmap_rq;
	
	bio = sqe->bio;

	BUG_ON(iov_iter_count(&i));

	// if (!bio_flagged(bio, BIO_USER_MAPPED))
	// 	rq->rq_flags |= RQF_COPY_USER;
	return 0;

unmap_rq:
	blk_rq_unmap_user(bio);
fail:
	sqe->bio = NULL;
	return ret;
}

static inline int sqe_data_dir(struct nvme_qdma_sqe *sqe)
{
	struct nvme_command *cmd = sqe->qe_data;

	return (cmd->common.opcode & 0x1) ? WRITE : READ;
}

static inline int sqe_dma_dir(struct nvme_qdma_sqe *sqe)
{
	struct nvme_command *cmd = sqe->qe_data;

	return (cmd->common.opcode & 0x1) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
}

static int nvme_qdma_rq_map_user(struct request_queue *q,
		    struct rq_map_data *map_data, struct nvme_qdma_sqe *sqe,
			void __user *ubuf,
		    unsigned long len, gfp_t gfp_mask)
{
	struct iovec iov;
	struct iov_iter i;
	int ret = import_single_range(sqe_data_dir(sqe), ubuf, len, &iov, &i);

	if (unlikely(ret < 0))
		return ret;

	return nvme_qdma_rq_map_user_iov(q, map_data, sqe, &i, gfp_mask);
}

static int nvme_qdma_sqe_map_data(struct nvme_qdma_queue *queue, struct nvme_qdma_request *req,
		struct request *rq, struct nvme_qdma_sqe *sqe, struct nvme_command *c)
{
	int nr_mapped, ret;
	unsigned short nr_phys_segments = 0;
	unsigned int data_len;

	bool is_compute_cmd = nvme_opcode_is_compute_cmd(c->common.opcode);
	uint64_t lba = le64_to_cpu(*((uint64_t *)&c->common.cdw10));
	bool memcpy_lba_from_host = is_compute_cmd && (lba >= 512);

	struct bvec_iter iter;
	struct bio_vec bv;

	u64 ubuf = le64_to_cpu(memcpy_lba_from_host ? lba : c->rw.dptr.prp1);
	u32 ulen = (le16_to_cpu(c->rw.length) + 1) * PAGE_SIZE;

	pr_debug("opcode is 0x%x ubuf is %llX ulen is %u\n", c->common.opcode, ubuf, ulen);

	ret = nvme_qdma_rq_map_user(rq->q, NULL, sqe, (void *)ubuf, ulen, GFP_KERNEL);

	data_len = sqe->bio->bi_iter.bi_size;

	bio_for_each_bvec(bv, sqe->bio, iter)
		nr_phys_segments++;

	pr_debug("blk_rq_nr_phys_segments is %u\n", nr_phys_segments);

	if (!nr_phys_segments) {
		sqe->sgl_buf = NULL;
		sqe->sg = NULL;
		return nvme_qdma_set_sg_null(c);
	}

	sqe->sg = mempool_alloc(queue->ctrl->iod_mempool, GFP_ATOMIC);
	if (!sqe->sg)
		goto out_free_sg;
	sg_init_table(sqe->sg, nr_phys_segments);
	sqe->nents = nvme_qdma_rq_map_sg(rq->q, sqe->bio, sqe->sg);
	if (!sqe->nents)
		goto out_free_sg;
	nr_mapped = dma_map_sg_attrs(&queue->device->xpdev->pdev->dev, sqe->sg, sqe->nents,
									sqe_dma_dir(sqe), DMA_ATTR_NO_WARN);
	if (!nr_mapped)
		goto out_free_sg;
	nvmq_print_sgl(sqe->sg, sqe->nents);
	nvme_qdma_setup_prps(queue->ctrl, req, sqe, &c->rw, data_len, sqe_dma_dir(sqe), memcpy_lba_from_host);
	// ret = nvme_qdma_map_sgls(queue, req, c, req->nents);
	// if (ret) {
	// 	pr_err("map data to sgl failed: %d\n", ret);
	// }
	return 0;
out_free_sg:
	mempool_free(sqe->sg, queue->ctrl->iod_mempool);
	return BLK_STS_RESOURCE;
}

static inline bool nvme_qdma_command_use_device_mem(struct nvme_command *cmd)
{
	u64 ubuf = le64_to_cpu(cmd->rw.dptr.prp1);
	if (unlikely(nvme_opcode_is_compute_cmd(cmd->common.opcode))) {
		u64 lba = le64_to_cpu(*((uint64_t *)&cmd->common.cdw10));
		return lba < 512 && ubuf < 512;
	} else {
		return ubuf < 512;
	}
}

static blk_status_t nvme_qdma_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct nvmq_ns *ns = hctx->queue->queuedata;
	struct nvme_qdma_queue *queue = hctx->driver_data;
	struct request *rq = bd->rq;
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	struct nvme_qdma_sqe *sqe;
	struct nvme_command *c;
	struct nvme_qdma_ctrl *ctrl = queue->ctrl;
	struct nvme_command *cmds;
	u8 num_cmds;
	struct qdma_request *qdma_req = &req->qdma_req;
	unsigned long dev_hndl;
	bool queue_ready = test_bit(nvme_qdma_Q_LIVE, &queue->flags);
	bool is_kernel = req->u.req.flags & NVME_REQ_USERKERNEL;
	bool is_raw_cmd = req->u.req.flags & NVME_REQ_USERCMD;
	blk_status_t ret;
	enum nvmq_map_option option;
	int err, i;
	bool special_command_sel = false;

	WARN_ON_ONCE(rq->tag < 0);
    pr_debug("queue rq");
	if (!nvmqf_check_ready(&queue->ctrl->ctrl, rq, queue_ready))
		return nvmqf_fail_nonready_command(&queue->ctrl->ctrl, rq);

	dev_hndl = queue->device->xpdev->dev_hndl;

	if (is_kernel) {
		pr_debug("is kernel\n");
		struct nvmq_kernel *knl = &req->u.knl;
		cmds = knl->cmds;
		num_cmds = knl->num_cmds;

		if (num_cmds > NVME_QDMA_KERNEL_MAX_COMMANDS) {
			pr_err("Kernel too many commands: %u\n", num_cmds);
			return BLK_STS_IOERR;
		}
	} else {
		cmds = req->u.req.cmd;
		num_cmds = 1;
	}

	INIT_LIST_HEAD(&req->sqe_list);

	for (i = 0; i < num_cmds; i++) {
		// Allocate a nvme_qdma_sqe from sqe_mempool
		struct nvme_qdma_sqe *sqe;
		
		if (unlikely(queue->qid == 0 || ctrl->sqe_mempool == NULL)) {
			// Manually allocate SQE since mempool is not created at this time
			sqe = kzalloc(sizeof(struct nvme_qdma_sqe), GFP_ATOMIC);
			if (!sqe) {
				pr_err("Failed to allocate sqe\n");
				goto err;
			}
			if (sqe_member_init(sqe, GFP_ATOMIC)) {
				pr_err("Failed to allocate sqe members\n");
				goto err;
			}
		} else {
			sqe = mempool_alloc(ctrl->sqe_mempool, GFP_ATOMIC);
			if (!sqe) {
				pr_err("Failed to allocate sqe\n");
				goto err;
			}
		}

		c = sqe->qe_data;

		if (is_kernel || is_raw_cmd) {
			sqe->sg = NULL;
			ret = nvmq_setup_raw_cmd(ns, rq, &cmds[i], c, i == 0);
		} else {
			ret = nvmq_setup_cmd(ns, rq, c);
		}

		if (ret)
			goto err;

		nvmq_diag_log_cmd("QUEUE_RQ", queue, rq, c, 0);
		list_add_tail(&sqe->entry, &req->sqe_list);
	}

	blk_mq_start_request(rq);

	list_for_each_entry(sqe, &req->sqe_list, entry) {
		c = sqe->qe_data;
		if (!is_kernel) {
			err = nvme_qdma_map_data(queue, rq, c);
		} else if (!nvme_qdma_command_use_device_mem(c)) {
			pr_debug("SQE %u uses local mem\n", c->common.opcode);
			err = nvme_qdma_sqe_map_data(queue, req, rq, sqe, c);
		} else {
			pr_debug("SQE %u uses device mem\n", c->common.opcode);
		}
		if (unlikely(err < 0)) {
			dev_err(queue->ctrl->ctrl.device,
					"Failed to map data (%d)\n", err);
			nvmq_cleanup_cmd(rq);
			goto err;
		}
	}

	// sqe->cqe.done = nvme_qdma_send_done;

	// ib_dma_sync_single_for_device(dev, sqe->dma,
	// 		sizeof(struct nvme_command), DMA_TO_DEVICE);

	ret = nvme_qdma_post_next_rsp(queue, rq->q, rq);
	if (ret) {
		pr_err("failed to post qdma recv: %d\n", ret);
		return ret;
	}


	// copy_from_user_page()
	/*
	use for computational storage device
	TODO Optimize it in the future!!
	*/
	special_command_sel = false;
	switch(c->common.opcode){
		case 0x85: //load_program
			special_command_sel = (((c->common.cdw10>>24)&0x1) == 0);
			special_command_sel = special_command_sel&&queue->qid == 0;
			break;
		case 0x88: //prog_mgmt
			special_command_sel = (((c->common.cdw10>>16)&0x1) == 1);
			special_command_sel = special_command_sel&&queue->qid == 0;
			break;
		case 0x89: //mmrange_set_mgmt
			special_command_sel = (((c->common.cdw10)&0x1) == 0);
			special_command_sel = special_command_sel&&queue->qid == 0;
			break;
		case 0x0d: //namespace mgmt
			special_command_sel = (((c->common.cdw10)&0x1) == 0)&&((c->common.cdw11>>24) == 0x3);
			special_command_sel = special_command_sel&&queue->qid == 0;
			break;
		default:
			break;
	}


	option = (nvme_is_fabrics(c)||special_command_sel)? QE_DATA : QE;

	//option = (nvme_is_fabrics(c))? QE_DATA : QE;
	if((blk_rq_nr_phys_segments(rq) > 0))
		pr_debug("map special command sgl1 size%d bool%d\n",qdma_req->sgl[1].len,special_command_sel);
	qdma_req->sgcnt = map_nvmq_req_to_sg(rq->q, rq, qdma_req->sgl, option, true);
	if((blk_rq_nr_phys_segments(rq) > 0))
		pr_debug("map special command sgl1 sizeAAA%d\n",qdma_req->sgl[1].len);
	pr_debug("map %d %X sgcnt %u segments%d\n", option, c->common.opcode, qdma_req->sgcnt,blk_rq_nr_phys_segments(rq));

	if (queue->qid == 0 || nvme_is_fabrics(c)) {
		// Fill the empty sgl
		if (blk_rq_nr_phys_segments(rq) > 0) {
			ret = nvme_qdma_map_sg_inline(queue, req, c);
			if (unlikely(ret)) {
				pr_err("set inline sgl failed: %d\n", ret);
			}
		}
	} else {
		// Add sgl_buf to qdma_req->sgl
		// qdma_req->sgcnt += qdma_map_page(req->sgl_buf, qdma_req->sgl);
	}

	pr_debug("Submit q %u cmd %u op %u slba %llu len %u flags 0x%X\n", queue->qid, c->common.command_id, c->common.opcode, c->rw.slba, c->rw.length, c->common.flags);
        

	qdma_req_update_count(qdma_req);
	//if(special_command_sel)
	//print_qdma_req(qdma_req);
	refcount_set(&req->ref, 2); /* send and recv completions */
	err = nvme_qdma_post_send(queue, qdma_req, false);
	if (unlikely(err)) {
		nvme_qdma_unmap_data(queue, rq);
		goto err;
	}

	return BLK_STS_OK;

err:
	if (err == -ENOMEM || err == -EAGAIN)
		ret = BLK_STS_RESOURCE;
	else
		ret = BLK_STS_IOERR;
// unmap_qe:
// 	ib_dma_unmap_single(dev, req->sqe.dma, sizeof(struct nvme_command),
// 			    DMA_TO_DEVICE);
	return ret;
}

static int nvme_qdma_poll(struct blk_mq_hw_ctx *hctx)
{
	// struct nvme_qdma_queue *queue = hctx->driver_data;

	// return ib_process_cq_direct(queue->ib_cq, -1);
	return 0;
}

static void nvme_qdma_complete_rq(struct request *rq)
{
	struct nvme_qdma_request *req = blk_mq_rq_to_pdu(rq);
	struct nvme_qdma_queue *queue = req->queue;
	// unsigned long dev_hndl = queue->device->dev;

	nvme_qdma_unmap_data(queue, rq);
	// ib_dma_unmap_single(ibdev, req->sqe.dma, sizeof(struct nvme_command),
	// 		    DMA_TO_DEVICE);
	nvmq_complete_rq(rq);
}

static int blk_mq_qdma_map_queues(struct blk_mq_queue_map *qmap, struct nvme_qdma_ctrl *ctrl)
{
	const struct cpumask *mask;
	unsigned int queue, cpu, qid;
	unsigned long devhndl = ctrl->device->xpdev->dev_hndl;

	for (queue = 0; queue < qmap->nr_queues; queue++) {
		qid = queue + qmap->queue_offset + 1;
		mask = qdma_queue_get_irq_affinity(devhndl, ctrl->queues[qid].qe_qp.h2c_qhndl);
		if (!mask)
			goto fallback;

		for_each_cpu(cpu, mask) {
			qmap->mq_map[cpu] = qmap->queue_offset + queue;
			pr_debug("Map cpu %u to queue %u, qid %u\n", cpu, queue, qid);
		}
	}

	return 0;

fallback:
	WARN_ON_ONCE(qmap->nr_queues > 1);
	blk_mq_map_queues(qmap);
	
	return 0;
}

static int nvme_qdma_map_queues(struct blk_mq_tag_set *set)
{
	struct nvme_qdma_ctrl *ctrl = set->driver_data;
	struct nvmqf_ctrl_options *opts = ctrl->ctrl.opts;

	if (opts->nr_write_queues && ctrl->io_queues[HCTX_TYPE_READ]) {
		/* separate read/write queues */
		set->map[HCTX_TYPE_DEFAULT].nr_queues =
			ctrl->io_queues[HCTX_TYPE_DEFAULT];
		set->map[HCTX_TYPE_DEFAULT].queue_offset = 0;
		set->map[HCTX_TYPE_READ].nr_queues =
			ctrl->io_queues[HCTX_TYPE_READ];
		set->map[HCTX_TYPE_READ].queue_offset =
			ctrl->io_queues[HCTX_TYPE_DEFAULT];
	} else {
		/* shared read/write queues */
		set->map[HCTX_TYPE_DEFAULT].nr_queues =
			ctrl->io_queues[HCTX_TYPE_DEFAULT];
		set->map[HCTX_TYPE_DEFAULT].queue_offset = 0;
		set->map[HCTX_TYPE_READ].nr_queues =
			ctrl->io_queues[HCTX_TYPE_DEFAULT];
		set->map[HCTX_TYPE_READ].queue_offset = 0;
	}

	blk_mq_qdma_map_queues(&set->map[HCTX_TYPE_DEFAULT], ctrl);
	blk_mq_qdma_map_queues(&set->map[HCTX_TYPE_READ], ctrl);

	if (opts->nr_poll_queues && ctrl->io_queues[HCTX_TYPE_POLL]) {
		/* map dedicated poll queues only if we have queues left */
		set->map[HCTX_TYPE_POLL].nr_queues =
				ctrl->io_queues[HCTX_TYPE_POLL];
		set->map[HCTX_TYPE_POLL].queue_offset =
			ctrl->io_queues[HCTX_TYPE_DEFAULT] +
			ctrl->io_queues[HCTX_TYPE_READ];
		blk_mq_map_queues(&set->map[HCTX_TYPE_POLL]);
	}

	dev_info(ctrl->ctrl.device,
		"mapped %d/%d/%d default/read/poll queues.\n",
		ctrl->io_queues[HCTX_TYPE_DEFAULT],
		ctrl->io_queues[HCTX_TYPE_READ],
		ctrl->io_queues[HCTX_TYPE_POLL]);

	return 0;
}

static const struct blk_mq_ops nvme_qdma_mq_ops = {
	.queue_rq	= nvme_qdma_queue_rq,
	.complete	= nvme_qdma_complete_rq,
	.init_request	= nvme_qdma_init_request,
	.exit_request	= nvme_qdma_exit_request,
	.init_hctx	= nvme_qdma_init_hctx,
	.timeout	= nvme_qdma_timeout,
	.map_queues	= nvme_qdma_map_queues,
	.poll		= nvme_qdma_poll,
};

static const struct blk_mq_ops nvme_qdma_admin_mq_ops = {
	.queue_rq	= nvme_qdma_queue_rq,
	.complete	= nvme_qdma_complete_rq,
	.init_request	= nvme_qdma_init_request,
	.exit_request	= nvme_qdma_exit_request,
	.init_hctx	= nvme_qdma_init_admin_hctx,
	.timeout	= nvme_qdma_timeout,
};

static void nvme_qdma_shutdown_ctrl(struct nvme_qdma_ctrl *ctrl, bool shutdown)
{
	nvme_qdma_teardown_io_queues(ctrl, shutdown);
	blk_mq_quiesce_queue(ctrl->ctrl.admin_q);
	if (shutdown)
		nvmq_shutdown_ctrl(&ctrl->ctrl);
	else
		nvmq_disable_ctrl(&ctrl->ctrl);
	nvme_qdma_teardown_admin_queue(ctrl, shutdown);
}

static void nvme_qdma_delete_ctrl(struct nvmq_ctrl *ctrl)
{
	nvme_qdma_shutdown_ctrl(to_qdma_ctrl(ctrl), true);
}

static void nvme_qdma_reset_ctrl_work(struct work_struct *work)
{
	struct nvme_qdma_ctrl *ctrl =
		container_of(work, struct nvme_qdma_ctrl, ctrl.reset_work);

	nvmq_stop_ctrl(&ctrl->ctrl);
	nvme_qdma_shutdown_ctrl(ctrl, false);

	if (!nvmq_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING)) {
		/* state change failure should never happen */
		WARN_ON_ONCE(1);
		return;
	}

	if (nvme_qdma_setup_ctrl(ctrl, false))
		goto out_fail;

	return;

out_fail:
	++ctrl->ctrl.nr_reconnects;
	nvme_qdma_reconnect_or_remove(ctrl);
}

static const struct nvmq_ctrl_ops nvme_qdma_ctrl_ops = {
	.name			= "qdma",
	.module			= THIS_MODULE,
	.flags			= NVME_F_FABRICS,
	.reg_read32		= nvmqf_reg_read32,
	.reg_read64		= nvmqf_reg_read64,
	.reg_write32		= nvmqf_reg_write32,
	.free_ctrl		= nvme_qdma_free_ctrl,
	.submit_async_event	= nvme_qdma_submit_async_event,
	.delete_ctrl		= nvme_qdma_delete_ctrl,
	.get_address		= nvmqf_get_address,
	.stop_ctrl		= nvme_qdma_stop_ctrl,
};

/*
 * Fails a connection request if it matches an existing controller
 * (association) with the same tuple:
 * <Host NQN, Host ID, local address, remote address, remote port, SUBSYS NQN>
 *
 * if local address is not specified in the request, it will match an
 * existing controller with all the other parameters the same and no
 * local port address specified as well.
 *
 * The ports don't need to be compared as they are intrinsically
 * already matched by the port pointers supplied.
 */
static bool
nvme_qdma_existing_controller(struct nvmqf_ctrl_options *opts)
{
	struct nvme_qdma_ctrl *ctrl;
	bool found = false;

	mutex_lock(&nvme_qdma_ctrl_mutex);
	list_for_each_entry(ctrl, &nvme_qdma_ctrl_list, list) {
		found = nvmqf_ip_options_match(&ctrl->ctrl, opts);
		if (found)
			break;
	}
	mutex_unlock(&nvme_qdma_ctrl_mutex);

	return found;
}

static struct xlnx_pci_dev *qdma_find_xpdev(const char *name)
{
	// Example name: qdma3c000
	int len = strlen(name);
	int pos = 4, i, rv;
	uint32_t v;
	// char *p;
	char msg_buf[32];
	struct xlnx_pci_dev *ret;

	/* qdma<N>*/
	if (len > 9) {
		pr_debug("interface name %s too long, expect qdma<N>.\n", name);
		return NULL;
	}
	if (strncmp(name, "qdma", 4)) {
		pr_debug("bad interface name %s, expect qdma<N>.\n", name);
		return NULL;
	}
	// if (name[4] == 'v' && name[5] == 'f') {
	// 	xcmd->vf = 1;
	// 	pos = 6;
	// } else {
	// 	xcmd->vf = 0;
	// 	pos = 4;
	// }
	for (i = pos; i < len; i++) {
		if (!isxdigit(name[i])) {
			pr_debug("%s unexpected <qdmaN>, %d.\n", name, i);
			return NULL;
		}
	}

	rv = kstrtouint(name + pos, 16, &v);
	if (rv) {
		pr_debug("bad parameter \"%s\", integer expected", name + pos);
		return NULL;
	}

	ret = xpdev_find_by_idx(v, msg_buf, 32);
	if (!ret) {
		pr_err("Failed to find qdma device %s: %s\n", name, msg_buf);
		return NULL;
	}

	return ret;
}

static struct nvme_qdma_device *nvme_qdma_get_dev(const char *name)
{
	struct xlnx_pci_dev *xpdev = qdma_find_xpdev(name);
	struct nvme_qdma_device *ndev;

	if (!xpdev)
		return NULL;

	mutex_lock(&device_list_mutex);
	list_for_each_entry(ndev, &device_list, entry) {
		if (ndev->xpdev == xpdev &&
		    nvme_qdma_dev_get(ndev))
			return ndev;
	}

	ndev = kzalloc(sizeof(*ndev), GFP_KERNEL);
	if (!ndev)
		return NULL;

	ndev->xpdev = xpdev;
	kref_init(&ndev->ref);

	mutex_init(&ndev->pfch_reg_mutex);

	// ndev->num_inline_segments = min(NVME_QDMA_MAX_INLINE_SEGMENTS,
	// 				ndev->dev->attrs.max_send_sge - 1);
	list_add(&ndev->entry, &device_list);

	mutex_unlock(&device_list_mutex);

	return ndev;
}

/*
 * Will slightly overestimate the number of pages needed.  This is OK
 * as it only leads to a small amount of wasted memory for the lifetime of
 * the I/O.
 */
static int nvmq_npages(unsigned size, struct nvme_qdma_ctrl *ctrl)
{
	unsigned nprps = DIV_ROUND_UP(size + ctrl->ctrl.page_size,
				      ctrl->ctrl.page_size);
	return DIV_ROUND_UP(8 * nprps, PAGE_SIZE - 8);
}

static unsigned int nvme_qdma_iod_alloc_size(struct nvme_qdma_ctrl *ctrl,
		unsigned int size, unsigned int nseg)
{
	size_t alloc_size;

	alloc_size = sizeof(__le64 *) * nvmq_npages(size, ctrl);

	return alloc_size + sizeof(struct scatterlist) * nseg;
}

static struct nvmq_ctrl *nvme_qdma_create_ctrl(struct device *dev,
		struct nvmqf_ctrl_options *opts)
{
	struct nvme_qdma_ctrl *ctrl;
	int ret;
	int node;
	bool changed;
	size_t alloc_size;
	size_t sqe_alloc_size = sizeof(struct nvme_qdma_sqe);

	node = dev_to_node(dev);
	if (node == NUMA_NO_NODE)
		set_dev_node(dev, first_memory_node);

	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return ERR_PTR(-ENOMEM);
	ctrl->ctrl.opts = opts;
	INIT_LIST_HEAD(&ctrl->list);

	// if (!(opts->mask & NVMQF_OPT_TRSVCID)) {
	// 	opts->trsvcid =
	// 		kstrdup(__stringify(nvme_qdma_IP_PORT), GFP_KERNEL);
	// 	if (!opts->trsvcid) {
	// 		ret = -ENOMEM;
	// 		goto out_free_ctrl;
	// 	}
	// 	opts->mask |= NVMQF_OPT_TRSVCID;
	// }

	// ret = inet_pton_with_scope(&init_net, AF_UNSPEC,
	// 		opts->traddr, opts->trsvcid, &ctrl->addr);
	// if (ret) {
	// 	pr_err("malformed address passed: %s:%s\n",
	// 		opts->traddr, opts->trsvcid);
	// 	goto out_free_ctrl;
	// }

	pr_debug("opts->mask is %X\n", opts->mask);

	if (opts->mask & NVMQF_OPT_TRADDR) {
		// ret = inet_pton_with_scope(&init_net, AF_UNSPEC,
		// 	opts->host_traddr, NULL, &ctrl->src_addr);
		ctrl->device = nvme_qdma_get_dev(opts->traddr);
		if (!ctrl->device) {
			pr_err("malformed src address passed: %s\n",
			       opts->host_traddr);
			goto out_free_ctrl;
		}
	}

	pr_debug("ctrl->device is %p\n", ctrl->device);

	if (!opts->duplicate_connect && nvme_qdma_existing_controller(opts)) {
		ret = -EALREADY;
		goto out_free_ctrl;
	}

	INIT_DELAYED_WORK(&ctrl->reconnect_work,
			nvme_qdma_reconnect_ctrl_work);
	INIT_WORK(&ctrl->err_work, nvme_qdma_error_recovery_work);
	INIT_WORK(&ctrl->ctrl.reset_work, nvme_qdma_reset_ctrl_work);

	ctrl->ctrl.queue_count = opts->nr_io_queues + opts->nr_write_queues +
				opts->nr_poll_queues + 1;
	ctrl->ctrl.sqsize = opts->queue_size - 1;
	ctrl->ctrl.kato = opts->kato;

	ret = -ENOMEM;
	ctrl->queues = kcalloc(ctrl->ctrl.queue_count, sizeof(*ctrl->queues),
				GFP_KERNEL);
	if (!ctrl->queues)
		goto out_free_ctrl;

	ret = nvmq_init_ctrl(&ctrl->ctrl, dev, &nvme_qdma_ctrl_ops,
				0 /* no quirks, we're perfect! */);
	if (ret)
		goto out_kfree_queues;

	changed = nvmq_change_ctrl_state(&ctrl->ctrl, NVME_CTRL_CONNECTING);
	WARN_ON_ONCE(!changed);

	ret = nvme_qdma_setup_ctrl(ctrl, true);
	if (ret)
		goto out_uninit_ctrl;

	/*
	 * Double check that our mempool alloc size will cover the biggest
	 * command we support.
	 */
	alloc_size = nvme_qdma_iod_alloc_size(ctrl, NVMQ_MAX_KB_SZ,
						NVMQ_MAX_SEGS);
	WARN_ON_ONCE(alloc_size > PAGE_SIZE);

	pr_debug("alloc_size is %lu\n", alloc_size);

	ctrl->iod_mempool = mempool_create_node(1, mempool_kmalloc,
						mempool_kfree,
						(void *) alloc_size,
						GFP_KERNEL, node);
	if (!ctrl->iod_mempool) {
		ret = -ENOMEM;
		goto out_free_ctrl;
	}

	ctrl->sqe_mempool = mempool_create_node(1, sqe_kmalloc,
						sqe_kfree,
						(void *) sqe_alloc_size,
						GFP_KERNEL, node);
	if (!ctrl->sqe_mempool) {
		ret = -ENOMEM;
		goto out_free_ctrl;
	}

	dev_info(ctrl->ctrl.device, "new ctrl: NQN \"%s\"\n", ctrl->ctrl.opts->subsysnqn);

	mutex_lock(&nvme_qdma_ctrl_mutex);
	list_add_tail(&ctrl->list, &nvme_qdma_ctrl_list);
	mutex_unlock(&nvme_qdma_ctrl_mutex);

	return &ctrl->ctrl;

out_uninit_ctrl:
	nvmq_uninit_ctrl(&ctrl->ctrl);
	nvmq_put_ctrl(&ctrl->ctrl);
	nvmq_put_ctrl(&ctrl->ctrl);
	if (ret > 0)
		ret = -EIO;
	return ERR_PTR(ret);
out_kfree_queues:
	kfree(ctrl->queues);
out_free_ctrl:
	kfree(ctrl);
	ctrl = NULL;
	return ERR_PTR(ret);
}

static struct nvmqf_transport_ops nvme_qdma_transport = {
	.name		= "qdma",
	.module		= THIS_MODULE,
	.required_opts	= NVMQF_OPT_TRADDR,
	.allowed_opts	= NVMQF_OPT_TRSVCID | NVMQF_OPT_RECONNECT_DELAY |
			  NVMQF_OPT_HOST_TRADDR | NVMQF_OPT_CTRL_LOSS_TMO |
			  NVMQF_OPT_NR_WRITE_QUEUES | NVMQF_OPT_NR_POLL_QUEUES |
			  NVMQF_OPT_TOS,
	.create_ctrl	= nvme_qdma_create_ctrl,
};

// static void nvme_qdma_remove_one(struct ib_device *ib_device, void *client_data)
// {
// 	struct nvme_qdma_ctrl *ctrl;
// 	struct nvme_qdma_device *ndev;
// 	bool found = false;

// 	mutex_lock(&device_list_mutex);
// 	list_for_each_entry(ndev, &device_list, entry) {
// 		if (ndev->dev == ib_device) {
// 			found = true;
// 			break;
// 		}
// 	}
// 	mutex_unlock(&device_list_mutex);

// 	if (!found)
// 		return;

// 	/* Delete all controllers using this device */
// 	mutex_lock(&nvme_qdma_ctrl_mutex);
// 	list_for_each_entry(ctrl, &nvme_qdma_ctrl_list, list) {
// 		if (ctrl->device->dev != ib_device)
// 			continue;
// 		nvme_delete_ctrl(&ctrl->ctrl);
// 	}
// 	mutex_unlock(&nvme_qdma_ctrl_mutex);

// 	flush_workqueue(nvmq_delete_wq);
// }

// static struct ib_client nvme_qdma_ib_client = {
// 	.name   = "nvme_qdma",
// 	.remove = nvme_qdma_remove_one
// };

static int __init nvme_qdma_init_module(void)
{
	int ret;

	// ret = ib_register_client(&nvme_qdma_ib_client);
	// if (ret)
	// 	return ret;
	pr_debug("Debug enabled\n");

	ret = nvmq_core_init();
	if (ret) {
		pr_err("Failed to init nvmq core: %d\n", ret);
		return ret;
	}

	ret = nvmqf_init();
	if (ret) {
		pr_err("Failed to init nvmq fabrics: %d\n", ret);
		return ret;
	}

	ret = nvmqf_register_transport(&nvme_qdma_transport);
	if (ret)
		return ret;

	return 0;

// err_unreg_client:
// 	ib_unregister_client(&nvme_qdma_ib_client);
	// return ret;
}

static void __exit nvme_qdma_cleanup_module(void)
{
	struct nvme_qdma_ctrl *ctrl;

	nvmqf_unregister_transport(&nvme_qdma_transport);
	// ib_unregister_client(&nvme_qdma_ib_client);

	mutex_lock(&nvme_qdma_ctrl_mutex);
	list_for_each_entry(ctrl, &nvme_qdma_ctrl_list, list)
		nvmq_delete_ctrl(&ctrl->ctrl);
	mutex_unlock(&nvme_qdma_ctrl_mutex);
	flush_workqueue(nvmq_delete_wq);

	nvmqf_exit();
	nvmq_core_exit();
}

module_init(nvme_qdma_init_module);
module_exit(nvme_qdma_cleanup_module);

MODULE_LICENSE("GPL v2");
