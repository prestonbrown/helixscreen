#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: service
# Service installation and management (systemd and SysV)
#
# Reads: INIT_SYSTEM, INSTALL_DIR, INIT_SCRIPT_DEST, SERVICE_NAME, SUDO

# Source guard
[ -n "${_HELIX_SERVICE_SOURCED:-}" ] && return 0
_HELIX_SERVICE_SOURCED=1

# SERVICE_NAME is defined in common.sh

# Portable in-place sed: GNU sed uses -i, BSD/macOS sed requires -i ''
_sed_inplace() {
    local pattern=$1 file=$2
    $SUDO sed -i "$pattern" "$file" 2>/dev/null || \
    $SUDO sed -i '' "$pattern" "$file" 2>/dev/null || true
}

# Fill the install-path placeholders (@@INSTALL_DIR@@, @@INSTALL_PARENT@@) in
# installed unit files. config/refresh-service-units.sh does the same on the
# device, where this library is not shipped.
# Args: UNIT_FILE...   Reads: INSTALL_DIR, SUDO
_template_install_paths() {
    local install_dir="${INSTALL_DIR:-/opt/helixscreen}" install_parent f
    install_parent="$(dirname "$install_dir")"
    for f in "$@"; do
        _sed_inplace "s|@@INSTALL_DIR@@|${install_dir}|g; s|@@INSTALL_PARENT@@|${install_parent}|g" "$f"
    done
}

# Returns true if this process is running under the NoNewPrivileges systemd constraint.
# When helix-screen self-updates, it spawns install.sh as a child process.  The
# helixscreen.service unit has NoNewPrivileges=true, so ALL sudo calls in install.sh
# will fail.  Callers use this to skip operations that require root (service file
# copy, daemon-reload, systemctl start) and instead let update_checker.cpp restart
# the process via exit(0), which the watchdog treats as "restart silently".
_has_no_new_privs() {
    # Primary: check /proc/self/status (kernel 4.10+)
    if [ -r /proc/self/status ] && grep -q '^NoNewPrivs:[[:space:]]*1' /proc/self/status 2>/dev/null; then
        return 0
    fi
    # Fallback for older kernels (e.g. SonicPad 4.9): if we know this is a
    # self-update spawned from the running app, the service unit's
    # NoNewPrivileges=true is inherited — sudo will fail.
    _is_self_update
}

# _is_self_update() is defined in common.sh (sourced before this module)

# Fix a known-broken PLATFORM_HOOKS path in an already-deployed init script.
#
# Init scripts shipped before 2026-04-20 sourced platform hooks from
# ${DAEMON_DIR}/assets/config/platform/hooks.sh, but the installer (and the
# deploy makefile) write hooks to ${DAEMON_DIR}/platform/hooks.sh.  Result:
# the file was never found, platform_stop_competing_uis() stayed a no-op,
# and stock UIs (Creality K2 /etc/init.d/app, etc.) ran alongside HelixScreen.
#
# Self-update deliberately skips copying the init script to preserve
# platform customizations (#314), so a pure source-tree fix can't reach
# already-installed users.  This surgical sed rewrites only that one
# known-broken substring; anything else the platform may have customized
# is left alone.
_migrate_init_script_hooks_path() {
    local init_script="${INIT_SCRIPT_DEST:-}"
    [ -n "$init_script" ] && [ -f "$init_script" ] || return 0
    if grep -q 'assets/config/platform/hooks\.sh' "$init_script" 2>/dev/null; then
        log_info "Migrating stale PLATFORM_HOOKS path in $init_script"
        _sed_inplace 's|assets/config/platform/hooks\.sh|platform/hooks.sh|' "$init_script"
    fi
}

# Install service (dispatcher)
# Calls install_service_systemd or install_service_sysv based on INIT_SYSTEM
install_service() {
    local platform=$1

    if [ "$platform" = "snapmaker-u1" ]; then
        install_service_snapmaker_u1
        return
    fi

    if [ "$INIT_SYSTEM" = "systemd" ]; then
        install_service_systemd
    else
        install_service_sysv
        # K2 (procd) needs an extra shim — see install_procd_shim_k2.
        # Non-fatal on purpose: by this point the stock UI is already
        # stopped and nothing of ours has started, so an abort would leave
        # the device with no UI at all — strictly worse than the incomplete
        # boot entry the function just detected and logged. The session UI
        # still comes up via start_service.
        if [ "$platform" = "k2" ]; then
            install_procd_shim_k2 || {
                log_error "K2 procd shim not verified — the UI will not autostart at boot"
                log_error "Manual fix: $SUDO /etc/init.d/helixscreen enable"
            }
        fi
    fi
}

# True when $1 is an rc.common script: its first line names /etc/rc.common,
# the shebang rc.common dispatches and a plain SysV script does not carry.
# read and case are shell builtins, so the check needs no external applet.
# A missing, unreadable or empty file answers non-zero (an empty first
# line is not a shebang).
is_rc_common_script() {
    local first
    IFS= read -r first 2>/dev/null < "$1" || return 1
    case "$first" in
        */etc/rc.common*) return 0 ;;
        *) return 1 ;;
    esac
}

# Verify one rc.d boot link points where rc.common's `enable` puts it. A
# mismatch — including a missing link — is a failed boot entry; the caller
# has already run `enable`, so the manual fix is to run it again by hand.
_verify_rcd_link() {
    local link="$1" expected="$2" script="$3"
    local actual
    actual="$($SUDO readlink "$link" 2>/dev/null || true)"
    if [ "$actual" != "$expected" ]; then
        log_error "$link -> '$actual' (expected '$expected')"
        log_error "The rc.d boot entry is incomplete; Manual fix: $SUDO $script enable"
        return 1
    fi
    return 0
}

# The START=/STOP= slot a script declares, as bare digits; prints it and
# returns 0, or returns 1 when nothing parseable remains. rc.common reads
# these directives by sourcing the script, where comments and quotes never
# survive — this reads without executing anything, so it strips what
# sourcing would have discarded: a trailing comment, surrounding quotes,
# CR from a CRLF edit, stray spaces. A duplicated directive keeps its
# newline and fails the digit check rather than silently picking a winner.
_rcd_slot() {
    local directive="$1" script="$2" value
    value="$(sed -n "s/^${directive}=//p" "$script" 2>/dev/null | tr -d '\r \t')"
    value="${value%%#*}"
    value="${value#\"}" ; value="${value%\"}"
    value="${value#\'}" ; value="${value%\'}"
    case "$value" in
        ''|*[!0-9]*) return 1 ;;
        *) printf '%s\n' "$value" ;;
    esac
}

# Enable an rc.common init script and verify the rc.d boot links it made.
#
# rc.common's `enable` writes relative links /etc/rc.d/S${START}<name> and
# K${STOP}<name> (START/STOP read from the script itself) and exits 0 when
# at least ONE link was made — so a partial enable (the S link without its
# K link, say) reads as success and leaves the boot entry silently
# incomplete. Drop the script's existing S??/K?? links first (an older
# install may have used a different two-digit slot — the same space
# rc.common's own disable globs), run `enable`, then verify each link by
# readlink against ../init.d/<name>.
#
# START is required: it is the boot slot, and without it rc.common makes
# no S link — there is no boot entry to verify. STOP is verified when
# declared: rc.common makes no K link without one, and this helper also
# runs against stock firmware scripts we do not author, so an absent or
# unparseable STOP downgrades to a warning and an S-only verification
# instead of failing the enable.
enable_and_verify_rcd() {
    local script="$1"
    local name start stop

    name="$(basename "$script")"

    # A missing or unreadable script must not be diagnosed as a directive
    # problem: the operator would be sent to edit a file that does not
    # exist while the real fault is the copy step or permissions.
    if [ ! -f "$script" ]; then
        log_error "$script: not found at verification time"
        log_error "The copy step failed; Manual fix: check the install logs above"
        return 1
    fi
    if [ ! -r "$script" ]; then
        log_error "$script: not readable — cannot verify its rc.d boot entry"
        return 1
    fi

    if ! start="$(_rcd_slot START "$script")"; then
        log_error "unparseable START= in $script (expected a two-digit slot)"
        log_error "Manual fix: set START=<nn> to a plain number in $script"
        return 1
    fi
    if ! stop="$(_rcd_slot STOP "$script")"; then
        log_warn "$name: no parseable STOP= in $script — the shutdown half of the boot entry is left unverified"
        stop=""
    fi

    # The glob must expand as root: an unprivileged shell cannot read
    # /etc/rc.d, the pattern would reach rm literally, and rm -f on the
    # literal name removes nothing without a sound.
    $SUDO sh -c "rm -f /etc/rc.d/S??\"$name\" /etc/rc.d/K??\"$name\"" \
        || log_warn "$name: could not drop a stale rc.d link; a duplicate boot entry may remain"

    if ! $SUDO "$script" enable; then
        log_error "$name: enable failed — the rc.d boot entry was not installed"
        log_error "Manual fix: $SUDO $script enable"
        return 1
    fi

    _verify_rcd_link "/etc/rc.d/S${start}${name}" "../init.d/$name" "$script" || return 1
    if [ -n "$stop" ]; then
        _verify_rcd_link "/etc/rc.d/K${stop}${name}" "../init.d/$name" "$script" || return 1
    fi

    log_info "$name: rc.d boot links verified (S${start}${name}${stop:+ K${stop}${name}})"
    return 0
}

# K2 (Tina Linux / procd) boot-dispatches init scripts carrying the
# `#!/bin/sh /etc/rc.common` shebang; a DEPEND directive is optional for
# boot dispatch. Our shared SysV-style S99helixscreen has no such shebang,
# so on K2 install a tiny procd-compatible /etc/init.d/helixscreen shim
# that delegates every action to the real SysV script, and repoint the
# rc.d entry at it: a link straight at the SysV script names a file rc.common
# will not dispatch.
install_procd_shim_k2() {
    local shim_src="${INSTALL_DIR}/config/helixscreen-k2-procd-shim.sh"
    local shim_dest="/etc/init.d/helixscreen"

    if [ ! -x /etc/rc.common ]; then
        log_warn "K2 procd shim: /etc/rc.common not found — skipping (boot autostart will not work)"
        return 0
    fi

    if [ ! -f "$shim_src" ]; then
        log_error "K2 procd shim source missing: $shim_src"
        log_error "The release package may be incomplete."
        log_error "Recovery: re-run the installer to download a fresh copy:"
        log_error "  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
        return 1
    fi

    log_info "Installing K2 procd boot shim..."
    $SUDO cp "$shim_src" "$shim_dest"
    $SUDO chmod +x "$shim_dest"

    # Drop any rc.d entry an older install left, then verify the fresh pair.
    enable_and_verify_rcd "$shim_dest" || return 1

    log_success "Installed K2 procd shim at $shim_dest (boot symlink verified)"
}

# K2 web-server carve-out (prestonbrown/helixscreen#1617). The runtime hook
# runs `/etc/init.d/app stop` + `disable`: stop kills any running
# web-server (killall -9 in the stock app's stop_service, ours included,
# even with the app already stopped) while disable only removes the app's
# rc.d links. The hook therefore restores the carve-out at the end of every
# HelixScreen start, through the rc.common procd script this installs at
# /etc/init.d/helix-k2-webserver — the service-shaped starter and
# supervisor whose registered instance procd respawns. Its rc.d boot entry
# is belt-and-braces next to the hook's restore.
#
# INSTALL half: runs BEFORE start_service, so the hook's first
# platform_stop_competing_uis finds the script and the procd instance is
# registered from the first launch (the hook's bare-launch fallback then
# stays unreachable for shipped payloads; it exists for degraded installs
# that never got this script). No-op when the stock app service is absent
# (a firmware without the stock set has nothing to carve out of) or when
# procd's rc.common is missing. The START half is
# start_k2_webserver_backend, which runs after start_service.
install_k2_webserver_backend() {
    [ "${1:-}" = "k2" ] || return 0

    if [ ! -f /etc/init.d/app ]; then
        log_info "No stock /etc/init.d/app on this host; skipping web-server carve-out"
        return 0
    fi

    if [ ! -x /etc/rc.common ]; then
        log_warn "K2 web-server carve-out: /etc/rc.common not found — skipping"
        return 0
    fi

    local src="${INSTALL_DIR}/config/k2-webserver.init"
    local dest="/etc/init.d/helix-k2-webserver"

    if [ ! -f "$src" ]; then
        log_warn "k2-webserver.init missing from ${INSTALL_DIR}/config (the payload being installed may predate prestonbrown/helixscreen#1617); the web-server carve-out will not survive reboot"
        return 0
    fi

    # A single sudo cp, like the shim's copy: /etc/init.d is root-owned
    # and the carve-out has no non-root install path that could write it.
    # The guard is load-bearing: main.sh calls this function as
    # `... || log_warn`, and under set -e an || context does not abort —
    # an unguarded cp failure would fall through to the ledger write and
    # the enable below. chmod stays non-fatal: without +x the rc.common
    # shebang cannot execute, and `enable` below surfaces that loudly.
    $SUDO cp "$src" "$dest" || {
        log_warn "Could not install $dest; the web-server carve-out will not survive reboot"
        return 0
    }
    $SUDO chmod +x "$dest" || log_warn "chmod failed on $dest"

    # Record the script for uninstall BEFORE any step below can fail: a
    # carve-out left on disk without its ledger entry survives uninstall as
    # an orphan competing with the restored stock UI.
    record_disabled_service "sysv-created" "$dest"

    # Drop any rc.d entry an older install left, then verify the fresh pair.
    enable_and_verify_rcd "$dest" || return 1

    log_info "Installed K2 web-server carve-out: $dest (boot symlink verified)"
    return 0
}

# START half of the carve-out install: bring web-server up for the current
# session, AFTER start_service. The service start's hook already ran
# platform_stop_competing_uis — its app stop took whatever web-server was
# live down, and its own restore brings the carve-out back — so this
# explicit start is belt-and-braces for paths that bypass the hook (a
# degraded install whose hook was never copied, say). A failed start is
# logged, not fatal: procd respawns a registered instance, and the verified
# boot entry brings it up at the next boot. No-op when the carve-out script
# is not installed.
start_k2_webserver_backend() {
    [ "${1:-}" = "k2" ] || return 0

    if [ ! -x /etc/init.d/helix-k2-webserver ]; then
        return 0
    fi

    if ! $SUDO /etc/init.d/helix-k2-webserver start 2>/dev/null; then
        log_warn "K2 web-server carve-out: start failed; procd respawn or the next boot brings it up"
    fi
    return 0
}

install_service_snapmaker_u1() {
    log_info "Configuring Snapmaker U1 autostart..."

    # --- Fix DAEMON_DIR in the init script (U1 keeps it in-place, not /etc/init.d/) ---
    local init_script="${INSTALL_DIR}/config/helixscreen.init"
    if [ ! -f "$init_script" ]; then
        log_error "Init script not found: $init_script"
        exit 1
    fi
    chmod +x "$init_script"
    _sed_inplace "s|DAEMON_DIR=.*|DAEMON_DIR=\"${INSTALL_DIR}\"|" "$init_script"

    # --- Patch S99screen to delegate to helixscreen.init on boot ---
    if [ -f "${INSTALL_DIR}/scripts/snapmaker-u1-setup-autostart.sh" ]; then
        if ! bash "${INSTALL_DIR}/scripts/snapmaker-u1-setup-autostart.sh" "${INSTALL_DIR}"; then
            log_error "Snapmaker U1 autostart configuration failed."
            log_error "Ensure /oem and /etc/init.d are writable."
            exit 1
        fi
    else
        log_error "Snapmaker U1 autostart script not found at ${INSTALL_DIR}/scripts/snapmaker-u1-setup-autostart.sh"
        log_error "The release package may be incomplete."
        log_error "Recovery: re-run the installer to download a fresh copy:"
        log_error "  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
        exit 1
    fi
}

# Install systemd service
install_service_systemd() {
    # During self-update, the main service file is already installed and may
    # contain platform customizations (#314) — don't overwrite it.
    # BUT: always update the update-watcher units (path + oneshot service).
    # They have no platform customizations and older versions had a PathExists
    # bug that causes an infinite restart loop after any update.
    if _is_self_update; then
        log_info "Skipping main service file install (self-update; preserving customizations)"
        update_watcher_if_stale
        return 0
    fi

    log_info "Installing systemd service..."

    local service_src="${INSTALL_DIR}/config/helixscreen.service"
    local service_dest="/etc/systemd/system/${SERVICE_NAME}.service"

    if [ ! -f "$service_src" ]; then
        log_error "Service file not found: $service_src"
        log_error "The release package may be incomplete."
        log_error "Recovery: re-run the installer to download a fresh copy:"
        log_error "  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
        exit 1
    fi

    # Under NoNewPrivileges (self-update spawned by helix-screen), sudo is blocked and
    # /etc/systemd/system/ is read-only in the service's mount namespace.  The service
    # is already installed and correct — skip reinstall.  The process restart is handled
    # by update_checker.cpp calling exit(0) after install.sh succeeds.
    if _has_no_new_privs; then
        if [ -f "$service_dest" ]; then
            log_info "Skipping service reinstall (NoNewPrivileges; already installed)"
            return 0
        fi
        log_error "Service not installed and NoNewPrivileges prevents installation"
        exit 1
    fi

    $SUDO cp "$service_src" "$service_dest"

    # Template placeholders (match SysV pattern in install_service_sysv).
    # KLIPPER_GROUP is the user's actual primary group (resolved via id -gn);
    # falling back to KLIPPER_USER would re-introduce the QIDI Q2 bug where
    # `mks` user has no `mks` group and systemd refuses to spawn the unit.
    local helix_user="${KLIPPER_USER:-root}"
    local helix_group="${KLIPPER_GROUP:-${KLIPPER_USER:-root}}"

    _sed_inplace "s|@@HELIX_USER@@|${helix_user}|g" "$service_dest"
    _sed_inplace "s|@@HELIX_GROUP@@|${helix_group}|g" "$service_dest"
    _template_install_paths "$service_dest"

    if ! $SUDO systemctl daemon-reload; then
        log_error "Failed to reload systemd daemon."
        exit 1
    fi

    # Install update watcher (restarts helixscreen after Moonraker web-type update)
    # Workaround for mainsail-crew/mainsail#2444: type: web lacks managed_services
    install_update_watcher_systemd

    log_success "Installed systemd service"
}

# During self-update, check if the deployed update-watcher path unit has the
# old PathExists directive (which causes infinite restart loops) and fix it.
# The watcher units have no platform customizations, only install-path placeholders.
# Under NoNewPrivileges we can't write to /etc/systemd/system/ or run
# systemctl daemon-reload, so we rely on helixscreen-update.service's
# ExecStart script to refresh the main service file on next Moonraker update.
# However, the PATH unit itself won't get refreshed that way, so we attempt
# a direct fix here and fall back to a warning if permissions block it.
update_watcher_if_stale() {
    local path_dest="/etc/systemd/system/helixscreen-update.path"
    local svc_dest="/etc/systemd/system/helixscreen-update.service"

    # Only relevant on systemd with the watcher installed
    [ "$INIT_SYSTEM" = "systemd" ] || return 0
    [ -f "$path_dest" ] || return 0

    # Detect staleness: PathExists bug (restart loop), missing sentinel check
    # (self-update double-trigger #536), or missing refresh-service-units.sh.
    local stale=false reason=""
    if grep -q '^PathExists=' "$path_dest" 2>/dev/null; then
        stale=true reason="PathExists bug (restart loop)"
    elif [ -f "$svc_dest" ] && ! grep -q 'self_restart_sentinel' "$svc_dest" 2>/dev/null; then
        stale=true reason="missing self-update sentinel check (#536)"
    elif [ -f "$svc_dest" ] && ! grep -q 'refresh-service-units' "$svc_dest" 2>/dev/null; then
        stale=true reason="missing service file refresh"
    fi

    if ! $stale; then
        log_info "Update watcher units are current"
        return 0
    fi

    log_warn "Stale update watcher units: $reason"

    local path_src="${INSTALL_DIR}/config/helixscreen-update.path"
    local svc_src="${INSTALL_DIR}/config/helixscreen-update.service"
    local install_dir="${INSTALL_DIR:-/opt/helixscreen}"

    if _has_no_new_privs; then
        # Can't write to /etc/systemd/system/ under NoNewPrivileges.
        # Attempt in-place fixes — may fail under ProtectSystem=strict.
        local fixed=false
        if grep -q '^PathExists=' "$path_dest" 2>/dev/null; then
            sed -i 's/^PathExists=/PathChanged=/' "$path_dest" 2>/dev/null && fixed=true
        fi
        if [ -f "$svc_src" ] && [ -f "$svc_dest" ]; then
            if cp "$svc_src" "$svc_dest" 2>/dev/null; then
                # sudo is blocked under NoNewPrivileges, so write directly.
                ( SUDO=""; _template_install_paths "$svc_dest" )
                fixed=true
            fi
        fi
        if $fixed; then
            log_success "Updated watcher units (best-effort under NoNewPrivileges)"
        else
            log_warn "Cannot update watcher units under NoNewPrivileges."
            log_warn "Fix: sudo ${install_dir}/config/refresh-service-units.sh"
        fi
        return 0
    fi

    # Have sudo access — do a full replacement
    if [ -f "$path_src" ] && [ -f "$svc_src" ]; then
        $SUDO cp "$path_src" "$path_dest"
        $SUDO cp "$svc_src" "$svc_dest"
        _template_install_paths "$path_dest" "$svc_dest"
        $SUDO systemctl daemon-reload 2>/dev/null || true
        $SUDO systemctl restart helixscreen-update.path 2>/dev/null || true
        log_success "Updated watcher units ($reason)"
    fi
}

# Install systemd path unit that restarts helixscreen after Moonraker extracts an update
install_update_watcher_systemd() {
    local path_src="${INSTALL_DIR}/config/helixscreen-update.path"
    local svc_src="${INSTALL_DIR}/config/helixscreen-update.service"
    local path_dest="/etc/systemd/system/helixscreen-update.path"
    local svc_dest="/etc/systemd/system/helixscreen-update.service"

    if [ ! -f "$path_src" ] || [ ! -f "$svc_src" ]; then
        log_info "Update watcher units not found, skipping"
        return 0
    fi

    $SUDO cp "$path_src" "$path_dest"
    $SUDO cp "$svc_src" "$svc_dest"
    _template_install_paths "$path_dest" "$svc_dest"

    $SUDO systemctl daemon-reload
    $SUDO systemctl enable helixscreen-update.path 2>/dev/null || true
    $SUDO systemctl start helixscreen-update.path 2>/dev/null || true

    log_info "Installed update watcher (helixscreen-update.path)"
}

# Install SysV init script
install_service_sysv() {
    # During self-update, the init script is already installed and correct.
    # Overwriting it destroys platform customizations (ZMOD, Klipper Mod) (#314).
    if _is_self_update; then
        log_info "Skipping init script install (self-update; already installed)"
        _migrate_init_script_hooks_path
        return 0
    fi

    log_info "Installing SysV init script..."

    local init_src="${INSTALL_DIR}/config/helixscreen.init"

    if [ ! -f "$init_src" ]; then
        log_error "Init script not found: $init_src"
        log_error "The release package may be incomplete."
        log_error "Recovery: re-run the installer to download a fresh copy:"
        log_error "  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
        exit 1
    fi

    # Use the dynamically set INIT_SCRIPT_DEST (varies by firmware)
    $SUDO cp "$init_src" "$INIT_SCRIPT_DEST"
    $SUDO chmod +x "$INIT_SCRIPT_DEST"

    # Update the DAEMON_DIR in the init script to match the install location
    # This is important for Klipper Mod which uses a different path
    _sed_inplace "s|DAEMON_DIR=.*|DAEMON_DIR=\"${INSTALL_DIR}\"|" "$INIT_SCRIPT_DEST"

    log_success "Installed SysV init script at $INIT_SCRIPT_DEST"
}

# Start service (dispatcher)
# Calls start_service_systemd or start_service_sysv based on INIT_SYSTEM.
# Snapmaker U1 uses its own init script path (patched S99screen, not S99helixscreen).
start_service() {
    local platform=${1:-}

    if [ "$platform" = "snapmaker-u1" ]; then
        start_service_snapmaker_u1
        return
    fi

    if [ "$INIT_SYSTEM" = "systemd" ]; then
        start_service_systemd
    else
        start_service_sysv
    fi
}

# Start service (Snapmaker U1)
# The U1 patches /etc/init.d/S99screen to delegate to helixscreen.init;
# there is no standalone S99helixscreen init script.
start_service_snapmaker_u1() {
    if _is_self_update; then
        log_info "Skipping service start (self-update; restart via watchdog)"
        return 0
    fi

    log_info "Starting HelixScreen (Snapmaker U1)..."

    local init_src="${INSTALL_DIR}/config/helixscreen.init"
    if [ ! -x "$init_src" ]; then
        chmod +x "$init_src" 2>/dev/null || true
    fi

    if [ ! -x "$init_src" ]; then
        log_error "Init script not found or not executable: $init_src"
        log_error "The release package may be incomplete."
        log_error "Recovery: re-run the installer to download a fresh copy:"
        log_error "  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
        exit 1
    fi

    # A plain "start" is a no-op when an instance is already up, which would
    # leave the OLD (pre-upgrade) binary running while we report success
    # (prestonbrown/helixscreen#1106). Restart so the new binary takes over.
    local action=start
    if pidof helix-screen >/dev/null 2>&1; then
        log_info "HelixScreen is already running -- restarting to load the new version..."
        action=restart
    fi

    if ! $SUDO "$init_src" "$action"; then
        log_error "Failed to start HelixScreen."
        log_error "Check logs: /var/log/helixscreen/launcher.log or ${INSTALL_DIR}/logs/launcher.log"
        exit 1
    fi

    # Wait for service to start (may be slow on embedded hardware)
    local _
    for _ in 1 2 3 4 5; do
        sleep 1
        if pidof helix-screen >/dev/null 2>&1; then
            log_success "HelixScreen is running!"
            return
        fi
    done
    log_warn "Service may still be starting..."
    log_warn "Check logs: /var/log/helixscreen/launcher.log or ${INSTALL_DIR}/logs/launcher.log"
}

# Start service (systemd)
start_service_systemd() {
    log_info "Enabling and starting HelixScreen (systemd)..."

    # Under NoNewPrivileges, systemctl is blocked.  The restart is handled by
    # update_checker.cpp: it calls exit(0) after we return, which the watchdog
    # treats as "normal exit — restart silently".
    if _has_no_new_privs; then
        log_info "Skipping service start (NoNewPrivileges; restart via watchdog)"
        return 0
    fi

    if ! $SUDO systemctl enable "$SERVICE_NAME"; then
        log_error "Failed to enable ${SERVICE_NAME} service."
        exit 1
    fi

    # "systemctl start" is a no-op when the service is already active, which
    # would leave the OLD (pre-upgrade) binary running while we report success
    # (prestonbrown/helixscreen#1106). Restart so the new binary takes over.
    local action=start
    if systemctl is-active --quiet "$SERVICE_NAME"; then
        log_info "HelixScreen is already running -- restarting to load the new version..."
        action=restart
    fi

    if ! $SUDO systemctl "$action" "$SERVICE_NAME"; then
        log_error "Failed to start ${SERVICE_NAME} service."
        log_error "Check logs with: sudo journalctl -u ${SERVICE_NAME} -n 50"
        exit 1
    fi

    # Wait for service to start (may be slow on embedded hardware)
    local _
    for _ in 1 2 3 4 5; do
        sleep 1
        if systemctl is-active --quiet "$SERVICE_NAME"; then
            log_success "HelixScreen is running!"
            return
        fi
    done
    log_warn "Service may still be starting..."
    log_warn "Check status with: systemctl status $SERVICE_NAME"
}

# Start service (SysV init)
start_service_sysv() {
    # During in-app self-update, the watchdog handles the restart via _exit(0).
    # Starting a second instance here would race with the still-running process.
    if _is_self_update; then
        log_info "Skipping service start (self-update; restart via watchdog)"
        return 0
    fi

    log_info "Starting HelixScreen (SysV init)..."

    if [ ! -x "$INIT_SCRIPT_DEST" ]; then
        log_error "Init script not executable: $INIT_SCRIPT_DEST"
        exit 1
    fi

    # A plain "start" is a no-op when an instance is already up, which would
    # leave the OLD (pre-upgrade) binary running while we report success
    # (prestonbrown/helixscreen#1106). Restart so the new binary takes over.
    local action=start
    if pidof helix-screen >/dev/null 2>&1; then
        log_info "HelixScreen is already running -- restarting to load the new version..."
        action=restart
    fi

    if ! $SUDO "$INIT_SCRIPT_DEST" "$action"; then
        log_error "Failed to start HelixScreen."
        log_error "Check logs: /var/log/helixscreen/launcher.log or ${INSTALL_DIR}/logs/launcher.log"
        exit 1
    fi

    # Wait for service to start (may be slow on embedded hardware)
    local _
    for _ in 1 2 3 4 5; do
        sleep 1
        if $SUDO "$INIT_SCRIPT_DEST" status >/dev/null 2>&1; then
            log_success "HelixScreen is running!"
            return
        fi
    done
    log_warn "Service may still be starting..."
    log_warn "Check: $INIT_SCRIPT_DEST status"
}

# Deploy platform-specific hook file
# Copies the correct hook file to $INSTALL_DIR/platform/hooks.sh so the
# init script can source it at runtime.
deploy_platform_hooks() {
    local install_dir="$1"
    local platform="$2"  # "ad5m-forgex", "ad5m-kmod", "pi", "k1"
    # Tarball ships platform hooks under assets/config/ as part of the
    # read-only seed bundle (see scripts/package.sh). Older installers looked
    # in config/platform/ — that path moved in the config/→assets/config/
    # refactor and the deploy_platform_hooks lookup got left behind,
    # silently dropping the hooks file for every sysv platform (#k2-no-hooks).
    local hooks_src="${install_dir}/assets/config/platform/hooks-${platform}.sh"

    if [ ! -f "$hooks_src" ]; then
        log_warn "No platform hooks for: $platform"
        return 0
    fi

    # Try without sudo first: during self-update INSTALL_DIR is pi-owned so no root
    # is needed.  Fall back to sudo for fresh installs where the directory may be
    # root-owned or not yet created.  Under NoNewPrivileges sudo is blocked, so
    # the sudo fallbacks must be silent and non-fatal.
    mkdir -p "${install_dir}/platform" 2>/dev/null || $SUDO mkdir -p "${install_dir}/platform" 2>/dev/null || true
    cp "$hooks_src" "${install_dir}/platform/hooks.sh" 2>/dev/null || $SUDO cp "$hooks_src" "${install_dir}/platform/hooks.sh" 2>/dev/null || true
    chmod +x "${install_dir}/platform/hooks.sh" 2>/dev/null || $SUDO chmod +x "${install_dir}/platform/hooks.sh" 2>/dev/null || true

    # Every copy above is non-fatal, so success has to be observed rather than
    # assumed. A missing hooks.sh leaves each platform_* function the no-op stub
    # the init script declares, and nothing else reports that.
    if [ -s "${install_dir}/platform/hooks.sh" ]; then
        log_info "Deployed platform hooks: $platform"
    else
        log_warn "Platform hooks NOT deployed: ${install_dir}/platform/hooks.sh is missing or empty"
    fi
}

# Fix ownership of install directory for non-root service users.
# The entire INSTALL_DIR must be user-owned so that in-app self-updates
# (which run under NoNewPrivileges=true, blocking sudo) can replace
# files without root access.  ProtectSystem=strict in the service file
# limits filesystem writes to ReadWritePaths regardless of ownership.
# Uses -h to avoid following symlinks outside INSTALL_DIR.
fix_install_ownership() {
    local user="${KLIPPER_USER:-}"
    local group="${KLIPPER_GROUP:-$user}"

    [ -n "$user" ] || return 0
    [ -d "$INSTALL_DIR" ] || return 0

    # Root-run platforms (ad5m/ad5x/k1/k2/cc1/u1) still need normalising, and
    # used to be skipped entirely.  Root's tar extract restores the uid/gid
    # baked into the release archive, so the tree ends up owned by the build
    # machine's numeric ids — a measured K2 had 890 of 915 files owned by uid
    # 1001, which has no /etc/passwd entry there.  The extract now passes -o so
    # fresh installs land as root, and this repairs installs made before that.
    log_info "Setting ownership to ${user}:${group}..."

    # Try without sudo first: during self-update under NoNewPrivileges,
    # sudo is blocked but files are already user-owned so chown succeeds
    # without it (or is a no-op).  Fall back to sudo for fresh installs
    # where root may own the directory.
    chown -Rh "${user}:${group}" "${INSTALL_DIR}" 2>/dev/null || \
        $SUDO chown -Rh "${user}:${group}" "${INSTALL_DIR}" 2>/dev/null || true
}

# Stop service for update
stop_service() {
    local platform=${1:-}

    if [ "$platform" = "snapmaker-u1" ]; then
        stop_service_snapmaker_u1
        return
    fi

    if [ "$INIT_SYSTEM" = "systemd" ]; then
        # Under NoNewPrivileges (self-update), sudo is blocked.  The service will be
        # restarted by the watchdog after exit(0) — stopping it here isn't needed.
        if _has_no_new_privs; then
            log_info "Skipping service stop (NoNewPrivileges; restart via watchdog)"
        elif systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
            log_info "Stopping existing HelixScreen service (systemd)..."
            $SUDO systemctl stop "$SERVICE_NAME" || true
        fi
    else
        # During in-app self-update, the app must stay running so the user sees
        # the update progress screen.  The watchdog restarts via _exit(0) after
        # install.sh exits — killing the process here would black-screen the display.
        if _is_self_update; then
            log_info "Skipping service stop (self-update; restart via watchdog)"
        else
            # Try the configured init script location first
            if [ -n "$INIT_SCRIPT_DEST" ] && [ -x "$INIT_SCRIPT_DEST" ]; then
                log_info "Stopping existing HelixScreen service (SysV)..."
                $SUDO "$INIT_SCRIPT_DEST" stop 2>/dev/null || true
            fi
            # Also check all possible locations (for updates/uninstalls)
            for init_script in $HELIX_INIT_SCRIPTS; do
                if [ -x "$init_script" ]; then
                    log_info "Stopping HelixScreen at $init_script..."
                    $SUDO "$init_script" stop 2>/dev/null || true
                fi
            done
            # Also try to kill by name (watchdog first to prevent crash dialog flash)
            # shellcheck disable=SC2086
            kill_process_by_name $HELIX_PROCESSES || true
        fi
    fi
}

# Stop service (Snapmaker U1)
stop_service_snapmaker_u1() {
    if _is_self_update; then
        log_info "Skipping service stop (self-update; restart via watchdog)"
        return
    fi

    local init_src="${INSTALL_DIR}/config/helixscreen.init"
    if [ -x "$init_src" ]; then
        log_info "Stopping existing HelixScreen service (Snapmaker U1)..."
        $SUDO "$init_src" stop 2>/dev/null || true
    fi
    # Also kill by name in case init script stop didn't clean up
    # shellcheck disable=SC2086
    kill_process_by_name $HELIX_PROCESSES || true
}
