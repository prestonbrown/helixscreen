#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Idempotently set KEY=VALUE in a helixscreen.env on a deployed device.
#
# helixscreen.env is deliberately EXCLUDED from every deploy (see
# DEPLOY_ASSET_EXCLUDES / DEPLOY_TAR_EXCLUDES in mk/cross.mk) so a redeploy
# never clobbers a device's local settings. That is also why a build-time flag
# alone can never turn on a runtime switch: nothing in the deploy writes this
# file. This script is the one thing that does, and only for the key it is told
# about.
#
# Usage: device-env-set.sh <ssh-target> <env-file-path> <KEY> <VALUE>
#
# Behaviour:
#   - existing "KEY=VALUE" with the same value  -> no-op, no backup, no restart
#   - existing "KEY=<other>" or "#KEY=..."      -> rewritten in place
#   - absent                                    -> appended with a comment
#   - file absent                               -> created
# A one-time backup (<file>.helix-bak) is taken before the first modification.
# BusyBox-safe: only sed/grep/mv/printf, no GNU-only flags.

set -euo pipefail

if [ $# -ne 4 ]; then
    echo "usage: $0 <ssh-target> <env-file> <KEY> <VALUE>" >&2
    exit 2
fi

TARGET="$1"
ENV_FILE="$2"
KEY="$3"
VALUE="$4"

case "$KEY" in
    *[!A-Za-z0-9_]* | '')
        echo "device-env-set: refusing malformed key '$KEY'" >&2
        exit 2
        ;;
esac

# The device half lives in its own file so it can be tested without a printer.
# Piped to `sh -s`, not scp'd: nothing is left behind on the device, and this
# works on the tar/ssh platforms (K1, AD5M, K2) that have no rsync.
REMOTE_HALF="$(dirname "$0")/device-env-set-remote.sh"
[ -f "$REMOTE_HALF" ] || { echo "device-env-set: missing $REMOTE_HALF" >&2; exit 1; }

ssh "$TARGET" 'sh -s' -- "$ENV_FILE" "$KEY" "$VALUE" < "$REMOTE_HALF"
