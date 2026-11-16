#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Time every check in quality-checks.sh without editing it.

The gate times each check itself and prints the list under QC_PROFILE=1; this
runs it that way and turns the list into a table with each check's share of
the wall clock, which is the number a commit waits for.

    scripts/qc_timing.py                    # same arguments as quality-checks.sh
    scripts/qc_timing.py --staged-only
    QC_TIMING_CSV=/tmp/qc.csv scripts/qc_timing.py

The gate's own output passes through unchanged and its exit status is this
script's exit status, so it can stand in for the gate anywhere, including a
hook. The table goes to stderr; QC_TIMING_CSV appends `iso,seconds,check` rows
for comparing runs.
"""

import os
import re
import subprocess
import sys
import time
from datetime import datetime

# The checks run in parallel and their output is replayed once all have
# finished, so the gaps between their headers in the transcript measure nothing.
# The gate's own per-check clock is the only timing there is.
PROFILE_HEADER = "gate times (ms), slowest first:"
PROFILE_ROW = re.compile(r"^\s*(\d+)\s+(qc_\w+)\s*$")
TOP_N = 8


def main() -> int:
    gate = os.path.join(os.path.dirname(os.path.abspath(__file__)), "quality-checks.sh")
    proc = subprocess.Popen(
        [gate, *sys.argv[1:]],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env={**os.environ, "QC_PROFILE": "1"},
    )

    spans: list[tuple[float, str]] = []
    started = time.monotonic()
    in_profile = False

    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        if line.strip() == PROFILE_HEADER:
            in_profile = True
            continue
        if in_profile:
            match = PROFILE_ROW.match(line)
            if match:
                spans.append((int(match.group(1)) / 1000.0, match.group(2)))
            else:
                in_profile = False

    status = proc.wait()
    total = time.monotonic() - started
    spans.sort(reverse=True)
    print(f"\n--- {len(spans)} checks, {total:.1f}s wall, slowest first ---", file=sys.stderr)
    for seconds, name in spans[:TOP_N]:
        share = 100.0 * seconds / total if total else 0.0
        print(f"  {seconds:6.1f}s  {share:4.1f}%  {name}", file=sys.stderr)

    csv = os.environ.get("QC_TIMING_CSV")
    if csv:
        stamp = datetime.now().isoformat(timespec="seconds")
        with open(csv, "a", encoding="utf-8") as handle:
            for seconds, name in spans:
                handle.write(f"{stamp},{seconds:.3f},{name}\n")

    return status


if __name__ == "__main__":
    sys.exit(main())
