#!/usr/bin/env python3
"""Summarize equal-output CPU and FPGA LWE encryption benchmarks."""

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path


LBA_BYTES = 4096
DONE_PACKET_BYTES = 64
HPU_NATIVE_LAYOUT = "hpu-native-psi64-v80"
HPU_NATIVE_BYTES_PER_U8 = 98_304
CPU_FIELDS = ("encrypt_ms", "native_pack_ms", "encrypt_and_pack_ms")
FPGA_FIELDS = (
    "slm_create_ms",
    "ssd_to_slm_ms",
    "program_setup_ms",
    "fpga_execute_ms",
    "slm_to_host_ms",
    "host_verify_ms",
    "cleanup_ms",
    "transport_ready_ms",
    "data_path_ms",
    "one_shot_transport_ready_ms",
    "one_shot_pipeline_ms",
    "process_ms",
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    rank = max(0, int(len(ordered) * fraction + 0.999999999) - 1)
    return ordered[min(rank, len(ordered) - 1)]


def require_columns(path: Path, fieldnames: list[str] | None, required: set[str]) -> None:
    available = set(fieldnames or [])
    missing = sorted(required - available)
    if missing:
        raise SystemExit(
            f"{path} 缺少新基准字段 {', '.join(missing)}；"
            "请用更新后的 benchmark 重新采样，不能混用旧 CPU-LWE CSV。"
        )


def read_cpu(path: Path, mode: str) -> dict[int, dict[str, list[float]]]:
    grouped: dict[int, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        require_columns(
            path,
            reader.fieldnames,
            {
                "backend",
                "mode",
                "batch_size",
                "output_layout",
                "physical_output_bytes_per_u8",
                *CPU_FIELDS,
            },
        )
        for row in reader:
            if row["backend"] != "cpu" or row["mode"] != mode:
                continue
            if row["output_layout"] != HPU_NATIVE_LAYOUT:
                continue
            physical_bytes = int(row["physical_output_bytes_per_u8"])
            if physical_bytes != HPU_NATIVE_BYTES_PER_U8:
                raise SystemExit(
                    f"{path} 的 CPU HPU-native 大小为 {physical_bytes}B/u8，"
                    f"预期 {HPU_NATIVE_BYTES_PER_U8}B/u8"
                )
            batch_size = int(row["batch_size"])
            for field in CPU_FIELDS:
                grouped[batch_size][field].append(float(row[field]))
    if not grouped:
        raise SystemExit(f"{path} 中没有 mode={mode} 的 HPU-native CPU 样本")
    return grouped


FpgaKey = tuple[int, int, int, str, int]


def read_fpga(path: Path, layout: str) -> dict[FpgaKey, dict[str, list[float]]]:
    grouped: dict[FpgaKey, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        require_columns(
            path,
            reader.fieldnames,
            {
                "backend",
                "batch_size",
                "slm_read_chunk_bytes",
                "slm_read_queue_depth",
                "output_layout",
                "physical_output_bytes_per_u8",
                *FPGA_FIELDS,
            },
        )
        for row in reader:
            if row["backend"] != "fpga" or row["output_layout"] != layout:
                continue
            key = (
                int(row["batch_size"]),
                int(row["slm_read_chunk_bytes"]),
                int(row["slm_read_queue_depth"]),
                row["output_layout"],
                int(row["physical_output_bytes_per_u8"]),
            )
            for field in FPGA_FIELDS:
                grouped[key][field].append(float(row[field]))
    if not grouped:
        raise SystemExit(f"{path} 中没有 output_layout={layout} 的 FPGA 样本")
    return grouped


def ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else float("inf")


def output_slm_bytes(batch_size: int, physical_bytes_per_u8: int) -> int:
    payload_and_done = batch_size * physical_bytes_per_u8 + DONE_PACKET_BYTES
    return ((payload_and_done + LBA_BYTES - 1) // LBA_BYTES) * LBA_BYTES


def slm_throughput_mib_s(
    batch_size: int, physical_bytes_per_u8: int, elapsed_ms: float
) -> float:
    return (
        output_slm_bytes(batch_size, physical_bytes_per_u8)
        / (1024 * 1024)
        / (elapsed_ms / 1000.0)
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare tfhe-rs encryption plus HPU-native packing with SUDA FPGA execution"
    )
    parser.add_argument("--cpu", required=True, type=Path)
    parser.add_argument("--fpga", required=True, type=Path)
    parser.add_argument("--cpu-mode", choices=("serial", "parallel"), default="serial")
    parser.add_argument("--fpga-layout", default=HPU_NATIVE_LAYOUT)
    args = parser.parse_args()

    cpu = read_cpu(args.cpu, args.cpu_mode)
    fpga = read_fpga(args.fpga, args.fpga_layout)
    configurations = sorted(key for key in fpga if key[0] in cpu)
    if not configurations:
        raise SystemExit("CPU 和 FPGA CSV 没有共同的批量大小")

    print(f"CPU mode: {args.cpu_mode}")
    print(f"Output layout: {args.fpga_layout} ({HPU_NATIVE_BYTES_PER_U8} B/u8)")
    print("等价输出加速比 = CPU(加密+HPU-native打包) / FPGA execute；大于1表示FPGA更快。")
    print()
    print(
        "| 批量(B) | SLM读块(KB) | QD | CPU加密(ms) | CPU native打包(ms) | "
        "CPU同层合计(ms) | FPGA执行(ms) | 等价输出加速比 | SLM回读(ms) | "
        "SLM回读(MiB/s) | FPGA传输就绪(ms) | FPGA一次性传输就绪(ms) |"
    )
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for batch_size, chunk_bytes, queue_depth, _, physical_bytes in configurations:
        cpu_samples = cpu[batch_size]
        fpga_samples = fpga[
            (batch_size, chunk_bytes, queue_depth, args.fpga_layout, physical_bytes)
        ]
        encrypt_median = statistics.median(cpu_samples["encrypt_ms"])
        pack_median = statistics.median(cpu_samples["native_pack_ms"])
        cpu_total_median = statistics.median(cpu_samples["encrypt_and_pack_ms"])
        fpga_execute_median = statistics.median(fpga_samples["fpga_execute_ms"])
        slm_read_median = statistics.median(fpga_samples["slm_to_host_ms"])
        transport_median = statistics.median(fpga_samples["transport_ready_ms"])
        one_shot_transport_median = statistics.median(
            fpga_samples["one_shot_transport_ready_ms"]
        )
        print(
            f"| {batch_size} | {chunk_bytes // 1024} | {queue_depth} | "
            f"{encrypt_median:.6f} | {pack_median:.6f} | {cpu_total_median:.6f} | "
            f"{fpga_execute_median:.6f} | "
            f"{ratio(cpu_total_median, fpga_execute_median):.3f}x | "
            f"{slm_read_median:.6f} | "
            f"{slm_throughput_mib_s(batch_size, physical_bytes, slm_read_median):.3f} | "
            f"{transport_median:.6f} | {one_shot_transport_median:.6f} |"
        )

    print()
    print("P95 抖动检查：")
    for batch_size, chunk_bytes, queue_depth, _, physical_bytes in configurations:
        cpu_samples = cpu[batch_size]
        fpga_samples = fpga[
            (batch_size, chunk_bytes, queue_depth, args.fpga_layout, physical_bytes)
        ]
        print(
            f"batch={batch_size} slm_read_chunk_bytes={chunk_bytes} "
            f"slm_read_queue_depth={queue_depth} "
            f"cpu_encrypt_and_pack_p95_ms={percentile(cpu_samples['encrypt_and_pack_ms'], 0.95):.6f} "
            f"fpga_execute_p95_ms={percentile(fpga_samples['fpga_execute_ms'], 0.95):.6f} "
            f"slm_to_host_p95_ms={percentile(fpga_samples['slm_to_host_ms'], 0.95):.6f} "
            f"fpga_transport_ready_p95_ms={percentile(fpga_samples['transport_ready_ms'], 0.95):.6f}"
        )

    print()
    print("说明：FPGA传输就绪 = SSD->SLM + FPGA execute + output SLM->Host，不含Host正确性验证。")
    print("CPU同层合计从Host内存中的明文开始，FPGA execute从input SLM中的明文开始；完整系统比较需另测SSD->Host CPU路径。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
