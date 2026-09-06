#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: every icon name written in ui_xml/ must exist in the codepoint table.
#
# WHY THIS NEEDS A GATE
#   `<icon src="NAME">` resolves NAME through ui_icon::lookup_codepoint() at
#   runtime. A name that is not in the table does not fail, does not abort, and
#   does not stop the panel from building — it logs one warning and substitutes
#   `image_broken_variant`. The user sees a broken-image glyph next to real
#   text and has no way to tell it apart from a deliberate one.
#
#   The sibling `text="#icon_name"` path cannot have this bug: those consts are
#   generated into globals.xml from the same header by gen_icon_consts.py, so an
#   unknown name is a missing const and the XML fails loudly. `src=` has no such
#   generator standing behind it. This gate is that missing half.
#
# WHAT IS CHECKED
#   Three ways a literal icon name reaches an <icon src=>:
#
#     1. DIRECT      <icon src="alert"/>
#     2. PROP DEFAULT   a component whose <icon> takes src="$p", where
#                       <prop name="p" default="radiator"/> supplies the name
#     3. INSTANCE SITE  <heater_icon src="thermometer"/> — the literal handed
#                       to that same $p at an instantiation
#
#   A name that arrives as a bare `$param` with no literal anywhere is not
#   checkable here and is not reported.
#
# KNOWN BLIND SPOT
#   Icon names built or chosen in C++ (POWER_ICONS[], favorite-macro configs,
#   anything passed to lookup_codepoint() from a string variable) are outside
#   this gate. Those call sites mostly validate against the table already —
#   see favorite_macro_widget.cpp — which is the C++-side equivalent.
#
# CORPUS ASSERTION
#   --summary prints how many references were scanned and fails when the direct
#   corpus is empty. A gate whose pattern silently stops matching reports "0
#   problems" forever and reads exactly like a passing gate; the count is the
#   only thing that tells the two apart.

import argparse
import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
CODEPOINTS_H = PROJECT_ROOT / "include" / "ui_icon_codepoints.h"
UI_XML = PROJECT_ROOT / "ui_xml"

# {"name", "\xF3..."} in the codepoint table.
RE_REGISTERED = re.compile(r'\{\s*"([a-z0-9_]+)"\s*,')
# An <icon ...> tag, captured whole so attributes can be picked out of it.
RE_ICON_TAG = re.compile(r"<icon\b[^>]*>")
RE_ATTR = re.compile(r'\b([a-z_][a-z0-9_]*)\s*=\s*"([^"]*)"')
# <prop name="src" type="string" default="radiator"/>
RE_PROP = re.compile(r"<prop\b[^>]*>")


def registered_icons() -> set:
    if not CODEPOINTS_H.exists():
        sys.exit(f"error: {CODEPOINTS_H} not found")
    return set(RE_REGISTERED.findall(CODEPOINTS_H.read_text(encoding="utf-8")))


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def is_literal(value: str) -> bool:
    """A name we can resolve statically: no $param and no #const indirection."""
    return bool(value) and "$" not in value and "#" not in value


def scan():
    """Return (direct, prop_defaults, instance_sites) reference lists.

    Each entry is (name, path, line, how).
    """
    direct, prop_defaults, instance_sites = [], [], []
    # component stem -> set of prop names that feed an <icon src=>
    icon_props: dict = {}
    files = sorted(UI_XML.rglob("*.xml"))

    for path in files:
        text = path.read_text(encoding="utf-8")
        for m in RE_ICON_TAG.finditer(text):
            attrs = dict(RE_ATTR.findall(m.group(0)))
            src = attrs.get("src")
            if src is None:
                continue
            if is_literal(src):
                direct.append((src, path, line_of(text, m.start()), "src="))
            elif src.startswith("$"):
                icon_props.setdefault(path.stem, set()).add(src[1:])

    # Pass 2 needs the full icon_props map, so it runs after every file is seen.
    for path in files:
        text = path.read_text(encoding="utf-8")
        props = icon_props.get(path.stem)
        if props:
            for m in RE_PROP.finditer(text):
                attrs = dict(RE_ATTR.findall(m.group(0)))
                name, default = attrs.get("name"), attrs.get("default")
                if name in props and default is not None and is_literal(default):
                    prop_defaults.append(
                        (default, path, line_of(text, m.start()), f"<prop {name}> default")
                    )

        for component, params in icon_props.items():
            for m in re.finditer(rf"<{re.escape(component)}\b[^>]*>", text):
                attrs = dict(RE_ATTR.findall(m.group(0)))
                for param in params:
                    value = attrs.get(param)
                    if value is not None and is_literal(value):
                        instance_sites.append(
                            (
                                value,
                                path,
                                line_of(text, m.start()),
                                f"<{component} {param}=>",
                            )
                        )

    return direct, prop_defaults, instance_sites


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--summary", action="store_true", help="one-line result plus counts")
    ap.add_argument("--list", action="store_true", help="list every scanned reference")
    args = ap.parse_args()

    known = registered_icons()
    direct, prop_defaults, instance_sites = scan()
    everything = direct + prop_defaults + instance_sites

    if args.list:
        for name, path, line, how in sorted(everything, key=lambda r: (str(r[1]), r[2])):
            mark = "ok " if name in known else "BAD"
            rel = path.relative_to(PROJECT_ROOT)
            print(f"{mark} {name:28} {rel}:{line} ({how})")

    print(
        f"Scanned {len(everything)} icon references across {len(set(r[1] for r in everything))} "
        f"files: {len(direct)} direct, {len(prop_defaults)} prop defaults, "
        f"{len(instance_sites)} instance sites"
    )
    print(f"Codepoint table: {len(known)} registered names")
    # stdout block-buffers to a pipe and stderr does not, so without this the
    # failure list below lands ABOVE the counts it is supposed to follow.
    sys.stdout.flush()

    # An empty direct corpus means the tag pattern stopped matching, not that
    # the tree is clean. Fail rather than report a vacuous pass.
    if not direct:
        print(
            "\n❌ No <icon src=\"...\"> references found at all.\n"
            "   The scan pattern no longer matches the XML. Fix this script, not the tree.",
            file=sys.stderr,
        )
        return 2

    bad = [r for r in everything if r[0] not in known]
    if not bad:
        print("✅ Every icon name resolves to a registered codepoint")
        return 0

    print(f"\n❌ {len(bad)} icon name(s) are not in {CODEPOINTS_H.name}:\n", file=sys.stderr)
    for name, path, line, how in sorted(bad, key=lambda r: (str(r[1]), r[2])):
        rel = path.relative_to(PROJECT_ROOT)
        print(f"   {rel}:{line}  {how}  '{name}'", file=sys.stderr)
    print(
        "\nEach renders image_broken_variant at runtime with only a log warning.\n"
        "Use a registered name, or add the codepoint to include/ui_icon_codepoints.h\n"
        "(a NEW glyph also needs `make regen-fonts` and a rebuild; an alias to a\n"
        "codepoint already in the table does not).",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
