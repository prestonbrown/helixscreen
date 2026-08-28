#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pins the one job that can actually run the ESP32 packed-asset round-trip.
#
# tests/python/test_esp32_pack_assets.py drives the real mkfrogfs.py out of
# managed_components/jkent__frogfs -- a gitignored tree with zero tracked files
# that only exists after `idf.py reconfigure`. Every other CI job is a plain
# checkout with no ESP-IDF, so the module can only skip there, and a skip means
# every assertion in it is silently discarded.
#
# It used to key its hard-fail on $CI, which failed the nightly Python job on a
# gap that job has no way to close. The coverage now lives in esp32-build.yml,
# which vendors the component as a side effect of packing the storage image and
# runs the module with HELIX_FROGFS_REQUIRED=1. That is a two-part contract --
# the workflow sets the variable, the module fails on it -- and nothing else
# reads either half, so silently dropping one end restores exactly the
# green-forever hole the module was written to prevent. Both ends are pinned
# here.

# Overridable so these gates can be proven to FIRE against hand-broken copies.
YML="${HELIX_TEST_ESP32_YML:-.github/workflows/esp32-build.yml}"
MODULE="${HELIX_TEST_PACK_MODULE:-tests/python/test_esp32_pack_assets.py}"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# The workflow documents its own frogfs handling in prose, so a bare grep finds
# the comment long before the step. Assert against comment-stripped YAML.
yml_code() {
    grep -vE '^[[:space:]]*#' "$YML"
}

@test "esp32-build runs the packed-asset round-trip module" {
    run bash -c "yml_code() { grep -vE '^[[:space:]]*#' '$YML'; }; yml_code | grep -q 'pytest tests/python/test_esp32_pack_assets.py'"
    [ "$status" -eq 0 ]
}

@test "esp32-build sets HELIX_FROGFS_REQUIRED for that step" {
    run bash -c "grep -vE '^[[:space:]]*#' '$YML' | grep -q 'HELIX_FROGFS_REQUIRED'"
    [ "$status" -eq 0 ]
}

@test "the module's hard-fail keys on HELIX_FROGFS_REQUIRED" {
    # Without this the workflow's env var is decoration and the module skips
    # even in the job that exists to run it.
    run grep -q 'os.environ.get("HELIX_FROGFS_REQUIRED")' "$MODULE"
    [ "$status" -eq 0 ]
}

@test "the module no longer hard-fails on bare \$CI" {
    # $CI is set in every job, including the ones where the packer cannot exist.
    run grep -q 'os.environ.get("CI")' "$MODULE"
    [ "$status" -ne 0 ]
}

@test "the round-trip step runs after the build that vendors frogfs" {
    # managed_components/ is populated by the `idf.py reconfigure` inside the
    # build step; running pytest before it would find no packer at all.
    build_line=$(grep -n 'idf.py reconfigure' "$YML" | head -1 | cut -d: -f1)
    test_line=$(grep -n 'pytest tests/python/test_esp32_pack_assets.py' "$YML" | head -1 | cut -d: -f1)
    [ -n "$build_line" ]
    [ -n "$test_line" ]
    [ "$test_line" -gt "$build_line" ]
}

@test "the module and the workflow agree on the vendored packer path" {
    # The workflow's `test -f` guard and the module's MKFROGFS constant must
    # name the same file, or one of them proves nothing about the other.
    run grep -q 'managed_components/jkent__frogfs/tools/mkfrogfs.py' "$YML"
    [ "$status" -eq 0 ]
    run grep -q '"jkent__frogfs" / "tools" / "mkfrogfs.py"' "$MODULE"
    [ "$status" -eq 0 ]
}
