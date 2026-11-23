#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/qc/copyright.sh: every .cpp/.c/.h/.mm under src/ and
# include/ carries the GPL SPDX line within its first three lines. The file runs
# inline when sourced and leaves its verdict in EXIT_CODE, so each case sources
# it in a scratch tree.

load helpers

setup() {
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    ROOT="$BATS_TEST_TMPDIR/tree"
    mkdir -p "$ROOT/src/ui" "$ROOT/include"
}

# run_gate: sources the gate in the scratch tree and prints its EXIT_CODE last.
run_gate() {
    run bash -c 'cd "$1" && STAGED_ONLY=false EXIT_CODE=0 && . "$2/scripts/qc/_lib.sh" && . "$2/scripts/qc/copyright.sh" && echo "EXIT_CODE=$EXIT_CODE"' \
        _ "$ROOT" "$REPO"
}

@test "a source file with no SPDX line is reported and fails the run" {
    printf '// SPDX-License-Identifier: GPL-3.0-or-later\nint a;\n' > "$ROOT/src/ui/ok.cpp"
    printf 'int b;\n' > "$ROOT/src/ui/bare.cpp"
    printf '#pragma once\n' > "$ROOT/include/bare.h"
    run_gate
    contains "Missing GPL v3 header: src/ui/bare.cpp" "$output"
    contains "Missing GPL v3 header: include/bare.h" "$output"
    [ "${output#*Missing GPL v3 header: src/ui/ok.cpp}" = "$output" ]
    contains "EXIT_CODE=1" "$output"
}

@test "an SPDX line within the first three lines passes, one on line four does not" {
    printf '// Copyright (C) 2025 356C LLC\n// SPDX-License-Identifier: GPL-3.0-or-later\nint a;\n' > "$ROOT/src/ui/three.cpp"
    printf '// one\n// two\n// three\n// SPDX-License-Identifier: GPL-3.0-or-later\n' > "$ROOT/src/ui/four.cpp"
    run_gate
    [ "${output#*Missing GPL v3 header: src/ui/three.cpp}" = "$output" ]
    contains "Missing GPL v3 header: src/ui/four.cpp" "$output"
}

@test "a tree whose files all carry the header passes" {
    printf '// SPDX-License-Identifier: GPL-3.0-or-later\nint a;\n' > "$ROOT/src/ui/ok.cpp"
    run_gate
    contains "proper copyright headers" "$output"
    contains "EXIT_CODE=0" "$output"
}

@test "the header check runs ahead of every gate in the commit hook" {
    run grep -qF 'qc/copyright.sh' "$REPO/scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
}
