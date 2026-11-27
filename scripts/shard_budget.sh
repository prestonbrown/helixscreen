#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A unit sweep shard's time budget, stretched by the load other work puts on
# the host.
#
#   scripts/shard_budget.sh <timeout-seconds>
#
# Prints, on one line:
#   <budget-seconds> <factor-x100> <load-x100> <cores> <capped>
#
# The budget exists to catch a hang, and a shard that a busy host starves is
# slow, not hung. With a load of L beside it on N cores, a thread runs at
# N / (L + 1) of full speed, so the factor is (load + 1) / cores. It never
# falls below 1, so an idle host and a host without /proc/loadavg (macOS)
# keep the budget they were given. It never rises above SHARD_BUDGET_CAP_X100
# (200 by default): a host loaded far past its cores must not turn a real hang
# into a long wait, and a capped budget says so in its last field. The load is
# the one-minute average when this runs, so work the sweep starts after that
# is not counted.
#
# Environment:
#   HELIX_SHARD_HOST_LOAD   the load in hundredths, instead of /proc/loadavg
#   HELIX_SHARD_HOST_CORES  the core count, instead of nproc
#   SHARD_BUDGET_CAP_X100   the most the factor may be (default 200)
# Tests name the load and the core count; nothing else sets them.

set -eu

if [ $# -ne 1 ] || ! [ "$1" -gt 0 ] 2>/dev/null; then
    echo "usage: $0 <timeout-seconds>" >&2
    exit 2
fi
timeout=$1

load=${HELIX_SHARD_HOST_LOAD:-}
if [ -z "$load" ]; then
    load=$(awk '{ printf "%d", $1 * 100 + 0.5 }' /proc/loadavg 2>/dev/null || true)
    load=${load:-0}
fi
cores=${HELIX_SHARD_HOST_CORES:-}
if [ -z "$cores" ]; then
    cores=$(nproc 2>/dev/null || true)
    cores=${cores:-1}
fi

factor=$(((load + 100 + cores - 1) / cores))
capped=0
cap=${SHARD_BUDGET_CAP_X100:-200}
if [ "$factor" -gt "$cap" ]; then
    factor=$cap
    capped=1
fi
if [ "$factor" -lt 100 ]; then
    factor=100
fi
budget=$(((timeout * factor + 99) / 100))

echo "$budget $factor $load $cores $capped"
