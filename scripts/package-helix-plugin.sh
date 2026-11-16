#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pack the HelixPrint plugin into one gzipped tarball for the release CDN.
#
# The files are the ones moonraker-plugin/remote-install.sh fetches
# (PLUGIN_FILES), at the tarball's top level, so the wrapper unpacks them
# straight into its staging directory. The release workflow uploads the result
# as releases/v<version>/helix-print-plugin.tar.gz.
#
# Usage:
#   scripts/package-helix-plugin.sh OUTPUT.tar.gz
#
# Exits 1, writing nothing, when a listed file is missing.

set -euo pipefail

out="${1:?usage: package-helix-plugin.sh OUTPUT.tar.gz}"
plugin_dir="$(cd "$(dirname "$0")/../moonraker-plugin" && pwd)"

files=$(sed -n 's/^PLUGIN_FILES="\(.*\)"$/\1/p' "$plugin_dir/remote-install.sh")
[ -n "$files" ] || { echo "no PLUGIN_FILES in $plugin_dir/remote-install.sh" >&2; exit 1; }

missing=""
for f in $files; do
    [ -f "$plugin_dir/$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || { echo "missing from $plugin_dir:$missing" >&2; exit 1; }

# Owned by root:root rather than the packing user: a printer that unpacks as
# root would otherwise restore a uid that does not exist there.
if tar --version 2>/dev/null | grep -q 'GNU tar'; then
    owner_flags=(--owner=0 --group=0 --numeric-owner)
else
    owner_flags=(--uid 0 --gid 0)
fi

mkdir -p "$(dirname "$out")"
tmp="$out.tmp.$$"
trap 'rm -f "$tmp"' EXIT
# shellcheck disable=SC2086  # one word per file
tar "${owner_flags[@]}" -czf "$tmp" -C "$plugin_dir" $files
mv "$tmp" "$out"
