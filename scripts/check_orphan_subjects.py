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
#   - Subjects read by a C++ observer or getter. Observers take a subject POINTER,
#     so the spelling at the read site is the MEMBER, which frequently does not
#     match the subject string: "ams_external_spool_color" registers
#     &external_spool_color_, "volume_value" registers &volume_value_subject_.
#     The member is therefore resolved from the registration site itself
#     (`lv_xml_register_subject(scope, "name", &member)`) and reads are matched
#     against that. Deriving `<name>_` instead silently misses the majority.
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
#   - Subjects fetched by name and then read through the resulting pointer. The
#     literal sits on the lv_xml_get_subject() line and the read is the NEXT
#     statement, so the window around a read site extends backwards as well as
#     forwards (see READ_WINDOW).
#   - Any registration carrying `// SUBJECT_OK: <reason>`. clang-format wraps
#     these calls freely, so the annotation is honoured anywhere in the two lines
#     following the name as well as on it.
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
# The member a registration hands over, so reads can be matched by pointer name.
MEMBER_RE = re.compile(r'&\s*([A-Za-z_][A-Za-z_0-9]*)')
# Any XML attribute that names a subject: bind_text=, bind_value=, subject=, ...
XML_REF_RE = re.compile(r'(?:bind_[a-z_]+|subject)="([^"]+)"')
# Expression attributes name subjects as bare identifiers: cond="a or b gt c".
XML_EXPR_RE = re.compile(r'(?:cond|expr)="([^"]+)"')
IDENT_RE = re.compile(r'[a-z_][a-z_0-9]*')
# A C++ observer on the same literal name.
# Any site that observes or reads a subject value. lv_label_bind_text() and the
# rest of LVGL's lv_*_bind_* family count: they attach an observer to the subject
# just as observe_*() does, they just do it from C++ instead of from XML.
# lv_xml_get_subject() counts too, and is the one form matched by NAME rather
# than by member: a consumer that does not own the subject resolves it out of
# the global scope by its literal string and then observes the pointer, so the
# member spelling never appears at the read site. job_queue_count has four such
# consumers (job_queue_widget, ui_job_queue_modal, print_status_widget x2) and
# no member-spelled read anywhere.
READ_SITE_RE = re.compile(
    r'(?:observe_[a-z_]+|lv_subject_add_observer\w*|lv_subject_get_\w+|lv_\w+_bind_\w+'
    r'|lv_xml_get_subject)\s*[(<]')
# How far either side of a read site to look for the subject's name or member.
# The window is SYMMETRIC because the name routinely precedes the read: fetching
# a subject by name and reading it on the next line —
#     lv_subject_t* on = lv_xml_get_subject(nullptr, "chamber_filter_fan_on");
#     tc->set_chamber_filter_fan(!on || lv_subject_get_int(on) != 1);
# — puts the literal ABOVE the lv_subject_get_* match. A forward-only window
# missed every read of that shape and reported a live subject as an orphan.
READ_WINDOW = 400
IDENT_TOKEN_RE = re.compile(r'[A-Za-z_][A-Za-z_0-9]*')
ALLOW_RE = re.compile(r'//\s*SUBJECT_OK:')


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent.parent


def collect_registrations(root: pathlib.Path):
    """Map subject name -> (sites, members)."""
    found: dict[str, list[str]] = {}
    members: dict[str, set[str]] = {}
    for d in SRC_DIRS:
        for path in (root / d).rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            if str(path.relative_to(root)) in SKIP_FILES:
                continue
            lines = path.read_text(errors="ignore").splitlines()
            for n, line in enumerate(lines, 1):
                # The call may wrap; accept the opt-out on any of its lines.
                if any(ALLOW_RE.search(l) for l in lines[n - 1:n + 2]):
                    continue
                for m in REGISTER_RE.finditer(line):
                    rel = path.relative_to(root)
                    name = m.group(1)
                    found.setdefault(name, []).append(f"{rel}:{n}")
                    # The pointer argument may sit on this line or the next.
                    tail = line[m.end():]
                    mem = MEMBER_RE.search(tail)
                    if mem:
                        members.setdefault(name, set()).add(mem.group(1))
    return found, members


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


def collect_read_text(root: pathlib.Path) -> str:
    """Concatenated text of every site that observes or reads a subject."""
    chunks = []
    for d in SRC_DIRS:
        for path in (root / d).rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            text = path.read_text(errors="ignore")
            for m in READ_SITE_RE.finditer(text):
                chunks.append(text[max(0, m.start() - READ_WINDOW):m.start() + READ_WINDOW])
    return "\n".join(chunks)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true", help="print every orphan with its site")
    ap.add_argument("--max-allowed", type=int, default=None, help="ratchet baseline")
    ap.add_argument("--summary", action="store_true", help="one-line result")
    args = ap.parse_args()

    root = repo_root()
    registered, members = collect_registrations(root)
    bound = collect_xml_refs(root)
    read_text = collect_read_text(root)

    def is_read(name: str) -> bool:
        if re.search(r'"' + re.escape(name) + r'"', read_text):
            return True
        for mem in members.get(name, ()):
            if re.search(r'\b' + re.escape(mem) + r'\b', read_text):
                return True
        return False

    orphans = {n: sites for n, sites in registered.items()
               if n not in bound and not is_read(n)}

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
