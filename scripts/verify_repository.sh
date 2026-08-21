#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

status=0

check_forbidden_files() {
    local pattern=$1
    local description=$2
    local matches

    matches=$(find . -path './.git' -prune -o -path './work' -prune -o \
        -type f -name "${pattern}" -print)
    if [[ -n "${matches}" ]]; then
        printf 'error: found %s:\n%s\n' "${description}" "${matches}" >&2
        status=1
    fi
}

check_forbidden_files '*.bincode' 'serialized key material'
check_forbidden_files '*.bin' 'binary dumps, key files, or board images'
check_forbidden_files 'BOOT.bin' 'board boot images'
check_forbidden_files '*.hpu' 'HPU archives'
check_forbidden_files '*.dcp' 'Vivado checkpoints'

large_files=$(find . -path './.git' -prune -o -path './work' -prune -o \
    -type f -size +50M -print)
if [[ -n "${large_files}" ]]; then
    printf 'error: files larger than 50 MiB:\n%s\n' "${large_files}" >&2
    status=1
fi

if rg -n --hidden --glob '!.git/**' --glob '!work/**' \
    --glob '!scripts/verify_repository.sh' \
    'BEGIN (OPENSSH|RSA|EC|DSA) PRIVATE KEY' .; then
    printf 'error: possible private key content found\n' >&2
    status=1
fi

if rg -n --hidden --glob '!.git/**' --glob '!work/**' \
    'XFL[[:alnum:]]{8,}' .; then
    printf 'error: possible hardware serial number found\n' >&2
    status=1
fi

if [[ ${status} -ne 0 ]]; then
    exit "${status}"
fi

printf 'Repository publication checks passed.\n'
