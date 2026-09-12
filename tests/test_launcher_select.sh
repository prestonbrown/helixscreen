#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for helix-launcher.sh binary selection logic

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors for output
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
BOLD='\033[1m'
RESET='\033[0m'

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
declare -a FAILED_TESTS

print_test() {
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -e "\n${CYAN}[TEST $TESTS_RUN]${RESET} $1"
}

assert() {
    local condition="$1"
    local message="$2"
    if eval "$condition"; then
        echo -e "${GREEN}  ✓${RESET} $message"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo -e "${RED}  ✗${RESET} $message"
        FAILED_TESTS+=("TEST $TESTS_RUN: $message")
        TESTS_FAILED=$((TESTS_FAILED + 1))
        return 1
    fi
}

# Create temp dir with mock binaries for testing
setup_mock_bindir() {
    local tmpdir
    tmpdir=$(mktemp -d)
    echo "$tmpdir"
}

cleanup_mock_bindir() {
    rm -rf "$1"
}

# Extract select_binary and everything it calls from the launcher, so the
# harness exercises the real code. A helper left out here does not fail the
# harness: the shell reports "not found", returns 127, and the caller reads
# that as an ordinary false — which looks exactly like a selection decision.
extract_select_binary() {
    local launcher="${PROJECT_ROOT}/scripts/helix-launcher.sh"
    local fn
    for fn in log libs_resolve probe_egl select_binary; do
        sed -n "/^${fn}()/,/^}/p" "$launcher"
    done
}

# Create a self-contained test script that includes select_binary
create_test_harness() {
    local bindir="$1"
    local harness="$bindir/_test_harness.sh"
    cat > "$harness" << 'HARNESS_EOF'
#!/bin/sh
# Extract select_binary from launcher
HARNESS_EOF
    extract_select_binary >> "$harness"
    echo 'select_binary "$1"' >> "$harness"
    chmod +x "$harness"
    echo "$harness"
}

# Test: no fallback binary → selects primary
test_no_fallback_selects_primary() {
    print_test "No fallback binary → selects primary"
    local bindir
    bindir=$(setup_mock_bindir)

    # Create only primary binary
    echo '#!/bin/sh' > "$bindir/helix-screen"
    chmod +x "$bindir/helix-screen"

    local harness
    harness=$(create_test_harness "$bindir")

    local result
    result=$(sh "$harness" "$bindir")

    assert "[ '$result' = '$bindir/helix-screen' ]" \
        "Selected primary when no fallback exists"

    cleanup_mock_bindir "$bindir"
}

# Test: primary has missing libs → selects fbdev
test_missing_libs_selects_fbdev() {
    print_test "Primary has missing libs → selects fbdev"
    local bindir
    bindir=$(setup_mock_bindir)

    # Create both binaries
    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"

    # Create a fake ldd that reports missing libs for primary
    local fake_ldd="$bindir/ldd"
    cat > "$fake_ldd" << 'EOF'
#!/bin/sh
case "$1" in
    *helix-screen-fbdev)
        echo "	linux-vdso.so.1 => (0x00007ffd)"
        echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x00007f)"
        ;;
    *helix-screen)
        echo "	linux-vdso.so.1 => (0x00007ffd)"
        echo "	libEGL.so.1 => not found"
        echo "	libGLESv2.so.2 => not found"
        echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x00007f)"
        ;;
esac
EOF
    chmod +x "$fake_ldd"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" sh "$harness" "$bindir")

    assert "[ '$result' = '$bindir/helix-screen-fbdev' ]" \
        "Selected fbdev when primary has missing GL libs"

    cleanup_mock_bindir "$bindir"
}

# Test: primary libs all satisfied → selects primary (DRM)
test_libs_satisfied_selects_primary() {
    print_test "Primary libs all satisfied → selects primary"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"

    # Create a fake ldd that reports all libs found
    local fake_ldd="$bindir/ldd"
    cat > "$fake_ldd" << 'EOF'
#!/bin/sh
echo "	linux-vdso.so.1 => (0x00007ffd)"
echo "	libEGL.so.1 => /usr/lib/aarch64-linux-gnu/libEGL.so.1 (0x00007f)"
echo "	libGLESv2.so.2 => /usr/lib/aarch64-linux-gnu/libGLESv2.so.2 (0x00007f)"
echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x00007f)"
EOF
    chmod +x "$fake_ldd"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" sh "$harness" "$bindir")

    assert "[ '$result' = '$bindir/helix-screen' ]" \
        "Selected primary when all libs satisfied"

    cleanup_mock_bindir "$bindir"
}

# Test: HELIX_DISPLAY_BACKEND forced to fbdev → skips DRM
test_env_forced_fbdev() {
    print_test "HELIX_DISPLAY_BACKEND=fbdev → selects fbdev"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(HELIX_DISPLAY_BACKEND=fbdev sh "$harness" "$bindir")

    assert "[ '$result' = '$bindir/helix-screen-fbdev' ]" \
        "Selected fbdev when HELIX_DISPLAY_BACKEND=fbdev"

    cleanup_mock_bindir "$bindir"
}

# Test: ldd not available → tries primary (best effort)
test_no_ldd_selects_primary() {
    print_test "ldd not available → selects primary (best effort)"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"

    local harness
    harness=$(create_test_harness "$bindir")
    # Create a wrapper dir with symlinks to essentials but NOT ldd
    local wrapper_dir="$bindir/_no_ldd"
    mkdir -p "$wrapper_dir"
    for cmd in sh grep command env; do
        local cmd_path
        cmd_path=$(command -v "$cmd" 2>/dev/null || true)
        [ -n "$cmd_path" ] && ln -sf "$cmd_path" "$wrapper_dir/$cmd"
    done
    local result
    result=$(PATH="$wrapper_dir" sh "$harness" "$bindir")

    assert "[ '$result' = '$bindir/helix-screen' ]" \
        "Selected primary when ldd not available"

    cleanup_mock_bindir "$bindir"
}

# Test: both binaries present, no env override, ldd says both fine → primary
test_both_fine_selects_primary() {
    print_test "Both binaries fine → selects primary"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"

    local fake_ldd="$bindir/ldd"
    cat > "$fake_ldd" << 'EOF'
#!/bin/sh
echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x00007f)"
EOF
    chmod +x "$fake_ldd"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" sh "$harness" "$bindir")

    assert "[ '$result' = '$bindir/helix-screen' ]" \
        "Selected primary when both binaries have satisfied deps"

    cleanup_mock_bindir "$bindir"
}

# Build a mock EGL binary whose --probe-egl exits with the given code.
make_mock_egl() {
    local path="$1"
    local rc="$2"
    local verdict="$3"
    cat > "$path" << EOF
#!/bin/sh
if [ "\$1" = "--probe-egl" ]; then
    echo "$verdict"
    exit $rc
fi
EOF
    chmod +x "$path"
}

# All libs resolve, for tests that care about the probe rather than ldd.
make_fake_ldd_ok() {
    cat > "$1/ldd" << 'EOF'
#!/bin/sh
echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x00007f)"
EOF
    chmod +x "$1/ldd"
}

# Test: probe reports a hardware renderer → selects EGL
test_probe_ok_selects_egl() {
    print_test "EGL probe succeeds → selects EGL binary"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"
    make_mock_egl "$bindir/helix-screen-egl" 0 "V3D 7.1.7"
    make_fake_ldd_ok "$bindir"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" sh "$harness" "$bindir" 2>/dev/null)

    assert "[ '$result' = '$bindir/helix-screen-egl' ]" \
        "Selected EGL binary when probe reports a hardware renderer"

    cleanup_mock_bindir "$bindir"
}

# Test: probe rejects a software renderer → DRM, never fbdev.
# Demoting two rungs on a GPU failure would hide a working middle rung.
test_probe_fail_selects_drm_not_fbdev() {
    print_test "EGL probe fails → selects DRM primary, not fbdev"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"
    make_mock_egl "$bindir/helix-screen-egl" 1 "software renderer (llvmpipe) - declining"
    make_fake_ldd_ok "$bindir"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" sh "$harness" "$bindir" 2>/dev/null)

    assert "[ '$result' = '$bindir/helix-screen' ]" \
        "Selected DRM primary when probe declined"
    assert "[ '$result' != '$bindir/helix-screen-fbdev' ]" \
        "Did not demote past the middle rung to fbdev"

    cleanup_mock_bindir "$bindir"
}

# Test: HELIX_DISPLAY_BACKEND=egl overrides a declining probe
test_env_forced_egl() {
    print_test "HELIX_DISPLAY_BACKEND=egl → selects EGL without probing"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"
    make_mock_egl "$bindir/helix-screen-egl" 1 "would decline"
    make_fake_ldd_ok "$bindir"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" HELIX_DISPLAY_BACKEND=egl sh "$harness" "$bindir" 2>/dev/null)

    assert "[ '$result' = '$bindir/helix-screen-egl' ]" \
        "Forced EGL even though the probe would decline"

    cleanup_mock_bindir "$bindir"
}

# Test: forcing fbdev still wins over an installed EGL binary
test_env_forced_fbdev_beats_egl() {
    print_test "HELIX_DISPLAY_BACKEND=fbdev → fbdev even with EGL installed"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"
    make_mock_egl "$bindir/helix-screen-egl" 0 "V3D 7.1.7"
    make_fake_ldd_ok "$bindir"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" HELIX_DISPLAY_BACKEND=fbdev sh "$harness" "$bindir" 2>/dev/null)

    assert "[ '$result' = '$bindir/helix-screen-fbdev' ]" \
        "Forced fbdev outranks an EGL binary whose probe succeeds"

    cleanup_mock_bindir "$bindir"
}

# Test: EGL binary present but its libs do not resolve → never probed
test_egl_missing_libs_skips_probe() {
    print_test "EGL binary with unresolvable libs → DRM primary"
    local bindir
    bindir=$(setup_mock_bindir)

    echo '#!/bin/sh' > "$bindir/helix-screen"
    echo '#!/bin/sh' > "$bindir/helix-screen-fbdev"
    chmod +x "$bindir/helix-screen" "$bindir/helix-screen-fbdev"
    # Exits 0 if it is ever run — so reaching the probe would select EGL and
    # this test would fail, proving the ldd gate is what kept it out.
    make_mock_egl "$bindir/helix-screen-egl" 0 "should never be consulted"

    cat > "$bindir/ldd" << 'EOF'
#!/bin/sh
case "$1" in
    *helix-screen-egl)
        echo "	libEGL.so.1 => not found"
        ;;
    *)
        echo "	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x00007f)"
        ;;
esac
EOF
    chmod +x "$bindir/ldd"

    local harness
    harness=$(create_test_harness "$bindir")
    local result
    result=$(PATH="$bindir:$PATH" sh "$harness" "$bindir" 2>/dev/null)

    assert "[ '$result' = '$bindir/helix-screen' ]" \
        "Skipped the EGL binary whose libraries do not resolve"

    cleanup_mock_bindir "$bindir"
}

# Run all tests
main() {
    echo -e "${BOLD}${CYAN}Launcher Binary Selection Test Harness${RESET}"
    echo -e "${CYAN}Project:${RESET} $PROJECT_ROOT"
    echo ""

    test_no_fallback_selects_primary
    test_missing_libs_selects_fbdev
    test_libs_satisfied_selects_primary
    test_env_forced_fbdev
    test_no_ldd_selects_primary
    test_both_fine_selects_primary
    test_probe_ok_selects_egl
    test_probe_fail_selects_drm_not_fbdev
    test_env_forced_egl
    test_env_forced_fbdev_beats_egl
    test_egl_missing_libs_skips_probe

    # Print summary
    echo ""
    echo -e "${BOLD}${CYAN}Test Summary${RESET}"
    echo -e "${CYAN}────────────────────────────────────────${RESET}"
    echo -e "Total tests: $TESTS_RUN"
    echo -e "${GREEN}Passed: $TESTS_PASSED${RESET}"

    if [ $TESTS_FAILED -gt 0 ]; then
        echo -e "${RED}Failed: $TESTS_FAILED${RESET}"
        echo ""
        echo -e "${RED}${BOLD}Failed tests:${RESET}"
        for test in "${FAILED_TESTS[@]}"; do
            echo -e "  ${RED}✗${RESET} $test"
        done
        exit 1
    else
        echo -e "${GREEN}${BOLD}✓ All tests passed!${RESET}"
        exit 0
    fi
}

main
