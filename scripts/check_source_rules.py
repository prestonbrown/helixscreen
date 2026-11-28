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
    section-returns             a return inside a Catch2 SECTION that has a later
                                sibling (// SECTION_RETURN_OK: <reason>; scans tests/)
    init-list-ternary           an initializer_list picked by a ternary, then stored
                                (// INIT_LIST_TERNARY_OK: <reason>; scans src/,
                                include/ and firmware/)
    getenv-setenv               a pointer from getenv() read after a setenv()
                                (no marker; scans tests/, src/, include/, tools/,
                                firmware/, android/, server/, moonraker-plugin/ and
                                ui_xml/, and a FILE may be a directory)
    xml-create-registered       an lv_xml_create() name that nothing can register
                                (no marker; scans src/)
    lvgl-subject-api            a subject set up through the API LVGL 9.6 deprecates, or
                                an lv_subject_t held by value (// SUBJECT_API_OK: <reason>;
                                scans src/, include/, tests/ and firmware/)

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
  check_source_rules.py --rule NAME --staged-only # a file-set rule over the staged blobs
  check_source_rules.py --root DIR ...            # resolve every scope under DIR

FILE arguments replace a file-set rule's default scope and are refused for a
pinned rule. --max-allowed=N lets a file-set rule pass with up to N sites
(default 0). Exit 0 when every selected rule passes, 1 when one fails.

--staged-only answers a file-set rule's default scope from the index instead of
the working tree: the paths the commit stages under the scope, each read as its
staged blob, so an edit left unstaged is never judged. A staged run prints what
fails; a rule that passes is quiet, with nothing staged under its scope or with
its staged files clean. FILE arguments are refused with it, and so is a rule
that cannot be judged on the staged blobs alone (the pinned rules,
xml-create-registered): exit 2 names each.

A failing rule prints its message and its sites as `file:line: text` lines.
section-returns, init-list-ternary, getenv-setenv and lvgl-subject-api print
the sites alone: scripts/code_lint.sh and scripts/qc/lvgl_subject_api.sh print
their message around them (lint_section_returns, lint_init_list_ternary,
lint_getenv_setenv, qc_lvgl_subject_api). lvgl-subject-api prints a site as
`file:line: call` or `file:line: storage` over its source line, and over the
working tree lists them under any --max-allowed, so a large one is how its
sites are listed.
"""

from __future__ import annotations

import argparse
import bisect
import io
import re
import subprocess
import sys
from contextlib import redirect_stdout
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

from path_scope import in_skipped_dir
from source_strip import blank_cxx
from staged_content import staged_files

SCRIPT_ROOT = Path(__file__).resolve().parent.parent

Files = Optional[list[Path]]


# ====================================================================
# Shared helpers
# ====================================================================
@dataclass(frozen=True)
class Scope:
    """The files a file-set rule scans when it is given no FILE arguments."""
    dirs: tuple[str, ...]
    suffixes: tuple[str, ...]
    # Directory names; a path with one of them anywhere below the root is outside the scope.
    skip: tuple[str, ...] = ()
    # True: only what `git ls-files` names, tracked or untracked and not ignored.
    # False: every file on disk.
    listed_by_git: bool = False
    # True: over the working tree, each directory's files sorted by path. False:
    # the walk's order, one suffix at a time.
    sorted_by_path: bool = False


# src/ and include/, the C++ the app compiles.
SRC_SCOPE = Scope(("src", "include"), (".cpp", ".h"))


class StagedIndex:
    """The index of the repository `root` lies in, as --staged-only reads it.

    `top` is None when `root` lies in no repository. `text` holds the staged blob
    of every path files() names, by path.
    """

    def __init__(self, root: Path):
        self.root = root.resolve()
        top = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip()
        self.top = Path(top).resolve() if top else None
        self.text: dict[Path, Optional[str]] = {}

    def files(self, scope: Scope) -> list[Path]:
        """The paths the commit stages under `scope`, each with its blob filed for read_text()."""
        paths = []
        for staged, text in staged_files(self.top, scope.suffixes):
            path = self.top / staged
            try:
                rel = path.relative_to(self.root)
            except ValueError:
                continue  # staged in the repository, but outside the root
            if rel.parts[0] in scope.dirs and not in_skipped_dir(rel, self.root, scope.skip):
                paths.append(path)
                self.text[path] = text
        return paths


# The index --staged-only reads; None while rules read the working tree.
_index: Optional[StagedIndex] = None


def use_staged_blobs(root: Path) -> bool:
    """Have read_text() serve the index of the repository `root` lies in.

    False when `root` lies in no repository: there is no index to read.
    """
    global _index
    _index = StagedIndex(root)
    return _index.top is not None


def scope_files(root: Path, scope: Scope) -> list[Path]:
    """Every file under `scope` in `root`: what a file-set rule scans when it names no FILE.

    The working tree, or under --staged-only the paths the commit stages under the
    scope, each filed for read_text() as its staged blob.
    """
    if _index is not None:
        return _index.files(scope)
    if scope.listed_by_git:
        listing = subprocess.check_output(
            [
                "git",
                "-C",
                str(root),
                "ls-files",
                "-z",
                "--cached",
                "--others",
                "--exclude-standard",
                *scope.dirs,
            ],
            text=True,
        )
        paths = [root / f for f in listing.split("\0") if f.endswith(scope.suffixes)]
    elif scope.sorted_by_path:
        paths = [p for d in scope.dirs for p in sorted((root / d).rglob("*")) if p.suffix in scope.suffixes]
    else:
        paths = [p for d in scope.dirs for s in scope.suffixes for p in (root / d).rglob("*" + s)]
    if scope.skip:
        paths = [p for p in paths if not in_skipped_dir(p, root, scope.skip)]
    return paths


def rel_path(root: Path, path: Path) -> str:
    """`path` relative to the root when it lies under it, else as given."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def read_text(path: Path, errors: str = "strict") -> Optional[str]:
    """The file's text, or None when it cannot be read or, decoded strictly, decoded.

    `errors="replace"` keeps a file that holds bytes UTF-8 cannot decode, each read
    as U+FFFD, instead of skipping the file.

    Under --staged-only the text is the path's staged blob, for a path scope_files()
    named. Any other path reads as missing: the working tree is never read.
    """
    if _index is not None:
        return _index.text.get(path)
    try:
        return path.read_text(encoding="utf-8", errors=errors)
    except (OSError, UnicodeDecodeError):
        return None


def read_lines(path: Path) -> Optional[list[str]]:
    text = read_text(path)
    return None if text is None else text.splitlines()


def code_lines(lines: list[str], strings: str = "keep") -> list[str]:
    """`lines` with every comment blanked and literals handled per `strings`, line for line.

    `strings` is blank_cxx's: "keep" leaves literals as written, "body" blanks what lies
    inside them and keeps the quotes, "blank" removes them quotes and all.
    """
    return blank_cxx("\n".join(lines), strings=strings).split("\n")


def marked(lines: list[str], line_no: int, marker: str, above: int) -> bool:
    """True when `marker` sits on line `line_no` (1-based) or up to `above` lines above it."""
    return marker in "\n".join(lines[max(0, line_no - 1 - above) : line_no])


# The identifier that ends where a search of the text ends.
IDENT_BEFORE = re.compile(r"([A-Za-z_]\w*)\s*$")


def report_sites(hits: list[str], max_allowed: int) -> int:
    """Print every `file:line: text` site when there are more than `max_allowed`; say whether it failed.

    For a rule whose message belongs to its caller: scripts/code_lint.sh names the
    rule under the sites this prints. Quiet when the rule passes.
    """
    if len(hits) <= max_allowed:
        return 0
    for hit in hits:
        print(hit)
    return 1


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
    files = files if files is not None else scope_files(root, SRC_SCOPE)
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
    files = files if files is not None else scope_files(root, SRC_SCOPE)
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
    hits = raw_print_state_sites(root, files if files is not None else scope_files(root, SRC_SCOPE))
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
XML_HIDE_SCOPE = Scope(("src", "plugins"), (".cpp", ".h"), listed_by_git=True)


def xml_create_then_hide(root: Path, files: Files, max_allowed: int) -> int:
    files = files if files is not None else scope_files(root, XML_HIDE_SCOPE)
    bad = []
    for f in files:
        text = read_text(f, errors="replace")
        # The assignment pattern backtracks at every identifier; a file that
        # never spells the call cannot match it.
        if text is None or "lv_xml_create" not in text:
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
                r"lv_obj_set_hidden\(\s*" + re.escape(var) + r"\s*,\s*true\s*\)"
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
# section-returns
# ====================================================================
# Catch2 finds sections lazily: each run of a test case enters one section it
# has not finished, and the SECTION macros the run passes on its way are what
# tell Catch the others exist. A `return` inside a section ends the run before
# the later sibling SECTIONs are reached, so Catch never learns of them and the
# case passes having run none of them. A return in a nested section hides the
# later siblings of every section around it the same way.
#
# A return in the last section of its scope, outside every section, or inside a
# lambda or local function body is fine. Siblings are sections with the same
# enclosing section (or, at the top, the same test case body), whatever plain
# blocks sit between. GIVEN/WHEN/THEN/AND_* and DYNAMIC_SECTION are sections too.
# Opt-out on the return's line or the line above: // SECTION_RETURN_OK: <reason>.
#
# Scope is tests/. scripts/code_lint.sh's lint_section_returns prints the message
# around the sites this rule reports.
SECTION_BLOCK = re.compile(
    r"\b(?:DYNAMIC_SECTION|SECTION|GIVEN|WHEN|THEN|AND_GIVEN|AND_WHEN|AND_THEN)\s*\("
)
RETURN_WORD = re.compile(r"\breturn\b")
# Keywords after whose `(` a `{` opens a control statement's block, not a body.
CONTROL_KEYWORDS = {"if", "while", "for", "switch", "catch"}
# What may sit between a lambda's or function's parameter list and its body.
LAMBDA_SPECIFIERS = re.compile(
    r"(?:\b(?:mutable|noexcept|const|override|final|constexpr)\b\s*|->\s*[\w:<>,\s*&]+?)*$"
)
SECTION_RETURN_OK = "SECTION_RETURN_OK"
SECTION_SCOPE = Scope(("tests",), (".cpp", ".h"), listed_by_git=True)


def section_bracket_pairs(text: str, open_ch: str, close_ch: str) -> dict[int, int]:
    """Map each bracket, open and close, to its partner."""
    pairs, stack = {}, []
    for m in re.finditer(re.escape(open_ch) + "|" + re.escape(close_ch), text):
        i = m.start()
        if text[i] == open_ch:
            stack.append(i)
        elif stack:
            j = stack.pop()
            pairs[i], pairs[j] = j, i
    return pairs


def section_is_function_body(text: str, brace: int, parens: dict[int, int]) -> bool:
    """A `{` opening a lambda or local function body: `return` leaves only it."""
    window = max(0, brace - 160)
    head = LAMBDA_SPECIFIERS.search(text[window:brace])
    k = (window + head.start() if head else brace) - 1
    while k >= 0 and text[k].isspace():
        k -= 1
    if k < 0:
        return False
    if text[k] == "]":
        return True  # [captures] { ... }
    if text[k] != ")" or k not in parens:
        return False
    p = parens[k]
    q = p - 1
    while q >= 0 and text[q].isspace():
        q -= 1
    if q >= 0 and text[q] == "]":
        return True  # [captures](args) { ... }
    m = IDENT_BEFORE.search(text[max(0, p - 80) : p])
    return m is not None and m.group(1) not in CONTROL_KEYWORDS


def section_return_sites(rel: str, raw: str) -> list[str]:
    """One `path:line: text` entry per unmarked return that hides a later sibling section."""
    text = blank_cxx(raw)
    braces = section_bracket_pairs(text, "{", "}")
    parens = section_bracket_pairs(text, "(", ")")

    # Each section as (body open, body close).
    sections = []
    for m in SECTION_BLOCK.finditer(text):
        close_paren = parens.get(m.end() - 1)
        if close_paren is None:
            continue
        k = close_paren + 1
        while k < len(text) and text[k].isspace():
            k += 1
        if k < len(text) and text[k] == "{" and k in braces:
            sections.append((k, braces[k]))  # else a call, not a section block
    if not sections:
        return []

    def enclosing_sections(pos: int) -> list[tuple[int, int]]:
        return [s for s in sections if s[0] < pos < s[1]]

    def outermost_block(pos: int) -> int:
        """Start of the outermost brace block around pos: the test case body."""
        start = pos
        for o, c in braces.items():
            if text[o] == "{" and o < pos < c and o < start:
                start = o
        return start

    parent = {}
    for s in sections:
        around = enclosing_sections(s[0])
        parent[s] = max(around)[0] if around else outermost_block(s[0])
    later_sibling = {
        s: any(o[0] > s[1] and parent[o] == parent[s] for o in sections) for s in sections
    }

    # Lambda and local function bodies: a return there leaves that function,
    # not the test case.
    functions = [
        (b, c)
        for b, c in braces.items()
        if b < c
        and text[b] == "{"
        and enclosing_sections(b)
        and (b, c) not in sections
        and section_is_function_body(text, b, parens)
    ]

    raw_lines = raw.split("\n")
    out = []
    for m in RETURN_WORD.finditer(text):
        pos = m.start()
        around = enclosing_sections(pos)
        if not any(later_sibling[s] for s in around):
            continue
        inner = max(around)
        if any(inner[0] < b < pos < c for b, c in functions):
            continue
        line = text.count("\n", 0, pos) + 1
        if marked(raw_lines, line, SECTION_RETURN_OK, above=1):
            continue
        out.append(f"{rel}:{line}: {raw_lines[line - 1].strip()}")
    return out


def section_returns(root: Path, files: Files, max_allowed: int) -> int:
    hits = []
    for f in files if files is not None else scope_files(root, SECTION_SCOPE):
        raw = read_text(f, errors="replace")
        if raw is None or "return" not in raw or not SECTION_BLOCK.search(raw):
            continue
        hits += section_return_sites(rel_path(root, f), raw)
    return report_sites(hits, max_allowed)


# ====================================================================
# init-list-ternary
# ====================================================================
# An initializer_list is a view of a temporary array that dies with the full
# expression that makes it. One picked by a ternary and then stored (a variable,
# a return value) is a view of freed stack, and whether that crashes depends on
# what reuses the slot, so no unit test catches it. Passed straight to a call it
# lives for that call and is fine. Opt-out: // INIT_LIST_TERNARY_OK: <reason>.
#
# Scope is src/, include/ and firmware/. scripts/code_lint.sh's
# lint_init_list_ternary prints the message around the sites this rule reports.
# The scan reads the code with literal bodies blanked and the quotes kept.
INIT_LIST_OPERAND = re.compile(r"(\?|(?<!:):(?!:))\s*std\s*::\s*initializer_list\s*<")
INIT_LIST_TERNARY_OK = "INIT_LIST_TERNARY_OK"
INIT_LIST_SCOPE = Scope(("src", "include", "firmware"), (".cpp", ".h", ".hpp"), listed_by_git=True)
CTOR_MEMBER_INIT = re.compile(r"\)\s*(?:noexcept\s*)?:(?!:)")
# Keywords that name no declarator: the identifier before a bracket is a name
# only when it is none of these.
NOT_A_DECLARATOR = {"if", "while", "for", "switch", "return", "else", "do", "try", "sizeof"}
STORE = re.compile(r"(?<![=!<>])=(?!=)|\breturn\b")


def init_list_named_opener(text: str, idx: int) -> Optional[re.Match]:
    """The identifier right before the bracket at idx, or None."""
    m = IDENT_BEFORE.search(text, 0, idx)
    if not m or m.group(1) in NOT_A_DECLARATOR:
        return None
    return m


def init_list_copied_into_owner(decl: str) -> bool:
    """A declaration whose own type is not an initializer_list (std::vector<T> v)
    copies the elements while the temporary array still lives."""
    decl = decl.strip()
    if "initializer_list" in decl or re.search(r"\bauto\b", decl):
        return False
    return re.search(r"[\w>]\s*[&*]?\s+[A-Za-z_]\w*\s*$", decl) is not None


def init_list_member_initializer(text: str, start: int, idx: int) -> bool:
    """`name(` inside a constructor's member-initializer list."""
    m = init_list_named_opener(text, idx)
    if m is None:
        return False
    before = text[start : m.start()].rstrip()
    return (
        before.endswith((":", ","))
        and not before.endswith("::")
        and CTOR_MEMBER_INIT.search(before) is not None
    )


def init_list_ternary_sites(rel: str, raw: str) -> list[str]:
    """One `path:line: text` entry per ternary initializer_list that is stored past its expression."""
    text = blank_cxx(raw, strings="body")
    raw_lines = raw.split("\n")
    out = []
    seen = set()
    for m in INIT_LIST_OPERAND.finditer(text):
        # The statement holding the ternary starts after the last ; or unmatched
        # {, stepping over the braces of the lists themselves.
        start, depth = m.start(), 0
        while start > 0:
            ch = text[start - 1]
            if ch == "}":
                depth += 1
            elif ch == "{":
                if depth == 0:
                    break
                depth -= 1
            elif ch == ";" and depth == 0:
                break
            start -= 1
        if start in seen:
            continue
        seen.add(start)
        stmt = text[start:m.start()]
        opens = []
        for i, ch in enumerate(stmt):
            if ch == "(":
                opens.append(start + i)
            elif ch == ")" and opens:
                opens.pop()
        if opens:
            # Inside parentheses: a call argument lives for that call, but a
            # constructor's member initializer `: keys_(c ? ...)` stores it.
            if not init_list_member_initializer(text, start, opens[-1]):
                continue
        elif (
            start > 0
            and text[start - 1] == "{"
            and init_list_named_opener(text, start - 1) is not None
        ):
            # `name{...}`: a declaration whose own type owns the elements copies them.
            if init_list_copied_into_owner(text[text.rfind(";", 0, start) + 1 : start - 1]):
                continue
        else:
            store = STORE.search(stmt)
            if not store:
                continue
            if store.group(0) == "=" and init_list_copied_into_owner(stmt[:store.start()]):
                continue
        line = text.count("\n", 0, m.start()) + 1
        first = text.count("\n", 0, start) + 1
        # The opt-out counts anywhere from the statement's first line through
        # the second line below the match.
        if marked(raw_lines, line + 2, INIT_LIST_TERNARY_OK, above=line + 2 - first):
            continue
        out.append(f"{rel}:{line}: {raw_lines[line - 1].strip()}")
    return out


def init_list_ternary(root: Path, files: Files, max_allowed: int) -> int:
    hits = []
    scope = files if files is not None else scope_files(root, INIT_LIST_SCOPE)
    for f in scope:
        raw = read_text(f, errors="replace")
        # The blanking keeps every code character in place, so INIT_LIST_OPERAND
        # cannot match in a file that never spells initializer_list; skip it
        # before the blanking.
        if raw is None or "initializer_list" not in raw:
            continue
        hits += init_list_ternary_sites(rel_path(root, f), raw)
    return report_sites(hits, max_allowed)


# ====================================================================
# getenv-setenv
# ====================================================================
# setenv() may reallocate the environ block and the strings it points at, so a
# `const char*` captured from getenv() and dereferenced after any later
# setenv()/unsetenv() reads whatever bytes live at that address now: a
# corrupted variable inherited by every std::system() child the test spawns
# (prestonbrown/helixscreen#1537). Copy the value into a std::string at capture
# (helix::ScopedEnv in tests/test_helpers/scoped_env.h does this, and its
# (name, value) constructor owns the set too) and restore from the copy.
#
# This is a spelling-level scanner, not a C++ parser, and it is NOT exhaustive.
# It sees char and auto* declarations (west-const, east-const, pointer-const,
# wrapped after the name) and bare `auto` initialized by the getenv() call
# itself, retires each capture at its closing brace (a capture declared in a
# control-statement header whose body opens on the same line retires with that
# statement), and blanks comments, literal contents and qualified names
# (`x.name`, `ptr->name`, `ns::name`, `name::member`) before matching. A read
# is stale only when a setenv()/unsetenv() call has COMPLETED - its closing
# paren sits between the capture and the read - so an argument of a call still
# open on the read's line stays quiet, wrapped or not, while a read after a
# call that closed earlier on the same line flags. Known-uncovered spellings:
# assignment-form capture (`const char* p; p = getenv(..)`), direct-init
# (`const char* p(getenv(..))`), the second declarator of a comma declaration
# (`const char* a = getenv(..), *b = getenv(..)`), a declaration wrapped
# BEFORE the name (`const char*\np = getenv(..)`), and bare `auto` with the
# getenv() on a continuation line. The 400-line backstop window exists only so
# a pathological single function cannot pin a name forever. Residual
# false-positive shapes: a same-named local in a NESTED scope of the same
# function, and a control-statement header that WRAPS across lines (its
# capture retires only at the function's closing brace).
#
# Scope is every first-party C and C++ root, with no opt-out marker.
# scripts/code_lint.sh's lint_getenv_setenv passes the roots; this rule walks a
# directory it is given.
GETENV_ROOTS = (
    "tests",
    "src",
    "include",
    "tools",
    "firmware",
    "android",
    "server",
    "moonraker-plugin",
    "ui_xml",
)
GETENV_SUFFIXES = (".cpp", ".h", ".cc", ".hpp", ".c")
GETENV_SCOPE = Scope(GETENV_ROOTS, GETENV_SUFFIXES, sorted_by_path=True)

# West-const, east-const and pointer-const spellings of a char pointer's
# declaration; (?!=) keeps == comparisons out.
GETENV_DECL = re.compile(
    r"(?:const\s+)?char\s*(?:const\s*)?\*\s*(?:const\s*)?" r"([A-Za-z_]\w*)\s*=(?!=)"
)
# `auto*` deduces the same raw pointer `char*` spells.
GETENV_DECL_AUTO_PTR = re.compile(
    r"(?:const\s+)?auto\s*\*\s*(?:const\s*)?" r"([A-Za-z_]\w*)\s*=(?!=)"
)
# Bare `auto` is tracked only when the initializer IS the getenv() call: a
# value captured straight into owned storage must not flag the copy's users.
GETENV_DECL_AUTO_DIRECT = re.compile(
    r"\bauto\s+([A-Za-z_]\w*)\s*=\s*" r"(?:std\s*::\s*)?getenv\s*\("
)
GETENV_CALL = re.compile(r"\bgetenv\s*\(")
SETENV_CALL = re.compile(r"\b(?:setenv|unsetenv)\s*\(")
GETENV_WINDOW = 400  # lines a capture stays live before the backstop retires it
GETENV_WRAP_MAX = 5  # continuation lines scanned for getenv() in a wrapped initializer


def getenv_call_completions(text: str) -> list[int]:
    """Offsets of every setenv()/unsetenv() call's closing paren, wrapped
    calls included: a read is stale only once the call has completed."""
    done = []
    for m in SETENV_CALL.finditer(text):
        j = m.end()
        paren = 1
        while j < len(text) and paren:
            if text[j] == "(":
                paren += 1
            elif text[j] == ")":
                paren -= 1
            j += 1
        if paren == 0:
            done.append(j - 1)
    return sorted(done)


def getenv_line_depths(line: str, start: int) -> tuple[list[int], list[int], int]:
    """Depth at each column, the minimum depth over each suffix, and the
    depth at line end, for scope retirement."""
    at = []
    run = start
    for c in line:
        at.append(run)
        if c == "{":
            run += 1
        elif c == "}":
            run -= 1
    at.append(run)
    suffix_min = list(at)
    for c in range(len(at) - 2, -1, -1):
        suffix_min[c] = min(suffix_min[c], suffix_min[c + 1])
    return at, suffix_min, run


def getenv_register_capture(m, line, i, lines, active, pos, depth_at, retire_col) -> None:
    initializer = line[m.end() :]
    # '{' ends an initializer the same way ';' does: a parameter default like
    # `const char* value = nullptr) : m_(m) {` must not swallow the
    # constructor body's getenv() as its own
    terminated = (";" in initializer) or ("{" in initializer)
    if not terminated:
        for cont in lines[i + 1 : i + 1 + GETENV_WRAP_MAX]:
            stop = re.search(r"[;{]", cont)
            if stop:
                initializer += " " + cont[: stop.start() + 1]
                break
            initializer += " " + cont
    if GETENV_CALL.search(initializer):
        # a redeclaration of the same name starts a new capture
        active[m.group(1)] = [i, retire_col, pos, depth_at]


def getenv_setenv_sites(rel: str, lines: list[str]) -> list[str]:
    """One `path:line: text` entry per getenv() capture that is first read after a setenv()."""
    text = "\n".join(lines)
    completions = getenv_call_completions(text)
    starts = []
    o = 0
    for ln in lines:
        starts.append(o)
        o += len(ln) + 1
    out = []
    depth = 0  # brace depth; a capture dies when its scope closes
    active = {}  # name -> [capture line, retire col, offset, depth]
    for i, line in enumerate(lines):
        at, smin, depth = getenv_line_depths(line, depth)
        for rx in (GETENV_DECL, GETENV_DECL_AUTO_PTR):
            for m in rx.finditer(line):
                prefix = line[: m.start(1)]
                # A capture declared in a for/if/while header whose body
                # opens on the same line is scoped to that statement: one
                # level deeper, and it retires from the body's opening
                # brace rather than from the name.
                in_header = (
                    prefix.count("(") - prefix.count(")") > 0
                    and re.search(r"\b(?:for|if|while)\b[^;{}]*$", prefix)
                    and "{" in line[m.end(1) :]
                )
                retire_col = (
                    line.rfind("{", m.end(1)) + 1 if in_header else m.end(1)
                )
                getenv_register_capture(
                    m,
                    line,
                    i,
                    lines,
                    active,
                    starts[i] + m.start(1),
                    at[m.start(1)] + (1 if in_header else 0),
                    retire_col,
                )
        for m in GETENV_DECL_AUTO_DIRECT.finditer(line):
            active[m.group(1)] = [i, m.end(1), starts[i] + m.start(1), at[m.start(1)]]
        # A read is stale only when a setenv()/unsetenv() completed at an
        # offset between the capture and the read: an argument of a call
        # still open at the read's position is evaluated before that call
        # runs, while a read after a closing paren earlier on the same
        # line is post-setenv and flags.
        for name, state in list(active.items()):
            if i < state[0] or i > state[0] + GETENV_WINDOW:
                continue
            # cfg.name, ptr->name, ns::name and name::member are different
            # entities, not uses of the captured local; blanked same-length
            # so offsets stay valid
            quiet = re.sub(
                r"(?:\.|->|::)\s*" + re.escape(name) + r"\b" + r"|\b" + re.escape(name) + r"\s*::",
                lambda mm: " " * len(mm.group(0)),
                line,
            )
            for um in re.finditer(r"\b" + re.escape(name) + r"\b", quiet):
                k = bisect.bisect_right(completions, state[2])
                if k < len(completions) and completions[k] < starts[i] + um.start():
                    out.append(
                        f"{rel}:{state[0] + 1}: '{name}' from getenv() is read at "
                        f"line {i + 1}, after a setenv() between the two; copy the "
                        f"value into a std::string at capture (helix::ScopedEnv)"
                    )
                    del active[name]
                    break
        # The running depth dipping below the depth at capture closes the
        # capture's scope, so one inline method's local never reaches the
        # next method's same-named local.
        for name, state in list(active.items()):
            if i == state[0]:
                if smin[state[1]] < state[3]:
                    del active[name]
            elif smin[0] < state[3]:
                del active[name]
    return out


def getenv_missing_roots(root: Path) -> list[str]:
    """The scan roots that hold nothing to scan: absent from the working tree, or
    under --staged-only from the index."""
    if _index is not None:
        listing = subprocess.run(
            ["git", "-C", str(_index.top), "ls-files", "-z"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
        held = set()
        for entry in listing.split("\0"):
            try:
                held.add((_index.top / entry).relative_to(_index.root).parts[0])
            except (ValueError, IndexError):
                pass  # an index entry outside the root, or the empty entry after the last NUL
        return [d for d in GETENV_ROOTS if d not in held]
    return [d for d in GETENV_ROOTS if not (root / d).exists()]


def getenv_setenv(root: Path, files: Files, max_allowed: int) -> int:
    if files is not None:
        paths, missing = [], []
        for given in files:
            if not given.exists():
                # A root that is not there would otherwise scan nothing and pass.
                missing.append(f"scan root does not exist: {rel_path(root, given)}")
            else:
                paths += sorted(given.rglob("*")) if given.is_dir() else [given]
    else:
        paths = scope_files(root, GETENV_SCOPE)
        missing = [f"scan root does not exist: {rel}" for rel in getenv_missing_roots(root)]

    hits = []
    for path in paths:
        if path.suffix not in GETENV_SUFFIXES:
            continue
        raw = read_text(path, errors="replace")
        # A read flags only after a setenv()/unsetenv() call, and a file that
        # never spells setenv has none: skip it before the character-level
        # blanking that dominates the scan's cost.
        if raw is None or "setenv" not in raw:
            continue
        lines = code_lines(raw.splitlines(), strings="body")
        hits += getenv_setenv_sites(rel_path(root, path), lines)

    # A missing root fails whatever --max-allowed says.
    for message in missing:
        print(message)
    return report_sites(hits, max_allowed) | (1 if missing else 0)


# ====================================================================
# xml-create-registered
# ====================================================================
# lv_xml_create(parent, "name", attrs) on an unknown name logs an error and
# returns NULL, so the row or card it was meant to build is silently missing.
#
# Registrable means one of:
#   - ui_xml/<name>.xml or ui_xml/components/<name>.xml exists (registered on
#     first use);
#   - a "<name>.xml" string literal in src/ (a path passed to
#     lv_xml_register_component_from_file());
#   - lv_xml_register_widget("<name>", ...) in src/;
#   - an lv_* name (LVGL's built-in widgets).
#
# Scope is src/, on disk whether tracked or not, with no opt-out marker. FILE
# arguments are the files searched for the calls; the names that can be
# registered still come from every source under src/, because a component is
# registered where it is filed, not only where it is created. --staged-only
# does not apply for the same reason: the staged blobs alone cannot say which
# names the tree registers.
# qc_xml_create_registered (scripts/qc/xml_create_registered.sh) runs it.
XML_CREATE_CALL = re.compile(r'lv_xml_create\s*\([^;]*?,\s*"([A-Za-z_][A-Za-z0-9_]*)"', re.S)
XML_FILE_LITERAL = re.compile(r'"(?:[^"]*/)?([A-Za-z_][A-Za-z0-9_]*)\.xml"')
XML_WIDGET_REGISTRATION = re.compile(r'lv_xml_register_widget\(\s*"([A-Za-z_][A-Za-z0-9_]*)"')
XML_CREATE_SCOPE = Scope(("src",), (".cpp", ".h", ".c"))


def xml_create_registered(root: Path, files: Files, max_allowed: int) -> int:
    sources = scope_files(root, XML_CREATE_SCOPE)
    texts: dict[Path, Optional[str]] = {p: read_text(p, errors="replace") for p in sources}
    for f in files or []:
        texts.setdefault(f, read_text(f, errors="replace"))

    registered = set()
    for d in ("ui_xml", "ui_xml/components"):
        registered.update(p.stem for p in (root / d).glob("*.xml"))
    for path in sources:
        text = texts[path]
        if text is not None:
            registered.update(XML_FILE_LITERAL.findall(text))
            registered.update(XML_WIDGET_REGISTRATION.findall(text))

    created = []
    for path in sources if files is None else files:
        text = texts[path]
        if text is None:
            continue
        for m in XML_CREATE_CALL.finditer(text):
            line = text.count("\n", 0, m.start(1)) + 1
            created.append((rel_path(root, path), line, m.group(1)))

    unregistered = [
        c for c in sorted(created) if c[2] not in registered and not c[2].startswith("lv_")
    ]
    if len(unregistered) > max_allowed:
        print(f"❌ {len(unregistered)} lv_xml_create() call(s) name a component with no XML file:")
        for path, line, name in unregistered:
            print(f'   {path}:{line}: "{name}" (no ui_xml/{name}.xml or ui_xml/components/{name}.xml)')
        return 1

    if unregistered:
        print(f"✅ XML create names: {len(unregistered)} <= {max_allowed}")
    else:
        print("✅ XML create names: every lv_xml_create() component in src/ can register")
    return 0


# ====================================================================
# lvgl-subject-api
# ====================================================================
# An LVGL subject is owned by helix::Subject (include/core/subject.h), never held
# by value and never set up through the subject API LVGL 9.6 deprecates. A subject
# held by value is torn down with lv_subject_deinit() and re-initialized in place;
# helix::Subject owns an lv_subject_create() node whose address survives reset()
# and every re-init. docs/devel/THREADING.md § "Subject ownership".
#
# Two shapes are counted, in code only (comments and string literals blanked):
#
#   call      lv_subject_init_{int,float,string,pointer,color,group}, lv_subject_deinit,
#             lv_subject_copy_string
#   storage   an lv_subject_t held by value: each declarator of a variable, member or
#             array declaration (`lv_subject_t a, b;` is two), lv_subject_t or lv_subject_t[]
#             as any template argument (array, vector, map, optional, pair, shared_ptr,
#             unique_ptr, make_unique, ...), new lv_subject_t. lv_subject_t* and
#             lv_subject_t& are not storage.
#
# Any site fails. A line carrying `SUBJECT_API_OK: <reason>`, or directly below a comment
# line carrying it, is skipped: for a test that pins LVGL's own subject API.
#
# Scope is src/, include/, tests/ and firmware/, outside build/ and managed_components/.
# qc_lvgl_subject_api (scripts/qc/lvgl_subject_api.sh) runs it, on the staged blobs
# at commit and over the tree at push, and prints this rule's message around the
# sites it reports. A line holding several sites is listed once; each one counts.
SUBJECT_SCOPE = Scope(
    ("src", "include", "tests", "firmware"),
    (".c", ".cc", ".cpp", ".h", ".hpp", ".inc"),
    skip=("build", "managed_components"),
    sorted_by_path=True,
)
SUBJECT_API_OK = "SUBJECT_API_OK:"

SUBJECT_CALL = re.compile(
    r"\b(lv_subject_init_(?:int|float|string|pointer|color|group)|lv_subject_deinit|"
    r"lv_subject_copy_string)\s*\("
)
# lv_subject_t by value as a template argument: std::array/vector/deque/list/map/optional/
# pair/shared_ptr/unique_ptr/make_unique and the like, including the T[] forms.
SUBJECT_TEMPLATE_ARG = re.compile(r"[<,]\s*(?:const\s+)?lv_subject_t\s*(?:\[\s*\])?\s*(?=[,>])")
SUBJECT_NEW = re.compile(r"\bnew\s+lv_subject_t\b")
# One declarator after the type; the declarator list continues with `, name` entries.
SUBJECT_DECLARATOR = r"[A-Za-z_]\w*\s*(?:\[[^\]]*\]\s*)*(?:\{[^}]*\}\s*)?(?:=\s*\{[^}]*\}\s*)?"
SUBJECT_DECL = re.compile(
    r"\blv_subject_t\s+(?!\*|&)("
    + SUBJECT_DECLARATOR
    + r"(?:,\s*(?![A-Za-z_]\w*\s+[A-Za-z_*&])"
    + SUBJECT_DECLARATOR
    + r")*)(?=[;,=)])"
)


def subject_commas_inside(declarators: str) -> int:
    """Commas inside [] or {} of a declarator list, which separate no declarators."""
    depth = 0
    inner = 0
    for c in declarators:
        if c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
        elif c == "," and depth > 0:
            inner += 1
    return inner


def subject_storage_sites(code: str) -> int:
    """By-value lv_subject_t sites on one line: each declarator, template argument and new."""
    n = len(SUBJECT_TEMPLATE_ARG.findall(code)) + len(SUBJECT_NEW.findall(code))
    for m in SUBJECT_DECL.finditer(code):
        n += m.group(1).count(",") + 1 - subject_commas_inside(m.group(1))
    return n


def lvgl_subject_sites(rel: str, text: str) -> list[str]:
    """One entry per deprecated call and per by-value lv_subject_t, a line listed once per shape.

    Each entry is `path:line: call` or `path:line: storage` over the source line;
    a line holding several sites of one shape yields that many equal entries.
    """
    hits = []
    raw_lines = text.splitlines()
    code_lines = blank_cxx(text).splitlines()
    prev_ok = False
    for i, raw in enumerate(raw_lines):
        code = code_lines[i] if i < len(code_lines) else ""
        opt_out = SUBJECT_API_OK in raw or prev_ok
        stripped = raw.strip()
        prev_ok = stripped.startswith(("//", "*", "/*")) and SUBJECT_API_OK in raw
        if opt_out:
            continue
        sites_on_line = (
            ("call", len(SUBJECT_CALL.findall(code))),
            ("storage", subject_storage_sites(code)),
        )
        for shape, n in sites_on_line:
            hits += [f"  {rel}:{i + 1}: {shape}\n      {stripped[:110]}"] * n
    return hits


def lvgl_subject_api(root: Path, files: Files, max_allowed: int) -> int:
    hits = []
    for f in files if files is not None else scope_files(root, SUBJECT_SCOPE):
        text = read_text(f, errors="replace")
        # Every shape names lv_subject; a file that never spells it needs no blanking.
        if text is None or "lv_subject" not in text:
            continue
        hits += lvgl_subject_sites(rel_path(root, f), text)

    for hit in dict.fromkeys(hits):
        print(hit)
    return 1 if len(hits) > max_allowed else 0


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
    # Why --staged-only cannot judge the rule, for one that reads more than its
    # own scope to decide. Empty when the mode applies.
    needs_whole_tree: str = ""


# A pinned rule under --staged-only.
PINNED_WHOLE_TREE = "it checks named files whole, and its wrapper runs it whole at commit"

RULES = {
    "print-state-cast": Rule(print_state_cast),
    "json-dump-utf8": Rule(json_dump_utf8),
    "raw-print-state": Rule(raw_print_state),
    "xml-create-then-hide": Rule(xml_create_then_hide),
    "section-returns": Rule(section_returns),
    "init-list-ternary": Rule(init_list_ternary),
    "getenv-setenv": Rule(getenv_setenv),
    "xml-create-registered": Rule(
        xml_create_registered,
        needs_whole_tree=(
            "it collects the names a created component can register under from every source "
            "under src/, which the staged blobs alone cannot answer for"
        ),
    ),
    "lvgl-subject-api": Rule(lvgl_subject_api),
    "touch-rotation-source": Rule(touch_rotation_source, pinned=True),
    "grid-metrics-single-source": Rule(grid_metrics_single_source, pinned=True),
    "filament-ops-neutral": Rule(filament_ops_neutral, pinned=True),
}


def usage_error(message: str) -> int:
    """Exit 2 on one line: the run asked for something that cannot be judged."""
    print(message, file=sys.stderr)
    return 2


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", type=Path, default=SCRIPT_ROOT, help="repository root")
    ap.add_argument(
        "--rule", dest="rules", action="append", choices=list(RULES), help="run only this rule"
    )
    ap.add_argument("--max-allowed", type=int, default=0, help="sites a file-set rule may hold")
    ap.add_argument(
        "--staged-only",
        action="store_true",
        help="a file-set rule's scope is the paths the commit stages, read as their staged blobs",
    )
    ap.add_argument("files", nargs="*", type=Path, help="files for a file-set rule")
    args = ap.parse_args(argv)

    names = args.rules or list(RULES)
    if args.staged_only and args.files:
        return usage_error(
            "--staged-only and FILE arguments do not combine: FILE arguments are read from the "
            "working tree, --staged-only reads only the index"
        )
    if args.files:
        pinned = [n for n in names if RULES[n].pinned]
        if pinned:
            ap.error(f"FILE arguments do not apply to pinned rule(s): {', '.join(pinned)}")
    if args.staged_only and not use_staged_blobs(args.root):
        return usage_error(
            f"--staged-only reads the index, and --root is not inside a git repository: {args.root}"
        )

    status = 0
    refused = False
    for name in names:
        rule = RULES[name]
        if args.staged_only and (rule.pinned or rule.needs_whole_tree):
            why = PINNED_WHOLE_TREE if rule.pinned else rule.needs_whole_tree
            usage_error(f"--staged-only does not apply to {name}: {why}")
            refused = True
        elif args.staged_only:
            # A staged run prints what fails. A rule passes quietly, with nothing
            # staged under its scope or with its staged files clean.
            with redirect_stdout(io.StringIO()) as out:
                code = rule.check(args.root, None, args.max_allowed)
            if code:
                sys.stdout.write(out.getvalue())
            status |= code
        else:
            status |= rule.check(args.root, args.files or None, args.max_allowed)
    return 2 if refused else status


if __name__ == "__main__":
    sys.exit(main())
