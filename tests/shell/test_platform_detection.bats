#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for detect_platform() in platform.sh
# Covers Pi 64-bit kernel with 32-bit userspace detection (the "not found" bug)

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Reset globals before each test
    KLIPPER_USER=""
    KLIPPER_HOME=""
    INIT_SCRIPT_DEST=""
    PREVIOUS_UI_SCRIPT=""
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    INSTALL_DIR="/opt/helixscreen"
    TMP_DIR=""

    # Source common.sh + platform.sh (skip source guards by unsetting them).
    # platform.sh calls common.sh's validate_install_dir/validate_tmp_dir; the
    # bundled installer always carries both.
    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    # common.sh defines the real log_* (they print to stderr, which bats folds
    # into $output). Restore helpers.bash's silent stubs.
    load helpers

    # Create a fake /etc/os-release indicating Debian (Pi-like system)
    mkdir -p "$BATS_TEST_TMPDIR/etc"
    echo 'ID=debian' > "$BATS_TEST_TMPDIR/etc/os-release"
    # Create a fake /home/pi directory to trigger is_pi=true
    mkdir -p "$BATS_TEST_TMPDIR/home/pi"
}

# Since detect_platform reads real /etc/os-release and /home/*, we
# redefine it with controlled inputs for testing. This helper captures
# the Pi userspace bitness detection logic from the real function.
_mock_pi_detect_platform() {
    local arch="$1"

    local userspace_bits
    userspace_bits=$(getconf LONG_BIT 2>/dev/null || echo "")
    if [ "$userspace_bits" = "64" ]; then
        echo "pi"
    elif [ "$userspace_bits" = "32" ]; then
        echo "pi32"
    else
        if file /usr/bin/id 2>/dev/null | grep -q "64-bit"; then
            echo "pi"
        elif file /usr/bin/id 2>/dev/null | grep -q "32-bit"; then
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
}

@test "Pi aarch64 kernel + 32-bit userspace (getconf) returns pi32" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command "getconf" "32"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "Pi aarch64 kernel + 64-bit userspace (getconf) returns pi" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command "getconf" "64"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Pi armv7l kernel + 32-bit userspace returns pi32" {
    detect_platform() { _mock_pi_detect_platform "armv7l"; }
    mock_command "getconf" "32"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "Pi aarch64 + getconf unavailable + file says 32-bit returns pi32" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command_fail "getconf"
    mock_command "file" "/usr/bin/id: ELF 32-bit LSB pie executable, ARM, EABI5"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "Pi aarch64 + getconf unavailable + file says 64-bit returns pi" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command_fail "getconf"
    mock_command "file" "/usr/bin/id: ELF 64-bit LSB pie executable, ARM aarch64"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Pi aarch64 + getconf unavailable + file unavailable falls back to kernel arch" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command_fail "getconf"
    mock_command_fail "file"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

# --- AD5X platform detection tests ---

# Mock detect_platform for AD5X: checks temp dir for AD5X indicators
_mock_ad5x_detect_platform() {
    local arch="$1"
    local mock_root="$BATS_TEST_TMPDIR"

    if [ "$arch" = "mips" ]; then
        if [ -d "$mock_root/usr/data" ] && { [ -d "$mock_root/usr/prog" ] || [ -f "$mock_root/ZMOD" ]; }; then
            echo "ad5x"
            return
        fi
    fi

    echo "unsupported"
}

@test "AD5X: MIPS arch + /usr/data + /usr/prog returns ad5x" {
    mkdir -p "$BATS_TEST_TMPDIR/usr/data" "$BATS_TEST_TMPDIR/usr/prog"
    detect_platform() { _mock_ad5x_detect_platform "mips"; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5x" ]
}

@test "AD5X: MIPS arch + /usr/data + /ZMOD file returns ad5x" {
    mkdir -p "$BATS_TEST_TMPDIR/usr/data"
    touch "$BATS_TEST_TMPDIR/ZMOD"
    detect_platform() { _mock_ad5x_detect_platform "mips"; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5x" ]
}

@test "AD5X: MIPS arch + /usr/data but NO /usr/prog and NO /ZMOD does NOT return ad5x" {
    mkdir -p "$BATS_TEST_TMPDIR/usr/data"
    detect_platform() { _mock_ad5x_detect_platform "mips"; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "unsupported" ]
}

@test "AD5X: non-MIPS arch + /usr/data + /usr/prog does NOT return ad5x" {
    detect_platform() {
        local arch="armv7l"

        if [ "$arch" = "mips" ]; then
            if [ -d "/usr/data" ] && { [ -d "/usr/prog" ] || [ -f "/ZMOD" ]; }; then
                echo "ad5x"
                return
            fi
        fi

        # Fall through to other detection
        echo "unsupported"
    }

    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "unsupported" ]
}

@test "AD5X: set_install_paths sets correct paths for ad5x" {
    # Mock detect_tmp_dir to avoid filesystem checks
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "ad5x"

    [ "$INSTALL_DIR" = "/srv/helixscreen" ]
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S80helixscreen" ]
    [ "$PREVIOUS_UI_SCRIPT" = "" ]
}

# --- AD5X Forge-X (mod-probed) detection tests ---
#
# A Forge-X AD5X carries none of the stock markers: no /ZMOD, no /usr/prog.
# The host profile's probe (env-overridable candidate roots, same convention
# as test_host_profile_guard.bats) is the evidence there, so these tests run
# the REAL detect_platform / detect_mod_flavor / mod_check_chroot_context
# against a sandboxed rig-shaped tree rather than mirroring their logic.

# Sandbox a Forge-X AD5X the way the rig looks from the host side: the mod's
# git tree plus the forge-x chroot, nothing else.
_setup_forgex_ad5x_sandbox() {
    install_gnu_stat_shim
    SANDBOX="$BATS_TEST_TMPDIR/forgex-root"
    mkdir -p "$SANDBOX/usr/data/config/mod/.shell" \
             "$SANDBOX/usr/data/.mod/.forge-x/usr/bin"
    touch "$SANDBOX/usr/data/config/mod/.shell/platform.sh"
    export HELIX_MOD_TREE_CANDIDATES="$SANDBOX/usr/data/config/mod"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/usr/data/.mod/.forge-x"
    unset _HELIX_HOST_PROFILE_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    host_profile_probe
}

# Sandbox a Z-Mod AD5X: chroot only, no forge-x tree, no /ZMOD (an absolute
# path the sandbox cannot fake — the probed chroot stands in for it).
_setup_zmod_ad5x_sandbox() {
    install_gnu_stat_shim
    SANDBOX="$BATS_TEST_TMPDIR/zmod-root"
    mkdir -p "$SANDBOX/usr/data/.mod/.zmod/usr/bin"
    export HELIX_MOD_TREE_CANDIDATES="$SANDBOX/no/mod/tree"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/usr/data/.mod/.zmod"
    unset _HELIX_HOST_PROFILE_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    host_profile_probe
}

@test "AD5X Forge-X: probe evidence alone returns ad5x from detect_platform" {
    _setup_forgex_ad5x_sandbox
    [ -n "$HOST_MOD_ROOT" ] || fail "probe did not find the sandbox tree"
    mock_command "uname" "mips"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5x" ]
}

@test "AD5X Forge-X: detect_mod_flavor returns forge_x from the probed tree" {
    _setup_forgex_ad5x_sandbox
    run detect_mod_flavor
    [ "$status" -eq 0 ]
    [ "$output" = "forge_x" ]
}

@test "AD5X Forge-X: mod_check_chroot_context passes (host-side install is the Forge-X design)" {
    _setup_forgex_ad5x_sandbox
    [ "$HOST_CHROOT_STATE" = "outside:$SANDBOX/usr/data/.mod/.forge-x" ] \
        || fail "probe did not see the forge-x chroot: $HOST_CHROOT_STATE"
    run mod_check_chroot_context
    [ "$status" -eq 0 ]
}

@test "ZMOD: mod_check_chroot_context refuses an install run outside the chroot" {
    _setup_zmod_ad5x_sandbox
    [ -n "$HOST_MOD_CHROOT" ] || fail "probe did not find the zmod chroot"
    # setup() silences the log_* stubs; make the refusal message assertable.
    log_error() { echo "ERROR: $*"; }
    run mod_check_chroot_context
    [ "$status" -ne 0 ]
    [[ "$output" == *"must run inside"* ]]
}

@test "AD5X Forge-X: the ad5x_check_chroot_context compat wrapper still answers" {
    _setup_forgex_ad5x_sandbox
    run ad5x_check_chroot_context
    [ "$status" -eq 0 ]
}

@test "detect_mod_flavor defaults to stock when no mod evidence exists" {
    # A bare host: no probed tree/chroot, no /ZMOD, no klipper_mod markers.
    HOST_MOD_ROOT=""
    HOST_MOD_CHROOT=""
    run detect_mod_flavor
    [ "$status" -eq 0 ]
    [ "$output" = "stock" ]
}

@test "detect_ad5m_firmware compat wrapper returns the mod flavor" {
    _setup_forgex_ad5x_sandbox
    run detect_ad5m_firmware
    [ "$status" -eq 0 ]
    [ "$output" = "forge_x" ]
}

@test "AD5X: detect_platform checks AD5X before K1 (ordering)" {
    # Verify the source code has AD5X check before K1 check
    # AD5X must be checked first because both have /usr/data, but only AD5X has /usr/prog
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    local ad5x_line k1_line
    ad5x_line=$(grep -n 'Check for FlashForge AD5X' "$platform_sh" | head -1 | cut -d: -f1)
    k1_line=$(grep -n 'Check for Creality K1' "$platform_sh" | head -1 | cut -d: -f1)
    [ -n "$ad5x_line" ]
    [ -n "$k1_line" ]
    [ "$ad5x_line" -lt "$k1_line" ]
}

# --- Source code regression tests ---

@test "platform.sh uses getconf LONG_BIT for userspace detection" {
    grep -q 'getconf LONG_BIT' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "platform.sh has file command fallback for userspace detection" {
    grep -q 'file /usr/bin/id' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "platform.sh warns about 64-bit kernel with 32-bit userspace" {
    grep -q '64-bit kernel with 32-bit userspace' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) also uses getconf LONG_BIT" {
    grep -q 'getconf LONG_BIT' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "platform.sh has AD5X detection with /usr/prog check" {
    grep -q '/usr/prog' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

# --- AD5M ZMOD firmware detection tests ---

# Mock detect_ad5m_firmware using temp dir paths
_mock_detect_ad5m_firmware() {
    local mock_root="$BATS_TEST_TMPDIR"
    if [ -f "$mock_root/ZMOD" ]; then
        echo "zmod"
        return
    fi
    if [ -d "$mock_root/root/printer_software" ] || [ -d "$mock_root/mnt/data/.klipper_mod" ]; then
        echo "klipper_mod"
        return
    fi
    echo "forge_x"
}

@test "AD5M: detect_ad5m_firmware returns zmod when /ZMOD exists" {
    detect_ad5m_firmware() { _mock_detect_ad5m_firmware; }
    touch "$BATS_TEST_TMPDIR/ZMOD"
    run detect_ad5m_firmware
    [ "$status" -eq 0 ]
    [ "$output" = "zmod" ]
}

@test "AD5M: detect_ad5m_firmware returns klipper_mod when no /ZMOD" {
    detect_ad5m_firmware() { _mock_detect_ad5m_firmware; }
    mkdir -p "$BATS_TEST_TMPDIR/root/printer_software"
    run detect_ad5m_firmware
    [ "$status" -eq 0 ]
    [ "$output" = "klipper_mod" ]
}

@test "AD5M: detect_ad5m_firmware prefers zmod over klipper_mod" {
    detect_ad5m_firmware() { _mock_detect_ad5m_firmware; }
    touch "$BATS_TEST_TMPDIR/ZMOD"
    mkdir -p "$BATS_TEST_TMPDIR/root/printer_software"
    run detect_ad5m_firmware
    [ "$status" -eq 0 ]
    [ "$output" = "zmod" ]
}

@test "AD5M: set_install_paths sets correct paths for zmod" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "ad5m" "zmod"

    [ "$INSTALL_DIR" = "/srv/helixscreen" ]
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S80helixscreen" ]
    [ "$PREVIOUS_UI_SCRIPT" = "" ]
}

@test "platform.sh checks /ZMOD before forge_x default" {
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    local zmod_line forge_x_line
    zmod_line=$(grep -n '/ZMOD' "$platform_sh" | grep -v 'AD5X\|mips\|usr/data' | head -1 | cut -d: -f1)
    forge_x_line=$(grep -n 'opt/config/mod/.root' "$platform_sh" | head -1 | cut -d: -f1)
    [ -n "$zmod_line" ]
    [ -n "$forge_x_line" ]
    [ "$zmod_line" -lt "$forge_x_line" ]
}

# --- One mod-flavor detector, called for ad5x too ---

@test "main.sh runs the mod-flavor detector for ad5m AND ad5x" {
    local main_sh="$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    grep -qF 'MOD_FLAVOR=$(detect_mod_flavor)' "$main_sh"
    grep -qF '[ "$platform" = "ad5m" ] || [ "$platform" = "ad5x" ]' "$main_sh"
}

@test "main.sh calls mod_check_chroot_context for ad5x" {
    grep -q 'mod_check_chroot_context' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

@test "platform.sh keeps the compat wrappers for the renamed mod functions" {
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    grep -q '^detect_ad5m_firmware()' "$platform_sh"
    grep -q '^ad5x_check_chroot_context()' "$platform_sh"
    # The wrappers delegate rather than fork the logic.
    grep -A1 '^detect_ad5m_firmware()' "$platform_sh" | grep -q 'detect_mod_flavor'
    grep -A1 '^ad5x_check_chroot_context()' "$platform_sh" | grep -q 'mod_check_chroot_context'
}

@test "install.sh (bundled) runs the mod-flavor detector for ad5x" {
    grep -q 'MOD_FLAVOR=$(detect_mod_flavor)' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q 'mod_check_chroot_context' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "uninstall.sh (bundled) still defines the compat wrappers" {
    local uninst="$WORKTREE_ROOT/scripts/uninstall.sh"
    grep -q '^detect_ad5m_firmware()' "$uninst"
    grep -q '^ad5x_check_chroot_context()' "$uninst"
}

# --- Creality K2 platform detection tests ---

# Mock detect_platform for K2 vs AD5M disambiguation (armv7l, kernel 5.4.61)
_mock_k2_detect_platform() {
    local mock_root="$BATS_TEST_TMPDIR"

    # K2 check (must come before AD5M)
    if [ -d "$mock_root/mnt/UDISK" ]; then
        if [ -f "$mock_root/etc/os-release" ] && grep -qi "openwrt\|tina" "$mock_root/etc/os-release" 2>/dev/null; then
            echo "k2"
            return
        fi
        if [ -d "$mock_root/mnt/UDISK/printer_data" ] || [ -d "$mock_root/mnt/UDISK/creality" ]; then
            echo "k2"
            return
        fi
    fi

    # AD5M fallback (both share armv7l + kernel 5.4.61)
    echo "ad5m"
}

@test "K2: armv7l + OpenWrt + /mnt/UDISK returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK"
    echo 'NAME="OpenWrt"' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: armv7l + /mnt/UDISK/printer_data (no os-release match) returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK/printer_data"
    echo 'ID=tina' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: detect_platform checks K2 before AD5M (ordering)" {
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    local k2_line ad5m_line
    k2_line=$(grep -n 'Check for Creality K2' "$platform_sh" | head -1 | cut -d: -f1)
    ad5m_line=$(grep -n 'Check for AD5M' "$platform_sh" | head -1 | cut -d: -f1)
    [ -n "$k2_line" ]
    [ -n "$ad5m_line" ]
    [ "$k2_line" -lt "$ad5m_line" ]
}

@test "K2: armv7l + Tina Linux + /mnt/UDISK returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK"
    echo 'NAME="Tina Linux"' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: armv7l + /mnt/UDISK/creality (no os-release match) returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK/creality"
    echo 'ID=buildroot' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: armv7l + empty /mnt/UDISK (no indicators) returns ad5m" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK"
    echo 'ID=buildroot' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m" ]
}

@test "K2: armv7l + kernel 5.4.61 WITHOUT /mnt/UDISK returns ad5m (not k2)" {
    # No /mnt/UDISK in temp dir -- falls through to AD5M
    detect_platform() { _mock_k2_detect_platform; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m" ]
}

@test "K2: set_install_paths sets correct paths for k2" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "k2"

    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S99helixscreen" ]
    [ "$KLIPPER_USER" = "root" ]
}

# Regression: K2 printer_data lives on /mnt/UDISK. Pre-2026-04-20 the K2
# branch hard-coded KLIPPER_HOME=/root (squashfs, no printer_data there),
# which caused setup_config_symlink to skip with "No printer_data/config
# found." Bootstrap projects (k2-improvements) paper over it with a
# /root/printer_data symlink, but the installer shouldn't depend on that.
@test "K2: set_install_paths points KLIPPER_HOME at /mnt/UDISK (printer_data lives there)" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "k2"

    [ "$KLIPPER_HOME" = "/mnt/UDISK" ]
}

# The bundled install.sh is what actually ships to users — make sure the
# fix isn't only present in the modular sources.
@test "install.sh (bundled) sets KLIPPER_HOME=/mnt/UDISK for k2" {
    grep -qE 'platform.*=.*"k2"' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q 'KLIPPER_HOME="/mnt/UDISK"' "$WORKTREE_ROOT/scripts/install.sh"
}

# Same regression class as K2: K1 printer_data is at /usr/data/printer_data,
# not /root. Without the fix, the fallback /root in detect_klipper_user
# made setup_config_symlink skip on K1 as well.
@test "K1: set_install_paths points KLIPPER_HOME at /usr/data" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "k1" "stock_klipper"

    [ "$KLIPPER_HOME" = "/usr/data" ]
    [ "$INSTALL_DIR" = "/usr/data/helixscreen" ]
}

@test "install.sh (bundled) sets KLIPPER_HOME=/usr/data for k1" {
    grep -qE 'platform.*=.*"k1"' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q 'KLIPPER_HOME="/usr/data"' "$WORKTREE_ROOT/scripts/install.sh"
}

# Snapmaker U1: detect_klipper_user would normally find /home/lava via
# filesystem scan, but that scan assumes klipper has already populated
# printer_data. On a freshly-flashed U1, detection can miss and fall back
# to /root — an explicit override makes install behavior deterministic.
@test "U1: set_install_paths points KLIPPER_HOME at /home/lava" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "snapmaker-u1"

    [ "$KLIPPER_HOME" = "/home/lava" ]
    [ "$INSTALL_DIR" = "/userdata/helixscreen" ]
}

@test "install.sh (bundled) sets KLIPPER_HOME=/home/lava for snapmaker-u1" {
    grep -qE 'platform.*=.*"snapmaker-u1"' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q 'KLIPPER_HOME="/home/lava"' "$WORKTREE_ROOT/scripts/install.sh"
}

# AD5M: /root/printer_data is the correct location (verified on hardware) —
# the fallback KLIPPER_HOME=/root already matches reality. Pin the expected
# behavior so no one "fixes" it later and regresses AD5M.
@test "AD5M forge_x: KLIPPER_HOME=/root is intentional (matches /root/printer_data)" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }
    # Simulate the detect_klipper_user fallback
    KLIPPER_HOME="/root"
    KLIPPER_USER="root"

    set_install_paths "ad5m" "forge_x"

    # AD5M forge_x branch intentionally does NOT override — the fallback is correct.
    [ "$KLIPPER_HOME" = "/root" ]
}

# --- Snapmaker U1 platform detection tests ---

# Mock detect_platform for U1: uses filesystem markers in temp dir
_mock_u1_detect_platform() {
    local mock_root="$BATS_TEST_TMPDIR"
    local u1_markers=0
    [ -d "$mock_root/home/lava" ] && u1_markers=$((u1_markers + 1))
    [ -d "$mock_root/home/lava/printer_data" ] && u1_markers=$((u1_markers + 1))
    [ -x "$mock_root/usr/bin/unisrv" ] && u1_markers=$((u1_markers + 1))
    [ -d "$mock_root/oem" ] && u1_markers=$((u1_markers + 1))
    if [ "$u1_markers" -ge 2 ]; then
        echo "snapmaker-u1"
        return
    fi
    echo "pi"
}

@test "U1: /home/lava + /oem (2 markers) returns snapmaker-u1" {
    detect_platform() { _mock_u1_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/home/lava"
    mkdir -p "$BATS_TEST_TMPDIR/oem"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "snapmaker-u1" ]
}

@test "U1: /home/lava + /home/lava/printer_data + /oem (3 markers) returns snapmaker-u1" {
    detect_platform() { _mock_u1_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/home/lava/printer_data"
    mkdir -p "$BATS_TEST_TMPDIR/oem"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "snapmaker-u1" ]
}

@test "U1: only /home/lava (1 marker) returns pi (not enough markers)" {
    detect_platform() { _mock_u1_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/home/lava"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "U1: /home/lava + unisrv executable (2 markers) returns snapmaker-u1" {
    detect_platform() { _mock_u1_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/home/lava"
    mkdir -p "$BATS_TEST_TMPDIR/usr/bin"
    touch "$BATS_TEST_TMPDIR/usr/bin/unisrv"
    chmod +x "$BATS_TEST_TMPDIR/usr/bin/unisrv"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "snapmaker-u1" ]
}

@test "U1: detect_platform checks U1 before Pi (ordering)" {
    # Verify the actual routing-emit order inside detect_platform(): the
    # `echo "snapmaker-u1"` branch must precede the first `echo "pi"` branch,
    # otherwise an aarch64 U1 falls through to the generic Pi tarball.
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    local u1_line pi_line
    u1_line=$(grep -n 'echo "snapmaker-u1"' "$platform_sh" | head -1 | cut -d: -f1)
    pi_line=$(grep -n 'echo "pi"' "$platform_sh" | head -1 | cut -d: -f1)
    [ -n "$u1_line" ]
    [ -n "$pi_line" ]
    [ "$u1_line" -lt "$pi_line" ]
}

@test "U1: set_install_paths sets correct paths for snapmaker-u1" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }
    set_install_paths "snapmaker-u1"
    [ "$INSTALL_DIR" = "/userdata/helixscreen" ]
    [ "$INIT_SYSTEM" = "sysv" ]
    [ "$KLIPPER_USER" = "root" ]
}

@test "platform.sh has /home/lava check for U1 detection" {
    grep -q '/home/lava' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

# ============================================================================
# Artillery M1 detection must NOT collide with QIDI Q2 (#1027).
# The M1 fingerprint is algo_app.service (Artillery's proprietary AI service).
# makerbase-client.services is the generic MKS-board LCD client shipped by
# other MakerBase-class SBCs too (QIDI Q2: hostname linaro-alip, user mks),
# so it must NOT be treated as a standalone M1 marker.
# ============================================================================

@test "platform.sh keys M1 detection on algo_app.service" {
    grep -q 'algo_app\.service' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "platform.sh does NOT treat makerbase-client as an M1 marker (#1027)" {
    # makerbase-client may still appear in comments, but must not gate detection
    # via a file existence check or a systemctl unit-file match.
    refute grep -qE '\[ -f [^]]*makerbase-client' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    ! grep -qE 'list-unit-files.*makerbase-client' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) does NOT treat makerbase-client as an M1 marker (#1027)" {
    refute grep -qE '\[ -f [^]]*makerbase-client' "$WORKTREE_ROOT/scripts/install.sh"
    ! grep -qE 'list-unit-files.*makerbase-client' "$WORKTREE_ROOT/scripts/install.sh"
}

# ============================================================================
# #1027 reopened: algo_app.service is NOT exclusive to the M1 — the QIDI Q2
# ships it too (field-confirmed: algo_app present, makerbase-client absent on a
# Q2). So M1 detection must be vetoed by the QIDI-class fingerprint (hostname
# linaro-alip + /home/mks) or the Q2 is still misdetected as m1.
# ============================================================================

# Mirror of the real M1 gate in detect_platform(); the algo_app probe and the
# QIDI veto are injected so we can exercise the branch logic without touching
# /etc/systemd, /etc/hostname, or /home.
_mock_m1_gate() {
    local is_m1=false
    if _has_algo_app; then
        is_m1=true
    fi
    if [ "$is_m1" = true ] && ! _is_qidi_class_sbc; then
        echo "m1"
        return 0
    fi
    echo "pi"
}

@test "M1 gate: algo_app present + NOT qidi-class → m1" {
    _has_algo_app() { return 0; }
    _is_qidi_class_sbc() { return 1; }
    run _mock_m1_gate
    [ "$status" -eq 0 ]
    [ "$output" = "m1" ]
}

@test "M1 gate: algo_app present + qidi-class (Q2) → pi, NOT m1 (#1027)" {
    _has_algo_app() { return 0; }
    _is_qidi_class_sbc() { return 0; }
    run _mock_m1_gate
    [ "$output" = "pi" ]
    [ "$output" != "m1" ]
}

@test "M1 gate: no algo_app → pi regardless of qidi signature" {
    _has_algo_app() { return 1; }
    _is_qidi_class_sbc() { return 1; }
    run _mock_m1_gate
    [ "$output" = "pi" ]
}

@test "platform.sh vetoes M1 detection with the QIDI-class fingerprint (#1027)" {
    grep -qE 'is_m1.*=.*true.*&&.*! _is_qidi_class_sbc' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) vetoes M1 detection with the QIDI-class fingerprint (#1027)" {
    grep -qE 'is_m1.*=.*true.*&&.*! _is_qidi_class_sbc' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "_is_qidi_class_sbc keys on hostname linaro-alip + /home/mks" {
    grep -q 'linaro-alip' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    grep -qE '_is_qidi_class_sbc\(\)' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

# QIDI firmware 01.01.02.01 (June 2026) renamed the Klipper user mks → qidi.
# _is_qidi_class_sbc must accept either home dir or the Q2 is misdetected as an
# Artillery M1 on the new firmware (#1047).
@test "_is_qidi_class_sbc also accepts /home/qidi (01.01.02 user rename, #1047)" {
    grep -qE '/home/mks.*\|\|.*/home/qidi' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) _is_qidi_class_sbc accepts /home/qidi (#1047)" {
    grep -qE '/home/mks.*\|\|.*/home/qidi' "$WORKTREE_ROOT/scripts/install.sh"
}

# ============================================================================
# get_download_platform() — maps platforms without dedicated release artifacts
# to a donor platform whose binary is ABI-compatible. Regression coverage for
# prestonbrown/helixscreen#970 + #971 (Artillery M1 had no helixscreen-m1.zip,
# so installer/updater hit 404s on every URL candidate).
# ============================================================================

@test "get_download_platform: pi → pi (identity)" {
    run get_download_platform "pi"
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "get_download_platform: pi32 → pi32 (identity)" {
    run get_download_platform "pi32"
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "get_download_platform: ad5m → ad5m (identity)" {
    run get_download_platform "ad5m"
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m" ]
}

@test "get_download_platform: k1 → k1 (identity)" {
    run get_download_platform "k1"
    [ "$status" -eq 0 ]
    [ "$output" = "k1" ]
}

@test "get_download_platform: k2 → k2 (identity)" {
    run get_download_platform "k2"
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "get_download_platform: snapmaker-u1 → snapmaker-u1 (identity)" {
    run get_download_platform "snapmaker-u1"
    [ "$status" -eq 0 ]
    [ "$output" = "snapmaker-u1" ]
}

@test "get_download_platform: m1 with 64-bit userspace → pi" {
    mock_command "getconf" "64"
    run get_download_platform "m1"
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "get_download_platform: m1 with 32-bit userspace → pi32" {
    mock_command "getconf" "32"
    run get_download_platform "m1"
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "get_download_platform: m1 with missing getconf defaults to pi (64-bit)" {
    # getconf absent → empty userspace_bits → falls into the 'else' branch
    # (treat as 64-bit). Artillery M1 Pro ships aarch64 userspace in practice.
    mock_command_fail "getconf"
    run get_download_platform "m1"
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "platform.sh exports get_download_platform" {
    grep -q '^get_download_platform()' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) has get_download_platform" {
    grep -q 'get_download_platform' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "main.sh passes download_platform (not platform) to download_release" {
    grep -q 'download_release "$version" "$download_platform"' \
        "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

@test "main.sh passes download_platform (not platform) to get_latest_version" {
    grep -q 'get_latest_version "$download_platform"' \
        "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

@test "install.sh (bundled) passes download_platform to download_release" {
    grep -q 'download_release "$version" "$download_platform"' \
        "$WORKTREE_ROOT/scripts/install.sh"
}

@test "release_info.json picks pi/pi32 for m1 (not helixscreen-m1.zip)" {
    # Regression guard: m1 (Artillery, a Debian SBC) self-updates to the pi/pi32
    # binary, never a non-existent helixscreen-m1.zip (which 404s on Moonraker
    # update). The asset is resolved by helix_self_update_asset() in platform.sh,
    # the single source of truth shared by moonraker.sh and mk/cross.mk.
    refute grep -q 'helixscreen-m1\.zip' "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"
    refute grep -q 'helixscreen-m1\.zip' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    # The m1 dispatch must resolve to pi or pi32 (by userspace bitness).
    local asset
    asset=$(helix_self_update_asset m1)
    [ "$asset" = "helixscreen-pi.zip" ] || [ "$asset" = "helixscreen-pi32.zip" ]

    # And the explicit m1 branch must still exist in get_download_platform() so
    # future readers don't strip the pi/pi32 dispatch as dead code.
    grep -q '^[[:space:]]*m1)' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}
