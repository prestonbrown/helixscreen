#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Keeps the timezone picker and the bundled TZif database in sync.
#
# TIMEZONE_ENTRIES in src/system/display_settings_manager.cpp is the list the
# user picks from. assets/zoneinfo/ is the TZif subset shipped for devices with
# no system tzdata (Elegoo Centauri Carbon / OpenCentauri COSMOS). When a zone
# is offered but not bundled, glibc silently resolves it to UTC — the picker
# accepts the choice, persists it, and the clock is simply wrong by the user's
# real offset. There is no error to notice.
#
# scripts/regen_zoneinfo.sh used to carry its own hand-copied ZONES array, so
# the two lists could drift with nothing to catch it. It now derives the list
# from the C++ table; these tests pin that it still does, and that the bundle
# on disk matches what the picker offers.

SRC="src/system/display_settings_manager.cpp"
REGEN="scripts/regen_zoneinfo.sh"
BUNDLE="assets/zoneinfo"

# Extract the IANA ids from TIMEZONE_ENTRIES — the second string of each
# {"Display (+0:00)", "Area/Location"} pair, between the array's braces.
extract_ids() {
    sed -n '/^static const TimezoneEntry TIMEZONE_ENTRIES\[\] = {/,/^};/p' "$SRC" |
        sed -n 's/.*{[[:space:]]*"[^"]*"[[:space:]]*,[[:space:]]*"\([^"]*\)"[[:space:]]*}.*/\1/p'
}

# The display half of each pair — what the user actually reads in the dropdown.
extract_labels() {
    sed -n '/^static const TimezoneEntry TIMEZONE_ENTRIES\[\] = {/,/^};/p' "$SRC" |
        sed -n 's/.*{[[:space:]]*"\([^"]*\)"[[:space:]]*,[[:space:]]*"[^"]*"[[:space:]]*}.*/\1/p'
}

@test "TIMEZONE_ENTRIES parses to a non-empty id list" {
    run extract_ids
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -gt 20 ]
}

@test "every offered timezone is bundled in assets/zoneinfo/" {
    local missing=""
    while read -r zone; do
        [ -n "$zone" ] || continue
        if [ ! -f "$BUNDLE/$zone" ]; then
            missing="$missing $zone"
        fi
    done < <(extract_ids)

    if [ -n "$missing" ]; then
        echo "Offered in TIMEZONE_ENTRIES but NOT in $BUNDLE/:$missing"
        echo "Re-run: $REGEN"
        false
    fi
}

@test "assets/zoneinfo/ carries no zone the picker does not offer" {
    local offered orphans=""
    offered=$(extract_ids | LC_ALL=C sort)

    while read -r path; do
        local zone="${path#"$BUNDLE"/}"
        if ! printf '%s\n' "$offered" | grep -qxF "$zone"; then
            orphans="$orphans $zone"
        fi
    done < <(find "$BUNDLE" -type f | LC_ALL=C sort)

    if [ -n "$orphans" ]; then
        echo "Bundled but not offered (dead weight on flash):$orphans"
        echo "Re-run: $REGEN"
        false
    fi
}

@test "regen script derives its zone list from the C++ table" {
    # A literal ZONES=( ... ) array of zone names is the drift bug this gate
    # exists to prevent. The script must read the ids out of the source file.
    run grep -q "display_settings_manager.cpp" "$REGEN"
    [ "$status" -eq 0 ]

    # No hand-maintained array of Area/Location literals.
    run grep -cE '^[[:space:]]+[A-Z][A-Za-z_]+/[A-Za-z_/+-]+$' "$REGEN"
    [ "$output" = "0" ]
}

@test "regen script lists exactly the offered zones when asked" {
    run bash "$REGEN" --list
    [ "$status" -eq 0 ]

    local expected actual
    expected=$(extract_ids | LC_ALL=C sort)
    actual=$(printf '%s\n' "$output" | LC_ALL=C sort)
    [ "$expected" = "$actual" ]
}

@test "no duplicate IANA ids in the picker" {
    local ids dups
    ids=$(extract_ids | LC_ALL=C sort)
    dups=$(printf '%s\n' "$ids" | uniq -d)
    if [ -n "$dups" ]; then
        echo "Duplicate IANA ids in TIMEZONE_ENTRIES: $dups"
        false
    fi
}

@test "UTC+7 is reachable and named for countries, not a colonial region" {
    # #1340: a user in Vietnam could not find UTC+7 because the only entry was
    # labelled "Indochina". The offset must be reachable AND findable by the
    # country names people actually search for.
    run extract_ids
    [ "$status" -eq 0 ]
    printf '%s\n' "$output" | grep -qxF "Asia/Ho_Chi_Minh"
    printf '%s\n' "$output" | grep -qxF "Asia/Bangkok"
    printf '%s\n' "$output" | grep -qxF "Asia/Jakarta"

    # Assert on the dropdown LABELS, not the whole file — the comment above the
    # table names "Indochina" to explain why it was dropped.
    run extract_labels
    [ "$status" -eq 0 ]
    ! printf '%s\n' "$output" | grep -q "Indochina"
}

@test "every offset a label advertises is one a user can actually pick" {
    # A label's "(+7:00)" is the only thing most users match against, so each
    # distinct offset must appear at least once. Guards against a future edit
    # deleting the sole entry for an offset.
    local labels
    labels=$(extract_labels)
    for off in "-11:00" "-10:00" "-8:00" "-6:00" "-5:00" "-3:00" "-2:00" "-1:00" \
        "+0:00" "+1:00" "+2:00" "+3:00" "+5:30" "+7:00" "+8:00" "+9:00" "+12:00" "+13:00"; do
        if ! printf '%s\n' "$labels" | grep -qF "($off)"; then
            echo "No timezone entry advertises offset ($off)"
            false
        fi
    done
}
