#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rewrite first-party code off the LVGL APIs that 9.6 deprecates (#2098).

Two kinds of rewrite, both read from lib/lvgl at run time so the table is LVGL's own:

1. Plain renames from LVGL's API map headers (include/lvgl/api_map/lv_api_map_*.h):
   every `#define OLD NEW` whose NEW is a single identifier, and every
   `typedef NEW OLD;`. Chains resolve to the final name. These are the names
   LV_DISABLE_API_MAPPING removes, less any name lv_conf_internal.h still reads as a
   config option. The map's static inline functions rewrite onto their replacement
   (lv_obj_move_foreground(o) -> lv_obj_move_to_index(o, -1), lv_task_handler ->
   lv_timer_handler). Map entries that are expressions or function-like macros are
   reported, never rewritten.

2. The deprecated flag calls, onto the per-flag functions declared in lv_obj.h:
     lv_obj_add_flag(o, LV_OBJ_FLAG_X)        -> lv_obj_set_x(o, true)
     lv_obj_remove_flag(o, LV_OBJ_FLAG_X)     -> lv_obj_set_x(o, false)
     lv_obj_set_flag(o, LV_OBJ_FLAG_X, v)     -> lv_obj_set_x(o, v)
     lv_obj_has_flag(o, LV_OBJ_FLAG_X)        -> lv_obj_is_x(o)
     lv_obj_has_flag_any(o, LV_OBJ_FLAG_X)    -> lv_obj_is_x(o)
   Only when the flag argument is one literal LV_OBJ_FLAG_* constant whose setter or
   getter exists. An OR-ed mask, a flag held in a variable, a USER/WIDGET/LAYOUT flag
   (no per-flag function exists) or a call the parser cannot split is left as it is
   and reported for a hand edit. A line carrying `LV_DEPRECATED_OK: <reason>` is
   exempt from the report and from --check.

The subject init/deinit API (lv_subject_init_*, lv_subject_deinit) is out of scope:
its only replacement, lv_subject_create(), heap-allocates the subject, which is a
storage change and not a rename.

String literals are never touched. Plain renames also apply inside comments; flag
calls are rewritten in code only. The rewrite is idempotent.

Usage:
  scripts/lvgl96_rename.py                 # rewrite the default first-party tree
  scripts/lvgl96_rename.py PATH...         # rewrite these files or directories
  scripts/lvgl96_rename.py --check [PATH...]   # exit 1 if a deprecated name remains in code
  scripts/lvgl96_rename.py --list-map      # print the rename table and exit
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from source_strip import blank_cxx  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LVGL_INC = os.path.join(REPO, "lib", "lvgl", "include", "lvgl")
DEFAULT_PATHS = ["src", "include", "tests", "lib/helix-xml", "firmware"]
EXTS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".ipp")
SKIP_DIRS = {".git", "build", "managed_components", "node_modules", ".pio"}
OPT_OUT = "LV_DEPRECATED_OK"

FLAG_FUNCS = ("lv_obj_add_flag", "lv_obj_remove_flag", "lv_obj_set_flag",
              "lv_obj_has_flag", "lv_obj_has_flag_any")

# The API map's static inline functions, each read against its LVGL 9.6 replacement:
# lv_obj_move_to_index() counts a negative index from the end and returns early on a
# parentless object, as the inline bodies do.
INLINE_RENAMES = {"lv_task_handler": "lv_timer_handler"}
INLINE_CALLS = {"lv_obj_move_foreground": "lv_obj_move_to_index(%s, -1)",
                "lv_obj_move_background": "lv_obj_move_to_index(%s, 0)"}
CALL_FUNCS = FLAG_FUNCS + tuple(INLINE_CALLS)
IDENT = re.compile(r"[A-Za-z_]\w*")


# ---------------------------------------------------------------- tables

def load_api_map():
    """Return (renames, complex): renames maps old -> final new name; complex is the
    set of mapped names whose replacement is not a single identifier."""
    raw, complex_ = {}, set()
    map_dir = os.path.join(LVGL_INC, "api_map")
    define = re.compile(r"^\s*#\s*define\s+(\w+)(\([^)]*\))?\s*(.*?)\s*$")
    typedef = re.compile(r"^\s*typedef\s+(\w+)\s+(\w+)\s*;")
    inline = re.compile(r"^\s*static\s+inline\b[^(]*?\b(\w+)\s*\(")
    for name in sorted(os.listdir(map_dir)):
        if not name.endswith(".h"):
            continue
        with open(os.path.join(map_dir, name), encoding="utf-8") as fh:
            lines = fh.read().replace("\\\n", " ").split("\n")
        for line in lines:
            m = define.match(line)
            if m:
                old, fnlike, new = m.group(1), m.group(2), m.group(3)
                if old.endswith("_H") or not new:
                    continue
                new = re.sub(r"/\*.*?\*/|//.*$", "", new).strip()
                if fnlike or not re.fullmatch(r"\w+", new):
                    complex_.add(old)
                else:
                    raw[old] = new
                continue
            m = typedef.match(line)
            if m:
                raw[m.group(2)] = m.group(1)
                continue
            m = inline.match(line)
            if m:
                fn = m.group(1)
                if fn in INLINE_RENAMES:
                    raw[fn] = INLINE_RENAMES[fn]
                elif fn not in INLINE_CALLS:
                    complex_.add(fn)
    # Some map entries point the other way: lv_conf_internal.h still reads the "old"
    # name as a live config option (LV_DRAW_LAYER_SIMPLE_BUF_SIZE), so it stays.
    with open(os.path.join(LVGL_INC, "config", "lv_conf_internal.h"), encoding="utf-8") as fh:
        live_config = set(re.findall(r"^\s*#\s*ifndef\s+(\w+)", fh.read(), re.M))
    for name in live_config:
        raw.pop(name, None)
        complex_.discard(name)
    renames = {}
    for old in raw:
        new, seen = raw[old], {old}
        while new in raw and new not in seen:
            seen.add(new)
            new = raw[new]
        renames[old] = new
    return renames, complex_


def load_flag_funcs():
    """Per-flag setters/getters declared in lv_obj.h: (setters, getters) as sets of
    flag suffixes ('hidden', 'clickable', ...)."""
    with open(os.path.join(LVGL_INC, "core", "lv_obj.h"), encoding="utf-8") as fh:
        text = fh.read()
    flags = set(re.findall(r"\bLV_OBJ_FLAG_(\w+)\s*=", text))
    setters = {f for f in flags
               if re.search(r"\bvoid\s+lv_obj_set_%s\s*\(\s*lv_obj_t\s*\*\s*obj\s*,\s*bool\b"
                            % f.lower(), text)}
    getters = {f for f in flags
               if re.search(r"\bbool\s+lv_obj_is_%s\s*\(\s*const\s+lv_obj_t\s*\*\s*obj\s*\)"
                            % f.lower(), text)}
    return setters, getters


# ---------------------------------------------------------------- lexing

CODE, COMMENT, STRING = 0, 1, 2


def classify(text):
    """Per-character class: CODE, COMMENT or STRING, from the shared source_strip
    lexer. Only meaningful at non-blank characters, which is all the callers ask."""
    full = blank_cxx(text)
    keep = blank_cxx(text, strings="keep")
    kind = bytearray(len(text))
    for i, c in enumerate(text):
        if full[i] == c:
            continue
        kind[i] = STRING if keep[i] == c else COMMENT
    return kind


def split_args(text, kind, open_paren):
    """Split the call whose '(' is at open_paren into top-level argument strings.
    Returns (args, index_after_close) or None when the call does not close."""
    depth, args, start, i, n = 0, [], open_paren + 1, open_paren, len(text)
    while i < n:
        if kind[i] != CODE:
            i += 1
            continue
        c = text[i]
        if c in "([{":
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
    return None


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def line_text(text, pos):
    s = text.rfind("\n", 0, pos) + 1
    e = text.find("\n", pos)
    return text[s:] if e < 0 else text[s:e]


def opted_out(text, start, end):
    """True when any line the span [start, end) touches carries the opt-out marker."""
    s = text.rfind("\n", 0, start) + 1
    e = text.find("\n", end)
    return OPT_OUT in (text[s:] if e < 0 else text[s:e])


# ---------------------------------------------------------------- rewriting

class Rewriter:
    def __init__(self):
        self.renames, self.complex = load_api_map()
        self.setters, self.getters = load_flag_funcs()
        names = sorted(set(self.renames) | self.complex | set(CALL_FUNCS), key=len, reverse=True)
        self.any_name = re.compile(r"\b(" + "|".join(map(re.escape, names)) + r")\b")

    def rename(self, text):
        kind = classify(text)
        out, last, count = [], 0, 0
        for m in self.any_name.finditer(text):
            old = m.group(1)
            if old not in self.renames or kind[m.start()] == STRING:
                continue
            if opted_out(text, m.start(), m.end()):
                continue
            out.append(text[last:m.start()])
            out.append(self.renames[old])
            last = m.end()
            count += 1
        out.append(text[last:])
        return "".join(out), count

    def flag_replacement(self, func, args):
        """The rewritten call text, or a reason string prefixed with '!'."""
        if func in INLINE_CALLS:
            if len(args) != 1:
                return "!argument count"
            return INLINE_CALLS[func] % args[0]
        want = 3 if func == "lv_obj_set_flag" else 2
        if len(args) != want:
            return "!argument count"
        flag = args[1].strip()
        m = re.fullmatch(r"LV_OBJ_FLAG_(\w+)", flag)
        if not m:
            return "!flag is not one literal LV_OBJ_FLAG_* (%s)" % " ".join(flag.split())
        name = m.group(1)
        if func.startswith("lv_obj_has_flag"):
            if name not in self.getters:
                return "!no lv_obj_is_%s() in LVGL 9.6" % name.lower()
            return "lv_obj_is_%s(%s)" % (name.lower(), args[0])
        if name not in self.setters:
            return "!no lv_obj_set_%s() in LVGL 9.6" % name.lower()
        value = {"lv_obj_add_flag": " true", "lv_obj_remove_flag": " false"}.get(func)
        if value is None:
            value = args[2]
        return "lv_obj_set_%s(%s,%s)" % (name.lower(), args[0], value)

    def flags(self, text, path, report):
        """Rewrite every call that maps cleanly, then report the rest once."""
        count, rewrite = 0, True
        while True:
            kind = classify(text)
            changed = False
            for m in re.finditer(r"\b(%s)\b" % "|".join(CALL_FUNCS), text):
                if kind[m.start()] != CODE:
                    continue
                func = m.group(1)
                j = m.end()
                while j < len(text) and text[j] in " \t\r\n":
                    j += 1
                parsed = split_args(text, kind, j) if j < len(text) and text[j] == "(" else None
                if parsed is None:
                    why, end = ("not a call" if j >= len(text) or text[j] != "(" else "unbalanced call"), m.end()
                else:
                    args, end = parsed
                    repl = self.flag_replacement(func, args)
                    if rewrite and not repl.startswith("!"):
                        text = text[:m.start()] + repl + text[end:]
                        count += 1
                        changed = True
                        break  # offsets moved; rescan (a nested call rewrites next pass)
                    why = repl[1:]
                if not rewrite and not opted_out(text, m.start(), end):
                    report.append((path, line_of(text, m.start()), func, why,
                                   line_text(text, m.start()).strip()))
            if not rewrite:
                return text, count
            if not changed:
                rewrite = False

    def remaining(self, text, path):
        """Deprecated names still present in code (not comments or strings)."""
        kind = classify(text)
        hits = []
        for m in self.any_name.finditer(text):
            if kind[m.start()] != CODE or opted_out(text, m.start(), m.end()):
                continue
            hits.append((path, line_of(text, m.start()), m.group(1),
                         line_text(text, m.start()).strip()))
        return hits


def iter_files(paths):
    for p in paths:
        full = p if os.path.isabs(p) else os.path.join(REPO, p)
        if os.path.isfile(full):
            if full.endswith(EXTS):
                yield full
            continue
        for dp, dn, fn in os.walk(full):
            dn[:] = sorted(d for d in dn if d not in SKIP_DIRS and not os.path.islink(os.path.join(dp, d)))
            for f in sorted(fn):
                if f.endswith(EXTS):
                    yield os.path.join(dp, f)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*", help="files or directories (default: %s)" % " ".join(DEFAULT_PATHS))
    ap.add_argument("--check", action="store_true", help="report, change nothing, exit 1 if anything remains")
    ap.add_argument("--list-map", action="store_true", help="print the rename table and exit")
    a = ap.parse_args()

    rw = Rewriter()
    if a.list_map:
        for old in sorted(rw.renames):
            print("%s -> %s" % (old, rw.renames[old]))
        for old in sorted(rw.complex):
            print("%s -> (expression; hand edit)" % old)
        print("flag setters: " + " ".join(sorted(s.lower() for s in rw.setters)))
        print("flag getters: " + " ".join(sorted(g.lower() for g in rw.getters)))
        return 0

    paths = a.paths or DEFAULT_PATHS
    total_ren = total_flag = files_changed = 0
    report, remaining = [], []
    for path in iter_files(paths):
        try:
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            continue
        if not rw.any_name.search(text):
            continue
        rel = os.path.relpath(path, REPO)
        if a.check:
            remaining.extend(rw.remaining(text, rel))
            continue
        new, n_ren = rw.rename(text)
        new, n_flag = rw.flags(new, rel, report)
        if new != text:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(new)
            files_changed += 1
        total_ren += n_ren
        total_flag += n_flag
        for path_, line, name, _txt in rw.remaining(new, rel):
            if name in rw.complex:
                report.append((path_, line, name, "API-map expression; hand edit", _txt))

    if a.check:
        for path, line, name, txt in remaining:
            print("%s:%d: %s: %s" % (path, line, name, txt))
        print("lvgl96_rename --check: %d deprecated use(s) remain" % len(remaining), file=sys.stderr)
        return 1 if remaining else 0

    for path, line, func, why, txt in report:
        print("HAND %s:%d: %s: %s | %s" % (path, line, func, why, txt))
    print("lvgl96_rename: %d rename(s), %d call(s) rewritten in %d file(s); %d left for hand edit"
          % (total_ren, total_flag, files_changed, len(report)), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
