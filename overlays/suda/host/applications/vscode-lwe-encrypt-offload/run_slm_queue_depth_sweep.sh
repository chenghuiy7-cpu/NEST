#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BENCH_SCRIPT=${BENCH_SCRIPT:-"${SCRIPT_DIR}/run_fpga_bench.sh"}
QUEUE_DEPTHS=${QUEUE_DEPTHS:-"1 2"}
BATCH_SIZES=${BATCH_SIZES:-128}
SLM_READ_CHUNK_BYTES=${SLM_READ_CHUNK_BYTES:-131072}
WARMUP=${WARMUP:-1}
ITERATIONS=${ITERATIONS:-5}
INPUT_LBAS=${INPUT_LBAS:-256}
SETTLE_SECONDS=${SETTLE_SECONDS:-1}
OUTPUT_CSV=${OUTPUT_CSV:-"${SCRIPT_DIR}/slm_queue_depth_sweep.csv"}
SLM_READ_TRACE=${SLM_READ_TRACE:-}
APPEND=${APPEND:-0}

if [[ ! -x "${BENCH_SCRIPT}" ]]; then
    echo "Benchmark script is not executable: ${BENCH_SCRIPT}" >&2
    exit 1
fi

append=${APPEND}
for queue_depth in ${QUEUE_DEPTHS}; do
    echo "===== SLM read queue depth: ${queue_depth} ====="
    APPEND=${append} \
    BATCH_SIZES="${BATCH_SIZES}" \
    WARMUP=${WARMUP} \
    ITERATIONS=${ITERATIONS} \
    INPUT_LBAS=${INPUT_LBAS} \
    SETTLE_SECONDS=${SETTLE_SECONDS} \
    SLM_READ_CHUNK_BYTES=${SLM_READ_CHUNK_BYTES} \
    SLM_READ_QUEUE_DEPTH=${queue_depth} \
    SLM_READ_TRACE="${SLM_READ_TRACE}" \
    OUTPUT_CSV="${OUTPUT_CSV}" \
        "${BENCH_SCRIPT}"
    append=1
done

echo "SLM queue-depth sweep CSV: ${OUTPUT_CSV}"
