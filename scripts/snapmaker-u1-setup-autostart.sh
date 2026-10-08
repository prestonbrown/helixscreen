#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Set up HelixScreen auto-start on Snapmaker U1
# Run on the device: ./snapmaker-u1-setup-autostart.sh /userdata/helixscreen
#
# This script:
# 1. Creates /oem/.debug to prevent overlay wipe on boot
# 2. Patches /etc/init.d/S99screen to start HelixScreen instead of stock GUI
# 3. Disables the stock UI binary /usr/bin/gui (chmod a-x) so no launcher can
#    exec it
#
# Firmware behavior (PAXX Extended Firmware vs stock Snapmaker firmware):
#   - PAXX 1.3 ships /etc/init.d/S99screen, the ONLY launcher of /usr/bin/gui.
#     Our S99screen patch (step 2) suppresses the stock UI there.
#   - PAXX 1.4 DELETED /etc/init.d/S99screen and launches /usr/bin/gui from a
#     runtime-generated path that is NOT present in the flashed rootfs, so it
#     cannot be patched by name. It DOES ship /etc/init.d/S99fb-http (step 4b).
#   - STOCK firmware ships NEITHER S99screen NOR S99fb-http — those exist only
#     in PAXX, which rebuilds the read-only squashfs to add them. On stock,
#     /usr/bin/gui is started by a supervisor binary, not by any /etc/init.d
#     script, so there is no display launcher to hook. The S99screen we write
#     in step 2 lives only in the writable overlay, and the busybox boot glob is
#     frozen from the squashfs BEFORE S01aoverlayfs pivots onto the overlay — so
#     an overlay-only S99screen runs at shutdown (rcK) but NEVER at boot. Step 4c
#     handles stock: it delegates from S99input-event-daemon, which DOES ship in
#     the stock squashfs (so it is in the boot glob) and runs after the overlay
#     pivot.
#   Step 3 is the launcher-independent SUPPRESSION fix: disabling the binary
#   itself means no launcher on any firmware (PAXX's S99screen/relocated path or
#   stock's supervisor) can exec /usr/bin/gui, so the stock UI never grabs the
#   framebuffer / DRM. We still write S99screen (step 2) because it is also our
#   HelixScreen LAUNCHER and runs at boot on PAXX 1.2/1.3.
#
# The patch is regenerated and compared to the on-disk version every run; if
# they differ we rewrite. This is self-healing: a legacy patch from an older
# helixscreen version (e.g. one that hardcoded the init path before
# helixscreen.init moved into config/) gets repaired on the next self-update,
# and the previous "skip if first 5 lines mention HelixScreen" heuristic that
# silently locked users to a broken patch is gone.
#
# To revert: run the HelixScreen uninstaller — it restores the /usr/bin/gui
# exec bit and the stock S99screen launcher. NOTE: a bare
# `rm -rf /userdata/helixscreen && reboot` is no longer sufficient — it leaves
# /usr/bin/gui disabled, so the stock UI will NOT come back. Use the
# uninstaller to fully revert.

set -e

# Testability sysroot prefix. Empty in production; the BATS suite points it at
# a mock rootfs so the host-system paths this script touches (/oem/.debug,
# /etc/init.d/S99screen, /usr/bin/gui) land under a temp dir instead of the
# real system. Only HOST paths get the prefix — the heredoc PATCH body below
# runs on the device and stays absolute.
SYSROOT="${HELIX_SETUP_ROOT:-}"

DEPLOY_DIR="${1:-/userdata/helixscreen}"

INIT_SCRIPT="$DEPLOY_DIR/config/helixscreen.init"
if [ ! -f "$INIT_SCRIPT" ]; then
    echo "Error: $INIT_SCRIPT not found"
    echo "Deploy HelixScreen first, then run this script"
    exit 1
fi

# Belt-and-suspenders: the S99screen / S99fb-http delegates only start HelixScreen
# if helixscreen.init is executable ([ -x ... ]); otherwise they silently fall
# through to the (disabled) stock UI and nothing comes up at boot. The installer
# normally chmods it, but a deploy that bypassed the installer can leave it
# non-executable — so guarantee it here.
chmod +x "$INIT_SCRIPT" 2>/dev/null || true

# Step 1: Create /oem/.debug to prevent overlay wipe on boot
# Without this, S01aoverlayfs runs: rm -rf /oem/overlay/*
if [ ! -f "${SYSROOT}/oem/.debug" ]; then
    touch "${SYSROOT}/oem/.debug"
    echo "Created ${SYSROOT}/oem/.debug (overlay persistence enabled)"
else
    echo "${SYSROOT}/oem/.debug already exists"
fi

# Step 2: Render desired S99screen patch into a temp file
S99_TARGET="${SYSROOT}/etc/init.d/S99screen"
TMP_PATCH=$(mktemp)
TMP_FB=""
TMP_IED=""
trap 'rm -f "$TMP_PATCH" "$TMP_FB" "$TMP_IED"' EXIT

cat > "$TMP_PATCH" << 'PATCH'
#!/bin/sh
#
# Start/stop GUI process
# Modified by HelixScreen: delegates to HelixScreen init when installed
#

GUI="/usr/bin/gui"
PIDFILE=/var/run/gui.pid

log()
{
	logger -p user.info -t "GUI[$$]" -- "$1"
	echo "$1"
}

# If HelixScreen is installed, delegate to its init script
for helix_init in /userdata/helixscreen/config/helixscreen.init /opt/helixscreen/config/helixscreen.init; do
    if [ -x "$helix_init" ]; then
        case "$1" in
          start)
            log "HelixScreen detected, starting instead of stock GUI"
            "$helix_init" start
            ;;
          stop)
            "$helix_init" stop
            ;;
          restart)
            "$helix_init" stop
            sleep 1
            "$helix_init" start
            ;;
          *)
            echo "Usage: $0 {start|stop|restart}"
            exit 1
        esac
        exit 0
    fi
done

# Stock GUI fallback (no HelixScreen installed)
case "$1" in
  start)
	log "Starting GUI process..."
	ulimit -c 102400
	start-stop-daemon -S -b -x "$GUI" -m -p "$PIDFILE"
	;;
  stop)
	log "Stopping GUI process..."
	start-stop-daemon -K -x "$GUI" -p "$PIDFILE" -o
	;;
  restart)
	"$0" stop
	sleep 1
	"$0" start
	;;
  *)
	echo "Usage: $0 {start|stop|restart}"
	exit 1
esac
PATCH

# Step 3: If the on-disk script already matches, skip the rewrite — but DO NOT
# exit, because the gui-disable step (Step 5) must still run on every invocation
# (e.g. a self-update that already has the current S99screen still needs to
# ensure /usr/bin/gui stays disabled on 1.4).
if [ -f "$S99_TARGET" ] && cmp -s "$TMP_PATCH" "$S99_TARGET"; then
    echo "S99screen already patched (current version)"
else
    # Step 4: Preserve the original stock S99screen the first time we replace it.
    # Detect "stock" by absence of any HelixScreen marker; once we've saved a
    # .stock copy we don't overwrite it.
    if [ -f "$S99_TARGET" ] && [ ! -f "$S99_TARGET.stock" ] && \
       ! grep -q HelixScreen "$S99_TARGET" 2>/dev/null; then
        cp "$S99_TARGET" "$S99_TARGET.stock"
        echo "Saved stock S99screen backup to $S99_TARGET.stock"
    fi

    cp "$TMP_PATCH" "$S99_TARGET"
    chmod +x "$S99_TARGET"
    echo "S99screen patched — HelixScreen will auto-start on boot"
fi

# Step 4b: Firmware 1.4 boot-glob hook — S99fb-http.
#
# CRITICAL (1.4 only): busybox init runs /etc/init.d/rcS from the read-only
# squashfs and expands its `for i in /etc/init.d/S??*` boot glob ONCE, *before*
# S01aoverlayfs pivot_roots onto the .debug-persisted overlay. 1.4 deleted
# S99screen from the squashfs, so our overlay-created S99screen is NOT in that
# frozen glob and never runs at boot (it only runs at *shutdown*, via rcK, once
# the overlay is active). The stock display launcher S99fb-http DOES ship in the
# 1.4 squashfs, so it IS in the boot glob and rcS executes the overlay copy of
# it after the pivot. We therefore install the same HelixScreen launcher
# delegate over S99fb-http, preserving the stock fb-http behavior (exec'd from
# the saved .stock) for the helix-not-installed case.
#
# 1.2/1.3 have no S99fb-http (they boot the UI via S99screen, hooked above), so
# this step is a no-op there — preserving 1.2/1.3 compatibility.
S99FB_TARGET="${SYSROOT}/etc/init.d/S99fb-http"
if [ -f "$S99FB_TARGET" ]; then
    TMP_FB=$(mktemp)
    # The wrapper body runs ON-DEVICE, so it uses absolute device paths (no
    # SYSROOT). Quoted heredoc — nothing here is expanded at render time.
    cat > "$TMP_FB" << 'FBPATCH'
#!/bin/sh
#
# Start/stop the on-device screen.
# Modified by HelixScreen: when HelixScreen is installed, launch it instead of
# the stock framebuffer-HTTP screen; otherwise fall back to the original
# behavior, preserved as /etc/init.d/S99fb-http.stock.
#
for helix_init in /userdata/helixscreen/config/helixscreen.init /opt/helixscreen/config/helixscreen.init; do
    if [ -x "$helix_init" ]; then
        case "$1" in
          start)   "$helix_init" start ;;
          stop)    "$helix_init" stop ;;
          restart) "$helix_init" stop; sleep 1; "$helix_init" start ;;
          *) echo "Usage: $0 {start|stop|restart}"; exit 1 ;;
        esac
        exit 0
    fi
done
# HelixScreen not installed — run the original fb-http behavior.
if [ -x /etc/init.d/S99fb-http.stock ]; then
    exec /etc/init.d/S99fb-http.stock "$@"
fi
exit 0
FBPATCH
    if cmp -s "$TMP_FB" "$S99FB_TARGET"; then
        echo "S99fb-http already patched (current version)"
    else
        # Preserve the stock S99fb-http the first time we replace it (detected by
        # absence of a HelixScreen marker), so the uninstaller can restore it.
        if [ ! -f "$S99FB_TARGET.stock" ] && \
           ! grep -q HelixScreen "$S99FB_TARGET" 2>/dev/null; then
            cp "$S99FB_TARGET" "$S99FB_TARGET.stock"
            echo "Saved stock S99fb-http backup to $S99FB_TARGET.stock"
        fi
        cp "$TMP_FB" "$S99FB_TARGET"
        chmod +x "$S99FB_TARGET"
        echo "S99fb-http patched — HelixScreen will auto-start on boot (firmware 1.4)"
    fi
else
    echo "No S99fb-http present (firmware 1.2/1.3) — S99screen hook handles boot"
fi

# Step 4c: Stock-firmware boot hook — S99input-event-daemon.
#
# Stock Snapmaker firmware ships NO display-launcher init script (/usr/bin/gui is
# started by a supervisor binary, not by /etc/init.d) and neither S99screen nor
# S99fb-http — both are PAXX-only additions baked into PAXX's rebuilt squashfs.
# On stock, the S99screen we write in step 2 lives only in the overlay, so the
# busybox boot glob (frozen from the read-only squashfs before S01aoverlayfs
# pivots) never runs it at boot.
#
# To auto-start on stock we delegate from a script that DOES ship in the stock
# squashfs (so it is in the boot glob) and runs after S01aoverlayfs.
# S99input-event-daemon fits: present on stock (and PAXX), sorts last, runs
# post-pivot, so rcS executes its overlay copy. Unlike the display launchers, its
# stock function (starting input-event-daemon) is one we KEEP — so we run the
# preserved stock script first, THEN start HelixScreen. helixscreen.init `start`
# is idempotent (pidof guard), so on PAXX — where S99fb-http/S99screen already
# started HelixScreen earlier in the same sequential boot glob — this second call
# is a harmless no-op. 1.2/1.3/stock without the binary: [ -f ] guard skips it.
S99IED_TARGET="${SYSROOT}/etc/init.d/S99input-event-daemon"
if [ -f "$S99IED_TARGET" ]; then
    TMP_IED=$(mktemp)
    # Wrapper body runs ON-DEVICE — absolute device paths, no SYSROOT. Quoted
    # heredoc: nothing is expanded at render time.
    cat > "$TMP_IED" << 'IEDPATCH'
#!/bin/sh
#
# Modified by HelixScreen: preserve the stock input-event-daemon behavior, then
# launch HelixScreen. On stock Snapmaker firmware this is the only boot-glob
# script that starts HelixScreen (stock ships no S99screen/S99fb-http display
# launcher). helixscreen.init `start` is idempotent, so on PAXX — where an
# earlier launcher already started HelixScreen — the call below is a no-op.
#
if [ -x /etc/init.d/S99input-event-daemon.stock ]; then
    /etc/init.d/S99input-event-daemon.stock "$@"
fi
for helix_init in /userdata/helixscreen/config/helixscreen.init /opt/helixscreen/config/helixscreen.init; do
    if [ -x "$helix_init" ]; then
        "$helix_init" "$1"
        break
    fi
done
exit 0
IEDPATCH
    if cmp -s "$TMP_IED" "$S99IED_TARGET"; then
        echo "S99input-event-daemon already patched (current version)"
    else
        # Preserve the stock script the first time we replace it (detected by
        # absence of a HelixScreen marker), so the uninstaller can restore it.
        if [ ! -f "$S99IED_TARGET.stock" ] && \
           ! grep -q HelixScreen "$S99IED_TARGET" 2>/dev/null; then
            cp "$S99IED_TARGET" "$S99IED_TARGET.stock"
            echo "Saved stock S99input-event-daemon backup to $S99IED_TARGET.stock"
        fi
        cp "$TMP_IED" "$S99IED_TARGET"
        chmod +x "$S99IED_TARGET"
        echo "S99input-event-daemon patched — HelixScreen will auto-start on boot (stock firmware)"
    fi
else
    echo "No S99input-event-daemon present — skipping stock boot hook"
fi

# Step 5: Neutralize the stock on-device UI so HelixScreen owns the display.
# 1.3 launches /usr/bin/gui via /etc/init.d/S99screen (patched above); 1.4
# removed S99screen and launches /usr/bin/gui from a runtime path we cannot
# patch by name. Disabling the binary itself is launcher-independent: no stock
# script on either firmware can exec it, so the stock UI never grabs the
# framebuffer / DRM. Reversible — the uninstaller restores the exec bit.
GUI_BIN="${SYSROOT}/usr/bin/gui"
if [ -x "$GUI_BIN" ]; then
    if chmod a-x "$GUI_BIN" 2>/dev/null; then
        echo "Disabled stock UI binary ($GUI_BIN) — HelixScreen owns the display"
    else
        echo "Warning: could not disable $GUI_BIN; stock UI may compete for the display"
    fi
elif [ -f "$GUI_BIN" ]; then
    echo "Stock UI binary already disabled ($GUI_BIN)"
else
    echo "Stock UI binary not present ($GUI_BIN) — nothing to disable"
fi

# Stock firmware starts SSH only from the stock UI's Root Access setting. The
# platform hook (ensure_root_ssh) takes over starting it, but the switch itself
# lives in the disabled stock UI.
if grep -q '^[^#]*custom_misc vertype' "${SYSROOT}/etc/init.d/S50dropbear" 2>/dev/null; then
    echo "Note: SSH stays available while the stock Root Access setting is on; HelixScreen starts it at boot."
    echo "      To change Root Access, uninstall HelixScreen, switch it in the stock UI, then reinstall."
fi

echo "To revert: run the HelixScreen uninstaller (restores /usr/bin/gui and S99screen)"
