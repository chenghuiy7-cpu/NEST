# lwe_encrypt SSD Data Generator

This standalone program reads a fixed 4096-byte `u8` plaintext file, writes
the 4KB page to a selected SSD namespace/LBA, and verifies the complete page
with an NVMe readback.

The default test vector is:

```text
testdata/plaintext_u8_4k.bin
```

Its SHA-256 is recorded in `testdata/plaintext_u8_4k_manifest.txt`.

```bash
cd /mnt/suda/host/applications/vscode-lwe-encrypt-data-gen
make
./vscode-lwe-encrypt-data-gen \
  --ssd-nsid 1 \
  --ssd-lba "$UNUSED_SSD_LBA"
```

Use `--input FILE` to select another exactly 4096-byte plaintext file.

The selected 4KB block is overwritten. The program prints `first_u8`, which
is the value consumed by the current one-request `lwe_encrypt` Host program.

```bash
cd ../vscode-lwe-encrypt-offload
./vscode-lwe-encrypt-offload \
  --ssd-nsid 1 \
  --ssd-lba "$UNUSED_SSD_LBA" \
  --expect "$FIRST_U8"
```
