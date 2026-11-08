#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Flag gcode sends whose error callback only logs but still claims the report.

`IMoonrakerAPI::execute_gcode` takes a trailing `caller_surfaces_errors`
(default true) meaning "my on_error actually SHOWS a human something". When that
claim is false, `MoonrakerRequestTracker` records the rejection for cross-channel
dedup and `GcodeErrorRouter` then suppresses its own report of Klipper's `!!`
broadcast -- so a rejected macro reaches nobody at all. See
include/rpc_error_policy.h.

A callback that only calls spdlog is exactly that false claim: the log is not a
user-visible report. This gate finds those sites.

Detection is deliberately conservative -- it only fires when the error lambda's
body is *entirely* logging (or empty). Anything that touches a notification, a
modal, or a subject is left alone, because a gate that cries wolf gets switched
off and then protects nothing.

Escape hatch for a deliberate site:

    // ERROR_OWNERSHIP_OK: cleanup failure is expected; router must stay quiet
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from source_strip import blank_cxx

DEFAULT_SRC_ROOT = Path("src")

# Sends that reach printer.gcode.script, i.e. the ones Klipper mirrors as `!!`.
# Plain JSON-RPC methods have no second channel, so the generic fallback is
# their only surface and this rule does not apply to them.
SEND_CALLS = ("execute_gcode",)

ANNOTATION = "ERROR_OWNERSHIP_OK"

# The parameter that settles the question explicitly. Its presence -- with any
# value -- means the author made a deliberate choice, so we stay quiet.
EXPLICIT_PARAM = "caller_surfaces_errors"

# Anything here in a callback body means it plausibly reaches a human. Kept
# broad on purpose: a false negative is a missed bug, a false positive is a
# disabled gate.
UI_MARKERS = (
    "NOTIFY_",
    "notify_error_tr",
    "notify_tr",
    "ui_notification_",
    "ToastManager",
    "show_toast",
    "show_error",
    "Modal",
    "modal_show",
    "lv_subject_",
    "set_error",
    "report_error",
    "notify_ams_error",
    "AmsErrorHelper",
    "present_",
)

LOG_CALL = re.compile(r"\bspdlog::\w+")
ERROR_LAMBDA = re.compile(r"\[[^\]]*\]\s*\(\s*const\s+MoonrakerError\s*&")


def match_delim(blanked: str, start: int, open_ch: str, close_ch: str) -> int:
    """Index just past the delimiter matching the one at `start`, or -1."""
    depth = 0
    for i in range(start, len(blanked)):
        if blanked[i] == open_ch:
            depth += 1
        elif blanked[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


def classify_callback(body: str) -> str:
    """'logs_only', 'empty', or 'surfaces'."""
    if any(marker in body for marker in UI_MARKERS):
        return "surfaces"
    stripped = body.strip().strip("{}").strip()
    if not stripped:
        return "empty"
    # Remove the logging calls (and their arguments) and see what is left. If
    # every remaining statement is trivial bookkeeping we treat it as log-only.
    without_logs = LOG_CALL.sub("", stripped)
    residue = re.sub(r"[\s;{}()\[\],.\"']", "", without_logs)
    # Heuristic: a body that is mostly logging plus small internal state resets
    # still tells the user nothing. Anything substantial gets the benefit of the
    # doubt and is treated as surfacing.
    if LOG_CALL.search(stripped) and len(residue) <= 160:
        return "logs_only"
    return "surfaces"


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return (line_no, call_name, kind) for each offending site."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    blanked = blank_cxx(text)
    hits: list[tuple[int, str, str]] = []

    for call in SEND_CALLS:
        for m in re.finditer(rf"\b{call}\s*\(", blanked):
            open_paren = blanked.index("(", m.start())
            end = match_delim(blanked, open_paren, "(", ")")
            if end == -1:
                continue
            call_src = text[m.start() : end]
            call_blanked = blanked[m.start() : end]

            if EXPLICIT_PARAM in call_src or ANNOTATION in call_src:
                continue
            # Also honour an annotation on the lines just above the call.
            prefix = text[max(0, m.start() - 400) : m.start()]
            if ANNOTATION in prefix:
                continue

            lam = ERROR_LAMBDA.search(call_blanked)
            if not lam:
                continue  # nullptr / named callback -- derivation handles it
            brace = call_blanked.find("{", lam.end())
            if brace == -1:
                continue
            body_end = match_delim(call_blanked, brace, "{", "}")
            if body_end == -1:
                continue
            body = call_src[brace:body_end]

            kind = classify_callback(body)
            if kind in ("logs_only", "empty"):
                line_no = text.count("\n", 0, m.start()) + 1
                hits.append((line_no, call, kind))
    return hits


def is_mock(path: Path) -> bool:
    """Mock implementations are test infrastructure -- no user is watching them,
    and tests/shell/test_code_lint.bats already exempts the *_mock.* family."""
    return "_mock" in path.name


def iter_sources(root: Path, files: list[str]) -> list[Path]:
    if files:
        return [
            Path(f) for f in files if f.endswith((".cpp", ".cc", ".h")) and not is_mock(Path(f))
        ]
    return sorted(p for p in root.rglob("*.cpp") if not is_mock(p))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="*", help="files to scan (default: src/)")
    ap.add_argument("--src-root", type=Path, default=DEFAULT_SRC_ROOT)
    ap.add_argument("--list", action="store_true", help="Print every site")
    ap.add_argument("--summary", action="store_true", help="one-line counts")
    ap.add_argument(
        "--max-allowed",
        type=int,
        default=None,
        help="Ratchet: fail if the count exceeds this. May fall, never rise.",
    )
    args = ap.parse_args()

    all_hits: list[tuple[Path, int, str, str]] = []
    for path in iter_sources(args.src_root, args.files):
        for line_no, call, kind in scan_file(path):
            all_hits.append((path, line_no, call, kind))

    count = len(all_hits)

    if args.list:
        for path, line_no, call, kind in all_hits:
            print(f"{path}:{line_no}: {call}() error callback {kind} but claims the report")

    if args.summary or not args.list:
        print(f"gcode error-ownership: {count} site(s) with a log-only callback claiming the report")

    if args.max_allowed is not None and count > args.max_allowed:
        print(
            f"FAIL: {count} site(s) exceeds the allowed {args.max_allowed}.\n"
            f"      A log-only error callback must pass {EXPLICIT_PARAM}=false, or the\n"
            f"      `!!` router is silenced and the failure reaches nobody.\n"
            f"      See include/rpc_error_policy.h. Run with --list to see them.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
