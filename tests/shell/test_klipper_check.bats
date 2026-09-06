#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for check_klipper_ecosystem() in scripts/lib/installer/requirements.sh

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Override log stubs so we can assert on output
    log_error()   { echo "ERROR: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_error log_info log_warn log_success

    # Reset source guard
    unset _HELIX_REQUIREMENTS_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/requirements.sh"
}

# Helper: write a ps mock that outputs the given file content
# Args: file containing ps output lines
setup_ps_mock() {
    local ps_file="$1"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/ps" <<MOCK
#!/bin/sh
cat "$ps_file"
MOCK
    chmod +x "$BATS_TEST_TMPDIR/bin/ps"
}

# Helper: run check_klipper_ecosystem in a subshell with controlled ps/wget
# Args: platform, ps_content_file, wget_exit_code (0=success)
run_klipper_check() {
    local platform="$1"
    local ps_file="$2"
    local wget_exit="${3:-0}"

    mkdir -p "$BATS_TEST_TMPDIR/bin"

    # Mock ps
    setup_ps_mock "$ps_file"

    # Mock wget
    printf '#!/bin/sh\nexit %d\n' "$wget_exit" > "$BATS_TEST_TMPDIR/bin/wget"
    chmod +x "$BATS_TEST_TMPDIR/bin/wget"

    # Hide curl so wget path is taken
    printf '#!/bin/sh\nexit 127\n' > "$BATS_TEST_TMPDIR/bin/curl"
    chmod +x "$BATS_TEST_TMPDIR/bin/curl"

    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"

    # Run non-interactively (stdin not a tty → non-interactive path)
    run check_klipper_ecosystem "$platform"
}

# ===========================================================================
# Platform filtering
# ===========================================================================

@test "check_klipper_ecosystem: skips on pi platform" {
    run check_klipper_ecosystem "pi"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "check_klipper_ecosystem: skips on pi32 platform" {
    run check_klipper_ecosystem "pi32"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ===========================================================================
# Happy path: both services running and responsive
# ===========================================================================

@test "check_klipper_ecosystem: passes when both klipper and moonraker running" {
    local ps_file="$BATS_TEST_TMPDIR/ps_output"
    cat > "$ps_file" <<'EOF'
  PID USER     COMMAND
  123 root     klippy/klippy.py
  456 root     moonraker/moonraker.py
EOF

    run_klipper_check "ad5m" "$ps_file" 0
    [ "$status" -eq 0 ]
    [[ "$output" == *"Klipper and Moonraker are running"* ]]
}

@test "check_klipper_ecosystem: works for k1 platform too" {
    local ps_file="$BATS_TEST_TMPDIR/ps_output"
    cat > "$ps_file" <<'EOF'
  PID USER     COMMAND
  123 root     klippy/klippy.py
  456 root     moonraker/moonraker.py
EOF

    run_klipper_check "k1" "$ps_file" 0
    [ "$status" -eq 0 ]
    [[ "$output" == *"Klipper and Moonraker are running"* ]]
}

# ===========================================================================
# Warning cases (non-interactive continues with warning)
# ===========================================================================

@test "check_klipper_ecosystem: warns when klipper not running" {
    local ps_file="$BATS_TEST_TMPDIR/ps_output"
    cat > "$ps_file" <<'EOF'
  PID USER     COMMAND
  456 root     moonraker/moonraker.py
EOF

    run_klipper_check "ad5m" "$ps_file" 0
    [ "$status" -eq 0 ]
    contains "Klipper does not appear to be running" "$output"
    [[ "$output" == *"Non-interactive mode"* ]]
}

@test "check_klipper_ecosystem: warns when moonraker not running" {
    local ps_file="$BATS_TEST_TMPDIR/ps_output"
    cat > "$ps_file" <<'EOF'
  PID USER     COMMAND
  123 root     klippy/klippy.py
EOF

    run_klipper_check "ad5m" "$ps_file" 0
    [ "$status" -eq 0 ]
    contains "Moonraker does not appear to be running" "$output"
    [[ "$output" == *"Non-interactive mode"* ]]
}

@test "check_klipper_ecosystem: warns when neither running" {
    local ps_file="$BATS_TEST_TMPDIR/ps_output"
    cat > "$ps_file" <<'EOF'
  PID USER     COMMAND
    1 root     init
EOF

    run_klipper_check "ad5m" "$ps_file" 0
    [ "$status" -eq 0 ]
    contains "Klipper does not appear to be running" "$output"
    [[ "$output" == *"Moonraker does not appear to be running"* ]]
}

@test "check_klipper_ecosystem: warns when moonraker running but not responding" {
    local ps_file="$BATS_TEST_TMPDIR/ps_output"
    cat > "$ps_file" <<'EOF'
  PID USER     COMMAND
  123 root     klippy/klippy.py
  456 root     moonraker/moonraker.py
EOF

    run_klipper_check "ad5m" "$ps_file" 1
    [ "$status" -eq 0 ]
    contains "not responding on http://127.0.0.1:7125" "$output"
    [[ "$output" == *"Non-interactive mode"* ]]
}

# ===========================================================================
# Interactive prompt (use HELIX_PREFLIGHT_RESPONSE env var to simulate)
# Note: We can't truly test tty interaction in bats, but we can test
# the read path by checking that non-interactive mode works correctly.
# ===========================================================================

@test "check_klipper_ecosystem: non-interactive mode continues with warning" {
    local ps_file="$BATS_TEST_TMPDIR/ps_output"
    cat > "$ps_file" <<'EOF'
  PID USER     COMMAND
    1 root     init
EOF

    run_klipper_check "ad5m" "$ps_file" 0
    [ "$status" -eq 0 ]
    [[ "$output" == *"Non-interactive mode: continuing anyway"* ]]
}
