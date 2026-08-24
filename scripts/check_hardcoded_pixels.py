#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that spacing and sizing go through design tokens, not raw pixel literals.

HelixScreen ships on screens from 480x272 to 1440p. A raw `style_pad_all="12"`
is 12px on every one of them, so a layout tuned on a 1024x600 dev window is
cramped on a Snapmaker U1 and lost in whitespace on a 1280x720 panel. The token
ladders in ui_xml/globals.xml exist so one attribute resolves per breakpoint:
theme_manager_resolve_px_tokens() (src/ui/theme_manager.cpp) picks the
`_micro.._xxlarge` variant for the live display and registers it under the base
name, which is what `#space_md` and theme_manager_get_spacing("space_md") read.

The tree drifted anyway. This gate freezes the drift so it can only shrink.

WHAT IS FLAGGED
  1. xml-pad   — style_pad_*/style_margin_* in ui_xml/ set to an integer >= 2.
     There is a token for every one of these; the ladders were built from the
     values already in the tree, so the _large column is a near-exact match for
     the literals authors reach for (4/6/8/12/20/24/40).
  2. xml-size  — width=/height= in ui_xml/ set to an integer >= 2 that equals a
     declared token's value at some breakpoint. Matching an existing value is
     the signal: it means the author reproduced a token by hand rather than
     naming it. A width nothing declares (a measured canvas, a one-off) is not
     flagged, because there is nothing to name it with.
  3. xml-tall  — a literal style_min_height/style_max_height above 231, the
     85% dialog budget on a 272px MICRO panel. Both real #1204 dialog bugs were
     this exact shape: hidden_network_modal's style_min_height="280" and
     debug_bundle_modal's style_max_height="400" were each taller than the whole
     screen, so the dialog rendered partly off it. A floor above the budget can
     never be satisfied; a cap above it simply is not a bound.
     Only fires when the component root does NOT bound its own height. A root
     that is a percentage, or that carries its own style_max_height, already
     clips everything inside it, so an inner cap there is a dead attribute
     rather than a bug -- job_queue_modal caps a list at 300 but is itself
     height="90%", i.e. 244 on a MICRO panel. Skipping those took this rule
     from 7 findings to 1, four of the six removed being false positives.
  4. cpp-pad   — lv_obj_set_style_pad_*/margin_* in src/ passed an integer >= 2.
     Same tokens, read through theme_manager_get_spacing().

WHAT IS NOT FLAGGED
  - 0 and 1. `style_pad_all="0"` is a reset, not a measurement, and 1px is a
    hairline divider (power_device_stack_widget.cpp, thermistor_widget.cpp) that
    no ladder should ever scale.
  - `width="1" flex_grow="1"`. This is a REQUIRED LVGL flex idiom — flex wraps
    against the declared width before growing — not a size. Removing it breaks
    the layout, so `width="1"` must stay legal forever.
  - Percentages, "content", and `#token` references. Those are already correct.
  - min_width/max_width, and min/max heights at or below the MICRO budget. A px
    guard paired with a percentage width is the house idiom for dialogs
    (action_prompt_modal.xml:10), and the number there is a clamp, not the
    layout. Only a HEIGHT above the budget is flagged — see xml-tall.
  - Anything inside an XML comment. A comment that documents a fix by quoting
    the attribute it replaced is documentation, not a violation.
  - Sub-token granularity: a value below 2 has no ladder, see above.
  - The dev-only panels (ui_xml/test_panel.xml, gcode_test_panel.xml,
    step_test_panel.xml and their C++), the crash/hang screens
    (helix_watchdog.cpp, ui_fatal_error.cpp), the glyph diagnostic panel, and
    the snake easter egg. The crash screens are the load-bearing exclusion:
    they render before or without theme init, where theme_manager_get_spacing()
    returns 0 and a token read would silently collapse the layout to nothing.
  - Any site carrying a SIZE_OK annotation — `<!-- SIZE_OK: reason -->` in XML,
    `// SIZE_OK: reason` in C++. Use it where a number is reasoned and cited,
    the way ui_xml/components/lock_screen.xml walks its pixel budget against the
    MICRO/TINY heights and ui_xml/color_picker.xml walks the swatch-grid floor.
    Mirrors the existing DECLARATIVE_OK / TIMER_DTOR_OK convention.

This is a RATCHET, not a wall. Hundreds of sites predate it. The number may go
DOWN — convert a site, then lower the baseline in scripts/quality-checks.sh —
but it must never go up.

Usage:
  check_hardcoded_pixels.py                    # fail on any violation
  check_hardcoded_pixels.py --max-allowed 250  # ratcheting baseline
  check_hardcoded_pixels.py --summary          # per-rule counts
  check_hardcoded_pixels.py --list             # every site, file:line
  check_hardcoded_pixels.py --rule xml-pad     # one rule only
  check_hardcoded_pixels.py --staged-only      # post-commit tree (pre-commit hook)

--staged-only is NOT "only staged files". It scans the tree the commit WILL
create (index content for staged paths, HEAD for the rest), built via
`git write-tree`. The ratchet baseline is a whole-tree count, so the check has
to see a whole tree; --staged-only makes that tree the would-be-committed one
rather than the dirty working one, so another session's unstaged WIP cannot
make a clean commit fail. The pre-commit hook (quality-checks.sh) passes this
flag; CI and manual runs use the default whole-working-tree scan.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

XML_DIRS = ("ui_xml",)
CPP_DIRS = ("src",)
CPP_EXTS = (".cpp", ".cc")

# android/app/src/main/assets/ui_xml/ is a gradle copy of ui_xml/
# (android/app/build.gradle: from('../../ui_xml')), not a source of truth.
SKIP_PARTS = ("android", "build", ".worktrees")

OPT_OUT = "SIZE_OK"

# How far back an annotation reaches. XML elements and their comments routinely
# span several lines, so a 1-line window (what check_imperative_ui.py uses for
# C++) would not cover a comment sitting above a wrapped element.
OPT_OUT_LOOKBACK = 6

# Dev-only surfaces and pre-theme crash screens. See WHAT IS NOT FLAGGED.
EXEMPT_FILES = {
    "ui_xml/test_panel.xml",
    "ui_xml/gcode_test_panel.xml",
    "ui_xml/step_test_panel.xml",
    "src/helix_watchdog.cpp",
    "src/ui/ui_fatal_error.cpp",
    "src/ui/ui_panel_gcode_test.cpp",
    "src/ui/ui_panel_glyphs.cpp",
    "src/ui/ui_snake_game.cpp",
}

# Tokens whose value is not a length. modal_backdrop_opacity is 0-255 alpha; a
# width that happens to equal 120 is not "reproducing a token by hand".
NON_LENGTH_TOKENS = ("opacity", "opa")

# Tokens that measure a GAP, not a BOX, and so are never the right answer for a
# width= or height=. Without this, `<text_small width="40">` matches space_xl=40
# and the gate suggests a padding token for a label width — a coincidence, and
# the fastest way to teach people the gate is noise. Spacing tokens still drive
# the xml-pad and cpp-pad rules, where they ARE the answer.
NON_SIZE_PREFIXES = ("space_", "spinner_arc_", "border_radius")
NON_SIZE_SUBSTRINGS = ("padding",)

# style_pad_all="12" / style_margin_top="8". Token refs (#name), percentages and
# "content" do not match, because the value group is digits-only.
XML_PAD_RE = re.compile(r'\bstyle_(?:pad|margin)_[a-z]+\s*=\s*"(-?\d+)"')

# width="36" / style_height="48". The lookbehind keeps min_width/max_height out:
# a px clamp paired with a percentage is the dialog idiom, not a hardcoded size.
XML_SIZE_RE = re.compile(r'(?<![\w_])(?:style_)?(?:width|height)\s*=\s*"(\d+)"')

# The MICRO panel is 272px tall and modal_dialog caps a dialog at 85% of that.
# A literal min/max height above this cannot fit the smallest supported screen.
MICRO_DIALOG_BUDGET = 231

XML_TALL_RE = re.compile(r'\bstyle_(?:min|max)_height\s*=\s*"(\d+)"')

# lv_obj_set_style_pad_all(obj, 12, 0) — second argument an integer literal.
CPP_PAD_RE = re.compile(
    r'\blv_obj_set_style_(?:pad|margin)_[a-z]+\s*\(\s*[^,()]+,\s*(-?\d+)\s*,')

RULES = ("xml-pad", "xml-size", "xml-tall", "cpp-pad")

FIX = {
    "xml-tall": "a responsive cap — style_max_height=\"#dialog_content_max\"",
    "xml-pad": 'a spacing token — style_pad_all="#space_md"',
    "xml-size": "a size token — width=\"#icon_button_size_lg\"",
    "cpp-pad": 'theme_manager_get_spacing("space_md")',
}


def repo_root() -> Path:
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True, check=False).stdout.strip()
    return Path(out) if out else Path.cwd()


def load_token_values(root: Path) -> dict[int, list[str]]:
    """value -> [base token names] for every SIZE token in globals.xml.

    Built from the file rather than hardcoded, so declaring a token
    automatically teaches the gate to recognise hand-copies of it.
    """
    path = root / "ui_xml" / "globals.xml"
    table: dict[int, list[str]] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return table
    for m in re.finditer(r'<px\s+name="([^"]+)"\s+value="(-?\d+)"', text):
        name, raw = m.group(1), int(m.group(2))
        if raw < 2:
            continue
        if any(s in name for s in NON_LENGTH_TOKENS):
            continue
        if name.startswith(NON_SIZE_PREFIXES) or any(s in name for s in NON_SIZE_SUBSTRINGS):
            continue
        base = base_token(name)
        names = table.setdefault(raw, [])
        if base not in names:
            names.append(base)
    return table


def base_token(name: str) -> str:
    """space_md_large -> space_md, so --list suggests the name authors write."""
    for suffix in ("_micro", "_tiny", "_small", "_medium",
                   "_large", "_xlarge", "_xxlarge"):
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return name


def annotated(lines: list[str], lineno: int) -> bool:
    """SIZE_OK on this line or within OPT_OUT_LOOKBACK lines above it."""
    start = max(0, lineno - 1 - OPT_OUT_LOOKBACK)
    return any(OPT_OUT in ln for ln in lines[start:lineno])


ROOT_VIEW_RE = re.compile(r"<view\b[^>]*>", re.S)
ROOT_HEIGHT_RE = re.compile(r'(?<![\w_])height="([^"]+)"')


def root_is_self_bounding(text: str) -> bool:
    """True when the component's root already bounds its own height.

    This is the difference between a real overflow and a dead attribute, and
    getting it wrong makes the xml-tall rule cry wolf. job_queue_modal caps its
    inner list at 300, which looks alarming on a 272px panel -- but its root is
    height="90%", so the dialog is 244 and the inner cap never binds. Only a
    root that sizes to content with no cap of its own leaves the inner number
    as the sole bound; that is the debug_bundle_modal / crash_report_modal
    shape that actually rendered off-screen.
    """
    m = ROOT_VIEW_RE.search(text)
    if not m:
        return False
    root = m.group(0)
    if "style_max_height" in root:
        return True
    h = ROOT_HEIGHT_RE.search(root)
    return bool(h and h.group(1).strip().endswith("%"))


def blank_comments(text: str) -> str:
    """Replace XML comment bodies with spaces, preserving length and newlines.

    A comment that explains a fix by quoting the old attribute -- and the good
    ones here do exactly that -- is documentation, not a violation. Offsets and
    line numbers are preserved so annotation lookup still reads the real lines.
    """
    def blank(m: re.Match) -> str:
        return "".join(ch if ch == "\n" else " " for ch in m.group(0))
    return re.sub(r"<!--.*?-->", blank, text, flags=re.S)


def scan_xml(text: str, rel: str, tokens: dict[int, list[str]]) -> list[tuple]:
    lines = text.split("\n")
    scan = blank_comments(text)
    hits = []

    # A min/max height literal taller than the MICRO dialog budget cannot be
    # satisfied on a 480x272 panel. Both real #1204 dialog bugs were this shape:
    # hidden_network's style_min_height="280" and debug_bundle's
    # style_max_height="400", each larger than the 272px screen itself.
    self_bounded = root_is_self_bounding(scan)
    for m in XML_TALL_RE.finditer(scan):
        value = int(m.group(1))
        if value <= MICRO_DIALOG_BUDGET:
            continue
        if self_bounded:
            continue  # the root already bounds it; this number never binds
        lineno = scan.count("\n", 0, m.start()) + 1
        if annotated(lines, lineno):
            continue
        hits.append((rel, lineno, "xml-tall", value,
                     f" > {MICRO_DIALOG_BUDGET} MICRO budget",
                     lines[lineno - 1].strip()[:80]))

    for rule, pattern in (("xml-pad", XML_PAD_RE), ("xml-size", XML_SIZE_RE)):
        for m in pattern.finditer(scan):
            value = int(m.group(1))
            if abs(value) < 2:
                continue
            note = ""
            if rule == "xml-size":
                names = tokens.get(value)
                if not names:
                    continue  # nothing declares this size; nothing to name it with
                note = " == " + "/".join(names[:3])
            lineno = scan.count("\n", 0, m.start()) + 1
            if annotated(lines, lineno):
                continue
            hits.append((rel, lineno, rule, value, note,
                         lines[lineno - 1].strip()[:80]))
    return hits


def scan_cpp(text: str, rel: str) -> list[tuple]:
    lines = text.split("\n")
    hits = []
    for m in CPP_PAD_RE.finditer(text):
        value = int(m.group(1))
        if abs(value) < 2:
            continue
        lineno = text.count("\n", 0, m.start()) + 1
        if annotated(lines, lineno):
            continue
        hits.append((rel, lineno, "cpp-pad", value, "",
                     lines[lineno - 1].strip()[:80]))
    return hits


def _git_text(args: list[str], root: Path) -> str:
    """Run git in root, return stdout (text). Never raises."""
    return subprocess.run(["git", "-C", str(root)] + args,
                          capture_output=True, text=True, check=False).stdout


def _catfile_batch(root: Path, revs: list[str], rels: list[str]):
    """Yield (rel, text) for each blob rev via one `git cat-file --batch`.

    The byte-count header makes this robust to newlines/binary in content; the
    streaming form (one process for every rev) is ~8x faster than spawning a
    `git show` per file (0.2s vs 1.5s for ~1000 files — benchmarked).
    """
    proc = subprocess.Popen(
        ["git", "-C", str(root), "cat-file", "--batch"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for rev, rel in zip(revs, rels):
            assert proc.stdin is not None and proc.stdout is not None
            proc.stdin.write((rev + "\n").encode())
            proc.stdin.flush()
            header = proc.stdout.readline().decode("utf-8", "replace").split()
            # "<sha> blob <size>" — skip "missing" / non-blob (submodule) entries.
            if len(header) < 3 or header[1] != "blob":
                continue
            size = int(header[2])
            content = proc.stdout.read(size)
            proc.stdout.read(1)  # trailing newline after each blob
            try:
                yield (rel, content.decode("utf-8"))
            except UnicodeDecodeError:
                continue
    finally:
        if proc.stdin is not None:
            proc.stdin.close()
        proc.wait()


def _in_scope(rel: str) -> bool:
    """True if rel is a file the gate scans — mirrors the default scan's dirs.

    Default mode globs ui_xml/**/*.xml and src/**/*.{cpp,cc}; the post-commit
    mode (ls-tree) sees the whole tree, so it must apply the same scoping or it
    would count tests/, firmware/, tools/ and inflate the ratchet.
    """
    if any(part in SKIP_PARTS for part in Path(rel).parts):
        return False
    if rel.endswith(".xml"):
        return rel.startswith("ui_xml/")
    if rel.endswith(CPP_EXTS):
        return rel.startswith("src/")
    return False


def collect(args, root: Path):
    """Yield (rel, text) for every file to scan, content already sourced.

    Three modes:
      - positional file args: read each from the working tree (fixtures, ad-hoc).
      - --staged-only: the POST-COMMIT TREE — `git write-tree` builds the tree
        the commit WILL create (index applied over HEAD), so unstaged WIP from
        another session never counts. Reading blobs via `git cat-file --batch`
        keeps it fast (one process). This is what the pre-commit hook needs: the
        ratchet baseline is a whole-tree count, so the check must see a whole
        tree, just the would-be-committed one rather than the dirty working one.
      - default: the whole working tree (CI, manual runs against a clean checkout).
    """
    if args.files:
        for f in args.files:
            try:
                yield (str(f), Path(f).read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError):
                continue
        return

    if args.staged_only:
        # The tree this commit will produce: index content for staged paths,
        # HEAD for everything else. `git write-tree` materialises it as one tree
        # object; ls-tree lists its files; cat-file --batch streams their blobs.
        tree = _git_text(["write-tree"], root).strip()
        if not tree:
            return  # not a git repo / empty index — nothing to check
        rels = [f for f in _git_text(
            ["ls-tree", "-r", "--name-only", tree], root).split("\n") if f and _in_scope(f)]
        yield from _catfile_batch(root, [f"{tree}:{r}" for r in rels], rels)
        return

    # Default: whole working tree.
    patterns = [(d, "*.xml") for d in XML_DIRS]
    patterns += [(d, f"*{ext}") for d in CPP_DIRS for ext in CPP_EXTS]
    for d, glob in patterns:
        for p in sorted((root / d).rglob(glob)):
            rel = p.relative_to(root).as_posix()
            # Test the RELATIVE path: the repo itself may live under a directory
            # named in SKIP_PARTS (every git worktree lives under .worktrees/),
            # which would make an absolute-path test skip the entire tree.
            if any(part in SKIP_PARTS for part in Path(rel).parts):
                continue
            try:
                yield (rel, p.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError):
                continue


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="Files to check (default: scan the tree)")
    ap.add_argument("--max-allowed", type=int, default=None,
                    help="Pass if total <= N (ratcheting baseline). Default: fail on any.")
    ap.add_argument("--summary", action="store_true", help="Per-rule counts only")
    ap.add_argument("--list", action="store_true", help="Print every site")
    ap.add_argument("--rule", choices=RULES, help="Restrict to one rule")
    ap.add_argument("--staged-only", action="store_true",
                    help="Scan the post-commit tree (index + HEAD), not the "
                         "dirty working tree — what the pre-commit hook uses so "
                         "unstaged WIP from another session cannot trip the ratchet")
    args = ap.parse_args()

    root = repo_root()
    tokens = load_token_values(root)

    hits: list[tuple] = []
    for rel, text in collect(args, root):
        if rel in EXEMPT_FILES or Path(rel).name in {
                Path(e).name for e in EXEMPT_FILES}:
            continue
        if rel.endswith(".xml"):
            hits += scan_xml(text, rel, tokens)
        elif rel.endswith(CPP_EXTS):
            hits += scan_cpp(text, rel)

    if args.rule:
        hits = [h for h in hits if h[2] == args.rule]

    by_rule: dict[str, int] = {}
    for h in hits:
        by_rule[h[2]] = by_rule.get(h[2], 0) + 1
    total = len(hits)

    if args.list:
        for rel, lineno, rule, value, note, src in sorted(hits):
            print(f"{rel}:{lineno}: [{rule}] {value}{note}  {src}")
        print()

    if args.summary or args.list:
        for rule in RULES:
            if args.rule and args.rule != rule:
                continue
            print(f"  {rule:<9} {by_rule.get(rule, 0):>5}   → {FIX[rule]}")
        print(f'  {"TOTAL":<9} {total:>5}')

    limit = args.max_allowed
    if limit is None:
        if total:
            print(f"❌ Hardcoded pixels: {total} literal spacing/size values.")
            return 1
        print("✅ Hardcoded pixels: none.")
        return 0

    if total > limit:
        print(f"❌ Hardcoded pixels: {total} violations exceeds baseline ({limit}).")
        print("   Use a design token; annotate a reasoned exception SIZE_OK.")
        print("   Run: python3 scripts/check_hardcoded_pixels.py --list")
        return 1
    if total < limit:
        print(f"✅ Hardcoded pixels: {total} (baseline {limit} — ratchet the baseline down)")
        return 0
    print(f"✅ Hardcoded pixels: {total} == baseline ({limit})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
