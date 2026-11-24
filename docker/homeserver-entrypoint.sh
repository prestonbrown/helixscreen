#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Entrypoint of the home-server image (docker/Dockerfile.homeserver).
#
#   PRINTER_URL     required. Where the printer's Moonraker is: 192.168.1.50,
#                     192.168.1.50:7125 or ws://printer.local:7125.
#   PAIR_CODE   optional. Six digits to use as the pairing code instead of a
#                     generated one. Only read when the config is created.
#   SCREEN_SIZE optional. A size name (small, medium, large) or WxH. Default 800x480.
#
# On the first start the config volume gets a settings file with Screen Sharing and
# Allow Remote Control on; the setup wizard is always skipped. Later starts leave it alone, so
# anything changed in the app survives a container update.
set -eu

cfg="${HELIX_CONFIG_DIR:-/config}"

if [ -z "${PRINTER_URL:-}" ]; then
    echo "PRINTER_URL is not set. Give the address of the printer's Moonraker, for example" >&2
    echo "  docker run -e PRINTER_URL=192.168.1.50:7125 ..." >&2
    exit 64
fi

url="$PRINTER_URL"
case "$url" in
    *://*) ;;
    *) url="ws://$url" ;;
esac
# No port named: Moonraker's default.
scheme="${url%%://*}"
rest="${url#*://}"
authority="${rest%%/*}"
case "$authority" in
    *:*) ;;
    *) url="$scheme://$authority:7125${rest#"$authority"}" ;;
esac

mkdir -p "$cfg"

if [ ! -e "$cfg/settings.json" ]; then
    seed=/opt/helixscreen/homeserver-settings.json
    case "${PAIR_CODE:-}" in
        [0-9][0-9][0-9][0-9][0-9][0-9])
            jq --arg c "$PAIR_CODE" '.remote_screen.pair_secret = $c' "$seed" >"$cfg/settings.json" ;;
        "") cp "$seed" "$cfg/settings.json" ;;
        *)
            echo "PAIR_CODE must be exactly six digits; ignoring it." >&2
            cp "$seed" "$cfg/settings.json" ;;
    esac
    echo "$(date '+%H:%M:%S') created $cfg/settings.json (Screen Sharing on)"
fi

echo "$(date '+%H:%M:%S') starting HelixScreen against $url"
exec /opt/helixscreen/bin/helix-screen --skip-wizard --moonraker "$url" \
    -s "${SCREEN_SIZE:-800x480}" "$@"
