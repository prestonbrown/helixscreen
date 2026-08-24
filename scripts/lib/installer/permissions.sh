#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: permissions
# Permission checks, sudo handling, and system permission rules (udev, polkit)
#
# Reads: (platform passed as argument), INSTALL_DIR, KLIPPER_USER, SUDO
# Writes: SUDO

# Source guard
[ -n "${_HELIX_PERMISSIONS_SOURCED:-}" ] && return 0
_HELIX_PERMISSIONS_SOURCED=1

# Initialize SUDO (will be set by check_permissions)
SUDO=""

# Check if running as root (required for AD5M/K1, optional for Pi)
# Sets: SUDO variable ("sudo" or "")
check_permissions() {
    local platform=$1

    case "$platform" in
    ad5m|ad5x|k1|k2)
        if [ "$(id -u)" != "0" ]; then
            log_error "Installation on $platform requires root privileges."
            log_error "Please run: sudo $0 $*"
            exit 1
        fi
        SUDO=""
        ;;
    *)
        # Pi: warn if not root but allow sudo
        if [ "$(id -u)" != "0" ]; then
            if ! command -v sudo >/dev/null 2>&1; then
                log_error "Not running as root and sudo is not available."
                log_error "Please run as root or install sudo."
                exit 1
            fi
            log_info "Not running as root. Will use sudo for privileged operations."
            SUDO="sudo"
        else
            SUDO=""
        fi
        ;;
    esac
}

# Check if a polkit rule for HelixScreen exists (either .rules or .pkla)
_polkit_rule_exists() {
    local pkla="/etc/polkit-1/localauthority/50-local.d/helixscreen-network.pkla"
    local rules="/etc/polkit-1/rules.d/49-helixscreen-network.rules"
    local rules_old="/etc/polkit-1/rules.d/50-helixscreen-network.rules"

    test -f "$rules" 2>/dev/null && return 0
    test -f "$rules_old" 2>/dev/null && return 0
    test -f "$pkla" 2>/dev/null && return 0
    return 1
}

# Check if deployed permission rules contain un-substituted templates
_permission_rules_need_repair() {
    local helix_user="$1"
    local pkla="/etc/polkit-1/localauthority/50-local.d/helixscreen-network.pkla"
    local rules="/etc/polkit-1/rules.d/49-helixscreen-network.rules"

    # Check pkla file for literal placeholder (parent dir may be root-only)
    if $SUDO test -f "$pkla" && $SUDO grep -q '@@HELIX_USER@@' "$pkla" 2>/dev/null; then
        return 0
    fi
    # Check JS rules file for literal placeholder
    if $SUDO test -f "$rules" && $SUDO grep -q '@@HELIX_USER@@' "$rules" 2>/dev/null; then
        return 0
    fi
    return 1
}

# Install system permission rules for non-root operation
# - udev rule: backlight brightness write access for video group
# - polkit rule: NetworkManager access for service user
# Only installed on platforms that run as non-root (Pi, generic Linux)
install_permission_rules() {
    local platform=$1
    local helix_user="${KLIPPER_USER:-root}"

    # Skip for platforms that run as root or if user is root
    case "$platform" in ad5m|ad5x|k1|k2) helix_user="root" ;; esac
    if [ "$helix_user" = "root" ]; then
        log_info "Skipping permission rules (running as root)"
        return 0
    fi

    # Under NoNewPrivileges (self-update), sudo is blocked.  Check if rules
    # are already correctly deployed before skipping.
    if _has_no_new_privs; then
        if command -v nmcli >/dev/null 2>&1 && ! _polkit_rule_exists; then
            log_warn "NetworkManager polkit rule is MISSING — Wi-Fi will not work as non-root."
            log_warn "Fix by re-running the installer:  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
        elif _permission_rules_need_repair "$helix_user"; then
            log_warn "Permission rules need repair (pkla/polkit file has un-substituted template)."
            log_warn "Wi-Fi may not work. Fix with:  sudo sed -i 's|@@HELIX_USER@@|${helix_user}|g' /etc/polkit-1/localauthority/50-local.d/helixscreen-network.pkla"
            log_warn "Or re-run the installer:  curl -fsSL https://releases.helixscreen.org/install.sh | sh -s -- --update"
        else
            log_info "Skipping permission rules (NoNewPrivileges; already installed)"
        fi
        return 0
    fi

    # --- Backlight udev rule ---
    local udev_src="${INSTALL_DIR}/config/99-helixscreen-backlight.rules"
    local udev_dest="/etc/udev/rules.d/99-helixscreen-backlight.rules"

    if [ -f "$udev_src" ] && [ -d /etc/udev/rules.d ]; then
        $SUDO cp "$udev_src" "$udev_dest"
        # Trigger udev to apply immediately for any existing backlight devices
        $SUDO udevadm trigger --subsystem-match=backlight 2>/dev/null || true
        log_info "Installed backlight udev rule"
    fi

    # --- NetworkManager polkit rule ---
    # All content is generated inline — no template files, no sed, no @@HELIX_USER@@.
    if command -v nmcli >/dev/null 2>&1; then
        # polkit JavaScript rules for newer polkit (Debian 12+, polkit >= 0.106)
        local rules_dir="/etc/polkit-1/rules.d"
        # polkit local authority (.pkla) for older polkit (Debian 11 and similar)
        local pkla_dir="/etc/polkit-1/localauthority/50-local.d"

        if $SUDO test -d "$rules_dir"; then
            local rules_dest="${rules_dir}/49-helixscreen-network.rules"
            if $SUDO tee "$rules_dest" > /dev/null << POLKIT_EOF
// Installed by HelixScreen — allow service user to manage NetworkManager
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager.") === 0 &&
        subject.user === "${helix_user}") {
        return polkit.Result.YES;
    }
});
POLKIT_EOF
            then
                log_info "Installed NetworkManager polkit rule (.rules for ${helix_user})"
            else
                log_warn "Failed to install polkit rule to ${rules_dest} — Wi-Fi scanning may not work"
            fi
            # Clean up old 50- rule and stale .pkla from older installs / OS upgrades
            $SUDO rm -f "${rules_dir}/50-helixscreen-network.rules" 2>/dev/null || true
            local stale_pkla="${pkla_dir}/helixscreen-network.pkla"
            if $SUDO test -f "$stale_pkla" 2>/dev/null; then
                $SUDO rm -f "$stale_pkla" 2>/dev/null || true
                log_info "Removed stale .pkla file (superseded by .rules)"
            fi
        elif $SUDO test -d "$pkla_dir"; then
            local pkla_dest="${pkla_dir}/helixscreen-network.pkla"
            if $SUDO tee "$pkla_dest" > /dev/null << PKLA_EOF
# SPDX-License-Identifier: GPL-3.0-or-later
# polkit local authority rule: allow HelixScreen service user to manage
# NetworkManager connections (Wi-Fi connect/disconnect/scan)
# Installed by HelixScreen for non-root service operation
[HelixScreen NetworkManager access]
Identity=unix-user:${helix_user}
Action=org.freedesktop.NetworkManager.*
ResultAny=yes
ResultInactive=yes
ResultActive=yes
PKLA_EOF
            then
                log_info "Installed NetworkManager polkit rule (.pkla for ${helix_user})"
            else
                log_warn "Failed to install polkit rule to ${pkla_dest} — Wi-Fi scanning may not work"
            fi
        else
            log_warn "polkit rules directory not found — Wi-Fi may not work as non-root"
        fi

        # Reload polkit to pick up the new rule immediately
        if systemctl is-active polkit >/dev/null 2>&1; then
            $SUDO systemctl restart polkit 2>/dev/null || true
            log_info "Restarted polkit to apply new rules"
        fi
    fi
}
