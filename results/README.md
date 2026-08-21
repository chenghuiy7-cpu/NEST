# Curated Results

This directory stores compact, publication-safe summaries rather than raw
plaintext/ciphertext dumps or multi-gigabyte logs.

- `encrypt-equivalent-layout.csv`: median operator-level CPU versus FPGA
  measurements. Both sides produce the same PSI64/V80 HPU-native layout.
- `full-pipeline-128b.csv`: one diagnostic 128-byte end-to-end run. This row
  establishes stage ownership and the dominant bottleneck; it is not a
  confidence interval.

For publication, rerun each configuration with warm-up, at least 30 measured
samples, fixed hardware/software revisions, and report median/P95.
