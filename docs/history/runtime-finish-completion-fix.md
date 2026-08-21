# LWE 输出结束 completion 记账修复（2026-07-13）

## 1. 上板现象

新比特流运行后，ARM 日志显示：

```text
LWE_CTX_VERIFY ... staged_u32=[2048,1,2,0,17] static_u32=[2048,1,2,0,17]
LWE_CH_CMPL RX完成 ... bytes=4096 tuser=0x0 result_before=0
...
LWE_CH_CMPL RX完成 ... bytes=4096 tuser=0x0 result_before=61440
LWE_CH_CMPL RX完成 ... bytes=320 tuser=0xff result_before=65536
HLSACC_REQ_CB ... result=65536
```

这证明 HLS context 偏移修复已经生效，算子不再输出 `LWEERROR`，并且已经产生密文 payload。剩余错误位于 ARM runtime 对最后一个 RX completion 的长度记账。

## 2. 根因

一个 u8 被拆成 4 个 radix block，每个 Big-LWE 密文包含 2049 个 u64。HLS 按 512 bit AXIS beat 输出，每个密文最后一个不满 64B 的 beat 会按 64B 物理包发送，因此物理 payload 为：

```text
4 × ceil(2049 × 8 / 64) × 64 = 65792B
```

SUDA 在 payload 后附加一个 512 bit、`TUSER=0xff` 的任务结束 beat，即 64B。AXI DMA 将最后 256B payload 和 64B 结束 beat 合并成一次 320B completion：

```text
前面已经累计：65536B
最后 completion：256B payload + 64B finish beat = 320B
```

`mcdma.c` 原逻辑只要发现 completion 的最终 `TUSER=0xff`，就直接结束请求，没有累计该 completion 中位于结束 beat 前面的 payload，因此最终只返回 65536B。

## 3. 修改内容

修改文件：

```text
device/platform/software_stack/nf_spdk/lib/nvmf/mcdma.c
```

新增结束 beat 长度定义：

```c
#define MCDMA_TASK_FINISH_BEAT_SIZE MCDMA_RX_BUF_SZ
```

RX completion 带 `TUSER=0xff` 时执行：

```c
payload_bytes = finish_completion_bytes - MCDMA_TASK_FINISH_BEAT_SIZE;
transfered_length += payload_bytes;
```

同时加入下溢保护：completion 非零但小于 64B 时记录错误，并按 0B payload 处理。

本次 320B completion 的预期记账为：

```text
payload_bytes = 320 - 64 = 256
request->result = 65536 + 256 = 65792
output SLM cur_used = 65792
```

结束 beat 仍保留在 output SLM 的 payload 后方。Host 依据 `result=65792` 只读取有效 payload，不需要重新拆分 DMA completion。

## 4. 编译验证

执行：

```bash
cd "${SUDA_ROOT}/device/platform/software_stack/nf_spdk"
bash scripts/arm_cross_compile.sh
```

结果：ARM 交叉编译和 `nvmf_tgt` 链接成功。项目原有 warning 仍存在，本次修改没有引入编译错误。

生成文件：

```text
device/platform/software_stack/nf_spdk/build/bin/nvmf_tgt
device/platform/software_stack/nf_spdk/build/bin/nvmf_tgt.lwe_finish_account_20260713
```

文件类型：AArch64 ELF，动态链接，解释器 `/lib/ld-linux-aarch64.so.1`。

SHA256：

```text
ef5c54105ac68ea55f592622d705fedee1d14ac93baf9369af36bacaf6376865
```

编译前本地 `nvmf_tgt` 的 SHA256 为：

```text
b1663fb16a6951c32b488c2a274bd70dae6b43f5be6dad917c79cfe53f66b2eb
```

## 5. ARM 部署与回滚

先从 x86 复制新程序，替换 `<ARM_IP>`：

```bash
scp "${SUDA_ROOT}/device/platform/software_stack/nf_spdk/build/bin/nvmf_tgt.lwe_finish_account_20260713" \
  root@<ARM_IP>:/tmp/nvmf_tgt.lwe_finish_account_20260713
```

在 ARM 上先备份当前程序：

```bash
cd /root/software_stack/nf_spdk/build/bin
cp -a nvmf_tgt nvmf_tgt.before_finish_account_20260713
sha256sum nvmf_tgt nvmf_tgt.before_finish_account_20260713
```

停止当前 `nvmf_tgt` 后替换：

```bash
cp /tmp/nvmf_tgt.lwe_finish_account_20260713 /root/software_stack/nf_spdk/build/bin/nvmf_tgt
chmod 755 /root/software_stack/nf_spdk/build/bin/nvmf_tgt
sha256sum /root/software_stack/nf_spdk/build/bin/nvmf_tgt
```

预期哈希是 `ef5c54105ac68ea55f592622d705fedee1d14ac93baf9369af36bacaf6376865`。随后重新启动 ARM 端 `run_nvmq.sh`，并重新连接 QEMU 侧 NVMQ。

若需要回滚：

```bash
cp /root/software_stack/nf_spdk/build/bin/nvmf_tgt.before_finish_account_20260713 \
  /root/software_stack/nf_spdk/build/bin/nvmf_tgt
```

## 6. 上板验证预期

运行 LWE Host 程序后，ARM 日志应出现：

```text
LWE_RX_FINISH_ACCOUNT ... bytes=320 payload_bytes=256 finish_bytes=64 result_before=65536
HLSACC_REQ_CB ... result=65792
```

QEMU Host 应继续执行 output SLM 读取，不再因 `result_bytes < 65792` 提前失败。随后应检查 65792B 物理 payload 的结构及 ClientKey 解密结果。

本次只修改 ARM runtime，不需要重新生成 HLS IP、Verilog 或整板比特流。
