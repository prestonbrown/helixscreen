#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: platform
# Platform detection: AD5M vs K1 vs Pi, firmware variant, installation paths
#
# Reads: -
# Writes: PLATFORM, AD5M_FIRMWARE, K1_FIRMWARE, INSTALL_DIR, INIT_SCRIPT_DEST, PREVIOUS_UI_SCRIPT, TMP_DIR, KLIPPER_USER, KLIPPER_GROUP, KLIPPER_HOME

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
# shellcheck disable=SC2034  # consumed by main.sh and competing_uis.sh
K1_FIRMWARE=""
KLIPPER_USER=""
KLIPPER_GROUP=""
KLIPPER_HOME=""
# Empty means "derive <KLIPPER_HOME>/printer_data/config". Only firmwares with
# no printer_data at all (CC1/COSMOS) set it. Read via klipper_config_dir().
KLIPPER_CONFIG_DIR=""

# Friendly description of the underlying SBC for the user-facing log line.
# Returns free-text (NOT used for routing). Pi/pi32 covers a long tail of
# aarch64/armv7l Klipper boxes (Raspberry Pi, BTT CB1, MKS-Pi, QIDI Q2/Plus,
# generic Armbian) — they all run the same binary, but "Detected: pi" reads
# as wrong to anyone whose printer says QIDI on the lid. This emits a label
# based on /proc/device-tree/model, hostname, and home-directory shape so
# the user sees their own hardware reflected back.
describe_hardware() {
    local _model="" _hostname="" _pretty=""

    if [ -r /proc/device-tree/model ]; then
        _model=$(tr -d '\0' </proc/device-tree/model 2>/dev/null)
    fi
    if [ -r /etc/hostname ]; then
        _hostname=$(cat /etc/hostname 2>/dev/null | tr -d '[:space:]')
    fi
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        _pretty=$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-${NAME:-}}")
    fi

    # Device-tree model is the strongest signal when present.
    case "$_model" in
        *"Raspberry Pi"*) echo "$_model"; return ;;
        *"BIGTREETECH"*|*"BTT"*) echo "BIGTREETECH SBC ($_model)"; return ;;
        *Rockchip*|*"RK3"*|*Allwinner*|*Amlogic*) ;;  # fall through, often too generic alone
    esac

    # Hostname + user-dir heuristics for boards without a useful DT model.
    # `linaro-alip` is the Linaro Debian reference rootfs hostname; QIDI ships it
    # on the Q2 (and likely Plus 4) running Klipper as `mks` on firmware 1.1.x.
    # The June-2026 01.01.02 refactor renamed that user to `qidi` (#1047), so
    # accept either home dir.
    if [ "$_hostname" = "linaro-alip" ] && { [ -d "/home/mks" ] || [ -d "/home/qidi" ]; }; then
        echo "QIDI-class SBC (likely Q2/Plus, hostname: $_hostname, user: mks/qidi)"
        return
    fi
    if [ -d "/home/biqu" ]; then
        echo "BIGTREETECH Pi/CB1 (user: biqu)"
        return
    fi
    if [ -d "/home/mks" ] || [ -d "/home/qidi" ]; then
        echo "MKS/QIDI-branded SBC (user: mks or qidi)"
        return
    fi
    if [ -d "/home/pi" ] && [ -z "$_model" ]; then
        echo "Raspberry Pi (user: pi)"
        return
    fi

    if [ -n "$_model" ]; then
        echo "$_model"
        return
    fi
    if [ -n "$_pretty" ]; then
        echo "ARM SBC ($_pretty)"
        return
    fi
    echo "ARM SBC"
}

# Resolve the primary group of a user, falling back to the user name.
# Some firmwares (e.g. QIDI Q2 stock) ship a `mks` user without a matching
# `mks` group, which makes `Group=mks` in the systemd unit fail with
# "Failed to determine group credentials". Use `id -gn` to find the actual
# primary group; only fall back to the user name when getent fails.
_resolve_primary_group() {
    local _user="$1"
    [ -n "$_user" ] || { echo "root"; return; }
    local _g
    _g=$(id -gn "$_user" 2>/dev/null) || _g=""
    if [ -n "$_g" ]; then
        echo "$_g"
    else
        echo "$_user"
    fi
}

# Print the owning user NAME of a path, or nothing if it cannot be determined.
#
# Order matters: `stat -c` is the GNU/BusyBox spelling and every device we ship
# to (AD5M, K1, K2, CC1, U1, Pi) speaks it, so it stays the first and normally
# the only thing tried. BSD stat (macOS dev machines) has no -c and spells the
# same field `-f '%Su'`; that runs only after the GNU form has already failed,
# so device behaviour is unchanged.
_file_owner_name() {
    local _p="$1"
    [ -n "$_p" ] || return 0
    local _o
    _o=$(stat -c '%U' "$_p" 2>/dev/null) || _o=""
    [ -n "$_o" ] || _o=$(stat -f '%Su' "$_p" 2>/dev/null) || _o=""
    echo "$_o"
}

# QIDI-class SBC fingerprint (Q2, and likely Plus 4): the Linaro Debian
# reference rootfs hostname `linaro-alip` plus the Klipper user home. This is the
# same signal get_hardware_label() uses to print "QIDI-class SBC". The user is
# `mks` on firmware 1.1.x and `qidi` on the 01.01.02 refactor (#1047), so accept
# either home dir.
#
# It exists to VETO Artillery M1 detection: the Q2 ships the stock Artillery
# `algo_app.service`, so algo_app alone cannot tell a Q2 apart from a real M1
# (field-confirmed on a Q2 in prestonbrown/helixscreen#1027 — algo_app.service
# present, makerbase-client.services absent). The hostname+user pair is the
# only reliable QIDI marker we have, and the M1 is not a linaro-alip host.
_is_qidi_class_sbc() {
    local _h=""
    [ -r /etc/hostname ] && _h=$(cat /etc/hostname 2>/dev/null | tr -d '[:space:]')
    [ "$_h" = "linaro-alip" ] && { [ -d /home/mks ] || [ -d /home/qidi ]; }
}

# Detect platform
# Returns: "ad5m", "ad5x", "cc1", "k1", "k2", "m1", "pi", "pi32", "snapmaker-u1", "x86", or "unsupported"
detect_platform() {
    local arch kernel
    arch=$(uname -m)
    kernel=$(uname -r)

    # Check for Elegoo Centauri Carbon running OpenCentauri COSMOS firmware.
    # MUST come before AD5M check — both are armv7l with kernel 5.4.61.
    # COSMOS replaces the stock Tina/OpenWrt userland with a Yocto-based rootfs
    # that ships Klipper, Moonraker, and its own update tool.
    if [ "$arch" = "armv7l" ] && [ -x "/usr/bin/update-cosmos" ]; then
        echo "cc1"
        return
    fi

    # Check for Creality K2 series (armv7l, OpenWrt/Tina Linux, /mnt/UDISK)
    # MUST come before AD5M check — both are armv7l with kernel 5.4.61
    if [ "$arch" = "armv7l" ] && [ -d "/mnt/UDISK" ]; then
        # K2 runs Tina Linux (OpenWrt-based) with storage on /mnt/UDISK
        if [ -f /etc/os-release ] && grep -qi "openwrt\|tina" /etc/os-release 2>/dev/null; then
            echo "k2"
            return
        fi
        # Fallback: /mnt/UDISK/printer_data is a strong K2 indicator even without os-release
        if [ -d "/mnt/UDISK/printer_data" ] || [ -d "/mnt/UDISK/creality" ]; then
            echo "k2"
            return
        fi
    fi

    # Check for AD5M (armv7l with specific kernel, NOT K2)
    if [ "$arch" = "armv7l" ]; then
        # AD5M has a specific kernel identifier
        if echo "$kernel" | grep -q "ad5m\|5.4.61"; then
            echo "ad5m"
            return
        fi
    fi

    # Check for FlashForge AD5X (MIPS with /usr/data and FlashForge indicators)
    # AD5X uses Ingenic X2600 (MIPS); identified by /usr/prog/ dir or /ZMOD file alongside /usr/data/
    if [ "$arch" = "mips" ]; then
        if [ -d "/usr/data" ] && { [ -d "/usr/prog" ] || [ -f "/ZMOD" ]; }; then
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

    # Snapmaker U1 (aarch64 + extended firmware markers)
    # Must check BEFORE generic Pi/ARM SBC since U1 is also aarch64 Debian
    if [ "$arch" = "aarch64" ]; then
        local u1_markers=0
        [ -d "/home/lava" ] && u1_markers=$((u1_markers + 1))
        [ -d "/home/lava/printer_data" ] && u1_markers=$((u1_markers + 1))
        [ -x "/usr/bin/unisrv" ] && u1_markers=$((u1_markers + 1))
        [ -d "/oem" ] && u1_markers=$((u1_markers + 1))
        # Check for RK3562 SoC in device-tree
        if [ -f /proc/device-tree/compatible ] && grep -q "rockchip,rk3562" /proc/device-tree/compatible 2>/dev/null; then
            u1_markers=$((u1_markers + 1))
        fi
        if [ "$u1_markers" -ge 2 ]; then
            echo "snapmaker-u1"
            return 0
        fi
    fi

    # Artillery M1 Pro (systemd Debian SBC running stock Artillery firmware).
    # MUST come before the generic Debian SBC fallback since M1 is Debian-based
    # and would otherwise be misdetected as plain "pi" — which would skip the
    # M1 platform hook that stops the stock UIs (algo_app + makerbase-client).
    #
    # Fingerprint: algo_app.service (Artillery's proprietary "AI" service). But
    # algo_app is NOT exclusive to the M1 — the QIDI Q2 ships it too (field-
    # confirmed: algo_app.service present, makerbase-client.services absent on a
    # Q2 — prestonbrown/helixscreen#1027). An earlier fix that keyed M1 solely on
    # algo_app therefore still misdetected the Q2 as m1. So we additionally VETO
    # via the QIDI-class fingerprint (hostname linaro-alip + /home/mks or
    # /home/qidi): a Q2 falls through to the normal pi/pi32 arch path below. The M1 hook still
    # stops both stock services regardless of which fingerprint triggered.
    if [ "$arch" = "aarch64" ] || [ "$arch" = "armv7l" ]; then
        local is_m1=false
        if [ -f /etc/systemd/system/algo_app.service ] || \
           [ -f /lib/systemd/system/algo_app.service ]; then
            is_m1=true
        elif command -v systemctl >/dev/null 2>&1 && \
             systemctl list-unit-files 2>/dev/null | grep -qE '^algo_app\.service\b'; then
            is_m1=true
        fi
        if [ "$is_m1" = true ] && ! _is_qidi_class_sbc; then
            echo "m1"
            return 0
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

        # 3. Well-known SBC user home directories (MainsailOS, BTT Pi, MKS, QIDI)
        #    QIDI is `mks` on firmware 1.1.x and `qidi` on 01.01.02 (#1047).
        if [ "$is_arm_sbc" = false ]; then
            if [ -d /home/pi ] || [ -d /home/mks ] || [ -d /home/biqu ] || [ -d /home/qidi ]; then
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

    # Check for x86_64 Linux (generic Debian/Ubuntu desktop or server)
    if [ "$arch" = "x86_64" ] || [ "$arch" = "i686" ] || [ "$arch" = "amd64" ]; then
        echo "x86"
        return
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

# Map a detected platform to the platform key used for release downloads.
#
# Most platforms are 1:1 (pi → pi, ad5m → ad5m, …). The exception is platforms
# like `m1` (Artillery M1 Pro) that have no dedicated release artifact: the
# generic pi/pi32 binary is ABI-compatible, so we reuse it instead of doubling
# CI for an identical build. The `m1` *platform* key is preserved everywhere
# else (hooks, banner, competing-UI shutdown) so M1-specific behavior still
# fires; only the download URL changes.
#
# Args: detected_platform
# Echoes: platform key to use when constructing release archive URLs
get_download_platform() {
    local detected=$1
    case "$detected" in
        m1)
            # Artillery M1 Pro is a Debian SBC. The pi/pi32 binary runs as-is.
            # Pick the variant matching the device's userspace bitness.
            local userspace_bits
            userspace_bits=$(getconf LONG_BIT 2>/dev/null || echo "")
            if [ "$userspace_bits" = "32" ]; then
                echo "pi32"
            else
                echo "pi"
            fi
            ;;
        *)
            echo "$detected"
            ;;
    esac
}

# Single source of truth: platform -> self-update release asset (zip) name.
#
# Both the Moonraker type:web updater (write_release_info() in moonraker.sh) and
# the build-time release_info.json baked by mk/cross.mk resolve the asset name
# through THIS function, so the two can't drift. Drift is not cosmetic: Moonraker
# downloads the asset whose name matches release_info.json's asset_name, and if
# no asset matches it falls back to the alphabetically-first release asset — a
# .sym debug file — then dies with "File is not a zip file"
# (prestonbrown/helixscreen#993). This must also agree with
# UpdateChecker::get_platform_key() (src/system/update_checker.cpp);
# tests/shell/test_update_platform_coverage.bats enforces the agreement.
#
# Convention: a platform's asset is helixscreen-<platform>.zip. The only borrows
# are handled by get_download_platform() (m1 -> pi/pi32 by userspace bitness) and
# the k1-dynamic dev/debug variant, which is not built by the release matrix and
# rides the stable k1 asset.
#
# Args: platform (detected platform key)
# Echoes: release asset filename, e.g. helixscreen-pi.zip
helix_self_update_asset() {
    case "$1" in
        k1-dynamic) echo "helixscreen-k1.zip" ;;
        *)          echo "helixscreen-$(get_download_platform "$1").zip" ;;
    esac
}

# AD5X (FlashForge / ZMOD) preflight: refuse to run outside the chroot.
#
# ZMOD installs HelixScreen into an overlay rooted at /usr/data/.mod/.zmod/.
# Inside the chroot the rootfs is the overlay (/, /etc, /opt, /srv all live
# under that overlay). Outside, those same paths point at the squashfs base
# view that helix-screen never sees — so a curl|sh, --local, --update, or
# --uninstall run from a fresh SSH session writes to the wrong filesystem
# entirely. The `/usr/data/.mod/.zmod` directory is only visible from outside
# the chroot, so its presence is the reliable "you forgot to chroot" tell.
#
# Aborts with an actionable message when called outside the chroot.
ad5x_check_chroot_context() {
    [ -d "/usr/data/.mod/.zmod" ] || return 0

    log_error ""
    log_error "=========================================================="
    log_error " AD5X manual installs must run inside the ZMOD chroot."
    log_error "=========================================================="
    log_error ""
    log_error "ZMOD installs HelixScreen into an overlay at:"
    log_error "  /usr/data/.mod/.zmod/"
    log_error ""
    log_error "Running this installer from your default SSH shell writes"
    log_error "into the squashfs base view, not the overlay HelixScreen"
    log_error "actually runs out of. That leaves you with broken paths,"
    log_error "no init script in scope, and a printer that boots into"
    log_error "the stock UI."
    log_error ""
    log_error "Enter the chroot first, then re-run your command:"
    log_error ""
    log_error "  chroot /usr/data/.mod/.zmod"
    log_error "  # then re-run: curl ... | sh   OR   sh install.sh --local <zip>"
    log_error "  # OR:          sh install.sh --update / --uninstall"
    log_error ""
    log_error "ZMOD also manages routine updates through Moonraker's update"
    log_error "manager — you only need this manual path for --local installs,"
    log_error "specific versions, or troubleshooting."
    log_error ""
    exit 1
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
            KLIPPER_GROUP=$(_resolve_primary_group "$svc_user")
            KLIPPER_HOME=$(eval echo "~$svc_user")
            log_info "Klipper user (systemd): $KLIPPER_USER (group: $KLIPPER_GROUP)"
            return 0
        fi
    fi

    # 2. Process table (catches running instances)
    local ps_user
    ps_user=$(ps -eo user,comm 2>/dev/null | awk '/klipper$/ && !/grep/ {print $1; exit}') || true
    if [ -n "$ps_user" ] && [ "$ps_user" != "root" ] && id "$ps_user" >/dev/null 2>&1; then
        KLIPPER_USER="$ps_user"
        KLIPPER_GROUP=$(_resolve_primary_group "$ps_user")
        KLIPPER_HOME=$(eval echo "~$ps_user")
        log_info "Klipper user (process): $KLIPPER_USER (group: $KLIPPER_GROUP)"
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
            KLIPPER_GROUP=$(_resolve_primary_group "$pd_user")
            KLIPPER_HOME="/home/$pd_user"
            log_info "Klipper user (printer_data): $KLIPPER_USER (group: $KLIPPER_GROUP)"
            return 0
        fi
    done

    # 4. Well-known users (checked in priority order)
    local known_user
    for known_user in biqu pi mks qidi; do
        if id "$known_user" >/dev/null 2>&1; then
            KLIPPER_USER="$known_user"
            KLIPPER_GROUP=$(_resolve_primary_group "$known_user")
            KLIPPER_HOME="/home/$known_user"
            log_info "Klipper user (well-known): $KLIPPER_USER (group: $KLIPPER_GROUP)"
            return 0
        fi
    done

    # 5. Fallback: root (embedded platforms, AD5M, K1)
    KLIPPER_USER="root"
    KLIPPER_GROUP="root"
    KLIPPER_HOME="/root"
    log_info "Klipper user (fallback): root"
    return 0
}

# Detect AD5M firmware variant (Klipper Mod vs Forge-X vs ZMOD)
# Only called when platform is "ad5m"
# Returns: "klipper_mod", "forge_x", or "zmod"
detect_ad5m_firmware() {
    # ZMOD indicator - check for /ZMOD marker file
    # ZMOD is used on AD5M, AD5M Pro, and AD5X (FlashForge series)
    if [ -f "/ZMOD" ]; then
        echo "zmod"
        return
    fi

    # Klipper Mod indicators - check for its specific directory structure
    # Klipper Mod runs in a chroot on /mnt/data/.klipper_mod/chroot
    # v00.05: printer software in /root/printer_software/
    # v00.06+: printer software in /opt/ (no /root/printer_software)
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

# Detect K1 firmware variant (Simple AF, Guilouz helper-script, or stock)
# Only called when platform is "k1"
# Returns: "simple_af", "guilouz", or "stock_klipper"
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

    # Guilouz Helper Script (community firmware mod for K1 series)
    if [ -d "/usr/data/helper-script" ]; then
        echo "guilouz"
        return
    fi

    # Default to stock_klipper (generic K1 with Klipper)
    echo "stock_klipper"
}

# Locations a HelixScreen install may already occupy, for the existing-install
# probe below. Mirrors HELIX_INSTALL_DIRS in uninstall.sh — same question, same
# answer. Overridable so the bats suite can point it at a sandbox.
: "${_HELIX_KNOWN_INSTALL_DIRS:=/opt/helixscreen /srv/helixscreen /usr/data/helixscreen /root/printer_software/helixscreen /user-resource/helixscreen /userdata/helixscreen}"

# Print the directory of an install that is already on disk, or return 1.
# Checks $KLIPPER_HOME/helixscreen first so an ecosystem install outranks a stale
# tree in one of the fixed locations.
#
# bin/helix-screen is what makes a directory an install: uninstall leaves the
# directory (and its config/) behind on several platforms, and treating that
# husk as a live install would pin every future install to it.
_detect_existing_install_dir() {
    _eid_candidates=""
    [ -n "${KLIPPER_HOME:-}" ] && _eid_candidates="${KLIPPER_HOME}/helixscreen"
    _eid_candidates="${_eid_candidates} ${_HELIX_KNOWN_INSTALL_DIRS}"

    for _eid_dir in $_eid_candidates; do
        if [ -x "${_eid_dir}/bin/helix-screen" ]; then
            printf '%s\n' "$_eid_dir"
            return 0
        fi
    done
    return 1
}

# Detect Pi install directory based on Klipper ecosystem presence
# Cascade (first match wins):
#   1. User explicitly set INSTALL_DIR → keep it
#   2. An install already on disk → keep it where it is
#   3. ~/klipper or ~/moonraker exists → ~/helixscreen
#   4. ~/printer_data exists → ~/helixscreen
#   5. moonraker.service is active → ~/helixscreen
#   6. Non-root service user with a home they own → ~/helixscreen
#   7. Fallback → /opt/helixscreen
# Requires: KLIPPER_HOME to be set (by detect_klipper_user)
# Sets: INSTALL_DIR
detect_pi_install_dir() {
    # 1. User explicitly set INSTALL_DIR — respect their choice, once the name
    #    guard has cleared it. INSTALL_DIR is mv'd aside and rm -rf'd on update
    #    and uninstall, so an unvalidated override destroys its target.
    if [ -n "$_USER_INSTALL_DIR" ]; then
        validate_install_dir "$_USER_INSTALL_DIR" || exit 1
        INSTALL_DIR="$_USER_INSTALL_DIR"
        log_info "Install directory (user override): $INSTALL_DIR"
        return 0
    fi

    # 2. An install already on disk decides, whatever the cascade below would
    #    have picked. Relocating it orphans the old tree and the config in it,
    #    and the cascade's answer is not stable across releases — a box that
    #    matched one branch on first install can match a different one later.
    _existing_install_dir=$(_detect_existing_install_dir) || _existing_install_dir=""
    if [ -n "$_existing_install_dir" ]; then
        INSTALL_DIR="$_existing_install_dir"
        log_info "Install directory (existing install): $INSTALL_DIR"
        return 0
    fi

    # Need KLIPPER_HOME for ecosystem detection
    if [ -z "$KLIPPER_HOME" ]; then
        INSTALL_DIR="/opt/helixscreen"
        return 0
    fi

    # 3. Klipper or Moonraker source directories
    if [ -d "$KLIPPER_HOME/klipper" ] || [ -d "$KLIPPER_HOME/moonraker" ]; then
        INSTALL_DIR="$KLIPPER_HOME/helixscreen"
        log_info "Install directory (klipper ecosystem): $INSTALL_DIR"
        return 0
    fi

    # 4. printer_data directory (Klipper config structure)
    if [ -d "$KLIPPER_HOME/printer_data" ]; then
        INSTALL_DIR="$KLIPPER_HOME/helixscreen"
        log_info "Install directory (printer_data): $INSTALL_DIR"
        return 0
    fi

    # 5. Moonraker service running (ecosystem present but maybe different layout)
    if command -v systemctl >/dev/null 2>&1; then
        if systemctl is-active --quiet moonraker.service 2>/dev/null || \
           systemctl is-active --quiet moonraker 2>/dev/null; then
            INSTALL_DIR="$KLIPPER_HOME/helixscreen"
            log_info "Install directory (moonraker service): $INSTALL_DIR"
            return 0
        fi
    fi

    # 6. No ecosystem, but a real non-root service user with a home they own.
    #    Prefer it over /opt, because an update applies by RENAMING the install
    #    root ("mv <root> <root>.old; mv <new> <root>") and rename mutates the
    #    parent's directory entries — so the parent is what has to be writable by
    #    the service user. /opt is root-owned, which leaves only install.sh's
    #    in-place fallback: delete the root's contents, then move the new ones in.
    #    That path works and is not going away, but it deletes before it moves, so
    #    a failure part-way leaves a half-installed tree (#970). A home-owned
    #    parent keeps the atomic route available.
    #
    #    This is the standalone-display shape: a Pi driving a panel with Klipper
    #    and Moonraker on another box, so every ecosystem check above misses.
    #    Escalation is not an answer here — the unit sets NoNewPrivileges=true, so
    #    sudo cannot run from the app or from the install.sh it forks.
    if [ "${KLIPPER_USER:-root}" != "root" ] && [ -d "$KLIPPER_HOME" ] && \
       [ "$(_file_owner_name "$KLIPPER_HOME")" = "$KLIPPER_USER" ]; then
        INSTALL_DIR="$KLIPPER_HOME/helixscreen"
        log_info "Install directory (service user home, no ecosystem): $INSTALL_DIR"
        return 0
    fi

    # 7. Fallback: no ecosystem, and no home we can rename inside (root installs,
    #    where /opt is writable anyway).
    INSTALL_DIR="/opt/helixscreen"
    return 0
}

# Detect best temp directory for extraction
# Mirrors get_helix_cache_dir() heuristic from app_globals.cpp:
# tries candidates in order, picks first writable dir with >= 100MB free.
# User can override via TMP_DIR env var.
# Sets: TMP_DIR
detect_tmp_dir() {
    # User already set TMP_DIR — respect it, but only after the name guard.
    # TMP_DIR is rm -rf'd on both the success and the failure path, so an
    # unvalidated override erases whatever it points at (validate_tmp_dir in
    # common.sh; the /mnt/UDISK incident).
    if [ -n "${TMP_DIR:-}" ]; then
        validate_tmp_dir "$TMP_DIR" || exit 1
        log_info "Temp directory (user override): $TMP_DIR"
        return 0
    fi

    local required_mb=100

    # Prefer install-dir SIBLING / home candidates FIRST, mirroring the running
    # app's C++ download-path probe. On devices where /tmp is a read-only
    # filesystem (e.g. OrangePi Zero3) the standard temp candidates below all
    # fail and the installer would fall back to /tmp and die at mkdir. The
    # install root partition is writable by definition (the binary lives there)
    # and usually the largest user partition.
    #
    # CRITICAL: these MUST stay OUTSIDE INSTALL_DIR (siblings), matching the
    # existing /data, /usr/data sibling pattern. TMP_DIR is rm -rf'd on cleanup
    # AND the --update flow (release.sh) does dotfile `rm -rf` inside INSTALL_DIR
    # and `mv INSTALL_DIR ...` on the atomic swap — a TMP_DIR under INSTALL_DIR
    # gets deleted/relocated out from under the extracted tree. So we use the
    # install dir's PARENT, never INSTALL_DIR itself. Dot-prefixed to stay
    # hidden and out of the way.
    # A platform may declare where its bulk storage actually is. On the K2 both
    # /opt (INSTALL_DIR's parent) and /usr/data live on a 240MB overlay while
    # the 27.5GB user partition is at /mnt/UDISK, so the generic "sibling of
    # INSTALL_DIR" rule below picks the small filesystem and a 60MB archive
    # fills it. Declared roots are probed first; they still go through the same
    # space/writability checks, so a unit that lacks the mount falls through.
    local candidates=""
    if [ -n "${TMP_DIR_PREFERRED:-}" ]; then
        # Name-guard it like any other TMP_DIR: whatever wins here is rm -rf'd
        # on exit, so a declared root that is a bare mountpoint (the /mnt/UDISK
        # incident shape) must be dropped rather than staged into.
        if _user_dir_name_ok "$TMP_DIR_PREFERRED" '*helixscreen-install*' '.helix-update-staging'; then
            candidates="$TMP_DIR_PREFERRED"
        else
            log_warn "Ignoring TMP_DIR_PREFERRED='$TMP_DIR_PREFERRED' (not an installer scratch dir name)"
        fi
    fi
    if [ -n "${INSTALL_DIR:-}" ]; then
        local install_parent
        install_parent=$(dirname "$INSTALL_DIR")
        candidates="$candidates $install_parent/.helixscreen-install"
    fi
    if [ -n "${HOME:-}" ]; then
        candidates="$candidates $HOME/.helixscreen-install"
    fi

    # /user-resource is CC1's large writable partition (4GB+); /data is tiny (~118MB)
    # and often too full once the tarball is downloaded there, so list /user-resource
    # ahead of the smaller candidates.
    candidates="$candidates /user-resource/helixscreen-install /data/helixscreen-install /mnt/data/helixscreen-install /usr/data/helixscreen-install /var/tmp/helixscreen-install /tmp/helixscreen-install"

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
# Sets: INSTALL_DIR, INIT_SCRIPT_DEST, PREVIOUS_UI_SCRIPT, TMP_DIR, KLIPPER_CONFIG_DIR
set_install_paths() {
    local platform=$1
    local firmware=${2:-}

    # Start from "derive it from KLIPPER_HOME" every time; only the branches
    # below opt out. Without the reset a stale value from the environment (or
    # from an earlier call in the same shell) would leak into another platform.
    KLIPPER_CONFIG_DIR=""

    if [ "$platform" = "ad5m" ]; then
        # AD5M runs the helix-screen service as root on all three firmwares
        # (klipper_mod, zmod, forge_x). Pin KLIPPER_USER/HOME explicitly so
        # clean_helix_state_dirs and any other consumer don't fall back to
        # the empty default and silently skip the user-home sweep.
        KLIPPER_USER="root"
        KLIPPER_GROUP="root"
        KLIPPER_HOME="/root"
        case "$firmware" in
            klipper_mod)
                # v00.05 and earlier: /root/printer_software/helixscreen
                # v00.06+: /opt/helixscreen (no /root/printer_software dir)
                if [ -d "/root/printer_software" ]; then
                    INSTALL_DIR="/root/printer_software/helixscreen"
                else
                    INSTALL_DIR="/opt/helixscreen"
                fi
                INIT_SCRIPT_DEST="/etc/init.d/S80helixscreen"
                PREVIOUS_UI_SCRIPT="/etc/init.d/S80klipperscreen"
                log_info "AD5M firmware: Klipper Mod"
                log_info "Install directory: ${INSTALL_DIR}"
                ;;
            zmod)
                INSTALL_DIR="/srv/helixscreen"
                INIT_SCRIPT_DEST="/etc/init.d/S80helixscreen"
                PREVIOUS_UI_SCRIPT=""
                log_info "AD5M firmware: ZMOD"
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
        # FlashForge AD5X - uses ZMOD, /usr/data structure, runs as root
        KLIPPER_USER="root"
        KLIPPER_GROUP="root"
        KLIPPER_HOME="/root"
        INSTALL_DIR="/srv/helixscreen"
        INIT_SCRIPT_DEST="/etc/init.d/S80helixscreen"
        PREVIOUS_UI_SCRIPT=""
        log_info "Platform: FlashForge AD5X (ZMOD)"
        log_info "Install directory: ${INSTALL_DIR}"
    elif [ "$platform" = "k1" ]; then
        # Creality K1 series - uses /usr/data structure.
        # printer_data is at /usr/data/printer_data (squashfs /root is RO).
        # Without the explicit override, detect_klipper_user falls back to
        # KLIPPER_HOME=/root and setup_config_symlink skips with
        # "No printer_data/config found" on every K1 install.
        INSTALL_DIR="/usr/data/helixscreen"
        INIT_SCRIPT_DEST="/etc/init.d/S99helixscreen"
        KLIPPER_USER="root"
        KLIPPER_GROUP="root"
        KLIPPER_HOME="/usr/data"
        case "$firmware" in
            simple_af)
                PREVIOUS_UI_SCRIPT="/etc/init.d/S99guppyscreen"
                log_info "K1 firmware: Simple AF"
                ;;
            guilouz)
                PREVIOUS_UI_SCRIPT=""
                log_info "K1 firmware: Guilouz Helper Script"
                ;;
            stock_klipper|*)
                PREVIOUS_UI_SCRIPT=""
                log_info "K1 firmware: Stock Klipper"
                ;;
        esac
        log_info "Install directory: ${INSTALL_DIR}"
    elif [ "$platform" = "k2" ]; then
        # Creality K2 series - OpenWrt/Tina Linux, storage on /mnt/UDISK.
        # printer_data lives on /mnt/UDISK (squashfs /root has no space).
        # Some K2 bootstrap projects (k2-improvements) drop a symlink
        # /root/printer_data -> /mnt/UDISK/printer_data/, but the installer
        # runs before that bootstrap for many users — point KLIPPER_HOME at
        # the actual storage so config symlinks work on first install.
        INSTALL_DIR="/opt/helixscreen"
        INIT_SCRIPT_DEST="/etc/init.d/S99helixscreen"
        PREVIOUS_UI_SCRIPT=""
        KLIPPER_USER="root"
        KLIPPER_GROUP="root"
        KLIPPER_HOME="/mnt/UDISK"
        # /opt and /usr/data are both on the 240MB overlay; /mnt/UDISK is the
        # 27.5GB user partition. Staging the download anywhere else fills the
        # overlay (a unit was found with a leaked 60MB archive on it).
        TMP_DIR_PREFERRED="/mnt/UDISK/helixscreen-install"
        # Older builds cached thumbnails/gcode on the overlay via /usr/data;
        # the app now caches on /mnt/UDISK, so reclaim the old location.
        # Also reclaim scratch dirs leaked by pre-EXIT-trap installers: one
        # unit held a 60MB archive at /usr/data/helixscreen-install for months.
        # shellcheck disable=SC2034  # consumed by release.sh (stale cache reclaim)
        STALE_CACHE_DIRS="/usr/data/helixscreen/cache /usr/data/helixscreen-install /opt/.helixscreen-install"
        log_info "Platform: Creality K2 series"
        log_info "Install directory: ${INSTALL_DIR}"
    elif [ "$platform" = "cc1" ]; then
        # Elegoo Centauri Carbon running OpenCentauri COSMOS.
        # - / is read-only squashfs; /etc is an overlay on /data (writable).
        # - /user-resource is the 6.3 GB ext4 partition where user installs go.
        # - COSMOS provides gui-switcher: drop /etc/init.d/<name>, then
        #   `config-manager ui screen_ui <name>` + restart gui-switcher.
        # - There is NO printer_data directory anywhere on the device. Klipper
        #   and Moonraker config live in /etc/klipper/config (moonraker.conf,
        #   printer.cfg, and the vendor *-readonly/ include dirs), which is
        #   also the `config` root Moonraker advertises over /server/files/roots.
        INSTALL_DIR="/user-resource/helixscreen"
        INIT_SCRIPT_DEST="/etc/init.d/helixscreen"
        PREVIOUS_UI_SCRIPT=""
        KLIPPER_USER="root"
        KLIPPER_GROUP="root"
        KLIPPER_HOME="/root"
        # shellcheck disable=SC2034  # consumed by common.sh klipper_config_dir()
        KLIPPER_CONFIG_DIR="/etc/klipper/config"
        INIT_SYSTEM="sysv"
        log_info "Platform: Elegoo Centauri Carbon (COSMOS)"
        log_info "Install directory: ${INSTALL_DIR}"
    elif [ "$platform" = "snapmaker-u1" ]; then
        # Snapmaker U1: klipper runs as user 'lava', printer_data lives at
        # /home/lava/printer_data. HelixScreen itself runs as root, but
        # config symlinks must target the lava-owned tree so Mainsail/Fluidd
        # can read them. detect_klipper_user normally finds /home/lava, but
        # on a freshly-flashed U1 before klipper has ever run that path may
        # not exist yet — make it explicit so the installer is deterministic.
        INSTALL_DIR="/userdata/helixscreen"
        KLIPPER_USER="root"
        KLIPPER_GROUP="root"
        KLIPPER_HOME="/home/lava"
        # shellcheck disable=SC2034  # consumed by service.sh, common.sh, competing_uis.sh, uninstall.sh
        INIT_SYSTEM="sysv"
        # U1 does NOT install /etc/init.d/S99helixscreen. install_service_snapmaker_u1
        # patches the stock /etc/init.d/S99screen to delegate to helixscreen.init for
        # start|stop|restart only (no `status`). Point INIT_SCRIPT_DEST at the real,
        # full-featured init script so post-install commands and stop_service work.
        INIT_SCRIPT_DEST="${INSTALL_DIR}/config/helixscreen.init"
        log_info "Platform: Snapmaker U1"
        log_info "Install directory: ${INSTALL_DIR}"
    else
        # Pi and other platforms — detect klipper user, then auto-detect install dir
        # shellcheck disable=SC2034  # consumed by service.sh and common.sh
        INIT_SCRIPT_DEST="/etc/init.d/S90helixscreen"
        # shellcheck disable=SC2034  # consumed by competing_uis.sh and the uninstaller bundle
        PREVIOUS_UI_SCRIPT=""
        detect_klipper_user
        detect_pi_install_dir
    fi

    # Final gate on whatever INSTALL_DIR we ended up with. Every hard-coded
    # platform path above already satisfies it; this catches a future branch
    # (or an override route added later) that would hand a bare data directory
    # to the mv/rm -rf in release.sh and uninstall.sh.
    validate_install_dir "$INSTALL_DIR" || exit 1

    # Auto-detect best temp directory (all platforms)
    detect_tmp_dir
}

# User-editable config files that live in printer_data/config/helixscreen/.
# These are the files users may want to edit from Fluidd/Mainsail, and that
# the app writes to at runtime. All other files in INSTALL_DIR/config/ are
# static assets reinstalled on each update.
HELIX_USER_CONFIG_FILES="settings.json helixscreen.env .disabled_services tool_spools.json crash_history.json"

# User-writable config DIRECTORIES that live in printer_data/config/helixscreen/.
#
# Same contract as HELIX_USER_CONFIG_FILES, one level up: the install dir gets a
# symlink and the real directory lives in printer_data. This matters because
# Moonraker's type:web update_manager does shutil.rmtree(INSTALL_DIR) before
# extracting the new ZIP, and rmtree UNLINKS a symlink it meets as a child entry
# rather than descending through it — so the real data on the far side survives.
# A real directory here is destroyed outright, which is exactly how users lost
# their custom printer images, themes and printer-DB extensions.
#
#   custom_images     PrinterImageManager::init()  (config_dir + "/custom_images/")
#   themes            writable_path("themes")      (user themes; defaults ship
#                                                   separately in assets/config/themes/defaults)
#   printer_database.d  writable_path("printer_database.d")  (user DB extensions)
HELIX_USER_CONFIG_DIRS="custom_images themes printer_database.d"

# Default content for runtime-created state files that have no packaged default.
#
# The file loop below refuses to create a symlink when the file exists in neither
# place (a dangling symlink is worse than none). For a file the app only ever
# writes AFTER the install completes, that rule means the very first Moonraker
# update still destroys it — there was nothing to link on install day. Seeding an
# empty-but-valid file at setup time closes that window.
#
# Echoes the seed content and returns 0 for a known file; returns 1 otherwise,
# which leaves the "no dangling symlink" behaviour untouched for everything else.
_helix_config_file_seed() {
    case "$1" in
        crash_history.json) printf '[]\n' ;;   # CrashHistory::load() expects a JSON array
        *) return 1 ;;
    esac
}

# Copy every entry under SRC that is missing from DST, never overwriting.
#
# Used to fold the shipped contents of a config directory (a fresh ZIP's
# themes/, printer_database.d/README.md) into the user's printer_data copy
# before the install-dir original is replaced by a symlink. Sorting guarantees
# a parent is created before its children.
# Args: SRC DST [SUDO_PREFIX]
# Returns: 0 if every entry now exists at DST, 1 on the first failure.
_helix_merge_config_dir() {
    local src="$1" dst="$2" sudo_prefix="${3:-}"
    local rel

    [ -d "$src" ] || return 0

    while IFS= read -r rel; do
        [ -n "$rel" ] && [ "$rel" != "." ] || continue
        rel="${rel#./}"
        if [ -d "${src}/${rel}" ] && [ ! -L "${src}/${rel}" ]; then
            [ -d "${dst}/${rel}" ] || $sudo_prefix mkdir -p "${dst}/${rel}" 2>/dev/null || return 1
        elif [ ! -e "${dst}/${rel}" ] && [ ! -L "${dst}/${rel}" ]; then
            $sudo_prefix cp -pR "${src}/${rel}" "${dst}/${rel}" 2>/dev/null || return 1
        fi
    done <<EOF
$(cd "$src" 2>/dev/null && find . 2>/dev/null | LC_ALL=C sort)
EOF
    return 0
}

# True when DIR holds at least one entry (including dotfiles).
# Used to cross-check the find(1) walk: a walk that reports nothing about a
# directory that demonstrably has contents means the walk failed, and a failed
# walk must never be read as "everything copied".
_helix_dir_has_entries() {
    local d="$1" e
    [ -d "$d" ] || return 1
    for e in "$d"/* "$d"/.[!.]* "$d"/..?*; do
        [ -e "$e" ] || [ -L "$e" ] || continue
        return 0
    done
    return 1
}

# True when every entry under SRC has a counterpart under DST.
#
# The precondition for deleting SRC. Deliberately independent of
# _helix_merge_config_dir's own return code: a `cp` that reports success but
# produces nothing (full filesystem, silently-dropped write) must not be
# allowed to authorise the delete.
# Args: SRC DST
_helix_config_dir_fully_merged() {
    local src="$1" dst="$2" rel seen=""

    [ -d "$dst" ] || return 1

    while IFS= read -r rel; do
        [ -n "$rel" ] && [ "$rel" != "." ] || continue
        seen=1
        rel="${rel#./}"
        [ -e "${dst}/${rel}" ] || [ -L "${dst}/${rel}" ] || return 1
    done <<EOF
$(cd "$src" 2>/dev/null && find . 2>/dev/null)
EOF

    # The walk found nothing. That is only trustworthy if the directory really
    # is empty; otherwise find failed and we know nothing about what was copied.
    if [ -z "$seen" ] && _helix_dir_has_entries "$src"; then
        return 1
    fi
    return 0
}

# Delete a user-config directory in the install dir after its contents have been
# copied into printer_data. Last line of defence on the only rm -rf this feature
# performs — same shape as _safe_remove_tmp_dir in common.sh.
#
# Refuses unless ALL of:
#   - SRC is exactly "${install_config}/${name}" for a name in HELIX_USER_CONFIG_DIRS
#   - SRC is a real directory, not a symlink (a symlink is unlinked elsewhere)
#   - DST holds a counterpart for every entry under SRC
# Args: INSTALL_CONFIG NAME DST [SUDO_PREFIX]
# Returns: 0 on success, 1 when it refuses (caller must then keep the real dir).
_safe_remove_migrated_config_dir() {
    local install_config="$1" name="$2" dst="$3" sudo_prefix="${4:-}"
    local src="${install_config}/${name}"

    # Name must be one we manage — never a path handed in from elsewhere.
    local known="" d
    for d in $HELIX_USER_CONFIG_DIRS; do
        [ "$d" = "$name" ] && known=1
    done
    if [ -z "$known" ]; then
        log_warn "Refusing to remove '$src' (not a managed user config directory)"
        return 1
    fi

    # Absolute, traversal-free, and sitting directly under a config/ directory.
    case "$install_config" in
        /*) ;;
        *) log_warn "Refusing to remove '$src' (install config path is not absolute)"; return 1 ;;
    esac
    case "$install_config" in
        *..*) log_warn "Refusing to remove '$src' (install config path contains '..')"; return 1 ;;
    esac
    if [ "${install_config##*/}" != "config" ]; then
        log_warn "Refusing to remove '$src' (parent is not a config/ directory)"
        return 1
    fi

    if [ -L "$src" ] || [ ! -d "$src" ]; then
        log_warn "Refusing to remove '$src' (not a real directory)"
        return 1
    fi

    if ! _helix_config_dir_fully_merged "$src" "$dst"; then
        log_warn "Not every file under $src reached $dst — keeping the original"
        return 1
    fi

    $sudo_prefix rm -rf "$src" 2>/dev/null || return 1
    return 0
}

# Set up editable config directory in printer_data/config/helixscreen/.
#
# Creates a real directory (not a symlink) in printer_data/config/helixscreen/
# and symlinks user-editable files FROM the install dir INTO printer_data.
# This reverses the old approach (directory symlink into install dir) which
# was blocked by Moonraker's update_manager reserved path protection.
#
# Layout after setup:
#   ~/printer_data/config/helixscreen/           (real directory)
#   ~/printer_data/config/helixscreen/settings.json     (real file)
#   ~/helixscreen/config/settings.json → above          (symlink)
#
# On upgrade from old layout, migrates files from install dir to printer_data.
# Reads: KLIPPER_HOME / KLIPPER_CONFIG_DIR (via klipper_config_dir), INSTALL_DIR
setup_config_symlink() {
    # Only proceed if we have a Klipper config dir and an install directory
    local pd_config
    pd_config="$(klipper_config_dir)"
    if [ -z "$pd_config" ] || [ -z "${INSTALL_DIR:-}" ]; then
        return 0
    fi

    local pd_helix="${pd_config}/helixscreen"
    local install_config="${INSTALL_DIR}/config"

    # Skip if the Klipper config dir doesn't exist
    if [ ! -d "$pd_config" ]; then
        log_info "No Klipper config dir at $pd_config, skipping config symlink"
        return 0
    fi

    # Skip if install config directory doesn't exist
    if [ ! -d "$install_config" ]; then
        log_warn "Install config directory not found: $install_config"
        return 0
    fi

    # --- Migrate from old layout (directory symlink) ---
    if [ -L "$pd_helix" ]; then
        log_info "Migrating from old config symlink layout..."
        local old_target
        old_target=$(readlink "$pd_helix" 2>/dev/null || echo "")
        $(path_sudo "$pd_helix") rm -f "$pd_helix"
        log_info "Removed old directory symlink (was: $old_target)"
    fi

    # --- Create printer_data/config/helixscreen/ directory ---
    if [ ! -d "$pd_helix" ]; then
        if $(file_sudo "$pd_config") mkdir -p "$pd_helix" 2>/dev/null; then
            log_info "Created $pd_helix"
        else
            log_warn "Could not create $pd_helix (permission denied?)"
            return 0
        fi
    fi

    # --- Migrate helixconfig.json → settings.json in printer_data if needed ---
    if [ -f "${pd_helix}/helixconfig.json" ] && [ ! -f "${pd_helix}/settings.json" ]; then
        $(file_sudo "$pd_helix") mv "${pd_helix}/helixconfig.json" "${pd_helix}/settings.json"
        log_info "Migrated printer_data config: helixconfig.json → settings.json"
    fi
    # Remove old symlink if it exists (new one will be created by HELIX_USER_CONFIG_FILES loop)
    if [ -L "${install_config}/helixconfig.json" ]; then
        $(file_sudo "$install_config") rm "${install_config}/helixconfig.json"
        log_info "Removed old helixconfig.json symlink"
    fi

    # --- Set up per-file symlinks ---
    local file
    for file in $HELIX_USER_CONFIG_FILES; do
        local pd_file="${pd_helix}/${file}"
        local install_file="${install_config}/${file}"

        # If the install dir has a real file (not a symlink), move it to printer_data
        if [ -f "$install_file" ] && [ ! -L "$install_file" ]; then
            # A settings.json already in printer_data outlived the install dir,
            # so extract_release's "no user config existed" verdict was wrong:
            # the packaged config below is about to be dropped in favour of that
            # file.  Clear the marker rather than leave it to answer for some
            # later unversioned document at this path.
            if [ "$file" = "settings.json" ] && [ -f "$pd_file" ]; then
                $(file_sudo "$install_config") rm -f "${install_config}/.helix-fresh-install" 2>/dev/null
            fi
            if [ ! -f "$pd_file" ]; then
                # Move the file to printer_data — check for success before removing original.
                # A silent cp failure followed by rm would permanently destroy the user's config.
                if $(file_sudo "$pd_helix") cp "$install_file" "$pd_file" 2>/dev/null; then
                    log_info "Migrated $file to printer_data"
                else
                    log_warn "Could not migrate $file to printer_data — keeping in install dir"
                    continue  # Skip symlink creation; leave the real file in place
                fi
            fi
            # Remove the original so we can create the symlink
            $(file_sudo "$install_config") rm -f "$install_file" 2>/dev/null
        fi

        # If pd_file still doesn't exist (e.g. runtime-created file like tool_spools.json
        # or .disabled_services that isn't packaged and hasn't been written yet), seed it
        # when we know a safe default, else skip symlink creation.  A dangling symlink is
        # worse than no symlink: reads return ENOENT and writes may fail depending on the
        # OS/filesystem.  The app will create the file at pd_file naturally;
        # setup_config_symlink will wire it up correctly on the next update or reinstall.
        if [ ! -f "$pd_file" ] && [ ! -L "$pd_file" ]; then
            local seed
            if seed=$(_helix_config_file_seed "$file"); then
                if printf '%s' "$seed" | $(file_sudo "$pd_helix") tee "$pd_file" >/dev/null 2>&1; then
                    log_info "Seeded $file in printer_data"
                else
                    log_warn "Could not seed $file in printer_data"
                fi
            fi
        fi
        if [ ! -f "$pd_file" ] && [ ! -L "$pd_file" ]; then
            # Only skip if install_file also doesn't exist or isn't already a symlink
            if [ ! -L "$install_file" ]; then
                continue
            fi
        fi

        # Create symlink: install_dir/config/file → printer_data/config/helixscreen/file
        if [ -L "$install_file" ]; then
            local current_target
            current_target=$(readlink "$install_file" 2>/dev/null || echo "")
            if [ "$current_target" = "$pd_file" ]; then
                continue  # Already correct
            fi
            $(file_sudo "$install_config") rm -f "$install_file" 2>/dev/null
        fi

        if $(file_sudo "$install_config") ln -s "$pd_file" "$install_file" 2>/dev/null; then
            : # Symlink created
        else
            log_warn "Could not create symlink for $file"
        fi
    done

    # --- Set up per-directory symlinks ---
    #
    # Migration order matters: copy first, verify, only then delete. Every exit
    # before the delete leaves the user's real directory untouched in the install
    # dir, which is the pre-fix status quo — worse than protected, never lost.
    local dir
    for dir in $HELIX_USER_CONFIG_DIRS; do
        local pd_dir="${pd_helix}/${dir}"
        local install_dir_path="${install_config}/${dir}"

        # Already a symlink: fix it if it points somewhere stale, else leave it.
        if [ -L "$install_dir_path" ]; then
            local current_dir_target
            current_dir_target=$(readlink "$install_dir_path" 2>/dev/null || echo "")
            if [ "$current_dir_target" = "$pd_dir" ]; then
                # Re-run of a completed migration. The target may still be missing
                # if printer_data was reset out from under us — recreate it so the
                # link is never dangling.
                [ -d "$pd_dir" ] || $(file_sudo "$pd_helix") mkdir -p "$pd_dir" 2>/dev/null || true
                continue
            fi
            $(file_sudo "$install_config") rm -f "$install_dir_path" 2>/dev/null
        fi

        # Ensure the printer_data side exists before anything is copied into it.
        if [ ! -d "$pd_dir" ]; then
            if ! $(file_sudo "$pd_helix") mkdir -p "$pd_dir" 2>/dev/null; then
                log_warn "Could not create $pd_dir — leaving $dir in the install dir"
                continue
            fi
        fi

        # Real directory in the install dir: fold its contents into printer_data
        # (never overwriting the user's copy), verify, then replace with a symlink.
        if [ -d "$install_dir_path" ] && [ ! -L "$install_dir_path" ]; then
            if ! _helix_merge_config_dir "$install_dir_path" "$pd_dir" "$(file_sudo "$pd_helix")"; then
                log_warn "Could not migrate $dir/ to printer_data — keeping it in the install dir"
                continue
            fi
            if ! _safe_remove_migrated_config_dir "$install_config" "$dir" "$pd_dir" \
                    "$(file_sudo "$install_config")"; then
                continue   # helper already logged why; original left intact
            fi
            log_info "Migrated $dir/ to printer_data"
        elif [ -e "$install_dir_path" ]; then
            # A plain file where we expect a directory. Not ours to delete.
            log_warn "$install_dir_path is not a directory — skipping"
            continue
        fi

        if $(file_sudo "$install_config") ln -s "$pd_dir" "$install_dir_path" 2>/dev/null; then
            : # Symlink created
        else
            log_warn "Could not create symlink for $dir/"
        fi
    done

    log_success "Config directory: $pd_helix"
    log_info "You can now edit HelixScreen config from Mainsail/Fluidd"
    return 0
}

# Remove config symlinks and optionally the printer_data directory.
# Called during uninstall. Preserves user files in printer_data.
# Reads: KLIPPER_HOME / KLIPPER_CONFIG_DIR (via klipper_config_dir), INSTALL_DIR
remove_config_symlink() {
    local pd_config
    pd_config="$(klipper_config_dir)"
    if [ -z "$pd_config" ] || [ -z "${INSTALL_DIR:-}" ]; then
        return 0
    fi

    local pd_helix="${pd_config}/helixscreen"
    local install_config="${INSTALL_DIR}/config"

    # Remove per-file symlinks from install dir
    local file
    for file in $HELIX_USER_CONFIG_FILES; do
        local install_file="${install_config}/${file}"
        if [ -L "$install_file" ]; then
            rm -f "$install_file" 2>/dev/null
        fi
    done

    # Remove per-directory symlinks. rm -f on a symlink-to-directory unlinks the
    # link itself, so the user's real data in printer_data is untouched.
    local dir
    for dir in $HELIX_USER_CONFIG_DIRS; do
        local install_dir_path="${install_config}/${dir}"
        if [ -L "$install_dir_path" ]; then
            rm -f "$install_dir_path" 2>/dev/null
        fi
    done

    # Remove old-style directory symlink if present
    if [ -L "$pd_helix" ]; then
        rm -f "$pd_helix" 2>/dev/null
        log_info "Removed old config directory symlink"
    fi

    # Leave printer_data/config/helixscreen/ directory intact — user's config files
    if [ -d "$pd_helix" ]; then
        log_info "User config preserved at: $pd_helix"
    fi

    return 0
}

# Fix known Klipper config issues on AD5M KlipperMod printers.
# The AD5M ships with screw_thread: CW-M4 but the correct value is CCW-M4.
# This causes SCREWS_TILT_CALCULATE to output inverted CW/CCW directions,
# leading users to tighten when they should loosen (and vice versa).
# Only applies to KlipperMod (Forge-X and ZMOD don't ship screws_tilt_adjust).
fix_ad5m_klipper_config() {
    # Guard: only run on AD5M KlipperMod (uses lowercase $platform from main())
    if [ "${platform:-}" != "ad5m" ] || [ "${AD5M_FIRMWARE:-}" != "klipper_mod" ]; then
        return 0
    fi

    log_info "Checking AD5M Klipper config for known issues..."

    local config_dir="${KLIPPER_HOME}/printer_data/config"
    if [ ! -d "$config_dir" ]; then
        log_info "No printer_data/config found, skipping Klipper config fixup"
        return 0
    fi

    # Find config file with screws_tilt_adjust containing exactly "CW-M4"
    # (not "CCW-M4" — grep -w ensures word boundary so CW-M4 won't match CCW-M4)
    local target_file=""
    for cfg_file in "$config_dir"/printer.base.cfg "$config_dir"/printer.cfg; do
        [ -f "$cfg_file" ] || continue
        if grep -q '\[screws_tilt_adjust\]' "$cfg_file" && \
           grep -qw 'CW-M4' "$cfg_file" && \
           ! grep -qw 'CCW-M4' "$cfg_file"; then
            target_file="$cfg_file"
            break
        fi
    done

    if [ -z "$target_file" ]; then
        log_info "AD5M screw_thread config OK (or not present)"
        return 0
    fi

    log_info "Found incorrect screw_thread in: $target_file"
    log_info "  CW-M4 → CCW-M4 (AD5M bed screws need reverse direction)"

    # Backup before modifying
    local backup_file="${target_file}.pre-helix"
    if [ ! -f "$backup_file" ]; then
        $SUDO cp "$target_file" "$backup_file"
        log_info "  Backup: $backup_file"
    fi

    # Fix: replace exactly "CW-M4" with "CCW-M4" on screw_thread lines only.
    # Anchored on line start (^) and end ($) to prevent matching CCW-M4.
    # Uses temp file instead of sed -i for portability (macOS vs BusyBox).
    local tmp_file="${target_file}.tmp"
    $SUDO sed 's/^\(screw_thread:[[:space:]]*\)CW-M4$/\1CCW-M4/' "$target_file" > "$tmp_file"
    $SUDO mv "$tmp_file" "$target_file"

    if grep -qw 'CCW-M4' "$target_file"; then
        log_success "Fixed AD5M screw_thread: CW-M4 → CCW-M4"
    else
        log_warn "Failed to fix screw_thread, restoring backup"
        $SUDO cp "$backup_file" "$target_file"
        return 1
    fi

    return 0
}
