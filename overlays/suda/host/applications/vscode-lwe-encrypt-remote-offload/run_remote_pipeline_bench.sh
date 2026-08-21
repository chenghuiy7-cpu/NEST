#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
APP=${APP:-"${SCRIPT_DIR}/vscode-lwe-encrypt-remote-offload"}
BATCH_SIZES=${BATCH_SIZES:-"1 16 32 128"}
REMOTE_OPERATIONS=${REMOTE_OPERATIONS:-"echo adds"}
WARMUP=${WARMUP:-2}
ITERATIONS=${ITERATIONS:-10}
SETTLE_SECONDS=${SETTLE_SECONDS:-1}
SSD_NSID=${SSD_NSID:-1}
SSD_LBA=${SSD_LBA:-65536}
INPUT_LBAS=${INPUT_LBAS:-auto}
SLM_READ_CHUNK_BYTES=${SLM_READ_CHUNK_BYTES:-131072}
REMOTE_HOST=${REMOTE_HOST:-127.0.0.1}
REMOTE_PORT=${REMOTE_PORT:-19090}
SCALAR=${SCALAR:-1}
KEY=${KEY:-/mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin}
OUTPUT_CSV=${OUTPUT_CSV:-remote_pipeline_benchmark_auto_lba.csv}
APPEND=${APPEND:-0}

CSV_HEADER='operation,batch_size,input_lbas,slm_read_chunk_bytes,request_id,iteration,slm_create_ms,ssd_to_slm_ms,program_setup_ms,fpga_execute_ms,slm_to_host_ms,fpga_unpack_verify_ms,csd_cleanup_ms,tcp_connect_ms,request_send_ms,wait_response_header_ms,response_receive_ms,telemetry_receive_ms,rpc_round_trip_ms,server_request_receive_ms,server_request_validate_ms,server_request_decode_ms,server_hpu_prepare_ms,server_hpu_enqueue_ms,server_hpu_wait_sync_ms,server_hpu_output_convert_ms,server_result_encode_ms,server_mem_sanitizer_ms,server_process_ms,server_response_send_ms,server_total_ms,host_result_verify_ms,dump_write_ms,online_e2e_ms,one_shot_e2e_ms,process_ms,rpc_minus_server_total_ms,request_payload_bytes,response_payload_bytes'

if [[ ! -x "${APP}" ]]; then
    echo "Benchmark application is not executable: ${APP}" >&2
    exit 1
fi
if [[ ! -f "${KEY}" ]]; then
    echo "Secret key file not found: ${KEY}" >&2
    exit 1
fi
if [[ "${APPEND}" != 1 || ! -s "${OUTPUT_CSV}" ]]; then
    printf '%s\n' "${CSV_HEADER}" > "${OUTPUT_CSV}"
elif [[ "$(head -n 1 "${OUTPUT_CSV}")" != "${CSV_HEADER}" ]]; then
    echo "Existing CSV header does not match this benchmark version: ${OUTPUT_CSV}" >&2
    exit 1
fi

common_args=(
    --ssd-nsid "${SSD_NSID}"
    --ssd-lba "${SSD_LBA}"
    --slm-read-chunk-bytes "${SLM_READ_CHUNK_BYTES}"
    --key "${KEY}"
    --server "${REMOTE_HOST}"
    --server-port "${REMOTE_PORT}"
    --scalar "${SCALAR}"
    --skip-dump
)

if [[ "${INPUT_LBAS}" != auto && ! "${INPUT_LBAS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "INPUT_LBAS must be 'auto' or a positive integer: ${INPUT_LBAS}" >&2
    exit 1
fi

echo "Remote pipeline benchmark: operations=[${REMOTE_OPERATIONS}] sizes=[${BATCH_SIZES}] warmup=${WARMUP} iterations=${ITERATIONS} input_lbas=${INPUT_LBAS}"

for operation in ${REMOTE_OPERATIONS}; do
    case "${operation}" in
        echo)
            cli_operation=echo
            record_operation=echo
            ;;
        adds|adds-hpu-native)
            cli_operation=adds
            record_operation=adds-hpu-native
            ;;
        *)
            echo "Unsupported REMOTE_OPERATIONS entry: ${operation}" >&2
            exit 1
            ;;
    esac
    for batch_size in ${BATCH_SIZES}; do
        if [[ ! "${batch_size}" =~ ^[1-9][0-9]*$ ]]; then
            echo "Invalid batch size: ${batch_size}" >&2
            exit 1
        fi
        if [[ "${INPUT_LBAS}" == auto ]]; then
            actual_input_lbas=$(((batch_size + 4095) / 4096))
            input_lba_args=()
        else
            actual_input_lbas=${INPUT_LBAS}
            input_lba_args=(--input-lbas "${actual_input_lbas}")
        fi

        echo "[${operation}] batch_size=${batch_size} input_lbas=${actual_input_lbas}: warming up"
        for ((iteration = 0; iteration < WARMUP; ++iteration)); do
            "${APP}" "${common_args[@]}" "${input_lba_args[@]}" \
                --plaintext-bytes "${batch_size}" \
                --remote-operation "${cli_operation}" \
                --benchmark \
                >/dev/null 2>&1
            sleep "${SETTLE_SECONDS}"
        done

        echo "[${operation}] batch_size=${batch_size} input_lbas=${actual_input_lbas}: measuring"
        for ((iteration = 0; iteration < ITERATIONS; ++iteration)); do
            output=$("${APP}" "${common_args[@]}" "${input_lba_args[@]}" \
                --plaintext-bytes "${batch_size}" \
                --remote-operation "${cli_operation}" \
                --benchmark 2>&1)
            printf '%s\n' "${output}"
            line=$(printf '%s\n' "${output}" | grep '^BENCH_REMOTE_PIPELINE_CSV,' | tail -n 1)
            prefix="BENCH_REMOTE_PIPELINE_CSV,${record_operation},${batch_size},${actual_input_lbas},${SLM_READ_CHUNK_BYTES},"
            if [[ "${line}" != "${prefix}"* ]]; then
                echo "Unexpected benchmark record: ${line}" >&2
                exit 1
            fi
            remainder=${line#"${prefix}"}
            request_id=${remainder%%,*}
            metrics=${remainder#*,}
            printf '%s,%s,%s,%s,%s,%s,%s\n' \
                "${record_operation}" "${batch_size}" "${actual_input_lbas}" \
                "${SLM_READ_CHUNK_BYTES}" "${request_id}" "${iteration}" \
                "${metrics}" >> "${OUTPUT_CSV}"
            sleep "${SETTLE_SECONDS}"
        done
    done
done

echo "Remote pipeline benchmark CSV: ${OUTPUT_CSV}"
