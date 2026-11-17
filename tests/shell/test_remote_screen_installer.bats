#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the remote screen nginx route (remote_screen.sh,
# prestonbrown/helixscreen#1825): the tagged /helixscreen/screen/ block goes
# into every server block that proxies Moonraker, the route file the app reads
# is written only once nginx accepts it, and uninstall takes back exactly what
# was added.
#
# Nothing touches the host: the nginx configs live in a temp dir
# (HELIX_NGINX_CONF, HELIX_NGINX_SITES_DIR), nginx and curl are shell stubs, and
# Moonraker is a port nobody listens on.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export ROUTE_FILE="$INSTALL_DIR/config/.remote_screen_route"
    export ROUTE_MARKER="$INSTALL_DIR/config/.nginx_remote_screen"

    export HELIX_INITD_DIR="$BATS_TEST_TMPDIR/etc/init.d"
    export HELIX_RCD_DIR="$BATS_TEST_TMPDIR/etc/rc.d"
    mkdir -p "$HELIX_INITD_DIR" "$HELIX_RCD_DIR"
    export HELIX_NGINX_CONF="$BATS_TEST_TMPDIR/etc/nginx/nginx.conf"
    export HELIX_NGINX_SITES_DIR="$BATS_TEST_TMPDIR/etc/nginx/sites-enabled"
    mkdir -p "$(dirname "$HELIX_NGINX_CONF")" "$HELIX_NGINX_SITES_DIR"
    export TMPDIR="$BATS_TEST_TMPDIR"

    SUDO=""
    export SUDO
    export MOONRAKER_URL="http://127.0.0.1:1"

    unset _HELIX_COMMON_SOURCED _HELIX_CAMERA_SOURCED _HELIX_REMOTE_SCREEN_SOURCED \
        _HELIX_HOST_PROFILE_SOURCED _HELIX_PRINT_CONF_SOURCED _HELIX_MOONRAKER_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/helix_print_conf.sh"  # installer_moonraker_url's
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"         # installer_moonraker_url
    . "$WORKTREE_ROOT/scripts/lib/installer/camera.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/remote_screen.sh"

    # Moonraker REST goes to a recording stub, never the network. A request
    # asking for the status code (-w) gets MOONRAKER_STATUS back.
    export CURL_LOG="$BATS_TEST_TMPDIR/curl.log"
    export MOONRAKER_STATUS=200
    curl() {
        echo "$*" >> "$CURL_LOG"
        case " $* " in *" -w "*) printf '%s' "$MOONRAKER_STATUS" ;; esac
        return 0
    }
    export -f curl
}

# A K1/K2-shaped nginx.conf: Fluidd on 4408 and Mainsail on 4409, both proxying
# Moonraker through `upstream apiserver`.
write_two_server_conf() {
    cat > "$HELIX_NGINX_CONF" <<'EOF'
http {
    upstream apiserver {
        ip_hash;
        server 127.0.0.1:7125;
    }

    server {
        listen 4408 default_server;
        root /usr/data/fluidd;

        location / {
            try_files $uri $uri/ /index.html;
        }

        location /websocket {
            proxy_pass http://apiserver/websocket;
            proxy_http_version 1.1;
        }

        location ~ ^/(printer|api|access|machine|server)/ {
            proxy_pass http://apiserver$request_uri;
        }
    }

    server {
        listen 4409 default_server;
        root /usr/data/mainsail;

        location /websocket {
            proxy_pass http://apiserver/websocket;
        }
    }
}
EOF
}

# nginx stub. $1: "auth" (built with auth_request) or "noauth"; $2: "ok" or
# "fail" for `nginx -t`.
stub_nginx() {
    export NGINX_BUILD="${1:-auth}" NGINX_VERDICT="${2:-ok}"
    export NGINX_T_LOG="$BATS_TEST_TMPDIR/nginx-t.log"
    nginx() {
        case "$1" in
            -V)
                echo "nginx version: nginx/1.22.1" >&2
                if [ "$NGINX_BUILD" = "auth" ]; then
                    echo "configure arguments: --with-http_ssl_module --with-http_auth_request_module" >&2
                else
                    echo "configure arguments: --with-http_ssl_module" >&2
                fi
                ;;
            -t)
                echo "nginx -t" >> "$NGINX_T_LOG"
                [ "$NGINX_VERDICT" = "ok" ] || { echo "nginx: configuration file test failed" >&2; return 1; }
                ;;
        esac
        return 0
    }
    export -f nginx

    cat > "$HELIX_INITD_DIR/nginx" <<EOF
#!/bin/sh
echo "\$1" >> "$BATS_TEST_TMPDIR/nginx.calls"
exit 0
EOF
    chmod +x "$HELIX_INITD_DIR/nginx"
}

# Lines of the server block listening on port $1, from config $2.
server_block() {
    awk -v port="$1" '
        /^[[:space:]]*server[[:space:]]*\{/ { buf = ""; inb = 1 }
        inb { buf = buf $0 "\n" }
        inb && /^    \}/ { if (buf ~ ("listen " port)) printf "%s", buf; inb = 0 }
    ' "$2"
}

@test "inserts the block once per Moonraker server block" {
    write_two_server_conf
    stub_nginx auth ok

    run install_remote_screen_route
    [ "$status" -eq 0 ]

    local port blk
    for port in 4408 4409; do
        blk="$(server_block "$port" "$HELIX_NGINX_CONF")"
        [ "$(printf '%s\n' "$blk" | grep -c 'location /helixscreen/screen/ {')" -eq 1 ]
        [ "$(printf '%s\n' "$blk" | grep -c 'location = /helixscreen/screen-auth {')" -eq 1 ]
        contains "auth_request /helixscreen/screen-auth;" "$blk"
        contains "proxy_pass http://127.0.0.1:7130/;" "$blk"
        contains 'proxy_set_header Host $http_host;' "$blk"
        contains "proxy_pass http://apiserver/access/user;" "$blk"
        # The mount without its slash redirects relative to the request, so a
        # path-prefixing or TLS reverse proxy in front keeps its own URL.
        [ "$(printf '%s\n' "$blk" | grep -c 'location = /helixscreen/screen { return 301 screen/; }')" -eq 1 ]
    done
    # Every inserted line carries the tag, so removal can be a plain line delete.
    [ "$(grep -c 'helixscreen/screen' "$HELIX_NGINX_CONF")" -eq 8 ]
    refute_sh "grep -n 'helixscreen' \"$HELIX_NGINX_CONF\" | grep -v 'helix-managed-remote-screen'"
    # The upstream block is not a server block and gets nothing.
    [ "$(grep -c 'helix-managed-remote-screen' "$HELIX_NGINX_CONF")" -eq 30 ]

    [ "$(cat "$ROUTE_FILE")" = "/helixscreen/screen/" ]
    [ -f "$ROUTE_MARKER" ]
    grep -q "nginx -t" "$NGINX_T_LOG"
    grep -q "reload" "$BATS_TEST_TMPDIR/nginx.calls"
}

@test "second run changes nothing" {
    write_two_server_conf
    stub_nginx auth ok

    install_remote_screen_route
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/after-first"

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/after-first"
    contains "already installed" "$output"
}

@test "second run changes nothing even without the marker" {
    write_two_server_conf
    stub_nginx auth ok

    install_remote_screen_route
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/after-first"
    rm -f "$ROUTE_MARKER" "$ROUTE_FILE"

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/after-first"
    [ -f "$ROUTE_MARKER" ]
    [ "$(cat "$ROUTE_FILE")" = "/helixscreen/screen/" ]
}

@test "a route from an older installer gains the relative redirect on the next run" {
    write_two_server_conf
    stub_nginx auth ok
    install_remote_screen_route
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/current"
    # The route as an installer without the redirect wrote it.
    grep -v 'location = /helixscreen/screen {' "$BATS_TEST_TMPDIR/current" > "$HELIX_NGINX_CONF"
    ! grep -q 'return 301' "$HELIX_NGINX_CONF"
    [ -f "$ROUTE_MARKER" ]

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    [ "$(grep -c 'location = /helixscreen/screen { return 301 screen/; }' "$HELIX_NGINX_CONF")" -eq 2 ]
    # The same config a fresh install writes.
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/current"
    [ "$(cat "$ROUTE_FILE")" = "/helixscreen/screen/" ]
}

@test "the upgrade adds nothing to configs and blocks without the route" {
    write_two_server_conf
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/plain"
    # Untagged blocks are left alone in upgrade mode.
    _remote_screen_filter upgrade "$BATS_TEST_TMPDIR/plain" > "$BATS_TEST_TMPDIR/filtered"
    cmp "$BATS_TEST_TMPDIR/filtered" "$BATS_TEST_TMPDIR/plain"

    stub_nginx auth ok
    install_remote_screen_route
    # A Moonraker site added after the route went in.
    cat > "$HELIX_NGINX_SITES_DIR/mainsail" <<'EOF'
server {
    listen 80 default_server;

    location /websocket {
        proxy_pass http://apiserver/websocket;
    }
}
EOF
    cp "$HELIX_NGINX_SITES_DIR/mainsail" "$BATS_TEST_TMPDIR/site"

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_SITES_DIR/mainsail" "$BATS_TEST_TMPDIR/site"
}

@test "failing nginx -t restores the original byte for byte and writes no route marker" {
    write_two_server_conf
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    stub_nginx auth fail

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    [ ! -e "$ROUTE_FILE" ]
    [ ! -e "$ROUTE_MARKER" ]
    [ ! -f "$BATS_TEST_TMPDIR/nginx.calls" ]
    # No backup or scratch copy is left behind.
    [ -z "$(find "$BATS_TEST_TMPDIR" -maxdepth 1 -name 'helix-nginx-*')" ]
}

@test "failing nginx -t on a later file reverts the files already edited" {
    write_two_server_conf
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    cat > "$HELIX_NGINX_SITES_DIR/mainsail" <<'EOF'
server {
    listen 80;
    location /websocket {
        proxy_pass http://127.0.0.1:7125/websocket;
    }
}
EOF
    cp "$HELIX_NGINX_SITES_DIR/mainsail" "$BATS_TEST_TMPDIR/orig-site"
    stub_nginx auth ok
    # Accept the first `nginx -t`, refuse the second.
    nginx_t_count=0
    nginx() {
        case "$1" in
            -V) echo "--with-http_auth_request_module" >&2 ;;
            -t) nginx_t_count=$((nginx_t_count + 1)); [ "$nginx_t_count" -lt 2 ] ;;
        esac
    }

    install_remote_screen_route
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    cmp "$HELIX_NGINX_SITES_DIR/mainsail" "$BATS_TEST_TMPDIR/orig-site"
    [ ! -e "$ROUTE_FILE" ]
    [ ! -e "$ROUTE_MARKER" ]
}

@test "nginx without auth_request is left untouched" {
    write_two_server_conf
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    stub_nginx noauth ok

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    [ ! -e "$ROUTE_FILE" ]
    [ ! -e "$ROUTE_MARKER" ]
}

@test "config with no Moonraker proxy is left untouched" {
    cat > "$HELIX_NGINX_CONF" <<'EOF'
http {
    upstream other {
        server 127.0.0.1:8080;
    }
    server {
        listen 80;
        location / { root /srv; }
        location /app/ {
            proxy_pass http://other/;
        }
    }
}
EOF
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    stub_nginx auth ok

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    [ ! -e "$ROUTE_FILE" ]
    [ ! -e "$ROUTE_MARKER" ]
}

@test "a proxy_pass to port 7125 counts as a Moonraker block" {
    cat > "$HELIX_NGINX_CONF" <<'EOF'
http {
    server {
        listen 80;
        location /server/ {
            proxy_pass http://localhost:7125/server/;
        }
    }
}
EOF
    stub_nginx auth ok

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    grep -q "proxy_pass http://localhost:7125/access/user;" "$HELIX_NGINX_CONF"
    [ -f "$ROUTE_MARKER" ]
}

@test "moonraker without /access/user is left untouched" {
    write_two_server_conf
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    stub_nginx auth ok
    export MOONRAKER_STATUS=404

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    [ ! -e "$ROUTE_FILE" ]
    [ ! -e "$ROUTE_MARKER" ]
    contains "404" "$output"
    grep -q "http://127.0.0.1:1/access/user" "$CURL_LOG"
}

@test "moonraker that requires a login still gets the route" {
    write_two_server_conf
    stub_nginx auth ok
    export MOONRAKER_STATUS=401

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    grep -q "location /helixscreen/screen/ {" "$HELIX_NGINX_CONF"
    [ "$(cat "$ROUTE_FILE")" = "/helixscreen/screen/" ]
}

@test "uninstall strips only tagged lines" {
    write_two_server_conf
    # A user line mentioning "screen" inside a Moonraker server block.
    awk '{ print } /root \/usr\/data\/mainsail;/ { print "        location /screen/ { proxy_pass http://127.0.0.1:8092/; }" }' \
        "$HELIX_NGINX_CONF" > "$BATS_TEST_TMPDIR/with-user-line"
    mv "$BATS_TEST_TMPDIR/with-user-line" "$HELIX_NGINX_CONF"
    grep -q 'location /screen/' "$HELIX_NGINX_CONF"
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    stub_nginx auth ok

    install_remote_screen_route
    [ -f "$ROUTE_MARKER" ]
    ! cmp -s "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"

    run remove_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    grep -q 'location /screen/ { proxy_pass http://127.0.0.1:8092/; }' "$HELIX_NGINX_CONF"
    [ ! -e "$ROUTE_FILE" ]
    [ ! -e "$ROUTE_MARKER" ]
    grep -q -- "-X DELETE http://127.0.0.1:1/server/webcams/item?name=HelixScreen%20Screen" "$CURL_LOG"
}

@test "uninstall without the marker leaves nginx alone but still drops the webcam" {
    write_two_server_conf
    stub_nginx auth ok
    install_remote_screen_route
    rm -f "$ROUTE_MARKER"
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/routed"

    run remove_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/routed"
    [ ! -e "$ROUTE_FILE" ]
    grep -q "DELETE" "$CURL_LOG"
}

@test "sites-enabled server blocks are edited too" {
    cat > "$HELIX_NGINX_CONF" <<'EOF'
http {
    include /etc/nginx/sites-enabled/*;
}
EOF
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    # MainsailOS keeps the site in sites-available and links it into sites-enabled.
    mkdir -p "$BATS_TEST_TMPDIR/etc/nginx/sites-available"
    cat > "$BATS_TEST_TMPDIR/etc/nginx/sites-available/mainsail" <<'EOF'
server {
    listen 80 default_server;

    location /websocket {
        proxy_pass http://apiserver/websocket;
    }
}
EOF
    cp "$BATS_TEST_TMPDIR/etc/nginx/sites-available/mainsail" "$BATS_TEST_TMPDIR/orig-site"
    ln -s "$BATS_TEST_TMPDIR/etc/nginx/sites-available/mainsail" "$HELIX_NGINX_SITES_DIR/mainsail"
    stub_nginx auth ok

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    [ -L "$HELIX_NGINX_SITES_DIR/mainsail" ]
    grep -q "location /helixscreen/screen/ {" "$BATS_TEST_TMPDIR/etc/nginx/sites-available/mainsail"
    grep -q "proxy_pass http://apiserver/access/user;" "$HELIX_NGINX_SITES_DIR/mainsail"
    [ "$(cat "$ROUTE_FILE")" = "/helixscreen/screen/" ]
    # Only the site itself is in sites-enabled: no backup for nginx to load.
    [ "$(ls "$HELIX_NGINX_SITES_DIR")" = "mainsail" ]

    run remove_remote_screen_route
    [ "$status" -eq 0 ]
    [ -L "$HELIX_NGINX_SITES_DIR/mainsail" ]
    cmp "$BATS_TEST_TMPDIR/etc/nginx/sites-available/mainsail" "$BATS_TEST_TMPDIR/orig-site"
}

@test "a platform whose firmware serves the screen is skipped" {
    write_two_server_conf
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    mkdir -p "$INSTALL_DIR/platform"
    cp "$WORKTREE_ROOT/assets/config/platform/hooks-snapmaker-u1.sh" "$INSTALL_DIR/platform/hooks.sh"
    stub_nginx auth ok

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    [ ! -e "$ROUTE_FILE" ]
    [ ! -e "$ROUTE_MARKER" ]
}

@test "no nginx installed is a clean skip" {
    write_two_server_conf
    cp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    _nginx_bin() { return 1; }

    run install_remote_screen_route
    [ "$status" -eq 0 ]
    cmp "$HELIX_NGINX_CONF" "$BATS_TEST_TMPDIR/orig"
    [ ! -e "$ROUTE_FILE" ]
}
