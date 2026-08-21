# Reproduction Guide

This document is intentionally explicit about machine roles. Replace every
placeholder with values from a local, untracked `config/nest.env` file.

## 1. Requirements

- one x86 host connected to a Fidus CSD and able to run the SUDA QEMU guest;
- one remote server with an initialized V80 HPU;
- Vivado/Vitis HLS 2020.2 for the CSD design;
- the V80-compatible Vivado/AMI environment required by the HPU backend;
- permission to use the required board images and HPU archive;
- locally generated PSI64 client/server/secret key material.

## 2. Reconstruct Source Trees

```bash
cp config/nest.env.example config/nest.env
./scripts/bootstrap.sh
```

The script checks out the revisions in `manifests/base-revisions.env` under
`work/` and copies NEST overlays into those trees. It never modifies an
existing checkout.

## 3. Generate Keys

Use the TFHE-rs examples in the reconstructed tree to generate matching PSI64
keys. Keep all resulting `.bincode` and `.bin` key files outside this Git
repository. Record SHA-256 manifests privately so that the CSD and HPU server
can be checked for key consistency.

## 4. Build The CSD Hardware

Build the two HLS operators first, update the operator-pool RTL, then run the
full block-design build with Vivado 2020.2. Verify implementation timing before
putting the image on a board. A generated `BOOT.bin` is a deployment artifact,
not source code, and must not be committed.

## 5. Start The ARM Runtime And QEMU

Cross-compile the ARM-side SPDK/NVMF runtime, deploy `nvmf_tgt`, initialize AXI
DMA/VFIO, and start `run_nvmq.sh`. On the QEMU guest, initialize NVMQ with three
I/O queues. This queue count is part of the validated configuration; four I/O
queues caused a physical QDMA queue-ID collision with the admin queue.

## 6. Start The Remote HPU Service

Deploy the TFHE-rs overlay to the HPU server, export the machine-local V80/AMI
variables, and run:

```bash
./scripts/lwe_remote_hpu/start_server_129.sh
```

The final `listen_addr=...` line means the server is ready and waiting for a
client; remaining attached to the terminal is expected.

## 7. Run The Full Pipeline

From the SUDA QEMU guest, run the reconstructed
`vscode-lwe-full-pipeline` application with non-overlapping source and
destination LBAs. Start with one byte, then validate 128 bytes. The run is
successful only when all of the following are present:

```text
lwe full SSD-to-remote-HPU-to-SSD pipeline passed
destination_ssd_readback_checked=yes
fpga_decrypt_checked=yes
remote_hpu_ciphertext_compute=passed
```

Keep exact commands and SHA-256 manifests in experiment notes; do not commit
secret keys or raw ciphertext files.
