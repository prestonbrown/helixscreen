#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Give printer_data/config/helixscreen/ to a non-root service user, so the app
# can save settings.json there. The one statement of that rule: the installer
# runs this after its last write into the directory, and the unit's root
# ExecStartPre (refresh-service-units.sh) runs it on every start, which is what
# repairs a printer that only ever updates in-app, where sudo is blocked.
#
# Usage: fix-user-config-owner.sh USER GROUP DIR
# Exits 0 when there is nothing to do or the repair worked, non-zero only when
# chown was refused, so a caller can retry with more privilege.
#
# A root service can already write the directory, and where another account owns
# printer_data (lava on the U1) that ownership stays. The directory sits in a
# tree the service user can write, so it is never followed if it is a symlink,
# and -h changes a link inside it rather than the file the link points at.
#
# A tree with the right owner costs one find and no chown, so normal starts do
# no recursive writes. Group alone does not trigger a repair: it does not stop
# the app writing, and a user may have changed it on purpose.

user="${1:-}"
group="${2:-$user}"
dir="${3:-}"

[ -n "$user" ] && [ "$user" != "root" ] || exit 0
[ -n "$dir" ] && [ -d "$dir" ] && [ ! -L "$dir" ] || exit 0

[ -n "$(find "$dir" ! -user "$user" -print 2>/dev/null | head -n 1)" ] || exit 0

chown -Rh "${user}:${group}" "$dir" 2>/dev/null
