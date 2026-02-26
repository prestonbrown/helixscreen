#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: platform
# Platform detection: AD5M vs K1 vs Pi, firmware variant, installation paths
#
# Reads: -
# Writes: PLATFORM, AD5M_FIRMWARE, K1_FIRMWARE, INSTALL_DIR, INIT_SCRIPT_DEST, PREVIOUS_UI_SCRIPT, TMP_DIR, KLIPPER_USER, KLIPPER_HOME

# Source guard
[ -n "${_HELIX_PLATFORM_SOURCED:-}" ] && return 0
_HELIX_PLATFORM_SOURCED=1

# Default paths (may be overridden by set_install_paths)
: "${INSTALL_DIR:=/opt/helixscreen}"
: "${TMP_DIR:=}"

# Capture user-provided INSTALL_DIR before we potentially override it.
# If the user explicitly set INSTALL_DIR (and it's not the default),
# we respect their choice over auto-detection.
_USER_INSTALL_DIR="${INSTALL_DIR}"
[ "$_USER_INSTALL_DIR" = "/opt/helixscreen" ] && _USER_INSTALL_DIR=""
INIT_SCRIPT_DEST=""
PREVIOUS_UI_SCRIPT=""
AD5M_FIRMWARE=""
K1_FIRMWARE=""
KLIPPER_USER=""
KLIPPER_HOME=""

# Detect platform
# Returns: "ad5m", "ad5x", "k1", "pi", "pi32", or "unsupported"
detect_platform() {
    local arch kernel
    arch=$(uname -m)
    kernel=$(uname -r)

    # Check for AD5M (armv7l with specific kernel)
    if [ "$arch" = "armv7l" ]; then
        # AD5M has a specific kernel identifier
        if echo "$kernel" | grep -q "ad5m\|5.4.61"; then
            echo "ad5m"
            return
        fi
    fi

    # Check for FlashForge AD5X (MIPS with /usr/data and FlashForge indicators)
    # AD5X uses Ingenic X2600 (MIPS), has /usr/prog/ and /usr/data/ layout
    if [ "$arch" = "mips" ]; then
        if [ -d "/usr/data" ] && [ -d "/usr/prog" ]; then
            echo "ad5x"
            return
        fi
    fi

    # Check for Creality K1 series (Simple AF or stock with Klipper)
    # K1 uses buildroot and has /usr/data structure
    if [ -f /etc/os-release ] && grep -q "buildroot" /etc/os-release 2>/dev/null; then
        # Buildroot-based system - check for K1 indicators
        if [ -d "/usr/data" ]; then
            # Check for K1-specific indicators (require at least 2 for confidence)
            # - get_sn_mac.sh is a Creality-specific script
            # - /usr/data/pellcorp is Simple AF
            # - /usr/data/printer_data with klipper is a strong K1 indicator
            local k1_indicators=0
            [ -x "/usr/bin/get_sn_mac.sh" ] && k1_indicators=$((k1_indicators + 1))
            [ -d "/usr/data/pellcorp" ] && k1_indicators=$((k1_indicators + 1))
            [ -d "/usr/data/printer_data" ] && k1_indicators=$((k1_indicators + 1))
            [ -d "/usr/data/klipper" ] && k1_indicators=$((k1_indicators + 1))
            # Also check for Creality-specific paths
            [ -f "/usr/data/creality/userdata/config/system_config.json" ] && k1_indicators=$((k1_indicators + 1))

            if [ "$k1_indicators" -ge 2 ]; then
                echo "k1"
                return
            fi
        fi
    fi

    # Check for Debian-family SBC (Raspberry Pi, MKS, BTT, Armbian, etc.)
    # Returns "pi" for 64-bit, "pi32" for 32-bit
    if [ "$arch" = "aarch64" ] || [ "$arch" = "armv7l" ]; then
        local is_arm_sbc=false

        # 1. os-release: check for Debian-family indicators
        #    ID_LIKE=debian appears in all derivatives (Ubuntu, Armbian, Raspbian, etc.)
        #    so grepping for "debian" alone catches most cases.
        if [ -f /etc/os-release ] && \
           grep -qi "debian\|raspbian\|ubuntu\|armbian" /etc/os-release 2>/dev/null; then
            is_arm_sbc=true
        fi

        # 2. Package manager: dpkg is the definitive Debian-family indicator.
        #    Catches any derivative not listed above.
        if [ "$is_arm_sbc" = false ] && command -v dpkg >/dev/null 2>&1; then
            is_arm_sbc=true
        fi

        # 3. Well-known SBC user home directories (MainsailOS, BTT Pi, MKS)
        if [ "$is_arm_sbc" = false ]; then
            if [ -d /home/pi ] || [ -d /home/mks ] || [ -d /home/biqu ]; then
                is_arm_sbc=true
            fi
        fi

        if [ "$is_arm_sbc" = true ]; then
            # Detect actual userspace bitness, not just kernel arch.
            # Many Pi systems run 64-bit kernel with 32-bit userspace,
            # which makes uname -m report aarch64 even though only
            # 32-bit binaries can execute.
            local userspace_bits
            userspace_bits=$(getconf LONG_BIT 2>/dev/null || echo "")
            if [ "$userspace_bits" = "64" ]; then
                echo "pi"
            elif [ "$userspace_bits" = "32" ]; then
                if [ "$arch" = "aarch64" ]; then
                    log_warn "64-bit kernel with 32-bit userspace detected — using pi32 build"
                fi
                echo "pi32"
            else
                # getconf unavailable — fall back to checking system binary
                if file /usr/bin/id 2>/dev/null | grep -q "64-bit"; then
                    echo "pi"
                elif file /usr/bin/id 2>/dev/null | grep -q "32-bit"; then
                    if [ "$arch" = "aarch64" ]; then
                        log_warn "64-bit kernel with 32-bit userspace detected — using pi32 build"
                    fi
                    echo "pi32"
                else
                    # Last resort: trust kernel arch
                    if [ "$arch" = "aarch64" ]; then
                        echo "pi"
                    else
                        echo "pi32"
                    fi
                fi
            fi
            return
        fi
    fi

    # Unknown ARM device - don't assume it's a Pi
    # Require explicit platform indicators to avoid false positives
    if [ "$arch" = "aarch64" ] || [ "$arch" = "armv7l" ]; then
        log_warn "Unknown ARM platform. Cannot auto-detect."
        echo "unsupported"
        return
    fi

    echo "unsupported"
}

# Detect the Klipper ecosystem user (who runs klipper/moonraker services)
# Detection cascade (most reliable first):
#   1. systemd: systemctl show klipper.service
#   2. Process table: ps for running klipper
#   3. printer_data scan: /home/*/printer_data
#   4. Well-known users: biqu, pi, mks
#   5. Fallback: root
# Sets: KLIPPER_USER, KLIPPER_HOME
detect_klipper_user() {
    # 1. systemd service owner (most reliable on Pi)
    if command -v systemctl >/dev/null 2>&1; then
        local svc_user
        svc_user=$(systemctl show -p User --value klipper.service 2>/dev/null) || true
        if [ -n "$svc_user" ] && [ "$svc_user" != "root" ] && id "$svc_user" >/dev/null 2>&1; then
            KLIPPER_USER="$svc_user"
            KLIPPER_HOME=$(eval echo "~$svc_user")
            log_info "Klipper user (systemd): $KLIPPER_USER"
            return 0
        fi
    fi

    # 2. Process table (catches running instances)
    local ps_user
    ps_user=$(ps -eo user,comm 2>/dev/null | awk '/klipper$/ && !/grep/ {print $1; exit}') || true
    if [ -n "$ps_user" ] && [ "$ps_user" != "root" ] && id "$ps_user" >/dev/null 2>&1; then
        KLIPPER_USER="$ps_user"
        KLIPPER_HOME=$(eval echo "~$ps_user")
        log_info "Klipper user (process): $KLIPPER_USER"
        return 0
    fi

    # 3. printer_data directory scan
    local pd_dir
    for pd_dir in /home/*/printer_data; do
        [ -d "$pd_dir" ] || continue
        local pd_user
        pd_user=$(echo "$pd_dir" | sed 's|^/home/||;s|/printer_data$||')
        if [ -n "$pd_user" ] && id "$pd_user" >/dev/null 2>&1; then
            KLIPPER_USER="$pd_user"
            KLIPPER_HOME="/home/$pd_user"
            log_info "Klipper user (printer_data): $KLIPPER_USER"
            return 0
        fi
    done

    # 4. Well-known users (checked in priority order)
    local known_user
    for known_user in biqu pi mks; do
        if id "$known_user" >/dev/null 2>&1; then
            KLIPPER_USER="$known_user"
            KLIPPER_HOME="/home/$known_user"
            log_info "Klipper user (well-known): $KLIPPER_USER"
            return 0
        fi
    done

    # 5. Fallback: root (embedded platforms, AD5M, K1)
    KLIPPER_USER="root"
    KLIPPER_HOME="/root"
    log_info "Klipper user (fallback): root"
    return 0
}

# Detect AD5M firmware variant (Klipper Mod vs Forge-X)
# Only called when platform is "ad5m"
# Returns: "klipper_mod" or "forge_x"
detect_ad5m_firmware() {
    # Klipper Mod indicators - check for its specific directory structure
    # Klipper Mod runs in a chroot on /mnt/data/.klipper_mod/chroot
    # and puts printer software in /root/printer_software/
    if [ -d "/root/printer_software" ] || [ -d "/mnt/data/.klipper_mod" ]; then
        echo "klipper_mod"
        return
    fi

    # Forge-X indicators - check for its mod overlay structure
    if [ -d "/opt/config/mod/.root" ]; then
        echo "forge_x"
        return
    fi

    # Default to forge_x (original behavior, most common)
    echo "forge_x"
}

# Detect K1 firmware variant (Simple AF vs other)
# Only called when platform is "k1"
# Returns: "simple_af" or "stock_klipper"
detect_k1_firmware() {
    # Simple AF (pellcorp/creality) indicators
    if [ -d "/usr/data/pellcorp" ]; then
        echo "simple_af"
        return
    fi

    # Check for GuppyScreen which Simple AF installs
    if [ -d "/usr/data/guppyscreen" ] && [ -f "/etc/init.d/S99guppyscreen" ]; then
        echo "simple_af"
        return
    fi

    # Default to stock_klipper (generic K1 with Klipper)
    echo "stock_klipper"
}

# Detect Pi install directory based on Klipper ecosystem presence
# Cascade (first match wins):
#   1. User explicitly set INSTALL_DIR → keep it
#   2. ~/klipper or ~/moonraker exists → ~/helixscreen
#   3. ~/printer_data exists → ~/helixscreen
#   4. moonraker.service is active → ~/helixscreen
#   5. Fallback → /opt/helixscreen
# Requires: KLIPPER_HOME to be set (by detect_klipper_user)
# Sets: INSTALL_DIR
detect_pi_install_dir() {
    # 1. User explicitly set INSTALL_DIR — respect their choice
    if [ -n "$_USER_INSTALL_DIR" ]; then
        INSTALL_DIR="$_USER_INSTALL_DIR"
        log_info "Install directory (user override): $INSTALL_DIR"
        return 0
    fi

    # Need KLIPPER_HOME for ecosystem detection
    if [ -z "$KLIPPER_HOME" ]; then
        INSTALL_DIR="/opt/helixscreen"
        return 0
    fi

    # 2. Klipper or Moonraker source directories
    if [ -d "$KLIPPER_HOME/klipper" ] || [ -d "$KLIPPER_HOME/moonraker" ]; then
        INSTALL_DIR="$KLIPPER_HOME/helixscreen"
        log_info "Install directory (klipper ecosystem): $INSTALL_DIR"
        return 0
    fi

    # 3. printer_data directory (Klipper config structure)
    if [ -d "$KLIPPER_HOME/printer_data" ]; then
        INSTALL_DIR="$KLIPPER_HOME/helixscreen"
        log_info "Install directory (printer_data): $INSTALL_DIR"
        return 0
    fi

    # 4. Moonraker service running (ecosystem present but maybe different layout)
    if command -v systemctl >/dev/null 2>&1; then
        if systemctl is-active --quiet moonraker.service 2>/dev/null || \
           systemctl is-active --quiet moonraker 2>/dev/null; then
            INSTALL_DIR="$KLIPPER_HOME/helixscreen"
            log_info "Install directory (moonraker service): $INSTALL_DIR"
            return 0
        fi
    fi

    # 5. Fallback: no ecosystem detected
    INSTALL_DIR="/opt/helixscreen"
    return 0
}

# Detect best temp directory for extraction
# Mirrors get_helix_cache_dir() heuristic from app_globals.cpp:
# tries candidates in order, picks first writable dir with >= 100MB free.
# User can override via TMP_DIR env var.
# Sets: TMP_DIR
detect_tmp_dir() {
    # User already set TMP_DIR — respect it
    if [ -n "${TMP_DIR:-}" ]; then
        log_info "Temp directory (user override): $TMP_DIR"
        return 0
    fi

    local required_mb=100
    local candidates="/data/helixscreen-install /mnt/data/helixscreen-install /usr/data/helixscreen-install /var/tmp/helixscreen-install /tmp/helixscreen-install"

    for candidate in $candidates; do
        local check_dir
        check_dir=$(dirname "$candidate")

        # Must exist (or be creatable) and be writable
        if [ ! -d "$check_dir" ]; then
            continue
        fi
        if [ ! -w "$check_dir" ] && ! $SUDO test -w "$check_dir" 2>/dev/null; then
            continue
        fi

        # Check free space (BusyBox df: KB in $4)
        local available_mb
        available_mb=$(df "$check_dir" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')
        if [ -z "$available_mb" ] || [ "$available_mb" -lt "$required_mb" ]; then
            continue
        fi

        TMP_DIR="$candidate"
        if [ "$check_dir" != "/tmp" ]; then
            log_info "Temp directory: $TMP_DIR (${available_mb}MB free)"
        fi
        return 0
    done

    # Last resort — /tmp even if small (will fail later at extraction with a clear error)
    TMP_DIR="/tmp/helixscreen-install"
    log_warn "No temp directory with ${required_mb}MB+ free found, using /tmp"
}

# Set installation paths based on platform and firmware
# Sets: INSTALL_DIR, INIT_SCRIPT_DEST, PREVIOUS_UI_SCRIPT, TMP_DIR
set_install_paths() {
    local platform=$1
    local firmware=${2:-}

    if [ "$platform" = "ad5m" ]; then
        case "$firmware" in
            klipper_mod)
                INSTALL_DIR="/root/printer_software/helixscreen"
                INIT_SCRIPT_DEST="/etc/init.d/S80helixscreen"
                PREVIOUS_UI_SCRIPT="/etc/init.d/S80klipperscreen"
                log_info "AD5M firmware: Klipper Mod"
                log_info "Install directory: ${INSTALL_DIR}"
                ;;
            forge_x|*)
                INSTALL_DIR="/opt/helixscreen"
                INIT_SCRIPT_DEST="/etc/init.d/S90helixscreen"
                PREVIOUS_UI_SCRIPT="/opt/config/mod/.root/S80guppyscreen"
                log_info "AD5M firmware: Forge-X"
                log_info "Install directory: ${INSTALL_DIR}"
                ;;
        esac
    elif [ "$platform" = "ad5x" ]; then
        # FlashForge AD5X - uses ZMOD, /usr/data structure
        INSTALL_DIR="/srv/helixscreen"
        INIT_SCRIPT_DEST="/etc/init.d/S80helixscreen"
        PREVIOUS_UI_SCRIPT=""
        log_info "Platform: FlashForge AD5X (ZMOD)"
        log_info "Install directory: ${INSTALL_DIR}"
    elif [ "$platform" = "k1" ]; then
        # Creality K1 series - uses /usr/data structure
        case "$firmware" in
            simple_af|*)
                INSTALL_DIR="/usr/data/helixscreen"
                INIT_SCRIPT_DEST="/etc/init.d/S99helixscreen"
                PREVIOUS_UI_SCRIPT="/etc/init.d/S99guppyscreen"
                log_info "K1 firmware: Simple AF"
                log_info "Install directory: ${INSTALL_DIR}"
                ;;
        esac
    else
        # Pi and other platforms — detect klipper user, then auto-detect install dir
        INIT_SCRIPT_DEST="/etc/init.d/S90helixscreen"
        PREVIOUS_UI_SCRIPT=""
        detect_klipper_user
        detect_pi_install_dir
    fi

    # Auto-detect best temp directory (all platforms)
    detect_tmp_dir
}

# Create symlink from printer_data/config/helixscreen → INSTALL_DIR/config
# Allows Mainsail/Fluidd users to edit HelixScreen config from the web UI.
# Only applies to Pi/Klipper platforms where printer_data exists.
# Gracefully skips if printer_data/config doesn't exist or permissions fail.
# Reads: KLIPPER_HOME, INSTALL_DIR
setup_config_symlink() {
    # Only proceed if we have a Klipper home and install directory
    if [ -z "${KLIPPER_HOME:-}" ] || [ -z "${INSTALL_DIR:-}" ]; then
        return 0
    fi

    local config_dir="${KLIPPER_HOME}/printer_data/config"
    local symlink_path="${config_dir}/helixscreen"
    local target="${INSTALL_DIR}/config"

    # Skip if printer_data/config doesn't exist
    if [ ! -d "$config_dir" ]; then
        log_info "No printer_data/config found, skipping config symlink"
        return 0
    fi

    # Skip if target config directory doesn't exist
    if [ ! -d "$target" ]; then
        log_warn "Install config directory not found: $target"
        return 0
    fi

    # Check if symlink already exists
    if [ -L "$symlink_path" ]; then
        local current_target
        current_target=$(readlink "$symlink_path" 2>/dev/null || echo "")
        if [ "$current_target" = "$target" ]; then
            log_info "Config symlink already exists and is correct"
            return 0
        fi
        # Wrong target — update it
        log_info "Updating config symlink (was: $current_target)"
        $(file_sudo "$symlink_path") rm -f "$symlink_path"
    elif [ -e "$symlink_path" ]; then
        # Something exists but isn't a symlink — don't destroy it
        log_warn "Config symlink path already exists as a regular file/directory: $symlink_path"
        log_warn "Skipping symlink creation to avoid data loss"
        return 0
    fi

    # Create the symlink
    if $(file_sudo "$config_dir") ln -s "$target" "$symlink_path" 2>/dev/null; then
        log_success "Config symlink: $symlink_path → $target"
        log_info "You can now edit HelixScreen config from Mainsail/Fluidd"
    else
        log_warn "Could not create config symlink (permission denied?)"
        log_warn "To create manually: ln -s $target $symlink_path"
    fi

    return 0
}
