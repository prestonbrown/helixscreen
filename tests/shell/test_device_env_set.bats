#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/device-env-set-remote.sh — the half of device-env-set.sh that runs ON
# a printer. It edits a live device's helixscreen.env, which the deploy tars
# deliberately never overwrite, so every failure mode here is one that damages
# config a redeploy will not repair.

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ENVSET="$PWD/scripts/device-env-set-remote.sh"
    WORK="$BATS_TEST_TMPDIR/dev"
    mkdir -p "$WORK"
}

@test "creates the env file and the key when neither exists" {
    run sh "$ENVSET" "$WORK/config/helixscreen.env" HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    run grep -c '^HELIX_REMOTE_CONTROL=1$' "$WORK/config/helixscreen.env"
    [ "$output" -eq 1 ]
}

@test "is a no-op when the key already holds that value" {
    printf 'HELIX_REMOTE_CONTROL=1\n' > "$WORK/a.env"
    run sh "$ENVSET" "$WORK/a.env" HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"already set"* ]]
    # No backup: nothing was modified, so there is no pristine copy to preserve.
    [ ! -f "$WORK/a.env.helix-bak" ]
    run grep -c '^HELIX_REMOTE_CONTROL=' "$WORK/a.env"
    [ "$output" -eq 1 ]
}

@test "flips a commented-out key in place rather than appending a duplicate" {
    # The stock env template ships documented-but-disabled entries. Appending
    # instead of editing leaves two definitions, and which one wins is then an
    # accident of file order rather than a decision.
    printf '# header\n#HELIX_REMOTE_CONTROL=0\nHELIX_NICE=5\n' > "$WORK/b.env"
    run sh "$ENVSET" "$WORK/b.env" HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    run grep -c '^HELIX_REMOTE_CONTROL=1$' "$WORK/b.env"
    [ "$output" -eq 1 ]
    run grep -c 'HELIX_REMOTE_CONTROL' "$WORK/b.env"
    [ "$output" -eq 1 ]
    run grep -c '^HELIX_NICE=5$' "$WORK/b.env"
    [ "$output" -eq 1 ]
}

@test "overwrites a differing value instead of duplicating it" {
    printf 'HELIX_REMOTE_CONTROL=0\n' > "$WORK/c.env"
    run sh "$ENVSET" "$WORK/c.env" HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    run grep -c 'HELIX_REMOTE_CONTROL' "$WORK/c.env"
    [ "$output" -eq 1 ]
    run grep -c '^HELIX_REMOTE_CONTROL=1$' "$WORK/c.env"
    [ "$output" -eq 1 ]
}

@test "edits through a symlink without replacing it" {
    # The Snapmaker U1 install points the in-tree env file at
    # /oem/printer_data/config/helixscreen/helixscreen.env. `sed -i` on a
    # symlink writes a REGULAR FILE over the link, detaching the device from
    # its real config while appearing to succeed.
    mkdir -p "$WORK/real"
    # The key must ALREADY be present: appending with >> follows a symlink
    # harmlessly, so only the sed -i branch exercises the hazard.
    printf 'HELIX_NICE=5\n#HELIX_REMOTE_CONTROL=0\n' > "$WORK/real/helixscreen.env"
    ln -s "$WORK/real/helixscreen.env" "$WORK/link.env"
    run sh "$ENVSET" "$WORK/link.env" HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    [ -L "$WORK/link.env" ]
    run grep -c '^HELIX_REMOTE_CONTROL=1$' "$WORK/real/helixscreen.env"
    [ "$output" -eq 1 ]
    # The backup belongs beside the real file, not beside the link.
    [ -f "$WORK/real/helixscreen.env.helix-bak" ]
    [ ! -e "$WORK/link.env.helix-bak" ]
}

@test "backs the file up once, keeping the pristine original" {
    printf 'HELIX_NICE=5\n' > "$WORK/d.env"
    run sh "$ENVSET" "$WORK/d.env" HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    run cat "$WORK/d.env.helix-bak"
    [ "$output" = "HELIX_NICE=5" ]
    # A second, different edit must NOT re-copy: that would overwrite the
    # pristine backup with an already-modified file.
    run sh "$ENVSET" "$WORK/d.env" HELIX_LOG_LEVEL debug
    [ "$status" -eq 0 ]
    run cat "$WORK/d.env.helix-bak"
    [ "$output" = "HELIX_NICE=5" ]
}

@test "leaves keys that merely share a prefix alone" {
    printf 'HELIX_REMOTE_CONTROL_EXTRA=keep\n' > "$WORK/e.env"
    run sh "$ENVSET" "$WORK/e.env" HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    run grep -c '^HELIX_REMOTE_CONTROL_EXTRA=keep$' "$WORK/e.env"
    [ "$output" -eq 1 ]
    run grep -c '^HELIX_REMOTE_CONTROL=1$' "$WORK/e.env"
    [ "$output" -eq 1 ]
}

@test "expands a leading tilde instead of making a directory named ~" {
    # PI_DEPLOY_DIR is `~/helixscreen`, and the path reaches the device as a
    # positional parameter, where the shell never expands a tilde. Unhandled,
    # this writes into a literal "~" directory and reports success while the
    # real env file stays untouched.
    fake="$BATS_TEST_TMPDIR/home"
    mkdir -p "$fake"
    run env HOME="$fake" sh "$ENVSET" '~/helixscreen/config/helixscreen.env' \
        HELIX_REMOTE_CONTROL 1
    [ "$status" -eq 0 ]
    [ ! -e "$fake/~" ]
    run grep -c '^HELIX_REMOTE_CONTROL=1$' "$fake/helixscreen/config/helixscreen.env"
    [ "$output" -eq 1 ]
}

@test "the outer script refuses a malformed key before touching ssh" {
    # Invoked as the deploy does (via its shebang) — running it under `sh` would
    # die on bash's `set -o pipefail` and exit 2 for the wrong reason.
    run bash -c "'$PWD/scripts/device-env-set.sh' nosuchhost /tmp/x.env 'BAD KEY;rm' 1 2>&1"
    [ "$status" -eq 2 ]
    [[ "$output" == *"malformed key"* ]]
}
