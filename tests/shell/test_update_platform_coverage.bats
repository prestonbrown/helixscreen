#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Guardrail for the in-app self-update platform picker.
#
# Silent brick history:
#   v0.99.41: HELIX_PLATFORM_SNAPMAKER_U1 missing from get_platform_key() →
#             U1 devices downloaded helixscreen-pi-*.tar.gz on self-update,
#             ended up with a binary that NEEDED libsystemd/libinput/libEGL/
#             libGLESv2/libgbm — none present on U1 → exec failure.
#   Earlier:  HELIX_PLATFORM_X86 missing from the same function (fixed by
#             640b2a6ab).
#
# These tests enforce that:
#   1. Every -DHELIX_PLATFORM_* define in mk/cross.mk has a matching branch
#      in src/system/update_checker.cpp::get_platform_key().
#   2. Every PLATFORM_TARGET in the release.yml matrix has a corresponding
#      entry in the known_platforms allowlist in test_update_checker.cpp
#      (so a missing branch fails unit tests instead of silently shipping).
#   3. Every platform key returned by get_platform_key() is also in the
#      known_platforms allowlist (catches typos in the return string).
#   4. The crash worker's KNOWN_PLATFORMS allowlist matches get_platform_key()
#      exactly. The worker refuses crash reports from platforms outside that
#      set, so a key missing there silently drops a real platform's reports,
#      and a stale extra key lets a fork keep filing issues.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
CROSS_MK="$WORKTREE_ROOT/mk/cross.mk"
UPDATE_CHECKER_CPP="$WORKTREE_ROOT/src/system/update_checker.cpp"
TEST_UPDATE_CHECKER_CPP="$WORKTREE_ROOT/tests/unit/test_update_checker.cpp"
RELEASE_YML="$WORKTREE_ROOT/.github/workflows/release.yml"
PLATFORM_SH="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
CRASH_WORKER_TS="$WORKTREE_ROOT/server/crash-worker/src/index.ts"

# Space-separated release matrix platforms from release.yml.
_release_matrix_platforms() {
    grep -E 'platform: *\[' "$RELEASE_YML" | head -1 \
        | sed -E 's/.*\[([^]]+)\].*/\1/' | tr ',' '\n' | tr -d ' '
}

@test "every HELIX_PLATFORM_* in cross.mk has a branch in get_platform_key()" {
    # Extract the -DHELIX_PLATFORM_X tokens from mk/cross.mk. The set is
    # authoritative: every platform we build for is tagged with one of these.
    local platforms
    platforms=$(grep -oE -- '-DHELIX_PLATFORM_[A-Z0-9_]+' "$CROSS_MK" \
                | sed 's/^-D//' | sort -u)

    [ -n "$platforms" ]  # sanity: we expect at least one

    local missing=""
    for p in $platforms; do
        # Accept either `#ifdef HELIX_PLATFORM_X` or `defined(HELIX_PLATFORM_X)`.
        if ! grep -qE "(ifdef $p\b|defined\($p\))" "$UPDATE_CHECKER_CPP"; then
            missing="$missing $p"
        fi
    done

    if [ -n "$missing" ]; then
        echo "The following HELIX_PLATFORM_* macros are built by mk/cross.mk"
        echo "but have no branch in UpdateChecker::get_platform_key():"
        echo "   $missing"
        echo ""
        echo "Add matching '#elif defined(...)' branches returning the correct"
        echo "asset key, OR the device will silently fall through to 'pi' on"
        echo "self-update and download the wrong release tarball."
        false
    fi
}

@test "every release.yml platform has a known_platforms entry in the unit test" {
    # Pull `platform: [...]` list from the release matrix.
    local matrix_line
    matrix_line=$(grep -E 'platform: *\[' "$RELEASE_YML" | head -1)
    [ -n "$matrix_line" ]

    # Strip everything outside the brackets, split on commas.
    local platforms
    platforms=$(echo "$matrix_line" | sed -E 's/.*\[([^]]+)\].*/\1/' \
                | tr ',' '\n' | tr -d ' ')

    [ -n "$platforms" ]

    # Pull known_platforms vector from test_update_checker.cpp.
    # The literal is split across lines; slurp a window and keep only quoted
    # tokens.
    local known
    known=$(awk '/std::vector<std::string> known_platforms = \{/,/\};/' \
            "$TEST_UPDATE_CHECKER_CPP" | grep -oE '"[a-z0-9_-]+"' | tr -d '"')

    [ -n "$known" ]

    local missing=""
    for p in $platforms; do
        # The k1 build target is "mips" → platform key "k1"; normalize.
        local key="$p"
        if [ "$key" = "mips" ]; then key="k1"; fi
        if ! echo "$known" | grep -qx "$key"; then
            missing="$missing $key"
        fi
    done

    if [ -n "$missing" ]; then
        echo "Release matrix ships these platforms but known_platforms in"
        echo "tests/unit/test_update_checker.cpp is missing:"
        echo "   $missing"
        echo ""
        echo "Add them to known_platforms so get_platform_key() tests catch"
        echo "the missing #elif branch that would ship the wrong tarball."
        false
    fi
}

@test "every string returned by get_platform_key() is in known_platforms" {
    # Extract the quoted return values from get_platform_key()'s body.
    local returns
    returns=$(awk '/std::string UpdateChecker::get_platform_key\(\)/,/^}/' \
              "$UPDATE_CHECKER_CPP" | grep -oE 'return "[a-z0-9_-]+"' \
              | grep -oE '"[a-z0-9_-]+"' | tr -d '"' | sort -u)

    [ -n "$returns" ]

    local known
    known=$(awk '/std::vector<std::string> known_platforms = \{/,/\};/' \
            "$TEST_UPDATE_CHECKER_CPP" | grep -oE '"[a-z0-9_-]+"' | tr -d '"')

    [ -n "$known" ]

    local bad=""
    for r in $returns; do
        if ! echo "$known" | grep -qx "$r"; then
            bad="$bad $r"
        fi
    done

    if [ -n "$bad" ]; then
        echo "get_platform_key() returns strings that are not in the test's"
        echo "known_platforms allowlist:"
        echo "   $bad"
        echo ""
        echo "Either fix the typo in the return string or add the key to"
        echo "known_platforms in tests/unit/test_update_checker.cpp."
        false
    fi
}

@test "crash worker KNOWN_PLATFORMS matches get_platform_key() exactly" {
    # get_platform_key() is the single source of truth for what a build can
    # report. The worker refuses crash reports whose platform is outside its
    # own allowlist, so the two must agree in BOTH directions: a key missing
    # from the worker drops that platform's real reports on the floor, and a
    # key the app can no longer emit leaves the door open.
    local returns
    returns=$(awk '/std::string UpdateChecker::get_platform_key\(\)/,/^}/' \
              "$UPDATE_CHECKER_CPP" | grep -oE 'return "[a-z0-9_-]+"' \
              | grep -oE '"[a-z0-9_-]+"' | tr -d '"' | sort -u)

    [ -n "$returns" ]

    local worker
    worker=$(awk '/const KNOWN_PLATFORMS = new Set\(\[/,/\]\);/' \
             "$CRASH_WORKER_TS" | grep -oE '"[a-z0-9_-]+"' | tr -d '"' | sort -u)

    # An empty extraction means the literal was reformatted and this gate is
    # reading nothing -- fail rather than silently pass over no corpus.
    [ -n "$worker" ]

    local only_cpp="" only_worker=""
    for r in $returns; do
        echo "$worker" | grep -qx "$r" || only_cpp="$only_cpp $r"
    done
    for w in $worker; do
        echo "$returns" | grep -qx "$w" || only_worker="$only_worker $w"
    done

    if [ -n "$only_cpp" ] || [ -n "$only_worker" ]; then
        echo "get_platform_key() and the crash worker's KNOWN_PLATFORMS disagree."
        [ -n "$only_cpp" ] && {
            echo "  in get_platform_key() but NOT in the worker:$only_cpp"
            echo "  -> the worker would refuse real crash reports from these."
        }
        [ -n "$only_worker" ] && {
            echo "  in the worker but NOT in get_platform_key():$only_worker"
            echo "  -> no build emits these; drop them from the allowlist."
        }
        echo ""
        echo "Fix KNOWN_PLATFORMS in server/crash-worker/src/index.ts, and"
        echo "remember the worker must be deployed for a new platform to be"
        echo "able to file crash reports at all."
        false
    fi
}

# --- Self-update asset picker (helix_self_update_asset) ----------------------
#
# helix_self_update_asset() in scripts/lib/installer/platform.sh is the single
# source of truth for the release asset a platform self-updates to. It is shared
# by moonraker.sh's write_release_info() (Moonraker type:web runtime fallback)
# AND mk/cross.mk's baked release_info.json, so the two can't drift. Moonraker
# downloads the asset whose name == release_info.json's asset_name; if no asset
# matches, it falls back to the alphabetically-first release asset — a .sym
# debug file — and dies with "File is not a zip file"
# (prestonbrown/helixscreen#993). These run the real function and check it
# resolves every platform to an asset the release matrix actually builds.

@test "helix_self_update_asset maps each matrix platform to its own artifact" {
    # shellcheck disable=SC1090
    source "$PLATFORM_SH"

    local matrix
    matrix=$(_release_matrix_platforms)
    [ -n "$matrix" ]

    # Every platform the matrix builds ships helixscreen-<platform>.zip, so each
    # must self-update to exactly its own artifact. Catches both a missing asset
    # (k1-dynamic.zip class) and a wrong-but-existing asset (ad5x->k1.zip class).
    local bad=""
    for p in $matrix; do
        local asset
        asset=$(helix_self_update_asset "$p")
        if [ "$asset" != "helixscreen-$p.zip" ]; then
            bad="$bad ${p}->${asset}(want helixscreen-${p}.zip)"
        fi
    done

    if [ -n "$bad" ]; then
        echo "helix_self_update_asset() does not map these matrix platforms to"
        echo "their own release artifact:"
        echo "   $bad"
        echo ""
        echo "A mismatch makes Moonraker download the wrong asset (or fall back"
        echo "to a .sym file -> 'File is not a zip file', #993). Fix the mapping"
        echo "in scripts/lib/installer/platform.sh."
        false
    fi
}

@test "helix_self_update_asset borrows resolve to assets the matrix builds" {
    # shellcheck disable=SC1090
    source "$PLATFORM_SH"

    local matrix
    matrix=$(_release_matrix_platforms)
    [ -n "$matrix" ]

    # Non-matrix platforms borrow another platform's artifact. The borrow target
    # must itself be a built asset, or Moonraker hits the #993 fallback.
    #   k1-dynamic (dev/debug variant) -> stable k1
    [ "$(helix_self_update_asset k1-dynamic)" = "helixscreen-k1.zip" ]
    #   m1 (Artillery, Debian SBC) -> pi or pi32 by userspace bitness
    local m1_asset
    m1_asset=$(helix_self_update_asset m1)
    case "$m1_asset" in
        helixscreen-pi.zip | helixscreen-pi32.zip) ;;
        *) echo "m1 resolved to unexpected asset: $m1_asset"; false ;;
    esac

    # Borrow targets must be in the matrix.
    local stem
    for stem in k1 pi pi32; do
        if ! echo "$matrix" | grep -qx "$stem"; then
            echo "borrow target '$stem' is not a release matrix platform"
            false
        fi
    done
}
