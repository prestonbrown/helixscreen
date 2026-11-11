#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/distribution_partners.py: the release check fails a release missing an
# asset a partner fetches by name, and the INSTALLER.md table matches the data (#1911).

load helpers

SCRIPT="scripts/distribution_partners.py"
DOC="docs/devel/INSTALLER.md"

# Every name a partner fetches for tag v1.2.3, written out rather than read from the data,
# so dropping or renaming an entry in scripts/distribution_partners.json fails here too.
FULL_LISTING="install.sh
helixscreen-ad5m.zip
helixscreen-ad5m-v1.2.3.tar.gz
helixscreen-ad5x.zip
helixscreen-ad5x-v1.2.3.tar.gz
helixscreen-cc1.zip
helixscreen-cc1-v1.2.3.tar.gz
helixscreen-snapmaker-u1-v1.2.3.tar.gz"

setup() {
    TEST_DIR="$(mktemp -d)"
    REL="$TEST_DIR/release-files"
    mkdir -p "$REL"
}

teardown() {
    rm -rf "$TEST_DIR"
}

# Create one empty file per listed name, plus assets no partner fetches.
listing() {
    while IFS= read -r name; do
        [ -n "$name" ] && : > "$REL/$name"
    done <<< "$1"
    : > "$REL/helixscreen-pi.zip"
    : > "$REL/helixscreen-pi-v1.2.3.tar.gz"
}

@test "a release carrying every partner asset passes" {
    listing "$FULL_LISTING"
    run python3 "$SCRIPT" check "$REL" --tag v1.2.3
    [ "$status" -eq 0 ]
    [[ "$output" == *"All 8 partner assets present"* ]]
}

@test "a release missing the Z-Mod zip fails and names Z-Mod" {
    listing "$(grep -v '^helixscreen-ad5x.zip$' <<< "$FULL_LISTING")"
    run python3 "$SCRIPT" check "$REL" --tag v1.2.3
    [ "$status" -eq 1 ]
    [[ "$output" == *"::error::release is missing helixscreen-ad5x.zip, which Z-Mod, Forge-X fetches"* ]]
}

@test "the PAXX archive is required under the release's own tag" {
    listing "$(sed 's/snapmaker-u1-v1.2.3/snapmaker-u1-v1.2.2/' <<< "$FULL_LISTING")"
    run python3 "$SCRIPT" check "$REL" --tag v1.2.3
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing helixscreen-snapmaker-u1-v1.2.3.tar.gz, which PAXX"* ]]
}

@test "a dry run of a subset requires only what it built, plus install.sh" {
    listing "install.sh"
    run python3 "$SCRIPT" check "$REL" --tag v0.0.0-dryrun.1 --platforms "pi x86"
    [ "$status" -eq 0 ]

    rm "$REL/install.sh"
    run python3 "$SCRIPT" check "$REL" --tag v0.0.0-dryrun.1 --platforms "pi x86"
    [ "$status" -eq 1 ]
    contains "missing install.sh" "$output"

    listing "install.sh"
    run python3 "$SCRIPT" check "$REL" --tag v0.0.0-dryrun.1 --platforms "mips"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing helixscreen-ad5x.zip"* ]]
}

@test "the INSTALLER.md partner table matches the data" {
    run python3 "$SCRIPT" doc "$DOC"
    [ "$status" -eq 0 ]
}

@test "a hand edit to the table fails the doc check, and --write repairs it" {
    cp "$DOC" "$TEST_DIR/doc.md"
    sed -i 's/| \*\*Z-Mod\*\*/| **ZMod**/' "$TEST_DIR/doc.md"
    run python3 "$SCRIPT" doc "$TEST_DIR/doc.md"
    [ "$status" -eq 1 ]
    contains "does not match scripts/distribution_partners.json" "$output"

    run python3 "$SCRIPT" doc "$TEST_DIR/doc.md" --write
    [ "$status" -eq 0 ]
    cmp "$DOC" "$TEST_DIR/doc.md"
}

@test "every partner asset is one the release workflow uploads" {
    # built_by names a release.yml matrix platform, or the installer artifact.
    run python3 -c '
import json, re, sys
wf = open(".github/workflows/release.yml").read()
plats = set(re.findall(r"\{\"platform\": \"([^\"]+)\"", wf))
bad = [f["asset"] + " <- " + f["built_by"]
       for p in json.load(open("scripts/distribution_partners.json"))["partners"]
       for f in p["fetches"] if f["built_by"] != "installer" and f["built_by"] not in plats]
print("\n".join(bad)); sys.exit(1 if bad or not plats else 0)'
    [ "$status" -eq 0 ]
}

@test "release.yml runs the check on release-files before the GitHub release is created" {
    run python3 -c '
import sys
wf = open(".github/workflows/release.yml").read()
check = wf.find("scripts/distribution_partners.py check release-files --tag \"$RELEASE_TAG\" --platforms \"$BUILT_PLATFORMS\"")
publish = wf.find("uses: softprops/action-gh-release")
sys.exit(0 if 0 <= check < publish and "BUILT_PLATFORMS: ${{ needs.plan.outputs.platforms }}" in wf else 1)'
    [ "$status" -eq 0 ]
}
