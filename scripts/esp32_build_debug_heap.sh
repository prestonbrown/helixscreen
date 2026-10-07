#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the K-Touch firmware with heap poisoning and task tracking
# (firmware/helixscreen-esp32/sdkconfig.debug-heap) in its own build directory, so the
# release build and its sdkconfig are untouched, then keeps the ELF that matches the
# image: a crash is only symbolized exactly against the ELF that was flashed.
#
# usage: scripts/esp32_build_debug_heap.sh [build-dir]
#   build-dir   relative to firmware/helixscreen-esp32 (default: build-debug-heap)
#
# Needs docker and the packed asset image the normal firmware build uses
# (build/storage_frogfs.bin, from esp32_stage_assets.py + esp32_pack_assets.py), which it
# copies into its own build directory. Works from a setup-worktree.sh tree: the main tree
# that the worktree's lib/ symlinks point into is mounted too, as esp32_syntax_check.py
# does. Flash from the build directory with its flash_args, as for a normal build.
set -euo pipefail

REPO=$(git rev-parse --show-toplevel)
# The tree holding the repository's .git: the main tree when REPO is a worktree.
PRIMARY=$(dirname "$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir)")
FW="$REPO/firmware/helixscreen-esp32"
BUILD="${1:-build-debug-heap}"
IDF_IMAGE="espressif/idf:v5.5.5"
ARCHIVE="${HELIX_CRASH_ELF_DIR:-$HOME/.helixscreen-crash-elves}"
DOCKER="${DOCKER:-docker}"

# The build refuses to configure without the packed asset image in its build directory.
if [ ! -f "$FW/$BUILD/storage_frogfs.bin" ]; then
    if [ ! -f "$FW/build/storage_frogfs.bin" ]; then
        echo "error: no packed asset image at firmware/helixscreen-esp32/build/storage_frogfs.bin" >&2
        echo "  run: python3 scripts/esp32_stage_assets.py && python3 scripts/esp32_pack_assets.py" >&2
        exit 1
    fi
    mkdir -p "$FW/$BUILD"
    cp "$FW/build/storage_frogfs.bin" "$FW/$BUILD/storage_frogfs.bin"
fi

MOUNTS=(-v "$REPO:$REPO")
if [ "$PRIMARY" != "$REPO" ]; then
    MOUNTS+=(-v "$PRIMARY:$PRIMARY:ro")
fi

# safe.directory: git inside the container (IDF's version stamp) runs as this uid
# against a tree whose .git points into the read-only main tree.
"$DOCKER" run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e HELIX_SDKCONFIG_OVERLAY=sdkconfig.debug-heap \
    "${MOUNTS[@]}" -w "$FW" "$IDF_IMAGE" \
    bash -c "git config --global --add safe.directory '*' && . /opt/esp/idf/export.sh >/dev/null && idf.py -B '$BUILD' -DSDKCONFIG='$BUILD/sdkconfig' build"

ELF="$FW/$BUILD/helixscreen_esp32.elf"
grep -Eq '^CONFIG_HEAP_POISONING_(COMPREHENSIVE|LIGHT)=y' "$FW/$BUILD/sdkconfig" ||
    { echo "error: $BUILD/sdkconfig is missing the debug-heap overlay" >&2; exit 1; }

SHA=$(cut -d' ' -f1 "$ELF.sha256")
NAME="${SHA:0:9}-$(git -C "$REPO" rev-parse --short HEAD)-debug-heap"
mkdir -p "$ARCHIVE"
cp "$ELF" "$ARCHIVE/$NAME.elf"
cp "$ELF.sha256" "$ARCHIVE/$NAME.elf.sha256"

echo "built:   $FW/$BUILD"
echo "ELF:     $ARCHIVE/$NAME.elf"
echo "match:   a crash's 'ELF file SHA256: ${SHA:0:9}' is this ELF"
