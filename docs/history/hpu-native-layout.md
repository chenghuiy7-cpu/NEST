# LWE 加密算子直接输出 HPU 原生物理格式修改记录

日期：2026-08-13

## 1. 修改目标

修改前，`lwe_encrypt` 输出的是 tfhe-rs CPU 侧的自然顺序 Big-LWE：

```text
[mask[0], mask[1], ..., mask[2047], body]
```

远端 HPU 使用前必须在 CPU 上完成 mask 的 11-bit bit-reversal、按 16 个系数为一组分发到两个 HPU processing cut，以及每个 cut 的物理 slot 对齐。

本次修改将这些与 HPU 密文表示相关的数据变换下沉到 HLS。新默认输出为 psi64/V80 HPU 原生输入布局，使远端不再构造 CPU `RadixCiphertext`，也不再执行系数重排和位宽转换。

需要注意：这里的“直接”表示计算数据已经是 HPU 原生系数顺序和 slot 布局。网络协议解析、内存分配/注册、把 PC0/PC1 slot 写入 HPU 的两个 memory cut 仍然属于远端 runtime 的职责；它不等同于 FPGA 到远端 HPU 的网络零拷贝。

## 2. 修改前可用版本备份

在修改任何 HLS、IP 和算子池 RTL 前，已保存当前确认可用的上板文件及旧算子版本：

```text
<private-backup>/lwe_encrypt_cpu_layout_working_20260813/
├── boot/BOOT.bin
├── boot/zynqmp.dtb
├── hls/
├── operator_pool_rtl/
└── SHA256SUMS
```

关键哈希：

```text
BOOT.bin  f10f801cd342bff46836896bf8e0145c52eb81ddfd679529fd56868034233da5
zynqmp.dtb 1168b1abae245b040f1c6c0d9afa39d9c9e9d1506f23b7fd483ac5016569bdb5
```

已执行并通过：

```bash
cd "${SUDA_ROOT}"
sha256sum -c backups/lwe_encrypt_cpu_layout_working_20260813/SHA256SUMS
```

## 3. 新的 HPU 原生布局

psi64 参数固定如下：

```text
Big-LWE dimension        = 2048
coefficient width        = 64 bit
radix blocks per u8      = 4
HPU processing cuts      = 2
physical slot per cut    = 3 x 4096 B = 12288 B
physical slot per LWE    = 2 x 12288 B = 24576 B
physical bytes per u8    = 4 x 24576 B = 98304 B
```

每个 Big-LWE 的输出顺序：

```text
PC0 slot, 12288 B:
  1024 个重排后的 mask 系数 + body + zero padding

PC1 slot, 12288 B:
  1024 个重排后的 mask 系数 + zero padding
```

mask 映射规则与 tfhe-rs 的 `HpuLweCiphertextOwned::create_from` 一致：

```text
hpu_index = reverse_11_bits(natural_mask_index)
group     = hpu_index / 16
lane      = hpu_index % 16
pc        = group % 2
pc_offset = (group / 2) * 16 + lane
```

psi64 的 NTT/PBS ciphertext width 都是 64 bit，因此原有 `msb2lsb` 位宽转换在该配置下是恒等变换，无需额外移位。

## 4. HLS 修改

修改文件：

```text
device/operators/hls/lwe_encrypt/lwe_encrypt.hpp
device/operators/hls/lwe_encrypt/lwe_encrypt.cpp
device/operators/hls/lwe_encrypt/test.cpp
device/operators/hls/lwe_encrypt/test_deep.cpp
```

主要改动：

1. context 的 `[191:160]` 新增 `output_layout`：
   - `0`：旧 CPU-LWE 格式。
   - `1`：HPU 原生物理格式。
2. 新增 HPU 原生加密输出路径，在生成 mask 的同时完成 11-bit bit-reversal 和 PC0/PC1 分发。
3. PC0/PC1 使用 HLS BRAM 暂存，然后分别输出 12KB slot 和零填充。
4. 保留旧 CPU-LWE 输出路径，便于恢复旧 BOOT 后使用 `--output-layout cpu` 做兼容测试。
5. HPU 原生模式只接受固定 psi64 参数和 u8 radix 模式，非法组合返回明确错误包。

## 5. SUDA Host 修改

修改：

```text
host/applications/vscode-lwe-encrypt-offload/vscode-lwe-encrypt-offload.cpp
host/applications/vscode-lwe-encrypt-offload/README.md
```

主要改动：

1. 默认向 context 写入 `output_layout=1`。
2. 新增 `--output-layout hpu-native|cpu`。
3. 按 98304B/u8 计算 HPU 原生 output SLM 大小。
4. Host 验证代码可以检查 PC0/PC1 padding、逆向读取自然 mask，并使用保存的 ClientKey 验证解密结果。
5. 为保持原有 mockup 和离线工具兼容，`LWEHLS01` dump 仍可写成逻辑 CPU-LWE；该转换仅用于本地验证/旧格式文件，不在新的远端 HPU 输入路径中使用。

修改：

```text
host/applications/vscode-lwe-encrypt-remote-offload/vscode-lwe-encrypt-remote-offload.cpp
host/applications/vscode-lwe-encrypt-remote-offload/lwe_remote_protocol.hpp
host/applications/vscode-lwe-encrypt-remote-offload/lwe_remote_protocol.cpp
host/applications/vscode-lwe-encrypt-remote-offload/README.md
```

主要改动：

1. 新增 RPC operation `2`，表示请求载荷已经是 HPU 原生物理布局。
2. fused 程序直接发送 HLS/SLM 返回的 HPU 原生 words，不再把输入重排为 CPU-LWE 后再发送。
3. 旧 operation `1` 的 CPU-LWE 路径仍保留，避免破坏此前已经跑通的客户端。

## 6. 远端 tfhe-rs/HPU 修改

修改：

```text
<tfhe-rs>/tfhe/src/integer/hpu/ciphertext/mod.rs
<tfhe-rs>/tfhe/examples/hpu/lwe_remote/protocol.rs
<tfhe-rs>/tfhe/examples/hpu/lwe_remote/bridge.rs
<tfhe-rs>/tfhe/examples/hpu/lwe_remote_server.rs
```

主要改动：

1. 新增 `HpuRadixCiphertext::from_hpu_lwe_ciphertexts`，直接接收已按 PC cut 排列的 `HpuLweCiphertextOwned`。
2. 远端 operation `2` 只校验参数、slot 大小与 zero padding，然后把每个 slot 的有效区映射为两个 HPU cut。
3. 该路径不调用 `HpuLweCiphertextOwned::create_from`，因此不会再次执行 bit-reversal、PC interleave 或 torus 位宽转换。
4. HPU 运算结果目前仍转换成 CPU-LWE 响应，以便 132 机器继续用 ClientKey 解密验证。也就是说，本次消除的是 HPU 输入前的数据格式转换。

## 7. 验证结果

### 7.1 HLS 功能验证

CSIM smoke test 通过：

```text
lwe_encrypt HLS smoke test passed.
mask_dimension=2048 plaintext_bytes=65 input_packets=2
radix_blocks=260 output_layout=hpu-native decrypt_checked=yes
CSim done with 0 errors
```

深度 C++ testbench 同时覆盖旧 CPU-LWE 和新 HPU-native 输出，均通过解密和布局检查。

### 7.2 HLS 综合与 IP 导出

Vitis HLS 2020.2 `csynth` 和 IP 打包完成：

```text
Target clock              4.000 ns / 250 MHz
Estimated clock           3.347 ns / 298.78 MHz
Clock uncertainty          1.080 ns
Effective timing budget    2.920 ns
BRAM_18K                  8
DSP                       8
FF                        21279
LUT                       46173
```

说明：上述只是 HLS 综合估计，不等于整板 implementation timing 已通过。3.347ns 虽小于名义 4ns 周期，但大于扣除 1.08ns uncertainty 后的 2.92ns 有效预算，因此 HLS 约束不是完全 clean。综合报告还显示输出 stream 循环未满足 II=1，native mask 重排循环本身达到 II=1。

生成物：

```text
device/operators/hls/lwe_encrypt/lwe_encrypt_ip.hpu_native_20260813.zip
SHA-256: a76b0265434c4b932356e224e09c992e10f514e57f745ed40b9f290de23c4434

device/operators/hls/lwe_encrypt/vitis_hls.csim_hpu_native_20260813.log
SHA-256: 12b8ac339559b400b1203c7c6c3be1678f4f581b448e3b3dc54c55d5bd52cec6

device/operators/hls/lwe_encrypt/vitis_hls.rtl_gen_hpu_native_20260813.log
SHA-256: 83681e18b77c80b2b8e5e1b601f56e2b844945364801c87a40dcad8b9cd24308
```

Vitis 2020.2 首次 IP pack 因日期生成的 `core_revision` 超出合法范围而失败，现有脚本将 revision 固定为 1 后重新打包成功。这不是 RTL 综合失败。

### 7.3 算子池 RTL

新 IP 中的 14 个 Verilog 文件已更新到：

```text
device/platform/basic_shell/nf-csd/shell/virt_one_drive/fpga/sources/hlsaccframework/lwe_encrypt/
```

新增的原生布局相关模块：

```text
lwe_encrypt_encrypt_encoded_lwe_hpu_native.v
lwe_encrypt_encrypt_encoded_lwe_hpu_native_pc0_V.v
lwe_encrypt_write_hpu_native_pc_slot.v
```

已逐文件确认算子池中的 14 个 Verilog 与新 IP zip 内文件一致。

### 7.4 Host 与 Rust 回归

通过项：

```text
vscode-lwe-encrypt-offload: make clean && make
vscode-lwe-encrypt-remote-offload: make clean && make
C++ RPC protocol loopback test: passed
Rust hpu_lwe_remote_server tests: 7 passed, 0 failed
```

Rust 测试包含：

```text
hpu_native_ciphertext_frame_round_trip
native_slot_import_matches_tfhe_cpu_conversion
```

第二项将同一组 2048 维 CPU Big-LWE 分别经 tfhe-rs 旧转换和新原生 slot 导入，逐 cut 比较完全一致。

## 8. 当前边界和风险

1. 新 HLS 的随机 mask/noise 仍是原型 PRNG/t-uniform-like sampler，并非 tfhe-rs CSPRNG 的 bit-exact 实现；本次只调整密文物理布局。
2. HPU 原生格式把每个 u8 的物理载荷从旧格式 65792B 增加到 98304B，因为两个 PC cut 都按 12KB slot 输出。网络流量约增加 49.4%。
3. 尚未执行完整 block design implementation、整板 timing 检查和新 BOOT 上板测试。
4. 当前板上仍是修改前 CPU-LWE BOOT。未换新 BOOT 前，新 Host 程序必须显式使用 `--output-layout cpu`；不能用默认 `hpu-native` 与旧 RTL 混跑。
5. 129 服务器也必须同步本次 tfhe-rs 文件并重编译服务端，旧服务端不认识 operation `2`。

## 9. 下一步

在 tmux 中生成完整比特流并保留独立日志：

```bash
cd "${SUDA_ROOT}/device/platform/basic_shell/nf-csd"
source /opt/Xilinx_2020.2/Vivado/2020.2/settings64.sh
bash build_bd.sh 2>&1 | tee build_bd_20260813_hpu_native.log
```

生成后必须先检查 implementation timing 和 BOOT/DTB 哈希，再决定是否替换板上文件。确认新 BOOT 可启动后，依次验证：

1. `--output-layout hpu-native` 的 1B 本地 ClientKey 解密。
2. 128B 连续明文的本地解密。
3. 同步并重编译 129 服务端，运行 operation `2` 的远端 HPU `ADDS`。
4. 对比旧 CPU-LWE RPC 与新原生 RPC 的 Host 格式转换时间、网络载荷和端到端延迟。
