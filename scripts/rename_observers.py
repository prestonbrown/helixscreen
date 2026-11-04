#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rewrite the retired core/observer_factory.h names onto observe<V>(...).

    observe_int_sync<P>(a)          -> observe<int>(a)
    observe_string<P>(a)            -> observe<const char*>(a)
    observe_int_immediate<P>(a)     -> observe<int>(a, Dispatch::Immediate)
    observe_string_immediate<P>(a)  -> observe<const char*>(a, Dispatch::Immediate)
    observe_print_state_immediate<P>(a) -> observe_print_state(a, Dispatch::Immediate)
    using helix::ui::observe_int_sync;  -> using helix::ui::observe;

The owner type is deduced from the second argument, so the explicit <P> goes away.
Idempotent: a tree with no old names is left untouched, which is how a branch that
still writes the old spelling catches up after rebasing onto this change:

    scripts/rename_observers.py            # every tracked file
    scripts/rename_observers.py PATH...    # just these
    scripts/rename_observers.py --check    # exit 1 if anything would change

observe_int_async has no mechanical rewrite (its update half needs an explicit
lifetime-guarded defer); it is reported and left alone.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (value type spelling, Immediate?) per retired name
NAMES = {
    "observe_int_sync": ("observe<int>", False),
    "observe_string": ("observe<const char*>", False),
    "observe_int_immediate": ("observe<int>", True),
    "observe_string_immediate": ("observe<const char*>", True),
    "observe_print_state_immediate": ("observe_print_state", True),
}
CODE_EXT = {".cpp", ".h", ".hpp", ".cc", ".inl"}
DOC_EXT = {".md"}
SKIP_PREFIXES = (
    "lib/",
    "build/",
    ".worktrees/",
    "docs/devel/plans/",
    "docs/superpowers/",
    "tests/shell/",
    "scripts/rename_observers.py",
    "scripts/CLAUDE.md",
    "include/core/observer_factory.h",
    "src/core/observer_factory.cpp",
)

NAME_RE = re.compile(
    r"(?<![\w])((?:helix::ui::|ui::)?)(observe_(?:int_sync|int_immediate|string_immediate|"
    r"string|print_state_immediate))\b"
)
USING_RE = re.compile(
    r"^(?P<indent>\s*)using\s+helix::ui::observe_"
    r"(?:int_sync|int_immediate|string_immediate|string|print_state_immediate)\s*;"
    r"[ \t]*$"
)
USING_STATE_RE = re.compile(r"^\s*using\s+helix::ui::observe_print_state\s*;")


def skip_literal(s, i):
    """If s[i] opens a string, char literal or comment, return the index past it."""
    c = s[i]
    if c in "\"'":
        j = i + 1
        while j < len(s) and s[j] != c:
            j += 2 if s[j] == "\\" else 1
        return j + 1
    if s.startswith("//", i):
        j = s.find("\n", i)
        return len(s) if j < 0 else j
    if s.startswith("/*", i):
        j = s.find("*/", i + 2)
        return len(s) if j < 0 else j + 2
    return i


def match_close(s, i, open_c, close_c):
    """s[i] == open_c; return the index of its matching close_c, or -1."""
    depth = 0
    while i < len(s):
        j = skip_literal(s, i)
        if j != i:
            i = j
            continue
        if s[i] == open_c:
            depth += 1
        elif s[i] == close_c:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def rewrite_code(text):
    out, pos = [], 0
    for m in NAME_RE.finditer(text):
        if m.start() < pos:
            continue
        qual, name = m.group(1), m.group(2)
        spelled, immediate = NAMES[name]
        j = m.end()
        k = j
        while k < len(text) and text[k] in " \t":
            k += 1
        # explicit owner template args: observe_int_sync<Panel>(
        end_tmpl = j
        if k < len(text) and text[k] == "<":
            close = match_close(text, k, "<", ">")
            if close > 0 and text[close + 1 : close + 12].lstrip(" \t").startswith("("):
                end_tmpl = close + 1
        p = end_tmpl
        while p < len(text) and text[p] in " \t":
            p += 1
        is_call = p < len(text) and text[p] == "("
        out.append(text[pos : m.start()])
        out.append(qual + spelled)
        pos = end_tmpl
        if is_call and immediate:
            close = match_close(text, p, "(", ")")
            if close > 0:
                args = text[p + 1 : close]
                disp = qual + "Dispatch::Immediate"
                if not args.strip():
                    new_args = "..., " + disp
                else:
                    trail = args[len(args.rstrip()) :]
                    new_args = args.rstrip() + ", " + disp + trail
                out.append(text[end_tmpl:p])
                out.append("(" + new_args + ")")
                pos = close + 1
        elif not is_call and end_tmpl == j:
            pass
    out.append(text[pos:])
    return "".join(out)


def rewrite_usings(text):
    lines, out, seen, need_dispatch = text.split("\n"), [], set(), False
    for ln in lines:
        m = USING_RE.match(ln)
        if not m:
            out.append(ln)
            continue
        if "immediate" in ln:
            need_dispatch = True
        new = m.group("indent") + "using helix::ui::observe;"
        # Only file-scope duplicates collapse; a using inside a function body stays.
        if m.group("indent") or new not in seen:
            seen.add(new)
            out.append(new)
    text = "\n".join(out)
    if need_dispatch and "using helix::ui::Dispatch;" not in text:
        text = text.replace(
            "using helix::ui::observe;", "using helix::ui::Dispatch;\nusing helix::ui::observe;", 1
        )
    return text


def process(path):
    text = path.read_text()
    new = text
    if "observe_" in new:
        new = rewrite_usings(new) if path.suffix in CODE_EXT else new
        new = rewrite_code(new)
    return text, new


def tracked_files():
    out = subprocess.run(
        ["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout
    return [f for f in out.split("\0") if f]


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if not a.startswith("--")]
    rels = args or tracked_files()
    changed, async_left = [], []
    for rel in rels:
        rel = str(Path(rel).resolve().relative_to(ROOT)) if Path(rel).is_absolute() else rel
        p = ROOT / rel
        if rel.startswith(SKIP_PREFIXES) or p.is_symlink() or not p.is_file():
            continue
        if p.suffix not in CODE_EXT | DOC_EXT:
            continue
        try:
            old, new = process(p)
        except UnicodeDecodeError:
            continue
        if "observe_int_async" in new:
            async_left.append(rel)
        if new != old:
            changed.append(rel)
            if not check:
                p.write_text(new)
    for rel in async_left:
        print(f"observe_int_async has no mechanical rewrite: {rel}", file=sys.stderr)
    if check:
        for rel in changed:
            print(f"retired observer name: {rel}")
        return 1 if changed or async_left else 0
    fmt = shutil.which("clang-format")
    code = [r for r in changed if Path(r).suffix in CODE_EXT]
    if fmt and code:
        subprocess.run([fmt, "-i", *code], cwd=ROOT, check=True)
    print(f"rewrote {len(changed)} files ({len(code)} formatted)")
    return 1 if async_left else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
