#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# homeserver_image.sh: build and start the home-server image
# (docker/Dockerfile.homeserver) in a run of .github/workflows/release.yml.
#
# The image is the released x86_64 desktop package, which the Dockerfile
# downloads and checks against a sha256. A release takes both from its channel
# manifest and refuses a manifest that serves another version: the re-run of an
# old release, or two releases at once, must never tag an image with contents
# it does not hold, because a registry has no downgrade guard. A dry run is not
# served, so the run's own package is put on a web server on this host for the
# length of the build. The build shares the host's network to reach that
# server: `docker build` runs on the runner's default builder, whose docker
# driver takes --network=host without an entitlement.
#
# The container is started once before anything is pushed, with no network and
# a printer address nothing answers, so it can never reach a real printer
# whatever a config names. Its screen server must listen for a browser on every
# address: the image's own health check proves loopback only.
#
# Subcommands, for tests/shell/test_homeserver_image.bats:
#     tags VERSION CHANNEL CHANNELS   the tags to push, one per line: VERSION,
#                                      each channel of CHANNELS, and latest
#                                      only when CHANNEL is stable
#     has-desktop MANIFEST_URL VERSION
#                                      0 and the package's sha256 on stdout
#                                      when the manifest serves VERSION and
#                                      lists an x86-desktop package, 1 when it
#                                      lists none, 2 when it cannot be read,
#                                      serves another version or names no
#                                      sha256
#     channel-current MANIFEST_URL VERSION
#                                      0 when the manifest serves VERSION, 2
#                                      when it serves another version or
#                                      cannot be read
#     stage ZIP TAG DIR               put ZIP at DIR/releases/TAG/, where the
#                                      Dockerfile looks, and print its sha256
#     build IMAGE TAG SHA256 BASE_URL VERSION REVISION SOURCE_URL
#     build-dryrun IMAGE TAG ZIP VERSION REVISION SOURCE_URL
#     listen FILE PORT                0 when FILE, a /proc/net/tcp, has a socket
#                                      listening on every address at PORT
#     started SETTINGS TCP PORT       the verdict on a started container
#     smoke IMAGE                     start the container, wait for /info, run
#                                      `started`, and stop it
#
# Env: HOMESERVER_IMAGE_PORT (the screen server's port in the image, 7130),
# HOMESERVER_SMOKE_TIMEOUT (seconds to wait for /info, 60),
# HOMESERVER_SERVE_WAIT (seconds to wait for the dry run's web server, 10).
# The smoke reads the time with `date +%s` and waits with `sleep`, so a test
# can run its minute at once with the fake clock.
set -euo pipefail

port=${HOMESERVER_IMAGE_PORT:-7130}
smoke_timeout=${HOMESERVER_SMOKE_TIMEOUT:-60}
serve_wait=${HOMESERVER_SERVE_WAIT:-10}
name=helixscreen-smoke

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
dockerfile="$root/docker/Dockerfile.homeserver"
context="$root/docker"

usage() { sed -n '/^#     tags/,/^#$/p' "${BASH_SOURCE[0]}" | sed 's/^#//'; }

die() {
    echo "homeserver_image: $*" >&2
    usage >&2
    exit 2
}

# 00000000 is every address and 0A is LISTEN.
listen() {
    local hex
    hex=$(printf '%04X' "$2")
    grep -Eq "^[[:space:]]*[0-9]+: 00000000:$hex 00000000:0000 0A" "$1"
}

started() {
    local settings=$1 tcp=$2 at=$3 id
    id=$(jq -er '.active_printer_id' "$settings") || return 1
    if ! jq -e --arg id "$id" \
            '.printers[$id].remote_screen.enabled == true and
             .printers[$id].remote_screen.control == true' "$settings" >/dev/null; then
        echo "screen sharing and remote control are not on for the active printer in $settings" >&2
        return 1
    fi
    listen "$tcp" "$at" || {
        echo "the screen server does not listen on every address at $at" >&2
        return 1
    }
}

stage() {
    local zip=$1 tag=$2 dir=$3
    [ -f "$zip" ] || { echo "no desktop package at $zip" >&2; return 1; }
    mkdir -p "$dir/releases/$tag"
    cp "$zip" "$dir/releases/$tag/helixscreen-x86-desktop.zip"
    sha256sum "$zip" | awk '{print $1}'
}

# The manifest at @p url on stdout, or nothing and a message on stderr.
read_manifest() {
    curl -fsSL --retry 3 "$1" || { echo "cannot read $1" >&2; return 1; }
}

# Fail unless @p manifest, read from @p url, serves @p version.
serves_version() {
    local manifest=$1 version=$2 url=$3 served
    served=$(jq -r '.version // empty' <<< "$manifest")
    if [ "$served" != "$version" ]; then
        echo "$url serves version '${served:-none}', not $version" >&2
        return 1
    fi
}

# What docker build takes before its own arguments: the dry run's host network.
BUILD_OPTS=()

# docker build of TAG's package, downloaded from BASE_URL and checked against
# SHA256, as IMAGE, labelled with VERSION and REVISION.
docker_build() {
    local image=$1 tag=$2 sha=$3 base=$4 version=$5 revision=$6 source=$7
    # shellcheck disable=SC2086 # an empty BUILD_OPTS must expand to nothing
    docker build ${BUILD_OPTS[@]+"${BUILD_OPTS[@]}"} \
        --build-arg "RELEASE=$tag" --build-arg "RELEASE_SHA256=$sha" \
        --build-arg "R2_BASE_URL=$base" \
        --label "org.opencontainers.image.source=$source" \
        --label "org.opencontainers.image.version=$version" \
        --label "org.opencontainers.image.revision=$revision" \
        -t "$image" -f "$dockerfile" "$context"
}

cmd=${1:-}
[ -n "$cmd" ] || { usage >&2; exit 2; }
shift

case "$cmd" in
tags)
    [ $# -eq 3 ] || die "tags takes VERSION, CHANNEL and CHANNELS"
    echo "$1"
    for channel in $3; do
        echo "$channel"
    done
    if [ "$2" = stable ]; then
        echo latest
    fi
    ;;

has-desktop)
    [ $# -eq 2 ] || die "has-desktop takes MANIFEST_URL and VERSION"
    manifest=$(read_manifest "$1") || exit 2
    serves_version "$manifest" "$2" "$1" || exit 2
    if ! jq -e '.assets["x86-desktop"]' <<< "$manifest" >/dev/null 2>&1; then
        echo "$1 lists no x86-desktop package" >&2
        exit 1
    fi
    sha=$(jq -er '.assets["x86-desktop"].zip_sha256' <<< "$manifest") || {
        echo "$1 lists an x86-desktop package with no sha256" >&2
        exit 2
    }
    echo "$sha"
    ;;

channel-current)
    [ $# -eq 2 ] || die "channel-current takes MANIFEST_URL and VERSION"
    manifest=$(read_manifest "$1") || exit 2
    serves_version "$manifest" "$2" "$1" || exit 2
    echo "$1 still serves $2"
    ;;

stage)
    [ $# -eq 3 ] || die "stage takes ZIP, TAG and DIR"
    stage "$1" "$2" "$3"
    ;;

build)
    [ $# -eq 7 ] || die "build takes IMAGE, TAG, SHA256, BASE_URL, VERSION, REVISION and SOURCE_URL"
    BUILD_OPTS=()
    docker_build "$@"
    ;;

build-dryrun)
    [ $# -eq 6 ] || die "build-dryrun takes IMAGE, TAG, ZIP, VERSION, REVISION and SOURCE_URL"
    serve=$(mktemp -d)
    server=""
    stop_server() {
        if [ -n "$server" ]; then
            kill "$server" 2>/dev/null || true
            wait "$server" 2>/dev/null || true
        fi
        rm -rf "$serve"
    }
    trap stop_server EXIT
    sha=$(stage "$3" "$2" "$serve")
    at=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    python3 -m http.server "$at" --bind 127.0.0.1 --directory "$serve" >/dev/null 2>&1 &
    server=$!
    url="http://127.0.0.1:$at/releases/$2/helixscreen-x86-desktop.zip"
    tries=0
    until curl -fsS -o /dev/null --max-time 2 "$url"; do
        tries=$((tries + 1))
        if [ "$tries" -ge $((serve_wait * 2)) ]; then
            echo "the web server on 127.0.0.1:$at never served $url" >&2
            exit 1
        fi
        sleep 0.5
    done
    BUILD_OPTS=(--network=host)
    docker_build "$1" "$2" "$sha" "http://127.0.0.1:$at" "$4" "$5" "$6"
    ;;

listen)
    [ $# -eq 2 ] || die "listen takes FILE and PORT"
    listen "$1" "$2"
    ;;

started)
    [ $# -eq 3 ] || die "started takes SETTINGS, TCP and PORT"
    started "$1" "$2" "$3"
    ;;

smoke)
    [ $# -eq 1 ] || die "smoke takes IMAGE"
    work=$(mktemp -d)
    trap 'docker rm -f "$name" >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
    docker run -d --network none --name "$name" -e PRINTER_URL=127.0.0.1:7125 "$1" >/dev/null
    deadline=$(( $(date +%s) + smoke_timeout ))
    tries=0
    until docker exec "$name" curl -fsS -m 2 "http://127.0.0.1:$port/info" >/dev/null 2>&1; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            echo "the screen server did not answer /info on port $port within ${smoke_timeout}s; container log:" >&2
            docker logs "$name" 2>&1 | tail -n 30 >&2 || true
            exit 1
        fi
        tries=$((tries + 1))
        sleep 1
    done
    docker exec "$name" cat /proc/net/tcp > "$work/tcp"
    docker exec "$name" cat /config/settings.json > "$work/settings.json"
    started "$work/settings.json" "$work/tcp" "$port"
    echo "ok: /info answered after about ${tries}s, the screen server listens on every address at $port, and remote control is on"
    ;;

*)
    die "unknown subcommand '$cmd'"
    ;;
esac
