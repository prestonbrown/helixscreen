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

@test "printing the asset needs none of the serving tools" {
    # A PATH that mirrors the host's tools minus the ones only serving uses,
    # the way CI images lack minisign.
    local bin="$BATS_TEST_TMPDIR/bin"
    mkdir -p "$bin"
    local d f
    for d in ${PATH//:/ }; do
        [ -d "$d" ] || continue
        for f in "$d"/*; do
            case "${f##*/}" in minisign|python3|docker|zip|unzip) continue ;; esac
            [ -e "$bin/${f##*/}" ] || ln -s "$f" "$bin/${f##*/}" 2>/dev/null || true
        done
    done
    PATH="$bin" run bash "$SCRIPT" --platform k1 --print-asset
    [ "$status" -eq 0 ]
    [ "$output" = "helixscreen-mips.zip mips" ]
}
