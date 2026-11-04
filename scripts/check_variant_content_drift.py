#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Warn when a staged change touches CONTENT in one half of a layout-variant
pair but not the other.

check_variant_parity.py compares WIRING between a ui_xml/<variant>/ override
and its base — widget name=, subject bindings, event callbacks, <api> props —
and deliberately not attributes, because reflow is the entire point of a
variant (see that script's docstring). That leaves a gap: bump a font, swap an
icon src=, or edit a translation_tag= in the base and forget the variant, and
every existing gate passes silently. The two screens slowly stop saying the
same thing.

This gate closes that gap for exactly three CONTENT-bearing attributes:

    src=              icon / image name
    text=             literal on-screen text
    translation_tag=  the word actually shown, via the translation table

Layout attributes (width, height, align, flex_*, style_*, pad_*, ...) are
deliberately not in that list and must never trip this gate — reflowing a
panel between breakpoints is the entire reason a variant file exists.

It is scoped to the STAGED diff, not the working tree: a file changed but not
staged is not about to be committed, and a base+variant pair staged TOGETHER is
the correct, silent case (the human already kept them in sync). Only when
exactly one half of a pair is staged, and that staged diff touches one of the
three attributes above, does it warn.

It is also scoped to WARN, never fail: additive divergence is legitimate and
already present in the tree — ui_xml/portrait/print_tune_panel.xml splits the
z-offset controls into rows the base keeps on one line, with their own help
text, because a narrow screen has no room for the single row. A hard gate
cannot tell that apart from rot; a human glancing at the file name and
attribute can.

Known false-positive shape: this reads the staged diff *line by line*
(`git diff --cached -U0`), not attribute by attribute. If a changed layout
attribute shares a line with an untouched content attribute — both live on the
same unwrapped XML element — the line as a whole still counts as touching that
attribute. Word-level diffing would close this, but the gate is warn-only by
design, so an occasional over-warn costs a glance, not a broken build; the
line-level version stays simple and never has a false SILENCE, which is the
failure mode that would actually matter.

Usage:
    ./scripts/check_variant_content_drift.py

Reads `git diff --cached` in the current repo — run it from a checkout, not
with file arguments.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Reuse the variant-directory list and base<->variant pairing logic rather than
# re-deriving them: two gates disagreeing about what counts as a variant would
# be its own bug.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_variant_parity import iter_variant_pairs  # noqa: E402

# The three attributes this gate cares about. Matched as `\bATTR="` so
# `text_color=` / `text_font=` never match `text=`, and `context=` never
# matches inside the middle of a longer word.
CONTENT_ATTR_RE = re.compile(r'\b(src|text|translation_tag)="')


def run_git(args: list[str], cwd: Path) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def repo_root() -> Path:
    return Path(run_git(["rev-parse", "--show-toplevel"], cwd=Path.cwd()).strip())


def staged_files(root: Path) -> set[str]:
    out = run_git(["diff", "--cached", "--name-only"], cwd=root)
    return {line for line in out.splitlines() if line}


def staged_content_attrs(root: Path, rel_path: str) -> set[str]:
    """Content-bearing attribute names touched by rel_path's staged diff.

    -U0 keeps only changed lines: an untouched line elsewhere in the file that
    happens to carry src=/text=/translation_tag= must not count.
    """
    diff = run_git(["diff", "--cached", "-U0", "--", rel_path], cwd=root)
    found: set[str] = set()
    for line in diff.splitlines():
        if not line or line[0] not in "+-" or line[:3] in ("+++", "---"):
            continue
        found.update(m.group(1) for m in CONTENT_ATTR_RE.finditer(line))
    return found


def main() -> int:
    root = repo_root()
    ui_xml = root / "ui_xml"
    if not ui_xml.is_dir():
        print(f"error: {ui_xml} not found", file=sys.stderr)
        return 1

    staged = staged_files(root)
    if not staged:
        print("✓ variant content drift: nothing staged")
        return 0

    warnings = 0
    for base_file, variant_file in iter_variant_pairs(ui_xml):
        base_rel = str(base_file.relative_to(root))
        variant_rel = str(variant_file.relative_to(root))

        base_staged = base_rel in staged
        variant_staged = variant_rel in staged
        if base_staged == variant_staged:
            # Neither half staged, or both staged together -- the latter means
            # the human already kept them in sync in this same commit.
            continue

        staged_rel, sibling_rel = (
            (base_rel, variant_rel) if base_staged else (variant_rel, base_rel)
        )
        attrs = staged_content_attrs(root, staged_rel)
        if not attrs:
            # Only layout attributes changed -- reflow is the point of a
            # variant, so this is the expected, silent case.
            continue

        warnings += 1
        attr_list = ", ".join(f"{a}=" for a in sorted(attrs))
        print(
            f"\n⚠️  {staged_rel} changes {attr_list} but its sibling\n"
            f"    {sibling_rel} is not staged.\n"
            "    If this is content (words, icons), mirror the change there too.\n"
            "    If it's pure reflow, ignore this -- this gate never blocks a commit."
        )

    if warnings:
        print(
            f"\n⚠️  {warnings} variant/base pair(s) may have drifted in content "
            "(warning only, does not fail).\n"
        )
    else:
        print("✓ variant content drift: no divergence in staged content")
    return 0


if __name__ == "__main__":
    sys.exit(main())
