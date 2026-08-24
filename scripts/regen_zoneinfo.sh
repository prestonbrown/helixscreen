#!/usr/bin/env bash
# Regenerate assets/zoneinfo/ from the host system's /usr/share/zoneinfo/.
#
# Ships a minimal TZif zoneinfo subset with the helixscreen install so that
# `setenv("TZ", "America/New_York") + tzset()` works on devices that don't
# bundle tzdata (notably Elegoo Centauri Carbon running OpenCentauri COSMOS).
#
# The zone list is DERIVED from TIMEZONE_ENTRIES in
# src/system/display_settings_manager.cpp — that table is the single source of
# truth. Add a zone there and re-run this script; there is no list to keep in
# sync by hand. tests/shell/test_zoneinfo_bundle_gate.bats fails if the bundle
# on disk drifts from the table.
#
# Usage: scripts/regen_zoneinfo.sh          # regenerate assets/zoneinfo/
#        scripts/regen_zoneinfo.sh --list   # print the derived zone list only
set -euo pipefail

SRC="/usr/share/zoneinfo"
DEST_REL="assets/zoneinfo"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${REPO_ROOT}/${DEST_REL}"
TABLE_SRC="${REPO_ROOT}/src/system/display_settings_manager.cpp"

# Pull the IANA ids out of the TIMEZONE_ENTRIES array: the second string of
# each {"Display (+0:00)", "Area/Location"} pair, scoped to the array body so
# no other brace-and-quote construct in the file can leak in.
extract_zones() {
    sed -n '/^static const TimezoneEntry TIMEZONE_ENTRIES\[\] = {/,/^};/p' "$TABLE_SRC" |
        sed -n 's/.*{[[:space:]]*"[^"]*"[[:space:]]*,[[:space:]]*"\([^"]*\)"[[:space:]]*}.*/\1/p'
}

if [ ! -f "$TABLE_SRC" ]; then
    echo "ERROR: cannot find $TABLE_SRC" >&2
    exit 1
fi

mapfile -t ZONES < <(extract_zones)

if [ "${#ZONES[@]}" -eq 0 ]; then
    echo "ERROR: parsed zero zones from TIMEZONE_ENTRIES in $TABLE_SRC" >&2
    echo "       The array's shape probably changed — update extract_zones()." >&2
    exit 1
fi

if [ "${1:-}" = "--list" ]; then
    printf '%s\n' "${ZONES[@]}"
    exit 0
fi

if [ ! -d "$SRC" ]; then
    echo "ERROR: $SRC does not exist on this host — install tzdata first." >&2
    exit 1
fi

# Verify every zone resolves BEFORE removing the existing bundle, so a host
# with incomplete tzdata cannot leave the tree with a half-populated
# assets/zoneinfo/ (which would ship zones silently missing from the picker).
missing=()
for z in "${ZONES[@]}"; do
    [ -f "$SRC/$z" ] || missing+=("$z")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "ERROR: host tzdata is missing ${#missing[@]} zone(s):" >&2
    printf '  %s\n' "${missing[@]}" >&2
    echo "       assets/zoneinfo/ left untouched. Update tzdata and retry." >&2
    exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST"

for z in "${ZONES[@]}"; do
    dest_file="$DEST/$z"
    mkdir -p "$(dirname "$dest_file")"
    cp "$SRC/$z" "$dest_file"
done

total_bytes=$(du -sb "$DEST" | cut -f1)
total_files=$(find "$DEST" -type f | wc -l)
echo "Regenerated $total_files zone files in $DEST_REL/ (${total_bytes} bytes)"
