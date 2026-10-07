#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: every name C++ passes to find_required() exists in the XML it searches,
in EVERY layout variant.

helix::ui::find_required() aborts a --test or unit-test run when a name is
missing, but only on the paths a run executes. This gate covers the rest
statically. For each C++ unit (a src/ .cpp plus the header of the same stem
under include/) it collects:

  - the components the unit creates: the component literal passed to
    lv_xml_create(), create_overlay_from_xml() or create_xml_hidden(), and the literal a
    component-name override returns (xml_component(), xml_component_name(),
    get_xml_component_name(), or a modal's component_name());
  - the literal names it passes to find_required() (a non-literal name is left
    to the runtime check).

Each name must appear as name="..." in one of those components, or in a
component it instantiates (transitively), under every layout the app can
resolve: the base files and each LayoutManager variant chain, parsed from
src/layout_manager.cpp#variant_chain itself, where a variant file shadows the base
file of the same relative path.

When the component is chosen at runtime (a row whose component depends on the
declared setting type), the call site names the candidates in an annotation on
the call's line or the line above it:

    // required-names: setting_toggle_row setting_slider_row

and the name must then exist in every candidate, in every variant.

Names looked up in plugin-supplied XML use find_optional(), which this gate
never reads: a third party's XML cannot fail our build.

Usage:
  check_required_names.py                    # report
  check_required_names.py --baseline FILE    # fail on a miss not listed there
  check_required_names.py --baseline FILE --write-baseline
  check_required_names.py --repo-root DIR    # scan another tree
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

def variant_chains(root: Path) -> dict[str, list[str]]:
    """Layout name -> variant directory chain, read from
    src/layout_manager.cpp#variant_chain so the gate cannot drift from the app.
    The empty chain is the base (standard) layout."""
    text = (root / "src" / "layout_manager.cpp").read_text()
    m = re.search(r"LayoutManager::variant_chain\(\)\s*const\s*\{(.*?)\n\}", text, re.S)
    if not m:
        sys.exit("check_required_names: cannot find LayoutManager::variant_chain()")
    chains = {}
    for body in re.findall(r"return\s*\{([^}]*)\};", m.group(1)):
        chain = re.findall(r'"(\w+)"', body)
        chains[chain[0] if chain else "standard"] = chain
    if "standard" not in chains or len(chains) < 2:
        sys.exit("check_required_names: variant_chain() did not parse into layouts")
    return chains


# Elements whose name= declares something other than a widget.
DECLARATION_TAGS = {"style", "px", "percentage", "const", "color", "string", "prop", "subject",
                    "int", "float", "bool", "api", "consts", "styles", "subjects", "font",
                    "image", "images", "fonts", "enumdef", "enum", "gradients", "gradient"}

ANNOTATION_RE = re.compile(r"//\s*required-names:\s*([\w\s]+)")
XML_COMPONENT_RE = re.compile(
    r'\b(?:xml_component|xml_component_name|get_xml_component_name|component_name)\(\)'
    r'[^{;]*\{\s*return\s+"([\w]+)"\s*;')


def call_args(text: str, open_paren: int) -> tuple[list[str], int]:
    """Top-level argument texts of the call whose '(' is at open_paren, and the
    index just past its ')'. Strings and nested calls are skipped whole."""
    args, depth, start, i = [], 0, open_paren + 1, open_paren
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i].strip())
                return args, i + 1
        elif c == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
        i += 1
    return args, i


def literal(arg: str) -> str | None:
    m = re.fullmatch(r'"([\w]+)"', arg)
    return m.group(1) if m else None


def calls(text: str, func: str):
    """(line, args) for each call of func in text."""
    for m in re.finditer(r"\b" + func + r"\s*\(", text):
        args, _ = call_args(text, m.end() - 1)
        yield text.count("\n", 0, m.start()) + 1, args


_COMMENT_OR_STRING = re.compile(r'"(?:\\.|[^"\\\n])*"|/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(text: str) -> str:
    # Blank out comments but keep line numbers; a "//" inside a string literal
    # (a URL) is not a comment.
    def blank(m: re.Match) -> str:
        t = m.group(0)
        if t[0] == '"':
            return t
        return re.sub(r"[^\n]", " ", t) if t[1] == "*" else ""

    return _COMMENT_OR_STRING.sub(blank, text)


class XmlTree:
    def __init__(self, root: Path, chains: dict[str, list[str]]):
        self.ui_xml = root / "ui_xml"
        variant_dirs = {d for chain in chains.values() for d in chain}
        # component name -> path relative to ui_xml (base location)
        self.components: dict[str, str] = {}
        for p in sorted(self.ui_xml.rglob("*.xml")):
            rel = p.relative_to(self.ui_xml)
            if rel.parts[0] in variant_dirs or rel.parts[0] == "translations":
                continue
            self.components.setdefault(p.stem, str(rel))
        # Variant-only components still resolve inside their own chain.
        for p in sorted(self.ui_xml.rglob("*.xml")):
            rel = p.relative_to(self.ui_xml)
            if rel.parts[0] in variant_dirs and len(rel.parts) > 1:
                self.components.setdefault(p.stem, str(Path(*rel.parts[1:])))
        self._parsed: dict[Path, tuple[set[str], set[str]]] = {}

    def resolve(self, component: str, chain: list[str]) -> Path | None:
        rel = self.components.get(component)
        if rel is None:
            return None
        for d in chain:
            p = self.ui_xml / d / rel
            if p.is_file():
                return p
        p = self.ui_xml / rel
        return p if p.is_file() else None

    def parse(self, path: Path) -> tuple[set[str], set[str]]:
        """(widget names, tags) declared in one file."""
        if path not in self._parsed:
            names: set[str] = set()
            tags: set[str] = set()
            try:
                tree = ET.parse(path)
            except ET.ParseError:
                self._parsed[path] = (names, tags)
                return self._parsed[path]

            def walk(el, in_decl: bool):
                in_decl = in_decl or el.tag in DECLARATION_TAGS
                if not in_decl:
                    tags.add(el.tag)
                    if el.get("extends"):
                        tags.add(el.get("extends"))
                    n = el.get("name")
                    if n and "$" not in n:
                        names.add(n)
                for child in el:
                    walk(child, in_decl)

            walk(tree.getroot(), False)
            self._parsed[path] = (names, tags)
        return self._parsed[path]

    def names(self, component: str, chain: list[str]) -> set[str] | None:
        """Every widget name component instantiates under chain, or None when
        the component has no XML file."""
        if self.resolve(component, chain) is None:
            return None
        out: set[str] = set()
        seen: set[str] = set()
        stack = [component]
        while stack:
            c = stack.pop()
            if c in seen:
                continue
            seen.add(c)
            p = self.resolve(c, chain)
            if p is None:
                continue
            names, tags = self.parse(p)
            out |= names
            stack.extend(t for t in tags if t in self.components)
        return out


def cpp_units(root: Path):
    headers = {p.stem: p for p in (root / "include").rglob("*.h")} if (root / "include").is_dir() else {}
    for cpp in sorted((root / "src").rglob("*.cpp")):
        files = [cpp]
        if cpp.stem in headers:
            files.append(headers[cpp.stem])
        yield cpp, files


def check(root: Path) -> list[tuple[str, str, str]]:
    """(site, name, reason) for every miss."""
    chains = variant_chains(root)
    xml = XmlTree(root, chains)
    misses = []
    for cpp, files in cpp_units(root):
        raw = {f: f.read_text(errors="replace") for f in files}
        if "find_required" not in raw[cpp]:
            continue
        created: set[str] = set()
        for f, text in raw.items():
            code = strip_comments(text)
            for func in ("lv_xml_create", "create_overlay_from_xml", "create_xml_hidden"):
                for _, args in calls(code, func):
                    if len(args) > 1 and literal(args[1]):
                        created.add(literal(args[1]))
            created |= set(XML_COMPONENT_RE.findall(code))

        lines = raw[cpp].splitlines()
        code = strip_comments(raw[cpp])
        rel = cpp.relative_to(root)
        for line, args in calls(code, "find_required"):
            if len(args) < 2:
                continue  # the declaration or a macro, not a lookup
            name = literal(args[1])
            if name is None:
                continue
            site = f"{rel}:{line}"
            annotation = None
            for ln in (line, line - 1):
                if 0 < ln <= len(lines):
                    m = ANNOTATION_RE.search(lines[ln - 1])
                    if m:
                        annotation = m.group(1).split()
            if annotation:
                for comp in annotation:
                    for variant, chain in chains.items():
                        found = xml.names(comp, chain)
                        if found is None:
                            misses.append((site, name, f"annotated component '{comp}' has no XML"))
                            break
                        if name not in found:
                            misses.append((site, name, f"not in '{comp}' ({variant} layout)"))
                continue
            if not created:
                misses.append((site, name, "the unit creates no XML component this gate can see; "
                                           "add a // required-names: annotation"))
                continue
            ok = False
            missing_in = []
            for comp in sorted(created):
                absent = [v for v, chain in chains.items()
                          if name not in (xml.names(comp, chain) or set())]
                if not absent:
                    ok = True
                    break
                missing_in.append(f"{comp} ({', '.join(absent)})")
            if not ok:
                misses.append((site, name, "missing from " + "; ".join(missing_in)))
    return misses


def key(site: str, name: str) -> str:
    return f"{site.split(':')[0]}|{name}"


def read_baseline(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    return {ln.strip() for ln in path.read_text().splitlines()
            if ln.strip() and not ln.startswith("#")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args()

    misses = check(args.repo_root.resolve())

    if args.write_baseline:
        if not args.baseline:
            print("--write-baseline needs --baseline PATH")
            return 2
        header = ("# find_required() names missing from their XML in some layout variant.\n"
                  "# Accepted debt, shrink-only: fix the XML or switch the site to\n"
                  "# find_optional(), then re-run with --write-baseline.\n")
        body = "".join(k + "\n" for k in sorted({key(s, n) for s, n, _ in misses}))
        args.baseline.write_text(header + body)
        print(f"wrote {args.baseline}: {len(misses)} misses")
        return 0

    accepted = read_baseline(args.baseline) if args.baseline else set()
    new = [(s, n, r) for s, n, r in misses if key(s, n) not in accepted]
    if new:
        print(f"❌ Required names: {len(new)} find_required() name(s) missing from the XML:")
        for s, n, r in new:
            print(f"     {s}: '{n}' {r}")
        print("   Fix the XML (every layout variant), or use find_optional() for a name "
              "that may legitimately be absent.")
        return 1
    print(f"✅ Required names: every find_required() literal exists in every layout variant "
          f"({len(misses)} accepted in baseline)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
