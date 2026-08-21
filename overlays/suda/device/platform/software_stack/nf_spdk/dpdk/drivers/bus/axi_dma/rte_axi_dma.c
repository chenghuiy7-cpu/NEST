#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_cycles.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include "rte_axi_dma.h"
#include "pthread.h"
#include "rte_atomic.h"
#include "rte_io.h"
#include "rte_memory.h"

#define SYSFS_PREFIX "/sys/bus/platform/devices"
#define BIT(X) (1 << X)

#define TXSOF BIT(31)
#define TXEOF BIT(30)
#define RXSOF BIT(27)
#define RXEOF BIT(26)

#define STS_LEN (BIT(26) - 1)

#define MCDMA_DEBUG  0

#define DMA_DBG(...)                \
  do                                \
  {                                 \
    if (MCDMA_DEBUG)                \
      fprintf(stderr, __VA_ARGS__); \
  } while (0)

struct mm2s_common_registers
{
  uint32_t ccr;
  uint32_t csr;
  uint32_t chen;
  uint32_t chser;
  uint32_t err;
  uint32_t ch_schd_type;
  uint32_t wrr_reg1;
  uint32_t wrr_reg2;
  uint32_t channels_serviced;
  uint32_t arcache_aruser;
  uint32_t intr_status;
  uint32_t _padding[5];
};

struct s2mm_common_registers
{
  uint32_t ccr;
  uint32_t csr;
  uint32_t chen;
  uint32_t chser;
  uint32_t err;
  uint32_t pktdrop;
  uint32_t channels_serviced;
  uint32_t awcache_awuser;
  uint32_t intr_status;
  uint32_t _padding[7];
};

struct mm2s_channel_register
{
  uint32_t cr;
  uint32_t sr;
  uint32_t curdesc_lsb;
  uint32_t curdesc_msb;
  uint32_t taildesc_lsb;
  uint32_t taildesc_msb;
  uint32_t pktcount_stat;
  uint32_t _padding[9];
};

struct s2mm_channel_register
{
  uint32_t cr;
  uint32_t sr;
  uint32_t curdesc_lsb;
  uint32_t curdesc_msb;
  uint32_t taildesc_lsb;
  uint32_t taildesc_msb;
  uint32_t pktdrop_stat;
  uint32_t pktcount_stat;
  uint32_t _padding[8];
};

struct bd_status
{
  uint32_t transfered_bytes : 26;
  uint32_t rxeof : 1;
  uint32_t rxsof : 1;
  uint32_t int_err : 1;
  uint32_t slv_err : 1;
  uint32_t dec_err : 1;
  uint32_t completed : 1;
};

struct mm2s_bd
{
  uint32_t next_desc;
  uint32_t next_desc_msb;
  uint32_t buf_addr;
  uint32_t buf_addr_msb;
  uint32_t rsvd;
  uint32_t ctrl;
  struct ctrl_sideband
  {
    uint32_t tuser : 16;
    uint32_t rsvd : 8;
    uint32_t tid : 8;
  } ctrl_sideband;
  struct bd_status status;
  uint32_t app0;
  uint32_t app1;
  uint32_t app2;
  uint32_t app3;
  uint32_t app4;
  uint32_t _padding[3];
};

struct s2mm_bd
{
  uint32_t next_desc;
  uint32_t next_desc_msb;
  uint32_t buf_addr;
  uint32_t buf_addr_msb;
  uint32_t rsvd;
  uint32_t ctrl;
  struct bd_status status;
  struct sideband_status
  {
    uint32_t tuser : 16;
    uint32_t tdest : 4;
    uint32_t rsvd : 4;
    uint32_t tid : 8;
  } sideband_status;
  uint32_t app0;
  uint32_t app1;
  uint32_t app2;
  uint32_t app3;
  uint32_t app4;
  uint32_t _padding[3];
};

union bd
{
  struct s2mm_bd rx;
  struct mm2s_bd tx;
};

union channel_reg
{
  struct s2mm_channel_register rx;
  struct mm2s_channel_register tx;
};

struct reg_lock
{
  pthread_mutex_t mutex;
};

static struct reg_lock lock;





// 初始化共享互斥锁
void init_lock()
{
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(&(lock.mutex), &attr);
  pthread_mutexattr_destroy(&attr);

  return;
}
const char *dirs[2] = {"RX", "TX"};

struct rte_axi_dma_device *rte_axi_dma_get_device(const char *name)
{
  init_lock();
  struct rte_axi_dma_device *dev =
      rte_zmalloc("OTHER", sizeof(struct rte_axi_dma_device), 0);
  struct vfio_region_info reg = {.argsz = sizeof(reg)};
  dev->device_info.argsz = sizeof(struct vfio_device_info);
  int ret;

  ret = rte_vfio_setup_device(SYSFS_PREFIX, name, &dev->vfio_dev_fd,
                              &dev->device_info);

  if (ret)
  {
    DMA_DBG("VFIO setup device failed!\n");
    return NULL;
  }

  reg.index = 0;
  ret = ioctl(dev->vfio_dev_fd, VFIO_DEVICE_GET_REGION_INFO, &reg);

  if (ret)
  {
    DMA_DBG("Couldn't get src region %d info\n", reg.index);
    return NULL;
  }

  DMA_DBG("- Region %d: size=0x%llx offset=0x%llx flags=0x%x\n", reg.index,
          reg.size, reg.offset, reg.flags);

  dev->base_regs = mmap(NULL, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        dev->vfio_dev_fd, reg.offset);

  if (dev->base_regs != MAP_FAILED)
  {
    DMA_DBG("Successful MMAP of src AXI DMA to address %p\n", dev->base_regs);
  }

  dev->mm2s = (volatile struct mm2s_common_registers *)dev->base_regs;
  dev->s2mm =
      (volatile struct s2mm_common_registers *)(dev->base_regs + 0x0500);

  dev->name = name;

  DMA_DBG("%s: mm2s regs 0x%lx, ccr addrs 0x%lx\n", name, (uintptr_t)dev->mm2s,
          (uintptr_t)&dev->mm2s->ccr);

  // volatile uint32_t *ccr = &dev->mm2s->ccr;
  // /* MCDMA.RESET */
  // *ccr |= BIT(2);
  // while (*ccr & BIT(2));

  // ccr = &dev->s2mm->ccr;
  // *ccr |= BIT(2);
  // while (*ccr & BIT(2));

  dev->mm2s->ccr |= BIT(2);
  while (dev->mm2s->ccr & BIT(2))
    ;

  dev->s2mm->ccr |= BIT(2);
  while (dev->s2mm->ccr & BIT(2))
    ;

  DMA_DBG("%s reset success\n", name);

  return dev;
}

static inline volatile struct mm2s_channel_register *tx_ch_reg_offset(
    struct rte_axi_dma_device *dev, int id)
{
  volatile struct mm2s_channel_register *base =
      (volatile struct mm2s_channel_register *)((volatile char *)dev->mm2s +
                                                0x40);
  return base + id;
}

static inline volatile struct s2mm_channel_register *rx_ch_reg_offset(
    struct rte_axi_dma_device *dev, int id)
{
  volatile struct s2mm_channel_register *base =
      (volatile struct s2mm_channel_register *)((volatile char *)dev->s2mm +
                                                0x40);
  return base + id;
}

static inline void virt2phys_64(uint64_t virt, volatile uint32_t *phys_lsb,
                                volatile uint32_t *phys_msb)
{
  rte_iova_t iova = rte_mem_virt2iova((const void *)virt);
  *phys_lsb = iova;
  *phys_msb = (uintptr_t)iova >> 32;
}

static struct rte_axi_dma_channel *rte_axi_dma_create_channel(
    struct rte_axi_dma_device *dev, int id, uint32_t num_bds, uint8_t tid,
    bool tx)
{
  struct rte_axi_dma_channel *ch;

  if (tx)
  {
    ch = &dev->tx_chs[id];
  }
  else
  {
    ch = &dev->rx_chs[id];
  }

  /* Enable channel */
  pthread_mutex_lock(&(lock.mutex));
  if (tx)
  {
    dev->mm2s->chen |= BIT(id);

    DMA_DBG("CHEN TX %lx\n", dev->mm2s->chen);
  }
  else
  {
    dev->s2mm->chen |= BIT(id);
    DMA_DBG("CHEN RX %lx\n", dev->s2mm->chen);
  }

  ch->dev = dev;
  if (tx)
  {
    ch->regs = (volatile union channel_reg *)tx_ch_reg_offset(dev, id);
  }
  else
  {
    ch->regs = (volatile union channel_reg *)rx_ch_reg_offset(dev, id);
  }
  ch->tx = tx;

  ch->bd_chain.tx = tx;

  DMA_DBG("Allocate %u bds for channel %d of device %s\n", num_bds, id,
          dev->name);

  /* Initialize buffer descriptor chain */
  ch->bd_chain.desc_arr =
      rte_zmalloc("AXI_DMA_MEM", num_bds * sizeof(struct rte_axi_dma_desc), 0);

  if (!ch->bd_chain.desc_arr)
  {
    RTE_LOG(CRIT, EAL, "Malloc bd chain failed\n");
    return NULL;
  }

  ch->bd_chain.bd_ring =
      rte_zmalloc("AXI_DMA_MEM", num_bds * sizeof(union bd), sizeof(union bd));
  
  if (!ch->bd_chain.bd_ring)
  {
    RTE_LOG(CRIT, EAL, "Malloc bd ring failed\n");
    return NULL;
  }

  ch->bd_chain.head = ch->bd_chain.desc_arr;
  ch->bd_chain.tail = ch->bd_chain.desc_arr;

  for (uint32_t i = 0; i < num_bds; i++)
  {
    ch->bd_chain.desc_arr[i].bd = &ch->bd_chain.bd_ring[i];
    ch->bd_chain.desc_arr[i].id = i;
    ch->bd_chain.desc_arr[i].tx = tx;
    uint64_t bd_phys = rte_mem_virt2iova(ch->bd_chain.desc_arr[i].bd);
    ch->bd_chain.desc_arr[i].bd_phys_lsb = bd_phys;
    ch->bd_chain.desc_arr[i].bd_phys_msb = bd_phys >> 32;
  }

  for (uint32_t i = 0; i < num_bds; i++)
  {
    struct rte_axi_dma_desc *next_bd =
        &ch->bd_chain.desc_arr[(i + 1) % num_bds];
    ch->bd_chain.desc_arr[i].next = next_bd;
    if (tx)
    {
      virt2phys_64((uintptr_t)next_bd->bd,
                   &ch->bd_chain.desc_arr[i].bd->tx.next_desc,
                   &ch->bd_chain.desc_arr[i].bd->tx.next_desc_msb);
    }
    else
    {
      virt2phys_64((uintptr_t)next_bd->bd,
                   &ch->bd_chain.desc_arr[i].bd->rx.next_desc,
                   &ch->bd_chain.desc_arr[i].bd->rx.next_desc_msb);
    }
  }

  if (tx)
  {
    /* Program CD and TD register */
    virt2phys_64((uintptr_t)ch->bd_chain.head->bd, &ch->regs->tx.curdesc_lsb,
                 &ch->regs->tx.curdesc_msb);
    rte_io_wmb();
    virt2phys_64((uintptr_t)ch->bd_chain.head->bd, &ch->regs->tx.taildesc_lsb,
                &ch->regs->tx.taildesc_msb);
    rte_io_wmb();
    /* Start channel */
    ch->regs->tx.cr |= BIT(0); // CH.RS

    /* Start Device */
    ch->dev->mm2s->ccr |= BIT(0);
    // virt2phys_64((uintptr_t)ch->bd_chain.head->bd,
    // &ch->regs->tx.taildesc_lsb,
    //              &ch->regs->tx.taildesc_msb);
  }
  else
  {
    /* Program CD and TD register */
    rte_io_rmb();
    printf("HEAD BD%llx virt address%llx\n",ch->bd_chain.head->bd,rte_mem_virt2iova(ch->bd_chain.head->bd));
    virt2phys_64((uintptr_t)ch->bd_chain.head->bd, &ch->regs->rx.curdesc_lsb,
                 &ch->regs->rx.curdesc_msb);
    rte_io_wmb();
    virt2phys_64((uintptr_t)ch->bd_chain.head->bd, &ch->regs->rx.taildesc_lsb,
                 &ch->regs->rx.taildesc_msb);
    // virt2phys_64((uintptr_t)ch->bd_chain.head->bd,
    // &ch->regs->rx.taildesc_lsb,
    //              &ch->regs->rx.taildesc_msb);
    rte_io_wmb();
    /* Start channel */
    ch->regs->rx.cr |= BIT(0); // CH.RS

    /* Start Device */
    dev->s2mm->ccr |= BIT(0);
    DMA_DBG("1CHEN %x TAILDESC%x  CURDESC%x BD_PHYS_LSB%llx\n", ch->dev->s2mm->chen,
            ch->regs->rx.taildesc_lsb, ch->regs->rx.curdesc_lsb, rte_mem_virt2iova(ch->bd_chain.head->bd));
  }
  pthread_mutex_unlock(&(lock.mutex));
  ch->id = id;

  DMA_DBG("Created channel %d on device %s\n", id, dev->name);

  return ch;
}

struct rte_axi_dma_channel *rte_axi_dma_create_tx_channel(
    struct rte_axi_dma_device *dev, int id, uint32_t num_bds, uint8_t tid)
{
  assert(id < AXI_MCDMA_MAX_CH);

  return rte_axi_dma_create_channel(dev, id, num_bds, tid, true);
}

struct rte_axi_dma_channel *rte_axi_dma_create_rx_channel(
    struct rte_axi_dma_device *dev, int id, uint32_t num_bds, uint8_t tid)
{
  assert(id < AXI_MCDMA_MAX_CH);

  return rte_axi_dma_create_channel(dev, id, num_bds, tid, false);
}

struct dummy_dma_io
{
  uint64_t start;
  uint64_t end;
  struct sideband
  {
    uint32_t tuser : 16;
    uint32_t tdest : 4;
    uint32_t rsvd : 4;
    uint32_t tid : 8;
  } sideband;
  struct bd_status status;
  uint32_t transfered_length;
};

static inline bool channel_empty(struct rte_axi_dma_channel *ch)
{
  struct rte_axi_dma_desc_ring *dev_buf = &ch->bd_chain;
  if (ch->count)
  {
    return false;
  }
  else
  {
    return dev_buf->head == dev_buf->tail;
  }
}

static inline bool channel_full(struct rte_axi_dma_channel *ch)
{
  struct rte_axi_dma_desc_ring *dev_buf = &ch->bd_chain;
  if (ch->count)
  {
    return dev_buf->head == dev_buf->tail;
  }
  else
  {
    return false;
  }
}

static int rte_axi_dma_add_buffer(struct rte_axi_dma_channel *ch,
                                  struct rte_axi_dma_iovec *iovs, int iovcnt,
                                  void *ctx)
{
  struct rte_axi_dma_desc_ring *dev_buf = &ch->bd_chain;
  struct rte_axi_dma_desc *tail = dev_buf->tail;
  
  union bd *tail_bd;
  struct dummy_dma_io *io = ctx;
   
  if (channel_full(ch))
  {
    RTE_LOG(CRIT, EAL, "Buffer is full\n");
    printf("CHannel id %d, dir %s count%d head%llx tail%llx\n",ch->id,ch->tx?"TX":"RX",ch->count,ch->bd_chain.head,ch->bd_chain.tail);
    return -1;
  }

  for (int i = 0; i < iovcnt; i++)
  {
    uint64_t buf = iovs[i].paddr;
    size_t len = iovs[i].iov_len;
    tail_bd = tail->bd;

    if (len >= BIT(26))
    {
      RTE_LOG(CRIT, EAL, "Buffer is too large at %lu\n", len);
      return -1;
    }

    if (ch->tx)
    {
      DMA_DBG("[%u %s] %u: Add buffer to bd %p: %lX %lu dest%d\n", ch->id,
              dirs[ch->tx], io->sideband.tid, tail_bd, buf, len,io->sideband.tdest);
      
      tail_bd->tx.buf_addr = buf;
      tail_bd->tx.buf_addr_msb = buf >> 32;
      tail_bd->tx.ctrl_sideband.rsvd = 0;
   
      tail_bd->tx.ctrl = len & (BIT(26) - 1);
      if (i == 0)
      {
        tail_bd->tx.ctrl |= BIT(31); // TX SoF
      }
      tail_bd->tx.ctrl_sideband.tid = io->sideband.tid;
      tail_bd->tx.ctrl_sideband.tuser = io->sideband.tuser;
      // if (g_eof) {
      // 	tail_bd->tx.ctrl |= BIT(30); // TX EoF
      // }
      // g_eof = !g_eof;
      tail_bd->tx.status.completed = 0; // Not completed

      // virt2phys_64((uintptr_t)tail->bd, &ch->regs->tx.taildesc_lsb,
      // &ch->regs->tx.taildesc_msb); DMA_DBG("virt2phys: bd_phys_lsb: %08X,
      // msb: %08X\n", ch->regs->tx.taildesc_lsb, ch->regs->tx.taildesc_msb);
    }
    else
    {
      DMA_DBG("[%u %s] Add buffer to bd %p: %lX %lu\n", ch->id, dirs[ch->tx],
              tail_bd, buf, len);
      // virt2phys_64((uintptr_t)buf, &tail_bd->rx.buf_addr,
      // &tail_bd->rx.buf_addr_msb);

      // rte_iova_t iova = rte_malloc_virt2iova(buf);
      tail_bd->rx.buf_addr = buf;
      tail_bd->rx.buf_addr_msb = buf >> 32;
      // DMA_DBG("%s: Tail is %llX, tail->next is %llX\n", ch->dev->name, tail,
      // tail->next);

      // TODO: How to get RX length?
      tail_bd->rx.ctrl = len & (BIT(26) - 1);
      // tail_bd->rx.ctrl |= BIT(31); // rX SoF
      // tail_bd->rx.ctrl |= BIT(30); // rX EoF

      tail_bd->rx.status.completed = 0; // Not completed

      // virt2phys_64((uintptr_t)tail->bd, &ch->regs->rx.taildesc_lsb,
      // &ch->regs->rx.taildesc_msb);
    }

    tail->opaque = ctx;
    tail->request_end = (i == iovcnt - 1);

    if (i == iovcnt - 1)
    {
      if (ch->tx)
      {
        tail_bd->tx.ctrl |= BIT(30); // TX EoF
        rte_wmb();
        rte_io_wmb();
        ch->regs->tx.taildesc_lsb = tail->bd_phys_lsb;
        rte_io_wmb();
        ch->regs->tx.taildesc_msb = tail->bd_phys_msb;
      }
      else
      {
        rte_wmb();
        rte_io_wmb();
        ch->regs->rx.taildesc_lsb = tail->bd_phys_lsb;
        rte_io_wmb();
        ch->regs->rx.taildesc_msb = tail->bd_phys_msb;
        DMA_DBG("1CHEN %x TAILDESC%x  CURDESC%x BD_PHYS_LSB%llx\n", ch->dev->s2mm->chen,
        ch->regs->rx.taildesc_lsb, ch->regs->rx.curdesc_lsb, rte_mem_virt2iova(ch->bd_chain.head->bd));
      }
    }
    /*
    if (ch->tx) {
      uint32_t *sideband = &tail_bd->tx.ctrl_sideband;
      DMA_DBG("bd ctrl: %X sideband: %X tid%x tuser%x\n", tail_bd->tx.ctrl,
    *sideband,tail_bd->tx.ctrl_sideband.tid,tail_bd->tx.ctrl_sideband.tuser); }
    else { DMA_DBG("bd ctrl: %X\n", tail_bd->tx.ctrl);
    }*/

    tail = tail->next;
    dev_buf->tail = tail;
    ch->count++;
  }
  io->start = rte_get_timer_cycles();

  /* Move tail to next position */
  // DMA_DBG("%s: Tail is %llX, tail->opaque is %llX, tail->next is %llX\n",
  // ch->dev->name, tail, tail->opaque, tail->next);
  return 0;
}

/* Expects physical addresses in iovs */
int rte_axi_dma_send(struct rte_axi_dma_channel *ch,
                     struct rte_axi_dma_iovec *iovs, int iovcnt, void *ctx)
{
  return rte_axi_dma_add_buffer(ch, iovs, iovcnt, ctx);
}

/* Expects physical addresses in iovs */
int rte_axi_dma_recv(struct rte_axi_dma_channel *ch,
                     struct rte_axi_dma_iovec *iovs, int iovcnt, void *ctx)
{
  return rte_axi_dma_add_buffer(ch, iovs, iovcnt, ctx);
}

void *rte_axi_dma_poll_complete(struct rte_axi_dma_channel *ch)
{
  struct rte_axi_dma_desc_ring *dev_buf = &ch->bd_chain;
  struct rte_axi_dma_desc *head = dev_buf->head;
  uint32_t err;
  struct bd_status status;

  // DMA_DBG("Polling %u\n", ch->id);

  err = ch->tx ? ch->dev->mm2s->err : ch->dev->s2mm->err;

  if (err)
  {
    RTE_LOG(CRIT, EAL, "%s error: %X id%d\n", (ch->tx ? "MM2S" : "S2MM"), err,
            ch->id);
  }

  // if (ch->tx) {
  // 	DMA_DBG("Head is %u, tail is %u, head bd ctrl %X, Head BD status: %X\n",
  // head->id, dev_buf->tail->id, head->bd->tx.ctrl, head->bd->tx.status);
  // 	DMA_DBG("Tail paddr is %X, cur paddr is %X, tail paddr is %X\n",
  // head->bd->tx.next_desc, ch->regs->tx.curdesc_lsb,
  // ch->regs->tx.taildesc_lsb); } else { 	DMA_DBG("Head is %u, tail is %u,
  // head bd ctrl %X, Head BD status: %X\n", head->id, dev_buf->tail->id,
  // head->bd->tx.ctrl, head->bd->rx.status); 	DMA_DBG("Tail paddr is %X, cur
  // paddr is %X, tail paddr is %X\n", head->bd->rx.next_desc,
  // ch->regs->rx.curdesc_lsb, ch->regs->rx.taildesc_lsb);
  // }

  if (channel_empty(ch))
  {
    // Buffer empty
    return NULL;
  }

  // DMA_DBG("Polling bd %p\n", head->bd);

  status = ch->tx ? head->bd->tx.status : head->bd->rx.status;
  rte_io_rmb();
  bool show_status = false;

  while (status.completed)
  {
    // if (ch->tx) {
    // 	DMA_DBG("%s: Head is %u, tail is %u, Head BD status: %X\n",
    // ch->dev->name, head->id, dev_buf->tail->id, head->bd->tx.status);
    // 	DMA_DBG("%s: Tail paddr is %X, cur paddr is %X, tail paddr is %X\n",
    // ch->dev->name, head->bd->tx.next_desc, ch->regs->tx.curdesc_lsb,
    // ch->regs->tx.taildesc); } else { 	DMA_DBG("%s: Head is %u, tail is
    // %u, Head BD status: %X\n", ch->dev->name, head->id, dev_buf->tail->id,
    // head->bd->rx.status); 	DMA_DBG("%s: Tail paddr is %X, cur paddr is %X,
    // tail paddr is %X\n", ch->dev->name, head->bd->rx.next_desc,
    // ch->regs->rx.curdesc_lsb, ch->regs->rx.taildesc_lsb);
    // }
    rte_io_rmb();
    uint32_t tid = ch->tx ? head->bd->tx.ctrl_sideband.tid
                          : head->bd->rx.sideband_status.tid;
    uint32_t tuser = ch->tx ? head->bd->tx.ctrl_sideband.tuser
                            : head->bd->rx.sideband_status.tuser;
    rte_io_rmb();
    DMA_DBG("[%u %s] %u: Completed with %X, opaque: %p, tuser: %u head address%llx count%d\n",
            ch->id, dirs[ch->tx], tid, status, head->opaque,
            (unsigned short)tuser,head->bd_phys_lsb,ch->regs->rx.pktcount_stat);
    // if (status != 2348810304) {
    // 	RTE_LOG(CRIT, EAL, "Status is %X, should be %X\n", status, 2348810304);
    // }
    bool request_end = head->request_end;
    void *opaque = head->opaque;

    dev_buf->head = head->next;
    
    ch->count--;
    uint32_t ctrl = ch->tx ? head->bd->tx.ctrl : head->bd->rx.ctrl;
    if (opaque)
    {
      struct dummy_dma_io *io = opaque;
      io->end = rte_get_timer_cycles();
      io->status = status;
      io->transfered_length -= ((ctrl & STS_LEN) - status.transfered_bytes);
      if (ch->tx)
      {
        memcpy(&io->sideband, &head->bd->tx.ctrl_sideband,
               sizeof(struct sideband));
      }
      else
      {
        memcpy(&io->sideband, &head->bd->rx.sideband_status,
               sizeof(struct sideband));
        // DMA_DBG("TAIL ADDRESS %x,HEAD ADDRESS %x\n",
        // ch->regs->rx.taildesc_lsb, ch->regs->rx.curdesc_lsb); DMA_DBG("NOTICE
        // ERR CODE%x S2MM COMPLETED%d \n PKT DROP%d %d RECV COUNT%d\n",
        // ch->dev->s2mm->err, ch->dev->s2mm->ccr, ch->dev->s2mm->pktdrop,
        // ch->regs->rx.pktdrop_stat, ch->regs->rx.pktcount_stat);
      }
      head->opaque = NULL;
      head->request_end = false;

      /*
       * All descriptors from one submission share the same software IO.
       * Segmented TX deliberately places EOF on every data descriptor, so
       * EOF cannot identify the end of the software request.  Return the IO
       * only after its final descriptor, just as multi-BD RX already does.
       */
      if (request_end)
      {
        io->status.transfered_bytes = io->transfered_length;
        return opaque;
      }
    }

    if (channel_empty(ch))
    {
      // Buffer empty
      return NULL;
    }
    rte_wmb();

    head = head->next;
    status = ch->tx ? head->bd->tx.status : head->bd->rx.status;
    rte_rmb();
  }
 
  return NULL;
}

void rte_axi_dma_channel_get_stat(struct rte_axi_dma_channel *ch,
                                  struct rte_axi_dma_channel_stat *stat)
{
  if (ch->tx)
  {
    stat->pkt_dropped = 0;
    stat->pkt_processed = ch->regs->tx.pktcount_stat;
  }
  else
  {
    stat->pkt_dropped = ch->regs->rx.pktdrop_stat;
    stat->pkt_processed = ch->regs->rx.pktcount_stat;
  }
}

void rte_axi_dma_stop_channel(struct rte_axi_dma_channel *ch)
{
  if(ch->tx){
    rte_io_wmb();
    ch->dev->mm2s->chen &= ~(uint32_t)(BIT(ch->id));
    return;
  }else{
    rte_io_wmb();
    ch->dev->s2mm->chen &= ~(uint32_t)(BIT(ch->id));
    return;
  }
  {
    if (ch->tx)
    {
      if(ch->regs->tx.curdesc_lsb!=ch->regs->tx.taildesc_lsb||ch->count!=0){
        rte_io_rmb();
        uint32_t val = (ch->regs)->tx.cr;
        val |= BIT(2);
        val &= ~BIT(0);
        rte_io_wmb();
        (ch->regs)->tx.cr = 0x10004;
        rte_io_wmb();
        while (!(ch->regs->tx.sr & BIT(2)))
        {
          rte_rmb();
        }
        (ch->regs)->tx.cr = 0x10000;
    }
    }
    else
    {
      if(ch->regs->rx.curdesc_lsb!=ch->regs->rx.taildesc_lsb||ch->count!=0){
        rte_io_rmb();
        rte_wmb();
        //ch->dev->s2mm->chen &= ~(uint32_t)(BIT(ch->id));
        uint32_t val = (ch->regs)->rx.cr;
        val |= BIT(2);
        val &= ~BIT(0);
        rte_io_wmb();
        rte_wmb();
        (ch->regs)->rx.cr = 0x100104;
        rte_io_wmb();
        rte_wmb();
        
        while (!(ch->regs->rx.sr & BIT(2)))
        {
          rte_rmb();
        }
        (ch->regs)->tx.cr = 0x100100;
        //ch->dev->s2mm->chen |= (uint32_t)(BIT(ch->id));
      }
    }
  }
}

void rte_axi_dma_enable_channel(volatile struct rte_axi_dma_channel *ch)
{
  DMA_DBG("Enable one compute channel %s,id%d\n", ch->tx ? "TX" : "RX",ch->id);
  rte_io_rmb();
  DMA_DBG("RESET CH %s\n",ch->tx?"TX":"RX");
  // refresh mcdma
  if(ch->bd_chain.head==ch->bd_chain.tail&&ch->count==0){
    //if no event in mcdma don not operate
    if((ch->tx&&ch->regs->tx.curdesc_lsb==ch->regs->tx.taildesc_lsb)||((!ch->tx)&&ch->regs->rx.curdesc_lsb==ch->regs->rx.taildesc_lsb)){
      if(ch->tx){
        rte_io_wmb();
        ch->dev->mm2s->chen |= BIT(ch->id);
      }else{
        rte_io_wmb();
        ch->dev->s2mm->chen |= BIT(ch->id);
      }
      DMA_DBG("NO NEED TO RESET!\n");
      return;
    }
  }
  ch->count = 0;
  if (ch->tx)
  {
    rte_io_rmb();
   {
      rte_io_wmb();
      uint32_t val = (ch->regs)->tx.cr;
      val |= BIT(2);
      val &= ~BIT(0);
      rte_write32(0x10004,&((ch->regs)->tx.cr));
      DMA_DBG("CR SET%lx\n",(ch->regs)->tx.cr);
      ch->dev->mm2s->chen |= BIT(ch->id);
      rte_io_wmb();
      while(!(rte_read32(&(ch->regs->tx.sr))&BIT(2)));
      DMA_DBG("SR GET%lx\n",(ch->regs)->tx.sr);
      rte_io_wmb();
      rte_write32(0x10000,&((ch->regs)->tx.cr));
      DMA_DBG("CR SET%lx\n",(ch->regs)->tx.cr);
      rte_io_wmb();

    }
  }
  else
  {
    {
      rte_io_rmb();
      rte_io_wmb();
      uint32_t val = (ch->regs)->rx.cr;
      val |= BIT(2);
      val &= ~BIT(0);
      rte_io_wmb(); 
      (ch->regs)->rx.cr = 0x10104;
      rte_io_wmb();
      
      while (!(ch->regs->rx.sr & BIT(2)))
      {
        rte_io_rmb();
      }
      ch->dev->s2mm->chen |= BIT(ch->id);
      rte_io_wmb();
      (ch->regs)->tx.cr = 0x10100;
      rte_io_wmb();
      
    }
  }
  rte_io_rmb();
  int err = ch->tx ? ch->dev->mm2s->err : ch->dev->s2mm->err;

  if (err)
  {
    RTE_LOG(CRIT, EAL, "%s error: %X id%d\n", (ch->tx ? "MM2S" : "S2MM"), err,
            ch->id);
  }
  ch->bd_chain.head = ch->bd_chain.desc_arr;
  ch->bd_chain.tail = ch->bd_chain.desc_arr;
  ch->count = 0;
  rte_wmb();
  if (ch->tx)
  {
    rte_io_wmb();
    /* Program CD and TD register */
    //virt2phys_64((uintptr_t)ch->bd_chain.head->bd,
    //             &ch->regs->tx.curdesc_lsb,
    //             &ch->regs->tx.curdesc_msb);
    rte_write32(ch->bd_chain.head->bd_phys_lsb,&(ch->regs->tx.curdesc_lsb));
    rte_io_wmb();
    rte_write32(ch->bd_chain.head->bd_phys_msb,&(ch->regs->tx.curdesc_msb));
    DMA_DBG("LSB SET%lx\n",(ch->regs)->tx.curdesc_lsb);
    /* Start channel */
    rte_io_wmb();
    //ch->regs->tx.cr |= BIT(0); // CH.RS
    rte_write32(0x10001,&(ch->regs->tx.cr));
    DMA_DBG("CR SET%lx\n",(ch->regs)->tx.cr);
    //ch->regs->tx.cr = 0x10001;
    rte_io_wmb();
    ch->bd_chain.desc_arr->bd->tx.status.completed = 0;
    //assert error
    DMA_DBG("ASSERT ERROR\n");
    ch->bd_chain.desc_arr->bd->tx.ctrl = 0;
    ch->bd_chain.desc_arr->bd->tx.ctrl |= BIT(31); // TX SoF
    ch->bd_chain.desc_arr->bd->tx.ctrl |= BIT(30); // TX EoF
    rte_io_wmb();
    rte_write32(ch->bd_chain.desc_arr->bd_phys_lsb,&(ch->regs->tx.taildesc_lsb));
    rte_write32(ch->bd_chain.desc_arr->bd_phys_msb,&(ch->regs->tx.taildesc_msb));
    rte_io_wmb();
    uint32_t val = (ch->regs)->tx.cr;
    val |= BIT(2);
    val &= ~BIT(0);
    rte_write32(0x10004,&((ch->regs)->tx.cr));
    DMA_DBG("CR SET%lx\n",(ch->regs)->tx.cr);
    ch->dev->mm2s->chen |= BIT(ch->id);
    rte_io_wmb();
    while(!(rte_read32(&(ch->regs->tx.sr))&BIT(2)));
    DMA_DBG("SR GET%lx\n",(ch->regs)->tx.sr);
    rte_io_wmb();
    rte_write32(0x10000,&((ch->regs)->tx.cr));
    DMA_DBG("CR SET%lx\n",(ch->regs)->tx.cr);
    rte_io_wmb();
    rte_io_wmb();
    rte_write32(ch->bd_chain.head->bd_phys_lsb,&(ch->regs->tx.curdesc_lsb));
    rte_io_wmb();
    rte_write32(ch->bd_chain.head->bd_phys_msb,&(ch->regs->tx.curdesc_msb));
    DMA_DBG("LSB SET%lx\n",(ch->regs)->tx.curdesc_lsb);
    /* Start channel */
    rte_io_wmb();
    rte_write32(0x10001,&(ch->regs->tx.cr));
    DMA_DBG("CR SET%lx\n",(ch->regs)->tx.cr);
    rte_io_wmb();
    ch->bd_chain.desc_arr->bd->tx.status.completed = 0;    
    /* Start Device */
    ch->dev->mm2s->ccr |= BIT(0);
  }
  else
  {
    /* Program CD and TD register */
    rte_io_wmb();
    virt2phys_64((uintptr_t)ch->bd_chain.head->bd,
                 &ch->regs->rx.curdesc_lsb,
                 &ch->regs->rx.curdesc_msb);
    /* Start channel */
    rte_io_wmb();
    ch->regs->rx.cr = 0x10101; // CH.RS
    rte_io_wmb();
    /* Start Device */
    ch->dev->s2mm->ccr |= BIT(0);
    rte_io_wmb();
      //assert error
      /*
    DMA_DBG("ASSERT ERROR\n");
    ch->bd_chain.desc_arr->bd->rx.ctrl = 0 & (BIT(26) - 1);
    ch->bd_chain.desc_arr->bd->rx.status.completed = 0;
    ch->bd_chain.desc_arr->bd->rx.status.rxeof = 0;
    ch->bd_chain.desc_arr->bd->rx.status.rxsof = 0;
    rte_io_wmb();
    rte_write32(ch->bd_chain.desc_arr->bd_phys_lsb,&(ch->regs->rx.taildesc_lsb));
    rte_write32(ch->bd_chain.desc_arr->bd_phys_msb,&(ch->regs->rx.taildesc_msb));
    rte_io_rmb();
    rte_io_wmb();
    uint32_t val = (ch->regs)->rx.cr;
    val |= BIT(2);
    val &= ~BIT(0);
    rte_io_wmb(); 
    (ch->regs)->rx.sr = 0x68;
    (ch->regs)->rx.cr = 0x10104;
    rte_io_wmb();

    while (!(ch->regs->rx.sr & BIT(2)))
    {
      rte_io_rmb();
    }
    rte_io_wmb();
    (ch->regs)->rx.cr = 0x10100;
    rte_io_wmb();
    rte_io_wmb();
    virt2phys_64((uintptr_t)ch->bd_chain.head->bd,
                 &ch->regs->rx.curdesc_lsb,
                 &ch->regs->rx.curdesc_msb);*/
    /* Start channel */
    rte_io_wmb();
    ch->regs->rx.cr |= BIT(0); // CH.RS
    
  }
  return;
}

/* Expects physical addresses in iovs */
int rte_axi_dma_send_seg(struct rte_axi_dma_channel *ch,
                         struct rte_axi_dma_iovec *iovs, int iovcnt, void *ctx,
                         bool last_data)
{
  struct rte_axi_dma_desc_ring *dev_buf = &ch->bd_chain;
  struct rte_axi_dma_desc *tail = dev_buf->tail;
  union bd *tail_bd;
  struct dummy_dma_io *io = ctx;
  DMA_DBG("IOVCNT SEND SEG%d\n",iovcnt);
  for (int i = 0; i < iovcnt; i++)
  {
    uint64_t buf = iovs[i].paddr;
    size_t len = iovs[i].iov_len;
    tail_bd = tail->bd;

    if (len >= BIT(26))
    {
      RTE_LOG(CRIT, EAL, "Buffer is too large at %lu\n", len);
      return -1;
    }

    if (ch->tx)
    {
      DMA_DBG("[%u %s] %u: Add buffer to bd %p: %lX %lu, first 64b: %lX tail phy address%lx cur phy address dest%d\n",
              ch->id, dirs[ch->tx], io->sideband.tid, tail_bd, buf, len,
              *(uint64_t *)iovs[i].iov_base,tail->bd_phys_lsb,ch->regs->tx.curdesc_lsb,io->sideband.tdest);
      tail_bd->tx.buf_addr = buf;
      tail_bd->tx.buf_addr_msb = buf >> 32;

      tail_bd->tx.ctrl = len & (BIT(26) - 1);

      tail_bd->tx.ctrl |= BIT(31); // TX SoF
      tail_bd->tx.ctrl |= BIT(30); // TX EoF
      tail_bd->tx.ctrl_sideband.tid = io->sideband.tid;
      tail_bd->tx.ctrl_sideband.tuser = io->sideband.tuser;
      tail_bd->tx.status.completed = 0; // Not completed

    }

    tail->opaque = ctx;
    tail->request_end = (i == iovcnt - 1 && !last_data);
    if (unlikely(i == iovcnt - 1 && !last_data))
    {
      {
        rte_io_wmb();
        virt2phys_64((uintptr_t)ch->bd_chain.tail->bd,
                     &ch->regs->tx.taildesc_lsb, &ch->regs->tx.taildesc_msb);
        rte_wmb();
      }
      DMA_DBG("1CHEN %x TAILDESC%x  CURDESC%x BD_PHYS_LSB%llx\n", ch->dev->s2mm->chen,
        ch->regs->rx.taildesc_lsb, ch->regs->rx.curdesc_lsb, rte_mem_virt2iova(ch->bd_chain.head->bd));
    }
    tail = tail->next;
    dev_buf->tail = tail;
    ch->count++;
  }
  io->start = rte_get_timer_cycles();
  if (last_data)
  {
    DMA_DBG("Send last data\n");
    io->transfered_length += 16;
    // 如果包含last_data，就需要发出一个空的数据包，代表整个数据的结束
    tail_bd = tail->bd;
    tail_bd->tx.buf_addr = iovs[0].paddr;
    tail_bd->tx.buf_addr_msb = iovs[0].paddr >> 32;
    tail_bd->tx.ctrl = 16 & (BIT(26) - 1);
    tail_bd->tx.ctrl |= BIT(31); // TX SoF
    tail_bd->tx.ctrl |= BIT(30); // TX EoF
    tail_bd->tx.ctrl_sideband.tid = io->sideband.tid;
    tail_bd->tx.ctrl_sideband.tuser = 0xff;
    tail_bd->tx.status.completed = 0; // Not completed
    tail->opaque = ctx;
    tail->request_end = true;

    rte_io_wmb();
    virt2phys_64((uintptr_t)ch->bd_chain.tail->bd, &ch->regs->tx.taildesc_lsb,
                 &ch->regs->tx.taildesc_msb);
    rte_wmb();
    tail = tail->next;
    dev_buf->tail = tail;
    ch->count++;
  }

  /* Move tail to next position */
  // DMA_DBG("%s: Tail is %llX, tail->opaque is %llX, tail->next is %llX\n",
  // ch->dev->name, tail, tail->opaque, tail->next);
  return 0;
}
