#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if the base locale's translation keys are not their own English text.

English registers NO translation pack at runtime (helix::ui::ensure_translation_
loaded skips kIdentityLocale): lv_translation_get() returns the tag on a miss,
and that only renders correct UI when the tag IS the English string. A
"semantic" key — value differing from the key, e.g.
`pre_print_option.timelapse.label: Timelapse` — therefore renders the RAW KEY
in the English UI (the v0.99.114 regression: the timelapse toggle row and every
first-run tour string showed dotted identifiers).

This gate makes that invariant a build-time failure instead of a shipped
screenshot. It checks translations/en.yml directly (not the generated XML) so
it fires even on machines that never run `make translations` (the Makefile has
a no-Python fallback that reuses stale artifacts).

Every other locale is exempt: its keys are English SOURCE text, which is
correctly not its UI text.

Exit status: 0 = en.yml is pure identity, 1 = non-identity entries found.
"""

import argparse
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EN_YML = REPO_ROOT / "translations" / "en.yml"

BASE_LOCALE = "en"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--file",
        type=Path,
        default=DEFAULT_EN_YML,
        help="translations master to check (default: translations/en.yml)",
    )
    args = parser.parse_args()

    en_yml = args.file
    if not en_yml.exists():
        print(f"error: {en_yml} not found", file=sys.stderr)
        return 1

    doc = yaml.safe_load(en_yml.read_text(encoding="utf-8"))
    locale = doc.get("locale") if isinstance(doc, dict) else None
    entries = doc.get("translations") if isinstance(doc, dict) else None
    if locale != BASE_LOCALE or not isinstance(entries, dict):
        print(f"error: {en_yml} is not the '{BASE_LOCALE}' translations master",
              file=sys.stderr)
        return 1

    bad = [(k, v) for k, v in entries.items() if k != v]
    if bad:
        print(
            f"error: {len(bad)} non-identity entr{'y is' if len(bad) == 1 else 'ies are'}"
            f" in {en_yml} — the English UI renders the RAW key for these"
            " (no en pack is loaded at runtime):\n",
            file=sys.stderr,
        )
        for key, val in sorted(bad):
            print(f"  {key!r} -> {val!r}", file=sys.stderr)
        print(
            "\nRename the key to its English text in translations/en.yml AND in"
            "\nevery translations/<locale>.yml, plus the C++/XML/JSON site that"
            "\nreferences it. If the English text already exists as a tag, point"
            "\nthe reference at the existing tag instead of adding a new key.",
            file=sys.stderr,
        )
        return 1

    print(f"ok: all {len(entries)} {BASE_LOCALE} entries are identity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
