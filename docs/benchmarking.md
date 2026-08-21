# Benchmark Methodology

## Correctness Before Performance

Every benchmark run must first verify:

- plaintext source bytes and expected prefix;
- FPGA encryption result size;
- remote HPU operation result;
- FPGA decryption output;
- destination SSD readback.

## Stage Definitions

Measure and report at least these stages separately:

1. SSD to input SLM;
2. FPGA program setup;
3. FPGA encryption execute;
4. encryption output SLM to host;
5. TCP request send;
6. remote request receive/decode;
7. HPU prepare/enqueue/wait/output conversion;
8. TCP response send/receive;
9. host to decryption input SLM;
10. FPGA decryption execute;
11. decrypted SLM to host;
12. plaintext write and SSD readback;
13. online end-to-end and one-shot process latency.

## Validated 128-Byte Observation

The first full-pipeline run was functionally correct but transfer-bound:

- FPGA encrypt: about 30.9 ms;
- encryption SLM to host: about 6.44 s;
- remote RPC: about 1.23 s;
- host to decryption SLM: about 60.14 s;
- FPGA decrypt: about 31.7 ms;
- online end-to-end: about 68.21 s.

This is one diagnostic run, not a publication-quality distribution. Repeat
warm-up and measured runs, report median/P95, and keep hardware/software
versions fixed. The dominant optimization target is currently host-to-SLM
write submission, followed by output-SLM readback.
