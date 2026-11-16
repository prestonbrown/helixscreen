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
    mkdir -p "$REL"
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
    run python3 "$SCRIPT" check "$REL" --tag v1.2.0-beta.1 --platforms "pi x86"
    [ "$status" -eq 0 ]

    rm "$REL/install.sh"
    run python3 "$SCRIPT" check "$REL" --tag v1.2.0-beta.1 --platforms "pi x86"
    [ "$status" -eq 1 ]
    contains "missing install.sh" "$output"

    listing "install.sh"
    run python3 "$SCRIPT" check "$REL" --tag v1.2.0-beta.1 --platforms "mips"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing helixscreen-ad5x.zip"* ]]
}

@test "the INSTALLER.md partner table matches the data" {
    run python3 "$SCRIPT" doc "$DOC"
    [ "$status" -eq 0 ]
}

@test "every partner names the maintainer as owner, and a partner without one fails to load" {
    run python3 "$SCRIPT" table
    [ "$status" -eq 0 ]
    contains "| Incumbent screen | Owner | Contact |" "${lines[0]}"
    rows=$(grep -c '^| \*\*' <<< "$output")
    [ "$rows" -eq "$(grep -c '| prestonbrown | ' <<< "$output")" ]
    [ "$rows" -ge 7 ]

    python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
del d["partners"][0]["owner"]
json.dump(d, open(sys.argv[2], "w"))' scripts/distribution_partners.json "$TEST_DIR/data.json"
    run python3 "$SCRIPT" --data "$TEST_DIR/data.json" table
    [ "$status" -ne 0 ]
    contains "has no 'owner'" "$output"
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

# Runs the workflow step's own script, as the release job would, in a scratch tree whose
# archives are named from VERSION.txt (mk/cross.mk), never from the run's tag.
run_partner_step() {
    local mode=$1 tag=$2
    local tree="$TEST_DIR/job"
    mkdir -p "$tree"
    ln -s "$PWD/scripts" "$tree/scripts"
    echo "1.2.0-beta.1" > "$tree/VERSION.txt"
    mv "$REL" "$tree/release-files"
    python3 -c '
import sys, yaml
wf = yaml.safe_load(open(".github/workflows/release.yml"))
steps = wf["jobs"]["release"]["steps"]
names = [s.get("name", "") for s in steps]
i = names.index("Verify distribution partner assets are present")
publish = next(n for n, s in enumerate(steps) if "softprops/action-gh-release" in s.get("uses", ""))
assert i < publish, "the check runs after the GitHub release is created"
assert steps[i]["env"]["BUILT_PLATFORMS"] == "${{ needs.plan.outputs.platforms }}"
print(steps[i]["run"])' > "$TEST_DIR/step.sh" || return 1
    (cd "$tree" && RELEASE_MODE=$mode RELEASE_TAG=$tag BUILT_PLATFORMS="ad5m mips cc1 snapmaker-u1" \
        bash "$TEST_DIR/step.sh")
}

@test "the release step checks a dry run under VERSION.txt and a tag push under its tag" {
    listing "$(sed 's/v1.2.3/v1.2.0-beta.1/' <<< "$FULL_LISTING")"
    run run_partner_step dryrun v0.0.0-dryrun.7
    [ "$status" -eq 0 ] || fail "dry run: $output"

    listing "$(sed 's/v1.2.3/v1.2.0-beta.1/' <<< "$FULL_LISTING")"
    rm -rf "$TEST_DIR/job"
    run run_partner_step publish v1.2.0-beta.1
    [ "$status" -eq 0 ] || fail "tag push: $output"

    listing "$(sed 's/v1.2.3/v1.2.0-beta.1/' <<< "$FULL_LISTING")"
    rm -rf "$TEST_DIR/job"
    run run_partner_step publish v1.2.0-beta.2
    [ "$status" -eq 1 ] || fail "a tag VERSION.txt disagrees with passed: $output"
    contains "missing helixscreen-snapmaker-u1-v1.2.0-beta.2.tar.gz" "$output"
}
