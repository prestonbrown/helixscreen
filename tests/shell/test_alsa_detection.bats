#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/lib/installer/audio.sh —
# ALSA "default" auto-configuration for boards with no card 0 (e.g. Pi driving
# an HDMI-audio screen like the BTT HDMI5, where the only outputs are
# vc4hdmi0/vc4hdmi1 at indices 1/2).

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_AUDIO_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/audio.sh"

    export SUDO=""
    export ASOUND_PROC_DIR="$BATS_TEST_TMPDIR/proc/asound"
    export ASOUND_CONF="$BATS_TEST_TMPDIR/etc/asound.conf"
    mkdir -p "$BATS_TEST_TMPDIR/etc"
}

# --- Fixture helpers -------------------------------------------------------

# make_card <index> <id> [--playback] [--eld-connected | --eld-disconnected]
make_card() {
    idx="$1"; id="$2"; shift 2
    dir="$ASOUND_PROC_DIR/card${idx}"
    mkdir -p "$dir"
    printf '%s\n' "$id" > "$dir/id"
    while [ $# -gt 0 ]; do
        case "$1" in
            --playback) mkdir -p "$dir/pcm0p" ;;
            --eld-connected)
                printf 'monitor_present\t\t1\neld_valid\t\t1\nmonitor_name\t\tHDMI5\n' \
                    > "$dir/eld#0.0" ;;
            --eld-disconnected)
                printf 'monitor_present\t\t0\neld_valid\t\t0\n' > "$dir/eld#0.0" ;;
        esac
        shift
    done
}

# =============================================================================
# alsa_pick_default_card: the pure detection core
# =============================================================================

@test "no /proc/asound → picks nothing" {
    run alsa_pick_default_card
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "card 0 present → picks nothing (stock default works)" {
    make_card 0 Headphones --playback
    make_card 1 vc4hdmi0 --playback --eld-connected
    run alsa_pick_default_card
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "no card 0, single HDMI card → picks it" {
    make_card 1 vc4hdmi0 --playback --eld-connected
    run alsa_pick_default_card
    [ "$status" -eq 0 ]
    [ "$output" = "vc4hdmi0" ]
}

@test "no card 0, two HDMI cards → picks the connected one (the reported BTT HDMI5 case)" {
    make_card 1 vc4hdmi0 --playback --eld-disconnected
    make_card 2 vc4hdmi1 --playback --eld-connected
    run alsa_pick_default_card
    [ "$status" -eq 0 ]
    [ "$output" = "vc4hdmi1" ]
}

@test "no card 0, no ELD info anywhere → falls back to lowest-indexed playback card" {
    make_card 1 sndrpiusbdac --playback
    make_card 2 vc4hdmi1 --playback
    run alsa_pick_default_card
    [ "$status" -eq 0 ]
    [ "$output" = "sndrpiusbdac" ]
}

@test "cards without a playback PCM are skipped" {
    make_card 1 vc4hdmi0            # capture-only / no pcm*p
    make_card 2 vc4hdmi1 --playback --eld-connected
    run alsa_pick_default_card
    [ "$status" -eq 0 ]
    [ "$output" = "vc4hdmi1" ]
}

# =============================================================================
# alsa_asound_conf_body: emitted config content
# =============================================================================

@test "conf body routes default to the given card" {
    run alsa_asound_conf_body vc4hdmi1
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'pcm.!default'
    echo "$output" | grep -q 'ctl.!default'
    echo "$output" | grep -q 'card "vc4hdmi1"'
    echo "$output" | grep -q 'device 0'
}

# =============================================================================
# configure_alsa_default: orchestration + guards
# =============================================================================

@test "writes asound.conf when no card 0 and a connected card exists" {
    make_card 1 vc4hdmi0 --playback --eld-disconnected
    make_card 2 vc4hdmi1 --playback --eld-connected
    run configure_alsa_default
    [ "$status" -eq 0 ]
    [ -f "$ASOUND_CONF" ]
    grep -q 'card "vc4hdmi1"' "$ASOUND_CONF"
}

@test "does NOT write when card 0 exists" {
    make_card 0 Headphones --playback
    run configure_alsa_default
    [ "$status" -eq 0 ]
    [ ! -e "$ASOUND_CONF" ]
}

@test "does NOT clobber an existing asound.conf" {
    make_card 1 vc4hdmi0 --playback --eld-connected
    printf 'pcm.!default { type hw; card SomethingElse }\n' > "$ASOUND_CONF"
    run configure_alsa_default
    [ "$status" -eq 0 ]
    grep -q 'SomethingElse' "$ASOUND_CONF"
    ! grep -q 'vc4hdmi0' "$ASOUND_CONF"
}

@test "does NOT write when ALSA is absent" {
    run configure_alsa_default
    [ "$status" -eq 0 ]
    [ ! -e "$ASOUND_CONF" ]
}
