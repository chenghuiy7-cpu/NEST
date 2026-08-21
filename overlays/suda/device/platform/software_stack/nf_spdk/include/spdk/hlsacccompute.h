#ifndef _HLSACCCOMPUTE
#define _HLSACCCOMPUTE
#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/axi_dma.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bit_array.h"
#include "stdatomic.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_HLSACCCOMPUTE_NUM_RINGS 4
#define SPDK_HLSACCCOMPUTE_SQ_SIZE 32
#define SPDK_HLSACCCOMPUTE_CQ_SIZE 32
#define SPDK_HLSACCCOMPUTE_BAR_PHYS_ADDR 0xB0070000
#define SPDK_HLSACCCOMPUTE_SQ_HEADER_OFFSET 0x4010
#define SPDK_HLSACCCOMPUTE_SQ_TAILER_OFFSET 0x4020
#define SPDK_HLSACCCOMPUTE_CQ_HEADER_OFFSET 0x4028
#define SPDK_HLSACCCOMPUTE_CQ_TAILER_OFFSET 0x4030
#define SPDK_HLSACCCOMPUTE_SQ_OFFSET 0x2000
#define SPDK_HLSACCCOMPUTE_CQ_OFFSET 0x1000
#define SPDK_HLSACCCOMPUTE_APP_AP_DONE_OFFSET 0x80
#define SPDK_HLSACCCOMPUTE_CONTROL_RESETN_OFFSET 0x88

#define TDEST_WIDTH 4
#define TDATA_WIDTH 512
#define TUSER_WIDTH 0
#define TID_WIDTH 4
#define FIFO_SIZE 4096 // 4KB

#define SPDK_HLS_ACCCOMPUTE_TIME_INTERVAL 20

#define SPDK_HLSACCCOMPUTE_PRIORITY_DEGRED 3

#define SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP 8

#define SPDK_HLSACCCOMPUTE_MAX_REQ_OPERATOR_SUM 5

#define SPDK_HLSACCCOMPUTE_MAX_OPERATORS_SUPPORT 64

#define SPDK_HLSACCCOMPUTE_SLICE_WINDOWS 8

#define FAIR_QUEUE_FIRST_LEVEL_NUM 4

#define FAIR_QUEUE_SECOND_LEVEL_NUM 8

#define OPCONFIG_PATH "/root/lyh_nf_spdk/opconfig.json"

#define REQUEST_MAX 16

#define SCHED_RR_TIME_SLICE 10


enum hlsacc_program_type{
    operator_library=0xc0,
    fusion_program=0xc1,
    xilinx_fpga_only_program=0xc2,
    xilinx_soc_only_progran=0xc3,
};

/// @brief 加速算子状态机的状态
enum AccFsmState
{
    IDLE,                           //部署了，但是没有工作
    UNIMPLEMENTED,                  //没部署，且没有工作
    WORKING,                        //部署在板卡上，且正在工作
    LOCKED                          //部署在板卡上，且没有在工作，但是不能被申请占用
};

enum AssSchedRetState
{
    FAILED,
    SUCCESS,
    UNDEFINED_FAILED,
    SUCCESS_STAGE1,
    OPERATOR_REAL_FIN
};

enum AssSchedCmdOpcode{
    FORCE_FREE_OPS=0,   //强行释放一组算子
    APPLY_OPS=1,        //申请一组算子
    QUERY_OPS=2,        //查询一组算子
    SUSPEND_OPS=3,      //挂起一组正在工作的算子
    RESUME_OPS=4,       //恢复一组挂起算子
};

/// @brief 描述发送给控制器的命令结构
/// @attention 计划一个SQE占据32B
union AssSchedCmd{
    struct{
        unsigned char cid:8;                       //1B
        unsigned char opc:8;              //1B
        unsigned char ops_num:8;                   //1B
        unsigned char cmd_length:8;                //1B
        unsigned char resvd[4];                    //4B
    }header;

    struct{
        unsigned long long context_address;
    }apply_ops_payload1;

    struct{
        unsigned short connections_num:16;
        struct{
            unsigned char from:8;
            unsigned char to:8;
        }connections[3];
    }apply_ops_payload2;
    struct{
        struct{                                   
            unsigned char from:8;
            unsigned char to:8;
        }connections[4];
    }apply_ops_payload3;

    struct{
        unsigned char op_lists[8];          
    }generic_ops_payload;

    
    u_int64_t raw_data;

};

static_assert(sizeof(union AssSchedCmd)==8);

union AssSchedRet{
    struct{
        unsigned int cid:8;
        unsigned int ops_num:8;//ops_num only used for query
        enum AssSchedRetState state:16;  
    }header;

    struct{
       struct{
        unsigned char op:8;
        unsigned char fsm_state:8;
       }state_pair[2];
    }query_payload;

};

/// @brief 描述一个Packet的大小以及TID位的数据
struct StreamBufElmDescriptor{
	unsigned char length:8;
	unsigned char id:8;
};


/// @brief 每个逻辑调度单元所对应的上下文，存储在内存当中
#pragma pack(1)
struct AccContext{
    //FIFO Descriptor
	struct{
        struct StreamBufElmDescriptor descriptor[28]; //56B
        unsigned long long descriptor_num; //8B
        struct{//无用数据，现在已经废弃
            	struct StreamBufElmDescriptor descriptor[28]; //56B
                unsigned long long descriptor_num; //8B
        }rubbish;
        unsigned long long available_buf_num; //8B Optimize Performance
        unsigned long long stream_buf_add_lists[7]; //56B     
    }privated_data;
    //Static Variable Data
    unsigned char static_data[2048];  //Only 2048B useful
    //Temp Use Data
    //临时被控制器使用的区域 
};
#pragma pack()


struct spdk_hlsacccompute_opcontext{
    struct AccContext context;
    uint64_t context_phy;
    TAILQ_ENTRY(spdk_hlsacccompute_opcontext) link;
    
};


struct StreamBuffer{
    unsigned char buffer[FIFO_SIZE];
};


struct spdk_hlsacccompute_bar
{
    volatile uint32_t *ppair_sqtailer;
    volatile uint32_t *ppair_cqtailer;
    volatile uint32_t *ppair_sqheader;
    volatile uint32_t *ppair_cqheader;
    volatile union AssSchedCmd *ppair_sq;
    volatile union AssSchedRet *ppair_cq;
    volatile uint32_t *user_resetn;
};

enum OperatorType
{
    PREEMPT_STREAM_OPERATOR,                                       // 支持抢占的流式算子（默认）
    SHARED_STREAM_OPERATOR,                                        // 无需抢占，即可实现共享的流式算子
    UNPREEMPT_STREAM_OPERATOR,                                     // 不可抢占，必须等待结束才能被调度的流式算子
    PREEMPT_NORMAL_OPERATOR,                                       // 支持抢占的非流式算子
    SHARED_NORMAL_OPERATOR,                                        // 无需抢占，即可实现共享的非流式算子
    UNPREEMPT_NORMAL_OPERATOR                                      // 不可抢占，必须等待结束才能被调度的非流式算子
};
struct spdk_hlsacccompute_request;
struct spdk_hlsacccompute_virtual_object;
struct spdk_hlsacccompute_opconfig;
struct spdk_hlsacccompute_op_elm;
struct spdk_hlsacccompute_op_inform
{
    uint32_t esti_executed_times;                           // us
    uint32_t worse_executed_times;                          // us
    uint32_t bram_size;
    uint32_t input_port_num;
    uint32_t output_port_num;
    char* operator_type_name;                               // 用于区分算子类别
    uint8_t operator_type_id;                               // 算子类型id
    
    enum OperatorType types;                                // 范围更广的算子类别
    TAILQ_HEAD(in,spdk_hlsacccompute_op_elm) elements;
    //uint8_t operator_dev_id;                                // 算子设备物理id
    uint8_t operator_slot_id;                               // 算子实际被部署在板卡上的id
    
};

struct spdk_hlsacccompute_op_elm
{
    enum AccFsmState state;
    struct spdk_hlsacccompute_request* request;             // 与算子相关联的请求
    struct spdk_hlsacccompute_opconfig* opconfig;
    uint8_t operator_dev_id;                                // 算子设备物理id
    uint8_t operator_slot_id;                               // 算子实际被部署在板卡上的id
    TAILQ_ENTRY(spdk_hlsacccompute_op_elm) link;
};

// 算子信息描述
struct spdk_hlsacccompute_opconfig
{
    uint32_t esti_executed_times;                           // us
    uint32_t worse_executed_times;                          // us
    uint32_t bram_size;
    uint32_t input_port_num;
    uint32_t output_port_num;
    char* operator_type_name;                               // 用于区分算子类别
    uint8_t operator_type_id;                               // 算子类型id
    
    enum OperatorType types;                                // 范围更广的算子类别
    TAILQ_HEAD(_,spdk_hlsacccompute_op_elm) elements;
    
};


//收到cqe之后的回调函数
typedef void (*spdk_hlsacccompute_cmd_cb_fn)(union AssSchedCmd *sqe, union AssSchedRet *cqe, void *cb_arg);

// 每个请求request对应的回调函数
typedef void (*spdk_hlsacccompute_req_cb_fn)(struct spdk_hlsacccompute_request *request,void *cb_arg);

static struct request_second_level_fair_queue{
    TAILQ_HEAD(,spdk_hlsacccompute_request) requests;
    uint32_t remaining_quantum;
};

static struct request_first_level_fair_queue{
    struct request_second_level_fair_queue que[FAIR_QUEUE_SECOND_LEVEL_NUM];
    uint32_t remaining_quantum;
};

struct spdk_hlsacccompute_channel;
struct spdk_hlsacccompute_program;

enum HLSDEV_WORKSTATE{
    HLSDEV_IDLE,
    HLSDEV_RUN_REQUEST,
    HLSDEV_SCHEDING,
    HLSDEV_ERROR,
};


struct spdk_hlsacccompute_sw_resources{
    struct spdk_thread* thread;
    int thread_id;
    TAILQ_ENTRY(spdk_hlsacccompute_sw_resources) link;
};

struct spdk_hlsacccompute_dev
{
    struct spdk_hlsacccompute_bar *bar;                                                         //和硬件交互的寄存器空间
    uint32_t inner_ppair_cqtailer;                                                              //spdk内部监控cq的尾指针
                                                                                                //内部使用的cq指针，
                                                                                                //当内部cq指针和控制器的cqtailer寄存器不一样，
                                                                                                //代表有新数据
    struct spdk_hlsacccompute_opconfig* opconfigs[SPDK_HLSACCCOMPUTE_MAX_OPERATORS_SUPPORT];
    uint32_t register_op_type_num;                                                              //总共注册了多少种算子类型
    
    spdk_hlsacccompute_cmd_cb_fn cmd_cb_fns[SPDK_HLSACCCOMPUTE_SQ_SIZE];                        //回调函数列表
    void *cmd_cb_args[SPDK_HLSACCCOMPUTE_SQ_SIZE];

    TAILQ_HEAD(, spdk_hlsacccompute_request) waiting_queue[SPDK_HLSACCCOMPUTE_SLICE_WINDOWS];    //等待队列
    int waiting_queue_ptr;                                                                       //每隔一个时间片移动一个指针
    int waiting_req_num;                                                                         //监控现在正在等待的请求数目
    TAILQ_HEAD(, spdk_hlsacccompute_request) working_queue;                                      //工作队列
    TAILQ_HEAD(, spdk_hlsacccompute_request) suspend_queue;                                      //发送暂停命令，但是还未确定暂停成功的队列    
    TAILQ_HEAD(, spdk_hlsacccompute_request) request_pool;
    TAILQ_HEAD(, spdk_hlsacccompute_channel) tx_channel_pool;
    TAILQ_HEAD(, spdk_hlsacccompute_channel) rx_channel_pool;
    TAILQ_HEAD(, spdk_hlsacccompute_virtual_object) vo_pool;
    TAILQ_HEAD(, spdk_hlsacccompute_opcontext) opcontext_pool;
    TAILQ_HEAD(, spdk_hlsacccompute_sw_resources) sw_resources_pool;
    struct spdk_mempool* mgnt_mempool;
    struct spdk_mempool* request_poolv2;
    struct spdk_mempool* opcontext_poolv2;
    struct spdk_hlsacccompute_program* program_list[64];                                         //程序列表，当前设置为固定大小
    struct request_first_level_fair_queue fair_scheduler_queue[FAIR_QUEUE_FIRST_LEVEL_NUM];     //Prepare for QoS
    uint32_t request_pool_ele_num;
    void* dev_mem_table;
    //TODO Fix it in the future
    atomic_uint next_mem_id;                                                                             //May Has Performance Problem!

    //poller
    struct spdk_poller* poller;
    struct spdk_cpuset* sw_cpuset;
    struct spdk_thread* compute_thread;
    enum HLSDEV_WORKSTATE dev_state;
    enum{
        SCHED_FCFS,
        SCHED_BASIC_PRIORITY_PREEMPT,
        SCHED_BASIC_RR,
        SCHED_BASIC_SOFTWARE_COCACULATE
    }schedule_strategy;
    
};

enum SPDK_HLSACCCOMPUTE_REQ_STATUS
{
    ACC_REQ_WAITING,                                        // 在等待调度队列中，等待被调度

    ACC_REQ_PREEMPTING,                                     //已经被调度，
                                                            //但是需要它需要的算子正在被其他请求占用，
                                                            //因此需要暂停其他请求

    ACC_REQ_APPLYING,                                       //已经被调度，需要申请一些算子

    ACC_REQ_EXECUTING,                                      // 已经被调度，正在被执行，即处在工作队列

    ACC_REQ_FINISHED,                                       // 调度结束

    ACC_REQ_ERROR,                                          // 存在错误，需要进行错误处理

    ACC_REQ_WAITING_PAUSE,                                   //已经发起暂停请求，等待收到暂停信号

    ACC_REQ_WAITING_CHANNEL ,                                //等待sqe执行成功，再申请sqe

    ACC_REQ_IDLE,
};

/**
 * \brief 跟踪一次调度请求所需的关键信息 
*/
struct spdk_hlsacccompute_task_tracker
{
    uint64_t has_executed_time;                             //当前请求执行了多久
    uint64_t responding_time;                               //响应时间跟踪
    enum SPDK_HLSACCCOMPUTE_REQ_STATUS status;              //当前请求所处的状态
    uint16_t outstanding_command_num;                       //当前还没得到返回的命令数量，每收到一个CQE，就会减少一个，到0的时候代表执行完毕
    uint16_t tokens;
    short waiting_preempt_req;                           //在正式运行这个请求前，还在抢占多少个算子
   
};

typedef int  (*data_in_write_func_t)(unsigned long long* buffer, unsigned int size, unsigned int port_id,int thread_id);
typedef int  (*data_out_read_func_t)(unsigned long long* buffer, unsigned int size, unsigned int port_id,int thread_id);
typedef void (*context_write_func_t)(unsigned long long* data, int size,unsigned int op_id,int thread_id);
typedef int  (*run_func_t)(int thread_id);
typedef int  (*data_last_func_t)(unsigned int port_id,int thread_id);
typedef int  (*get_data_size_func_t)(unsigned int port_id,int thread_id);


/**
 * \brief 用于描述计算程序的结构
 */
struct spdk_hlsacccompute_program{
    union AssSchedCmd applyops[22];                                      // 申请算子的命令集合（避免命令再处理）
    union AssSchedCmd pauseops[3];                                       // 暂停算子的命令集合（避免命令再处理）
    union AssSchedCmd freeops[3];                                        // 释放算子的命令集合（避免命令再处理）
    uint32_t apply_ops_size;         
    uint32_t program_id;
    uint8_t apply_operators_id_map[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP]; // 算子逻辑号到算子类型的映射
    uint8_t input_channel_destination[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
    uint32_t esti_executed_time;                                         // 预估在不抢占的情况下，需要多久能够执行完成
    uint32_t max_responded_time;                                         // 从发出请求到处理完成需要等待的最长时间
    void* software_data;                                                 // 如果需要以软件的方式运行程序，程序会装载到这个数据中
    uint8_t apply_operators_num;                                         // 总共申请了多少个算子
    uint8_t input_channum;                                               // 需要多少通道作为数据源
    uint8_t output_channum;                                              // 需要多少输出通道作为数据源
    bool activated;
    uint32_t resvd;
    uint32_t software_data_size;
    uint32_t software_data_id;
    data_in_write_func_t data_in_write;
    data_out_read_func_t data_out_read;
    context_write_func_t context_write;
    run_func_t run;
    data_last_func_t last;
    get_data_size_func_t get_data_size;
};


/**
 * 虚拟对象，用于描述通道的数据源和数据目的
 */
struct spdk_hlsacccompute_virtual_object{
    bool is_mem;        //如果为true，代表是设备/主机内存，为false，代表是块设备文件
    //io vector
    //如果是设备内存，指向的是一段虚拟地址，如果不是设备内存，而是文件，则是文件描述表的指针
    uint64_t iov_base; /* Starting address */
    size_t iov_len; /* Length in bytes */
    size_t cur_used;
    TAILQ_ENTRY( spdk_hlsacccompute_virtual_object) link;
};



/**
 * \brief 描述一次用户请求 请求会被解析成多条命令
 */
struct spdk_hlsacccompute_request
{
    struct spdk_hlsacccompute_dev *dev;                     // 对应的加速设备
    struct spdk_hlsacccompute_program *program;             // 请求对应的计算程序
    uint8_t dynamic_id_map[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
    struct spdk_hlsacccompute_op_elm *op_elm[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP]; //申请到的算子记录
    uint16_t priority;                                      // 这个请求的初始优先级是多少
    uint16_t run_way;                                       // 运行方式 0 fusion 1 software 2 hardware
    struct spdk_hlsacccompute_task_tracker tracker;         // 跟踪请求的运行时信息
    TAILQ_ENTRY(spdk_hlsacccompute_request) link;           // 计算请求链表指针
    spdk_hlsacccompute_req_cb_fn req_cb_fns;                // 请求结束后要执行的回调函数
    void *req_cb_args;                                      // 请求结束后执行回调函数需要携带的参数
    struct spdk_hlsacccompute_channel* tx_channel[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];                               
    uint8_t tx_channel_num;
    struct spdk_hlsacccompute_channel* rx_channel[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
    uint8_t rx_channel_num;
    TAILQ_HEAD(, spdk_hlsacccompute_virtual_object) tx_vos[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
    TAILQ_HEAD(, spdk_hlsacccompute_virtual_object) rx_vos[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
    struct spdk_hlsacccompute_opcontext* acccontext[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];  //指针数组，包含了算子上下文
    union AssSchedCmd applyops[22];                                      // 申请算子的命令集合（避免命令再处理）
    union AssSchedCmd pauseops[3];                                       // 暂停算子的命令集合（避免命令再处理）
    union AssSchedCmd freeops[3];                                        // 释放算子的命令集合（避免命令再处理）
    uint32_t request_id;
    struct spdk_hlsacccompute_sw_resources* software_resources;
    struct spdk_hlsacccompute_request* next_request;                     // 等待请求执行完成后，执行下一个请求
    uint64_t result;
};

//struct volink {  struct spdk_hlsacccompute_virtual_object *tqh_first;  struct spdk_hlsacccompute_virtual_object * *tqh_last; };
/**
 * \brief 用于描述算子数据源和终点的抽象通道
 * 使用顺序 request部署->建立通道
 * 通道处理数据完成->触发request回调函数->回调函数判断request类型->如果是长服务就不释放通道，如果不是回调函数再触发channel_release函数
 */
struct spdk_hlsacccompute_channel{
    struct spdk_hlsacccompute_request* req;
    uint8_t channel_id;
    uint8_t dest_id;
    uint8_t virtual_channel_id;//动态映射，计算程序的virtual_id和实际的channel_id保持动态一一映射的关系
    bool is_tx;         //是否为发送/接收方向
    bool support_reuse; //是否支持重用，如果支持重用，同一计算任务可以选择重复利用此条通道而无需开辟新通道
    bool support_reset; //是否支持软重置，如果不支持重置通道，那这条通道属于不可抢占资源
    void* channel_info; //根据channel实际对应的硬件设备，需要获取的固定信息
    /**
     * \brief 通道发送的抽象函数
     * 包含通道指针，数据源，数据大小以及数据约束，约束表示通道每次发送以什么粒度进行
     * 譬如align=4096，则数据以一个页为粒度进行处理
     */
    int (*channel_send)(struct spdk_hlsacccompute_channel* ch);
    /**
    * \brief 通道接收的抽象函数
    * 包含通道指针，数据目的，数据大小以及数据约束，约束表示通道每次发送以什么粒度进行
    * 譬如align=4096，则数据以一个页为粒度进行处理
    */
    int (*channel_recv)(struct spdk_hlsacccompute_channel* ch);
    /**
    * \brief 通道发送/接收完成的回调函数
    * \param ch 通道的指针
    */
    int (*channel_done)(struct spdk_hlsacccompute_channel* ch);
    /**
     * \brief 暂停通道的处理
     */
    int (*channel_pause)(struct spdk_hlsacccompute_channel* ch);
    int (*channel_poller)(void* ctx);
    int (*channel_apply)(struct spdk_hlsacccompute_channel* ch);
    int (*channel_release)(struct spdk_hlsacccompute_channel* ch,bool context_save);
    TAILQ_ENTRY(spdk_hlsacccompute_channel) link;
};



int spdk_hlsacccompute_dev_init(struct spdk_hlsacccompute_dev *dev, uint64_t bar_phys_addr);

int spdk_hlsacccompute_qpair_create(struct spdk_hlsacccompute_dev *dev);

int spdk_hlsacccompute_poll_cq(struct spdk_hlsacccompute_dev *dev); 

int spdk_hlsacccompute_init_opconfig(struct spdk_hlsacccompute_dev *dev);

struct spdk_hlsacccompute_request *spdk_hlsacccompute_create_request(struct spdk_hlsacccompute_dev *dev,struct spdk_hlsacccompute_program* program);

int spdk_hlsacccompute_register_channel(struct spdk_hlsacccompute_dev* dev,struct spdk_hlsacccompute_channel* ch);

struct spdk_hlsacccompute_channel* spdk_hlsacccompute_apply_channel(struct spdk_hlsacccompute_dev* dev,bool is_tx);

int spdk_hlsacccompute_release_channel(struct spdk_hlsacccompute_dev* dev,struct spdk_hlsacccompute_channel* ch);

int spdk_hlsacccompute_run_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request* request, bool preempt);

int spdk_hlsacccompute_pause_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request *request, void* cb_args,spdk_hlsacccompute_cmd_cb_fn cb_fn);

int spdk_hlsacccompute_free_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request* request, bool sendsqe);

void spdk_hlsacccompute_schedule_request(struct spdk_hlsacccompute_dev *dev);

int spdk_hlsacccompute_poller(void* arg);

int spdk_hlsacccompute_devmem_malloc(struct spdk_hlsacccompute_dev* dev,
                                    size_t size,size_t align,void** vaddr,void** paddr);
int spdk_hlsacccompute_devmem_free(struct spdk_hlsacccompute_dev* dev,void* vaddr,int id);

int spdk_hlsacccompute_devmem_lookup(struct spdk_hlsacccompute_dev* dev,int id,void** vaddr,void** paddr);

int spdk_hlsacccompute_get_program_container(struct spdk_hlsacccompute_program** program);

int spdk_hlsacccompute_add_program(struct spdk_hlsacccompute_dev* dev,
                                  struct spdk_hlsacccompute_program* program);

int spdk_hlsacccompute_lookup_program(struct spdk_hlsacccompute_dev* dev,
                                  struct spdk_hlsacccompute_program** program,
                                  int pind);

int spdk_hlsacccompute_add_program_with_id(struct spdk_hlsacccompute_dev *dev,
                                   struct spdk_hlsacccompute_program *program,
                                   int id);

int spdk_hlsacccompute_del_program(struct spdk_hlsacccompute_dev* dev,
                                  int program_id);

int spdk_hlsacccompute_release_program_container(struct spdk_hlsacccompute_program** program);

int spdk_hlsacccompute_add_program_sw_data(struct spdk_hlsacccompute_program** program,
                                  int size);

void spdk_hlsacccompute_norm_cb_fn(struct spdk_hlsacccompute_request *request,void *cb_arg);

int spdk_hlsacccompute_dump_program_data(struct spdk_hlsacccompute_program* program);

#ifdef __cplusplus
}
#endif
#endif
