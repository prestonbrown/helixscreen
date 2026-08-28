#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: camera
# K2 ustreamer camera setup: replace the stock proprietary WebRTC pipeline
# (which HelixScreen and fluidd cannot consume) with a static ustreamer MJPEG
# server, point both UIs at it, and make uninstall fully restore stock state.
#
# Reuses the framework's reversal machinery: the stock webrtc init scripts are
# disabled via record_disabled_service "sysv-chmod" so the existing
# reenable_disabled_services() (uninstall.sh) chmod +x's them back on uninstall.
# cam_app is hotplug-launched with no watchdog, so killing it is clean and it
# returns on reboot once webrtc is re-enabled.
#
# Reads: INSTALL_DIR, SUDO, _PY_BIN (via _has_python from common.sh),
#        record_disabled_service (competing_uis.sh)
# Writes: /etc/init.d/ustreamer, ${INSTALL_DIR}/config/.disabled_services,
#         ${INSTALL_DIR}/config/.webcams_backup.json,
#         ${INSTALL_DIR}/config/.camera_migrated (marker), moonraker webcams

# Source guard
[ -n "${_HELIX_CAMERA_SOURCED:-}" ] && return 0
_HELIX_CAMERA_SOURCED=1

# Moonraker base URL (host:port). Overridable for tests.
: "${MOONRAKER_URL:=http://127.0.0.1:7125}"

# init.d / rc.d directories. Resolved per-call (not at source time) so the BATS
# suite can redirect them away from the host. Production callers leave the
# HELIX_INITD_DIR / HELIX_RCD_DIR env vars unset.
_initd_dir() { echo "${HELIX_INITD_DIR:-/etc/init.d}"; }
_rcd_dir()   { echo "${HELIX_RCD_DIR:-/etc/rc.d}"; }

# Marker recording that we migrated moonraker's webcam list (so uninstall knows
# to restore from the backup). Lives in INSTALL_DIR/config alongside the backup.
_camera_marker_file()  { echo "${INSTALL_DIR}/config/.camera_migrated"; }
_camera_backup_file()  { echo "${INSTALL_DIR}/config/.webcams_backup.json"; }

# Our webcam entry name in moonraker.
HELIX_WEBCAM_NAME="HelixScreen Camera"

# Marker recording which moonraker.conf we reversibly commented out the
# community "K2-Camera-main" mod's [webcam Default] iframe entry in (so uninstall
# can un-comment it). Lives in INSTALL_DIR/config; one conf path per line.
_k2cam_marker_file() { echo "${INSTALL_DIR}/config/.k2cam_webcam_disabled"; }

# The reversible prefix used to comment out the K2-Camera-main [webcam Default]
# block. Uninstall strips this exact prefix back off.
K2CAM_DISABLE_PREFIX="#helix-k2cam-disabled# "

# --- nginx /webcam/ proxy-block tuning --------------------------------------
# We register the camera through the K2's stock nginx /webcam/ proxy (see the
# URL-form choice in install_camera_k2 -- the relative form survives DHCP lease
# changes). On Tina Linux /var/log is a symlink onto the /tmp tmpfs, the K2
# ships no logrotate, and Creality's Moonraker fork puts the gcode upload temp
# file on that SAME tmpfs via tempfile.gettempdir(). So anything of ours that
# grows in /tmp eventually breaks uploads: the write fails with ENOSPC, the
# exception escapes tornado's body reader, no HTTP response is ever sent, and
# the uploading client just sees "connection reset by peer".
#
# Two directives are needed, and BOTH matter -- either one alone still fills the
# tmpfs:
#
#   access_log off       Without it nginx logs every camera request. Under the
#                        old mjpegstreamer-adaptive registration that was ~5
#                        requests/sec, ~147 MB/day.
#
#   proxy_buffering off  Without it nginx buffers the upstream response to a
#                        temp file under /tmp/lib/nginx/proxy. For a normal
#                        finite response that is harmless, but an MJPEG stream
#                        NEVER ENDS -- measured at ~950 KB/s, which fills a
#                        244 MB tmpfs in about two minutes. This is the one that
#                        bites once the webcam is registered as plain
#                        'mjpegstreamer' (a single persistent
#                        multipart/x-mixed-replace connection) rather than the
#                        per-frame-polling adaptive type.
#
# We scope both to the /webcam/ block -- the traffic we caused -- and leave the
# server-level access_log and everything else alone. Reversible on uninstall.

# nginx config path. Env-overridable so the BATS suite can redirect it off the
# host, same as _initd_dir/_rcd_dir. Resolved per-call, not at source time.
_nginx_conf_path() { echo "${HELIX_NGINX_CONF:-/etc/nginx/nginx.conf}"; }

# The stock K2 access log fed by that proxy.
_nginx_access_log_path() { echo "${HELIX_NGINX_ACCESS_LOG:-/var/log/nginx/fluidd-access.log}"; }

# Marker recording that we edited the block. Name kept as-is for compatibility
# with installs made before proxy_buffering was added to the same edit.
_nginx_accesslog_marker_file() { echo "${INSTALL_DIR}/config/.nginx_webcam_accesslog"; }

# Tag embedded in every line we insert, making both the idempotency check and
# the removal exact rather than pattern-guessed. Deliberately a PREFIX of the
# older 'helix-managed-webcam-accesslog' tag, so a substring delete also cleans
# up lines written by the previous version of this installer.
NGINX_WEBCAM_TAG="helix-managed-webcam"

# True when $2 (a directive name) already appears inside the /webcam/ block,
# whether we put it there or the user did. Scoped to that block only.
_webcam_block_has_directive() {
    awk -v d="$2" '
        /^[[:space:]]*location[[:space:]]+\/webcam\/[[:space:]]*\{/ { inblk = 1; next }
        inblk && /^[[:space:]]*\}/ { inblk = 0 }
        inblk && $1 == d { found = 1 }
        END { exit(found ? 0 : 1) }
    ' "$1"
}

# Validate the edited config and reload nginx. Returns non-zero if `nginx -t`
# rejects it, so the caller can roll back -- we must never be the reason
# someone's web UI stops serving.
_nginx_validate_and_reload() {
    if command -v nginx >/dev/null 2>&1; then
        if ! $SUDO nginx -t >/dev/null 2>&1; then
            return 1
        fi
    fi
    local initd
    initd="$(_initd_dir)"
    if [ -x "${initd}/nginx" ]; then
        $SUDO "${initd}/nginx" reload >/dev/null 2>&1 || true
    fi
    return 0
}

# Add the directives above to the stock /webcam/ proxy block. Idempotent, and
# per-directive: a block that already has one keeps it and gains only the other.
# Every unexpected shape is a clean skip, never an error: no nginx.conf, no
# /webcam/ block (a replaced nginx is not ours to edit), or both already set.
_tune_webcam_proxy_block() {
    local conf need_log need_buf
    conf="$(_nginx_conf_path)"
    [ -f "$conf" ] || return 0

    grep -q '^[[:space:]]*location[[:space:]]\+/webcam/[[:space:]]*{' "$conf" 2>/dev/null || return 0

    need_log=1; need_buf=1
    _webcam_block_has_directive "$conf" access_log && need_log=0
    _webcam_block_has_directive "$conf" proxy_buffering && need_buf=0
    [ "$need_log" -eq 0 ] && [ "$need_buf" -eq 0 ] && return 0

    local backup tmp
    backup="${conf}.helix-bak"
    tmp="${conf}.helix-new"
    $SUDO cp "$conf" "$backup" 2>/dev/null || return 0

    if ! awk -v tag="$NGINX_WEBCAM_TAG" -v nlog="$need_log" -v nbuf="$need_buf" '
        {
            print
            if (!ins && $0 ~ /^[[:space:]]*location[[:space:]]+\/webcam\/[[:space:]]*\{/) {
                match($0, /^[[:space:]]*/)
                indent = substr($0, 1, RLENGTH) "    "
                if (nlog == 1)
                    print indent "access_log off; # " tag ": camera frames would fill the tmpfs log"
                if (nbuf == 1)
                    print indent "proxy_buffering off; # " tag ": endless MJPEG stream would buffer into the tmpfs"
                ins = 1
            }
        }
    ' "$conf" > "$tmp" 2>/dev/null; then
        $SUDO rm -f "$tmp"
        return 0
    fi

    $SUDO cp "$tmp" "$conf"
    $SUDO rm -f "$tmp"

    if ! _nginx_validate_and_reload; then
        log_warn "nginx rejected the config after tuning the /webcam/ block — reverting"
        $SUDO cp "$backup" "$conf"
        $SUDO rm -f "$backup"
        return 1
    fi

    $SUDO rm -f "$backup"
    $SUDO touch "$(_nginx_accesslog_marker_file)"
    log_info "Tuned nginx /webcam/ block (no access log, no proxy buffering) so the camera cannot fill the K2's tmpfs"
    return 0
}

# Reverse _tune_webcam_proxy_block. Strips only our tagged lines, and only when
# the marker says we were the ones who added them.
_restore_webcam_proxy_block() {
    local marker conf
    marker="$(_nginx_accesslog_marker_file)"
    [ -f "$marker" ] || return 0

    conf="$(_nginx_conf_path)"
    if [ -f "$conf" ]; then
        $SUDO sed -i "/${NGINX_WEBCAM_TAG}/d" "$conf" 2>/dev/null || true
        _nginx_validate_and_reload || \
            log_warn "nginx rejected the config after untuning the /webcam/ block"
        log_info "Restored the stock nginx /webcam/ block"
    fi
    $SUDO rm -f "$marker"
    return 0
}

# Empty an already-oversized fluidd-access.log. An affected K2 arrives here with
# its tmpfs already full, so preventing further growth is not enough to unwedge
# uploads. The file is on tmpfs and is cleared on every reboot anyway, and the
# traffic in it is ours, so truncating is safe.
_truncate_oversized_webcam_log() {
    local log max size
    log="$(_nginx_access_log_path)"
    [ -f "$log" ] || return 0

    max="${HELIX_WEBCAM_LOG_MAX_BYTES:-20971520}"
    size="$(wc -c < "$log" 2>/dev/null | tr -d ' ')"
    [ -n "$size" ] || return 0
    [ "$size" -gt "$max" ] 2>/dev/null || return 0

    log_warn "nginx access log is $((size / 1024 / 1024)) MB and filling the tmpfs — truncating"
    log_warn "  (${log} is volatile; it is cleared on every reboot)"
    if [ -n "$SUDO" ]; then
        $SUDO sh -c ': > "$1"' _ "$log" 2>/dev/null || true
    else
        : > "$log" 2>/dev/null || true
    fi
    return 0
}

# Detect the community "K2-Camera-main" mod. It REPLACES Moonraker (backs up
# /usr/share/moonraker -> /usr/share/moonraker_backup, drops in its own copy)
# and ships an iframe camera viewer whose [webcam Default] entry conflicts with
# HelixScreen's ustreamer camera. Signature = the mod's source dir OR its
# Moonraker backup exists. Paths are env-overridable so the BATS suite can point
# them at temp dirs. Returns 0 if detected, 1 otherwise.
_detect_k2_camera_mod() {
    local k2cam_dir k2cam_mr_backup
    k2cam_dir="${HELIX_K2CAM_DIR:-/root/K2-Camera-main}"
    k2cam_mr_backup="${HELIX_K2CAM_MR_BACKUP:-/usr/share/moonraker_backup}"
    [ -d "$k2cam_dir" ] || [ -d "$k2cam_mr_backup" ]
}

# Locate the moonraker.conf that actually contains the [webcam Default] iframe
# entry. Search order: find_moonraker_conf (if available), then the K2-Camera
# Moonraker location, then the standard MOONRAKER_CONF_PATHS. The base for the
# K2-Camera location and the standard paths is env-overridable for tests via
# HELIX_K2CAM_MR_DIR. Echoes the first existing file whose contents contain a
# "[webcam Default]" line, or empty.
_locate_k2cam_webcam_conf() {
    local mr_dir candidate conf
    mr_dir="${HELIX_K2CAM_MR_DIR:-/usr/share/moonraker}"

    # find_moonraker_conf is defined in moonraker.sh; it may not be sourced in
    # every context, so guard the call.
    if command -v find_moonraker_conf >/dev/null 2>&1; then
        candidate="$(find_moonraker_conf 2>/dev/null)"
        if [ -n "$candidate" ] && [ -f "$candidate" ] && \
            grep -q '^\[webcam Default\]' "$candidate" 2>/dev/null; then
            echo "$candidate"
            return 0
        fi
    fi

    # The K2-Camera-main Moonraker copy keeps its conf here.
    candidate="${mr_dir}/moonraker.conf"
    if [ -f "$candidate" ] && grep -q '^\[webcam Default\]' "$candidate" 2>/dev/null; then
        echo "$candidate"
        return 0
    fi

    # Standard search paths (defined in moonraker.sh).
    for conf in ${MOONRAKER_CONF_PATHS:-}; do
        if [ -f "$conf" ] && grep -q '^\[webcam Default\]' "$conf" 2>/dev/null; then
            echo "$conf"
            return 0
        fi
    done

    echo ""
}

# Restart Moonraker so a moonraker.conf change takes effect. Finds the init
# script via _initd_dir (or /etc/init.d/moonraker) and `restart`s it (guarded,
# errors ignored). If no init script is found, warn that a manual restart is
# needed.
_restart_moonraker() {
    local initd script
    initd="$(_initd_dir)"
    for script in "${initd}/moonraker" "/etc/init.d/moonraker"; do
        if [ -x "$script" ] || [ -f "$script" ]; then
            $SUDO "$script" restart 2>/dev/null || true
            return 0
        fi
    done
    log_warn "Could not find a Moonraker init script — restart Moonraker manually for the camera change to take effect."
    return 0
}

# Reversibly comment out the K2-Camera-main [webcam Default] iframe section in
# whichever moonraker.conf contains it, record the affected path for reversal,
# and restart Moonraker. Comments the block from the "[webcam Default]" header
# through the line before the next "[section]" header / a blank line / EOF, by
# prefixing each line with K2CAM_DISABLE_PREFIX. Idempotent: lines already
# prefixed are skipped, and the marker is not duplicated.
_disable_k2cam_webcam() {
    local conf marker tmp
    conf="$(_locate_k2cam_webcam_conf)"
    if [ -z "$conf" ]; then
        log_warn "K2-Camera-main detected, but no moonraker.conf with a [webcam Default] entry was found — nothing to disable."
        return 0
    fi

    # Comment the [webcam Default] block via awk (robust for the section range).
    tmp="$(mktemp)"
    awk -v pfx="$K2CAM_DISABLE_PREFIX" '
        BEGIN { in_block = 0 }
        # Already-disabled lines: pass through untouched (idempotent).
        index($0, pfx) == 1 { print; next }
        # Header of the block: enter, comment it.
        /^\[webcam Default\]/ { in_block = 1; print pfx $0; next }
        in_block {
            # End of the block: next section header, a blank line, then stop.
            if ($0 ~ /^\[/ || $0 ~ /^[[:space:]]*$/) { in_block = 0; print; next }
            print pfx $0; next
        }
        { print }
    ' "$conf" > "$tmp"

    if [ -s "$tmp" ]; then
        $(file_sudo "$conf") cp "$tmp" "$conf" 2>/dev/null || \
            log_warn "Could not write commented [webcam Default] back to $conf"
    fi
    rm -f "$tmp"

    # Record the affected conf path for reversal (idempotent — no duplicates).
    marker="$(_k2cam_marker_file)"
    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi
    if [ ! -f "$marker" ] || ! grep -qF "$conf" "$marker" 2>/dev/null; then
        echo "$conf" | $(file_sudo "${INSTALL_DIR}/config") tee -a "$marker" >/dev/null
    fi

    log_info "Commented the K2-Camera-main [webcam Default] entry in $conf (reversible)."
    _restart_moonraker
}

# Detect the K2's primary LAN IPv4 address (busybox-compatible).
# Strategy:
#   1. `ip route get 1.1.1.1` and pull the "src <addr>" field (the address the
#      kernel would use to reach the internet — i.e. the primary LAN IP).
#   2. Fall back to the default-route interface's first inet address.
#   3. Fall back to the first non-loopback inet address from `ip addr`.
# Echoes the address, or empty if none found.
detect_lan_ip() {
    local ip_addr=""

    if command -v ip >/dev/null 2>&1; then
        # 1. src field from a route lookup toward a public address.
        ip_addr=$(ip route get 1.1.1.1 2>/dev/null | \
            awk '{ for (i = 1; i <= NF; i++) if ($i == "src") { print $(i+1); exit } }')

        # 2. Default-route interface's first inet address.
        if [ -z "$ip_addr" ]; then
            local def_iface
            def_iface=$(ip route 2>/dev/null | awk '/^default/ { print $5; exit }')
            if [ -n "$def_iface" ]; then
                ip_addr=$(ip -4 addr show "$def_iface" 2>/dev/null | \
                    awk '/inet / { sub(/\/.*/, "", $2); print $2; exit }')
            fi
        fi

        # 3. First non-loopback inet address anywhere.
        if [ -z "$ip_addr" ]; then
            ip_addr=$(ip -4 addr show 2>/dev/null | \
                awk '/inet / && $2 !~ /^127\./ { sub(/\/.*/, "", $2); print $2; exit }')
        fi
    fi

    # Last resort: ifconfig (busybox) for boxes without iproute2.
    if [ -z "$ip_addr" ] && command -v ifconfig >/dev/null 2>&1; then
        ip_addr=$(ifconfig 2>/dev/null | \
            awk '/inet (addr:)?[0-9]/ {
                for (i = 1; i <= NF; i++) {
                    a = $i; sub(/^addr:/, "", a)
                    if (a ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/ && a !~ /^127\./) { print a; exit }
                }
            }')
    fi

    echo "$ip_addr"
}

# Query moonraker's webcam list, return the raw JSON "webcams" array (the value
# of result.webcams) on stdout, or empty on any failure. Uses python3/urllib
# (K2 has no curl/wget). Caller is responsible for parsing.
_moonraker_get_webcams_json() {
    _has_python || return 1
    "$_PY_BIN" - "$MOONRAKER_URL" <<'PYEOF' 2>/dev/null
import json, sys, urllib.request
base = sys.argv[1].rstrip('/')
try:
    with urllib.request.urlopen(base + '/server/webcams/list', timeout=5) as r:
        data = json.load(r)
    cams = data.get('result', {}).get('webcams', [])
    sys.stdout.write(json.dumps(cams))
except Exception:
    sys.exit(1)
PYEOF
}

# Returns 0 if moonraker already has a webcam exposing a usable MJPEG/ustreamer
# stream (service name contains mjpeg/ustreamer AND a non-empty stream_url).
# Used to bail out without stomping a working camera setup.
_moonraker_has_usable_mjpeg() {
    _has_python || return 1
    "$_PY_BIN" - "$MOONRAKER_URL" <<'PYEOF' 2>/dev/null
import json, sys, urllib.request
base = sys.argv[1].rstrip('/')
try:
    with urllib.request.urlopen(base + '/server/webcams/list', timeout=5) as r:
        data = json.load(r)
except Exception:
    sys.exit(2)  # unreachable -> not "has usable" (distinct from found)
cams = data.get('result', {}).get('webcams', [])
for c in cams:
    svc = (c.get('service') or '').lower()
    url = c.get('stream_url') or ''
    if ('mjpeg' in svc or 'ustreamer' in svc) and url.strip():
        sys.exit(0)   # found a usable one
sys.exit(1)           # none found
PYEOF
}

# Returns 0 if moonraker's webcam list contains an entry we installed (its name
# == $HELIX_WEBCAM_NAME, "HelixScreen Camera"). Lets install_camera_k2 tell our
# own camera apart from a third-party one: a usable-but-ours camera must NOT
# short-circuit the function on upgrade (we want to converge it — re-register
# with the corrected service type, refresh the init script), whereas a usable
# third-party camera should be left untouched.
_moonraker_has_our_webcam() {
    _has_python || return 1
    "$_PY_BIN" - "$MOONRAKER_URL" "$HELIX_WEBCAM_NAME" <<'PYEOF' 2>/dev/null
import json, sys, urllib.request
base, name = sys.argv[1].rstrip('/'), sys.argv[2]
try:
    with urllib.request.urlopen(base + '/server/webcams/list', timeout=5) as r:
        data = json.load(r)
except Exception:
    sys.exit(2)  # unreachable -> not "ours" (distinct from found)
cams = data.get('result', {}).get('webcams', [])
for c in cams:
    if c.get('name', '') == name:
        sys.exit(0)   # our webcam is registered
sys.exit(1)           # not ours
PYEOF
}

# Probe whether ustreamer is listening on the given port (busybox-compatible).
# Tries python3 socket connect first; falls back to the /snapshot endpoint.
# Args: $1 = port. Returns 0 if reachable.
_ustreamer_listening() {
    local port="$1"
    if _has_python; then
        "$_PY_BIN" - "$port" <<'PYEOF' 2>/dev/null && return 0
import socket, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
try:
    s.connect(('127.0.0.1', port))
    sys.exit(0)
except Exception:
    sys.exit(1)
finally:
    s.close()
PYEOF
    fi
    return 1
}

# Back up moonraker's current webcam list to the backup file (for restore on
# uninstall) and drop the migration marker. Idempotent: never overwrites an
# existing backup (the first backup is the true pre-HelixScreen state; a second
# install pass must not archive our own migrated list as if it were stock).
# Args: $1 = webcams JSON array
_record_webcam_backup() {
    local cams_json="$1"
    local backup marker
    backup="$(_camera_backup_file)"
    marker="$(_camera_marker_file)"

    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    if [ ! -f "$backup" ]; then
        printf '%s' "$cams_json" | $(file_sudo "${INSTALL_DIR}/config") tee "$backup" >/dev/null
    fi
    # Marker is harmless to (re)touch; presence is what uninstall checks.
    $(file_sudo "${INSTALL_DIR}/config") touch "$marker" 2>/dev/null || \
        : > "$marker" 2>/dev/null || true
}

# Probe the K2's stock nginx webcam proxy (location /webcam/ -> 127.0.0.1:8080,
# fronted on port 4408). When it serves an image, a RELATIVE stream URL
# (/webcam/?action=stream) is preferable to an absolute http://<lan_ip>:8080 one:
# the relative form resolves against whatever origin the client loaded fluidd from,
# so it survives DHCP lease changes and multi-homed eth/wlan interface flips that
# would otherwise strand a baked-in absolute IP (the "camera dead in fluidd after a
# while" failure). Returns 0 only when the proxy returns an image snapshot.
_k2_webcam_proxy_serves() {
    _has_python || return 1
    "$_PY_BIN" - <<'PYEOF' 2>/dev/null
import sys, urllib.request
try:
    with urllib.request.urlopen(
            'http://127.0.0.1:4408/webcam/?action=snapshot', timeout=4) as r:
        ct = (r.headers.get('Content-Type') or '').lower()
        sys.exit(0 if r.status == 200 and ct.startswith('image/') else 1)
except Exception:
    sys.exit(1)
PYEOF
}

# Delete the stock iframe "Default" webcam (if present) and POST our ustreamer
# webcam pointed at the given stream/snapshot URLs. Idempotent on the moonraker
# side: POST /server/webcams/item upserts by name, so re-running just refreshes it.
# Args: $1 = stream_url, $2 = snapshot_url. Returns non-zero only on hard python
# failure.
_moonraker_migrate_webcams() {
    local stream_url="$1" snap_url="$2"
    _has_python || return 1
    "$_PY_BIN" - "$MOONRAKER_URL" "$stream_url" "$snap_url" "$HELIX_WEBCAM_NAME" <<'PYEOF' 2>/dev/null
import json, sys, urllib.request, urllib.parse
base, stream, snap, name = sys.argv[1].rstrip('/'), sys.argv[2], sys.argv[3], sys.argv[4]

def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        r.add_header('Content-Type', 'application/json')
    with urllib.request.urlopen(r, timeout=5) as resp:
        return json.load(resp)

# Drop the stock iframe/webrtc "Default" webcam if it exists (it points at the
# proprietary WebRTC iframe HelixScreen/fluidd can't render). Only DATABASE-
# sourced entries can be removed via the API; a `source: config` webcam (e.g.
# the DnG K2-Camera hack defines one in moonraker.conf) is read-only here — we
# leave it and print CONFIG_DEFAULT_LEFT so the shell can warn the user.
try:
    cams = req('GET', '/server/webcams/list').get('result', {}).get('webcams', [])
    for c in cams:
        nm = c.get('name', '')
        svc = (c.get('service') or '').lower()
        src = (c.get('source') or '').lower()
        if nm == 'Default' and ('iframe' in svc or 'webrtc' in svc):
            if src == 'config':
                sys.stdout.write('CONFIG_DEFAULT_LEFT\n')
            else:
                req('DELETE', '/server/webcams/item?name=' + urllib.parse.quote(nm))
except Exception:
    pass  # non-fatal; the POST below is what matters

try:
    # Service type, and why it is neither of the two obvious alternatives:
    #
    #   NOT 'ustreamer': fluidd/mainsail render MJPEG by service type and ship no
    #   'ustreamer' renderer -- it shows "service not supported!" and never
    #   displays frames.
    #
    #   NOT 'mjpegstreamer-adaptive': that mode fetches ONE HTTP REQUEST PER
    #   FRAME (each with a &cacheBust= param to defeat caching), up to target_fps.
    #   On the K2 that meant ~5 request/response cycles a second through nginx
    #   and ustreamer on a 488 MB SoC, and its access-log volume filled the /tmp
    #   tmpfs -- which is also where Creality's Moonraker fork puts the upload
    #   temp file, so gcode uploads then died mid-stream with ENOSPC and the
    #   client saw "connection reset by peer".
    #
    # Plain 'mjpegstreamer' consumes ustreamer's /stream as a single persistent
    # multipart/x-mixed-replace connection: one request instead of thousands per
    # minute. It renders in fluidd (both renderers ship in 1.30.0) and mainsail,
    # and still matches HelixScreen's own is_mjpeg consumer check (which keys on
    # the 'mjpeg' substring). Frame rate stays governed server-side by
    # ustreamer's own --desired-fps in helixscreen-ustreamer-k2.sh.
    req('POST', '/server/webcams/item', {
        'name': name,
        'service': 'mjpegstreamer',
        'stream_url': stream,
        'snapshot_url': snap,
        'enabled': True,
        'target_fps': 15,
    })
except Exception:
    sys.exit(1)
sys.exit(0)
PYEOF
}

# Install the ustreamer camera setup on K2. No-op on any other platform.
# Idempotent and reversal-aware. See module header.
# Args: $1 = platform
install_camera_k2() {
    local platform="${1:-}"
    [ "$platform" = "k2" ] || return 0

    log_info "Configuring K2 ustreamer camera..."

    # (a0) DETECT the community "K2-Camera-main" mod and reversibly disable its
    # conflicting [webcam Default] iframe entry. Runs BEFORE the usable-MJPEG
    # early-return so it fires even when our camera is already set up. We do NOT
    # undo their Moonraker swap — that's their restore.sh's job.
    if _detect_k2_camera_mod; then
        log_warn "Detected the community 'K2-Camera-main' mod (it replaces Moonraker and"
        log_warn "ships an iframe camera viewer that conflicts with HelixScreen's camera)."
        log_warn "Disabling its conflicting [webcam Default] iframe entry (reversible)."
        log_warn "To fully remove the mod and restore stock Moonraker, run:"
        log_warn "    /root/K2-Camera-main/restore.sh"
        _disable_k2cam_webcam
    fi

    # (a) DETECT: don't stomp an already-working MJPEG/ustreamer camera.
    # Leave a third-party working camera alone, but always converge OUR OWN setup
    # on upgrade (re-register with the corrected service type, refresh the init
    # script). _moonraker_has_our_webcam() keys on the "HelixScreen Camera" name.
    if _moonraker_has_usable_mjpeg && ! _moonraker_has_our_webcam; then
        log_info "Moonraker already exposes a usable MJPEG camera we did not install — leaving it untouched"
        return 0
    fi

    # (b) Verify the bundled ustreamer binary is present + executable.
    local ustreamer_bin="${INSTALL_DIR}/bin/ustreamer"
    if [ ! -x "$ustreamer_bin" ]; then
        log_warn "ustreamer binary missing or not executable at $ustreamer_bin"
        log_warn "Skipping K2 camera setup (release bundle may be incomplete)"
        return 0
    fi

    # (c) Free the device: disable + stop the stock WebRTC pipeline so ustreamer
    # can claim exclusive /dev/video0. chmod a-x mirrors the competing_uis
    # pattern and is recorded as sysv-chmod so uninstall's
    # reenable_disabled_services() chmod +x's them back. cam_app is
    # hotplug-launched (no watchdog) so killing it is clean.
    local initd script
    initd="$(_initd_dir)"
    for script in "${initd}/webrtc" "${initd}/webrtc_local"; do
        if [ -f "$script" ]; then
            log_info "Disabling stock WebRTC init script: $script"
            $SUDO "$script" stop 2>/dev/null || true
            $SUDO chmod a-x "$script" 2>/dev/null || true
            record_disabled_service "sysv-chmod" "$script"
        fi
    done
    # Release the device held by the stock capture apps (ignore errors).
    killall cam_app cam_sub_app 2>/dev/null || true

    # (d) Install + enable the procd ustreamer service (idempotent).
    local svc_dest="${initd}/ustreamer"
    local svc_src="${INSTALL_DIR}/config/helixscreen-ustreamer-k2.sh"
    if [ ! -f "$svc_src" ]; then
        log_warn "ustreamer init source missing: $svc_src — skipping camera service install"
        return 0
    fi

    # Overwrite-if-differs (not skip-if-exists) is how upgrades ship init-script
    # logic fixes — e.g. the newer script reclaims /dev/video0 by killing the
    # stock cam_app grabber before launching ustreamer (the "NO LIVE VIDEO" fix).
    # The script's editable config block is just our standard defaults, so
    # clobbering it on upgrade is acceptable.
    if [ -f "$svc_dest" ] && cmp -s "$svc_src" "$svc_dest"; then
        log_info "ustreamer init script already current at $svc_dest"
    else
        if [ -f "$svc_dest" ]; then
            log_info "Updating ustreamer init script (migrating to current version)..."
        else
            log_info "Installing ustreamer procd init script..."
        fi
        $SUDO cp "$svc_src" "$svc_dest" 2>/dev/null || \
            log_warn "Could not install ustreamer init at $svc_dest"
        $SUDO chmod +x "$svc_dest" 2>/dev/null || true
        $SUDO "$svc_dest" enable 2>/dev/null || \
            log_warn "ustreamer enable failed — camera may not autostart at boot"
    fi

    # Always (re)start; restart is safe if already running.
    if [ -x "$svc_dest" ]; then
        $SUDO "$svc_dest" restart 2>/dev/null || $SUDO "$svc_dest" start 2>/dev/null || true
    fi

    # Verify it's actually listening before we point moonraker at it. The retry
    # count is overridable (HELIX_USTREAMER_PROBE_TRIES) so tests can skip the
    # multi-second wait when no real ustreamer is running.
    local port="8080"
    local tries="${HELIX_USTREAMER_PROBE_TRIES:-5}"
    local i=0 listening=false
    while [ "$i" -lt "$tries" ]; do
        if _ustreamer_listening "$port"; then
            listening=true
            break
        fi
        i=$((i + 1))
        [ "$i" -lt "$tries" ] && sleep 1
    done
    if [ "$listening" = true ]; then
        log_success "ustreamer is serving MJPEG on :$port"
    else
        log_warn "ustreamer does not appear to be listening on :$port (check /dev/video0)"
    fi

    # (d2) Keep our own camera traffic from filling the K2's tmpfs. Must run
    # before the Moonraker section, which returns early when Moonraker is down.
    _tune_webcam_proxy_block || true
    _truncate_oversized_webcam_log

    # (e) Moonraker webcam migration (preserve/fix fluidd). Back up the current
    # list, then delete the stock iframe and add our ustreamer webcam. If
    # moonraker is unreachable, warn but leave ustreamer running (the camera
    # works once moonraker comes up — don't fail the whole install).
    # Guard the assignment: _moonraker_get_webcams_json exits non-zero when
    # Moonraker is unreachable, which would abort under the installer's `set -e`.
    local cams_json=""
    cams_json="$(_moonraker_get_webcams_json)" || cams_json=""
    if [ -z "$cams_json" ]; then
        log_warn "Moonraker unreachable at $MOONRAKER_URL — ustreamer is running, but"
        log_warn "the webcam was not registered. It will work once Moonraker is up;"
        log_warn "re-run the installer or add the webcam manually if needed."
        return 0
    fi

    # Choose the webcam URL form. Prefer a RELATIVE URL served through the K2's
    # stock nginx /webcam/ proxy — it is immune to the DHCP lease changes and
    # eth/wlan interface flips that strand an absolute http://<lan_ip>:8080 URL
    # (fluidd resolves it against its own origin). Only fall back to the absolute
    # LAN IP when that proxy isn't serving (non-stock nginx), which still needs a
    # detected IP. Note: a remote HelixScreen consumer must reach the K2 through
    # the same nginx front (port 4408) for the relative form to resolve.
    local stream_url snap_url reg_desc
    if _k2_webcam_proxy_serves; then
        stream_url="/webcam/?action=stream"
        snap_url="/webcam/?action=snapshot"
        reg_desc="$stream_url (via nginx proxy)"
    else
        local lan_ip
        lan_ip="$(detect_lan_ip)"
        if [ -z "$lan_ip" ]; then
            log_warn "Could not determine the K2's LAN IP and the nginx /webcam/ proxy is not serving — skipping webcam registration"
            log_warn "ustreamer is running on :$port; add the webcam manually if needed."
            return 0
        fi
        stream_url="http://${lan_ip}:${port}/stream"
        snap_url="http://${lan_ip}:${port}/snapshot"
        reg_desc="http://${lan_ip}:${port}/"
    fi

    # Record the pre-migration list ONCE (true stock state) for reversal.
    _record_webcam_backup "$cams_json"

    log_info "Registering ustreamer webcam (${reg_desc}) in Moonraker..."
    local migrate_out
    if migrate_out="$(_moonraker_migrate_webcams "$stream_url" "$snap_url")"; then
        log_success "Moonraker webcam configured for HelixScreen + fluidd"
        case "$migrate_out" in
            *CONFIG_DEFAULT_LEFT*)
                log_warn "A stock 'Default' webcam is defined in your Moonraker *config* (e.g. the K2-Camera hack) — it can't be removed via the API."
                log_warn "  HelixScreen uses the MJPEG stream regardless; fluidd will list both until you remove that config entry."
                ;;
        esac
    else
        log_warn "Failed to register the ustreamer webcam in Moonraker"
        log_warn "ustreamer is running on :$port; add it manually in fluidd if needed."
    fi
}

# Reverse install_camera_k2. No-op on any other platform. Idempotent.
# NOTE: re-enabling the stock webrtc init scripts is handled by the framework's
# reenable_disabled_services() (chmod +x on the recorded sysv-chmod entries);
# we deliberately do NOT duplicate that here. cam_app returns on reboot.
# Args: $1 = platform
uninstall_camera_k2() {
    local platform="${1:-}"
    [ "$platform" = "k2" ] || return 0

    log_info "Removing K2 ustreamer camera..."

    # (a) Stop, disable, and remove the ustreamer service + binary.
    local initd rcd svc_dest
    initd="$(_initd_dir)"
    rcd="$(_rcd_dir)"
    svc_dest="${initd}/ustreamer"
    if [ -f "$svc_dest" ]; then
        $SUDO "$svc_dest" stop 2>/dev/null || true
        $SUDO "$svc_dest" disable 2>/dev/null || true
        $SUDO rm -f "$svc_dest"
        # Belt-and-suspenders: drop any procd rc.d symlinks it created.
        $SUDO rm -f "${rcd}/S95ustreamer" "${rcd}/K05ustreamer" 2>/dev/null || true
    fi
    killall ustreamer 2>/dev/null || true
    $SUDO rm -f "${INSTALL_DIR}/bin/ustreamer" 2>/dev/null || true

    # (a2) Put the nginx /webcam/ block back the way we found it.
    _restore_webcam_proxy_block

    # (b) Restore moonraker webcams from the backup, if we migrated them.
    local marker backup
    marker="$(_camera_marker_file)"
    backup="$(_camera_backup_file)"
    if [ -f "$marker" ]; then
        if [ -f "$backup" ] && _has_python; then
            log_info "Restoring Moonraker webcam list from backup..."
            "$_PY_BIN" - "$MOONRAKER_URL" "$backup" "$HELIX_WEBCAM_NAME" <<'PYEOF' 2>/dev/null || \
                log_warn "Could not fully restore Moonraker webcams (Moonraker may be down)"
import json, sys, urllib.request, urllib.parse
base, backup_path, name = sys.argv[1].rstrip('/'), sys.argv[2], sys.argv[3]

def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        r.add_header('Content-Type', 'application/json')
    with urllib.request.urlopen(r, timeout=5) as resp:
        return json.load(resp)

# Remove our entry first.
try:
    req('DELETE', '/server/webcams/item?name=' + urllib.parse.quote(name))
except Exception:
    pass

# Re-POST every backed-up (stock) webcam entry.
try:
    with open(backup_path) as f:
        cams = json.load(f)
except Exception:
    cams = []

for c in cams:
    if not c.get('name'):
        continue
    body = {
        'name': c.get('name'),
        'service': c.get('service', 'mjpegstreamer'),
        'enabled': c.get('enabled', True),
    }
    for k in ('stream_url', 'snapshot_url', 'target_fps', 'flip_horizontal',
              'flip_vertical', 'rotation', 'aspect_ratio', 'icon', 'location'):
        if c.get(k) is not None:
            body[k] = c[k]
    try:
        req('POST', '/server/webcams/item', body)
    except Exception:
        pass
sys.exit(0)
PYEOF
        else
            log_warn "Webcam backup missing or python unavailable — only removed our entry"
        fi
        $(path_sudo "$backup") rm -f "$backup" 2>/dev/null || true
        $(path_sudo "$marker") rm -f "$marker" 2>/dev/null || true
    fi

    # (c) Re-enable the K2-Camera-main [webcam Default] entry we commented out, if
    # any. For each recorded conf path, strip the K2CAM_DISABLE_PREFIX back off,
    # then restart Moonraker so it takes effect. Finally remove the marker.
    local k2cam_marker conf tmp restored=false
    k2cam_marker="$(_k2cam_marker_file)"
    if [ -f "$k2cam_marker" ]; then
        while IFS= read -r conf; do
            [ -n "$conf" ] || continue
            [ -f "$conf" ] || continue
            log_info "Re-enabling the K2-Camera-main [webcam Default] entry in $conf..."
            tmp="$(mktemp)"
            sed "s/^${K2CAM_DISABLE_PREFIX}//" "$conf" > "$tmp"
            if [ -s "$tmp" ]; then
                $(file_sudo "$conf") cp "$tmp" "$conf" 2>/dev/null || \
                    log_warn "Could not un-comment [webcam Default] in $conf"
                restored=true
            fi
            rm -f "$tmp"
        done < "$k2cam_marker"
        [ "$restored" = true ] && _restart_moonraker
        $(path_sudo "$k2cam_marker") rm -f "$k2cam_marker" 2>/dev/null || true
    fi

    log_success "K2 ustreamer camera removed (stock WebRTC re-enabled on reboot)"
}
