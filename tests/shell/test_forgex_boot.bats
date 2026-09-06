#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for ForgeX boot sequence coordination:
# - lib/installer/main.sh applies all three screen.sh patches (not just backlight)
# - helixscreen.init calls platform_wait_for_boot_complete with skip mechanism
# - hooks-ad5m-forgex.sh implements platform_wait_for_boot_complete
# - Bundled install.sh stays in sync with modular installer
# - Uninstall properly reverses all patches

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    # helix-launcher.sh runs `killall helix-watchdog helix-screen ...`, which is
    # not scoped to this test. bats runs FILES in parallel, so an unmocked run
    # reaches across and kills the long-lived instance test_headless_display.bats
    # is driving - it dies cleanly mid-startup and that test fails for no reason
    # of its own.
    mock_command_script "killall" 'exit 0'
}

# --- install-dev.sh must apply all ForgeX patches ---

# Orchestration lives in lib/installer/main.sh now (sourced by both
# install-dev.sh and the bundled install.sh).
@test "main.sh calls patch_forgex_screen_sh" {
    grep -q 'patch_forgex_screen_sh' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

@test "main.sh calls patch_forgex_screen_drawing" {
    grep -q 'patch_forgex_screen_drawing' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

@test "main.sh calls install_forgex_logged_wrapper" {
    grep -q 'install_forgex_logged_wrapper' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

# --- Bundled install.sh parity ---

@test "bundled install.sh calls all three ForgeX patches" {
    grep -q 'patch_forgex_screen_sh' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q 'patch_forgex_screen_drawing' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q 'install_forgex_logged_wrapper' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundle-installer.sh sources main.sh which holds ForgeX patches" {
    grep -q 'main\.sh' "$WORKTREE_ROOT/scripts/bundle-installer.sh"
    grep -q 'patch_forgex_screen_sh' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    grep -q 'patch_forgex_screen_drawing' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    grep -q 'install_forgex_logged_wrapper' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

# --- helixscreen.init boot complete hook ---

@test "helixscreen.init defines platform_wait_for_boot_complete default" {
    grep -q 'platform_wait_for_boot_complete()' "$WORKTREE_ROOT/config/helixscreen.init"
}

@test "helixscreen.init calls platform_wait_for_boot_complete in start" {
    # Must be called AFTER platform_wait_for_services, before launching helix-screen
    grep -q 'platform_wait_for_boot_complete' "$WORKTREE_ROOT/config/helixscreen.init"
    # Verify ordering: wait_for_services comes before wait_for_boot_complete
    local services_line boot_line
    services_line=$(grep -n 'platform_wait_for_services' "$WORKTREE_ROOT/config/helixscreen.init" | head -1 | cut -d: -f1)
    boot_line=$(grep -n 'platform_wait_for_boot_complete' "$WORKTREE_ROOT/config/helixscreen.init" | tail -1 | cut -d: -f1)
    [ "$boot_line" -gt "$services_line" ]
}

@test "helixscreen.init has skip mechanism for boot wait" {
    # HELIX_NO_BOOT_WAIT must be checkable to bypass the wait (for debugging)
    grep -q 'HELIX_NO_BOOT_WAIT' "$WORKTREE_ROOT/config/helixscreen.init"
}

# --- hooks-ad5m-forgex.sh boot complete implementation ---

@test "ForgeX hooks implement platform_wait_for_boot_complete" {
    grep -q 'platform_wait_for_boot_complete()' "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
}

@test "ForgeX boot complete checks for S99root process" {
    grep -q 'S99root' "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
}

@test "ForgeX boot complete has timeout" {
    # Must not hang forever waiting for S99root
    grep -A20 'platform_wait_for_boot_complete()' "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh" | grep -q 'timeout'
}

# --- ForgeX boot complete functional tests ---

@test "platform_wait_for_boot_complete returns immediately when S99root missing" {
    # Source the hooks file
    . "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"

    # No S99root file exists in the test env → should return 0 immediately
    run platform_wait_for_boot_complete
    [ "$status" -eq 0 ]
}

@test "platform_wait_for_boot_complete returns when no matching process" {
    # Create a fake S99root file so the function doesn't bail early
    mkdir -p "$BATS_TEST_TMPDIR/opt/config/mod/.root"
    touch "$BATS_TEST_TMPDIR/opt/config/mod/.root/S99root"

    # Create a test version with configurable path and short timeout
    cat > "$BATS_TEST_TMPDIR/test_hook.sh" << 'EOF'
platform_wait_for_boot_complete() {
    local s99root="$1"
    if [ ! -f "$s99root" ]; then
        return 0
    fi
    echo "Waiting for ForgeX boot to complete..."
    local timeout=3
    local waited=0
    while [ "$waited" -lt "$timeout" ]; do
        if ! ps w 2>/dev/null | grep -v grep | grep -q "S99root"; then
            echo "ForgeX boot complete after ${waited}s"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    echo "Warning: ForgeX boot still running after ${timeout}s, starting anyway"
    return 1
}
EOF
    . "$BATS_TEST_TMPDIR/test_hook.sh"

    # No S99root process running → should return 0 quickly
    run platform_wait_for_boot_complete "$BATS_TEST_TMPDIR/opt/config/mod/.root/S99root"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ForgeX boot complete"* ]]
}

# --- Patch content verification ---

# The draw-command list lives in FORGEX_DRAW_COMMANDS rather than inline in the
# patch function, because which commands exist is per-firmware: 1.4.0/1.4.1 have
# draw_loading + draw_splash + boot_message, and 1.4.2 drops the first and third
# and adds splash_start. Assert the declaration, not a fixed window after the
# function - see the note above assert_consults_flag.
forgex_draw_commands_decl() {
    grep '^FORGEX_DRAW_COMMANDS=' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh"
}

@test "FORGEX_DRAW_COMMANDS covers the 1.4.0/1.4.1 draw commands" {
    local decl
    decl=$(forgex_draw_commands_decl)
    [ -n "$decl" ] || fail "FORGEX_DRAW_COMMANDS is not declared"
    printf '%s\n' "$decl" | grep -q 'draw_splash'  || fail "draw_splash missing"
    printf '%s\n' "$decl" | grep -q 'draw_loading' || fail "draw_loading missing"
    printf '%s\n' "$decl" | grep -q 'boot_message' || fail "boot_message missing"
}

@test "FORGEX_DRAW_COMMANDS covers the 1.4.2 splash entry point" {
    # S00init runs `screen.sh splash_start`, which owns /dev/fb0 for the boot.
    forgex_draw_commands_decl | grep -q 'splash_start' || fail "splash_start missing"
}

@test "FORGEX_DRAW_COMMANDS does not block splash_stop" {
    # Guarding splash_stop would strand the splash process on screen forever.
    refute_sh "grep '^FORGEX_DRAW_COMMANDS=' '$WORKTREE_ROOT/scripts/lib/installer/forgex.sh' | grep -q splash_stop"
}

@test "patch_forgex_screen_drawing verifies every command it set out to guard" {
    # A whole-file grep for helixscreen_active cannot do this: one successful
    # insertion would vouch for every label that silently failed to match.
    local body
    body=$(awk '/^patch_forgex_screen_drawing\(\)/,/^}/' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh")
    [ -n "$body" ] || fail "patch_forgex_screen_drawing() not found in forgex.sh"
    printf '%s\n' "$body" | grep -q 'forgex_case_is_guarded' \
        || fail "patch does not verify per-command; a partial application would report success"
}

@test "logged wrapper strips --send-to-screen when flag exists" {
    grep -A30 'install_forgex_logged_wrapper' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh" | grep -q '\-\-send-to-screen'
}

@test "logged wrapper preserves argument quoting" {
    # The wrapper must use "$@" not unquoted $args to preserve spaces in arguments
    # Active path should use 'set --' + 'set -- "$@" "$arg"' pattern or similar
    # Check that the exec line in the active path quotes its arguments
    local wrapper_section
    wrapper_section=$(sed -n '/WRAPPER_EOF/,/WRAPPER_EOF/p' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh")
    # The exec in the helixscreen_active branch must quote args
    echo "$wrapper_section" | grep -q 'exec.*/logged-real "\$@"'
}

# --- Uninstall parity ---

@test "uninstall_forgex calls unpatch_forgex_screen_drawing" {
    awk '/^uninstall_forgex\(\)/,/^}/' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh" \
        | grep -q 'unpatch_forgex_screen_drawing'
}

@test "unpatch_forgex_screen_drawing function exists" {
    grep -q 'unpatch_forgex_screen_drawing()' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh"
}

@test "unpatch_forgex_screen_drawing matches patch comment string" {
    # The unpatch awk must match the EXACT comment string the patch inserts
    local patch_comment unpatch_comment
    patch_comment=$(awk '/^patch_forgex_screen_drawing\(\)/,/^}/' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh" | grep '# Skip when')
    unpatch_comment=$(awk '/^unpatch_forgex_screen_drawing\(\)/,/^}/' "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh" | grep '# Skip when')
    # Both must contain the same identifying string
    echo "$patch_comment" | grep -q 'Skip when HelixScreen'
    echo "$unpatch_comment" | grep -q 'Skip when HelixScreen'
}

# --- Bundled install.sh uninstall parity ---

@test "bundled install.sh has unpatch_forgex_screen_drawing function" {
    grep -q 'unpatch_forgex_screen_drawing()' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled install.sh uninstall_forgex calls unpatch_forgex_screen_drawing" {
    awk '/^uninstall_forgex\(\)/,/^}/' "$WORKTREE_ROOT/scripts/install.sh" \
        | grep -q 'unpatch_forgex_screen_drawing'
}

@test "bundled install.sh logged wrapper uses string accumulation for args" {
    # The active path must use $args (string accumulation) — the old set-- pattern
    # was broken because set-- clears positional params before the loop iterates
    sed -n '/WRAPPER_EOF/,/WRAPPER_EOF/p' "$WORKTREE_ROOT/scripts/install.sh" | grep 'exec.*/logged-real' | head -1 | grep -q '\$args'
}

@test "uninstall.sh calls unpatch_forgex_screen_drawing" {
    grep -q 'unpatch_forgex_screen_drawing' "$WORKTREE_ROOT/scripts/uninstall.sh"
}

# --- helixscreen_active flag coordination ---

@test "ForgeX hooks create helixscreen_active flag in pre_start" {
    # Extract the full platform_pre_start() function body, then check for the flag.
    # Don't use grep -A<N>: the function body grows over time and any fixed
    # context window goes stale (broke on 481a2f176 logging refactor).
    awk '/^platform_pre_start\(\)/,/^}/' "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh" | grep -q 'helixscreen_active'
}

@test "ForgeX hooks remove helixscreen_active flag in post_stop" {
    awk '/^platform_post_stop\(\)/,/^}/' "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh" | grep -q 'helixscreen_active'
}

# Each patch site must consult the flag itself. A whole-file occurrence count
# cannot say that: forgex.sh carries 16 mentions (3 of them comments), so the
# old `-ge 8` threshold survived the removal of an entire patch site.
#
# awk pulls the function body rather than grep -A<N>, for the reason spelled out
# at the pre_start test above: a fixed context window goes stale as bodies grow.
assert_consults_flag() {
    local fn=$1
    local body
    body=$(awk "/^${fn}\\(\\)/,/^\\}/" "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh")
    [ -n "$body" ] || fail "${fn}() not found in forgex.sh — this guard is watching nothing"
    printf '%s\n' "$body" | grep -q 'helixscreen_active' \
        || fail "${fn}() does not consult /tmp/helixscreen_active — stock firmware will draw over HelixScreen"
}

@test "patch_forgex_screen_sh checks the helixscreen_active flag" {
    assert_consults_flag patch_forgex_screen_sh
}

@test "patch_forgex_screen_drawing checks the helixscreen_active flag" {
    assert_consults_flag patch_forgex_screen_drawing
}

@test "install_forgex_logged_wrapper checks the helixscreen_active flag" {
    assert_consults_flag install_forgex_logged_wrapper
}

@test "both unpatch functions still recognise the helixscreen_active marker" {
    # An unpatch that stops matching the marker leaves the patch applied
    # forever. Recognition now lives in the shared strip helper (armed on the
    # marker comment, confirmed by the flag's if-line) and in
    # forgex_case_is_guarded, so each unpatch must route through the helper -
    # a hand-rolled remover in a function body is how the cross-family
    # corruption came back.
    assert_consults_flag forgex_strip_guard_blocks
    local body
    body=$(awk '/^unpatch_forgex_screen_sh\(\)/,/^}/' \
        "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh")
    printf '%s\n' "$body" | grep -q 'forgex_strip_guard_blocks' \
        || fail "unpatch_forgex_screen_sh does not route through the strip helper"
    body=$(awk '/^unpatch_forgex_screen_drawing\(\)/,/^}/' \
        "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh")
    printf '%s\n' "$body" | grep -q 'forgex_strip_guard_blocks' \
        || fail "unpatch_forgex_screen_drawing does not route through the strip helper"
}

# --- Boot splash ownership: ForgeX splash daemon vs helix-splash ---
#
# ForgeX's splash daemon owns /dev/fb0 from S00init until S99root stops it at
# the end of boot. S90helixscreen starts helix-splash before forking its wait
# subshell, so without this default two processes draw to the framebuffer for
# the whole S90->S99 window.

splash_probe_fixture() {
    unset HELIX_NO_SPLASH
    export FORGEX_SPLASH_BIN="$BATS_TEST_TMPDIR/splash"
}

@test "ForgeX 1.4.2 (splash daemon present) suppresses the HelixScreen splash" {
    # The daemon owns /dev/fb0 from S00init until S99root; running ours beside
    # it puts two writers on the framebuffer for the whole S90->S99 window.
    splash_probe_fixture
    printf '#!/bin/sh\n' > "$FORGEX_SPLASH_BIN"
    chmod +x "$FORGEX_SPLASH_BIN"
    . "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
    [ "$HELIX_NO_SPLASH" = "1" ]
}

@test "ForgeX <=1.4.1 (no splash daemon) keeps the HelixScreen splash" {
    # Those releases blit a static load.img/splash.img and never repaint, so
    # nothing competes for the framebuffer and ours is the only boot progress.
    splash_probe_fixture
    rm -f "$FORGEX_SPLASH_BIN"
    . "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
    [ "$HELIX_NO_SPLASH" = "0" ]
}

@test "ForgeX hook lets an explicit HELIX_NO_SPLASH override win" {
    # Dev deploys and debugging must still be able to force our splash back on
    # even on a release that ships the daemon.
    export FORGEX_SPLASH_BIN="$BATS_TEST_TMPDIR/splash"
    printf '#!/bin/sh\n' > "$FORGEX_SPLASH_BIN"
    chmod +x "$FORGEX_SPLASH_BIN"
    HELIX_NO_SPLASH=0
    export HELIX_NO_SPLASH
    . "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
    [ "$HELIX_NO_SPLASH" = "0" ]
}

@test "ForgeX hook probes for the daemon instead of parsing version.txt" {
    # version.txt is not trustworthy: no 1.4.2 tag exists, main still reports
    # 1.4.1, and tag 1.3.1 ships a version.txt reading 1.3.0.
    hook="$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
    grep -q 'FORGEX_SPLASH_BIN' "$hook"
    # Comments may discuss version.txt; no CODE line may read it.
    run sh -c "grep -v '^[[:space:]]*#' '$hook' | grep -c 'version\.txt'"
    [ "$(last_line)" = "0" ]
}

@test "ForgeX hook resolves HELIX_NO_SPLASH at file scope, not inside a function" {
    # helixscreen.init sources hooks at file scope and reads HELIX_NO_SPLASH in
    # start(); an assignment buried in a hook function would never be seen.
    awk '/^[a-zA-Z_]+\(\)/ { in_fn = 1 }
         in_fn && /^}/     { in_fn = 0; next }
         !in_fn && /HELIX_NO_SPLASH=1/ { found = 1 }
         END { exit found ? 0 : 1 }' \
        "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
}

@test "helixscreen.init guards the early splash on HELIX_NO_SPLASH" {
    grep -qE 'HELIX_NO_SPLASH:-0.*!=.*"1"' "$WORKTREE_ROOT/config/helixscreen.init"
}

@test "helix-launcher.sh guards its splash on HELIX_NO_SPLASH" {
    grep -qE 'HELIX_NO_SPLASH:-0' "$WORKTREE_ROOT/scripts/helix-launcher.sh"
}

# --- Feather display offer pre-dismissal ---
#
# ForgeX's display_offer.cfg raises an action:prompt a few seconds after every
# Klipper start offering to switch the display to Feather, which would take the
# screen away from HelixScreen. Its "Never show again" button only sets the
# mod_params variable show_feather_promo to 0, so seeding that variable at
# install time suppresses the prompt entirely.

promo_fixture() {
    export FORGEX_VAR_FILE="$BATS_TEST_TMPDIR/variables.cfg"
    export FORGEX_OFFER_CFG="$BATS_TEST_TMPDIR/display_offer.cfg"
    touch "$FORGEX_OFFER_CFG"
    SUDO=""
    log_info() { :; }
    log_success() { :; }
    log_warn() { :; }
    . "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh"
}

@test "feather promo: a pending value is rewritten to 0" {
    promo_fixture
    printf "[Variables]\ndisplay = 'GUPPY'\nshow_feather_promo = 1\n" > "$FORGEX_VAR_FILE"
    run dismiss_forgex_feather_promo
    [ "$status" -eq 0 ]
    grep -qE "^show_feather_promo = 0$" "$FORGEX_VAR_FILE"
    # Unrelated keys must survive untouched.
    grep -qE "^display = 'GUPPY'$" "$FORGEX_VAR_FILE"
    [ "$(grep -c show_feather_promo "$FORGEX_VAR_FILE")" -eq 1 ]
}

@test "feather promo: an absent key is added inside [Variables]" {
    promo_fixture
    printf "[Variables]\ndisplay = 'GUPPY'\n" > "$FORGEX_VAR_FILE"
    run dismiss_forgex_feather_promo
    [ "$status" -eq 0 ]
    # Must land after the section header, not orphaned before it.
    [ "$(head -1 "$FORGEX_VAR_FILE")" = "[Variables]" ]
    [ "$(sed -n '2p' "$FORGEX_VAR_FILE")" = "show_feather_promo = 0" ]
    grep -qE "^display = 'GUPPY'$" "$FORGEX_VAR_FILE"
}

@test "feather promo: an already-dismissed value is left alone" {
    promo_fixture
    printf "[Variables]\nshow_feather_promo = 0\n" > "$FORGEX_VAR_FILE"
    cp "$FORGEX_VAR_FILE" "$BATS_TEST_TMPDIR/before"
    run dismiss_forgex_feather_promo
    [ "$status" -eq 0 ]
    diff "$BATS_TEST_TMPDIR/before" "$FORGEX_VAR_FILE"
}

@test "feather promo: older ForgeX without the offer is not touched" {
    promo_fixture
    rm -f "$FORGEX_OFFER_CFG"
    printf "[Variables]\ndisplay = 'GUPPY'\n" > "$FORGEX_VAR_FILE"
    run dismiss_forgex_feather_promo
    [ "$status" -eq 0 ]
    # No variable may be invented on a release whose mod_params never defines it.
    ! grep -q show_feather_promo "$FORGEX_VAR_FILE"
}

@test "feather promo: missing variables.cfg reports failure, creates nothing" {
    promo_fixture
    rm -f "$FORGEX_VAR_FILE"
    run dismiss_forgex_feather_promo
    [ "$status" -ne 0 ]
    [ ! -f "$FORGEX_VAR_FILE" ]
}

@test "installer wires dismiss_forgex_feather_promo into configure_platform" {
    grep -q 'dismiss_forgex_feather_promo' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    grep -q 'dismiss_forgex_feather_promo' "$WORKTREE_ROOT/scripts/install.sh"
}

# --- GuppyScreen launcher must be disabled, not just its init script ---

@test "configure_forgex_display disables the .root/guppyscreen launcher" {
    # zdisplay.sh apply_display_off() calls .root/guppyscreen directly, so
    # disabling only S80guppyscreen leaves the collision reachable on any
    # SET_MOD display change. The path must derive from the probe
    # (forgex_mod_root), never a pinned /opt/config literal - the AD5X's
    # host-side mod tree is /usr/data/config/mod.
    body=$(sed -n '/^configure_forgex_display()/,/^}/p' \
        "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh")
    echo "$body" | grep -q 'guppy_bin="$(forgex_mod_root)/.root/guppyscreen"'
    echo "$body" | grep -q 'chmod a-x "\$guppy_bin"'
    refute_sh "printf '%s\\n' \"\$body\" | grep -q '/opt/config/mod'"
}

# --- netd owns networking on ForgeX 1.4.2+ ---
#
# netd loads the Wi-Fi driver, runs its own wpa_supplicant, and enforces a
# single transport. It does that teardown in S55boot; our hooks run in S90, so
# anything we start here survives as a stray process on an interface netd
# believes it removed.

netd_fixture() {
    export FORGEX_NETD_BIN="$BATS_TEST_TMPDIR/netd"
    . "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
}

@test "netd probe: true when the daemon ships, false when it does not" {
    netd_fixture
    rm -f "$FORGEX_NETD_BIN"
    run forgex_netd_owns_network
    [ "$status" -ne 0 ]
    printf '#!/bin/sh\n' > "$FORGEX_NETD_BIN"
    chmod +x "$FORGEX_NETD_BIN"
    run forgex_netd_owns_network
    [ "$status" -eq 0 ]
}

# Build an environment in which platform_start_wpa_supplicant WOULD act:
# a wlan interface, a wpa_supplicant binary, and its config all present.
wpa_fixture() {
    export FORGEX_NETD_BIN="$BATS_TEST_TMPDIR/netd"
    export FORGEX_NET_SYSFS="$BATS_TEST_TMPDIR/net"
    export FORGEX_WPA_BIN="$BATS_TEST_TMPDIR/wpa_supplicant"
    export FORGEX_WPA_CONF="$BATS_TEST_TMPDIR/wpa_supplicant.conf"
    mkdir -p "$FORGEX_NET_SYSFS/wlan0"
    printf '#!/bin/sh\necho FAKE_WPA_RAN\n' > "$FORGEX_WPA_BIN"
    chmod +x "$FORGEX_WPA_BIN"
    touch "$FORGEX_WPA_CONF"
    . "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
}

@test "wpa_supplicant DOES start when no netd is present (positive control)" {
    # Without this the suppression test below is vacuous: the function has a
    # later guard that also returns 0 silently when no wlan* exists.
    wpa_fixture
    rm -f "$FORGEX_NETD_BIN"
    run platform_start_wpa_supplicant
    [ "$status" -eq 0 ]
    contains "Starting wpa_supplicant" "$output"
    [[ "$output" == *"FAKE_WPA_RAN"* ]]
}

@test "wpa_supplicant is NOT started when netd owns the radio" {
    wpa_fixture
    printf '#!/bin/sh\n' > "$FORGEX_NETD_BIN"
    chmod +x "$FORGEX_NETD_BIN"
    run platform_start_wpa_supplicant
    [ "$status" -eq 0 ]
    lacks "Starting wpa_supplicant" "$output"
    [[ "$output" != *"FAKE_WPA_RAN"* ]]
}

@test "wifi driver load returns early when netd owns the radio" {
    # The netd guard must be the first statement, ahead of the interface probe.
    body=$(sed -n '/^platform_load_wifi_driver()/,/^}/p' \
        "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh")
    [ "$(echo "$body" | sed -n '2p')" = "    if forgex_netd_owns_network; then" ]
    netd_fixture
    printf '#!/bin/sh\n' > "$FORGEX_NETD_BIN"
    chmod +x "$FORGEX_NETD_BIN"
    run platform_load_wifi_driver
    [ "$status" -eq 0 ]
    [[ "$output" != *"Loading WiFi driver"* ]]
}

@test "netd probe is defined at file scope so both hook consumers see it" {
    awk '/^[a-zA-Z_]+\(\)/ { in_fn = 1 }
         in_fn && /^}/     { in_fn = 0; next }
         !in_fn && /FORGEX_NETD_BIN=/ { found = 1 }
         END { exit found ? 0 : 1 }' \
        "$WORKTREE_ROOT/assets/config/platform/hooks-ad5m-forgex.sh"
}

# --- Which hooks file a Forge-X AD5X gets ---
#
# A Forge-X AD5X reports BOTH platform=ad5x and flavor=forge_x, and the two
# dispatch cases in install_platform_hooks pull opposite ways: the flavor case
# says ad5m-forgex, the platform case overrides to ad5x (the Z-Mod hook). The
# host profile's hook key exists precisely to break that tie: it is set only
# when the mod's own tree layout was probed, and names the rig's actual
# payload layout (ad5x-forgex). It must outrank both cases.
#
# The deployed file is compared by content, not by tracing which key string
# won: deploy_platform_hooks copies hooks-<key>.sh onto platform/hooks.sh, so
# the bytes at the destination are the behavior that ships.

_load_install_platform_hooks() {
    # main.sh's source-time traps (ERR/EXIT) are stripped from the copy: an
    # ERR trap left armed inside a bats test fires on the first failing
    # assertion into an undefined error_handler and swallows bats' result
    # line for that test.
    local main_patched="$BATS_TEST_TMPDIR/main.sh"
    sed -e "/^trap /d" \
        "$WORKTREE_ROOT/scripts/lib/installer/main.sh" > "$main_patched"
    unset _HELIX_MAIN_SOURCED _HELIX_SERVICE_SOURCED _HELIX_PLATFORM_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    # platform.sh holds resolve_platform_hook_key, which install_platform_hooks
    # delegates the whole dispatch to (it lives there rather than in main.sh so
    # scripts/device-profile.sh can ask the same question over ssh without
    # sourcing the orchestrator and its traps). Sourced ahead of main.sh, which
    # is the bundle's own module order.
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    # shellcheck disable=SC1090
    . "$main_patched"
    # platform.sh defines the real log_* helpers' callers; restore helpers.bash's
    # silent stubs so they cannot write into the assertions below.
    load helpers
}

# Stage every hooks file the dispatch could pick into a sandbox payload tree,
# so the assertion reads the destination copy, exactly as a real install
# would (the tarball ships assets/config/platform/ wholesale).
stage_hook_candidates() {
    INSTALL_DIR="$BATS_TEST_TMPDIR/payload/helixscreen"
    mkdir -p "$INSTALL_DIR/assets/config/platform"
    local f
    for f in hooks-ad5x-forgex.sh hooks-ad5x.sh hooks-ad5m-forgex.sh; do
        cp "$WORKTREE_ROOT/assets/config/platform/$f" \
           "$INSTALL_DIR/assets/config/platform/$f"
    done
    platform="ad5x"
    MOD_FLAVOR="forge_x"
    AD5M_FIRMWARE="forge_x"
}

@test "install_platform_hooks: the probed rig key outranks the ad5x platform arm" {
    _load_install_platform_hooks
    stage_hook_candidates
    HOST_PLATFORM_HOOK_KEY="ad5x-forgex"

    install_platform_hooks

    [ -f "$INSTALL_DIR/platform/hooks.sh" ] \
        || fail "no hooks deployed at all"
    cmp -s "$INSTALL_DIR/assets/config/platform/hooks-ad5x-forgex.sh" \
           "$INSTALL_DIR/platform/hooks.sh" \
        || fail "deployed hooks are not the forge-x rig file"
}

@test "install_platform_hooks: without the probe key ad5x keeps the Z-Mod hook" {
    # Control: an AD5X the probe did not recognize as the Forge-X rig (e.g.
    # Z-Mod) must keep getting the ad5x hook — flavor forge_x must not leak
    # the AD5M's forge-x file onto a non-rig box.
    _load_install_platform_hooks
    stage_hook_candidates
    HOST_PLATFORM_HOOK_KEY=""

    install_platform_hooks

    cmp -s "$INSTALL_DIR/assets/config/platform/hooks-ad5x.sh" \
           "$INSTALL_DIR/platform/hooks.sh" \
        || fail "ad5x without the probe key must deploy hooks-ad5x.sh"
}

@test "install_platform_hooks: an AD5M host still gets the ad5m-forgex hook" {
    # Control: the flavor dispatch that every installed AD5M Forge-X box
    # relies on is unchanged.
    _load_install_platform_hooks
    stage_hook_candidates
    platform="ad5m"
    HOST_PLATFORM_HOOK_KEY=""

    install_platform_hooks

    cmp -s "$INSTALL_DIR/assets/config/platform/hooks-ad5m-forgex.sh" \
           "$INSTALL_DIR/platform/hooks.sh" \
        || fail "ad5m + forge_x must deploy hooks-ad5m-forgex.sh"
}
