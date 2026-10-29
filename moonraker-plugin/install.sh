#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixPrint Moonraker Plugin Installer
#
# This script installs the helix_print Moonraker plugin.
#
# Usage:
#   ./install.sh              # Interactive install (manual config steps)
#   ./install.sh --auto       # Full auto-install (updates config, restarts Moonraker)
#   ./install.sh --uninstall  # Remove the plugin
#
# On a Moonraker host HelixScreen does not run on, remote-install.sh fetches
# a release's plugin files and runs this script from them.

set -e

# Get script directory (POSIX-compatible)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_FILE="$SCRIPT_DIR/helix_print.py"
AUTO_MODE=false

# Exit codes for --uninstall and --uninstall-auto:
#   0 - uninstalled; nothing left in the config to clean up
#   2 - the plugin was removed, but the phase-tracking strip skipped or
#       failed a config file, which needs a human to look at it
#   1 (or any other nonzero) - the uninstall itself failed; the plugin may
#       still be present
EXIT_UNINSTALL_OK=0
EXIT_UNINSTALL_FAILED=1
EXIT_UNINSTALL_NEEDS_ATTENTION=2

# Exit codes for --relink (HelixPluginInstaller reads them):
#   0 - the link already names this tree's plugin; nothing done
#   3 - the link was re-pointed at this tree's plugin
#   4 - no plugin of ours to repair (no link, a regular file, or no
#       [helix_print] section); nothing done
#   1 - the link could not be repaired
EXIT_RELINK_CURRENT=0
EXIT_RELINK_DONE=3
EXIT_RELINK_NOT_OURS=4

# Exit codes for --restart-moonraker (HelixPluginInstaller reads them), those
# of moonraker_restart in helix_print_conf.sh:
#   0 - Moonraker restarted and runs this tree's plugin
#   3 - Moonraker needs a manual restart: it refused, did not restart, did
#       not load the plugin, or runs the old one
#   4 - a print is printing or paused; nothing restarted
#   6 - the print state could not be read; nothing restarted
#   7 - Moonraker does not answer

# Colors for output (works with printf)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() {
    printf "${GREEN}[INFO]${NC} %s\n" "$1"
}

warn() {
    printf "${YELLOW}[WARN]${NC} %s\n" "$1"
}

error() {
    printf "${RED}[ERROR]${NC} %s\n" "$1"
    exit 1
}

# helix_print_section_files and friends: which config files hold our sections.
[ -f "$SCRIPT_DIR/helix_print_conf.sh" ] || error "Missing $SCRIPT_DIR/helix_print_conf.sh"
# shellcheck source=moonraker-plugin/helix_print_conf.sh
. "$SCRIPT_DIR/helix_print_conf.sh"

# Moonraker source roots on known vendor layouts. Keep in step with
# MOONRAKER_SRC_PATHS in scripts/lib/installer/moonraker.sh; a bats test
# fails when an entry there is missing here.
MOONRAKER_SRC_PATHS="
/home/pi/moonraker
/home/biqu/moonraker
/home/mks/moonraker
/home/qidi/moonraker
/home/klipper/moonraker
/home/lava/moonraker
/root/moonraker
/root/printer_software/moonraker
/usr/data/moonraker
/usr/share/moonraker
/mnt/UDISK/moonraker
/userdata/moonraker
/opt/moonraker
"

# Config directories on known vendor layouts, the directories of
# MOONRAKER_CONF_PATHS in scripts/lib/installer/moonraker.sh.
MOONRAKER_CONFIG_DIRS="
/home/pi/printer_data/config
/home/pi/klipper_config
/home/biqu/printer_data/config
/home/mks/printer_data/config
/home/qidi/printer_data/config
/root/printer_data/config
/opt/config/printer_data/config
/opt/config
/usr/data/config/printer_data/config
/usr/data/printer_data/config
/etc/klipper/config
"

# Print Moonraker's components directory under a source root. Three layouts:
#   <root>/moonraker/components            git checkout (~/moonraker)
#   <root>/components                      the package directory itself
#   <root>/moonraker/moonraker/components  Creality K1/K2: the repo nested
#                                          inside an install directory
components_under() {
    for sub in "$1/moonraker/components" "$1/components" "$1/moonraker/moonraker/components"; do
        if [ -d "$sub" ]; then
            printf '%s' "$sub"
            return 0
        fi
    done
    return 1
}

# Translate a path on this side into the same file as the running Moonraker
# names it: strip its chroot prefix when the path lies inside the chroot. A
# path outside it is only visible to Moonraker through a bind mount at the
# same place (Forge-X binds /opt/config), which the caller checks.
path_for_moonraker() {
    mroot=$(running_moonraker_root)
    case "$1" in
        "$mroot"/*) [ -n "$mroot" ] && { printf '%s' "${1#"$mroot"}"; return 0; } ;;
    esac
    printf '%s' "$1"
}

# Print path @1 as the running Moonraker names it (path_for_moonraker), or
# fail when nothing exists there from Moonraker's root: a path outside its
# chroot that no bind mount brings in.
moonraker_visible_path() {
    seen=$(path_for_moonraker "$1")
    [ -e "$(running_moonraker_root)$seen" ] || return 1
    printf '%s' "$seen"
}

# Print the python3 inside the running Moonraker's chroot, as the chroot
# names it. Fails when Moonraker runs in no chroot, when chroot(8) is not
# here to enter it, or when the chroot holds no python3.
chroot_python3() {
    mroot=$(running_moonraker_root)
    [ -n "$mroot" ] && command -v chroot > /dev/null 2>&1 || return 1
    for py in /usr/bin/python3 /bin/python3 /usr/local/bin/python3; do
        if [ -x "$mroot$py" ]; then
            printf '%s' "$py"
            return 0
        fi
    done
    return 1
}

# Whether a process with ids @1 (uid, then groups) holds permission @2 (r
# or x) on @3, by its mode bits: the owner's, else a group's, else other's.
# uid 0 holds both.
ids_may() {
    # shellcheck disable=SC2012  # BusyBox find has no -printf; ls -n is the portable numeric owner and mode
    ls -ldn "$3" 2>/dev/null | awk -v ids="$1" -v want="$2" '
        NR == 1 {
            n = split(ids, id, " ")
            if (id[1] == 0) { ok = 1; exit }
            if ($3 == id[1]) bits = substr($1, 2, 3)
            else {
                bits = substr($1, 8, 3)
                for (i = 2; i <= n; i++) if (id[i] == $4) { bits = substr($1, 5, 3); break }
            }
            c = substr(bits, want == "r" ? 1 : 3, 1)
            ok = (want == "r") ? (c == "r") : (c == "x" || c == "s" || c == "t")
            exit
        }
        END { exit !ok }'
}

# Print the first path that keeps a Moonraker with ids @1 (running_moonraker_ids)
# from loading the plugin at @2 (its path from here): a directory above it
# Moonraker cannot search, or a .py beside it Moonraker cannot read. Python
# imports the plugin's siblings from that directory too. Prints nothing when
# every one is readable, or when @1 is empty (no Moonraker runs here).
moonraker_cannot_read() {
    ids="$1"
    shift
    [ -n "$ids" ] || return 0
    real=$(readlink -f "$1" 2>/dev/null) || real="$1"
    dir=$(dirname "$real")
    mroot=$(running_moonraker_root)
    walk=""
    rest="${dir#/}"
    while [ -n "$rest" ]; do
        walk="$walk/${rest%%/*}"
        case "$rest" in */*) rest="${rest#*/}" ;; *) rest="" ;; esac
        # Moonraker in a chroot searches only the directories inside it.
        case "$walk/" in "$mroot"/*) ;; *) continue ;; esac
        if ! ids_may "$ids" x "$walk"; then
            printf '%s' "$walk"
            return 0
        fi
    done
    for py in "$real" "$dir"/*.py; do
        [ -f "$py" ] || continue
        if ! ids_may "$ids" r "$py"; then
            printf '%s' "$py"
            return 0
        fi
    done
}

# Stop through error() when the running Moonraker cannot load the plugin
# from @1, naming its user and what blocks it.
require_moonraker_can_read() {
    ids=$(running_moonraker_ids)
    blocked=$(moonraker_cannot_read "$ids" "$1")
    [ -n "$blocked" ] || return 0
    uid=${ids%% *}
    user=$(awk -F: -v u="$uid" '$3 == u { print $1; exit }' /etc/passwd 2>/dev/null)
    error "Moonraker runs as ${user:-uid $uid}, which cannot read $blocked, so it cannot load the plugin.
Give that user read access to the plugin files and search access to each directory above them, or install HelixScreen where it can reach them. Nothing was changed."
}

# Whether a link can be made in directory @1: a probe link is made there and
# removed. test -w alone passes for root on a read-only filesystem.
can_link_in() {
    probe="$1/.helix_print.probe.$$"
    ln -s "$PLUGIN_FILE" "$probe" 2>/dev/null || return 1
    rm -f "$probe"
}

# The pre-flights every link to the plugin takes before it is made in
# components directory @1: a link can be made there, Moonraker sees the plugin
# from its root, and it can read what the link names. Sets link_to to the
# plugin's path as Moonraker names it, which differs from ours when Moonraker
# runs in a chroot. Stops through error() when one fails.
check_plugin_link() {
    can_link_in "$1" || error "Cannot write to components directory: $1"
    # shellcheck disable=SC2034  # read by the caller that makes the link
    link_to=$(moonraker_visible_path "$PLUGIN_FILE") ||
        error "Moonraker cannot see $PLUGIN_FILE from its root ($(running_moonraker_root))"
    require_moonraker_can_read "$PLUGIN_FILE"
}

# Find Moonraker's components directory. An explicit path wins, then the
# user's home, then the Moonraker that is actually running (whatever its
# layout), then the known vendor roots, then a pip-installed package.
find_moonraker_components() {
    for root in "$1" "$HOME/moonraker" "$HOME/klipper_config/moonraker"; do
        if [ -n "$root" ] && components_under "$root"; then
            return 0
        fi
    done

    script=$(running_moonraker_args | awk '/moonraker\.py$/ { print; exit }')
    if [ -n "$script" ]; then
        script="$(running_moonraker_root)$script"
        if [ -d "$(dirname "$script")/components" ]; then
            printf '%s' "$(dirname "$script")/components"
            return 0
        fi
    fi

    for root in $MOONRAKER_SRC_PATHS; do
        if components_under "$root"; then
            return 0
        fi
    done

    pip_loc=$(python3 -c "import moonraker; print(moonraker.__path__[0])" 2>/dev/null || true)
    if [ -n "$pip_loc" ] && [ -d "$pip_loc/components" ]; then
        printf '%s' "$pip_loc/components"
    fi
}

# Find the directory holding moonraker.conf: the user's home first, then the
# conf the running Moonraker loads, then the known vendor layouts. A directory,
# not a file, because helix_conf_file and its callers name <dir>/moonraker.conf;
# a running conf under another basename fails the check below and the next
# candidate is tried.
find_config_dir() {
    conf=$(running_moonraker_conf)
    for loc in \
        "$HOME/printer_data/config" \
        "$HOME/klipper_config" \
        "${conf:+$(dirname "$conf")}" \
        $MOONRAKER_CONFIG_DIRS
    do
        if [ -n "$loc" ] && [ -d "$loc" ] && [ -f "$loc/moonraker.conf" ]; then
            printf '%s' "$loc"
            return 0
        fi
    done
    printf ''
}

# Print the file the [helix_print] section belongs in, given the directory
# holding moonraker.conf. Normally moonraker.conf itself. A firmware mod that
# owns moonraker.conf (a symlink into the mod's own tree, which its updates
# rewrite) includes a user.moonraker.conf for the user's sections: Forge-X's
# `[include mod_data/user.moonraker.conf]`. The section goes there instead.
helix_conf_file() {
    conf="$1/moonraker.conf"
    if [ -L "$conf" ]; then
        # Trailing blanks after "]" include the CR of a file saved on Windows;
        # neither changes what Moonraker includes.
        inc=$(sed -n 's/^\[include[[:space:]]\{1,\}\([^]*?]*user\.moonraker\.conf\)[[:space:]]*\][[:space:]]*$/\1/p' "$conf" 2>/dev/null |
            tail -n 1)
        if [ -n "$inc" ]; then
            case "$inc" in
                /*) printf '%s' "$(running_moonraker_root)$inc" ;;
                *) printf '%s' "$1/$inc" ;;
            esac
            return 0
        fi
    fi
    printf '%s' "$conf"
}

# Print the running Klipper's extras/virtual_sdcard.py, the code that decides
# which files SDCARD_PRINT_FILE can open. Empty when no Klipper runs here.
running_klipper_sdcard_source() {
    for f in "${HELIX_PROC_ROOT:-/proc}"/[0-9]*/cmdline; do
        [ -r "$f" ] || continue
        klippy=$(tr '\000' '\n' < "$f" 2>/dev/null | awk '/klippy\/klippy\.py$/ { print; exit }')
        [ -n "$klippy" ] || continue
        root=$(readlink "$(dirname "$f")/root" 2>/dev/null) || root=""
        [ "$root" = "/" ] && root=""
        for sdcard in "${root%/}$(dirname "$klippy")/extras/virtual_sdcard.py" \
                      "$(dirname "$klippy")/extras/virtual_sdcard.py"; do
            if [ -f "$sdcard" ]; then
                printf '%s' "$sdcard"
                return 0
            fi
        done
        return 0
    done
}

# Generic probe: a virtual_sdcard that prunes dot-directories from its walk
# with str.startswith, the idiomatic spelling of the filter.
sdcard_prunes_dot_dirs_generic() {
    grep -Eq "dirs\[:\] *= *\[d for d in dirs if not d\.startswith\(['\"]\.['\"]\)" "$1"
}

# Vendor probe, Forge-X: its Klipper patch "Ignore hidden files and
# directories" (ff5m commit aab9d340) spells the filter as an index compare.
sdcard_prunes_dot_dirs_forgex() {
    grep -Eq "dirs\[:\] *= *\[d for d in dirs if not d\[0\] *== *['\"]\.['\"]" "$1"
}

# Capability: whether the running Klipper hides dot-directories from
# SDCARD_PRINT_FILE. There a print started through the plugin's symlink under
# a hidden directory fails with "Unable to open file", so the symlink directory
# has to be a visible one.
klipper_hides_dot_dirs() {
    sdcard=$(running_klipper_sdcard_source)
    [ -n "$sdcard" ] || return 1
    for probe in sdcard_prunes_dot_dirs_generic sdcard_prunes_dot_dirs_forgex; do
        "$probe" "$sdcard" && return 0
    done
    return 1
}

# Give [helix_print] in @1 a visible `symlink_dir: helix_print`, unless the
# section already sets symlink_dir (the user's value stands).
ensure_visible_symlink_dir() {
    file="$1"
    if awk '/^\[helix_print\]/ { s = 1; next } /^\[/ { s = 0 }
            s && /^[[:space:]]*symlink_dir[[:space:]]*[:=]/ { found = 1 }
            END { exit !found }' "$file"; then
        info "Keeping the symlink_dir already set in $(basename "$file")"
        return 0
    fi
    info "Klipper skips hidden directories here: setting symlink_dir: helix_print"
    helix_rewrite_conf "$file" \
        '{ print } !done && /^\[helix_print\]/ { print "symlink_dir: helix_print"; done = 1 }'
}

# Set `enabled: True` in place of any `enabled` option in @1's [helix_print]
# section that reads as false to Moonraker (false, no, off or 0, any case),
# with a backup. A file with no such line is left untouched. Exits through
# error() when the file needs the edit and cannot take it.
enable_helix_section() {
    file="$1"
    # shellcheck disable=SC2016  # awk's own $ fields
    _eh_disabled='/^\[/ { s = ($0 ~ /^\[helix_print\]/) }
        s && tolower($0) ~ /^[ \t]*enabled[ \t]*[:=][ \t]*(false|no|off|0)[ \t\r]*$/'
    awk "$_eh_disabled { found = 1 } END { exit !found }" "$file" || return 0
    backup_file=$(backup_config "$file") || error "Could not back up $file"
    info "Created backup: $backup_file"
    info "Turning the plugin back on: enabled: True in $(basename "$file")"
    helix_rewrite_conf "$file" "$_eh_disabled { print \"enabled: True\"; next } { print }" ||
        error "Could not write $file - see $backup_file"
}

# Copy config file @1 to @1.bak.helix.<timestamp>, keeping only that newest of
# the backups this script makes of it, and print the backup's path. Backups
# anyone else made are never touched. Returns 1 when the copy failed; older
# backups are then left alone.
backup_config() {
    _bk_new="$1.bak.helix.$(date +%Y%m%d_%H%M%S)"
    cp "$1" "$_bk_new" || return 1
    for _bk_old in "$1".bak.helix.[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9]; do
        if [ -f "$_bk_old" ] && [ "$_bk_old" != "$_bk_new" ]; then
            rm -f "$_bk_old"
        fi
    done
    printf '%s' "$_bk_new"
}

# Remove the sections whose header matches extended regex @2 from config file
# @1 with awk program @3 (helix_strip_awk), with a backup. Returns 1 when the
# file had such a section and could not be edited.
remove_helix_section() {
    file="$1"
    [ -f "$file" ] && grep -Eq "$2" "$file" || return 0
    if ! backup_file=$(backup_config "$file"); then
        warn "Could not back up $(basename "$file") - leaving its HelixPrint sections in place"
        return 1
    fi
    info "Created backup: $backup_file"
    info "Removing HelixPrint sections from $(basename "$file")"
    if ! helix_rewrite_conf "$file" "$3"; then
        warn "Could not remove HelixPrint sections from $(basename "$file") - see $backup_file"
        return 1
    fi
    return 0
}

# Remove [update_manager helix_print] from every file Moonraker reads its
# config from, config directory @1 included, each with a backup. That entry
# updates a git checkout of the plugin, which Moonraker no longer loads once
# the link names this tree. Returns 1 when any file could not be edited.
remove_plugin_updater_entries() {
    _rpu_status=0
    for _rpu_file in $(helix_conf_files_matching "$1/moonraker.conf" "$(running_moonraker_root)" "$HELIX_PRINT_UPDATER_RE"); do
        remove_helix_section "$_rpu_file" "$HELIX_PRINT_UPDATER_RE" "$HELIX_PRINT_UPDATER_STRIP_AWK" || _rpu_status=1
    done
    return "$_rpu_status"
}

# Main installation
main() {
    info "HelixPrint Moonraker Plugin Installer"
    printf '\n'

    # Check plugin file exists
    if [ ! -f "$PLUGIN_FILE" ]; then
        error "Plugin file not found: $PLUGIN_FILE"
    fi

    # Find Moonraker
    components_dir=$(find_moonraker_components "$1")

    if [ -z "$components_dir" ]; then
        error "Could not find Moonraker installation.
Please provide the path: ./install.sh /path/to/moonraker"
    fi

    info "Components directory: $components_dir"
    printf '\n'

    # Create symlink
    target="$components_dir/helix_print.py"

    if [ -f "$target" ] && [ ! -L "$target" ]; then
        error "A file (not symlink) exists at $target
Please remove it manually before installing."
    fi

    check_plugin_link "$components_dir"
    # Use ln -sf for atomic replacement (removes existing symlink first)
    ln -sf "$link_to" "$target"
    info "Created symlink: $target -> $link_to"
    printf '\n'

    # Remind about configuration
    info "Installation complete!"
    printf '\n'
    printf '%s\n' "Next steps:"
    printf '%s\n' "  1. Add the following to your moonraker.conf:"
    printf '\n'
    printf '%s\n' "     [helix_print]"
    printf '%s\n' "     # enabled: True"
    printf '%s\n' "     # temp_dir: .helix_temp"
    printf '%s\n' "     # symlink_dir: .helix_print"
    printf '%s\n' "     # cleanup_delay: 86400"
    printf '\n'
    printf '%s\n' "  2. Restart Moonraker (from Mainsail/Fluidd, or reboot the printer)"
    printf '\n'
    printf '%s\n' "  3. Verify the plugin is loaded:"
    config_dir=$(find_config_dir)
    printf '%s\n' "     curl $(moonraker_local_url "$config_dir/moonraker.conf" "$(running_moonraker_root)")/server/helix/status"
    printf '\n'
}

# Uninstall function (interactive)
uninstall() {
    info "Uninstalling HelixPrint plugin..."
    exit_status=$EXIT_UNINSTALL_OK

    components_dir=$(find_moonraker_components "$1")

    if [ -z "$components_dir" ]; then
        error "Could not find Moonraker installation."
    fi

    target="$components_dir/helix_print.py"

    if [ -L "$target" ]; then
        rm "$target"
        info "Removed symlink: $target"
    elif [ -f "$target" ]; then
        warn "Found regular file (not symlink) at $target"
        warn "Please remove it manually if desired."
    else
        info "Plugin symlink not found (already uninstalled?)"
    fi

    config_dir=$(find_config_dir)
    if [ -n "$config_dir" ]; then
        # The plugin itself is already gone at this point, so any nonzero
        # here - a skipped file or a failed one - is "needs attention", not
        # a failed uninstall.
        strip_phase_tracking_instrumentation "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
        remove_plr_mesh_include "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
        remove_klipper_companions "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
    fi

    printf '\n'
    printf '%s\n' "Don't forget to:"
    printf '%s\n' "  1. Remove [helix_print] section from moonraker.conf"
    printf '%s\n' "  2. Restart Moonraker (from Mainsail/Fluidd, or reboot the printer)"

    return "$exit_status"
}

# The version in this tree's helix_print.py, its PLUGIN_VERSION line.
bundled_plugin_version() {
    sed -n 's/^PLUGIN_VERSION = "\([^"]*\)"[[:space:]]*$/\1/p' "$PLUGIN_FILE" 2>/dev/null | awk 'NR == 1'
}

# The restart an auto install (@1 install) or uninstall (@1 uninstall) ends
# with, through moonraker_restart at MOONRAKER_URL: an install needs the
# plugin_restart_mode restart, and a config one when Moonraker runs this
# tree's plugin already; an uninstall a config one. --no-restart leaves it to
# the caller (the app restarts Moonraker over its own API). Returns 0 when
# Moonraker restarted, 2 when it does not answer, and 1 when it still needs a
# restart; moonraker_restart has said why.
finish_with_restart() {
    if [ "$NO_RESTART" = "true" ]; then
        info "Leaving the Moonraker restart to the caller"
        return 0
    fi
    info "Restarting Moonraker..."
    restart_url=$(moonraker_local_url "")
    restart_mode=config
    bundled=""
    if [ "$1" = install ]; then
        bundled=$(bundled_plugin_version)
        restart_mode=$(plugin_restart_mode "$restart_url" "$bundled")
        [ "$restart_mode" != none ] || restart_mode=config
    fi
    restarted=0
    moonraker_restart "$restart_mode" "$restart_url" info warn "$bundled" || restarted=$?
    case "$restarted" in
        "$HELIX_RESTART_DONE") return 0 ;;
        "$HELIX_RESTART_DOWN") return 2 ;;
        *) return 1 ;;
    esac
}

# Strip phase-tracking instrumentation from PRINT_START via the bundled
# strip_phase_tracking.py, which owns the marker format and the safety
# contract around editing a printer's config (matched blocks only, a
# verified backup, symlinks and modes preserved). python3 is what Moonraker
# itself runs on: here, or inside the chroot a Moonraker like Forge-X's runs
# in, which is the only python3 such a printer has. Its absence from both
# means something else on this printer is already broken. Either way this
# never aborts the rest of uninstall - it reports the outcome and returns it,
# so the caller can fold it into its own exit status instead of claiming
# success it cannot back up.
#
# Returns strip_phase_tracking.py's own exit status unchanged (0 clean, 2
# needs attention, 1 failed) so a caller can wire it straight through; a
# missing python3 is reported as EXIT_UNINSTALL_NEEDS_ATTENTION since
# PRINT_START could not be checked at all.
strip_phase_tracking_instrumentation() {
    scan_dir="$1"
    strip_py="$SCRIPT_DIR/strip_phase_tracking.py"

    strip_status=$EXIT_UNINSTALL_OK
    if command -v python3 > /dev/null 2>&1; then
        python3 "$strip_py" "$scan_dir" || strip_status=$?
    elif chroot_py=$(chroot_python3) &&
        chroot_strip_py=$(moonraker_visible_path "$strip_py") &&
        chroot_scan_dir=$(moonraker_visible_path "$scan_dir"); then
        chroot "$(running_moonraker_root)" "$chroot_py" "$chroot_strip_py" "$chroot_scan_dir" || strip_status=$?
    else
        warn "python3 not found - could not check PRINT_START for phase-tracking instrumentation"
        warn "If a previous HelixScreen instrumented PRINT_START, remove any block between"
        warn "'# <<< HELIX_TRACKING v2 >>>' and '# <<< /HELIX_TRACKING >>>' by hand, then restart Klipper"
        return "$EXIT_UNINSTALL_NEEDS_ATTENTION"
    fi
    if [ "$strip_status" -ne 0 ]; then
        warn "Phase-tracking strip reported a failure - see the output above"
    fi
    return "$strip_status"
}

# Strip the two mesh-include lines power-loss recovery adds to printer.cfg
# and remove the helix-plr directory they include, through the plugin's own
# rule (helix_plr.py's uninstall_mesh_include): one copy of which lines are
# ours and how the file is backed up. python3 resolves like
# strip_phase_tracking_instrumentation's - here, or inside the chroot a
# Moonraker like Forge-X's runs in. A printer.cfg without the lines, or a
# missing printer.cfg, is the do-nothing case. Never aborts the rest of the
# uninstall - a failure is reported and folds into "needs attention".
# Run a plugin-module uninstall step: python code that puts the plugin's own
# directory on sys.path (argv[1]) and takes the config directory (argv[2]). It
# runs with the host's python3, or the python inside the chroot Moonraker runs
# in, with both paths translated. Prints what the step printed; returns its
# exit status, or 127 when no python can run it.
run_uninstall_step() {
    _us_code="$1"
    _us_config="$2"
    if command -v python3 > /dev/null 2>&1; then
        python3 -c "$_us_code" "$SCRIPT_DIR" "$_us_config"
    elif _us_py=$(chroot_python3) &&
        _us_dir=$(moonraker_visible_path "$SCRIPT_DIR") &&
        _us_cfg=$(moonraker_visible_path "$_us_config"); then
        chroot "$(running_moonraker_root)" "$_us_py" -c "$_us_code" "$_us_dir" "$_us_cfg"
    else
        return 127
    fi
}

remove_plr_mesh_include() {
    config_dir="$1"
    [ -n "$config_dir" ] || return 0
    [ -f "$config_dir/printer.cfg" ] || return 0

    plr_strip_code='
import pathlib
import sys
sys.path.insert(0, sys.argv[1])
import helix_plr
problem = helix_plr.uninstall_mesh_include(pathlib.Path(sys.argv[2]))
if problem is None:
    sys.exit(0)
print(problem)
sys.exit(2)
'
    plr_status=0
    problem=$(run_uninstall_step "$plr_strip_code" "$config_dir") || plr_status=$?
    if [ "$plr_status" -eq 127 ]; then
        warn "python3 not found - could not remove the power-loss mesh include from printer.cfg"
        warn "If power-loss recovery added [include helix-plr/*.cfg] to it, remove those two lines and the helix-plr directory by hand"
        return "$EXIT_UNINSTALL_NEEDS_ATTENTION"
    fi
    if [ "$plr_status" -ne 0 ]; then
        warn "Could not remove the power-loss mesh include from printer.cfg${problem:+ - $problem}"
        return "$EXIT_UNINSTALL_NEEDS_ATTENTION"
    fi
    return "$EXIT_UNINSTALL_OK"
}

# Klipper companions: the include line, the helix_companions directory and
# every module file the plugin put into Klipper's extras.
remove_klipper_companions() {
    config_dir="$1"
    [ -n "$config_dir" ] || return 0
    [ -d "$config_dir/helix_companions" ] || return 0

    comp_code='
import pathlib
import sys
sys.path.insert(0, sys.argv[1])
import helix_companions
problem = helix_companions.uninstall_companions(pathlib.Path(sys.argv[2]))
if problem is None:
    sys.exit(0)
print(problem)
sys.exit(2)
'
    comp_status=0
    problem=$(run_uninstall_step "$comp_code" "$config_dir") || comp_status=$?
    if [ "$comp_status" -eq 127 ]; then
        warn "python3 not found - could not remove the Klipper companions"
        warn "Remove the [include helix_companions/*.cfg] lines from printer.cfg, the helix_companions directory, and the helix_*.py files HelixPrint added to Klipper's klippy/extras"
        return "$EXIT_UNINSTALL_NEEDS_ATTENTION"
    fi
    if [ "$comp_status" -ne 0 ]; then
        warn "Could not remove the Klipper companions${problem:+ - $problem}"
        return "$EXIT_UNINSTALL_NEEDS_ATTENTION"
    fi
    return "$EXIT_UNINSTALL_OK"
}

# Auto-uninstall function (non-interactive, for HelixScreen integration)
auto_uninstall() {
    info "HelixPrint Auto-Uninstall Mode"
    printf '\n'
    exit_status=$EXIT_UNINSTALL_OK

    # Find Moonraker (auto-detect only)
    components_dir=$(find_moonraker_components "")

    if [ -z "$components_dir" ]; then
        error "Could not auto-detect Moonraker installation."
    fi

    target="$components_dir/helix_print.py"

    # Find config directory
    config_dir=$(find_config_dir)

    # Remove symlink
    if [ -L "$target" ]; then
        rm "$target"
        info "Removed symlink: $target"
    elif [ -f "$target" ]; then
        error "Found regular file (not symlink) at $target - manual removal required"
    else
        info "Plugin symlink not found (already uninstalled?)"
    fi

    # helix_macros.cfg and its printer.cfg include are left in place: the
    # macros are shared HelixScreen helpers (HELIX_START_PRINT,
    # HELIX_CLEAN_NOZZLE and friends) that keep working without this plugin.
    # Any HELIX_PHASE_*/HELIX_READY calls this plugin's own instrumentation
    # left in PRINT_START are removed, with a backup, by the call below. The
    # plugin itself is already gone at this point, so any nonzero here - a
    # skipped file or a failed one - is "needs attention", not a failed
    # uninstall.
    if [ -n "$config_dir" ]; then
        strip_phase_tracking_instrumentation "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
        remove_plr_mesh_include "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
        remove_klipper_companions "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
    fi

    # Remove the plugin's sections from every file Moonraker reads them from:
    # moonraker.conf and anything it includes, such as the user file a
    # mod-owned moonraker.conf includes. The plugin itself is already gone at
    # this point, so a failure backing up or editing a config here is "needs
    # attention", not a failed uninstall, and must not trip set -e and skip
    # the rest of this function.
    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        for file in $(helix_print_owned_files "$config_dir/moonraker.conf" "$(running_moonraker_root)"); do
            remove_helix_section "$file" "$HELIX_PRINT_OWNED_RE" "$HELIX_PRINT_STRIP_AWK" ||
                exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
        done
    fi

    # The plugin is already gone, so a restart that never happened is
    # reported, not turned into a failed uninstall.
    MOONRAKER_URL=$(moonraker_local_url "$config_dir/moonraker.conf" "$(running_moonraker_root)")
    finished=0
    finish_with_restart uninstall || finished=$?
    if [ "$finished" -ne 0 ]; then
        printf '\n'
        [ "$exit_status" -eq "$EXIT_UNINSTALL_OK" ] || warn "PRINT_START needs a look too - see the warnings above"
        if [ "$finished" -eq 2 ]; then
            warn "Plugin removed; Moonraker starts without it"
        else
            warn "Plugin removed, but Moonraker keeps it loaded until it restarts"
        fi
        return "$exit_status"
    fi

    printf '\n'
    if [ "$exit_status" -eq "$EXIT_UNINSTALL_OK" ]; then
        info "Auto-uninstall complete!"
    else
        warn "Auto-uninstall complete, but PRINT_START needs a look - see the warnings above"
    fi

    return "$exit_status"
}

# Re-point an installed plugin's link at this tree (--relink): what the app
# runs at startup, so a plugin installed from an install directory that has
# since moved, or been replaced, loads from the tree beside the running app.
# Never creates a link that is not there: an install is the user's choice.
# Where the link is ours it also removes [update_manager helix_print], with a
# backup: that entry updates a git checkout Moonraker no longer loads. Nothing
# else in the config is touched, and the checkout itself is left on disk.
# Returns an EXIT_RELINK_* code; a config edit that fails only warns.
relink() {
    components_dir=$(find_moonraker_components "")
    [ -n "$components_dir" ] || error "Could not auto-detect Moonraker installation."
    target="$components_dir/helix_print.py"

    if [ ! -L "$target" ]; then
        info "No plugin link at $target - nothing to repair"
        return "$EXIT_RELINK_NOT_OURS"
    fi
    current=$(readlink "$target")
    case "$current" in
        */helix_print.py) ;;
        *)
            info "$target links to $current, not a HelixPrint plugin - leaving it"
            return "$EXIT_RELINK_NOT_OURS"
            ;;
    esac
    config_dir=$(find_config_dir)
    if [ -z "$config_dir" ] ||
        [ -z "$(helix_print_section_files "$config_dir/moonraker.conf" "$(running_moonraker_root)")" ]; then
        info "No [helix_print] section configured - leaving $target alone"
        return "$EXIT_RELINK_NOT_OURS"
    fi

    # The link text is Moonraker's path, which differs from ours in a chroot.
    # Compared as files, not paths: through a bind mount (Forge-X binds
    # /opt/config) one file has two paths that resolve apart. -ef is in dash,
    # BusyBox ash and bash, every shell this runs under.
    mroot=$(running_moonraker_root)
    case "$current" in
        /*) seen="$mroot$current" ;;
        *) seen="$components_dir/$current" ;;
    esac
    # shellcheck disable=SC3013
    if [ -e "$seen" ] && [ "$seen" -ef "$PLUGIN_FILE" ]; then
        info "The plugin link already names $PLUGIN_FILE"
        remove_plugin_updater_entries "$config_dir" ||
            warn "Left [update_manager helix_print] in place - see the warnings above"
        return "$EXIT_RELINK_CURRENT"
    fi

    check_plugin_link "$components_dir"
    ln -sf "$link_to" "$target" || error "Could not re-point $target"
    info "Re-pointed $target -> $link_to (was $current)"
    remove_plugin_updater_entries "$config_dir" ||
        warn "Left [update_manager helix_print] in place - see the warnings above"
    info "Moonraker no longer loads the plugin from $(dirname "$current"); a copy there that nothing else uses can be removed"
    return "$EXIT_RELINK_DONE"
}

# Auto-install function (non-interactive, for HelixScreen integration)
auto_install() {
    info "HelixPrint Auto-Install Mode"
    printf '\n'

    AUTO_MODE=true

    # Check plugin file exists
    if [ ! -f "$PLUGIN_FILE" ]; then
        error "Plugin file not found: $PLUGIN_FILE"
    fi

    # Find Moonraker (auto-detect only)
    components_dir=$(find_moonraker_components "")

    if [ -z "$components_dir" ]; then
        error "Could not auto-detect Moonraker installation."
    fi

    # Find config directory
    config_dir=$(find_config_dir)

    info "Moonraker components: $components_dir"
    info "Config: ${config_dir:-not found}"
    printf '\n'

    # Pre-flight checks, all before the first write: the link can be made,
    # the config can be edited, and Moonraker can read what the link names.
    check_plugin_link "$components_dir"

    section_file=""
    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        section_file=$(helix_conf_file "$config_dir")
        if [ -e "$section_file" ] && [ ! -w "$section_file" ]; then
            error "Cannot write to $section_file"
        fi
    fi

    # Create symlink
    target="$components_dir/helix_print.py"

    if [ -f "$target" ] && [ ! -L "$target" ]; then
        error "A file (not symlink) exists at $target"
    fi

    # Auto-configure moonraker.conf if possible
    if [ -n "$section_file" ]; then
        # A section in any file Moonraker reads counts, so the install never
        # writes a second one beside it.
        section_holder=""
        for file in $(helix_print_section_files "$config_dir/moonraker.conf" "$(running_moonraker_root)"); do
            [ -n "$section_holder" ] || section_holder="$file"
            # An install ends with the plugin working, so a section that
            # switches it off is switched back on.
            enable_helix_section "$file"
        done
        if [ -n "$section_holder" ]; then
            info "Config section [helix_print] already exists in $section_holder"
        else
            # Create backup before modifying config
            if [ -f "$section_file" ]; then
                backup_file=$(backup_config "$section_file")
                info "Created backup: $backup_file"
            fi

            info "Adding [helix_print] section to $section_file"
            # End an unterminated last line first. The blank line after it is
            # the one HELIX_PRINT_STRIP_AWK takes back out on uninstall.
            if [ -s "$section_file" ] && [ -n "$(tail -c 1 "$section_file")" ]; then
                printf '\n' >> "$section_file"
            fi
            printf '\n' >> "$section_file"
            printf '%s\n' "[helix_print]" >> "$section_file"
            printf '%s\n' "# HelixScreen plugin - auto-configured" >> "$section_file"
            section_holder="$section_file"
        fi

        if klipper_hides_dot_dirs; then
            ensure_visible_symlink_dir "$section_holder"
        fi

        remove_plugin_updater_entries "$config_dir" ||
            error "Could not remove [update_manager helix_print] - see the warnings above"
    else
        warn "Could not find moonraker.conf - manual config required"
    fi

    # The link goes last: an install that stops on a config write before it
    # leaves the previous link naming the tree it named, which a caller that
    # put that tree back (remote-install.sh) relies on. ln -sf replaces an
    # existing symlink atomically.
    ln -sf "$link_to" "$target"
    info "Created symlink: $target -> $link_to"

    # The install itself is done by now, so a restart that never happened is
    # reported, never a failure that would roll it back.
    # shellcheck disable=SC2034  # read by moonraker_local_url (helix_print_conf.sh)
    MOONRAKER_URL=$(moonraker_local_url "$config_dir/moonraker.conf" "$(running_moonraker_root)")
    finished=0
    finish_with_restart install || finished=$?
    if [ "$finished" -eq 2 ]; then
        printf '\n'
        warn "Plugin installed, but Moonraker is not running"
        return 0
    fi
    if [ "$finished" -ne 0 ]; then
        printf '\n'
        warn "Plugin installed, but it does not load until Moonraker restarts"
        return 0
    fi

    printf '\n'
    info "Auto-install complete!"
}

# Restart Moonraker so it runs the plugin code on disk (--restart-moonraker):
# what the app runs after --relink re-pointed the link, or when the running
# plugin reports another version than this tree's. The restart is the one
# plugin_restart_mode names, and none when Moonraker runs this tree's version
# already. moonraker_restart asks Moonraker whether a print runs
# (HELIX_JOB_STATE stands in only when it cannot be asked). Returns
# moonraker_restart's code.
restart_for_plugin() {
    config_dir=$(find_config_dir)
    restart_url=$(moonraker_local_url "$config_dir/moonraker.conf" "$(running_moonraker_root)")
    bundled=$(bundled_plugin_version)
    restart_mode=$(plugin_restart_mode "$restart_url" "$bundled")
    if [ "$restart_mode" = none ]; then
        info "Moonraker runs HelixPrint plugin $bundled already; no restart needed"
        return "$HELIX_RESTART_DONE"
    fi
    moonraker_restart "$restart_mode" "$restart_url" info warn "$bundled"
}

show_help() {
    printf '%s\n' "Usage: $0 [OPTIONS] [MOONRAKER_PATH]"
    printf '\n'
    printf '%s\n' "Options:"
    printf '%s\n' "  --auto, -a              Full auto-install (updates config, restarts Moonraker)"
    printf '%s\n' "  --no-restart            Before --auto/--uninstall-auto: skip the Moonraker restart;"
    printf '%s\n' "                          the caller restarts it (e.g. via Moonraker's server.restart)"
    printf '%s\n' "  --uninstall, -u         Remove the plugin symlink and strip PRINT_START"
    printf '%s\n' "                          instrumentation, if any (interactive)"
    printf '%s\n' "  --uninstall-auto        Full auto-uninstall: removes the symlink and config"
    printf '%s\n' "                          section, strips PRINT_START instrumentation, restarts"
    printf '%s\n' "                          Moonraker"
    printf '%s\n' "  --relink                Re-point an installed plugin's link at this copy;"
    printf '%s\n' "                          exit 0 already current, 3 re-pointed, 4 nothing of"
    printf '%s\n' "                          ours to repair, 1 failed. Drops a git checkout's"
    printf '%s\n' "                          [update_manager helix_print]. Never restarts Moonraker"
    printf '%s\n' "  --restart-moonraker     Restart Moonraker over its API so it loads the plugin"
    printf '%s\n' "                          on disk; exit 0 restarted, 3 needs a manual"
    printf '%s\n' "                          restart, 4 held off by a print, 6 print state unknown,"
    printf '%s\n' "                          7 Moonraker not answering"
    printf '%s\n' "  --help, -h              Show this help message"
    printf '\n'
    printf '%s\n' "Exit codes for --uninstall and --uninstall-auto:"
    printf '%s\n' "  0   uninstalled; nothing left in the config to clean up"
    printf '%s\n' "  2   the plugin was removed, but a config file was skipped or failed during"
    printf '%s\n' "      the phase-tracking strip and needs a human to look at it"
    printf '%s\n' "  1   (or any other nonzero) the uninstall itself failed"
    printf '\n'
    printf '%s\n' "Arguments:"
    printf '%s\n' "  MOONRAKER_PATH     Path to Moonraker installation (auto-detected if not provided)"
    printf '\n'
    printf '%s\n' "Environment Variables:"
    printf '%s\n' "  MOONRAKER_URL      URL for Moonraker's restart request and health check"
    printf '%s\n' "                     Default: http://127.0.0.1:<[server] port in moonraker.conf, else 7125>"
    printf '%s\n' "                     Example: MOONRAKER_URL=http://192.168.1.100:7125 ./install.sh --auto"
    printf '\n'
    printf '%s\n' "Examples:"
    printf '%s\n' "  ./install.sh --auto      # Auto-install (updates moonraker.conf, restarts Moonraker)"
}

# Parse arguments
NO_RESTART=false
while [ $# -gt 0 ]; do
    case "$1" in
        --no-restart)
            NO_RESTART=true
            shift
            ;;
        --auto|-a)
            AUTO_MODE=true
            shift
            ;;
        --uninstall|-u)
            uninstall "$2"
            exit $?
            ;;
        --uninstall-auto)
            auto_uninstall
            exit $?
            ;;
        --relink)
            status=0
            relink || status=$?
            exit "$status"
            ;;
        --restart-moonraker)
            status=0
            restart_for_plugin || status=$?
            exit "$status"
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            # Assume it's a moonraker path for interactive mode
            if [ "$AUTO_MODE" = "true" ]; then
                auto_install
            else
                main "$1"
            fi
            exit 0
            ;;
    esac
done

# No arguments left - run in appropriate mode
if [ "$AUTO_MODE" = "true" ]; then
    auto_install
else
    main ""
fi
