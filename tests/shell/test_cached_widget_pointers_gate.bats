#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_cached_widget_pointers.py, the ratchet on raw
# lv_obj_t* data members (prestonbrown/helixscreen#1298).
#
# The gate's value is that a new cached widget pointer cannot land silently, so
# the member shapes it must count matter most. It must also stay quiet about
# locals, parameters, comments and WidgetRef members, or it reports code that is
# already right and gets switched off.

load helpers

GATE="scripts/check_cached_widget_pointers.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FILE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/demo.h"
}

# ------------------------------------------------------------ counted shapes

@test "a raw member initialised to nullptr is counted" {
    printf '    lv_obj_t* label_ = nullptr;\n' > "$FILE"
    run python3 "$GATE" --max-allowed=0 "$FILE"
    [ "$status" -eq 1 ]
    contains "1 > baseline 0" "$output"
}

@test "an uninitialised member, a static member and a C array are each counted" {
    cat > "$FILE" <<'EOF'
    lv_obj_t* row_;
    static lv_obj_t* s_backdrop_;
    lv_obj_t* panels_[4] = {nullptr};
EOF
    run python3 "$GATE" --max-allowed=0 "$FILE"
    [ "$status" -eq 1 ]
    contains "3 > baseline 0" "$output"
}

@test "a count at the baseline passes" {
    printf '    lv_obj_t* label_ = nullptr;\n' > "$FILE"
    run python3 "$GATE" --max-allowed=1 "$FILE"
    [ "$status" -eq 0 ]
    contains "1 <= baseline (1)" "$output"
}

# ------------------------------------------------------------ quiet shapes

@test "locals, parameters, comments and WidgetRef members are not counted" {
    cat > "$FILE" <<'EOF'
    lv_obj_t* label = lv_obj_find_by_name(root, "x");
    void set_root(lv_obj_t* root_);
    // lv_obj_t* old_label_ = nullptr;
    helix::ui::WidgetRef label_;
    std::vector<lv_obj_t*> rows_;
EOF
    run python3 "$GATE" --max-allowed=0 "$FILE"
    [ "$status" -eq 0 ]
    contains "0 <= baseline (0)" "$output"
}

@test "a member that says why it stays raw is not counted" {
    cat > "$FILE" <<'EOF'
    lv_obj_t* scratch_ = nullptr; // WIDGET_PTR_OK: rebuilt synchronously every frame
    // WIDGET_PTR_OK: owned by this class and deleted in its destructor
    lv_obj_t* root_ = nullptr;
EOF
    run python3 "$GATE" --max-allowed=0 "$FILE"
    [ "$status" -eq 0 ]
}
