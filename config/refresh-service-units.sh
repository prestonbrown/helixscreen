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

template_unit() {
    local src="$1" dest="$2"
    cp "$src" "$dest" || return 1
    sed -i \
        -e "s|@@HELIX_USER@@|${USER_VAL:-root}|g" \
        -e "s|@@HELIX_GROUP@@|${GROUP_VAL:-root}|g" \
        -e "s|@@INSTALL_DIR@@|${IDIR}|g" \
        -e "s|@@INSTALL_PARENT@@|${PDIR}|g" \
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

# --- Restore config symlinks after Moonraker update ---
#
# Moonraker type:web does shutil.rmtree(INSTALL_DIR) then extracts a fresh ZIP.
# This destroys the symlinks from INSTALL_DIR/config/ → printer_data/config/helixscreen/
# and replaces them with default config files from the release ZIP.  The user's real
# config survives in printer_data (outside the managed path) but is orphaned.
#
# Re-create symlinks so the app reads the user's config instead of defaults.
# Runs as root (inherited from the calling service unit), so no sudo needed.

# Keep in sync with scripts/lib/installer/platform.sh.
HELIX_USER_CONFIG_FILES="settings.json helixscreen.env .disabled_services tool_spools.json crash_history.json"
HELIX_USER_CONFIG_DIRS="custom_images themes printer_database.d"

# Discover printer_data/config/helixscreen/ from the service user's home.
# Try the service user's home first, then scan common locations.
# Echoes the path, or nothing when none is found.
discover_pd_helix() {
    local user_home=""

    if [ -n "$USER_VAL" ] && [ "$USER_VAL" != "root" ]; then
        user_home=$(eval echo "~${USER_VAL}" 2>/dev/null || echo "")
    fi

    # Check user home first, then common Klipper user homes, then /root
    for candidate in \
        "${user_home}/printer_data/config/helixscreen" \
        /home/pi/printer_data/config/helixscreen \
        /home/biqu/printer_data/config/helixscreen \
        /home/mks/printer_data/config/helixscreen \
        /root/printer_data/config/helixscreen \
        /usr/data/printer_data/config/helixscreen; do
        [ -z "$candidate" ] && continue
        if [ -d "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 0
}

# Copy every entry under SRC missing from DST, never overwriting. Sorted so a
# parent directory is created before its children.
# Args: SRC DST — returns 0 when every entry now exists at DST.
merge_config_dir() {
    local src="$1" dst="$2" rel

    [ -d "$src" ] || return 0

    while IFS= read -r rel; do
        [ -n "$rel" ] && [ "$rel" != "." ] || continue
        rel="${rel#./}"
        if [ -d "${src}/${rel}" ] && [ ! -L "${src}/${rel}" ]; then
            [ -d "${dst}/${rel}" ] || mkdir -p "${dst}/${rel}" 2>/dev/null || return 1
        elif [ ! -e "${dst}/${rel}" ] && [ ! -L "${dst}/${rel}" ]; then
            cp -pR "${src}/${rel}" "${dst}/${rel}" 2>/dev/null || return 1
        fi
    done <<EOF
$(cd "$src" 2>/dev/null && find . 2>/dev/null | LC_ALL=C sort)
EOF
    return 0
}

# True when DIR holds at least one entry (including dotfiles). Cross-checks the
# find(1) walk: a walk reporting nothing about a demonstrably non-empty
# directory means the walk failed, and a failed walk must never be read as
# "everything copied".
dir_has_entries() {
    local d="$1" e
    [ -d "$d" ] || return 1
    for e in "$d"/* "$d"/.[!.]* "$d"/..?*; do
        [ -e "$e" ] || [ -L "$e" ] || continue
        return 0
    done
    return 1
}

# True when every entry under SRC has a counterpart under DST. The precondition
# for deleting SRC, checked independently of merge_config_dir's return code.
config_dir_fully_merged() {
    local src="$1" dst="$2" rel seen=""

    [ -d "$dst" ] || return 1

    while IFS= read -r rel; do
        [ -n "$rel" ] && [ "$rel" != "." ] || continue
        seen=1
        rel="${rel#./}"
        [ -e "${dst}/${rel}" ] || [ -L "${dst}/${rel}" ] || return 1
    done <<EOF
$(cd "$src" 2>/dev/null && find . 2>/dev/null)
EOF

    if [ -z "$seen" ] && dir_has_entries "$src"; then
        return 1
    fi
    return 0
}

# Last line of defence on the only rm -rf this script performs. IDIR is derived
# from the script's own location, so a script copied somewhere unexpected must
# not be able to point the delete at arbitrary user data.
# Args: INSTALL_CONFIG
install_config_path_ok() {
    local c="${1%/}"
    case "$c" in
        /*) ;;
        *) return 1 ;;
    esac
    case "$c" in
        *..*) return 1 ;;
    esac
    [ "${c##*/}" = "config" ] || return 1
    # Parent must be a HelixScreen install dir — same rule as validate_install_dir.
    case "${c%/config}" in
        *helixscreen*) return 0 ;;
    esac
    return 1
}

restore_config_symlinks() {
    local install_config="${IDIR}/config"
    [ -d "$install_config" ] || return 0

    local pd_helix
    pd_helix=$(discover_pd_helix)
    [ -n "$pd_helix" ] || return 0

    for file in $HELIX_USER_CONFIG_FILES; do
        local pd_file="${pd_helix}/${file}"
        local install_file="${install_config}/${file}"

        # Already a correct symlink — nothing to do
        if [ -L "$install_file" ]; then
            local target
            target=$(readlink "$install_file" 2>/dev/null || echo "")
            [ "$target" = "$pd_file" ] && continue
            rm -f "$install_file"
        fi

        # User's config exists in printer_data — replace the default with a symlink
        if [ -f "$pd_file" ]; then
            # Remove the default file shipped in the ZIP
            rm -f "$install_file" 2>/dev/null
            ln -s "$pd_file" "$install_file" 2>/dev/null || true
        fi
    done

    # --- Directories ---
    #
    # The ZIP always extracts these as real directories holding whatever the
    # release ships (themes/, printer_database.d/README.md). Fold anything the
    # user doesn't already have into printer_data — so a newly shipped theme
    # still reaches them — then replace the real directory with a symlink so the
    # NEXT rmtree unlinks a symlink instead of deleting their data.
    #
    # Copy, verify, then delete. Any failure leaves the real directory in place.
    for dir in $HELIX_USER_CONFIG_DIRS; do
        local pd_dir="${pd_helix}/${dir}"
        local install_dir_path="${install_config}/${dir}"

        install_config_path_ok "$install_config" || continue

        if [ -L "$install_dir_path" ]; then
            local dir_target
            dir_target=$(readlink "$install_dir_path" 2>/dev/null || echo "")
            if [ "$dir_target" = "$pd_dir" ]; then
                [ -d "$pd_dir" ] || mkdir -p "$pd_dir" 2>/dev/null || true
                continue
            fi
            rm -f "$install_dir_path" 2>/dev/null
        fi

        [ -d "$pd_dir" ] || mkdir -p "$pd_dir" 2>/dev/null || continue

        if [ -d "$install_dir_path" ] && [ ! -L "$install_dir_path" ]; then
            merge_config_dir "$install_dir_path" "$pd_dir" || continue
            config_dir_fully_merged "$install_dir_path" "$pd_dir" || continue
            rm -rf "$install_dir_path" 2>/dev/null || continue
        elif [ -e "$install_dir_path" ]; then
            continue   # a plain file where we expect a directory — not ours to delete
        fi

        ln -s "$pd_dir" "$install_dir_path" 2>/dev/null || true
    done
}

restore_config_symlinks
