#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every member a firmware vtable or constructor names has a firmware definition.

The ESP32 firmware leaves the files in app_srcs_excluded.txt out of the image and,
where the app still needs a class one of them implements, defines that class's
members in its own sources instead (task10_pending_stubs.cpp and the other stub
files under firmware/helixscreen-esp32/). The compiler emits a class's vtable in
the unit that defines its key function, the first virtual the class declares
without an inline body, and a constructor or destructor needs that vtable. Once
a firmware source defines any of them, the vtable is in the image and names
every virtual the class has. A virtual an excluded file defines and no firmware
source does is then an undefined reference at the ELF link, although no firmware
code calls it by name: callers reach it through the interface.

`check_esp32_app_srcs.py --link` sees the references native objects record. A
stub file has no native object, and on the host the vtable lives in the excluded
file's object, so that check cannot see this; this gate reads source instead.

WHAT IS FLAGGED
  A `Class::member` an excluded file defines out of line, that no firmware-compiled
  source (app_srcs.txt, a repo source a firmware CMakeLists.txt names, or a source
  under firmware/helixscreen-esp32/) defines, where either
    - member is declared `virtual`, `override` or `final` in Class's definition,
      and a firmware-compiled source defines Class's key function, a constructor
      or the destructor, or
    - member is the constructor, and a source under firmware/helixscreen-esp32/
      constructs Class (make_unique, make_shared, new, or a value).

NOT FLAGGED
  - A non-virtual member a listed file calls: the native objects record that
    call, and `check_esp32_app_srcs.py --link` checks it.
  - A class the excluded file defines itself, and the constructor of a class name
    more than one header defines: source alone cannot tell those apart.
  - A member whose definition line, or the line above it, carries
    `ESP32_STUB_OK: <reason>`.

Classes are matched by their unqualified name, the way the stubs spell them.

usage: check_esp32_stub_coverage.py [--manifest F] [--exclusions F] [--src-root D]
                                    [--firmware-root D] [--if-touched PATH...]
--if-touched (the commit hook) checks only when one of PATHs can change the answer:
a header, an excluded source, a firmware-tree file, or this gate's own inputs.
Exit 0 clean, 1 on a finding.
"""

from __future__ import annotations

import argparse
import functools
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_esp32_app_srcs import (  # noqa: E402
    DEFAULT_EXCLUSIONS, DEFAULT_FIRMWARE_ROOT, DEFAULT_MANIFEST, DEFAULT_SRC_ROOT,
    QUALIFIED_CALLEE, STRING_LITERAL, defined_functions, display, firmware_code_lines,
    firmware_defines, firmware_local_code, is_excluded, load_exclusions, load_manifest,
    scan_universe, split_exclusions)

MARKER = "ESP32_STUB_OK"
HEADER_EXTS = (".h", ".hpp")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SCOPE = re.compile(r"(\w+)::")
QUALIFIED = re.compile(r"(\w+)::(~?\w+)\s*\(")


def member_of(name: str) -> tuple[str, str] | None:
    """`ns::Cls::fn` -> ('Cls', 'fn'); a free function -> None."""
    parts = name.split("::")
    return (parts[-2], parts[-1]) if len(parts) > 1 else None


def class_members(names: set[str]) -> set[tuple[str, str]]:
    return {m for n in names if (m := member_of(n))}


def stripped(text: str) -> str:
    code = STRING_LITERAL.sub('""', text)
    return COMMENT.sub(" ", code)


def matching_brace(code: str, open_at: int) -> int:
    depth = 0
    for k in range(open_at, len(code)):
        depth += {"{": 1, "}": -1}.get(code[k], 0)
        if depth == 0:
            return k
    return len(code)


def member_declarations(body: str) -> list[tuple[str, bool]]:
    """(declaration, has an inline body) for each of the class body's own members."""
    out, start, k = [], 0, 0
    while k < len(body):
        c = body[k]
        if c == "{":
            out.append((body[start:k], True))
            k = matching_brace(body, k) + 1
            start = k
            continue
        if c == ";":
            out.append((body[start:k], False))
            start = k + 1
        k += 1
    out.append((body[start:], False))
    return out


def declared_name(decl: str, member: str) -> re.Match | None:
    return re.search(r"(?<![\w:])" + re.escape(member) + r"\s*\(", decl)


def is_virtual_decl(decl: str, m: re.Match) -> bool:
    return bool(re.search(r"\bvirtual\b", decl[:m.start()])
                or re.search(r"\)[^()]*\b(?:override|final)\b", decl[m.end():]))


FUNCTION_NAME = re.compile(r"(?<![\w:])(~?\w+)\s*\(")


CLASS_HEAD = re.compile(r"\b(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?(?:\w+\s+)?(\w+)"
                        r"(?:\s+final)?\s*(?::[^;{]*)?\{")


class ClassIndex:
    """Class name -> the member declarations of each definition the headers hold."""

    def __init__(self, roots: list[Path]):
        self.heads: dict[str, list[tuple[int, int]]] = {}
        self.cache: dict[str, list[list[tuple[str, bool]]]] = {}
        self.code: list[str] = []
        for r in roots:
            for p in sorted(r.rglob("*") if r.is_dir() else []):
                if p.suffix not in HEADER_EXTS or "/lib/" in str(p):
                    continue
                raw = p.read_text(errors="replace")
                if "class" not in raw and "struct" not in raw:
                    continue
                code = stripped(raw)
                self.code.append(code)
                for m in CLASS_HEAD.finditer(code):
                    self.heads.setdefault(m.group(1), []).append(
                        (len(self.code) - 1, m.end() - 1))

    def definitions(self, cls: str) -> list[list[tuple[str, bool]]]:
        if cls not in self.cache:
            self.cache[cls] = [
                member_declarations(self.code[i][at + 1:matching_brace(self.code[i], at)])
                for i, at in self.heads.get(cls, [])]
        return self.cache[cls]

    def is_virtual(self, cls: str, member: str) -> bool:
        for decls in self.definitions(cls):
            for d, _ in decls:
                m = declared_name(d, member)
                if m and is_virtual_decl(d, m):
                    return True
        return False

    def key_function(self, cls: str) -> str | None:
        """The first virtual the class declares without defining it: its vtable's home.

        The compiler emits the vtable in the unit that defines this member; a class
        without one (every virtual inline or pure) emits it wherever it is constructed.
        """
        for decls in self.definitions(cls):
            for d, has_body in decls:
                m = FUNCTION_NAME.search(d)
                if (m and not has_body and is_virtual_decl(d, m)
                        and not re.search(r"\)[^()]*=\s*(?:0|default|delete)\s*$", d)):
                    return m.group(1)
        return None


def constructs(code: str, cls: str) -> bool:
    """Whether firmware-tree code builds a `cls`: make_unique/shared, new, or a value."""
    c = re.escape(cls)
    return bool(re.search(rf"\bmake_(?:unique|shared)\s*<\s*(?:\w+::)*{c}\s*>"
                          rf"|\bnew\s+(?:\w+::)*{c}\b"
                          rf"|(?<![\w:~]){c}\s+\w+\s*[({{;]"
                          rf"|(?<![\w:~]){c}\s*\{{", code))


def waived(text: str) -> set[tuple[str, str]]:
    """Members whose definition line, or the line above it, carries the marker."""
    lines = text.splitlines()
    out = set()
    for i, line in enumerate(lines):
        if MARKER in line:
            for near in lines[i:i + 2]:
                out |= set(QUALIFIED.findall(near))
    return out


def files_naming(base: Path, files: list[str], classes: set[str]) -> list[str]:
    """The files whose text holds `Cls::` for one of `classes` (a superset: grep -F)."""
    if not files:
        return []
    res = subprocess.run(["grep", "-lF", "-f", "-", "--", *files], cwd=base, text=True,
                         input="".join(f"{c}::\n" for c in sorted(classes)),
                         capture_output=True)
    if res.returncode > 1:
        sys.exit(f"grep failed: {res.stderr.strip()}")
    return res.stdout.split()


@dataclass(frozen=True)
class Finding:
    excluded: str
    cls: str
    member: str
    why: str
    where: str


def compute(manifest: Path, exclusions: Path, src_root: Path,
            firmware_root: Path) -> list[Finding]:
    base = src_root.parent
    included, _ = load_manifest(manifest)
    ex_files, ex_dirs = split_exclusions(load_exclusions(exclusions))
    universe = scan_universe(src_root)
    # A component's CMakeLists names some repo sources itself (helixnet's, the mock
    # printer's), under a Kconfig switch or not: each is a firmware definition.
    named = {m for c in firmware_root.rglob("CMakeLists.txt")
             for m in re.findall(r"\$\{REPO_ROOT\}/(src/[\w./-]+\.cpp)", c.read_text())}
    included |= named
    listed = sorted(f for f in included if (base / f).is_file() and f.endswith(".cpp"))
    excluded = sorted(f for f in universe if f not in included and f.endswith(".cpp")
                      and is_excluded(f, ex_files, ex_dirs))

    cmake = manifest.parent / "CMakeLists.txt"
    cmake_text = cmake.read_text() if cmake.exists() else ""
    defines = firmware_defines(cmake_text, base)
    include_dirs = [base / d for d in re.findall(r"\$\{REPO_ROOT\}/([\w./-]+)", cmake_text)]

    def code_of(path: Path) -> str:
        return "\n".join(c for _, c in firmware_code_lines(path.read_text(errors="replace"),
                                                           defines))

    local_code = {display(p): c for p, c in
                  firmware_local_code(firmware_root, defines, include_dirs).items()}
    wanted: dict[str, set[tuple[str, str]]] = {}
    waivers: dict[str, set[tuple[str, str]]] = {}
    for f in excluded:
        text = (base / f).read_text(errors="replace")
        if "::" not in text:
            continue
        code = stripped(text)
        # A class the file itself defines is local to it: nothing else can reach it.
        local = set(CLASS_HEAD.findall(code))
        wanted[f] = {(cls, member)
                     for cls, member in class_members(defined_functions(code, QUALIFIED_CALLEE))
                     if cls not in local}
        waivers[f] = waived(text)
    index = ClassIndex([base / "include", src_root, firmware_root])
    # Only a constructor or a virtual can be reached without a call the native objects see.
    classes = {cls for members in wanted.values() for cls, member in members
               if cls in index.heads and (member == cls or index.is_virtual(cls, member))}
    if not classes:
        return []

    # Only a firmware source that names one of those classes can define its members.
    sources = {f: c for f, c in local_code.items()
               if not classes.isdisjoint(SCOPE.findall(c))}
    for f in files_naming(base, listed, classes):
        sources[f] = code_of(base / f)
    defined: dict[tuple[str, str], str] = {}
    for f, code in sources.items():
        for member in class_members(defined_functions(code, QUALIFIED_CALLEE)):
            defined.setdefault(member, f)

    @functools.cache
    def vtable_home(cls: str) -> str | None:
        """The firmware source that emits `cls`'s vtable: its key function's, or a
        constructor's or destructor's, which need that vtable to exist."""
        for member in (index.key_function(cls), cls, "~" + cls):
            if member and (cls, member) in defined:
                return defined[(cls, member)]
        return None

    findings = []
    for f, members in sorted(wanted.items()):
        for cls, member in sorted(members):
            if (cls, member) in defined or (cls, member) in waivers[f]:
                continue
            if member == cls:
                # A name several headers define (nested helpers like `Pending`) cannot
                # be told apart from source, so only a unique class is checked.
                if len(index.heads.get(cls, [])) != 1:
                    continue
                home = next((g for g, c in local_code.items() if cls in c
                             and cls not in CLASS_HEAD.findall(c) and constructs(c, cls)),
                            None)
                if home:
                    findings.append(Finding(f, cls, member, "constructed", home))
            elif (home := vtable_home(cls)) and index.is_virtual(cls, member):
                findings.append(Finding(f, cls, member, "virtual; its vtable is emitted",
                                        home))
    return findings


def touches(paths: list[str], args: argparse.Namespace) -> bool:
    """Whether a change to one of `paths` can change this gate's answer."""
    ex_files, ex_dirs = split_exclusions(load_exclusions(args.exclusions))
    inputs = {display(args.manifest), display(args.exclusions), display(Path(__file__)),
              "scripts/check_esp32_app_srcs.py"}
    fw = display(args.firmware_root).rstrip("/") + "/"
    return any(p.endswith(HEADER_EXTS) or p.startswith(fw) or p in inputs
               or (p.startswith("src/") and is_excluded(p, ex_files, ex_dirs))
               for p in paths)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--exclusions", type=Path, default=DEFAULT_EXCLUSIONS)
    ap.add_argument("--src-root", type=Path, default=DEFAULT_SRC_ROOT)
    ap.add_argument("--firmware-root", type=Path, default=DEFAULT_FIRMWARE_ROOT)
    ap.add_argument("--if-touched", nargs="*", metavar="PATH",
                    help="skip unless one of these repo paths can change the answer")
    args = ap.parse_args()

    if args.if_touched is not None and not touches(args.if_touched, args):
        print("OK: no staged header, excluded source or firmware file.")
        return 0
    findings = compute(args.manifest, args.exclusions, args.src_root, args.firmware_root)
    if not findings:
        print("OK: every member a firmware vtable or constructor needs from an excluded "
              "file has a firmware definition.")
        return 0
    print(f"FAIL: {len(findings)} member(s) the firmware links are defined only in an "
          "app_srcs_excluded.txt file.\n      The ESP32 link has no definition for them:",
          file=sys.stderr)
    for x in findings:
        print(f"        {x.excluded}  {x.cls}::{x.member}  ({x.why} in {x.where})",
              file=sys.stderr)
    print("\n      Stub each beside the class's other firmware definitions, or mark its "
          f"definition\n      `// {MARKER}: <reason>` when the firmware never links it.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
