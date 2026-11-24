#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# serve-local-update.sh names the zip and the manifest asset key from the
# installer's platform-to-asset table, so a K1 is served the unified MIPS zip
# under the key the app looks up.

SCRIPT="$BATS_TEST_DIRNAME/../../scripts/serve-local-update.sh"

@test "k1 is served helixscreen-mips.zip under the mips key" {
    run bash "$SCRIPT" --platform k1 --print-asset
    [ "$status" -eq 0 ]
    [ "$output" = "helixscreen-mips.zip mips" ]
}

@test "a platform with its own zip keeps its name as the key" {
    run bash "$SCRIPT" --platform snapmaker-u1 --print-asset
    [ "$status" -eq 0 ]
    [ "$output" = "helixscreen-snapmaker-u1.zip snapmaker-u1" ]
}
