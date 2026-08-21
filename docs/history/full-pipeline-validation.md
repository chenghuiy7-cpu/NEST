# LWE 加密、远端 HPU 计算、FPGA 解密完整通路开发记录

日期：2026-08-20

## 目标

在 132 机器的一个 Host 进程内完成：

```text
SSD -> SLM -> FPGA lwe_encrypt -> Host 内存 -> TCP
    -> 129 机器真实 HPU -> TCP -> 132 Host 内存 -> SLM
    -> FPGA lwe_decrypt -> SSD
```

中间密文不写文件。FPGA 加密输出、TCP 请求、TCP 响应和 FPGA 解密输入均采用 `hpu-native-psi64-v80` 固定物理布局。

## 协议调整

保留已有协议行为：

- `op=0`：echo，请求和响应保持原布局。
- `op=1`：CPU 逻辑 LWE 输入，CPU 逻辑 LWE 输出。
- `op=2`：HPU 原生输入，CPU 逻辑 LWE 输出，用于兼容旧客户端。

新增：

- `op=3`：HPU 原生输入，真实 HPU 执行 u8 `ADDS`，HPU 原生输出。

每个 HPU LWE 仍占两个 PC slot，每个 slot 为 1536 个 u64：

```text
PC0: 1025 个有效 word + 511 个零填充 word
PC1: 1024 个有效 word + 512 个零填充 word
```

每个 u8 有 4 个 radix block，因此一个 u8 的 TCP 请求和响应均为：

```text
4 * 2 * 1536 * 8 = 98304 B
```

远端服务不再对 `op=3` 的 HPU 结果调用 `to_radix_ciphertext()`。服务端直接取回每个 HPU LWE 的两个 memory cut，恢复固定 slot 补零后返回。

## Host 一体化程序

新增目录：

```text
host/applications/vscode-lwe-full-pipeline
```

主要行为：

1. 使用 `nvme_slm_copy` 将源 SSD LBA 直接搬到加密 input SLM。
2. 运行 `operator_type=2, program_id=11` 的 `lwe_encrypt`。
3. 将 output SLM 的 HPU 原生 payload 原样读入 Host 内存并作为 `op=3` TCP 请求发送。
4. 收到 HPU 原生响应后，不做逻辑 LWE 重排，按 4KB 请求写入解密 input SLM。
5. 运行 `operator_type=3, program_id=12` 的 `lwe_decrypt`。
6. 读取 packed u8 输出，按 4KB LBA 补零后写入目标 SSD。
7. 默认回读目标 SSD 并逐字节校验。

Host 侧现有 Big-LWE 解密仅作为正确性旁路检查；送往网络和解密 SLM 的 payload 始终是原始 HPU-native buffer，没有依赖旁路检查产生的数据。

程序拒绝源、目标 LBA 范围重叠，避免覆盖输入数据。

## 远端部署

在 132 机器同步代码到 129：

```bash
cd "${TFHE_RS_ROOT}"

rsync -avR \
  -e "ssh -p ${HPU_REMOTE_SSH_PORT} -i ${HPU_REMOTE_SSH_KEY}" \
  tfhe/Cargo.toml \
  tfhe/src/integer/hpu/ciphertext/mod.rs \
  tfhe/examples/hpu/lwe_remote/ \
  tfhe/examples/hpu/lwe_remote_server.rs \
  backends/tfhe-hpu-backend/src/ffi/v80/mod.rs \
  scripts/lwe_remote_hpu/start_server_129.sh \
  "${HPU_REMOTE_USER}@${HPU_REMOTE_HOST}:${HPU_REMOTE_TFHE_RS}/"
```

`hpu_config_remote_no_reload.toml` 当前只存在于 129 机器，不从 132 覆盖。129 机器启动：

```bash
cd "${TFHE_RS_ROOT}"

export HPU_REMOTE_CONFIG="${TFHE_RS_ROOT}/hpu_config_remote_no_reload.toml"
./scripts/lwe_remote_hpu/start_server_129.sh \
  2>&1 | tee hpu_lwe_remote_server_op3.log
```

出现 `listen_addr=0.0.0.0:19090` 后表示服务正在监听，终端停在这里是正常现象。

## 132/QEMU 构建与测试

```bash
cd /mnt/suda/host/applications/vscode-lwe-full-pipeline
make clean && make -j
```

以 128B、源 LBA 65536、目标 LBA 131072 为例。执行前必须确认目标 LBA 未被其他测试使用：

```bash
./vscode-lwe-full-pipeline \
  --ssd-nsid 1 \
  --ssd-lba 65536 \
  --input-lbas 1 \
  --plaintext-bytes 128 \
  --output-ssd-nsid 1 \
  --output-ssd-lba 131072 \
  --expect 171 \
  --server "${HPU_REMOTE_HOST}" \
  --server-port 19090 \
  --scalar 1 \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --benchmark \
  2>&1 | tee lwe_full_pipeline_128b.log
```

成功标志：

```text
lwe full SSD-to-remote-HPU-to-SSD pipeline passed
remote HPU ciphertext compute passed
fpga_decrypt_checked=yes
destination_ssd_readback_checked=yes
```

## 本地验证

- 新一体化 C++ 程序编译通过。
- 原有 C++ TCP 协议回归和新增 `op=3` 原生 payload 回环测试通过。
- Rust HPU 服务端 example 测试 7/7 通过。
- 原生 bridge 测试确认 HPU slot 导入、导出后的全部 word 完全一致。
- Rust 格式检查和启动脚本 `bash -n` 通过。

尚未在当前开发终端直接执行真实端到端测试；该步骤需要 QEMU 的 NVMQ 通路和 129 机器真实 HPU 服务同时在线。
