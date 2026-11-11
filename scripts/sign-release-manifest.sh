#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# sign-release-manifest.sh --mode publish|dryrun [--pubkey FILE] MANIFEST...
#
# Signs each channel manifest with minisign (prehashed) into MANIFEST.minisig and checks the
# signature before anything is uploaded. The easy installer refuses a release list whose
# signature does not verify against the release key compiled into it.
#
#   publish  Signs with the release key from HELIX_RELEASE_MINISIGN_KEY (the text of the
#            minisign .key file; its password, if it has one, in
#            HELIX_RELEASE_MINISIGN_PASSWORD) and verifies against FILE, the public key the
#            clients compile in. A missing secret, a placeholder FILE or a secret that is not
#            FILE's pair fails: a release never publishes an unsigned manifest.
#   dryrun   Never reads the release key. Signs with a key generated for this run and verifies
#            against it, so the whole signing path runs without the secret, and nothing a dry
#            run leaves in R2 verifies against the key clients trust.
#
# Setup of the key and the secrets: docs/devel/RELEASE_PROCESS.md § "Signed channel manifests".

set -euo pipefail

usage() { echo "Usage: $0 --mode publish|dryrun [--pubkey FILE] MANIFEST..." >&2; exit 2; }

MODE=""
PUBKEY="$(dirname "$0")/../.github/release-minisign.pub"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="${2:-}"; shift 2 ;;
        --pubkey) PUBKEY="${2:-}"; shift 2 ;;
        --) shift; break ;;
        -*) usage ;;
        *) break ;;
    esac
done
[[ "$MODE" == publish || "$MODE" == dryrun ]] || usage
[[ $# -ge 1 ]] || usage
for m in "$@"; do [[ -f "$m" ]] || { echo "::error::no manifest at $m" >&2; exit 2; }; done

# The key line of a minisign .pub file: RW plus 54 base64 characters.
is_pubkey_file() {
    [[ -f "$1" ]] && sed -n 2p "$1" | grep -Eq '^RW[A-Za-z0-9+/]{54}$'
}

if [[ "$MODE" == publish ]]; then
    if [[ -z "${HELIX_RELEASE_MINISIGN_KEY:-}" ]]; then
        echo "::error::HELIX_RELEASE_MINISIGN_KEY is not set. A release must publish signed manifests; see docs/devel/RELEASE_PROCESS.md (Signed channel manifests)." >&2
        exit 1
    fi
    if ! is_pubkey_file "$PUBKEY"; then
        echo "::error::$PUBKEY does not hold a minisign public key (still the placeholder?). Paste the release key's .pub file there; see docs/devel/RELEASE_PROCESS.md (Signed channel manifests)." >&2
        exit 1
    fi
fi

command -v minisign >/dev/null || { echo "::error::minisign is not installed" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
chmod 700 "$work"

password=""
if [[ "$MODE" == publish ]]; then
    seckey="$work/release.key"
    (umask 077; printf '%s\n' "$HELIX_RELEASE_MINISIGN_KEY" > "$seckey")
    password="${HELIX_RELEASE_MINISIGN_PASSWORD:-}"
    verify_with="$PUBKEY"
else
    echo "::notice::Dry run: manifests are signed with a key generated for this run; the release key is never read."
    seckey="$work/dryrun.key"
    verify_with="$work/dryrun.pub"
    minisign -G -W -p "$verify_with" -s "$seckey" >/dev/null
fi

for m in "$@"; do
    rm -f "$m.minisig"
    if [[ -n "$password" ]]; then
        printf '%s\n' "$password" | minisign -S -s "$seckey" -m "$m" -x "$m.minisig" >/dev/null
    else
        minisign -S -s "$seckey" -m "$m" -x "$m.minisig" </dev/null >/dev/null
    fi
    # -H refuses a legacy (not prehashed) signature, as the installer does.
    if ! minisign -V -H -q -p "$verify_with" -m "$m" -x "$m.minisig"; then
        echo "::error::the signature of $m does not verify against $verify_with; the signing key is not the pair of the public key clients trust" >&2
        rm -f "$m.minisig"
        exit 1
    fi
    echo "Signed $m -> $m.minisig"
done
