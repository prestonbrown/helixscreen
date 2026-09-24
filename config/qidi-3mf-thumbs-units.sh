#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install or refresh the QIDI .3mf thumbnail helper units
# (prestonbrown/helixscreen#1713).
#
# One copy of the gate, shared by both callers so they cannot drift:
#   - scripts/lib/installer/competing_uis.sh runs this (via $SUDO) at install
#     time, passing the Klipper user it resolved;
#   - config/refresh-service-units.sh runs it as root after an update
#     replaces the install dir; the in-app update path has no sudo under
#     NoNewPrivileges, so that refresh is how an updating machine gets them.
#
# The gate is a capability, not a vendor: a Moonraker whose file_manager
# metadata.py has generate_thumb_path advertises
# gcodes/.thumbs/<subdir>/<stem>/plate_N.png for every .3mf without writing
# it. Only QIDI's Moonraker fork carries that function.
#
# Usage: qidi-3mf-thumbs-units.sh <user> [group]
# <user> is the Klipper/Moonraker user -- the one helixscreen.service runs
# as, so the helper's .thumbs output is owned by the user that reads it.
# Runs as whatever the caller is; writing the units needs root (sudo from the
# installer, root from the update service).
#
# HELIX_QIDI_HOME overrides the user's home and HELIX_QIDI_GCODES_DIR the
# gcodes root (tests, odd mounts). Always exits 0: a failed capability check
# is a skip, not an error.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIR="$(dirname "$SCRIPT_DIR")"

qlog() { echo "qidi-3mf-thumbs-units: $*" >&2; }

UNIT_USER="${1:-}"
UNIT_GROUP="${2:-${1:-}}"

command -v systemctl >/dev/null 2>&1 || exit 0

# No root fallback: root-owned thumbnails in a user-owned gcodes tree outlive
# every later regeneration.
if [ -z "$UNIT_USER" ] || [ "$UNIT_USER" = "root" ]; then
    qlog "no non-root service user to run the helper as -- skipping"
    exit 0
fi

user_home="${HELIX_QIDI_HOME:-$(eval echo "~${UNIT_USER}" 2>/dev/null || true)}"

# Same discovery order as moonraker.sh: the klipper home's moonraker first,
# then any MOONRAKER_SRC_PATHS; three sub-layouts per root.
metadata=""
for root in "${user_home}/moonraker" ${MOONRAKER_SRC_PATHS:-}; do
    [ -n "$root" ] || continue
    for sub in "$root/moonraker/components/file_manager/metadata.py" \
               "$root/components/file_manager/metadata.py" \
               "$root/moonraker/moonraker/components/file_manager/metadata.py"; do
        if [ -f "$sub" ]; then
            metadata="$sub"
            break 2
        fi
    done
done
if [ -z "$metadata" ]; then
    qlog "no Moonraker metadata.py found under ${user_home:-<no home>} -- skipping"
    exit 0
fi
if ! grep -q "generate_thumb_path" "$metadata" 2>/dev/null; then
    qlog "Moonraker metadata.py extracts .3mf thumbnails itself (no generate_thumb_path) -- skipping"
    exit 0
fi

gcodes_dir="${HELIX_QIDI_GCODES_DIR:-${user_home}/printer_data/gcodes}"
if [ ! -d "$gcodes_dir" ]; then
    qlog "gcodes directory not found at $gcodes_dir -- skipping"
    exit 0
fi

python_bin="$(command -v python3 2>/dev/null || true)"
if [ -z "$python_bin" ]; then
    qlog "python3 not found -- skipping"
    exit 0
fi

path_src="${IDIR}/config/helixscreen-3mf-thumbs.path"
svc_src="${IDIR}/config/helixscreen-3mf-thumbs.service"
helper_src="${IDIR}/config/qidi_3mf_thumbs.py"
if [ ! -f "$path_src" ] || [ ! -f "$svc_src" ] || [ ! -f "$helper_src" ]; then
    qlog "payload missing under ${IDIR}/config -- skipping"
    exit 0
fi

path_dest="/etc/systemd/system/helixscreen-3mf-thumbs.path"
svc_dest="/etc/systemd/system/helixscreen-3mf-thumbs.service"

# A private staging dir: this runs as root, and a fixed name under a
# world-writable /tmp could be pre-planted as a symlink.
TMPD="${TMP_DIR:-/tmp}"
mkdir -p "$TMPD" 2>/dev/null || true
stage_dir="$(mktemp -d "${TMPD}/helixscreen-3mf-thumbs.XXXXXX" 2>/dev/null)" || {
    qlog "could not create a staging directory -- skipping"
    exit 0
}
trap 'rm -rf "$stage_dir"' EXIT
staged_path="${stage_dir}/helixscreen-3mf-thumbs.path"
staged_svc="${stage_dir}/helixscreen-3mf-thumbs.service"

# Substitute the unit templates' placeholders into staged copies. Staging
# rather than sed -i on the destination keeps the cmp below meaningful: it
# compares what would be installed against what is installed.
template_unit() {
    sed -e "s|@@GCODES_DIR@@|${gcodes_dir}|g" \
        -e "s|@@INSTALL_DIR@@|${IDIR}|g" \
        -e "s|@@HELIX_USER@@|${UNIT_USER}|g" \
        -e "s|@@HELIX_GROUP@@|${UNIT_GROUP}|g" \
        -e "s|@@PYTHON3@@|${python_bin}|g" \
        "$1" > "$2" 2>/dev/null
}

if ! template_unit "$path_src" "$staged_path" \
    || ! template_unit "$svc_src" "$staged_svc"; then
    qlog "could not template the units -- skipping"
    exit 0
fi

# Copy STAGED over DEST when they differ; sets changed=true when it copies.
changed=false
refresh_unit() {
    if [ -f "$2" ] && cmp -s "$1" "$2"; then
        return 0
    fi
    cp "$1" "$2" || return 1
    changed=true
}

if ! refresh_unit "$staged_path" "$path_dest" \
    || ! refresh_unit "$staged_svc" "$svc_dest"; then
    qlog "could not install the units (need root?) -- skipping"
    exit 0
fi

if [ "$changed" = true ]; then
    qlog "installed QIDI .3mf thumbnail helper units (gcodes root: $gcodes_dir)"
    systemctl daemon-reload 2>/dev/null || true
else
    qlog "QIDI .3mf thumbnail helper units already current"
fi

# Cheap, idempotent state repair even when nothing changed: enabled means the
# boot backfill runs, a started path unit means uploads are watched from now.
systemctl enable helixscreen-3mf-thumbs.service 2>/dev/null || true
systemctl enable helixscreen-3mf-thumbs.path 2>/dev/null || true
systemctl start helixscreen-3mf-thumbs.path 2>/dev/null || true
if [ "$changed" = true ]; then
    # One initial backfill over whatever the stock screen used to maintain.
    systemctl start helixscreen-3mf-thumbs.service 2>/dev/null || true
fi
exit 0
