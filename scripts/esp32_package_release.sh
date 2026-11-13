#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# esp32_package_release.sh - Bundle a built K-Touch firmware into the release zip.
#
# Usage: esp32_package_release.sh TAG [BUILD_DIR] [OUT_DIR]
#
#   TAG        Release tag, e.g. v1.1.0-beta.4 (names the zip)
#   BUILD_DIR  ESP-IDF build directory (default firmware/helixscreen-esp32/build)
#   OUT_DIR    Where the zip lands (default releases)
#
# Writes OUT_DIR/helixscreen-esp32-ktouch-TAG.zip holding one folder with:
#   helixscreen-esp32-ktouch-factory.bin  every image merged, flashed at 0x0
#   flash_args + the images it names      for an update that keeps WiFi
#   README.txt                            both esptool commands
#   helixscreen-esp32-ktouch-logtok.json  the token database that turns this build's
#                                         console log into text (detokenize_log.py)
#
# Needs esptool importable by $PYTHON (default python3): the ESP-IDF image's
# python has it. Called by .github/workflows/esp32-build.yml when releasing.

set -euo pipefail

TAG="${1:?usage: esp32_package_release.sh TAG [BUILD_DIR] [OUT_DIR]}"
BUILD_DIR="${2:-firmware/helixscreen-esp32/build}"
OUT_DIR="${3:-releases}"
PYTHON="${PYTHON:-python3}"

NAME="helixscreen-esp32-ktouch-${TAG}"
FACTORY="helixscreen-esp32-ktouch-factory.bin"
LOGTOK="helixscreen-esp32-ktouch-logtok.json"
PARTITIONS="$BUILD_DIR/../partitions.csv"

[ -f "$BUILD_DIR/flash_args" ] || { echo "error: no flash_args in $BUILD_DIR (build first)" >&2; exit 1; }
# Without its database a release's console log cannot be read at all.
[ -f "$BUILD_DIR/helixscreen_esp32.logtok.json" ] ||
    { echo "error: no helixscreen_esp32.logtok.json in $BUILD_DIR (build first)" >&2; exit 1; }
python3 -c 'import json, sys; sys.exit(0 if json.load(open(sys.argv[1])).get("tokens") else 1)' \
    "$BUILD_DIR/helixscreen_esp32.logtok.json" ||
    { echo "error: $BUILD_DIR/helixscreen_esp32.logtok.json has no tokens (built with CONFIG_HELIX_LOG_TOKENIZE off?)" >&2; exit 1; }
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/$NAME"
mkdir -p "$PKG"

# flash_args: an options line, then "<offset> <relative path>" per image.
cp "$BUILD_DIR/flash_args" "$PKG/"
tail -n +2 "$BUILD_DIR/flash_args" | while read -r _offset path; do
    [ -n "$path" ] || continue
    mkdir -p "$PKG/$(dirname "$path")"
    cp "$BUILD_DIR/$path" "$PKG/$path"
done

cp "$BUILD_DIR/helixscreen_esp32.logtok.json" "$PKG/$LOGTOK"

# No --fill-flash-size: the image must stop short of the cfg partition, so a
# fresh install leaves saved settings alone.
(cd "$PKG" && "$PYTHON" -m esptool --chip esp32s3 merge_bin -o "$FACTORY" @flash_args)

cfg_offset=$(awk -F, '$1 == "cfg" { gsub(/ /, "", $4); print $4 }' "$PARTITIONS")
[ -n "$cfg_offset" ] || { echo "error: no cfg partition in $PARTITIONS" >&2; exit 1; }
size=$(wc -c < "$PKG/$FACTORY" | tr -d ' ')
if [ "$size" -gt $((cfg_offset)) ]; then
    printf 'error: %s is %d bytes and would overwrite cfg at %s\n' "$FACTORY" "$size" "$cfg_offset" >&2
    exit 1
fi

cat > "$PKG/README.txt" <<EOF
HelixScreen ${TAG} for the BigTreeTech K-Touch (ESP32-S3) - ALPHA

Install esptool once:   pipx install esptool  (or pip install esptool in a virtualenv)
Connect the K-Touch by USB-C. Its port is /dev/ttyUSB0 on Linux,
/dev/cu.usbserial-* or /dev/cu.wchusbserial* on macOS, COMx on Windows (CH340 driver).
Run the commands from inside this folder. If a write fails, retry with -b 115200.

Fresh install (also wipes saved WiFi; settings are kept):
  python3 -m esptool --chip esp32s3 -p PORT -b 460800 write_flash 0x0 ${FACTORY}

Update an existing HelixScreen install (keeps WiFi and settings):
  python3 -m esptool --chip esp32s3 -p PORT -b 460800 write_flash @flash_args

The serial console prints most log lines as "$..." records. To read a capture:
  python3 scripts/detokenize_log.py ${LOGTOK} serial.log
(scripts/detokenize_log.py is in the HelixScreen source tree.)

Full guide: https://github.com/prestonbrown/helixscreen/blob/main/docs/user/INSTALL.md
EOF

rm -f "$OUT_DIR/$NAME.zip"
(cd "$STAGE" && zip -qr "$OUT_DIR/$NAME.zip" "$NAME")
echo "Packaged $OUT_DIR/$NAME.zip ($FACTORY: $size bytes, cfg at $cfg_offset)"
