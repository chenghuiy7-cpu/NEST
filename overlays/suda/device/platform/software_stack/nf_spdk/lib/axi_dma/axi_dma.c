#include "spdk/axi_dma.h"
#include "spdk/log.h"
#include "spdk/memory.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/trace.h"

#include "spdk_internal/trace_defs.h"

#define MAX_AXI_DMA_IOS 65536

SPDK_TRACE_REGISTER_FN(axi_dma_trace, "axi_dma", TRACE_GROUP_AXI_DMA)
{
	spdk_trace_register_object(OBJECT_AXI_DMA_IO, 'a');
	spdk_trace_register_description("AXI_DMA_SUBMIT", TRACE_AXI_DMA_SUBMIT,
					OWNER_NONE, OBJECT_AXI_DMA_IO, 1,
					SPDK_TRACE_ARG_TYPE_INT, "h2c");
	spdk_trace_register_description("AXI_DMA_CMPL", TRACE_AXI_DMA_CMPL,
					OWNER_NONE, OBJECT_AXI_DMA_IO, 1,
					SPDK_TRACE_ARG_TYPE_INT, "h2c");
}

// static struct spdk_mempool *g_io_pool = NULL;
static int loop_cnt = 0;
// static struct spdk_poller *stat_poller = NULL;
// static uint32_t poll_ticks[4];

// static int stat_poller_fn(void *ctx)
// {
//     uint32_t ticks_hz = spdk_get_ticks_hz();
//     printf("%4.2lf %4.2lf %4.2lf %4.2lf\n",
//            (double)poll_ticks[0] / ticks_hz, (double)poll_ticks[1] / ticks_hz,
//            (double)poll_ticks[2] / ticks_hz, (double)poll_ticks[3] / ticks_hz);
    
//     poll_ticks[0] = poll_ticks[1] = poll_ticks[2] = poll_ticks[3] = 0;
// }

int 
spdk_axi_dma_poller(void *ctx)
{
    struct spdk_axi_dma_ch *tx_ch = ctx;
    int cnt = 0;

    // printf("Polling\n");
    loop_cnt++;

    uint64_t tick = spdk_get_ticks();

    struct spdk_axi_dma_io *io = spdk_env_axi_dma_poll_complete(tx_ch->env_ch);

    // poll_ticks[spdk_env_get_current_core()] += spdk_get_ticks() - tick;

    while (io && cnt < 1024) {

        spdk_trace_record(TRACE_AXI_DMA_CMPL, 0, 0, (uintptr_t)io, spdk_get_ticks() - tick);
        loop_cnt = 0;

        spdk_axi_dma_cb_fn cb = io->cb;
        cb(io, 0);
        cnt += io->iovcnt;
        io = spdk_env_axi_dma_poll_complete(tx_ch->env_ch);
    }

    return cnt;
}

//这个Poller和原有的不一样，不返回iovcnt的完成数目，返回的是传输的长度
struct spdk_axi_dma_io* 
spdk_axi_dma_poller_for_compute(void *ctx)
{
    struct spdk_axi_dma_ch *ch = ctx;
    int cnt = 0;
    loop_cnt++;

    //uint64_t tick = spdk_get_ticks();

    struct spdk_axi_dma_io *io = spdk_env_axi_dma_poll_complete(ch->env_ch);
    return io;
}


// static int spdk_axi_dma_rx_poller(void *ctx)
// {
//     struct spdk_axi_dma_ch *tx_ch = ctx;

//     // printf("Polling\n");

//     struct spdk_axi_dma_io *io = spdk_env_axi_dma_poll_complete(tx_ch->env_ch);

//     if (io) {
//         // SPDK_NOTICELOG("Completed RX!\n");
//         spdk_axi_dma_cb_fn cb = io->cb;
//         cb(io, 0);
//         // printf("Buffer content: %s\n", (char *)io->buf);
//     }

//     return 0;
// }

struct spdk_axi_dma_dev *spdk_axi_dma_get_device(const char *name)
{
    struct spdk_axi_dma_dev *dev = spdk_malloc(sizeof(struct spdk_axi_dma_dev), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);

    if (!dev) {
        SPDK_ERRLOG("Failed to allocate device\n");
        return NULL;
    }

    dev->env_dev = spdk_env_axi_dma_get_device(name);

    SPDK_NOTICELOG("env_dev is %p\n", dev->env_dev);

    if (!dev->env_dev) {
        SPDK_ERRLOG("Failed to get device %s\n", name);
        spdk_free(dev);
        return NULL;
    }

    // if (spdk_unlikely(!stat_poller)) {
    //     stat_poller = spdk_poller_register(stat_poller_fn, NULL, 1000000);
    // }

    return dev;
}

struct spdk_axi_dma_ch *spdk_axi_dma_create_tx_channel(struct spdk_axi_dma_dev *dev, uint32_t num_bds, int id, uint8_t tid)
{
    struct spdk_axi_dma_ch *ch = spdk_malloc(sizeof(struct spdk_axi_dma_ch), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);

    if (!ch) {
        SPDK_ERRLOG("Failed to allocate device\n");
        return NULL;
    }

    if (id >= 4) {
        SPDK_WARNLOG("Creating TX channel id %d >= 4, use %d instead\n", id, id % 4);
        id %= 4;
    }

    ch->env_ch = spdk_env_axi_dma_create_tx_channel(dev->env_dev, num_bds, id, tid);

    if (!ch->env_ch) {
        SPDK_ERRLOG("Failed to create TX channel\n");
        spdk_free(ch) ; 
        return NULL;
    }

    ch->thread = spdk_get_thread();
    // ch->cmpl_poller = spdk_poller_register(spdk_axi_dma_poller, ch, 0);
    ch->id = id;

    if (spdk_unlikely(spdk_simple_pool_init(&ch->io_pool, num_bds, sizeof(struct spdk_axi_dma_io)) != 0)) {
        SPDK_ERRLOG("Failed to allocate axi dma io pool for channel %d\n", id);
    }

    return ch;
}

struct spdk_axi_dma_ch *spdk_axi_dma_create_rx_channel(struct spdk_axi_dma_dev *dev, uint32_t num_bds, int id, uint8_t tid)
{
    struct spdk_axi_dma_ch *ch = spdk_malloc(sizeof(struct spdk_axi_dma_ch), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);

    if (!ch) {
        SPDK_ERRLOG("Failed to allocate device\n");
        return NULL;
    }

    if (id >= 4) {
        SPDK_WARNLOG("Creating RX channel id %d >= 4, use %d instead\n", id, id % 4);
        id %= 4;
    }

    ch->env_ch = spdk_env_axi_dma_create_rx_channel(dev->env_dev, num_bds, id, tid);

    if (!ch->env_ch) {
        SPDK_ERRLOG("Failed to create TX channel\n");
        spdk_free(ch);
        return NULL;
    }

    ch->thread = spdk_get_thread();
    // ch->cmpl_poller = spdk_poller_register(spdk_axi_dma_rx_poller, ch, 0);
    ch->id = id;

    if (spdk_unlikely(spdk_simple_pool_init(&ch->io_pool, num_bds, sizeof(struct spdk_axi_dma_io)) != 0)) {
        SPDK_ERRLOG("Failed to allocate axi dma io pool for channel %d\n", id);
    }

    return ch;
}

int spdk_axi_dma_tx_channel_send(struct spdk_axi_dma_ch *ch, struct spdk_axi_dma_iovec *iovs, int iovcnt, spdk_axi_dma_cb_fn cb, void *ctx, struct spdk_axi_dma_ctrl *ctrl)
{
    struct spdk_axi_dma_io *io = spdk_simple_pool_get(&ch->io_pool);
    if (!io) {
        SPDK_ERRLOG("Failed to allocate spdk_axi_dma_io\n");
        return -ENOMEM;
    }

    io->ch = ch;
    io->iovs = iovs;
    io->iovcnt = iovcnt;
    io->cb = cb;
    io->ctx = ctx;
    io->transfered_length = 0;
    // io->start = spdk_get_ticks();
    if (ctrl != NULL) {
        memcpy(&io->ctrl, ctrl, sizeof(struct spdk_axi_dma_ctrl));
    }

    spdk_trace_record(TRACE_AXI_DMA_SUBMIT, 0, 0, (uintptr_t)io, 0);

    for (int i = 0; i < iovcnt; i++) {
        uint64_t len = io->iovs[i].iov_len;
        io->transfered_length += len;
        io->iovs[i].paddr = spdk_vtophys(io->iovs[i].iov_base, &len);
        //SPDK_DEBUGLOG(nvme,"TRACE DMA VTOPHYS ADDR%lx\n",io->iovs[i].paddr);
        if (io->iovs[i].paddr == SPDK_VTOPHYS_ERROR || len != io->iovs[i].iov_len) {
            SPDK_ERRLOG("vtophys failed: %lX %lu\n", io->iovs[i].paddr, len);
            return -EFAULT;
        }
    }

    int ret = spdk_env_axi_dma_tx_channel_send(ch->env_ch, iovs, iovcnt, io);

    return ret;
}

int spdk_axi_dma_rx_channel_recv(struct spdk_axi_dma_ch *ch, struct spdk_axi_dma_iovec *iovs, int iovcnt, spdk_axi_dma_cb_fn cb, void *ctx)
{
    struct spdk_axi_dma_io *io = spdk_simple_pool_get(&ch->io_pool);
    if (!io) {
        SPDK_ERRLOG("Failed to allocate spdk_axi_dma_io\n");
        return -ENOMEM;
    }

    io->ch = ch;
    io->iovs = iovs;
    io->iovcnt = iovcnt;
    io->cb = cb;
    io->ctx = ctx;
    io->transfered_length = 0;
    io->used_iovcnt = 0;
    spdk_trace_record(TRACE_AXI_DMA_SUBMIT, 0, 0, (uintptr_t)io, 1);

    for (int i = 0; i < iovcnt; i++) {
        uint64_t len = io->iovs[i].iov_len;
        io->transfered_length += len;
        // TODO: No need for vtophys conversion here because all buffers are pre-allocated
        io->iovs[i].paddr = spdk_vtophys(io->iovs[i].iov_base, &len);
        if (io->iovs[i].paddr == SPDK_VTOPHYS_ERROR || len != io->iovs[i].iov_len) {
            SPDK_ERRLOG("vtophys failed: %lX %lu\n", io->iovs[i].paddr, len);
            spdk_simple_pool_put(&ch->io_pool, io);
            return -EFAULT;
        }
    }

    // uint64_t tick = spdk_get_ticks();

    int ret = spdk_env_axi_dma_rx_channel_recv(ch->env_ch, iovs, iovcnt, io);

    if (ret != 0) {
        spdk_simple_pool_put(&ch->io_pool, io);
    }

    // poll_ticks[spdk_env_get_current_core()] += spdk_get_ticks() - tick;

    return ret;
}

void spdk_axi_dma_ch_get_stat(struct spdk_axi_dma_ch *ch, struct spdk_axi_dma_ch_stat *stat)
{
    spdk_env_axi_dma_channel_get_stat(ch->env_ch, (struct spdk_env_axi_dma_channel_stat *)stat);
}

inline void spdk_axi_dma_stop_channel(struct spdk_axi_dma_ch *ch){
    spdk_env_axi_dma_stop_channel(ch->env_ch);
}

inline void spdk_axi_dma_enable_channel(struct spdk_axi_dma_ch *ch){
    spdk_env_axi_dma_enable_channel(ch->env_ch);
}

void spdk_axi_dma_io_free(struct spdk_axi_dma_io *io)
{
    struct spdk_axi_dma_ch *ch = io->ch;

    spdk_simple_pool_put(&ch->io_pool, io);
}

int spdk_axi_dma_tx_channel_send_seg(struct spdk_axi_dma_ch *ch, struct spdk_axi_dma_iovec *iovs, int iovcnt, spdk_axi_dma_cb_fn cb, void *ctx, struct spdk_axi_dma_ctrl *ctrl,bool last_data)
{
    struct spdk_axi_dma_io *io = spdk_simple_pool_get(&ch->io_pool);
    if (!io) {
        SPDK_ERRLOG("Failed to allocate spdk_axi_dma_io\n");
        assert(false);
        return -ENOMEM;
    }
    io->used_iovcnt = 0;
    io->ch = ch;
    io->iovs = iovs;
    io->iovcnt = iovcnt;
    io->cb = cb;
    io->ctx = ctx;
    io->transfered_length = 0;
    // io->start = spdk_get_ticks();
    if (ctrl != NULL) {
        memcpy(&io->ctrl, ctrl, sizeof(struct spdk_axi_dma_ctrl));
    }

    spdk_trace_record(TRACE_AXI_DMA_SUBMIT, 0, 0, (uintptr_t)io, 0);

    for (int i = 0; i < iovcnt; i++) {
        uint64_t len = io->iovs[i].iov_len;
        io->transfered_length += len;
        io->iovs[i].paddr = spdk_vtophys(io->iovs[i].iov_base, &len);
        //SPDK_DEBUGLOG(nvme,"TRACE DMA VTOPHYS ADDR%lx\n",io->iovs[i].paddr);
        if (io->iovs[i].paddr == SPDK_VTOPHYS_ERROR || len != io->iovs[i].iov_len) {
            SPDK_ERRLOG("vtophys failed: %lX %lu\n", io->iovs[i].paddr, len);
            return -EFAULT;
        }
        
    }

    int ret = spdk_env_axi_dma_tx_channel_send_seg(ch->env_ch, iovs, iovcnt, io,last_data);
    return ret;
}
