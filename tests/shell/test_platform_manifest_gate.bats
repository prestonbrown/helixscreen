#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_platform_manifest.py and scripts/platform_manifest.py.
#
# The gate exists because platform facts had no home: panel geometry, install prefixes
# and which size class a package ships were restated in shell, make, C++ and prose, and
# the copies disagreed. It is ADVISORY, so the one thing these tests must pin hardest is
# that advisory does not mean inert - it has to report a real regression and it has to
# exit non-zero under --strict. A gate that cannot go red is a gate that reports nothing
# forever and reads exactly like a passing one.
#
# The deriver half matters just as much: an unknown platform must fail loudly rather than
# fall through to "generate every size class", which is a success-shaped no-op that would
# ship a working package and hide the typo.

load helpers

GATE="scripts/check_platform_manifest.py"
DERIVE="scripts/platform_manifest.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/manifest"
    mkdir -p "$WORK"
}

# A tree the gate can inspect, holding only the files it reads. Findings are
# provoked by editing this copy, never the working tree: these run in a checkout
# other sessions commit from, so a test that aborts between damage and repair
# would leave the repo broken.
make_tree() {
    TREE="$WORK/tree"
    rm -rf "$TREE"
    mkdir -p "$TREE/mk" "$TREE/assets/config" "$TREE/scripts/lib/installer" "$TREE/src/system"
    cp assets/config/platforms.json "$TREE/assets/config/"
    cp mk/cross.mk mk/images.mk "$TREE/mk/"
    cp scripts/lib/installer/common.sh scripts/lib/installer/platform.sh "$TREE/scripts/lib/installer/"
    cp src/system/log_collector.cpp src/system/update_checker.cpp \
       src/system/debug_bundle_collector.cpp "$TREE/src/system/"
}

# --------------------------------------------------------------------------
# The deriver
# --------------------------------------------------------------------------

@test "deriver: rotated panel reports its post-rotation resolution" {
    run python3 "$DERIVE" effective-resolution k2
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ "$output" = "800x480" ]
}

@test "deriver: native landscape panel is not rotated twice" {
    run python3 "$DERIVE" effective-resolution ad5x
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ "$output" = "800x480" ]
}

@test "deriver: an 800x480 panel selects the medium tier" {
    # The classes are UiBreakpoint tiers: 800x480 has a narrow axis of 480, which
    # is Medium. Naming it "small" is the old prerender-only vocabulary.
    run python3 "$DERIVE" splash-3d-sizes k2
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ "$output" = "medium" ]
}

@test "deriver: a 480x272 panel selects micro, not an overdrawing tiny" {
    run python3 "$DERIVE" splash-3d-sizes cc1
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ "$output" = "micro" ]
}

@test "deriver: a platform with an unmeasured variant also gets its hedge class" {
    run python3 "$DERIVE" splash-3d-sizes k1
    [ "$status" -eq 0 ] || fail "command failed: $output"
    contains "medium" "$output"
    contains "small" "$output"
}

@test "deriver: every class it can return is a layout breakpoint name" {
    # ultrawide is an aspect rather than a tier and is the one deliberate
    # exception; everything else must be a word ui_breakpoint.h parses.
    run python3 "$DERIVE" list
    [ "$status" -eq 0 ] || fail "list failed: $output"
    for p in $output; do
        run python3 "$DERIVE" splash-3d-sizes "$p"
        [ "$status" -eq 0 ] || fail "splash-3d-sizes $p failed: $output"
        for cls in $output; do
            case "$cls" in
                micro|tiny|small|medium|large|xlarge|xxlarge|ultrawide) ;;
                *) fail "$p selects '$cls', which is not a breakpoint tier" ;;
            esac
        done
    done
}

@test "deriver: a runtime-variable panel restricts nothing" {
    run python3 "$DERIVE" splash-3d-sizes pi
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ -z "$output" ]
}

@test "deriver: an unknown platform fails instead of silently selecting everything" {
    run python3 "$DERIVE" splash-3d-sizes not-a-platform
    [ "$status" -ne 0 ] || fail "expected a non-zero exit, got: $output"
    contains "unknown platform" "$output"
}

@test "deriver: printer image size follows the effective width, not the panel width" {
    # The K2 panel is 480 wide and would select 150 if rotation were ignored.
    run python3 "$DERIVE" printer-image-size k2
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ "$output" = "300" ]
}

@test "deriver: every platform in the manifest answers every derived question" {
    run python3 "$DERIVE" list
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ -n "$output" ]
    for p in $output; do
        run python3 "$DERIVE" splash-3d-sizes "$p"
        [ "$status" -eq 0 ] || fail "splash-3d-sizes $p failed: $output"
        run python3 "$DERIVE" effective-resolution "$p"
        [ "$status" -eq 0 ] || fail "effective-resolution $p failed: $output"
    done
}

# --------------------------------------------------------------------------
# The gate
# --------------------------------------------------------------------------

@test "gate: advisory mode exits 0 even with findings" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}

@test "gate: --strict exits non-zero while findings remain" {
    run python3 "$GATE" --strict
    [ "$status" -eq 1 ]
}

@test "gate: the unmodified tree reports no prerequisite or literal findings" {
    make_tree
    run python3 "$GATE" --quiet --root "$TREE"
    lacks "gen-splash-3d" "$output"
    lacks "hardcodes --sizes" "$output"
}

@test "gate: reports a package target wired to another platform's splash classes" {
    make_tree
    sed -i 's|^package-k2: \(.*\)gen-splash-3d-k2|package-k2: \1gen-splash-3d-k1|' "$TREE/mk/cross.mk"
    run python3 "$GATE" --quiet --root "$TREE"
    contains "package-k2 depends on gen-splash-3d-k1" "$output"
}

@test "gate: reports a package target with no splash prerequisite at all" {
    make_tree
    sed -i 's|^package-k2: \(.*\) gen-splash-3d-k2|package-k2: \1|' "$TREE/mk/cross.mk"
    run python3 "$GATE" --quiet --root "$TREE"
    contains "package-k2 has no gen-splash-3d prerequisite" "$output"
}

@test "gate: reports a size class hardcoded back into the build files" {
    make_tree
    printf '\ngen-splash-3d-regression:\n\t$(SPLASH_3D_PYTHON) x --sizes medium\n' >> "$TREE/mk/images.mk"
    run python3 "$GATE" --quiet --root "$TREE"
    contains "hardcodes --sizes" "$output"
}

@test "gate: reports an install root the discovery lists have not been taught" {
    make_tree
    python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); \
d["platforms"]["k2"]["storage"]["root"]="/brand-new-mount/helixscreen"; \
json.dump(d,open(p,"w"),indent=2)' "$TREE/assets/config/platforms.json"
    run python3 "$GATE" --quiet --root "$TREE"
    contains "/brand-new-mount/helixscreen" "$output"
}

@test "gate: reports a platform whose panel is not hardware-verified" {
    run python3 "$GATE" --quiet
    contains "not hardware-verified" "$output"
}

@test "gate: a missing build directory is silent, not a failure" {
    run python3 "$GATE" --strict --build-dir "$WORK/no-such-build"
    lacks "renders" "$output"
}

@test "gate: every platform named by a package target has a manifest entry" {
    run python3 "$GATE" --quiet
    lacks "no manifest entry" "$output"
}

# --------------------------------------------------------------------------
# prune-assets: what actually bounds the shipped payload
# --------------------------------------------------------------------------

# A staged release tree with both printer sizes, every splash class, and source
# PNGs - the shape a release has before pruning.
make_stage() {
    STAGE="$WORK/stage"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/assets/images/prerendered" "$STAGE/assets/images/printers/prerendered"
    for cls in micro tiny small medium large xlarge ultrawide; do
        for mode in light dark; do
            echo x > "$STAGE/assets/images/prerendered/splash-3d-$mode-$cls.bin"
        done
    done
    for cls in tiny medium large xlarge; do
        echo x > "$STAGE/assets/images/prerendered/splash-logo-$cls.bin"
    done
    for printer in creality-k2 generic-corexy voron-v2; do
        echo x > "$STAGE/assets/images/printers/$printer.png"
        for size in 150 300; do
            echo x > "$STAGE/assets/images/printers/prerendered/$printer-$size.bin"
        done
    done
}

@test "prune: keeps only the splash class the panel selects" {
    make_stage
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    run ls "$STAGE/assets/images/prerendered"
    contains "splash-3d-dark-medium.bin" "$output"
    lacks "splash-3d-dark-micro.bin" "$output"
    lacks "splash-3d-dark-xlarge.bin" "$output"
    lacks "ultrawide" "$output"
}

@test "prune: keeps only the printer render size the panel selects" {
    make_stage
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    run ls "$STAGE/assets/images/printers/prerendered"
    contains "creality-k2-300.bin" "$output"
    lacks "creality-k2-150.bin" "$output"
}

@test "prune: a 480x272 panel keeps micro and the 150px renders" {
    make_stage
    run python3 "$DERIVE" prune-assets cc1 "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    run ls "$STAGE/assets/images/prerendered"
    contains "splash-3d-dark-micro.bin" "$output"
    lacks "splash-3d-dark-medium.bin" "$output"
    run ls "$STAGE/assets/images/printers/prerendered"
    contains "voron-v2-150.bin" "$output"
    lacks "voron-v2-300.bin" "$output"
}

@test "prune: the generic fallback render always survives" {
    # get_prerendered_printer_path falls back to generic-corexy for a printer it
    # has no art for. Pruning that would turn a missing image into no image.
    make_stage
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    [ -f "$STAGE/assets/images/printers/prerendered/generic-corexy-300.bin" ] || \
        fail "generic fallback was pruned"
}

@test "prune: drops source PNGs once every printer has a render" {
    make_stage
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    contains "dropped 3 source PNG" "$output"
    run ls "$STAGE/assets/images/printers"
    lacks ".png" "$output"
}

@test "prune: KEEPS source PNGs when a printer has no render at that size" {
    # The PNG is the fallback for a printer with no prerendered art. Dropping it
    # would silently degrade that printer to the generic image.
    make_stage
    echo x > "$STAGE/assets/images/printers/exotic-printer.png"
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    contains "kept source PNGs" "$output"
    [ -f "$STAGE/assets/images/printers/exotic-printer.png" ] || fail "uncovered PNG was pruned"
    [ -f "$STAGE/assets/images/printers/voron-v2.png" ] || fail "covered PNG pruned despite the hold"
}

@test "prune: a runtime-variable panel keeps everything" {
    make_stage
    local before
    before="$(find "$STAGE" -type f | wc -l)"
    run python3 "$DERIVE" prune-assets pi "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    contains "not known until runtime" "$output"
    [ "$(find "$STAGE" -type f | wc -l)" -eq "$before" ] || fail "pruned a variable-panel package"
}

@test "prune: running twice removes nothing the second time" {
    make_stage
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    [ "$status" -eq 0 ] || fail "first prune failed: $output"
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    [ "$status" -eq 0 ] || fail "second prune failed: $output"
    contains "removed 0 file(s)" "$output"
}

@test "prune: a missing staged root is an error, not a silent success" {
    run python3 "$DERIVE" prune-assets k2 "$WORK/no-such-release"
    [ "$status" -ne 0 ] || fail "prune reported success for a root that does not exist"
}

# --------------------------------------------------------------------------
# Tracker music: a platform fact, asked of the platform
# --------------------------------------------------------------------------

@test "prune: drops tracker music on a platform with no tracker player" {
    make_stage
    mkdir -p "$STAGE/assets/sounds"
    echo x > "$STAGE/assets/sounds/theme.mod"
    run python3 "$DERIVE" prune-assets k2 "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    contains "dropped assets/sounds" "$output"
    [ ! -f "$STAGE/assets/sounds/theme.mod" ] || fail "tracker music shipped to a platform that cannot play it"
}

@test "prune: keeps tracker music where the player is compiled in" {
    # ad5x drives the tracker's synth fallback through jz_pwm, so its music is
    # playable and must survive.
    make_stage
    mkdir -p "$STAGE/assets/sounds"
    echo x > "$STAGE/assets/sounds/theme.mod"
    run python3 "$DERIVE" prune-assets ad5x "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    lacks "dropped assets/sounds" "$output"
    [ -f "$STAGE/assets/sounds/theme.mod" ] || fail "pruned music a platform can play"
}

@test "prune: sound is judged even when the panel is unknown at build time" {
    # The early return for a variable panel must not skip the sound question.
    make_stage
    mkdir -p "$STAGE/assets/sounds"
    echo x > "$STAGE/assets/sounds/theme.mod"
    run python3 "$DERIVE" prune-assets pi "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    [ -f "$STAGE/assets/sounds/theme.mod" ] || fail "pi has a tracker; its music was pruned"
}

@test "prune: a platform with sound but no tracker still loses the music" {
    # AD5M has a PWM buzzer for tone SFX but no tracker: its single core
    # busy-waits and kills prints.
    make_stage
    mkdir -p "$STAGE/assets/sounds"
    echo x > "$STAGE/assets/sounds/theme.mod"
    run python3 "$DERIVE" prune-assets ad5m "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    contains "dropped assets/sounds" "$output"
}

@test "every platform declares whether it has a tracker" {
    run python3 "$DERIVE" list
    [ "$status" -eq 0 ] || fail "list failed: $output"
    for p in $output; do
        run python3 "$DERIVE" get "$p" sound.has_tracker
        [ "$status" -eq 0 ] || fail "$p: sound.has_tracker lookup failed"
        case "$output" in
            true|false) ;;
            *) fail "$p declares sound.has_tracker as '$output'" ;;
        esac
    done
}

# --------------------------------------------------------------------------
# Assets stripped from every release because nothing displays them
# --------------------------------------------------------------------------

# release-clean-assets deletes these outright. That is only safe while no
# production code registers them, so this is the tripwire: if someone starts
# using one, the test fails and points at the strip rather than letting a
# release ship a reference to a file it removed.
@test "stripped top-level images have no production consumer" {
    for f in "printer.png" "orcaslicer test cube.PNG"; do
        run grep -rIl --fixed-strings "$f" src/ include/ ui_xml/ assets/config/
        [ -z "$output" ] || fail "$f is stripped from releases but referenced by: $output"
    done
}

@test "the image the app actually registers is not stripped" {
    # printer_400.png is the registered one; stripping it would be the mistake
    # the case above exists to prevent, in the other direction.
    run grep -rIl --fixed-strings "printer_400.png" src/
    [ -n "$output" ] || fail "printer_400.png is no longer registered; revisit the strip list"
    run grep -c "rm -f .*assets/images/printer.png" mk/cross.mk
    [ "$output" = "1" ] || fail "expected exactly one strip rule for printer.png, got $output"
    # Look for a deletion of it, not a mention: the strip rule's comment names
    # printer_400.png to explain which image survives.
    run grep -cE "(rm -f|-delete).*printer_400" mk/cross.mk
    [ "$output" = "0" ] || fail "mk/cross.mk deletes printer_400.png, which the app registers"
}
