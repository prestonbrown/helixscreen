#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything main() calls before confirm_point, and everything confirm_point
# itself calls before its first write (the mkdir of TMP_DIR that precedes
# log_open), must only read: --dry-run exits inside confirm_point, and the plan
# screen promises nothing has changed yet. The scan is one level deep: a name
# on READ_ONLY vouches for the functions it calls in turn. The e2e sandbox only
# simulates a Debian host, so this check covers the K1, AD5M and mod-payload
# branches it never runs.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
MAIN="$WORKTREE_ROOT/scripts/lib/installer/main.sh"
PLAN="$WORKTREE_ROOT/scripts/lib/installer/plan.sh"

# Functions known to only read. Adding one here is a claim that it, and every
# function it calls, writes nothing and runs no sudo, apt or systemctl change.
# uninstall, image_install and image_first_boot are the exceptions: each is a
# separate mode that exits before confirm_point is reached, so none runs on the
# install path.
READ_ONLY="host_profile_probe parse_installer_args mod_payload_autodetect
_refuse_uninstall_from_install_dir _refuse_if_firmware_managed detect_platform
get_download_platform print_platform_banner mod_check_chroot_context
detect_mod_flavor detect_k1_firmware log_k1_ssh_state set_install_paths mod_payload_mode_block
check_permissions check_requirements detect_missing_unzip
detect_missing_runtime_deps check_disk_space detect_init_system
check_klipper_ecosystem resolve_update_channel parse_tarball_version
get_latest_version match_channel_to_version detect_competing_uis
detect_moonraker_integration detect_kiauh probe_release usage uninstall
image_install image_first_boot
log_info log_warn log_error log_note log_success plan_set plan_count_steps
print_banner print_plan plan_missing_libs plan_adds_line plan_printer_line
parse_json_string_field display_path tty_confirm _has_no_new_privs step
step_done _plan_sep confirm_clean_install resolve_x86_flavour
x86_flavour_plan_text detect_package_beside_installer settle_package_beside_installer
_progress_emit"

# Names called in main() between its opening line and confirm_point, and in
# confirm_point up to its mkdir of TMP_DIR.
_pre_confirm_calls() {
    {
        awk '/^main\(\) *\{/{on=1; next} on && /^[[:space:]]*confirm_point/{exit} on' "$MAIN"
        awk '/^confirm_point\(\) *\{/{on=1; next} on && /mkdir -p "\$TMP_DIR"/{exit} on' "$PLAN"
    } | sed 's/#.*//' \
        | grep -oE '(^|[;&|({ ]|\$\()[a-z_][a-z0-9_]*' \
        | sed -E 's/^[;&|({ ]|^\$\(//' \
        | sort -u
}

@test "the scan covers confirm_point's own calls up to its first write" {
    calls=$(_pre_confirm_calls)
    for fn in plan_printer_line parse_json_string_field print_banner tty_confirm; do
        echo "$calls" | grep -qx "$fn" || { echo "the scan misses $fn, called by confirm_point"; false; }
    done
    if echo "$calls" | grep -qx log_open; then echo "the scan runs past confirm_point's first write"; false; fi
}

@test "main() has a confirm_point" {
    grep -q '^[[:space:]]*confirm_point' "$MAIN"
}

@test "every installer function called before the confirm point's first write is read-only" {
    lib="$WORKTREE_ROOT/scripts/lib/installer"
    defined=$(grep -hoE '^[a-z_][a-z0-9_]*\(\)' "$lib"/*.sh | tr -d '()' | sort -u)
    bad=""
    for fn in $(_pre_confirm_calls); do
        echo "$defined" | grep -qx "$fn" || continue
        case " $(echo $READ_ONLY) " in *" $fn "*) ;; *) bad="$bad $fn" ;; esac
    done
    [ -z "$bad" ] || { echo "not on the read-only list:$bad"; false; }
}

@test "nothing before confirm_point uses apt, systemctl changes or SUDO directly" {
    run sh -c "awk '/^main\\(\\) *\\{/{on=1; next} on && /^[[:space:]]*confirm_point/{exit} on' '$MAIN' | grep -nE 'apt-get|systemctl (stop|start|enable|disable|restart|mask)|\\\$SUDO|mkdir|rm -'"
    [ "$status" -ne 0 ]
}
