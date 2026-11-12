#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Signed channel manifests (prestonbrown/helixscreen#1834): release.yml signs every
# channel's manifest.json with scripts/sign-release-manifest.sh and publishes
# manifest.json.minisig beside it. The workflow checks read release.yml; the script checks
# run it, and the ones that sign need minisign.

load helpers

YML="${HELIX_TEST_RELEASE_YML:-.github/workflows/release.yml}"
SIGN=scripts/sign-release-manifest.sh

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    M="$BATS_TEST_TMPDIR/manifest-beta.json"
    echo '{"version":"1.2.3","assets":{}}' > "$M"
}

@test "every channel manifest is signed after it is generated and before anything uploads" {
    run python3 - "$YML" <<'PY'
import re, sys, yaml
steps = yaml.safe_load(open(sys.argv[1]))["jobs"]["upload-r2"]["steps"]
names = [s.get("name", "") for s in steps]
bad = []
try:
    inst = names.index("Install minisign")
    sign = names.index("Generate and sign channel manifests")
    up = names.index("Upload artifacts and manifests to R2")
except ValueError as e:
    sys.exit(f"missing step: {e}")
if not inst < sign < up:
    bad.append(f"order is install={inst} sign={sign} upload={up}")
run = steps[sign]["run"]
gen = run.find('--output "release-files/manifest-${CHANNEL}.json"')
call = run.find('scripts/sign-release-manifest.sh --mode "$RELEASE_MODE" "${manifests[@]}"')
if gen < 0 or call < 0 or call < gen:
    bad.append("the signing step does not sign the manifests it generates")
if 'manifests+=("release-files/manifest-${CHANNEL}.json")' not in run:
    bad.append("not every channel's manifest is collected for signing")
if "generate-manifest.sh" in steps[up]["run"]:
    bad.append("the upload step generates a manifest after signing")
print("\n".join(bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

@test "each channel's signature is uploaded beside its manifest, first, and checked as served" {
    run python3 - "$YML" <<'PY'
import re, sys, yaml
steps = yaml.safe_load(open(sys.argv[1]))["jobs"]["upload-r2"]["steps"]
run = next(s["run"] for s in steps if s.get("name") == "Upload artifacts and manifests to R2")
# Join backslash continuations so each command is one line.
run = re.sub(r"\s*\\\n\s*", " ", run)
sig = run.find('s3cp "release-files/manifest-${CHANNEL}.json.minisig" '
               '"s3://${R2_BUCKET}/${R2_PREFIX}${CHANNEL}/manifest.json.minisig"')
man = run.find('s3cp "release-files/manifest-${CHANNEL}.json" '
               '"s3://${R2_BUCKET}/${R2_PREFIX}${CHANNEL}/manifest.json"')
served = run.find('${R2_PREFIX}${CHANNEL}/manifest.json.minisig" '
                  '| cmp -s - "release-files/manifest-${CHANNEL}.json.minisig"')
bad = []
if sig < 0: bad.append("no upload of the channel's manifest.json.minisig")
if man < 0: bad.append("no upload of the channel's manifest.json")
if sig >= 0 and man >= 0 and sig > man: bad.append("the signature uploads after the manifest")
if served < 0: bad.append("the served signature is not compared with the one signed")
# A transient miss on one channel must not cost the later channels their upload.
loop = run.rfind("for attempt in 1 2 3 4; do", 0, served)
if served >= 0 and (loop < 0 or "sig_served=true; break" not in run[served:served + 200]):
    bad.append("the served-signature check is not retried")
print("\n".join(bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the release key reaches only the signing step, and only in a release" {
    run python3 - "$YML" <<'PY'
import re, sys, yaml
jobs = yaml.safe_load(open(sys.argv[1]))["jobs"]
bad, seen = [], set()
for jname, job in jobs.items():
    if "HELIX_RELEASE_MINISIGN" in yaml.safe_dump(job.get("env") or {}):
        bad.append(f"{jname}: job env carries the release key")
    for s in job.get("steps", []):
        where = f"{jname} / {s.get('name', '?')}"
        for k, v in (s.get("env") or {}).items():
            m = re.search(r"secrets\.(HELIX_RELEASE_MINISIGN_\w+)", str(v))
            if not m:
                continue
            if where != "upload-r2 / Generate and sign channel manifests":
                bad.append(f"{where}: reads {m.group(1)}")
            want = "${{ env.RELEASE_MODE == 'publish' && secrets.%s || '' }}" % m.group(1)
            if str(v).strip() != want:
                bad.append(f"{where}: {k} is not gated on a release: {v}")
            seen.add(m.group(1))
        if "secrets.HELIX_RELEASE_MINISIGN" in str(s.get("run", "")):
            bad.append(f"{where}: run text reads the release key")
if "HELIX_RELEASE_MINISIGN_KEY" not in seen:
    bad.append("no step is given HELIX_RELEASE_MINISIGN_KEY")
print("\n".join(bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

# ------------------------------------------------------------- the script

placeholder() {
    printf 'untrusted comment: PLACEHOLDER\nPLACEHOLDER_RELEASE_PUBLIC_KEY\n' > "$BATS_TEST_TMPDIR/placeholder.pub"
    echo "$BATS_TEST_TMPDIR/placeholder.pub"
}

# keypair NAME [PASSWORD]: NAME.pub and NAME.key in the test dir, password-protected when given.
keypair() {
    local d="$BATS_TEST_TMPDIR"
    if [ -n "${2:-}" ]; then
        printf '%s\n%s\n' "$2" "$2" | minisign -G -p "$d/$1.pub" -s "$d/$1.key" >/dev/null 2>&1
    else
        minisign -G -W -p "$d/$1.pub" -s "$d/$1.key" >/dev/null 2>&1
    fi
}

@test "a release without the key secret fails and signs nothing" {
    run env -u HELIX_RELEASE_MINISIGN_KEY "$SIGN" --mode publish --pubkey "$(placeholder)" "$M"
    [ "$status" -eq 1 ] || fail "$output"
    [[ "$output" == *"HELIX_RELEASE_MINISIGN_KEY is not set"* ]] || fail "$output"
    [ ! -e "$M.minisig" ]
}

@test "a release against the placeholder public key fails and signs nothing" {
    run env HELIX_RELEASE_MINISIGN_KEY=anything "$SIGN" --mode publish --pubkey "$(placeholder)" "$M"
    [ "$status" -eq 1 ] || fail "$output"
    [[ "$output" == *"does not hold a minisign public key"* ]] || fail "$output"
    [ ! -e "$M.minisig" ]
}

@test "a release signs with the secret key and its signature verifies, prehashed, against the public key" {
    command -v minisign >/dev/null || skip "minisign not installed"
    keypair rel pw
    run env HELIX_RELEASE_MINISIGN_KEY="$(cat "$BATS_TEST_TMPDIR/rel.key")" \
        HELIX_RELEASE_MINISIGN_PASSWORD=pw \
        "$SIGN" --mode publish --pubkey "$BATS_TEST_TMPDIR/rel.pub" "$M"
    [ "$status" -eq 0 ] || fail "$output"
    run minisign -V -H -p "$BATS_TEST_TMPDIR/rel.pub" -m "$M" -x "$M.minisig"
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" == *"Trusted comment: file:manifest.json channel:beta"* ]] || fail "$output"
}

@test "a release whose secret is not the public key's pair fails and leaves no signature" {
    command -v minisign >/dev/null || skip "minisign not installed"
    keypair rel
    keypair other
    run env HELIX_RELEASE_MINISIGN_KEY="$(cat "$BATS_TEST_TMPDIR/other.key")" \
        "$SIGN" --mode publish --pubkey "$BATS_TEST_TMPDIR/rel.pub" "$M"
    [ "$status" -eq 1 ] || fail "$output"
    [[ "$output" == *"does not verify against"* ]] || fail "$output"
    [ ! -e "$M.minisig" ]
}

@test "a dry run signs with a key of its own and never the release key" {
    command -v minisign >/dev/null || skip "minisign not installed"
    keypair rel
    run env HELIX_RELEASE_MINISIGN_KEY="$(cat "$BATS_TEST_TMPDIR/rel.key")" \
        "$SIGN" --mode dryrun --pubkey "$BATS_TEST_TMPDIR/rel.pub" "$M"
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" == *"the release key is never read"* ]] || fail "$output"
    [ -s "$M.minisig" ]
    run minisign -V -q -p "$BATS_TEST_TMPDIR/rel.pub" -m "$M" -x "$M.minisig"
    [ "$status" -ne 0 ] || fail "a dry-run signature verifies against the release key"
}
