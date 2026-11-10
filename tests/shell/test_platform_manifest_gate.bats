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
    mkdir -p "$TREE/mk" "$TREE/assets/config" "$TREE/scripts/lib/installer" "$TREE/src/system" "$TREE/include" "$TREE/config"
    cp assets/config/platforms.json "$TREE/assets/config/"
    cp config/restore-user-config.sh "$TREE/config/"
    cp mk/cross.mk mk/images.mk "$TREE/mk/"
    cp scripts/lib/installer/common.sh scripts/lib/installer/platform.sh "$TREE/scripts/lib/installer/"
    cp src/system/log_collector.cpp src/system/update_checker.cpp \
       src/system/debug_bundle_collector.cpp src/system/helix_cache_dir.cpp "$TREE/src/system/"
    cp include/helix_install_roots.h "$TREE/include/"
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

@test "gate: reports a literal display width handed to the printer-image lookup" {
    make_tree
    cat > "$TREE/src/system/fake_widget.cpp" <<'EOF'
std::string preview(const std::string& stem) {
    return get_prerendered_printer_path(stem, 480);
}
EOF
    run python3 "$GATE" --quiet --root "$TREE"
    contains "literal width 480" "$output"
}

@test "gate: reports a shipped tier size written as a literal" {
    make_tree
    cat > "$TREE/src/system/fake_lookup.cpp" <<'EOF'
std::string tier(const std::string& stem) {
    const int size = 300;
    return "assets/images/printers/prerendered/" + stem + "-" + std::to_string(size) + ".bin";
}
EOF
    run python3 "$GATE" --quiet --root "$TREE"
    contains "literal size 300" "$output"
}

@test "gate: silent on the custom-image cache, which keeps every size" {
    # Custom images are written on the device and nothing prunes them, so naming
    # both sizes there is correct. Flagging it would train people to ignore this.
    make_tree
    cat > "$TREE/src/system/fake_custom.cpp" <<'EOF'
void convert(const std::string& stem) {
    write_bin(custom_dir_ + stem + "-300.bin", 300);
    write_bin(custom_dir_ + stem + "-150.bin", 150);
}
EOF
    run python3 "$GATE" --quiet --root "$TREE"
    lacks "printer-image-sizes" "$output"
}

@test "gate: silent on a doc comment that names a tier file" {
    make_tree
    cat > "$TREE/src/system/fake_doc.cpp" <<'EOF'
/// "A:assets/images/printers/prerendered/creality-k1c-150.bin" -> "creality-k1c"
static std::string basename_of(const std::string& p) { return p; }
EOF
    run python3 "$GATE" --quiet --root "$TREE"
    lacks "printer-image-sizes" "$output"
}

@test "gate: reports an install root the discovery lists have not been taught" {
    make_tree
    python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); \
d["platforms"]["k2"]["storage"]["root"]="/brand-new-mount/helixscreen"; \
json.dump(d,open(p,"w"),indent=2)' "$TREE/assets/config/platforms.json"
    run python3 "$GATE" --quiet --root "$TREE"
    contains "/brand-new-mount/helixscreen" "$output"
}

@test "check_cache_rungs reports a manifest cache_dir the cascade lacks" {
    make_tree
    run python3 "$GATE" --quiet --root "$TREE"
    lacks "[cache-rungs]" "$output"

    python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); \
d["platforms"]["k2"]["storage"]["cache_dir"]="/brand-new-mount/helixscreen-state/cache"; \
json.dump(d,open(p,"w"),indent=2)' "$TREE/assets/config/platforms.json"
    run python3 "$GATE" --quiet --root "$TREE"
    contains "[cache-rungs]" "$output"
    contains "/brand-new-mount/helixscreen-state/cache" "$output"
}

# Edit one storage field of the scratch tree's manifest: platform, key, JSON value
# (null deletes the key).
set_storage() {
    python3 -c 'import json,sys; p,pid,k,v=sys.argv[1:]; d=json.load(open(p)); \
st=d["platforms"][pid]["storage"]; v=json.loads(v); \
st.pop(k,None) if v is None else st.__setitem__(k,v); \
json.dump(d,open(p,"w"),indent=2)' "$TREE/assets/config/platforms.json" "$@"
}

@test "check_user_config_outside_payload reports a config dir inside the payload root" {
    make_tree
    run python3 "$GATE" --quiet --root "$TREE"
    lacks "[config-in-payload]" "$output"

    set_storage ad5x user_config_dir '"/srv/helixscreen/config"'
    run python3 "$GATE" --quiet --root "$TREE"
    contains "[config-in-payload]" "$output"
    contains "ad5x: user_config_dir /srv/helixscreen/config is inside the payload root" "$output"
}

@test "check_user_config_outside_payload reports a platform that names no config dir" {
    make_tree
    set_storage ad5x user_config_dir null
    run python3 "$GATE" --quiet --root "$TREE"
    contains "ad5x: no user_config_dir, so user config stays in /srv/helixscreen/config" "$output"
}

@test "check_user_config_outside_payload checks each firmware's root" {
    make_tree
    set_storage ad5m user_config_dir_by_firmware \
        '{"klipper_mod": "/root/printer_data/config/helixscreen", "zmod": "/srv/helixscreen/config", "forge_x": "/root/printer_data/config/helixscreen"}'
    run python3 "$GATE" --quiet --root "$TREE"
    contains "ad5m (zmod): user_config_dir /srv/helixscreen/config is inside the payload root /srv/helixscreen" "$output"
}

@test "check_user_config_outside_payload reports a config dir the device never resolves" {
    make_tree
    set_storage k1 user_config_dir '"/brand-new-mount/printer_data/config/helixscreen"'
    run python3 "$GATE" --quiet --root "$TREE"
    contains "k1: config/restore-user-config.sh never resolves user_config_dir /brand-new-mount" "$output"
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

@test "prune: a runtime-variable panel keeps every size class" {
    make_stage
    local before
    before="$(find "$STAGE" -type f | wc -l)"
    run python3 "$DERIVE" prune-assets pi "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    contains "not known until runtime" "$output"
    [ "$(find "$STAGE" -type f | wc -l)" -eq "$before" ] || fail "pruned a variable-panel package"
}

@test "prune: a variable panel keeps the source PNGs the image cache reads" {
    # generate_cached_printer_image() sources from the PNG whenever the home
    # widget outgrows the 300px tier, which a large HDMI panel does.
    make_stage
    run python3 "$DERIVE" prune-assets pi "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    [ -f "$STAGE/assets/images/printers/voron-v2.png" ] || fail "pruned a PNG an unbounded panel can need"
}

@test "deriver: a package with candidate boards selects the union of their classes" {
    run python3 "$DERIVE" splash-3d-sizes mips
    [ "$status" -eq 0 ] || fail "command failed: $output"
    [ "$output" = "medium small" ] || fail "mips selected '$output'"
}

@test "prune: candidate boards bound a variable panel's package" {
    # mips is one binary for the K1 and the AD5X, both 800x480 effective.
    make_stage
    run python3 "$DERIVE" prune-assets mips "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    run ls "$STAGE/assets/images/prerendered"
    contains "splash-3d-dark-medium.bin" "$output"
    contains "splash-3d-dark-small.bin" "$output"
    lacks "splash-3d-dark-micro.bin" "$output"
    lacks "splash-3d-dark-xlarge.bin" "$output"
    [ -f "$STAGE/assets/images/printers/prerendered/voron-v2-300.bin" ] || fail "pruned the tier both boards select"
    [ -f "$STAGE/assets/images/printers/prerendered/voron-v2-150.bin" ] || fail "pruned the K1 hedge tier"
    run ls "$STAGE/assets/images/printers"
    lacks ".png" "$output"
}

@test "prune: candidate boards keep the PNGs when one kept tier is incomplete" {
    make_stage
    rm "$STAGE/assets/images/printers/prerendered/voron-v2-150.bin"
    run python3 "$DERIVE" prune-assets mips "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    contains "kept source PNGs" "$output"
    [ -f "$STAGE/assets/images/printers/voron-v2.png" ] || fail "dropped the PNG of a printer missing its 150px render"
}

@test "prune: a candidate without a fixed panel is an error" {
    make_tree
    python3 - "$TREE/assets/config/platforms.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
m["platforms"]["mips"]["panel"]["candidates"] = ["k1", "pi"]
json.dump(m, open(p, "w"))
PY
    run python3 -c "
import sys; sys.path.insert(0, 'scripts')
import platform_manifest as pm
try:
    pm.splash_3d_classes_for(pm.load('$TREE/assets/config/platforms.json'), 'mips')
except pm.ManifestError as e:
    print('refused:', e)"
    contains "refused: mips: candidate 'pi' has no fixed panel" "$output"
}

@test "deriver: an inline candidate panel contributes its own class and tier" {
    # The Creator 5 boards ride the mips binary with no platform entry of their
    # own, so their panels sit inline in the candidate list.
    make_tree
    python3 - "$TREE/assets/config/platforms.json" <<'PY'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
m["platforms"]["mips"]["panel"]["candidates"].append(
    {"board": "tiny-board", "width": 480, "height": 272, "rotate": 0})
json.dump(m, open(p, "w"))
PY
    run python3 -c "
import sys; sys.path.insert(0, 'scripts')
import platform_manifest as pm
m = pm.load('$TREE/assets/config/platforms.json')
print(' '.join(pm.splash_3d_classes_for(m, 'mips')), pm.printer_image_sizes_for(m, 'mips'))"
    [ "$status" -eq 0 ] || fail "derive failed: $output"
    contains "micro" "$output"
    contains "[150, 300]" "$output"
}

@test "prune: drops the translation files no locale loads, on every platform" {
    for p in pi k2; do
        make_stage
        mkdir -p "$STAGE/ui_xml/translations"
        for f in translations.xml en.xml de.xml; do echo x > "$STAGE/ui_xml/translations/$f"; done
        run python3 "$DERIVE" prune-assets "$p" "$STAGE"
        [ "$status" -eq 0 ] || fail "$p: prune failed: $output"
        [ ! -f "$STAGE/ui_xml/translations/translations.xml" ] || fail "$p: shipped the merged catalogue"
        [ ! -f "$STAGE/ui_xml/translations/en.xml" ] || fail "$p: shipped the English identity pack"
        [ -f "$STAGE/ui_xml/translations/de.xml" ] || fail "$p: pruned a locale pack the app loads"
    done
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
# Assets that reach no screen
# --------------------------------------------------------------------------

# release-clean-assets deletes this outright. That is only safe while nothing
# registers it, so this is the tripwire: if someone starts using it, the test
# fails and points at the strip rather than letting a release ship a reference
# to a file it removed.
@test "the stripped test fixture has no consumer" {
    run grep -rIl --fixed-strings "orcaslicer test cube.PNG" src/ include/ ui_xml/ assets/config/
    [ -z "$output" ] || fail "the orcaslicer cube is stripped from releases but referenced by: $output"
}

@test "printer.png is gone and nothing reaches for it" {
    # A 2 MB 1024x1536 source nothing displayed: the app registers
    # printer_400.png and printer art renders at 300px. Deleted rather than
    # stripped, so a reference to it would now be a broken path, not a big one.
    [ ! -f assets/images/printer.png ] || fail "assets/images/printer.png is back"
    run grep -rIl --fixed-strings "assets/images/printer.png" src/ include/ ui_xml/ assets/config/ tests/unit/
    [ -z "$output" ] || fail "assets/images/printer.png no longer exists but is referenced by: $output"
}

@test "the image the app actually registers survives" {
    # printer_400.png is the registered one, and three unit tests now use its
    # path as their distinguishable thumbnail token. Stripping it would be the
    # mistake the cases above exist to prevent, in the other direction.
    run grep -rIl --fixed-strings "printer_400.png" src/
    [ -n "$output" ] || fail "printer_400.png is no longer registered; revisit the strip list"
    [ -f assets/images/printer_400.png ] || fail "printer_400.png is missing from the tree"
    run grep -cE "(rm -f|-delete).*printer_400" mk/cross.mk
    [ "$output" = "0" ] || fail "mk/cross.mk deletes printer_400.png, which the app registers"
}

# --------------------------------------------------------------------------
# Generator and gate wiring
# --------------------------------------------------------------------------

@test "the splash generator's canvas list matches the manifest" {
    # prerender_size_class.cpp says "Must match SCREEN_SIZES in gen_splash_3d.py",
    # which is an instruction to a human rather than a check. This is the check:
    # every class the generator builds must be one the manifest knows, at the
    # same composited width and height, or the app asks for a file the generator
    # never made, or measures one it did make wrongly.
    run python3 - <<'EOF'
import json, re, sys
src = open("scripts/gen_splash_3d.py").read()
block = re.search(r"SCREEN_SIZES = \[(.*?)\]", src, re.S).group(1)
gen = {m[0]: (int(m[1]), int(m[2])) for m in re.findall(r'\("([a-z_-]+)",\s*(\d+),\s*(\d+),', block)}
classes = json.load(open("assets/config/platforms.json"))["size_classes"]
widths = classes["splash_composite_width"]
heights = classes["splash_composite_height"]
bad = [f"{k}: generator {w}x{h}, manifest {widths.get(k)}x{heights.get(k)}"
       for k, (w, h) in gen.items() if (widths.get(k), heights.get(k)) != (w, h)]
missing = [k for k in set(heights) | set(widths) if k not in gen]
if bad or missing:
    print("MISMATCH " + "; ".join(bad + [f"manifest class {k} has no canvas" for k in missing]))
    sys.exit(1)
print("OK %d classes" % len(gen))
EOF
    [ "$status" -eq 0 ] || fail "$output"
    contains "OK" "$output"
}

@test "quality-checks.sh actually runs the platform manifest gate" {
    # A gate nobody invokes reports nothing forever and reads exactly like a
    # passing one. Match an INVOCATION, not a mention: the file also names the
    # script in a comment, so a grep for the filename stays green even when the
    # call itself is gone.
    run grep -cE '^[[:space:]]*(python3|\$\(PY\)|\.venv/bin/python)[[:space:]]+scripts/check_platform_manifest\.py' scripts/qc/decl_ui.sh
    [ "$output" != "0" ] || fail "scripts/qc/decl_ui.sh no longer CALLS the manifest gate"
}

@test "the splash generator accepts every class a platform can select" {
    # A platform selecting a class the generator refuses would fail the package
    # build rather than ship wrong, but it fails late and opaquely.
    run python3 "$DERIVE" list
    [ "$status" -eq 0 ] || fail "list failed"
    local names
    names="$(python3 -c "
import re
src = open('scripts/gen_splash_3d.py').read()
block = re.search(r'SCREEN_SIZES = \[(.*?)\]', src, re.S).group(1)
print(' '.join(re.findall(r'\(\"([a-z_-]+)\",', block)))")"
    for p in $output; do
        run python3 "$DERIVE" splash-3d-sizes "$p"
        for cls in $output; do
            case " $names " in
                *" $cls "*) ;;
                *) fail "$p selects '$cls' but gen_splash_3d.py cannot generate it" ;;
            esac
        done
    done
}

# --------------------------------------------------------------------------
# The shell sweep list, which cannot read the manifest at install time
# --------------------------------------------------------------------------
#
# The installer runs on devices where python is probed for, never assumed, so
# HELIX_INSTALL_DIRS stays a literal. This is what holds that literal to the
# manifest instead of to somebody's memory.

@test "gate: reports a manifest root the uninstall sweep would miss" {
    make_tree
    python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); \
d["platforms"]["k2"]["storage"]["root"]="/brand-new-mount/helixscreen"; \
json.dump(d,open(p,"w"),indent=2)' "$TREE/assets/config/platforms.json"
    run python3 "$GATE" --quiet --root "$TREE"
    contains "HELIX_INSTALL_DIRS does not sweep /brand-new-mount/helixscreen" "$output"
}

@test "gate: the unmodified tree has every manifest root in the sweep" {
    make_tree
    run python3 "$GATE" --quiet --root "$TREE"
    lacks "does not sweep" "$output"
}

@test "gate: home-relative fallbacks are not demanded of the sweep" {
    # kHomeInstallRoots are $KLIPPER_HOME fallbacks for a Pi-class box. An
    # uninstall sweeping a user's home directory would be a worse bug than the
    # one this check exists for.
    make_tree
    run python3 "$GATE" --quiet --root "$TREE"
    lacks "/home/pi/helixscreen is searched" "$output"
    lacks "/home/biqu/helixscreen is searched" "$output"
}

@test "gate: names a root the app reads but the uninstaller never removes" {
    # Built in the sandbox rather than pointed at a real drift: the finding this
    # pins is one the tree is supposed to have none of, so borrowing a live
    # example makes the case disappear the moment someone fixes it.
    make_tree
    python3 - "$TREE/include/helix_install_roots.h" <<'EOF'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8").read()
s = s.replace('inline constexpr const char* kInstallRoots[] = {',
              'inline constexpr const char* kInstallRoots[] = {\n    "/unswept/helixscreen",',
              1)
io.open(p, "w", encoding="utf-8").write(s)
EOF
    run python3 "$GATE" --quiet --root "$TREE"
    contains "/unswept/helixscreen is searched by the app as a payload root" "$output"
}

# --------------------------------------------------------------------------
# prune-assets: CJK filename glyphs (sizes from scripts/translations/cjk_filename_fonts.py)
# --------------------------------------------------------------------------

make_font_stage() {
    make_stage
    mkdir -p "$STAGE/assets/fonts/cjk/filename"
    for px in 10 11 12 14 16 18 20 24 26 28 32 40 48 64; do
        echo x > "$STAGE/assets/fonts/cjk/filename/noto_sans_cjk_filename_$px.bin"
    done
}

@test "prune: a fixed panel keeps only the filename glyph sizes its tier reaches at any UI scale" {
    # 800x480 in a font tier 4 build: font_body 18 steps up to 28 (32 is not
    # linked), font_small light_16 to 18 and font_xs light_12 to 18.
    make_ui_font_stage
    run python3 "$DERIVE" prune-assets ad5m "$STAGE" 4
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    run ls "$STAGE/assets/fonts/cjk/filename"
    [ "$(echo "$output" | tr '\n' ' ')" = "$(printf 'noto_sans_cjk_filename_%s.bin ' 12 14 16 18 20 24 26 28)" ] \
        || fail "800x480 at tier 4 kept: $output"
}

@test "prune: an unbounded panel keeps every filename glyph size" {
    make_font_stage
    run python3 "$DERIVE" prune-assets pi "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    [ "$(ls "$STAGE/assets/fonts/cjk/filename" | wc -l)" -eq 14 ] || fail "pruned a pi filename font"
}

@test "prune: refuses a package missing a filename glyph size its panel draws" {
    make_font_stage
    rm "$STAGE/assets/fonts/cjk/filename/noto_sans_cjk_filename_16.bin"
    run python3 "$DERIVE" prune-assets ad5m "$STAGE"
    [ "$status" -ne 0 ] || fail "pruned a package that cannot draw 16px filenames: $output"
    contains "noto_sans_cjk_filename_16.bin" "$output"
}

# --------------------------------------------------------------------------
# prune-assets: translation CJK bins by the build's HELIX_MAX_FONT_TIER,
# evaluated from include/cjk_font_faces.inc's own guards
# --------------------------------------------------------------------------

make_ui_font_stage() {
    make_font_stage
    git ls-files 'assets/fonts/cjk/*.bin' | grep -v '/filename/' | while read -r f; do
        echo x > "$STAGE/assets/fonts/cjk/$(basename "$f")"
    done
}

@test "prune: a font tier 4 build keeps exactly the CJK bins its face table maps" {
    make_ui_font_stage
    run python3 "$DERIVE" prune-assets ad5m "$STAGE" 4
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    run ls "$STAGE/assets/fonts/cjk"
    contains "noto_sans_cjk_28.bin" "$output"
    contains "noto_sans_cjk_light_18.bin" "$output"
    contains "noto_sans_cjk_bold_28.bin" "$output"
    lacks "noto_sans_cjk_32.bin" "$output"
    lacks "noto_sans_cjk_bold_48.bin" "$output"
    lacks "noto_sans_cjk_light_20.bin" "$output"
    [ "$(ls "$STAGE"/assets/fonts/cjk/*.bin | wc -l)" -eq 23 ] || fail "tier 4 kept: $output"
}

@test "prune: a font tier 6 build keeps every CJK bin" {
    make_ui_font_stage
    before=$(ls "$STAGE"/assets/fonts/cjk/*.bin | wc -l)
    run python3 "$DERIVE" prune-assets pi "$STAGE" 6
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    [ "$(ls "$STAGE"/assets/fonts/cjk/*.bin | wc -l)" -eq "$before" ] || fail "pruned a tier 6 CJK bin"
}

@test "prune: refuses a package missing a CJK bin its build loads" {
    make_ui_font_stage
    rm "$STAGE/assets/fonts/cjk/noto_sans_cjk_28.bin"
    run python3 "$DERIVE" prune-assets ad5m "$STAGE" 4
    [ "$status" -ne 0 ] || fail "pruned a package its build cannot draw 28px CJK from: $output"
    contains "noto_sans_cjk_28.bin" "$output"
}

@test "prune: without a font tier the translation CJK bins are left alone" {
    make_ui_font_stage
    before=$(ls "$STAGE"/assets/fonts/cjk/*.bin | wc -l)
    run python3 "$DERIVE" prune-assets ad5m "$STAGE"
    [ "$status" -eq 0 ] || fail "prune failed: $output"
    [ "$(ls "$STAGE"/assets/fonts/cjk/*.bin | wc -l)" -eq "$before" ] || fail "pruned without a tier"
}

@test "prune: every filename size a label reaches at any UI scale ships in every package" {
    for platform in pi pi32 ad5m cc1 mips k1-dynamic k2 snapmaker-u1 x86 x86-desktop; do
        tier=$(scratch_make -s PLATFORM_TARGET="$platform" print-var-HELIX_MAX_FONT_TIER | tail -n1)
        make_ui_font_stage
        run python3 "$DERIVE" prune-assets "$platform" "$STAGE" "$tier"
        [ "$status" -eq 0 ] || fail "$platform prune failed: $output"
        run python3 - "$platform" "$tier" "$STAGE" <<'PY'
import os, re, sys
sys.path.insert(0, "scripts"); sys.path.insert(0, "scripts/translations")
import platform_manifest as pm, cjk_filename_fonts as cf
platform, tier, stage = sys.argv[1], int(sys.argv[2]), sys.argv[3]
m = pm.load()
panels = pm.candidate_panels(m, platform)
tiers = {pm.font_breakpoint(m, *pm.effective_resolution(e)) for e in panels} or set(cf.TIERS)
text = open("ui_xml/globals.xml", encoding="utf-8").read()
reach = {l.split(":")[0]: l.split(":")[1].split()
         for l in open("scripts/translations/font_scale_reach.txt") if ":" in l}
linked = {f: px for f, _b, px in cf.face_rows(tier)}
missing = []
for t in tiers:
    for token in cf.FILENAME_FONT_TOKENS:
        face = re.search(f'name="{token}_{t}" value="([a-z0-9_]+)"', text).group(1)
        for f in reach[face]:
            path = f"{stage}/assets/fonts/cjk/filename/noto_sans_cjk_filename_{linked[f]}.bin" if f in linked else None
            if path and not os.path.isfile(path):
                missing.append(f"{t}:{token}->{f}")
print(" ".join(missing))
PY
        [ -z "$output" ] || fail "$platform (tier $tier) lacks bins for: $output"
    done
}
