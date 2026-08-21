#!/usr/bin/env python3

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


ONLINE_STAGES = (
    ("ssd_to_slm", "ssd_to_slm_ms"),
    ("program_setup", "program_setup_ms"),
    ("fpga_execute", "fpga_execute_ms"),
    ("slm_to_host", "slm_to_host_ms"),
    ("fpga_unpack_verify", "fpga_unpack_verify_ms"),
    ("csd_cleanup", "csd_cleanup_ms"),
    ("tcp_connect", "tcp_connect_ms"),
    ("rpc_round_trip", "rpc_round_trip_ms"),
)

REMOTE_PROCESS_STAGES = (
    ("request_validate", "server_request_validate_ms"),
    ("request_decode", "server_request_decode_ms"),
    ("hpu_prepare", "server_hpu_prepare_ms"),
    ("hpu_enqueue", "server_hpu_enqueue_ms"),
    ("hpu_wait_sync", "server_hpu_wait_sync_ms"),
    ("hpu_output_convert", "server_hpu_output_convert_ms"),
    ("result_encode", "server_result_encode_ms"),
    ("mem_sanitizer", "server_mem_sanitizer_ms"),
)


def values(rows, field):
    return [float(row[field]) for row in rows]


def mean(rows, field):
    return statistics.mean(values(rows, field))


def median(rows, field):
    return statistics.median(values(rows, field))


def percentile(raw_values, fraction):
    ordered = sorted(raw_values)
    index = max(0, math.ceil(len(ordered) * fraction) - 1)
    return ordered[index]


def p95(rows, field):
    return percentile(values(rows, field), 0.95)


def operation_class(operation):
    return "adds" if operation in ("adds", "adds-hpu-native") else operation


def row_float_sum(row, fields):
    return sum(float(row[field]) for field in fields)


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def fmt(value):
    return f"{value:.3f}"


def main():
    parser = argparse.ArgumentParser(
        description="Prepare publication-oriented ablation data from the remote LWE pipeline CSV."
    )
    parser.add_argument("source_csv")
    parser.add_argument("output_dir")
    args = parser.parse_args()

    source_path = Path(args.source_csv)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    with source_path.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise SystemExit("source CSV has no samples")

    required = {
        "operation",
        "batch_size",
        "input_lbas",
        "online_e2e_ms",
        "one_shot_e2e_ms",
        "server_process_ms",
        "server_total_ms",
        "server_request_receive_ms",
        "server_response_send_ms",
        "request_payload_bytes",
        "response_payload_bytes",
    }
    required.update(field for _, field in ONLINE_STAGES)
    required.update(field for _, field in REMOTE_PROCESS_STAGES)
    missing = sorted(required.difference(rows[0]))
    if missing:
        raise SystemExit(f"source CSV is missing fields: {', '.join(missing)}")

    groups = defaultdict(list)
    for row in rows:
        groups[(operation_class(row["operation"]), int(row["batch_size"]))].append(row)

    batch_sizes = sorted({batch_size for _, batch_size in groups})
    for batch_size in batch_sizes:
        if ("adds", batch_size) not in groups or ("echo", batch_size) not in groups:
            raise SystemExit(f"batch {batch_size} must contain both echo and adds samples")

    # Figure 1: mutually exclusive online stages. Means are used because their
    # sum (plus the measured residual) exactly equals mean online_e2e_ms.
    online_rows = []
    online_fields = [field for _, field in ONLINE_STAGES]
    for batch_size in batch_sizes:
        samples = groups[("adds", batch_size)]
        output = {
            "batch_size_bytes": batch_size,
            "sample_count": len(samples),
            "input_lbas": int(samples[0]["input_lbas"]),
            "ssd_copy_bytes": int(samples[0]["input_lbas"]) * 4096,
            "request_payload_bytes": int(samples[0]["request_payload_bytes"]),
            "response_payload_bytes": int(samples[0]["response_payload_bytes"]),
        }
        for stage_name, field in ONLINE_STAGES:
            output[f"{stage_name}_mean_ms"] = mean(samples, field)
            output[f"{stage_name}_p50_ms"] = median(samples, field)
            output[f"{stage_name}_p95_ms"] = p95(samples, field)
        residuals = [
            float(row["online_e2e_ms"]) - row_float_sum(row, online_fields)
            for row in samples
        ]
        output["orchestration_residual_mean_ms"] = statistics.mean(residuals)
        output["online_e2e_mean_ms"] = mean(samples, "online_e2e_ms")
        output["online_e2e_p50_ms"] = median(samples, "online_e2e_ms")
        output["online_e2e_p95_ms"] = p95(samples, "online_e2e_ms")
        output["slm_create_p50_ms"] = median(samples, "slm_create_ms")
        output["one_shot_e2e_p50_ms"] = median(samples, "one_shot_e2e_ms")
        online_rows.append(output)

    online_column_order = [
        "batch_size_bytes",
        "sample_count",
        "input_lbas",
        "ssd_copy_bytes",
        "request_payload_bytes",
        "response_payload_bytes",
    ]
    for stage_name, _ in ONLINE_STAGES:
        online_column_order.extend(
            [
                f"{stage_name}_mean_ms",
                f"{stage_name}_p50_ms",
                f"{stage_name}_p95_ms",
            ]
        )
    online_column_order.extend(
        [
            "orchestration_residual_mean_ms",
            "online_e2e_mean_ms",
            "online_e2e_p50_ms",
            "online_e2e_p95_ms",
            "slm_create_p50_ms",
            "one_shot_e2e_p50_ms",
        ]
    )
    write_csv(
        output_dir / "figure1_online_e2e_stage_data.csv",
        online_column_order,
        online_rows,
    )
    online_long_rows = []
    for row in online_rows:
        for stage_name, _ in ONLINE_STAGES:
            for statistic in ("mean", "p50", "p95"):
                online_long_rows.append(
                    {
                        "batch_size_bytes": row["batch_size_bytes"],
                        "stage": stage_name,
                        "statistic": statistic,
                        "latency_ms": row[f"{stage_name}_{statistic}_ms"],
                    }
                )
        online_long_rows.append(
            {
                "batch_size_bytes": row["batch_size_bytes"],
                "stage": "orchestration_residual",
                "statistic": "mean",
                "latency_ms": row["orchestration_residual_mean_ms"],
            }
        )
    write_csv(
        output_dir / "figure1_online_e2e_stage_long.csv",
        ["batch_size_bytes", "stage", "statistic", "latency_ms"],
        online_long_rows,
    )

    # Figure 2: same-payload echo/adds ablation. Difference-of-medians is an
    # experimental increment, not a pure RTL execution time.
    rpc_rows = []
    for batch_size in batch_sizes:
        echo = groups[("echo", batch_size)]
        adds = groups[("adds", batch_size)]
        echo_rpc = median(echo, "rpc_round_trip_ms")
        adds_rpc = median(adds, "rpc_round_trip_ms")
        echo_e2e = median(echo, "online_e2e_ms")
        adds_e2e = median(adds, "online_e2e_ms")
        rpc_rows.append(
            {
                "batch_size_bytes": batch_size,
                "sample_count_per_mode": len(adds),
                "request_payload_bytes": int(adds[0]["request_payload_bytes"]),
                "echo_rpc_p50_ms": echo_rpc,
                "echo_rpc_p95_ms": p95(echo, "rpc_round_trip_ms"),
                "adds_rpc_p50_ms": adds_rpc,
                "adds_rpc_p95_ms": p95(adds, "rpc_round_trip_ms"),
                "adds_minus_echo_rpc_p50_ms": adds_rpc - echo_rpc,
                "server_hpu_wait_sync_p50_ms": median(adds, "server_hpu_wait_sync_ms"),
                "server_process_p50_ms": median(adds, "server_process_ms"),
                "echo_online_e2e_p50_ms": echo_e2e,
                "adds_online_e2e_p50_ms": adds_e2e,
                "adds_minus_echo_online_e2e_p50_ms": adds_e2e - echo_e2e,
            }
        )
    write_csv(
        output_dir / "figure2_rpc_ablation_data.csv",
        list(rpc_rows[0]),
        rpc_rows,
    )

    # Figure 3: mutually exclusive stages inside process_request() on server
    # 129. Request receive and response send sit outside remote_process.
    remote_rows = []
    remote_fields = [field for _, field in REMOTE_PROCESS_STAGES]
    for batch_size in batch_sizes:
        samples = groups[("adds", batch_size)]
        output = {
            "batch_size_bytes": batch_size,
            "sample_count": len(samples),
            "request_payload_bytes": int(samples[0]["request_payload_bytes"]),
        }
        for stage_name, field in REMOTE_PROCESS_STAGES:
            output[f"{stage_name}_mean_ms"] = mean(samples, field)
            output[f"{stage_name}_p50_ms"] = median(samples, field)
            output[f"{stage_name}_p95_ms"] = p95(samples, field)
        process_residuals = [
            float(row["server_process_ms"]) - row_float_sum(row, remote_fields)
            for row in samples
        ]
        total_residuals = [
            float(row["server_total_ms"])
            - float(row["server_request_receive_ms"])
            - float(row["server_process_ms"])
            - float(row["server_response_send_ms"])
            for row in samples
        ]
        output["remote_process_residual_mean_ms"] = statistics.mean(process_residuals)
        output["remote_process_mean_ms"] = mean(samples, "server_process_ms")
        output["remote_process_p50_ms"] = median(samples, "server_process_ms")
        output["remote_process_p95_ms"] = p95(samples, "server_process_ms")
        output["server_request_receive_mean_ms"] = mean(samples, "server_request_receive_ms")
        output["server_response_send_mean_ms"] = mean(samples, "server_response_send_ms")
        output["server_total_residual_mean_ms"] = statistics.mean(total_residuals)
        output["server_total_mean_ms"] = mean(samples, "server_total_ms")
        output["server_total_p50_ms"] = median(samples, "server_total_ms")
        output["server_total_p95_ms"] = p95(samples, "server_total_ms")
        remote_rows.append(output)

    remote_column_order = [
        "batch_size_bytes",
        "sample_count",
        "request_payload_bytes",
    ]
    for stage_name, _ in REMOTE_PROCESS_STAGES:
        remote_column_order.extend(
            [
                f"{stage_name}_mean_ms",
                f"{stage_name}_p50_ms",
                f"{stage_name}_p95_ms",
            ]
        )
    remote_column_order.extend(
        [
            "remote_process_residual_mean_ms",
            "remote_process_mean_ms",
            "remote_process_p50_ms",
            "remote_process_p95_ms",
            "server_request_receive_mean_ms",
            "server_response_send_mean_ms",
            "server_total_residual_mean_ms",
            "server_total_mean_ms",
            "server_total_p50_ms",
            "server_total_p95_ms",
        ]
    )
    write_csv(
        output_dir / "figure3_remote_server_stage_data.csv",
        remote_column_order,
        remote_rows,
    )
    remote_long_rows = []
    for row in remote_rows:
        for stage_name, _ in REMOTE_PROCESS_STAGES:
            for statistic in ("mean", "p50", "p95"):
                remote_long_rows.append(
                    {
                        "batch_size_bytes": row["batch_size_bytes"],
                        "stage": stage_name,
                        "statistic": statistic,
                        "latency_ms": row[f"{stage_name}_{statistic}_ms"],
                    }
                )
        remote_long_rows.append(
            {
                "batch_size_bytes": row["batch_size_bytes"],
                "stage": "remote_process_residual",
                "statistic": "mean",
                "latency_ms": row["remote_process_residual_mean_ms"],
            }
        )
    write_csv(
        output_dir / "figure3_remote_server_stage_long.csv",
        ["batch_size_bytes", "stage", "statistic", "latency_ms"],
        remote_long_rows,
    )

    report = output_dir / "端到端消融画图数据说明.md"
    with report.open("w", encoding="utf-8") as output:
        output.write("# 端到端消融画图数据说明\n\n")
        output.write(f"原始数据：`{source_path.resolve()}`\n\n")
        request_bytes_per_u8 = sorted(
            {
                int(row["request_payload_bytes"]) // int(row["batch_size"])
                for row in rows
            }
        )
        response_bytes_per_u8 = sorted(
            {
                int(row["response_payload_bytes"]) // int(row["batch_size"])
                for row in rows
            }
        )
        output.write(
            "网络载荷：每个 u8 的请求载荷为 "
            f"`{request_bytes_per_u8}`B，响应载荷为 `{response_bytes_per_u8}`B。\n\n"
        )
        sample_counts = sorted({len(samples) for samples in groups.values()})
        input_layout = sorted(
            {
                (int(row["batch_size"]), int(row["input_lbas"]))
                for row in rows
            }
        )
        input_layout_text = "、".join(
            f"{batch}B->{lbas} LBA ({lbas * 4096}B)"
            for batch, lbas in input_layout
        )
        output.write(
            f"每组测量样本数为 `{sample_counts}`。SSD 到 SLM 实际搬运口径："
            f"{input_layout_text}。\n\n"
        )
        output.write("## 图1：完整在线端到端阶段分解\n\n")
        output.write(
            "使用 `figure1_online_e2e_stage_data.csv`。若画堆叠柱状图，应选各阶段的 "
            "`*_mean_ms` 和 `orchestration_residual_mean_ms`，它们严格加和为 "
            "`online_e2e_mean_ms`。P50/P95 用于报告典型值和长尾，不应把各字段 P50 "
            "强行解释为一个可精确加和的端到端样本。\n\n"
        )
        output.write("严格可加和的阶段均值（ms）：\n\n")
        output.write("| 明文(B) | SSD->SLM | 配置程序 | FPGA加密 | SLM->Host | Host解包验证 | CSD清理 | TCP连接 | RPC | 其他调度 | 在线E2E均值 |\n")
        output.write("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for row in online_rows:
            output.write(
                f"| {row['batch_size_bytes']} | {fmt(row['ssd_to_slm_mean_ms'])} | "
                f"{fmt(row['program_setup_mean_ms'])} | {fmt(row['fpga_execute_mean_ms'])} | "
                f"{fmt(row['slm_to_host_mean_ms'])} | {fmt(row['fpga_unpack_verify_mean_ms'])} | "
                f"{fmt(row['csd_cleanup_mean_ms'])} | {fmt(row['tcp_connect_mean_ms'])} | "
                f"{fmt(row['rpc_round_trip_mean_ms'])} | "
                f"{fmt(row['orchestration_residual_mean_ms'])} | "
                f"{fmt(row['online_e2e_mean_ms'])} |\n"
            )
        output.write("\n各阶段 P50 与端到端 P50/P95（ms）：\n\n")
        output.write("| 明文(B) | SSD->SLM | 配置程序 | FPGA加密 | SLM->Host | Host解包验证 | CSD清理 | TCP连接 | RPC | 在线E2E P50 | 在线E2E P95 |\n")
        output.write("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for batch_size in batch_sizes:
            samples = groups[("adds", batch_size)]
            output.write(
                f"| {batch_size} | {fmt(median(samples, 'ssd_to_slm_ms'))} | "
                f"{fmt(median(samples, 'program_setup_ms'))} | "
                f"{fmt(median(samples, 'fpga_execute_ms'))} | "
                f"{fmt(median(samples, 'slm_to_host_ms'))} | "
                f"{fmt(median(samples, 'fpga_unpack_verify_ms'))} | "
                f"{fmt(median(samples, 'csd_cleanup_ms'))} | "
                f"{fmt(median(samples, 'tcp_connect_ms'))} | "
                f"{fmt(median(samples, 'rpc_round_trip_ms'))} | "
                f"{fmt(median(samples, 'online_e2e_ms'))} | "
                f"{fmt(p95(samples, 'online_e2e_ms'))} |\n"
            )
        output.write("\n## 图2：同载荷 RPC 消融\n\n")
        output.write(
            "使用 `figure2_rpc_ablation_data.csv`。`echo` 传输与 `adds` 等大的请求和响应，"
            "但不调用 HPU，表示 TCP、协议、socket 缓冲、内存复制与调度的组合基线。"
            "`adds-echo` 是远端计算栈增量估计，不是纯 HPU RTL 时间。\n\n"
        )
        output.write("| 明文(B) | 网络载荷/方向(B) | Echo RPC P50 | Adds RPC P50 | Adds-Echo | HPU等待+同步 P50 |\n")
        output.write("|---:|---:|---:|---:|---:|---:|\n")
        for row in rpc_rows:
            output.write(
                f"| {row['batch_size_bytes']} | {row['request_payload_bytes']} | "
                f"{fmt(row['echo_rpc_p50_ms'])} | {fmt(row['adds_rpc_p50_ms'])} | "
                f"{fmt(row['adds_minus_echo_rpc_p50_ms'])} | "
                f"{fmt(row['server_hpu_wait_sync_p50_ms'])} |\n"
            )
        output.write("\n## 图3：129 服务器内部阶段\n\n")
        output.write(
            "使用 `figure3_remote_server_stage_data.csv`。堆叠远端处理时同样使用 `*_mean_ms`。"
            "其中 `hpu_wait_sync` 同时包含输入同步、HPU 执行等待和结果 Device->Host 同步，"
            "当前公开 API 无法继续拆成纯 H2D、RTL 和 D2H。\n\n"
        )
        output.write("| 明文(B) | Decode | HPU准备 | HPU入队 | HPU等待+同步 | HPU输出转换 | 结果编码 | Sanitizer | 远端处理P50 |\n")
        output.write("|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for batch_size in batch_sizes:
            samples = groups[("adds", batch_size)]
            output.write(
                f"| {batch_size} | {fmt(median(samples, 'server_request_decode_ms'))} | "
                f"{fmt(median(samples, 'server_hpu_prepare_ms'))} | "
                f"{fmt(median(samples, 'server_hpu_enqueue_ms'))} | "
                f"{fmt(median(samples, 'server_hpu_wait_sync_ms'))} | "
                f"{fmt(median(samples, 'server_hpu_output_convert_ms'))} | "
                f"{fmt(median(samples, 'server_result_encode_ms'))} | "
                f"{fmt(median(samples, 'server_mem_sanitizer_ms'))} | "
                f"{fmt(median(samples, 'server_process_ms'))} |\n"
            )
        output.write("\n## 计时边界\n\n")
        output.write("- `online_e2e`：SSD 开始搬入 input SLM，到远端结果密文完整进入 132 Host 内存。\n")
        output.write("- `slm_create`：不包含在 `online_e2e`，但包含在 `one_shot_e2e`。\n")
        output.write("- `host_result_verify` 和结果落盘：发生在 `online_e2e` 结束之后。\n")
        output.write("- `fpga_execute`：SUDA execute 命令的同步延迟，不是单独的纯 RTL cycle 时间。\n")
        output.write("- `rpc_round_trip`：不含 TCP 建连；建连由 `tcp_connect` 单独统计。\n")

    print(f"source_samples={len(rows)}")
    print(f"output_dir={output_dir.resolve()}")
    for path in sorted(output_dir.iterdir()):
        print(path.name)


if __name__ == "__main__":
    main()
