#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for helix-launcher.sh environment handling:
# - Display backend defaulting (fbdev on Linux)
# - Env file sourcing (helixscreen.env)
# - Environment variable precedence
# - No env file present
# - All env file variables
# Split from test_helix_launcher.bats for parallel execution.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers
    # helix-launcher.sh runs `killall helix-watchdog helix-screen ...`, which is
    # not scoped to this test. bats runs FILES in parallel, so an unmocked run
    # reaches across and kills the long-lived instance test_headless_display.bats
    # is driving - it dies cleanly mid-startup and that test fails for no reason
    # of its own.
    mock_command_script "killall" 'exit 0'

    # Create a mock install layout so the launcher can find binaries
    export MOCK_INSTALL="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$MOCK_INSTALL/bin"
    mkdir -p "$MOCK_INSTALL/config"

    # Create fake binaries that just exit
    printf '#!/bin/sh\nexit 0\n' > "$MOCK_INSTALL/bin/helix-screen"
    printf '#!/bin/sh\nexit 0\n' > "$MOCK_INSTALL/bin/helix-splash"
    chmod +x "$MOCK_INSTALL/bin/helix-screen" "$MOCK_INSTALL/bin/helix-splash"
    # No watchdog — launcher will run helix-screen directly

    # Extract the env-handling portion of the launcher into a testable snippet.
    # We source just the variable setup logic without actually launching anything.
    # This avoids needing real binaries, display hardware, etc.
    cat > "$BATS_TEST_TMPDIR/env_setup.sh" << 'ENVEOF'
#!/bin/sh
# Minimal harness that runs just the env-handling parts of helix-launcher.sh

# These would normally be derived from $0 / binary detection
SCRIPT_DIR="$MOCK_INSTALL/bin"
BIN_DIR="$MOCK_INSTALL/bin"
INSTALL_DIR="$MOCK_INSTALL"

# --- Begin: extracted from helix-launcher.sh ---

# Source environment configuration file if present.
_helix_env_file=""
for _env_path in \
    "${INSTALL_DIR}/config/helixscreen.env" \
    /etc/helixscreen/helixscreen.env; do
    if [ -f "$_env_path" ]; then
        _helix_env_file="$_env_path"
        break
    fi
done
unset _env_path

if [ -n "$_helix_env_file" ]; then
    _lineno=0
    while IFS= read -r _line || [ -n "$_line" ]; do
        _lineno=$((_lineno + 1))
        _line=$(printf '%s' "$_line" | sed -e 's/\r$//' \
                                            -e 's/^[ 	]*//' \
                                            -e 's/[ 	]*$//' \
                                            -e 's/^export[ 	][ 	]*//')
        case "$_line" in
            '#'*|'') continue ;;
        esac
        case "$_line" in
            [A-Za-z_]*=*) ;;
            *)
                echo "[helix-launcher] warning: ${_helix_env_file}:${_lineno}: ignored malformed line: $_line" >&2
                continue
                ;;
        esac
        _var="${_line%%=*}"
        case "$_var" in
            *[!A-Za-z0-9_]*)
                echo "[helix-launcher] warning: ${_helix_env_file}:${_lineno}: invalid variable name '$_var'" >&2
                continue
                ;;
        esac
        eval "_existing=\"\${${_var}:-}\""
        if [ -z "$_existing" ]; then
            if ! eval "export $_line" 2>/dev/null; then
                echo "[helix-launcher] warning: ${_helix_env_file}:${_lineno}: failed to export: $_line" >&2
            fi
        fi
    done < "$_helix_env_file"
    unset _line _var _existing _lineno
fi
unset _helix_env_file

# Resolve debug/logging settings: CLI flags > env vars (incl. env file) > defaults
#
# HAND-COPIED from helix-launcher.sh, and deliberately WITHOUT the
# platform-hook sourcing that precedes these lines in the real script. Only the
# env-file precedence is under test here. Hook-exported HELIX_LOG_* ordering is
# covered against the real launcher in test_helix_launcher_e2e.bats.
DEBUG_MODE="${CLI_DEBUG:-${HELIX_DEBUG:-0}}"
LOG_DEST="${CLI_LOG_DEST:-${HELIX_LOG_DEST:-auto}}"
LOG_FILE="${CLI_LOG_FILE:-${HELIX_LOG_FILE:-}}"
LOG_LEVEL="${CLI_LOG_LEVEL:-${HELIX_LOG_LEVEL:-}}"

# Default display backend to fbdev on embedded Linux targets.
if [ -z "${HELIX_DISPLAY_BACKEND:-}" ]; then
    case "$(uname -s)" in
        Linux)
            export HELIX_DISPLAY_BACKEND=fbdev
            ;;
    esac
fi

# --- End: extracted from helix-launcher.sh ---
ENVEOF
    chmod +x "$BATS_TEST_TMPDIR/env_setup.sh"

    # Create a mock helix-screen that writes its args to a file for inspection
    cat > "$MOCK_INSTALL/bin/helix-screen" << 'MOCKEOF'
#!/bin/sh
# Write all args to a file for test inspection
for arg in "$@"; do
    echo "$arg"
done > "$MOCK_INSTALL/helix_screen_args.txt"
exit 0
MOCKEOF
    chmod +x "$MOCK_INSTALL/bin/helix-screen"
}

# Helper: run the env setup snippet and print a variable's value
run_env_setup() {
    # Run in a subshell to isolate env changes
    sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$$1\""
}

# =============================================================================
# Display backend defaulting
# =============================================================================

@test "launcher defaults HELIX_DISPLAY_BACKEND to fbdev on Linux" {
    # Only meaningful on Linux, but the logic checks uname
    unset HELIX_DISPLAY_BACKEND
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_DISPLAY_BACKEND)
    if [ "$(uname -s)" = "Linux" ]; then
        [ "$result" = "fbdev" ]
    else
        # On macOS, the fallback doesn't trigger (no Linux case match)
        [ "$result" = "" ]
    fi
}

@test "launcher respects existing HELIX_DISPLAY_BACKEND=drm from environment" {
    export HELIX_DISPLAY_BACKEND=drm
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$HELIX_DISPLAY_BACKEND\"")
    [ "$result" = "drm" ]
}

@test "launcher respects HELIX_DISPLAY_BACKEND=sdl from environment" {
    export HELIX_DISPLAY_BACKEND=sdl
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$HELIX_DISPLAY_BACKEND\"")
    [ "$result" = "sdl" ]
}

# =============================================================================
# Env file sourcing
# =============================================================================

@test "launcher sources helixscreen.env from install dir" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
MOONRAKER_HOST=myprinter.local
MOONRAKER_PORT=7125
EOF
    unset MOONRAKER_HOST
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_HOST)
    [ "$result" = "myprinter.local" ]
}

@test "launcher sources MOONRAKER_PORT from env file" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
MOONRAKER_HOST=localhost
MOONRAKER_PORT=8080
EOF
    unset MOONRAKER_PORT
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_PORT)
    [ "$result" = "8080" ]
}

@test "env file skips commented lines" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
# This is a comment
#HELIX_DISPLAY_BACKEND=drm
MOONRAKER_HOST=localhost
EOF
    unset HELIX_DISPLAY_BACKEND MOONRAKER_HOST
    # The commented HELIX_DISPLAY_BACKEND=drm should NOT be set
    # So on Linux it should fall through to the fbdev default
    result_backend=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_DISPLAY_BACKEND)
    result_host=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_HOST)
    [ "$result_host" = "localhost" ]
    if [ "$(uname -s)" = "Linux" ]; then
        [ "$result_backend" = "fbdev" ]
    fi
}

@test "env file skips blank lines" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'

MOONRAKER_HOST=localhost

MOONRAKER_PORT=7125

EOF
    unset MOONRAKER_HOST MOONRAKER_PORT
    result_host=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_HOST)
    result_port=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_PORT)
    [ "$result_host" = "localhost" ]
    [ "$result_port" = "7125" ]
}

# =============================================================================
# Precedence: environment > env file > hardcoded default
# =============================================================================

@test "existing env var takes precedence over env file" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
MOONRAKER_HOST=from-file
EOF
    export MOONRAKER_HOST=from-env
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$MOONRAKER_HOST\"")
    [ "$result" = "from-env" ]
}

@test "env file HELIX_DISPLAY_BACKEND takes precedence over hardcoded fbdev default" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DISPLAY_BACKEND=drm
EOF
    unset HELIX_DISPLAY_BACKEND
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_DISPLAY_BACKEND)
    [ "$result" = "drm" ]
}

@test "environment HELIX_DISPLAY_BACKEND overrides env file value" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DISPLAY_BACKEND=drm
EOF
    export HELIX_DISPLAY_BACKEND=fbdev
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$HELIX_DISPLAY_BACKEND\"")
    [ "$result" = "fbdev" ]
}

@test "full precedence chain: env > file > default" {
    # Scenario: env file says drm, environment says sdl
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DISPLAY_BACKEND=drm
EOF
    export HELIX_DISPLAY_BACKEND=sdl
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$HELIX_DISPLAY_BACKEND\"")
    [ "$result" = "sdl" ]
}

# =============================================================================
# No env file present
# =============================================================================

@test "launcher works with no env file present" {
    # No helixscreen.env in either location
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_DISPLAY_BACKEND MOONRAKER_HOST
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_DISPLAY_BACKEND)
    if [ "$(uname -s)" = "Linux" ]; then
        [ "$result" = "fbdev" ]
    else
        [ "$result" = "" ]
    fi
}

# =============================================================================
# All env file variables from helixscreen.env are supported
# =============================================================================

@test "env file supports HELIX_FB_DEVICE" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_FB_DEVICE=/dev/fb1
EOF
    unset HELIX_FB_DEVICE
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_FB_DEVICE)
    [ "$result" = "/dev/fb1" ]
}

@test "env file supports HELIX_DRM_DEVICE" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DRM_DEVICE=/dev/dri/card1
EOF
    unset HELIX_DRM_DEVICE
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_DRM_DEVICE)
    [ "$result" = "/dev/dri/card1" ]
}

@test "env file supports HELIX_LOG_LEVEL" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_LOG_LEVEL=debug
EOF
    unset HELIX_LOG_LEVEL
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_LOG_LEVEL)
    [ "$result" = "debug" ]
}

@test "env file supports HELIX_AUTO_QUIT_MS" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_AUTO_QUIT_MS=5000
EOF
    unset HELIX_AUTO_QUIT_MS
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_AUTO_QUIT_MS)
    [ "$result" = "5000" ]
}

# =============================================================================
# Tolerant parsing: common user typos that historically silently no-op'd
# =============================================================================

@test "env file accepts 'export VAR=value' (bash habit)" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
export HELIX_TOUCH_CALIBRATE=1
EOF
    unset HELIX_TOUCH_CALIBRATE
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_TOUCH_CALIBRATE)
    [ "$result" = "1" ]
}

@test "env file accepts leading whitespace before VAR=value" {
    printf '    HELIX_TOUCH_CALIBRATE=1\n' > "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_TOUCH_CALIBRATE
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_TOUCH_CALIBRATE)
    [ "$result" = "1" ]
}

@test "env file accepts trailing whitespace after value" {
    printf 'MOONRAKER_HOST=localhost   \n' > "$MOCK_INSTALL/config/helixscreen.env"
    unset MOONRAKER_HOST
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_HOST)
    [ "$result" = "localhost" ]
}

@test "env file accepts CRLF line endings" {
    printf 'HELIX_TOUCH_CALIBRATE=1\r\nMOONRAKER_HOST=localhost\r\n' \
        > "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_TOUCH_CALIBRATE MOONRAKER_HOST
    result_cal=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_TOUCH_CALIBRATE)
    result_host=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_HOST)
    [ "$result_cal" = "1" ]
    [ "$result_host" = "localhost" ]
}

@test "env file tolerates 'export' + leading whitespace combined" {
    printf '  export HELIX_TOUCH_CALIBRATE=1\n' > "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_TOUCH_CALIBRATE
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_TOUCH_CALIBRATE)
    [ "$result" = "1" ]
}

@test "env file warns on malformed line without dropping later lines" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
NOT A VAR
MOONRAKER_HOST=localhost
EOF
    unset MOONRAKER_HOST
    # Capture stderr to assert a warning fired
    err_output=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c \
        ". \"$BATS_TEST_TMPDIR/env_setup.sh\" 2>&1 1>/dev/null; echo")
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_HOST)
    # Later valid line still loaded
    [ "$result" = "localhost" ]
    # Warning fired
    echo "$err_output" | grep -q "warning.*ignored malformed line"
}

@test "env file warns on invalid variable name with special chars" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
BAD-NAME=value
MOONRAKER_HOST=localhost
EOF
    unset MOONRAKER_HOST
    err_output=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c \
        ". \"$BATS_TEST_TMPDIR/env_setup.sh\" 2>&1 1>/dev/null; echo")
    result=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup MOONRAKER_HOST)
    [ "$result" = "localhost" ]
    echo "$err_output" | grep -qE "warning.*invalid variable name|warning.*ignored malformed line"
}

# =============================================================================
# MALLOC_ARENA_MAX on memory-constrained boards
#
# These extract the block from the REAL helix-launcher.sh rather than copying it
# into the harness above. A copied snippet passes forever after the original is
# changed or deleted; extraction fails loudly instead (assert_extracted).
# =============================================================================

# Pull the arena block out of the launcher and eval it with a fake meminfo.
# Args: $1 = MemTotal in kB to report. Echoes the resulting MALLOC_ARENA_MAX
# (empty when the block left it unset).
run_arena_block() {
    local mem_kb="$1"
    local meminfo="$BATS_TEST_TMPDIR/meminfo"

    printf 'MemTotal:       %s kB\nMemFree:         10000 kB\n' "$mem_kb" > "$meminfo"

    awk '/^# Cap glibc.s per-thread malloc arenas/{f=1} f{print} f&&/^fi$/{exit}' \
        "$LAUNCHER" > "$BATS_TEST_TMPDIR/arena_block.sh"

    HELIX_MEMINFO_FILE="$meminfo" sh -c "
        set -e
        . '$BATS_TEST_TMPDIR/arena_block.sh'
        echo \"\${MALLOC_ARENA_MAX:-}\"
    "
}

# Guard: if the block ever stops being extractable, every test below would
# silently pass against an empty file. Fail instead.
assert_extracted() {
    [ -s "$BATS_TEST_TMPDIR/arena_block.sh" ]
    grep -q "MALLOC_ARENA_MAX" "$BATS_TEST_TMPDIR/arena_block.sh"
}

@test "arena cap: applied on a CC1-sized board (114 MB)" {
    result=$(run_arena_block 114656)
    assert_extracted
    [ "$result" = "2" ]
}

@test "arena cap: applied on an AD5M-sized board (110 MB)" {
    result=$(run_arena_block 110404)
    assert_extracted
    [ "$result" = "2" ]
}

@test "arena cap: applied on a K2 Plus-sized board (488 MB)" {
    # Closest measured device below the threshold — pins the gap, so moving the
    # line down past 488 MB has to be a deliberate edit, not an accident.
    result=$(run_arena_block 499952)
    assert_extracted
    [ "$result" = "2" ]
}

@test "arena cap: NOT applied on a Snapmaker U1-sized board (962 MB)" {
    result=$(run_arena_block 985000)
    assert_extracted
    [ -z "$result" ]
}

@test "arena cap: NOT applied on a CB1-sized board (987 MB)" {
    result=$(run_arena_block 1010636)
    assert_extracted
    [ -z "$result" ]
}

@test "arena cap: NOT applied on a desktop-sized machine" {
    result=$(run_arena_block 16000000)
    assert_extracted
    [ -z "$result" ]
}

@test "arena cap: an existing user value always wins" {
    local meminfo="$BATS_TEST_TMPDIR/meminfo"
    printf 'MemTotal:       114656 kB\n' > "$meminfo"
    awk '/^# Cap glibc.s per-thread malloc arenas/{f=1} f{print} f&&/^fi$/{exit}' \
        "$LAUNCHER" > "$BATS_TEST_TMPDIR/arena_block.sh"
    assert_extracted

    # A user who set 8 in helixscreen.env on a constrained board keeps 8.
    result=$(HELIX_MEMINFO_FILE="$meminfo" MALLOC_ARENA_MAX=8 sh -c "
        set -e
        . '$BATS_TEST_TMPDIR/arena_block.sh'
        echo \"\$MALLOC_ARENA_MAX\"
    ")
    [ "$result" = "8" ]
}

@test "arena cap: unreadable meminfo leaves glibc's default alone" {
    awk '/^# Cap glibc.s per-thread malloc arenas/{f=1} f{print} f&&/^fi$/{exit}' \
        "$LAUNCHER" > "$BATS_TEST_TMPDIR/arena_block.sh"
    assert_extracted

    result=$(HELIX_MEMINFO_FILE="$BATS_TEST_TMPDIR/no-such-meminfo" sh -c "
        set -e
        . '$BATS_TEST_TMPDIR/arena_block.sh'
        echo \"\${MALLOC_ARENA_MAX:-}\"
    ")
    [ -z "$result" ]
}

@test "arena cap: garbage MemTotal does not abort the launcher under set -e" {
    local meminfo="$BATS_TEST_TMPDIR/meminfo"
    printf 'MemTotal:       not-a-number kB\n' > "$meminfo"
    awk '/^# Cap glibc.s per-thread malloc arenas/{f=1} f{print} f&&/^fi$/{exit}' \
        "$LAUNCHER" > "$BATS_TEST_TMPDIR/arena_block.sh"
    assert_extracted

    # The launcher runs under `set -e`; a non-numeric value must fall through
    # rather than make an arithmetic test abort the whole startup.
    run sh -c "
        set -e
        HELIX_MEMINFO_FILE='$meminfo'
        . '$BATS_TEST_TMPDIR/arena_block.sh'
        echo \"exit-ok:\${MALLOC_ARENA_MAX:-unset}\"
    "
    [ "$status" -eq 0 ]
    [ "$output" = "exit-ok:unset" ]
}

# =============================================================================
# AD5X heap-diag predicate — ZMOD or Forge-X
#
# Same extraction discipline as the arena block: pull the real block out of
# helix-launcher.sh so a copied snippet can't keep passing after the original
# changes. The arch comes from a fake uname on PATH (the block calls uname
# itself); the layout probes resolve under HELIX_AD5X_PROBE_ROOT pointing at a
# sandbox fake root — these tests must never create /ZMOD or /usr/prog on the
# build host. Pinned to the SAME truth table as the C++ predicate
# helix::ad5x_mod_layout_present() (tests/unit/test_platform_info.cpp).
# =============================================================================

# Echo the resulting MALLOC_CHECK_ (empty when the block left it unset) for a
# given fake uname -m and sandbox rootfs layout.
run_heap_block() {
    local fake_arch="$1" root="$2"
    local fakebin="$BATS_TEST_TMPDIR/fakebin"
    mkdir -p "$fakebin"
    printf '#!/bin/sh\ncase "$1" in -m) echo "%s";; -r) echo "5.10.99-helix";; *) echo Linux;; esac\n' \
        "$fake_arch" > "$fakebin/uname"
    chmod +x "$fakebin/uname"

    awk '/^# Heap-corruption diagnostics/{f=1} f{print} f&&/^unset _arch _kernel _enable_heap_diag$/{exit}' \
        "$LAUNCHER" > "$BATS_TEST_TMPDIR/heap_block.sh"

    PATH="$fakebin:$PATH" HELIX_AD5X_PROBE_ROOT="$root" sh -c "
        set -e
        . '$BATS_TEST_TMPDIR/heap_block.sh'
        echo \"\${MALLOC_CHECK_:-}\"
    "
}

# Guard: if the block ever stops being extractable, every test below would
# silently pass against an empty file. Fail instead.
assert_heap_extracted() {
    [ -s "$BATS_TEST_TMPDIR/heap_block.sh" ]
    grep -q "MALLOC_CHECK_" "$BATS_TEST_TMPDIR/heap_block.sh"
    grep -q "_arch" "$BATS_TEST_TMPDIR/heap_block.sh"
}

@test "heap diag: ZMOD layout (/usr/prog dir) enables on mips" {
    local root="$BATS_TEST_TMPDIR/zmod-prog"
    mkdir -p "$root/usr/prog"
    result=$(run_heap_block mips "$root")
    assert_heap_extracted
    [ "$result" = "3" ]
}

@test "heap diag: ZMOD layout (/ZMOD marker file) enables on mips" {
    local root="$BATS_TEST_TMPDIR/zmod-marker"
    mkdir -p "$root"
    touch "$root/ZMOD"
    result=$(run_heap_block mips "$root")
    assert_heap_extracted
    [ "$result" = "3" ]
}

@test "heap diag: Forge-X chroot layout (mod tree reachable, no /ZMOD, no /usr/prog) enables on mips" {
    # The rig: no /ZMOD, no /usr/prog, no /usr/data inside the chroot — only
    # the mod's git tree, bind-mounted at /opt/config/mod. platform.sh
    # reachability must carry the predicate on its own.
    local root="$BATS_TEST_TMPDIR/forgex"
    mkdir -p "$root/opt/config/mod/.shell"
    touch "$root/opt/config/mod/.shell/platform.sh"
    result=$(run_heap_block mips "$root")
    assert_heap_extracted
    [ "$result" = "3" ]
}

@test "heap diag: Forge-X host-side spelling (/usr/data/config/mod) enables on mips" {
    local root="$BATS_TEST_TMPDIR/forgex-host"
    mkdir -p "$root/usr/data/config/mod/.shell"
    touch "$root/usr/data/config/mod/.shell/platform.sh"
    result=$(run_heap_block mips "$root")
    assert_heap_extracted
    [ "$result" = "3" ]
}

@test "heap diag: plain layout leaves MALLOC_CHECK_ unset on mips" {
    # K1 shares mips and carries none of the markers — the arch alone must
    # never arm the diagnostics.
    local root="$BATS_TEST_TMPDIR/plain"
    mkdir -p "$root"
    result=$(run_heap_block mips "$root")
    assert_heap_extracted
    [ -z "$result" ]
}

@test "heap diag: Forge-X layout does not enable on a non-mips host" {
    local root="$BATS_TEST_TMPDIR/forgex-x86"
    mkdir -p "$root/opt/config/mod/.shell"
    touch "$root/opt/config/mod/.shell/platform.sh"
    result=$(run_heap_block x86_64 "$root")
    assert_heap_extracted
    [ -z "$result" ]
}
