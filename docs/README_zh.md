# NEST 中文项目导读

NEST 是一套面向近存储同态计算的研究原型，打通了下面这条数据与控制通路：

```text
CSD SSD 明文
  -> SLM
  -> FPGA LWE 加密
  -> Host 内存
  -> TCP
  -> 远端 V80 HPU 同态计算
  -> TCP
  -> Host 内存
  -> SLM
  -> FPGA LWE 解密
  -> CSD SSD 明文结果
```

当前版本已经完成 1B 与 128B 连续 `u8` 数据的功能验证。128B 实验中，
远端 HPU 执行 `ADDS +1` 后，输入首字节由 `0xab` 正确变为 `0xac`，FPGA
解密检查和目标 SSD 回读检查均通过。

## 阅读顺序

1. 阅读 [`architecture.md`](architecture.md)，了解 CSD、Host 和远端 HPU
   三侧模块及数据格式。
2. 阅读 [`reproduction.md`](reproduction.md)，准备两台服务器、Fidus CSD、
   V80 HPU 和工具链。
3. 使用 `scripts/bootstrap.sh` 拉取固定版本的 SUDA 与 TFHE-rs，并应用
   `overlays/` 中的项目改动。
4. 按照 [`benchmarking.md`](benchmarking.md) 重复正确性与性能实验。
5. 遇到 QDMA、SLM 或 bitstream 问题时查看
   [`known-issues.md`](known-issues.md)。

## 仓库边界

NEST 只保存项目相关源码、集成修改、脚本、文档与经过整理的实验结果。
以下内容不会上传：

- Vivado/Vitis HLS 编译目录和临时日志；
- `BOOT.bin`、DCP、bitstream 和 V80 HPU archive；
- TFHE client/server key、LWE secret key 和 SSH key；
- 大型明文、密文 dump；
- 完整复制的 Linux、QEMU、SUDA、SPDK 与 TFHE-rs 源码树。

这样既能让读者看清 NEST 自己修改了什么，也能避免仓库膨胀到几十 GB。
