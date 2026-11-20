#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: XML event callbacks and their C++ registrations agree.

XML names a callback (callback=, *_callback=, event_cb=) and C++ registers a
function under that name in LVGL's one global table. Nothing checks the pair,
and each half fails silently:

  unregistered  XML names a callback no C++ registers: the widget does nothing.
  unreferenced  C++ registers a name no XML uses: dead code that still claims
                the name.
  duplicate     two files register the same name: the table is last-write-wins,
                so which handler runs depends on registration order.

Registrations counted: lv_xml_register_event_cb(scope, "name", ...), every
{"name", ...} entry of a register_xml_callbacks({...}) table, and every
{"name", ...} pair in a file that registers names from its own table in a loop
(a non-literal lv_xml_register_event_cb name). A name built at runtime cannot be
matched statically; its XML references show up as unregistered and sit in the
baseline.

XML references counted: the values of callback=, *_callback= and event_cb=
attributes, and the value half of a C++ "..._callback", "name" attribute pair
handed to lv_xml_create/modal_show. A value forwarded from a component
parameter ($name) is the caller's business and is skipped.

This is a RATCHET keyed on names: --baseline lists today's accepted debt and
says when an entry can be dropped. A new unregistered or duplicate name fails:
each is a widget that misbehaves. A new unreferenced name is bookkeeping, dead
code that harms nothing at runtime, so it warns and exits ratchet.WARN_EXIT (3)
when it is the only finding (prestonbrown/helixscreen#2004).

Usage:
  check_orphan_callbacks.py                    # counts
  check_orphan_callbacks.py --list             # every finding
  check_orphan_callbacks.py --baseline FILE [--write-baseline]
  check_orphan_callbacks.py --repo-root DIR
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path
import ratchet  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_required_names import call_args  # noqa: E402

from source_strip import blank_cxx  # noqa: E402
from qc_file_cache import FileCache  # noqa: E402

XML_REF_RE = re.compile(r'\b(?:[a-z0-9_]*callback|event_cb)="([^"]*)"')
# C++ naming a callback through a component's `$..._callback` prop:
# lv_xml_create(parent, "comp", {"click_callback", "on_x", nullptr}).
CPP_ATTR_REF_RE = re.compile(r'"[a-z0-9_]*callback"\s*,\s*"(\w+)"')
ENTRY_RE = re.compile(r'\{\s*"([A-Za-z_]\w*)"\s*,')


def source_files(root: Path) -> list[Path]:
    return sorted(f for d in ("src", "include") for ext in ("*.cpp", "*.h")
                  for f in (root / d).rglob(ext))


def file_registrations(text: str) -> list[str]:
    """The callback names one source's text registers, in order, repeats kept."""
    code = blank_cxx(text, strings="keep")
    names: list[str] = []
    loop_registers = False
    for m in re.finditer(r"\blv_xml_register_event_cb\s*\(", code):
        args, _ = call_args(code, m.end() - 1)
        if len(args) < 3:
            continue  # a declaration or a wrapper's own body
        lit = re.fullmatch(r'"(\w+)"', args[1])
        if lit:
            names.append(lit.group(1))
        elif args[1] != "cb.name":  # cb.name: register_xml_callbacks itself
            loop_registers = True
    for m in re.finditer(r"\bregister_xml_callbacks\s*\(", code):
        args, end = call_args(code, m.end() - 1)
        if not args or args[0].startswith("std::initializer_list"):
            continue
        names.extend(ENTRY_RE.findall(code[m.end():end]))
    if loop_registers:
        names.extend(ENTRY_RE.findall(code))
    return names


def cpp_attr_refs(text: str) -> list[str]:
    """Callback names a source passes through a component's `$..._callback` prop."""
    return CPP_ATTR_REF_RE.findall(blank_cxx(text, strings="keep"))


# Every script whose code decides a cached per-file answer below.
_SALT = [Path(__file__), Path(__file__).with_name("source_strip.py"),
         Path(__file__).with_name("check_required_names.py")]


def collect_registrations(root: Path, files=None, read=None,
                          read_key: str | None = None) -> dict[str, set[str]]:
    """name -> files registering it.

    `files` narrows the scan to a build's own sources and `read` lets a caller
    preprocess each file's text; both default to src/ and include/ as is.
    Each file's answer is cached by content (qc_file_cache); a caller passing `read`
    names what it does with `read_key`, or its files are parsed afresh.
    """
    regs: dict[str, set[str]] = defaultdict(set)
    cache = FileCache("callback_registrations", _SALT)
    for f in source_files(root) if files is None else files:
        rel = str(f.relative_to(root))
        if read is None:
            names = cache.get(f, "raw", file_registrations)
        elif read_key is not None:
            names = cache.get(f, read_key, lambda _t, f=f: file_registrations(read(f)))
        else:
            names = file_registrations(read(f))
        for name in names:
            regs[name].add(rel)
    cache.save()
    return regs


def collect_xml_refs(root: Path, xml_files=None) -> dict[str, set[str]]:
    """name -> XML files referencing it. xml_files limits the XML read; default ui_xml/."""
    refs: dict[str, set[str]] = defaultdict(set)
    ui_xml = root / "ui_xml"
    if not ui_xml.is_dir():
        return refs
    for f in sorted(ui_xml.rglob("*.xml")) if xml_files is None else sorted(xml_files):
        rel = str(f.relative_to(root))
        for value in XML_REF_RE.findall(f.read_text(errors="replace")):
            if value and "$" not in value and re.fullmatch(r"\w+", value):
                refs[value].add(rel)
    src = root / "src"
    if src.is_dir():
        cache = FileCache("callback_cpp_refs", _SALT)
        for f in sorted(src.rglob("*.cpp")):
            for name in cache.get(f, "raw", cpp_attr_refs):
                refs[name].add(str(f.relative_to(root)))
        cache.save()
    return refs


def findings(root: Path) -> dict[str, str]:
    """'kind:name' -> where."""
    regs = collect_registrations(root)
    refs = collect_xml_refs(root)
    out = {}
    for name, files in regs.items():
        if name not in refs:
            out[f"unreferenced:{name}"] = ", ".join(sorted(files))
        if len(files) > 1:
            out[f"duplicate:{name}"] = ", ".join(sorted(files))
    for name, files in refs.items():
        if name not in regs:
            out[f"unregistered:{name}"] = ", ".join(sorted(files))
    return out


def read_baseline(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    return {ln.strip() for ln in path.read_text().splitlines()
            if ln.strip() and not ln.startswith("#")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args()

    found = findings(args.repo_root.resolve())
    if args.list:
        for k in sorted(found):
            print(f"{k}  ({found[k]})")

    if args.write_baseline:
        if not args.baseline:
            print("--write-baseline needs --baseline PATH")
            return 2
        header = ("# XML event callbacks whose C++ registration and XML use disagree.\n"
                  "# Accepted debt, shrink-only: register it, reference it, give each owner\n"
                  "# its own name, or delete it - then re-run with --write-baseline.\n")
        args.baseline.write_text(header + "".join(k + "\n" for k in sorted(found)))
        print(f"wrote {args.baseline}: {len(found)} findings")
        return 0

    if not args.baseline:
        kinds = defaultdict(int)
        for k in found:
            kinds[k.split(":")[0]] += 1
        print("orphan callbacks: " + ", ".join(f"{n} {k}" for k, n in sorted(kinds.items())))
        return 0

    accepted = read_baseline(args.baseline)
    where = ratchet.shown(args.baseline, args.repo_root)
    new = sorted(set(found) - accepted)
    broken = [k for k in new if not k.startswith("unreferenced:")]
    dead = [k for k in new if k.startswith("unreferenced:")]
    if broken:
        print(f"❌ Orphan callbacks: {len(broken)} not in {where}:")
        for k in broken:
            print(f"     {k}  ({found[k]})")
    if dead:
        print(f"⚠️  Orphan callbacks: {len(dead)} registered name(s) no XML uses, not in "
              f"{where}. Dead code, so a warning: delete the registration, or add "
              f"it with --write-baseline.")
        for k in dead:
            print(f"     {k}  ({found[k]})")
    if broken:
        return 1
    if dead:
        return ratchet.WARN_EXIT
    gone = sorted(accepted - set(found))
    if gone:
        return ratchet.under_baseline(
            "Orphan callbacks", f"{where} (re-run with --write-baseline)",
            [f"{k}: no longer a finding" for k in gone],
            f"✅ Orphan callbacks: {len(found)} accepted, {len(gone)} fewer than "
            f"{where}; re-run with --write-baseline to hold the gain "
            f"({', '.join(gone)}).",
            bookkeeping=True)
    print(f"✅ Orphan callbacks: {len(found)} findings, all accepted debt in {where}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
