#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/release-moonraker-asset.sh: the zip copy an old Moonraker downloads
# as a release's assets[0], and release.yml's use of it (#2147).

load helpers

SCRIPT="scripts/release-moonraker-asset.sh"
YML=".github/workflows/release.yml"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REL="$BATS_TEST_TMPDIR/release-files"
    mkdir -p "$REL/symbols"
    # The shape of a real release: every platform's tarball and zip, the
    # installer and the prefixed symbol maps.
    for f in helixscreen-ad5m-v1.2.0.tar.gz helixscreen-ad5m.zip helixscreen-ad5x.zip \
             helixscreen-pi-v1.2.0.tar.gz helixscreen-pi.zip install.sh symbols-ad5m.sym.zst; do
        printf '%s\n' "$f" > "$REL/$f"
    done
}

@test "the copy's name is the one the installer arms an old Moonraker for" {
    local want
    want=$(. scripts/lib/installer/platform.sh >/dev/null 2>&1 && helix_moonraker_first_asset)
    run sh "$SCRIPT" name
    [ "$status" -eq 0 ]
    [ "$output" = "$want" ] || fail "script '$output', installer '$want'"
}

@test "stage copies the platform's zip under the first-sorting name" {
    run sh "$SCRIPT" stage "$REL"
    [ "$status" -eq 0 ] || fail "$output"
    cmp -s "$REL/helixscreen-ad5m.zip" "$REL/helixscreen-ad5m-moonraker.zip" \
        || fail "the copy differs from helixscreen-ad5m.zip"
    [ -f "$REL/helixscreen-ad5m.zip" ] || fail "the original zip is gone"
}

@test "stage fails when another asset would sort first" {
    printf 'x\n' > "$REL/helixscreen-ad5m-a.zip"
    run sh "$SCRIPT" stage "$REL"
    [ "$status" -ne 0 ] || fail "a name sorting before the copy was accepted"
    [[ "$output" == *"helixscreen-ad5m-a.zip"* ]] || fail "the error does not name it: $output"
}

@test "stage fails when the platform's zip is missing" {
    rm -f "$REL/helixscreen-ad5m.zip"
    run sh "$SCRIPT" stage "$REL"
    [ "$status" -ne 0 ] || fail "staged without a source zip"
}

@test "check reads the first name of the API's listing" {
    run sh "$SCRIPT" check <<'EOF'
helixscreen-ad5m-moonraker.zip
helixscreen-ad5m-v1.2.0.tar.gz
EOF
    [ "$status" -eq 0 ] || fail "$output"

    run sh "$SCRIPT" check <<'EOF'
helixscreen-ad5m-v1.2.0.tar.gz
helixscreen-ad5m-moonraker.zip
EOF
    [ "$status" -ne 0 ] || fail "a listing led by the tarball passed"
}

@test "release.yml stages the copy and checks the published order" {
    run python3 - "$YML" <<'PY'
import sys, yaml
steps = yaml.safe_load(open(sys.argv[1]))["jobs"]["release"]["steps"]
names = [s.get("name", "") for s in steps]
def idx(pred):
    return next(i for i, s in enumerate(steps) if pred(s.get("run", "")))
stage = idx(lambda r: "release-moonraker-asset.sh stage release-files" in r)
check = idx(lambda r: "release-moonraker-asset.sh check" in r)
publish = names.index("Create GitHub Release")
dryrun = names.index("Upload would-be release assets (dry run)")
syms = names.index("Prepare symbol files for release")
bad = []
if not (syms < stage < publish < check):
    bad.append(f"order: symbols {syms}, stage {stage}, publish {publish}, check {check}")
if stage > dryrun:
    bad.append("the dry-run artifact is uploaded before the copy is staged")
if steps[check].get("if") != "env.RELEASE_MODE == 'publish'":
    bad.append("the published-order check is not gated on publish")
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "$output"
}
