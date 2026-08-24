#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Git 3-way merge driver for .claude-recall/*.json usage counters.

These files are per-lesson counter maps ({"L109": {"uses": 7, ...}}), which a
text merge cannot combine: two worktrees that each cited a lesson produce
conflicting scalars, and taking either side silently discards the other's work.

Because git hands a merge driver the ANCESTOR as well as both sides, the true
total is recoverable: each side's delta from the ancestor is real work, so
    merged = ancestor + (ours - ancestor) + (theirs - ancestor)
Two trees that cited L109 three and two times from a shared base of 7 land on
12, not 10 or 9.

Field semantics:
  uses / injections / citations  additive, as above (monotonic counters)
  velocity                       max() - a decayed metric recall recomputes,
                                 not a counter, so summing deltas inflates it
  last (ISO date) and other str  max() - ISO dates sort lexicographically
  ids on only one side           taken as-is (a lesson added in that tree)
  ids dropped on either side     honored as a deletion (recall delete)

Usage (as configured in .gitattributes + git config):
    recall_stats_merge.py %O %A %B
%A is both the "ours" input and the output path. Exits 0 on a clean merge, or
1 to let git fall back to a normal conflict if any input is unparseable - a
conflict is recoverable, a corrupted counter file is not.
"""
import json
import sys

# Counters where each side's delta from the ancestor is independent work.
ADDITIVE_FIELDS = ("uses", "injections", "citations")


def load(path):
    """Parse a merge input. A missing or empty file is an absent ancestor."""
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except FileNotFoundError:
        return {}, ""
    if not text.strip():
        return {}, ""
    return json.loads(text), text


def detect_style(text):
    """Recover the file's formatting so a merge does not reflow the whole file.

    recall writes stats.json indented and injection-stats.json compact; either
    one reformatted wholesale would bury the real change in a full-file diff.
    """
    body = text.strip()
    if "\n" in body:
        return {"indent": 2, "separators": (",", ": ")}
    return {"separators": (",", ":")}


def ordered_keys(ours, theirs):
    """Keys in "ours" order, then any "theirs"-only ones.

    Preserving the incoming order keeps a merge from reflowing the whole file
    into a different key order, which would bury the counter change in noise.
    """
    keys = list(ours)
    keys.extend(k for k in theirs if k not in ours)
    return keys


def merge_entry(anc, ours, theirs):
    """Combine one lesson's counters from the three sides."""
    merged = {}
    for field in ordered_keys(ours, theirs):
        anc_v = anc.get(field)
        ours_v = ours.get(field, anc_v)
        theirs_v = theirs.get(field, anc_v)

        if field in ADDITIVE_FIELDS and isinstance(ours_v, (int, float)) \
                and isinstance(theirs_v, (int, float)):
            base = anc_v if isinstance(anc_v, (int, float)) else 0
            merged[field] = max(0, base + (ours_v - base) + (theirs_v - base))
        elif ours_v is None:
            merged[field] = theirs_v
        elif theirs_v is None:
            merged[field] = ours_v
        else:
            # velocity, dates, and anything else: prefer the larger/later side.
            try:
                merged[field] = max(ours_v, theirs_v)
            except TypeError:
                merged[field] = ours_v
    return merged


def main(argv):
    if len(argv) < 4:
        print("usage: recall_stats_merge.py %O %A %B", file=sys.stderr)
        return 1
    anc_path, ours_path, theirs_path = argv[1], argv[2], argv[3]

    try:
        ancestor, _ = load(anc_path)
        ours, ours_text = load(ours_path)
        theirs, _ = load(theirs_path)
    except (json.JSONDecodeError, OSError) as exc:
        print(f"recall_stats_merge: {exc} - falling back to conflict", file=sys.stderr)
        return 1

    style = detect_style(ours_text)

    merged = {}
    for key in ordered_keys(ours, theirs):
        in_anc = key in ancestor
        # Present in the ancestor but gone from one side: an intentional delete.
        if in_anc and (key not in ours or key not in theirs):
            continue
        if key not in ours:
            merged[key] = theirs[key]
        elif key not in theirs:
            merged[key] = ours[key]
        elif isinstance(ours[key], dict) and isinstance(theirs[key], dict):
            merged[key] = merge_entry(ancestor.get(key, {}), ours[key], theirs[key])
        else:
            merged[key] = ours[key]

    out = json.dumps(merged, **style)
    if ours_text.endswith("\n"):
        out += "\n"
    with open(ours_path, "w", encoding="utf-8") as handle:
        handle.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
