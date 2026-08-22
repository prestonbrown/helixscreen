#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: an LVGL subject registered from C++ that nothing ever reads is dead
# UI state.
#
# The declarative contract is subject-in-the-middle: C++ publishes, XML binds. The
# XML linter already validates one direction — a bind_* naming a subject nobody
# registers is an UNKNOWN_SUBJECT_REF. Nothing validates the other. A subject that
# is registered, kept current, and read by neither an XML binding nor a C++
# observer costs a registration slot, a buffer and every update that writes it,
# and renders nothing. It is the shape a binding leaves behind when the widget
# that used it is deleted or renamed.
#
# Found this way (2026-08-21): network_label, volume_value and
# print_status_layout_mode were each registered and written with zero readers.
#
# NOT flagged:
#   - Subjects read by a C++ observer, matched two ways because observers take a
#     subject POINTER, not a name: the quoted name near the call, and the member
#     the INIT_SUBJECT_*/UI_MANAGED_SUBJECT_* macros derive from it (`<name>_`).
#     Name-matching alone misses e.g. observe_int_sync(&extruder_version_, ...).
#   - Subjects referenced from XML by ANY attribute, not just bind_*/subject=.
#     Component parameters forward a subject name under a caller-chosen name
#     (`<ams_env_indicator temp_text="ams_env_ind_detail_temp_text"/>`), and
#     expressions name them as bare identifiers inside cond=/expr=. Rather than
#     enumerate the forms, every attribute value and every expression identifier
#     is collected. Over-collecting is the safe direction here: it can only clear
#     an orphan, never invent one.
#   - Names composed at runtime — XML `${...}` splices and C++ that builds the
#     name with a format/concat. Neither side can be matched statically, so a
#     registration whose name is not a plain literal is skipped entirely.
#   - Any registration line carrying `// SUBJECT_OK: <reason>`.
#
# This is a RATCHET, not a wall: --max-allowed freezes today's count so the debt
# can only shrink.
#
# Usage:
#   check_orphan_subjects.py                      # count
#   check_orphan_subjects.py --list               # name every orphan
#   check_orphan_subjects.py --max-allowed 26     # ratcheting baseline

import argparse
import pathlib
import re
import sys

SRC_DIRS = ("src", "include")
# The subject macros' own doc comments register "my_count"/"name"/"temperature"
# in example code. Scanning the definition file finds those, not real state.
SKIP_FILES = ("include/state/subject_macros.h",)
XML_DIR = "ui_xml"

# register_subject("name", ...) and lv_xml_register_subject(scope, "name", ...)
REGISTER_RE = re.compile(
    r'(?:lv_xml_register_subject\s*\([^,]+,\s*|(?<![a-z_])register_subject\s*\(\s*)"([a-z_0-9]+)"')
# Any XML attribute that names a subject: bind_text=, bind_value=, subject=, ...
XML_REF_RE = re.compile(r'(?:bind_[a-z_]+|subject)="([^"]+)"')
# Expression attributes name subjects as bare identifiers: cond="a or b gt c".
XML_EXPR_RE = re.compile(r'(?:cond|expr)="([^"]+)"')
IDENT_RE = re.compile(r'[a-z_][a-z_0-9]*')
# A C++ observer on the same literal name.
# Any site that observes or reads a subject value.
READ_SITE_RE = re.compile(
    r'(?:observe_[a-z_]+|lv_subject_add_observer\w*|lv_subject_get_\w+)\s*[(<]')
IDENT_TOKEN_RE = re.compile(r'[A-Za-z_][A-Za-z_0-9]*')
ALLOW_RE = re.compile(r'//\s*SUBJECT_OK:')


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent.parent


def collect_registrations(root: pathlib.Path):
    """Map subject name -> list of "path:line" registration sites."""
    found: dict[str, list[str]] = {}
    for d in SRC_DIRS:
        for path in (root / d).rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            if str(path.relative_to(root)) in SKIP_FILES:
                continue
            for n, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
                if ALLOW_RE.search(line):
                    continue
                for m in REGISTER_RE.finditer(line):
                    rel = path.relative_to(root)
                    found.setdefault(m.group(1), []).append(f"{rel}:{n}")
    return found


def collect_xml_refs(root: pathlib.Path) -> set[str]:
    refs: set[str] = set()
    for path in (root / XML_DIR).rglob("*.xml"):
        text = path.read_text(errors="ignore")
        # Any attribute value that is a bare identifier may be a subject name:
        # bind_text=, subject=, and caller-named component params alike.
        for m in re.finditer(r'="([^"]*)"', text):
            val = m.group(1)
            if "${" in val or val.startswith(("#", "$")):
                continue
            # `@name` is the subject-reference spelling for widget attributes
            # that also accept a literal (primary_text="@spoolman_edit_save_text").
            if val.startswith("@"):
                val = val[1:]
            if IDENT_RE.fullmatch(val):
                refs.add(val)
        # Expressions name subjects as bare identifiers: cond="a or b gt c".
        for m in XML_EXPR_RE.finditer(text):
            refs.update(IDENT_RE.findall(m.group(1)))
    return refs


def collect_cpp_readers(root: pathlib.Path) -> set[str]:
    """Subject names that some C++ site observes or reads.

    Observers and getters take a subject POINTER, so the spelling at the call
    site is the member the subject macros derive (`foo_`), not the string "foo".
    Calls also wrap across lines, so the file is joined before scanning rather
    than windowed.
    """
    readers: set[str] = set()
    for d in SRC_DIRS:
        for path in (root / d).rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            text = path.read_text(errors="ignore")
            for m in READ_SITE_RE.finditer(text):
                chunk = text[m.start():m.start() + 400]
                for tok in IDENT_TOKEN_RE.findall(chunk):
                    readers.add(tok[:-1] if tok.endswith("_") else tok)
                for q in re.finditer(r'"([a-z_0-9]+)"', chunk):
                    readers.add(q.group(1))
    return readers


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true", help="print every orphan with its site")
    ap.add_argument("--max-allowed", type=int, default=None, help="ratchet baseline")
    ap.add_argument("--summary", action="store_true", help="one-line result")
    args = ap.parse_args()

    root = repo_root()
    registered = collect_registrations(root)
    bound = collect_xml_refs(root)
    observed = collect_cpp_readers(root)

    orphans = {n: sites for n, sites in registered.items()
               if n not in bound and n not in observed}

    count = len(orphans)
    if args.list:
        for name in sorted(orphans):
            print(f"{name}\n    {'; '.join(orphans[name])}")

    if args.max_allowed is None:
        print(f"orphan subjects: {count} of {len(registered)} registered")
        return 0

    if count > args.max_allowed:
        print(f"❌ Orphan subjects: {count} > baseline {args.max_allowed}. "
              f"A subject nothing binds or observes renders nothing — bind it, "
              f"read it, or delete it. Run --list to see them.")
        return 1

    if count < args.max_allowed:
        print(f"✅ Orphan subjects: {count} < baseline {args.max_allowed} — "
              f"lower --max-allowed in quality-checks.sh to {count} to hold the gain.")
        return 0

    print(f"✅ Orphan subjects: {count} == baseline ({args.max_allowed})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
