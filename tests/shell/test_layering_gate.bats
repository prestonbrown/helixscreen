#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_layering.py (prestonbrown/helixscreen#1946): from its
# fork point a branch may not add an include that points up the layer stack. The
# ratchet is proven to fail on the shape it guards (a new upward include, in a .cpp
# or in a header a lower layer owns, with no grant) and to stay quiet on the idioms
# beside it (a grandfathered edge, a rename, a grant, a header-only header, an
# unlayered directory).

load helpers

SCRIPTS="$BATS_TEST_DIRNAME/../../scripts"
GATE="$SCRIPTS/check_layering.py"

setup() {
    REPO="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    mkdir -p "$REPO/src/printer" "$REPO/src/ui" "$REPO/src/application" "$REPO/include"
    git -C "$REPO" init -q -b main
    git -C "$REPO" config user.email t@example.com
    git -C "$REPO" config user.name t
    echo '#pragma once' > "$REPO/include/ui_toast_manager.h"
    echo '#include "ui_toast_manager.h"' > "$REPO/src/ui/ui_toast_manager.cpp"
    echo '#pragma once' > "$REPO/include/printer_state.h"
    printf '#include "printer_state.h"\n#include "ui_toast_manager.h"\n' > "$REPO/src/printer/printer_state.cpp"
    echo '#include "printer_state.h"' > "$REPO/src/printer/clean.cpp"
    echo '#pragma once' > "$REPO/include/header_only.h"
    echo '#pragma once' > "$REPO/include/data_root_resolver.h"
    echo '#include "data_root_resolver.h"' > "$REPO/src/application/data_root_resolver.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
    BASE=$(git -C "$REPO" rev-parse HEAD)
    git -C "$REPO" checkout -qb feature
}

commit_all() {
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "$1" ${2:+-m "$2"}
}

layering() {
    run python3 "$GATE" --root "$REPO" --base "$BASE"
}

@test "a new upward include fails and names the file, the header and both layers" {
    echo '#include "ui_toast_manager.h"' >> "$REPO/src/printer/clean.cpp"
    commit_all "toast from the printer"
    layering
    [ "$status" -eq 1 ]
    contains "src/printer/clean.cpp: upward includes grew 0 -> 1" "$output"
    contains "include/ui_toast_manager.h (src/ui, ui) from state" "$output"
    contains "Layering: src/printer/clean.cpp <reason>" "$output"
}

@test "an upward include spelled with a parent directory is counted" {
    echo '#include "../ui/ui_panel_thing.h"' >> "$REPO/src/printer/clean.cpp"
    echo '#pragma once' > "$REPO/src/ui/ui_panel_thing.h"
    echo '#include "ui_panel_thing.h"' > "$REPO/src/ui/ui_panel_thing.cpp"
    commit_all "a relative include"
    layering
    [ "$status" -eq 1 ]
    contains "src/printer/clean.cpp: upward includes grew 0 -> 1" "$output"
}

@test "an upward include the fork point already has is grandfathered, and may be removed" {
    echo '// touched' >> "$REPO/src/printer/printer_state.cpp"
    commit_all "touch"
    layering
    [ "$status" -eq 0 ]
    contains "upward includes 1 -> 1" "$output"
    echo '#include "printer_state.h"' > "$REPO/src/printer/printer_state.cpp"
    commit_all "drop the toast"
    layering
    [ "$status" -eq 0 ]
    contains "upward includes 1 -> 0" "$output"
}

@test "removing an upward include in one file does not pay for adding one in another" {
    echo '#include "printer_state.h"' > "$REPO/src/printer/printer_state.cpp"
    echo '#include "ui_toast_manager.h"' >> "$REPO/src/printer/clean.cpp"
    commit_all "move the toast"
    layering
    [ "$status" -eq 1 ]
    contains "src/printer/clean.cpp: upward includes grew 0 -> 1" "$output"
    lacks "printer_state.cpp" "$output"
}

@test "a renamed file is compared against its old path" {
    git -C "$REPO" mv src/printer/printer_state.cpp src/printer/printer_state_impl.cpp
    commit_all "rename"
    layering
    [ "$status" -eq 0 ]
    lacks "printer_state_impl.cpp" "$output"
}

@test "a Layering: grant line in a branch commit exempts that one file" {
    echo '#include "ui_toast_manager.h"' >> "$REPO/src/printer/clean.cpp"
    commit_all "toast" "Layering: src/printer/clean.cpp needs the toast until the notice router lands"
    layering
    [ "$status" -eq 0 ]
    echo '#include "data_root_resolver.h"' >> "$REPO/src/printer/printer_state.cpp"
    commit_all "resolver too"
    layering
    [ "$status" -eq 1 ]
    contains "src/printer/printer_state.cpp: upward includes grew 1 -> 2" "$output"
    lacks "clean.cpp" "$output"
}

@test "a header a lower layer owns may not start including an upper layer's header" {
    echo '#include "ui_toast_manager.h"' >> "$REPO/include/printer_state.h"
    commit_all "toast from the state header"
    layering
    [ "$status" -eq 1 ]
    contains "include/printer_state.h: upward includes grew 0 -> 1" "$output"
}

@test "a header with no .cpp is header-only and counts in neither direction" {
    echo '#include "header_only.h"' >> "$REPO/src/printer/clean.cpp"
    echo '#include "ui_toast_manager.h"' >> "$REPO/include/header_only.h"
    commit_all "header-only both ways"
    layering
    [ "$status" -eq 0 ]
    contains "upward includes 1 -> 1" "$output"
}

@test "a header with no .cpp but a ui_ name or an include/ui/ path belongs to ui" {
    echo '#pragma once' > "$REPO/include/ui_nav.h"
    mkdir -p "$REPO/include/ui"
    echo '#pragma once' > "$REPO/include/ui/widget_helpers.h"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "two header-only ui headers"
    BASE=$(git -C "$REPO" rev-parse HEAD)
    echo '#include "ui_nav.h"' >> "$REPO/src/printer/clean.cpp"
    echo '#include "ui/widget_helpers.h"' >> "$REPO/src/printer/printer_state.cpp"
    commit_all "nav and helpers from the printer"
    layering
    [ "$status" -eq 1 ]
    contains "src/printer/clean.cpp: upward includes grew 0 -> 1" "$output"
    contains "include/ui_nav.h (src/ui, ui) from state" "$output"
    contains "src/printer/printer_state.cpp: upward includes grew 1 -> 2" "$output"
    contains "include/ui/widget_helpers.h (src/ui, ui) from state" "$output"
}

@test "a directory missing from the layer table is reported, never failed" {
    mkdir -p "$REPO/src/newdir"
    echo '#include "ui_toast_manager.h"' > "$REPO/src/newdir/x.cpp"
    commit_all "unlayered"
    layering
    [ "$status" -eq 0 ]
    run python3 "$GATE" --root "$REPO" --report
    [ "$status" -eq 0 ]
    contains "Unlayered directories (add each to LAYERS): src/newdir" "$output"
}

@test "a cleared pair fails anywhere at HEAD, with no branch work at all" {
    run python3 - "$SCRIPTS" "$REPO" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import check_layering as m
m.CLEARED_PAIRS = (("printer", "ui_toast_manager.h"),)
sys.exit(m.main(["--root", sys.argv[2], "--base", "HEAD"]))
EOF
    [ "$status" -eq 1 ]
    contains "src/printer/printer_state.cpp includes include/ui_toast_manager.h: src/printer no longer includes it anywhere" "$output"
}

@test "--report prints the matrix and every upward pair" {
    run python3 "$GATE" --root "$REPO" --report
    [ "$status" -eq 0 ]
    contains "Upward edges: 1 in 1 files" "$output"
    contains "src/printer -> src/ui: 1; ui_toast_manager.h(1)" "$output"
}

@test "a branch with no commits is not measured" {
    run python3 "$GATE" --root "$REPO" --base HEAD
    [ "$status" -eq 0 ]
    contains "no committed work" "$output"
}

# --- the qc wrapper -------------------------------------------------------------

qc_layering_run() {
    run bash -c '
        STAGED_ONLY=false; QC_TMP="$(mktemp -d)"; QC_COUNTS="$QC_TMP/counts"
        . "$1/scripts/qc/_lib.sh"
        . "$1/scripts/qc/layering.sh"
        cd "$2" && mkdir -p scripts && ln -sf "$1/scripts/check_layering.py" scripts/ \
            && ln -sf "$1/scripts/diff_base.py" scripts/
        qc_layering' _ "$BATS_TEST_DIRNAME/../.." "$REPO"
}

@test "the qc wrapper measures the branch from its fork point and fails on a new upward include" {
    echo '#include "ui_toast_manager.h"' >> "$REPO/src/printer/clean.cpp"
    commit_all "toast"
    qc_layering_run
    [ "$status" -eq 1 ]
    contains "src/printer/clean.cpp: upward includes grew 0 -> 1" "$output"
}

@test "the qc wrapper passes a branch that adds none" {
    echo '// touched' >> "$REPO/src/printer/clean.cpp"
    commit_all "touch"
    qc_layering_run
    [ "$status" -eq 0 ]
    contains "Layering" "$output"
}

@test "the qc wrapper triggers on C++ under src and include and on its script" {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    re="$(qc_trigger layering)"
    [ -n "$re" ]
    for p in src/ui/ui_x.cpp src/printer/x.h include/x.h scripts/check_layering.py; do
        echo "$p" | grep -qE "$re" || fail "$p should trigger"
    done
    for p in docs/devel/README.md tests/unit/test_x.cpp tests/shell/test_x.bats ui_xml/globals.xml; do
        if echo "$p" | grep -qE "$re"; then fail "$p should not trigger"; fi
    done
}
