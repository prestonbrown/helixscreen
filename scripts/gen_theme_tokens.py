#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate src/generated/theme_token_table.cpp from ui_xml/*.xml.

Mirrors theme_manager.cpp's runtime scan exactly: top-level *.xml only
(no recursion), files sorted alphabetically, collecting the name=/value=
attributes of every <color>, <px> and <string> element at any depth,
last-wins on duplicate (type, name) across and within files.

Usage: gen_theme_tokens.py [--check]
  --check: regenerate to a string and exit 1 if the committed file differs.
"""
import os
import sys
import xml.parsers.expat as expat

UI_XML_DIR = "ui_xml"
OUT_PATH = "src/generated/theme_token_table.cpp"
TYPES = ("color", "px", "string")


def cstr(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def collect() -> dict:
    # Uses raw xml.parsers.expat (no namespace processing) rather than
    # xml.etree.ElementTree, matching theme_manager.cpp's own
    # XML_ParserCreate(nullptr) exactly. ui_xml/*.xml uses colon-suffixed
    # pseudo-attributes for state variants (e.g. style_text_color:checked)
    # which ElementTree's namespace-aware parser rejects as an
    # "unbound prefix" — plain expat treats the colon as a literal
    # character, same as the C++ scanner.
    tokens = {}  # (type, name) -> value, insertion order preserved
    files = sorted(
        f for f in os.listdir(UI_XML_DIR)
        if f.endswith(".xml") and os.path.isfile(os.path.join(UI_XML_DIR, f))
    )
    for fname in files:
        path = os.path.join(UI_XML_DIR, fname)
        with open(path, "rb") as f:
            content = f.read()
        if not content:
            continue

        def start_element(name, attrs, _tokens=tokens):
            if name not in TYPES:
                return
            name_attr = attrs.get("name")
            value_attr = attrs.get("value")
            if name_attr is not None and value_attr is not None:
                _tokens[(name, name_attr)] = value_attr

        parser = expat.ParserCreate()
        parser.StartElementHandler = start_element
        try:
            parser.Parse(content, True)
        except expat.ExpatError as e:
            # Runtime scanner keeps partial results on parse errors; a
            # lint-clean tree should never hit this. Fail loudly instead.
            print(f"error: {path}: {e}", file=sys.stderr)
            sys.exit(1)
    return tokens


def render(tokens: dict) -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// GENERATED FILE — DO NOT EDIT. Regenerate with: make regen-tokens",
        "// Source of truth: ui_xml/*.xml (top level, sorted, last-wins).",
        "// Parity-gated against the runtime scanner by test_theme_token_table.cpp.",
        '#include "theme_token_table.h"',
        "",
        "namespace helix::theme_tokens {",
        "",
        "const TokenEntry k_token_table[] = {",
    ]
    for (t, name), value in tokens.items():
        lines.append(f"    {{{cstr(t)}, {cstr(name)}, {cstr(value)}}},")
    lines += [
        "};",
        "",
        "const size_t k_token_table_count = sizeof(k_token_table) / sizeof(k_token_table[0]);",
        "",
        "} // namespace helix::theme_tokens",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    if not os.path.isdir(UI_XML_DIR):
        print("error: run from the repo root (ui_xml/ not found)", file=sys.stderr)
        return 1
    content = render(collect())
    if "--check" in sys.argv:
        try:
            with open(OUT_PATH) as f:
                if f.read() == content:
                    print("token table is up to date")
                    return 0
        except FileNotFoundError:
            pass
        print(f"STALE: {OUT_PATH} — run 'make regen-tokens'", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w") as f:
        f.write(content)
    print(f"wrote {OUT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
