#!/usr/bin/env python3

import argparse
import csv
import statistics
from collections import defaultdict


METRICS = (
    "ssd_to_slm_ms",
    "fpga_execute_ms",
    "slm_to_host_ms",
    "rpc_round_trip_ms",
    "request_send_ms",
    "response_receive_ms",
    "server_request_receive_ms",
    "server_process_ms",
    "server_hpu_wait_sync_ms",
    "server_response_send_ms",
    "server_total_ms",
    "online_e2e_ms",
    "one_shot_e2e_ms",
    "process_ms",
)


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    index = int((len(ordered) - 1) * fraction + 0.999999999)
    return ordered[index]


def median(rows, field):
    return statistics.median(float(row[field]) for row in rows)


def operation_class(operation):
    return "adds" if operation in ("adds", "adds-hpu-native") else operation


def main():
    parser = argparse.ArgumentParser(
        description="Summarize the CSD -> FPGA -> TCP -> remote HPU pipeline benchmark."
    )
    parser.add_argument("csv_path")
    args = parser.parse_args()

    with open(args.csv_path, newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise SystemExit("CSV contains no benchmark samples")

    missing = [field for field in METRICS if field not in rows[0]]
    if missing:
        raise SystemExit(f"CSV is missing fields: {', '.join(missing)}")

    groups = defaultdict(list)
    for row in rows:
        groups[(operation_class(row["operation"]), int(row["batch_size"]))].append(row)

    for (operation, batch_size), samples in groups.items():
        input_lbas = {int(row["input_lbas"]) for row in samples}
        if len(input_lbas) != 1:
            raise SystemExit(
                f"mixed input_lbas for operation={operation}, batch={batch_size}: "
                f"{sorted(input_lbas)}"
            )

    print("| 模式 | 明文(B) | 输入LBA | SSD搬运(B) | 样本 | SSD->SLM(ms) | FPGA(ms) | SLM->Host(ms) | RPC中位(ms) | RPC P95(ms) | 远端处理(ms) | HPU等待+同步(ms) | 在线端到端(ms) |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for (operation, batch_size), samples in sorted(groups.items(), key=lambda item: (item[0][1], item[0][0])):
        rpc_values = [float(row["rpc_round_trip_ms"]) for row in samples]
        input_lbas = int(samples[0]["input_lbas"])
        print(
            f"| {operation} | {batch_size} | {input_lbas} | "
            f"{input_lbas * 4096} | {len(samples)} | "
            f"{median(samples, 'ssd_to_slm_ms'):.3f} | "
            f"{median(samples, 'fpga_execute_ms'):.3f} | "
            f"{median(samples, 'slm_to_host_ms'):.3f} | "
            f"{statistics.median(rpc_values):.3f} | "
            f"{percentile(rpc_values, 0.95):.3f} | "
            f"{median(samples, 'server_process_ms'):.3f} | "
            f"{median(samples, 'server_hpu_wait_sync_ms'):.3f} | "
            f"{median(samples, 'online_e2e_ms'):.3f} |"
        )

    print("\n同载荷消融（中位数）：")
    print("| 明文(B) | echo RPC: 网络+协议(ms) | adds RPC(ms) | adds-echo增量(ms) | 远端HPU等待+同步(ms) | 完整在线端到端(ms) |")
    print("|---:|---:|---:|---:|---:|---:|")
    batch_sizes = sorted({batch for _, batch in groups})
    for batch_size in batch_sizes:
        echo = groups.get(("echo", batch_size))
        adds = groups.get(("adds", batch_size))
        if not echo or not adds:
            continue
        echo_rpc = median(echo, "rpc_round_trip_ms")
        adds_rpc = median(adds, "rpc_round_trip_ms")
        print(
            f"| {batch_size} | {echo_rpc:.3f} | {adds_rpc:.3f} | "
            f"{adds_rpc - echo_rpc:.3f} | "
            f"{median(adds, 'server_hpu_wait_sync_ms'):.3f} | "
            f"{median(adds, 'online_e2e_ms'):.3f} |"
        )

    print("\n说明：echo RPC使用与adds相同大小的密文请求和响应，但远端不调用HPU。")
    print("它表示TCP传输、协议编解码、socket缓冲和调度的合并成本，不等同于单向链路传播时延。")


if __name__ == "__main__":
    main()
