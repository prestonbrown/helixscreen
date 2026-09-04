#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_printer_image_invalidation.py — the gate keeping
# UI code from deleting a printer image cache it is about to need.
#
# invalidate_printer_image_cache(path) removes every generated scaled .bin for
# that source image, and check_or_generate_cache() rebuilds one synchronously on
# the main thread. A widget that re-resolves the same image on every activation
# and invalidates unconditionally therefore pays a full decode-and-resize plus a
# flash write each time the panel opens, and never keeps a cache long enough to
# use it.
#
# src/system/ is exempt because import_image() rewrites the pixels behind an
# existing path: there the caches really are stale while the path is unchanged,
# which is the one case the guarded form would wrongly skip.

GATE="scripts/check_printer_image_invalidation.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/src"
    mkdir -p "$FIXTURE/ui/panel_widgets" "$FIXTURE/system"
}

guarded_call() {
    cat > "$FIXTURE/ui/panel_widgets/printer_image_widget.cpp" <<'EOF'
void PrinterImageWidget::refresh_printer_image() {
    helix::invalidate_printer_image_cache_if_changed(current_source_path_, source_path);
}
EOF
}

@test "the committed tree is clean" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"change-guarded"* ]]
}

@test "an unguarded call in UI code fails" {
    cat > "$FIXTURE/ui/panel_widgets/printer_image_widget.cpp" <<'EOF'
void PrinterImageWidget::refresh_printer_image() {
    helix::invalidate_printer_image_cache(current_source_path_);
}
EOF
    run python3 "$GATE" --src "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"printer_image_widget.cpp"* ]]
}

@test "the failure names the offending line number" {
    printf '\n\n\nhelix::invalidate_printer_image_cache(p);\n' \
        > "$FIXTURE/ui/ui_printer_manager_overlay.cpp"
    run python3 "$GATE" --src "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"ui_printer_manager_overlay.cpp:4"* ]]
}

@test "the guarded form alone is silent" {
    guarded_call
    run python3 "$GATE" --src "$FIXTURE"
    [ "$status" -eq 0 ]
}

@test "an unguarded call under system/ is allowed" {
    cat > "$FIXTURE/system/printer_image_manager.cpp" <<'EOF'
void PrinterImageManager::import_image() {
    invalidate_printer_image_cache("A:" + path_300);
    invalidate_printer_image_cache("A:" + path_150);
}
EOF
    run python3 "$GATE" --src "$FIXTURE"
    [ "$status" -eq 0 ]
}

@test "a call in a line comment is not a call site" {
    cat > "$FIXTURE/ui/ui_printer_manager_overlay.cpp" <<'EOF'
// Prefer this over invalidate_printer_image_cache(old_path) on a refresh.
void PrinterManagerOverlay::refresh_printer_info() {}
EOF
    run python3 "$GATE" --src "$FIXTURE"
    [ "$status" -eq 0 ]
}

@test "a call in a block comment is not a call site" {
    cat > "$FIXTURE/ui/ui_printer_manager_overlay.cpp" <<'EOF'
/* Do not reach for invalidate_printer_image_cache(path) here: a refresh
   usually resolves to the same image. */
void PrinterManagerOverlay::refresh_printer_info() {}
EOF
    run python3 "$GATE" --src "$FIXTURE"
    [ "$status" -eq 0 ]
}

@test "a header declaring the unconditional form outside system/ is flagged" {
    cat > "$FIXTURE/ui/cache_shim.h" <<'EOF'
inline void drop(const std::string& p) { invalidate_printer_image_cache(p); }
EOF
    run python3 "$GATE" --src "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"cache_shim.h"* ]]
}
