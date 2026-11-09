#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: main
# Top-level installer orchestration: argument parsing, platform detection,
# preflight, download, install, post-install. Single source of truth shared
# by install-dev.sh and the bundled install.sh.
#
# Reads: every other lib/installer module
# Writes: nothing; orchestrates the install flow

# Source guard
[ -n "${_HELIX_MAIN_SOURCED:-}" ] && return 0
_HELIX_MAIN_SOURCED=1

# Set up error trap (ERR is bash-specific, skip on POSIX shells like dash/ash)
# shellcheck disable=SC3047
trap 'error_handler $LINENO' ERR 2>/dev/null || true

# Resolve the open step, keep the log and remove the scratch dir however the
# installer ends.
#
# The ERR trap above is a bash extension and is silently discarded on the
# ash/dash shells every embedded platform runs, so without this trap an
# interrupted or failing install leaks the whole download - tens of megabytes
# stranded on a partition that may only have a couple of hundred.
#
# The log lives in the scratch dir until finalize_install_log moves it, so the
# failure report runs first. cleanup_on_success is idempotent (it tests for
# the directory first) so the explicit call on the success path is
# unaffected, and it routes through _safe_remove_tmp_dir, which is what
# refuses to rm -rf a mountpoint. A signal handler that returns resumes the
# script with its download gone, so each signal exits with the shell's own
# 128+N status, and the EXIT trap then reports it. The signal traps print
# nothing: after an SSH drop the terminal is gone, and a failed write under
# set -e would end the trap before the log is kept.
trap 'installer_exit_report $?; cleanup_on_success' EXIT
trap 'STEP_FAIL_REASON=interrupted; exit 129' HUP
trap 'STEP_FAIL_REASON=interrupted; exit 130' INT
trap 'STEP_FAIL_REASON=interrupted; exit 143' TERM

# The last progress line: ok on a clean exit, else the failing step's reason
# (or the exit status when no step named one). Sent once per run.
_progress_result() { # exit status
    [ "$PROGRESS" = 1 ] && [ "$PROGRESS_RESULT_SENT" = 0 ] || return 0
    PROGRESS_RESULT_SENT=1
    if [ "$STEP_OPEN" = 1 ]; then
        _progress_emit step "$STEP_PROGRESS_N" "$STEP_TOTAL" fail "$STEP_TITLE" "${STEP_FAIL_REASON:-}"
        STEP_OPEN=0
    fi
    if [ "$1" -eq 0 ]; then
        _progress_emit result ok
    else
        _progress_emit result fail "${STEP_FAIL_REASON:-exit $1}"
    fi
}

# The failure block's closing lines, for a run that passed the confirm point
# and then stopped: the failed step, what state the printer is in, and where
# the full log is. The log is kept before anything prints, and nothing here
# may end the shell early: stderr can be a terminal that no longer exists.
installer_exit_report() { # exit status
    set +e
    if ! { [ "$1" -ne 0 ] && [ -n "${HELIX_CONFIRMED:-}" ]; }; then
        _progress_result "$1"
        return 0
    fi
    _ier_state=$(install_state_line)
    finalize_install_log
    step_fail "${STEP_FAIL_REASON:-}"
    printf '\n' >&2
    if [ -n "$_ier_state" ]; then
        _log_write "STATE $_ier_state"
        printf '%s\n' "$_ier_state" >&2
    fi
    if [ -n "${INSTALL_LOG_KEPT:-}" ]; then
        printf '%s\n' "Full log: $(display_path "$INSTALL_LOG_KEPT")" >&2
    fi
    _progress_result "$1"
    return 0
}

# "a", "a and b", "a, b and c".
_and_list() {
    case $# in
        0) ;;
        1) printf '%s' "$1" ;;
        *)
            _al=""
            while [ $# -gt 2 ]; do _al="$_al$1, "; shift; done
            printf '%s%s and %s' "$_al" "$1" "$2"
            ;;
    esac
}

# What a stopped run left on the printer, from the marks apply_install and
# extract_release set as they change it: INSTALL_SWAPPED (the new tree
# landed, or an in-place replace began), INSTALL_BACKUP (the previous tree,
# put back by a rollback when it no longer exists), INSTALL_STOPPED (services
# stopped) and INSTALL_REMOVED_OLD (--clean). Empty once every step is done.
install_state_line() {
    [ -n "${INSTALL_COMPLETE:-}" ] && return 0
    case "${INSTALL_SWAPPED:-}" in
        in-place)
            printf 'HelixScreen at %s was partly replaced; re-run the installer to finish.' "$INSTALL_DIR"
            return 0
            ;;
        swapped)
            if [ -n "${INSTALL_BACKUP:-}" ] && [ -d "$INSTALL_BACKUP" ]; then
                printf 'The new install is in place but not set up; the previous one is kept at %s. Re-run the installer to finish.' "$INSTALL_BACKUP"
            else
                printf 'The new install is in place but not set up; re-run the installer to finish.'
            fi
            return 0
            ;;
    esac

    _isl=""
    if [ -n "${INSTALL_BACKUP:-}" ]; then
        if [ -d "$INSTALL_BACKUP" ]; then
            _isl="The previous install is at $INSTALL_BACKUP"
        else
            _isl="The previous install was put back"
        fi
    fi
    if [ -n "${INSTALL_STOPPED:-}" ]; then
        # shellcheck disable=SC2086  # names are single words
        set -- $INSTALL_STOPPED
        if [ $# -eq 1 ]; then _isl_v=was; else _isl_v=were; fi
        _isl="${_isl:+$_isl; }$(_and_list "$@") $_isl_v stopped"
    fi
    if [ -n "${INSTALL_REMOVED_OLD:-}" ]; then
        if [ -n "$_isl" ]; then
            _isl="$_isl; the old install was removed"
        else
            _isl="The old install was removed"
        fi
    fi
    if [ -z "$_isl" ]; then
        printf 'Nothing on your printer was changed after this step.'
    else
        printf '%s; re-run the installer to finish.' "$_isl"
    fi
}

# Print usage
usage() {
    echo "HelixScreen Installer"
    echo ""
    echo "Usage: $0 [options] [ARCHIVE]"
    echo ""
    echo "Given a release archive (.zip or .tar.gz), it installs that archive."
    echo "Run from inside an extracted release package with no --version,"
    echo "--local or --update, it installs that package. Otherwise it installs"
    echo "the latest release for your update channel."
    echo ""
    echo "Options:"
    echo "  --update       Update existing installation (preserves config)"
    echo "  --source NAME  Channel running this install, for opt-in telemetry:"
    echo "                 $(install_source_allowlist | tr ' ' '|'). Kept across --update"
    echo "  --uninstall    Remove HelixScreen"
    echo "  --clean        Clean install: remove old installation completely,"
    echo "                 including config and caches. The disabled-services"
    echo "                 ledger is kept, so a later uninstall can still"
    echo "                 re-enable a stock UI this install disabled."
    echo "                 Asks for confirmation."
    echo "  --yes, -y      Confirm destructive prompts non-interactively."
    echo "                 Required for --clean when no terminal can answer"
    echo "                 (e.g. curl ... | sh -s -- --clean --yes)"
    echo "  --dry-run      Show what would be installed and changed, then exit."
    echo "                 Changes nothing; exits non-zero if a check would stop the install."
    echo "  --progress     Also print one machine-readable HELIX-PROGRESS line per event"
    echo "                 on stdout (see docs/devel/INSTALLER.md)."
    echo "  --verbose, -v  Print every detail line and command output as it happens."
    echo "                 The full log is always written either way."
    echo "  --version VER  Install specific version (default: latest)"
    echo "  --local FILE   Install from local archive (.zip or .tar.gz, skip download)."
    echo "                 Naming the archive without --local does the same"
    echo "  --skip-kiauh-registration"
    echo "                 Skip KIAUH extension registration (default: install if KIAUH detected)"
    echo "  --standalone   Self-managed install beside your printer's mod"
    echo "                 (Forge-X / Z-Mod). Default on such hosts is the"
    echo "                 payload install: contents replaced in place, no"
    echo "                 service installed or started (the mod owns the UI"
    echo "                 service), config/ and platform/ preserved."
    echo "  --payload-root PATH  Payload root (default: the mod's own tree on"
    echo "                 Forge-X hosts, AD5X and AD5M alike). A host that"
    echo "                 still carries an older standalone install is"
    echo "                 offered adoption of that root; declining leaves it"
    echo "                 untouched, with the migration steps printed"
    echo "  --desktop      x86_64 Linux: run HelixScreen in a window on this desktop,"
    echo "                 installed for your user only (no root, no service)"
    echo "  --kiosk        x86_64 Linux: take over the screen as a kiosk service."
    echo "                 Without either, a fresh install on a computer with a"
    echo "                 desktop asks (no terminal: desktop), and an existing"
    echo "                 install keeps its flavour"
    echo "  --auto-update  Also write the [update_manager helixscreen] stanza"
    echo "                 into the mod's user.moonraker.conf (opt-in: a stanza"
    echo "                 is a real side effect)"
    echo "  --help         Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                    # Fresh install, latest version"
    echo "  $0 --update           # Update existing installation"
    echo "  $0 --clean            # Remove old install completely, then install"
    echo "  $0 --clean --yes      # Same, without the interactive confirmation"
    echo "  $0 --version v1.1.0   # Install specific version"
    echo "  $0 --local /tmp/helixscreen-ad5m.tar.gz  # Install from local file"
    echo "  $0 --desktop helixscreen-x86-desktop-v1.1.0.tar.gz  # Same, no --local needed"
    echo "  ./helixscreen/install.sh --desktop  # Install the package you extracted"
    echo "  $0 --standalone       # Self-managed install beside the mod (mod hosts)"
    echo "  $0 --desktop          # x86_64: a window on your desktop, for your user"
}

# Parse the command line into the mode globals main() reads. Split out of
# main() so the parser is testable on its own (the mode flags especially:
# HELIX_MOD_PAYLOAD is the one variable that arms the mod-owned destruct
# exemption, and only this function may set it).
parse_installer_args() {
    update_mode=false
    uninstall_mode=false
    clean_mode=false
    ASSUME_YES=false
    # shellcheck disable=SC2034  # consumed by main()'s read-only pass
    DRY_RUN=false
    version=""
    local_tarball=""
    package_dir=""
    skip_kiauh_registration=false
    MOD_PAYLOAD_ROOT=""
    HELIX_MOD_PAYLOAD_UPDATES=""
    STANDALONE_INSTALL=""
    MOD_PAYLOAD_FLAG_GIVEN=""
    # shellcheck disable=SC2034  # consumed by record_install_source (printer_seed.sh)
    INSTALL_SOURCE_ARG=""
    # shellcheck disable=SC2034  # consumed by desktop.sh (resolve_x86_flavour)
    X86_FLAVOUR_FLAG=""

    while [ $# -gt 0 ]; do
        case $1 in
            --update)
                update_mode=true
                shift
                ;;
            --uninstall)
                uninstall_mode=true
                shift
                ;;
            --clean)
                clean_mode=true
                shift
                ;;
            --yes|-y|--force)
                # Explicit non-interactive consent for destructive prompts.
                # ASSUME_YES is read by clean_old_installation (uninstall.sh);
                # it is deliberately NOT inferred from a non-TTY stdin, since
                # the documented `curl ... | sh` invocation always has one.
                # shellcheck disable=SC2034  # consumed by uninstall.sh (clean_old_installation)
                ASSUME_YES=true
                shift
                ;;
            --dry-run)
                # shellcheck disable=SC2034  # consumed by main()'s read-only pass
                DRY_RUN=true
                shift
                ;;
            --progress)
                PROGRESS=1
                shift
                ;;
            --verbose|-v)
                # shellcheck disable=SC2034  # consumed by common.sh (log_info, run_logged)
                HELIX_INSTALL_VERBOSE=1
                shift
                ;;
            --version)
                if [ -z "${2:-}" ]; then
                    log_error "--version requires a version argument"
                    exit 1
                fi
                version="$2"
                shift 2
                ;;
            --local)
                if [ -z "${2:-}" ]; then
                    log_error "--local requires a file path argument"
                    exit 1
                fi
                _set_local_archive "$2"
                shift 2
                ;;
            --source)
                if [ -z "${2:-}" ]; then
                    log_error "--source requires a name"
                    exit 1
                fi
                # shellcheck disable=SC2034  # consumed by record_install_source (printer_seed.sh)
                INSTALL_SOURCE_ARG=$(install_source_normalize "$2")
                if [ "$INSTALL_SOURCE_ARG" != "$2" ]; then
                    log_warn "--source is not a known channel; recording \"$INSTALL_SOURCE_ARG\""
                fi
                shift 2
                ;;
            --skip-kiauh-registration)
                skip_kiauh_registration=true
                shift
                ;;
            --standalone)
                STANDALONE_INSTALL=1
                shift
                ;;
            --payload-root)
                if [ -z "${2:-}" ]; then
                    log_error "--payload-root requires a path argument"
                    exit 1
                fi
                MOD_PAYLOAD_ROOT="$2"
                shift 2
                ;;
            --desktop|--kiosk)
                if [ -n "$X86_FLAVOUR_FLAG" ] && [ "$X86_FLAVOUR_FLAG" != "${1#--}" ]; then
                    log_error "--desktop and --kiosk cannot be combined"
                    exit 1
                fi
                X86_FLAVOUR_FLAG="${1#--}"
                shift
                ;;
            --auto-update)
                # shellcheck disable=SC2034  # consumed by moonraker.sh (configure_moonraker_updates)
                HELIX_MOD_PAYLOAD_UPDATES=1
                shift
                ;;
            --no-mod-payload)
                # Deprecated alias for --standalone (pre-release courtesy).
                log_info "--no-mod-payload is deprecated; use --standalone"
                STANDALONE_INSTALL=1
                shift
                ;;
            --mod-payload-root)
                # Deprecated alias for --payload-root.
                if [ -z "${2:-}" ]; then
                    log_error "--mod-payload-root requires a path argument"
                    exit 1
                fi
                log_info "--mod-payload-root is deprecated; use --payload-root"
                MOD_PAYLOAD_ROOT="$2"
                shift 2
                ;;
            --mod-payload-updates)
                # Deprecated alias for --auto-update.
                log_info "--mod-payload-updates is deprecated; use --auto-update"
                # shellcheck disable=SC2034  # consumed by moonraker.sh (configure_moonraker_updates)
                HELIX_MOD_PAYLOAD_UPDATES=1
                shift
                ;;
            --mod-payload)
                # Compat no-op: the payload contract is auto-detected on hosts
                # the mod profile recognizes. Kept so documented invocations
                # keep working; mod_payload_mode_block logs the no-op notice.
                MOD_PAYLOAD_FLAG_GIVEN=1
                shift
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                # A release archive named on its own is what --local names.
                case "$1" in
                    *.zip | *.tar.gz | *.tgz)
                        if [ -f "$1" ]; then
                            _set_local_archive "$1"
                            shift
                            continue
                        fi
                        ;;
                esac
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done

    # A payload root is a payload-contract request; it cannot also name where
    # a self-managed install goes (that is INSTALL_DIR).
    if [ "${STANDALONE_INSTALL:-}" = "1" ] && [ -n "$MOD_PAYLOAD_ROOT" ]; then
        log_error "--payload-root cannot be combined with --standalone"
        exit 1
    fi

    # The uninstall branch never reaches the confirm point, where a dry run
    # stops, so it would remove everything.
    if [ "$uninstall_mode" = true ] && [ "$DRY_RUN" = true ]; then
        log_error "--dry-run cannot be combined with --uninstall; nothing was removed"
        exit 1
    fi
}

# Record the archive this run installs; naming two is a mistake, not a choice.
_set_local_archive() {
    if [ -n "$local_tarball" ] && [ "$local_tarball" != "$1" ]; then
        log_error "Only one archive can be installed at a time: $local_tarball and $1"
        exit 1
    fi
    local_tarball="$1"
}

# Auto-detect the payload contract: a bare install on a host the mod profile
# recognizes IS a payload install - the mod owns the payload tree and its OTA,
# so the in-place payload contract is the default and every standalone
# destructive shape stays refused. Runs BEFORE
# set_install_paths: its validate gate needs the contract armed to accept the
# mod's payload root as INSTALL_DIR.
#
# Overrides: --standalone opts back into the self-managed install; naming a
# payload root opts into the contract on any host (on a mod host that is
# already the default). HELIX_MOD_PAYLOAD stays env-scrubbed (host_profile.sh)
# - the probe and these flags are its only setters.
mod_payload_autodetect() {
    [ "${STANDALONE_INSTALL:-}" = "1" ] && return 0
    # An explicit --payload-root opts in on any host: the operator named the
    # root, which is the whole decision.
    if [ -n "${MOD_PAYLOAD_ROOT:-}" ]; then
        HELIX_MOD_PAYLOAD=1
        return 0
    fi
    # Auto-detect only the mod's own shape: the tree WITH its Buildroot
    # chroot, which both Forge-X layouts carry (AD5X and AD5M alike - one
    # mod, one contract shape, one descriptor). A probed tree without a
    # chroot is a mod mid-install or half-removed, so it requires an explicit
    # --payload-root. The chroot answer exists from host_profile_probe, which
    # main() runs before this.
    [ "${HOST_CHROOT_STATE:-none}" = "none" ] && return 0
    if [ -n "${HOST_MOD_ROOT:-}" ]; then
        HELIX_MOD_PAYLOAD=1
    fi
}

# The legacy AD5M population's adopt-or-warn. ad5m+forge_x hosts exist in the
# field with an install at /opt/helixscreen and an S90helixscreen service, and
# a payload install that silently relocated to the mod's default root would
# strand them. So the armed install OFFERS to adopt the legacy root as its
# payload root - outside the mod's git tree, which is also the OTA-durable
# answer - and a declined or unanswerable offer proceeds at the mod default
# with the exact manual migration commands.
#
# Nothing is ever deleted here: the legacy root and its service are the
# operator's to remove, with the commands printed below or the shipped
# uninstaller. An adoption IS recorded (record_payload_root_if_payload, after
# the confirm point), which is also how the next run resumes it without
# re-asking.
payload_legacy_adopt_or_warn() {
    # Only an armed, bare payload install reaching for the mod's default
    # root: an explicit --payload-root or INSTALL_DIR is the operator's own
    # choice, --standalone never reaches here armed, and uninstall is not an
    # install.
    [ "${HELIX_MOD_PAYLOAD:-}" = "1" ] || return 0
    [ -z "${MOD_PAYLOAD_ROOT:-}" ] || return 0
    [ -z "${_USER_INSTALL_DIR:-}" ] || return 0
    [ "$uninstall_mode" != true ] || return 0
    [ "${STANDALONE_INSTALL:-}" != "1" ] || return 0
    [ -n "${HOST_LEGACY_INSTALL_ROOT:-}" ] || return 0
    [ "${INSTALL_DIR:-}" = "${HOST_INSTALL_ROOT:-}" ] || return 0

    local legacy="$HOST_LEGACY_INSTALL_ROOT"
    local svc="${HOST_LEGACY_INIT_SCRIPT:-/etc/init.d/S90helixscreen}"

    # A prior adopt recorded its choice; resuming it is not relocation.
    if [ "$(read_payload_root_record 2>/dev/null || true)" = "$legacy" ]; then
        INSTALL_DIR="$legacy"
        validate_install_dir "$INSTALL_DIR" || exit 1
        log_info "Payload root: resuming the adopted root $INSTALL_DIR"
        log_info "(recorded in $(host_payload_root_record); clear that file to re-choose)"
        return 0
    fi

    if payload_legacy_prompt_adopt "$legacy"; then
        INSTALL_DIR="$legacy"
        # The probe's platform-keyed candidate, not an operator path: the
        # name gate still applies (a legacy root is a canonical platform
        # root, so this cannot fail on a real rig).
        validate_install_dir "$INSTALL_DIR" || exit 1
        log_info "Adopted the existing install at $INSTALL_DIR as the payload root"
        log_info "(outside the mod's git tree, so a Forge-X OTA cannot remove it)"
        # The service STAYS: it is the ONE boot path this adopted root has,
        # and the in-place update keeps the launcher it starts current. The
        # mod starts nothing of ours - a payload install at the mod's own root
        # gets OUR service inside the mod chroot's /etc/init.d, where
        # set_install_paths points INIT_SCRIPT_DEST and where Forge-X's
        # .root/start.sh (running inside the chroot) starts every S* it finds.
        # An adopted legacy root sits outside that chroot, so its host-side
        # S90 script is what boots it.
        # It is named for removal only at uninstall time, when it is stale.
        log_info "Keeping the standalone service $svc - it is this payload's"
        log_info "boot path: an adopted root sits outside the mod's chroot, so"
        log_info "the chroot's init.d cannot reach it."
        return 0
    fi

    log_warn "An older standalone HelixScreen install exists at $legacy"
    log_warn "(service: $svc). This payload install uses the mod's root instead:"
    log_warn "  $INSTALL_DIR"
    log_warn "The old install is left untouched. To finish the migration by hand:"
    log_warn "  cp $legacy/config/settings.json ${HOST_CONFIG_DIR:-}/settings.json"
    log_warn "  rm $svc"
    log_warn "  rm -rf $legacy"
    log_warn "Or adopt that root as the payload root:"
    log_warn "  --payload-root $legacy"
}

# Ask the adopt question on the terminal, which tty_confirm reaches under
# curl|sh too. With nothing that can answer, the offer is declined and the
# install proceeds at the mod default. --yes does not answer it: --yes
# consents to destructive prompts, and where the payload lands is a choice.
# Returns 0 to adopt.
payload_legacy_prompt_adopt() {
    local assume_yes="${ASSUME_YES:-false}" rc=0
    ASSUME_YES=false
    tty_confirm "Adopt the existing install at $1 as the payload root?" n || rc=1
    ASSUME_YES="$assume_yes"
    return "$rc"
}

# Payload-mode wiring. Runs after set_install_paths (INSTALL_DIR holds the
# mod's payload root, an explicit env INSTALL_DIR, or the platform default)
# and before the pre-flight checks, so every later step sees the mode's
# answers.
#
# Root precedence: --payload-root > an adopted legacy root (the operator's
# recorded or just-given choice) > whatever set_install_paths chose (an
# explicit env INSTALL_DIR > the host profile's HOST_INSTALL_ROOT).
mod_payload_mode_block() {
    if [ -n "${MOD_PAYLOAD_ROOT:-}" ]; then
        INSTALL_DIR="$MOD_PAYLOAD_ROOT"
        # The override gets the same gate every other INSTALL_DIR passes: it
        # must name helixscreen, and a mod-owned root needs the payload
        # contract this run is already in (mod_payload_autodetect arms it on
        # the flag, and --standalone + a payload root is refused at parse -
        # so the guard below is the enforced form of that invariant, not an
        # expectation about a caller elsewhere).
        host_refuse_mod_owned "install into" "$INSTALL_DIR"
        validate_install_dir "$INSTALL_DIR" || exit 1
        log_info "Payload root (--payload-root): $INSTALL_DIR"
    fi

    # The legacy-population choice, before anything reads or records the
    # root: it may move INSTALL_DIR off the mod default.
    payload_legacy_adopt_or_warn

    # Compat no-op notice for the old opt-in flag, whichever way it lands.
    if [ "${MOD_PAYLOAD_FLAG_GIVEN:-}" = "1" ]; then
        if [ "${HOST_SERVICE_MECHANISM:-}" = "mod-managed" ]; then
            log_info "--mod-payload is implied on this host (no effect)"
        else
            log_info "--mod-payload has no effect here; the payload contract applies"
            log_info "where your printer's mod (Forge-X / Z-Mod) owns the UI"
        fi
    fi

    # A payload root inside the mod's git tree does not survive a Forge-X
    # OTA -- their update_manager is type: git_repo and git clean -fd removes
    # every untracked path in it, a payload included.
    # Only the payload contract can reach a mod-owned INSTALL_DIR
    # (set_install_paths' install-dir gate refuses it otherwise), so this
    # fires in payload mode and never else. The probed default lives outside
    # the tree, so this reaches only an operator-chosen in-tree root.
    if host_path_is_mod_owned "${INSTALL_DIR:-}"; then
        log_warn "This payload root lives inside the firmware mod's git tree."
        log_warn "A Forge-X OTA removes it: their updater cleans untracked files"
        log_warn "in the mod's repo. Prefer a root outside the tree:"
        # Point at the default's own location: mod_data beside the mod tree,
        # the same root a bare install uses -- never a bare
        # <data-mount>/helixscreen path, which on the AD5M lands inside the
        # partition the vendor symlinks into Moonraker's gcodes root.
        # host_mod_data()'s fallback spelling exists on both layouts.
        log_warn "  --payload-root $(host_mod_data)/helixscreen"
    fi

    if [ "${HELIX_MOD_PAYLOAD:-}" != "1" ]; then
        # A self-managed install on a mod host is the operator's choice
        # (--standalone, or an explicit INSTALL_DIR); it is not refused, but
        # the service starts from the mod's chroot at boot, not from here.
        if [ "${HOST_SERVICE_MECHANISM:-}" = "mod-managed" ]; then
            log_warn "The firmware mod on this host owns the UI service."
            log_warn "This standalone install will not be started automatically."
            if [ "${STANDALONE_INSTALL:-}" = "1" ]; then
                log_warn "Re-run without --standalone for the payload install."
            else
                log_warn "An explicit INSTALL_DIR picks the root, not the contract;"
                log_warn "re-run without it, or name the root with --payload-root."
            fi
        fi
        return 0
    fi

    if [ "$uninstall_mode" != true ]; then
        # Capture what the PREVIOUS install recorded before this run's write
        # replaces it: --clean must sweep the root that exists on disk, and
        # its sweep runs after this block (clean_old_installation reads this
        # capture, since the record now names THIS run's root instead).
        # shellcheck disable=SC2034  # consumed by uninstall.sh (clean_old_installation sweeps it)
        HELIX_PRIOR_PAYLOAD_ROOT=$(read_payload_root_record 2>/dev/null || true)

        if [ "${MOD_PAYLOAD_FLAG_GIVEN:-}" = "1" ]; then
            log_info "--mod-payload: replacing payload contents in place at $INSTALL_DIR"
        else
            log_info "Payload install (auto-detected): replacing contents in place at $INSTALL_DIR"
        fi
        log_info "Service installs into the mod's chroot; it starts at boot"
    fi
    if [ "${HOST_SERVICE_MECHANISM:-}" != "mod-managed" ]; then
        log_warn "--payload-root names a root this host's profile did not find;"
        log_warn "applying the in-place payload contract anyway."
    fi
}

# Record where this payload install actually lands, so a later armed uninstall
# removes THIS root (its own --payload-root, else this record, else the probed
# default). main() calls it after the confirm point, before --clean sweeps:
# the sweep reads HELIX_PRIOR_PAYLOAD_ROOT, captured by mod_payload_mode_block
# before this write replaces the record. Install runs only: an uninstall must
# not re-point the record on its way out the door.
record_payload_root_if_payload() {
    [ "${HELIX_MOD_PAYLOAD:-}" = "1" ] || return 0
    record_payload_root "$INSTALL_DIR"
}

# Configure platform-specific settings before stopping competing UIs
# (ForgeX display mode, stock UI disable, screen.sh patching)
configure_platform() {
    # The stock FlashForge UI's own files gate this, not the mod flavor: a
    # mod-less AD5M (flavor stock) still ships /opt/auto_run.sh starting
    # ffstartup-arm, and that UI fights us for the framebuffer whatever
    # firmware is on the box. disable_stock_firmware_ui no-ops wherever those
    # files are absent.
    disable_stock_firmware_ui || true

    case "${AD5M_FIRMWARE:-}" in
        forge_x)
            # The display takeover is the one forgex step that must fail the
            # install: its non-zero return means the vendor UI still owns the
            # display slot, and an install that continued would ship two UIs
            # fighting over the framebuffer. The steps below it harden around
            # a takeover that already succeeded - best-effort by design.
            configure_forgex_display || {
                log_error "ForgeX display takeover failed - the vendor UI still owns the display slot"
                log_error "The install did not complete; address the ForgeX message above and re-run"
                return 1
            }
            dismiss_forgex_feather_promo || true
            patch_forgex_screen_sh || true
            patch_forgex_screen_drawing || true
            install_forgex_logged_wrapper || true
            ;;
        klipper_mod)
            # Klipper Mod-specific install-time configuration (if any)
            ;;
    esac
}

# Deploy platform-specific hooks for the init script
# Must be called after extract_release (hooks are in the release package)
install_platform_hooks() {
    local platform_hook
    platform_hook=$(resolve_platform_hook_key "$platform")

    if [ -n "$platform_hook" ]; then
        deploy_platform_hooks "$INSTALL_DIR" "$platform_hook"
    fi
}

# Print the post-detection platform banner.
#
# For Pi-class SBCs (platform=pi/pi32 — which covers a long tail of ARM Linux
# boxes including QIDI Q2/Plus, BTT CB1, MKS-Pi, generic Armbian) we lead with
# the friendly hardware label and reframe "pi" as the install package. Plain
# "Detected platform: pi" reads as wrong to anyone whose printer says QIDI on
# the lid — they see "pi" first and assume we mis-identified their device.
# Actual Raspberry Pi owners get the platform-first ordering.
#
# All other platforms (k1, k2, ad5m, snapmaker-u1, x86, …) get the single
# "Detected platform: X" line — there's no device-name ambiguity to clear up.
# ad5x is the exception on the non-Pi side: one install package covers the
# AD5X and both Creator 5 boards, so the board name leads and the key is
# reframed as the package, same shape as the non-Pi SBC case above.
#
# Sets HARDWARE_LABEL, the board name the plan's Printer line leads with
# (empty where the platform key already says it).
print_platform_banner() {
    local platform="$1"
    local _hw_label
    HARDWARE_LABEL=""

    if [ "$platform" = "ad5x" ]; then
        HARDWARE_LABEL=$(ad5x_board_name)
        log_info "Detected hardware: ${BOLD}${HARDWARE_LABEL}${NC}"
        log_info "Install package: ${BOLD}${platform}${NC} (unified MIPS FlashForge build)"
        return 0
    fi

    if [ "$platform" != "pi" ] && [ "$platform" != "pi32" ]; then
        log_info "Detected platform: ${BOLD}${platform}${NC}"
        return 0
    fi

    _hw_label=$(describe_hardware)
    HARDWARE_LABEL="$_hw_label"
    case "$_hw_label" in
        "Raspberry Pi"*)
            log_info "Detected platform: ${BOLD}${platform}${NC}"
            log_info "Hardware: ${_hw_label}"
            ;;
        *)
            log_info "Detected hardware: ${BOLD}${_hw_label}${NC}"
            log_info "Install package: ${BOLD}${platform}${NC} (generic ARM Linux build, compatible with your SBC)"
            ;;
    esac
}

# Tell a K2 owner how AI failure detection stands after this install (#1378).
#
# Two independent parts of the install stop the STOCK detector, and neither
# announces itself:
#
#   - assets/config/platform/hooks-k2.sh platform_stop_competing_uis() stops and
#     disables /etc/init.d/app. That procd service is the stock UI, and the same
#     service carries Creality's capture/infer/pause daemons (Monitor,
#     master-server, app-server). The init script re-runs the hook on every
#     start, so the disable is re-asserted at each boot.
#   - install_camera_k2() (camera.sh) hands /dev/video0 to ustreamer, which
#     kills the stock cam_app grabber on every launch. The node is single-stream,
#     so nothing else can read the chamber camera while HelixScreen runs.
#
# HelixScreen ships its own spaghetti detector for exactly this gap: while a
# print is active it captures a frame from the ustreamer camera and runs
# /usr/bin/detection (Creality's on-device model) on it, pausing the print on a
# hit. The notice says both halves - stock service stops, HelixScreen detection
# takes over - so the owner knows what is watching their prints.
#
# The text stays ASCII: it lands on a BusyBox console on the printer. No-op on
# every other platform.
print_k2_stock_ai_notice() {
    [ "${1:-}" = "k2" ] || return 0

    log_warn "Creality's stock AI failure detection service is now DISABLED on this"
    log_warn "  printer: HelixScreen replaces the stock UI (/etc/init.d/app), which also"
    log_warn "  runs Creality's failure detector, and takes over the chamber camera."
    log_warn "  HelixScreen provides its own spaghetti detection instead: while a print"
    log_warn "  is active it captures a chamber-camera frame, runs Creality's on-device"
    log_warn "  detection model (/usr/bin/detection) on it, and pauses the print with a"
    log_warn "  warning when it sees spaghetti - or only warns, if detection is set not"
    log_warn "  to pause. Its interval and threshold come from the printer's own"
    log_warn "  ai_control settings (pastaTime / pastaTruth); the on/off and pause"
    log_warn "  choices start from those settings (switch / pausePrint) and then live in"
    log_warn "  Settings > Safety & Alerts."
    log_warn "  To get the stock detector back, uninstall HelixScreen:"
    log_warn "    curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall"
    log_warn "  Uninstalling re-enables both the stock UI service and the stock camera."
}

# Refuse to run --uninstall from a script sitting inside the dir we're about to
# delete. The release tarball ships install.sh into $INSTALL_DIR for
# offline --local updates; users sometimes invoke that copy with --uninstall,
# which "works" on Linux only because the kernel keeps the inode open after rm.
# Force the user to copy out. No-op (returns 0) when INSTALL_DIR isn't known
# yet or $0 lives elsewhere, so it's safe to call more than once.
_refuse_uninstall_from_install_dir() {
    local _script_dir _script_abs _install_norm
    _script_dir="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || _script_dir=""
    [ -n "$_script_dir" ] || return 0
    [ -n "${INSTALL_DIR:-}" ] || return 0
    _script_abs="${_script_dir}/$(basename "$0")"
    _install_norm="${INSTALL_DIR%/}"
    case "$_script_abs" in
        "$_install_norm"/* | "$_install_norm")
            log_error "Refusing to run --uninstall from inside \$INSTALL_DIR"
            log_error "  script:      $_script_abs"
            log_error "  INSTALL_DIR: $INSTALL_DIR"
            log_error ""
            log_error "Copy the script out first, then re-run:"
            log_error "  cp '$_script_abs' /tmp/install.sh"
            log_error "  sh /tmp/install.sh --uninstall"
            exit 1
            ;;
    esac
}

# Refuse to run on a device where HelixScreen is managed by the firmware
# (e.g. PAXX's Snapmaker U1 firmware, which bind-mounts our binary over
# /usr/bin/gui, supervises it via lmd, and owns updates through a pinned
# helixscreen-pkg). Running the standalone installer there would trample the
# firmware-managed setup. Detection: either marker present ⇒ firmware-managed.
#
# Advanced users can force past the guard with HELIX_IGNORE_FIRMWARE_MANAGED=1.
# Tests set HELIX_FIRMWARE_MANAGED_MARKER to a temp root to exercise the guard
# without touching the real /oem or /etc trees.
_refuse_if_firmware_managed() {
    if [ "${HELIX_IGNORE_FIRMWARE_MANAGED:-0}" = "1" ]; then
        log_warn "HELIX_IGNORE_FIRMWARE_MANAGED=1 set — skipping firmware-managed guard"
        return 0
    fi

    _fw_root="${HELIX_FIRMWARE_MANAGED_MARKER:-}"
    _fw_dir_marker="${_fw_root}/oem/apps/helixscreen"
    _fw_hook_marker="${_fw_root}/etc/hooks/lmd.d/30-helixscreen.sh"

    if [ -d "$_fw_dir_marker" ] || [ -f "$_fw_hook_marker" ]; then
        if [ -d "$_fw_dir_marker" ]; then
            _fw_detected="$_fw_dir_marker"
        else
            _fw_detected="$_fw_hook_marker"
        fi
        log_error "=========================================="
        log_error "HelixScreen is managed by your firmware on this device."
        log_error "Detected: $_fw_detected"
        log_error "=========================================="
        log_error ""
        log_error "Use the firmware configuration to enable, disable, or update"
        log_error "HelixScreen (Snapmaker Components > Touchscreen GUI)."
        log_error "Do NOT run this installer — it would overwrite the"
        log_error "firmware-managed setup."
        log_error ""
        log_error "To override this check anyway (advanced users), re-run with:"
        log_error "  HELIX_IGNORE_FIRMWARE_MANAGED=1 sh install.sh"
        exit 1
    fi
}

# Main installation flow
main() {
    # Probe the host once, before anything consults it: the mod-ownership
    # guard backs set_install_paths' install-dir gate (and detect_tmp_dir's
    # override branch), so HOST_MOD_ROOT must already be probed by then.
    host_profile_probe

    # Parse arguments, then settle the payload contract BEFORE set_install_paths:
    # its install-dir gate needs to know whether the mod's payload root is ours
    # to write. Autodetect (probe) plus the operator's overrides decide.
    parse_installer_args "$@"
    mod_payload_autodetect
    # Before the platform settles: an x86 package's flavour feeds that choice.
    detect_package_beside_installer

    # Self-delete safety guard runs as early as possible — before platform
    # detection, which exits on "unsupported" hardware and would otherwise
    # preempt the guard. Only fires when INSTALL_DIR is already known (env);
    # the post-set_install_paths call below covers the normal runtime case.
    if [ "$uninstall_mode" = true ]; then
        _refuse_uninstall_from_install_dir
    fi

    # Refuse to install/update over a firmware-managed HelixScreen. Runs before
    # platform detection and any install work so we never trample the firmware's
    # setup. Uninstall is exempt — leave the firmware's own teardown to it.
    if [ "$uninstall_mode" != true ]; then
        _refuse_if_firmware_managed
    fi

    # Detect platform. On x86 the flavour is part of the answer: a desktop
    # install is platform x86-desktop from here on, with its own download.
    platform=$(detect_platform)
    print_platform_banner "$platform"
    resolve_x86_flavour
    _progress_emit platform "$platform"
    # For platforms that share a binary with pi/pi32 (e.g. m1), the download
    # URL uses the donor platform key while $platform stays put so hooks,
    # banner, and competing-UI shutdown still target the real device.
    download_platform=$(get_download_platform "$platform")

    # AD5X: refuse to run outside the ZMOD chroot — applies to fresh install,
    # --update, --uninstall, and --local. Inside the chroot the check is a
    # no-op, and a Forge-X host never trips it (its install is host-side into
    # the mod's git tree, so the guard is inert there).
    if [ "$platform" = "ad5x" ]; then
        mod_check_chroot_context
    fi

    if [ "$platform" = "unsupported" ]; then
        log_error "Unsupported platform: $(uname -m)"
        log_error "HelixScreen supports:"
        log_error "  - Raspberry Pi (aarch64/armv7l)"
        log_error "  - FlashForge Adventurer 5M (armv7l)"
        log_error "  - FlashForge AD5X / Creator 5 / Creator 5 Pro (mips, Z-Mod)"
        log_error "  - Creality K1 series with Simple AF"
        log_error "  - Creality K2 series (K2/K2 Pro/K2 Plus)"
        log_error "  - x86_64 Debian/Ubuntu (x86_64)"
        exit 1
    fi

    # For AD5M/AD5X/K1, detect the firmware/mod flavor and set appropriate paths.
    # The FlashForge mods ship for both Adventurer platforms, so ad5m and ad5x
    # share one detector — a Forge-X AD5X gets the Forge-X layout, not the
    # ZMOD one the bare ad5x paths describe. AD5M_FIRMWARE is the compat alias
    # for MOD_FLAVOR.
    local firmware=""
    if [ "$platform" = "ad5m" ] || [ "$platform" = "ad5x" ]; then
        MOD_FLAVOR=$(detect_mod_flavor)
        AD5M_FIRMWARE="$MOD_FLAVOR"
        firmware="$MOD_FLAVOR"
    elif [ "$platform" = "k1" ]; then
        K1_FIRMWARE=$(detect_k1_firmware)
        firmware="$K1_FIRMWARE"
    fi
    set_install_paths "$platform" "$firmware"

    # --mod-payload mode wiring (root precedence, OTA warning, mod-host
    # notice) - after detection, before the requirements checks.
    mod_payload_mode_block
    settle_package_beside_installer

    # Check permissions
    check_permissions "$platform"

    # Handle uninstall (doesn't need all checks). The self-delete guard also
    # ran early (before platform detection); this second call covers the normal
    # case where INSTALL_DIR was computed by set_install_paths just above.
    if [ "$uninstall_mode" = true ]; then
        _refuse_uninstall_from_install_dir
        uninstall "$platform"
        exit 0
    fi

    # Defensive: if uninstall_mode is still true at this point, the early
    # exit above is broken — fail loudly rather than running the install
    # path, which REINSTALLS HelixScreen in answer to --uninstall.
    if [ "$uninstall_mode" = true ]; then
        log_error "internal error: install path entered with uninstall_mode=true"
        log_error "please report at https://github.com/prestonbrown/helixscreen/issues"
        exit 99
    fi

    # Pre-flight checks. Everything from here to the confirm point only reads:
    # --dry-run exits there, and the plan screen promises nothing has changed.
    # A static test holds every call here to a read-only allowlist.
    log_info "Running pre-flight checks..."
    detect_missing_unzip
    check_requirements
    detect_missing_runtime_deps "$platform"
    check_disk_space "$platform"
    detect_init_system
    check_klipper_ecosystem "$platform"

    # An update or reinstall must follow the channel the installed app is on:
    # a beta user updating through KIAUH or a re-run of the installer would
    # otherwise be handed the stable build. No-op on a fresh install.
    # Taken before anything is written: install-time settings seeding is for a
    # fresh install only.
    fresh_install=true
    if [ "$update_mode" = true ] || [ -d "$INSTALL_DIR" ]; then
        fresh_install=false
        resolve_update_channel
    fi

    # Get version: a handed-over archive or package names its own.
    if [ -n "$local_tarball" ]; then
        # Validate local file exists
        if [ ! -f "$local_tarball" ]; then
            log_error "Local archive not found: $local_tarball"
            exit 1
        fi
        # Extract version from filename if possible. Only the tar.gz layout
        # carries a version in the name (helixscreen-<plat>-v1.2.3.tar.gz).
        # The unversioned helixscreen-<plat>.zip gets "local" as a placeholder.
        version=$(parse_tarball_version "$local_tarball")
        if [ -z "$version" ]; then
            version="local"
        fi
        log_info "Installing from local file: ${BOLD}${local_tarball}${NC}"
    elif [ -n "$package_dir" ]; then
        # The package beside the installer is what was handed over, whatever
        # the channel's latest is.
        # shellcheck disable=SC2154  # set by release.sh (detect_package_beside_installer)
        version="$package_version"
        # shellcheck disable=SC2154  # set by release.sh (detect_package_beside_installer)
        log_info "Installing the package next to this installer: ${version} (${package_platform})"
    elif [ -z "$version" ]; then
        version=$(get_latest_version "$download_platform")
    fi
    # A prerelease installed by name, as --version, an archive whose name
    # carries it, or the package beside the installer, puts the install on
    # beta. After resolve_update_channel, so a channel read from the installed
    # config outranks it. A stable version and an unversioned zip ("local")
    # move nothing, and the latest a channel resolved to is already on it.
    match_channel_to_version "$version"
    log_info "Target version: ${BOLD}${version}${NC}"

    # The release must exist before the plan offers it; HEAD requests only.
    probe_release "$version" "$download_platform"
    # After detect_init_system: systemd units are only looked for on systemd.
    detect_competing_uis
    detect_moonraker_integration "$platform"
    detect_kiauh

    confirm_point "$platform" "$version"
    apply_install
}

# The Installing libraries step: done with what apt added, hidden when nothing
# was missing, failed when apt could not install a package. The install goes
# on either way; verify_binary_deps stops it if a library it needs is missing.
install_libraries_step() {
    local libs ok=true
    libs=$(plan_missing_libs)
    step "Installing libraries" "Installed libraries"
    install_missing_unzip || ok=false
    install_runtime_deps "$platform" || ok=false
    if [ "$ok" != true ]; then
        step_fail "continuing; see above"
    elif [ -n "$libs" ]; then
        step_done "$libs"
    else
        step_skip
    fi
}

# The Downloading step: fetch (or stage the --local) archive into TMP_DIR.
download_step() {
    local detail
    if [ -n "$local_tarball" ]; then
        step "Unpacking local archive" "Unpacked local archive"
        use_local_tarball "$local_tarball"
    elif [ -n "${package_dir:-}" ]; then
        step "Using the package next to the installer" "Used the package next to the installer"
        use_package_tree "$package_dir"
    else
        step "Downloading" "Downloaded"
        download_release "$version" "$download_platform"
    fi
    detail=$(file_size_text "$(_archive_tmp_path)")
    if [ -n "${ARCHIVE_SHA256_VERIFIED:-}" ]; then
        detail="${detail:+$detail$(_plan_sep)}SHA256 verified"
    fi
    step_done "$detail"
}

# Everything after the confirm point, as the steps plan_count_steps counted:
# a step with nothing to do is skipped only under the same conditions that
# left it out of the count, or the [n/N] numbering is wrong.
apply_install() {
    local uis stopping seed_pid

    if [ "${platform:-}" = "x86-desktop" ]; then
        apply_desktop_install
        return 0
    fi

    # The machine changes from here on.
    if [ "$platform" = "ad5m" ]; then
        cleanup_ad5m_gcodes_root
    fi
    record_payload_root_if_payload
    # A write probe sudo would have had to prompt for runs now that it can.
    if [ -n "${DISK_CHECK_DEFERRED:-}" ]; then
        check_disk_space "$platform"
    fi

    install_libraries_step

    # Download/stage the release archive BEFORE any step that modifies the
    # running printer (stock-UI disable, competing-UI shutdown, old-install
    # cleanup, service stop): a failed download must leave the machine exactly
    # as it was - stock UI enabled, old install intact, service running. The
    # download also needs the network, and stopping UIs can take it away
    # (e.g. Snapmaker U1's stock GUI owns wpa_supplicant, so restarting it
    # drops WiFi/SSH mid-update).
    download_step

    # Configure platform-specific settings before stopping UIs. The title
    # names what detection found; without a find the step stays hidden
    # unless the platform half fails.
    uis="${COMPETING_UIS_FOUND:-}"
    stopping=$(stop_step_noun)
    step "Stopping $stopping" "Stopped $stopping"
    configure_platform
    INSTALL_STOPPED="${INSTALL_STOPPED:-}${uis:+ $uis}"
    stop_competing_uis
    if [ -n "$uis" ]; then step_done; else step_skip; fi

    step "Installing files" "Installed files"
    # Clean old installation if requested
    if [ "$clean_mode" = true ]; then
        INSTALL_REMOVED_OLD=1
        clean_old_installation "$platform"
    fi

    if [ "$update_mode" = true ]; then
        if [ ! -d "$INSTALL_DIR" ]; then
            log_warn "No existing installation found. Performing fresh install."
        elif ! _is_self_update; then
            INSTALL_STOPPED="HelixScreen${INSTALL_STOPPED:+ $INSTALL_STOPPED}"
        fi
        stop_service "$platform"
    fi

    # Clear the payload's directory of any state kept there, before anything
    # extracts on top of it.
    migrate_previous_state_dir
    # Rename a state root whose name had to change (AD5M dot-prefix), before
    # the new payload's hook looks for it at the new name.
    migrate_state_root

    extract_release "$platform"
    fix_install_ownership
    open_plugin_to_moonraker
    step_done "$(display_path "$INSTALL_DIR")"

    step "Setting up service" "Set up service"
    install_service "$platform"
    install_platform_hooks

    # System permission rules for a non-root service user: the backlight udev
    # rule (makes /sys/class/backlight/*/brightness group-writable by video, so
    # dimming and sleep work) and the NetworkManager polkit rule.
    #
    # Placement is load-bearing on both sides. It must come after
    # extract_release, because the udev rule ships inside the release package at
    # $INSTALL_DIR/config/, and before start_service, so udevadm has already
    # re-applied the ownership by the time the UI first writes brightness.
    #
    # Runs on fresh install and --update alike (both reach this line), and
    # self-skips on the root-only platforms (ad5m/ad5x/k1/k2), when KLIPPER_USER
    # is root, and under NoNewPrivileges where sudo is unavailable.
    install_permission_rules "$platform"

    # QIDI: take over the .3mf plate-thumbnail duty the stopped stock screen
    # carried (prestonbrown/helixscreen#1713). No-op off QIDI-class hosts and
    # on firmware whose Moonraker extracts thumbnails itself. Post-extract
    # because the helper and its unit templates ship in the payload's config/.
    install_qidi_3mf_thumbs

    # Install KIAUH extension if KIAUH is detected
    install_kiauh_extension "$skip_kiauh_registration" || true

    # K1: ensure SSH (dropbear) is running — disabling S99start_app also stops
    # SSH (#535). Runs on both fresh install and self-update.
    if [ "$platform" = "k1" ]; then
        ensure_k1_ssh
        # Re-record the stock UI disable against the payload that just landed:
        # extract_release replaces INSTALL_DIR between the stop step and the
        # config symlink setup, and a self-update run skips the stop step
        # entirely. Idempotent (record_disabled_service dedups).
        record_k1_stock_ui_disable
        # Install and start the stock Creality backend trio
        # (prestonbrown/helixscreen#1468). Runs post-extract (the init script
        # ships in the release package) and on self-update, which skips
        # stop_competing_uis and would otherwise never see it installed.
        install_k1_creality_backend
    fi

    # Verify all shared library dependencies are satisfied before starting
    verify_binary_deps "$platform"

    # Create platform cache directory
    case "$platform" in
        ad5m)
            $SUDO mkdir -p /data/.helixscreen/cache
            ;;
        k1)
            $SUDO mkdir -p /usr/data/helixscreen/cache
            ;;
    esac
    step_done

    step "Connecting to Moonraker" "Connected to Moonraker"
    # Symlink config into printer_data (Pi/Klipper only - enables web UI editing)
    setup_config_symlink

    # State the env file's owner/mode rather than inheriting them from the
    # extract/restore umask; the launcher refuses to evaluate anything else.
    # After setup_config_symlink so a migrated printer_data copy is pinned too.
    pin_env_file

    # Configure Moonraker update_manager (Pi only - enables web UI updates)
    configure_moonraker_updates "$platform"
    step_done "$(moonraker_step_detail)"

    # Everything left prepares the first start, and its step resolves when
    # the UI starts. A payload install's UI starts from the mod at boot, so
    # its step only finishes the setup.
    if plan_starts_ui; then
        step "Starting HelixScreen" "Started HelixScreen"
    else
        step "Finishing setup" "Finished setup"
    fi

    # K2: replace the stock proprietary WebRTC camera (which HelixScreen and
    # fluidd can't consume) with a static ustreamer MJPEG server and point both
    # UIs at it. No-op on every other platform. Runs after the binary is
    # extracted and Moonraker is reachable; safe (detect-first) and reversible
    # (stock WebRTC disable is recorded for re-enable on uninstall).
    install_camera_k2 "$platform" || true

    # Route the built-in screen server through the web UI's nginx, behind
    # Moonraker's auth, where nginx fronts Moonraker. Every platform; skips
    # cleanly (the app then binds the LAN and pairs) where it cannot.
    install_remote_screen_route || true

    # Install platform-specific helix-recover.sh used by PrinterRecoveryService
    # when klippy_uds is dead and firmware_restart can't proxy. No-op on stock
    # systemd platforms (pi/pi32/x86) where services.restart handles it.
    configure_local_recovery "$platform"

    # Fix known Klipper config issues (AD5M screw_thread, etc.)
    fix_ad5m_klipper_config || true

    # Generic per-printer install-time layer (#986): detect a known printer
    # model and apply its bundled settings seed + Klipper include, if any.
    # detect_printer_model() is conservative and returns empty for unknown
    # hardware, so this is a no-op on every platform without a registered id.
    seed_pid=$(detect_printer_model)
    if [ -n "$seed_pid" ]; then
        log_info "Recognized printer model: ${seed_pid} -- applying install-time defaults"
        install_klipper_include_for_printer "$seed_pid" || true
    fi
    seed_install_time_settings "$fresh_install" "$seed_pid"
    seed_update_channel
    record_install_source

    # The camera, include and seed steps above record their uninstall ledgers
    # as plain files in the install dir. Relocate them into printer_data now,
    # or a Moonraker web update would delete them before the next installer run.
    setup_config_symlink quiet

    # Configure ALSA "default" when the board has no card 0 (e.g. Pi + HDMI-audio
    # screens like the BTT HDMI5, whose only outputs are vc4hdmi0/vc4hdmi1 at
    # indices 1/2). Safe no-op when "default" already works or /etc/asound.conf
    # already exists.
    configure_alsa_default || true

    # K2: install the web-server carve-out's init script
    # (prestonbrown/helixscreen#1617) BEFORE the service start, so the
    # hook's first platform_stop_competing_uis finds the script and the
    # procd instance is registered from the first launch. No-op off K2.
    # The || guard keeps a carve-out failure non-fatal under set -eu: a
    # supplementary backend must not abort the install — the function has
    # already logged the fault and the manual fix.
    install_k2_webserver_backend "$platform" ||
        log_warn "Web-server carve-out incomplete; the UI install itself is fine"

    # Every step that writes into printer_data has run, and the service must
    # find that directory writable on its first start.
    fix_user_config_ownership

    # Start service
    start_service "$platform"

    # K2: bring the carve-out up for this session. The service start just
    # ran the hook, whose app stop (killall -9) took whatever web-server
    # was live down; the hook's own restore, this start, and procd's
    # respawn bring it back. Belt-and-braces for paths that bypass the
    # hook. No-op off K2; non-fatal on the same || contract.
    start_k2_webserver_backend "$platform" ||
        log_warn "Web-server carve-out not started; the UI install itself is fine"
    step_done
    # Past here only cleanup runs: the new install is in place,
    # so a failure report has no rollback state to describe.
    INSTALL_COMPLETE=1

    cleanup_old_install
    cleanup_migrated_install
    cleanup_superseded_payload
    cleanup_stale_cache_dirs
    retire_legacy_config_backups

    finalize_install_log
    cleanup_on_success
    print_summary "$version"

    if [ "$platform" = "ad5m" ] || [ "$platform" = "k1" ] || [ "$platform" = "k2" ]; then
        log_note "You may need to reboot for the display to update."
    fi

    # Last thing on screen: the safety feature this install took away.
    print_k2_stock_ai_notice "$platform"
}

# The desktop flavour's steps after the confirm point. A separate list rather
# than skips threaded through apply_install: a desktop install shares the
# screen and the machine, so of the kiosk steps (libraries, competing UIs,
# service, permission rules, Moonraker, KIAUH, recovery, ALSA, printer seeding)
# none applies, and plan_count_steps counts exactly these four.
apply_desktop_install() {
    download_step

    step "Installing files" "Installed files"
    if [ "$clean_mode" = true ]; then
        INSTALL_REMOVED_OLD=1
        remove_desktop_root "$INSTALL_DIR" || exit 1
    fi
    extract_release "$platform"
    verify_binary_deps "$platform"
    seed_update_channel
    record_install_source
    step_done "$(display_path "$INSTALL_DIR")"

    step "Adding HelixScreen to the app menu" "Added HelixScreen to the app menu"
    if write_desktop_entry; then
        step_done "$(display_path "$(desktop_entry_path)")"
    else
        step_fail "launch it from $(display_path "$INSTALL_DIR/bin/helix-screen")"
    fi
    INSTALL_COMPLETE=1

    cleanup_old_install
    finalize_install_log
    cleanup_on_success
    print_summary "$version"
}
