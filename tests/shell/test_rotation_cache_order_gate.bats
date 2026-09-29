#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_rotation_cache_order.py — the source-order gate
# keeping DisplayManager's resolution cache behind set_display_rotation().
# apply_rotation's body is #ifdef'd out of the test binary (HELIX_DISPLAY_SDL),
# so a lint is the only thing that makes a wrong-order revert fail.

load helpers

SCRIPT="scripts/check_rotation_cache_order.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TMP_DIR="$BATS_TMPDIR/rotation-cache-order-$$"
    mkdir -p "$TMP_DIR"
}

teardown() {
    rm -rf "$TMP_DIR"
}

@test "gate passes on the real display_manager.cpp" {
    run python3 "$SCRIPT"
    [ "$status" -eq 0 ]
}

@test "gate fails when a function caches the resolution before settling rotation" {
    printf '%s\n' \
        'void DisplayManager::apply_rotation(int degrees) {' \
        '    m_width = lv_display_get_horizontal_resolution(m_display);' \
        '    m_height = lv_display_get_vertical_resolution(m_display);' \
        '    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
        '}' > "$TMP_DIR/swapped.cpp"
    run python3 "$SCRIPT" --file "$TMP_DIR/swapped.cpp"
    [ "$status" -eq 1 ]
    contains "before set_display_rotation()" "$output"
    contains "apply_rotation()" "$output"
}

@test "gate catches a definition whose return type wraps to its own line" {
    # clang-format leaves `DisplayManager::name(` at column 0 when the return
    # type is long; the definition must still start a block.
    printf '%s\n' \
        'DisplayManager::some_long_return_type(' \
        '    m_width = lv_display_get_horizontal_resolution(m_display);' \
        '    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
        '}' > "$TMP_DIR/wrapped.cpp"
    run python3 "$SCRIPT" --file "$TMP_DIR/wrapped.cpp"
    [ "$status" -eq 1 ]
}

@test "a comment naming set_display_rotation does not count as the settle call" {
    # resize_timer_cb's shape, with prose mentioning the call: the comment
    # must neither satisfy the order for a real settle nor defeat the
    # no-settle exemption for an external-resize refresh.
    printf '%s\n' \
        'void DisplayManager::resize_timer_cb(lv_timer_t* timer) {' \
        '    // set_display_rotation() would un-swap this, but none runs here.' \
        '    m_width = lv_display_get_horizontal_resolution(m_display);' \
        '}' > "$TMP_DIR/commented.cpp"
    run python3 "$SCRIPT" --file "$TMP_DIR/commented.cpp"
    [ "$status" -eq 0 ]
}

@test "gate stays quiet on a cache refresh with no rotation settle" {
    # resize_timer_cb's shape: an external resize (Android fold/unfold) is the
    # thing being caught up to; no settle call exists to order against.
    printf '%s\n' \
        'void DisplayManager::resize_timer_cb(lv_timer_t* timer) {' \
        '    auto* self = static_cast<DisplayManager*>(lv_timer_get_user_data(timer));' \
        '    self->m_width = lv_display_get_horizontal_resolution(self->m_display);' \
        '    self->m_height = lv_display_get_vertical_resolution(self->m_display);' \
        '}' > "$TMP_DIR/external.cpp"
    run python3 "$SCRIPT" --file "$TMP_DIR/external.cpp"
    [ "$status" -eq 0 ]
}

@test "gate passes the correct order in a function with both calls" {
    printf '%s\n' \
        'void DisplayManager::apply_rotation(int degrees) {' \
        '    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);' \
        '    m_width = lv_display_get_horizontal_resolution(m_display);' \
        '    m_height = lv_display_get_vertical_resolution(m_display);' \
        '}' > "$TMP_DIR/correct.cpp"
    run python3 "$SCRIPT" --file "$TMP_DIR/correct.cpp"
    [ "$status" -eq 0 ]
}

@test "gate reports a missing file without a verdict" {
    run python3 "$SCRIPT" --file "$TMP_DIR/no-such-file.cpp"
    [ "$status" -eq 2 ]
}
