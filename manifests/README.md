# Source Manifests

`base-revisions.env` is the source of truth for the upstream repositories and
commits onto which NEST overlays are applied.

The overlay mechanism intentionally produces a dirty upstream checkout. Use
`git -C work/suda diff --stat` and `git -C work/tfhe-rs diff --stat` to inspect
the project delta. Never point `apply_overlays.sh` at an active development
tree: it requires both the exact base revision and a clean working tree.

Before publishing a release, record:

- the NEST commit SHA;
- the two pinned upstream SHAs;
- SHA-256 values for privately deployed board images and keys;
- toolchain versions and hardware identifiers in the experiment notebook,
  not in this public repository.
