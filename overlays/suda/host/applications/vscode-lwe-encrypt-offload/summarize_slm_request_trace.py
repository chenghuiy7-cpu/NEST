#!/usr/bin/env python3
"""Summarize per-request latency emitted by vscode-lwe-encrypt-offload."""

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    rank = max(0, int(len(ordered) * fraction + 0.999999999) - 1)
    return ordered[min(rank, len(ordered) - 1)]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize per-request output SLM read latency"
    )
    parser.add_argument("trace", type=Path)
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--slow-ms", type=float, default=100.0)
    args = parser.parse_args()

    runs: dict[str, list[dict[str, str]]] = {}
    by_index: dict[int, list[dict[str, str]]] = defaultdict(list)
    with args.trace.open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            runs.setdefault(row["run_nonce"], []).append(row)
            by_index[int(row["request_index"])].append(row)

    if not runs:
        raise SystemExit("SLM request trace contains no samples")

    all_success_ms: list[float] = []
    failed_samples = 0
    complete_runs = 0
    print(
        "| run_nonce | QD | 完成请求 | 预期请求 | 总请求耗时(ms) | "
        "请求P50(ms) | 请求P95(ms) | 最慢(ms) | 最慢序号 |"
    )
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for nonce, rows in runs.items():
        successful = [row for row in rows if int(row["ret"]) == 0]
        failed_samples += len(rows) - len(successful)
        values = [float(row["elapsed_ms"]) for row in successful]
        expected = int(rows[0]["expected_request_count"])
        queue_depth = int(rows[0].get("queue_depth", "") or 1)
        if len(successful) == expected:
            complete_runs += 1
        if not values:
            print(
                f"| {nonce} | {queue_depth} | 0 | {expected} | "
                "n/a | n/a | n/a | n/a | n/a |"
            )
            continue
        all_success_ms.extend(values)
        slowest = max(successful, key=lambda row: float(row["elapsed_ms"]))
        print(
            f"| {nonce} | {queue_depth} | {len(successful)} | {expected} | "
            f"{sum(values):.3f} | "
            f"{statistics.median(values):.3f} | {percentile(values, 0.95):.3f} | "
            f"{float(slowest['elapsed_ms']):.3f} | {slowest['request_index']} |"
        )

    slow_count = sum(value >= args.slow_ms for value in all_success_ms)
    print()
    print(
        f"runs={len(runs)} complete_runs={complete_runs} "
        f"samples={len(all_success_ms)} failed_samples={failed_samples}"
    )
    print(
        "request_latency_ms "
        f"mean={statistics.mean(all_success_ms):.3f} "
        f"p50={statistics.median(all_success_ms):.3f} "
        f"p95={percentile(all_success_ms, 0.95):.3f} "
        f"p99={percentile(all_success_ms, 0.99):.3f} "
        f"max={max(all_success_ms):.3f} "
        f"slow_threshold={args.slow_ms:.3f} slow_count={slow_count}"
    )

    index_summaries = []
    for index, rows in by_index.items():
        successful = [row for row in rows if int(row["ret"]) == 0]
        if not successful:
            continue
        values = [float(row["elapsed_ms"]) for row in successful]
        index_summaries.append(
            {
                "index": index,
                "length": int(successful[0]["length_bytes"]),
                "count": len(values),
                "median": statistics.median(values),
                "p95": percentile(values, 0.95),
                "max": max(values),
                "slow": sum(value >= args.slow_ms for value in values),
            }
        )

    print()
    print(f"按P95排序的最慢请求位置（前{args.top}项）：")
    print("| 请求序号 | 长度(B) | 样本数 | 中位(ms) | P95(ms) | 最大(ms) | 慢请求数 |")
    print("|---:|---:|---:|---:|---:|---:|---:|")
    for summary in sorted(
        index_summaries, key=lambda item: (item["p95"], item["max"]), reverse=True
    )[: args.top]:
        print(
            f"| {summary['index']} | {summary['length']} | {summary['count']} | "
            f"{summary['median']:.3f} | {summary['p95']:.3f} | "
            f"{summary['max']:.3f} | {summary['slow']} |"
        )

    print()
    print(f"全局最慢的前{args.top}个单次请求：")
    print("| run_nonce | 请求序号 | worker | offset(B) | 长度(B) | 延迟(ms) |")
    print("|---|---:|---:|---:|---:|---:|")
    all_rows = [row for rows in runs.values() for row in rows if int(row["ret"]) == 0]
    for row in sorted(
        all_rows, key=lambda item: float(item["elapsed_ms"]), reverse=True
    )[: args.top]:
        print(
            f"| {row['run_nonce']} | {row['request_index']} | "
            f"{row.get('worker_index', 'n/a') or 'n/a'} | "
            f"{row['offset_bytes']} | {row['length_bytes']} | "
            f"{float(row['elapsed_ms']):.3f} |"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
