#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Move LVGL subjects held by value onto helix::Subject (include/core/subject.h).

Mechanical rewrites only, in code (comments and string literals are never touched):

  lv_subject_t name_;                     -> helix::Subject name_;
  lv_subject_t rows_[N];                  -> helix::Subject rows_[N];
  lv_subject_t (or lv_subject_t[]) as any template argument -> helix::Subject (or [])
  std::make_unique<lv_subject_t>()        -> std::make_unique<helix::Subject>()
  lv_subject_init_T(&EXPR, ARGS)          -> EXPR.init_T(ARGS)
  lv_subject_init_T(PTR.get(), ARGS)      -> PTR->init_T(ARGS)
  lv_subject_deinit(&EXPR)                -> EXPR.reset()
  lv_subject_deinit(OTHER)                -> helix::subject_reset(OTHER)
  lv_subject_copy_string(                 -> lv_subject_set_string(
  lv_subject_t& p  (a parameter the file inits or resets through &p)  -> helix::Subject& p

and adds `#include "core/subject.h"` to a file it changed. T is int, float, string,
pointer or color.

Everything it cannot prove is printed with its line and left alone, for a hand edit:
an init whose first argument is neither `&EXPR` nor `PTR.get()`, a group subject,
`new lv_subject_t`, `sizeof(lv_subject_t)`, and pointer arithmetic over subject storage
(`&X[i]` or `X.data()` next to `+ - ++ --` or a relational operator, and a subtraction
involving a subject pointer). That arithmetic compiles after the rewrite and is wrong:
`&X[i]` is the handle of a separate heap node. Use the container index instead.

Plan: docs/devel/plans/2026-11-15-lvgl96-subject-storage.md. The rewrite is idempotent.

Usage:
  scripts/subject_codemod.py PATH...          # rewrite these files or directories
  scripts/subject_codemod.py --check PATH...  # report only; exit 1 if anything would change
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from source_strip import blank_cxx  # noqa: E402

SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")
INCLUDE_LINE = '#include "core/subject.h"'
INIT_TYPES = ("int", "float", "string", "pointer", "color")

DECL = re.compile(r"\blv_subject_t(?=\s+(?!\*|&)[A-Za-z_]\w*\s*(?:\[[^\]]*\]\s*)*(?:\{[^}]*\}\s*)?[;,=)])")
CONTAINER = re.compile(r"([<,]\s*(?:const\s+)?)lv_subject_t(?=\s*(?:\[\s*\])?\s*[,>])")
INIT = re.compile(r"\blv_subject_init_(\w+)\s*\(")
DEINIT = re.compile(r"\blv_subject_deinit\s*\(")
COPY_STRING = re.compile(r"\blv_subject_copy_string\b")
REF_PARAM = re.compile(r"\blv_subject_t\s*&\s*([A-Za-z_]\w*)")
NEW_SUBJECT = re.compile(r"\bnew\s+lv_subject_t\b")
SIZEOF_SUBJECT = re.compile(r"\bsizeof\s*\(\s*lv_subject_t\s*\)")
ARRAY_DECL_NAME = re.compile(
    r"\b(?:lv_subject_t|helix::Subject)\s+([A-Za-z_]\w*)\s*\[|"
    r"<\s*(?:lv_subject_t|helix::Subject)\s*(?:,[^>]*)?>\s*([A-Za-z_]\w*)")
POINTER_NAME = re.compile(r"\blv_subject_t\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)")


def matching_paren(code: str, open_idx: int) -> int:
    """Index of the ')' closing the '(' at open_idx, or -1."""
    depth = 0
    for i in range(open_idx, len(code)):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def split_first_arg(code: str, start: int, end: int) -> int:
    """Index of the top-level comma ending the first argument in code[start:end], or end."""
    depth = 0
    for i in range(start, end):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            return i
    return end


def line_no(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


class Result:
    def __init__(self) -> None:
        self.manual: list[tuple[int, str]] = []
        self.changed = False


def rewrite(text: str, rel: str = "") -> tuple[str, Result]:
    res = Result()
    code = blank_cxx(text)
    edits: list[tuple[int, int, str]] = []
    lines = text.splitlines()

    def manual(offset: int, why: str) -> None:
        n = line_no(text, offset)
        if any(seen == n for seen, _ in res.manual):
            return
        snippet = lines[n - 1].strip() if n - 1 < len(lines) else ""
        res.manual.append((n, f"{why}: {snippet[:100]}"))

    for m in DECL.finditer(code):
        edits.append((m.start(), m.end(), "helix::Subject"))
    for m in CONTAINER.finditer(code):
        s = m.start() + len(m.group(1))
        edits.append((s, s + len("lv_subject_t"), "helix::Subject"))
    for m in COPY_STRING.finditer(code):
        edits.append((m.start(), m.end(), "lv_subject_set_string"))

    init_ref_names: set[str] = set()
    for m in INIT.finditer(code):
        kind = m.group(1)
        open_idx = m.end() - 1
        close_idx = matching_paren(code, open_idx)
        if close_idx < 0:
            manual(m.start(), "unbalanced init call")
            continue
        if kind not in INIT_TYPES:
            manual(m.start(), f"lv_subject_init_{kind}: hand conversion")
            continue
        comma = split_first_arg(code, open_idx + 1, close_idx)
        first = text[open_idx + 1:comma].strip()
        rest = text[comma + 1:close_idx].strip() if comma < close_idx else ""
        target = owner_expr(first)
        if target is None:
            manual(m.start(), "init on a pointer the codemod cannot follow")
            continue
        if first.startswith("&"):
            name = re.match(r"&\s*([A-Za-z_]\w*)\s*$", first)
            if name:
                init_ref_names.add(name.group(1))
        edits.append((m.start(), close_idx + 1, f"{target}init_{kind}({rest})"))

    for m in DEINIT.finditer(code):
        open_idx = m.end() - 1
        close_idx = matching_paren(code, open_idx)
        if close_idx < 0:
            manual(m.start(), "unbalanced deinit call")
            continue
        arg = text[open_idx + 1:close_idx].strip()
        target = owner_expr(arg)
        if target is not None and arg.startswith("&"):
            name = re.match(r"&\s*([A-Za-z_]\w*)\s*$", arg)
            if name:
                init_ref_names.add(name.group(1))
            edits.append((m.start(), close_idx + 1, f"{target}reset()"))
        else:
            edits.append((m.start(), close_idx + 1, f"helix::subject_reset({arg})"))

    for m in REF_PARAM.finditer(code):
        if m.group(1) in init_ref_names:
            edits.append((m.start(), m.start() + len("lv_subject_t"), "helix::Subject"))

    for m in NEW_SUBJECT.finditer(code):
        manual(m.start(), "new lv_subject_t: use std::make_unique<helix::Subject>()")
    for m in SIZEOF_SUBJECT.finditer(code):
        manual(m.start(), "sizeof(lv_subject_t)")
    flag_pointer_arithmetic(code, manual)

    out = apply_edits(text, edits)
    if out != text:
        res.changed = True
        out = add_include(out)
    return out, res


def owner_expr(first: str) -> str | None:
    """`EXPR.` for `&EXPR`, `PTR->` for `PTR.get()`, else None."""
    if first.startswith("&"):
        expr = first[1:].strip()
        if not expr:
            return None
        return f"{expr}." if is_postfix(expr) else f"({expr})."
    get = re.fullmatch(r"([A-Za-z_][\w.\->\[\]]*)\.get\(\)", first)
    if get:
        return f"{get.group(1)}->"
    return None


def is_postfix(expr: str) -> bool:
    """True when `expr` is a name followed only by `.x`, `->x`, `::x`, `[...]` or `(...)`,
    so `expr.member` binds as written."""
    flat = expr
    while True:
        nxt = re.sub(r"\[[^\[\]()]*\]|\([^\[\]()]*\)", "", flat)
        if nxt == flat:
            break
        flat = nxt
    return re.fullmatch(r"[A-Za-z_]\w*(?:(?:\.|->|::)[A-Za-z_]\w*)*", flat.replace(" ", "")) is not None


def flag_pointer_arithmetic(code: str, manual) -> None:
    arrays = {a or b for a, b in ARRAY_DECL_NAME.findall(code)}
    pointers = set(POINTER_NAME.findall(code))
    if arrays:
        alt = "|".join(sorted(map(re.escape, arrays)))
        elem = rf"&\s*(?:[A-Za-z_]\w*(?:\.|->))*(?:{alt})\s*\[[^\]]*\]"
        data = rf"(?:[A-Za-z_]\w*(?:\.|->))*(?:{alt})\s*\.\s*data\s*\(\s*\)"
        rel = r"(?:\+\+|--|\+(?!\+)|-(?![->])|<=?|>=?)"
        pat = re.compile(rf"(?:{elem}|{data})\s*{rel}|{rel}\s*(?:{elem}|{data})")
        for m in pat.finditer(code):
            if "&&" in code[max(0, m.start() - 1):m.end() + 1]:
                continue
            manual(m.start(), "pointer arithmetic over subject storage")
    if pointers:
        alt = "|".join(sorted(map(re.escape, pointers)))
        pat = re.compile(rf"\b(?:{alt})\s*-(?![->=])\s*[&A-Za-z_]|[\w)\]]\s*-(?![->=])\s*(?:{alt})\b")
        for m in pat.finditer(code):
            manual(m.start(), "subtraction involving a subject pointer")


def apply_edits(text: str, edits: list[tuple[int, int, str]]) -> str:
    edits = sorted(set(edits), key=lambda e: e[0])
    out: list[str] = []
    pos = 0
    for start, end, repl in edits:
        if start < pos:
            continue
        out.append(text[pos:start])
        out.append(repl)
        pos = end
    out.append(text[pos:])
    return "".join(out)


def add_include(text: str) -> str:
    if INCLUDE_LINE in text:
        return text
    lines = text.split("\n")
    quoted = [i for i, ln in enumerate(lines) if ln.startswith('#include "')]
    if quoted:
        lines.insert(quoted[-1] + 1, INCLUDE_LINE)
    else:
        any_inc = [i for i, ln in enumerate(lines) if ln.startswith("#include")]
        at = any_inc[0] if any_inc else next(
            (i + 1 for i, ln in enumerate(lines) if ln.startswith("#pragma once")), 0)
        lines.insert(at, INCLUDE_LINE)
    return "\n".join(lines)


def iter_files(paths: list[str]):
    for p in paths:
        path = Path(p)
        if path.is_dir():
            for f in sorted(path.rglob("*")):
                if f.is_file() and f.suffix in SUFFIXES:
                    yield f
        elif path.is_file():
            yield path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--check", action="store_true", help="report only, change nothing")
    args = parser.parse_args()

    changed = 0
    manual_total = 0
    for f in iter_files(args.paths):
        text = f.read_text(encoding="utf-8", errors="replace")
        out, res = rewrite(text, str(f))
        for n, why in res.manual:
            print(f"{f}:{n}: MANUAL {why}")
        manual_total += len(res.manual)
        if res.changed:
            changed += 1
            if args.check:
                print(f"{f}: would change")
            else:
                f.write_text(out, encoding="utf-8")
    verb = "would change" if args.check else "changed"
    print(f"{changed} file(s) {verb}, {manual_total} site(s) left for a hand edit.")
    return 1 if args.check and changed else 0


if __name__ == "__main__":
    sys.exit(main())
