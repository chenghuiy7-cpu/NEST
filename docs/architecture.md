# System Architecture

## Roles

### CSD ARM runtime

The ARM-side SPDK/NVMF runtime manages SLM objects, schedules HLS operators,
and moves payloads between SSD, FPGA streams, and host-visible queues.

### Host/QEMU applications

Host applications submit SUDA/NVMQ commands, create SLM ranges, activate FPGA
programs, transfer HPU-native ciphertexts, and collect benchmark telemetry.
The full-pipeline application deliberately keeps intermediate ciphertexts in
memory unless a diagnostic dump is explicitly requested.

### CSD FPGA operators

- `lwe_encrypt` reads packed `u8` plaintext and emits PSI64/V80 HPU-native
  radix ciphertext slots.
- `lwe_decrypt` consumes the returned HPU-native slots and emits packed `u8`
  plaintext.

Both operators use the big LWE secret-key coefficients staged in operator
context memory by the ARM runtime.

### Remote HPU service

The TCP service validates the request geometry, imports HPU-native ciphertext
words without a host-side logical-layout conversion, executes the requested
HPU operation, exports the HPU-native result, and returns stage telemetry.

## Validated Data Geometry

For PSI64/V80, one clear `u8` is represented by four radix blocks. Each block
contains two HPU memory cuts, and each cut reserves 1,536 `u64` words:

```text
4 radix blocks * 2 cuts * 1536 words * 8 bytes = 98,304 bytes/u8
```

Therefore a 128-byte batch produces a 12,582,912-byte HPU-native payload in
each network direction.

## End-To-End Ownership

| Stage | Owner | Main interface |
|---|---|---|
| SSD to input SLM | CSD ARM runtime | SUDA/NVMQ SLM copy |
| LWE encryption | CSD FPGA | AXI Stream HLS operator |
| Output SLM to host | Host + NVMQ | QDMA-backed SLM read |
| Remote request/response | Host and HPU server | TCP binary protocol |
| Homomorphic operation | V80 HPU | TFHE-rs HPU backend |
| Host to decrypt SLM | Host + NVMQ | QDMA-backed SLM write |
| LWE decryption | CSD FPGA | AXI Stream HLS operator |
| Plaintext to SSD | CSD ARM runtime | SUDA/NVMQ SLM copy |

Detailed implementation notes are preserved under `docs/history/` after the
source overlays are assembled.
