#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Put the user's config back after Moonraker's type:web update replaced the
# install dir, on every platform.
#
# Moonraker rmtree()s the install dir and extracts a fresh zip. That removes the
# symlinks from config/ into printer_data/config/helixscreen/ and platform/hooks.sh.
# Two callers run this afterwards:
#   - refresh-service-units.sh (systemd), from the update watcher and service start
#   - helixscreen.init start() (SysV, and the K2 procd shim that delegates to it)
# It is idempotent, cheap when nothing changed, and never fails its caller.
#
# Reads USER_VAL (the service user, optional) from the environment. The install
# dir is the parent of this script's directory.
# HELIX_RESTORE_LIB_ONLY=1 defines the functions without running them.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIR="${IDIR:-$(dirname "$SCRIPT_DIR")}"
USER_VAL="${USER_VAL:-}"

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
HELIX_USER_CONFIG_FILES="settings.json helixscreen.env .disabled_services tool_spools.json pending_remap.json crash_history.json user_filaments.json .webcams_backup.json .camera_migrated .klipper_includes .k2cam_webcam_disabled .seeded_settings .platform_hook .nginx_webcam_accesslog .nginx_remote_screen .remote_screen_route"
HELIX_USER_CONFIG_DIRS="custom_images themes printer_database.d"

# Locate printer_data/config/helixscreen/.
#
# The installer records the directory it resolved (HELIX_PD_HELIX_DIR, from the
# init script or the systemd unit), and that record is the answer. The scan
# below only serves an install that predates the record; it lists the roots
# the installer can resolve a Klipper home to, most specific first. /root is
# last because the K2 and AD5M roots (/mnt/UDISK, /usr/data) sit beside a
# /root that is not where their config lives.
# Echoes the path, or nothing when none is found.
discover_pd_helix() {
    local user_home="" candidate

    if [ -n "${HELIX_PD_HELIX_DIR:-}" ] && [ -d "$HELIX_PD_HELIX_DIR" ]; then
        printf '%s' "$HELIX_PD_HELIX_DIR"
        return 0
    fi

    if [ -n "$USER_VAL" ] && [ "$USER_VAL" != "root" ]; then
        user_home=$(eval echo "~${USER_VAL}" 2>/dev/null || echo "")
    fi

    for candidate in \
        "${user_home}/printer_data/config/helixscreen" \
        /home/pi/printer_data/config/helixscreen \
        /home/biqu/printer_data/config/helixscreen \
        /home/mks/printer_data/config/helixscreen \
        /home/qidi/printer_data/config/helixscreen \
        /home/lava/printer_data/config/helixscreen \
        /usr/data/printer_data/config/helixscreen \
        /mnt/UDISK/printer_data/config/helixscreen \
        /etc/klipper/config/helixscreen \
        /root/printer_data/config/helixscreen; do
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

# Log a restore decision. The init script discards this script's stdout, so
# the syslog is the only place a skipped or saved copy is visible.
restore_log() {
    if command -v logger >/dev/null 2>&1; then
        logger -t helixscreen "restore-user-config: $*" 2>/dev/null || true
    fi
    echo "restore-user-config: $*"
}

# Write FILE's content to DEST through a temporary file, so DEST is never seen
# half-written. Plain cp: the copy is owned by whoever runs this, so DEST's
# owner is applied afterwards when it can be read.
# Args: SRC DEST [OWNER_REF]  (OWNER_REF is a file whose owner DEST takes)
atomic_copy() {
    local src="$1" dest="$2" ref="${3:-}" tmp owner
    tmp="${dest}.tmp"
    cp "$src" "$tmp" 2>/dev/null || { rm -f "$tmp" 2>/dev/null; return 1; }
    touch -r "$src" "$tmp" 2>/dev/null || true
    if [ -n "$ref" ] && [ -e "$ref" ]; then
        owner=$(stat -c %u:%g "$ref" 2>/dev/null || true)
        [ -n "$owner" ] && chown "$owner" "$tmp" 2>/dev/null || true
    fi
    mv -f "$tmp" "$dest" 2>/dev/null || { rm -f "$tmp" 2>/dev/null; return 1; }
    return 0
}

# Decide what to do with a real file in the install dir whose printer_data twin
# exists, before the real file is replaced by a symlink. printer_data always
# wins: the user's file is the one the app must read. The real file is kept as
# <name>.pre-restore only when something wrote it after the zip was extracted
# (it is newer than release_info.json, which the extraction writes), because
# otherwise it is the packaged copy and holds nothing of the user's. Such a
# copy does not replace an earlier one: that moves to <name>.pre-restore.1.
#
# A ledger (a dotfile that is not JSON) is line-oriented state both copies may
# have appended to, so its two copies are merged line by line into printer_data
# instead. A JSON dotfile is a snapshot, and printer_data keeps it.
#
# Never fails the caller: a copy that cannot be saved is logged and the real
# file is still replaced by the link, since the user's file must win.
# Args: NAME REAL PD
reconcile_real_file() {
    local name="$1" real="$2" pd="$3" merged

    cmp -s "$real" "$pd" 2>/dev/null && return 0

    case "$name" in
        *.json) ;;
        .*)
            merged="${pd}.merge"
            if sort -u "$pd" "$real" > "$merged" 2>/dev/null \
                && atomic_copy "$merged" "$pd" "$pd"; then
                restore_log "merged $name into printer_data"
            else
                restore_log "could not merge $name; printer_data copy kept"
            fi
            rm -f "$merged" 2>/dev/null
            return 0
            ;;
    esac

    if [ -f "${IDIR}/release_info.json" ] \
        && [ -z "$(find "$real" -newer "${IDIR}/release_info.json" 2>/dev/null)" ]; then
        restore_log "dropped packaged $name (older than the release)"
        return 0
    fi

    if [ -e "${pd}.pre-restore" ]; then
        mv -f "${pd}.pre-restore" "${pd}.pre-restore.1" 2>/dev/null \
            || rm -f "${pd}.pre-restore" 2>/dev/null
    fi
    if atomic_copy "$real" "${pd}.pre-restore"; then
        restore_log "kept the install-dir $name as $(basename "$pd").pre-restore"
    else
        restore_log "could not save the install-dir $name; printer_data copy kept"
    fi
    return 0
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

        # A dotfile is runtime state no release zip ships. A real one here with
        # nothing in printer_data is a ledger written before it was relocated:
        # move it across so the next rmtree cannot take it. Only dotfiles: this
        # runs after a zip extraction, so any other real file here is the
        # release's default, not the user's (the installer's own migration,
        # which runs on a tree the user has been editing, moves every file).
        case "$file" in
            .*)
                if [ ! -e "$pd_file" ] && [ -f "$install_file" ] && [ ! -L "$install_file" ]; then
                    cp -p "$install_file" "$pd_file" 2>/dev/null || continue
                fi
                ;;
        esac

        # User's config exists in printer_data — replace the real file with a
        # symlink, after keeping whatever it held that printer_data lacks.
        if [ -f "$pd_file" ]; then
            if [ -f "$install_file" ] && [ ! -L "$install_file" ]; then
                reconcile_real_file "$file" "$install_file" "$pd_file"
            fi
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

# Moonraker's rmtree also deletes platform/hooks.sh, which only the installer
# writes. Without it every platform_* function is the init script's no-op stub
# (the stock UI is no longer stopped, the cache dir falls back to tmpfs). The
# installer records which hook key it deployed in config/.platform_hook, a user
# config file restored above; the matching template ships in the release.
restore_platform_hooks() {
    local hooks="${IDIR}/platform/hooks.sh" keyfile="${IDIR}/config/.platform_hook"
    local key src owner

    [ -s "$hooks" ] && return 0
    [ -f "$keyfile" ] || return 0
    key=$(head -n1 "$keyfile" 2>/dev/null | tr -d ' \r\n')
    case "$key" in
        ""|*[!A-Za-z0-9_-]*) return 0 ;;
    esac
    src="${IDIR}/assets/config/platform/hooks-${key}.sh"
    [ -f "$src" ] || return 0

    mkdir -p "${IDIR}/platform" 2>/dev/null || return 0
    cp "$src" "$hooks" 2>/dev/null || return 0
    chmod +x "$hooks" 2>/dev/null || true
    owner=$(stat -c %u:%g "$IDIR" 2>/dev/null || true)
    if [ -n "$owner" ]; then
        chown "$owner" "${IDIR}/platform" "$hooks" 2>/dev/null || true
    fi
}

if [ -z "${HELIX_RESTORE_LIB_ONLY:-}" ]; then
    restore_config_symlinks || true
    restore_platform_hooks || true
    exit 0
fi
