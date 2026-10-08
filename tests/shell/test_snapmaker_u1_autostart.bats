#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for snapmaker-u1-setup-autostart.sh (display takeover) across PAXX
# Extended Firmware 1.3 and 1.4 AND stock Snapmaker firmware, plus the matching
# uninstall restore block.
#
# Firmware difference under test:
#   PAXX 1.3 ships /etc/init.d/S99screen (the only launcher of /usr/bin/gui).
#   PAXX 1.4 deleted S99screen, ships /etc/init.d/S99fb-http instead.
#   STOCK ships NEITHER — /usr/bin/gui is started by a supervisor binary, not an
#     init script. The only squashfs-resident, boot-glob, late launcher present
#     on stock is /etc/init.d/S99input-event-daemon, so the script delegates from
#     it (preserving its stock function) to autostart HelixScreen at boot.
# The launcher-independent fix disables /usr/bin/gui (chmod a-x) so no launcher
# on any firmware can start the stock UI. The autostart script is driven against
# a mock rootfs via HELIX_SETUP_ROOT.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    log_info() { echo "INFO: $*"; }
    log_warn() { echo "WARN: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_info log_warn log_success

    export MOCK_ROOT="$BATS_TEST_TMPDIR/u1"
    mkdir -p "$MOCK_ROOT/oem" "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/bin"

    # The deploy dir must hold an executable config/helixscreen.init or the
    # autostart script bails out early.
    export DEPLOY_DIR="$BATS_TEST_TMPDIR/deploy"
    mkdir -p "$DEPLOY_DIR/config"
    cat > "$DEPLOY_DIR/config/helixscreen.init" <<'EOF'
#!/bin/sh
echo "helixscreen init $1"
EOF
    chmod +x "$DEPLOY_DIR/config/helixscreen.init"

    # Stock UI binary present and executable, as it ships on the device.
    cat > "$MOCK_ROOT/usr/bin/gui" <<'EOF'
#!/bin/sh
echo "stock gui"
EOF
    chmod +x "$MOCK_ROOT/usr/bin/gui"

    AUTOSTART="$WORKTREE_ROOT/scripts/snapmaker-u1-setup-autostart.sh"
}

# Pre-create a stock S99screen (no HelixScreen marker, launches /usr/bin/gui),
# matching firmware 1.3.
seed_stock_s99screen() {
    cat > "$MOCK_ROOT/etc/init.d/S99screen" <<'EOF'
#!/bin/sh
# Start/stop GUI process
start-stop-daemon -S -b -x /usr/bin/gui -m -p /var/run/gui.pid
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen"
}

# Pre-create a stock S99fb-http (no HelixScreen marker, the 1.4 framebuffer-HTTP
# screen launcher). 1.4 ships this in the squashfs (so it IS in the boot glob);
# 1.2/1.3 do NOT have it.
seed_stock_s99fbhttp() {
    cat > "$MOCK_ROOT/etc/init.d/S99fb-http" <<'EOF'
#!/bin/sh
# Stock fb-http remote-screen launcher
NAME="fb-http"
DAEMON="/usr/local/bin/fb-http.py"
start-stop-daemon -S -b -x "$DAEMON" -m -p /var/run/$NAME.pid
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99fb-http"
}

# Pre-create a stock S99input-event-daemon (no HelixScreen marker). Present on
# both stock and PAXX squashfs; on stock it is the ONLY boot-glob launcher we can
# hook (stock ships no S99screen/S99fb-http display launcher).
seed_stock_s99ied() {
    cat > "$MOCK_ROOT/etc/init.d/S99input-event-daemon" <<'EOF'
#!/bin/sh
[ -f /usr/bin/input-event-daemon ] || exit 0
case "$1" in
  start) input-event-daemon ;;
  stop)  killall input-event-daemon ;;
esac
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
}

run_autostart() {
    HELIX_SETUP_ROOT="$MOCK_ROOT" bash "$AUTOSTART" "$DEPLOY_DIR"
}

@test "autostart 1.4: no pre-existing S99screen — disables gui, creates marked launcher" {
    # Firmware 1.4: /etc/init.d/S99screen is absent in the flashed rootfs.
    [ ! -e "$MOCK_ROOT/etc/init.d/S99screen" ]

    run run_autostart
    [ "$status" -eq 0 ]

    # Overlay persistence enabled.
    [ -f "$MOCK_ROOT/oem/.debug" ]

    # The launcher-independent kill switch fired: gui is no longer executable.
    [ -f "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]

    # HelixScreen launcher was created and carries the marker.
    [ -f "$MOCK_ROOT/etc/init.d/S99screen" ]
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen"
}

@test "autostart 1.3: pre-existing stock S99screen — backs it up, patches it, disables gui" {
    seed_stock_s99screen

    run run_autostart
    [ "$status" -eq 0 ]

    # Stock launcher preserved.
    [ -f "$MOCK_ROOT/etc/init.d/S99screen.stock" ]
    grep -q "start-stop-daemon" "$MOCK_ROOT/etc/init.d/S99screen.stock"
    refute grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen.stock"

    # Live launcher now ours.
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen"

    # Binary kill switch fired on 1.3 too.
    [ -f "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
}

@test "autostart idempotent: second run exits 0, gui stays disabled, .debug present" {
    run run_autostart
    [ "$status" -eq 0 ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]

    # Second run with the launcher already current must still succeed and must
    # not re-enable gui (the early-skip must not bypass the gui-disable step).
    run run_autostart
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "already patched"
    [ -f "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
    [ -f "$MOCK_ROOT/oem/.debug" ]
}

@test "autostart ensures helixscreen.init is executable (delegate -x guard)" {
    # A deploy that bypassed the installer can leave the init non-executable,
    # which makes the S99screen/S99fb-http delegates silently skip HelixScreen.
    chmod a-x "$DEPLOY_DIR/config/helixscreen.init"
    [ ! -x "$DEPLOY_DIR/config/helixscreen.init" ]

    run run_autostart
    [ "$status" -eq 0 ]
    [ -x "$DEPLOY_DIR/config/helixscreen.init" ]
}

@test "autostart: gui already disabled is reported, not re-enabled" {
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    run run_autostart
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "already disabled"
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
}

# --- Firmware 1.4 boot-glob hook: S99fb-http -------------------------------
# On 1.4 the squashfs deleted S99screen and our overlay-created S99screen is not
# in the boot-time rcS glob (rcS runs from squashfs, expands S?? before the
# overlay pivot), so it never starts helix at boot. The stock display launcher
# S99fb-http IS in the squashfs glob, so we hook it too.

@test "autostart 1.4: hooks S99fb-http — saves .stock, writes helix delegate" {
    seed_stock_s99fbhttp   # 1.4: fb-http present, no S99screen seeded

    run run_autostart
    [ "$status" -eq 0 ]

    # Stock fb-http preserved.
    [ -f "$MOCK_ROOT/etc/init.d/S99fb-http.stock" ]
    grep -q "fb-http.py" "$MOCK_ROOT/etc/init.d/S99fb-http.stock"
    refute grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99fb-http.stock"

    # Live S99fb-http now delegates to HelixScreen's init and falls back to .stock.
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99fb-http"
    grep -q "helixscreen.init" "$MOCK_ROOT/etc/init.d/S99fb-http"
    grep -q "S99fb-http.stock" "$MOCK_ROOT/etc/init.d/S99fb-http"
    [ -x "$MOCK_ROOT/etc/init.d/S99fb-http" ]
}

@test "autostart 1.2/1.3: no S99fb-http present — S99screen hooked, fb-http skipped (compat)" {
    seed_stock_s99screen   # 1.3: S99screen present, NO S99fb-http

    run run_autostart
    [ "$status" -eq 0 ]

    # 1.3 boot launcher (S99screen) still hooked — compatibility preserved.
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen"
    [ -f "$MOCK_ROOT/etc/init.d/S99screen.stock" ]

    # We must NOT fabricate an S99fb-http where the firmware has none.
    [ ! -e "$MOCK_ROOT/etc/init.d/S99fb-http" ]
    echo "$output" | grep -qi "1.2/1.3"
}

@test "autostart 1.4 idempotent: second run keeps S99fb-http patched, .stock intact" {
    seed_stock_s99fbhttp

    run run_autostart
    [ "$status" -eq 0 ]
    run run_autostart
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "S99fb-http already patched"
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99fb-http"
    # .stock must still be the stock (not overwritten with our delegate).
    grep -q "fb-http.py" "$MOCK_ROOT/etc/init.d/S99fb-http.stock"
    ! grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99fb-http.stock"
}

# --- Stock firmware boot-glob hook: S99input-event-daemon ------------------
# Stock ships no S99screen/S99fb-http display launcher; /usr/bin/gui is started
# by a supervisor binary. The only squashfs-resident, late, boot-glob launcher
# is S99input-event-daemon, whose stock function (input-event-daemon) we KEEP.
# So we delegate: run the saved .stock first, THEN start HelixScreen.

@test "autostart stock: hooks S99input-event-daemon — saves .stock, delegate preserves stock + starts helix" {
    seed_stock_s99ied   # stock: only S99input-event-daemon, NO S99screen/S99fb-http
    [ ! -e "$MOCK_ROOT/etc/init.d/S99screen" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99fb-http" ]

    run run_autostart
    [ "$status" -eq 0 ]

    # Stock script preserved without our marker.
    [ -f "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock" ]
    grep -q "input-event-daemon" "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock"
    refute grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock"

    # Live launcher: carries marker, calls helixscreen.init, AND preserves the
    # stock daemon via the .stock delegate (we keep input-event-daemon running).
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
    grep -q "helixscreen.init" "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
    grep -q "S99input-event-daemon.stock" "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
    [ -x "$MOCK_ROOT/etc/init.d/S99input-event-daemon" ]

    # gui kill-switch still fires on stock.
    [ -f "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
}

@test "autostart: no S99input-event-daemon present — stock hook skipped, no fabrication" {
    # PAXX-without-the-binary or any rootfs lacking it: must not create one.
    [ ! -e "$MOCK_ROOT/etc/init.d/S99input-event-daemon" ]
    run run_autostart
    [ "$status" -eq 0 ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99input-event-daemon" ]
    echo "$output" | grep -q "No S99input-event-daemon present"
}

@test "autostart stock idempotent: second run keeps S99input-event-daemon patched, .stock intact" {
    seed_stock_s99ied

    run run_autostart
    [ "$status" -eq 0 ]
    run run_autostart
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "S99input-event-daemon already patched"
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
    # .stock must remain the stock script, not our delegate.
    grep -q "input-event-daemon" "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock"
    ! grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock"
}

@test "autostart stock: notes that SSH follows the Root Access setting" {
    cat > "$MOCK_ROOT/etc/init.d/S50dropbear" <<'EOF'
#!/bin/sh
if [ "$2" != "--force" ] ; then
	[ x"$(custom_misc vertype 2>/dev/null)" = x"dbg" ] || exit 0
fi
EOF
    run run_autostart
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "Root Access"
}

@test "autostart PAXX: no Root Access note when S50dropbear has no --force gate" {
    cat > "$MOCK_ROOT/etc/init.d/S50dropbear" <<'EOF'
#!/bin/sh
# if [ "$2" != "--force" ] ; then
# 	[ x"$(custom_misc vertype 2>/dev/null)" = x"dbg" ] || exit 0
# fi
EOF
    run run_autostart
    [ "$status" -eq 0 ]
    ! echo "$output" | grep -q "Root Access"
}

# --- Uninstall restore block -------------------------------------------------
# Extract just the snapmaker-u1 branch from uninstall.sh and exercise it against
# the mock rootfs, using the same path-rewriting trick as test_cc1_uninstall.

extract_u1_restore() {
    cat > "$BATS_TEST_TMPDIR/u1_restore.sh" <<'SH_EOF'
#!/bin/sh
SH_EOF
    awk '
        /^    # Snapmaker U1: re-enable the stock UI/ { capture=1 }
        capture { print }
        capture && /^    fi$/ && ++blocks==1 { exit }
    ' "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" >> "$BATS_TEST_TMPDIR/u1_restore.sh"

    # Portable in-place rewrite (BSD sed on macOS needs a suffix arg for -i;
    # GNU sed does not — sidestep both by writing through a temp file).
    sed \
        -e "s|/usr/bin/gui|$MOCK_ROOT/usr/bin/gui|g" \
        -e "s|/etc/init.d/S99fb-http|$MOCK_ROOT/etc/init.d/S99fb-http|g" \
        -e "s|/etc/init.d/S99screen|$MOCK_ROOT/etc/init.d/S99screen|g" \
        -e "s|/etc/init.d/S99input-event-daemon|$MOCK_ROOT/etc/init.d/S99input-event-daemon|g" \
        -e "s|/oem/.debug|$MOCK_ROOT/oem/.debug|g" \
        "$BATS_TEST_TMPDIR/u1_restore.sh" > "$BATS_TEST_TMPDIR/u1_restore.sh.tmp"
    mv "$BATS_TEST_TMPDIR/u1_restore.sh.tmp" "$BATS_TEST_TMPDIR/u1_restore.sh"

    u1_restore() {
        local platform="snapmaker-u1"
        local restored_ui=""
        # shellcheck disable=SC1091
        . "$BATS_TEST_TMPDIR/u1_restore.sh"
        printf '%s' "$restored_ui"
    }
}

@test "uninstall u1 (1.4): re-enables gui and removes HelixScreen-created S99screen" {
    # Post-install 1.4 state: gui disabled, our marked S99screen, no .stock.
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    cat > "$MOCK_ROOT/etc/init.d/S99screen" <<'EOF'
#!/bin/sh
# Modified by HelixScreen
exec /userdata/helixscreen/config/helixscreen.init "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen"

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]
    # restored_ui is set (the marker substring survives the mock-path rewrite).
    echo "$output" | grep -q "Snapmaker stock UI"

    # gui re-enabled, our launcher removed (1.4 had none to restore).
    [ -x "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99screen" ]
}

@test "uninstall u1 (1.4): re-enables gui and restores stock S99fb-http from .stock" {
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    # Post-install 1.4 state: our delegate is live, stock fb-http saved as .stock.
    cat > "$MOCK_ROOT/etc/init.d/S99fb-http" <<'EOF'
#!/bin/sh
# Modified by HelixScreen
exec /userdata/helixscreen/config/helixscreen.init "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99fb-http"
    cat > "$MOCK_ROOT/etc/init.d/S99fb-http.stock" <<'EOF'
#!/bin/sh
start-stop-daemon -S -b -x /usr/local/bin/fb-http.py -m -p /var/run/fb-http.pid
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99fb-http.stock"

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]

    [ -x "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99fb-http.stock" ]
    [ -f "$MOCK_ROOT/etc/init.d/S99fb-http" ]
    grep -q "fb-http.py" "$MOCK_ROOT/etc/init.d/S99fb-http"
    ! grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99fb-http"
}

@test "uninstall u1 (1.3): re-enables gui and restores stock S99screen from .stock" {
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    cat > "$MOCK_ROOT/etc/init.d/S99screen" <<'EOF'
#!/bin/sh
# Modified by HelixScreen
exec /userdata/helixscreen/config/helixscreen.init "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen"
    cat > "$MOCK_ROOT/etc/init.d/S99screen.stock" <<'EOF'
#!/bin/sh
start-stop-daemon -S -b -x /usr/bin/gui -m -p /var/run/gui.pid
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99screen.stock"

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]

    [ -x "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99screen.stock" ]
    [ -f "$MOCK_ROOT/etc/init.d/S99screen" ]
    grep -q "start-stop-daemon" "$MOCK_ROOT/etc/init.d/S99screen"
    ! grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99screen"
}

@test "uninstall u1 (stock): re-enables gui and restores stock S99input-event-daemon from .stock" {
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    # Post-install stock state: our delegate is live, stock saved as .stock.
    cat > "$MOCK_ROOT/etc/init.d/S99input-event-daemon" <<'EOF'
#!/bin/sh
# Modified by HelixScreen
[ -x /etc/init.d/S99input-event-daemon.stock ] && /etc/init.d/S99input-event-daemon.stock "$@"
exec /userdata/helixscreen/config/helixscreen.init "$1"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
    cat > "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock" <<'EOF'
#!/bin/sh
[ -f /usr/bin/input-event-daemon ] || exit 0
input-event-daemon
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock"

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]

    [ -x "$MOCK_ROOT/usr/bin/gui" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock" ]
    [ -f "$MOCK_ROOT/etc/init.d/S99input-event-daemon" ]
    grep -q "input-event-daemon" "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
    ! grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
}

# --- /oem/.debug overlay-persistence flag removal ---------------------------
# The installer creates /oem/.debug (autostart step 1) so S01aoverlayfs does NOT
# wipe /oem/overlay/* on boot, persisting our overlay changes. Leaving it behind
# is an uninstall-incompleteness bug: it's a firmware debug-mode flag not present
# on a clean stock U1. The uninstall block must drop it as its final step so the
# next boot reverts the overlay-upper back to pristine squashfs-lower stock.

@test "uninstall u1: removes leftover /oem/.debug overlay-persistence flag" {
    # Post-install state: the flag the installer touched is present.
    touch "$MOCK_ROOT/oem/.debug"
    [ -f "$MOCK_ROOT/oem/.debug" ]

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]

    # The uninstall dropped the flag — stock overlay-wipe-on-boot is restored.
    [ ! -e "$MOCK_ROOT/oem/.debug" ]
    # /oem itself must survive — we only remove the flag file, nothing else.
    [ -d "$MOCK_ROOT/oem" ]
}

@test "uninstall u1: no /oem/.debug present is a no-op (no error)" {
    # A stock device (or one where the flag was already cleared) must not error.
    [ ! -e "$MOCK_ROOT/oem/.debug" ]

    extract_u1_restore
    run u1_restore
    [ "$status" -eq 0 ]

    [ ! -e "$MOCK_ROOT/oem/.debug" ]
    [ -d "$MOCK_ROOT/oem" ]
}
