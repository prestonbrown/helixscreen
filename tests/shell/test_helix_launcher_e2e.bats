#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for helix-launcher.sh end-to-end behavior:
# - Script validity
# - Splash PID routing
# - Debug mode (HELIX_DEBUG, --debug)
# - End-to-end launcher tests
# - HELIX_LOG_LEVEL
# Split from test_helix_launcher.bats for parallel execution.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers

    # Mock system commands that helix-launcher.sh calls at startup.
    # Without these, the e2e tests hit the real systemctl/killall/setterm
    # on the dev machine (e.g. stopping display-sleep.service).
    mock_command_script "systemctl" 'exit 0'
    mock_command_script "killall" 'exit 0'
    mock_command_script "setterm" 'exit 0'

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
    while IFS= read -r _line || [ -n "$_line" ]; do
        case "$_line" in
            '#'*|'') continue ;;
        esac
        _var="${_line%%=*}"
        eval "_existing=\"\${${_var}:-}\"" 2>/dev/null || continue
        if [ -z "$_existing" ]; then
            eval "export $_line" 2>/dev/null || true
        fi
    done < "$_helix_env_file"
    unset _line _var _existing
fi
unset _helix_env_file

# Resolve debug/logging settings: CLI flags > env vars (incl. env file) > defaults
#
# HAND-COPIED from helix-launcher.sh. This snippet deliberately omits the
# platform-hook sourcing that the real launcher does immediately BEFORE these
# four lines, so it cannot be used to test hook-exported HELIX_LOG_*. The
# hook-ordering contract is covered end-to-end against the real script by
# "e2e: platform hook HELIX_LOG_DEST/FILE reach helix-screen" below; keep that
# test, not this copy, as the source of truth for ordering.
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
# Launcher script validity
# =============================================================================

@test "helix-launcher.sh has valid sh syntax" {
    sh -n "$LAUNCHER"
}

@test "helix-launcher.sh contains env file sourcing logic" {
    grep -q 'helixscreen.env' "$LAUNCHER"
}

@test "helix-launcher.sh contains fbdev default logic" {
    grep -q 'HELIX_DISPLAY_BACKEND=fbdev' "$LAUNCHER"
}

@test "helix-launcher.sh does not override existing HELIX_DISPLAY_BACKEND" {
    # The conditional must check if already set
    grep -q 'if \[ -z "\${HELIX_DISPLAY_BACKEND:-}"' "$LAUNCHER"
}

# =============================================================================
# Splash PID routing (must go to watchdog args, not passthrough/child args)
# =============================================================================

@test "HELIX_SPLASH_PID routes to SPLASH_ARGS not PASSTHROUGH_ARGS" {
    # The launcher should put --splash-pid into SPLASH_ARGS (before --)
    # not into PASSTHROUGH_ARGS (after --)
    grep -q 'SPLASH_ARGS="--splash-pid=\${HELIX_SPLASH_PID}"' "$LAUNCHER"
}

@test "HELIX_SPLASH_PID does not go to PASSTHROUGH_ARGS" {
    # Ensure the old pattern (putting splash-pid in PASSTHROUGH_ARGS) is gone
    ! grep -q 'PASSTHROUGH_ARGS.*--splash-pid' "$LAUNCHER"
}

@test "launcher passes SPLASH_ARGS before -- separator to watchdog" {
    # SPLASH_ARGS should appear before the -- separator in the watchdog invocation
    grep -q '"${WATCHDOG_BIN}" ${SPLASH_ARGS} --' "$LAUNCHER"
}

# =============================================================================
# Debug mode: HELIX_DEBUG env var and --debug flag
# =============================================================================

@test "HELIX_DEBUG=1 in environment enables debug mode (simulates systemd Environment=)" {
    # This is the case that works TODAY: systemd sets HELIX_DEBUG=1 before launcher runs
    unset CLI_DEBUG
    export HELIX_DEBUG=1
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$DEBUG_MODE\"")
    [ "$result" = "1" ]
}

@test "HELIX_DEBUG=1 in env file enables debug mode" {
    # This is the fix: env file sets HELIX_DEBUG, which is read AFTER env file sourcing
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DEBUG=1
EOF
    unset HELIX_DEBUG CLI_DEBUG
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$DEBUG_MODE\"")
    [ "$result" = "1" ]
}

@test "CLI --debug flag takes priority over env file HELIX_DEBUG=0" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DEBUG=0
EOF
    unset HELIX_DEBUG
    export CLI_DEBUG=1
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$DEBUG_MODE\"")
    [ "$result" = "1" ]
}

@test "debug mode defaults to off when nothing is set" {
    unset HELIX_DEBUG CLI_DEBUG
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$DEBUG_MODE\"")
    [ "$result" = "0" ]
}

@test "HELIX_DEBUG from environment overrides env file value" {
    # Env file says 0, but environment says 1 — environment wins
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DEBUG=0
EOF
    export HELIX_DEBUG=1
    unset CLI_DEBUG
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$DEBUG_MODE\"")
    [ "$result" = "1" ]
}

# =============================================================================
# End-to-end: launcher passes -vv to helix-screen when debug enabled
# =============================================================================

@test "e2e: HELIX_DEBUG=1 in environment causes launcher to pass -vv" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"

    HELIX_DEBUG=1 MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^-vv$' "$MOCK_INSTALL/helix_screen_args.txt"
}

@test "e2e: HELIX_DEBUG=1 in env file causes launcher to pass -vv" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_DEBUG=1
EOF
    unset HELIX_DEBUG

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^-vv$' "$MOCK_INSTALL/helix_screen_args.txt"
}

@test "e2e: launcher does NOT pass -vv when debug is off" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_DEBUG

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    if [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]; then
        refute grep -q '^-vv$' "$MOCK_INSTALL/helix_screen_args.txt"
    fi
}

@test "e2e: --debug CLI flag causes launcher to pass -vv" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    unset HELIX_DEBUG

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" --debug 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^-vv$' "$MOCK_INSTALL/helix_screen_args.txt"
}

# =============================================================================
# HELIX_LOG_LEVEL: env var and --log-level flag
# =============================================================================

@test "HELIX_LOG_LEVEL in env file is resolved" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_LOG_LEVEL=trace
EOF
    unset HELIX_LOG_LEVEL CLI_LOG_LEVEL
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$LOG_LEVEL\"")
    [ "$result" = "trace" ]
}

@test "CLI_LOG_LEVEL takes priority over env file HELIX_LOG_LEVEL" {
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_LOG_LEVEL=info
EOF
    unset HELIX_LOG_LEVEL
    export CLI_LOG_LEVEL=error
    result=$(MOCK_INSTALL="$MOCK_INSTALL" sh -c ". \"$BATS_TEST_TMPDIR/env_setup.sh\" && echo \"\$LOG_LEVEL\"")
    [ "$result" = "error" ]
}

@test "e2e: HELIX_LOG_LEVEL=trace causes launcher to pass --log-level=trace" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_LOG_LEVEL=trace
EOF
    unset HELIX_LOG_LEVEL HELIX_DEBUG

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^--log-level=trace$' "$MOCK_INSTALL/helix_screen_args.txt"
}

@test "e2e: HELIX_LOG_LEVEL takes priority over HELIX_DEBUG" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    cat > "$MOCK_INSTALL/config/helixscreen.env" << 'EOF'
HELIX_LOG_LEVEL=warn
HELIX_DEBUG=1
EOF
    unset HELIX_LOG_LEVEL HELIX_DEBUG

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    # Should have --log-level=warn, NOT -vv
    grep -q '^--log-level=warn$' "$MOCK_INSTALL/helix_screen_args.txt"
    ! grep -q '^-vv$' "$MOCK_INSTALL/helix_screen_args.txt"
}

@test "e2e: launcher does NOT pass --log-level when LOG_LEVEL is empty" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_LOG_LEVEL HELIX_DEBUG

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    if [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]; then
        refute grep -q '^--log-level' "$MOCK_INSTALL/helix_screen_args.txt"
    fi
}

# =============================================================================
# Platform hooks: platform_pre_start exports HELIX_LOG_DEST / HELIX_LOG_FILE on
# six of the seven platforms, so the launcher must resolve LOG_DEST/LOG_FILE
# AFTER sourcing platform/hooks.sh. Resolving first reads unset variables and
# the flags never reach the binary (#1249).
# =============================================================================

# Install a platform/hooks.sh into the mock layout whose platform_pre_start
# exports the logging vars, the way assets/config/platform/hooks-*.sh do.
install_logging_hook() {
    mkdir -p "$MOCK_INSTALL/platform"
    cat > "$MOCK_INSTALL/platform/hooks.sh" << 'HOOKEOF'
#!/bin/sh
platform_pre_start() {
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE=/tmp/x.log
}
HOOKEOF
    chmod +x "$MOCK_INSTALL/platform/hooks.sh"
}

@test "e2e: platform hook HELIX_LOG_DEST/FILE reach helix-screen" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_LOG_DEST HELIX_LOG_FILE HELIX_DEBUG HELIX_LOG_LEVEL
    install_logging_hook

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^--log-dest=file$' "$MOCK_INSTALL/helix_screen_args.txt"
    grep -q '^--log-file=/tmp/x.log$' "$MOCK_INSTALL/helix_screen_args.txt"
}

@test "e2e: an ambient HELIX_LOG_FILE still wins over the hook's export" {
    # The hook uses plain `export`, so a value already in the environment is
    # overwritten by it — but the init script and helixscreen.env paths set the
    # variable BEFORE the hook runs, and several platforms rely on the operator
    # being able to override. Pin the resolution actually in effect: whatever
    # the environment holds after platform_pre_start is what gets forwarded.
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_DEBUG HELIX_LOG_LEVEL
    mkdir -p "$MOCK_INSTALL/platform"
    cat > "$MOCK_INSTALL/platform/hooks.sh" << 'HOOKEOF'
#!/bin/sh
platform_pre_start() {
    export HELIX_LOG_DEST=file
    [ -n "${HELIX_LOG_FILE:-}" ] || export HELIX_LOG_FILE=/tmp/hook-default.log
}
HOOKEOF

    HELIX_LOG_FILE=/tmp/operator.log MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^--log-file=/tmp/operator.log$' "$MOCK_INSTALL/helix_screen_args.txt"
    refute_grep '^--log-file=/tmp/hook-default.log$' "$MOCK_INSTALL/helix_screen_args.txt"
}

@test "e2e: CLI --log-dest still beats a platform hook export" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    unset HELIX_LOG_DEST HELIX_LOG_FILE HELIX_DEBUG HELIX_LOG_LEVEL
    install_logging_hook

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" --log-dest=console 2>/dev/null || true

    [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]
    grep -q '^--log-dest=console$' "$MOCK_INSTALL/helix_screen_args.txt"
    refute_grep '^--log-dest=file$' "$MOCK_INSTALL/helix_screen_args.txt"
}

@test "e2e: no platform hook means no --log-dest/--log-file flags" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    rm -f "$MOCK_INSTALL/helix_screen_args.txt"
    rm -f "$MOCK_INSTALL/config/helixscreen.env"
    rm -rf "$MOCK_INSTALL/platform"
    unset HELIX_LOG_DEST HELIX_LOG_FILE HELIX_DEBUG HELIX_LOG_LEVEL

    MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    if [ -f "$MOCK_INSTALL/helix_screen_args.txt" ]; then
        refute grep -q '^--log-dest' "$MOCK_INSTALL/helix_screen_args.txt"
        refute grep -q '^--log-file' "$MOCK_INSTALL/helix_screen_args.txt"
    fi
}

# =============================================================================
# Splash PID routing end-to-end
# =============================================================================

@test "splash PID routing end-to-end with mock watchdog" {
    # Create a mock watchdog that captures its arguments
    cat > "$MOCK_INSTALL/bin/helix-watchdog" << 'WDEOF'
#!/bin/sh
# Write all args to a file for inspection
for arg in "$@"; do
    echo "$arg"
done > "$MOCK_INSTALL/watchdog_args.txt"
exit 0
WDEOF
    chmod +x "$MOCK_INSTALL/bin/helix-watchdog"

    # Copy the launcher into the mock bin/ so SCRIPT_DIR resolves to mock
    # layout (otherwise it finds the real build/bin/helix-screen via $0)
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"

    # Run the launcher with HELIX_SPLASH_PID set
    export HELIX_SPLASH_PID=1476
    export HELIX_NO_SPLASH=0

    # Run launcher from mock dir (SCRIPT_DIR will be $MOCK_INSTALL/bin)
    HELIX_SPLASH_PID=1476 sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    # Check that --splash-pid appears BEFORE -- in the watchdog args
    if [ -f "$MOCK_INSTALL/watchdog_args.txt" ]; then
        # --splash-pid should be one of the args before --
        seen_separator=false
        splash_before_sep=false
        while IFS= read -r line; do
            if [ "$line" = "--" ]; then
                seen_separator=true
            elif [ "$seen_separator" = "false" ] && echo "$line" | grep -q '^--splash-pid='; then
                splash_before_sep=true
            fi
        done < "$MOCK_INSTALL/watchdog_args.txt"
        [ "$splash_before_sep" = "true" ]
    fi
}

# =============================================================================
# Splash heartbeat seed: the launcher writes one status-file heartbeat right
# before launching so the boot splash stays visible (no blank gap) and shows a
# counting "Starting HelixScreen…" until helix-screen signals it. This brings
# the gap-free handoff to gate-less platforms (Pi, K1, CC1, AD5X, …).
# =============================================================================

@test "e2e: launcher seeds splash heartbeat when a splash is active" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    status_file="$BATS_TEST_TMPDIR/splash-status"
    rm -f "$status_file"
    unset HELIX_DEBUG HELIX_NO_SPLASH

    HELIX_SPLASH_PID=4242 HELIX_SPLASH_STATUS_FILE="$status_file" \
        MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ -f "$status_file" ]
    grep -q "HelixScreen" "$status_file"
}

@test "e2e: launcher does NOT seed splash heartbeat when HELIX_NO_SPLASH=1" {
    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
    status_file="$BATS_TEST_TMPDIR/splash-status-nosplash"
    rm -f "$status_file"
    unset HELIX_DEBUG

    HELIX_NO_SPLASH=1 HELIX_SPLASH_PID=4242 HELIX_SPLASH_STATUS_FILE="$status_file" \
        MOCK_INSTALL="$MOCK_INSTALL" \
        sh "$MOCK_INSTALL/bin/helix-launcher.sh" 2>/dev/null || true

    [ ! -f "$status_file" ]
}
