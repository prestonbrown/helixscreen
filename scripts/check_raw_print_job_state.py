#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every read of the raw print wire must say why it is not on the lifecycle.

`helix::PrintJobState` is what `print_stats.state` said. It cannot express a job
the app has committed to but the printer has not reported yet, so a semantic
question asked of it - "does a job own the toolhead right now?" - is blind for the
whole of a pre-print window. That blindness shipped: 21 motion controls stayed
enabled while the toolhead homed and probed, the home print card read idle, a
queue tap deleted the job it then failed to start.

The derived axis is `PrintState` (include/print_lifecycle_state.h), published as
the `print_lifecycle` subject and reached through `get_print_lifecycle()`.

**This gate does not forbid the wire.** Roughly eighty sites legitimately want it:
the parse itself, terminal-outcome formatting, telemetry's phase tracker,
navigation's activation edge, the PRINT_START collector's arming. What it forbids
is reading the wire *silently*, because a reader cannot tell a deliberate wire
read from one that predates the lifecycle - and the difference is a bug class.

Per-line opt-out, and the whole point of the gate:

    // RAW_PRINT_STATE_OK: <why the wire is the right answer here>

Write a real reason. "keeping raw" is not one; the reasons that turned out to
matter were things like "print_filename still holds the PREVIOUS job during a
preparing window" and "this would wipe the data the tracker exists to collect".
"""
import re
import sys
from pathlib import Path

# The three back doors, in the order a sweep would find them.
# The three states where Preparing changes the answer, plus the two accessors
# that hand out the wire. Terminal states are NOT here: derive_print_state()
# maps COMPLETE/CANCELLED/ERROR one-to-one, so reading them raw carries no blind
# spot - a switch that also names them is caught by its PRINTING/PAUSED arm.
PATTERNS = [
    re.compile(r"\bPrintJobState::(PRINTING|PAUSED|STANDBY)\b"),
    re.compile(r"\bget_print_job_state\s*\("),
    re.compile(r"get_print_state_enum_subject\s*\("),
]

# Two matches this close together are one decision - a switch's arms, or a
# two-line condition. Reporting each separately would demand a marker per arm.
GROUP_GAP = 6

COMMENT = re.compile(r"//.*$")
OPT_OUT = "RAW_PRINT_STATE_OK:"

# How far above a site the marker may sit. A switch statement puts several arms
# under one comment, so this is deliberately looser than a per-line rule.
WINDOW = 12

# The derivation layer IS the wire-to-lifecycle boundary: the parse, the name
# table, and derive_print_state() itself. Marking every arm there would be noise
# about the one file whose job is to name every arm.
ALLOWLIST = {
    "src/printer/print_lifecycle_state.cpp",
    "src/printer/printer_state.cpp",
    "include/printer_state.h",
    # The test drivers exist precisely to spell the wire states out.
    "tests/test_helpers/print_state_test_drivers.h",
}


def scan(root: Path, files: list[Path]) -> list[str]:
    hits = []
    for f in files:
        try:
            rel = str(f.resolve().relative_to(root))
        except ValueError:
            rel = str(f)
        if rel in ALLOWLIST:
            continue
        try:
            lines = f.read_text().splitlines()
        except (OSError, UnicodeDecodeError):
            continue
        raw = []
        for i, line in enumerate(lines, 1):
            # A comment NAMING the enum is documentation, not a decision.
            code = COMMENT.sub("", line)
            if not any(p.search(code) for p in PATTERNS):
                continue
            window = "\n".join(lines[max(0, i - 1 - WINDOW) : i])
            if OPT_OUT in window:
                continue
            raw.append((i, line.strip()))

        # Collapse each run into the site a reader would call one decision.
        prev = None
        for i, text in raw:
            if prev is not None and i - prev <= GROUP_GAP:
                prev = i
                continue
            hits.append(f"{rel}:{i}: {text}")
            prev = i
    return hits


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    max_allowed = 0
    args = []
    for a in argv[1:]:
        if a.startswith("--max-allowed="):
            max_allowed = int(a.split("=", 1)[1])
        else:
            args.append(a)

    if args:
        files = [Path(a) for a in args]
    else:
        files = [
            p
            for d in ("src", "include")
            for ext in ("*.cpp", "*.h")
            for p in (root / d).rglob(ext)
        ]

    hits = scan(root, files)
    if len(hits) > max_allowed:
        print(f"Raw print-state reads with no stated reason: {len(hits)} > {max_allowed}")
        for h in hits:
            print("  " + h)
        print()
        print("  Ask the lifecycle instead:")
        print("    state.get_print_lifecycle()          -> PrintState")
        print("    job_holds_machine(lifecycle)         -> Preparing || Printing || Paused")
        print("    observe_print_lifecycle(subject, ..) -> typed observer")
        print(f"  The wire IS right here? Say why: // {OPT_OUT} <reason>")
        return 1

    print(f"✅ raw print-state reads: {len(hits)} <= baseline ({max_allowed})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
