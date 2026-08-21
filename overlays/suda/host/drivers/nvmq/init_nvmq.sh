#!/usr/bin/env bash

set -e

pci_device=0000:01:00.0
qdma_module=/mnt/suda/host/drivers/qdma/driver/src/qdma-pf.ko
qmax_file="/sys/bus/pci/devices/$pci_device/qdma/qmax"

insmod "$qdma_module"

if [ ! -e "$qmax_file" ]; then
    echo "QDMA probe failed: $qmax_file was not created." >&2
    echo "Run ./diagnose_qdma.sh and inspect dmesg before loading nvmq." >&2
    exit 1
fi

echo 128 > "$qmax_file"
insmod nvmq.ko # dyndbg==pf
