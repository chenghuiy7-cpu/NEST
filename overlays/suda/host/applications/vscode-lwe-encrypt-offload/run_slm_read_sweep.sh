#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BENCH_SCRIPT=${BENCH_SCRIPT:-"${SCRIPT_DIR}/run_fpga_bench.sh"}
# Start with the useful low-request-count paths. The 4KB legacy path can block
# the NVMQ completion chain after thousands of synchronous requests, so it is
# only exercised when CHUNK_SIZES explicitly includes it.
CHUNK_SIZES=${CHUNK_SIZES:-"131072 32768"}
BATCH_SIZES=${BATCH_SIZES:-128}
WARMUP=${WARMUP:-1}
ITERATIONS=${ITERATIONS:-3}
INPUT_LBAS=${INPUT_LBAS:-256}
SLM_READ_QUEUE_DEPTH=${SLM_READ_QUEUE_DEPTH:-1}
SETTLE_SECONDS=${SETTLE_SECONDS:-0}
OUTPUT_CSV=${OUTPUT_CSV:-"${SCRIPT_DIR}/slm_read_sweep.csv"}
APPEND=${APPEND:-0}

if [[ ! -x "${BENCH_SCRIPT}" ]]; then
    echo "Benchmark script is not executable: ${BENCH_SCRIPT}" >&2
    exit 1
fi

append=${APPEND}
for chunk_bytes in ${CHUNK_SIZES}; do
    echo "===== SLM read chunk: ${chunk_bytes} bytes ====="
    APPEND=${append} \
    BATCH_SIZES="${BATCH_SIZES}" \
    WARMUP=${WARMUP} \
    ITERATIONS=${ITERATIONS} \
    INPUT_LBAS=${INPUT_LBAS} \
    SETTLE_SECONDS=${SETTLE_SECONDS} \
    SLM_READ_CHUNK_BYTES=${chunk_bytes} \
    SLM_READ_QUEUE_DEPTH=${SLM_READ_QUEUE_DEPTH} \
    OUTPUT_CSV="${OUTPUT_CSV}" \
        "${BENCH_SCRIPT}"
    append=1
done

echo "SLM read sweep CSV: ${OUTPUT_CSV}"
