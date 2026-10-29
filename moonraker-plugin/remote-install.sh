#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck disable=SC3043  # `local` in the helpers copied from the main installer; BusyBox ash and dash have it
#
# HelixPrint Moonraker plugin: install or remove it on a Moonraker host that
# HelixScreen does not run on. The app shows this command pinned to its own
# release, so the plugin always matches the app talking to it:
#
#   curl -fsSL https://raw.githubusercontent.com/prestonbrown/helixscreen/<REF>/moonraker-plugin/remote-install.sh | sh -s -- <REF>
#   ... | sh -s -- <REF> --uninstall
#
# <REF> is a release tag (v1.2.0) or main. The plugin files of that ref go to
# HELIX_PLUGIN_DIR (~/helix_print), and their own install.sh does the rest, the
# same routine the app runs on a local install: Moonraker and config discovery,
# [include] handling, backups and the [helix_print] section. Needs sh and one of
# curl, wget or python3; no git, no bash.
#
# No [update_manager helix_print] is written: Moonraker's updater follows a git
# branch or a release zip, neither of which is the plugin of the app's release.
# After the app updates, the command it shows is pinned to the new release.

set -e

PLUGIN_BASE_URL="${HELIX_PLUGIN_BASE_URL:-https://raw.githubusercontent.com/prestonbrown/helixscreen}"
PLUGIN_DIR="${HELIX_PLUGIN_DIR:-$HOME/helix_print}"

# Everything install.sh and the plugin need from moonraker-plugin/. A bats test
# fails when a shipped .py or .sh is missing here.
PLUGIN_FILES="install.sh helix_print_conf.sh helix_print.py helix_3mf.py helix_analysis.py helix_capabilities.py helix_companions.py helix_config.py helix_edits.py helix_file_queue.py helix_filament.py helix_fitcheck.py helix_gcode_reader.py helix_journal.py helix_photos.py helix_plr.py helix_plr_qidi.py helix_plr_render.py helix_plr_scan.py helix_print_hooks.py helix_printer_cfg.py helix_storage.py helix_vendor_ace.py helix_vendor_ad5x.py helix_vendor_push.py strip_phase_tracking.py klippy/helix_pa_fit.py klippy/helix_pa_gauge.py"

info() { printf '[INFO] %s\n' "$1"; }
error() {
    printf '[ERROR] %s\n' "$1" >&2
    exit 1
}

# The fetch helpers below are the main installer's own
# (scripts/lib/installer/release.sh, common.sh): this script runs piped from
# GitHub and cannot source them. A bats test holds each copy equal.

# Detect whether `curl` is curl proper, with the flags we pass it. Creality K1
# firmware ships libhv's example client as /usr/bin/curl: it answers to the name,
# prints "curl version 1.0.0" and rejects -s, -S, -L, -o and -f. Real curl's first
# --version output starts "curl <digit>...". Shell builtins only, so it holds on
# a PATH with no head or grep. Cache the result so we only probe once.
_REAL_CURL=""
_has_real_curl() {
    if [ -z "$_REAL_CURL" ]; then
        _REAL_CURL=no
        if command -v curl >/dev/null 2>&1; then
            case "$(curl --version 2>/dev/null)" in
                "curl "[0-9]*) _REAL_CURL=yes ;;
            esac
        fi
    fi
    [ "$_REAL_CURL" = "yes" ]
}

_INSTALLER_UA="helixscreen-installer/1.0"

_PY_BIN=""
_PY_PROBED=""
_has_python() {
    if [ -z "$_PY_PROBED" ]; then
        _PY_PROBED=1
        for _cand in python3 python; do
            if command -v "$_cand" >/dev/null 2>&1 && \
               "$_cand" -c 'import urllib.request' >/dev/null 2>&1; then
                _PY_BIN="$_cand"
                break
            fi
        done
    fi
    [ -n "$_PY_BIN" ]
}

_py_get() {
    _has_python || return 1
    HELIX_PY_URL="$1" HELIX_PY_DEST="$2" HELIX_PY_UA="$_INSTALLER_UA" \
        HELIX_PY_TIMEOUT="${3:-300}" "$_PY_BIN" - <<'PYEOF'
import os, sys, urllib.request, shutil
url = os.environ["HELIX_PY_URL"]
dest = os.environ["HELIX_PY_DEST"]
ua = os.environ["HELIX_PY_UA"]
try:
    timeout = float(os.environ.get("HELIX_PY_TIMEOUT", "300"))
except ValueError:
    timeout = 300.0
try:
    req = urllib.request.Request(url, headers={"User-Agent": ua})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        if dest == "-":
            shutil.copyfileobj(resp, sys.stdout.buffer)
        else:
            with open(dest, "wb") as out:
                shutil.copyfileobj(resp, out)
except Exception:
    sys.exit(1)
PYEOF
}

_py_download() { _py_get "$1" "$2" "${3:-300}"; }

# Download URL @1 to file @2. Each transport falls through to the next when it
# gets nothing: curl proper, then wget (BusyBox's may lack TLS), then python's
# urllib, which every Moonraker host has. An HTTP error is a failure on all
# three, so a 404 page never stands in for a file.
fetch_file() {
    rm -f "$2"
    if _has_real_curl && curl -fsSL --connect-timeout 10 --max-time 120 -A "$_INSTALLER_UA" \
        -o "$2" "$1" 2>/dev/null && [ -s "$2" ]; then
        return 0
    fi
    if command -v wget >/dev/null 2>&1 && wget -q -T 60 -O "$2" "$1" 2>/dev/null && [ -s "$2" ]; then
        return 0
    fi
    if _py_download "$1" "$2" 120 && [ -s "$2" ]; then
        return 0
    fi
    rm -f "$2"
    return 1
}

# The git ref of release @1: a version becomes its tag (v1.2.0-beta.3), with or
# without the leading v, build metadata dropped; anything else (main, dev) is
# main. The app's plugin_release_ref() holds the same rule.
release_ref() {
    _rr="${1#v}"
    case "$_rr" in
        [0-9]*) printf 'v%s' "${_rr%%+*}" ;;
        *) printf 'main' ;;
    esac
}

# Fetch the plugin files of ref @1 into the empty directory @2.
fetch_plugin() {
    for _fp_file in $PLUGIN_FILES; do
        mkdir -p "$(dirname "$2/$_fp_file")" || error "Could not create the directory of $_fp_file"
        fetch_file "$PLUGIN_BASE_URL/$1/moonraker-plugin/$_fp_file" "$2/$_fp_file" ||
            error "Could not download $_fp_file of $1 from $PLUGIN_BASE_URL"
    done
    grep -q '^PLUGIN_VERSION = ' "$2/helix_print.py" ||
        error "The downloaded helix_print.py is not the HelixPrint plugin"
    [ "$(head -n 1 "$2/install.sh")" = "#!/bin/sh" ] ||
        error "The downloaded install.sh is not the plugin installer"
}

# Whether @1 holds a plugin this script installed: the release files, or the
# git checkout an older remote install cloned there.
is_plugin_dir() {
    [ -f "$1/helix_print.py" ] || [ -f "$1/moonraker-plugin/helix_print.py" ]
}

# What place_plugin moved aside. It stays until the install has taken, since
# the plugin link still names a file in it.
PLUGIN_OLD="$PLUGIN_DIR.old.$$"

# Put the files staged in @1 in PLUGIN_DIR, in place of what was there.
place_plugin() {
    if [ -e "$PLUGIN_DIR" ]; then
        mv "$PLUGIN_DIR" "$PLUGIN_OLD" || error "Could not move $PLUGIN_DIR aside"
    fi
    if ! mv "$1" "$PLUGIN_DIR"; then
        [ ! -e "$PLUGIN_OLD" ] || mv "$PLUGIN_OLD" "$PLUGIN_DIR"
        error "Could not create $PLUGIN_DIR"
    fi
}

# Undo place_plugin after a failed install: the new files go back to the
# stage @1, which the exit trap removes, and what was there returns.
restore_plugin() {
    [ -e "$PLUGIN_OLD" ] || return 0
    mv "$PLUGIN_DIR" "$1" && mv "$PLUGIN_OLD" "$PLUGIN_DIR"
}

main() {
    ref=""
    uninstall=false
    for arg in "$@"; do
        case "$arg" in
            --uninstall) uninstall=true ;;
            -*) error "Unknown option: $arg" ;;
            *) ref=$(release_ref "$arg") ;;
        esac
    done
    [ -n "$ref" ] || error "Usage: remote-install.sh <release, e.g. v1.2.0> [--uninstall]"

    if [ -e "$PLUGIN_DIR" ] && ! is_plugin_dir "$PLUGIN_DIR"; then
        error "$PLUGIN_DIR exists and holds no HelixPrint plugin - move it aside first"
    fi

    # Staged beside PLUGIN_DIR so placing it is a rename on one filesystem.
    mkdir -p "$(dirname "$PLUGIN_DIR")"
    stage="$(dirname "$PLUGIN_DIR")/.helix_print.stage.$$"
    rm -rf "$stage"
    mkdir "$stage" || error "Could not create $stage"
    trap 'rm -rf "$stage"' EXIT
    trap 'exit 1' INT TERM HUP

    info "Downloading the HelixPrint plugin ($ref)"
    fetch_plugin "$ref" "$stage"

    # install.sh reads nothing from stdin, which here is the rest of this script.
    if [ "$uninstall" = true ]; then
        status=0
        sh "$stage/install.sh" --uninstall-auto < /dev/null || status=$?
        # 0 removed, 2 removed with a config file needing a look; anything
        # else stopped part-way (an error, a missing command, ^C).
        case "$status" in
            0 | 2) ;;
            *) error "Uninstall failed (exit $status) - $PLUGIN_DIR left in place" ;;
        esac
        if is_plugin_dir "$PLUGIN_DIR"; then
            rm -rf "$PLUGIN_DIR"
            info "Removed $PLUGIN_DIR"
        fi
        exit "$status"
    fi

    place_plugin "$stage"
    status=0
    sh "$PLUGIN_DIR/install.sh" --auto < /dev/null || status=$?
    if [ "$status" -ne 0 ]; then
        had_previous=false
        [ -e "$PLUGIN_OLD" ] && had_previous=true
        if restore_plugin "$stage"; then
            if [ "$had_previous" = true ]; then
                printf '[ERROR] %s\n' "Install failed - the previous $PLUGIN_DIR is back in place" >&2
            else
                printf '[ERROR] %s\n' "Install failed" >&2
            fi
        else
            printf '[ERROR] %s\n' "Install failed, and the previous plugin could not be put back from $PLUGIN_OLD" >&2
        fi
        exit "$status"
    fi
    rm -rf "$PLUGIN_OLD"
}

# Called last, so a download cut short runs nothing.
main "$@"
