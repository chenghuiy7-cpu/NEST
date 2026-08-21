# CSD加密到远端HPU计算端到端基准支持记录

日期：2026-08-12

> 历史口径说明：本轮所有批量均显式设置 `input_lbas=256`，所以 SSD 到 input SLM
> 固定搬运 1MiB。该数据保留为固定输入搬运量基线，不应解释为按明文批量变化的
> SSD 到 SLM 开销。2026-08-13 起，常规端到端测试改为按
> `ceil(plaintext_bytes / 4096)` 自动选择输入 LBA，并写入新的 CSV。

## 目标

测量以下真实数据通路，并对各阶段做可重复的消融：

```text
CSD SSD明文
  -> input SLM
  -> FPGA lwe_encrypt
  -> output SLM
  -> 132 Host内存逻辑Big-LWE
  -> TCP发送到129
  -> 129真实V80 HPU执行ADDS
  -> TCP返回结果密文
  -> 132 Host内存验证/可选落盘
```

## 本次实现

1. 融合Host程序新增 `--benchmark`、`--skip-dump` 和
   `--slm-read-chunk-bytes`，默认采用已验证的128KB同步SLM回读。
2. 客户端分别测量SLM创建、SSD到SLM、program setup、FPGA execute、
   SLM到Host、FPGA输出解包验证、CSD资源释放、TCP连接、请求发送、等待响应头、
   响应密文接收、Host结果验证和文件写入。
3. `LWERPC01`增加可选v2遥测。v1客户端保持原行为；只有v2客户端要求服务端追加
   128B `LWEBEN01`计时尾帧。
4. 129服务端按同一 `request_id` 返回请求接收、metadata校验、Big-LWE到
   `RadixCiphertext`转换、HPU变量准备、命令入队、等待与同步、结果转换、序列化、
   memory sanitizer、响应发送和服务端总时间。
5. 增加 `echo` 操作：请求和响应密文尺寸与 `adds` 完全相同，但不调用HPU，作为
   网络、socket、协议编解码和内存复制的同载荷消融基线。
6. 增加 `run_remote_pipeline_bench.sh` 和 `summarize_remote_pipeline.py`，支持
   warm-up、重复采样、中位数/P95以及echo/adds对比。

## HPU计时边界

`HpuRadixCiphertext::from_radix_ciphertext()`只创建后端变量并写入Host侧映射内存，
文档明确说明此时尚未发生FPGA传输。`&hpu_input + scalar`通过channel异步入队，
真正完成操作需要等待结果。

服务端现在显式调用 `hpu_output.wait()`：

- `server_hpu_prepare_ms`：CPU Radix到HPU后端变量的构造和Host内存布局；
- `server_hpu_enqueue_ms`：命令对象构建与异步入队；
- `server_hpu_wait_sync_ms`：输入同步、HPU执行等待及结果Device到Host同步的组合；
- `server_hpu_output_convert_ms`：同步完成后的HPU格式到CPU Radix对象转换。

由于公开API没有单独的“仅上传”“仅等待RTL”“仅下载”同步点，当前不能进一步把
`server_hpu_wait_sync_ms`严谨拆成三个互斥阶段。若论文需要纯硬件执行周期，应继续读取
HPU runtime cycle counter或增加后端内部埋点。

## 网络计时边界

- `tcp_connect_ms`不包含在 `rpc_round_trip_ms` 内，但包含在完整在线端到端时间内。
- `request_send_ms`表示132将header和请求payload写入socket的时间，不保证数据已全部
  到达129应用层。
- `server_request_receive_ms`表示129从开始读取header到完整读取payload。
- `server_response_send_ms`表示129完成响应header和payload的socket写入，不保证132已
  完整收到。
- `response_receive_ms`表示132读取响应密文payload的时间。
- `echo rpc`是最可靠的同载荷网络/协议基线，但仍包含序列化、内存复制和系统调度，
  不能称为纯网络传播时延。
- `adds rpc - echo rpc`可作为远端计算栈增量的实验估计；正式结论还应同时引用服务端
  直接测得的decode、HPU和encode阶段。

## 代码与验证

涉及文件：

- `suda/host/applications/vscode-lwe-encrypt-remote-offload/`
- `hpu/tfhe-rs/tfhe/examples/hpu/lwe_remote_server.rs`
- `hpu/tfhe-rs/tfhe/examples/hpu/lwe_remote/protocol.rs`

本地验证：

- C++融合应用以 `-O2 -std=c++14`编译通过；仅有仓库libnvme头文件的既有warning。
- C++回环协议测试通过，包括v2响应和128B遥测尾帧。
- Rust V80服务端 `cargo check --release --features hpu-v80`通过。
- 原v1 Rust客户端 `cargo check --release --no-default-features --features integer`通过。

真实性能数字仍需在132 QEMU/NVMQ、CSD和129 V80服务均正常时采集。
