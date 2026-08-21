#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "${repo_root}/manifests/base-revisions.env"

work_dir=${NEST_WORK_DIR:-"${repo_root}/work"}
suda_dir="${work_dir}/suda"
tfhe_rs_dir="${work_dir}/tfhe-rs"

clone_at_revision() {
    local repository=$1
    local revision=$2
    local destination=$3

    if [[ -e "${destination}" ]]; then
        printf 'error: destination already exists: %s\n' "${destination}" >&2
        printf 'Use a new NEST_WORK_DIR or inspect the existing tree manually.\n' >&2
        return 1
    fi

    GIT_LFS_SKIP_SMUDGE=1 git clone --filter=blob:none "${repository}" "${destination}"
    git -C "${destination}" checkout --detach "${revision}"
}

mkdir -p "${work_dir}"

clone_at_revision "${SUDA_REPOSITORY}" "${SUDA_REVISION}" "${suda_dir}"
clone_at_revision "${TFHE_RS_REPOSITORY}" "${TFHE_RS_REVISION}" "${tfhe_rs_dir}"

"${repo_root}/scripts/apply_overlays.sh" "${work_dir}"

printf '\nNEST source trees are ready:\n'
printf '  SUDA:    %s\n' "${suda_dir}"
printf '  TFHE-rs: %s\n' "${tfhe_rs_dir}"
printf '\nContinue with docs/reproduction.md.\n'
