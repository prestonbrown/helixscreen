#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decision logic of scripts/ci/homeserver_image.sh, with docker replaced by a
# fake that records its calls. The dry run's web server is real, and the fake
# docker downloads from it the way the Dockerfile does. The real build, the
# container and the registry run in CI.

SCRIPT="scripts/ci/homeserver_image.sh"
NAME=helixscreen-smoke

# A container's /proc/net/tcp: a loopback listener on 5432, the screen server
# listening on every address at 7130, and a client connected to it.
TCP_LOOPBACK_ONLY='  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:1538 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 35057 1 0000000000000000 100 0 0 10 0
   1: 0100007F:1BDA 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 35060 1 0000000000000000 100 0 0 10 0'
TCP_LAN='  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:1538 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 35057 1 0000000000000000 100 0 0 10 0
   1: 00000000:1BDA 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 35060 1 0000000000000000 100 0 0 10 0
   2: 00000000:1BDA 0100007F:CB30 01 00000000:00000000 00:00000000 00000000   100        0 35100 1 0000000000000000 20 4 30 10 -1'
# The server's port with a client connection and a closed socket on it, and
# nothing listening.
TCP_NO_SERVER='  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:1538 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 35057 1 0000000000000000 100 0 0 10 0
   2: 00000000:1BDA 0100007F:CB30 01 00000000:00000000 00:00000000 00000000   100        0 35100 1 0000000000000000 20 4 30 10 -1
   3: 00000000:1BDA 00000000:0000 08 00000000:00000000 00:00000000 00000000   100        0 35101 1 0000000000000000 100 0 0 10 0'

# What docker/Dockerfile.homeserver seeds, and the same file with remote
# control left off.
SETTINGS_ON='{"active_printer_id":"p1","printers":{"p1":{"remote_screen":{"enabled":true,"control":true,"webcam":false}}}}'
SETTINGS_OFF='{"active_printer_id":"p1","printers":{"p1":{"remote_screen":{"enabled":true,"control":false,"webcam":false}}}}'

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    export HOMESERVER_SMOKE_TIMEOUT=3
    # The smoke's minute of waiting runs at once.
    mock_fake_clock
    CALLS="$BATS_TEST_TMPDIR/docker.calls"
    export DOCKER_CALLS="$CALLS"
    export SERVER_PID_FILE="$BATS_TEST_TMPDIR/server.pid"
    : > "$CALLS"
    : > "$SERVER_PID_FILE"
    export SMOKE_TCP="$BATS_TEST_TMPDIR/tcp"
    export SMOKE_SETTINGS="$BATS_TEST_TMPDIR/settings.json"
    printf '%s\n' "$TCP_LAN" > "$SMOKE_TCP"
    printf '%s\n' "$SETTINGS_ON" > "$SMOKE_SETTINGS"
}

# Assert that $2 has a line equal to $1.
has_line() {
    printf '%s\n' "$2" | grep -Fxq -- "$1" ||
        { printf 'expected a line:\n%s\nin:\n%s\n' "$1" "$2" >&2; return 1; }
}

fake_docker() {
    cat > "$BATS_TEST_TMPDIR/docker" << 'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$DOCKER_CALLS"
case "$1" in
    run) echo 0123456789ab ;;
    exec)
        case "$3" in
            curl) echo '{"ok":true}'; exit "${SMOKE_UP_RC:-0}" ;;
            cat)
                case "$4" in
                    /proc/net/tcp) cat "$SMOKE_TCP" ;;
                    /config/settings.json) cat "$SMOKE_SETTINGS" ;;
                esac
                ;;
        esac
        ;;
    logs) echo "fake container log" ;;
esac
EOF
    chmod +x "$BATS_TEST_TMPDIR/docker"
    export PATH="$BATS_TEST_TMPDIR:$PATH"
}

# --------------------------------------------------------------- tags

@test "tags: a stable release is pushed as its version, stable and latest" {
    run "$SCRIPT" tags 1.1.0 stable "stable"
    [ "$status" -eq 0 ]
    [ "$output" = "1.1.0
stable
latest" ]
}

@test "tags: a beta release is pushed as its version, beta and dev, never latest" {
    run "$SCRIPT" tags 1.2.0-beta.2 beta "beta dev"
    [ "$status" -eq 0 ]
    [ "$output" = "1.2.0-beta.2
beta
dev" ]
}

@test "tags: a dev release is pushed as its version and dev" {
    run "$SCRIPT" tags 1.2.0-alpha.5 dev "dev"
    [ "$status" -eq 0 ]
    [ "$output" = "1.2.0-alpha.5
dev" ]
}

@test "tags: without a version, a channel and the channel list it is a usage error" {
    run "$SCRIPT" tags 1.1.0
    [ "$status" -eq 2 ]
    [ ! -e "$CALLS" ] || [ ! -s "$CALLS" ]
}

# -------------------------------------------------------- has-desktop

@test "has-desktop: a manifest that lists the desktop package" {
    echo '{"assets":{"x86-desktop":{"zip_url":"https://r.example.invalid/a.zip"}}}' \
        > "$BATS_TEST_TMPDIR/manifest.json"
    mock_command_script curl 'cat "$MANIFEST"'
    export MANIFEST="$BATS_TEST_TMPDIR/manifest.json"
    run "$SCRIPT" has-desktop https://r.example.invalid/beta/manifest.json
    [ "$status" -eq 0 ]
}

@test "has-desktop: a manifest without the desktop package, as a 1.0 release, is a skip and not an error" {
    echo '{"assets":{"pi":{"zip_url":"https://r.example.invalid/pi.zip"}}}' > "$BATS_TEST_TMPDIR/manifest.json"
    mock_command_script curl 'cat "$MANIFEST"'
    export MANIFEST="$BATS_TEST_TMPDIR/manifest.json"
    run "$SCRIPT" has-desktop https://r.example.invalid/stable/manifest.json
    [ "$status" -eq 1 ]
    contains "lists no x86-desktop package" "$output"
}

@test "has-desktop: a manifest that cannot be read is an error, never a skip" {
    mock_command_script curl 'exit 7'
    run "$SCRIPT" has-desktop https://r.example.invalid/beta/manifest.json
    [ "$status" -eq 2 ]
    contains "cannot read" "$output"
}

# --------------------------------------------------------------- stage

@test "stage: the package sits where the Dockerfile looks for a release, and the sha256 printed is that file's" {
    zip="$BATS_TEST_TMPDIR/helixscreen-x86-desktop.zip"
    serve="$BATS_TEST_TMPDIR/serve"
    printf 'desktop package\n' > "$zip"
    run "$SCRIPT" stage "$zip" v0.0.0-dryrun.42 "$serve"
    [ "$status" -eq 0 ]
    cmp "$zip" "$serve/releases/v0.0.0-dryrun.42/helixscreen-x86-desktop.zip"
    [ "$output" = "86384a3bb2af7162bb250823454a34ae378a82592e4abd98b9e9d641a32a0681" ]
}

@test "stage: a missing package fails before anything is served" {
    run "$SCRIPT" stage "$BATS_TEST_TMPDIR/missing.zip" v1.0.0 "$BATS_TEST_TMPDIR/serve"
    [ "$status" -ne 0 ]
    contains "no desktop package at" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/serve" ]
}

# ------------------------------------------------------------- listen

write_tcp() { printf '%s\n' "$1" > "$BATS_TEST_TMPDIR/tcp"; }

@test "listen: a socket listening on every address at the port" {
    write_tcp "$TCP_LOOPBACK_ONLY"
    printf '%s\n' "$TCP_LAN" | sed -n 3p >> "$BATS_TEST_TMPDIR/tcp"
    run "$SCRIPT" listen "$BATS_TEST_TMPDIR/tcp" 7130
    [ "$status" -eq 0 ]
}

@test "listen: loopback only does not reach a browser on another machine" {
    write_tcp "$TCP_LOOPBACK_ONLY"
    run "$SCRIPT" listen "$BATS_TEST_TMPDIR/tcp" 7130
    [ "$status" -eq 1 ]
    [ -z "$output" ]
}

@test "listen: a connection or a closed socket on the port is not a listener" {
    write_tcp "$TCP_NO_SERVER"
    run "$SCRIPT" listen "$BATS_TEST_TMPDIR/tcp" 7130
    [ "$status" -eq 1 ]
}

@test "listen: another port listening on every address does not count" {
    write_tcp "$TCP_LAN"
    run "$SCRIPT" listen "$BATS_TEST_TMPDIR/tcp" 5432
    [ "$status" -eq 1 ]
}

# ------------------------------------------------------------ started

@test "started: remote control on in the settings and the server listening for the browser" {
    run "$SCRIPT" started "$SMOKE_SETTINGS" "$SMOKE_TCP" 7130
    [ "$status" -eq 0 ]
}

@test "started: remote control off in the settings the container wrote" {
    printf '%s\n' "$SETTINGS_OFF" > "$SMOKE_SETTINGS"
    run "$SCRIPT" started "$SMOKE_SETTINGS" "$SMOKE_TCP" 7130
    [ "$status" -eq 1 ]
    contains "screen sharing and remote control are not on" "$output"
}

@test "started: a server on loopback only" {
    printf '%s\n' "$TCP_LOOPBACK_ONLY" > "$SMOKE_TCP"
    run "$SCRIPT" started "$SMOKE_SETTINGS" "$SMOKE_TCP" 7130
    [ "$status" -eq 1 ]
    contains "does not listen on every address" "$output"
}

@test "started: settings with no active printer" {
    echo '{"printers":{}}' > "$SMOKE_SETTINGS"
    run "$SCRIPT" started "$SMOKE_SETTINGS" "$SMOKE_TCP" 7130
    [ "$status" -ne 0 ]
}

# -------------------------------------------------------------- smoke

@test "smoke: a container with no network and a printer address nothing answers, stopped afterwards" {
    fake_docker
    run "$SCRIPT" smoke img
    [ "$status" -eq 0 ]
    contains "ok: /info answered" "$output"
    [ "$(head -n 1 "$CALLS")" = "run -d --network none --name $NAME -e PRINTER_URL=127.0.0.1:7125 img" ]
    has_line "exec $NAME curl -fsS -m 2 http://127.0.0.1:7130/info" "$(cat "$CALLS")"
    [ "$(tail -n 1 "$CALLS")" = "rm -f $NAME" ]
}

@test "smoke: a screen server that never answers fails, and the container is still stopped" {
    fake_docker
    export SMOKE_UP_RC=7
    run "$SCRIPT" smoke img
    [ "$status" -eq 1 ]
    contains "did not answer /info" "$output"
    [ "$(tail -n 1 "$CALLS")" = "rm -f $NAME" ]
}

@test "smoke: a server on loopback only fails, and the container is still stopped" {
    fake_docker
    printf '%s\n' "$TCP_LOOPBACK_ONLY" > "$SMOKE_TCP"
    run "$SCRIPT" smoke img
    [ "$status" -eq 1 ]
    contains "does not listen on every address" "$output"
    [ "$(tail -n 1 "$CALLS")" = "rm -f $NAME" ]
}

@test "smoke: a container that exits before the server answers is still stopped" {
    fake_docker
    export SMOKE_UP_RC=1
    run "$SCRIPT" smoke img
    [ "$status" -eq 1 ]
    [ "$(tail -n 1 "$CALLS")" = "rm -f $NAME" ]
}

# -------------------------------------------------------------- build

@test "build-release: the Dockerfile reads the channel from the release's public URL" {
    cat > "$BATS_TEST_TMPDIR/docker" << 'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$DOCKER_CALLS"
EOF
    chmod +x "$BATS_TEST_TMPDIR/docker"
    export PATH="$BATS_TEST_TMPDIR:$PATH"
    run "$SCRIPT" build-release img:ci beta https://r.example.invalid 1.2.0-beta.2 abc123 \
        https://github.com/prestonbrown/helixscreen
    [ "$status" -eq 0 ]
    [ "$(cat "$CALLS")" = "build --build-arg CHANNEL=beta --build-arg R2_BASE_URL=https://r.example.invalid --label org.opencontainers.image.source=https://github.com/prestonbrown/helixscreen --label org.opencontainers.image.version=1.2.0-beta.2 --label org.opencontainers.image.revision=abc123 -t img:ci -f $PWD/docker/Dockerfile.homeserver $PWD/docker" ]
}

@test "build-dryrun: the run's own package is served to the Dockerfile from this host, then the server stops" {
    # A real web server needs real sleeps between its tries.
    PATH="${PATH#"$BATS_TEST_TMPDIR/bin:"}"
    # The real python3, behind a shim that writes down the server's pid.
    real_python3=$(command -v python3)
    mkdir -p "$BATS_TEST_TMPDIR/py"
    cat > "$BATS_TEST_TMPDIR/py/python3" << EOF
#!/bin/sh
[ "\$1 \$2" != "-m http.server" ] || echo \$\$ > "$SERVER_PID_FILE"
exec "$real_python3" "\$@"
EOF
    chmod +x "$BATS_TEST_TMPDIR/py/python3"
    zip="$BATS_TEST_TMPDIR/helixscreen-x86-desktop.zip"
    printf 'desktop package\n' > "$zip"
    export SOURCE_ZIP="$zip" SERVED_ZIP="$BATS_TEST_TMPDIR/served.zip"
    # The fake build fetches the package the way the Dockerfile does.
    cat > "$BATS_TEST_TMPDIR/docker" << 'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$DOCKER_CALLS"
base='' release=''
while [ $# -gt 0 ]; do
    case "$1" in
        R2_BASE_URL=*) base=${1#R2_BASE_URL=} ;;
        RELEASE=*) release=${1#RELEASE=} ;;
    esac
    shift
done
curl -fsSL --max-time 5 "$base/releases/$release/helixscreen-x86-desktop.zip" -o "$SERVED_ZIP"
cmp "$SERVED_ZIP" "$SOURCE_ZIP"
EOF
    chmod +x "$BATS_TEST_TMPDIR/docker"
    export PATH="$BATS_TEST_TMPDIR:$BATS_TEST_TMPDIR/py:$PATH"
    run "$SCRIPT" build-dryrun img:ci v0.0.0-dryrun.42 "$zip" 0.0.0-dryrun.42 abc123 \
        https://github.com/prestonbrown/helixscreen
    [ "$status" -eq 0 ]
    [ -s "$SERVED_ZIP" ]
    calls=$(cat "$CALLS")
    # One call, the port the only part that varies.
    case "$calls" in
        "build --network=host --build-arg RELEASE=v0.0.0-dryrun.42 --build-arg RELEASE_SHA256=86384a3bb2af7162bb250823454a34ae378a82592e4abd98b9e9d641a32a0681 --build-arg R2_BASE_URL=http://127.0.0.1:"[0-9]*" --label org.opencontainers.image.source=https://github.com/prestonbrown/helixscreen --label org.opencontainers.image.version=0.0.0-dryrun.42 --label org.opencontainers.image.revision=abc123 -t img:ci -f $PWD/docker/Dockerfile.homeserver $PWD/docker") ;;
        *) fail "unexpected docker calls: $calls" ;;
    esac
    # The server that answered the build is gone once the build has.
    [ -s "$SERVER_PID_FILE" ] || fail "the build started no web server"
    run kill -0 "$(cat "$SERVER_PID_FILE")"
    [ "$status" -ne 0 ]
}

teardown() {
    # A build that left its server behind must not outlive its test.
    if [ -s "${SERVER_PID_FILE:-}" ]; then
        kill "$(cat "$SERVER_PID_FILE")" 2>/dev/null || true
    fi
}
