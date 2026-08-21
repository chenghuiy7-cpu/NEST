# Repository Validation

The publication scaffold was validated against clean local checkouts at the
revisions in `manifests/base-revisions.env`.

## Passed

- repository security/publication scan;
- shell syntax checks for bootstrap and remote deployment scripts;
- Rust formatting for all overlaid NEST sources;
- offline `cargo check` for seven NEST HPU examples;
- remote server unit tests: 7 passed, 0 failed;
- C++14 compilation of all five SUDA LWE application translation units;
- C++ remote protocol test build and link.

## Environment-Limited Checks

The local protocol executable could not open a loopback socket inside the
restricted validation sandbox. The same protocol test has passed in the
project's normal host environment.

A cold build of the pinned SUDA `libnvme` reached an existing compatibility
error between its vendored liburing header and the validation host's kernel
headers (`BLOCK_URING_CMD_DISCARD` missing). The NEST application sources
compile cleanly, and they link in the validated SUDA/QEMU environment where
the matching `libnvme.a` is built. This prerequisite is documented as a known
environment issue rather than hidden by committing a binary library.

Hardware validation remains represented by the recorded 1B/128B successful
board runs. CI cannot replace Vivado timing closure, ARM boot, QDMA/NVMQ, or
V80 HPU tests.
