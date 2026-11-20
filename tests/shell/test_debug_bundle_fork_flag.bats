#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/debug-bundle.sh flags a bundle whose binary was built from a commit
# this repo does not have (a fork build), so triage sees it on the first line.
# curl is stubbed to serve a fixture bundle; git runs against a scratch repo.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # A copy of the script inside a scratch repo, so "in our repo" means a
    # commit the test made, not whatever this checkout happens to hold.
    export FAKE_ROOT="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$FAKE_ROOT/scripts"
    cp "$WORKTREE_ROOT/scripts/debug-bundle.sh" "$FAKE_ROOT/scripts/"
    git -C "$FAKE_ROOT" init -q
    git -C "$FAKE_ROOT" -c user.email=t@t -c user.name=t commit -q --allow-empty -m base
    OUR_HASH=$(git -C "$FAKE_ROOT" rev-parse --short=7 HEAD)

    export HELIX_ADMIN_KEY=test
    export BUNDLE_FIXTURE="$BATS_TEST_TMPDIR/bundle.json"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/curl" <<'STUB'
#!/bin/sh
while [ $# -gt 0 ]; do
    case "$1" in
        -o) cp "$BUNDLE_FIXTURE" "$2"; shift 2 ;;
        *) shift ;;
    esac
done
printf 200
STUB
    chmod +x "$BATS_TEST_TMPDIR/bin/curl"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

bundle_with_log_line() {
    printf '{"version":"0.99.118","printer":{},"system":{},"log_tail":["[Application] HelixScreen 0.99.118 (%s)"]}' \
        "$1" > "$BUNDLE_FIXTURE"
}

@test "debug-bundle summary: a log-line hash the repo lacks is flagged as a fork build" {
    bundle_with_log_line b1bf021
    run "$FAKE_ROOT/scripts/debug-bundle.sh" CODE --summary
    [ "$status" -eq 0 ]
    contains "FORK BUILD: commit b1bf021 is not in our repo" "$output"
}

@test "debug-bundle summary: our own commit is not flagged" {
    bundle_with_log_line "$OUR_HASH"
    run "$FAKE_ROOT/scripts/debug-bundle.sh" CODE --summary
    [ "$status" -eq 0 ]
    contains "v0.99.118" "$output"
    lacks "FORK BUILD" "$output"
}

@test "debug-bundle pretty: the git_hash field wins over the log line" {
    printf '{"version":"1.2.0","git_hash":"deadbee","printer":{},"system":{},"settings":{},"log_tail":["HelixScreen 1.2.0 (%s)"]}' \
        "$OUR_HASH" > "$BUNDLE_FIXTURE"
    run "$FAKE_ROOT/scripts/debug-bundle.sh" CODE
    [ "$status" -eq 0 ]
    contains "FORK BUILD: commit deadbee is not in our repo" "$output"
}

@test "debug-bundle summary: no hash anywhere, or 'unknown', is not flagged" {
    printf '{"version":"1.2.0","git_hash":"unknown","printer":{},"system":{}}' > "$BUNDLE_FIXTURE"
    run "$FAKE_ROOT/scripts/debug-bundle.sh" CODE --summary
    [ "$status" -eq 0 ]
    contains "v1.2.0" "$output"
    lacks "FORK BUILD" "$output"
}
