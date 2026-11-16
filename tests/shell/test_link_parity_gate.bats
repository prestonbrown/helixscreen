#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_link_parity.py - the variant-link parity gate.
#
# The fbdev binary is linked from a prerequisite list written out by hand
# beside the main link's, and a gap between them surfaces only as
# undefined references in the one configuration that compiles the missing
# object in. These tests seed that drift into a copy of the build files and
# require the gate to name the object, so a gate that stopped comparing
# anything could not pass them.

GATE="scripts/check_link_parity.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    REPO="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/link_parity"
    mkdir -p "$WORK"
}

# A tree make can parse whose files are the repository's own, except for the
# .mk file a test copies in and edits. Drift is never seeded into the working
# tree: other sessions build and commit from this checkout. build/ is a fresh
# directory so the parse-time stamps land in the fixture.
make_tree() {
    TREE="$WORK/tree"
    rm -rf "$TREE"
    mkdir -p "$TREE/mk" "$TREE/build"
    local entry name
    for entry in "$REPO"/* "$REPO"/.[!.]*; do
        name="$(basename "$entry")"
        case "$name" in mk|build) continue ;; esac
        ln -s "$entry" "$TREE/$name"
    done
    for entry in "$REPO"/mk/*; do
        ln -s "$entry" "$TREE/mk/$(basename "$entry")"
    done
}

# Replace a symlinked mk/ file with a copy edited by sed expression $2.
edit_mk() {
    local file="$1" expr="$2"
    rm "$TREE/mk/$file"
    sed "$expr" "$REPO/mk/$file" > "$TREE/mk/$file"
    if cmp -s "$REPO/mk/$file" "$TREE/mk/$file"; then
        fail "sed '$expr' left mk/$file unchanged; the seed no longer matches the file"
    fi
}

# --- the real tree passes ----------------------------------------------------

@test "passes on the repository as committed" {
    run python3 "$GATE" --repo-root "$(scratch_tree)"
    [ "$status" -eq 0 ] || fail "gate failed on the real tree: $output"
    contains "variant links match" "$output"
    # Every variant platform in mk/cross.mk was actually compared.
    contains "pi-both/fbdev" "$output"
    contains "pi32-both/fbdev" "$output"
    contains "x86-both/fbdev" "$output"
}

# --- seeded drift ------------------------------------------------------------

@test "fails when the fbdev link drops an object the main link uses" {
    make_tree
    edit_mk pi-dual-link.mk 's/ \$(REMOTE_LINENOISE_OBJ) \\$/ \\/'
    run python3 "$GATE" --repo-root "$TREE" --platform pi-both
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    contains "build/pi-fbdev/bin/helix-screen does not link build/pi/obj/linenoise.o" "$output"
}

@test "an EGL path object is excused from the fbdev link through EGL_PATH_OBJS" {
    # The same seed as the main-link-only case below, declared as part of the
    # EGL path: the fbdev binary carries no GPU path, so this one is no drift.
    printf '%s\n' 'EGL_PATH_OBJS += $(OBJ_DIR)/seeded_gpu.o' '$(TARGET): $(OBJ_DIR)/seeded_gpu.o' > "$WORK/seed.mk"
    run python3 "$GATE" --repo-root "$(scratch_tree)" --platform pi32-both --extra-makefile "$WORK/seed.mk"
    [ "$status" -eq 0 ] || fail "expected exit 0, got $status: $output"
    printf '%s\n' '$(TARGET): $(OBJ_DIR)/seeded_gpu.o' > "$WORK/seed.mk"
    run python3 "$GATE" --repo-root "$(scratch_tree)" --platform pi32-both --extra-makefile "$WORK/seed.mk"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    contains "does not link build/pi32/obj/seeded_gpu.o" "$output"
}

@test "fails when an object is added to the main link only" {
    printf '%s\n' '$(TARGET): $(OBJ_DIR)/seeded_drift.o' > "$WORK/seed.mk"
    run python3 "$GATE" --repo-root "$(scratch_tree)" --platform x86-both --extra-makefile "$WORK/seed.mk"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    contains "does not link build/x86/obj/seeded_drift.o" "$output"
}

@test "fails when a declared replacement list no longer exists" {
    # A renamed swap variable must not quietly widen into "allow nothing" or
    # "allow everything"; the gate has to say it lost track of it.
    printf '%s\n' 'undefine DRM_CRASH_OBJ' > "$WORK/seed.mk"
    run python3 "$GATE" --repo-root "$(scratch_tree)" --platform pi-both --extra-makefile "$WORK/seed.mk"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    contains "DRM_CRASH_OBJ" "$output"
}

@test "fails when make cannot parse the platform" {
    printf '%s\n' '$(error seeded parse failure)' > "$WORK/seed.mk"
    run python3 "$GATE" --repo-root "$(scratch_tree)" --platform pi-both --extra-makefile "$WORK/seed.mk"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    contains "seeded parse failure" "$output"
}

@test "fails when mk/cross.mk names no variant platform" {
    make_tree
    rm "$TREE/mk/cross.mk"
    grep -v -e 'PI_DUAL_LINK' "$REPO/mk/cross.mk" > "$TREE/mk/cross.mk"
    run python3 "$GATE" --repo-root "$TREE"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    contains "names no platform" "$output"
}

# --- wiring --------------------------------------------------------------------

@test "quality-checks runs the gate" {
    grep -Eq '^QC_ALL=.*[" ]qc_link_parity[" ]' scripts/quality-checks.sh \
        || fail "qc_link_parity is not in QC_ALL"
}

@test "a staged build file triggers the gate and a source file does not" {
    local re
    re="$(qc_trigger link_parity)"
    [ -n "$re" ] || fail "no QC_TRIGGER_qc_link_parity"
    for path in Makefile mk/pi-dual-link.mk mk/egl-link.mk mk/rules.mk scripts/check_link_parity.py; do
        printf '%s\n' "$path" | grep -Eq "$re" || fail "$path does not trigger the gate"
    done
    refute_sh "printf '%s\n' src/ui/home/ui_panel_home.cpp | grep -Eq '$re'"
}

@test "qc_link_parity reports the gate's verdict" {
    . scripts/qc/link_parity.sh
    section_time() { :; }
    # The wrapper names no tree, so the gate it runs is pointed at scratch_tree.
    mock_command_script python3 "exec $(command -v python3) \"\$@\" --repo-root '$(scratch_tree)'"
    run qc_link_parity
    [ "$status" -eq 0 ] || fail "qc_link_parity failed: $output"
    contains "variant links match" "$output"
}
