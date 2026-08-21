# vscode-lwe-encrypt-remote-offload

该程序把已经验证的两段式流程合并为一个 132/QEMU Host 进程：

```text
SSD -> input SLM -> FPGA lwe_encrypt -> output SLM
    -> Host 内存中的 HPU 原生 PC0/PC1 slot
    -> LWERPC01/TCP -> 129 直接导入原生 slot -> 真实 HPU ADDS
    -> Host 内存中的结果密文
    -> 最终 LWEHLS01 文件
```

FPGA 原始密文不写中间文件。程序读完 output SLM 后会保留原始 PC slot 作为网络载荷；
Host 端的逆向解包只用于旁路正确性验证，不参与发送数据的生成。随后程序会先释放
FPGA program、memory range 和 input/output SLM，再等待远端 HPU，避免在网络阶段占用
CSD 资源。

当前 `adds` 请求使用 `operation=2`：每个 u8 含 4 个 radix Big-LWE，每个 Big-LWE
按 `[PC0 12KB][PC1 12KB]` 传输，总计 96KB/u8。129 端不再执行 bit-reversal、
PC 交织或 torus 位宽转换，只截取两个 slot 的有效系数区并构造 HPU 变量。

## 构建

```bash
cd host/applications/vscode-lwe-encrypt-remote-offload
make -j4
make test
```

`make test` 只在本机回环 TCP 上验证 `LWERPC01`，不访问 FPGA 或 129。

## 运行

先在 129 启动 `hpu_lwe_remote_server`，然后在 132 的 QEMU 中执行：

```bash
cd /mnt/suda/host/applications/vscode-lwe-encrypt-remote-offload

./vscode-lwe-encrypt-remote-offload \
  --ssd-nsid 1 \
  --ssd-lba 65536 \
  --plaintext-bytes 128 \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --server "${HPU_REMOTE_HOST}" \
  --server-port 19090 \
  --scalar 1 \
  --output lwe_encrypt_remote_hpu_result_128b.bin
```

成功标志：

```text
intermediate_ciphertext_dump=none
host_big_lwe_key_decrypt_checked=yes
remote_hpu_ciphertext_compute=passed
lwe_encrypt remote HPU pipeline passed
```

`--output` 是远端 HPU 计算完成后的最终 `LWEHLS01` 文件，不是 FPGA 加密中间文件。
该文件仍包含用于实验校验的预期明文字节；网络请求只发送 metadata 和 HPU 原生 slot，
不会发送明文参考或私钥。

## 端到端性能测试

融合程序使用协议 v2 请求129返回固定128B计时尾帧。原有Rust客户端仍使用v1，
服务端会按请求版本响应，因此两套客户端可以同时保留。

单次真实HPU测试：

```bash
./vscode-lwe-encrypt-remote-offload \
  --ssd-nsid 1 \
  --ssd-lba 65536 \
  --plaintext-bytes 128 \
  --slm-read-chunk-bytes 131072 \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --server "${HPU_REMOTE_HOST}" \
  --server-port 19090 \
  --remote-operation adds \
  --scalar 1 \
  --benchmark \
  --skip-dump
```

网络与协议消融使用相同密文尺寸的 `echo`。服务端验证metadata后直接返回密文，
不导入或调用HPU：

```bash
./vscode-lwe-encrypt-remote-offload \
  --ssd-nsid 1 --ssd-lba 65536 \
  --plaintext-bytes 128 --slm-read-chunk-bytes 131072 \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --server "${HPU_REMOTE_HOST}" --server-port 19090 \
  --remote-operation echo --benchmark --skip-dump
```

重复测试并汇总：

```bash
BATCH_SIZES="1 16 32 128" \
REMOTE_OPERATIONS="echo adds" \
WARMUP=2 ITERATIONS=10 SETTLE_SECONDS=1 \
SLM_READ_CHUNK_BYTES=131072 \
OUTPUT_CSV=remote_pipeline_benchmark_auto_lba.csv \
./run_remote_pipeline_bench.sh

./summarize_remote_pipeline.py remote_pipeline_benchmark_auto_lba.csv \
  | tee remote_pipeline_benchmark_summary.md
```

程序未显式指定 `--input-lbas` 时，按
`ceil(plaintext_bytes / 4096)` 自动选择最小输入 LBA 数。benchmark 脚本的
`INPUT_LBAS` 默认值同样为 `auto`；只有做固定搬运量的对照实验时才设置为具体整数。
因此 1B、16B、32B、128B 明文都搬运一个 4KB LBA，4097B 明文搬运两个 LBA。
CSV 的 `input_lbas` 和程序输出的 `ssd_copy_bytes` 记录实际搬运量。

主要计时口径：

- `online_e2e_ms`：从SSD明文开始搬入input SLM，到远端结果密文完整进入132 Host内存。
- `one_shot_e2e_ms`：在上述范围基础上加入input/output SLM创建。
- `process_ms`：程序参数处理、密钥读取、资源创建、结果验证等完整进程内流程；使用
  `--skip-dump` 时不包含结果文件写入。
- `rpc_round_trip_ms`：不含TCP建连，从请求header发送开始，到响应密文和计时尾帧接收完成。
- `request_send_ms`、`response_receive_ms`：132端socket发送和接收阶段。
- `server_request_receive_ms`、`server_response_send_ms`：129端读取请求和写入响应socket阶段。
- `server_hpu_enqueue_ms`：向异步HPU后端提交命令。
- `server_hpu_wait_sync_ms`：等待HPU操作完成，并完成后端Device到Host同步。当前公开API
  无法将输入H2D、纯硬件执行和结果D2H完全拆开，因此不应把它单独表述为纯RTL计算时间。

`echo` 的RPC时间是相同请求/响应尺寸下TCP传输、协议编解码、socket缓冲、内存复制和
调度的合并成本。它适合作为网络通路消融，但不等于单向网络传播时延。
