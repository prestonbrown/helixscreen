#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: image
# Install into a disk image's root filesystem at build time (--target-root),
# and the first-boot pass (--first-boot) that finishes what only the booted
# printer can do (prestonbrown/helixscreen#1913).
#
# Reads: IMAGE_TARGET_ROOT, IMAGE_PLATFORM_ARG, IMAGE_USER_ARG,
#        IMAGE_OWN_DISPLAY, first_boot_mode, local_tarball, DRY_RUN,
#        INSTALL_SOURCE_ARG, skip_kiauh_registration
# Writes: under the target root only (--target-root); the booted printer's
#         config, rules and Moonraker conf (--first-boot)

# Source guard
[ -n "${_HELIX_IMAGE_SOURCED:-}" ] && return 0
_HELIX_IMAGE_SOURCED=1

# An image install runs on the image builder's machine, which is not the
# printer: it starts nothing, asks no Moonraker and reads nothing of the build
# host's own state. Every path it writes is the target root plus the path the
# booted printer will see, and every path written INTO a file (unit lines,
# Moonraker's path:, symlink targets) is the booted printer's path alone.
#
# What needs the booted printer is listed, one step per line, in this file in
# the install's config/; the first-boot unit runs `install.sh --first-boot`
# while it exists, and that run deletes it once every step has passed.
IMAGE_FIRST_BOOT_MARKER=".first_boot_pending"

# The image's passwd fields for user $1 as "uid:gid:home", from the image's own
# /etc/passwd: the build host's accounts say nothing about the printer's.
_image_passwd_entry() {
    awk -F: -v u="$1" '$1 == u { print $3 ":" $4 ":" $6; exit }' "$IMAGE_ROOT/etc/passwd" 2>/dev/null
}

# The Klipper user the image runs: klipper.service's User=, else the one home
# holding a printer_data. Prints nothing when neither settles it.
_image_detect_user() {
    local _idu_unit _idu_user _idu_pd _idu_found="" _idu_count=0
    for _idu_unit in "$IMAGE_ROOT/etc/systemd/system/klipper.service" \
                     "$IMAGE_ROOT/lib/systemd/system/klipper.service" \
                     "$IMAGE_ROOT/usr/lib/systemd/system/klipper.service"; do
        [ -f "$_idu_unit" ] || continue
        _idu_user=$(sed -n 's/^User=[[:space:]]*//p' "$_idu_unit" | head -n 1)
        if [ -n "$_idu_user" ] && [ "$_idu_user" != root ]; then
            echo "$_idu_user"
            return 0
        fi
    done
    for _idu_pd in "$IMAGE_ROOT"/home/*/printer_data; do
        [ -d "$_idu_pd" ] || continue
        _idu_found=$(basename "$(dirname "$_idu_pd")")
        _idu_count=$((_idu_count + 1))
    done
    [ "$_idu_count" -eq 1 ] && echo "$_idu_found"
    return 0
}

# The KlipperScreen enablement links in the image, one per line: the unit is
# enabled exactly when one exists.
_image_klipperscreen_links() {
    local _ikl
    for _ikl in "$IMAGE_ROOT"/etc/systemd/system/*.wants/KlipperScreen.service; do
        [ -L "$_ikl" ] && echo "$_ikl"
    done
    return 0
}

# Packages from HELIX_PI_RUNTIME_DEPS the image's dpkg database lacks, read
# with --admindir so the build host's own packages never answer. Sets
# IMAGE_DEPS_CHECKED=no when the image has no dpkg database to read.
_image_missing_runtime_deps() {
    local _imd_admin="$IMAGE_ROOT/var/lib/dpkg" _imd_dep _imd_missing=""
    IMAGE_DEPS_CHECKED=yes
    if [ ! -f "$_imd_admin/status" ] || ! command -v dpkg-query >/dev/null 2>&1; then
        IMAGE_DEPS_CHECKED=no
        return 0
    fi
    for _imd_dep in $HELIX_PI_RUNTIME_DEPS; do
        dpkg-query --admindir="$_imd_admin" -W -f='${Status}' "$_imd_dep" 2>/dev/null |
            grep -q "install ok installed" || _imd_missing="${_imd_missing:+$_imd_missing }$_imd_dep"
    done
    echo "$_imd_missing"
}

# Settle everything the image install will do, writing nothing. Exits on
# anything that would leave the image unable to run HelixScreen.
image_resolve() {
    local _ir_entry _ir_missing

    IMAGE_ROOT="${IMAGE_TARGET_ROOT%/}"
    while [ "${IMAGE_ROOT%/}" != "$IMAGE_ROOT" ]; do IMAGE_ROOT="${IMAGE_ROOT%/}"; done
    [ -d "${IMAGE_ROOT:-/}" ] || _image_refuse "Target root not found: $IMAGE_TARGET_ROOT"
    [ -f "$IMAGE_ROOT/etc/passwd" ] ||
        _image_refuse "$IMAGE_TARGET_ROOT has no etc/passwd; name the image's root filesystem"
    [ -d "$IMAGE_ROOT/etc/systemd" ] ||
        _image_refuse "$IMAGE_TARGET_ROOT has no etc/systemd; image installs need a systemd image"
    # shellcheck disable=SC2154  # set by main.sh (parse_installer_args)
    [ -f "$local_tarball" ] || _image_refuse "Local archive not found: $local_tarball"
    if [ -n "${_USER_INSTALL_DIR:-}" ]; then
        log_warn "Ignoring INSTALL_DIR=${_USER_INSTALL_DIR}: an image install goes in the Klipper user's home"
    fi

    IMAGE_USER="${IMAGE_USER_ARG:-$(_image_detect_user)}"
    [ -n "$IMAGE_USER" ] ||
        _image_refuse "Could not tell which user runs Klipper in this image; name it with --user NAME"
    _ir_entry=$(_image_passwd_entry "$IMAGE_USER")
    [ -n "$_ir_entry" ] ||
        _image_refuse "User $IMAGE_USER is not in the image's /etc/passwd; create it before installing"
    IMAGE_UID="${_ir_entry%%:*}"
    _ir_entry="${_ir_entry#*:}"
    IMAGE_GID="${_ir_entry%%:*}"
    IMAGE_HOME="${_ir_entry#*:}"
    [ "$IMAGE_UID" != 0 ] || _image_refuse "HelixScreen runs as the Klipper user, not root; name that user with --user"
    IMAGE_GROUP=$(awk -F: -v g="$IMAGE_GID" '$3 == g { print $1; exit }' "$IMAGE_ROOT/etc/group" 2>/dev/null)
    IMAGE_GROUP="${IMAGE_GROUP:-$IMAGE_GID}"

    if [ ! -d "$IMAGE_ROOT$IMAGE_HOME/printer_data" ] && [ ! -d "$IMAGE_ROOT$IMAGE_HOME/klipper" ] &&
       [ ! -d "$IMAGE_ROOT$IMAGE_HOME/moonraker" ]; then
        _image_refuse "No Klipper install for $IMAGE_USER in this image ($IMAGE_HOME has no printer_data, klipper or moonraker); install Klipper and Moonraker first"
    fi

    IMAGE_INSTALL_DIR="$IMAGE_HOME/helixscreen"
    if [ -e "$IMAGE_ROOT$IMAGE_INSTALL_DIR" ]; then
        _image_refuse "The image already holds $IMAGE_INSTALL_DIR; an image install starts from an image without HelixScreen"
    fi
    IMAGE_PD_HELIX="$IMAGE_HOME/printer_data/config/helixscreen"

    _ir_missing=$(_image_missing_runtime_deps)
    if [ -n "$_ir_missing" ]; then
        log_error "The image lacks packages HelixScreen needs: $_ir_missing"
        log_error "Install them in the image first, e.g. inside its chroot:"
        log_error "  apt-get install -y --no-install-recommends $_ir_missing"
        exit 1
    fi

    IMAGE_KS_LINKS=$(_image_klipperscreen_links)
    if [ -z "$IMAGE_KS_LINKS" ] || [ "${IMAGE_OWN_DISPLAY:-}" = 1 ]; then
        IMAGE_DISPLAY_OWNER="helixscreen"
    else
        IMAGE_DISPLAY_OWNER="KlipperScreen"
    fi

    IMAGE_MOONRAKER_CONF="$IMAGE_HOME/printer_data/config/moonraker.conf"
    if [ ! -f "$IMAGE_ROOT$IMAGE_MOONRAKER_CONF" ]; then
        IMAGE_MOONRAKER="first-boot"
    elif has_update_manager_section "$IMAGE_ROOT$IMAGE_MOONRAKER_CONF"; then
        IMAGE_MOONRAKER="present"
    elif [ "$(moonraker_asset_name_support_in "$(moonraker_update_manager_dir_under "$IMAGE_ROOT$IMAGE_HOME/moonraker")")" = unsupported ]; then
        IMAGE_MOONRAKER="unsupported"
    else
        IMAGE_MOONRAKER="write"
    fi

    IMAGE_FIRST_BOOT_STEPS="config-links permission-rules alsa remote-screen"
    [ "${skip_kiauh_registration:-false}" = true ] || IMAGE_FIRST_BOOT_STEPS="$IMAGE_FIRST_BOOT_STEPS kiauh"
    [ "$IMAGE_MOONRAKER" = first-boot ] && IMAGE_FIRST_BOOT_STEPS="$IMAGE_FIRST_BOOT_STEPS moonraker"
    return 0
}

_image_plan_line() {
    printf '  %-16s %s\n' "$1" "$2"
}

image_print_plan() {
    local _ipp_mr
    case "$IMAGE_MOONRAKER" in
        write) _ipp_mr="add [update_manager helixscreen] to $IMAGE_MOONRAKER_CONF" ;;
        present) _ipp_mr="$IMAGE_MOONRAKER_CONF already has [update_manager helixscreen]" ;;
        unsupported) _ipp_mr="skipped: the image's Moonraker predates asset_name support" ;;
        *) _ipp_mr="no moonraker.conf in the image yet; added on first boot" ;;
    esac
    printf '%s\n' "HelixScreen image install"
    _image_plan_line "Target root:" "${IMAGE_ROOT:-/}"
    _image_plan_line "Platform:" "$IMAGE_PLATFORM_ARG"
    _image_plan_line "Archive:" "$local_tarball"
    _image_plan_line "User:" "$IMAGE_USER ($IMAGE_UID:$IMAGE_GID, group $IMAGE_GROUP)"
    _image_plan_line "Install:" "$IMAGE_INSTALL_DIR"
    _image_plan_line "Display:" "$IMAGE_DISPLAY_OWNER starts at boot"
    _image_plan_line "Moonraker:" "$_ipp_mr"
    _image_plan_line "First boot:" "$IMAGE_FIRST_BOOT_STEPS"
    if [ "${IMAGE_DEPS_CHECKED:-yes}" = no ]; then
        _image_plan_line "Libraries:" "not checked (no dpkg database in the image); needs $HELIX_PI_RUNTIME_DEPS"
    fi
}

# Enable unit $1 in the image the way `systemctl enable` would: a link in each
# target its [Install] section names, pointing at the booted printer's path.
_image_enable_unit() {
    local _ieu_dir="$IMAGE_ROOT/etc/systemd/system" _ieu_target
    for _ieu_target in $(sed -n 's/^WantedBy=//p' "$_ieu_dir/$1"); do
        mkdir -p "$_ieu_dir/${_ieu_target}.wants"
        ln -sf "/etc/systemd/system/$1" "$_ieu_dir/${_ieu_target}.wants/$1"
    done
}

# The oneshot that runs the first-boot steps before HelixScreen first starts.
# It stays enabled and inert once the marker is gone; a failed run keeps the
# marker, so the next boot tries again.
_image_first_boot_unit() {
    cat <<EOF
# SPDX-License-Identifier: GPL-3.0-or-later
# Finishes an image install on the printer's first boot: the setup steps listed
# in ${IMAGE_INSTALL_DIR}/config/${IMAGE_FIRST_BOOT_MARKER}. Inert once that file is gone.
[Unit]
Description=HelixScreen first-boot setup
ConditionPathExists=${IMAGE_INSTALL_DIR}/config/${IMAGE_FIRST_BOOT_MARKER}
After=local-fs.target network.target moonraker.service
Before=${SERVICE_NAME}.service

[Service]
Type=oneshot
ExecStart=/bin/sh ${IMAGE_INSTALL_DIR}/install.sh --first-boot
TimeoutStartSec=300

[Install]
WantedBy=multi-user.target
EOF
}

image_install_units() {
    local _iiu_dir="$IMAGE_ROOT/etc/systemd/system" _iiu_unit
    mkdir -p "$_iiu_dir"

    [ -f "$INSTALL_DIR/config/${SERVICE_NAME}.service" ] ||
        _image_refuse "The release has no config/${SERVICE_NAME}.service; it may be incomplete"
    cp "$INSTALL_DIR/config/${SERVICE_NAME}.service" "$_iiu_dir/${SERVICE_NAME}.service"
    (
        INSTALL_DIR="$IMAGE_INSTALL_DIR"
        _template_service_unit "$_iiu_dir/${SERVICE_NAME}.service" "$IMAGE_USER" "$IMAGE_GROUP" "$IMAGE_PD_HELIX"
    )
    for _iiu_unit in helixscreen-update.path helixscreen-update.service; do
        [ -f "$INSTALL_DIR/config/$_iiu_unit" ] || continue
        cp "$INSTALL_DIR/config/$_iiu_unit" "$_iiu_dir/$_iiu_unit"
        ( INSTALL_DIR="$IMAGE_INSTALL_DIR"; _template_install_paths "$_iiu_dir/$_iiu_unit" )
    done
    _image_first_boot_unit > "$_iiu_dir/$HELIX_FIRST_BOOT_UNIT"

    _image_enable_unit "$HELIX_FIRST_BOOT_UNIT"
    # The update watcher restarts HelixScreen after a Moonraker update, so it
    # is enabled only where HelixScreen is the screen that starts.
    if [ "$IMAGE_DISPLAY_OWNER" = helixscreen ]; then
        _image_enable_unit "${SERVICE_NAME}.service"
        [ -f "$_iiu_dir/helixscreen-update.path" ] && _image_enable_unit helixscreen-update.path
    fi
    return 0
}

# --own-display: KlipperScreen stays installed but no longer starts, recorded
# in the disabled-services ledger so an uninstall on the printer enables it
# again.
image_take_display() {
    [ "${IMAGE_OWN_DISPLAY:-}" = 1 ] && [ -n "$IMAGE_KS_LINKS" ] || return 0
    local _itd_link
    for _itd_link in $IMAGE_KS_LINKS; do
        rm -f "$_itd_link"
    done
    record_disabled_service systemd KlipperScreen
}

image_configure_moonraker() {
    local _icm_conf="$IMAGE_ROOT$IMAGE_MOONRAKER_CONF"
    case "$IMAGE_MOONRAKER" in
        write)
            ( INSTALL_DIR="$IMAGE_INSTALL_DIR"; add_update_manager_section "$_icm_conf" )
            ensure_moonraker_asvc "$_icm_conf"
            ;;
        unsupported)
            log_warn "The image's Moonraker predates release_info.json asset_name support, so"
            log_warn "its Update button would delete ${IMAGE_INSTALL_DIR}. Skipping the"
            log_warn "[update_manager helixscreen] section; HelixScreen's own updater still works."
            ensure_moonraker_asvc "$_icm_conf"
            ;;
        present)
            ensure_moonraker_asvc "$_icm_conf"
            ;;
    esac
    return 0
}

# The image's numeric ids, so the tree belongs to the printer's user whatever
# that uid means on the build host.
image_fix_ownership() {
    chown -Rh "${IMAGE_UID}:${IMAGE_GID}" "$INSTALL_DIR" 2>/dev/null ||
        log_warn "Could not hand $IMAGE_INSTALL_DIR to $IMAGE_USER; the service fixes ownership when it first starts"
    # The copy add_update_manager_section keeps of moonraker.conf sits in the
    # user's printer_data beside it.
    local _ifo_bak="$IMAGE_ROOT${IMAGE_MOONRAKER_CONF}.bak.helixscreen"
    [ ! -f "$_ifo_bak" ] || chown "${IMAGE_UID}:${IMAGE_GID}" "$_ifo_bak" 2>/dev/null ||
        log_warn "Could not hand ${IMAGE_MOONRAKER_CONF}.bak.helixscreen to $IMAGE_USER"
    return 0
}

# The installed copy of install.sh runs the first-boot pass, so the release
# must ship one that knows it. On refusal the tree this run created goes again.
_image_require_first_boot_support() {
    grep -q -- '--first-boot' "$INSTALL_DIR/install.sh" 2>/dev/null && return 0
    case "$INSTALL_DIR" in
        */helixscreen) rm -rf "$INSTALL_DIR" ;;
    esac
    _image_refuse "This release's install.sh cannot finish an image install on first boot; use a newer release"
}

image_apply() {
    local _ia_step
    # shellcheck disable=SC2034  # read by the shared helpers this calls
    SUDO=""
    platform="$IMAGE_PLATFORM_ARG"
    # shellcheck disable=SC2034  # consumed by moonraker.sh (helix_self_update_asset)
    PLATFORM="$platform"
    # The image's paths, seen from here: every shared helper below writes
    # through these.
    INSTALL_DIR="$IMAGE_ROOT$IMAGE_INSTALL_DIR"
    # shellcheck disable=SC2034  # consumed by common.sh (klipper_config_dir, config_backup_tiers)
    KLIPPER_HOME="$IMAGE_ROOT$IMAGE_HOME"
    # shellcheck disable=SC2034  # consumed by common.sh (klipper_config_dir)
    KLIPPER_CONFIG_DIR=""
    # shellcheck disable=SC2034  # consumed by common.sh (config_backup_tiers)
    HELIX_STATE_VAR_LIB="$IMAGE_ROOT/var/lib/helixscreen"
    # shellcheck disable=SC2034  # consumed by common.sh (config_backup_tiers)
    HELIX_STATE_ROOT_HOME="$IMAGE_ROOT/root/.helixscreen"
    TMP_DIR="${TMPDIR:-/tmp}/helixscreen-install-image.$$"
    validate_tmp_dir "$TMP_DIR" || exit 1
    # No channel is known for an image, so its marker says other unless the
    # builder names one.
    INSTALL_SOURCE_ARG="${INSTALL_SOURCE_ARG:-other}"

    match_channel_to_version "$(parse_tarball_version "$local_tarball")"
    use_local_tarball "$local_tarball"
    mkdir -p "$(dirname "$INSTALL_DIR")"
    extract_release "$platform"
    cd / 2>/dev/null || true
    _image_require_first_boot_support

    deploy_platform_hooks "$INSTALL_DIR" pi
    image_install_units
    image_take_display
    image_configure_moonraker
    seed_update_channel
    record_install_source

    : > "$INSTALL_DIR/config/$IMAGE_FIRST_BOOT_MARKER"
    for _ia_step in $IMAGE_FIRST_BOOT_STEPS; do
        echo "$_ia_step" >> "$INSTALL_DIR/config/$IMAGE_FIRST_BOOT_MARKER"
    done

    image_fix_ownership
    cleanup_on_success
}

image_print_summary() {
    printf '\n%s\n' "Installed HelixScreen into ${IMAGE_ROOT:-/} at $IMAGE_INSTALL_DIR."
    printf '%s\n' "First boot finishes: $IMAGE_FIRST_BOOT_STEPS"
    if [ "$IMAGE_DISPLAY_OWNER" = helixscreen ]; then
        printf '%s\n' "HelixScreen starts at boot."
    else
        printf '%s\n' "KlipperScreen owns the display and starts at boot; HelixScreen is installed but disabled."
        printf '%s\n' "To switch on the printer:"
        printf '%s\n' "  sudo systemctl disable --now KlipperScreen"
        printf '%s\n' "  sudo systemctl enable --now ${SERVICE_NAME} helixscreen-update.path"
        printf '%s\n' "Or rebuild the image with --own-display."
    fi
}

# The --target-root mode, from parse to summary. A separate mode, like
# uninstall: it never reaches main()'s confirm point. --dry-run stops it after
# the plan, before anything is written.
image_install() {
    image_resolve
    image_print_plan
    if [ "${DRY_RUN:-false}" = true ]; then
        printf '%s\n' "Dry run, nothing changed."
        exit 0
    fi
    image_apply
    image_print_summary
}

# How long first boot waits for Moonraker to answer before the steps that talk
# to it are left for the next boot. It starts alongside this unit, and on a
# Pi with several MCUs it can take a minute or more to listen.
IMAGE_FIRST_BOOT_MOONRAKER_SECS="${HELIX_FIRST_BOOT_MOONRAKER_SECS:-120}"

# True once Moonraker answers /server/info at the URL this install would use,
# probing once a second for IMAGE_FIRST_BOOT_MOONRAKER_SECS. The answer is kept
# for the rest of the run.
_image_moonraker_up() {
    case "${_IMAGE_MOONRAKER_UP:-}" in
        yes) return 0 ;;
        no) return 1 ;;
    esac
    local _imu_url _imu_end
    _imu_url=$(installer_moonraker_url)
    _imu_end=$(($(helix_now) + IMAGE_FIRST_BOOT_MOONRAKER_SECS))
    while :; do
        moonraker_request GET "$_imu_url/server/info" 2 || :
        if [ -n "$HTTP_STATUS" ]; then
            _IMAGE_MOONRAKER_UP=yes
            return 0
        fi
        if [ "$(helix_now)" -ge "$_imu_end" ]; then
            log_warn "Moonraker did not answer within ${IMAGE_FIRST_BOOT_MOONRAKER_SECS} seconds; its steps wait for the next boot"
            _IMAGE_MOONRAKER_UP=no
            return 1
        fi
        sleep 1
    done
}

# Run first-boot step $1; true only when it is finished. A step that talks to
# Moonraker counts as finished only when Moonraker answered, since without it
# install_remote_screen_route settles on the LAN and returns success, and a
# stanza the moonraker step adds stays unread until Moonraker restarts.
_image_first_boot_step() {
    case "$1" in
        config-links) setup_config_symlink && pin_env_file ;;
        permission-rules) install_permission_rules "$platform" ;;
        alsa) configure_alsa_default ;;
        remote-screen) _image_moonraker_up && install_remote_screen_route ;;
        kiauh) install_kiauh_extension false ;;
        moonraker)
            _image_moonraker_up || return 1
            MOONRAKER_RESTART_RC=""
            configure_moonraker_updates "$platform" || return 1
            [ "${MOONRAKER_RESTART_RC:-$HELIX_RESTART_DONE}" = "$HELIX_RESTART_DONE" ]
            ;;
        *) log_warn "Skipping unknown first-boot step: $1" ;;
    esac
}

# --first-boot, on the booted printer: run each step the marker lists, then
# write back only the ones that did not finish, deleting the marker when none
# is left. Paths come from main()'s own detection, which finds the install the
# image carries. A step left over makes the run fail, so the unit shows it, and
# the next boot tries that step again.
image_first_boot() {
    local platform="$1" _ifb_marker _ifb_steps _ifb_step _ifb_left=""
    _ifb_marker="$INSTALL_DIR/config/$IMAGE_FIRST_BOOT_MARKER"
    if [ ! -f "$_ifb_marker" ]; then
        log_info "No first-boot setup is pending at $INSTALL_DIR"
        return 0
    fi
    detect_init_system
    # Read whole first: a step reading stdin must not eat the list.
    _ifb_steps=$(cat "$_ifb_marker")
    for _ifb_step in $_ifb_steps; do
        if ! _image_first_boot_step "$_ifb_step" </dev/null; then
            log_warn "First-boot step not finished: $_ifb_step; the next boot tries again"
            _ifb_left="${_ifb_left}${_ifb_step}
"
        fi
    done
    # The steps above record their uninstall ledgers in the install dir; move
    # them into printer_data, where a Moonraker update cannot delete them.
    setup_config_symlink quiet
    fix_user_config_ownership
    if [ -n "$_ifb_left" ]; then
        printf '%s' "$_ifb_left" > "$_ifb_marker"
        log_error "First-boot setup is not finished: $(printf '%s' "$_ifb_left" | tr '\n' ' ')"
        exit 1
    fi
    rm -f "$_ifb_marker"
    log_success "First-boot setup finished"
}
