#! /usr/bin/env bash

script_dir=$(readlink -f "$(dirname "$0")")
spdk_dir=$(readlink -f "$script_dir/..")
conf_file=conf_debug.json
runtime_lib_dir="$spdk_dir/build/arm-runtime-lib"
export HLSACC_OPERATOR_CONFIG="${HLSACC_OPERATOR_CONFIG:-$spdk_dir/config.json}"

if [ -d "$runtime_lib_dir" ]; then
    export LD_LIBRARY_PATH="$runtime_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

cd "$script_dir"

while getopts :bdmnt flag
do
    echo "$flag"
    case "$flag" in
    "b")
        export HLSACC_OPERATOR_CONFIG="$spdk_dir/config_legacy_blowfish.json"
        ;;
    "d")
        prefix="gdb --args"
        ;;
    "m")
	prefix="taskset 0x01"
	;;
    *)
        echo "invalid flag '$flag'"
    esac
done

$prefix "$spdk_dir/build/bin/nvmf_tgt" -c "$conf_file" -m 0x0f -e nvmf_mcdma -L nvme -L nvmq -L nvmf
# $prefix ../build/bin/nvmf_tgt -c $conf_file -m 0x01 -e axi_dma # nvmf_mcdma # ,axi_dma
# $prefix ../build/bin/nvmf_tgt -c $conf_file -m 0x01
