#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source rules: C++ shapes that compile, run, and answer the wrong question.

Each rule names one shape in this tree's C++ that the compiler accepts and no
unit test reaches. All of them are the same kind of check, a pattern searched
in a set of C++ files with an optional per-site marker, so they share one
script; each keeps its own section below with its scope, marker and message.

  file-set rules (scan src/ and include/, or the FILEs given):
    print-state-cast            static_cast of an lv_subject int into a print enum
                                (// PRINT_STATE_CAST_OK: <reason>)
    json-dump-utf8              bare .dump() on a save path (// JSON_DUMP_OK: <reason>)
    raw-print-state             a read of the raw print wire with no stated reason
                                (// RAW_PRINT_STATE_OK: <reason>)
    xml-create-then-hide        an lv_xml_create() result hidden right after the build
                                (// XML_HIDDEN_OK: <reason>; scans src/ and plugins/)

  pinned rules (check named files under --root, no marker):
    touch-rotation-source       display backends gate the stored touch range on
                                the applied rotation, not /display/rotate
    grid-metrics-single-source  grid_edit_mode.cpp asks GridLayout for the grid's
                                dimensions in one place
    filament-ops-neutral        filament-op code never asks which backend it is on

Usage:
  check_source_rules.py                           # every rule
  check_source_rules.py --rule NAME [--rule ...]  # the named rules only
  check_source_rules.py --rule NAME FILE...       # one file-set rule over FILEs
  check_source_rules.py --root DIR ...            # resolve every scope under DIR

FILE arguments replace a file-set rule's default scope and are refused for a
pinned rule. --max-allowed=N lets a file-set rule pass with up to N sites
(default 0). Exit 0 when every selected rule passes, 1 when one fails.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

from source_strip import blank_cxx

SCRIPT_ROOT = Path(__file__).resolve().parent.parent

Files = Optional[list[Path]]


# ====================================================================
# Shared helpers
# ====================================================================
def cxx_sources(root: Path) -> list[Path]:
    """Every .cpp and .h under src/ and include/."""
    return [
        p for d in ("src", "include") for ext in ("*.cpp", "*.h") for p in (root / d).rglob(ext)
    ]


def rel_path(root: Path, path: Path) -> str:
    """`path` relative to the root when it lies under it, else as given."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def read_lines(path: Path) -> Optional[list[str]]:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError):
        return None


def code_lines(lines: list[str]) -> list[str]:
    """`lines` with every comment blanked and literals kept, line for line."""
    return blank_cxx("\n".join(lines), strings="keep").split("\n")


def marked(lines: list[str], line_no: int, marker: str, above: int) -> bool:
    """True when `marker` sits on line `line_no` (1-based) or up to `above` lines above it."""
    return marker in "\n".join(lines[max(0, line_no - 1 - above) : line_no])


# ====================================================================
# print-state-cast
# ====================================================================
# lv_subject_get_int() returns int, so `static_cast<PrintState>(...)` compiles
# against WHICHEVER subject the author happened to name. The two print enums do
# not share numbering past index 0:
#
#     PrintJobState  STANDBY=0  PRINTING=1   PAUSED=2   COMPLETE=3  CANCELLED=4 ERROR=5
#     PrintState     Idle=0     Preparing=1  Printing=2 Paused=3    Complete=4  ...
#
# so pairing the cast with the wrong subject is silent: a COMPLETE job reads back
# as Paused, a PRINTING one as Preparing. The typed accessors pair each subject
# with its own enum: state.get_print_lifecycle() -> PrintState,
# state.get_print_job_state() -> PrintJobState.
#
# Two shapes, one mistake. In the second, an observer lambda takes `int`, and the
# cast lands three to thirty lines away from the get_*_subject() call that
# decided which enum is correct, so a reader cannot check the pairing at a
# glance. The typed observer factories are the replacement.
PRINT_STATE_CAST = re.compile(
    r"static_cast<\s*(PrintState|helix::PrintJobState|PrintJobState)\s*>\s*\("
    r"(?:\s|//[^\n]*)*(lv_subject_get_int|[A-Za-z_][A-Za-z0-9_]*\s*\))"
)
PRINT_STATE_CAST_OK = "PRINT_STATE_CAST_OK:"

# clang-format wraps a long accessor chain right after the cast's opening paren,
# so `static_cast<PrintState>(` and the lv_subject_get_int() that names the
# subject land on different lines and a per-line search sees neither. The
# pattern's \s* spans newlines, so each line is searched joined with its
# successors. The codebase's idiom also puts the RAW_PRINT_STATE_OK comment
# (which raw-print-state demands) INSIDE the parens, between the cast and the
# read, so the inter-paren gap accepts line comments and the window covers a
# cast plus two comment lines plus the read.
CAST_SPAN_LINES = 4

# The derivation layer converts between the wire and the lifecycle, and
# core/observer_factory.h is where the subject/enum pairing is SUPPOSED to live:
# observe_print_state() and observe_print_lifecycle() each name the one subject
# they are for, which is what makes every call site checkable.
PRINT_STATE_CAST_ALLOWLIST = {
    "src/printer/printer_print_state.cpp",  # get_print_job_state / get_print_lifecycle themselves
    "src/printer/printer_print_state_status.cpp",  # update_from_status, which writes the subject
    "src/printer/print_lifecycle_state.cpp",
    "include/core/observer_factory.h",
}


def print_state_cast(root: Path, files: Files, max_allowed: int) -> int:
    files = files if files is not None else cxx_sources(root)
    hits = []
    for f in files:
        rel = rel_path(root, f)
        if rel in PRINT_STATE_CAST_ALLOWLIST:
            continue
        lines = read_lines(f)
        if lines is None:
            continue
        for i in range(1, len(lines) + 1):
            span = "\n".join(lines[i - 1 : i - 1 + CAST_SPAN_LINES])
            m = PRINT_STATE_CAST.search(span)
            if not m:
                continue
            if span[: m.start()].count("\n"):
                # The match starts on an earlier line; that line's own window
                # holds the whole match, so it is reported exactly once there.
                continue
            if marked(lines, i, PRINT_STATE_CAST_OK, above=3):
                continue
            # Collapse the matched span to one line so a wrapped read reports
            # as the statement it is, not a line ending in a bare `(`.
            matched = " ".join(part.strip() for part in m.group(0).split("\n"))
            hits.append(f"{rel}:{i}: {matched}")

    if len(hits) > max_allowed:
        print("Hand-cast of an lv_subject int into a print enum:")
        for h in hits:
            print("  " + h)
        print()
        print("  Use the typed accessor or factory that owns the pairing:")
        print("    state.get_print_lifecycle()          -> PrintState")
        print("    state.get_print_job_state()          -> PrintJobState")
        print("    observe_print_state(subject, ..)     -> handler takes PrintJobState")
        print("    observe_print_lifecycle(subject, ..) -> handler takes PrintState")
        print(f"  Genuinely needed? Annotate with // {PRINT_STATE_CAST_OK} <reason>")
        return 1

    print(f"✅ print-state casts: {len(hits)} hand-casts ({len(files)} files scanned)")
    return 0


# ====================================================================
# json-dump-utf8
# ====================================================================
# nlohmann's default error handler is error_handler_t::strict, so dump() raises
# json::type_error.316 for any string holding bytes UTF-8 cannot decode. The
# strings that reach a save path are the ones nothing validates (SSIDs, printer
# and tool names, file names, macro text, gcode responses), so under strict one
# stray byte costs the entire document. Unguarded, the throw unwinds through
# LVGL's C frames and terminates the process; behind a catch, the write is
# silently skipped and the user's change is gone at next boot.
#
# The fix is helix::json_util::safe_dump() from json_utils.h, which passes
# error_handler_t::replace. A try/catch around the save is still worth keeping
# for bad_alloc; it is not a substitute.
#
# Flagged: the three shapes that move a document out of the process, a stream
# insertion, an HTTP body and a socket send. A JSON-RPC request is already
# registered as pending when it is serialized, so a throw there strands the
# entry as well as escaping the caller. Not flagged: `.dump()` as a log
# argument (the line is thrown away; a save path is where the loss is
# permanent), safe_dump() itself, and tests.
#
# `.dump(` never matches `safe_dump(`: the helper has no member-access dot in
# front of it, which keeps the correct form out of every pattern.
JSON_DUMP_PATTERNS = [
    (re.compile(r"<<[^;]*?\.dump\s*\("), "serialized into a stream"),
    (re.compile(r"\bbody\s*=\s*[^;]*?\.dump\s*\("), "serialized into an HTTP body"),
    (re.compile(r"\bsend\s*\([^;]*?\.dump\s*\("), "serialized into a socket send"),
]
JSON_DUMP_OK = "JSON_DUMP_OK:"


def json_dump_utf8(root: Path, files: Files, max_allowed: int) -> int:
    files = files if files is not None else cxx_sources(root)
    hits = []
    for f in files:
        rel = rel_path(root, f)
        lines = read_lines(f)
        if lines is None or not any(".dump" in line for line in lines):
            continue
        for i, (raw, code) in enumerate(zip(lines, code_lines(lines)), 1):
            for pattern, what in JSON_DUMP_PATTERNS:
                if not pattern.search(code):
                    continue
                # On the line, or on a comment line just above it.
                if not marked(lines, i, JSON_DUMP_OK, above=2):
                    hits.append(f"{rel}:{i}: {what}: {raw.strip()}")
                break

    if len(hits) > max_allowed:
        print("Bare .dump() on a save path — invalid UTF-8 throws here:")
        for h in hits:
            print("  " + h)
        print()
        print('  Use the replacing serializer from #include "json_utils.h":')
        print("    ofs << helix::json_util::safe_dump(doc, 2);")
        print("    req->body = helix::json_util::safe_dump(batch);")
        print(f"  Genuinely needed? Annotate with // {JSON_DUMP_OK} <reason>")
        return 1

    print(f"✅ JSON save paths: {len(hits)} bare dumps ({len(files)} files scanned)")
    return 0


# ====================================================================
# raw-print-state
# ====================================================================
# `helix::PrintJobState` is what `print_stats.state` said. It cannot express a
# job the app has committed to but the printer has not reported yet, so a
# semantic question asked of it ("does a job own the toolhead right now?") is
# blind for the whole of a pre-print window: motion controls stay enabled while
# the toolhead homes and probes, the home print card reads idle. The derived
# axis is `PrintState` (include/print_lifecycle_state.h), published as the
# `print_lifecycle` subject and reached through `get_print_lifecycle()`.
#
# This rule does not forbid the wire. Roughly eighty sites legitimately want it:
# the parse itself, terminal-outcome formatting, telemetry's phase tracker,
# navigation's activation edge, the PRINT_START collector's arming. It forbids
# reading the wire SILENTLY, because a deliberate wire read and a stale one look
# identical. The marker needs a real reason; "keeping raw" is not one.
#
# The three states where Preparing changes the answer, plus the two accessors
# that hand out the wire. Terminal states are NOT here: derive_print_state()
# maps COMPLETE/CANCELLED/ERROR one-to-one, so reading them raw carries no blind
# spot, and a switch that also names them is caught by its PRINTING/PAUSED arm.
RAW_PRINT_STATE_PATTERNS = [
    re.compile(r"\bPrintJobState::(PRINTING|PAUSED|STANDBY)\b"),
    re.compile(r"\bget_print_job_state\s*\("),
    re.compile(r"get_print_state_enum_subject\s*\("),
]
RAW_PRINT_STATE_OK = "RAW_PRINT_STATE_OK:"

# Two matches this close together are one decision, a switch's arms or a
# two-line condition. Reporting each separately would demand a marker per arm.
RAW_PRINT_STATE_GROUP_GAP = 6

# How far above a site the marker may sit. A switch statement puts several arms
# under one comment, so this is deliberately looser than a per-line rule.
RAW_PRINT_STATE_WINDOW = 12

# The derivation layer IS the wire-to-lifecycle boundary: the parse, the name
# table, and derive_print_state() itself.
RAW_PRINT_STATE_ALLOWLIST = {
    "src/printer/print_lifecycle_state.cpp",
    "src/printer/printer_state.cpp",
    "include/printer_state.h",
    # The test drivers exist precisely to spell the wire states out.
    "tests/test_helpers/print_state_test_drivers.h",
}
RAW_PRINT_STATE_TRIGGERS = ("PrintJobState::", "get_print_job_state", "get_print_state_enum_subject")


def raw_print_state_sites(root: Path, files: list[Path]) -> list[str]:
    """One `path:line: text` entry per unmarked decision that reads the wire."""
    hits = []
    for f in files:
        rel = rel_path(root, f)
        if rel in RAW_PRINT_STATE_ALLOWLIST:
            continue
        lines = read_lines(f)
        if lines is None or not any(t in line for line in lines for t in RAW_PRINT_STATE_TRIGGERS):
            continue
        raw = []
        for i, (line, code) in enumerate(zip(lines, code_lines(lines)), 1):
            # A comment NAMING the enum is documentation, not a decision.
            if not any(p.search(code) for p in RAW_PRINT_STATE_PATTERNS):
                continue
            if marked(lines, i, RAW_PRINT_STATE_OK, above=RAW_PRINT_STATE_WINDOW):
                continue
            raw.append((i, line.strip()))

        # Collapse each run into the site a reader would call one decision.
        prev = None
        for i, text in raw:
            if prev is None or i - prev > RAW_PRINT_STATE_GROUP_GAP:
                hits.append(f"{rel}:{i}: {text}")
            prev = i
    return hits


def raw_print_state(root: Path, files: Files, max_allowed: int) -> int:
    hits = raw_print_state_sites(root, files if files is not None else cxx_sources(root))
    if len(hits) > max_allowed:
        print(f"Raw print-state reads with no stated reason: {len(hits)} > {max_allowed}")
        for h in hits:
            print("  " + h)
        print()
        print("  Ask the lifecycle instead:")
        print("    state.get_print_lifecycle()          -> PrintState")
        print("    job_holds_machine(lifecycle)         -> Preparing || Printing || Paused")
        print("    observe_print_lifecycle(subject, ..) -> typed observer")
        print(f"  The wire IS right here? Say why: // {RAW_PRINT_STATE_OK} <reason>")
        return 1

    print(f"✅ raw print-state reads: {len(hits)} <= baseline ({max_allowed})")
    return 0


# ====================================================================
# xml-create-then-hide
# ====================================================================
# The tree is visible while lv_xml_create() builds it, so a widget that runs a
# layout pass during the build (a dropdown with a selection does) queues the
# root's whole area for redraw even though the caller hides it on return: a
# wasted full render on every open on slow displays. Build it with
# helix::ui::create_xml_hidden() (include/ui_panel_common.h) instead. A create
# meant to be visible-then-hidden (a pooled list row built under its list)
# takes the marker on the create or the hide line.
XML_HIDDEN_OK = "XML_HIDDEN_OK"
XML_HIDE_WINDOW = 40  # lines after the create in which a hide counts as "right after"

XML_CREATE_ASSIGN = re.compile(
    r"(?P<var>[A-Za-z_][\w.\->]*)\s*=\s*"
    r"(?:static_cast<\s*lv_obj_t\s*\*\s*>\s*\(|\(\s*lv_obj_t\s*\*\s*\)\s*)?"
    r"\s*(?:static_cast<\s*lv_obj_t\s*\*\s*>\s*\()?\s*lv_xml_create\s*\(",
    re.S,
)


def tracked_cxx(root: Path, dirs: tuple[str, ...]) -> list[Path]:
    """Every tracked or untracked-but-not-ignored .cpp/.h under `dirs`."""
    out = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", *dirs],
        text=True,
    )
    return [root / f for f in out.split() if f.endswith((".cpp", ".h"))]


def xml_create_then_hide(root: Path, files: Files, max_allowed: int) -> int:
    files = files if files is not None else tracked_cxx(root, ("src", "plugins"))
    bad = []
    for f in files:
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        # The assignment pattern backtracks at every identifier; a file that
        # never spells the call cannot match it.
        if "lv_xml_create" not in text:
            continue
        rel = rel_path(root, f)
        lines = text.split("\n")
        for m in XML_CREATE_ASSIGN.finditer(text):
            var = m.group("var")
            start = text.count("\n", 0, m.start())
            end = text.count("\n", 0, m.end())
            if any(XML_HIDDEN_OK in lines[i] for i in range(start, end + 1)):
                continue
            hide = re.compile(
                r"lv_obj_add_flag\(\s*" + re.escape(var) + r"\s*,\s*LV_OBJ_FLAG_HIDDEN\b"
            )
            for i in range(end, min(end + XML_HIDE_WINDOW, len(lines))):
                if hide.search(lines[i]):
                    if XML_HIDDEN_OK not in lines[i]:
                        bad.append(f"{rel}:{start + 1}: {var} built visible, hidden at line {i + 1}")
                    break

    if len(bad) <= max_allowed:
        print("✅ XML create-then-hide: none" if not bad else f"✅ XML create-then-hide: {len(bad)}")
        return 0
    print("❌ lv_xml_create() result hidden right after the build (a wasted full render):")
    for o in bad:
        print(f"   {o}")
    print("   Use helix::ui::create_xml_hidden() (include/ui_panel_common.h).")
    print(f"   Meant to be built visible? // {XML_HIDDEN_OK}: <reason>")
    return 1


# ====================================================================
# touch-rotation-source
# ====================================================================
# A touch range solved on a rotated panel folds the rotation into
# (min,max,swap) and double-applies it at runtime (prestonbrown/helixscreen#1394).
# Both display backends therefore refuse a stored range when the panel is
# rotated, and the calibration panel refuses to solve a new one. All three must
# answer the same question from the same source.
#
# The `/display/rotate` config key is only the REQUEST. It differs from the
# applied rotation in both directions:
#
#   * CLI `--rotate` or HELIX_DISPLAY_ROTATION with no `/display/rotate` key:
#     the display is rotated, the key reads 0, and a backend trusting the key
#     programs the stored range from the broken window.
#   * A DRM->fbdev rotation fallback that fails: display_manager.cpp logs
#     "Continuing without rotation" and never calls lv_display_set_rotation(),
#     so the display is unrotated while the key still reads 90/270. A backend
#     trusting the key discards a legitimate stored range on every boot.
#
# A lint and not a unit test: the gates live in create_input_pointer(), which
# needs a real fbdev or DRM device and cannot run headless, and no unit test
# kills a revert of either backend to the config-key read.
#
# Per backend source: it calls display_rotation_degrees() or
# display_is_rotated(), and it performs no Config read of "/display/rotate".
# Prose mentions of the key in comments are fine; the check targets the read
# form (`get<...>("/display/rotate"`), not the string.
TOUCH_ROTATION_SOURCES = (
    "src/platform/display/display_backend_fbdev.cpp",
    "src/platform/display/display_backend_drm.cpp",
)

# Either helper answers the question; both are defined in include/display_backend.h.
TOUCH_ROTATION_HELPERS = ("display_rotation_degrees", "display_is_rotated")

TOUCH_ROTATION_FORBIDDEN_READ = re.compile(r"""get\s*<[^>]*>\s*\(\s*["']/display/rotate["']""")


def touch_rotation_failures(root: Path, rel: str) -> list[str]:
    path = root / rel
    if not path.is_file():
        return [f"{rel}: missing - update TOUCH_ROTATION_SOURCES if the backend was renamed"]

    text = path.read_text(encoding="utf-8")
    failures: list[str] = []

    if not any(helper in text for helper in TOUCH_ROTATION_HELPERS):
        failures.append(
            f"{rel}: no call to {' or '.join(TOUCH_ROTATION_HELPERS)}.\n"
            f"      The stored-range gate must ask the display for its rotation."
        )

    for lineno, line in enumerate(text.splitlines(), start=1):
        if TOUCH_ROTATION_FORBIDDEN_READ.search(line):
            failures.append(
                f"{rel}:{lineno}: reads /display/rotate.\n"
                f"      {line.strip()}\n"
                f"      That key is the requested rotation, not the applied one. Use\n"
                f"      display_rotation_degrees() so this gate and the calibration\n"
                f"      panel's gate cannot disagree (prestonbrown/helixscreen#1394)."
            )

    return failures


def touch_rotation_source(root: Path, files: Files, max_allowed: int) -> int:
    failures: list[str] = []
    for rel in TOUCH_ROTATION_SOURCES:
        failures.extend(touch_rotation_failures(root, rel))

    if failures:
        print("FAIL: the touch-range rotation gate must read the display, not the config key.\n")
        for failure in failures:
            print(f"  {failure}")
        print()
        print("   Run: python3 scripts/check_source_rules.py --rule touch-rotation-source")
        return 1

    print(
        f"OK: {len(TOUCH_ROTATION_SOURCES)} display backend(s) gate the stored touch range on the "
        f"applied display rotation."
    )
    return 0


# ====================================================================
# grid-metrics-single-source
# ====================================================================
# GridEditMode::current_metrics() is the one place allowed to ask GridLayout for
# the grid's dimensions; every other path takes a CellMetrics from it instead of
# calling GridLayout directly, so gutter handling and int-vs-float rounding
# cannot drift between call sites.
#
# Counting is per LINE, not per call: two matches on one source line register
# as one hit. That is why current_metrics() puts get_cols() and get_rows() on
# separate lines; folding them onto one line would consume only one of the two
# slots and leave a free slot for a future duplicate elsewhere in the file.
#
# The pattern matches static `GridLayout::get_*` calls only. It does not see the
# instance accessors (`GridLayout temp_grid(bp); temp_grid.cols();`), which this
# file also uses; those are outside the rule's reach.
GRID_METRICS_TARGET = Path("src/ui/home/grid_edit_mode.cpp")
GRID_METRICS_CALL = re.compile(r"GridLayout::get_(cols|rows|dimensions)\b")
GRID_METRICS_LIMIT = 2  # one get_cols + one get_rows, both inside current_metrics()


def grid_metrics_single_source(root: Path, files: Files, max_allowed: int) -> int:
    path = root / GRID_METRICS_TARGET
    if not path.is_file():
        print(f"ERROR: {GRID_METRICS_TARGET} not found", file=sys.stderr)
        return 1

    hits = [
        (n, line.strip())
        for n, line in enumerate(path.read_text().splitlines(), 1)
        if GRID_METRICS_CALL.search(line)
    ]

    if len(hits) > GRID_METRICS_LIMIT:
        print(
            f"ERROR: {GRID_METRICS_TARGET} has {len(hits)} grid-dimension call sites "
            f"(max {GRID_METRICS_LIMIT})."
        )
        print("Cell geometry belongs in GridEditMode::current_metrics(); take a")
        print("CellMetrics from it instead of recomputing.")
        for n, text in hits:
            print(f"  {GRID_METRICS_TARGET}:{n}: {text}")
        print("   Take a helix::CellMetrics from GridEditMode::current_metrics() instead.")
        return 1

    print(f"OK: {GRID_METRICS_TARGET} has {len(hits)} grid-dimension call site(s)")
    return 0


# ====================================================================
# filament-ops-neutral
# ====================================================================
# Wall: the filament-op surfaces never ask which backend they are on. They ask
# capability questions (AmsBackend::head_filament_ops() and the rest), so a
# `get_type()` call or an `AmsType::` name in one of them fails, with no
# baseline and no escape hatch. docs/devel/FILAMENT_MANAGEMENT.md has the
# contract.
#
# Scope is the filament-op code only. buffer_status_modal, ui_ams_detail,
# ui_wizard_summary and ui_panel_ams still branch on the type and are outside it
# until they are ported; the identify wizard is the one place the type belongs.
FILAMENT_OPS_SCOPE = (
    "include/filament_action_set.h",
    "src/ui/filament/filament_action_set.cpp",
    "include/filament_op_dispatch.h",
    "include/filament_op_execute.h",
    "src/ui/filament/filament_op_execute.cpp",
    "include/ui_ams_sidebar.h",
    "src/ui/filament/ui_ams_sidebar.cpp",
    "include/ui_ams_context_menu.h",
    "src/ui/filament/ui_ams_context_menu.cpp",
    "include/ui_panel_filament.h",
    "src/ui/filament/ui_panel_filament.cpp",
    "include/ui_filament_runout_handler.h",
    "src/ui/filament/ui_filament_runout_handler.cpp",
    "src/ui/filament/ui_runout_guidance_modal.cpp",
    "src/ui/panel_widgets/filament_sensor_widget.cpp",
    "src/ui/panel_widgets/print_status_widget.cpp",
    "include/ui_batch_filament_modal.h",
    "src/ui/filament/ui_batch_filament_modal.cpp",
    "src/ui/filament/ui_unload_lane_choice.cpp",
    "src/ui/filament/ui_manual_pull_prompt.cpp",
)

FILAMENT_OPS_SHAPE = re.compile(r"\bget_type\s*\(\s*\)|\bAmsType::")


def filament_ops_neutral(root: Path, files: Files, max_allowed: int) -> int:
    missing = [rel for rel in FILAMENT_OPS_SCOPE if not (root / rel).is_file()]
    if missing:
        # A renamed or deleted file would otherwise pass with nothing checked.
        print(
            "Filament-op scope names files that do not exist; update FILAMENT_OPS_SCOPE in "
            "scripts/check_source_rules.py:"
        )
        for rel in missing:
            print(f"  {rel}")
        return 1

    hits = []
    for rel in FILAMENT_OPS_SCOPE:
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
        for number, code in enumerate(code_lines(text.splitlines()), start=1):
            if FILAMENT_OPS_SHAPE.search(code):
                hits.append(f"{rel}:{number}: {code.strip()}")

    if hits:
        print("Filament-op code asks which backend it is on. Ask a capability instead:")
        for hit in hits:
            print(f"  {hit}")
        print(f"{len(hits)} backend-type reference(s) in filament-op code")
        return 1
    return 0


# ====================================================================
# The table
# ====================================================================
@dataclass(frozen=True)
class Rule:
    check: Callable[[Path, Files, int], int]
    # A pinned rule checks a fixed file list under --root; FILE arguments do not apply.
    pinned: bool = False


RULES = {
    "print-state-cast": Rule(print_state_cast),
    "json-dump-utf8": Rule(json_dump_utf8),
    "raw-print-state": Rule(raw_print_state),
    "xml-create-then-hide": Rule(xml_create_then_hide),
    "touch-rotation-source": Rule(touch_rotation_source, pinned=True),
    "grid-metrics-single-source": Rule(grid_metrics_single_source, pinned=True),
    "filament-ops-neutral": Rule(filament_ops_neutral, pinned=True),
}


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", type=Path, default=SCRIPT_ROOT, help="repository root")
    ap.add_argument(
        "--rule", dest="rules", action="append", choices=list(RULES), help="run only this rule"
    )
    ap.add_argument("--max-allowed", type=int, default=0, help="sites a file-set rule may hold")
    ap.add_argument("files", nargs="*", type=Path, help="files for a file-set rule")
    args = ap.parse_args(argv)

    names = args.rules or list(RULES)
    if args.files:
        pinned = [n for n in names if RULES[n].pinned]
        if pinned:
            ap.error(f"FILE arguments do not apply to pinned rule(s): {', '.join(pinned)}")

    status = 0
    for name in names:
        status |= RULES[name].check(args.root, args.files or None, args.max_allowed)
    return status


if __name__ == "__main__":
    sys.exit(main())
