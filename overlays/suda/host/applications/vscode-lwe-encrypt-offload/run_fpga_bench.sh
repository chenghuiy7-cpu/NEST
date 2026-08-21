#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
APP=${APP:-"${SCRIPT_DIR}/vscode-lwe-encrypt-offload"}
SSD_NSID=${SSD_NSID:-1}
SSD_LBA=${SSD_LBA:-65536}
INPUT_LBAS=${INPUT_LBAS:-1}
SLM_READ_CHUNK_BYTES=${SLM_READ_CHUNK_BYTES:-131072}
SLM_READ_QUEUE_DEPTH=${SLM_READ_QUEUE_DEPTH:-1}
SLM_READ_TRACE=${SLM_READ_TRACE:-}
OUTPUT_LAYOUT=${OUTPUT_LAYOUT:-hpu-native}
KEY=${KEY:-"/mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin"}
BATCH_SIZES=${BATCH_SIZES:-"1 2 4 8 16 32 64 128"}
WARMUP=${WARMUP:-3}
ITERATIONS=${ITERATIONS:-10}
TASKSET_CPU=${TASKSET_CPU:-}
OUTPUT_CSV=${OUTPUT_CSV:-"${SCRIPT_DIR}/fpga_benchmark.csv"}
APPEND=${APPEND:-0}
SETTLE_SECONDS=${SETTLE_SECONDS:-0}

case "${OUTPUT_LAYOUT}" in
    hpu-native)
        OUTPUT_LAYOUT_NAME=hpu-native-psi64-v80
        ;;
    cpu)
        OUTPUT_LAYOUT_NAME=64-byte-padded
        ;;
    *)
        echo "Unsupported OUTPUT_LAYOUT=${OUTPUT_LAYOUT}; use hpu-native or cpu" >&2
        exit 1
        ;;
esac

CSV_HEADER='backend,batch_size,input_lbas,slm_read_chunk_bytes,slm_read_queue_depth,iteration,output_layout,physical_output_bytes_per_u8,slm_create_ms,ssd_to_slm_ms,program_setup_ms,fpga_execute_ms,slm_to_host_ms,host_verify_ms,dump_write_ms,cleanup_ms,transport_ready_ms,data_path_ms,one_shot_transport_ready_ms,one_shot_pipeline_ms,process_ms,per_u8_kernel_us,per_u8_data_path_us,kernel_plaintext_bytes_per_s,data_path_plaintext_bytes_per_s'

if [[ ! -x "${APP}" ]]; then
    echo "FPGA application is not executable: ${APP}" >&2
    exit 1
fi
if [[ ! -f "${KEY}" ]]; then
    echo "Big-LWE key is missing: ${KEY}" >&2
    exit 1
fi

run_app() {
    if [[ -n "${TASKSET_CPU}" ]]; then
        taskset -c "${TASKSET_CPU}" "${APP}" "$@"
    else
        "${APP}" "$@"
    fi
}

run_one() {
    local batch_size=$1
    local trace_path=${2:-}
    local -a args=(
        --ssd-nsid "${SSD_NSID}"
        --ssd-lba "${SSD_LBA}"
        --input-lbas "${INPUT_LBAS}"
        --plaintext-bytes "${batch_size}"
        --slm-read-chunk-bytes "${SLM_READ_CHUNK_BYTES}"
        --slm-read-queue-depth "${SLM_READ_QUEUE_DEPTH}"
        --output-layout "${OUTPUT_LAYOUT}"
        --key "${KEY}"
        --benchmark
        --skip-dump
    )
    if [[ -n "${trace_path}" ]]; then
        args+=(--slm-read-trace "${trace_path}")
    fi
    run_app "${args[@]}"
}

mkdir -p -- "$(dirname -- "${OUTPUT_CSV}")"
if [[ "${APPEND}" == 1 && -s "${OUTPUT_CSV}" ]]; then
    existing_header=$(head -n 1 "${OUTPUT_CSV}")
    if [[ "${existing_header}" != "${CSV_HEADER}" ]]; then
        echo "Existing CSV header does not match: ${OUTPUT_CSV}" >&2
        exit 1
    fi
    echo "Appending to existing FPGA benchmark CSV: ${OUTPUT_CSV}"
else
    printf '%s\n' "${CSV_HEADER}" >"${OUTPUT_CSV}"
fi

echo "FPGA benchmark: sizes=[${BATCH_SIZES}] warmup=${WARMUP} iterations=${ITERATIONS} input_lbas=${INPUT_LBAS} output_layout=${OUTPUT_LAYOUT} slm_read_chunk_bytes=${SLM_READ_CHUNK_BYTES} slm_read_queue_depth=${SLM_READ_QUEUE_DEPTH}"
if [[ -n "${SLM_READ_TRACE}" ]]; then
    echo "SLM request trace (measurement rounds only): ${SLM_READ_TRACE}"
fi
for batch_size in ${BATCH_SIZES}; do
    iteration_base=0
    if [[ "${APPEND}" == 1 ]]; then
        iteration_base=$(awk -F, -v batch="${batch_size}" -v lbas="${INPUT_LBAS}" -v chunk="${SLM_READ_CHUNK_BYTES}" -v qd="${SLM_READ_QUEUE_DEPTH}" -v layout="${OUTPUT_LAYOUT_NAME}" \
            'NR > 1 && $1 == "fpga" && $2 == batch && $3 == lbas && $4 == chunk && $5 == qd && $7 == layout { count++ } END { print count + 0 }' \
            "${OUTPUT_CSV}")
    fi

    echo "[FPGA] batch_size=${batch_size}: warming up"
    for ((round = 0; round < WARMUP; ++round)); do
        run_one "${batch_size}" "" >/dev/null 2>&1
        if [[ "${SETTLE_SECONDS}" != 0 ]]; then
            sleep "${SETTLE_SECONDS}"
        fi
    done

    echo "[FPGA] batch_size=${batch_size}: measuring from iteration=${iteration_base}"
    for ((iteration = 0; iteration < ITERATIONS; ++iteration)); do
        if ! output=$(run_one "${batch_size}" "${SLM_READ_TRACE}"); then
            echo >&2
            echo "FPGA benchmark failed at batch_size=${batch_size} iteration=$((iteration_base + iteration))." >&2
            echo "Completed samples remain in ${OUTPUT_CSV}." >&2
            echo "After recovering the runtime, resume with:" >&2
            echo "APPEND=1 BATCH_SIZES=\"${batch_size}\" WARMUP=0 ITERATIONS=$((ITERATIONS - iteration)) INPUT_LBAS=${INPUT_LBAS} OUTPUT_LAYOUT=${OUTPUT_LAYOUT} SLM_READ_CHUNK_BYTES=${SLM_READ_CHUNK_BYTES} SLM_READ_QUEUE_DEPTH=${SLM_READ_QUEUE_DEPTH} SLM_READ_TRACE=${SLM_READ_TRACE} OUTPUT_CSV=${OUTPUT_CSV} $0" >&2
            exit 1
        fi
        line=$(printf '%s\n' "${output}" | grep '^BENCH_FPGA_CSV,' | tail -n 1)
        prefix="BENCH_FPGA_CSV,${batch_size},${INPUT_LBAS},${SLM_READ_CHUNK_BYTES},${SLM_READ_QUEUE_DEPTH},"
        if [[ "${line}" != "${prefix}${OUTPUT_LAYOUT_NAME},"* ]]; then
            echo "Missing or malformed benchmark line for batch ${batch_size}" >&2
            exit 1
        fi
        printf 'fpga,%s,%s,%s,%s,%d,%s\n' \
            "${batch_size}" \
            "${INPUT_LBAS}" \
            "${SLM_READ_CHUNK_BYTES}" \
            "${SLM_READ_QUEUE_DEPTH}" \
            "$((iteration_base + iteration))" \
            "${line#${prefix}}" >>"${OUTPUT_CSV}"
        printf '.'
        if [[ "${SETTLE_SECONDS}" != 0 ]]; then
            sleep "${SETTLE_SECONDS}"
        fi
    done
    printf '\n'
done

echo "FPGA benchmark CSV: ${OUTPUT_CSV}"
