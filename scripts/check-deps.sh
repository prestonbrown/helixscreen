#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - Modular Dependency Checker
#
# Usage:
#   ./scripts/check-deps.sh           # Full check (desktop development)
#   ./scripts/check-deps.sh --minimal # Essential deps only (cross-compilation)
#
# Environment Variables:
#   CC, CXX     - Override compiler commands for cross-compilation check
#   LVGL_DIR    - Override LVGL submodule path (default: lib/lvgl)
#   SPDLOG_DIR  - Override spdlog submodule path (default: lib/spdlog)
#   LIBHV_DIR   - Override libhv submodule path (default: lib/libhv)
#   WPA_DIR     - Override wpa_supplicant submodule path (default: lib/wpa_supplicant)

set -e

# =============================================================================
# Colors and Formatting
# =============================================================================

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RESET='\033[0m'
BOLD='\033[1m'

# =============================================================================
# State Tracking
# =============================================================================

ERRORS=0
WARNINGS=0
MISSING=""

# =============================================================================
# Paths (can be overridden via environment)
# =============================================================================

LVGL_DIR="${LVGL_DIR:-lib/lvgl}"
SPDLOG_DIR="${SPDLOG_DIR:-lib/spdlog}"
LIBHV_DIR="${LIBHV_DIR:-lib/libhv}"
WPA_DIR="${WPA_DIR:-lib/wpa_supplicant}"
VENV="${VENV:-.venv}"

# =============================================================================
# Utility Functions
# =============================================================================

ok() {
    echo -e "${GREEN}✓${RESET} $1"
}

fail() {
    local msg="$1"
    local dep_name="$2"
    echo -e "${RED}✗${RESET} $msg"
    ERRORS=$((ERRORS + 1))
    [ -n "$dep_name" ] && MISSING="$MISSING $dep_name"
}

warn() {
    echo -e "${YELLOW}⚠${RESET} $1"
    WARNINGS=$((WARNINGS + 1))
}

skip() {
    echo -e "${YELLOW}⊘${RESET} $1 (skipped)"
}

info() {
    echo -e "${CYAN}ℹ${RESET} $1"
}

hint() {
    # Print install hints for different package managers
    local pkg="$1"
    local brew_pkg="$2"
    local apt_pkg="$3"
    local dnf_pkg="$4"

    echo -e "  Install: ${YELLOW}brew install ${brew_pkg:-$pkg}${RESET} (macOS)"
    echo -e "         ${YELLOW}sudo apt install ${apt_pkg:-$pkg}${RESET} (Debian/Ubuntu)"
    echo -e "         ${YELLOW}sudo dnf install ${dnf_pkg:-$pkg}${RESET} (Fedora/RHEL)"
}

# Check if a command exists and print version
check_command() {
    local cmd="$1"
    local name="${2:-$1}"
    local show_hint="${3:-}"

    if command -v "$cmd" >/dev/null 2>&1; then
        local version
        version=$("$cmd" --version 2>&1 | head -n1)
        ok "$name found: $version"
        return 0
    else
        fail "$name not found" "$name"
        [ -n "$show_hint" ] && eval "$show_hint"
        return 1
    fi
}

# Check if a pkg-config package exists
check_pkg() {
    local pkg="$1"
    local name="${2:-$1}"

    if pkg-config --exists "$pkg" 2>/dev/null; then
        local version
        version=$(pkg-config --modversion "$pkg" 2>/dev/null || echo "unknown")
        ok "$name found: $version"
        return 0
    else
        return 1
    fi
}

# Check if a directory exists (for submodules)
check_dir() {
    local dir="$1"
    local name="$2"
    local required="${3:-true}"

    if [ -d "$dir" ]; then
        ok "$name found: $dir"
        return 0
    else
        if [ "$required" = "true" ]; then
            fail "$name not found" "$name"
            echo -e "  Run: ${YELLOW}git submodule update --init --recursive${RESET}"
        else
            warn "$name not found (optional)"
        fi
        return 1
    fi
}

# =============================================================================
# Dependency Category Checks
# =============================================================================

check_essential() {
    echo ""
    echo -e "${BOLD}Essential Build Dependencies${RESET}"

    # C Compiler
    # Handle ccache-wrapped compilers like "ccache cc" - extract actual compiler
    local cc_cmd="${CC:-cc}"
    local cc_actual="${cc_cmd##* }"  # Get last word (handles "ccache cc" -> "cc")
    if command -v "$cc_actual" >/dev/null 2>&1; then
        ok "C compiler found: $($cc_cmd --version 2>&1 | head -n1)"
    else
        fail "C compiler ($cc_cmd) not found" "cc"
        hint "clang" "llvm" "clang" "clang"
    fi

    # C++ Compiler
    # Handle ccache-wrapped compilers like "ccache c++" - extract actual compiler
    local cxx_cmd="${CXX:-c++}"
    local cxx_actual="${cxx_cmd##* }"  # Get last word (handles "ccache c++" -> "c++")
    if command -v "$cxx_actual" >/dev/null 2>&1; then
        ok "C++ compiler found: $($cxx_cmd --version 2>&1 | head -n1)"
    else
        fail "C++ compiler ($cxx_cmd) not found" "c++"
        hint "clang" "llvm" "g++" "gcc-c++"
    fi

    # Make
    check_command "make" "make" 'hint "make" "make" "make" "make"'

    # pkg-config
    check_command "pkg-config" "pkg-config" 'hint "pkg-config" "pkg-config" "pkg-config" "pkgconfig"'
}

check_submodules() {
    echo ""
    echo -e "${BOLD}Submodules${RESET}"

    check_dir "$LVGL_DIR/src" "LVGL" true
    check_dir "$SPDLOG_DIR/include" "spdlog" true
    check_dir "$LIBHV_DIR" "libhv" true

    # wpa_supplicant only needed on Linux
    if [ "$(uname -s)" != "Darwin" ]; then
        check_dir "$WPA_DIR/wpa_supplicant" "wpa_supplicant" true
    fi
}

check_libraries() {
    echo ""
    echo -e "${BOLD}Libraries${RESET}"

    # fmt (optional - spdlog uses bundled headers if not found)
    if check_pkg fmt; then
        :  # ok already printed
    else
        skip "fmt library not found (optional - spdlog uses bundled headers)"
    fi

    # OpenSSL (Linux only - macOS uses system)
    # Skip for embedded targets with ENABLE_SSL=no (local Moonraker doesn't need TLS)
    if [ "$(uname -s)" != "Darwin" ]; then
        if [ "$ENABLE_SSL" = "no" ]; then
            ok "openssl: disabled (ENABLE_SSL=no)"
        elif check_pkg openssl || check_pkg libssl; then
            :  # ok already printed
        elif [ -f "/usr/include/openssl/ssl.h" ] || [ -f "/usr/local/include/openssl/ssl.h" ]; then
            ok "OpenSSL found (system headers)"
        elif [ -n "$CC" ] && command -v "${CC##* }" >/dev/null 2>&1; then
            # Cross-compilation: check the compiler's sysroot for OpenSSL headers
            # ARM standalone toolchains: gcc -print-sysroot returns <prefix>/<triple>/libc
            # but OpenSSL installs to <prefix>/<triple>/include (one level up from libc)
            # CC may have a ccache prefix ("ccache arm-linux-gnueabihf-gcc"); strip it.
            CC_BIN="${CC##* }"
            SSL_FOUND=0
            SYSROOT=$("$CC_BIN" -print-sysroot 2>/dev/null || true)
            if [ -n "$SYSROOT" ] && [ -f "$SYSROOT/usr/include/openssl/ssl.h" ]; then
                ok "OpenSSL found (cross-compiler sysroot/usr)"
                SSL_FOUND=1
            elif [ -n "$SYSROOT" ] && [ -f "$SYSROOT/include/openssl/ssl.h" ]; then
                ok "OpenSSL found (cross-compiler sysroot)"
                SSL_FOUND=1
            elif [ -n "$SYSROOT" ]; then
                TOOLCHAIN_PREFIX=$(dirname "$SYSROOT")
                if [ -f "$TOOLCHAIN_PREFIX/include/openssl/ssl.h" ]; then
                    ok "OpenSSL found (toolchain: $TOOLCHAIN_PREFIX)"
                    SSL_FOUND=1
                elif [ -f "$TOOLCHAIN_PREFIX/sysroot/usr/include/openssl/ssl.h" ]; then
                    ok "OpenSSL found (toolchain sysroot/usr: $TOOLCHAIN_PREFIX)"
                    SSL_FOUND=1
                fi
            fi
            if [ $SSL_FOUND -eq 0 ]; then
                fail "OpenSSL development libraries not found" "openssl"
                hint "openssl" "openssl" "libssl-dev" "openssl-devel"
            fi
        else
            fail "OpenSSL development libraries not found" "openssl"
            hint "openssl" "openssl" "libssl-dev" "openssl-devel"
        fi
    fi

    # libhv status
    if check_pkg libhv 2>/dev/null; then
        :  # system version
    elif [ -f "$LIBHV_DIR/lib/libhv.a" ]; then
        ok "libhv: Using submodule version"
    else
        info "libhv: Will be built from submodule"
    fi

    # libnl (Linux only - required for WiFi backend)
    # Can be satisfied by system package OR submodule
    if [ "$(uname -s)" != "Darwin" ]; then
        if check_pkg libnl-3.0; then
            :  # ok already printed
        elif [ -d "lib/libnl" ] && [ -f "lib/libnl/configure.ac" ]; then
            ok "libnl: Using submodule (will build from source)"
        else
            fail "libnl-3.0 not found (required for WiFi backend)" "libnl"
            hint "libnl" "" "libnl-3-dev libnl-genl-3-dev" "libnl3-devel"
        fi
    fi

    # libusb (USB printer detection). The native Linux build links -lusb-1.0
    # unconditionally, so the headers are required there. macOS detects it via
    # pkg-config and compiles the detector out when absent. Cross targets get it
    # from their Docker image or build without it (ad5m, cc1), so --minimal
    # skips it entirely.
    if [ $MINIMAL -eq 0 ]; then
        if [ "$(uname -s)" = "Darwin" ]; then
            if ! check_pkg libusb-1.0 "libusb"; then
                skip "libusb not found (optional on macOS - USB printer detection compiled out)"
            fi
        elif ! check_pkg libusb-1.0 "libusb"; then
            fail "libusb-1.0 development headers not found (the Linux build links -lusb-1.0)" "libusb"
            hint "libusb" "libusb" "libusb-1.0-0-dev" "libusb1-devel"
        fi
    fi

    # ALSA (sound output on Linux). Optional at build time, but without the
    # headers the ALSA backend is compiled out and the app runs silent, which
    # reads as a sound bug rather than a missing package - so warn, not skip.
    if [ $MINIMAL -eq 0 ] && [ "$(uname -s)" = "Linux" ]; then
        if ! check_pkg alsa "ALSA"; then
            warn "ALSA development headers not found (sound output backend will be compiled out)"
            echo -e "  Install: ${YELLOW}sudo apt install libasound2-dev${RESET} (Debian/Ubuntu)"
            echo -e "           ${YELLOW}sudo dnf install alsa-lib-devel${RESET} (Fedora/RHEL)"
        fi
    fi
}

check_desktop_tools() {
    echo ""
    echo -e "${BOLD}Desktop Development Tools${RESET}"

    # SDL2
    if command -v sdl2-config >/dev/null 2>&1; then
        ok "SDL2 found: $(sdl2-config --version)"
    else
        info "SDL2 not found - will build from submodule"
        # cmake is required for SDL2 build
        if ! check_command "cmake" "cmake (for SDL2 build)"; then
            hint "cmake" "cmake" "cmake" "cmake"
        fi
    fi

    # macOS version check
    if [ "$(uname -s)" = "Darwin" ]; then
        local macos_version
        macos_version=$(sw_vers -productVersion 2>/dev/null | cut -d. -f1-2)
        local major minor
        major=$(echo "$macos_version" | cut -d. -f1)
        minor=$(echo "$macos_version" | cut -d. -f2)

        # Require 10.15+ for modern CoreWLAN/CoreLocation APIs
        if [ "$major" -lt 10 ] || { [ "$major" -eq 10 ] && [ "$minor" -lt 15 ]; }; then
            fail "macOS $macos_version is too old (need 10.15+)" "macos"
            echo "  Reason: CoreWLAN/CoreLocation modern APIs for WiFi support"
        else
            ok "macOS version $macos_version >= 10.15"
        fi
    fi

    # npm/node (for font generation)
    if command -v npm >/dev/null 2>&1; then
        ok "npm found: $(npm --version)"
        if [ -f "node_modules/.bin/lv_font_conv" ]; then
            ok "lv_font_conv installed"
        else
            warn "lv_font_conv not installed"
            echo -e "  Run: ${YELLOW}npm install${RESET}"
        fi
    else
        fail "npm not found (needed for font generation)" "npm"
        hint "npm" "node" "npm" "npm"
    fi

    # Python (for image tools)
    if command -v python3 >/dev/null 2>&1; then
        ok "python3 found: $(python3 --version)"

        # Check venv support
        if ! python3 -c "import venv, ensurepip" 2>/dev/null; then
            warn "python3-venv not installed"
            echo -e "  Install: ${YELLOW}sudo apt install python3-venv${RESET} (Debian/Ubuntu)"
        elif [ ! -f "$VENV/bin/python3" ]; then
            warn "Python venv not set up"
            echo -e "  Run: ${YELLOW}make venv-setup${RESET}"
        else
            ok "Python venv: $VENV"

            # Check for required Python packages
            local missing_pkgs=""
            if ! "$VENV/bin/python3" -c "import png" >/dev/null 2>&1; then
                missing_pkgs="$missing_pkgs pypng"
            fi
            if ! "$VENV/bin/python3" -c "import lz4" >/dev/null 2>&1; then
                missing_pkgs="$missing_pkgs lz4"
            fi

            if [ -n "$missing_pkgs" ]; then
                warn "Missing Python packages:$missing_pkgs"
                echo -e "  Run: ${YELLOW}make venv-setup${RESET}"
            else
                ok "Python packages (pypng, lz4) installed"
            fi
        fi
    else
        fail "python3 not found" "python3"
        hint "python3" "python3" "python3" "python3"
    fi

    # Formatting tools (warnings only)
    if command -v clang-format >/dev/null 2>&1; then
        ok "clang-format found: $(clang-format --version | head -n1)"
    else
        warn "clang-format not found (needed for code formatting)"
        hint "clang-format" "clang-format" "clang-format" "clang-tools-extra"
    fi

    if command -v xmllint >/dev/null 2>&1; then
        ok "xmllint found"
    else
        warn "xmllint not found (needed for XML validation)"
        hint "xmllint" "libxml2" "libxml2-utils" "libxml2"
    fi

    # Shell testing and linting tools (warnings only)
    if command -v shellcheck >/dev/null 2>&1; then
        ok "shellcheck found: $(shellcheck --version | grep '^version:' | awk '{print $2}')"
    else
        warn "shellcheck not found (needed for shell script linting)"
        hint "shellcheck" "shellcheck" "shellcheck" "ShellCheck"
    fi

    if command -v bats >/dev/null 2>&1; then
        ok "bats found: $(bats --version)"
    else
        warn "bats not found (needed for shell/platform hook tests)"
        hint "bats" "bats-core" "bats" "bats"
    fi

    # Fast linker. Warning only: GNU ld is correct everywhere and only slower.
    # The answer comes from the same helper the Makefile uses, so what is reported
    # here is what the build will actually do — including the case where the mold
    # on PATH is not the mold clang will exec.
    local picker desc choice mold_bin mold_ver lld_bin
    picker="$(dirname "$0")/pick-fast-linker.sh"
    if [ -x "$picker" ]; then
        desc=$("$picker" "${CXX:-c++}" --describe 2>/dev/null || echo '|||')
        IFS='|' read -r choice mold_bin mold_ver lld_bin <<< "$desc"
        if [ "$choice" = "mold" ]; then
            ok "fast linker: mold $mold_ver ($mold_bin)"
        elif [ -n "$mold_bin" ]; then
            # Deliberately not a fail: the build is correct, it just gets no speedup.
            warn "mold $mold_ver at $mold_bin links this tree incorrectly (#1584) — not used"
            echo -e "  Install mold 2.x ${YELLOW}over that path${RESET}; a newer one elsewhere on"
            echo -e "  PATH does not help, ${YELLOW}${CXX:-c++}${RESET} resolves this one first."
            [ -n "$lld_bin" ] && info "falling back to lld ($lld_bin)"
        elif [ "$choice" = "lld" ]; then
            ok "fast linker: lld ($lld_bin)"
        else
            warn "no fast linker found — each test link takes ~25s longer under GNU ld"
            hint "mold" "mold" "mold" "mold"
        fi
    fi
}

check_docker_tools() {
    echo ""
    echo -e "${BOLD}Docker Cross-Compilation Tools (optional)${RESET}"

    # Docker itself
    if command -v docker >/dev/null 2>&1; then
        ok "docker found: $(docker --version | head -n1)"

        # Check for BuildKit support (needed for modern docker build)
        # BuildKit can be enabled via:
        #   1. docker buildx (preferred, comes with Docker Desktop and newer Docker CLI)
        #   2. DOCKER_BUILDKIT=1 environment variable (legacy method)
        if docker buildx version >/dev/null 2>&1; then
            ok "docker buildx found: $(docker buildx version | head -n1)"
        else
            warn "docker buildx not found (needed for cross-compilation builds)"
            echo "  The legacy Docker builder is deprecated and will be removed."
            if [ "$(uname -s)" = "Darwin" ]; then
                echo -e "  Install: ${YELLOW}brew install docker-buildx${RESET}"
            else
                echo -e "  Install: ${YELLOW}https://docs.docker.com/go/buildx/${RESET}"
            fi
        fi
    else
        info "docker not found (only needed for cross-compilation)"
        if [ "$(uname -s)" = "Darwin" ]; then
            echo -e "  Install: ${YELLOW}brew install colima docker docker-buildx && colima start${RESET}"
        else
            echo -e "  Install: ${YELLOW}https://docs.docker.com/engine/install/${RESET}"
        fi
    fi
}

check_canvas_libs() {
    echo ""
    echo -e "${BOLD}Canvas/Image Libraries (optional)${RESET}"

    local canvas_missing=""

    if check_pkg cairo; then
        :
    else
        warn "cairo not found"
        canvas_missing="$canvas_missing cairo"
    fi

    if check_pkg pango; then
        :
    else
        warn "pango not found"
        canvas_missing="$canvas_missing pango"
    fi

    if check_pkg libpng; then
        :
    else
        warn "libpng not found"
        canvas_missing="$canvas_missing libpng"
    fi

    # These are truly optional
    if check_pkg libjpeg 2>/dev/null; then
        :
    else
        info "libjpeg not found (optional, for JPEG support)"
    fi

    if check_pkg librsvg-2.0 librsvg 2>/dev/null; then
        :
    else
        info "librsvg not found (optional, for SVG support)"
    fi

    if [ -n "$canvas_missing" ]; then
        echo ""
        echo -e "  ${CYAN}To install canvas dependencies:${RESET}"
        if [ "$(uname -s)" = "Darwin" ]; then
            echo -e "  ${YELLOW}brew install$canvas_missing${RESET}"
        elif [ -f /etc/debian_version ]; then
            local debian_pkgs=""
            for lib in $canvas_missing; do
                case $lib in
                    cairo) debian_pkgs="$debian_pkgs libcairo2-dev" ;;
                    pango) debian_pkgs="$debian_pkgs libpango1.0-dev" ;;
                    libpng) debian_pkgs="$debian_pkgs libpng-dev" ;;
                esac
            done
            echo -e "  ${YELLOW}sudo apt install$debian_pkgs${RESET}"
        elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
            local fedora_pkgs=""
            for lib in $canvas_missing; do
                case $lib in
                    cairo) fedora_pkgs="$fedora_pkgs cairo-devel" ;;
                    pango) fedora_pkgs="$fedora_pkgs pango-devel" ;;
                    libpng) fedora_pkgs="$fedora_pkgs libpng-devel" ;;
                esac
            done
            echo -e "  ${YELLOW}sudo dnf install$fedora_pkgs${RESET}"
        fi
    fi
}

print_summary() {
    echo ""

    if [ $ERRORS -gt 0 ]; then
        echo -e "${RED}${BOLD}✗ Missing required dependencies:${RESET}$MISSING"
        echo ""
        echo -e "Run: ${YELLOW}make install-deps${RESET}"
        return 1
    elif [ $WARNINGS -gt 0 ]; then
        echo -e "${YELLOW}⚠ Some optional dependencies missing (build may still work)${RESET}"
        return 0
    else
        echo -e "${GREEN}${BOLD}✓ All dependencies satisfied!${RESET}"
        return 0
    fi
}

# =============================================================================
# Main
# =============================================================================

MINIMAL=0
for arg in "$@"; do
    case "$arg" in
        --minimal|-m)
            MINIMAL=1
            ;;
        --help|-h)
            echo "Usage: $0 [--minimal]"
            echo ""
            echo "Options:"
            echo "  --minimal, -m   Only check essential dependencies (for cross-compilation)"
            echo "  --help, -h      Show this help message"
            exit 0
            ;;
    esac
done

echo -e "${CYAN}${BOLD}Checking build dependencies...${RESET}"

if [ $MINIMAL -eq 1 ]; then
    info "Minimal mode: checking essential dependencies only"
fi

# Always check essential deps
check_essential
check_submodules
check_libraries

# Full check includes desktop tools
if [ $MINIMAL -eq 0 ]; then
    check_desktop_tools
    check_canvas_libs
    check_docker_tools
fi

# Print summary and exit with appropriate code
print_summary
exit $?
