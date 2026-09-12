#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_rotation_cache_order.py — the source-order gate
# keeping DisplayManager's resolution cache behind set_display_rotation().
# apply_rotation's body is #ifdef'd out of the test binary (HELIX_DISPLAY_SDL),
# so a lint is the only thing that makes a wrong-order revert fail.
#
# Every case here mutates one thing in a source the gate otherwise accepts, so
# a red result is attributable to that mutation and nothing else. The baseline
# case proves the unmutated source passes.

load helpers

SCRIPT="scripts/check_rotation_cache_order.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TMP_DIR="$BATS_TMPDIR/rotation-cache-order-$$"
    mkdir -p "$TMP_DIR"
    SRC="$TMP_DIR/display_manager.cpp"
}

teardown() {
    rm -rf "$TMP_DIR"
}

# Write a source holding everything the gate's census requires: init and
# apply_rotation with one settle/cache pair each, run_rotation_probe with two.
# Any argument after the output path replaces apply_rotation's body.
#
# init carries the `#ifndef HELIX_DISPLAY_SDL` guard the real file has and
# run_rotation_probe a format string with braces in it, so every case here
# also exercises the parser against the two things that can end a function
# body in the wrong place.
write_source() {
    local out="$1"
    shift
    {
        printf '%s\n' \
            'bool DisplayManager::init(const Config& config) {' \
            '#ifndef HELIX_DISPLAY_SDL' \
            '    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
            '    m_width = lv_display_get_horizontal_resolution(m_display);' \
            '    m_height = lv_display_get_vertical_resolution(m_display);' \
            '#endif' \
            '    return true;' \
            '}' \
            '' \
            'void DisplayManager::apply_rotation(int degrees) {'
        if [ "$#" -eq 0 ]; then
            printf '%s\n' \
                '    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
                '    m_width = lv_display_get_horizontal_resolution(m_display);' \
                '    m_height = lv_display_get_vertical_resolution(m_display);'
        else
            printf '    %s\n' "$@"
        fi
        printf '%s\n' \
            '}' \
            '' \
            'void DisplayManager::run_rotation_probe() {' \
            '    for (int i = 0; i < num_rotations; i++) {' \
            '        m_backend->set_display_rotation(rotations[i], phys_w, phys_h);' \
            '        m_width = lv_display_get_horizontal_resolution(m_display);' \
            '        m_height = lv_display_get_vertical_resolution(m_display);' \
            '        spdlog::info("[DisplayManager] probe {}x{}", m_width, m_height);' \
            '    }' \
            '    m_backend->set_display_rotation(confirmed_lv_rot, phys_w, phys_h);' \
            '    m_width = lv_display_get_horizontal_resolution(m_display);' \
            '    m_height = lv_display_get_vertical_resolution(m_display);' \
            '}'
    } > "$out"
}

@test "gate passes on the real display_manager.cpp" {
    run python3 "$SCRIPT"
    [ "$status" -eq 0 ]
    # The count is the difference between a pass and a gate looking at nothing.
    contains "4 guarded set_display_rotation()/cache pair(s)" "$output"
}

@test "the synthetic source every other case mutates passes unmutated" {
    write_source "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 0 ]
    contains "OK:" "$output"
}

@test "gate fails when a function caches the resolution before settling rotation" {
    write_source "$SRC" \
        'm_width = lv_display_get_horizontal_resolution(m_display);' \
        'm_height = lv_display_get_vertical_resolution(m_display);' \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "before set_display_rotation()" "$output"
    contains "apply_rotation()" "$output"
}

@test "gate fails when the guarded function is renamed away" {
    write_source "$TMP_DIR/base.cpp"
    sed 's/DisplayManager::apply_rotation/DisplayManager::apply_display_rotation/' \
        "$TMP_DIR/base.cpp" > "$SRC"
    refute cmp -s "$TMP_DIR/base.cpp" "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "DisplayManager::apply_rotation() is not defined here" "$output"
}

@test "gate fails when the guarded pair is extracted into a helper" {
    write_source "$SRC" \
        'cache_effective_resolution();' \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
        '}' \
        '' \
        'void DisplayManager::cache_effective_resolution() {' \
        'm_width = lv_display_get_horizontal_resolution(m_display);' \
        'm_height = lv_display_get_vertical_resolution(m_display);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "DisplayManager::apply_rotation() performs 0 set_display_rotation()/cache pair(s)" "$output"
}

@test "gate fails when the settle call is routed through a wrapper" {
    # The order rules match the callee by name, so the wrapper hides the call
    # from them entirely and the function looks like the no-settle exemption.
    # The census is what refuses to call that a pass.
    write_source "$SRC" \
        'm_width = lv_display_get_horizontal_resolution(m_display);' \
        'm_height = lv_display_get_vertical_resolution(m_display);' \
        'settle_backend_rotation(lv_rot, phys_w, phys_h);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "DisplayManager::apply_rotation() performs 0 set_display_rotation()/cache pair(s)" "$output"
    lacks "OK:" "$output"
}

@test "a line comment naming set_display_rotation does not count as the settle call" {
    write_source "$SRC" \
        '// set_display_rotation() would un-swap this, but none runs here.' \
        'm_width = lv_display_get_horizontal_resolution(m_display);' \
        'm_height = lv_display_get_vertical_resolution(m_display);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "DisplayManager::apply_rotation() performs 0 set_display_rotation()/cache pair(s)" "$output"
}

@test "a block comment naming set_display_rotation does not satisfy the order" {
    write_source "$SRC" \
        '/* the backend settles this with set_display_rotation(rot, w, h) below */' \
        'm_width = lv_display_get_horizontal_resolution(m_display);' \
        'm_height = lv_display_get_vertical_resolution(m_display);' \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "before set_display_rotation()" "$output"
    contains "apply_rotation()" "$output"
}

@test "a cache write wrapped onto two lines is still seen" {
    # clang-format breaks after `m_width =` when the line is long enough, so
    # the write has to be found across the break as well as on one line.
    write_source "$SRC" \
        'm_width =' \
        '    lv_display_get_horizontal_resolution(m_display);' \
        'm_height =' \
        '    lv_display_get_vertical_resolution(m_display);' \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "before set_display_rotation()" "$output"
}

@test "a helper defined after a guarded method does not inherit its settle call" {
    # Function bodies are brace-matched, so the helper is judged on its own
    # contents rather than on the settle call in the method above it.
    write_source "$TMP_DIR/base.cpp"
    {
        cat "$TMP_DIR/base.cpp"
        printf '%s\n' \
            '' \
            'namespace {' \
            'void refresh_after_rotate(DisplayManager* dm) {' \
            '    dm->m_width = lv_display_get_horizontal_resolution(dm->m_display);' \
            '    dm->backend()->set_display_rotation(dm->rotation(), phys_w, phys_h);' \
            '}' \
            '}  // namespace'
    } > "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "refresh_after_rotate()" "$output"
}

@test "a third wrongly ordered pair cannot borrow an earlier settle call" {
    write_source "$TMP_DIR/base.cpp"
    {
        cat "$TMP_DIR/base.cpp"
        printf '%s\n' \
            '' \
            'void DisplayManager::restore_rotation() {' \
            '    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
            '    m_width = lv_display_get_horizontal_resolution(m_display);' \
            '    m_height = lv_display_get_vertical_resolution(m_display);' \
            '' \
            '    m_width = lv_display_get_horizontal_resolution(m_display);' \
            '    m_height = lv_display_get_vertical_resolution(m_display);' \
            '    m_backend->set_display_rotation(confirmed_lv_rot, phys_w, phys_h);' \
            '}'
    } > "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "before set_display_rotation() in DisplayManager::restore_rotation()" "$output"
}

@test "a settle call with no cache read after it is reported" {
    write_source "$SRC" \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
        'spdlog::info("[DisplayManager] rotated {}", degrees);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "is followed by no resolution cache read" "$output"
}

@test "ROTATION_CACHE_OK opts a settle call out of the no-cache-read finding" {
    write_source "$SRC" \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
        'm_width = lv_display_get_horizontal_resolution(m_display);' \
        'm_height = lv_display_get_vertical_resolution(m_display);' \
        '' \
        '// ROTATION_CACHE_OK: the caller re-reads the resolution itself' \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 0 ]
}

@test "gate stays quiet on a cache refresh with no rotation settle" {
    # resize_timer_cb's shape: an external resize (Android fold/unfold) is the
    # thing being caught up to; no settle call exists to order against.
    write_source "$TMP_DIR/base.cpp"
    {
        cat "$TMP_DIR/base.cpp"
        printf '%s\n' \
            '' \
            'void DisplayManager::resize_timer_cb(lv_timer_t* timer) {' \
            '    auto* self = static_cast<DisplayManager*>(lv_timer_get_user_data(timer));' \
            '    self->m_width = lv_display_get_horizontal_resolution(self->m_display);' \
            '    self->m_height = lv_display_get_vertical_resolution(self->m_display);' \
            '}'
    } > "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 0 ]
}

@test "gate catches a definition whose return type wraps to its own line" {
    # clang-format leaves `DisplayManager::name(` at column 0 when the return
    # type is long; the definition must still be found.
    write_source "$TMP_DIR/base.cpp"
    {
        cat "$TMP_DIR/base.cpp"
        printf '%s\n' \
            '' \
            'std::pair<int, int>' \
            'DisplayManager::effective_resolution(int degrees) {' \
            '    m_width = lv_display_get_horizontal_resolution(m_display);' \
            '    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
            '    return {m_width, m_height};' \
            '}'
    } > "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "DisplayManager::effective_resolution()" "$output"
}

@test "--rules-only still enforces the ordering it is named for" {
    # The flag exists so this file can feed the rules a source that is not a
    # whole display_manager.cpp. It must not be a way to pass the gate.
    write_source "$SRC" \
        'm_width = lv_display_get_horizontal_resolution(m_display);' \
        'm_backend->set_display_rotation(lv_rot, phys_w, phys_h);'
    run python3 "$SCRIPT" --file "$SRC" --rules-only
    [ "$status" -eq 1 ]
    contains "before set_display_rotation()" "$output"
}

@test "the pre-commit gate runs the census, not just the ordering rules" {
    run grep -n "check_rotation_cache_order.py" scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    lacks "--rules-only" "$output"
}

@test "gate reports a missing file without a verdict" {
    run python3 "$SCRIPT" --file "$TMP_DIR/no-such-file.cpp"
    [ "$status" -eq 2 ]
}
