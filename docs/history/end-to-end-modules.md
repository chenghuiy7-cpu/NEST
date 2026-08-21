# CSD 加密到远端 HPU 计算端到端模块说明

## 1. 已跑通流程的边界

目前已经在实机上跑通并验证的完整数据通路为：

```text
132 服务器                                      129 服务器
┌───────────────────────────────┐               ┌──────────────────────────┐
│ x86 Host / QEMU               │               │ TCP 服务                 │
│                               │               │   协议解析与参数校验     │
│ SUDA Host 应用                │   TCP 密文    │   CPU-LWE -> HPU 格式    │
│   创建/控制 SLM               │──────────────>│   V80 HPU 执行 ADDS      │
│   启动 FPGA LWE 加密          │               │   HPU -> CPU-LWE         │
│   回读并整理密文              │<──────────────│   响应序列化             │
│   封装 LWERPC01               │  结果密文     └─────────────┬────────────┘
│   接收并解密验证              │                             │ PCIe
└──────────────┬────────────────┘                             ▼
               │ PCIe/QDMA                              ┌──────────┐
               ▼                                        │ V80 HPU  │
┌───────────────────────────────────────────────────┐   └──────────┘
│ Fidus CSD                                         │
│                                                   │
│ SSD -> input SLM -> HLS lwe_encrypt -> output SLM│
│        ARM/SPDK runtime + FPGA accelerator fabric │
└───────────────────────────────────────────────────┘
```

验证结果包括：

- SSD 中的连续 u8 明文能够进入 CSD 的 input SLM；
- FPGA `lwe_encrypt` 能够生成可解密的 2048 维 Big-LWE radix 密文；
- 1B 和 128B 批量密文能够通过 TCP 发送到 129；
- 129 的真实 V80 HPU 能够执行 u8 同态标量加法 `ADDS +1`；
- 返回 132 的结果密文可使用原密钥解密，128 个 u8 的结果全部正确。

这里所说的“已跑通版本”是 CPU-LWE 基线：FPGA 输出经过 132 Host 去除物理 padding、
恢复逻辑 Big-LWE，并由 129 Host 转换为 HPU 输入表示。2026-08-13 开始开发的
HPU-native 直出版本已经通过 CSIM、综合和软件单元测试，但尚未完成新 BOOT 的整板实机
端到端验证，因此展示实验结果时应将两版分开。

## 2. CSD 侧模块

### 2.1 SSD 与存储命名空间

SSD 保存待加密的连续 u8 明文。Host 通过 SSD namespace、起始 LBA 和明文字节数指定
输入位置。NVMe/SLM 搬运以 4KB LBA 为最小单位，因此实际搬运长度为：

```text
ssd_copy_bytes = ceil(plaintext_bytes / 4096) * 4096
```

FPGA 只消费 `plaintext_bytes` 个有效字节，LBA 尾部的对齐数据不参与加密。

### 2.2 ARM 侧 `nvmf_tgt` 与 SPDK runtime

Fidus CSD 的 ARM 核运行修改后的 SPDK `nvmf_tgt`。它负责接收 132 Host 发来的 NVMe
计算命令，并管理 SLM、算子程序和 FPGA 执行请求。

主要代码：

```text
device/platform/software_stack/nf_spdk/lib/nvmf/mcdma.c
device/platform/software_stack/nf_spdk/lib/hlsacccompute/hlsacccompute.c
device/platform/software_stack/nf_spdk/lib/axi_dma/axi_dma.c
```

### 2.3 MCDMA/AXI DMA 数据搬运模块

MCDMA transport 接收 QDMA 传入的命令和数据，解析请求状态，并组织 Host 内存、CSD
设备内存和 FPGA AXI Stream 之间的数据搬运。AXI DMA channel 实际提交和回收 FPGA
输入、输出 stream 请求。

它承担的典型搬运包括：

- SSD 数据复制到 input SLM；
- input SLM 数据送入 HLS 算子；
- HLS 输出写入 output SLM；
- output SLM 数据返回 132 Host。

### 2.4 SLM

SLM 是 SUDA runtime 暴露的设备侧共享内存 namespace，不应简单等同于 Host DRAM。
input SLM 保存从 SSD 搬来的明文，output SLM 保存 FPGA 产生的密文。Host 使用 memory
range set 把两个 SLM 与一次计算任务绑定。

### 2.5 `hlsacccompute` 调度器

该模块管理算子池、program、operator 和 DMA channel。它负责：

- 加载和激活 Host 定义的计算 program；
- 检查所需算子是否可用；
- 为请求分配 `lwe_encrypt` 算子和输入/输出 channel；
- 搬入 context；
- 启动 FPGA 算子并在结束后返回结果字节数。

### 2.6 Accelerator framework 与算子池

FPGA block design 中的 accelerator framework 连接 QDMA/MCDMA 数据流、算子控制器和
HLS 算子池。`lwe_encrypt` 已作为一种 operator type 集成到算子池。

### 2.7 HLS `lwe_encrypt` 算子

算子对每个 u8 做以下工作：

1. 将 u8 拆成 4 个 2-bit radix block；
2. 根据 TFHE 编码参数形成 encoded plaintext；
3. 对每个 block 生成一个 2048 维 Big-LWE；
4. 计算 `body = dot(mask, secret_key) + encoded + noise`；
5. 通过 AXI Stream 输出 mask、body 和结束包。

旧的已跑通布局中，每个 u8 的逻辑密文大小为：

```text
4 * (2048 + 1) * 8B = 65568B
```

FPGA stream/SLM 物理布局还包含 64B 对齐 padding，因此每个 u8 实际写入 65792B。

算子 context 包含明文数量、LWE 参数、随机种子/nonce 和保存的 2048 维二进制私钥。
私钥不会通过 TCP 发送到 129，但为了完成客户端加密，它会由 132 Host 作为 context
传入 CSD 算子。

## 3. 132 Host 侧模块

### 3.1 QEMU 与 VFIO/QDMA 设备透传

QEMU guest 运行测试应用和 SUDA Host 驱动。宿主机通过 VFIO 将目标 Fidus PCIe
function 透传给 QEMU，使 guest 能直接使用 QDMA endpoint。

### 3.2 `nvmq` 与 QDMA 内核驱动

Host 内核的 `nvmq` 驱动接收用户态 NVMe passthrough 命令，处理地址映射和请求队列，
再通过 QDMA H2C/C2H queue 与 CSD 通信。

主要代码：

```text
host/drivers/nvmq/
```

### 3.3 `libnvme` / SUDA Host API

融合应用通过 `libnvme` 调用以下 SUDA API：

- 创建 input/output SLM；
- 执行 SSD 到 input SLM 的 `nvme_slm_copy`；
- 创建 memory range set；
- 加载、激活和执行 HLS program；
- 从 output SLM 回读密文；
- 停用程序并释放 CSD 资源。

### 3.4 融合应用 `vscode-lwe-encrypt-remote-offload`

这是 132 侧的总控程序，负责串起 CSD 加密与远端 HPU 计算：

```text
host/applications/vscode-lwe-encrypt-remote-offload/
```

主要职责为：

1. 解析 SSD LBA、明文批量、远端地址、标量等参数；
2. 读取 Big-LWE 私钥并构造 HLS context；
3. 调用 SUDA API 完成 SSD -> SLM -> FPGA -> SLM；
4. 将 output SLM 密文回读到 132 Host 内存；
5. 对密文布局和本地解密结果做旁路正确性验证；
6. 构造 RPC metadata，发送密文到 129；
7. 接收远端 HPU 结果并再次解密验证；
8. 可选保存最终结果，benchmark 模式下输出各阶段延迟。

融合流程不会把 FPGA 中间密文落盘。中间密文只进入 Host 内存，然后立即发送；最终结果
才根据参数选择是否写入文件。

### 3.5 CPU-LWE 物理布局整理

已跑通基线中，Host 从每个 64B 对齐的 FPGA 物理块中提取有效 mask/body，得到 tfhe-rs
自然顺序的逻辑 Big-LWE。该步骤还会使用同一私钥检查密文是否解出原始 u8。

这一步是 HPU-native 直出版本计划消除的主要 Host 数据格式处理。

### 3.6 `LWERPC01` 协议客户端

协议模块负责在 TCP 上发送固定头部、metadata 和密文 payload。metadata 描述：

- request ID 和操作码；
- u8 数量与每个 u8 的 radix block 数；
- Big-LWE dimension；
- message/carry/padding width；
- delta 和密文字数；
- 远端 `ADDS` 使用的 scalar。

网络请求不携带明文参考和私钥。协议 v2 还接收 129 返回的计时尾帧，用于端到端消融。

### 3.7 本地结果验证

132 保存能够解密结果的 ClientKey/Big-LWE 私钥。远端返回密文后，本地将其解密并检查：

```text
result[i] == plaintext[i] + scalar mod 256
```

这证明远端操作是在密文上完成，而不是由 129 获得明文后计算。

## 4. 129 远端服务器模块

### 4.1 `hpu_lwe_remote_server`

该 Rust 服务监听 `0.0.0.0:19090`，接收 132 的密文请求，调用真实 V80 HPU，再返回
结果密文：

```text
hpu/tfhe-rs/tfhe/examples/hpu/lwe_remote_server.rs
```

服务启动时会初始化 HPU、加载 compressed server key，并确保硬件 reload policy 为
`never`，避免服务启动时重载板卡导致服务器失联。

### 4.2 协议解析与 metadata 校验

`protocol.rs` 负责读写 `LWERPC01` frame，并限制最大 payload。服务端会检查 LWE
dimension、radix block 数、message/carry width、delta 和密文字数是否与 psi64/V80
配置一致，防止错误格式进入 HPU runtime。

### 4.3 密文桥接模块

已跑通 CPU-LWE 基线使用 `bridge.rs` 完成：

```text
网络 u64 words
  -> LweCiphertextOwned
  -> shortint Ciphertext
  -> RadixCiphertext
  -> HpuRadixCiphertext
```

这一过程补齐 tfhe-rs 所需 metadata，并把 CPU 自然顺序 Big-LWE 转为 HPU memory cut
格式。它只改变密文表示，不执行解密。

### 4.4 tfhe-rs HPU backend

HPU backend 管理 V80 设备、固件、命令队列和设备内存。服务端加载与客户端密钥配套的
`CompressedServerKey`，但不会加载 ClientKey，也不能解密用户数据。

### 4.5 V80 HPU 同态计算

当前远端操作为 u8 `ADDS`：

```rust
let hpu_output = hpu_input + scalar;
hpu_output.wait();
```

加法命令异步入队，`wait()` 等待 HPU 完成并把结果同步回 129 Host。当前测得的
`hpu_wait_sync` 包含输入同步、HPU 执行等待和 Device-to-Host 同步，不能严格称为纯
RTL 计算时间。

### 4.6 结果转换与响应

HPU 输出被转换回 CPU `RadixCiphertext`，序列化为逻辑 Big-LWE words，再通过 TCP 返回
132。服务端同时返回 request receive、decode、HPU prepare、enqueue、wait/sync、output
convert、encode 和 response send 等计时数据。

### 4.7 `echo` 消融路径

`echo` 使用与 `adds` 相同大小的请求和响应，但不调用 HPU，直接返回输入密文。它用于
测量 TCP、协议、socket 缓冲、内存复制和调度的组合开销。

## 5. 数据面与控制面

### 5.1 数据面

```text
SSD明文
 -> input SLM
 -> FPGA AXI Stream
 -> lwe_encrypt
 -> output SLM
 -> QDMA C2H
 -> 132 Host密文缓冲区
 -> TCP socket
 -> 129 Host密文缓冲区
 -> V80 HPU设备内存
 -> HPU计算结果
 -> 129 Host
 -> TCP socket
 -> 132 Host结果缓冲区
```

### 5.2 控制面

```text
132应用
 -> 创建SLM和memory range
 -> load/activate/execute HLS program
 -> 指定明文数量、context和算子类型
 -> RPC指定operation、scalar和metadata
 -> 129服务校验请求
 -> tfhe-rs HPU backend入队ADDS命令并等待完成
 -> 两端返回状态和计时信息
```

## 6. 密钥和信任边界

| 位置 | 持有的密钥 | 用途 |
|---|---|---|
| 132 Host | ClientKey/Big-LWE 私钥 | 构造 CSD 加密 context、最终解密验证 |
| Fidus CSD | 单次任务 context 中的 Big-LWE 私钥 | FPGA 客户端加密 |
| 129 Host/HPU | CompressedServerKey | 执行同态运算，不能解密 |
| TCP 请求 | 不含私钥和明文参考 | 只包含 metadata、操作参数和密文 |

当前 TCP 协议没有提供 TLS、身份认证或消息完整性保护。实验网络可以使用，若扩展为跨
不可信网络的系统，需要增加 TLS/mTLS、请求认证、防重放和 payload 完整性校验。

## 7. 展示时可用的一分钟讲解

本系统将客户端加密下沉到 Fidus 可计算存储设备。132 服务器通过 SUDA API 指定 SSD
中的连续 u8 明文，CSD runtime 将数据搬到 SLM，再调度 FPGA 上的 `lwe_encrypt` 算子
生成 2048 维 Big-LWE radix 密文。密文通过 QDMA 回到 132 Host 内存，由融合程序封装
成 `LWERPC01` 请求并通过 TCP 发送到 129。129 只持有 ServerKey，不能解密数据；它将
密文导入真实 V80 HPU，执行同态 `ADDS +1`，然后把结果密文返回 132。最后 132 使用原
ClientKey 解密，已经验证 1B 和 128B 批量的所有结果都正确。整个链路证明了 SSD 数据
可以在不向远端暴露明文和私钥的情况下，完成 CSD 加密、网络传输和真实 HPU 密文计算。

## 8. 后续 HPU-native 直出版本

后续版本将 mask bit-reversal、PC0/PC1 分发和 slot padding 下沉到 HLS，使 output SLM
中的密文已经是 HPU 原生物理布局。这样可以删除 132 和 129 上的 CPU-LWE 到 HPU 格式
重排，但仍然需要：

- output SLM 回读或直接网络发送机制；
- RPC header/metadata；
- 129 侧协议解析和 HPU 内存注册；
- HPU 命令入队、等待和结果返回。

该版本目前不能作为“完整实机已跑通”结果展示，完成新 BOOT 上板和 operation 2 的真实
HPU 端到端验证后，才可以替换本文件中的 CPU-LWE 基线流程。
