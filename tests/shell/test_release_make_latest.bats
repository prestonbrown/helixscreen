#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/release-make-latest.sh, which decides whether a release
# becomes GitHub's "latest". Moonraker's stable channel installs whatever
# releases/latest names, downgrades included (helixscreen#2147), so "latest"
# must never move backwards.

load helpers

SCRIPT="scripts/release-make-latest.sh"
WORKFLOW=".github/workflows/release.yml"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "release-make-latest.sh passes shellcheck" {
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck "$SCRIPT"
}

@test "a release newer than latest becomes latest" {
    run "$SCRIPT" v1.1.0 false v1.0.10
    [ "$status" -eq 0 ]
    [ "$output" = "true" ]
}

@test "a release on an older line does not take latest back" {
    run "$SCRIPT" v1.0.11 false v1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "false" ]
}

@test "a re-run of the latest release keeps it latest" {
    # make_latest=false on the current latest could demote it.
    run "$SCRIPT" v1.1.0 false v1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "true" ]
}

@test "with no latest release yet, a stable release becomes latest" {
    run "$SCRIPT" v1.1.0 false ""
    [ "$status" -eq 0 ]
    [ "$output" = "true" ]
    run "$SCRIPT" v1.1.0 false
    [ "$output" = "true" ]
}

@test "a prerelease never becomes latest, even above the current one" {
    run "$SCRIPT" v1.2.0-beta.1 true v1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "false" ]
    run "$SCRIPT" v1.2.0-beta.1 true ""
    [ "$output" = "false" ]
}

@test "precedence is semver, not string order" {
    # String order ranks 1.0.9 above 1.0.10.
    run "$SCRIPT" v1.0.10 false v1.0.9
    [ "$output" = "true" ]
    # A stable release outranks its own prerelease.
    run "$SCRIPT" v1.1.0 false v1.1.0-rc.4
    [ "$output" = "true" ]
}

@test "a latest tag that cannot be ranked is an error, not a verdict" {
    run "$SCRIPT" v1.1.0 false nightly
    [ "$status" -ne 0 ]
    [[ "$output" != "true" && "$output" != "false" ]]
}

@test "an unrecognised prerelease value is an error" {
    run "$SCRIPT" v1.1.0 yes v1.0.10
    [ "$status" -eq 2 ]
}

@test "the release step takes make_latest from the decision" {
    grep -q 'scripts/release-make-latest.sh' "$WORKFLOW"
    grep -qE '^ +make_latest: \$\{\{ steps\.latest\.outputs\.make_latest \}\}' "$WORKFLOW"
}

@test "the published release takes make_latest from the decision" {
    grep -qE '^ +make_latest: \$\{\{ steps\.latest\.outputs\.make_latest \}\}' "$WORKFLOW"
    grep -qE -- '--latest="\$MAKE_LATEST"' "$WORKFLOW"
}

@test "nothing public happens before the release-signing approval" {
    run python3 - "$WORKFLOW" <<'PY'
import sys, yaml
jobs = yaml.safe_load(open(sys.argv[1]))["jobs"]
problems = []
create = [s for s in jobs["release"]["steps"] if "softprops/action-gh-release" in str(s.get("uses", ""))]
if not create or create[0]["with"].get("draft") is not True:
    problems.append("Create Release does not leave a draft")
if "upload-r2" not in (jobs["publish-release"].get("needs") or []):
    problems.append("publish-release does not wait on upload-r2")
for job in ("verify-release-assets", "publish-android", "notify-website"):
    if "publish-release" not in (jobs[job].get("needs") or []):
        problems.append(job + " does not wait on publish-release")
if any("Discord" in str(s.get("name", "")) for s in jobs["release"]["steps"]):
    problems.append("Create Release still posts to Discord")
for p in problems:
    print(p, file=sys.stderr)
sys.exit(1 if problems else 0)
PY
    [ "$status" -eq 0 ]
}
