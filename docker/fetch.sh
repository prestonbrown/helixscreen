#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fetch - hardened download helper for the toolchain images.
#
#   Usage: fetch <url> [output-path]
#
# Every toolchain image pulls a compiler tarball plus OpenSSL and zlib sources
# over the network. Each of those is a single point of failure for a release
# whenever the image's Docker layer cache is cold, and a bare `wget -q` gives up
# on the two most common transient failures without retrying at all. Measured
# with GNU wget 1.21.4:
#
#   DNS failure          `wget --tries=5 https://<nonexistent>/x`
#                        -> gives up in 24 ms. --tries is ignored entirely;
#                           wget treats host resolution as fatal and NO flag
#                           changes that. Only a fresh process can re-resolve,
#                           which is what the outer loop below exists for.
#
#   Connection refused   `wget --tries=3 http://127.0.0.1:1/x`
#                        -> gives up in 4 ms unless --retry-connrefused is
#                           given, in which case it retries as asked.
#
# The v0.99.111 cc1 toolchain build died 52 ms into a zlib download whose URL
# was, and still is, valid. That timescale is one of the two classes above, not
# a slow or truncated transfer, so retry flags alone would not have saved it.
#
# Flags, and why each is not the default:
#
#   --retry-connrefused  see above; off by default.
#   --timeout=30         wget has NO connect timeout by default, so a blackholed
#                        SYN hangs the layer for the kernel's full TCP retry
#                        window. This caps DNS, connect and idle-read at 30 s.
#                        It is an idle timeout, not a total one, so it is safe
#                        for the 150 MB toolchain tarballs.
#   --waitretry=10       linear backoff (1 s, 2 s, ... 10 s) between wget's own
#                        retries. Without it the retries are worthless.
#   --tries=5            deliberately LOWER than wget's default of 20: 20 tries
#                        with no backoff burn through in milliseconds against
#                        the same broken resolver or route, and 20 tries *with*
#                        backoff would stall a layer for minutes before the
#                        outer loop gets a chance to re-resolve. 5 inner x 3
#                        outer is 15 attempts spread over a useful window.
#   -nv                  -q suppresses the failure *reason* along with the
#                        progress bar (verified: exit 4, empty stderr), which is
#                        why the cc1 failure logged no cause. -nv keeps errors
#                        and one line per download, without the progress bar
#                        that would otherwise spam the CI log.
#
# Not covered here: integrity. Only Dockerfile.ad5x checksums what it downloads.
# Pinning a sha256 for the ARM, Bootlin, OpenSSL and zlib tarballs would also
# catch a silently corrupt transfer, and is worth doing separately.

set -eu

if [ $# -lt 1 ]; then
    echo "usage: fetch <url> [output-path]" >&2
    exit 2
fi

url=$1
out=${2:-$(basename "$url")}

attempts=3
attempt=1

while :; do
    printf '==> fetch (attempt %d/%d): %s\n' "$attempt" "$attempts" "$url"

    rc=0
    wget --retry-connrefused \
         --timeout=30 \
         --waitretry=10 \
         --tries=5 \
         -nv \
         -O "$out" \
         "$url" || rc=$?

    if [ "$rc" -eq 0 ]; then
        exit 0
    fi

    # Never leave a truncated file behind for a later tar/configure to trip on.
    rm -f "$out"

    if [ "$attempt" -ge "$attempts" ]; then
        printf '!!! fetch failed after %d attempts (last wget exit %d): %s\n' \
            "$attempts" "$rc" "$url" >&2
        exit "$rc"
    fi

    delay=$((attempt * 20))
    printf '    attempt %d failed (wget exit %d); retrying in %ds\n' \
        "$attempt" "$rc" "$delay" >&2
    sleep "$delay"
    attempt=$((attempt + 1))
done
