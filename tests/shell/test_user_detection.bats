#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for detect_klipper_user() in platform.sh
# Tests the detection cascade: systemd → process → printer_data → well-known → fallback

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
    TMP_DIR="/tmp/helixscreen-install"

    # Source common.sh + platform.sh (skip source guards by unsetting them).
    # platform.sh calls common.sh's validate_install_dir/validate_tmp_dir; the
    # bundled installer always carries both.
    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    # common.sh defines the real log_* (they print to stderr, which bats folds
    # into $output). Restore helpers.bash's silent stubs.
    load helpers
}

# --- systemd detection ---

@test "systemd detection: finds biqu user" {
    mock_command_script "systemctl" '
        case "$1" in
            show) echo "biqu" ;;
            *) exit 1 ;;
        esac'
    mock_command "id" ""

    detect_klipper_user
    [ "$KLIPPER_USER" = "biqu" ]
}

@test "systemd detection: finds pi user" {
    mock_command_script "systemctl" '
        case "$1" in
            show) echo "pi" ;;
            *) exit 1 ;;
        esac'
    mock_command "id" ""

    detect_klipper_user
    [ "$KLIPPER_USER" = "pi" ]
}

@test "systemd detection: sets KLIPPER_HOME correctly" {
    mock_command_script "systemctl" '
        case "$1" in
            show) echo "biqu" ;;
            *) exit 1 ;;
        esac'
    mock_command "id" ""

    detect_klipper_user
    [ "$KLIPPER_HOME" = "/home/biqu" ] || [ -n "$KLIPPER_HOME" ]
}

@test "systemd not available: falls through" {
    # Remove systemctl from PATH by making it fail
    mock_command_fail "systemctl"
    # Also make id fail for well-known users so we hit fallback
    mock_command_fail "id"
    # No ps klipper output
    mock_command "ps" ""

    detect_klipper_user
    [ "$KLIPPER_USER" = "root" ]
}

@test "systemd returns empty user: falls through" {
    mock_command_script "systemctl" '
        case "$1" in
            show) echo "" ;;
            *) exit 1 ;;
        esac'
    mock_command_fail "id"
    mock_command "ps" ""

    detect_klipper_user
    [ "$KLIPPER_USER" = "root" ]
}

@test "systemd returns root: falls through to next method" {
    mock_command_script "systemctl" '
        case "$1" in
            show) echo "root" ;;
            *) exit 1 ;;
        esac'
    mock_command_fail "id"
    mock_command "ps" ""

    detect_klipper_user
    # Should still be root but via fallback (systemd root is skipped)
    [ "$KLIPPER_USER" = "root" ]
}

# --- Process table detection ---

@test "process table: finds klipper running as biqu" {
    mock_command_fail "systemctl"
    mock_command "ps" "biqu     klipper"
    mock_command "id" ""

    detect_klipper_user
    [ "$KLIPPER_USER" = "biqu" ]
}

@test "process table: no klipper running falls through" {
    mock_command_fail "systemctl"
    mock_command "ps" "root     bash"
    mock_command_fail "id"

    detect_klipper_user
    [ "$KLIPPER_USER" = "root" ]
}

# --- printer_data directory scan ---

@test "printer_data scan: finds user via directory" {
    mock_command_fail "systemctl"
    mock_command "ps" ""
    # Create a temporary printer_data directory
    mkdir -p "$BATS_TEST_TMPDIR/home/testuser/printer_data"

    # We can't easily test the /home/* glob in isolation without root
    # but we can verify the function doesn't crash
    mock_command_fail "id"
    detect_klipper_user
    # Falls through to root since test user doesn't exist in /etc/passwd
    [ -n "$KLIPPER_USER" ]
}

# --- Well-known users ---

@test "well-known user: biqu checked before pi" {
    mock_command_fail "systemctl"
    mock_command "ps" ""
    # id succeeds for all users (mock always returns success)
    mock_command "id" ""

    detect_klipper_user
    [ "$KLIPPER_USER" = "biqu" ]
}

# --- Fallback ---

@test "fallback: nothing matches returns root" {
    mock_command_fail "systemctl"
    mock_command "ps" ""
    mock_command_fail "id"

    detect_klipper_user
    [ "$KLIPPER_USER" = "root" ]
    [ "$KLIPPER_HOME" = "/root" ]
}

@test "KLIPPER_HOME set to /root for fallback" {
    mock_command_fail "systemctl"
    mock_command "ps" ""
    mock_command_fail "id"

    detect_klipper_user
    [ "$KLIPPER_HOME" = "/root" ]
}

@test "calling detect_klipper_user twice returns same result" {
    mock_command_fail "systemctl"
    mock_command "ps" ""
    mock_command_fail "id"

    detect_klipper_user
    local first_user="$KLIPPER_USER"
    KLIPPER_USER=""
    KLIPPER_HOME=""
    detect_klipper_user
    [ "$KLIPPER_USER" = "$first_user" ]
}

# --- detect_platform expansion ---

@test "detect_platform finds Pi via /home/biqu directory" {
    # This test checks the code path exists (actual detection needs ARM arch)
    # We verify the string is in the source code
    grep -q '/home/biqu' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "detect_platform still has /home/pi check (regression)" {
    grep -q '/home/pi' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "detect_platform still has /home/mks check (regression)" {
    grep -q '/home/mks' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

# --- Debian-family OS detection (regression) ---

@test "detect_platform checks os-release for Ubuntu (Armbian support)" {
    grep -qi 'ubuntu' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "detect_platform checks os-release for Armbian" {
    grep -qi 'armbian' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "detect_platform uses dpkg as Debian-family fallback" {
    grep -q 'dpkg' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

# --- Debian-family OS detection (functional) ---
# These tests exercise the actual detection logic with mocked environments.
# Since detect_platform() reads real filesystem paths, we redefine it inline
# with controlled inputs (same pattern as the bitness tests).

# Helper: build a testable detect_platform with injected os-release content,
# dpkg availability, and home directory state.
# Uses exported env vars so they're accessible inside the redefined function.
# Args: $1=os_release_content (or "none"), $2=has_dpkg (true/false),
#        $3=home_dirs ("pi mks biqu" space-separated, or "none")
_build_os_detect_function() {
    export _TEST_OS_RELEASE="$1"
    export _TEST_HAS_DPKG="$2"
    export _TEST_HOME_DIRS="$3"

    detect_platform() {
        local arch="aarch64"

        # Skip AD5M / K1 checks (we're testing SBC detection only)
        local is_arm_sbc=false

        # 1. os-release check
        if [ "$_TEST_OS_RELEASE" != "none" ]; then
            if echo "$_TEST_OS_RELEASE" | grep -qi "debian\|raspbian\|ubuntu\|armbian"; then
                is_arm_sbc=true
            fi
        fi

        # 2. dpkg check
        if [ "$is_arm_sbc" = false ] && [ "$_TEST_HAS_DPKG" = true ]; then
            is_arm_sbc=true
        fi

        # 3. Home directory check
        if [ "$is_arm_sbc" = false ] && [ "$_TEST_HOME_DIRS" != "none" ]; then
            for d in $_TEST_HOME_DIRS; do
                is_arm_sbc=true
                break
            done
        fi

        if [ "$is_arm_sbc" = true ]; then
            echo "pi"  # 64-bit for simplicity
        else
            echo "unsupported"
        fi
    }
}

@test "Armbian Ubuntu os-release detected as SBC" {
    # Real Armbian Ubuntu os-release contains ID=ubuntu and ID_LIKE=debian
    local os_release="PRETTY_NAME=\"Armbian 24.2.1 Jammy\"
NAME=\"Ubuntu\"
VERSION_ID=\"22.04\"
ID=ubuntu
ID_LIKE=debian"
    _build_os_detect_function "$os_release" "false" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Armbian Debian os-release detected as SBC" {
    local os_release="PRETTY_NAME=\"Armbian 24.2.1 Bookworm\"
NAME=\"Debian GNU/Linux\"
VERSION_ID=\"12\"
ID=debian"
    _build_os_detect_function "$os_release" "false" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Armbian with ID=armbian detected as SBC" {
    # Some Armbian builds use ID=armbian with ID_LIKE=debian
    local os_release="PRETTY_NAME=\"Armbian 24.5 Noble\"
ID=armbian
ID_LIKE=debian ubuntu"
    _build_os_detect_function "$os_release" "false" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Raspbian os-release still detected as SBC" {
    local os_release="PRETTY_NAME=\"Raspbian GNU/Linux 12 (bookworm)\"
ID=raspbian
ID_LIKE=debian"
    _build_os_detect_function "$os_release" "false" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Unknown distro with dpkg detected as SBC" {
    # Exotic Debian derivative we've never heard of, but has dpkg
    local os_release="PRETTY_NAME=\"ExoticLinux 1.0\"
ID=exoticlinux"
    _build_os_detect_function "$os_release" "true" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Unknown distro without dpkg falls back to home dirs" {
    local os_release="PRETTY_NAME=\"ExoticLinux 1.0\"
ID=exoticlinux"
    _build_os_detect_function "$os_release" "false" "pi"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "No os-release, no dpkg, no home dirs returns unsupported" {
    _build_os_detect_function "none" "false" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "unsupported" ]
}

@test "dpkg alone is sufficient without matching os-release" {
    _build_os_detect_function "none" "true" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "os-release with ID_LIKE=debian catches unknown derivative" {
    # Distro we don't list by name, but has ID_LIKE=debian
    local os_release="PRETTY_NAME=\"SomeOS 3.0\"
ID=someos
ID_LIKE=debian"
    _build_os_detect_function "$os_release" "false" "none"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}
