#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release.yml dry-run mode: a workflow_dispatch must build and upload everything
# while publishing nothing. The guard lint below reads the
# workflow; the verifier tests run scripts/verify-dry-run-release.sh against a
# fixture built with the real manifest generator.

load helpers

YML="${HELIX_TEST_RELEASE_YML:-.github/workflows/release.yml}"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# ------------------------------------------------------------- guard lint

@test "RELEASE_MODE is publish only for a tag push" {
    run python3 - "$YML" <<'PY'
import re, sys, yaml
with open(sys.argv[1]) as fh:
    expr = yaml.safe_load(fh)["env"]["RELEASE_MODE"]
inner = re.fullmatch(r"\$\{\{(.*)\}\}", expr.strip()).group(1)
# Translate the GitHub expression into Python, whose and/or return operands
# the same way. An unknown term is a NameError, so a rewrite fails here
# instead of passing unexamined.
py = (inner.replace("startsWith(github.ref, 'refs/tags/v')", "is_tag")
           .replace("github.event_name == 'push'", "is_push")
           .replace("&&", " and ").replace("||", " or ").replace("!", " not "))
cases = [
    # is_tag, is_push, expected
    (True,  True,  "publish"),   # tag push
    (True,  False, "dryrun"),    # dispatch on a tag
    (False, False, "dryrun"),    # dispatch on a branch
    (False, True,  "dryrun"),    # branch push (not a trigger, but never publish)
]
bad = []
for is_tag, is_push, want in cases:
    got = eval(py, {}, dict(is_tag=is_tag, is_push=is_push))
    if got != want:
        bad.append(f"tag={is_tag} push={is_push}: {got!r}, want {want!r}")
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}

# An allowlist: a step that is not gated on publish may not use a secret,
# publish through an action, or make a mutating call, except the Android
# signing steps (the build needs the key) and R2 steps that write only under a
# dry-run-asserted R2_PREFIX. Prints one line per offender.
lint_publish_guards() {
    python3 - "$1" <<'PY'
import re, sys, yaml
with open(sys.argv[1]) as fh:
    jobs = yaml.safe_load(fh)["jobs"]

PUBLISH_ACTIONS = ("softprops/action-gh-release", "r0adkll/upload-google-play",
                   "peter-evans/repository-dispatch")
MUTATING = [r"\bs3api\b", r"\bwrangler\b", r"\brclone\b", r"\bgh\s+release\b",
            r"\bgh\s+api\b.*\s(-X|--method)\b",
            r"\bcurl\b.*\s(-X|--request)\s*['\"]?(POST|PUT|DELETE|PATCH)\b"]
SIGNING = {("build-android", "Materialize upload keystore"),
           ("build-android", "Build Android APKs (release)"),
           ("build-android", "Build Android AAB (release)")}
STEP_GATE = "env.RELEASE_MODE == 'publish'"
JOB_GATE = "needs.plan.outputs.mode == 'publish'"
PREFIX_ASSERT = '[[ "$R2_PREFIX" == dry-run/?*/ ]]'
PREFIX_ENV = "${{ needs.plan.outputs.r2_prefix }}"

def r2_prefixed(job, run):
    paths = re.findall(r"s3://[^\s\"']*", run)
    return (PREFIX_ASSERT in run and paths
            and all(p.startswith("s3://${R2_BUCKET}/${R2_PREFIX}") for p in paths)
            and (job.get("env") or {}).get("R2_PREFIX") == PREFIX_ENV)

bad = []
for jname, job in jobs.items():
    if "secrets." in yaml.safe_dump(job.get("env") or {}):
        bad.append(f"{jname}: job-level env hands a secret to every step")
    if JOB_GATE in str(job.get("if", "")):
        continue
    for step in job.get("steps", []):
        if STEP_GATE in str(step.get("if", "")):
            continue
        name = step.get("name", "?")
        where = f"{jname} / {name}"
        # Join backslash continuations so a multi-line curl is one line.
        run = re.sub(r"\\\n\s*", " ", step.get("run", ""))
        if step.get("uses", "").startswith(PUBLISH_ACTIONS):
            bad.append(f"{where}: publishing action without a publish gate")
        for pat in MUTATING:
            if re.search(pat, run):
                bad.append(f"{where}: mutating call /{pat}/ without a publish gate")
        uses_secret = "secrets." in yaml.safe_dump(step)
        if "s3://" in run or "aws s3" in run:
            if not r2_prefixed(job, run):
                bad.append(f"{where}: R2 access not confined to an asserted dry-run R2_PREFIX")
        elif uses_secret and (jname, name) not in SIGNING:
            bad.append(f"{where}: uses a secret without a publish gate")
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY
}

@test "every step using a secret or publishing is gated on publish or allowlisted" {
    run lint_publish_guards "$YML"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the guard lint fires on ungated publishing, secrets and unprefixed R2 writes" {
    local broken="$BATS_TEST_TMPDIR/release.yml"
    sed -e "/name: Create GitHub Release/{n;/RELEASE_MODE == 'publish'/d}" \
        -e 's|s3://${R2_BUCKET}/${R2_PREFIX}install.sh|s3://${R2_BUCKET}/install.sh|' \
        "$YML" > "$broken"
    run lint_publish_guards "$broken"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Create GitHub Release: publishing action without a publish gate"* ]] || fail "$output"
    [[ "$output" == *"Upload artifacts and manifests to R2: R2 access not confined"* ]] || fail "$output"
}

@test "the guard lint fires on a planted ungated step using a secret or a mutating call" {
    local broken="$BATS_TEST_TMPDIR/release.yml"
    awk '{ print }
         /^      run: make test-shell( |$)/ {
             print "    - name: Planted secret"
             print "      env:"
             print "        TOKEN: ${{ secrets.SOME_TOKEN }}"
             print "      run: echo \"$TOKEN\" > /dev/null"
             print "    - name: Planted curl"
             print "      run: |"
             print "        curl -s \\"
             print "          -X DELETE https://example.invalid/x"
             print "    - name: Planted rclone"
             print "      run: rclone copy a b"
         }' "$YML" > "$broken"
    run lint_publish_guards "$broken"
    [ "$status" -ne 0 ]
    [[ "$output" == *"validate-shell / Planted secret: uses a secret without a publish gate"* ]] || fail "$output"
    [[ "$output" == *"validate-shell / Planted curl: mutating call"* ]] || fail "$output"
    [[ "$output" == *"validate-shell / Planted rclone: mutating call"* ]] || fail "$output"
}

# ------------------------------------------------------------- verifier

VERSION=0.0.0-dryrun.42
BASE=https://cdn.example.invalid/dry-run/42/

# A consistent artifact + R2 prefix for pi and x86 on channels beta and dev.
make_fixture() {
    ART="$BATS_TEST_TMPDIR/art" R2="$BATS_TEST_TMPDIR/r2"
    REL="$R2/releases/v$VERSION"
    mkdir -p "$ART" "$REL" "$R2/beta" "$R2/dev"
    local p
    for p in pi x86; do
        echo "$p tarball" > "$ART/helixscreen-$p-v1.1.0-beta.3.tar.gz"
        echo "$p zip" > "$ART/helixscreen-$p.zip"
    done
    echo '#!/bin/sh' > "$ART/install.sh"
    cp "$ART"/*.tar.gz "$ART"/*.zip "$REL/"
    cp "$ART/install.sh" "$R2/install.sh"
    scripts/generate-manifest.sh --version "$VERSION" --tag "v$VERSION" --notes n \
        --dir "$REL" --base-url "${BASE}releases/v$VERSION" \
        --zip-exclude "" --output "$R2/beta/manifest.json" >/dev/null
    cp "$R2/beta/manifest.json" "$R2/dev/manifest.json"
}

verify() {
    run scripts/verify-dry-run-release.sh --artifact "$ART" --r2 "$R2" --base-url "$BASE" \
        --version "$VERSION" --platforms "pi x86" --channels "beta dev"
}

@test "verifier passes a consistent dry run" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    verify
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" == *"beta -> releases/v$VERSION/helixscreen-x86.zip"* ]] || fail "$output"
}

@test "verifier fails when install.sh differs" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    echo changed >> "$R2/install.sh"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"install.sh differs"* ]] || fail "$output"
}

@test "verifier fails when a manifest points outside the dry-run prefix" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    sed -i 's|/dry-run/42/releases|/releases|' "$R2/dev/manifest.json"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"dev manifest points outside the dry-run prefix"* ]] || fail "$output"
}

@test "verifier fails when an uploaded file does not match its manifest sha256" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    echo corrupt >> "$REL/helixscreen-pi.zip"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"sha256 for ${BASE}releases/v$VERSION/helixscreen-pi.zip"* ]] || fail "$output"
}

@test "verifier fails when a platform's zip never reached R2" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    rm "$REL/helixscreen-x86.zip"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"x86 zip in $REL missing"* ]] || fail "$output"
}

# ------------------------------------------------------- symbol flattening

# upload-symbols' "Flatten symbol files" step, run against a download laid out
# the way actions/download-artifact leaves it.
flatten_symbols() {
    python3 - "$YML" > "$BATS_TEST_TMPDIR/flatten.sh" <<'PY'
import sys, yaml
with open(sys.argv[1]) as fh:
    steps = yaml.safe_load(fh)["jobs"]["upload-symbols"]["steps"]
print(next(s["run"] for s in steps if s.get("name") == "Flatten symbol files"))
PY
    local script="$BATS_TEST_TMPDIR/flatten.sh"
    cd "$BATS_TEST_TMPDIR/ws" || return 1
    run env PLATFORMS="$1" bash -e "$script"
    cd - >/dev/null || return 1
}

# plant DIR: an x86-style artifact (drm + fbdev under a shared build/ root)
plant_dual() {
    mkdir -p "$1/x86/bin" "$1/x86-fbdev/bin"
    touch "$1/x86/bin/helix-screen.sym" "$1/x86/bin/helix-screen.debug" \
          "$1/x86-fbdev/bin/helix-screen.sym" "$1/x86-fbdev/bin/helix-screen.debug"
}

@test "symbol flattening handles a lone artifact extracted without its directory" {
    mkdir -p "$BATS_TEST_TMPDIR/ws/artifacts"
    plant_dual "$BATS_TEST_TMPDIR/ws/artifacts"
    flatten_symbols "x86"
    [ "$status" -eq 0 ] || fail "$output"
    local f
    for f in x86.sym x86.debug x86-fbdev.sym x86-fbdev.debug; do
        [ -f "$BATS_TEST_TMPDIR/ws/symbol-files/$f" ] || fail "missing $f: $output"
    done
}

@test "symbol flattening handles per-artifact directories from the full matrix" {
    local ws="$BATS_TEST_TMPDIR/ws"
    mkdir -p "$ws/artifacts/symbols-ad5m"
    plant_dual "$ws/artifacts/symbols-x86"
    touch "$ws/artifacts/symbols-ad5m/helix-screen.sym" "$ws/artifacts/symbols-ad5m/helix-screen.debug"
    flatten_symbols "x86 ad5m"
    [ "$status" -eq 0 ] || fail "$output"
    local f
    for f in x86.sym x86-fbdev.sym ad5m.sym ad5m.debug; do
        [ -f "$ws/symbol-files/$f" ] || fail "missing $f: $output"
    done
}

@test "symbol flattening refuses a flat download when plan built several platforms" {
    mkdir -p "$BATS_TEST_TMPDIR/ws/artifacts"
    plant_dual "$BATS_TEST_TMPDIR/ws/artifacts"
    flatten_symbols "x86 ad5m"
    [ "$status" -ne 0 ]
    [[ "$output" == *"not one platform"* ]] || fail "$output"
}
