#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Time every check in quality-checks.sh without editing it.

Each check announces itself on its own line before it starts work, so the gap
between one announcement and the next is that check's cost. Most checks do not
report their own elapsed time, which is why the gate's total is the only figure
anyone has: this fills that in for all of them at once.

    scripts/qc_timing.py                    # same arguments as quality-checks.sh
    scripts/qc_timing.py --staged
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

# A check header opens with an emoji and carries a gerund. Anything else is a
# finding, a note, or a blank line, and belongs to the check above it.
#
# The gate announces a check with `echo -n`, so the line does not complete until
# that check has produced its first output: a header therefore ARRIVES at the
# check's end, and the gap since the previous header is the cost of the check
# that just announced itself, not of the one before it.
HEADER = re.compile(r"^[^\w\s\[(]+\s+(Checking|Running|Validating|Verifying|Scanning)\b(.*)$")
TOP_N = 8


def main() -> int:
    gate = os.path.join(os.path.dirname(os.path.abspath(__file__)), "quality-checks.sh")
    proc = subprocess.Popen(
        [gate, *sys.argv[1:]],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        # Every python gate downstream buffers when its stdout is a pipe, which
        # would time the flush rather than the work.
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )

    spans: list[tuple[float, str]] = []
    started = time.monotonic()
    t0 = started

    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        match = HEADER.match(line.strip())
        if not match:
            continue
        now = time.monotonic()
        name = re.sub(r"\.{2,}.*$", "", line.strip())
        spans.append((now - t0, name))
        t0 = now

    status = proc.wait()
    now = time.monotonic()

    total = now - started
    spans.append((now - t0, "(after the last check: summary, cleanup)"))
    spans.sort(reverse=True)
    print(f"\n--- {len(spans)} checks in {total:.1f}s, slowest first ---", file=sys.stderr)
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
