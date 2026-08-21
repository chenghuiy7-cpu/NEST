# lwe_encrypt FPGA Host Application

This application copies enough 4KB SSD blocks to input SLM with
`nvme_slm_copy`, then encrypts an exact, user-selected number of consecutive
`u8` bytes with the SUDA `lwe_encrypt` FPGA operator. Each byte produces four
2-bit radix Big-LWE ciphertexts.

The default FPGA stream layout is `hpu-native`: every Big-LWE is emitted as a
12KB PC0 slot followed by a 12KB PC1 slot. The mask is already in the psi64
11-bit bit-reversed and 16-coefficient interleaved order expected by the HPU;
the body is PC0 word 1024. Use `--output-layout cpu` only with an older CPU-LWE
bitstream. The `LWEHLS01` debug dump remains logical CPU-LWE for compatibility
with existing offline tools.

## Board Software Stack

The ARM-side software stack must use a `config.json` that contains:

```json
{
  "operator_type_id": 2,
  "operator_type_name": "lwe_encrypt",
  "operator_inport_num": 1,
  "operator_outport_num": 1,
  "esti_executed_times": 200,
  "worse_executed_times": 1000,
  "bram_size": 2048,
  "slot_id": 2
}
```

After copying the updated software stack or `config.json` to the ARM system, restart the `mcdma/run_nvmq.sh` process.

## Build

From the Host VM:

```bash
cd /mnt/suda/host/applications/vscode-lwe-encrypt-offload
make
```

## Run

To encrypt the first byte at an SSD LBA:

```bash
./vscode-lwe-encrypt-offload --ssd-nsid 1 --ssd-lba "$SSD_LBA"
```

To encrypt 128 consecutive bytes, one 4KB input LBA is sufficient:

```bash
./vscode-lwe-encrypt-offload --ssd-nsid 1 --ssd-lba "$SSD_LBA" \
  --plaintext-bytes 128
```

Output SLM data is read in 128KB requests by default. The request size is
configurable in 4KB units; use the legacy 4KB behavior when isolating a runtime
or transport problem:

```bash
./vscode-lwe-encrypt-offload --ssd-nsid 1 --ssd-lba "$SSD_LBA" \
  --plaintext-bytes 128 --slm-read-chunk-bytes 4096
```

To encrypt 256 bytes while explicitly selecting the copied SSD range:

```bash
./vscode-lwe-encrypt-offload --ssd-nsid 1 --ssd-lba "$SSD_LBA" \
  --plaintext-bytes 256 --input-lbas 1
```

`--input-lbas` defaults to `ceil(plaintext_bytes / 4096)`. A larger value is
allowed, but only the first `--plaintext-bytes` bytes are encrypted. The old
`--encrypt-count` option remains as a deprecated alias whose value now also
means plaintext bytes.

To check the expected first byte during decryption:

```bash
./vscode-lwe-encrypt-offload --ssd-nsid 1 --ssd-lba "$SSD_LBA" \
  --expect 42 --zero-noise
```

This application never writes the SSD. Use the separate
`vscode-lwe-encrypt-data-gen` application to prepare one random 4KB plaintext
block. Batches larger than 4KB require consecutive source LBAs containing the
remaining plaintext bytes.

The output file defaults to:

```text
lwe_encrypt_fpga_ciphertexts.bin
```

It uses the existing `LWEHLS01` format and records the exact plaintext byte
count. It can be checked by the tfhe-rs HPU mockup import test.

The internal mask PRNG and noise sampler in the current HLS operator are prototypes and are not bit-exact tfhe-rs cryptographic randomness.

## Performance Benchmark

`--benchmark` prints stage timings and one `BENCH_FPGA_CSV` record. The record
includes the selected output layout, physical bytes per u8, and
`transport_ready_ms = SSD->SLM + FPGA execute + output SLM->Host`. Combine it
with `--skip-dump` to exclude ciphertext file I/O while keeping SLM readback and
host-side correctness verification:

```bash
./vscode-lwe-encrypt-offload --ssd-nsid 1 --ssd-lba 65536 \
  --input-lbas 1 --plaintext-bytes 128 --benchmark --skip-dump
```

Run the repeatable batch benchmark from the Host VM with:

```bash
OUTPUT_LAYOUT=hpu-native WARMUP=3 ITERATIONS=10 INPUT_LBAS=1 \
  ./run_fpga_bench.sh
```

The output `fpga_benchmark.csv` separates FPGA execution, output-ready transfer,
host verification, one-shot setup, and full process time. New HPU-native CSVs
must not be appended to legacy CPU-LWE benchmark files. To compare several SLM
request sizes without mixing their samples:

```bash
BATCH_SIZES=128 WARMUP=1 ITERATIONS=3 INPUT_LBAS=256 \
  ./run_slm_read_sweep.sh
```

The sweep tries 128KB and 32KB reads by default. The legacy 8KB and 4KB paths
must be requested explicitly because thousands of synchronous 4KB reads can
expose an NVMQ/QDMA completion stall. For a single 4KB baseline sample, use:

```bash
CHUNK_SIZES=4096 BATCH_SIZES=128 WARMUP=0 ITERATIONS=1 \
  OUTPUT_CSV=slm_read_4k_baseline.csv ./run_slm_read_sweep.sh
```

Completed samples remain in `slm_read_sweep.csv` if a later size fails. See
`docs/architecture/LWE加密算子性能测试方法.md` for the matching tfhe-rs CPU
baseline and speedup definitions.

### Per-request SLM latency

Use `--slm-read-trace PATH` to append one CSV row for every synchronous SLM
read. Samples are kept in memory during the read loop and written after the
loop, so file output is not included in individual request latency. With the
benchmark script, only measured rounds are traced; warm-up rounds are excluded:

```bash
BATCH_SIZES=128 WARMUP=3 ITERATIONS=20 INPUT_LBAS=256 \
SLM_READ_CHUNK_BYTES=131072 SETTLE_SECONDS=1 \
SLM_READ_TRACE=slm_read_requests_128k_20runs.csv \
OUTPUT_CSV=slm_read_trace_runs_128k_20runs.csv \
  ./run_fpga_bench.sh
```

Each run has a random `run_nonce`; 20 complete 128KB runs should produce 1300
request rows. Summarize per-run latency, slow request positions, and the
globally slowest requests with:

```bash
./summarize_slm_request_trace.py slm_read_requests_128k_20runs.csv \
  | tee slm_read_requests_128k_20runs_summary.md
```

Queue-depth traces use a newer CSV schema. Use a new trace filename instead of
appending QD measurements to an older serial trace file.

### Concurrent SLM reads

`--slm-read-queue-depth` selects one, two, or four Host worker threads. Workers
share the NVMe file descriptor but submit non-overlapping SLM offsets into
non-overlapping Host buffer regions. Queue depth one is the default and exactly
preserves the validated serial path.

Test queue depth two once before running a sweep:

```bash
./vscode-lwe-encrypt-offload \
  --ssd-nsid 1 --ssd-lba 65536 --input-lbas 256 \
  --plaintext-bytes 128 --slm-read-chunk-bytes 131072 \
  --slm-read-queue-depth 2 \
  --slm-read-trace slm_read_qd2_smoke.csv \
  --key /mnt/suda/device/operators/hls/lwe_encrypt/testdata/psi64_big_lwe_secret_key.bin \
  --benchmark --skip-dump
```

After the smoke test decrypts all 128 bytes successfully, compare QD1 and QD2:

```bash
QUEUE_DEPTHS="1 2" BATCH_SIZES=128 WARMUP=1 ITERATIONS=5 \
INPUT_LBAS=256 SLM_READ_CHUNK_BYTES=131072 \
SLM_READ_TRACE=slm_read_qd12_requests.csv \
OUTPUT_CSV=slm_read_qd12_runs.csv \
  ./run_slm_queue_depth_sweep.sh
```

Only test QD4 explicitly after QD2 remains stable. A blocked ioctl can leave the
NVMQ queue unusable until the QEMU/NVMQ connection is restarted.
