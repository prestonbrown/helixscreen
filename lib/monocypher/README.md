# Monocypher 4.0.2 (vendored)

Unmodified `src/monocypher.{c,h}` and `src/optional/monocypher-ed25519.{c,h}` from the
Monocypher 4.0.2 release tarball (https://monocypher.org/download/monocypher-4.0.2.tar.gz,
sha256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`), flattened into one
directory. Dual-licensed CC0 / 2-clause BSD (`LICENCE.md`).

HelixScreen uses BLAKE2b and Ed25519 (SHA-512) from it to check minisign signatures on release
manifests: `src/system/release_signature.cpp`, `docs/devel/RELEASE_SIGNING.md`.
