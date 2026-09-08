#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report widgets whose XML carries two or more bind_state_* writing one state.

LVGL's state observer asserts BOTH polarities on every fire: it adds or removes
the state unconditionally, based only on its own subject. Two bindings aimed at
the same state on the same widget therefore do not compose into "either reason
disables this". They overwrite each other, and the subject that notified last
decides alone. A widget guarded that way sits in the wrong state whenever the
last notifier is the satisfied one.

The correct form for a union is a single expression binding:

    <bind_state_if cond="a eq 1 or b eq 1" state="disabled"/>

This is a REPORTING tool, not a gate: it exits 0 whatever it finds, because the
inventory still needs triage per widget. Some hits may be benign by
construction, e.g. a prop-driven widget whose call sites only ever set one of
the two bindings.

Usage:
    scripts/scan_duplicate_state_binds.py [--root DIR] [--json]
"""

import argparse
import json
import pathlib
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter

# Panel XML qualifies a style by widget state as `style_bg_opa:disabled="100"`.
# A generic XML parser reads the colon as a namespace prefix and rejects the
# whole file as an unbound prefix, so those attributes are rewritten to a flat
# name before parsing. Skipping the files instead would drop them from the
# inventory silently, which is the one failure mode this tool must not have.
_STATE_QUALIFIED_ATTR = re.compile(r'(\s[A-Za-z_][\w-]*):([A-Za-z_][\w-]*)=')


def _parse(path):
    src = path.read_text(encoding="utf-8")
    return ET.fromstring(_STATE_QUALIFIED_ATTR.sub(r"\1__\2=", src))


def scan_file(path):
    """Yield (widget, state, [(tag, subject_or_cond), ...]) for each hit."""
    try:
        root = _parse(path)
    except (ET.ParseError, OSError, UnicodeDecodeError) as exc:
        # Loud, and reflected in the exit status, so a parse regression can
        # never be mistaken for a clean scan.
        print(f"ERROR: could not parse {path}: {exc}", file=sys.stderr)
        raise

    for el in root.iter():
        # Direct children only. A nested widget's bindings are its own, and
        # counting descendants would merge unrelated widgets into one tally.
        binds = [c for c in el if c.tag.startswith("bind_state_")]
        counts = Counter(c.get("state") for c in binds if c.get("state"))
        for state, n in counts.items():
            if n < 2:
                continue
            detail = [
                (c.tag, c.get("subject") or c.get("cond") or "?")
                for c in binds
                if c.get("state") == state
            ]
            yield el.get("name") or el.tag, state, detail


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default="ui_xml", help="directory to scan (default: ui_xml)")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    args = ap.parse_args()

    hits = []
    scanned = 0
    for path in sorted(pathlib.Path(args.root).rglob("*.xml")):
        for widget, state, detail in scan_file(path):
            hits.append({
                "file": str(path),
                "widget": widget,
                "state": state,
                "bindings": [{"tag": t, "on": s} for t, s in detail],
            })
        scanned += 1

    if args.json:
        print(json.dumps(hits, indent=2))
    else:
        for h in hits:
            print(f"{h['file']}  <{h['widget']}>  {len(h['bindings'])}x state={h['state']}")
            for b in h["bindings"]:
                print(f"      {b['tag']}  {b['on']}")
        print(f"\n{len(hits)} widget(s) with two or more bind_state_* writing one state"
              f" across {scanned} file(s) scanned")

    return 0


if __name__ == "__main__":
    sys.exit(main())
