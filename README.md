# NEST

NEST is a research prototype for an end-to-end, near-storage encrypted
computing path:

```text
SSD -> CSD SLM -> FPGA LWE encryption -> host memory -> TCP
    -> remote HPU computation -> TCP -> host memory
    -> CSD SLM -> FPGA LWE decryption -> SSD
```

The current prototype integrates SUDA/NVMQ, two Vitis HLS operators
(`lwe_encrypt` and `lwe_decrypt`), and the TFHE-rs HPU backend. It supports
batched `u8` plaintexts and the PSI64/V80 HPU-native ciphertext layout.

> Research status: the data path has passed 1-byte and 128-byte end-to-end
> correctness tests. The internal FPGA random/noise generator is a prototype
> and is not bit-exact TFHE-rs randomness. Do not use this repository for
> production cryptography.

## Why This Repository Is Small

SUDA and TFHE-rs are large upstream projects. NEST does not vendor their build
trees, generated artifacts, board images, cryptographic keys, or third-party
source trees. Instead, it pins known base revisions and stores only the files
modified by this project under `overlays/`.

```text
NEST/
├── config/                 # Environment templates; no machine secrets
├── docs/                   # Architecture, deployment, experiments, debugging
├── manifests/              # Pinned upstream revisions and overlay inventory
├── overlays/
│   ├── suda/               # Files overlaid onto a clean SUDA checkout
│   └── tfhe-rs/            # Files overlaid onto a clean TFHE-rs checkout
├── scripts/                # Bootstrap, validation, build/deploy helpers
└── licenses/               # Upstream license copies
```

## Quick Start

```bash
git clone https://github.com/chenghuiy7-cpu/NEST.git
cd NEST
./scripts/bootstrap.sh
```

This creates pinned working copies under `work/` and applies the NEST
overlays. Hardware setup, key generation, bitstream generation, ARM runtime,
QEMU, and remote HPU service steps are documented in
[`docs/reproduction.md`](docs/reproduction.md).

## Reproduced Result

The validated 128-byte flow performs the following operation:

1. read 128 plaintext bytes from the CSD SSD;
2. encrypt them on the CSD FPGA into HPU-native PSI64/V80 ciphertexts;
3. send the ciphertexts to a remote V80 HPU service;
4. execute homomorphic scalar addition (`+1`) on the HPU;
5. return the untouched HPU-native result;
6. decrypt on the CSD FPGA and write the 128 clear bytes back to SSD.

The result prefix changed from `ab a4 47 ...` to `ac a5 48 ...`, and both the
FPGA-side check and SSD readback check passed.

## Documentation

- [中文项目导读](docs/README_zh.md)
- [System architecture](docs/architecture.md)
- [Reproduction guide](docs/reproduction.md)
- [Benchmark methodology](docs/benchmarking.md)
- [Known issues](docs/known-issues.md)
- [Repository validation](docs/validation.md)
- [Development history](docs/history/README.md)
- [Curated benchmark results](results/README.md)

## Security And Artifacts

Never commit client/server keys, LWE secret keys, SSH keys, board serials,
`BOOT.bin`, device trees, HPU archives, Vivado checkpoints, or raw ciphertext
dumps. See [`SECURITY.md`](SECURITY.md).

## License

NEST-owned orchestration code and documentation are released under the MIT
License. Overlaid SUDA and TFHE-rs files retain their upstream notices and
licenses. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
