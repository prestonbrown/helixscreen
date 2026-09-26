#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Count raw lv_obj_t* data members, so the number can only fall.

A member that caches a widget outlives the widget whenever something other than
its owner deletes the tree: a raw lv_obj_delete(), a hot-reload rebuild, a
backdrop-tap dismissal. Every lifetime guard keyed on the owner (LifetimeToken,
observer weak_alive) still reads valid then, so a deferred callback that
null-checks the member walks freed memory. helix::ui::WidgetRef
(include/ui_widget_ref.h) keys on the widget's own delete and clears itself.

This is a ratchet, not a ban: many existing members are only ever read
synchronously while their tree is known alive. New code should hold a
WidgetRef. A raw member that is genuinely right says why:

    lv_obj_t* scratch_ = nullptr; // WIDGET_PTR_OK: <reason>

(prestonbrown/helixscreen#1298)
"""
import re
import sys
from pathlib import Path

# A data member: the house trailing-underscore name, a scalar or a C array,
# optionally static. Locals, parameters and containers do not match.
MEMBER = re.compile(r"^\s*(?:static\s+)?lv_obj_t\s*\*\s*[A-Za-z_]\w*_\s*(?:\[[^\]]*\])?\s*(?:[=;{])")
OPT_OUT = "WIDGET_PTR_OK:"


def scan(root: Path, files: list[Path]) -> list[str]:
    hits = []
    for f in files:
        try:
            rel = str(f.resolve().relative_to(root))
        except ValueError:
            rel = str(f)
        try:
            lines = f.read_text().splitlines()
        except (OSError, UnicodeDecodeError):
            continue
        for i, line in enumerate(lines, 1):
            if not MEMBER.match(line):
                continue
            above = lines[i - 2] if i >= 2 else ""
            if OPT_OUT in line or OPT_OUT in above:
                continue
            hits.append(f"{rel}:{i}: {line.strip()}")
    return hits


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    max_allowed = 0
    show_list = False
    args = []
    for a in argv[1:]:
        if a.startswith("--max-allowed="):
            max_allowed = int(a.split("=", 1)[1])
        elif a == "--list":
            show_list = True
        else:
            args.append(a)

    if args:
        files = [Path(a) for a in args]
    else:
        files = sorted(
            p for d in ("src", "include") for ext in ("*.cpp", "*.h") for p in (root / d).rglob(ext)
        )

    hits = scan(root, files)
    if show_list:
        for h in hits:
            print(h)
    if len(hits) > max_allowed:
        print(f"Raw lv_obj_t* members: {len(hits)} > baseline {max_allowed}")
        print("  Hold a cached widget as helix::ui::WidgetRef (include/ui_widget_ref.h):")
        print("  it clears itself when the widget dies, whoever deletes it.")
        print(f"  A raw member that is right here? Say why: // {OPT_OUT} <reason>")
        print("  Run: python3 scripts/check_cached_widget_pointers.py --list")
        return 1

    print(f"✅ raw lv_obj_t* members: {len(hits)} <= baseline ({max_allowed})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
