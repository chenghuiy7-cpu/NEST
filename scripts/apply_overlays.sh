#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work_dir=${1:-${NEST_WORK_DIR:-"${repo_root}/work"}}
source "${repo_root}/manifests/base-revisions.env"

apply_overlay() {
    local name=$1
    local overlay=$2
    local destination=$3
    local expected_revision=$4

    if [[ ! -d "${destination}/.git" ]]; then
        printf 'error: %s is not a Git checkout: %s\n' "${name}" "${destination}" >&2
        return 1
    fi

    local actual_revision
    actual_revision=$(git -C "${destination}" rev-parse HEAD)
    if [[ "${actual_revision}" != "${expected_revision}" ]]; then
        printf 'error: %s revision mismatch: expected %s, found %s\n' \
            "${name}" "${expected_revision}" "${actual_revision}" >&2
        return 1
    fi

    if [[ -n "$(git -C "${destination}" status --porcelain --untracked-files=all)" ]]; then
        printf 'error: refusing to overwrite a non-clean %s checkout: %s\n' \
            "${name}" "${destination}" >&2
        return 1
    fi

    cp -a "${overlay}/." "${destination}/"
    printf 'Applied %s overlay to %s\n' "${name}" "${destination}"
}

apply_overlay SUDA "${repo_root}/overlays/suda" "${work_dir}/suda" "${SUDA_REVISION}"
apply_overlay TFHE-rs "${repo_root}/overlays/tfhe-rs" "${work_dir}/tfhe-rs" "${TFHE_RS_REVISION}"
