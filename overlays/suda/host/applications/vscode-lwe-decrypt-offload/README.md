# vscode-lwe-decrypt-offload

该程序用于独立验证 SUDA 算子池中的 `lwe_decrypt`（operator type ID 3）。它将 psi64/V80 HPU-native Big-LWE 密文写入 input SLM，执行 FPGA 解密，并从 output SLM 读取连续的 `u8` 明文。

输入支持两种形式：

- `LWEHLS01`：现有加密/远端 HPU 程序保存的逻辑 Big-LWE dump。程序会在 Host 内存中将其重排为 HPU-native 物理布局，dump 中的明文参考值用于逐字节校验。
- `hpu-native`：不带文件头的原始 HPU-native payload。每个 `u8` 固定占 `98304B`；可配合 `--expect-file` 校验。

## 编译

```bash
cd /mnt/suda/host/applications/vscode-lwe-decrypt-offload
make
```

ARM 端的 `/root/software_stack/nf_spdk/config.json` 还必须包含
`operator_type_id=3`、`operator_type_name=lwe_decrypt`、`slot_id=3`，并使用不带
`-b` 参数的 `mcdma/run_nvmq.sh` 启动。修改配置后需要重启 ARM `nvmf_tgt` 和
QEMU/NVMQ 连接，配置不会通过更换 `BOOT.bin` 自动更新。

## 先检查输入文件

该步骤不访问 FPGA，但会用同一 secret key 执行一次 Host 参考解密，检查 dump、重排结果和 key 是否匹配：

```bash
./vscode-lwe-decrypt-offload \
  --input ../vscode-lwe-encrypt-offload/lwe_encrypt_remote_hpu_result_1b.bin \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --inspect-only
```

## 上板验证远端 HPU 的 1B 结果

已知原始明文为 `59`，远端执行 `+1` 后应解密为 `60`：

```bash
./vscode-lwe-decrypt-offload \
  --input ../vscode-lwe-encrypt-offload/lwe_encrypt_remote_hpu_result_1b.bin \
  --expect 60 \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --output lwe_decrypt_remote_hpu_result_1b.bin \
  --benchmark 2>&1 | tee lwe_decrypt_remote_hpu_result_1b.log
```

## 验证 128B 结果

`LWEHLS01` 自带 128 个预期明文字节，因此会自动进行完整逐字节比较：

```bash
./vscode-lwe-decrypt-offload \
  --input ../vscode-lwe-encrypt-offload/lwe_encrypt_remote_hpu_result_128b.bin \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --output lwe_decrypt_remote_hpu_result_128b.bin \
  --benchmark 2>&1 | tee lwe_decrypt_remote_hpu_result_128b.log
```

成功标志包括：

```text
lwe_decrypt FPGA execution passed
correctness_checked=yes
```

若最终网络服务直接返回原始 HPU-native payload，可使用：

```bash
./vscode-lwe-decrypt-offload \
  --input remote_hpu_native.bin \
  --input-format hpu-native \
  --plaintext-bytes 128 \
  --expect-file expected_plaintext_128b.bin \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin
```
