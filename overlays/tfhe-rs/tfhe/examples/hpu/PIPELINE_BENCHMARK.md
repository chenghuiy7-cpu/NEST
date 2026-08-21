# HPU Pipeline Benchmark

该 benchmark 用于测量：

```text
SSD -> host memory -> V80 HPU TFHE computation -> host memory -> optional SSD writeback
```

也就是：

```text
SSD -> 主机内存 -> V80 HPU TFHE 计算 -> 主机内存 -> 可选 SSD 写回
```

它复用了仓库中已有的 TFHE-rs HPU 栈：

- `backends/tfhe-hpu-backend`：用于 `HpuDevice`、V80 QDMA/AMI 访问、内存管理和 IOp 派发。
- `tfhe/src/integer/hpu/ciphertext/mod.rs`：用于 `HpuRadixCiphertext` 转换和 HPU 运算。
- `tfhe/examples/hpu/bench.rs` 和 `tfhe/examples/hpu/hlapi.rs`：作为初始化和运算调用模型。

## 测量内容

包含两个 Rust example：

- `hpu_pipeline_dataset`：生成测试样例，输出可见十进制明文文件、可落盘 CPU radix ciphertext 文件、匹配的 client key 和 compressed server key。
- `hpu_pipeline_bench`：读取已生成的数据文件，执行 SSD -> host -> HPU -> host -> optional SSD writeback 流程并输出结果。

阶段拆分：

- `ssd_read`：只测量 `fs::read(input)`。Linux page cache 状态由外部系统环境决定，harness 不主动控制。
- `host_preprocessing`：解析输入 records。旧 `--input` 模式会在这里将明文加密为 CPU radix ciphertext；推荐的 `--ciphertext-input` 模式会在这里反序列化已落盘的 CPU radix ciphertext。
- `host_to_hpu_transfer`：将 CPU radix ciphertext 转换为 `HpuRadixCiphertext`。在 V80 上，这一阶段包含 QDMA host-to-card 写入。
- `hpu_compute`：派发 HPU IOp，并等待完成 ack。
- `hpu_to_host_transfer`：将 `HpuRadixCiphertext` 转回 CPU radix ciphertext。在 V80 上，这一阶段包含 QDMA card-to-host 读取。
- `host_postprocessing`：解密、和明文 reference 对比校验，并准备输出 bytes。
- `ssd_writeback`：可选输出文件写回。只有传入 `--sync-write` 时才会执行 `fsync`。

冷启动和初始化开销会单独报告：

- HPU device/config 打开。
- client key / compressed server key 生成，或者从文件加载。
- `tfhe::integer::hpu::init_device`，包括 key material 和 firmware material 上传。

steady-state repetitions 不包含上述 init costs。支持 warmup，warmup 结果不会写入 raw repetition rows。

## 构建

真实 Alveo V80 硬件环境：

```bash
cd tfhe-rs
source setup_hpu.sh --config v80 -p
source /opt/Xilinx_2025.1/Vivado/2025.1/Vivado/settings64.sh
# 或者直接设置 XILINX_VIVADO，路径必须指向包含 bin/vivado 的目录：
# export XILINX_VIVADO=/opt/Xilinx_2025.1/Vivado/2025.1/Vivado
# export PATH="${XILINX_VIVADO}/bin:${PATH}"
cargo build --release --features hpu-v80 --example hpu_pipeline_dataset --example hpu_pipeline_bench
```

如果用于 mockup/simulation 开发，使用 `--features hpu` 构建，并先启动 mockup。simulation 结果不是 V80 硬件结果。

## 推荐单次运行：先生成数据，再跑 benchmark

建议使用绝对路径保存数据和结果，避免相对路径 `target/...` 落到当前 repo 的
`<tfhe-rs>/target/...` 下。

### 1. 先设置想要的数据集参数

通常只需要改这一组变量：

```bash
cd "${TFHE_RS_ROOT}"

export INTEGER_WIDTH=64
export PLAINTEXT_BYTES=4096
export SIZE_TAG=4kb
export SEED=24301

export TYPE_TAG="u${INTEGER_WIDTH}"
export DATA_ROOT="${TFHE_RS_ROOT}/target/hpu-pipeline-bench/manual-${TYPE_TAG}-${SIZE_TAG}"
export DATA_DIR="${DATA_ROOT}/inputs"
export RESULTS_DIR="${DATA_ROOT}/results"
mkdir -p "${DATA_DIR}" "${RESULTS_DIR}"
```

这些变量的含义：

- `INTEGER_WIDTH`：每个明文整数的 bit 宽度，例如 `8` 或 `64`。benchmark 阶段必须使用同一个值。
- `PLAINTEXT_BYTES`：逻辑二进制定长明文大小，例如 `4096` 表示 4KB，`$((4 * 1024 * 1024))` 表示 4MiB。
- `SIZE_TAG`：只用于目录名和文件名，例如 `4kb`、`4mb`。真正大小由 `PLAINTEXT_BYTES` 决定，文件名不会决定数据大小。
- `SEED`：确定性生成明文样例。相同参数和 seed 会生成相同明文。
- `DATA_ROOT`：本次数据集根目录。建议每组数据单独一个目录，避免 u8/u64、4KB/4MB 文件混在一起。

常用设置示例：

```bash
# u64, 4KB 逻辑明文
export INTEGER_WIDTH=64
export PLAINTEXT_BYTES=4096
export SIZE_TAG=4kb

# u8, 4KB 逻辑明文
export INTEGER_WIDTH=8
export PLAINTEXT_BYTES=4096
export SIZE_TAG=4kb

# u64, 4MiB 逻辑明文
export INTEGER_WIDTH=64
export PLAINTEXT_BYTES=$((4 * 1024 * 1024))
export SIZE_TAG=4mb
```

### 2. 生成可见明文、落盘密文和匹配密钥

先确认上一步的变量已经设置好，然后运行：

```bash
cd "${TFHE_RS_ROOT}"

./target/release/examples/hpu_pipeline_dataset \
  --config '${HPU_BACKEND_DIR}/config_store/${HPU_CONFIG}/hpu_config.toml' \
  --params '${HPU_BACKEND_DIR}/../../mockups/tfhe-hpu-mockup/params/tuniform_64b_pfail128_psi64.toml' \
  --plaintext-output "${DATA_DIR}/plain_${TYPE_TAG}_${SIZE_TAG}.txt" \
  --plaintext-binary-output "${DATA_DIR}/plain_${TYPE_TAG}_${SIZE_TAG}.bin" \
  --ciphertext-output "${DATA_DIR}/cipher_${TYPE_TAG}_${SIZE_TAG}.bin" \
  --client-key-output "${DATA_DIR}/client_key_${TYPE_TAG}_${SIZE_TAG}.bin" \
  --server-key-output "${DATA_DIR}/server_key_${TYPE_TAG}_${SIZE_TAG}.bin" \
  --plaintext-bytes "${PLAINTEXT_BYTES}" \
  --integer-width "${INTEGER_WIDTH}" \
  --seed "${SEED}" \
  --force
```

`hpu_pipeline_dataset` 只读取离线配置和参数文件，不打开 V80 设备，因此生成样例时不会触发 HPU backend 的 fresh reload。`--params` 必须和后续 V80 bitstream 的 TFHE 参数匹配；默认 V80 配置使用 `tuniform_64b_pfail128_psi64.toml`。

`--plaintext-bytes` 表示逻辑上的二进制定长明文载荷大小，不表示十进制文本文件大小。record 数按下面公式计算：

```text
operand_bytes = ceil(INTEGER_WIDTH / 8)
record_bytes = 2 * operand_bytes
record_count = PLAINTEXT_BYTES / record_bytes
```

例如：

- `INTEGER_WIDTH=64, PLAINTEXT_BYTES=4096`：`4096 / (2 * 8) = 256` 条 record。
- `INTEGER_WIDTH=8, PLAINTEXT_BYTES=4096`：`4096 / (2 * 1) = 2048` 条 record。
- `INTEGER_WIDTH=64, PLAINTEXT_BYTES=4MiB`：`4194304 / (2 * 8) = 262144` 条 record。

生成完成后，建议确认文件位置和大小：

```bash
ls -lh "${DATA_DIR}"
head "${DATA_DIR}/plain_${TYPE_TAG}_${SIZE_TAG}.txt"
stat -c '%n %s bytes' "${DATA_DIR}/plain_${TYPE_TAG}_${SIZE_TAG}.bin" "${DATA_DIR}/cipher_${TYPE_TAG}_${SIZE_TAG}.bin"
```

### 3. 设置 benchmark 参数并运行

这一步指定“测哪个数据集”和“怎么测”。`DATA_DIR` 必须指向上一步生成的同一组数据，`INTEGER_WIDTH` 必须和生成数据时一致。

常用可改参数：

- `BATCH_SIZE`：每个 batch 处理多少条 record。
- `REPETITIONS`：正式测量轮数。
- `WARMUPS`：预热轮数，不进入最终统计。
- `OPERATION`：`add`、`sub` 或 `mul`。
- `OPERAND_MODE`：`encrypted-encrypted` 或 `encrypted-clear`。
- `CLEAR_SCALAR`：固定常数操作数。例如 `1` 表示 `+1`，`2` 表示 `+2`。如果要使用原始第二列 rhs，不要传 `--clear-scalar`。

示例：对每条 record 的第一列密文执行 `+1`，并把结果密文写回 SSD：

```bash
export BATCH_SIZE=64
export REPETITIONS=5
export WARMUPS=1
export OPERATION=add
export OPERAND_MODE=encrypted-encrypted
export CLEAR_SCALAR=1

export RUN_ID="${OPERATION}${CLEAR_SCALAR}_${TYPE_TAG}_${SIZE_TAG}_ctct"
export RUN_DIR="${RESULTS_DIR}/${RUN_ID}_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${RUN_DIR}"
export DATASET_SIZE=$(wc -l < "${DATA_DIR}/plain_${TYPE_TAG}_${SIZE_TAG}.txt")

./target/release/examples/hpu_pipeline_bench \
  --config '${HPU_BACKEND_DIR}/config_store/${HPU_CONFIG}/hpu_config.toml' \
  --no-reload \
  --plaintext-input "${DATA_DIR}/plain_${TYPE_TAG}_${SIZE_TAG}.txt" \
  --ciphertext-input "${DATA_DIR}/cipher_${TYPE_TAG}_${SIZE_TAG}.bin" \
  --client-key "${DATA_DIR}/client_key_${TYPE_TAG}_${SIZE_TAG}.bin" \
  --server-key "${DATA_DIR}/server_key_${TYPE_TAG}_${SIZE_TAG}.bin" \
  --dataset-size "${DATASET_SIZE}" \
  --batch-size "${BATCH_SIZE}" \
  --repetitions "${REPETITIONS}" \
  --warmups "${WARMUPS}" \
  --integer-width "${INTEGER_WIDTH}" \
  --operation "${OPERATION}" \
  --operand-mode "${OPERAND_MODE}" \
  --clear-scalar "${CLEAR_SCALAR}" \
  --ciphertext-output "${RUN_DIR}/${RUN_ID}_cipher_result.bin" \
  --output "${RUN_DIR}/${RUN_ID}_decrypted_result.txt" \
  --output-format text \
  --results-dir "${RUN_DIR}" \
  --run-id "${RUN_ID}" \
  --sync-write
```

如果要测试原始两列密文运算 `Enc(lhs) op Enc(rhs)`，删除 `--clear-scalar "${CLEAR_SCALAR}"` 这一行，并把 `RUN_ID` 改成不带常数的名字，例如：

```bash
export RUN_ID="${OPERATION}_${TYPE_TAG}_${SIZE_TAG}_${OPERAND_MODE}"
```

跑完后查看结果：

```bash
ls -lh "${RUN_DIR}"
head "${RUN_DIR}/${RUN_ID}_decrypted_result.txt"
cat "${RUN_DIR}/${RUN_ID}.csv"
```

支持的运算：

- `add`
- `sub`
- `mul`

支持的 operand modes：

- `encrypted-encrypted`：encrypted lhs op encrypted rhs。
- `encrypted-clear`：encrypted lhs op clear rhs。

如果要对已有密文执行固定明文常数运算，例如“每个加密 lhs 都加 1”，不用改写明文输入文件。直接使用原始 `plain_${TYPE_TAG}_${SIZE_TAG}.txt` 作为正确性参考，并传入：

```bash
--operand-mode encrypted-clear \
--clear-scalar 1
```

如果当前 bitstream/backend 的 encrypted-clear scalar 路径验证失败，也可以用 ct-ct 路径验证“密文 + 1”数据流：

```bash
--operand-mode encrypted-encrypted \
--clear-scalar 1
```

此时 host 会把常数 `1` 加密成 rhs ciphertext，V80 执行 `Enc(lhs) + Enc(1)`，输出仍是 HPU 计算后的结果密文。

harness 会解密每个结果，并与 cleartext reference 对比以验证正确性。建议在共享机器上始终传入 `--no-reload`；这样如果当前 V80 状态不能直接复用，程序会写出 `*.blocker.json` 并退出，而不会卸载驱动、重扫 PCIe 或重新烧 bitstream。如果缺少 V80 drivers、QDMA device nodes、bitstream archives 或 board setup，运行也会以非零状态退出。它不会伪造 benchmark 结果。

`--ciphertext-output` 写回的是 HPU 计算后读回 host 的 CPU `RadixCiphertext` 序列；`--output` 写回的是解密后的结果值，配合 `--output-format text` 可以生成可见十进制结果文件。

兼容的旧模式仍然保留：`hpu_pipeline_bench --input ... --generate-input` 会生成一个二进制明文输入文件，并在每轮 benchmark 的 host preprocessing 阶段现场加密。该模式便于快速调通，但不满足“密文落盘”的测试目标。

## Sweep 批量运行

```bash
cd tfhe-rs
SIZES="1024 4096 16384" \
BATCHES="16 64 256" \
OPS="add sub mul" \
MODES="encrypted-encrypted encrypted-clear" \
REPETITIONS=5 \
WARMUPS=1 \
WIDTH=64 \
FEATURES=hpu-v80 \
scripts/hpu_pipeline/run_sweep.sh
```

sweep 脚本会构建 `hpu_pipeline_dataset` 和 `hpu_pipeline_bench`，按 `SIZES` 先生成确定性的明文/密文/key 文件，再运行所有参数组合，并调用 summarizer 汇总结果。

如果希望按逻辑二进制定长明文字节数 sweep，而不是按 record 数 sweep，可以使用 `PLAINTEXT_BYTES`：

```bash
PLAINTEXT_BYTES="4096 16384" WIDTH=64 scripts/hpu_pipeline/run_sweep.sh
```

脚本会自动按 `plaintext_bytes / (2 * ceil(WIDTH / 8))` 计算对应的 `--dataset-size`。

## 结果文件

每次 benchmark run 会写出：

- `<run_id>.json`：完整 raw report，包括 init timings、stage definitions、environment、arguments 和每个 repetition 的结果。
- `<run_id>.csv`：每个 measured steady-state repetition 一行。
- `<run_id>.blocker.json`：仅在 run 被阻塞、没有产生有效测量结果时写出。

sweep summarizer 会写出：

- `summary.csv`
- `summary.md`
- 如果安装了 `matplotlib`，写出 `stage_breakdown.png`。
- 如果安装了 `matplotlib`，写出 `throughput.png`。

## Raw CSV Schema

每一行表示一个 measured repetition。

必需列：

- `run_id`
- `repetition`
- `operation`
- `operand_mode`
- `integer_width`
- `dataset_size`
- `batch_size`
- `input_kind`
- `input_path`
- `plaintext_path`
- `ciphertext_path`
- `output_path`
- `ciphertext_output_path`
- `input_bytes`
- `plaintext_bytes`
- `output_bytes`
- `ciphertext_output_bytes`
- `throughput_ops_per_s`
- `validation_passed`
- `first_mismatch`
- `ssd_read_ns`
- `host_preprocessing_ns`
- `host_to_hpu_transfer_ns`
- `hpu_compute_ns`
- `hpu_to_host_transfer_ns`
- `host_postprocessing_ns`
- `ssd_writeback_ns`
- `total_ns`

## 数据文件格式

### 可见十进制明文文件

`hpu_pipeline_dataset --plaintext-output` 生成 UTF-8 文本：

```text
lhs0 rhs0
lhs1 rhs1
...
```

数字是十进制 `u128`，已按 `--integer-width` 截断到对应 bit width。该文件用于正确性校验，也作为 `encrypted-clear` 模式的 clear rhs 来源。

如果生成器传入 `--plaintext-bytes`，该参数按二进制定长明文来计算 record 数：

```text
operand_bytes = ceil(integer_width / 8)
record_bytes = 2 * operand_bytes
record_count = plaintext_bytes / record_bytes
```

例如 `--integer-width 64 --plaintext-bytes 4096` 会生成 256 条 record。输出的十进制明文文件只是可见表示，实际文件大小不作为 4KB 基准。`hpu_pipeline_bench` 也支持同样的 `--plaintext-bytes`，因此 benchmark 阶段不需要手工把 4096 bytes 换算成 256 条 record。

如果传入 `--plaintext-binary-output`，生成器还会输出一个无 header 的 raw binary 明文文件：

```text
records: repeated {
  lhs: little-endian fixed-width integer, operand_bytes bytes
  rhs: little-endian fixed-width integer, operand_bytes bytes
}
```

这个文件的大小等于 `record_count * 2 * operand_bytes`。因此 `--integer-width 64 --plaintext-bytes 4096` 时，`plain_u64_4kb.bin` 严格是 4096 bytes。

### 落盘密文文件

`hpu_pipeline_dataset --ciphertext-output` 生成二进制 CPU ciphertext 文件：

```text
magic[8] = "HPUCTX1\0"
version: u32 little-endian
integer_width: u32 little-endian
record_count: u64 little-endian
seed: u128 little-endian
records: repeated {
  record_len: u64 little-endian
  record_payload: bincode({ lhs: RadixCiphertext, rhs: RadixCiphertext })
}
```

这里的密文是 CPU 侧 `RadixCiphertext`，不是 V80 板上内存镜像。benchmark 读取该文件后，在 `host_to_hpu_transfer` 阶段调用 `HpuRadixCiphertext::from_radix_ciphertext` 转成 HPU 侧 ciphertext；真实 V80 上这个转换窗口包含 QDMA host-to-card 写入。

### 旧二进制明文输入

兼容模式下，`hpu_pipeline_bench --generate-input --input` 会创建确定性的二进制明文输入文件：

```text
magic[8] = "HPUPIP1\0"
version: u32 little-endian
integer_width: u32 little-endian
record_count: u64 little-endian
seed: u128 little-endian
records: repeated { lhs: u128 little-endian, rhs: u128 little-endian }
```

如果旧 `--input` 文件没有二进制 magic，输入会按 UTF-8 文本解析，内容应为以空白分隔的数字 token，每条 record 两个 token：`lhs rhs`。支持十进制和 `0x...` 十六进制。

## 注意事项和限制

harness 不会 drop Linux page cache，也不会修改 CPU frequency governors。如果需要严格的 cold-cache SSD 测量，请使用外部系统设置完成。

HPU backend 当前没有通过 public Rust API 暴露更底层的 RTL sub-stage timing。因此 `hpu_compute` 测量的是从 IOp dispatch 到 completion wait/ack 的时间。

对于 V80，`HpuRadixCiphertext::from_radix_ciphertext` 会走到 `MemZone::write_bytes`，而后者使用 QDMA `pwrite`；因此 harness 将这个转换窗口报告为 `host_to_hpu_transfer`。
