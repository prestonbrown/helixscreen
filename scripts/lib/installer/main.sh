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

# Remove the scratch dir however the installer ends.
#
# The ERR trap above is a bash extension and is silently discarded on the
# ash/dash shells every embedded platform runs, so an interrupted or failing
# install used to leak the whole download: a K2 was found holding a 60MB
# helixscreen.zip from four months earlier, on a 240MB overlay partition.
#
# cleanup_on_success is idempotent (it tests for the directory first) so the
# explicit call on the success path is unaffected, and it routes through
# _safe_remove_tmp_dir, which is what refuses to rm -rf a mountpoint.
trap 'cleanup_on_success' EXIT INT TERM

# Print usage
usage() {
    echo "HelixScreen Installer"
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --update       Update existing installation (preserves config)"
    echo "  --uninstall    Remove HelixScreen"
    echo "  --clean        Clean install: remove old installation completely,"
    echo "                 including config and caches (asks for confirmation)"
    echo "  --yes, -y      Confirm destructive prompts non-interactively."
    echo "                 Required for --clean when stdin is not a terminal"
    echo "                 (e.g. curl ... | sh -s -- --clean --yes)"
    echo "  --version VER  Install specific version (default: latest)"
    echo "  --local FILE   Install from local archive (.zip or .tar.gz, skip download)"
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
    echo "  $0 --standalone       # Self-managed install beside the mod (mod hosts)"
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
    version=""
    local_tarball=""
    skip_kiauh_registration=false
    MOD_PAYLOAD_ROOT=""
    HELIX_MOD_PAYLOAD_UPDATES=""
    STANDALONE_INSTALL=""
    MOD_PAYLOAD_FLAG_GIVEN=""

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
                local_tarball="$2"
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
                # keep working; mod_payload_mode_block says what it did.
                MOD_PAYLOAD_FLAG_GIVEN=1
                shift
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
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
}

# Auto-detect the payload contract (2026-08-31 steer): a bare install on a
# host the mod profile recognized IS a payload install - the mod owns the UI
# service and its OTA, so the in-place payload contract is the default and
# every standalone destructive shape stays refused. Runs BEFORE
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
    # chroot, which both Forge-X layouts carry (the AD5X rig's bootstrap and
    # mod-managed service the rig cycle verified; the AD5M rig shares the
    # same contract shape - one mod, one descriptor). A probed tree without
    # a chroot is a mod mid-install or half-removed, so it keeps the
    # pre-payload behavior (explicit --payload-root only). The chroot answer
    # exists from host_profile_probe, which main() runs before this.
    [ "${HOST_CHROOT_STATE:-none}" = "none" ] && return 0
    if [ -n "${HOST_MOD_ROOT:-}" ]; then
        HELIX_MOD_PAYLOAD=1
    fi
}

# The legacy AD5M population's adopt-or-warn (Task 10). Before the payload
# contract, our installer put ad5m+forge_x hosts at /opt/helixscreen with an
# S90helixscreen service; that population still exists, and a payload install
# that silently relocated to the mod's default root would strand it (A1's
# no-silent-conversion rule). So the armed install OFFERS to adopt the legacy
# root as its payload root - outside the mod's git tree, which is also the
# OTA-durable answer (Open Decision 1) - and a declined or unanswerable offer
# proceeds at the mod default with the exact manual migration commands.
#
# Nothing is ever deleted here: the legacy root and its service are the
# operator's to remove, with the commands printed below or the shipped
# uninstaller. An adoption IS recorded (the record write later in
# mod_payload_mode_block), which is also how the next run resumes it without
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
        # The service STAYS: the mod's own service starts only the mod's tree
        # (their .shell/helixscreen.sh: HELIX_ROOT=$MOD_ROOT/.bin/helixscreen)
        # and the payload contract installs none, so this legacy script is the
        # ONE boot path the adopted root has - the in-place update keeps the
        # launcher it starts current. "Mod owns the service" describes a root
        # inside its tree; this is the one payload install with OUR service.
        # It is named for removal only at uninstall time, when it is stale.
        log_info "Keeping the standalone service $svc - it is this payload's boot"
        log_info "path (the mod's service starts only the mod's tree; the payload"
        log_info "contract installs none of its own)."
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

# Ask the adopt question where it can be answered: a TTY. The curl|sh pipe
# cannot answer (stdin carries the script), so every other stdin declines -
# the same rule confirm_clean_install applies, except a decline proceeds at
# the mod default rather than aborting.
# Returns 0 to adopt.
payload_legacy_prompt_adopt() {
    [ -t 0 ] || return 1
    printf "Adopt the existing install at %s as the payload root? [y/N] " "$1"
    read -r response
    case "$response" in
        [yY][eE][sS]|[yY]) return 0 ;;
        *) return 1 ;;
    esac
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

    # Open Decision 1: a payload root inside the mod's git tree does not
    # survive a Forge-X OTA -- their update_manager is type: git_repo and
    # git clean -fd removes .bin/helixscreen, which is untracked there.
    # Only the payload contract can reach a mod-owned INSTALL_DIR
    # (set_install_paths' install-dir gate refuses it otherwise), so this
    # fires in payload mode and never else.
    if host_path_is_mod_owned "${INSTALL_DIR:-}"; then
        log_warn "This payload root lives inside the firmware mod's git tree."
        log_warn "A Forge-X OTA removes it: their updater cleans untracked files"
        log_warn "in the mod's repo. Prefer a root outside the tree:"
        # The example must exist on THIS rig: the mod's data mount (/usr/data
        # on the AD5X, /data on the AD5M), not the hard-coded AD5X path an
        # AD5M operator would follow onto a partition their rig does not
        # have. Unprobed corner (flag-armed, no chroot): keep the AD5X
        # literal, the shape that path was written for.
        local od1_mount
        od1_mount="$(host_mod_data_mount)"
        [ -n "$od1_mount" ] || od1_mount="/usr/data"
        log_warn "  --payload-root $od1_mount/helixscreen"
    fi

    if [ "${HELIX_MOD_PAYLOAD:-}" != "1" ]; then
        # A self-managed install on a mod host is the operator's choice
        # (--standalone, or an explicit INSTALL_DIR); it is not refused, but
        # the mod owns the UI service, so this installer will never start it.
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
        # Record where this payload install actually landed, so a later armed
        # uninstall removes THIS root (its own --payload-root, else this
        # record, else the probed default). Install runs only: an uninstall
        # must not re-point the record on its way out the door.
        record_payload_root "$INSTALL_DIR"

        if [ "${MOD_PAYLOAD_FLAG_GIVEN:-}" = "1" ]; then
            log_info "--mod-payload: replacing payload contents in place at $INSTALL_DIR"
        else
            log_info "Payload install (auto-detected): replacing contents in place at $INSTALL_DIR"
        fi
        log_info "The mod owns the UI service; none is installed or started"
    fi
    if [ "${HOST_SERVICE_MECHANISM:-}" != "mod-managed" ]; then
        log_warn "--payload-root names a root this host's profile did not find;"
        log_warn "applying the in-place payload contract anyway."
    fi
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
            configure_forgex_display || true
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
    local platform_hook=""
    case "${AD5M_FIRMWARE:-}" in
        forge_x)     platform_hook="ad5m-forgex" ;;
        klipper_mod) platform_hook="ad5m-kmod" ;;
        zmod)        platform_hook="ad5m-zmod" ;;
    esac

    # Platform hooks (pi32 shares Pi hooks). The AD5X used to share ad5m-zmod on
    # the assumption that both ZMOD firmwares have the same layout; they do not.
    # The AD5X runs inside a chroot at /usr/data/.mod/.zmod, installs to
    # /srv/helixscreen, and has no /data at all, so the AD5M hook's
    # HELIX_CACHE_DIR=/data/helixscreen/cache pointed at a path that is not there.
    case "$platform" in
        pi|pi32)       platform_hook="pi" ;;
        k1)            platform_hook="k1" ;;
        k2)            platform_hook="k2" ;;
        cc1)           platform_hook="cc1" ;;
        m1)            platform_hook="m1" ;;
        ad5x)          platform_hook="ad5x" ;;
        snapmaker-u1)  platform_hook="snapmaker-u1" ;;
    esac

    # A probed mod host outranks both dispatches above. HOST_PLATFORM_HOOK_KEY
    # is set only when the mod's own tree layout was found, and names the
    # payload layout that rig actually runs. Without this a Forge-X AD5X
    # reports platform=ad5x AND flavor=forge_x — the flavor case picks
    # ad5m-forgex, the platform case overrides to ad5x (the Z-Mod hook) — and
    # neither dispatch knows the forge-x payload layout exists.
    if [ -n "${HOST_PLATFORM_HOOK_KEY:-}" ]; then
        platform_hook="$HOST_PLATFORM_HOOK_KEY"
    fi

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
# Actual Raspberry Pi owners keep the original ordering.
#
# All other platforms (k1, k2, ad5m, snapmaker-u1, x86, …) get the single
# "Detected platform: X" line — there's no device-name ambiguity to clear up.
print_platform_banner() {
    local platform="$1"
    local _hw_label

    if [ "$platform" != "pi" ] && [ "$platform" != "pi32" ]; then
        log_info "Detected platform: ${BOLD}${platform}${NC}"
        return 0
    fi

    _hw_label=$(describe_hardware)
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

# Refuse to run --uninstall from a script sitting inside the dir we're about to
# delete. The release tarball ships scripts/install.sh into $INSTALL_DIR for
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

    printf '\n'
    printf '%b\n' "${BOLD}========================================${NC}"
    printf '%b\n' "${BOLD}       HelixScreen Installer${NC}"
    printf '%b\n' "${BOLD}========================================${NC}"
    printf '\n'

    # Detect platform
    platform=$(detect_platform)
    # For platforms that share a binary with pi/pi32 (e.g. m1), the download
    # URL uses the donor platform key while $platform stays put so hooks,
    # banner, and competing-UI shutdown still target the real device.
    download_platform=$(get_download_platform "$platform")
    print_platform_banner "$platform"

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
        log_error "  - Creality K1 series with Simple AF"
        log_error "  - Creality K2 series (K2/K2 Pro/K2 Plus)"
        log_error "  - x86_64 Debian/Ubuntu (x86_64)"
        exit 1
    fi

    # For AD5M/AD5X/K1, detect the firmware/mod flavor and set appropriate paths.
    # The FlashForge mods ship for both Adventurer platforms, so ad5x runs the
    # same detector ad5m always has — a Forge-X AD5X is no longer misread as
    # the ZMOD layout the ad5x paths assumed. AD5M_FIRMWARE is the compat
    # alias for consumers predating that.
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
    # path, which is the failure mode that caused user reports of
    # "--uninstall reinstalled HelixScreen".
    if [ "$uninstall_mode" = true ]; then
        log_error "internal error: install path entered with uninstall_mode=true"
        log_error "please report at https://github.com/prestonbrown/helixscreen/issues"
        exit 99
    fi

    # Pre-flight checks
    log_info "Running pre-flight checks..."
    check_requirements
    install_runtime_deps "$platform"
    check_disk_space "$platform"
    detect_init_system
    check_klipper_ecosystem "$platform"

    # Get version (skip if using local archive)
    if [ -n "$local_tarball" ]; then
        # Validate local file exists
        if [ ! -f "$local_tarball" ]; then
            log_error "Local archive not found: $local_tarball"
            exit 1
        fi
        # Extract version from filename if possible. Only the tar.gz layout
        # carries a version in the name (helixscreen-<plat>-v1.2.3.tar.gz).
        # The unversioned helixscreen-<plat>.zip gets "local" as a placeholder.
        version=$(echo "$local_tarball" | sed -n 's/.*helixscreen-[^-]*-\(v[0-9.]*\)\.tar\.gz/\1/p')
        if [ -z "$version" ]; then
            version="local"
        fi
        log_info "Installing from local file: ${BOLD}${local_tarball}${NC}"
    else
        if [ -z "$version" ]; then
            version=$(get_latest_version "$download_platform")
        fi
    fi
    log_info "Target version: ${BOLD}${version}${NC}"

    # Configure platform-specific settings before stopping UIs
    configure_platform

    # Stop competing UIs
    stop_competing_uis

    # Clean old installation if requested
    if [ "$clean_mode" = true ]; then
        clean_old_installation "$platform"
    fi

    # Download/stage the release archive BEFORE stopping the service.
    # Stopping helixscreen first can disrupt the network on some platforms
    # (e.g. Snapmaker U1 where platform_post_stop restarts the stock GUI which
    # owns wpa_supplicant and drops WiFi/SSH mid-update). Staging first also
    # means a failed download leaves the running service untouched.
    if [ -n "$local_tarball" ]; then
        use_local_tarball "$local_tarball"
    else
        download_release "$version" "$download_platform"
    fi

    if [ "$update_mode" = true ]; then
        if [ ! -d "$INSTALL_DIR" ]; then
            log_warn "No existing installation found. Performing fresh install."
        fi
        stop_service "$platform"
    fi

    extract_release "$platform"
    fix_install_ownership
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

    # Install KIAUH extension if KIAUH is detected
    install_kiauh_extension "$skip_kiauh_registration" || true

    # K1: ensure SSH (dropbear) is running — recovers from #535 where disabling
    # S99start_app also killed SSH. Runs on both fresh install and self-update.
    if [ "$platform" = "k1" ]; then
        ensure_k1_ssh
    fi

    # Verify all shared library dependencies are satisfied before starting
    verify_binary_deps "$platform"

    # Create platform cache directory
    case "$platform" in
        ad5m)
            $SUDO mkdir -p /data/helixscreen/cache
            ;;
        k1)
            $SUDO mkdir -p /usr/data/helixscreen/cache
            ;;
    esac

    # Symlink config into printer_data (Pi/Klipper only - enables web UI editing)
    setup_config_symlink

    # Configure Moonraker update_manager (Pi only - enables web UI updates)
    configure_moonraker_updates "$platform"

    # K2: replace the stock proprietary WebRTC camera (which HelixScreen and
    # fluidd can't consume) with a static ustreamer MJPEG server and point both
    # UIs at it. No-op on every other platform. Runs after the binary is
    # extracted and Moonraker is reachable; safe (detect-first) and reversible
    # (stock WebRTC disable is recorded for re-enable on uninstall).
    install_camera_k2 "$platform" || true

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
    local seed_pid
    seed_pid=$(detect_printer_model)
    if [ -n "$seed_pid" ]; then
        log_info "Recognized printer model: ${seed_pid} -- applying install-time defaults"
        seed_settings_for_printer "$seed_pid" || true
        install_klipper_include_for_printer "$seed_pid" || true
    else
        seed_from_moonraker_detection || true
    fi

    # Configure ALSA "default" when the board has no card 0 (e.g. Pi + HDMI-audio
    # screens like the BTT HDMI5, whose only outputs are vc4hdmi0/vc4hdmi1 at
    # indices 1/2). Safe no-op when "default" already works or /etc/asound.conf
    # already exists.
    configure_alsa_default || true

    # Start service
    start_service "$platform"
    cleanup_old_install
    cleanup_stale_cache_dirs
    retire_legacy_config_backups

    # Cleanup on success
    cleanup_on_success

    printf '\n'
    printf '%b\n' "${GREEN}${BOLD}========================================${NC}"
    printf '%b\n' "${GREEN}${BOLD}    Installation Complete!${NC}"
    printf '%b\n' "${GREEN}${BOLD}========================================${NC}"
    printf '\n'
    echo "HelixScreen ${version} installed to ${INSTALL_DIR}"
    echo ""
    print_post_install_commands
    echo ""

    if [ "$platform" = "ad5m" ] || [ "$platform" = "k1" ] || [ "$platform" = "k2" ]; then
        echo "Note: You may need to reboot for the display to update."
    fi
}
