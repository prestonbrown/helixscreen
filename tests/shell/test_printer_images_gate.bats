#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_printer_images.py: every image
# printer_database.json names (an entry's own and each image_variants one) is
# on disk, since a missing one silently renders generic-corexy. KNOWN_MISSING
# is a ratchet: a listed image stays quiet until its art lands, and then the
# gate fails until the entry leaves the list.
#
# The gate reads the database and images relative to its own location, so each
# test runs a copy inside a scratch tree.

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"
    mkdir -p "$WORK/scripts" "$WORK/assets/config" "$WORK/assets/images/printers"
    cp scripts/check_printer_images.py "$WORK/scripts/"
    touch "$WORK/assets/images/printers/voron-2.png" "$WORK/assets/images/printers/ender-3.png"
    # The real KNOWN_MISSING entry, read from the gate so the fixture follows it.
    KNOWN="$(python3 -c 'import ast,sys
t = ast.parse(open(sys.argv[1]).read())
for n in t.body:
    if isinstance(n, ast.AnnAssign) and getattr(n.target, "id", "") == "KNOWN_MISSING":
        print(sorted(ast.literal_eval(n.value))[0])' scripts/check_printer_images.py)"
    [ -n "$KNOWN" ] || fail "could not read KNOWN_MISSING from the gate"
}

db() {
    printf '%s\n' "$1" > "$WORK/assets/config/printer_database.json"
    run python3 "$WORK/scripts/check_printer_images.py"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags an entry whose image is not on disk" {
    db '{"printers": [{"id": "voron_2", "image": "voron-2.png"},
                      {"id": "bambu_x1", "image": "bambu-x1.png"}]}'
    [ "$status" -eq 1 ]
    contains "bambu-x1.png  <- bambu_x1" "$output"
    lacks "voron-2.png" "$output"
}

@test "flags a missing image_variants image" {
    db '{"printers": [{"id": "ender", "image": "ender-3.png",
                       "image_variants": [{"image": "ender-3-s1.png"}]}]}'
    [ "$status" -eq 1 ]
    contains "ender-3-s1.png" "$output"
}

@test "a KNOWN_MISSING image whose art now exists fails until it leaves the list" {
    touch "$WORK/assets/images/printers/$KNOWN"
    db "{\"printers\": [{\"id\": \"k\", \"image\": \"$KNOWN\"}]}"
    [ "$status" -eq 1 ]
    contains "now have artwork" "$output"
    contains "$KNOWN" "$output"
}

# ------------------------------------------------ idioms that must stay QUIET

@test "every reference resolving passes" {
    db '{"printers": [{"id": "voron_2", "image": "voron-2.png",
                       "image_variants": [{"image": "ender-3.png"}, "not-a-dict"]}]}'
    [ "$status" -eq 0 ]
    contains "every database reference resolves" "$output"
}

@test "an entry with no image, or an empty one, is not a reference" {
    db '{"printers": [{"id": "a"}, {"id": "b", "image": ""}]}'
    [ "$status" -eq 0 ]
}

@test "a KNOWN_MISSING image with no art stays quiet" {
    db "{\"printers\": [{\"id\": \"k\", \"image\": \"$KNOWN\"}]}"
    [ "$status" -eq 0 ]
    contains "1 known-missing" "$output"
}

@test "--list reports without failing and marks new versus known" {
    db "{\"printers\": [{\"id\": \"k\", \"image\": \"$KNOWN\"}, {\"id\": \"n\", \"image\": \"new.png\"}]}"
    run python3 "$WORK/scripts/check_printer_images.py" --list
    [ "$status" -eq 0 ]
    contains "(known)" "$output"
    contains "(NEW)" "$output"
}

@test "quality-checks runs the gate from qc_design_pixels" {
    grep -q 'python3 scripts/check_printer_images.py' scripts/qc/design_pixels.sh
}
