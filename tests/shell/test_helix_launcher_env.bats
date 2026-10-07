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

    # The launcher's trust gate refuses a group-writable env file, and this
    # suite's `cat >` fixtures inherit the host umask (0002 on this dev box
    # lands 0664). Every fixture here must read as a trusted 0644 file.
    umask 022

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
    # The copy pins resolved VALUES only: note wording and log prefixes are
    # deliberately exempt here, covered by the note tests that run the
    # shipped launcher.
    cat > "$BATS_TEST_TMPDIR/env_setup.sh" << 'ENVEOF'
#!/bin/sh
# Minimal harness that runs just the env-handling parts of helix-launcher.sh

# These would normally be derived from $0 / binary detection
SCRIPT_DIR="$MOCK_INSTALL/bin"
BIN_DIR="$MOCK_INSTALL/bin"
INSTALL_DIR="$MOCK_INSTALL"

# --- Begin: extracted from helix-launcher.sh ---

# The launcher's own log() and env-file functions, extracted in setup().
. "$BATS_TEST_TMPDIR/env_parse.sh"
helix_load_env_file

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

    # The env-file parse runs from the shipped launcher's own functions, so
    # the harness cannot drift from it. An extraction that finds nothing
    # fails here rather than letting every test read an empty parse.
    {
        awk '/^log\(\) \{/{f=1} f{print} f&&/^}$/{exit}' "$LAUNCHER"
        awk '/^helix_env_stat\(\) \{/{f=1} f{print} /^helix_load_env_file\(\) \{/{g=1} g&&/^}$/{exit}' "$LAUNCHER"
    } > "$BATS_TEST_TMPDIR/env_parse.sh"
    grep -q '^helix_load_env_file() {' "$BATS_TEST_TMPDIR/env_parse.sh"
    grep -q '^log() {' "$BATS_TEST_TMPDIR/env_parse.sh"

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

@test "a skipped env-file line says so under HELIX_DEBUG=1 and is silent otherwise" {
    # The skip is the precedence rule acting; without a trace of it, an
    # operator's file value looks accepted and discarded. Run the SHIPPED
    # launcher in query mode (--print-env NAME): it performs the same
    # env-file parse startup does, prints the resolved value on stdout and
    # the note on stderr, and exits before any display side effect or
    # daemon - so what ships is exactly what is asserted here.
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
MOONRAKER_HOST=from-file
EOF
    HELIX_DEBUG=1 MOONRAKER_HOST=from-env \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/loud.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "from-env" ]
    grep -q 'MOONRAKER_HOST already set in environment; file value ignored' "$BATS_TEST_TMPDIR/loud.log"
    HELIX_DEBUG=0 MOONRAKER_HOST=from-env \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > /dev/null 2> "$BATS_TEST_TMPDIR/quiet.log"
    [ ! -s "$BATS_TEST_TMPDIR/quiet.log" ]
    # Query mode launched nothing.
    [ ! -e "$MOCK_INSTALL/helix_screen_args.txt" ]
}

@test "a duplicate env-file key names the file, not the environment, as the winner" {
    # When the same key appears twice, what outranked the second line is the
    # FIRST line of the same file, not the shell environment - and the note
    # must say so, or the operator goes hunting for an env var that is not
    # set anywhere.
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
MOONRAKER_HOST=first
MOONRAKER_HOST=second
EOF
    HELIX_DEBUG=1 "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/dup.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "first" ]
    grep -q 'MOONRAKER_HOST already set by an earlier line of this file; this value ignored' "$BATS_TEST_TMPDIR/dup.log"
    if grep -q 'already set in environment' "$BATS_TEST_TMPDIR/dup.log"; then
        fail "note blames the environment for a duplicate key in the file"
    fi
}

@test "the extracted parse and the shipped launcher agree on tolerance shapes" {
    # env_setup.sh runs the launcher's parse functions without binaries; the
    # shipped launcher answers --print-env NAME through its own startup path.
    # Both must resolve every tolerance shape to the pinned value.
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    local var shape want copy_val shipped_val
    while IFS='|' read -r var shape want; do
        case "$var" in ''|'#'*) continue ;; esac
        printf '%s\n' "$shape" > "$MOCK_INSTALL/config/helixscreen.env"
        copy_val=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup "$var")
        shipped_val=$(env -u "$var" "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env "$var")
        [ "$copy_val" = "$want" ] || fail "extracted parse mis-reads [$shape] as [$copy_val]"
        [ "$shipped_val" = "$want" ] || fail "shipped launcher mis-reads [$shape] as [$shipped_val]"
    done <<'EOF'
HELIX_FB_DEVICE|HELIX_FB_DEVICE=/dev/fb1|/dev/fb1
HELIX_FB_DEVICE|   HELIX_FB_DEVICE=/dev/fb2|/dev/fb2
HELIX_FB_DEVICE|HELIX_FB_DEVICE=/dev/fb3   |/dev/fb3
HELIX_FB_DEVICE|export HELIX_FB_DEVICE=/dev/fb4|/dev/fb4
HELIX_FB_DEVICE|export   HELIX_FB_DEVICE=/dev/fb5|/dev/fb5
HELIX_FB_DEVICE|HELIX_FB_DEVICE='/dev/fb6'|/dev/fb6
HELIX_FB_DEVICE|HELIX_FB_DEVICE="/dev/fb7"|/dev/fb7
HELIX_FB_DEVICE|HELIX_FB_DEVICE=/dev/fb8 # trailing note|/dev/fb8
EOF
    # CRLF line endings: written with printf so the CR is real.
    printf 'HELIX_FB_DEVICE=/dev/fb9\r\n' > "$MOCK_INSTALL/config/helixscreen.env"
    copy_val=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_FB_DEVICE)
    shipped_val=$(env -u HELIX_FB_DEVICE "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_FB_DEVICE)
    [ "$copy_val" = "/dev/fb9" ] && [ "$shipped_val" = "/dev/fb9" ]
    # Duplicate keys: the first definition wins.
    printf 'HELIX_FB_DEVICE=/first\nHELIX_FB_DEVICE=/second\n' > "$MOCK_INSTALL/config/helixscreen.env"
    copy_val=$(MOCK_INSTALL="$MOCK_INSTALL" run_env_setup HELIX_FB_DEVICE)
    shipped_val=$(env -u HELIX_FB_DEVICE "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_FB_DEVICE)
    [ "$copy_val" = "/first" ] && [ "$shipped_val" = "/first" ]
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
# Trust gate: the parse evaluates file lines, so owner and mode decide whether
# the file is read at all. A refused file is skipped with a warning naming the
# fix; the launcher comes up (or answers --print-env) on defaults. Coverage of
# both branches of the owner rule needs uids the test user cannot produce, so
# `stat` is faked on PATH for those (same technique as the heap-diag uname).
# =============================================================================

# Fake stat answering the gate's exact question (`stat -L -c '%u %a' FILE`)
# with a fixed uid/mode pair, on every call alike - so a self-heal re-stat
# reports the same mode the first one did. FAKE_STAT_FAIL=1 makes it fail
# outright, standing in for a rootfs with no usable stat. Any other invocation
# fails too, so the BSD fallback form never reaches a real stat mid-test.
make_fake_stat() {
    mkdir -p "$BATS_TEST_TMPDIR/fakebin"
    printf '#!/bin/sh\n[ "${FAKE_STAT_FAIL:-0}" = "1" ] && exit 1\n[ "$1" = "-L" ] && [ "$2" = "-c" ] && echo "${FAKE_STAT_UID:-0} ${FAKE_STAT_MODE:-644}" || exit 1\n' \
        > "$BATS_TEST_TMPDIR/fakebin/stat"
    chmod +x "$BATS_TEST_TMPDIR/fakebin/stat"
}

# Per-path fake stat for the printer_data layout: each `PATH UID MODE` line in
# $BATS_TEST_TMPDIR/statmap answers `stat -L -c '%u %a' PATH`, and a path with
# no line fails. `id -u` reports 0, so the launcher judges as root, the way the
# SysV firmware devices run it.
make_fake_stat_map() {
    mkdir -p "$BATS_TEST_TMPDIR/fakebin"
    cat > "$BATS_TEST_TMPDIR/fakebin/stat" <<'FAKE'
#!/bin/sh
[ "$1" = "-L" ] && [ "$2" = "-c" ] || exit 1
if [ "$3" = "%h" ]; then
    if [ -f "$BATS_TEST_TMPDIR/nlinkmap" ]; then
        while read -r p n; do
            [ "$p" = "$4" ] && { echo "$n"; exit 0; }
        done < "$BATS_TEST_TMPDIR/nlinkmap"
    fi
    echo 1
    exit 0
fi
while read -r p u m; do
    [ "$p" = "$4" ] && { echo "$u $m"; exit 0; }
done < "$BATS_TEST_TMPDIR/statmap"
exit 1
FAKE
    printf '#!/bin/sh\n[ "$1" = "-u" ] && echo 0 && exit 0\nexec /usr/bin/id "$@"\n' \
        > "$BATS_TEST_TMPDIR/fakebin/id"
    chmod +x "$BATS_TEST_TMPDIR/fakebin/stat" "$BATS_TEST_TMPDIR/fakebin/id"
}

# The Snapmaker U1 layout: config/helixscreen.env links into printer_data
# owned by the Klipper user (uid 1000), where Mainsail and Fluidd save it.
# Arguments are the modes/owners that vary per test.
make_u1_layout() {
    local link_dir_uid="$1" real_dir_mode="$2" file_uid="$3"
    mkdir -p "$BATS_TEST_TMPDIR/printer_data/config/helixscreen"
    printf 'MOONRAKER_HOST=u1-web-edited.local\n' \
        > "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env"
    ln -s "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env" \
        "$MOCK_INSTALL/config/helixscreen.env"
    local real real_dir
    real=$(readlink -f "$MOCK_INSTALL/config/helixscreen.env")
    real_dir="${real%/*}"
    {
        echo "$MOCK_INSTALL/config/helixscreen.env $file_uid 644"
        echo "$real $file_uid 644"
        echo "$MOCK_INSTALL/config $link_dir_uid 755"
        echo "$real_dir 1000 $real_dir_mode"
    } > "$BATS_TEST_TMPDIR/statmap"
}

run_launcher_as_root() {
    env -u MOONRAKER_HOST PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/gate.log"
}

@test "a root launcher loads a symlinked env file owned by the printer_data user (U1)" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat_map
    make_u1_layout 0 755 1000
    run_launcher_as_root
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "u1-web-edited.local" ]
    [ ! -s "$BATS_TEST_TMPDIR/gate.log" ]
}

@test "printer_data user trust needs a printer_data directory nobody else can write" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat_map
    make_u1_layout 0 775 1000
    run_launcher_as_root
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "owned by uid 1000" "$BATS_TEST_TMPDIR/gate.log"
}

@test "printer_data user trust needs the link itself placed by root or this user" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat_map
    make_u1_layout 12345 755 1000
    run_launcher_as_root
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "owned by uid 1000" "$BATS_TEST_TMPDIR/gate.log"
}

@test "a file in printer_data owned by someone other than its directory's owner is refused" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat_map
    make_u1_layout 0 755 12345
    run_launcher_as_root
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "owned by uid 12345" "$BATS_TEST_TMPDIR/gate.log"
    # The advice names the directory's owner, which keeps web editing working.
    grep -qF "chown 1000 " "$BATS_TEST_TMPDIR/gate.log"
}

@test "env file owned by the launcher's own user at 0644 loads" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'MOONRAKER_HOST=self-owned.local\n' > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    run env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST
    [ "$status" -eq 0 ]
    [ "$output" = "self-owned.local" ]
}

@test "root-owned 0644 env file loads while the launcher runs as a normal user" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat
    printf 'MOONRAKER_HOST=root-owned.local\n' > "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        FAKE_STAT_UID=0 FAKE_STAT_MODE=644 \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/gate.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "root-owned.local" ]
    [ ! -s "$BATS_TEST_TMPDIR/gate.log" ]
}

@test "group-writable env file owned by this user is repaired to 0644 and loaded" {
    # A write bit on a file the owner rule already accepted is a shipping
    # fault, not an attack: web updates and deploys land the file without
    # pinning it, and a refusal would blank settings on every update.
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'MOONRAKER_HOST=self-healed.local\n' \
        > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 664 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/heal.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "self-healed.local" ]
    grep -q "repaired .* from mode 664 to 0644" "$BATS_TEST_TMPDIR/heal.log"
    [ "$(stat -c '%a' "$MOCK_INSTALL/config/helixscreen.env")" = "644" ]
}

@test "world-writable env file stays refused when the mode cannot be repaired, and is never evaluated" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    mkdir -p "$BATS_TEST_TMPDIR/fakebin"
    printf '#!/bin/sh\nexit 1\n' > "$BATS_TEST_TMPDIR/fakebin/chmod"
    chmod +x "$BATS_TEST_TMPDIR/fakebin/chmod"
    printf 'MOONRAKER_HOST=$(touch "$BATS_TEST_TMPDIR/pwned")\n' \
        > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 666 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/refuse.log"
    # Refusal skips the file; it does not abort the launcher.
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "group- or world-writable" "$BATS_TEST_TMPDIR/refuse.log"
    grep -q "mode 666" "$BATS_TEST_TMPDIR/refuse.log"
    grep -qF "chmod 644 $MOCK_INSTALL/config/helixscreen.env" "$BATS_TEST_TMPDIR/refuse.log"
    # The repair could not run, so the command substitution in the refused
    # line never did either.
    [ ! -e "$BATS_TEST_TMPDIR/pwned" ]
    [ "$(stat -c '%a' "$MOCK_INSTALL/config/helixscreen.env")" = "666" ]
}

@test "a repair that does not change what stat reports still refuses the file" {
    # The re-stat after chmod is fail-closed: the fake stat keeps reporting
    # 0664 no matter what happened on disk, so the file is refused even though
    # the chmod itself succeeded.
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat
    printf 'MOONRAKER_HOST=never-loaded\n' > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        FAKE_STAT_UID=0 FAKE_STAT_MODE=664 \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/refuse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "group- or world-writable" "$BATS_TEST_TMPDIR/refuse.log"
}

@test "a symlinked env file is judged on its target, and loads at 0644" {
    # The per-file installs symlink config/helixscreen.env into
    # printer_data/config; a stat that did not dereference reads the link's
    # own 777 mode and refuses the file on every boot.
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    mkdir -p "$BATS_TEST_TMPDIR/printer_data/config/helixscreen"
    printf 'MOONRAKER_HOST=behind-symlink.local\n' \
        > "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env"
    chmod 644 "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env"
    ln -s "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env" \
        "$MOCK_INSTALL/config/helixscreen.env"
    run env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST
    [ "$status" -eq 0 ]
    [ "$output" = "behind-symlink.local" ]
}

@test "a symlinked env file with a writable target repairs the target, not the link" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    mkdir -p "$BATS_TEST_TMPDIR/printer_data/config/helixscreen"
    printf 'MOONRAKER_HOST=heal-through-link.local\n' \
        > "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env"
    chmod 664 "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env"
    ln -s "$BATS_TEST_TMPDIR/printer_data/config/helixscreen/helixscreen.env" \
        "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/heal.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "heal-through-link.local" ]
    # The repair landed on the real file the link points at, and the link
    # itself survives as a link.
    [ "$(stat -L -c '%a' "$MOCK_INSTALL/config/helixscreen.env")" = "644" ]
    [ -L "$MOCK_INSTALL/config/helixscreen.env" ]
    grep -q "repaired" "$BATS_TEST_TMPDIR/heal.log"
}

@test "env file owned by a third user is refused even at 0644" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat
    printf 'MOONRAKER_HOST=never-loaded\n' > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        FAKE_STAT_UID=12345 FAKE_STAT_MODE=644 \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/refuse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "owned by uid 12345" "$BATS_TEST_TMPDIR/refuse.log"
    grep -qF "chown root:root $MOCK_INSTALL/config/helixscreen.env && chmod 644 $MOCK_INSTALL/config/helixscreen.env" "$BATS_TEST_TMPDIR/refuse.log"
}

@test "env file whose owner or mode cannot be read is refused, not assumed safe" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat
    printf 'MOONRAKER_HOST=never-loaded\n' > "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" FAKE_STAT_FAIL=1 \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/refuse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "cannot determine owner/mode" "$BATS_TEST_TMPDIR/refuse.log"
}

# The launcher's --print-env answer for NAME, with the env file holding the
# literal text in $2.
print_env_from_file() {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf '%s\n' "$2" > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u "$1" "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env "$1" \
        2> "$BATS_TEST_TMPDIR/parse.log"
}

@test "a command substitution is never run, and its key is skipped" {
    local out
    out=$(print_env_from_file MOONRAKER_HOST "MOONRAKER_HOST=\$(touch $BATS_TEST_TMPDIR/pwned)")
    [ ! -e "$BATS_TEST_TMPDIR/pwned" ]
    [ "$out" = "" ]
    grep -q "MOONRAKER_HOST holds shell syntax" "$BATS_TEST_TMPDIR/parse.log"
}

@test "backticks are never run, and their key is skipped" {
    local out
    out=$(print_env_from_file MOONRAKER_HOST "MOONRAKER_HOST=\`touch $BATS_TEST_TMPDIR/pwned\`")
    [ ! -e "$BATS_TEST_TMPDIR/pwned" ]
    [ "$out" = "" ]
    grep -q "MOONRAKER_HOST holds shell syntax" "$BATS_TEST_TMPDIR/parse.log"
}

@test "a generated secret is skipped, never exported as its own recipe" {
    local out
    out=$(print_env_from_file HELIX_REMOTE_HTTP_TOKEN 'HELIX_REMOTE_HTTP_TOKEN=$(openssl rand -hex 16)')
    [ "$out" = "" ]
    out=$(print_env_from_file HELIX_REMOTE_HTTP_TOKEN 'HELIX_REMOTE_HTTP_TOKEN=${SECRET}')
    [ "$out" = "" ]
    grep -q "write the final value itself" "$BATS_TEST_TMPDIR/parse.log"
}

@test "a value spliced into the app's flags may not carry whitespace or globs" {
    [ "$(print_env_from_file HELIX_LOG_LEVEL 'HELIX_LOG_LEVEL="info --splash-pid 1"')" = "" ]
    grep -q "HELIX_LOG_LEVEL may not contain whitespace" "$BATS_TEST_TMPDIR/parse.log"
    [ "$(print_env_from_file HELIX_DPI 'HELIX_DPI=1*')" = "" ]
    [ "$(print_env_from_file HELIX_LOG_LEVEL 'HELIX_LOG_LEVEL=debug')" = "debug" ]
}

@test "the log file must live under /tmp, /var/log or the install dir" {
    [ "$(print_env_from_file HELIX_LOG_FILE 'HELIX_LOG_FILE=/tmp/helixscreen.log')" = "/tmp/helixscreen.log" ]
    mkdir -p "$MOCK_INSTALL/logs"
    [ "$(print_env_from_file HELIX_LOG_FILE "HELIX_LOG_FILE=$MOCK_INSTALL/logs/helix.log")" = "$MOCK_INSTALL/logs/helix.log" ]
    [ "$(print_env_from_file HELIX_LOG_FILE 'HELIX_LOG_FILE=/etc/profile')" = "" ]
    grep -q "HELIX_LOG_FILE must be a \*.log file" "$BATS_TEST_TMPDIR/parse.log"
    [ "$(print_env_from_file HELIX_LOG_FILE 'HELIX_LOG_FILE=/tmp/../etc/x.log')" = "" ]
    [ "$(print_env_from_file HELIX_LOG_FILE 'HELIX_LOG_FILE=/etc/init.d/x.log')" = "" ]
}

@test "a log path through a symlinked directory is judged where it lands" {
    local target="/tmp/helix-bats-$$-linkdir"
    ln -s /etc "$target"
    [ "$(print_env_from_file HELIX_LOG_FILE "HELIX_LOG_FILE=$target/x.log")" = "" ]
    rm -f "$target"
}

@test "a log directory under /tmp owned by another user is refused" {
    # A web-owned /tmp/d could be swapped for a symlink after the check.
    local d="/tmp/helix-bats-$$-logdir"
    mkdir -p "$d"
    make_fake_stat_map
    printf '%s 1000 755\n' "$d" > "$BATS_TEST_TMPDIR/statmap"
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'HELIX_LOG_FILE=%s/helix.log\n' "$d" > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    # The env file itself must still pass the gate as root-owned.
    printf '%s 0 644\n' "$MOCK_INSTALL/config/helixscreen.env" >> "$BATS_TEST_TMPDIR/statmap"
    env -u HELIX_LOG_FILE PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_LOG_FILE \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/parse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "HELIX_LOG_FILE must be" "$BATS_TEST_TMPDIR/parse.log"
    # The same directory owned by root loads.
    printf '%s 0 755\n%s 0 644\n' "$d" "$MOCK_INSTALL/config/helixscreen.env" > "$BATS_TEST_TMPDIR/statmap"
    env -u HELIX_LOG_FILE PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_LOG_FILE \
        > "$BATS_TEST_TMPDIR/value.out" 2>/dev/null
    rmdir "$d"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "$d/helix.log" ]
}

@test "an existing log file with a second hard link is refused" {
    # A hard link planted at an allowed path still shares its inode with a
    # file elsewhere: appending to the log would write that file as root.
    local d="/tmp/helix-bats-$$-hardlink"
    mkdir -p "$d"
    : > "$d/helix.log"
    ln "$d/helix.log" "$d/helix.log.2"
    make_fake_stat_map
    printf '%s 0 755\n%s 0 644\n%s 0 644\n' \
        "$d" "$d/helix.log" "$MOCK_INSTALL/config/helixscreen.env" \
        > "$BATS_TEST_TMPDIR/statmap"
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'HELIX_LOG_FILE=%s/helix.log\n' "$d" > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    # nlink 2: both names for the inode are refused.
    printf '%s/helix.log 2\n%s/helix.log.2 2\n' "$d" "$d" > "$BATS_TEST_TMPDIR/nlinkmap"
    env -u HELIX_LOG_FILE PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_LOG_FILE \
        > "$BATS_TEST_TMPDIR/value.out" 2> "$BATS_TEST_TMPDIR/parse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "HELIX_LOG_FILE must be" "$BATS_TEST_TMPDIR/parse.log"
    # nlink 1: the same single-link file loads.
    printf '%s/helix.log 1\n' "$d" > "$BATS_TEST_TMPDIR/nlinkmap"
    env -u HELIX_LOG_FILE PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_LOG_FILE \
        > "$BATS_TEST_TMPDIR/value.out" 2>/dev/null
    rm -f "$d/helix.log" "$d/helix.log.2"
    rmdir "$d"
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "$d/helix.log" ]
}

@test "ALSA device names are limited to hardware PCMs" {
    [ "$(print_env_from_file HELIX_ALSA_DEVICE 'HELIX_ALSA_DEVICE=plughw:CARD=vc4hdmi,DEV=0')" = "plughw:CARD=vc4hdmi,DEV=0" ]
    [ "$(print_env_from_file HELIX_ALSA_DEVICE 'HELIX_ALSA_DEVICE=default')" = "default" ]
    [ "$(print_env_from_file HELIX_ALSA_DEVICE "HELIX_ALSA_DEVICE=file:FILE='|touch /tmp/pwned'")" = "" ]
    grep -q "HELIX_ALSA_DEVICE must be default" "$BATS_TEST_TMPDIR/parse.log"
    [ "$(print_env_from_file HELIX_ALSA_DEVICE 'HELIX_ALSA_DEVICE=hw:0|x')" = "" ]
}

@test "HELIX_PWM_SOUND takes only <chip>:<channel>" {
    [ "$(print_env_from_file HELIX_PWM_SOUND 'HELIX_PWM_SOUND=0:0')" = "0:0" ]
    [ "$(print_env_from_file HELIX_PWM_SOUND 'HELIX_PWM_SOUND=2:13')" = "2:13" ]
    [ "$(print_env_from_file HELIX_PWM_SOUND 'HELIX_PWM_SOUND=0')" = "" ]
    grep -q "HELIX_PWM_SOUND must be <chip>:<channel>" "$BATS_TEST_TMPDIR/parse.log"
    [ "$(print_env_from_file HELIX_PWM_SOUND 'HELIX_PWM_SOUND=0:0:1')" = "" ]
    [ "$(print_env_from_file HELIX_PWM_SOUND 'HELIX_PWM_SOUND=:0')" = "" ]
    [ "$(print_env_from_file HELIX_PWM_SOUND 'HELIX_PWM_SOUND=0:')" = "" ]
    [ "$(print_env_from_file HELIX_PWM_SOUND 'HELIX_PWM_SOUND=0:a')" = "" ]
}

@test "HELIX_NICE takes 0-19 and HELIX_REMOTE_SOCKET a /tmp or /run path" {
    [ "$(print_env_from_file HELIX_NICE 'HELIX_NICE=5')" = "5" ]
    [ "$(print_env_from_file HELIX_NICE 'HELIX_NICE=19')" = "19" ]
    [ "$(print_env_from_file HELIX_NICE 'HELIX_NICE=-5')" = "" ]
    grep -q "HELIX_NICE must be 0-19" "$BATS_TEST_TMPDIR/parse.log"
    [ "$(print_env_from_file HELIX_NICE 'HELIX_NICE=20')" = "" ]
    [ "$(print_env_from_file HELIX_NICE 'HELIX_NICE=5x')" = "" ]
    [ "$(print_env_from_file HELIX_REMOTE_SOCKET 'HELIX_REMOTE_SOCKET=/tmp/helix.sock')" = "/tmp/helix.sock" ]
    [ "$(print_env_from_file HELIX_REMOTE_SOCKET 'HELIX_REMOTE_SOCKET=/run/helix.sock')" = "/run/helix.sock" ]
    [ "$(print_env_from_file HELIX_REMOTE_SOCKET 'HELIX_REMOTE_SOCKET=/etc/x.sock')" = "" ]
    [ "$(print_env_from_file HELIX_REMOTE_SOCKET 'HELIX_REMOTE_SOCKET=/tmp/../etc/x')" = "" ]
}

@test "directory keys that steer where the app loads code from are refused" {
    local k
    for k in HELIX_DATA_DIR HELIX_CONFIG_DIR HELIX_CACHE_DIR HELIX_TMP_DIR; do
        [ "$(print_env_from_file "$k" "$k=/home/lava/printer_data/config/x")" = "" ] ||
            fail "$k was exported"
        grep -q "$k is not a setting" "$BATS_TEST_TMPDIR/parse.log"
    done
}

@test "every key the shipped env template names is on the allowlist" {
    # A template key the parse refuses is a setting users edit to no effect.
    local keys k missing=""
    keys=$(grep -oE '^#?[ ]?[A-Z][A-Z0-9_]*=' "$WORKTREE_ROOT/config/helixscreen.env" |
        sed 's/^#[ ]\{0,1\}//; s/=$//' | sort -u)
    [ -n "$keys" ] || fail "no keys extracted from the template"
    echo "$keys" | grep -qx HELIX_LOG_LEVEL || fail "extraction missed a known key"
    for k in $keys; do
        sh -c ". '$BATS_TEST_TMPDIR/env_parse.sh'; helix_env_key_allowed '$k'" ||
            missing="$missing $k"
    done
    [ -z "$missing" ] || fail "template keys the parse refuses:$missing"
}

@test "--print-env takes the same names the file parse does" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    run "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env 1BAD
    [ "$status" -eq 2 ]
    [[ "$output" == *"not a variable name"* ]]
}

@test "a variable reference is never expanded, and its key is skipped" {
    local out
    out=$(HOME=/root print_env_from_file MOONRAKER_HOST 'MOONRAKER_HOST=$HOME/x')
    [ "$out" = "" ]
    grep -q "MOONRAKER_HOST holds shell syntax" "$BATS_TEST_TMPDIR/parse.log"
    [ "$(print_env_from_file MOONRAKER_HOST 'MOONRAKER_HOST=price$5')" = 'price$5' ]
}

@test "one pair of surrounding quotes is stripped and inner spaces are kept" {
    [ "$(print_env_from_file MOONRAKER_HOST 'MOONRAKER_HOST="two words"')" = "two words" ]
    [ "$(print_env_from_file MOONRAKER_HOST "MOONRAKER_HOST='single quoted'")" = "single quoted" ]
    [ "$(print_env_from_file MOONRAKER_HOST 'MOONRAKER_HOST="kept # hash" # note')" = "kept # hash" ]
    [ "$(print_env_from_file MOONRAKER_HOST 'MOONRAKER_HOST=bare value')" = "bare value" ]
}

@test "an unterminated quote is refused with a warning" {
    [ "$(print_env_from_file MOONRAKER_HOST 'MOONRAKER_HOST="open')" = "" ]
    grep -q "unterminated quote" "$BATS_TEST_TMPDIR/parse.log"
}

@test "LD_PRELOAD, PATH and other non-settings are refused, once per key" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'LD_PRELOAD=/tmp/evil.so\nLD_PRELOAD=/tmp/evil2.so\nPATH=/tmp/evil\nBASH_ENV=/tmp/x\nHELIX_FB_HTTP=/tmp/evil.py\n' \
        > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    run env -u LD_PRELOAD -u HELIX_FB_HTTP "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env LD_PRELOAD
    [ "$output" != "/tmp/evil.so" ]
    env -u LD_PRELOAD "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env PATH \
        > "$BATS_TEST_TMPDIR/path.out" 2> "$BATS_TEST_TMPDIR/refuse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/path.out")" != "/tmp/evil" ]
    [ "$(grep -c 'LD_PRELOAD is not a setting' "$BATS_TEST_TMPDIR/refuse.log")" = "1" ]
    grep -q 'PATH is not a setting' "$BATS_TEST_TMPDIR/refuse.log"
    grep -q 'BASH_ENV is not a setting' "$BATS_TEST_TMPDIR/refuse.log"
    grep -q 'HELIX_FB_HTTP is not a setting' "$BATS_TEST_TMPDIR/refuse.log"
}

@test "an allowlisted non-HELIX key is exported" {
    [ "$(print_env_from_file MALLOC_ARENA_MAX 'MALLOC_ARENA_MAX=4')" = "4" ]
}

@test "a symlink chain into printer_data does not earn the directory owner's trust" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat_map
    make_u1_layout 0 755 1000
    # Re-aim the install link through a middle link.
    local real
    real=$(readlink -f "$MOCK_INSTALL/config/helixscreen.env")
    ln -s "$real" "$BATS_TEST_TMPDIR/middle.env"
    rm "$MOCK_INSTALL/config/helixscreen.env"
    ln -s "$BATS_TEST_TMPDIR/middle.env" "$MOCK_INSTALL/config/helixscreen.env"
    run_launcher_as_root
    [ "$(cat "$BATS_TEST_TMPDIR/value.out")" = "" ]
    grep -q "owned by uid 1000" "$BATS_TEST_TMPDIR/gate.log"
}

# =============================================================================
# Launcher -> app handoff (prestonbrown/helixscreen#1712)
#
# A refusal only the log knows about changes nothing for the user, so the
# launcher exports HELIX_ENV_FILE_REFUSED (kind|detail|expected|path) /
# HELIX_ENV_LINES_SKIPPED before it starts helix-screen. --print-env NAME
# observes exactly the value the app would read: the same parse runs, then the
# variable is printed - and the handoff variables are unset before the parse,
# so an ambient value cannot forge a refusal.
# =============================================================================

@test "a refused env file is handed to the app as HELIX_ENV_FILE_REFUSED" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    make_fake_stat
    printf 'MOONRAKER_HOST=never-loaded\n' > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        FAKE_STAT_UID=12345 FAKE_STAT_MODE=644 \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_FILE_REFUSED \
        > "$BATS_TEST_TMPDIR/refused.out" 2> "$BATS_TEST_TMPDIR/refuse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/refused.out")" = \
        "owner|12345|root:root|$MOCK_INSTALL/config/helixscreen.env" ]
    # A whole-file refusal never reaches the parse, so no line skips either.
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        FAKE_STAT_UID=12345 FAKE_STAT_MODE=644 \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_LINES_SKIPPED \
        > "$BATS_TEST_TMPDIR/skipped.out" 2>/dev/null
    [ "$(cat "$BATS_TEST_TMPDIR/skipped.out")" = "" ]
}

@test "a mode refusal is handed to the app with the mode kind" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    mkdir -p "$BATS_TEST_TMPDIR/fakebin"
    printf '#!/bin/sh\nexit 1\n' > "$BATS_TEST_TMPDIR/fakebin/chmod"
    chmod +x "$BATS_TEST_TMPDIR/fakebin/chmod"
    printf 'MOONRAKER_HOST=never-loaded\n' > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 666 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        PATH="$BATS_TEST_TMPDIR/fakebin:$PATH" \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_FILE_REFUSED \
        > "$BATS_TEST_TMPDIR/refused.out" 2> "$BATS_TEST_TMPDIR/refuse.log"
    # kind|detail|expected|path: a mode problem carries no uid and no chown.
    [ "$(cat "$BATS_TEST_TMPDIR/refused.out")" = \
        "mode|||$MOCK_INSTALL/config/helixscreen.env" ]
}

@test "skipped lines are handed to the app as HELIX_ENV_LINES_SKIPPED" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'nonsense without an equals sign\nLD_PRELOAD=/tmp/evil.so\nHELIX_NICE=31\nMOONRAKER_HOST=ok.local\n' \
        > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST -u LD_PRELOAD \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_LINES_SKIPPED \
        > "$BATS_TEST_TMPDIR/skipped.out" 2> "$BATS_TEST_TMPDIR/parse.log"
    [ "$(cat "$BATS_TEST_TMPDIR/skipped.out")" = \
        "line 1:malformed line|LD_PRELOAD:not a setting this file may change|HELIX_NICE:must be 0-19 (a negative nice would let the UI starve Klipper)" ]
    # The parse continues past the skipped lines: the good key still loads,
    # and line skips are not a whole-file refusal.
    env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_FILE_REFUSED \
        > "$BATS_TEST_TMPDIR/refused.out" 2>/dev/null
    [ "$(cat "$BATS_TEST_TMPDIR/refused.out")" = "" ]
    env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env MOONRAKER_HOST \
        > "$BATS_TEST_TMPDIR/host.out" 2>/dev/null
    [ "$(cat "$BATS_TEST_TMPDIR/host.out")" = "ok.local" ]
}

@test "a clean env file hands off neither variable" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'MOONRAKER_HOST=clean.local\n' > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_FILE_REFUSED \
        > "$BATS_TEST_TMPDIR/refused.out" 2>/dev/null
    env -u MOONRAKER_HOST "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_LINES_SKIPPED \
        > "$BATS_TEST_TMPDIR/skipped.out" 2>/dev/null
    [ "$(cat "$BATS_TEST_TMPDIR/refused.out")" = "" ]
    [ "$(cat "$BATS_TEST_TMPDIR/skipped.out")" = "" ]
}

@test "the skipped-lines handoff is capped at 12 entries" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    : > "$MOCK_INSTALL/config/helixscreen.env"
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        echo "NOT_A_SETTING_$i=x"
    done >> "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_LINES_SKIPPED \
        > "$BATS_TEST_TMPDIR/skipped.out" 2>/dev/null
    # 12 real entries plus the sentinel row that announces the cap: 13 entries
    # join on 12 separators, and the 13th line onward never reaches the app.
    [ "$(tr -dc '|' < "$BATS_TEST_TMPDIR/skipped.out" | wc -c)" = "12" ]
    grep -q 'NOT_A_SETTING_12:' "$BATS_TEST_TMPDIR/skipped.out"
    ! grep -q 'NOT_A_SETTING_13:' "$BATS_TEST_TMPDIR/skipped.out"
    grep -q 'more skipped:' "$BATS_TEST_TMPDIR/skipped.out"
}

@test "a skipped-lines handoff at the cap exactly carries no sentinel" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    : > "$MOCK_INSTALL/config/helixscreen.env"
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
        echo "NOT_A_SETTING_$i=x"
    done >> "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    env -u MOONRAKER_HOST \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_LINES_SKIPPED \
        > "$BATS_TEST_TMPDIR/skipped.out" 2>/dev/null
    # The list is complete at 12 entries, so no sentinel row: the app words
    # the count as exact, never "at least 12".
    [ "$(tr -dc '|' < "$BATS_TEST_TMPDIR/skipped.out" | wc -c)" = "11" ]
    grep -q 'NOT_A_SETTING_12:' "$BATS_TEST_TMPDIR/skipped.out"
    ! grep -q 'more skipped' "$BATS_TEST_TMPDIR/skipped.out"
}

@test "an ambient handoff value cannot forge a refusal" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    printf 'MOONRAKER_HOST=clean.local\n' > "$MOCK_INSTALL/config/helixscreen.env"
    chmod 644 "$MOCK_INSTALL/config/helixscreen.env"
    HELIX_ENV_FILE_REFUSED=spoofed HELIX_ENV_LINES_SKIPPED=spoofed \
        env -u MOONRAKER_HOST \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_FILE_REFUSED \
        > "$BATS_TEST_TMPDIR/refused.out" 2>/dev/null
    HELIX_ENV_FILE_REFUSED=spoofed HELIX_ENV_LINES_SKIPPED=spoofed \
        env -u MOONRAKER_HOST \
        "$MOCK_INSTALL/bin/helix-launcher.sh" --print-env HELIX_ENV_LINES_SKIPPED \
        > "$BATS_TEST_TMPDIR/skipped.out" 2>/dev/null
    [ "$(cat "$BATS_TEST_TMPDIR/refused.out")" = "" ]
    [ "$(cat "$BATS_TEST_TMPDIR/skipped.out")" = "" ]
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
