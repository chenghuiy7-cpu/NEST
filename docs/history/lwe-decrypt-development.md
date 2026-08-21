# LWE 解密算子开发记录

## 目标

- 新增独立 `lwe_decrypt` HLS 算子，不修改现有 `lwe_encrypt` 接口和 RTL。
- 输入采用 psi64/V80 HPU-native Big-LWE 物理布局。
- 每四个 2-bit radix block 解密并重组为一个连续 u8。
- 2048-bit Big-LWE secret key 通过该算子自己的 SUDA context 传入。
- 输出为打包的连续 u8 AXI Stream，并保持 payload 与 `TUSER=0xff` 结束包分离。

## 修改前备份

- `backups/lwe_decrypt_pre_20260819_142600`
- 备份目录内 `SHA256SUMS` 记录原始文件哈希。

## 初始实现

- 新增 `device/operators/hls/lwe_decrypt/lwe_decrypt.hpp`
- 新增 `device/operators/hls/lwe_decrypt/lwe_decrypt.cpp`
- 新增 `device/operators/hls/lwe_decrypt/test.cpp`
- 新增 `device/operators/hls/lwe_decrypt/run_hls.tcl`
- 将 `lwe_decrypt` 加入 `device/operators/Makefile` 的独立硬件算子目标。

解密关系为：

```text
phase = body - dot(mask, secret_key) mod 2^64
radix_block = round(phase / 2^59) & 0x3
u8 = block0 | block1<<2 | block2<<4 | block3<<6
```

当前实现仅接受已经固定并验证过的 `hpu-native-psi64-v80` 输入格式，避免引入 CPU-LWE/HPU-native 双格式分支影响面积和时序。

## 初步验证

- 本地 C++ 测试通过：65 个 u8、260 个 Big-LWE 全部解密正确。
- Vitis HLS CSIM 通过，0 error。
- 首次 CSYNTH 通过：估算频率 310.10MHz，26,096 LUT、8,381 FF、0 BRAM、0 DSP。
- 根据 SUDA 流接口要求，将 AXIS `register_mode` 显式设为 `off`，并移除对 void return 无效的 `ap_none` pragma，随后重新综合确认资源。

## COSIM 激励调整

- 保留 `LWE_DECRYPT_TEST_U8_COUNT` 编译宏，可继续执行 65B 深度功能测试。
- 默认 HLS testbench 改为 3B，避免 65B HPU-native 输入使 XSIM 生成约 10 万深度的输入 FIFO。
- 本调整只缩小默认仿真激励，不改变 `lwe_decrypt` 算子接口、参数或实现，也不修改现有 `lwe_encrypt`。

验证结果：

- 65B 本地深度测试通过，解密 65 个连续 u8，共处理 260 个 Big-LWE。
- 默认 3B HLS C testbench 通过。
- Verilog 编译和 elaboration 通过，成功生成 `lwe_decrypt` 仿真 snapshot。
- XSIM 2020.2 在启动 snapshot 时仍报告 `ERROR: unknown error occurred`，尚未进入 RTL 时序仿真；该问题与将 FIFO 深度从 99841 降至 4609 无关。
- 因此本次不能声明 C/RTL COSIM clean pass，但目前没有观察到 C 模型错误、RTL 编译错误或 testbench 比对错误。

日志：`device/operators/hls/lwe_decrypt/vitis_hls.cosim_small_20260819.log`。

## RTL 生成和算子池接入

- 生成 `lwe_decrypt_ip.zip`，SHA-256：`3df2a004378633e420b3c0e69eb5b37ac4c4f0e68a9cf5bfdefd782b9f0f042d`。
- 将 5 个 Verilog 文件复制到独立目录 `shell/virt_one_drive/fpga/sources/hlsaccframework/lwe_decrypt/`，没有覆盖 `lwe_encrypt/`。
- 在工程源文件列表中单独加入 `lwe_decrypt/*.v`。
- 新增 `OperatorController_3`、`static_var_bram3` 和 operator type ID `3`。
- 将控制请求、响应、context 恢复和数据 crossbar 从 3 个算子槽位扩展为 4 个；解密数据入口使用 `TDEST=0x30..0x3f`。
- 原有 type ID 保持不变：0/1/2 的实例和连接未改名，`lwe_encrypt` 仍为 type ID 2。

## 算子池 OOC 综合结果

- 使用 Vivado 2020.2 对更新后的 `accframework` 执行 OOC 综合，综合与 DCP 写出均成功：0 error。
- 新 DCP：`work_farm/fpga/vivado_out/shell_virt_one_drive_accframework_fidus/dcp/accframework.dcp`。
- DCP SHA-256：`93d89e10895bf56e6a2ce9c1adbdb5c0ba15f4ef4fbc6b4b023b8728727a1499`。
- OOC 算子池总资源：50,380 CLB LUT、57,860 FF、71 RAMB36、40 RAMB18、10 DSP。
- `lwe_decrypt` HLS 本体在算子池网表中的实际资源：3,722 LUT、3,850 FF、0 BRAM、0 DSP。
- 独立 `OperatorController_3`：2,759 LUT、696 FF、8 RAMB36、1 RAMB18。
- `static_var_bram3`：8 RAMB36，用于独立保存解密配置和 2048-bit Big-LWE secret key context。
- HLS 最终估算时钟为 2.941ns（约 340.02MHz），HLS 估算资源为 26,120 LUT、8,375 FF；实现网表经过常量传播和逻辑优化后明显低于 HLS 估算。

OOC 工程中的 AXIS 时钟关联、BRAM 地址位宽等 critical warning 与现有 `lwe_encrypt` 实例的同类 warning 一致。OOC DCP 未带入完整顶层时钟树，时序报告显示大量未约束路径，因此不能用该报告宣布整机 timing clean；最终结论必须以完整 `build_bd.sh` 的 post-route timing 为准。

## 加密算子隔离性检查

- 将 `lwe_encrypt.cpp`、`lwe_encrypt.hpp`、`test.cpp`、`test_deep.cpp`、`run_hls.tcl` 与修改前备份逐文件计算 SHA-256，五个文件完全一致。
- 解密 RTL 位于独立 `lwe_decrypt/` 目录，没有覆盖算子池中的 `lwe_encrypt/` RTL。
- 原 `lwe_encrypt` 的 operator type ID 2、`TDEST=0x20..0x2f` 和控制器实例保持不变；新解密算子使用 type ID 3、`TDEST=0x30..0x3f`。

## 当前验证边界

- 已通过：65B 连续明文的 C++ 深度测试、HLS CSIM、HLS CSYNTH、RTL 生成、Block Design 生成与验证、算子池 OOC 综合和 DCP 写出。
- 未 clean pass：Vitis HLS COSIM。Verilog 编译、elaboration 和 snapshot 生成成功，但 XSIM 2020.2 在启动 snapshot 时报告通用 `unknown error`，没有进入 testbench 比对阶段。
- 尚未执行：完整 shell implementation、post-route timing、BOOT.bin 生成和上板解密验证。

## 2026-08-19 完整比特流构建结果

- 完整 `build_bd.sh` 流程运行结束，综合、布局布线、bitgen 和 Bootgen 均正常退出。
- `write_bitstream completed successfully`，Bootgen 报告 `Bootimage generated successfully`。
- 生成文件：
  - `BOOT.bin`：37,518,168B，SHA-256 为 `e3745eb93b96709da05485a98585b56924e2d781a03934df241aa16cf6801aaa`。
  - `zynqmp.dtb`：28,488B，SHA-256 为 `1168b1abae245b040f1c6c0d9afa39d9c9e9d1506f23b7fd483ac5016569bdb5`，与此前版本一致。
- 布线状态正常：545,131 条可布线网络全部完成，0 routing error。
- post-route 资源：238,524 CLB LUT（45.63%）、322,872 CLB Register（30.88%）、559.5 BRAM Tile（56.86%）、49 URAM（38.28%）、10 DSP（0.51%）、51,541 CLB（78.88%）。
- post-route setup timing 未通过：WNS=-0.083ns、TNS=-0.593ns，共 11 个 failing endpoints；hold timing 通过。
- 11 条失败路径均位于 250MHz 的 `u_qdma_ep_axi_aclk` QDMA C2H 内部逻辑，最差路径不经过 `lwe_decrypt`。新增算子提高了整体布局布线压力，使上一版 HPU-native 镜像的 WNS=+0.016ns 变为轻微负裕量。

结论：该构建已经产出完整镜像，但不能标记为 timing clean。考虑到本平台此前出现过负时序镜像无法启动的情况，在重新实现获得 WNS>=0 之前，不建议用它覆盖板卡上的已知可用 `BOOT.bin`。

## 2026-08-20 Host 上板验证程序

- 新增独立应用 `host/applications/vscode-lwe-decrypt-offload/`，不修改已跑通的 `vscode-lwe-encrypt-offload`。
- 默认调用 operator type ID 3、program slot 12，与 `lwe_encrypt` 的 type ID 2、program slot 11 分离。
- 输入支持现有 `LWEHLS01` 逻辑密文 dump，以及无文件头的 raw HPU-native payload。
- 对 `LWEHLS01`，程序先读取其中的逻辑 Big-LWE 系数和明文参考值，再按 11-bit bit-reversal、双 PC cut、每 cut 12KB slot 的规则恢复 HPU-native 输入。
- 将 HPU-native payload 从 Host 写入 input SLM，通过 context 传入 2048-bit 二进制 Big-LWE secret key，执行 `lwe_decrypt` 后从 output SLM 读取打包 u8。
- `LWEHLS01` 自带的 clear reference 用于逐字节校验；raw HPU-native 输入可通过 `--expect-file` 或 `--expect` 校验。
- 提供 `--inspect-only`，可在不访问 FPGA 的情况下验证 dump 解析、参数和 HPU-native 重排。
- 提供 `--benchmark`，分别记录 Host 解析/重排、Host->SLM、program setup、FPGA execute、SLM->Host、校验/写盘和清理延迟。

## 首次上板测试：program load 返回 138

- 1B 远端 HPU 结果已通过 Host 参考解密：输入参考值和参考解密结果均为 `0x3c`，说明 dump、key 和 Host 侧 HPU-native 重排一致。
- input/output SLM 创建和 Host->input SLM 写入成功，但 `nvme_load_hlsacc_program` 返回 `138`，尚未进入 FPGA execute。
- 根因是 ARM runtime 启动时读取的 `device/platform/software_stack/nf_spdk/config.json` 只注册了 type ID 0、1、2；硬件新增了 type ID 3，但软件算子配置表未同步。
- 在 `config.json` 中新增 `lwe_decrypt`：`operator_type_id=3`、`slot_id=3`、单输入/单输出、`bram_size=2048`。
- 该修复只修改 runtime 启动配置，不需要重新编译 `nvmf_tgt` 或重新生成 `BOOT.bin`；将新配置复制到 ARM 并重启 `run_nvmq.sh` 后生效。

配置同步后 type ID 3 已能完成 program load 和 activate，原错误 138 消失；首次 execute 返回 `-1`，随后 deactivate/unload 返回 880。此时故障已经从“runtime 未注册算子”推进到“FPGA execute/数据通道阶段”。清理错误属于 execute 异常后的连带现象，根因需结合该次 ARM 日志中的 context、TX、RX 和 callback 最后到达位置判断。Host 程序补充打印 execute 的 `errno` 和等待时长，便于区分中断、超时和设备状态错误。

## 2026-08-20 解密输入流提前结束问题

- ARM 日志确认 type ID 3 的 context 已正确搬入，配置字段为 `[1,2048,0,134217728,2]`，数据路由目的端为 `TDEST=0x30`。
- 单个 u8 的 HPU-native 输入应为 96KB，但首次 TX 提交 24 个 4KB page 后，MCDMA 只累计了 12KB，随即又从 offset 12KB 提交剩余 84KB；第一次提交本身已经包含完整 96KB 和最终结束描述符，因此第二次提交实际是在结束包之后重复发送后 84KB 数据。
- 根因位于 DPDK AXI DMA 完成语义：`rte_axi_dma_send_seg()` 为每个 TX 数据 BD 设置 EOF，而完成轮询将任意 EOF 当作整次 software IO 完成。同一批 24 个 BD 共用一个 IO 指针，因此 IO 会在前几个 BD 完成后被提前返回并复用，最终 16B、`TUSER=0xff` 的任务结束包也会提前到达 HLS。
- 该错误会破坏一次任务的输入边界，并让同一个 software IO 被提前释放/复用；`lwe_decrypt` 可能先完成原始 96KB，也可能受到结束包后的重复 84KB 污染。回调 `result=64` 本身不能区分 1B 解密成功输出和 64B 错误包，最终必须结合输出内容校验；但重复 TX 和 IO 生命周期错误已经由日志与代码对应关系确认。此时 context、密钥和算子路由并非故障点。
- 修复 DPDK TX completion：为每次多 BD 提交只在最后一个描述符设置 `request_end`，轮询到该描述符后才返回一次聚合 completion，不再用每个 BD 的 EOF 判断 software IO 完成。
- 修复 MCDMA 聚合记账：最终 TX completion 包含 96KB payload 和 16B 输入结束包，扣除 16B 后只将有效 payload 累计到 `cur_used`；RX 仍扣除独立的 64B 输出结束包。
- 修改前备份：`backups/lwe_decrypt_tx_completion_pre_20260820_113350`。
- 本修复只涉及 ARM runtime/DPDK 软件，不修改 HLS RTL、算子池和 `BOOT.bin`。
- ARM DPDK AXI DMA 驱动和 SPDK `nvmf_tgt` 已完成交叉编译；编译过程无 error，保留了仓库已有的 warning。
- 新二进制：`device/platform/software_stack/nf_spdk/build/bin/nvmf_tgt.lwe_decrypt_txfix_20260820`。
- 新二进制 SHA-256：`4690c7b5c4c209cbf4db82cf9e2b7c678dbfeb7530b44e3e29f1da84adc374f1`。
- `readelf` 显示 AXI DMA/DPDK 未作为外部动态库依赖，`nm` 也确认 `rte_axi_dma_poll_complete` 和 `rte_axi_dma_send_seg` 已静态链接进新 `nvmf_tgt`，因此 ARM 侧只需替换该可执行文件。
- 最终聚合记账改为 NOTICE 级中文日志，默认启动参数下也会打印 `LWE_FINISH_ACCOUNT`，无需额外开启 `hlsacc` debug log。
- 上板复测的关键预期：单个 u8 应只看到一次 `LWE_TX_SEND`，其 `size=98304`、`iovcnt=24`、`last_data=1`；聚合 TX completion 应为 `98320B`，扣除 16B 结束包后累计 `98304B`，不应再出现从 `cur_used=12288` 发起的 86016B 二次提交。

## 2026-08-20 Host 写入 input SLM 卡在 FETCH_PRP

- 部署 TX completion 修复版 `nvmf_tgt` 后，其 SHA-256 已在 ARM 上核对为 `4690c7b5c4c209cbf4db82cf9e2b7c678dbfeb7530b44e3e29f1da84adc374f1`。
- 新一轮测试停在 Host 的 `writing HPU-native ciphertext to input SLM`，尚未执行 program load，更未进入 `lwe_decrypt` HLS。
- ARM 日志显示 SLM_WRITE 请求进入 `opc=0x5 / FETCH_PRP`：用于搬运 4KB PRP 表的 RX 和 TX 均提交成功，TX completion 已返回，但始终没有对应的 `HANDC_RX_CMPL`。因此请求无法从 `FETCH_PRP` 推进到实际数据写入阶段。
- 该问题是一次 96KB、多页 Host->SLM 写入触发的 runtime PRP-list 路径故障，与解密 context、密钥、HLS 输入流和刚修复的多 BD AXI DMA TX completion 属于不同阶段。
- 为先完成解密算子功能验证，独立解密 Host 应用将 input SLM 写入粒度改为 4KB；单个 u8 的 96KB HPU-native 密文拆为 24 次写入，从而每次请求均走单页直接数据路径，不再进入 `FETCH_PRP`。
- 该调整只改变 Host 到 input SLM 的搬运请求边界。memory range 仍覆盖完整 96KB，执行时 runtime 仍向 `lwe_decrypt` 提交一个完整的 96KB HPU-native 输入流，因此不会改变算子接口和密文布局。
- output SLM 回读仍保留 128KB 的最大请求粒度；当前 1B 明文输出实际只占一个 4KB SLM 页。
- 修改前备份：`backups/lwe_decrypt_host_slm_write_pre_20260820_121534`。该修复只需重新编译并运行 Host 应用，不需要替换 ARM `nvmf_tgt` 或重新生成 `BOOT.bin`。
- Host 应用重新编译成功，仅有 libnvme 头文件中的既有 warning；新二进制 SHA-256 为 `de067082ea80fe26c08caff1979935058568240013aa79575b5b89b1e1d53290`。
- 使用同一份 1B 远端 HPU 结果执行 `--inspect-only` 已通过，解析后的 HPU-native 大小仍为 98,304B，Host 参考解密结果仍为 `0x3c`。

## 2026-08-20 首次上板解密成功

- 输入为远端真实 HPU 对 FPGA 加密密文执行 `ADDS 1` 后返回的 `lwe_encrypt_remote_hpu_result_1b.bin`；原明文为 59，期望计算结果为 60（`0x3c`）。
- Host 首先将 `LWEHLS01` 逻辑密文恢复为 98,304B 的 psi64/V80 HPU-native 物理布局，再以 24 个 4KB 请求写入 input SLM。
- 使用 `taskset -c 1` 固定应用后，全部 Host->SLM 写入成功，program load、activate 和 FPGA execute 均完成。
- FPGA 返回 `result_bytes=64`：当前 OperatorController 以一个 64B AXIS beat 保存物理输出，但其中有效明文为首个 1B；output SLM 本身按页分配为 4,096B。
- FPGA 解密结果为 `0x3c`（60），与命令行期望值、Host 参考解密值一致；程序报告 `correctness_checked=yes` 和 `lwe_decrypt FPGA execution passed`。
- 本次阶段延迟：Host 解析/重排 5.559ms、SLM 创建 13.257ms、Host->SLM 252.294ms、program setup 66.629ms、FPGA execute 8.201ms、SLM->Host 16.362ms、校验/写盘 1.070ms、清理 46.389ms；FPGA pipeline 合计 356.774ms。
- 当前功能结论：`远端 HPU 结果密文 -> Host -> input SLM -> FPGA lwe_decrypt -> u8` 已首次正确跑通，且独立 `lwe_encrypt` 未被修改。
- 当前 runtime 限制：未固定 CPU 时，同一进程的连续 SLM write 会从可用的 `qid=2` 迁移到异常的 `qid=4` 并超时；本次固定 CPU 1 后实际使用可用的 `qid=1`。因此 `taskset -c 1` 是功能验证的临时规避，NVMQ 多队列 H2C 稳定性仍需单独修复。

## 2026-08-20 NVMQ I/O 队列数限制为 3

- Host 的 `host/drivers/nvmq/nvmq0_opts` 原先配置 `nr_io_queues=5`；当前 4-vCPU QEMU 实际建立 `qid=1..4` 四个 I/O 队列，另有管理队列 `qid=0`。
- ARM MCDMA runtime 只有 4 个物理通道，并按逻辑 qid 对 4 取模映射；因此 `qid=4` 会与管理队列 `qid=0` 复用物理 channel 0。实测连续 Host->SLM H2C 写入从 `qid=2` 迁移到 `qid=4` 后，后者只有 H2C submit、没有正常 completion，最终以 `ret=-6` 超时。
- 将 `nr_io_queues` 从 5 固定为 3，使 Host 只建立 `qid=1..3` 三个 I/O 队列；加上 `qid=0` 管理队列后，与 4 个物理 MCDMA 通道一一对应，避免创建会映射回 channel 0 的 `qid=4`。
- 这是 Host 连接配置调整，不修改 ARM `nvmf_tgt`、HLS RTL 或 `BOOT.bin`。配置需在重新初始化 NVMQ 连接时生效；已有连接不能在线改变队列数量。
- 回退值为 `nr_io_queues=5`。验证目标是在不使用 `taskset` 的情况下反复执行解密，并确认内核日志只出现 I/O `qid=1..3`、无 `qid=4`、`ret=-6` 或 I/O timeout。
- 首次无 `taskset` 上板复测通过：24 个 4KB Host->SLM 写入、program load/activate、FPGA execute、SLM 回读和结果校验均正常；FPGA 解密结果为 `0x3c`（60），与远端 HPU `ADDS 1` 的预期一致。
- 该次内核日志实际使用 `qid=1` 和 `qid=3`，说明进程运行期间发生了队列迁移，但两个队列均能完成请求；未出现 `qid=4`、I/O timeout、`ret=-6`、`Failed to send` 或 `QDMA recv error`。这验证了限制为三个 I/O 队列能够避免此前的 qid4/channel0 冲突。
- 本次阶段延迟：Host 解析/重排 6.258ms、SLM 创建 12.785ms、Host->SLM 368.077ms、program setup 63.112ms、FPGA execute 8.578ms、SLM->Host 18.351ms、校验/写盘 1.159ms、清理 49.914ms；FPGA pipeline 合计 470.927ms。单次延迟只用于功能记录，不能替代多轮性能统计。

## 2026-08-20 远端 HPU 128B 结果批量解密通过

- 输入为真实远端 HPU 对 128B FPGA 加密密文执行 `ADDS 1` 后返回的 `lwe_encrypt_remote_hpu_result_128b.bin`；程序识别出 128 个 u8，并恢复为 12,582,912B HPU-native 输入。
- Host 参考解密通过；FPGA `lwe_decrypt` 返回 `result_bytes=128`，最终 128B 明文与参考值逐字节一致，前缀为 `aca548f0b93f53e57b92aa9a9a0be541...`，程序报告 `correctness_checked=yes` 和 `lwe_decrypt FPGA execution passed`。
- 12,582,912B 输入按当前 4KB Host->SLM 粒度拆成 3,072 个请求；输出明文仅 128B，output SLM 按页分配为 4,096B，因此 SLM->Host 实际仍只有一个 4KB read，请求上限 128KB 在本次未用满。
- 本次阶段延迟：解析/重排 144.227ms、SLM 创建 16.657ms、Host->SLM 58,554.418ms、program setup 71.445ms、FPGA execute 25.349ms、SLM->Host 15.236ms、校验/写盘 1.685ms、清理 60.654ms；FPGA pipeline 合计 58,683.139ms。
- 功能结论：`128B 远端 HPU 结果密文 -> Host -> SLM -> FPGA lwe_decrypt -> 128B u8` 已正确跑通。性能瓶颈不是解密算子，而是 3,072 个串行 4KB Host->SLM 请求；后续需要修复多页 SLM write/FETCH_PRP 路径或实现可靠的大粒度写入。
