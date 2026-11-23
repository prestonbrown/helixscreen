#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lint gate: a head is named and mounted through the head identity.

On a printer whose G-code tool numbers name filament lanes (AFC, Happy Hare),
`T3` is a lane, not a nozzle: labeling a head with it shows the wrong number,
and sending `T<n>` for a head choice loads a lane and swaps filament. The
single sources are:

  who a head is   helix::head_identity(), ToolState::head_identity(),
                  head_identity_for_tool(), head_identity_for_extruder()
  how it reads    head_*_label() and active_head_*_label() (display_numbering.h)
  nozzle names    ui::temperature::nozzle_display_name()
  mounting it     AmsBackend::head_select_gcode(tool)

Four shapes are flagged in src/ui and src/printer:

  display-label    A read of ToolInfo::display_label. It is a lane's label on
                   a lane rig; head text comes from the head-label functions.
  extruder-name    A read of ExtruderInfo::display_name outside the arguments
                   of nozzle_display_name(). It is "Nozzle T<i>" in G-code
                   style, a tool number for an extruder.
  tool-label       tool_label(), active_tool_label(), active_tool_name_label()
                   or active_tool_badge_label() fed a head: an argument that
                   names a head, representative_tool(), active_tool_index() or
                   display_label.
  head-command     A `T<n>`, SELECT_TOOL or ACTIVATE_EXTRUDER command built in
                   a statement that sends or stores G-code, outside the body of
                   a head_select_gcode() implementation.

Not flagged: comments, string-literal text (except to recognize a command),
anything inside an spdlog:: call, and the body of head_select_gcode().

A lane-side use (a lane badge, the Assigned Tool, a slot label) and a backend
whose tools ARE its heads (toolchanger, Snapmaker, INDX) annotate the line:

    helix::ui::tool_label(lane.tool); // HEAD_LABEL_OK: <why this is not a head>

The token may sit on any line of the statement, or in the `//` comment block
directly above it.

Usage:
  scripts/check_head_labels.py                 # every file under src/ui, src/printer
  scripts/check_head_labels.py --staged-only   # staged blobs only (pre-commit)
  scripts/check_head_labels.py FILE...         # the named files
"""
from __future__ import annotations

import argparse
import bisect
import os
import re
import subprocess
import sys
from pathlib import Path

from source_strip import blank_cxx
from staged_content import read_index_blobs, staged_paths

SCAN_DIRS = ('src/ui', 'src/printer')
SCAN_EXTS = ('.cpp', '.cc', '.h', '.hpp')
OPT_OUT = 'HEAD_LABEL_OK'

LABEL_FUNCS = ('tool_label', 'active_tool_label', 'active_tool_name_label',
               'active_tool_badge_label')
LABEL_CALL_RE = re.compile(r'\b(?:' + '|'.join(LABEL_FUNCS) + r')\s*\(')
# An argument that names a head, or a tool number taken from head selection.
HEADY_ARG_RE = re.compile(
    r'\bheads?\b|\bhead_[A-Za-z0-9_]+|\b[A-Za-z0-9_]+_head\b|'
    r'\brepresentative_tool\s*\(|\bactive_tool_index\s*\(|\bdisplay_label\b')

DISPLAY_LABEL_RE = re.compile(r'(?:\.|->)\s*display_label\b(?!\s*=[^=])')

NOZZLE_NAME_CALL_RE = re.compile(r'\bnozzle_display_name\s*\(')
DISPLAY_NAME_RE = re.compile(r'(?:\.|->)\s*display_name\b(?!\s*=[^=])')
# Receivers of an ExtruderInfo: a declared variable, the second half of a
# structured binding over extruders, or the map's value reached in one expression.
EXT_DECL_RE = re.compile(r'\bExtruderInfo\s*[&*]?\s*(?:const\s*)?([A-Za-z_]\w*)')
EXT_BIND_RE = re.compile(
    r'(?:auto|const\s+auto)\s*&?\s*\[\s*\w+\s*,\s*(\w+)\s*\]\s*:\s*[^)]*\bextruders\w*')
EXT_DIRECT_RE = re.compile(
    r'\bextruders\w*\s*(?:\(\s*\))?\s*(?:\[[^\]]*\]|\.at\([^)]*\))\s*\.\s*display_name\b')

# A command literal: bare T<n> (a format or a concatenation), or the select
# commands. The literal's text is read from the comment-only blanked view.
CMD_LITERAL_RE = re.compile(
    r'"(?:T(?:\{\d*\}|%d)|(?:AFC_)?SELECT_TOOL\b[^"]*|ACTIVATE_EXTRUDER\b[^"]*)"|'
    r'"T"\s*\+|'
    r'"(?:AFC_)?SELECT_TOOL\b|"ACTIVATE_EXTRUDER\b')
# The statement sends or keeps G-code (a log or message that mentions T<n> does not).
GCODE_SINK_RE = re.compile(
    r'\w*gcode\w*\s*(?:\(|=(?!=))|\b(?:cmd|command)\w*\s*(?:=(?!=)|\()|'
    r'\bensure_homed_then\s*\(|\bline\s*\(|\bscript\s*(?:=(?!=)|\()', re.I)
SELECT_LITERAL_RE = re.compile(r'SELECT_TOOL|ACTIVATE_EXTRUDER')
# tool_label() spelled into a command.
TOOL_LABEL_CMD_RE = re.compile(r'\btool_label\s*\(')

HEAD_SELECT_DEF_RE = re.compile(
    r'\bhead_select_gcode\s*\([^;{)]*\)\s*(?:const)?\s*(?:override)?\s*(?:final)?\s*\{')
SPDLOG_RE = re.compile(r'\bspdlog::\w+\s*(?:<[^>]*>)?\s*\(')

WHY = {
    'display-label': 'ToolInfo::display_label is a lane\'s label on a lane rig',
    'extruder-name': 'ExtruderInfo::display_name names a nozzle by tool number',
    'tool-label': 'a head labeled by a G-code tool number',
    'head-command': 'a head choice sent as a raw tool command',
}

ADVICE = """
A head is who head_identity()/head_identity_for_tool()/head_identity_for_extruder()
say it is, spelled by head_label_in_form()/active_head_name_label()/
active_head_badge_label() (include/display_numbering.h), or by
ui::temperature::nozzle_display_name() on temperature surfaces. A head is
mounted with AmsBackend::head_select_gcode(tool), never a raw T<n>.
A lane-side use, or a backend whose tools are its heads, annotates the line:
    // HEAD_LABEL_OK: <why this is not a head>"""


def _match_paren(s: str, open_idx: int) -> int:
    """Index of the `)` closing the `(` at open_idx, or -1."""
    depth = 0
    for i in range(open_idx, min(len(s), open_idx + 6000)):
        c = s[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
    return -1


def _match_brace(s: str, open_idx: int) -> int:
    depth = 0
    for i in range(open_idx, len(s)):
        c = s[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i
    return len(s) - 1


def _mask(chars: list[str], start: int, end: int) -> None:
    for k in range(start, end):
        if chars[k] != '\n':
            chars[k] = ' '


def scan_source(path: str, src: str) -> list[tuple[str, int, str, str]]:
    """[(path, line, kind, source line)] for each unannotated offender."""
    if not re.search(r'display_label|display_name|tool_label|tool_name_label|tool_badge_label|'
                     r'"T"|"T[{%]|SELECT_TOOL|ACTIVATE_EXTRUDER', src):
        return []

    clean_chars = list(blank_cxx(src))
    keep_chars = list(blank_cxx(src, strings='keep'))
    clean = ''.join(clean_chars)

    # spdlog calls and head_select_gcode bodies are out of scope in both views.
    for m in SPDLOG_RE.finditer(clean):
        end = _match_paren(clean, m.end() - 1)
        if end > 0:
            _mask(clean_chars, m.start(), end + 1)
            _mask(keep_chars, m.start(), end + 1)
    clean = ''.join(clean_chars)
    for m in HEAD_SELECT_DEF_RE.finditer(clean):
        end = _match_brace(clean, m.end() - 1)
        _mask(clean_chars, m.start(), end + 1)
        _mask(keep_chars, m.start(), end + 1)
    clean = ''.join(clean_chars)
    keep = ''.join(keep_chars)

    lines = src.splitlines()
    starts = [0]
    for i, ch in enumerate(src):
        if ch == '\n':
            starts.append(i + 1)

    def line_of(off: int) -> int:
        return bisect.bisect_right(starts, off)

    def stmt_bounds(off: int) -> tuple[int, int]:
        lo = max(clean.rfind(';', 0, off), clean.rfind('{', 0, off), clean.rfind('}', 0, off)) + 1
        hi = clean.find(';', off)
        return lo, (hi + 1 if hi >= 0 else len(clean))

    def opted_out(first: int, last: int) -> bool:
        for ln in range(first, last + 1):
            if 0 < ln <= len(lines) and OPT_OUT in lines[ln - 1]:
                return True
        # A `//` block directly above, which clang-format may wrap onto several lines.
        ln = first - 1
        while ln >= 1 and lines[ln - 1].lstrip().startswith('//'):
            if OPT_OUT in lines[ln - 1]:
                return True
            ln -= 1
        return False

    found: dict[tuple[int, str], str] = {}

    def report(off: int, kind: str) -> None:
        lo, hi = stmt_bounds(off)
        first = line_of(lo + len(clean[lo:hi]) - len(clean[lo:hi].lstrip()))
        last = line_of(max(lo, hi - 1))
        ln = line_of(off)
        if opted_out(min(first, ln), max(last, ln)):
            return
        found.setdefault((ln, kind), lines[ln - 1].strip() if ln <= len(lines) else '')

    for m in DISPLAY_LABEL_RE.finditer(clean):
        report(m.start(), 'display-label')

    nozzle_spans = []
    for m in NOZZLE_NAME_CALL_RE.finditer(clean):
        end = _match_paren(clean, m.end() - 1)
        if end > 0:
            nozzle_spans.append((m.start(), end))

    def in_nozzle_call(off: int) -> bool:
        return any(a <= off <= b for a, b in nozzle_spans)

    receivers = set(EXT_DECL_RE.findall(clean)) | set(EXT_BIND_RE.findall(clean))
    recv_re = (re.compile(r'\b(?:' + '|'.join(map(re.escape, sorted(receivers))) +
                          r')\s*(?:\.|->)\s*display_name\b(?!\s*=[^=])')
               if receivers else None)
    for rx in (recv_re, EXT_DIRECT_RE):
        if rx is None:
            continue
        for m in rx.finditer(clean):
            if not in_nozzle_call(m.start()):
                report(m.start(), 'extruder-name')

    for m in LABEL_CALL_RE.finditer(clean):
        end = _match_paren(clean, m.end() - 1)
        if end < 0:
            continue
        args = clean[m.end():end]
        if HEADY_ARG_RE.search(args):
            report(m.start(), 'tool-label')

    for m in CMD_LITERAL_RE.finditer(keep):
        lo, hi = stmt_bounds(m.start())
        if SELECT_LITERAL_RE.search(m.group(0)) or GCODE_SINK_RE.search(clean[lo:hi]):
            report(m.start(), 'head-command')
    for m in TOOL_LABEL_CMD_RE.finditer(clean):
        lo, hi = stmt_bounds(m.start())
        if re.search(r'(?:\w*gcode\w*|cmd\w*|command\w*)\s*(?:=(?!=)|\()[^;]*$',
                     clean[lo:m.start()], re.I):
            report(m.start(), 'head-command')

    return [(path, ln, kind, text) for (ln, kind), text in sorted(found.items())]


def scan_file(path: str) -> list[tuple[str, int, str, str]]:
    try:
        src = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    return scan_source(path, src)


def staged_targets() -> list[str]:
    return [p for p in staged_paths(suffixes=SCAN_EXTS) if p.startswith(SCAN_DIRS)]


def tree_targets() -> list[str]:
    out = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '--',
                          *SCAN_DIRS], capture_output=True, text=True, check=False).stdout
    return [p for p in out.splitlines() if p.endswith(SCAN_EXTS)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--staged-only', action='store_true',
                    help='Scan the staged blobs of src/ui and src/printer sources')
    ap.add_argument('paths', nargs='*', help='Files to scan (default: the whole scope)')
    args = ap.parse_args()

    hits: list[tuple[str, int, str, str]] = []
    if args.staged_only:
        targets = staged_targets()
        blobs = dict(read_index_blobs(targets))
        for p in sorted(targets):
            if p in blobs:
                hits += scan_source(p, blobs[p])
    else:
        if args.paths:
            targets = [p for p in args.paths if p.endswith(SCAN_EXTS) and os.path.isfile(p)]
        else:
            root = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True,
                                  text=True, check=False).stdout.strip()
            if root:
                os.chdir(root)
            targets = tree_targets()
        for p in sorted(targets):
            hits += scan_file(p)

    if not hits:
        print('✅ Head labels: every head is named and mounted through the head identity.')
        return 0
    print(f'❌ Head labels: {len(hits)} site(s) name or mount a head by tool number.')
    for path, ln, kind, text in hits:
        print(f'   {path}:{ln}: [{kind}] {text}')
    kinds = sorted({k for _, _, k, _ in hits})
    for k in kinds:
        print(f'   {k}: {WHY[k]}')
    print(ADVICE)
    return 1


if __name__ == '__main__':
    sys.exit(main())
