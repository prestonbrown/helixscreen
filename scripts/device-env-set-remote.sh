#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Device half of scripts/device-env-set.sh — piped to `sh -s` over ssh, and run
# directly by tests/shell/test_device_env_set.bats. Kept in its own file so the
# logic that touches a printer's config can be exercised without a printer.
#
# Usage: sh device-env-set-remote.sh <env-file> <KEY> <VALUE>
# POSIX sh only: the K1, AD5M, CC1 and K2 all run BusyBox ash.

set -eu

ENV_FILE="$1"
KEY="$2"
VALUE="$3"

# PI_DEPLOY_DIR is `~/helixscreen`. The path arrives as a positional parameter,
# and a tilde inside a variable is NEVER expanded by the shell — left alone,
# mkdir -p would create a directory literally named "~" in the home dir and
# write the config into it, leaving the real env file untouched while reporting
# success. rsync-based steps get away with it because their paths go through a
# remote shell unquoted; this one does not.
# shellcheck disable=SC2088  # matching the LITERAL tilde is the point here
case "$ENV_FILE" in
    "~/"*) ENV_FILE="$HOME/${ENV_FILE#\~/}" ;;
    "~")   ENV_FILE="$HOME" ;;
esac

# Follow a symlink to its target before editing: the Snapmaker U1 install points
# the in-tree env file at /oem/printer_data/config/helixscreen/helixscreen.env,
# and `sed -i` on a symlink replaces the LINK with a regular file, silently
# detaching the device's real config.
if [ -L "$ENV_FILE" ]; then
    ENV_FILE="$(readlink -f "$ENV_FILE" 2>/dev/null || echo "$ENV_FILE")"
fi

if [ -f "$ENV_FILE" ] && grep -q "^${KEY}=${VALUE}\$" "$ENV_FILE"; then
    echo "  ${KEY}=${VALUE} already set in ${ENV_FILE}"
    exit 0
fi

mkdir -p "$(dirname "$ENV_FILE")"
[ -f "$ENV_FILE" ] || : > "$ENV_FILE"
# One backup, taken before the FIRST modification. Re-copying on every deploy
# would overwrite the pristine original with an already-modified one.
[ -f "${ENV_FILE}.helix-bak" ] || cp "$ENV_FILE" "${ENV_FILE}.helix-bak"

# Matches a live "KEY=" and a commented-out "#KEY=" / "# KEY=" alike, so the
# stock env template's documented-but-disabled entries get flipped on in place
# instead of gaining a duplicate further down the file, where the later
# definition would win by accident rather than by intent.
if grep -q "^#\{0,1\}[[:space:]]*${KEY}=" "$ENV_FILE"; then
    sed -i "s|^#\{0,1\}[[:space:]]*${KEY}=.*|${KEY}=${VALUE}|" "$ENV_FILE"
    echo "  ${KEY}=${VALUE} updated in ${ENV_FILE}"
else
    printf '\n# Set by scripts/device-env-set.sh at deploy time\n%s=%s\n' \
        "$KEY" "$VALUE" >> "$ENV_FILE"
    echo "  ${KEY}=${VALUE} added to ${ENV_FILE}"
fi
