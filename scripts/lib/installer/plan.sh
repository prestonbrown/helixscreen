#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The plan the read-only pass builds, shown before anything changes.

# Source guard
[ -n "${_HELIX_PLAN_SOURCED:-}" ] && return 0
_HELIX_PLAN_SOURCED=1

_PLAN=""

plan_set() { # key value
    _PLAN="${_PLAN}$(printf '  %-10s %s' "$1" "$2")
"
    _log_write "PLAN $1: $2"
}

print_plan() {
    printf '%s' "$_PLAN" >&2
}

# Steps the run will show, so a no-terminal run can number them [n/N].
# apply_install skips exactly the steps left out here.
plan_count_steps() {
    # Checking system, Downloading, Installing files, Adding to the app menu.
    if [ "${platform:-}" = "x86-desktop" ]; then
        STEP_TOTAL=4
        return 0
    fi
    STEP_TOTAL=6
    [ -n "$(plan_missing_libs)" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    [ -n "${COMPETING_UIS_FOUND:-}" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    return 0
}

# Packages the install adds with apt, space-separated.
plan_missing_libs() {
    # Package names never contain spaces, so word splitting joins the lists.
    # shellcheck disable=SC2086
    echo ${MISSING_UNZIP_PKG:-} ${MISSING_RUNTIME_DEPS:-}
}

# False on a payload install whose UI the mod starts at the next boot, true
# on one that restarts the UI it replaced (mod_payload_restarts_ui).
plan_starts_ui() {
    [ "${HOST_SERVICE_MECHANISM:-}" != "mod-managed" ] || mod_payload_restarts_ui
}

_plan_sep() { if [ "$UI_UTF8" = 1 ]; then printf ' · '; else printf ', '; fi; }

# The Printer line: board, firmware flavor, init system, Klipper user.
plan_printer_line() { # platform
    _ppl="${HARDWARE_LABEL:-$1}"
    _ppl_fw="${MOD_FLAVOR:-${K1_FIRMWARE:-}}"
    [ -n "$_ppl_fw" ] && _ppl="$_ppl$(_plan_sep)$_ppl_fw"
    [ -n "${INIT_SYSTEM:-}" ] && _ppl="$_ppl$(_plan_sep)$INIT_SYSTEM"
    [ -n "${KLIPPER_USER:-}" ] && _ppl="$_ppl$(_plan_sep)user $KLIPPER_USER"
    printf '%s' "$_ppl"
}

# The Add line: what the install registers with Moonraker and KIAUH.
plan_adds_line() {
    _pal=""
    for _pa in ${MOONRAKER_ADDS:-}; do
        case "$_pa" in
            update-manager) _pa_text="Moonraker update manager" ;;
            allowlist) _pa_text="service allowlist" ;;
            *) continue ;;
        esac
        _pal="${_pal:+$_pal$(_plan_sep)}$_pa_text"
    done
    [ -n "${KIAUH_DIR:-}" ] && _pal="${_pal:+$_pal$(_plan_sep)}KIAUH extension"
    [ "${platform:-}" = "x86-desktop" ] && _pal="${_pal:+$_pal$(_plan_sep)}app menu entry"
    printf '%s' "$_pal"
}

# The confirm point: everything main() runs before it only reads, everything
# after it changes the machine. Shows the plan, exits on --dry-run or a "no",
# asks sudo for its password once, then opens the log. Returns only when the
# install goes ahead.
confirm_point() { # platform version
    # An update reads "Update <installed> -> <target>" when release_info.json
    # names the installed version, and like a fresh install otherwise. The
    # installed binary is never run here: one that hangs would stop the
    # installer before the plan.
    _cp_label=Install
    _cp_install="$2 (${R2_CHANNEL:-stable})"
    if [ "${update_mode:-false}" = true ] && [ -f "${INSTALL_DIR:-}/release_info.json" ]; then
        _cp_from=$(parse_json_string_field version < "$INSTALL_DIR/release_info.json")
        if [ -n "$_cp_from" ]; then
            if [ "$UI_UTF8" = 1 ]; then _cp_arrow="→"; else _cp_arrow="->"; fi
            _cp_label=Update
            _cp_install="$_cp_from $_cp_arrow $_cp_install"
        fi
    fi
    _cp_libs=$(plan_missing_libs)
    _cp_add="$(plan_adds_line)"
    _cp_mode="$(x86_flavour_plan_text)"

    plan_set Printer "$(plan_printer_line "$1")"
    [ -n "$_cp_mode" ] && plan_set Mode "$_cp_mode"
    [ -d "${INSTALL_DIR:-}" ] && plan_set Found "HelixScreen at $INSTALL_DIR"
    plan_set "$_cp_label" "$_cp_install${PROBE_SIZE_TEXT:+$(_plan_sep)$PROBE_SIZE_TEXT}"
    [ "${clean_mode:-false}" = true ] && plan_set Remove \
        "$(display_path "${INSTALL_DIR:-}"), its settings and config backups, thumbnail caches (--clean)"
    [ -n "$_cp_libs" ] && plan_set Libraries "$_cp_libs (apt)"
    [ -n "${COMPETING_UIS_FOUND:-}" ] && plan_set Disable "$COMPETING_UIS_FOUND"
    [ -n "$_cp_add" ] && plan_set Add "$_cp_add"
    [ -n "${KIAUH_WRITE_SKIPPED:-}" ] && plan_set KIAUH \
        "found at $(display_path "$KIAUH_WRITE_SKIPPED"), not writable without sudo: extension skipped"
    [ -n "${DISK_CHECK_DEFERRED:-}" ] && plan_set Disk "would check after sudo"
    # Under NoNewPrivileges sudo cannot run, and the steps that need it skip.
    if [ -n "${SUDO:-}" ] && ! _has_no_new_privs; then
        plan_set sudo "needed for: service, ${_cp_libs:+libraries, }${KIAUH_SUDO:+KIAUH extension, }udev and polkit rules"
    fi

    print_banner "$2" "${R2_CHANNEL:-stable}"
    print_plan
    printf '\n' >&2

    if [ "${DRY_RUN:-false}" = true ]; then
        printf '%s\n' "Dry run, nothing changed." >&2
        exit 0
    fi

    # --clean deletes what the Remove line lists, so its Continue? is the
    # consent and defaults to no. Otherwise --update is the decision, so it
    # never asks.
    if [ "${clean_mode:-false}" = true ]; then
        confirm_clean_install
    elif [ "${update_mode:-false}" != true ] && [ "$UI_TTY" = 1 ]; then
        tty_confirm "Continue?" y || { printf '%s\n' "Nothing changed." >&2; exit 0; }
    fi

    # Under NoNewPrivileges (the in-app updater) sudo cannot work at all; the
    # steps that need it already check _has_no_new_privs and skip.
    if [ -n "${SUDO:-}" ] && ! _has_no_new_privs && ! $SUDO -n true 2>/dev/null; then
        printf '%s\n' "sudo is needed to install the service and system files." >&2
        $SUDO -v || { log_error "sudo was not granted; nothing changed."; exit 1; }
    fi
    # shellcheck disable=SC2034  # consumed by requirements.sh (_fs_probe_write_kb)
    HELIX_CONFIRMED=1

    plan_count_steps
    step "Checking system" "Checked system"
    step_done "$(plan_printer_line "$1")"
    mkdir -p "$TMP_DIR" 2>/dev/null || $SUDO mkdir -p "$TMP_DIR"
    log_open "$TMP_DIR/install.log" || true
}

# The Connected to Moonraker step's detail: what the install registered.
moonraker_step_detail() {
    _msd=""
    for _ma in ${MOONRAKER_ADDS:-}; do
        case "$_ma" in
            update-manager) _ma_text="update manager" ;;
            allowlist) _ma_text="service allowlist" ;;
            *) continue ;;
        esac
        _msd="${_msd:+$_msd$(_plan_sep)}$_ma_text"
    done
    [ -n "${HELIX_CONFIG_EDITABLE:-}" ] && _msd="${_msd:+$_msd$(_plan_sep)}config editable in Mainsail/Fluidd"
    printf '%s' "$_msd"
}

_summary_row() { printf '  %-10s %s\n' "$1" "$2" >&2; }

# The closing block of a successful run: what is running, where its config
# is, how it updates, what it disabled, and where the log is.
print_summary() { # version
    _ps_channel="${R2_CHANNEL:-stable}"
    printf '\n' >&2
    if [ "${platform:-}" = "x86-desktop" ]; then
        printf '%b\n\n' "${BOLD}HelixScreen $1 is installed ($_ps_channel channel).${NC}" >&2
        _summary_row Launch "HelixScreen in your app menu, or $(display_path "$INSTALL_DIR/bin/helix-screen")"
        _summary_row Printer "the first start asks for it, or add --moonraker ws://PRINTER_IP:7125"
        _summary_row Config "$(display_path "$INSTALL_DIR/config")"
        _summary_row Updates "announced in the app; install them by re-running this installer"
        _summary_row Remove "re-run this installer with --uninstall"
        [ -z "${INSTALL_LOG_KEPT:-}" ] || _summary_row Log "$(display_path "$INSTALL_LOG_KEPT")"
        return 0
    fi
    if plan_starts_ui; then
        printf '%b\n\n' "${BOLD}HelixScreen $1 is running ($_ps_channel channel).${NC}" >&2
    else
        printf '%b\n\n' "${BOLD}HelixScreen $1 is installed ($_ps_channel channel). Reboot to start it.${NC}" >&2
        _summary_row Service "${INIT_SCRIPT_DEST:-}"
        _summary_row "App log" "$(display_path "$INSTALL_DIR/logs/launcher.log")"
    fi
    if [ -n "${HELIX_CONFIG_EDITABLE:-}" ]; then
        _summary_row Config "$(display_path "$HELIX_CONFIG_EDITABLE")  (editable in Mainsail/Fluidd)"
    else
        _summary_row Config "$(display_path "$INSTALL_DIR/config")"
    fi
    _ps_conf=$(find_moonraker_conf 2>/dev/null || true)
    if [ -n "$_ps_conf" ] && has_update_manager_section "$_ps_conf"; then
        _summary_row Updates "Mainsail/Fluidd update manager, or re-run with --update"
    else
        _summary_row Updates "re-run with --update"
    fi
    [ -n "${COMPETING_UIS_FOUND:-}" ] && _summary_row Disabled "$COMPETING_UIS_FOUND  (re-enabled by --uninstall)"
    [ -n "${KIAUH_EXT_ADDED:-}" ] && _summary_row KIAUH "restart KIAUH to see the HelixScreen extension"
    [ -z "${INSTALL_LOG_KEPT:-}" ] || _summary_row Log "$(display_path "$INSTALL_LOG_KEPT")"
}
