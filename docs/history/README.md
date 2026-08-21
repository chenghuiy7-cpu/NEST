# Development History

These documents preserve the engineering trail that led to the validated
pipeline. They are useful for understanding failures, design decisions, and
benchmark evolution, but they are not the primary setup guide. Commands and
paths have been sanitized for publication, and older documents may describe
layouts or measurements superseded by the current HPU-native flow.

- [`lwe-encrypt-development.md`](lwe-encrypt-development.md): encryption
  operator design, formats, and early integration debugging.
- [`lwe-decrypt-development.md`](lwe-decrypt-development.md): HPU-native
  decryption operator and Host-to-SLM issues.
- [`hpu-native-layout.md`](hpu-native-layout.md): direct FPGA generation of
  the PSI64/V80 physical slot layout.
- [`runtime-finish-completion-fix.md`](runtime-finish-completion-fix.md):
  AXI stream completion accounting fix.
- [`remote-pipeline-benchmark.md`](remote-pipeline-benchmark.md): remote RPC
  stage instrumentation and echo ablation.
- [`hpu-native-benchmark-results.md`](hpu-native-benchmark-results.md):
  CPU/FPGA operator-level comparison at equivalent output layout.
- [`end-to-end-modules.md`](end-to-end-modules.md): module ownership across
  CSD, Host, and remote HPU.
- [`full-pipeline-validation.md`](full-pipeline-validation.md): first complete
  FPGA-encrypt/HPU-compute/FPGA-decrypt/SSD validation.
