#ifndef RTE_AXI_DMA_H
#define RTE_AXI_DMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <bits/types/struct_iovec.h>

#include <rte_debug.h>
#include <rte_interrupts.h>
#include <rte_dev.h>
#include <rte_bus.h>
#include <rte_vfio.h>

#define AXI_MCDMA_MAX_CH 16

struct mm2s_common_registers;
struct s2mm_common_registers;
struct mm2s_channel_register;
struct s2mm_channel_register;

struct mm2s_bd;
struct s2mm_bd;

// struct rte_axi_dma_tx_desc {
// 	struct mm2s_bd *bd;
// 	struct rte_axi_dma_tx_desc *next;
// 	void *opaque;
// 	uint32_t id;
// };

// struct rte_axi_dma_rx_desc {
// 	struct s2mm_bd *bd;
// 	struct rte_axi_dma_rx_desc *next;
// 	void *opaque;
// 	uint32_t id;
// };

union bd;

struct rte_axi_dma_iovec {
	void *iov_base;
	size_t iov_len;
	uint64_t paddr;
};

struct rte_axi_dma_desc {
	union bd *bd;
	struct rte_axi_dma_desc *next;
	void *opaque;
	uint32_t id;
	uint32_t bd_phys_lsb;
	uint32_t bd_phys_msb;
	bool tx;
	bool request_end;
};

struct rte_axi_dma_desc_ring {
	struct rte_axi_dma_desc *desc_arr;
	union bd *bd_ring;
	struct rte_axi_dma_desc *head;
	struct rte_axi_dma_desc *tail;
	bool tx;
};

// struct rte_axi_dma_tx_desc_ring {
// 	struct rte_axi_dma_tx_desc *desc_arr;
// 	struct mm2s_bd *bd_ring;
// 	struct rte_axi_dma_tx_desc *head;
// 	struct rte_axi_dma_tx_desc *tail;
// };

// struct rte_axi_dma_rx_desc_ring {
// 	struct rte_axi_dma_rx_desc *desc_arr;
// 	struct s2mm_bd *bd_ring;
// 	struct rte_axi_dma_rx_desc *head;
// 	struct rte_axi_dma_rx_desc *tail;
// };

union channel_reg;

struct rte_axi_dma_device;

struct rte_axi_dma_channel {
	struct rte_axi_dma_device *dev;
	volatile union channel_reg *regs;
	struct rte_axi_dma_desc_ring bd_chain;
	uint32_t count;
	bool tx;
	uint8_t id;
};

struct rte_axi_dma_channel_stat {
	uint32_t pkt_dropped;
	uint32_t pkt_processed;
};

// struct rte_axi_dma_tx_channel {
// 	struct rte_axi_dma_device *dev;
// 	volatile struct mm2s_channel_register *regs;
// 	struct rte_axi_dma_tx_desc_ring bd_chain;
// 	bool started;
// };

// struct rte_axi_dma_rx_channel {
// 	struct rte_axi_dma_device *dev;
// 	volatile struct s2mm_channel_register *regs;
// 	struct rte_axi_dma_rx_desc_ring bd_chain;
// };

struct rte_axi_dma_device {
	int vfio_dev_fd;
	volatile uint8_t *base_regs;
	struct vfio_device_info device_info;
	volatile struct mm2s_common_registers *mm2s;
	volatile struct s2mm_common_registers *s2mm;
	struct rte_axi_dma_channel tx_chs[AXI_MCDMA_MAX_CH];
	struct rte_axi_dma_channel rx_chs[AXI_MCDMA_MAX_CH];
	const char *name;
};

struct rte_axi_dma_device *rte_axi_dma_get_device(const char *name);

struct rte_axi_dma_channel *rte_axi_dma_create_tx_channel(struct rte_axi_dma_device *dev, int id, uint32_t num_bds, uint8_t tid);

struct rte_axi_dma_channel *rte_axi_dma_create_rx_channel(struct rte_axi_dma_device *dev, int id, uint32_t num_bds, uint8_t tid);

int rte_axi_dma_send(struct rte_axi_dma_channel *ch, struct rte_axi_dma_iovec *iovs, int iovcnt, void *ctx);

int rte_axi_dma_recv(struct rte_axi_dma_channel *ch, struct rte_axi_dma_iovec *iovs, int iovcnt, void *ctx);

void *rte_axi_dma_poll_complete(struct rte_axi_dma_channel *ch);
// int rte_axi_dma_channel_recv(struct rte_axi_dma_device *dev, void *buf, uint64_t len);

void rte_axi_dma_channel_get_stat(struct rte_axi_dma_channel *ch, struct rte_axi_dma_channel_stat *stat);

inline void rte_axi_dma_stop_channel(struct rte_axi_dma_channel *ch);

inline void rte_axi_dma_enable_channel(volatile struct rte_axi_dma_channel *ch);

int rte_axi_dma_send_seg(struct rte_axi_dma_channel *ch, struct rte_axi_dma_iovec *iovs, int iovcnt, void *ctx,bool last_data);

#endif // !RTE_AXI_DMA_H
