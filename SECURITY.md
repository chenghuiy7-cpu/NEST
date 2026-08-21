# Security And Publication Rules

This repository is a research prototype. Report accidental credential or key
publication privately to the repository maintainer.

The following files must never be committed:

- SSH private keys and tokens;
- TFHE client keys, server keys, and LWE secret keys;
- hardware serial numbers tied to a specific board;
- `BOOT.bin`, device trees, bitstreams, DCP files, and HPU archives;
- raw plaintext/ciphertext dumps from experiments;
- local `.env` files and machine-specific absolute paths.

Use the templates under `config/` and generate keys locally. Run
`./scripts/verify_repository.sh` before every public push.

The FPGA random/noise generator is an internal prototype. Passing functional
decryption tests does not establish cryptographic security.
