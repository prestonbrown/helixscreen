#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# sign-release-manifest.sh --mode publish|dryrun [--key main|dev] [--pubkey FILE] MANIFEST...
#
# Signs each channel manifest with minisign (prehashed) into MANIFEST.minisig and checks the
# signature before anything is uploaded. The easy installer refuses a release list whose
# signature does not verify against the release key compiled into it.
#
#   publish  Signs with the secret key from the environment and verifies against FILE, the
#            public key the clients compile in. A missing secret, a placeholder FILE or a
#            secret that is not FILE's pair fails: a release never publishes an unsigned
#            manifest.
#   dryrun   Never reads a secret key. Signs with a key generated for this run and verifies
#            against it, so the whole signing path runs without the secret, and nothing a dry
#            run leaves in R2 verifies against the key clients trust.
#
#   --key main  (default) The release key: HELIX_RELEASE_MINISIGN_KEY (the text of the
#               minisign .key file) and HELIX_RELEASE_MINISIGN_PASSWORD (its password, if any);
#               FILE defaults to .github/release-minisign.pub.
#   --key dev   The dev-channel key: HELIX_DEV_MINISIGN_KEY and HELIX_DEV_MINISIGN_PASSWORD;
#               FILE defaults to .github/release-minisign-dev.pub. Clients trust it on the dev
#               channel only, so it refuses, in either mode, any manifest but manifest-dev.json.
#
# Setup of the keys and the secrets: docs/devel/RELEASE_SIGNING.md.

set -euo pipefail

usage() { echo "Usage: $0 --mode publish|dryrun [--key main|dev] [--pubkey FILE] MANIFEST..." >&2; exit 2; }

MODE=""
KEY=main
PUBKEY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="${2:-}"; shift 2 ;;
        --key) KEY="${2:-}"; shift 2 ;;
        --pubkey) PUBKEY="${2:-}"; shift 2 ;;
        --) shift; break ;;
        -*) usage ;;
        *) break ;;
    esac
done
[[ "$MODE" == publish || "$MODE" == dryrun ]] || usage
[[ $# -ge 1 ]] || usage
for m in "$@"; do [[ -f "$m" ]] || { echo "::error::no manifest at $m" >&2; exit 2; }; done

case "$KEY" in
    main)
        SECRET_VAR=HELIX_RELEASE_MINISIGN_KEY
        PASSWORD_VAR=HELIX_RELEASE_MINISIGN_PASSWORD
        PUBKEY="${PUBKEY:-$(dirname "$0")/../.github/release-minisign.pub}"
        ;;
    dev)
        SECRET_VAR=HELIX_DEV_MINISIGN_KEY
        PASSWORD_VAR=HELIX_DEV_MINISIGN_PASSWORD
        PUBKEY="${PUBKEY:-$(dirname "$0")/../.github/release-minisign-dev.pub}"
        for m in "$@"; do
            if [[ "$(basename "$m")" != manifest-dev.json ]]; then
                echo "::error::the dev key signs the dev channel's manifest only, not $m; a release publishing to any other channel signs with the release key" >&2
                exit 1
            fi
        done
        ;;
    *) usage ;;
esac

# The key line of a minisign .pub file: RW plus 54 base64 characters.
is_pubkey_file() {
    [[ -f "$1" ]] && sed -n 2p "$1" | grep -Eq '^RW[A-Za-z0-9+/]{54}$'
}

if [[ "$MODE" == publish ]]; then
    if [[ -z "${!SECRET_VAR:-}" ]]; then
        echo "::error::$SECRET_VAR is not set. A release must publish signed manifests; see docs/devel/RELEASE_SIGNING.md." >&2
        exit 1
    fi
    if ! is_pubkey_file "$PUBKEY"; then
        echo "::error::$PUBKEY does not hold a minisign public key (still the placeholder?). Paste the $KEY key's .pub file there; see docs/devel/RELEASE_SIGNING.md." >&2
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
    (umask 077; printf '%s\n' "${!SECRET_VAR}" > "$seckey")
    password="${!PASSWORD_VAR:-}"
    verify_with="$PUBKEY"
else
    echo "::notice::Dry run ($KEY key): manifests are signed with a key generated for this run; the release key is never read."
    seckey="$work/dryrun.key"
    verify_with="$work/dryrun.pub"
    minisign -G -W -p "$verify_with" -s "$seckey" >/dev/null
fi

for m in "$@"; do
    rm -f "$m.minisig"
    # The signed comment names the file as published (<channel>/manifest.json), not the
    # local manifest-<channel>.json it is signed from.
    channel=$(basename "$m" .json)
    trusted="file:manifest.json channel:${channel#manifest-}"
    if [[ -n "$password" ]]; then
        printf '%s\n' "$password" | minisign -S -s "$seckey" -m "$m" -x "$m.minisig" -t "$trusted" >/dev/null
    else
        minisign -S -s "$seckey" -m "$m" -x "$m.minisig" -t "$trusted" </dev/null >/dev/null
    fi
    # -H refuses a legacy (not prehashed) signature, as the installer does.
    if ! minisign -V -H -q -p "$verify_with" -m "$m" -x "$m.minisig"; then
        echo "::error::the signature of $m does not verify against $verify_with; the signing key is not the pair of the public key clients trust" >&2
        rm -f "$m.minisig"
        exit 1
    fi
    echo "Signed $m -> $m.minisig"
done
