#include "spdk/env.h"
#include "spdk/hlsacccompute.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include <assert.h>
#include "spdk/hash_table.h"
#include "stdatomic.h"
#include "spdk/barrier.h"
#include "dlfcn.h"
#include "sys/mman.h"
#include "sys/stat.h"
#include "fcntl.h"
#include "unistd.h"
#include "spdk/blowfish.h"


int __spdk_hlsacccompute_run_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request *request, bool preempt);
static int mem_fd = -1;

void *spdk_hlsacccompute_addr_map(uint64_t addr)
{

    if (mem_fd == -1)
    {
        mem_fd = open("/dev/mem", O_RDWR);
        if (mem_fd < 0)
        {
            fprintf(stderr, "Failed to open /dev/mem\n");
            // 此处报错后没有处理异常，以后可能会出错
            assert(false);
        }
    }

    uint64_t addr_aligned = addr & (~(PAGE_SIZE - 1));
    uint64_t offset = addr - addr_aligned;
    void *addr_alloc = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, addr_aligned);
    if (addr_alloc == MAP_FAILED)
    {
        fprintf(stderr, "Failed to map BAR\n");
        close(mem_fd);
    }
    return (void *)((uint64_t)addr_alloc + offset);
}

int spdk_hlsacccompute_dev_init(struct spdk_hlsacccompute_dev *dev, uint64_t bar_phys_addr)
{
    dev->bar = (struct spdk_hlsacccompute_bar *)calloc(1, sizeof(struct spdk_hlsacccompute_bar));
    dev->bar->ppair_sqtailer = (uint32_t *)spdk_hlsacccompute_addr_map(bar_phys_addr + SPDK_HLSACCCOMPUTE_SQ_TAILER_OFFSET);
    dev->bar->ppair_sqheader = (uint32_t *)spdk_hlsacccompute_addr_map(bar_phys_addr + SPDK_HLSACCCOMPUTE_SQ_HEADER_OFFSET);
    dev->bar->ppair_cqtailer = (uint32_t *)spdk_hlsacccompute_addr_map(bar_phys_addr + SPDK_HLSACCCOMPUTE_CQ_TAILER_OFFSET);
    dev->bar->ppair_cqheader = (uint32_t *)spdk_hlsacccompute_addr_map(bar_phys_addr + SPDK_HLSACCCOMPUTE_CQ_HEADER_OFFSET);
    dev->bar->ppair_sq = (union AssSchedCmd *)spdk_hlsacccompute_addr_map(bar_phys_addr + SPDK_HLSACCCOMPUTE_SQ_OFFSET);
    dev->bar->ppair_cq = (union AssSchedRet *)spdk_hlsacccompute_addr_map(bar_phys_addr + SPDK_HLSACCCOMPUTE_CQ_OFFSET);
    dev->bar->user_resetn = (uint32_t *)spdk_hlsacccompute_addr_map(bar_phys_addr + SPDK_HLSACCCOMPUTE_CONTROL_RESETN_OFFSET);

    if (dev->bar->ppair_cqtailer == MAP_FAILED || dev->bar->ppair_cqheader == MAP_FAILED ||
        dev->bar->ppair_sqtailer == MAP_FAILED || dev->bar->ppair_sqheader == MAP_FAILED ||
        dev->bar->ppair_sq == MAP_FAILED || dev->bar->ppair_cq == MAP_FAILED)
    {
        fprintf(stderr, "Failed to map BAR\n");
        close(mem_fd);
        return -1;
    }
    for(int i=0;i<64;i++)
    {
        dev->program_list[i] = NULL;
    }

    *(dev->bar->user_resetn) = 0;
    *(dev->bar->user_resetn) = 1;
    *(dev->bar->ppair_sqtailer) = 0;
    *(dev->bar->ppair_cqheader) = 0;
    dev->inner_ppair_cqtailer = 0;
    dev->sw_cpuset = spdk_cpuset_alloc();
    if (dev->sw_cpuset != NULL)
    {
        spdk_cpuset_zero(dev->sw_cpuset);
        spdk_cpuset_set_cpu(dev->sw_cpuset, 1, true);
        spdk_cpuset_set_cpu(dev->sw_cpuset, 3, true);
    }
    else
    {
        SPDK_ERRLOG("FAILED TO CREATE SOFTWARE CPU SET\n");
        return -1;
    }
    spdk_hlsacccompute_qpair_create(dev);
    if (spdk_hlsacccompute_init_opconfig(dev) != 0) {
        SPDK_ERRLOG("Failed to initialize operator configuration\n");
        return -1;
    }
    dev->dev_mem_table = spdk_cuckoo_table_create(spdk_env_get_socket_id(spdk_env_get_current_core()), sizeof(struct spdk_hlsacccompute_virtual_object));
    dev->next_mem_id = 0x1;
    if (dev->dev_mem_table == NULL)
    {
        SPDK_ERRLOG("FAILED TO CREATE TABLE\n");
        return -1;
    }
   
    dev->dev_state = IDLE;
    /*
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr))
    {
        SPDK_ERRLOG("pthread_mutexattr_init() failed\n");
        return -1;
    }

    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE))
    {
        SPDK_ERRLOG("pthread_mutexattr_settype() failed\n");
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    if (pthread_mutex_init(&dev->req_lock, &attr))
    {
        SPDK_ERRLOG("pthread_mutex_init() failed\n");
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    pthread_mutexattr_destroy(&attr);*/

    return 0;
}

static void spdk_hlsacccompute_tx_channel_cmpl(struct spdk_axi_dma_io *io, int status)
{
    SPDK_DEBUGLOG(hlsacc,"NICE FINISH TX CHANNEL SENDING!!!!!\n");
}


int spdk_hlsacccompute_poll_cq(struct spdk_hlsacccompute_dev *dev);
int spdk_hlsacccompute_poller(void *arg)
{
    //SPDK_DEBUGLOG(hlsacc,"POLLING\n");
    static int last_time = 0;
    last_time++;
    struct spdk_hlsacccompute_dev *dev = (struct spdk_hlsacccompute_dev *)arg;
    spdk_hlsacccompute_poll_cq(dev);
    if (last_time == SCHED_RR_TIME_SLICE)
    {
        last_time = 0;
        //spdk_hlsacccompute_schedule_request(dev);
    }
    return 0;
}

int spdk_hlsacccompute_qpair_create(struct spdk_hlsacccompute_dev *dev)
{
    // 初始化请求需要调度的两个队列
    TAILQ_INIT(&dev->working_queue);
    TAILQ_INIT(&dev->request_pool);
    TAILQ_INIT(&dev->tx_channel_pool);
    TAILQ_INIT(&dev->rx_channel_pool);
    for (int i = 0; i < SPDK_HLSACCCOMPUTE_SLICE_WINDOWS; i++)
    {
        TAILQ_INIT(&(dev->waiting_queue[i]));
    }
    for (int i = 0; i < REQUEST_MAX; i++)
    {
        struct spdk_hlsacccompute_request *req = spdk_zmalloc(sizeof(struct spdk_hlsacccompute_request), 2, NULL, SPDK_ENV_SOCKET_ID_ANY,
                                                              SPDK_MALLOC_SHARE);
        TAILQ_INSERT_HEAD(&dev->request_pool, req, link);
    }
    for (int i = 0; i < SPDK_HLSACCCOMPUTE_MAX_OPERATORS_SUPPORT; i++)
    {
        (dev->opconfigs[i])=NULL;
    }
    // 初始化poll

    dev->poller = spdk_poller_register(spdk_hlsacccompute_poller, (void *)(dev), 0);
    dev->compute_thread = spdk_get_thread();
    // 初始化vo_pool
    TAILQ_INIT(&dev->vo_pool);
    struct spdk_hlsacccompute_virtual_object *ob = spdk_malloc(sizeof(struct spdk_hlsacccompute_virtual_object) * 8192, 2, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
    for (int i = 0; i < 8192; i++)
    {
        TAILQ_INSERT_TAIL(&dev->vo_pool, &(ob[i]), link);
    }
    // 初始化opcontext
    TAILQ_INIT(&dev->opcontext_pool);
    void *contexts = spdk_zmalloc(PAGE_SIZE * 4096, PAGE_SIZE*4096, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
    for (int i = 0; i < 2048; i++)
    {
        int size = 0;
        struct spdk_hlsacccompute_opcontext *context = contexts + PAGE_SIZE * i *2;
        context->context_phy = spdk_vtophys(context, &size);
        TAILQ_INSERT_TAIL(&dev->opcontext_pool, context, link);
    }
    dev->mgnt_mempool = spdk_mempool_create("hlsacc_mgnt",4096,PAGE_SIZE,SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,spdk_env_get_socket_id(spdk_env_get_current_core()));
    if(dev->mgnt_mempool==NULL){
        SPDK_ERRLOG("Failed to Allocate mgnt mempool\n");
        assert(false);
    }
    dev->request_poolv2 = spdk_mempool_create("hlsacc_req",4096,sizeof(struct spdk_hlsacccompute_request),SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,spdk_env_get_socket_id(spdk_env_get_current_core()));
    if(dev->request_poolv2==NULL){
        SPDK_ERRLOG("Failed to Allocate request mempool\n");
        assert(false);
    }
    dev->request_pool_ele_num = 4096;
    dev->opcontext_poolv2 = spdk_mempool_create("hlsacc_context",4096,4096*2,SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,spdk_env_get_socket_id(spdk_env_get_current_core()));
    if(dev->opcontext_poolv2==NULL){
        SPDK_ERRLOG("Failed to Allocate mgnt mempool\n");
        assert(false);
    }
    
    //Device Spec
    //TODO 让它能够可配置
    TAILQ_INIT(&(dev->sw_resources_pool));
    for(int i=0;i<2;i++){
        struct spdk_thread* sw_thread = spdk_thread_create(NULL,dev->sw_cpuset);
        struct spdk_hlsacccompute_sw_resources* resources = spdk_malloc(sizeof(struct spdk_hlsacccompute_sw_resources),2,NULL,SPDK_ENV_SOCKET_ID_ANY,SPDK_MALLOC_SHARE);
        resources->thread = sw_thread;
        resources->thread_id = i;
        TAILQ_INSERT_TAIL(&(dev->sw_resources_pool),resources,link);
    }

    return 0;
}

int spdk_hlsacccompute_poll_cq(struct spdk_hlsacccompute_dev *dev)
{
    uint32_t cqheader_incr_num;
    bool has_cqe;
    cqheader_incr_num = 0;
    has_cqe = false;
    while (dev->inner_ppair_cqtailer != *(dev->bar->ppair_cqtailer))
    {
        spdk_rmb();
        spdk_wmb();
        uint32_t ppair_cqheader = (*(dev->bar->ppair_cqheader) + cqheader_incr_num) % SPDK_HLSACCCOMPUTE_CQ_SIZE;
        spdk_wmb();
        int cid = dev->bar->ppair_cq[ppair_cqheader].header.cid;
        SPDK_DEBUGLOG(hlsacc,"Operator Finish, cid%u,status %u inner_cq_tailer%u cqheader%d\n", cid,
                       dev->bar->ppair_cq[ppair_cqheader].header.state, dev->inner_ppair_cqtailer,ppair_cqheader);
        cqheader_incr_num++;
        spdk_wmb();
        dev->inner_ppair_cqtailer = (dev->inner_ppair_cqtailer + 1) % SPDK_HLSACCCOMPUTE_SQ_SIZE;
        // 调用回调函数
        if (dev->cmd_cb_fns[cid] != NULL)
        {          
            if (cqheader_incr_num != 0)
            {
                spdk_wmb();
                (*(dev->bar->ppair_cqheader)) = (*(dev->bar->ppair_cqheader) + cqheader_incr_num) % SPDK_HLSACCCOMPUTE_SQ_SIZE;
                cqheader_incr_num = 0;
                spdk_rmb();
            }
            dev->cmd_cb_fns[cid](
                &dev->bar->ppair_sq[cid],
                &dev->bar->ppair_cq[ppair_cqheader],
                dev->cmd_cb_args[cid]);
        }else{
            //SPDK_ERRLOG("OH NO ! NO CALL BACK FUNCTION!\n");
        }
        has_cqe = true;
    }
    if (cqheader_incr_num != 0)
    {
        spdk_wmb();
        (*(dev->bar->ppair_cqheader)) = (*(dev->bar->ppair_cqheader) + cqheader_incr_num) % SPDK_HLSACCCOMPUTE_SQ_SIZE;
    }
    return has_cqe;
}

int spdk_hlsacccompute_poll_cq_wrapper(void *arg)
{
    struct spdk_hlsacccompute_dev *dev = (struct spdk_hlsacccompute_dev *)arg;
    return spdk_hlsacccompute_poll_cq(dev);
}

// JSON解析规则
static const struct spdk_json_object_decoder operator_decoders[] = {
    {"operator_type_id", offsetof(struct spdk_hlsacccompute_op_inform, operator_type_id), spdk_json_decode_uint32},
    {"operator_type_name", offsetof(struct spdk_hlsacccompute_op_inform, operator_type_name), spdk_json_decode_string},
    {"operator_inport_num", offsetof(struct spdk_hlsacccompute_op_inform, input_port_num), spdk_json_decode_uint32},
    {"operator_outport_num", offsetof(struct spdk_hlsacccompute_op_inform, output_port_num), spdk_json_decode_uint32},
    {"esti_executed_times", offsetof(struct spdk_hlsacccompute_op_inform, esti_executed_times), spdk_json_decode_uint32},
    {"worse_executed_times", offsetof(struct spdk_hlsacccompute_op_inform, worse_executed_times), spdk_json_decode_uint32},
    {"bram_size", offsetof(struct spdk_hlsacccompute_op_inform, bram_size), spdk_json_decode_uint32},
    {"slot_id",offsetof(struct spdk_hlsacccompute_op_inform,operator_slot_id),spdk_json_decode_uint32}
};

static int parse_operator(struct spdk_json_val *val, struct spdk_hlsacccompute_op_inform **op)
{
    struct spdk_hlsacccompute_op_inform *new_op;
    int rc = 0;
    //new_op = calloc(1, sizeof(*new_op));
    new_op = *op;
    if (new_op == NULL)
    {
        SPDK_ERRLOG("Failed to allocate operator info\n");
        return -ENOMEM;
    }
    rc = spdk_json_decode_object(val, operator_decoders,
                                 SPDK_COUNTOF(operator_decoders),
                                 new_op);
    //new_op->state = IDLE;
    new_op->types = PREEMPT_STREAM_OPERATOR;
    if (rc < 0)
    {
        SPDK_ERRLOG("Failed to decode operator: %s\n", spdk_strerror(-rc));
        free(new_op);
        return rc;
    }

    //*op = new_op;
    return 0;
}

static int parse_operators_array(struct spdk_json_val *values, struct spdk_hlsacccompute_dev *dev)
{
    struct spdk_json_val *operator_val;
    struct spdk_hlsacccompute_op_inform *op_inform;
    struct spdk_hlsacccompute_opconfig* op;
    size_t array_size, i;
    int rc;
    array_size = values->len;
    operator_val = values + 1;
    i = -1;
    while (operator_val != NULL)
    {
        op_inform = malloc(sizeof(struct spdk_hlsacccompute_op_inform));
        memset(op_inform,0,sizeof(struct spdk_hlsacccompute_op_inform));
        ++i;
        rc = parse_operator(operator_val, &op_inform);
        if (rc < 0)
        {
            SPDK_ERRLOG("Failed to parse operator %zu\n", i);
            return rc;
        }
        struct spdk_hlsacccompute_op_elm* elm = calloc(1,sizeof(*elm));
        if(elm==NULL){
            SPDK_ERRLOG("Failed to allocate elm!\n");
            assert(false);
        }
        SPDK_DEBUGLOG(hlsacc,"OPERATOR_TYPE_ID%d\n", op_inform->operator_type_id);
        int operator_type_id = op_inform->operator_type_id;
      
        elm->operator_slot_id = op_inform->operator_slot_id;
        //SPDK_NOTICELOG("GET SLOT ID%d GET TYPE ID%d\n",elm->operator_slot_id,operator_type_id);
        if (dev->opconfigs[operator_type_id]==NULL)
        {
            op = op_inform;
            /*
            op->esti_executed_times = op_inform->esti_executed_times;
            op->worse_executed_times = op_inform->worse_executed_times;
            op->bram_size = op_inform->bram_size;
            op->input_port_num = op_inform->input_port_num;
            op->output_port_num = op_inform->output_port_num;
            op->operator_type_id = op_inform->operator_type_id;
            op->operator_type_name = op_inform->operator_type_name;
            op->types = op_inform->types;*/
            dev->register_op_type_num++;
            dev->opconfigs[op->operator_type_id] = op;
            TAILQ_INIT(&(op->elements));
        }else{
            free(op_inform);
            op = dev->opconfigs[operator_type_id];
        }
        
        //TAILQ_INSERT_TAIL(&(dev->opconfigs[op->operator_type_id]), op, link);
        
     
        elm->request = NULL;
        elm->opconfig = op;
        elm->state = IDLE;
        TAILQ_INSERT_TAIL(&(dev->opconfigs[operator_type_id]->elements),elm,link);
        operator_val = spdk_json_next(operator_val);
    }

    return 0;
}

int spdk_hlsacccompute_init_opconfig(struct spdk_hlsacccompute_dev *dev)
{
    FILE *f;
    const char *config_path;
    char *buffer = NULL;
    long file_size;
    struct spdk_json_val *values = NULL;
    size_t values_cnt = 0;
    struct spdk_json_val *operators_val;
    int rc = -1;

    config_path = getenv("HLSACC_OPERATOR_CONFIG");
    if (config_path == NULL || config_path[0] == '\0') {
        config_path = "/root/software_stack/nf_spdk/config.json";
    }

    SPDK_NOTICELOG("Loading operator configuration from %s\n", config_path);
    f = fopen(config_path, "r");
    if (!f)
    {
        SPDK_ERRLOG("Failed to open operator config file %s\n", config_path);
        return -1;
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 分配缓冲区
    buffer = malloc(file_size + 1);
    if (!buffer)
    {
        SPDK_ERRLOG("Failed to allocate buffer\n");
        fclose(f);
        return -1;
    }

    // 读取文件内容
    if (fread(buffer, 1, file_size, f) != (size_t)file_size)
    {
        SPDK_ERRLOG("Failed to read config file\n");
        goto cleanup;
    }
    buffer[file_size] = '\0';
    fclose(f);
    f = NULL;

    // 解析JSON
    values_cnt = spdk_json_parse(buffer, file_size, NULL, 0, NULL, SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
    if (values_cnt == SPDK_JSON_PARSE_INCOMPLETE || values_cnt == SPDK_JSON_PARSE_INCOMPLETE)
    {
        SPDK_ERRLOG("Failed to parse JSON config (phase 1)\n");
        goto cleanup;
    }

    values = calloc(values_cnt, sizeof(struct spdk_json_val));
    if (values == NULL)
    {
        SPDK_ERRLOG("Failed to allocate values array\n");
        rc = -ENOMEM;
        goto cleanup;
    }

    rc = spdk_json_parse(buffer, file_size, values, values_cnt, NULL, SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
    if (rc < 0)
    {
        SPDK_ERRLOG("Failed to parse JSON config (phase 2)\n");
        goto cleanup;
    }

    // 查找operators数组
    operators_val = spdk_json_object_first(values);
    while (operators_val != NULL)
    {
        if (spdk_json_strequal(operators_val, "operators") &&
            (operators_val + 1)->type == SPDK_JSON_VAL_ARRAY_BEGIN)
        {
            rc = parse_operators_array(operators_val + 1, dev);
            break;
        }
        operators_val = spdk_json_next(operators_val);
    }

    if (rc < 0)
    {
        SPDK_ERRLOG("Failed to parse operators array\n");
        goto cleanup;
    }

    rc = 0;
    struct spdk_hlsacccompute_op_elm *op_elm;
    struct spdk_hlsacccompute_opconfig *op;
    for (int i = 0; i < dev->register_op_type_num; i++)
    {
        op = dev->opconfigs[i];
        TAILQ_FOREACH(op_elm, &(dev->opconfigs[i]->elements), link)
        {
            // 遍历所有Operators结果
            printf("Operator: index %d type=%s, in=%u, out=%u, time=%u, bram=%u, type_id=%d, slot_id=%d, dev_id=%d\n",
                           i,
                           op->operator_type_name,
                           op->input_port_num,
                           op->output_port_num,
                           op->esti_executed_times,
                           op->bram_size,
                           op->operator_type_id,
                           op_elm->operator_slot_id,
                           op_elm->operator_dev_id);
        }
    }

cleanup:
    if (f)
    {
        fclose(f);
    }
    free(buffer);
    free(values);
    return rc;
}

static inline void alloc_op_address(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_opcontext **context_array, struct spdk_hlsacccompute_program* program)
{
    // TODO，如果分配失败，这个地方不会进行错误处理，后续修复一下！！
    // TODO，如果opconfig已经被占走，这里是会出错的，需要修复一下！！
    int op_cnt = program->apply_operators_num;
    for (int i = 0; i < op_cnt; i++)
    {
        struct spdk_hlsacccompute_opcontext *context = TAILQ_FIRST(&(dev->opcontext_pool));
        TAILQ_REMOVE(&(dev->opcontext_pool), context, link);
        //struct spdk_hlsacccompute_opcontext* context = spdk_mempool_get(dev->opcontext_poolv2);
        assert(context!=NULL);
        if(context!=NULL&&(dev->opconfigs[program->apply_operators_id_map[i]])!=NULL){
            void* fifo_addr[8];
            //if(context->context_phy==NULL){
            context->context_phy = spdk_vtophys(context,NULL);
            memset(context,0,sizeof(struct AccContext));
            struct spdk_hlsacccompute_opconfig* config = dev->opconfigs[program->apply_operators_id_map[i]];//TAILQ_FIRST(&(dev->opconfigs[program->apply_operators_id_map[i]]));
            int ret = spdk_mempool_get_bulk(dev->mgnt_mempool,fifo_addr,config->input_port_num);
            if(ret == 0)
            for(int j = 0;j < config->input_port_num;j++){
                //It contains an big bug!!!
                context->context.privated_data.stream_buf_add_lists[j] = (unsigned long long)(fifo_addr[j]);
                //context->context.privated_data.available_buf_num++;
            }else{
                SPDK_ERRLOG("Failed to map operator\n");
                assert(false);
            }
        }else{
            SPDK_ERRLOG("Failed to map operator\n");
            assert(false);
        }
        (context_array)[i] = context;

    }
}

struct spdk_hlsacccompute_request *spdk_hlsacccompute_create_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_program *program)
{
    
    if (dev->request_pool_ele_num == 0)
    {
        /*
        for (int i = 0; i < REQUEST_MAX; i++)
        {
            struct spdk_hlsacccompute_request *req = spdk_zmalloc(sizeof(struct spdk_hlsacccompute_request), 2,
                                                                  NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
            req->dev = dev;
            ////SPDK_DEBUGLOG(hlsacc,"ALLOCATE CONTEXT ADDRESS%llx\n",req);
            for (int i = 0; i < 8; i++)
            {
                TAILQ_INIT(&(req->rx_vos[i]));
                TAILQ_INIT(&(req->tx_vos[i]));
            }
            TAILQ_INSERT_HEAD(&dev->request_pool, req, link);
        }
        dev->request_pool_ele_num += REQUEST_MAX;*/
        SPDK_ERRLOG("Failed to allocate request!\n");
        assert(false);
    }
    //struct spdk_hlsacccompute_request *req = TAILQ_FIRST(&(dev->request_pool));
    struct spdk_hlsacccompute_request* req = spdk_mempool_get(dev->request_poolv2);
    assert(req!=NULL);
    for (int i = 0; i < 8; i++)
    {
        TAILQ_INIT(&(req->rx_vos[i]));
        TAILQ_INIT(&(req->tx_vos[i]));
    }
    req->dev = dev;
    //TAILQ_REMOVE(&(dev->request_pool), req, link);
    spdk_wmb();
    ////SPDK_DEBUGLOG(hlsacc,"GET REQUEST ADDRESS%llx request ele num%d\n",req,dev->request_pool_ele_num);
    for (int i = 0; i < 8; i++)
    {
        req->acccontext[i] = NULL;
    }
    req->tracker.status = ACC_REQ_APPLYING;
    dev->request_pool_ele_num--;
    req->program = program;

   
    req->request_id = -1;
    assert(req->tx_channel[0]==NULL);
    assert(req->rx_channel[0]==NULL);
    req->result = 0;
    alloc_op_address(dev, req->acccontext,program);
    return req;
}

int spdk_hlsacccompute_register_channel(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_channel *ch)
{
    // 这里需要分出tx和rx的两个channel池！
    if (ch->is_tx)
        TAILQ_INSERT_TAIL(&dev->tx_channel_pool, ch, link);
    else
        TAILQ_INSERT_TAIL(&dev->rx_channel_pool, ch, link);
    return 0;
}

struct spdk_hlsacccompute_channel *spdk_hlsacccompute_apply_channel(struct spdk_hlsacccompute_dev *dev, bool is_tx)
{
    // 如果没有从通道池中分配出通道，返回NULL
    TAILQ_HEAD(, spdk_hlsacccompute_channel) * ch;
    if (is_tx)
    {
        ch = &dev->tx_channel_pool;
    }
    else
    {
        ch = &dev->rx_channel_pool;
    }
    if (TAILQ_EMPTY(ch))
    {
        return NULL;
    }
    else
    {
        struct spdk_hlsacccompute_channel *channel = TAILQ_FIRST(ch);
        TAILQ_REMOVE(ch, channel, link);
        channel->channel_apply(channel);
        return channel;
    }
}

int spdk_hlsacccompute_release_channel(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_channel *ch)
{
    if (ch == NULL)
    {
        SPDK_ERRLOG("Failed To Release Channel Because channel pointer is NULL!\n");
        return -1;
    }
    else
    {
        ch->channel_release(ch, false);
        if (ch->is_tx)
            TAILQ_INSERT_TAIL(&dev->tx_channel_pool, ch, link);
        else
            TAILQ_INSERT_TAIL(&dev->rx_channel_pool, ch, link);
        // for(int i=0;i<256;i++);

        return 0;
    }
}

static inline int execute_sqe(struct spdk_hlsacccompute_dev *dev,
                              union AssSchedCmd *cmd,
                              struct spdk_hlsacccompute_request *req,
                              void *cb_args,
                              spdk_hlsacccompute_cmd_cb_fn cb_fn)
{
    int tailer = *(dev->bar->ppair_sqtailer);
    int ret = tailer;
    int index = 0;
    switch (cmd->header.opc)
    {
    // 申请需要涉及至多7个算子，命令长度不定，但是其他命令只需要长度固定为2的命令
    case APPLY_OPS:
        index = (tailer + req->program->apply_ops_size) % SPDK_HLSACCCOMPUTE_SQ_SIZE;
        SPDK_DEBUGLOG(hlsacc,"EXECUTE SQE,SQ TAILER%d CID%d AFTER%d\n", *(dev->bar->ppair_sqtailer),tailer,index);
        SPDK_DEBUGLOG(hlsacc,"BRAM CONTEXT%llx\n",cmd[1].apply_ops_payload1.context_address);
       
        spdk_rmb();
        SPDK_DEBUGLOG(hlsacc,"CONNECT [%x]->[%x]\n",cmd[2].apply_ops_payload2.connections->from,cmd[2].apply_ops_payload2.connections->to);
        
        //SPDK_DEBUGLOG(hlsacc,"EXECUTE NEW TAILER%d\n",index);
        spdk_wmb();
        cmd[0].header.cid = tailer;
        SPDK_DEBUGLOG(hlsacc,"TAILER%d CID%d\n",tailer,cmd[0].header.cid);
        // memcpy((dev->bar->ppair_sq+tailer), (void *)cmd, req->program->apply_ops_size * sizeof(union AssSchedCmd));
        for (int i = 0; i < req->program->apply_ops_size; i++)
        {
            spdk_wmb();
            ////SPDK_DEBUGLOG(hlsacc,"CMD RAW DATA%llx\n",(cmd[i].raw_data));
            dev->bar->ppair_sq[(tailer++) % SPDK_HLSACCCOMPUTE_SQ_SIZE].raw_data = (cmd[i]).raw_data;
        }
        SPDK_DEBUGLOG(hlsacc,"CID%d\n",cmd[0].header.cid);
        dev->cmd_cb_args[cmd[0].header.cid] = (void *)cb_args;
        dev->cmd_cb_fns[cmd[0].header.cid] = cb_fn;
        spdk_wmb();
        //sleep(1);
        *(dev->bar->ppair_sqtailer) = index;

        break;
    case SUSPEND_OPS:
    case RESUME_OPS:
    case FORCE_FREE_OPS:

        index = (tailer + 2) % SPDK_HLSACCCOMPUTE_SQ_SIZE;
        spdk_rmb();
        SPDK_DEBUGLOG(hlsacc,"EXECUTE SQE,SQ TAILER%d CID%d AFTER%d\n", *(dev->bar->ppair_sqtailer),tailer,index);
        spdk_wmb();
        cmd[0].header.cid = tailer;
        for (int i = 0; i < 2; i++)
        {
            spdk_wmb();
            spdk_rmb();
            ////SPDK_DEBUGLOG(hlsacc,"CMD RAW DATA%llx\n",(cmd[i].raw_data));
            dev->bar->ppair_sq[(tailer++) % SPDK_HLSACCCOMPUTE_SQ_SIZE].raw_data = (cmd[i]).raw_data;
        }
        dev->cmd_cb_args[cmd[0].header.cid] = (void *)cb_args;
        dev->cmd_cb_fns[cmd[0].header.cid] = cb_fn;
        spdk_wmb();
        //sleep(1);
        *(dev->bar->ppair_sqtailer) = index;

        break;
    }
    return ret;
}

static inline void id_remap(union AssSchedCmd *src_cmd,
                            union AssSchedCmd *dst_cmd,
                            unsigned char id_map[],
                            struct spdk_hlsacccompute_opcontext **opcontext_map,
                            struct spdk_hlsacccompute_channel **tx_ch,
                            struct spdk_hlsacccompute_channel **rx_ch,
                            int tx_channel_num,
                            int rx_channel_num)
{
    if (src_cmd[0].header.opc != dst_cmd[0].header.opc)
    {
        memcpy(dst_cmd, src_cmd, sizeof(union AssSchedCmd));
    }
    int ops_num;
    int j;
    switch (src_cmd[0].header.opc)
    {
    case APPLY_OPS:
        ops_num = src_cmd[0].header.ops_num;
        j = 0;
        for (int i = 0; i < ops_num; i++)
        {
            ++j;
            dst_cmd[j].apply_ops_payload1.context_address = opcontext_map[i]->context_phy;
            SPDK_DEBUGLOG(hlsacc,"CONTEXT MAP PHY%llx\n",opcontext_map[i]->context_phy);
            ++j;
            dst_cmd[j].apply_ops_payload2.connections_num = src_cmd[j].apply_ops_payload2.connections_num;
            for (int k = 0; k < dst_cmd[j].apply_ops_payload2.connections_num && k < 3; k++)
            {
                int from = src_cmd[j].apply_ops_payload2.connections[k].from;
                int to = src_cmd[j].apply_ops_payload2.connections[k].to;
                from = (id_map[from >> 4] << 4) | (from & 0xf);
                if ((to & 0xF0) == 0xF0)
                {
                    // 如果to有这个位置，表示实际目的地不是算子而是通道
                    to = rx_ch[to & 0xF]->channel_id;
                }
                else
                    to = (id_map[to >> 4] << 4) | (to & 0xf);
                dst_cmd[j].apply_ops_payload2.connections[k].from = from;
                dst_cmd[j].apply_ops_payload2.connections[k].to = to;
            }
            if (spdk_unlikely(dst_cmd[j].apply_ops_payload2.connections_num > 3))
            {
                ++j;
                for (int k = 0; k < dst_cmd[j].apply_ops_payload2.connections_num - 3; k++)
                {
                    int from = src_cmd[j].apply_ops_payload2.connections[k].from;
                    int to = src_cmd[j].apply_ops_payload2.connections[k].to;
                    from = (id_map[from >> 4] << 4) | (from & 0xf);
                    if ((to & 0xF0) == 0xF0)
                    {
                        // 如果to有这个位置，表示实际目的地不是算子而是通道
                        to = rx_ch[to & 0xFF]->channel_id;
                    }
                    else
                        to = (id_map[to >> 4] << 4) | (to & 0xf);
                    dst_cmd[j].apply_ops_payload2.connections[k].from = from;
                    dst_cmd[j].apply_ops_payload2.connections[k].to = to;
                }
            }
        }
        break;
    case SUSPEND_OPS:
    case FORCE_FREE_OPS:
    case RESUME_OPS:
        ops_num = src_cmd[0].header.ops_num;
        dst_cmd[0].header.ops_num = ops_num;
        j = 0;
        for (int i = 0; i < ops_num; i++)
        {
            int src_id = src_cmd[1].generic_ops_payload.op_lists[i];
            dst_cmd[1].generic_ops_payload.op_lists[i] = id_map[src_id];
        }
        break;
    }
}

void cqe_recv_norm_cb(union AssSchedCmd *sqe, union AssSchedRet *cqe, void *cb_arg)
{
    struct spdk_hlsacccompute_request *req = cb_arg;
    if(req->tracker.status==ACC_REQ_WAITING_PAUSE){
        spdk_hlsacccompute_pause_request(req->dev,req,NULL,NULL);
        return;
    }
    if(req->program!=NULL)
        __spdk_hlsacccompute_run_request(req->dev, req, false);
    else{
        //SPDK_DEBUGLOG(hlsacc,"OH NO BAD!\n");
    }
}

void sw_cb(void* ctx){
    struct spdk_hlsacccompute_request* req = (struct spdk_hlsacccompute_request*)ctx;
    req->req_cb_fns(req,req->req_cb_args);
}

void grep_matching(char* result,char* buf,char* param_str) {
    int cnt = 0;
    for(int i=0;i<20;i++){
        printf("%c ",buf[i]);
    }
    printf("\n");
    for (int i = 0; i < 65536; i++) {
      result[i] = -1;
      for (int j = 0; j < 16384; j++) {
        bool matched = true;
        if (j + 2 <= 16384) {
          for (int k = 0; k < 2; k++) {
            cnt ++;
            matched &= (param_str[k] == buf[i*16384+(j + k)]);
          }
          if (matched) {
            result[i] = j;
            break;
          }
        }
      }
    }
    printf("COUNT%d\n",cnt);
  }

static void __spdk_hlsacccompute_run_easy_sw(void *ctx)
{
    struct spdk_hlsacccompute_request *req = ctx;
    struct spdk_hlsacccompute_program *program = req->program;
    assert(program!=NULL);
    ////SPDK_DEBUGLOG(hlsacc,"REQUEST ID%d\n",req->request_id);
    data_in_write_func_t data_in_write = program->data_in_write;
    data_out_read_func_t data_out_read = program->data_out_read;
    context_write_func_t context_write = program->context_write;
    run_func_t run = program->run;
    data_last_func_t last = program->last;
    
    if (!data_in_write||!data_out_read||!context_write||!run||!last)
    {
        SPDK_ERRLOG("Failed to lookup func\n");
        char* error = dlerror();
        if (error != NULL) {
            SPDK_ERRLOG("dlopen error: %s\n", error);
            
        }
    }
    else
    {
        struct spdk_hlsacccompute_virtual_object* tx_ob,*rx_ob;

        tx_ob = TAILQ_FIRST(&(req->tx_vos[0]));
        rx_ob = TAILQ_FIRST(&(req->rx_vos[0]));
        
        
        context_write(&(req->acccontext[0]->context),sizeof(struct AccContext),0,0);
        data_in_write(tx_ob->iov_base+tx_ob->cur_used,(tx_ob->iov_len-tx_ob->cur_used),0,req->software_resources->thread_id);
        data_out_read(rx_ob->iov_base+rx_ob->cur_used,(rx_ob->iov_len-rx_ob->cur_used),0,req->software_resources->thread_id);
        //data_in_write(tx_ob->iov_base,tx_ob->iov_len) ;
        //SPDK_DEBUGLOG(hlsacc,"WRITE FINISHED\n");
     
        //run_blowfish_with_data(tx_ob->iov_base,rx_ob->iov_base);
        ////SPDK_DEBUGLOG(hlsacc,"BEGIN");
       
        //grep_matching(rx_ob->iov_base,tx_ob->iov_base,(char*)(req->acccontext[0]->context.static_data));
        while (1)
        {
            run(req->software_resources->thread_id);
            if(last(0,req->software_resources->thread_id)){
                break;
            }
        }
        ////SPDK_DEBUGLOG(hlsacc,"END");
       
        TAILQ_REMOVE(&(req->rx_vos[0]), rx_ob, link);
        TAILQ_REMOVE(&(req->tx_vos[0]), tx_ob, link);
        TAILQ_INSERT_TAIL(&(req->dev->tx_channel_pool),tx_ob,link);
        TAILQ_INSERT_TAIL(&(req->dev->rx_channel_pool),rx_ob,link);
    }
   
    ////SPDK_DEBUGLOG(hlsacc,"SOFTWARE FINISH ID%d\n",req->request_id);
    if(program->get_data_size!=NULL){
        req->result = program->get_data_size(0,req->software_resources->thread_id);
        //SPDK_NOTICELOG("REQ RESULT %d\n",req->result);
    }
    //SPDK_NOTICELOG("REQUEST CB ARGS%llx\n",req->req_cb_args);

    spdk_thread_send_msg(req->dev->compute_thread,sw_cb,req);
    
}

static void spdk_hlsacccompute_run_easy_sw(void *ctx)
{
    pthread_t id;
     // 获取当前SPDK线程所在的核心ID
    struct spdk_hlsacccompute_request* req = ctx;
    int core_id = spdk_env_get_current_core();
    ////SPDK_DEBUGLOG(hlsacc,"EXECUTE REQUEST ID%d\n",req->request_id);
    ////SPDK_DEBUGLOG(hlsacc,"RUN CORE AT %d\n",core_id);
    // 设置线程属性，指定CPU亲和性
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    
    // 创建CPU亲和性掩码并设置
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
        // 将CPU亲和性掩码应用到线程属性
    int rc = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        SPDK_ERRLOG("cpu core affinity set fault: %s\n", strerror(rc));
        pthread_attr_destroy(&attr);
        return;
    }
    
    assert(ctx!=NULL);
    assert(req->program!=NULL);
    //pthread_create(&id,&attr,__spdk_hlsacccompute_run_easy_sw,ctx);
    __spdk_hlsacccompute_run_easy_sw(ctx);
    //pthread_detach(id);
    
}


static unsigned long long start_ticks,end_ticks;

int __spdk_hlsacccompute_run_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request *request, bool preempt)
{
    dev->dev_state = HLSDEV_RUN_REQUEST;
    struct spdk_hlsacccompute_program *program;
    program = request->program;
    struct spdk_hlsacccompute_op_elm *preempt_ops[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
    struct spdk_hlsacccompute_op_elm *non_preempt_ops[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
    static atomic_int id_allocator = 0;
    if(request->request_id == -1){
        
        int val = atomic_fetch_add(&id_allocator, 1);
        request->request_id = val;
    }
    int preempt_ops_num = 0;
    int non_preempt_ops_num = 0;
    int ret = 0;
    SPDK_DEBUGLOG(hlsacc,"CURRENT REQUEST ID%d\n",request->request_id);
    if (request->tracker.status == ACC_REQ_APPLYING || request->tracker.status == ACC_REQ_WAITING)
    {
       
        // 首先检查算子的占用状况，判断是否需要执行抢占
        spdk_hlsacccompute_dump_program_data(request->program);
        SPDK_DEBUGLOG(hlsacc,"REQ CB ARGS%llx\n", request->req_cb_args);
        start_ticks = spdk_get_ticks();
        //SPDK_NOTICELOG("BEGIN REQUEST!\n");
        char sw_name[30];
        struct spdk_cpuset *cpuset;
        // spdk_cpuset_set_cpu()
        switch (request->run_way)
        {
        case 0: // ignored default hardware
            break;
        case 1:
            // software
            //SPDK_DEBUGLOG(hlsacc,"Execute software\n");
            //cpuset = spdk_cpuset_alloc();
            if (request->program->input_channum != 1 || request->program->output_channum != 1 || request->program->apply_operators_num != 1)
            {
                SPDK_ERRLOG("Failed to run request on software mode,exchange to hardware mode\n");
                request->run_way = 0;
                dev->dev_state = HLSDEV_IDLE;
                return -1;
                break;
            }else if(request->program->software_data==NULL){
                dev->dev_state = HLSDEV_IDLE;
                return -1;
            }
            sprintf(sw_name, "acc_prog_%d", request->request_id);

            //request->software_thread = spdk_thread_create(sw_name, dev->sw_cpuset);
            if(TAILQ_EMPTY(&(dev->sw_resources_pool))){
                dev->dev_state = HLSDEV_IDLE;
                return -1;
            }
            request->software_resources = TAILQ_FIRST(&(dev->sw_resources_pool));
            TAILQ_REMOVE(&(dev->sw_resources_pool),request->software_resources,link);
            if (request->software_resources != NULL)
            {
                //SPDK_DEBUGLOG(hlsacc,"SUBMIT SW REQID%d\n",request->request_id);
                spdk_thread_send_msg(spdk_get_thread(), spdk_hlsacccompute_run_easy_sw, request);
            }
            else
            {
                SPDK_ERRLOG("Failed to create compute thread,choose to create hardware mode\n");
                dev->dev_state = HLSDEV_IDLE;
                return -1;
            }
            request->tracker.status = ACC_REQ_EXECUTING;
            dev->dev_state = HLSDEV_IDLE;
            TAILQ_INSERT_TAIL(&dev->working_queue, request, link);
            return 0;
            break;
        case 2:
            // hardware
            break;
        default:
            SPDK_ERRLOG("Undefined program type\n");
            return -1;
        }
        for (int i = 0; i < program->apply_operators_num; i++)
        {
            int needed_type_id = program->apply_operators_id_map[i];
            if (dev->opconfigs[needed_type_id]==NULL||TAILQ_EMPTY(&dev->opconfigs[needed_type_id]->elements))
            {
                ret = -1;
                return ret;
            }
            struct spdk_hlsacccompute_opconfig *op = (dev->opconfigs[needed_type_id]);
            struct spdk_hlsacccompute_op_elm *op_elm = TAILQ_FIRST(&(dev->opconfigs[needed_type_id]->elements));
            for (int j = 0; j < non_preempt_ops_num; j++)
            {
                if (op_elm == non_preempt_ops[j])
                {
                    // 当出现请求分配2个相同功能的算子，实际分配的算子地址不能是同一个
                    op_elm = TAILQ_NEXT((op_elm), link);
                }
            }
            for (int j = 0; j < preempt_ops_num; j++)
            {
                if (op_elm == preempt_ops[j])
                {
                    // 当出现请求分配2个相同功能的算子，实际分配的算子地址不能是同一个
                    op_elm = TAILQ_NEXT((op_elm), link);
                }
            }
            // 为request绑定opconfig，后续出现问题再设置为空
            if(op_elm==NULL){
                SPDK_ERRLOG("ALLOCATE WRONG OPERATOR!\n");
            }
            request->op_elm[i] = op_elm;
            // 当然也要绑定dynamic_map
            // 在重映射阶段，如果计算请求曾经被调用过，而且通道不支持重置，那通道的目的id是不可被修改为其他值的，所以会报错
            if (request->tx_channel[0] != NULL && op_elm->operator_slot_id != request->dynamic_id_map[i])
            {
                SPDK_ERRLOG("THE CHANNEL DOES NOT SUPPORT RESCHED, CAN NOT CHANGE OPERATOR ID\n");
                return -1;
            }
            request->dynamic_id_map[i] = op_elm->operator_slot_id;
            //SPDK_DEBUGLOG(hlsacc,"DYNAMIC ID MAP [%d],map to [%d]\n",i,request->dynamic_id_map[i]);
            if (op_elm->state != IDLE)
            {
                // 当前未部署到板卡上属于未定义行为
                if (op_elm->state == UNIMPLEMENTED)
                {
                    ret = -1;
                    dev->dev_state = HLSDEV_IDLE;
                    return ret;
                }else if(!preempt||op_elm->state==LOCKED){
                    dev->dev_state = HLSDEV_IDLE;
                    //SPDK_DEBUGLOG(hlsacc,"PREPARE FOR NEXT SCHED\n");
                    //spdk_hlsacccompute_prepare_for_next_sched(request,dev->schedule_strategy);
                    return -1;
                }
                else
                {
                    preempt_ops[preempt_ops_num++] = op_elm;
                    spdk_wmb();
                }
            }
            else
            {
                non_preempt_ops[non_preempt_ops_num++] = op_elm;
                spdk_wmb();
            }
            if ((op_elm->state == WORKING&&op_elm->request==NULL) || 
                (op_elm->request != NULL && (op_elm->request->tracker.status == ACC_REQ_WAITING || op_elm->request->tracker.status == ACC_REQ_WAITING_PAUSE)))
            {
                // 这代表当前已经有在抢占的了，所以不改变行为，不进行抢占
                dev->dev_state = HLSDEV_IDLE;
                SPDK_DEBUGLOG(hlsacc,"PREPARE FOR NEXT SCHED\n");
                //spdk_hlsacccompute_prepare_for_next_sched(request, dev->schedule_strategy);
                return -1;
            }
        }
      
        struct spdk_hlsacccompute_request *need_pause_request[SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP];
        int pause_request_num = 0;

        if (spdk_likely(preempt))
        {
            for (int i = 0; i < preempt_ops_num; i++)
            {
                struct spdk_hlsacccompute_op_elm *need_pause_op_elm = preempt_ops[i];
                if (need_pause_op_elm->request != NULL)
                {
                    struct spdk_hlsacccompute_task_tracker *tracker = &(need_pause_op_elm->request)->tracker;
                    if (tracker->status == ACC_REQ_WAITING || tracker->status == ACC_REQ_WAITING_PAUSE ||
                    need_pause_op_elm->request->priority>=request->priority)
                    {
                        SPDK_ERRLOG("Could Not Apply Locked Operator\n");
                        SPDK_DEBUGLOG(hlsacc,"PREPARE FOR NEXT SCHED\n");
                        return -1;
                    }
                    else if (tracker->status == ACC_REQ_EXECUTING)
                    {
                        int j = 0;
                        
                        for (; j < pause_request_num; j++)
                        {
                            if (need_pause_request[j] == need_pause_op_elm->request)
                            {
                                break;
                            }
                        }
                        if (j == pause_request_num)
                        {
                            need_pause_request[j] = need_pause_op_elm->request;
                            if(need_pause_request[j]->priority>=request->priority){
                                preempt = false;
                            }
                            pause_request_num++;
                        }
                    }
                }
                else
                {
                    continue;
                }
            }
            // Locked OpConfig
            for (int i = 0; preempt&&(i < preempt_ops_num); i++)
            {
                struct spdk_hlsacccompute_op_elm *need_pause_op_elm = preempt_ops[i];
                // Remove From Head
                TAILQ_REMOVE(&(need_pause_op_elm->opconfig->elements), need_pause_op_elm, link);
                //SPDK_DEBUGLOG(hlsacc,"PLOCKED\n");
                need_pause_op_elm->state = LOCKED;
                TAILQ_INSERT_TAIL(&(need_pause_op_elm->opconfig->elements), need_pause_op_elm, link);
                need_pause_op_elm->request = request;
            }
           

        }
     
        // Locked OpConfig
        if (((!preempt) && preempt_ops_num == 0) || preempt)
        {
            for (int i = 0; i < non_preempt_ops_num; i++)
            {
                struct spdk_hlsacccompute_op_elm *need_pause_op_elm = non_preempt_ops[i];
                // Remove From Head
                //SPDK_DEBUGLOG(hlsacc,"UPLOCKED preempt%d preempt_ops_num%d\n",preempt,preempt_ops_num);
                TAILQ_REMOVE(&(need_pause_op_elm->opconfig->elements), need_pause_op_elm, link);
                need_pause_op_elm->state = LOCKED;
                TAILQ_INSERT_TAIL(&(need_pause_op_elm->opconfig->elements), need_pause_op_elm, link);
                need_pause_op_elm->request = request;
            }
        }
        else
        {
            dev->dev_state = HLSDEV_IDLE;
            //spdk_hlsacccompute_prepare_for_next_sched(request, dev->schedule_strategy);
            SPDK_DEBUGLOG(hlsacc,"PREPARE FOR NEXT SCHED\n");
            return -1;
        }
        //TODO 此处存在一些设计性问题，需要得到解决
        //抢占请求的时候，通道看上去被释放，但是实际上并没有！
        //因此这可能在某种情况下带来严重的性能问题，导致计算整个卡死，这是需要后续修复的问题！
        request->tx_channel_num = request->program->input_channum;
        for (int i = 0; i < request->program->output_channum; i++)
        {
            struct spdk_hlsacccompute_channel *ch;
            if (request->rx_channel[i] == NULL)
            {
                ch = spdk_hlsacccompute_apply_channel(dev, false);
                request->rx_channel[i] = ch;
                if(ch==NULL)
                    return -1;
            }
            else
                ch = request->rx_channel[i];

            ch->virtual_channel_id = i;
            ch->req = request;
        }
        for (int i = 0; i < request->program->input_channum; i++)
        {
            struct spdk_hlsacccompute_channel *ch;
            if (request->tx_channel[i] == NULL)
            {
                ch = spdk_hlsacccompute_apply_channel(dev, true);
                request->tx_channel[i] = ch;
                if(ch==NULL)
                    return -1;
            }
            else
                ch = request->tx_channel[i];
            ch->virtual_channel_id = i;
            ch->req = request;
            //SPDK_DEBUGLOG(hlsacc,"DEST BASIC%d,opmap%d\n",request->program->input_channel_destination[i],request->dynamic_id_map[request->program->input_channel_destination[i] ]);
            ch->dest_id = (request->dynamic_id_map[request->program->input_channel_destination[i] << 4] << 4) | (request->program->input_channel_destination[i] & 0xF);
        }

        request->rx_channel_num = request->program->output_channum;
        // 此处应当发起SQE，对ID进行重映射
        id_remap(request->program->applyops, request->applyops, request->dynamic_id_map, request->acccontext, request->tx_channel, request->rx_channel, request->program->input_channum, request->program->output_channum);
        id_remap(request->program->pauseops, request->pauseops, request->dynamic_id_map, NULL, NULL, NULL, 0, 0);
        id_remap(request->program->freeops, request->freeops, request->dynamic_id_map, NULL, NULL, NULL, 0, 0);
        request->tracker.waiting_preempt_req = preempt_ops_num;
       // SPDK_NOTICELOG("FIRST REQUEST SETUP\n");
	if (preempt_ops_num != 0)
        {
            request->tracker.status = ACC_REQ_PREEMPTING;
             // Generate Pause Request
             for (int i = 0; i < pause_request_num; i++)
             {
                 need_pause_request[i]->next_request = request;
                 spdk_hlsacccompute_pause_request(dev, need_pause_request[i], need_pause_request[i], cqe_recv_norm_cb);
             }
        }
        else
        {
            goto sqe_execute;
        }
        return ret;
    }
    else if (request->tracker.status == ACC_REQ_PREEMPTING)
    {
        request->tracker.waiting_preempt_req--;
        //SPDK_NOTICELOG("PREEMPTING FINISHED\n");
    sqe_execute:
        if (request->tracker.waiting_preempt_req > 0)
            return 0;
        request->tracker.status = ACC_REQ_EXECUTING;
        TAILQ_INSERT_TAIL(&dev->working_queue, request, link);
        //SPDK_DEBUGLOG(hlsacc,"SUBMIT HW REQID%d\n",request->request_id);
        int tail = (*(dev->bar->ppair_cqtailer));
        //for(int i=0;i<request->program->apply_operators_num;i++){
            //request->config[i]->state = WORKING;
        //    request->config[i]->state = IDLE;
        //}
        // request->acccontext[0]->context.static_data
        int cid = execute_sqe(dev, request->applyops, request, (void *)request, cqe_recv_norm_cb);
        
        // 直接开始分配通道
        for (int i = 0; i < request->program->input_channum; i++)
        {
            struct spdk_hlsacccompute_channel *ch;

            ch = request->tx_channel[i];
            ch->virtual_channel_id = i;
            ch->req = request;
            ch->dest_id = (request->dynamic_id_map[request->program->input_channel_destination[i] << 4] << 4) | (request->program->input_channel_destination[i] & 0xF);
        }
        request->tx_channel_num = request->program->input_channum;
        for (int i = 0; i < request->program->output_channum; i++)
        {
            struct spdk_hlsacccompute_channel *ch = request->rx_channel[i];

            request->rx_channel[i] = ch;
            ch->virtual_channel_id = i;
            ch->req = request;
        }
        request->rx_channel_num = request->program->output_channum;
        
        // 假装小睡一会， 万一发出去的命令就收到结果了呢？
        for (int i = 0; i < 64; i++);
        // TODO这里有BUG execute sqe的时候应该返回cid，然后根据cid判断有没有执行完成
        //int new_tail = (*(dev->bar->ppair_cqtailer));
        //SPDK_DEBUGLOG(hlsacc,"CID%d NEWCID%d\n", cid, dev->bar->ppair_cq[(new_tail - 1) % SPDK_HLSACCCOMPUTE_CQ_SIZE].header.cid);
        //May takes BUG :(
        //dev->inner_ppair_cqtailer = new_tail;
        /*
        if (tail != new_tail && (cid == dev->bar->ppair_cq[(new_tail - 1) % SPDK_HLSACCCOMPUTE_CQ_SIZE].header.cid))
        {
            SPDK_DEBUGLOG(hlsacc,"DIRECT RUN!\n");

            // 默认已经请求成功，所以直接进行部署！
            for (int i = 0; i < request->rx_channel_num; i++)
            {
                struct spdk_hlsacccompute_channel *ch = request->rx_channel[i];
                ch->channel_recv(ch);
            }
            for (int i = 0; i < request->tx_channel_num; i++)
            {
                struct spdk_hlsacccompute_channel *ch = request->tx_channel[i];
                ch->channel_send(ch);
            }
            // 修改opconfig的状态
            for (int i = 0; i < request->program->apply_operators_num; i++)
            {
                request->config[i]->state = WORKING;
            }
        }
        else*/

        {
            request->tracker.status = ACC_REQ_WAITING_CHANNEL;
            SPDK_DEBUGLOG(hlsacc,"WAITING RUN!\n");
        }
        spdk_hlsacccompute_poll_cq(dev);
    }
    else if (request->tracker.status == ACC_REQ_WAITING_CHANNEL)
    {
        //SPDK_NOTICELOG("ENTERING RUN!\n");
        end_ticks = spdk_get_ticks();
        uint64_t duration_us = (end_ticks-start_ticks)/(spdk_get_ticks_hz()/1000000);
        //SPDK_NOTICELOG("DURATION US%llx\n",duration_us);
        dev->dev_state = HLSDEV_IDLE;
        for (int i = 0; i < request->rx_channel_num; i++)
        {
            struct spdk_hlsacccompute_channel *ch = request->rx_channel[i];
            //SPDK_NOTICELOG("channelid%dRX VOS DATA BASEADD%llx LEN %d CUR_USED %ld\n",ch->channel_id,request->rx_vos[0].tqh_first->iov_base,request->rx_vos[0].tqh_first->iov_len,request->rx_vos[0].tqh_first->cur_used);
            ch->channel_recv(ch);
        }
        spdk_wmb();
        for (int i = 0; i < request->tx_channel_num; i++)
        {
            struct spdk_hlsacccompute_channel *ch = request->tx_channel[i];
         
            //SPDK_NOTICELOG("channelid%dTX VOS DATA BASEADD%llx LEN %d CUR_USED %ld\n",ch->channel_id,request->tx_vos[0].tqh_first->iov_base,request->tx_vos[0].tqh_first->iov_len,request->tx_vos[0].tqh_first->cur_used);
            ch->channel_send(ch);
        }
        request->tracker.status = ACC_REQ_EXECUTING;
        // 修改opconfig的状态
        for (int i = 0; i < request->program->apply_operators_num; i++)
        {
            request->op_elm[i]->state = WORKING;
            //SPDK_DEBUGLOG(hlsacc,"ELM ADDRESS%llx\n",request->op_elm[i]);
        }
        //SPDK_DEBUGLOG(hlsacc,"CHANGE STATE!\n");
        SPDK_DEBUGLOG(hlsacc,"TX CHANNEL NUM %d RX_CHANNEL_NUM!\n",request->tx_channel_num,request->rx_channel_num);
    }
    return ret;
}



int spdk_hlsacccompute_run_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request *request, bool preempt)
{
    request->tracker.responding_time = spdk_get_ticks();
   
    int ret = __spdk_hlsacccompute_run_request(dev,request,preempt);
    if(ret==-1){
        spdk_hlsacccompute_prepare_for_next_sched(request,dev->schedule_strategy);
    }
    return ret;
}

int spdk_hlsacccompute_pause_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request *request, void *cb_args, spdk_hlsacccompute_cmd_cb_fn cb_fn)
{
    bool canreset = true;
    //SPDK_DEBUGLOG(hlsacc,"PAUSEING REQUEST TD%d\n",request->request_id);
    switch(request->tracker.status){
        case ACC_REQ_EXECUTING:
            request->tracker.status = ACC_REQ_WAITING_PAUSE;
            for (int i = 0; i < request->tx_channel_num; i++)
            {
                request->tx_channel[i]->channel_pause(request->tx_channel[i]);
                //canreset = canreset & (request->tx_channel[i]->support_reset);
            }
            // 如果不能进行一个重置，那就不释放通道了
            if (canreset)
            {
                for (int i = 0; i < request->tx_channel_num; i++)
                {
                    spdk_hlsacccompute_release_channel(dev, request->tx_channel[i]);
                }
                for (int i = 0; i < request->tx_channel_num; i++)
                {
                    if(request->tx_channel[i]!=NULL)
                    request->tx_channel[i]->req = NULL;
                    request->tx_channel[i] = NULL;
                }
                
                for (int i = 0; i < request->rx_channel_num; i++)
                {
                   request->rx_channel[i]->channel_poller(request->rx_channel[i]);
                }   
            }
            else
            {
                for (int i = 0; i < request->tx_channel_num; i++)
                {
                    request->tx_channel[i]->channel_pause(request->tx_channel[i]);
                }
                for (int i = 0; i < request->rx_channel_num; i++)
                {
                    request->rx_channel[i]->channel_pause(request->rx_channel[i]);
                }
            }    
            if(request->tracker.status!=ACC_REQ_IDLE&&request->tracker.status!=ACC_REQ_FINISHED){
                execute_sqe(dev, request->pauseops, request, (void *)cb_args, cb_fn);
                //clear Op config
                for(int i=0;i<request->program->apply_operators_num;i++){
                    request->op_elm[i]->request = NULL;
                    request->op_elm[i]->state = request->op_elm[i]->state==LOCKED?LOCKED:IDLE;
                    request->op_elm[i] = NULL;
                }
                TAILQ_REMOVE(&dev->working_queue, request, link);       
                
            }
            else{
                SPDK_DEBUGLOG(hlsacc,"REQUEST IDLE!!\n");
                if(request->next_request)
                    __spdk_hlsacccompute_run_request(request->dev,request->next_request,false);
            }
            break;
        case ACC_REQ_WAITING_PAUSE:
            SPDK_DEBUGLOG(hlsacc,"DUMP DESCRIPTOR NUM%d AVAILABLE BUF NUM%d\n",request->acccontext[0]->context.privated_data.descriptor_num,request->acccontext[0]->context.privated_data.available_buf_num);
            SPDK_DEBUGLOG(hlsacc,"DESCRITPOR FIRST BYTES%llx,DESCRIPTOR FIFO SEL%d\n",*(unsigned long long*)(request->acccontext[0]->context.privated_data.stream_buf_add_lists[0]),request->acccontext[0]->context.privated_data.descriptor[0].length);
            
            for(int i=0;i<request->program->apply_operators_num;i++){
                if(request->acccontext[i]->context.privated_data.descriptor_num!=0
                    &&request->acccontext[i]->context.privated_data.descriptor[request->acccontext[i]->context.privated_data.descriptor_num-1].length==0){
                    request->acccontext[i]->context.privated_data.descriptor_num--;
                }
            }
            
            request->tracker.status = ACC_REQ_WAITING;
            for (int i = 0; i < request->rx_channel_num; i++)
            {
                request->rx_channel[i]->channel_pause(request->rx_channel[i]);
            }
            for (int i = 0; i < request->rx_channel_num; i++)
            {
                spdk_hlsacccompute_release_channel(dev, request->rx_channel[i]);
            }
            for (int i = 0; i < request->rx_channel_num; i++)
            {
                if(request->rx_channel[i]!=NULL)
                request->rx_channel[i]->req = NULL;
                request->rx_channel[i] = NULL;
            }
            spdk_hlsacccompute_prepare_for_next_sched(request, dev->schedule_strategy);
            if(request->next_request)
                __spdk_hlsacccompute_run_request(request->dev,request->next_request,false);
            break;
        default:
            SPDK_ERRLOG("Undefined Status!\n");
            assert(false);
            break;
    }
   
    return 0;
}

void spdk_hlsacccompute_prepare_for_next_sched(struct spdk_hlsacccompute_request *request, int sched_strategy)
{
    //SPDK_DEBUGLOG(hlsacc,"REQUEST%d PREPARE FOR NEXT SCHED%d\n",request->request_id,sched_strategy);
    SPDK_DEBUGLOG(hlsacc,"PREPARE FOR NEXT SCHED request id%d\n",request->request_id);
    sched_strategy = SCHED_BASIC_PRIORITY_PREEMPT;
    if (sched_strategy == SCHED_FCFS||sched_strategy == SCHED_BASIC_PRIORITY_PREEMPT)
    {
        struct spdk_hlsacccompute_dev *dev = request->dev;
        if(request->dev!=NULL)
        TAILQ_INSERT_TAIL(&(dev->waiting_queue[(dev->waiting_queue_ptr + 0) % SPDK_HLSACCCOMPUTE_SLICE_WINDOWS]), request, link);
        request->tracker.status = ACC_REQ_WAITING;
    } else if (sched_strategy == SCHED_BASIC_SOFTWARE_COCACULATE){
        struct spdk_hlsacccompute_dev *dev = request->dev;
        int oldway = request->run_way;
        request->run_way=1;
        int ret = __spdk_hlsacccompute_run_request(request->dev,request,false);
        if(ret == -1){
            request->run_way=oldway;
            if(request->dev!=NULL)
            TAILQ_INSERT_TAIL(&(dev->waiting_queue[(dev->waiting_queue_ptr + 0) % SPDK_HLSACCCOMPUTE_SLICE_WINDOWS]), request, link);
            request->tracker.status = ACC_REQ_WAITING;
        }
    
    }

    return 0;
}

void spdk_hlsacccompute_norm_cb_fn(struct spdk_hlsacccompute_request *request, void *cb_arg)
{
    // spdk_hlsacccompute_free_request(request->dev,request,true);
}

int spdk_hlsacccompute_free_request(struct spdk_hlsacccompute_dev *dev, struct spdk_hlsacccompute_request *request, bool sendsqe)
{
    //SPDK_DEBUGLOG(hlsacc,"FREE REQ%d\n",request->request_id);
    //SPDK_NOTICELOG("FREE REQUEST\n");
    request->tracker.status = ACC_REQ_FINISHED;
    volatile uint64_t old_time = request->tracker.responding_time;
    request->tracker.responding_time = spdk_get_ticks() - old_time;
    if (sendsqe&&request->run_way!=1)
        execute_sqe(dev, request->freeops, request, (void *)request, NULL);
    // 结束软件线程
    else if(request->run_way==1&&request->software_resources!=NULL){
        SPDK_DEBUGLOG(hlsacc,"Free Request %llx\n",request->software_resources->thread);
        //spdk_thread_destroy(request->software_thread);
        TAILQ_INSERT_TAIL(&(dev->sw_resources_pool),request->software_resources,link);
        request->software_resources = NULL;
        SPDK_DEBUGLOG(hlsacc,"FREE FINISHED\n");
    }
    
    for (int i = 0; i < request->program->apply_operators_num; i++)
    {
        
        for(int j=0;j<7;j++){
            SPDK_DEBUGLOG(hlsacc,"DUMP DESCRIPTOR NUM%d AVAILABLE BUF NUM%d\n",request->acccontext[i]->context.privated_data.descriptor_num,request->acccontext[i]->context.privated_data.available_buf_num);
            if(request->acccontext[i]->context.privated_data.stream_buf_add_lists[j]!=NULL){
                spdk_mempool_put(dev->mgnt_mempool,request->acccontext[i]->context.privated_data.stream_buf_add_lists[j]);
                request->acccontext[i]->context.privated_data.stream_buf_add_lists[j] = NULL;
                ////SPDK_DEBUGLOG(hlsacc,"MEMPOOL ELM SIZE%d\n",spdk_mempool_count(dev->mgnt_mempool));
            }else
                break;
        }
    }
    TAILQ_REMOVE(&dev->working_queue, request, link);
    if(request->run_way!=1){
        // 回收算子上下文
        
        for (int i = 0; i < request->program->apply_operators_num; i++)
        {
            
            memset(&(request->acccontext[i]->context), 0, sizeof(struct AccContext));

            if(request->acccontext[i]!=NULL)
            //spdk_mempool_put(dev->opcontext_poolv2,request->acccontext[i]);
            TAILQ_INSERT_TAIL(&(dev->opcontext_pool), request->acccontext[i], link);

            struct spdk_hlsacccompute_op_elm *op_elm = request->op_elm[i];
            if(op_elm!=NULL){
                op_elm->request = NULL;
                op_elm->state = IDLE;
                TAILQ_REMOVE(&(dev->opconfigs[op_elm->opconfig->operator_type_id]->elements), op_elm, link);
                TAILQ_INSERT_HEAD(&(dev->opconfigs[op_elm->opconfig->operator_type_id]->elements), op_elm, link);
                SPDK_DEBUGLOG(hlsacc,"Free OPS%d NAME%s\n",op_elm->opconfig->operator_type_id,op_elm->opconfig->operator_type_name);
            }

        }
        // 回收通道
        for (int i = 0; i < request->tx_channel_num; i++)
        {
            struct spdk_hlsacccompute_channel *ch = request->tx_channel[i];
            if(ch!=NULL){
                ch->channel_pause(ch);
                spdk_hlsacccompute_release_channel(dev, ch);
                if(request->tx_channel[i]!=NULL)
                    request->tx_channel[i]->req = NULL;
                request->tx_channel[i] = NULL;
            }
        }
        for (int i = 0; i < request->rx_channel_num; i++)
        {
            struct spdk_hlsacccompute_channel *ch = request->rx_channel[i];
            if(ch!=NULL){
                ch->channel_pause(ch);
                spdk_hlsacccompute_release_channel(dev, ch);
                if(request->rx_channel[i]!=NULL)
                    request->rx_channel[i]->req = NULL;
                request->rx_channel[i] = NULL;
            }
        }
      
    }
    
    // 回收virtual object
    for(int i=0;i<SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP;i++){
        if(TAILQ_EMPTY(&(request->rx_vos[i]))){
            break;
        }else{
            while(!TAILQ_EMPTY(&(request->rx_vos[0]))){
                struct spdk_hlsacccompute_virtual_object* ob = TAILQ_FIRST(&(request->rx_vos[i]));
                TAILQ_REMOVE(&(request->rx_vos[i]),ob,link);
                TAILQ_INSERT_TAIL(&(dev->vo_pool),ob,link);
            }
        }
    }
    for(int i=0;i<SPDK_HLSACCCOMPUTE_MAX_OPERATOR_CAP;i++){
        if(TAILQ_EMPTY(&(request->tx_vos[i]))){
            break;
        }else{
            while(!TAILQ_EMPTY(&(request->tx_vos[i]))){
                struct spdk_hlsacccompute_virtual_object* ob = TAILQ_FIRST(&(request->tx_vos[i]));
                TAILQ_REMOVE(&(request->tx_vos[i]),ob,link);
                TAILQ_INSERT_TAIL(&(dev->vo_pool),ob,link);
            }
        }
    }

    memset(request,0,sizeof(struct spdk_hlsacccompute_request));
    request->request_id = -1;
    request->tracker.status = ACC_REQ_IDLE;
    //TAILQ_INSERT_TAIL(&dev->request_pool, request, link);
    if(request!=NULL){
        spdk_mempool_put(dev->request_poolv2,request);
        dev->request_pool_ele_num++;
    }else if(dev->request_pool_ele_num<=0){
        SPDK_ERRLOG("FAILED TO FREE REQUEST MULTI_CORE_WRONG\n");
        assert(false);
    }
    // 自动化的触发调度
    spdk_hlsacccompute_schedule_request(dev);
    return 0;
}

// 是否需要添加一个new_request类型，标记有新的类型产生
void spdk_hlsacccompute_schedule_request(struct spdk_hlsacccompute_dev *dev)
{
    if(dev->dev_state != HLSDEV_IDLE){
        dev->waiting_queue_ptr = 0;
        return;
    }
    dev->dev_state = HLSDEV_SCHEDING;
    TAILQ_HEAD(, spdk_hlsacccompute_request)
    re_sched_queue;
    struct spdk_hlsacccompute_request *request;
    TAILQ_INIT(&re_sched_queue);
    //struct spdk_hlsaccompute_request* last_req = TAILQ_LAST()
    while (!TAILQ_EMPTY(&dev->waiting_queue[dev->waiting_queue_ptr]))
    {
        //SPDK_DEBUGLOG(hlsacc,"SCHED\n");
        request = TAILQ_FIRST(&dev->waiting_queue[dev->waiting_queue_ptr]);
        TAILQ_REMOVE(&dev->waiting_queue[dev->waiting_queue_ptr], request, link);
        int ret = 0;
        if(request->priority < 220){
            //SPDK_DEBUGLOG(hlsacc,"DONOT PREEMPT\n");
            ret = __spdk_hlsacccompute_run_request(dev, request, false);
        }else{
            //SPDK_DEBUGLOG(hlsacc,"NEED PREEMPT\n");
            ret = __spdk_hlsacccompute_run_request(dev, request, true);
        }
        if (ret == -1)
        {
            TAILQ_INSERT_TAIL(&re_sched_queue, request, link);
        }
    }
    while (!TAILQ_EMPTY(&re_sched_queue))
    {
        request = TAILQ_FIRST(&(re_sched_queue));
        TAILQ_REMOVE(&re_sched_queue, request, link);
        spdk_hlsacccompute_prepare_for_next_sched(request, dev->schedule_strategy);
    }
    dev->waiting_queue_ptr = 0;
    dev->dev_state = HLSDEV_IDLE;
}



int spdk_hlsacccompute_devmem_malloc(struct spdk_hlsacccompute_dev *dev,
                                     size_t size, size_t align, void **vaddr, void **paddr)
{
    void *vaddr_ptr;
    void *paddr_ptr;
    //vaddr_ptr = spdk_dma_malloc((size_t)size, (size_t)align, NULL);
    vaddr_ptr = spdk_malloc(size,PAGE_SIZE,NULL,SPDK_ENV_SOCKET_ID_ANY,SPDK_MALLOC_DMA);
    unsigned long long int len = -1;
    paddr_ptr = spdk_vtophys(vaddr_ptr, &len);
    if (vaddr != NULL)
    {
        *vaddr = vaddr_ptr;
    }
    if (paddr != NULL)
    {
        *paddr = paddr_ptr;
    }
    struct spdk_hlsacccompute_virtual_object ob;
    ob.cur_used = 0;
    ob.is_mem = true;
    ob.iov_base = vaddr_ptr;
    ob.iov_len = size;
    if (spdk_unlikely((dev->dev_mem_table) == NULL))
    {
        SPDK_ERRLOG("FAILED DEVMEMTABLE IS EMPTY!\n");
        return -1;
    }
    void *entry_ptr;
    //if ((int)len < (int)size || paddr_ptr == SPDK_VTOPHYS_ERROR)
    if (paddr_ptr == SPDK_VTOPHYS_ERROR)
    {
        SPDK_ERRLOG("FAILED TO MALLOC DATA,SIZE %d LEN %d got VADDR%llx PADDR%llx\n", (int)size, (int)len, vaddr_ptr, paddr_ptr);
        spdk_free(vaddr_ptr);
        if (paddr_ptr == SPDK_VTOPHYS_ERROR)
        {
            SPDK_ERRLOG("SPDK_VTOPHYS_ERROR\n");
        }
        return -1;
    }

    // May Has Performance Problem!
    // Need To Provide an Id Alloactor
    unsigned int key = atomic_fetch_add(&(dev->next_mem_id), 1);

    int ret = spdk_cuckoo_table_entry_add(dev->dev_mem_table, key, (void *)(&ob), &entry_ptr);

    return key;
}

int spdk_hlsacccompute_devmem_free(struct spdk_hlsacccompute_dev *dev, void *vaddr, int id)
{
    if (vaddr != NULL && id < 0)
        spdk_free(vaddr);
    else
    {
        struct spdk_hlsacccompute_virtual_object ob;
        int ret = spdk_cuckoo_table_entry_delete(dev->dev_mem_table, id, &ob);
        spdk_free(ob.iov_base);
        return ret;
    }
    return 0;
}

int spdk_hlsacccompute_devmem_lookup(struct spdk_hlsacccompute_dev *dev, int id, void **vaddr, void **paddr)
{

    struct spdk_hlsacccompute_virtual_object *ob;
    unsigned int idd = id;
    int ret = spdk_cuckoo_table_lookup(dev->dev_mem_table, idd, &ob);
    if (ret == 0 && ob != NULL)
    {
        *vaddr = ob->iov_base;
        *paddr = spdk_vtophys((void *)(ob->iov_base), NULL);
    }
    if (ob == NULL)
        return -1;
    return ret;
}

int spdk_hlsacccompute_add_program(struct spdk_hlsacccompute_dev *dev,
                                   struct spdk_hlsacccompute_program *program)
{
    for (int i = 0; i < 64; i++)
    {
        if (dev->program_list[i] == NULL)
        {
            // 先进行一下程序的正确性检查
            for (int j = 0; j < program->apply_operators_num; j++)
            {
                uint8_t operator_type_id = program->apply_operators_id_map[j];
                if (operator_type_id >= SPDK_HLSACCCOMPUTE_MAX_OPERATORS_SUPPORT ||
                    dev->opconfigs[operator_type_id] == NULL)
                {
                    SPDK_ERRLOG("Program references undefined operator type %u\n",
                                operator_type_id);
                    return -1;
                }
            }
            dev->program_list[i] = program;
            program->program_id = i;
            return i;
        }
    }
    return -1;
}

int spdk_hlsacccompute_dump_program_data(struct spdk_hlsacccompute_program *program)
{
    SPDK_DEBUGLOG(hlsacc,"PROGRAM DATA:\n, PROGRAM_ID%d\nAPPLY_OPS_SIZE%d\nAPPLY_OPERATORS_NUM%d\nINPUT_CHANNUM%d\nOUTPUT_CHANNUM%d\nAPPLYOPS HEADER OPC%d,OPSNUM%d\n", program->program_id,
                   program->apply_ops_size,
                   program->apply_operators_num,
                   program->input_channum,
                   program->output_channum,
                   program->applyops[0].header.opc,
                   program->applyops->header.ops_num);
    SPDK_DEBUGLOG(hlsacc,"PROGRAM DATA:FREE OPLIST0%d 1%d 2%d 3%d,NUM%d\n", program->freeops[1].generic_ops_payload.op_lists[0],
                   program->freeops[1].generic_ops_payload.op_lists[1],
                   program->freeops[1].generic_ops_payload.op_lists[2],
                   program->freeops[1].generic_ops_payload.op_lists[3],
                   program->freeops[0].header.ops_num);
    SPDK_DEBUGLOG(hlsacc,"PROGRAM DATA:APPLY CON NUM%ld CON FROM %d CON TO %d OPS NUM%d\n",
                   program->applyops[2].apply_ops_payload2.connections_num,
                   program->applyops[2].apply_ops_payload2.connections[0].from,
                   program->applyops[2].apply_ops_payload2.connections[0].to,
                   program->applyops[0].header.ops_num);
    return 0;
}

int spdk_hlsacccompute_add_program_with_id(struct spdk_hlsacccompute_dev *dev,
                                           struct spdk_hlsacccompute_program *program,
                                           int id)
{
    if (id < 0 || id >= 64)
    {
        SPDK_ERRLOG("Invalid program ID %d\n", id);
        return -1;
    }

    spdk_hlsacccompute_dump_program_data(program);
    for (int i = id; i == id; i++)
    {

        if (dev->program_list[i] == NULL)
        {
            // 先进行一下程序的正确性检查
            for (int j = 0; j < program->apply_operators_num; j++)
            {
                uint8_t operator_type_id = program->apply_operators_id_map[j];
                if (operator_type_id >= SPDK_HLSACCCOMPUTE_MAX_OPERATORS_SUPPORT ||
                    dev->opconfigs[operator_type_id] == NULL)
                {
                    SPDK_ERRLOG("Program references undefined operator type %u\n",
                                operator_type_id);
                    return -1;
                }
            }
            if (dev->program_list[i] != NULL)
            {
                return -1;
            }
            struct spdk_hlsacccompute_program *val = NULL;
            while (!atomic_compare_exchange_weak(&dev->program_list[i], &val, program))
            {
                if (val != NULL)
                {
                    return -1;
                }
            }
            // dev->program_list[i] = program;
            program->program_id = i;
            program->activated = false;

            return i;
        }
    }
    SPDK_ERRLOG("Failed To Add Program\n");
    return -1;
}

int spdk_hlsacccompute_del_program(struct spdk_hlsacccompute_dev *dev,
                                   int program_id)
{

    if (dev->program_list[program_id] != NULL)
    {
        dev->program_list[program_id] = NULL;
    }
    return 0;
}

int spdk_hlsacccompute_lookup_program(struct spdk_hlsacccompute_dev *dev,
                                      struct spdk_hlsacccompute_program **program,
                                      int pind)
{
    if (dev->program_list[pind] == NULL)
    {
        return -1;
    }
    else
    {
        *program = dev->program_list[pind];
        return 0;
    }
}


int spdk_hlsacccompute_add_program_sw_data(struct spdk_hlsacccompute_program **program,
                                           int size)
{
    if(size!=0){
        //(*program)->software_data = spdk_malloc(size,PAGE_SIZE,NULL,SPDK_ENV_SOCKET_ID_ANY,SPDK_MALLOC_SHARE);
        static atomic_uint_least32_t id = 0;
        atomic_uint_least32_t val = atomic_fetch_add(&id, 1);
        (*program)->software_data_size = size;
        (*program)->software_data_id = val;
        char name[30];
        sprintf(name, "acc_lib_%d.so", val);
        int shm_fd = shm_open(name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IXUSR);
        if (shm_fd == -1)
        {
            SPDK_ERRLOG("Failed to create shared memory\n");
            return -1;
        }
        if (ftruncate(shm_fd, size) != 0)
        {
            SPDK_ERRLOG("Failed to create shared memory size\n");
            return -1;
        }
        char *address;
        address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        (*program)->software_data = address;
        if ((*program)->software_data == NULL)
        {
            return -1;
        }
    }else{
        char path[100];
        //SPDK_NOTICELOG("BEGIN TO LOAD\n");
        sprintf(path,"/dev/shm/acc_lib_%d.so", (*program)->software_data_id);
        void* handle = dlopen(path,RTLD_NOW|RTLD_LOCAL);
        if(handle!=NULL){
            (*program)->data_in_write = dlsym(handle, "data_in_write");
            (*program)->data_out_read = dlsym(handle, "data_out_read");
            (*program)->context_write = dlsym(handle,"context_write");
            (*program)->run = dlsym(handle, "run");
            (*program)->last = dlsym(handle,"data_last");
            (*program)->get_data_size = dlsym(handle,"get_data_size");
        
        if (!((*program)->data_in_write)||!((*program)->data_out_read)||!((*program)->context_write)||!((*program)->run)||!((*program)->last))
        {
            SPDK_ERRLOG("Failed to lookup func\n");
            char* error = dlerror();
            if (error != NULL) {
                SPDK_ERRLOG("dlopen error: %s\n", error);
                munmap((*program)->software_data,size);
                (*program)->software_data = NULL;
            }
        }
        }else{
            SPDK_ERRLOG("Failed to open handle\n");
            char* error = dlerror();
            if (error != NULL) {
                SPDK_ERRLOG("dlopen error: %s\n", error);
                munmap((*program)->software_data,size);
                (*program)->software_data = NULL;
            }
        }
    }
   
    return 0;
}

static int spdk_hlsacccompute_del_program_sw_data(struct spdk_hlsacccompute_program **program)
{
    if ((*program)->software_data == NULL)
    {
        return -1;
    }
    munmap((*program)->software_data, (*program)->software_data_size);
    char name[30];
    sprintf(name, "acc_lib_%d", (*program)->software_data_id);
    int ret = shm_unlink(name);
    return ret;
}

int spdk_hlsacccompute_get_program_container(struct spdk_hlsacccompute_program **program)
{
    *program = spdk_zmalloc(sizeof(struct spdk_hlsacccompute_program), 2, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    return 0;
}

int spdk_hlsacccompute_release_program_container(struct spdk_hlsacccompute_program **program)
{
    int ret = 0;
    ret = spdk_hlsacccompute_del_program_sw_data(program);
    spdk_free(((void *)(*program)));
    return ret;
}
SPDK_LOG_REGISTER_COMPONENT(hlsacc);
