# SUDA LWE 加密算子原型开发与测试总结

## 1. 文档目的

本文总结 2026 年 6 月 9 日至 2026 年 7 月 13 日期间，在 SUDA 可计算存储平台上开发、集成和测试 `lwe_encrypt` HLS 原型算子的过程。内容重点包括：算法边界、数据布局、软硬件接口、主要故障、根因、修复方法、验证证据及尚未完成的工作。

本文可作为后续论文撰写、实验复现和工程迭代的基础材料。原始逐日操作记录仍保存在 `suda/logs/`，本文对其中已经被后续实验证实或推翻的中间判断进行了整理。

## 2. 当前结论

截至 2026 年 7 月 13 日，`lwe_encrypt` 原型已经完成至少一次板上端到端功能验证：

1. Host 从板卡 SSD 的 namespace 1、LBA 65536 读取测试明文。
2. 明文经 SUDA SLM 和 AXIS 数据通路进入 FPGA 上的 `lwe_encrypt` 算子。
3. 算子将一个 `u8` 拆成 4 个 2-bit radix block，并生成 4 个 2048 维 Big-LWE 密文。
4. 密文写入 output SLM，Host 成功读回并保存为 `LWEHLS01` 格式。
5. Host 使用与 tfhe-rs ClientKey 同源的 Big-LWE secret key 按 LWE 公式解密，恢复出原始明文 `0x3b`，即十进制 59。
6. 板上 dump 被包装为 tfhe-rs `RadixCiphertext` 后，通过正式转换路径送入 HPU mockup 执行 `ADDS(1)`；结果密文解密为 60，mockup 报告执行了 11 次 PBS。

最终一次成功输出的关键字段为：

```text
lwe_encrypt FPGA execution passed
decrypted_count=1
decrypted_first_u8=0x3b (59)
exec_result=65792 expected_logical_bytes=65568 physical_bytes=65792
output_layout=64-byte-padded elapsed_ms=5.781
host_key_decrypt_checked=yes
```

这证明了当前原型在固定参数、固定密钥和单个 `u8` 输入下，已经打通“SSD -> SLM -> FPGA LWE 加密 -> SLM -> Host 解密”的功能链路。

该结果还不能证明以下内容：

- 当前随机数和噪声采样达到 tfhe-rs 的密码学安全性或与其逐位一致；
- HLS C/RTL COSIM 已经通过；
- 整体设计已经满足严格的 implementation timing signoff；
- 输出密文无需额外适配即可直接进入真实 HPU FPGA 硬件；
- 多输入、长时间、并发和压力场景已经稳定通过；
- `5.781 ms` 可以作为严谨的算子性能或加速比结论。

因此，当前成果应准确表述为“完成了面向 HPU 参数的 Big-LWE 加密 HLS 原型及一次板上端到端可解密性验证”。

## 3. 算法边界与参数选择

### 3.1 LWE、Big-LWE 与 GLWE 的关系

HPU 参数文件为：

```text
<tfhe-rs>/mockups/tfhe-hpu-mockup/params/tuniform_64b_pfail128_psi64.toml
```

关键参数如下：

| 参数 | 数值 | 含义 |
| --- | ---: | --- |
| `lwe_dimension` | 879 | Small-LWE 维度，主要与 keyswitch 输入侧相关 |
| `glwe_dimension` | 1 | GLWE 多项式向量维度 |
| `polynomial_size` | 2048 | 每个 GLWE 多项式的系数数量 |
| Big-LWE dimension | 2048 | `glwe_dimension * polynomial_size` |
| `message_width` | 2 | 每个 shortint/radix block 保存 2 bit 消息 |
| `carry_width` | 2 | 每个 block 预留 2 bit carry |
| `ciphertext_width` | 64 | Torus 密文系数宽度为 64 bit |
| `glwe_noise_distribution` | `TUniformBound=17` | 当前 Big-LWE 原型使用的噪声边界参数 |

当前算子的直接输出面向 HPU 使用的 Big-LWE 边界，因此 mask dimension 固定为 2048，而不是 TOML 中的 Small-LWE dimension 879。这里的 Big-LWE 仍然是普通 LWE 密文，数据形式为一维的 `[mask..., body]`，不是 GLWE 密文；它的 secret key 来自 GLWE secret key 的展平形式。

### 3.2 一个 u8 为什么拆成 4 个密文

tfhe-rs radix integer 使用多个 shortint block 表示一个整数。当前 message width 为 2，因此一个 8-bit 明文需要：

```text
8 / 2 = 4 个 radix block
```

四个 block 按最低有效位优先排列：

```text
block_i = (clear_u8 >> (2 * i)) & 0x3, i = 0, 1, 2, 3
```

由于 message width、carry width 和 padding bit 分别为 2、2 和 1，编码缩放因子为：

```text
delta_log2 = 64 - (2 + 2 + 1) = 59
delta = 2^59
encoded_i = block_i * delta
```

### 3.3 LWE 核心计算

对每个 radix block，算子生成一个 2048 维 LWE 密文：

```text
mask = (a_0, a_1, ..., a_2047)
body = dot(mask, secret_key) + encoded + noise mod 2^64
ciphertext = [a_0, a_1, ..., a_2047, body]
```

解密时计算：

```text
phase = body - dot(mask, secret_key) mod 2^64
```

再根据 `delta=2^59` 对 phase 取整和解码，即可恢复 2-bit block，最后重组得到原始 `u8`。

## 4. 算子接口与数据布局

### 4.1 HLS 顶层接口

算子顶层函数为：

```cpp
void lwe_encrypt(
    Acc_Data &data_in,
    Acc_Data &data_out,
    ap_uint<512> context[256]);
```

其中：

- `data_in`：512-bit AXIS 输入流；
- `data_out`：512-bit AXIS 输出流；
- `context`：算子配置和 secret key 所在的 BRAM 接口。

### 4.2 Context ABI

SUDA 的 `AccContext` 在 HLS 可见的 BRAM 起始位置保留了 192B，即 3 个 512-bit word，用于 runtime 私有状态。因此：

```text
HLS context[0..2]  = SUDA runtime 私有区域
HLS context[3]     = lwe_encrypt 配置字
HLS context[4...]  = 打包后的 2048-bit Big-LWE secret key
```

Host 侧传入的是 `AccContext.static_data` 的相对布局：配置从 offset 0 开始，密钥从 offset 64 开始。OperatorController 映射后，HLS 分别在 `context[3]` 和 `context[4]` 读取它们。

配置字包括：

| bit 范围 | 参数 | 当前值或作用 |
| --- | --- | --- |
| `[31:0]` | `mask_dimension` | 2048 |
| `[63:32]` | `request_count` | 要加密的 u8 数量 |
| `[95:64]` | `input_mode` | 2，表示 u8 radix 模式 |
| `[127:96]` | `noise_mode` | 0 为内部原型采样，1 为输入噪声 |
| `[159:128]` | `noise_bound_log2` | 17 |
| `[319:256]` | `delta` | `2^59` |
| `[383:320]` | `rng_seed` | mask/noise PRNG seed |
| `[447:384]` | `nonce` | 每次运行的 nonce |

密钥文件包含 2048 个二进制系数，每个系数以一个字节保存；写入 context 时压缩为每个系数 1 bit。当前保存密钥中有 1015 个系数为 1。

### 4.3 输入格式

在 u8 radix 模式下，每个 64B 输入 beat 的低 8 bit 保存一个明文：

```text
data_in[7:0] = clear_u8
```

当前测试从 SSD 复制 256 个 4KB LBA，共 1MiB 到 input SLM，但 `encrypt_count=1` 时只把第一个 64B 数据 beat 作为计算输入，实际加密其中 byte 0。较大的 SSD 拷贝范围用于贴合 SUDA 既有示例的数据搬运路径，不代表 1MiB 数据都被加密。

### 4.4 输出格式与字节数

每个 Big-LWE 密文包含：

```text
2048 个 mask u64 + 1 个 body u64 = 2049 个 u64
```

逻辑大小为：

```text
2049 * 8 = 16392B
4 * 16392 = 65568B
```

AXIS 宽度为 64B。每个密文需要 257 个 AXIS beat，最后一个 beat 只有 body 有效，但当前 OperatorController 和输出布局按完整 64B 物理 beat 处理：

```text
ceil(16392 / 64) * 64 = 257 * 64 = 16448B
4 * 16448 = 65792B
```

因此：

- `expected_logical_bytes=65568`：去掉每个密文尾部 padding 后的 tfhe-rs `[mask..., body]` 数据量；
- `physical_bytes=65792`：FPGA 到 output SLM 的 AXIS 物理 payload 数据量；
- `output_slm_bytes=69632`：17 个 4KB page，用于容纳 payload、64B 任务结束 beat 及页对齐余量。

SLM 是 SUDA runtime 管理的设备内存对象和命名空间抽象，不应简单等同于 FPGA 片上 BRAM 或某一块固定的“FPGA DRAM”。Host 通过 NVMe SLM API 创建、引用和读取它，ARM runtime 再根据 memory range 和物理地址组织 DMA 数据搬运。

### 4.5 任务结束协议

SUDA 使用独立的无效数据 beat 表示任务结束：

```text
payload beat: TUSER=0x00, TLAST=0
finish beat:  TUSER=0xff, TLAST=1, size=64B
```

`lwe_encrypt` 必须先输出全部 ciphertext payload，再转发输入侧的 finish beat。结束 beat 不属于密文，也不应参与加密或计入 `request->result`。

## 5. 开发与集成内容

### 5.1 HLS 算子

主要文件：

```text
device/operators/hls/lwe_encrypt/lwe_encrypt.hpp
device/operators/hls/lwe_encrypt/lwe_encrypt.cpp
device/operators/hls/lwe_encrypt/test.cpp
device/operators/hls/lwe_encrypt/test_deep.cpp
device/operators/hls/lwe_encrypt/run_hls.tcl
```

实现内容包括：

- 2048 维 Big-LWE mask 生成；
- 二进制 secret key 点积；
- clear、encoded 和 u8 radix 三种输入模式；
- 内部原型噪声采样或外部噪声输入；
- 64-bit 模 `2^64` 的 body 计算；
- `[mask..., body]` 输出及 512-bit AXIS 打包；
- SUDA finish beat 转发；
- context 参数错误时输出 `LWEERROR` 错误包，避免 runtime 永久等待。

### 5.2 密钥生成与 tfhe-rs 验证工具

密钥和验证程序位于：

```text
hpu/tfhe-rs/tfhe/examples/hpu/export_lwe_encrypt_key.rs
hpu/tfhe-rs/tfhe/examples/hpu/verify_lwe_encrypt_output.rs
suda/device/operators/hls/lwe_encrypt/testdata/psi64_shortint_ks32_client_key.bincode
suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin
suda/device/operators/hls/lwe_encrypt/testdata/psi64_key_manifest.txt
```

保存的 `ClientKey` 与展平后的 Big-LWE secret key 来自同一次 tfhe-rs 密钥生成，保证 HLS 加密和 tfhe-rs 解密使用同一组密钥材料。

### 5.3 算子池与 Block Design

生成的 Verilog 被加入：

```text
device/platform/basic_shell/nf-csd/shell/virt_one_drive/fpga/sources/hlsaccframework/lwe_encrypt/
```

Block Design 中增加了：

- `lwe_encrypt_0`；
- `OperatorController_2`；
- `static_var_bram2`；
- operator type id 2、slot id 2；
- AXIS control/context/data switch 端口和路由；
- 对应时钟、复位、BRAM 和 `ap_start/ap_done` 连接。

ARM runtime 的 `config.json` 中注册：

```json
{
  "operator_type_id": 2,
  "operator_type_name": "lwe_encrypt",
  "operator_inport_num": 1,
  "operator_outport_num": 1,
  "esti_executed_times": 200,
  "worse_executed_times": 1000,
  "bram_size": 2048,
  "slot_id": 2
}
```

### 5.4 Host 应用

相关程序为：

```text
host/applications/vscode-lwe-encrypt-data-gen/
host/applications/vscode-lwe-encrypt-offload/
```

数据生成程序将固定的 4KB 随机 u8 测试数据写入 SSD，并执行 readback 校验。offload 程序负责创建 input/output SLM、复制 SSD 数据、创建 memory range set、加载并激活 FPGA program、提交 context、读取 output SLM、解密验证和导出密文。

## 6. 主要问题、根因与解决方法

| 阶段 | 现象 | 根因 | 解决方法 | 验证 |
| --- | --- | --- | --- | --- |
| 算法边界 | 不确定应使用 879 还是 2048 维 | 混淆了 Small-LWE、Big-LWE 和 GLWE | 追踪 tfhe-rs/HPU 参数链路，选择展平 GLWE key 对应的 2048 维 Big-LWE 边界 | tfhe-rs ClientKey 可解密 HLS 风格 Big-LWE |
| Radix 表示 | 不确定一个 u8 应产生几个密文 | 每个 shortint block 仅保存 2-bit message | 在 HLS 内部拆成 4 个 radix block，最低有效 block 优先 | C++ testbench 重组并恢复 u8 |
| 密钥输入 | 算子不知道如何获得 ClientKey | FPGA 核心只需要 LWE secret key，不需要整个高层 ClientKey 对象 | 用 tfhe-rs 导出同源 Big-LWE secret key，经 context BRAM 传入 | key manifest 和解密验证通过 |
| HLS 启动 | Host 卡在 FPGA execution | `ap_start` 可能早于 TX 数据到达，`data_in.empty()` 使算子提前返回 | 改为阻塞式 `data_in.read()` | 算子能够等待并消费实际输入流 |
| 输出结束 | runtime 等不到正确完成或提前完成 | payload 和 `TUSER=0xff` finish 标志混用 | payload 统一 `TUSER=0/TLAST=0`，最后单独转发 finish beat | ARM 可观察到 payload 后的结束 completion |
| 错误路径 | 配置错误时 Host 永久等待 | HLS 直接 return，没有结束数据 | 输出带 `LWEERROR` magic 和错误码的结束包 | 错误从“卡死”转为可诊断错误包 |
| Context 地址 | 只收到 `LWEERROR/code=1`，没有密文 | HLS 从 `context[0]` 读配置，但 SUDA 前 3 个 word 是私有区 | 配置改读 `context[3]`，密钥从 `context[4]` 起读 | runtime staged/static 都为 `[2048,1,2,0,17]`，RTL 地址也与之匹配 |
| 输入 DMA 长度 | `DATA USED IS BIGGER THAN NEEDED` | 最后一段实际数据不足 4KB，却按完整 4KB descriptor 提交 | 最后一段 TX 使用实际剩余长度 | execution 从卡死推进到正常完成 |
| output SLM read | `Failed to allocate spdk_axi_dma_io` | 一个 software IO 对象关联多个 RX BD，完成和释放生命周期不清晰，且大请求增加资源压力 | RX 路径按一个 BD 对应一个 software IO；Host 按 4KB 分块读取并限制 EINTR 重试 | ARM 不再出现该分配错误，Host 可逐页读取 |
| result 为 0 | Host 显示 execution completed，但 output read 卡住 | HLS 实际输出的是错误 finish 包，没有 ciphertext payload | 增加 context 和 RX 包内容诊断，定位 `LWEERROR`，再修复 context 偏移 | 修复后出现连续 ciphertext completion |
| result 少 256B | ARM 已收到大量 payload，但 `result=65536`，预期 65792 | DMA 把最后 256B payload 和 64B finish beat 合并为 320B completion；旧 runtime 见 `TUSER=0xff` 后丢弃整个 completion | finish completion 记账为 `320-64=256B`，只排除结束 beat | `result_bytes=65792`，Host 成功读回并解密 |
| QDMA 初始化 | 驱动报告 invalid config BAR | QEMU 最初直通了另一用户使用的 XDMA 卡，BAR signature 为 `0x1fc0` | 区分三张 903f 设备，改用目标 QDMA 卡；QDMA config signature 应为 `0x1fd3` | QDMA driver 识别 EQDMA Soft IP、512 queues |
| ARM 程序 | `Exec format error` | 将 x86_64 `nvmf_tgt` 放到 AArch64 ARM 上执行 | 在 x86 主机交叉编译 AArch64 版本 | `file` 显示 ARM aarch64 ELF |
| ARM 动态库 | 缺少 `libssl.so.3`、`libcrypto.so.3` | 交叉编译环境和板上 rootfs 动态库版本不一致 | 补齐匹配的 AArch64 OpenSSL 3 运行库或统一 sysroot | `ldd` 不再显示 not found |
| HLS 工具环境 | 2022.2 settings 指向不存在的 `/mnt` 路径 | SUDA toolset 配置与本机安装不一致 | 使用已安装的 Vitis/Vivado 2020.2，并处理 HLS 2020.2 兼容项 | CSIM、CSYNTH 和 IP export 完成 |
| IP 导出 | `core_revision` 日期值溢出 | Vitis HLS 2020.2 的 revision 字段限制 | `run_hls.tcl` fallback 使用 revision 1 | 成功生成 `lwe_encrypt_ip.zip` |
| COSIM | XSIM 报 `unknown error occurred` | C testbench 和 RTL snapshot 已生成，但 Vivado 2020.2 simulator runtime 启动失败 | 已保存日志，尚未彻底解决 | 不能声称 COSIM 通过 |
| 整板启动 | 部分新镜像上板后 ARM 无法启动 | 与未 clean 的整板时序或构建产物有关，但缺少足够证据归因到单一原因 | 每次替换前备份 BOOT/dtb，校验 SHA-256，保留串口和已知可用恢复镜像 | 使用读卡器和已知可用镜像恢复板卡 |

## 7. 关键故障的跨层分析

### 7.1 “FPGA execution completed”不等于“密文已经产生”

SUDA runtime 将收到 `TUSER=0xff` 视为任务结束。早期错误路径中，HLS 因 context 无效直接输出 64B `LWEERROR` 结束包。runtime 因此可以完成 execute 命令，但 `request->result=0`，output SLM 中没有有效 ciphertext payload。

所以 execute API 返回成功只表示任务协议已经结束，不代表算法输出长度和内容正确。Host 必须同时检查：

```text
result_bytes == expected physical payload bytes
```

并继续执行密文结构和解密验证。

### 7.2 最后一个 completion 为什么是 320B

预期物理 payload 是 65792B。DMA 先完成 16 个 4096B payload，共 65536B，剩余：

```text
65792 - 65536 = 256B payload
```

随后还有一个 64B finish beat。硬件把二者聚合为同一个 RX completion：

```text
256B payload + 64B finish = 320B completion, TUSER=0xff
```

旧 runtime 根据 completion 最后一个 beat 的 `TUSER` 判断结束，却没有累计前面的 256B payload，因此只返回 65536B。最终修复在 `mcdma.c` 中把带结束标志的 completion 按以下方式记账：

```text
payload_bytes = completion_bytes - 64
```

结束 beat 仍负责结束任务，但不计入密文结果长度。

### 7.3 为什么 Blowfish 能过而 LWE 曾经不能过

Blowfish 输出量和 packet 边界更接近 SUDA 原有假设，没有同时触发 LWE 的以下特殊组合：

- 一个 u8 扩展为 4 个大型密文；
- 每个 16392B 逻辑密文都需要单独补齐到 64B beat；
- output SLM 跨越 17 个 4KB page；
- 最后 payload 与 finish beat 被 DMA 合并到一个 completion；
- LWE 还需要额外的 context 和 2048-bit secret key。

因此 Blowfish 通过只能证明基础 SUDA 数据通路可用，不能证明新增 LWE 算子的 context ABI、输出长度和 finish completion 处理正确。

## 8. 验证过程与结果

### 8.1 软件公式和小维度测试

使用小维度 LWE 先验证：

- 64-bit wrapping arithmetic；
- `[mask..., body]` 顺序；
- `body-dot(mask,key)` 可恢复 encoded plaintext；
- 外部噪声模式和内部原型噪声范围。

### 8.2 2048 维 Big-LWE C++ 测试

`test_deep.cpp` 使用保存的 2048 维 key，对 clear block 和 u8 radix 模式进行解密检查。一次通过记录为：

```text
small encoded-input LWE reference passed. dimension=16 ciphertext_words=34 decrypt_checked=yes
HPU Big-LWE clear-input saved-key reference passed. dimension=2048 ciphertext_words=8196 decrypt_checked=yes
loaded saved psi64 Big-LWE secret key. dimension=2048 ones=1015
HPU u8-radix saved-key reference passed. u8_inputs=4 radix_blocks=16 ciphertext_words=32784 decrypt_checked=yes
```

### 8.3 tfhe-rs ClientKey 验证

本地 HLS 测试输出被包装为 shortint ciphertext 后，调用 tfhe-rs `ClientKey.decrypt()` 解密各 2-bit block，并重组 u8。这一步比 testbench 中手写 `body-dot(mask,key)` 更接近真实 tfhe-rs 解密链路。

最终板上测试程序打印的 `host_key_decrypt_checked=yes` 表示 Host 使用同源 Big-LWE secret key 完成了 `body-dot(mask,key)` 解密检查。随后新增的 `hpu_lwe_encrypt_mockup_test` 又将这一次板上 dump 包装为 tfhe-rs `RadixCiphertext`，调用 Rust `ClientKey.decrypt_radix()` 恢复出 59，并将其转换为 `HpuRadixCiphertext` 参与 mockup 运算。

建议复核命令：

```bash
cd "${TFHE_RS_ROOT}"
cargo run -p tfhe --features hpu \
  --example hpu_verify_lwe_encrypt_output -- \
  --client-key "${TFHE_CLIENT_KEY_FILE}" \
  --ciphertext-dump /path/to/lwe_encrypt_fpga_ciphertexts.bin
```

### 8.4 HPU mockup 同态运算验证

板上 dump 通过 `HpuRadixCiphertext::from_radix_ciphertext` 进入 HPU mockup，并执行 8-bit `ADDS(1)`：

```text
imported_u8_count=1
imported_mask_dimension=2048
cpu_radix_import_decrypt_checked=yes
input[0]=59 scalar=1 decrypted_result=60 expected=60
hpu_operation=ADDS
hpu_mockup_ciphertext_compute_checked=yes
lwe_encrypt_mockup_compatibility=passed
```

mockup 的行为模型报告为：

```text
TimeRpt { cycle: 978360, duration: 3.261ms }
InstructionKind { MemLd: 4, MemSt: 4, Arith: 21, Pbs: 11, Sync: 1 }
```

这证明 FPGA 密文已经通过 HPU mockup 的格式转换、密文加载、同态计算和结果读回链路。该时间是 300 MHz mockup 行为模型估算，不是真实 HPU 硬件实测。

### 8.5 HLS 工具验证

当前状态：

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 普通 C++ test | 通过 | 小维度、2048 维和 u8 radix 解密检查通过 |
| HLS CSIM | 通过 | `CSim done with 0 errors` |
| HLS CSYNTH | 通过 | 成功生成 RTL 和 IP zip |
| HLS COSIM | 未 clean pass | snapshot 构建后被 XSIM 2020.2 runtime 的 unknown error 阻断 |

最新 HLS 综合估算：

| 指标 | 结果 |
| --- | ---: |
| 目标周期 | 4.00 ns |
| HLS 估算周期 | 3.302 ns |
| DSP | 8 |
| FF | 13293 |
| LUT | 30649 |
| BRAM_18K | 0 |

该报告只反映 HLS IP 的估算，不代表完整 shell 的 post-route timing。

### 8.6 整板实现和时序

2026 年 7 月 10 日整板构建成功生成 bitstream 和 BOOT image，但最终 post-route timing 为：

```text
WNS=-0.009 ns
TNS=-0.009 ns
WHS=0.005 ns
THS=0.000 ns
```

即 setup timing 仍有 9 ps 负裕量，不能视为严格 timing clean。关键路径出现在 `OperatorController_2` FIFO 和 `static_var_bram2` 附近，而不是直接出现在 LWE 点积算术路径。该镜像实际能够启动并完成一次功能测试，但论文和工程验收中仍应把 timing closure 列为待完成事项。

### 8.7 板上端到端测试

测试明文文件写入 SSD 后的 readback 信息为：

```text
bytes=4096
first_u8=59
first_16_bytes=3b29dbc64dee1e0ec551d218576d2c30
fnv1a64=0xb4c5ae9d465ad514
readback_verified=yes
```

最终测试命令为：

```bash
./vscode-lwe-encrypt-offload \
  --ssd-nsid 1 \
  --ssd-lba 65536 \
  --input-lbas 256 \
  --encrypt-count 1 \
  --expect 59 \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --output lwe_encrypt_fpga_ciphertexts.bin
```

最终验证同时满足：

- execute 返回 `result_bytes=65792`；
- output SLM 的 17 个 4KB page 均可读取；
- Host 识别 `64-byte-padded` 布局；
- 四个 radix block 均能解密；
- 重组结果为 `0x3b`；
- 导出密文文件成功。

## 9. 已验证边界与未验证边界

### 9.1 已验证

- TOML 参数到 2048 维 Big-LWE 原型参数的映射；
- 一个 u8 到 4 个 2-bit radix block 的拆分；
- LWE mask/body 数学关系；
- 保存密钥的 context 传输；
- HLS CSIM 和综合；
- 算子池、Block Design 和 ARM operator registry 集成；
- SSD 到 input SLM 的数据路径；
- FPGA 输出到 output SLM 及 Host 分页读取；
- 物理 payload 长度 65792B 的 runtime 记账；
- 使用同源 key 的 Host 解密与明文恢复。
- 板上 dump 的 Rust `ClientKey.decrypt_radix()` 验证；
- `RadixCiphertext -> HpuRadixCiphertext` 正式转换；
- HPU mockup 中包含 11 次 PBS 的 `ADDS(1)` 运算及结果解密。

### 9.2 尚未验证或尚未完成

- 使用 tfhe-rs 相同或密码学等价的 CSPRNG、mask generator 和噪声分布；
- C/RTL COSIM clean pass；
- full-shell setup timing 收敛至非负裕量；
- 多个 u8 连续加密和跨页输入的正确性；
- 多次随机实验的错误率和稳定性；
- 高并发、多任务和抢占场景；
- 真实 HPU FPGA 硬件对该密文执行 PBS、加法、比较等同态运算；
- 密钥安全加载、隔离、擦除和生命周期管理；
- 侧信道、随机数复用和 nonce 冲突分析；
- 严谨的吞吐率、延迟、资源、功耗和 CPU 对比实验。

## 10. 当前原型的安全性限制

当前 `next_random()` 使用简单的 xorshift 风格状态更新，`sample_tuniform_like_noise()` 也是为了功能验证而实现的近似采样。它们能够产生不同 mask 并提供有界小噪声，但不能作为生产环境的 TFHE 密码学随机源。

论文中必须明确区分：

```text
数学格式兼容和可解密性验证
!=
密码学安全性验证
```

后续至少应实现或对接：

- 与 tfhe-rs 参数定义一致的 TUniform noise sampler；
- 可证明安全的种子来源和 CSPRNG；
- 每个 ciphertext 独立且不可复用的 mask 随机流；
- seed/nonce 的持久化策略和冲突检测；
- secret key 在 Host、ARM、context BRAM 中的安全传输与清理。

## 11. 后续实验建议

建议按照以下顺序推进：

1. 解决 XSIM/COSIM 环境问题，取得 RTL 与 C 模型一致的 clean pass。
2. 修复 full-shell 9 ps setup violation，并保存 timing-clean 的 DCP、bitstream、BOOT、报告和 SHA-256。
3. 对 `encrypt_count=1,2,4,16,...` 做边界和跨页测试，检查长度公式及 finish completion。
4. 用多组随机 u8 重复测试，分别记录加密、DMA、SLM read 和解密时间。
5. 将内部随机源替换为 tfhe-rs 兼容或密码学安全实现，并做噪声分布统计。
6. 将当前 Host 侧 `LWEHLS01 -> RadixCiphertext` 包装逻辑整理为可复用接口。
7. 在真实 HPU FPGA 硬件上重复 mockup 已通过的 `ADDS(1)`，再用同一 ClientKey 解密结果。
8. 与 CPU 侧 tfhe-rs 加密建立相同参数、相同批量和相同数据路径下的性能基线。
9. 测量 FPGA 资源、频率、吞吐、端到端延迟、功耗和数据搬运占比。
10. 增加异常测试：错误 key、错误 dimension、output SLM 不足、任务取消和 runtime 重启。

## 12. 论文可提炼的工程贡献

在现阶段，可以从以下角度组织论文材料，但应避免超出实验证据：

1. 将 tfhe-rs radix/Big-LWE 客户端加密过程映射为 512-bit 流式 HLS 算子。
2. 建立 TOML 参数、Big-LWE 数据表示、secret key 和 SUDA context ABI 之间的映射。
3. 在可计算存储路径上打通 SSD、SLM、FPGA 算子和 Host 解密的端到端验证链路。
4. 揭示并修复可变长度密码学输出与 SUDA finish beat、DMA completion 聚合之间的协议问题。
5. 形成从数学参考、CSIM、tfhe-rs ClientKey 解密到板上运行的分层验证方法。

当前最稳妥的论文式结论是：

> 本工作实现了一个面向固定 HPU 参数的 2048 维 Big-LWE HLS 加密原型，并在 SUDA 可计算存储平台上完成了单字节 radix 加密的端到端板级功能验证。FPGA 输出的四个 Big-LWE 密文能够被同源 ClientKey 正确解密，并作为 `HpuRadixCiphertext` 在 HPU mockup 中完成包含 PBS 的标量加法，结果正确恢复为期望明文。当前实现仍需进一步完成密码学随机源、严格时序收敛、C/RTL 协同仿真及真实 HPU 硬件兼容性验证。

## 13. 关键文件索引

### 算法与 HLS

```text
device/operators/hls/lwe_encrypt/lwe_encrypt.hpp
device/operators/hls/lwe_encrypt/lwe_encrypt.cpp
device/operators/hls/lwe_encrypt/test.cpp
device/operators/hls/lwe_encrypt/test_deep.cpp
device/operators/hls/lwe_encrypt/run_hls.tcl
```

### Host 与 runtime

```text
host/applications/vscode-lwe-encrypt-data-gen/
host/applications/vscode-lwe-encrypt-offload/
device/platform/software_stack/nf_spdk/lib/nvmf/mcdma.c
device/platform/software_stack/nf_spdk/lib/hlsacccompute/hlsacccompute.c
device/platform/software_stack/nf_spdk/config.json
```

### FPGA 集成

```text
device/platform/basic_shell/nf-csd/shell/virt_one_drive/fpga/scripts/accframework.tcl
device/platform/basic_shell/nf-csd/shell/virt_one_drive/scripts/prj_setup.tcl
device/platform/basic_shell/nf-csd/shell/virt_one_drive/fpga/sources/hlsaccframework/lwe_encrypt/
```

### tfhe-rs 工具和参数

```text
<tfhe-rs>/mockups/tfhe-hpu-mockup/params/tuniform_64b_pfail128_psi64.toml
<tfhe-rs>/tfhe/examples/hpu/export_lwe_encrypt_key.rs
<tfhe-rs>/tfhe/examples/hpu/verify_lwe_encrypt_output.rs
<tfhe-rs>/tfhe/examples/hpu/lwe_encrypt_mockup_test.rs
```

### 详细日志

```text
logs/lwe_encrypt_operator_2026-06-09.md
logs/lwe_encrypt_stream_fix_2026-06-26.md
logs/lwe_encrypt_context_offset_fix_20260710.md
logs/lwe_runtime_finish_completion_fix_20260713.md
logs/lwe_encrypt_hpu_mockup_test_20260713.md
device/platform/basic_shell/nf-csd/build_bd_20260710_lwe_context_offset_fix.log
```

## 14. 构建产物校验信息

撰写本文时，本地关键产物 SHA-256 为：

```text
lwe_encrypt_ip.zip
660de2feeaf8fd2593a8d345904aae711a1a8a6b6fb6d3a49626ac12ee0aee03

ready_for_download/fidus/BOOT.bin
00bc113d483975f2623c0607c9d476bdf0098e4b538848366d42e550bf452e5c

nvmf_tgt.lwe_finish_account_20260713
ef5c54105ac68ea55f592622d705fedee1d14ac93baf9369af36bacaf6376865
```

已知可用于板卡恢复的旧 BOOT image 曾记录为：

```text
42435feb0a58c838882f1a96e133a7e9bebf2236a3187f1d275fd996b6775df0
```

SHA-256 只能证明两个文件在实际工程可接受的概率意义上内容一致，不能替代对镜像版本、构建参数和上板功能的记录。
