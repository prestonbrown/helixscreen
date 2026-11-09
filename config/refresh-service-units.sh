#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Refresh systemd unit files from newly extracted install dir templates.
#
# Called by helixscreen-update.service after Moonraker extracts a new release.
# This script lives in the install dir (not /etc/systemd/system/) so Moonraker's
# extraction always updates it — solving the chicken-and-egg where the installer's
# global sed pass would corrupt @@placeholder@@ patterns embedded directly in the
# systemd unit file.
#
# Reads User/Group from the CURRENTLY installed helixscreen.service before
# overwriting, then templates all @@placeholders@@ in the new copies.
# Also refreshes the watcher units (update.service + update.path) so future
# Moonraker updates pick up any fixes to the watcher mechanism itself.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIR="$(dirname "$SCRIPT_DIR")"
PDIR="$(dirname "$IDIR")"

DEST="/etc/systemd/system/helixscreen.service"

# Nothing to do if main service isn't installed
[ -f "$DEST" ] || exit 0

# Wait for Moonraker's extraction to settle before we touch INSTALL_DIR.
#
# helixscreen-update.path fires PathChanged on release_info.json as soon as
# Moonraker recreates it during zip extraction; the update.service then waits
# 10s and calls us.  On slow MIPS flash (AD5X / K1) Moonraker may still be
# extracting at that point, so refresh_config_symlinks would race against the
# extraction and leave the install dir in a state Moonraker can't finish
# cleaning up — producing the "ENOTEMPTY: /srv/helixscreen" toast in Mainsail
# and a binary-missing crash loop afterward.
#
# Heuristic: binary + release_info.json both present and the binary's size
# stable across two polls 2s apart.  Times out after 90s and proceeds anyway
# (a half-broken refresh is still better than not refreshing at all — e.g.
# during recovery when the binary is missing entirely).
wait_for_install_stable() {
    local binary="${IDIR}/bin/helix-screen"
    local release_info="${IDIR}/release_info.json"
    local budget=90 elapsed=0 prev_size=-1 stable=0 size

    while [ "$elapsed" -lt "$budget" ]; do
        if [ -x "$binary" ] && [ -f "$release_info" ]; then
            size=$(stat -c%s "$binary" 2>/dev/null || echo -1)
            if [ "$size" -gt 0 ] && [ "$size" = "$prev_size" ]; then
                stable=$((stable + 1))
                [ "$stable" -ge 2 ] && return 0
            else
                stable=0
                prev_size="$size"
            fi
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "refresh-service-units: install dir not stable after ${budget}s — proceeding" >&2
    return 1
}

wait_for_install_stable || true

# Read current identity from the installed service file BEFORE overwriting
USER_VAL="$(grep "^User=" "$DEST" | cut -d= -f2)"
GROUP_VAL="$(grep "^Group=" "$DEST" | cut -d= -f2)"
PD_HELIX_VAL="$(sed -n 's/^# HELIX_PD_HELIX=//p' "$DEST" | head -n1)"
PD_HELIX_SED="$(printf '%s' "$PD_HELIX_VAL" | sed 's/[\\&|]/\\&/g')"

template_unit() {
    local src="$1" dest="$2"
    cp "$src" "$dest" || return 1
    sed -i \
        -e "s|@@HELIX_USER@@|${USER_VAL:-root}|g" \
        -e "s|@@HELIX_GROUP@@|${GROUP_VAL:-root}|g" \
        -e "s|@@INSTALL_DIR@@|${IDIR}|g" \
        -e "s|@@INSTALL_PARENT@@|${PDIR}|g" \
        -e "s|@@HELIX_PD_HELIX@@|${PD_HELIX_SED}|g" \
        "$dest"
}

# Refresh main service
SRC="${IDIR}/config/helixscreen.service"
[ -f "$SRC" ] && template_unit "$SRC" "$DEST"

# Refresh watcher units (this service + path unit)
for F in helixscreen-update.service helixscreen-update.path; do
    FSRC="${IDIR}/config/${F}"
    FDEST="/etc/systemd/system/${F}"
    [ -f "$FSRC" ] && template_unit "$FSRC" "$FDEST"
done

systemctl daemon-reload

# Refresh the QIDI .3mf thumbnail helper units when that duty is active here
# (prestonbrown/helixscreen#1713). The in-app update path cannot install them
# itself (NoNewPrivileges blocks sudo), so this root-privileged refresh is
# what brings them to an updating machine. The gate lives inside the helper
# script so install-time and update-time cannot disagree about when it
# applies; USER_VAL is the user the main service runs as, i.e. the Klipper
# user, read above before the service file was overwritten.
[ -x "${IDIR}/config/qidi-3mf-thumbs-units.sh" ] && \
    "${IDIR}/config/qidi-3mf-thumbs-units.sh" "${USER_VAL:-root}" "${GROUP_VAL:-root}" || true

# --- Restore user config and platform hooks after a Moonraker update ---
#
# The same restore runs from the SysV init script, so it lives in its own file.
# Failure is not fatal here: the units above are already refreshed.
[ -f "${SCRIPT_DIR}/restore-user-config.sh" ] && \
    USER_VAL="$USER_VAL" HELIX_PD_HELIX_DIR="$PD_HELIX_VAL" sh "${SCRIPT_DIR}/restore-user-config.sh" || true

# --- Repair printer_data ownership ---
#
# An in-app update runs under NoNewPrivileges and cannot fix a config directory
# an earlier root install left behind; this start-time root pass can. It comes
# last so anything restored above as root is covered too.
OWNER_HOME="$(getent passwd "${USER_VAL:-}" 2>/dev/null | cut -d: -f6)"
[ -f "${SCRIPT_DIR}/fix-user-config-owner.sh" ] && \
    sh "${SCRIPT_DIR}/fix-user-config-owner.sh" "${USER_VAL:-}" "${GROUP_VAL:-}" "${PD_HELIX_VAL:-${OWNER_HOME:+${OWNER_HOME}/printer_data/config/helixscreen}}" || true
