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

# check_signed_before_upload YML JOB MODE: in JOB, minisign is installed, every channel
# manifest is generated and signed with --mode MODE, and only then does anything upload.
check_signed_before_upload() {
    run python3 - "$@" <<'PY'
import re, sys, yaml
yml, job, mode = sys.argv[1:4]
steps = yaml.safe_load(open(yml))["jobs"][job]["steps"]
names = [s.get("name", "") for s in steps]
bad = []
try:
    inst = names.index("Install minisign")
    sign = names.index("Generate and sign channel manifests")
    up = names.index("Upload artifacts and manifests to R2")
except ValueError as e:
    sys.exit(f"{job}: missing step: {e}")
if not inst < sign < up:
    bad.append(f"order is install={inst} sign={sign} upload={up}")
run = steps[sign]["run"]
gen = run.find('--output "release-files/manifest-${CHANNEL}.json"')
call = run.find('scripts/sign-release-manifest.sh --mode %s "${manifests[@]}"' % mode)
if gen < 0 or call < 0 or call < gen:
    bad.append("the signing step does not sign the manifests it generates")
if 'manifests+=("release-files/manifest-${CHANNEL}.json")' not in run:
    bad.append("not every channel's manifest is collected for signing")
if "generate-manifest.sh" in steps[up]["run"]:
    bad.append("the upload step generates a manifest after signing")
print("\n".join(f"{job}: {b}" for b in bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

# check_signature_uploaded_first YML JOB PREFIX: each channel's .minisig goes up before its
# manifest, under PREFIX, and the served signature is compared with the one signed, retried.
check_signature_uploaded_first() {
    run python3 - "$@" <<'PY'
import re, sys, yaml
yml, job, pfx = sys.argv[1:4]
steps = yaml.safe_load(open(yml))["jobs"][job]["steps"]
run = next(s["run"] for s in steps if s.get("name") == "Upload artifacts and manifests to R2")
# Join backslash continuations so each command is one line.
run = re.sub(r"\s*\\\n\s*", " ", run)
sig = run.find('s3cp "release-files/manifest-${CHANNEL}.json.minisig" '
               '"s3://${R2_BUCKET}/%s${CHANNEL}/manifest.json.minisig"' % pfx)
man = run.find('s3cp "release-files/manifest-${CHANNEL}.json" '
               '"s3://${R2_BUCKET}/%s${CHANNEL}/manifest.json"' % pfx)
served = run.find('%s${CHANNEL}/manifest.json.minisig" '
                  '| cmp -s - "release-files/manifest-${CHANNEL}.json.minisig"' % pfx)
bad = []
if sig < 0: bad.append("no upload of the channel's manifest.json.minisig")
if man < 0: bad.append("no upload of the channel's manifest.json")
if sig >= 0 and man >= 0 and sig > man: bad.append("the signature uploads after the manifest")
if served < 0: bad.append("the served signature is not compared with the one signed")
# A transient miss on one channel must not cost the later channels their upload.
loop = run.rfind("for attempt in 1 2 3 4; do", 0, served)
if served >= 0 and (loop < 0 or "sig_served=true; break" not in run[served:served + 200]):
    bad.append("the served-signature check is not retried")
print("\n".join(f"{job}: {b}" for b in bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

@test "every channel manifest is signed after it is generated and before anything uploads" {
    check_signed_before_upload "$YML" upload-r2 '"$RELEASE_MODE"'
}

@test "each channel's signature is uploaded beside its manifest, first, and checked as served" {
    check_signature_uploaded_first "$YML" upload-r2 '${R2_PREFIX}'
}

@test "the release key reaches only the signing step, only in a release, behind its environment" {
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
# The key is an environment secret: a release reads it only after a reviewer approves
# release-signing; a dry run takes no environment.
env = jobs["upload-r2"].get("environment")
if env != "${{ needs.plan.outputs.mode == 'publish' && 'release-signing' || '' }}":
    bad.append(f"upload-r2 is not gated on the release-signing environment: {env}")
print("\n".join(bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

# ------------------------------------------------------------- recover-r2.yml

# A recovery republishes a tag's manifest; an unsigned one beside the previous release's
# signature is refused by every client, so it signs exactly as a release does.
RECOVER=.github/workflows/recover-r2.yml

@test "a recovery signs every channel manifest before anything uploads" {
    check_signed_before_upload "$RECOVER" recover publish
}

@test "a recovery uploads each channel's signature first and checks it as served" {
    check_signature_uploaded_first "$RECOVER" recover ""
}

@test "a recovery reads the release key only in its signing step, behind release-signing, from the tag" {
    run python3 - "$RECOVER" <<'PY'
import re, sys, yaml
wf = yaml.safe_load(open(sys.argv[1]))
jobs = wf["jobs"]
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
            if where != "recover / Generate and sign channel manifests":
                bad.append(f"{where}: reads {m.group(1)}")
            seen.add(m.group(1))
        if "secrets.HELIX_RELEASE_MINISIGN" in str(s.get("run", "")):
            bad.append(f"{where}: run text reads the release key")
if "HELIX_RELEASE_MINISIGN_KEY" not in seen:
    bad.append("no step is given HELIX_RELEASE_MINISIGN_KEY")
rec = jobs["recover"]
if rec.get("environment") != "release-signing":
    bad.append(f"recover is not gated on the release-signing environment: {rec.get('environment')}")
# release-signing admits only v* tags, so a dispatch from a branch is refused by name before
# the environment would refuse it with nothing said about how to run it.
needs = rec.get("needs") or []
needs = [needs] if isinstance(needs, str) else needs
guard = next((jobs[n] for n in needs if n in jobs), None)
guard_run = "".join(str(s.get("run", "")) for s in (guard or {}).get("steps", []))
if guard is None or guard.get("environment"):
    bad.append("recover does not wait on a guard job outside the environment")
elif '"$REF" != "refs/tags/$TAG"' not in guard_run or "--ref" not in guard_run:
    bad.append("the guard does not refuse a dispatch whose ref is not the tag, naming --ref")
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
