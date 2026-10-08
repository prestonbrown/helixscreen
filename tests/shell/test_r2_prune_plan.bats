#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/r2-prune-plan.sh — which versions an R2 prune may delete.
# The rule under test: a version a channel manifest points at is never
# deleted, however many newer prereleases share the store.

load helpers

SCRIPT="scripts/r2-prune-plan.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

BETAS="1.1.0-beta.1
1.1.0-beta.2
1.1.0-beta.3
1.1.0-beta.4
1.1.0-beta.5
1.1.0-beta.6"

@test "stable outranked by six betas survives when pinned" {
    run "$SCRIPT" plan 5 1.0.3 <<< "$BETAS
1.0.3"
    [ "$status" -eq 0 ]
    [ "$output" = "1.1.0-beta.1" ]
}

@test "unpinned old stable is pruned once betas fill retention" {
    run "$SCRIPT" plan 5 1.0.3 <<< "$BETAS
1.0.2
1.0.3"
    [ "$status" -eq 0 ]
    [ "$output" = "1.0.2
1.1.0-beta.1" ]
}

@test "pinned versions do not use up retention slots" {
    run "$SCRIPT" plan 2 1.0.3 <<< "1.0.1
1.0.2
1.0.3
1.1.0-beta.1
1.1.0-beta.2
1.1.0-beta.3"
    [ "$status" -eq 0 ]
    [ "$output" = "1.0.1
1.0.2
1.1.0-beta.1" ]
}

@test "deletes oldest first by semver, not byte order" {
    run "$SCRIPT" plan 1 <<< "1.1.0
1.1.0-beta.10
1.1.0-beta.9"
    [ "$status" -eq 0 ]
    [ "$output" = "1.1.0-beta.9
1.1.0-beta.10" ]
}

@test "within retention deletes nothing" {
    run "$SCRIPT" plan 5 <<< "1.0.2
1.0.3"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "an unrankable version fails instead of guessing" {
    run "$SCRIPT" plan 1 <<< "1.0.3
not-a-version"
    [ "$status" -ne 0 ]
}

@test "non-numeric RETAIN is refused" {
    run "$SCRIPT" plan five <<< "1.0.3"
    [ "$status" -eq 2 ]
}

# pinned reads the bucket through aws; a stub on PATH stands in for it.
stub_aws() {
    STUB="$BATS_TEST_TMPDIR/bin"
    mkdir -p "$STUB"
    cat > "$STUB/aws" <<'EOF'
#!/usr/bin/env bash
case "$3" in
    s3://b/stable/manifest.json) echo '{"version":"1.0.3"}' ;;
    s3://b/beta/manifest.json)   echo '{"version":"1.1.0-beta.6"}' ;;
    s3://b/dev/manifest.json)    [ -n "${AWS_FAIL_DEV:-}" ] && exit 1; echo '{"version":"1.1.0-beta.6"}' ;;
    *) exit 1 ;;
esac
EOF
    chmod +x "$STUB/aws"
}

@test "pinned prints each channel's manifest version" {
    stub_aws
    PATH="$STUB:$PATH" R2_BUCKET=b R2_ENDPOINT=e run "$SCRIPT" pinned
    [ "$status" -eq 0 ]
    [ "$output" = "1.0.3
1.1.0-beta.6
1.1.0-beta.6" ]
}

@test "pinned fails when any channel manifest is unreadable" {
    stub_aws
    PATH="$STUB:$PATH" AWS_FAIL_DEV=1 R2_BUCKET=b R2_ENDPOINT=e run "$SCRIPT" pinned
    [ "$status" -ne 0 ]
}
