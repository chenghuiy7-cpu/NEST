#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../.." && pwd)
OUTPUT=${1:-${REPO_ROOT}/target/lwe_remote_hpu_server_129.tar.gz}
SERVER_KEY_SOURCE=${SERVER_KEY_SOURCE:-}
EXPECTED_SERVER_KEY_SHA256=${HPU_REMOTE_SERVER_KEY_SHA256:-}

: "${SERVER_KEY_SOURCE:?set SERVER_KEY_SOURCE to the locally generated compressed server key}"
: "${EXPECTED_SERVER_KEY_SHA256:?set HPU_REMOTE_SERVER_KEY_SHA256 from the private key manifest}"

FILES=(
    tfhe/Cargo.toml
    tfhe/examples/hpu/export_lwe_encrypt_key.rs
    tfhe/examples/hpu/export_lwe_server_key.rs
    tfhe/examples/hpu/lwe_encrypt_mockup_test.rs
    tfhe/examples/hpu/lwe_remote_client.rs
    tfhe/examples/hpu/lwe_remote_server.rs
    tfhe/examples/hpu/lwe_remote/bridge.rs
    tfhe/examples/hpu/lwe_remote/dump.rs
    tfhe/examples/hpu/lwe_remote/protocol.rs
    tfhe/examples/hpu/pipeline_bench.rs
    tfhe/examples/hpu/pipeline_dataset.rs
    tfhe/examples/hpu/verify_lwe_encrypt_output.rs
    tfhe/src/integer/hpu/ciphertext/mod.rs
    backends/tfhe-hpu-backend/src/ffi/v80/ami.rs
    backends/tfhe-hpu-backend/src/ffi/v80/mod.rs
    scripts/lwe_remote_hpu/start_server_129.sh
)

for relative_path in "${FILES[@]}"; do
    if [[ ! -f "${REPO_ROOT}/${relative_path}" ]]; then
        echo "missing deployment source: ${REPO_ROOT}/${relative_path}" >&2
        exit 1
    fi
done

if [[ ! -f "${SERVER_KEY_SOURCE}" ]]; then
    echo "missing compressed server key: ${SERVER_KEY_SOURCE}" >&2
    exit 1
fi

actual_key_sha256=$(sha256sum "${SERVER_KEY_SOURCE}" | awk '{print $1}')
if [[ "${actual_key_sha256}" != "${EXPECTED_SERVER_KEY_SHA256}" ]]; then
    echo "unexpected compressed server key SHA-256: ${actual_key_sha256}" >&2
    exit 1
fi

staging=$(mktemp -d)
trap 'rm -rf "${staging}"' EXIT

for relative_path in "${FILES[@]}"; do
    install -D -m 0644 \
        "${REPO_ROOT}/${relative_path}" \
        "${staging}/${relative_path}"
done
chmod 0755 "${staging}/scripts/lwe_remote_hpu/start_server_129.sh"

install -m 0644 \
    "${SERVER_KEY_SOURCE}" \
    "${staging}/psi64_integer_compressed_server_key.bincode"

cp \
    "${REPO_ROOT}/backends/tfhe-hpu-backend/config_store/v80/hpu_config.toml" \
    "${staging}/hpu_config_remote_no_reload.toml"
sed -i 's/force_reload="false"/force_reload="never"/' \
    "${staging}/hpu_config_remote_no_reload.toml"

if ! grep -q 'force_reload="never"' "${staging}/hpu_config_remote_no_reload.toml"; then
    echo "failed to create no-reload V80 configuration" >&2
    exit 1
fi

(
    cd "${staging}"
    find . -type f ! -name SHA256SUMS -print0 \
        | sort -z \
        | xargs -0 sha256sum > SHA256SUMS
)

mkdir -p "$(dirname -- "${OUTPUT}")"
tar -C "${staging}" -czf "${OUTPUT}" .

echo "server_bundle=${OUTPUT}"
echo "server_bundle_sha256=$(sha256sum "${OUTPUT}" | awk '{print $1}')"
echo "server_key_sha256=${actual_key_sha256}"
echo "source_of_truth=${REPO_ROOT}"
