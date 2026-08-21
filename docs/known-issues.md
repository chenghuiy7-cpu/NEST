# Known Issues

## NVMQ Queue-ID Collision

With four QEMU vCPUs and `nr_io_queues=4`, the guest generated I/O queue IDs
1 through 4 while queue 0 was already used by the admin path. The underlying
QDMA channel mapping reused physical queue 0 for logical queue 4, causing
timeouts and requests stuck in uninterruptible sleep. The validated temporary
configuration is `nr_io_queues=3`.

## Small SLM Write Requests

The current decrypt path writes HPU-native ciphertext to SLM as independent
4 KiB requests. A 128-byte plaintext expands to 12 MiB and therefore requires
3,072 writes. This dominates end-to-end latency. Larger write granularity and
safe request pipelining need controlled evaluation.

## SLM Read Long Tails

Sequential 128 KiB output reads are stable but may show millisecond-to-second
long-tail requests. Queue depth greater than one has previously hung the
prototype, so concurrency changes require a clean ARM runtime and QEMU test.

## Bitstream Timing

Vivado may generate an image even when implementation timing is not clean.
Images with timing violations have previously failed to boot the ARM system.
Always inspect timing reports and keep a known-good `BOOT.bin` backup outside
the repository before board deployment.

## Prototype Randomness

The FPGA encryption noise/PRNG implementation is intended for functional
integration. It is not a bit-exact implementation of TFHE-rs randomness and
has not undergone cryptographic validation.

## Cold libnvme Build Depends On The Validated Host Image

On a generic Ubuntu host, the pinned SUDA vendored liburing headers may not
match the installed kernel headers; one observed failure was an undefined
`BLOCK_URING_CMD_DISCARD`. Build Host applications inside the documented SUDA
QEMU/toolchain image (or align liburing and kernel headers) before linking.
NEST intentionally does not publish a machine-specific prebuilt `libnvme.a`.
