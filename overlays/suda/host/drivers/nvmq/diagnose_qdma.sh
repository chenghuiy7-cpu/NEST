#!/usr/bin/env bash

set -u

pci_device=${1:-0000:01:00.0}
device_path="/sys/bus/pci/devices/$pci_device"

if [ ! -d "$device_path" ]; then
    echo "PCI device $pci_device was not found." >&2
    exit 1
fi

echo "== PCI device =="
lspci -nnvv -s "${pci_device#0000:}"

echo
echo "== Bound driver =="
if [ -L "$device_path/driver" ]; then
    bound_driver=$(basename "$(readlink -f "$device_path/driver")")
    echo "$bound_driver"
else
    bound_driver=unbound
    echo "unbound"
fi

echo
echo "== Runtime power =="
for attribute in control runtime_status; do
    if [ -r "$device_path/power/$attribute" ]; then
        printf "%s: " "$attribute"
        cat "$device_path/power/$attribute"
    fi
done

if [ "$bound_driver" = "vfio-pci" ]; then
    echo
    echo "WARNING: vfio-pci may place an idle device in D3."
    echo "BAR reads returning 0xffffffff in D3 are not a valid hardware diagnosis."
fi

echo
echo "== BAR resources =="
nl -v 0 -ba "$device_path/resource"

echo
echo "== First DWORD of each memory BAR =="
python3 - "$device_path" <<'PY'
import mmap
import pathlib
import struct
import sys

device_path = pathlib.Path(sys.argv[1])

for index in range(6):
    resource = device_path / f"resource{index}"
    if not resource.exists() or resource.stat().st_size < 4:
        continue

    try:
        with resource.open("r+b", buffering=0) as handle:
            mapping = mmap.mmap(handle.fileno(), mmap.PAGESIZE)
            value = struct.unpack_from("<I", mapping, 0)[0]
            mapping.close()
        block_id = value >> 16
        if block_id == 0x1FD3:
            suffix = " (valid QDMA config signature)"
        elif block_id == 0x1FC0:
            suffix = " (XDMA block signature, not a QDMA config BAR)"
        else:
            suffix = ""
        print(f"BAR{index}: 0x{value:08x}{suffix}")
    except (OSError, ValueError) as error:
        print(f"BAR{index}: unreadable ({error})")
PY

echo
echo "== DMA signatures at standard register offsets =="
python3 - "$device_path" <<'PY'
import mmap
import pathlib
import struct
import sys

device_path = pathlib.Path(sys.argv[1])
signatures = {
    0x1FD3: "QDMA config",
    0x1FC0: "XDMA H2C",
    0x1FC1: "XDMA C2H",
    0x1FC2: "XDMA interrupt",
    0x1FC3: "XDMA config",
}
probe_offsets = [
    0x0000,
    0x0100,
    0x0200,
    0x0300,
    0x1000,
    0x1100,
    0x1200,
    0x1300,
    0x2000,
    0x3000,
]

for index in range(6):
    resource = device_path / f"resource{index}"
    if not resource.exists() or resource.stat().st_size < 4:
        continue

    resource_size = resource.stat().st_size
    scan_size = min(resource_size, max(probe_offsets) + mmap.PAGESIZE)
    matches = []
    try:
        with resource.open("r+b", buffering=0) as handle:
            mapping = mmap.mmap(handle.fileno(), scan_size)
            for offset in probe_offsets:
                if offset + 4 > scan_size:
                    continue
                value = struct.unpack_from("<I", mapping, offset)[0]
                name = signatures.get(value >> 16)
                if name is not None:
                    matches.append((offset, value, name))
            mapping.close()
    except (OSError, ValueError) as error:
        print(f"BAR{index}: unreadable ({error})")
        continue

    if not matches:
        print(f"BAR{index}: no known DMA signatures")
        continue

    print(f"BAR{index}:")
    for offset, value, name in matches:
        print(f"  offset 0x{offset:04x}: 0x{value:08x} ({name})")
PY

echo
echo "Expected: BAR0's first DWORD has upper 16 bits equal to 0x1fd3."
echo "A 0x1fc0 prefix identifies an XDMA block and is not valid for this QDMA driver."
echo
echo "== Recent QDMA messages =="
dmesg | grep -i qdma | tail -40
