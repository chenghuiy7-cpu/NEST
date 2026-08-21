#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=${REMOTE_TFHE_RS:-$(cd -- "${SCRIPT_DIR}/../.." && pwd)}
CONFIG=${HPU_REMOTE_CONFIG:-${REPO_ROOT}/hpu_config_remote_no_reload.toml}
SERVER_KEY=${HPU_REMOTE_SERVER_KEY:-${REPO_ROOT}/psi64_integer_compressed_server_key.bincode}
BIND_ADDR=${HPU_REMOTE_BIND:-0.0.0.0:19090}
EXPECTED_SERVER_KEY_SHA256=${HPU_REMOTE_SERVER_KEY_SHA256:-}

export HPU_BACKEND_DIR=${HPU_BACKEND_DIR:-${REPO_ROOT}/backends/tfhe-hpu-backend}
export HPU_CONFIG=${HPU_CONFIG:-v80}
export RUST_LOG=${RUST_LOG:-info}

: "${XILINX_VIVADO:?set XILINX_VIVADO to the authorized Vivado installation}"
: "${V80_PCIE_DEV:?set V80_PCIE_DEV to the local V80 PCIe bus ID}"
: "${V80_SERIAL_NUMBER:?set V80_SERIAL_NUMBER for the local V80 board}"
: "${AMI_PATH:?set AMI_PATH to the local AMI driver directory}"
: "${EXPECTED_SERVER_KEY_SHA256:?set HPU_REMOTE_SERVER_KEY_SHA256 after verifying the key manifest}"

export XILINX_VIVADO V80_PCIE_DEV V80_SERIAL_NUMBER AMI_PATH

cd "${REPO_ROOT}"

grep -q 'force_reload="never"' "${CONFIG}" || {
    echo "refusing unsafe config without force_reload=\"never\": ${CONFIG}" >&2
    exit 1
}

actual_key_sha256=$(sha256sum "${SERVER_KEY}" | awk '{print $1}')
if [[ "${actual_key_sha256}" != "${EXPECTED_SERVER_KEY_SHA256}" ]]; then
    echo "unexpected compressed server key SHA-256: ${actual_key_sha256}" >&2
    exit 1
fi

grep -q 'force == "never"' backends/tfhe-hpu-backend/src/ffi/v80/mod.rs || {
    echo "backend does not implement the no-reload policy" >&2
    exit 1
}

grep -q 'require_reload_disabled' tfhe/examples/hpu/lwe_remote_server.rs || {
    echo "server does not enforce the no-reload policy" >&2
    exit 1
}

grep -q 'OP_ADD_SCALAR_U8_HPU_NATIVE_ROUNDTRIP' \
    tfhe/examples/hpu/lwe_remote_server.rs || {
    echo "server source does not implement HPU-native response operation op=3" >&2
    exit 1
}

printf '%s\n' \
    "HPU_CONFIG=${HPU_CONFIG}" \
    "V80_PCIE_DEV=${V80_PCIE_DEV}" \
    "V80_SERIAL_NUMBER=${V80_SERIAL_NUMBER}" \
    "HPU_REMOTE_CONFIG=${CONFIG}" \
    "HPU_REMOTE_SERVER_KEY=${SERVER_KEY}" \
    "HPU_REMOTE_SERVER_KEY_SHA256=${EXPECTED_SERVER_KEY_SHA256}" \
    "HPU_REMOTE_BIND=${BIND_ADDR}"

cargo build --release --features hpu-v80 --example hpu_lwe_remote_server

grep -aFq \
    'fresh reload disabled by force_reload=never' \
    target/release/examples/hpu_lwe_remote_server || {
        echo "built server does not contain the no-reload backend guard" >&2
    exit 1
}

grep -aFq \
    'adds-hpu-native-roundtrip' \
    target/release/examples/hpu_lwe_remote_server || {
        echo "built server does not contain HPU-native round-trip op=3" >&2
        exit 1
    }

exec sudo -E env RUST_BACKTRACE=1 RUST_LOG="${RUST_LOG}" \
    "${REPO_ROOT}/target/release/examples/hpu_lwe_remote_server" \
    --bind "${BIND_ADDR}" \
    --config "${CONFIG}" \
    --server-key "${SERVER_KEY}"
