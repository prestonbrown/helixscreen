#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: remote_screen
# nginx route for the app's built-in screen server (prestonbrown/helixscreen#1825).
#
# Where nginx fronts Moonraker, the screen server binds 127.0.0.1 and is reached
# through /helixscreen/screen/ on the web UI's own port, gated by an
# auth_request to Moonraker's /access/user: whoever Moonraker would let send
# G-code may view and control, nobody else reaches the port. The app picks that
# mode when the route file exists; without it the app binds the LAN and pairs.
# So the route file is written only once nginx has accepted the block.
#
# Uses camera.sh's nginx edit helpers (_nginx_conf_path, _nginx_bin,
# _nginx_edit_validated, _nginx_strip_tagged_lines, _nginx_validate_and_reload).
#
# Reads: INSTALL_DIR, SUDO, MOONRAKER_URL, HELIX_NGINX_CONF,
#        HELIX_NGINX_SITES_DIR, _PY_BIN (via _has_python from common.sh)
# Writes: the nginx configs that proxy Moonraker,
#         ${INSTALL_DIR}/config/.remote_screen_route,
#         ${INSTALL_DIR}/config/.nginx_remote_screen (marker)

# Source guard
[ -n "${_HELIX_REMOTE_SCREEN_SOURCED:-}" ] && return 0
_HELIX_REMOTE_SCREEN_SOURCED=1

# Carried by every line we insert, so removal is an exact `grep -v`.
NGINX_REMOTE_SCREEN_TAG="helix-managed-remote-screen"

# The mount the app reads from the route file; it registers its webcam URLs
# under it.
REMOTE_SCREEN_MOUNT="/helixscreen/screen/"

# Moonraker webcam the app registers, URL-encoded for /server/webcams/item.
REMOTE_SCREEN_WEBCAM_QUERY="name=HelixScreen%20Screen"

_remote_screen_route_file()  { _ledger_path .remote_screen_route; }
_remote_screen_marker_file() { _ledger_path .nginx_remote_screen; }

# sites-enabled directory (MainsailOS/KIAUH put the mainsail/fluidd server
# blocks there). Resolved per-call so the BATS suite can redirect it.
_nginx_sites_dir() { echo "${HELIX_NGINX_SITES_DIR:-/etc/nginx/sites-enabled}"; }

# Every nginx config file the route goes into, one per line: the main config,
# then each site. Install and removal visit the same set.
_remote_screen_conf_files() {
    local f sites
    f="$(_nginx_conf_path)"
    [ -f "$f" ] && echo "$f"
    sites="$(_nginx_sites_dir)"
    [ -d "$sites" ] || return 0
    for f in "$sites"/*; do
        [ -f "$f" ] && echo "$f"
    done
    return 0
}

# True when the firmware serves the screen itself. Its platform hook then
# defines start_remote_screen (the Snapmaker U1 starts fb-http from it), and a
# second server would compete with it for the same display.
_remote_screen_firmware_serves() {
    grep -q '^start_remote_screen()' "${INSTALL_DIR}/platform/hooks.sh" 2>/dev/null
}

# Print config $1 with the route added before the closing brace of every
# server block that proxies Moonraker: one with a `location /websocket`, or a
# proxy_pass to port 7125 or to an upstream in the same file that points there.
# The auth subrequest goes where that block already sends Moonraker traffic
# (its /websocket proxy_pass first), else 127.0.0.1:7125. A block that already
# carries the tag gets only the mount's trailing-slash redirect, if it lacks it.
# With $1 "upgrade" (then the config is $2), untagged blocks are left alone.
#
# The redirect is relative ("screen/"): nginx's own redirect for the mount
# without its slash names this server's scheme, host and root path, which sends
# a browser behind a TLS or path-prefixing reverse proxy somewhere else.
_remote_screen_filter() {
    local only_tagged=0
    if [ "$1" = "upgrade" ]; then
        only_tagged=1
        shift
    fi
    awk -v tag="$NGINX_REMOTE_SCREEN_TAG" -v mount="$REMOTE_SCREEN_MOUNT" -v only_tagged="$only_tagged" '
        BEGIN {
            bare = mount; sub(/\/$/, "", bare)
            leaf = bare; sub(/.*\//, "", leaf)
        }
        function code(s) { sub(/#.*/, "", s); return s }
        function count(s, re,   t) { t = s; return gsub(re, "", t) }
        function pp_host(s,   t) {
            if (!match(s, /proxy_pass[[:space:]]+[^;[:space:]]+/)) return ""
            t = substr(s, RSTART, RLENGTH)
            sub(/^proxy_pass[[:space:]]+/, "", t)
            sub(/^[A-Za-z]+:\/\//, "", t)
            sub(/[\/;$].*$/, "", t)
            if (t ~ /^unix:/) return ""
            return t
        }
        function is_moonraker(h) { return h ~ /:7125$/ || (h in mr_up) }
        FNR == NR {
            l = code($0)
            if (!in_srv && l ~ /^[[:space:]]*upstream[[:space:]]+[^[:space:]{]+[[:space:]]*\{/) {
                up = l; sub(/^[[:space:]]*upstream[[:space:]]+/, "", up); sub(/[[:space:]{].*$/, "", up)
                in_up = 1; up_depth = depth
            }
            if (in_up && l ~ /^[[:space:]]*server[[:space:]]/ && l ~ /:7125/) mr_up[up] = 1
            if (!in_srv && !in_up && l ~ /^[[:space:]]*server[[:space:]]*\{/) {
                n++; in_srv = 1; srv_depth = depth
                indent[n] = $0; match(indent[n], /^[[:space:]]*/); indent[n] = substr(indent[n], 1, RLENGTH) "    "
                want_ws = 0
            }
            if (in_srv) {
                if (index($0, tag)) tagged[n] = 1
                if (index($0, "location = " bare " {")) redirected[n] = 1
                if (l ~ /^[[:space:]]*location[[:space:]]+(=[[:space:]]*)?\/websocket/) { ws[n] = 1; want_ws = 1 }
                h = pp_host(l)
                if (h != "") {
                    if (want_ws && ws_host[n] == "") ws_host[n] = h
                    hosts[n] = hosts[n] " " h
                }
            }
            depth += count(l, "[{]") - count(l, "[}]")
            if (in_up && depth <= up_depth) in_up = 0
            if (in_srv && depth <= srv_depth) { in_srv = 0; close_line[n] = FNR }
            next
        }
        FNR == 1 {
            for (i = 1; i <= n; i++) {
                if (!close_line[i]) continue
                if (tagged[i]) {
                    if (!redirected[i]) { ins_at[close_line[i]] = i; redirect_only[i] = 1 }
                    continue
                }
                if (only_tagged) continue
                target = ""
                if (ws_host[i] != "") target = ws_host[i]
                k = split(hosts[i], hl, " ")
                for (j = 1; j <= k && target == ""; j++) if (is_moonraker(hl[j])) target = hl[j]
                if (!ws[i]) {
                    found = 0
                    for (j = 1; j <= k; j++) if (is_moonraker(hl[j])) found = 1
                    if (!found) continue
                }
                if (target == "") target = "127.0.0.1:7125"
                ins_at[close_line[i]] = i
                ins_up[i] = target
            }
        }
        FNR in ins_at {
            i = ins_at[FNR]; p = indent[i]; q = p "    "; c = " # " tag
            if (!redirect_only[i]) {
                print p "location " mount " {" c
                print q "auth_request /helixscreen/screen-auth;" c
                print q "proxy_pass http://127.0.0.1:7130/;" c
                print q "proxy_set_header Host $http_host;" c
                print q "proxy_buffering off;" c
                print q "access_log off;" c
                print p "}" c
                print p "location = /helixscreen/screen-auth {" c
                print q "internal;" c
                print q "proxy_pass http://" ins_up[i] "/access/user;" c
                print q "proxy_pass_request_body off;" c
                print q "proxy_set_header Content-Length \"\";" c
                print q "proxy_set_header X-Real-IP $remote_addr;" c
                print p "}" c
            }
            # Last, so an upgrade that adds only this line matches a fresh install.
            print p "location = " bare " { return 301 " leaf "/; }" c
        }
        { print }
    ' "$1" "$1"
}

# Add the route to every Moonraker-proxying server block, then record it for
# the app (route file) and for uninstall (marker). Every skip returns 0: the
# app falls back to LAN mode, so a missing route never fails an install.
install_remote_screen_route() {
    local marker bin conf confs edited rc routed status
    marker="$(_remote_screen_marker_file)"
    if [ -f "$marker" ]; then
        log_info "Remote screen: nginx route already installed"
        _remote_screen_upgrade_route
        return 0
    fi
    if _remote_screen_firmware_serves; then
        log_info "Remote screen: the firmware serves the screen itself; no nginx route"
        return 0
    fi
    bin="$(_nginx_bin)" || bin=""
    if [ -z "$bin" ]; then
        log_info "Remote screen: no nginx here; the screen server will use the LAN"
        return 0
    fi
    if ! "$bin" -V 2>&1 | grep -q http_auth_request_module; then
        log_info "Remote screen: nginx lacks auth_request; the screen server will use the LAN"
        return 0
    fi

    # auth_request turns anything but 2xx/401/403 into a 500, and the route
    # file moves the app off the LAN: without a Moonraker that answers
    # /access/user (it has no [authorization] section, or is unreachable) the
    # route would leave no way in at all.
    status="$(_remote_screen_moonraker_status GET /access/user)"
    case "$status" in
        200|401) ;;
        *)
            log_info "Remote screen: Moonraker /access/user answered ${status}; the screen server will use the LAN"
            return 0
            ;;
    esac

    confs="$(_remote_screen_conf_files)"
    edited=""
    while IFS= read -r conf; do
        [ -n "$conf" ] || continue
        rc=0
        _nginx_edit_validated "$conf" _remote_screen_filter </dev/null || rc=$?
        case "$rc" in
            0) edited="${edited}${conf}
" ;;
            1)
                # The file nginx refused is already back; undo the ones it
                # accepted so the configs stay all-or-nothing.
                printf '%s' "$edited" | while IFS= read -r conf; do
                    [ -n "$conf" ] && _nginx_strip_tagged_lines "$conf" "$NGINX_REMOTE_SCREEN_TAG"
                done
                [ -n "$edited" ] && { _nginx_validate_and_reload || true; }
                log_warn "nginx rejected the remote screen route; reverted, the screen server will use the LAN"
                return 0
                ;;
        esac
    done <<EOF
$confs
EOF

    routed=0
    while IFS= read -r conf; do
        [ -n "$conf" ] && grep -q "$NGINX_REMOTE_SCREEN_TAG" "$conf" 2>/dev/null && routed=1
    done <<EOF
$confs
EOF
    if [ "$routed" -eq 0 ]; then
        log_info "Remote screen: no nginx server block proxies Moonraker; the screen server will use the LAN"
        return 0
    fi

    printf '%s\n' "$REMOTE_SCREEN_MOUNT" | $SUDO tee "$(_remote_screen_route_file)" >/dev/null
    $SUDO touch "$marker"
    log_info "Remote screen: added nginx route ${REMOTE_SCREEN_MOUNT} (auth via Moonraker)"
    return 0
}

# Bring a route an older installer added up to date (the relative trailing-slash
# redirect). Best effort: a file nginx refuses is put back as it was, and the
# route keeps working without the redirect.
_remote_screen_upgrade_route() {
    local conf
    while IFS= read -r conf; do
        [ -n "$conf" ] || continue
        grep -q "$NGINX_REMOTE_SCREEN_TAG" "$conf" 2>/dev/null || continue
        _nginx_edit_validated "$conf" _remote_screen_filter upgrade </dev/null || true
    done <<EOF
$(_remote_screen_conf_files)
EOF
    return 0
}

# Send $1 (an HTTP method) to Moonraker path $2 and print the response status,
# or 000 when nothing answered. curl where it is real, else python3 (the K2 has
# no curl; the K1's impostor curl prints no status and falls through too).
_remote_screen_moonraker_status() {
    local url code=""
    url="$(installer_moonraker_url)$2"
    if command -v curl >/dev/null 2>&1; then
        code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X "$1" "$url" 2>/dev/null)"
    fi
    case "$code" in
        [1-5][0-9][0-9]) echo "$code"; return 0 ;;
    esac
    if _has_python; then
        code="$("$_PY_BIN" - "$1" "$url" <<'PYEOF' 2>/dev/null
import sys, urllib.request, urllib.error
try:
    with urllib.request.urlopen(urllib.request.Request(sys.argv[2], method=sys.argv[1]), timeout=5) as r:
        print(r.status)
except urllib.error.HTTPError as e:
    print(e.code)
except Exception:
    print('000')
PYEOF
)"
    fi
    echo "${code:-000}"
}

# Best-effort DELETE of the app's webcam entry, in case the app never ran to
# remove it.
_remote_screen_delete_webcam() {
    _remote_screen_moonraker_status DELETE "/server/webcams/item?${REMOTE_SCREEN_WEBCAM_QUERY}" >/dev/null
    return 0
}

# Reverse install_remote_screen_route. Strips the tagged lines only when the
# marker says we added them; the route file and the webcam go regardless, since
# the app registers the webcam in LAN mode too.
remove_remote_screen_route() {
    local marker conf stripped
    marker="$(_remote_screen_marker_file)"
    if [ -f "$marker" ]; then
        stripped=0
        while IFS= read -r conf; do
            [ -n "$conf" ] || continue
            _nginx_strip_tagged_lines "$conf" "$NGINX_REMOTE_SCREEN_TAG" && stripped=1
        done <<EOF
$(_remote_screen_conf_files)
EOF
        if [ "$stripped" -eq 1 ]; then
            _nginx_validate_and_reload ||
                log_warn "nginx rejected the config after removing the remote screen route"
            log_info "Removed the nginx remote screen route"
        fi
    fi
    remove_state_file "$(_remote_screen_route_file)"
    remove_state_file "$marker"
    _remote_screen_delete_webcam
    return 0
}
