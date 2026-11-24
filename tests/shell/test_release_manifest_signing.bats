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
call = run.find('scripts/sign-release-manifest.sh --mode %s --key "$SIGNING_KEY" "${manifests[@]}"'
                % mode)
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

# gha_eval: a Python module (on stdin of `python3 - ...` via $GHA_EVAL) that evaluates the
# GitHub expressions these workflows use: `${{ a == 'x' && 'y' || '' }}` over named contexts.
# An identifier the context does not name is an error, so an expression that reads anything
# but what a test supplies fails the test.
GHA_EVAL=$(cat <<'PY'
import re

def gha_eval(text, ctx):
    text = str(text).strip()
    m = re.fullmatch(r"\$\{\{(.*)\}\}", text, re.S)
    if not m:
        return text
    expr = m.group(1)
    out, pos = [], 0
    tok = re.compile(r"\s+|'[^']*'|&&|\|\||==|!=|\(|\)|[A-Za-z_][\w.-]*")
    while pos < len(expr):
        t = tok.match(expr, pos)
        if not t:
            raise ValueError(f"cannot parse {expr!r} at {pos}")
        v = t.group(0)
        pos = t.end()
        if v.isspace() or v in "()":
            out.append(v)
        elif v.startswith("'"):
            out.append(repr(v[1:-1]))
        elif v in ("&&", "||"):
            out.append(" and " if v == "&&" else " or ")
        elif v in ("==", "!="):
            out.append(v)
        else:
            if v not in ctx:
                raise KeyError(f"expression reads {v}, which the test does not supply")
            out.append(repr(ctx[v]))
    return eval("".join(out), {"__builtins__": {}})
PY
)

@test "every channel manifest is signed after it is generated and before anything uploads" {
    check_signed_before_upload "$YML" upload-r2 '"$RELEASE_MODE"'
}

@test "each channel's signature is uploaded beside its manifest, first, and checked as served" {
    check_signature_uploaded_first "$YML" upload-r2 '${R2_PREFIX}'
}

@test "the signing keys reach only the signing step, only in a release, behind their environment" {
    run python3 - "$YML" <<PY
$GHA_EVAL
import re, sys, yaml
jobs = yaml.safe_load(open(sys.argv[1]))["jobs"]
bad, seen = [], set()
for jname, job in jobs.items():
    if "MINISIGN" in yaml.safe_dump(job.get("env") or {}):
        bad.append(f"{jname}: job env carries a signing key")
    for s in job.get("steps", []):
        where = f"{jname} / {s.get('name', '?')}"
        for k, v in (s.get("env") or {}).items():
            m = re.search(r"secrets\.(HELIX_\w+_MINISIGN_\w+)", str(v))
            if not m:
                continue
            if where != "upload-r2 / Generate and sign channel manifests":
                bad.append(f"{where}: reads {m.group(1)}")
            seen.add(m.group(1))
        if re.search(r"secrets\.HELIX_\w*MINISIGN", str(s.get("run", ""))):
            bad.append(f"{where}: run text reads a signing key")
for want in ("HELIX_RELEASE_MINISIGN_KEY", "HELIX_DEV_MINISIGN_KEY"):
    if want not in seen:
        bad.append(f"no step is given {want}")
print("\n".join(bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

# The environment upload-r2 runs in, and the key its signing step is handed, for each run
# mode and each key the release job resolved: the dev key only in release-signing-dev, the
# release key only in release-signing, neither in a dry run.
@test "upload-r2 signs with the key its environment holds, chosen by the release job" {
    run python3 - "$YML" <<PY
$GHA_EVAL
import sys, yaml
jobs = yaml.safe_load(open(sys.argv[1]))["jobs"]
job = jobs["upload-r2"]
step = next(s for s in job["steps"] if s.get("name") == "Generate and sign channel manifests")
secrets = {
    "secrets.HELIX_RELEASE_MINISIGN_KEY": "MAIN-KEY",
    "secrets.HELIX_RELEASE_MINISIGN_PASSWORD": "MAIN-PW",
    "secrets.HELIX_DEV_MINISIGN_KEY": "DEV-KEY",
    "secrets.HELIX_DEV_MINISIGN_PASSWORD": "DEV-PW",
}
want_env = {("publish", "main"): "release-signing", ("publish", "dev"): "release-signing-dev",
            ("dryrun", "main"): "", ("dryrun", "dev"): ""}
bad = []
needs = job.get("needs") or []
if "release" not in needs or "plan" not in needs:
    bad.append(f"upload-r2 does not wait on the jobs that choose its environment: {needs}")
if "signing" not in (jobs["release"].get("outputs") or {}):
    bad.append("the release job has no signing output")
for (mode, signing), env_want in want_env.items():
    ctx = {"needs.plan.outputs.mode": mode, "needs.release.outputs.signing": signing,
           "env.RELEASE_MODE": mode, **secrets}
    got = gha_eval(job.get("environment"), ctx)
    if got != env_want:
        bad.append(f"{mode}/{signing}: environment {got!r}, want {env_want!r}")
    env = {k: gha_eval(v, ctx) for k, v in step["env"].items()
           if "MINISIGN" in k or k == "SIGNING_KEY"}
    want = {k: "" for k in ("HELIX_RELEASE_MINISIGN_KEY", "HELIX_RELEASE_MINISIGN_PASSWORD",
                            "HELIX_DEV_MINISIGN_KEY", "HELIX_DEV_MINISIGN_PASSWORD")}
    if mode == "publish" and signing == "main":
        want.update(HELIX_RELEASE_MINISIGN_KEY="MAIN-KEY", HELIX_RELEASE_MINISIGN_PASSWORD="MAIN-PW")
    if mode == "publish" and signing == "dev":
        want.update(HELIX_DEV_MINISIGN_KEY="DEV-KEY", HELIX_DEV_MINISIGN_PASSWORD="DEV-PW")
    want["SIGNING_KEY"] = signing
    if env != want:
        bad.append(f"{mode}/{signing}: signing step env {env}, want {want}")
print("\n".join(bad)); sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

# The release job's channel step writes release-channel.sh's KEY=value lines, signing among
# them, to its outputs; the job's signing output is that step's.
@test "the release job resolves the signing key with the channels, from release-channel.sh" {
    run python3 - "$YML" <<'PY'
import sys, yaml
job = yaml.safe_load(open(sys.argv[1]))["jobs"]["release"]
bad = []
if job["outputs"].get("signing") != "${{ steps.channel.outputs.signing }}":
    bad.append(f"signing output: {job['outputs'].get('signing')}")
if job["outputs"].get("channels") != "${{ steps.channel.outputs.channels }}":
    bad.append("channels and signing come from different steps")
step = next(s for s in job["steps"] if s.get("id") == "channel")
if 'scripts/release-channel.sh --tag "$GITHUB_REF_NAME" | tee -a "$GITHUB_OUTPUT"' not in step["run"]:
    bad.append("the channel step does not write release-channel.sh's output for the tag")
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

@test "a recovery reads a signing key only in its signing step, behind its environment, from the tag" {
    run python3 - "$RECOVER" <<PY
$GHA_EVAL
import re, sys, yaml
wf = yaml.safe_load(open(sys.argv[1]))
jobs = wf["jobs"]
bad, seen = [], set()
for jname, job in jobs.items():
    if "MINISIGN" in yaml.safe_dump(job.get("env") or {}):
        bad.append(f"{jname}: job env carries a signing key")
    for s in job.get("steps", []):
        where = f"{jname} / {s.get('name', '?')}"
        for k, v in (s.get("env") or {}).items():
            m = re.search(r"secrets\.(HELIX_\w+_MINISIGN_\w+)", str(v))
            if not m:
                continue
            if where != "recover / Generate and sign channel manifests":
                bad.append(f"{where}: reads {m.group(1)}")
            seen.add(m.group(1))
        if re.search(r"secrets\.HELIX_\w*MINISIGN", str(s.get("run", ""))):
            bad.append(f"{where}: run text reads a signing key")
for want in ("HELIX_RELEASE_MINISIGN_KEY", "HELIX_DEV_MINISIGN_KEY"):
    if want not in seen:
        bad.append(f"no step is given {want}")
rec = jobs["recover"]
# Both environments admit only tags, so a dispatch from a branch is refused by name before
# an environment would refuse it with nothing said about how to run it.
needs = rec.get("needs") or []
needs = [needs] if isinstance(needs, str) else needs
guard = jobs.get("guard")
guard_run = "".join(str(s.get("run", "")) for s in (guard or {}).get("steps", []))
if "guard" not in needs or guard is None or guard.get("environment"):
    bad.append("recover does not wait on a guard job outside the environment")
elif '"\$REF" != "refs/tags/\$TAG"' not in guard_run or "--ref" not in guard_run:
    bad.append("the guard does not refuse a dispatch whose ref is not the tag, naming --ref")
if guard is not None and "signing" not in (guard.get("outputs") or {}):
    bad.append("the guard resolves no signing key")
step = next(s for s in rec["steps"] if s.get("name") == "Generate and sign channel manifests")
secrets = {
    "secrets.HELIX_RELEASE_MINISIGN_KEY": "MAIN-KEY",
    "secrets.HELIX_RELEASE_MINISIGN_PASSWORD": "MAIN-PW",
    "secrets.HELIX_DEV_MINISIGN_KEY": "DEV-KEY",
    "secrets.HELIX_DEV_MINISIGN_PASSWORD": "DEV-PW",
}
for signing, env_want, key_want in (("main", "release-signing", ("MAIN-KEY", "")),
                                     ("dev", "release-signing-dev", ("", "DEV-KEY"))):
    ctx = {"needs.guard.outputs.signing": signing, **secrets}
    got = gha_eval(rec.get("environment"), ctx)
    if got != env_want:
        bad.append(f"{signing}: environment {got!r}, want {env_want!r}")
    keys = (gha_eval(step["env"]["HELIX_RELEASE_MINISIGN_KEY"], ctx),
            gha_eval(step["env"]["HELIX_DEV_MINISIGN_KEY"], ctx))
    if keys != key_want:
        bad.append(f"{signing}: signing step is handed {keys}, want {key_want}")
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

# ------------------------------------------------------------- the dev key

@test "the dev key signs the dev manifest, and its signature verifies against the dev public key" {
    command -v minisign >/dev/null || skip "minisign not installed"
    keypair dev pw
    D="$BATS_TEST_TMPDIR/manifest-dev.json"
    echo '{"version":"1.3.0-alpha.1","assets":{}}' > "$D"
    run env -u HELIX_RELEASE_MINISIGN_KEY HELIX_DEV_MINISIGN_KEY="$(cat "$BATS_TEST_TMPDIR/dev.key")" \
        HELIX_DEV_MINISIGN_PASSWORD=pw \
        "$SIGN" --mode publish --key dev --pubkey "$BATS_TEST_TMPDIR/dev.pub" "$D"
    [ "$status" -eq 0 ] || fail "$output"
    run minisign -V -H -p "$BATS_TEST_TMPDIR/dev.pub" -m "$D" -x "$D.minisig"
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" == *"Trusted comment: file:manifest.json channel:dev"* ]] || fail "$output"
}

@test "the dev key never signs another channel's manifest, in a release or a dry run" {
    command -v minisign >/dev/null || skip "minisign not installed"
    keypair dev
    D="$BATS_TEST_TMPDIR/manifest-dev.json"
    echo '{"version":"1.3.0-alpha.1","assets":{}}' > "$D"
    for mode in publish dryrun; do
        run env HELIX_DEV_MINISIGN_KEY="$(cat "$BATS_TEST_TMPDIR/dev.key")" \
            "$SIGN" --mode "$mode" --key dev --pubkey "$BATS_TEST_TMPDIR/dev.pub" "$D" "$M"
        [ "$status" -eq 1 ] || fail "$mode: $output"
        [[ "$output" == *"signs the dev channel's manifest only"* ]] || fail "$mode: $output"
        [ ! -e "$M.minisig" ] && [ ! -e "$D.minisig" ] || fail "$mode: a signature was left"
    done
}

@test "a dev-key release reads only the dev secret, and a release-key release only the release secret" {
    command -v minisign >/dev/null || skip "minisign not installed"
    keypair rel
    keypair dev
    D="$BATS_TEST_TMPDIR/manifest-dev.json"
    echo '{"version":"1.3.0-alpha.1","assets":{}}' > "$D"
    # The release key's secret does not stand in for the dev key's.
    run env -u HELIX_DEV_MINISIGN_KEY HELIX_RELEASE_MINISIGN_KEY="$(cat "$BATS_TEST_TMPDIR/rel.key")" \
        "$SIGN" --mode publish --key dev --pubkey "$BATS_TEST_TMPDIR/dev.pub" "$D"
    [ "$status" -eq 1 ] || fail "$output"
    [[ "$output" == *"HELIX_DEV_MINISIGN_KEY is not set"* ]] || fail "$output"
    [ ! -e "$D.minisig" ]
    # Nor the dev key's for the release key's.
    run env -u HELIX_RELEASE_MINISIGN_KEY HELIX_DEV_MINISIGN_KEY="$(cat "$BATS_TEST_TMPDIR/dev.key")" \
        "$SIGN" --mode publish --pubkey "$BATS_TEST_TMPDIR/rel.pub" "$M"
    [ "$status" -eq 1 ] || fail "$output"
    [[ "$output" == *"HELIX_RELEASE_MINISIGN_KEY is not set"* ]] || fail "$output"
    [ ! -e "$M.minisig" ]
}

@test "a dev-key release checks against the committed dev public key, and fails closed on its placeholder" {
    echo '{"version":"1.3.0-alpha.1","assets":{}}' > "$BATS_TEST_TMPDIR/manifest-dev.json"
    if sed -n 2p .github/release-minisign-dev.pub | grep -Eq '^RW[A-Za-z0-9+/]{54}$'; then
        skip "the dev public key is committed"
    fi
    run env HELIX_DEV_MINISIGN_KEY=anything "$SIGN" --mode publish --key dev \
        "$BATS_TEST_TMPDIR/manifest-dev.json"
    [ "$status" -eq 1 ] || fail "$output"
    [[ "$output" == *"release-minisign-dev.pub does not hold a minisign public key"* ]] || fail "$output"
}
