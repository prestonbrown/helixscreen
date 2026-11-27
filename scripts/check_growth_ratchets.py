#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: a branch does not grow the codebase sideways (prestonbrown/helixscreen#1945).

Two ratchets, each measured from the branch's fork point (scripts/diff_base.py)
to HEAD over committed work, so there is no baseline file for parallel branches
to conflict on. A count may fall; a branch may not raise it.

  global access    A class that receives a dependency through a constructor or
                   method parameter uses that parameter, not the global
                   accessor. Counted per file: calls to the accessor inside the
                   class body or inside an out-of-line `Class::method` body, for
                   every class that has a parameter of the dependency's type
                   anywhere in the tree (its class body, or the parameter list
                   of a `Class::method` definition), and every class deriving
                   from one at any depth. Two classes with one name are told
                   apart by namespace: only the one that receives it is a
                   receiver. A file may not gain one.
  active instance  Static-storage pointers that hand an XML callback "the
                   current instance" (`static Foo* active_instance_;`,
                   `static Foo* s_active_`, an anonymous-namespace
                   `Foo* s_active_ctx = nullptr;`, `static Foo* s_current_`,
                   `static Foo* current_instance_`, ...). The branch's total may
                   not rise; an out-of-line `Foo* Foo::active_instance_ =`
                   definition is not a second pointer and is not counted, and
                   a name containing `inactive` is not the pattern at all. A
                   declaration that is not a per-callback owner lookup opts out with
                   `// ACTIVE_INSTANCE_OK: <reason>` on its line.

One ban, on the whole tree at HEAD rather than the branch's change: no call to
lv_obj_is_valid() or lv_obj_is_in_widget_tree() anywhere under src/, include/ or tests/
(comments and strings not counted). Neither can tell a live widget from a new one at a
freed address; hold a helix::ui::WidgetRef or WeakWidget (include/ui_widget_ref.h), and
in a test use helix::test::widget_in_tree() (prestonbrown/helixscreen#1983).

Also on the whole tree at HEAD, app code only: no call to the bare static
Modal::show("component") factory (a string literal or a .c_str() as the component), nor
to a helix::ui::modal_show() wrapper around it.
Its dialog has no owner, so nothing hears it close; show it through
helix::ui::ComponentModal::show_owned(), modal_confirm(), or a Modal subclass
(prestonbrown/helixscreen#1958). Tests may still call it.

Also on the whole tree at HEAD: inside an LV_EVENT_DELETE callback, a WidgetRef is never
compared with the event's target. Its own hook can run first, so it already reads nullptr
and the comparison is silently false; the callback asks names_dying() instead.

Every finding exits 1. File size is not judged here: scripts/ratchet_report.py counts
the C++ files over FILE_CAP lines each week.

Usage:
  check_growth_ratchets.py --base auto     # the fork point diff_base.py picks
  check_growth_ratchets.py --base REV      # REV...HEAD
  check_growth_ratchets.py --report        # current counts on HEAD, per file
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

from source_strip import blank_cxx

# The line count past which scripts/ratchet_report.py counts a C++ file as oversized:
# p97 of the tree's C++ files (median 180, p95 ~1080).
FILE_CAP = 1500

CXX_SOURCES = (":(glob)src/**/*.cpp", ":(glob)src/**/*.h", ":(glob)include/**/*.h",
               ":(glob)tests/**/*.cpp", ":(glob)tests/**/*.h")
APP_SOURCES = (":(glob)src/**/*.cpp", ":(glob)src/**/*.h", ":(glob)include/**/*.h")


# (label, parameter type, global accessor)
DEPENDENCIES = (
    ("PrinterState", r"PrinterState", "get_printer_state"),
    ("Moonraker API", r"I?MoonrakerAPI", "get_moonraker_api"),
)

_QUAL = r"(?:\w+::)*"
_PTR_TAIL = r"(?:const\s*)?[&*]+\s*(?:const\s+)?\w*\s*(?:=[^,;(){}]*)?[,)]"


def _param_re(type_re: str) -> re.Pattern:
    """A parameter (named or not, defaulted or not) of `type_re` by reference or pointer."""
    return re.compile(rf"[(,]\s*(?:const\s+)?{_QUAL}{type_re}\s*{_PTR_TAIL}")


# "inactive" names the opposite of the pattern, so a name containing it is not one.
_NOT_INACTIVE = r"(?!\w*inactive)"
# "current" is the other word for the same slot: `s_current_panel`, a member `current_`,
# `current_instance_`. A bare `s_instance` is a singleton slot, not this pattern.
_CURRENT_NAME = r"(?:s_|g_)current\w*|current_|current_instance\w*"
_ACTIVE_NAME = rf"(?:s_|g_)?{_NOT_INACTIVE}\w*active\w*|(?:s_|g_)\w+_instance_?|{_CURRENT_NAME}"
ACTIVE_OPT_OUT = "ACTIVE_INSTANCE_OK:"
_KEYWORDS = {"return", "delete", "throw", "case", "goto", "else", "new"}
ACTIVE_STATIC_RE = re.compile(
    rf"^[ \t]*(?:inline\s+)?static\s+(?:inline\s+)?(?:const\s+)?([\w:<>]+)\s*\*\s*(?:const\s+)?"
    rf"({_ACTIVE_NAME})\s*(?:=\s*nullptr\s*|\{{\s*(?:nullptr)?\s*\}}\s*)?;", re.M)
ACTIVE_GLOBAL_RE = re.compile(
    rf"^[ \t]*(?:const\s+)?([\w:<>]+)\s*\*\s*((?:s_|g_){_NOT_INACTIVE}\w*active\w*|(?:s_|g_)\w+_instance_?"
    rf"|(?:s_|g_)current\w*)"
    rf"\s*(?:=\s*nullptr\s*|\{{\s*\}}\s*)?;", re.M)

VALIDITY_NAMES = ("lv_obj_is_valid", "lv_obj_is_in_widget_tree")

CLASS_HEAD_RE = re.compile(
    r"(?<![\w])(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?(?:[A-Z_][A-Z0-9_]*\s+)*(\w+)\s*"
    r"(?:final\s*)?(?::[^;{}]*)?$")
ENUM_HEAD_RE = re.compile(r"\benum\b")
NAMESPACE_HEAD_RE = re.compile(r"(?:\bnamespace\b[\w:\s]*|\bextern\s+\"C\"\s*)$")
METHOD_HEAD_RE = re.compile(r"(\w+)\s*(?:<[^<>()]*>)?\s*::\s*(~?\w+|operator\s*[^\s(]+)\s*\(")
# An out-of-line definition's owner with the qualifier written before it:
# `helix::ui::Foo::bar(` gives ("helix::ui::", "Foo", "bar"). A return type's own
# qualifier never fits, since the owner must be followed by `::` at once.
QUAL_METHOD_HEAD_RE = re.compile(
    r"((?:\w+\s*::\s*)*)(\w+)\s*(?:<[^<>()]*>)?\s*::\s*(~?\w+|operator\s*[^\s(]+)\s*\(")
NAMESPACE_NAME_RE = re.compile(r"\bnamespace\b\s*([A-Za-z_]\w*(?:::\w+)*)?\s*$")
ANONYMOUS_NAMESPACE = "(anonymous)"
# A base as a class head writes it, access and `virtual` dropped: `::Panel`,
# `other::Panel`, `Panel`, `Base<T>`.
BASE_RE = re.compile(r"(?:\b(?:public|protected|private|virtual)\s+)*"
                     r"((?:::)?\s*(?:\w+\s*::\s*)*\w+)\s*(?:<[^<>]*>)?")


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True).stdout


def read_blobs(root: Path, rev: str, paths) -> dict[str, str]:
    """Contents of `paths` at `rev`; a path missing there is left out."""
    if not paths:
        return {}
    req = "".join(f"{rev}:{p}\n" for p in paths).encode()
    out = subprocess.run(["git", "-C", str(root), "cat-file", "--batch"], input=req,
                         check=True, capture_output=True).stdout
    blobs, pos = {}, 0
    for p in paths:
        nl = out.index(b"\n", pos)
        header = out[pos:nl].split()
        pos = nl + 1
        if header[-1] == b"missing":
            continue
        size = int(header[2])
        blobs[p] = out[pos:pos + size].decode("utf-8", "replace")
        pos += size + 1
    return blobs


STATIC_DECL_RE = re.compile(r"(?:^|[;{}:])\s*(?:inline\s+|constexpr\s+)*static\b[^;{}()=]*?\b(~?\w+)\s*\(")
STATIC_HEAD_RE = re.compile(r"^\s*(?:(?:public|private|protected)\s*:\s*)?(?:\[\[[^\]]*\]\]\s*)?"
                            r"(?:inline\s+|constexpr\s+)*static\b")


class Span:
    """A class body, a member function defined inside one, or an out-of-line
    `Owner::method` definition with its parameter list: its owning class, the
    namespace that class was declared in, its name when it is an out-of-line
    definition, and whether it is a static member (no instance to use)."""
    __slots__ = ("start", "end", "owner", "method", "static", "ns")

    def __init__(self, start, end, owner, method=None, static=False, ns=""):
        self.start, self.end, self.owner = start, end, owner
        self.method, self.static, self.ns = method, static, ns

    def key(self, shared):
        """The class as the ratchet names it: by its bare name, or by its
        namespace and name when another class body shares that name."""
        return f"{self.ns}::{self.owner}" if self.ns and self.owner in shared else self.owner

    def bare(self):
        return self.owner.rsplit("::", 1)[-1]


def namespace_of(qualifier: str, enclosing: str) -> str:
    """The namespace an out-of-line definition names its owner in: the
    qualifier's, absolute when it starts with `::`, else below `enclosing`."""
    parts = [p for p in re.split(r"\s*::\s*", qualifier.strip()) if p]
    if qualifier.lstrip().startswith("::"):
        return "::".join(parts)
    return "::".join(p for p in (enclosing, "::".join(parts)) if p)


def namespace_chain(ns: str):
    """`ns`, then each namespace above it, innermost first; "" is the global one."""
    chain = [ns]
    while "::" in ns:
        ns = ns.rsplit("::", 1)[0]
        chain.append(ns)
    if ns:
        chain.append("")
    return chain


def scopes(code: str) -> list[Span]:
    """Every class body, every member function defined inside one, and every
    `Owner::method` definition with its parameter list. Namespaces are
    transparent to nesting and name the classes declared inside them."""
    spans = []
    # (kind, owner, start, method, static, name, ns); kind: ns | class | fn | member | block.
    # A namespace frame carries its `name`; a class, member or definition frame
    # carries in `ns` the namespace its class was declared in.
    stack = []

    def path():
        return "::".join(f[5] for f in stack if f[0] == "ns" and f[5])

    head = 0
    owner_fn = method_fn = ns_fn = None  # the out-of-line definition later top-level blocks continue
    for m in re.finditer(r"[{};]", code):
        ch, i = m.group(), m.start()
        in_class = bool(stack) and stack[-1][0] == "class"
        top = not stack or stack[-1][0] == "ns"
        if ch == ";":
            if top:
                owner_fn = method_fn = ns_fn = None
            head = i + 1
            continue
        if ch == "{":
            header = code[head:i]
            stripped = header.strip()
            if top and NAMESPACE_HEAD_RE.search(stripped):
                name = ""
                if "namespace" in stripped:
                    named = NAMESPACE_NAME_RE.search(stripped)
                    name = named.group(1) if named and named.group(1) else ANONYMOUS_NAMESPACE
                stack.append(("ns", None, i, None, False, name, None))
            elif (top or in_class) and (cm := CLASS_HEAD_RE.search(stripped)) \
                    and not ENUM_HEAD_RE.search(stripped):
                stack.append(("class", cm.group(1), i, None, False, None, path()))
            elif top:
                start = i
                if "(" in header:
                    heads = QUAL_METHOD_HEAD_RE.findall(header.split("(")[0] + "(")
                    owner_fn, method_fn = heads[-1][1:] if heads else (None, None)
                    ns_fn = namespace_of(heads[-1][0], path()) if heads else None
                    start = head
                stack.append(("fn", owner_fn, start, method_fn, False, None, ns_fn))
            elif in_class:
                is_static = "(" in header and bool(STATIC_HEAD_RE.match(header))
                stack.append(("member", stack[-1][1], head, None, is_static, None, stack[-1][6]))
            else:
                stack.append(("block", None, i, None, False, None, None))
            head = i + 1
            continue
        if stack:
            kind, owner, start, method, static, _name, ns = stack.pop()
            if kind in ("class", "fn", "member") and owner:
                spans.append(Span(start, i, owner, method, static, ns or ""))
    return spans


def span_at(spans: list[Span], pos: int) -> Span | None:
    best = None
    for sp in spans:
        if sp.start <= pos <= sp.end and (best is None or sp.end - sp.start < best.end - best.start):
            best = sp
    return best


def static_methods(spans: list[Span], code: str, shared=frozenset()) -> dict[str, set[str]]:
    found: dict[str, set[str]] = {}
    for sp in spans:
        if sp.method is None and not sp.static and code[sp.start] == "{":
            names = STATIC_DECL_RE.findall(code[sp.start:sp.end + 1])
            found.setdefault(sp.key(shared), set()).update(names)
    return found


def has_instance(sp: Span | None, statics: dict[str, set[str]], shared=frozenset()) -> bool:
    if sp is None or sp.static:
        return False
    return not (sp.method and sp.method in statics.get(sp.key(shared), ()))


def receivers(blobs: dict[str, str], param: re.Pattern, shared=frozenset()):
    """Classes with a non-static member taking `param`, and every class's static methods."""
    parsed = []
    statics: dict[str, set[str]] = {}
    for text in blobs.values():
        code = blank_cxx(text)
        spans = scopes(code)
        parsed.append((code, spans))
        for owner, names in static_methods(spans, code, shared).items():
            statics.setdefault(owner, set()).update(names)
    found = set()
    for code, spans in parsed:
        for m in param.finditer(code):
            sp = span_at(spans, m.start())
            if not has_instance(sp, statics, shared):
                continue
            if sp.method is None and code[sp.start] == "{":
                # A declaration in the class body: static if its own statement says so.
                decl = max(code.rfind(c, sp.start, m.start()) for c in ";{}") + 1
                if STATIC_HEAD_RE.match(code[decl:m.start()]):
                    continue
            found.add(sp.key(shared))
    return found, statics


def accessor_calls(text: str, accessor: str, classes: set[str],
                   statics: dict[str, set[str]], shared=frozenset()) -> int:
    code = blank_cxx(text)
    calls = [m.start() for m in re.finditer(rf"\b{accessor}\s*\(", code)]
    if not calls:
        return 0
    spans = scopes(code)
    local = static_methods(spans, code, shared)
    merged = {k: statics.get(k, set()) | local.get(k, set()) for k in set(statics) | set(local)}
    n = 0
    for pos in calls:
        sp = span_at(spans, pos)
        if sp is not None and sp.key(shared) in classes and has_instance(sp, merged, shared):
            n += 1
    return n

def active_instances(text: str) -> list[str]:
    kept = (line for line in text.split("\n") if ACTIVE_OPT_OUT not in line)
    code = blank_cxx("\n".join(kept))
    names = []
    for rx in (ACTIVE_STATIC_RE, ACTIVE_GLOBAL_RE):
        for m in rx.finditer(code):
            # A `char*` is a C string (a tag, a name), never an instance.
            if m.group(1) not in _KEYWORDS and m.group(1) != "char":
                names.append(m.group(2))
    return names


def banned_validity_calls(root: Path) -> list[str]:
    """`path:line: name` for every banned call in the C++ sources at HEAD."""
    hits = []
    for name in VALIDITY_NAMES:
        for path, text in tree_with(root, "HEAD", name, CXX_SOURCES).items():
            code = blank_cxx(text)
            for m in re.finditer(rf"\b{name}\s*\(", code):
                hits.append(f"{path}:{code.count(chr(10), 0, m.start()) + 1}: {name}()")
    return sorted(hits)


_FACTORY_RE = re.compile(r"\b(Modal::show|modal_show)\s*\(")
_FACTORY_ARG_RE = re.compile(r'\s*(?:"|[\w.>\-]+\.c_str\(\s*\)\s*[,)])')


def bare_modal_factory_calls(root: Path) -> list[str]:
    """`path:line` for every Modal::show("component") call in app code at HEAD."""
    hits = []
    paths = {**tree_with(root, "HEAD", "Modal::show"), **tree_with(root, "HEAD", "modal_show")}
    for path, text in paths.items():
        code = blank_cxx(text)
        for m in _FACTORY_RE.finditer(code):
            if m.group(1) == "modal_show" or _FACTORY_ARG_RE.match(text, m.end()):
                hits.append(f"{path}:{code.count(chr(10), 0, m.start()) + 1}")
    return sorted(hits)


_EVENT_TARGET = r"(?:static_cast<[^<>]*>\s*\(\s*)?lv_event_get_(?:current_)?target(?:_obj)?\s*\(\s*\w+\s*\)\s*\)?"
_DELETE_CB_RE = re.compile(r"lv_obj_add_event_cb\(\s*[^,;]+,\s*&?(?:\w+::)*(\w+)\s*,\s*LV_EVENT_DELETE\b")
_DELETE_LAMBDA_RE = re.compile(r"lv_obj_add_event_cb\(\s*[^,;]+,\s*\[[^\]]*\]\s*\([^)]*\)\s*(?=\{)")
_CLASS_RE = re.compile(r"\b(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?(\w+)\s*(?:final\s*)?(?::([^;{}()]*))?\{")
_REF_MEMBER_RE = re.compile(r"\bWidgetRef\s+(\w+)\s*(?:;|\{|=|\[)")


def _braced(code: str, start: int) -> str:
    """The balanced `{...}` opening at `start`."""
    depth = 0
    for i in range(start, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[start:i + 1]
    return code[start:]


def _class_table(blobs: dict[str, str]) -> dict[str, tuple[set[str], list[str]]]:
    """class name -> (its own WidgetRef members, its bases)."""
    table: dict[str, tuple[set[str], list[str]]] = {}
    for text in blobs.values():
        code = blank_cxx(text)
        for m in _CLASS_RE.finditer(code):
            members, bases = table.setdefault(m.group(1), (set(), []))
            members.update(_REF_MEMBER_RE.findall(_braced(code, m.end() - 1)))
            bases.extend(re.findall(r"(\w+)\s*(?:<[^,{]*>)?\s*(?:,|$)", m.group(2) or ""))
    return table


def _ref_members(table, name: str, seen=None) -> set[str]:
    seen = seen if seen is not None else set()
    if name in seen or name not in table:
        return set()
    seen.add(name)
    members, bases = table[name]
    found = set(members)
    for base in bases:
        found |= _ref_members(table, base, seen)
    return found


def _target_compare(body: str, handles: set[str]) -> list[int]:
    """Offsets in `body` where a name in `handles` meets the event's target in == or !=."""
    if not handles:
        return []
    locals_ = re.findall(rf"\b(\w+)\s*=\s*{_EVENT_TARGET}\s*;", body)
    target = "|".join([_EVENT_TARGET] + [rf"\b{re.escape(n)}\b" for n in locals_])
    handle = rf"(?:\w+(?:->|\.))*(?:{'|'.join(map(re.escape, sorted(handles)))})\b(?:\.get\(\))?"
    shapes = (rf"(?<![\w.>]){handle}\s*[!=]=\s*(?:{target})|(?:->|\.){handle}\s*[!=]=\s*(?:{target})",
              rf"(?:{target})\s*[!=]=\s*{handle}")
    return sorted({m.start() for rx in shapes for m in re.finditer(rx, body)})


def delete_callback_ref_compares(root: Path) -> list[str]:
    """`path:line` for every comparison of a WidgetRef with the target in a DELETE callback."""
    blobs = read_blobs(root, "HEAD", list_files(root, "HEAD", tests=False))
    table = _class_table(blobs)
    hits = []
    for path, text in blobs.items():
        if "LV_EVENT_DELETE" not in text:
            continue
        code = blank_cxx(text)
        file_refs = set(_REF_MEMBER_RE.findall(code))
        bodies = []
        names = set(_DELETE_CB_RE.findall(code))
        for name in names:
            for m in re.finditer(rf"(?:\b(\w+)\s*::\s*)?\b{name}\s*\([^()]*lv_event_t\s*\*[^()]*\)\s*\{{", code):
                bodies.append((m.end() - 1, m.group(1)))
        for m in _DELETE_LAMBDA_RE.finditer(code):
            body = _braced(code, m.end())
            if re.match(r"\s*,\s*LV_EVENT_DELETE\b", code[m.end() + len(body):]):
                bodies.append((m.end(), None))
        for start, owner in bodies:
            body = _braced(code, start)
            handles = file_refs | (_ref_members(table, owner) if owner else set())
            for off in _target_compare(body, handles):
                hits.append(f"{path}:{code.count(chr(10), 0, start + off) + 1}")
    return sorted(set(hits))


def is_source(path: str, tests: bool = True) -> bool:
    roots = ("src/", "include/", "tests/") if tests else ("src/", "include/")
    if not path.startswith(roots):
        return False
    return path.endswith(".h") or (path.endswith(".cpp") and not path.startswith("include/"))


def list_files(root: Path, rev: str, tests: bool) -> list[str]:
    return [p for p in git(root, "ls-tree", "-r", "--name-only", rev).split()
            if is_source(p, tests)]


def tree_with(root: Path, rev: str, needle: str, sources=APP_SOURCES) -> dict[str, str]:
    """Sources at `rev` (app code by default) that contain the fixed string `needle`."""
    try:
        hits = git(root, "grep", "-l", "-F", needle, rev, "--", *sources).split()
    except subprocess.CalledProcessError:
        hits = []
    paths = [h.split(":", 1)[1] for h in hits]
    return read_blobs(root, rev, paths)


def class_namespaces(root: Path, rev: str, names) -> dict[str, set[str]]:
    """Bare class name -> the namespaces its class bodies sit in at `rev`, for
    the names asked for."""
    if not names:
        return {}
    # POSIX extended, which is all git grep -E takes on every platform: no
    # (?:...) group, no \b, so the name's end is spelled out.
    head = (r"(class|struct)[[:space:]]+(" + "|".join(sorted(names))
            + r")([^[:alnum:]_]|$)")
    try:
        hits = git(root, "grep", "-l", "-E", head, rev, "--", *APP_SOURCES).split()
    except subprocess.CalledProcessError:
        return {}
    found: dict[str, set[str]] = {}
    for text in read_blobs(root, rev, [h.split(":", 1)[1] for h in hits]).values():
        code = blank_cxx(text)
        for sp in scopes(code):
            if sp.method is None and code[sp.start] == "{" and sp.owner in names:
                found.setdefault(sp.owner, set()).add(sp.ns)
    return found


def shared_names(root: Path, rev: str, names) -> set[str]:
    """The names in `names` that class bodies in more than one namespace carry at `rev`."""
    return {name for name, spaces in class_namespaces(root, rev, names).items()
            if len(spaces) > 1}


def base_namespace(written: str, chain, sites: dict[str, set[str]]):
    """The namespace of a base as a class head writes it, or None when the
    known class sites cannot say: `::a::B` is absolute, `a::B` is taken as
    written (below the class's own namespace when that is where B lives), and
    a bare `B` is the one nearest the class in its namespace chain."""
    w = re.sub(r"\s+", "", written)
    bare = w.rsplit("::", 1)[-1]
    spaces = sites.get(bare, set())
    if w.startswith("::"):
        return w[2:-len(bare)].rstrip(":")
    if "::" in w:
        ns = w[:-len(bare)].rstrip(":")
        for above in chain:
            there = f"{above}::{ns}" if above else ns
            if there in spaces:
                return there
        return None
    for above in chain:
        if above in spaces:
            return above
    return None


def derived_classes(root: Path, rev: str, bases: set[str], sites=None, shared=frozenset()):
    """Classes at `rev` deriving from a class named in `bases`: by bare name,
    or by namespace and name for a name in `shared`, told apart by `sites`
    (what class_namespaces returns for those names). A base whose namespace
    the sites cannot place counts as the receiver of its name, as it always
    did."""
    frontier = {b.rsplit("::", 1)[-1] for b in bases}
    found: set[str] = set(bases)
    while frontier:
        args = [a for name in sorted(frontier) for a in ("-e", name)]
        try:
            hits = git(root, "grep", "-l", "-w", "-F", *args, rev, "--", *APP_SOURCES).split()
        except subprocess.CalledProcessError:
            break
        frontier = set()
        for text in read_blobs(root, rev, [h.split(":", 1)[1] for h in hits]).values():
            code = blank_cxx(text)
            for sp in scopes(code):
                if sp.method is not None or code[sp.start] != "{":
                    continue
                decl = max(code.rfind(c, 0, sp.start) for c in ";{}") + 1
                head_text = code[decl:sp.start].strip()
                head = CLASS_HEAD_RE.search(head_text)
                if not head or head.group(1) != sp.owner:
                    continue
                bases = re.search(r":([^;{}]*)$", head_text[head.end(1):])
                for written in BASE_RE.findall(bases.group(1) if bases else ""):
                    bare = re.sub(r"\s+", "", written).rsplit("::", 1)[-1]
                    ns = base_namespace(written, namespace_chain(sp.ns), sites or {})
                    if bare in shared:
                        inherits = ns is None or f"{ns}::{bare}" in found
                    else:
                        inherits = bare in found
                    if inherits:
                        key = sp.key(shared)
                        if key not in found:
                            found.add(key)
                            frontier.add(sp.owner.rsplit("::", 1)[-1])
    return found


def receivers_at(root: Path, rev: str, type_re: str, subclasses: bool = True, shared=None):
    """(receiving classes, static methods per class) at `rev`. A class is named
    by its bare name, or by namespace and name when another class body at `rev`
    shares that name: `shared` is that set of names, and None finds it.
    `subclasses=False` skips the inheritance walk for a caller that only needs
    the statics."""
    blobs = tree_with(root, rev, type_re.replace("I?", ""))
    if shared is None:
        classes, statics = receivers(blobs, _param_re(type_re))
        if subclasses:
            classes = with_subclasses(root, rev, classes)
        shared = shared_names(root, rev, classes)
        if not shared:
            return classes, statics
    classes, statics = receivers(blobs, _param_re(type_re), shared)
    return (with_subclasses(root, rev, classes, shared) if subclasses else classes), statics


def with_subclasses(root: Path, rev: str, classes: set[str], shared=frozenset()) -> set[str]:
    """`classes` plus every class at `rev` deriving from one, at any depth: a class
    that inherits the dependency's member receives it as surely as its base does.
    A name two classes carry is followed by namespace."""
    found = derived_classes(root, rev, {c for c in classes if c.rsplit("::", 1)[-1] not in shared})
    if any(c.rsplit("::", 1)[-1] in shared for c in classes):
        sites: dict[str, set[str]] = {}
        for n in sorted(shared):
            spaces = class_namespaces(root, rev, [n]).get(n, set())
            sites[n] = spaces
        found |= derived_classes(root, rev, classes, sites, shared)
    return found | classes


def global_access_counts(root: Path, rev: str, type_re: str, accessor: str,
                         classes: set[str] | None = None, only=None,
                         shared: set[str] | None = None) -> Counter:
    """Accessor calls per file inside `classes` (default: the classes receiving the
    dependency at `rev`), with static members judged by `rev`'s own declarations."""
    if classes is None:
        classes, statics = receivers_at(root, rev, type_re)
        shared = shared_names(root, rev, {k.rsplit("::", 1)[-1] for k in classes})
    else:
        # Both sides count the classes the caller named; the shared ones keep
        # their namespace-qualified keys, so the statics are keyed the same way.
        _, statics = receivers_at(root, rev, type_re, subclasses=False, shared=shared)
        shared = shared or frozenset()
    blobs = tree_with(root, rev, accessor)
    counts = Counter()
    for path, text in blobs.items():
        if only is not None and path not in only:
            continue
        n = accessor_calls(text, accessor, classes, statics, shared)
        if n:
            counts[path] = n
    return counts


def changed(root: Path, base: str):
    """(old_path, new_path) for every C++ source the branch touched; '' for absent."""
    rows = []
    for row in git(root, "diff", "--name-status", "-M", "--no-ext-diff",
                   f"{base}...HEAD", "--", *CXX_SOURCES).splitlines():
        parts = row.split("\t")
        status = parts[0][0]
        if status == "R" or status == "C":
            rows.append((parts[1] if status == "R" else "", parts[2]))
        elif status == "A":
            rows.append(("", parts[1]))
        elif status == "D":
            rows.append((parts[1], ""))
        else:
            rows.append((parts[1], parts[1]))
    return rows


def report(root: Path) -> int:
    for label, type_re, accessor in DEPENDENCIES:
        counts = global_access_counts(root, "HEAD", type_re, accessor)
        print(f"{label}: {sum(counts.values())} {accessor}() calls in {len(counts)} files "
              f"inside classes that receive it")
        for path, n in sorted(counts.items()):
            print(f"  {n:4d}  {path}")
    total = 0
    for path, text in read_blobs(root, "HEAD", list_files(root, "HEAD", tests=False)).items():
        for name in active_instances(text):
            total += 1
            print(f"  active instance: {path}: {name}")
    print(f"active instance pointers: {total}")
    banned = banned_validity_calls(root)
    print(f"banned widget validity calls: {len(banned)}")
    for hit in banned:
        print(f"  {hit}")
    factory = bare_modal_factory_calls(root)
    print(f"bare Modal::show factory calls: {len(factory)}")
    for hit in factory:
        print(f"  {hit}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", help="REV, or 'auto' for diff_base.py's fork point")
    ap.add_argument("--report", action="store_true", help="print HEAD's counts and exit")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    root = Path(git(Path(args.root), "rev-parse", "--show-toplevel").strip())
    if args.report:
        return report(root)
    banned = banned_validity_calls(root)
    if banned:
        for hit in banned:
            print(f"❌ {hit}: banned, it cannot tell a live widget from a new one at a freed "
                  f"address. Hold a helix::ui::WidgetRef or WeakWidget (include/ui_widget_ref.h); "
                  f"a test uses helix::test::widget_in_tree()")
        return 1
    factory = bare_modal_factory_calls(root)
    if factory:
        for hit in factory:
            print(f"❌ {hit}: the bare Modal::show(\"component\") factory leaves the dialog "
                  f"with no owner, so nothing hears it close. Use "
                  f"helix::ui::ComponentModal::show_owned(), modal_confirm() or a Modal "
                  f"subclass (prestonbrown/helixscreen#1958)")
        return 1
    compares = delete_callback_ref_compares(root)
    if compares:
        for hit in compares:
            print(f"❌ {hit}: a WidgetRef compared with the target in an LV_EVENT_DELETE callback. "
                  f"Its hook can run first and read nullptr; use names_dying(target)")
        return 1
    if not args.base:
        ap.error("--base is required")

    base = args.base
    if base == "auto":
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from diff_base import resolve_base  # noqa: E402
        base, why = resolve_base(root)
        if base == "HEAD":
            print(f"❌ Growth ratchets: no fork point to measure from ({why}). A shallow "
                  f"or single-branch checkout has no upstream; fetch one, or pass --base REV")
            return 1
    if base == "HEAD" or git(root, "rev-parse", base) == git(root, "rev-parse", "HEAD"):
        print("✅ Growth ratchets: no committed work to measure")
        return 0
    fork = git(root, "merge-base", base, "HEAD").strip()
    scope = f"{base[:12]}...HEAD"

    rows = changed(root, base)
    old = read_blobs(root, fork, [o for o, _ in rows if o])
    new = read_blobs(root, "HEAD", [n for _, n in rows if n])
    failures = []

    touched_old = {o for o, _ in rows if o}
    touched_new = {n for _, n in rows if n}
    renamed = {n: o for o, n in rows if o and n}
    renamed_to = {o: n for n, o in renamed.items()}
    for label, type_re, accessor in DEPENDENCIES:
        if not any(accessor in t for t in (*old.values(), *new.values())):
            continue
        # Both sides count the classes that received the dependency at the fork
        # point: a class that starts receiving it on this branch is mid-migration,
        # and is held to the ratchet from the next branch on.
        fork_classes, _ = receivers_at(root, fork, type_re)
        # A class that gains a namesake on this branch is told from it on both sides.
        head_classes, _ = receivers_at(root, "HEAD", type_re)
        shared = (shared_names(root, fork, fork_classes)
                  | shared_names(root, "HEAD", head_classes))
        if shared:
            fork_classes, _ = receivers_at(root, fork, type_re, shared=shared)
        before = global_access_counts(root, fork, type_re, accessor, fork_classes, touched_old,
                                      shared)
        after = global_access_counts(root, "HEAD", type_re, accessor, fork_classes, touched_new,
                                     shared)
        for path, n in sorted(after.items()):
            was = before.get(renamed.get(path, path), 0)
            if n > was:
                failures.append(f"{path}: {accessor}() inside a class that receives "
                                f"{label} grew {was} -> {n}: use the parameter the class "
                                f"already has")

    def active(blobs):
        return Counter(f"{p}: {name}" for p, t in blobs.items() if not p.startswith("tests/")
                       for name in active_instances(t))
    act_before = active({renamed_to.get(p, p): t for p, t in old.items()})
    act_after = active(new)
    grown = sum(act_after.values()) - sum(act_before.values())
    if grown > 0:
        names = "; ".join(sorted((act_after - act_before).elements()))
        failures.append(f"static active-instance pointers grew by {grown} ({names}): route "
                        f"the XML callback through the widget's user data or an existing "
                        f"owner instead of a new static")

    if failures:
        for f in failures:
            print(f"❌ {f}")
        print(f"   Measured {scope}. Ratchets: prestonbrown/helixscreen#1945.")
        return 1
    print(f"✅ Growth ratchets ({scope}): {len(rows)} C++ files checked, active-instance "
          f"pointers {grown:+d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
