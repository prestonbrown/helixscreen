#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fixture tests of lint_env_file_vars_live (scripts/code_lint.sh): every
# uncommented variable in config/helixscreen.env is read by the app or the
# launcher, and the Moonraker target is not offered there. The verdict over the
# real tree is qc_code_lint's.

load helpers

setup() {
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    # shellcheck source=scripts/code_lint.sh
    . "$REPO/scripts/code_lint.sh"
    cd "$BATS_TEST_TMPDIR" || return 1
    mkdir -p config scripts src/system include
    printf '#!/bin/sh\nexec helix-screen ${LAUNCHER_ONLY:+--flag}\n' > scripts/helix-launcher.sh
    printf 'const char* v = getenv("APP_READ");\n' > src/system/reader.cpp
}

@test "a variable the app or the launcher reads passes; commented examples are ignored" {
    printf '# comment\n#NOBODY_READS=1\nAPP_READ=1\nLAUNCHER_ONLY=yes\n' > config/helixscreen.env
    run lint_env_file_vars_live
    [ "$status" -eq 0 ] || fail "$output"
}

@test "an uncommented variable nothing reads fails and is named" {
    printf 'APP_READ=1\nDEAD_SETTING=2\n' > config/helixscreen.env
    run lint_env_file_vars_live
    [ "$status" -eq 1 ]
    contains "DEAD_SETTING" "$output"
    lacks "APP_READ" "$output"
}

@test "an active Moonraker target fails even when something reads it" {
    printf 'MOONRAKER_HOST=printer.local\n' > config/helixscreen.env
    printf 'getenv("MOONRAKER_HOST")\n' >> src/system/reader.cpp
    run lint_env_file_vars_live
    [ "$status" -eq 1 ]
    contains "settings.json" "$output"
}

@test "a missing env file or launcher fails instead of passing" {
    run lint_env_file_vars_live
    [ "$status" -eq 1 ]
    contains "config/helixscreen.env is gone" "$output"
}
