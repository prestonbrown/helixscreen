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
# WHAT COUNTS AS A REGISTRATION: every family that publishes a name, not just the
# quoted-literal call. Most of the population goes through INIT_SUBJECT_*,
# UI_MANAGED_SUBJECT_* or UI_SUBJECT_INIT_AND_REGISTER_*, each of which spells
# the name as a bare literal argument. A family the collector does not match is
# not merely unflagged, it is UNCOUNTED - and since the result is a count checked
# against a baseline, a population that shrinks can only report green.
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
# This is a RATCHET, not a wall: --baseline names the orphans that are accepted
# debt today, so the list can only shrink. Keying on names rather than a count
# also catches a swap - one orphan removed while another arrives.
#
# Usage:
#   check_orphan_subjects.py                      # count
#   check_orphan_subjects.py --list               # name every orphan
#   check_orphan_subjects.py --baseline FILE      # fail on any orphan not listed there
#   check_orphan_subjects.py --baseline FILE --write-baseline
#   check_orphan_subjects.py --repo-root DIR      # scan a tree other than this one

import argparse
import pathlib
import re
import sys

SRC_DIRS = ("src", "include")
# The subject macros' own doc comments register "my_count"/"name"/"temperature"
# in example code. Scanning the definition file finds those, not real state.
SKIP_FILES = ("include/state/subject_macros.h",)
XML_DIR = "ui_xml"

# register_subject("name", ...), register_subject_in_current_scope("name", ...) and
# lv_xml_register_subject(scope, "name", ...). The scoped spelling is what the
# subject macros expand to, and callers also use it directly.
REGISTER_RE = re.compile(
    r'(?:lv_xml_register_subject\s*\([^,]+,\s*'
    r'|(?<![a-z_])register_subject(?:_in_current_scope)?\s*\(\s*)"([a-z_0-9]+)"')
# INIT_SUBJECT_*(name, ...) registers under #name and hands over &name##_, so the
# name is never a quoted literal at the call site and the member is always the
# name with a trailing underscore. Matching only the quoted form leaves every
# macro-registered subject unexamined.
MACRO_REGISTER_RE = re.compile(
    r'(?<![A-Za-z_])INIT_SUBJECT_(?:INT_VOLATILE|INT|STRING)\s*\(\s*([a-z_0-9]+)')
# The macro families that take the member as their FIRST argument and the XML
# name as a quoted literal at a fixed distance from the END of the argument
# list: UI_MANAGED_SUBJECT_*(subject, ..., xml_name, manager) closes with the
# SubjectManager, UI_SUBJECT_INIT_AND_REGISTER_*(subject, ..., name) closes with
# the name. Anchoring on the tail rather than a per-macro argument index keeps a
# new type variant counted, since those add buffer/size/initial arguments in the
# middle. Matching neither family leaves most of the registered population
# unexamined while the gate still reports a count.
MANAGED_MACRO_RE = re.compile(
    r'(?<![A-Za-z_])UI_(MANAGED_SUBJECT|SUBJECT_INIT_AND_REGISTER)_'
    r'(?:INT_VOLATILE|INT|STRING_N|STRING|POINTER|COLOR)\s*\(')
# Position of the name argument counted back from the end of the call, by family.
MANAGED_NAME_FROM_END = {"MANAGED_SUBJECT": 2, "SUBJECT_INIT_AND_REGISTER": 1}
# A name argument the gate can act on. Anything else - a std::string, a
# concatenation, a per-slot key - is composed at runtime and skipped, the same
# as a non-literal name at a raw registration site.
LITERAL_NAME_RE = re.compile(r'\s*"([a-z_0-9]+)"\s*\Z')
# The macros are documented with worked examples in their own headers and in
# every base class that mentions them, so a doc comment registers "my_subject"
# and "item_count" as readily as real code does.
COMMENT_LINE_RE = re.compile(r'^\s*(?:\*|//)')
# The member a registration hands over, so reads can be matched by pointer name.
MEMBER_RE = re.compile(r'&\s*([A-Za-z_][A-Za-z_0-9]*)')
# An accessor body that only yields the subject's address.
HANDOVER_RE = re.compile(r'return\s+&\s*[A-Za-z_][A-Za-z_0-9]*\s*;')
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
# — puts the literal ABOVE the lv_subject_get_* match. Reaching backwards is
# only sound because registrations are masked out of the scanned text first
# (see mask_registrations): otherwise the window swallows the declaration of
# the very subject it is deciding about.
READ_WINDOW = 400
IDENT_TOKEN_RE = re.compile(r'[A-Za-z_][A-Za-z_0-9]*')
ALLOW_RE = re.compile(r'//\s*SUBJECT_OK:')


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent.parent


def call_arguments(text: str, open_paren: int):
    """Top-level arguments of the call whose '(' sits at `open_paren`.

    Returns (args, end_offset). Nesting and string literals are tracked so a
    comma inside lv_tr("a, b") or buf.size() does not split an argument.
    """
    args: list[str] = []
    depth = 0
    start = open_paren + 1
    i = open_paren
    in_string = False
    while i < len(text):
        c = text[i]
        if in_string:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_string = False
        elif c == '"':
            in_string = True
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i])
                return args, i + 1
        elif c == "," and depth == 1:
            args.append(text[start:i])
            start = i + 1
        i += 1
    return args, len(text)


def find_managed_macro_calls(text: str):
    """Yield (name, member, start, end) for every macro-family registration."""
    for m in MANAGED_MACRO_RE.finditer(text):
        if COMMENT_LINE_RE.match(text[text.rfind("\n", 0, m.start()) + 1:m.start()]):
            continue
        # The pattern ends on the opening paren, so it is the character before
        # the match end.
        args, end = call_arguments(text, m.end() - 1)
        from_end = MANAGED_NAME_FROM_END[m.group(1)]
        if len(args) < from_end + 1:
            continue
        literal = LITERAL_NAME_RE.match(args[-from_end])
        if not literal:
            continue
        member = IDENT_TOKEN_RE.match(args[0].strip())
        yield literal.group(1), member.group(0) if member else None, m.start(), end


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
            text = path.read_text(errors="ignore")
            lines = text.splitlines()
            rel = path.relative_to(root)
            # The macro families wrap freely, so their name literal is often not
            # on the line the macro starts. Scan the whole text for those.
            for name, member, start, end in find_managed_macro_calls(text):
                first = text.count("\n", 0, start)
                last = text.count("\n", 0, end)
                if any(ALLOW_RE.search(l) for l in lines[first:last + 2]):
                    continue
                found.setdefault(name, []).append(f"{rel}:{first + 1}")
                if member:
                    members.setdefault(name, set()).add(member)
            for n, line in enumerate(lines, 1):
                # Prose naming a macro is not a registration, and the macros are
                # documented with worked examples wherever they are mentioned.
                if COMMENT_LINE_RE.match(line):
                    continue
                # The call may wrap; accept the opt-out on any of its lines.
                if any(ALLOW_RE.search(l) for l in lines[n - 1:n + 2]):
                    continue
                for m in REGISTER_RE.finditer(line):
                    name = m.group(1)
                    found.setdefault(name, []).append(f"{rel}:{n}")
                    # The pointer argument may sit on this line or the next.
                    tail = line[m.end():]
                    mem = MEMBER_RE.search(tail)
                    if mem:
                        members.setdefault(name, set()).add(mem.group(1))
                for m in MACRO_REGISTER_RE.finditer(line):
                    name = m.group(1)
                    found.setdefault(name, []).append(f"{rel}:{n}")
                    members.setdefault(name, set()).add(f"{name}_")
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


def mask_registrations(text: str) -> str:
    """Blank out every registration call, preserving offsets and line breaks.

    Registering a subject is not reading it, but the call spells out both the
    name and the member, and registrations sit a few lines from the observers
    that read them — that is the ordinary shape of init_subjects(). Left in the
    text, a registration falls inside the window around some neighbouring read
    site and satisfies the read check for itself, which clears exactly the
    population this gate exists to find. Masking is length-preserving so the
    windows stay aligned with the offsets READ_SITE_RE reports.
    """
    out = list(text)
    # `return &subject_;` hands the subject over; it does not read it. The body
    # sits in the owner's header among that class's other inline reads, so left
    # in the text it falls inside a neighbouring read window and satisfies the
    # member check for itself - the same self-clearing a registration would do.
    for m in HANDOVER_RE.finditer(text):
        for j in range(m.start(), m.end()):
            if out[j] != "\n":
                out[j] = " "
    # A macro-family call spells both the name and the member, so it clears
    # itself and its neighbours from inside a read window exactly as a raw
    # registration does. Masked here by its parsed extent, since these wrap.
    for _name, _member, start, end in find_managed_macro_calls(text):
        for j in range(start, end):
            if out[j] != "\n":
                out[j] = " "
    spans = [m.start() for m in REGISTER_RE.finditer(text)]
    spans += [m.start() for m in MACRO_REGISTER_RE.finditer(text)]
    for start in sorted(spans):
        open_paren = text.find("(", start)
        if open_paren == -1:
            continue
        depth = 0
        end = len(text)
        for i in range(open_paren, len(text)):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        for j in range(start, end):
            if out[j] != "\n":
                out[j] = " "
    return "".join(out)


def collect_read_text(root: pathlib.Path) -> str:
    """Concatenated text of every site that observes or reads a subject."""
    chunks = []
    for d in SRC_DIRS:
        for path in (root / d).rglob("*"):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            text = mask_registrations(path.read_text(errors="ignore"))
            for m in READ_SITE_RE.finditer(text):
                chunks.append(text[max(0, m.start() - READ_WINDOW):m.start() + READ_WINDOW])
    return "\n".join(chunks)


BASELINE_HEADER = """\
# LVGL subjects that are registered and kept current but that nothing reads.
#
# This is debt to drive down, not a permitted shape: every line is dead UI state
# that costs a registration slot, a buffer and every update written to it, and
# renders nothing. Bind it from XML, read it from C++, or delete it - then re-run
# with --write-baseline so the list can only shrink. A subject that is not on
# this list and has no reader fails the gate.
#
# Generated by:
#   scripts/check_orphan_subjects.py --baseline scripts/orphan_subject_baseline.txt \\
#       --write-baseline
#
# Format:  <subject name>  # <registration site>
"""


def read_baseline(path: pathlib.Path) -> set:
    if not path.exists():
        return set()
    names = set()
    for line in path.read_text().splitlines():
        entry = line.split("#", 1)[0].strip()
        if entry:
            names.add(entry)
    return names


def write_baseline(path: pathlib.Path, orphans: dict) -> None:
    lines = [BASELINE_HEADER]
    for name in sorted(orphans):
        lines.append(f"{name}  # {'; '.join(orphans[name])}\n")
    path.write_text("".join(lines))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true", help="print every orphan with its site")
    ap.add_argument("--baseline", type=pathlib.Path, default=None,
                    help="accepted-orphan list to check against (gate mode)")
    ap.add_argument("--write-baseline", action="store_true",
                    help="rewrite --baseline from today's orphans")
    ap.add_argument("--summary", action="store_true", help="one-line result only")
    ap.add_argument("--repo-root", type=pathlib.Path, default=None,
                    help="tree to scan (default: this repository)")
    args = ap.parse_args()

    root = args.repo_root.resolve() if args.repo_root else repo_root()
    registered, members = collect_registrations(root)
    bound = collect_xml_refs(root)
    read_text = collect_read_text(root)

    def is_read(name: str) -> bool:
        if re.search(r'"' + re.escape(name) + r'"', read_text):
            return True
        # A consumer that does not own the subject reaches it through the
        # owner's accessor, whose identifier swallows the subject name whole:
        # lv_label_bind_text(label, state.get_hardware_issues_label_subject(), ...)
        # tokenises as one word, so neither the name nor the member is spelled.
        # Only a CALL counts, and only through an object: `.` or `->`. A bare
        # identifier would let an uncalled getter defined among the owner's
        # other inline reads clear its own subject, and `::` is the
        # out-of-line DEFINITION of that getter - the house spelling for
        # PrinterState and AmsState accessors - which vouches for the subject
        # with zero callers. Doc comments naming `Class::get_x_subject()` do
        # the same.
        # The accessor is named after the MEMBER as often as after the subject:
        # WidthSensorManager publishes "filament_width_diameter" from diameter_
        # and hands it out as get_diameter_subject(). Both spellings come from
        # the registration site.
        for spelling in {name, *(m.rstrip("_") for m in members.get(name, ()))}:
            if re.search(r'(?:\.|->)\s*get_' + re.escape(spelling) + r'_subject\s*\(',
                         read_text):
                return True
        for mem in members.get(name, ()):
            if re.search(r'\b' + re.escape(mem) + r'\b', read_text):
                return True
        return False

    orphans = {n: sites for n, sites in registered.items()
               if n not in bound and not is_read(n)}

    count = len(orphans)
    if args.list and not args.summary:
        for name in sorted(orphans):
            print(f"{name}\n    {'; '.join(orphans[name])}")

    if args.write_baseline:
        if args.baseline is None:
            print("--write-baseline needs --baseline PATH")
            return 2
        write_baseline(args.baseline, orphans)
        print(f"wrote {args.baseline}: {count} orphan subjects")
        return 0

    if args.baseline is None:
        print(f"orphan subjects: {count} of {len(registered)} registered")
        return 0

    accepted = read_baseline(args.baseline)
    added = sorted(set(orphans) - accepted)
    if added:
        print(f"❌ Orphan subjects: {len(added)} not in {args.baseline}. A subject nothing "
              f"binds or observes renders nothing — bind it, read it, or delete it:")
        for name in added:
            print(f"     {name}  ({'; '.join(orphans[name])})")
        return 1

    gone = sorted(accepted - set(orphans))
    if gone:
        print(f"✅ Orphan subjects: {count} of {len(registered)} registered, "
              f"{len(gone)} fewer than {args.baseline} — re-run with --write-baseline "
              f"to hold the gain ({', '.join(gone)}).")
        return 0

    print(f"✅ Orphan subjects: {count} of {len(registered)} registered, all accepted debt "
          f"in {args.baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
