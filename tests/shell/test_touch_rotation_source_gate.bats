#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_touch_rotation_source.py — the gate keeping the
# display backends' stored-touch-range check on the APPLIED display rotation
# rather than the `/display/rotate` config key.
#
# Why a lint carries this instead of a unit test: the gate it protects lives in
# create_input_pointer(), which needs a real fbdev or DRM device and cannot run
# headless, so no unit test can reach it.
#
# The key is only the REQUEST and differs from the applied rotation both ways:
# CLI/env rotation leaves the key at 0 on a rotated display (#1394 stays live),
# and a failed DRM->fbdev rotation fallback leaves the key set on a display that
# was never rotated (a legitimate stored range is discarded every boot).
#
# As with any ratchet, the quiet half matters as much as the catch half: a gate
# that fired on the prose mentions of the key in those files' own comments would
# be red on every commit and would get switched off.

GATE="scripts/check_touch_rotation_source.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src/api"

    # Both backends in their correct form: ask the display, and mention the key
    # only in prose.
    for f in display_backend_fbdev display_backend_drm; do
        cat > "$ROOT/src/api/$f.cpp" <<'EOF'
// Asked of the display, not of `/display/rotate`, because the key is only the
// request and differs from the applied rotation both ways.
void create_input_pointer() {
    const int applied_rotation = display_rotation_degrees();
    if (applied_rotation != 0) {
        ignore_stored_range();
    }
}
EOF
    done
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT"
}

# ------------------------------------------------------------- the quiet half

@test "passes when both backends ask the display" {
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK: 2 display backend(s)"* ]]
}

@test "a comment naming /display/rotate is not a violation" {
    # The real files both carry exactly this prose. If it tripped the gate the
    # gate would be useless.
    grep -q '/display/rotate' "$ROOT/src/api/display_backend_drm.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "display_is_rotated() satisfies it as well as display_rotation_degrees()" {
    sed -i 's/display_rotation_degrees()/display_is_rotated()/' \
        "$ROOT/src/api/display_backend_fbdev.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------- the catch half
#
# The two ways the gate can be defeated. Both must be red.

@test "flags a backend reverted to the config key" {
    cat > "$ROOT/src/api/display_backend_fbdev.cpp" <<'EOF'
void create_input_pointer() {
    const int applied_rotation = Config::get_instance()->get<int>("/display/rotate", 0);
    if (applied_rotation != 0) {
        ignore_stored_range();
    }
}
EOF
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"reads /display/rotate"* ]]
    [[ "$output" == *"display_backend_fbdev.cpp"* ]]
}

@test "flags a backend that dropped the helper without adding the key back" {
    # The subtler revert: the gate is simply deleted rather than swapped.
    cat > "$ROOT/src/api/display_backend_drm.cpp" <<'EOF'
void create_input_pointer() {
    apply_stored_range();
}
EOF
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"no call to display_rotation_degrees"* ]]
    [[ "$output" == *"display_backend_drm.cpp"* ]]
}

@test "flags a renamed or removed backend rather than passing vacuously" {
    rm "$ROOT/src/api/display_backend_drm.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing"* ]]
}

# ------------------------------------------------------------- the real tree

@test "the checked-in backends satisfy the gate" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}
