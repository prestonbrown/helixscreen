#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Gate: a hold handler that acts ends its press, so the release is not also a tap.
#
# Taps bind CLICKED, which LVGL sends on every release, the one ending a long press
# included. The hold handler is the only code that knows whether the hold meant
# something, so when it acts it calls helix::ui::end_press() (include/ui_utils.h); a
# handler that declines leaves the press alone and the release is still the tap.
# See docs/devel/LVGL9_XML_GUIDE.md § "Tap and hold".
#
# Every long_pressed handler is found and its body read:
#   - XML  <event_cb trigger="long_pressed" callback="NAME"/>, NAME resolved through
#     the C++ registration that names it ("NAME", lambda-or-function). A $prop callback
#     is resolved through every instance of the component that sets the prop.
#   - C++  lv_obj_add_event_cb(obj, FN, LV_EVENT_LONG_PRESSED, ...).
# A body passes when it calls end_press( or carries `HOLD_DECLINES_OK: <reason>` (a
# gesture machine that needs the release, such as the keyboard's alternates popover).
# A handler that cannot be found is reported too, so the gate fails closed.
#
# The handler's own body is read, and the body of any function it hands its event to
# (`helper(e, ...)`), one level deep. A handler that delegates the decision to a method
# without the event calls end_press itself on that method's answer, which keeps the
# decision visible here.
#
# Usage:
#   ./scripts/check_hold_ends_press.py              # src/ + ui_xml/ under the repo
#   ./scripts/check_hold_ends_press.py --root DIR   # DIR/src + DIR/ui_xml

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

PASS_MARKERS = ("end_press(", "HOLD_DECLINES_OK")

CPP_REGISTRATION = re.compile(
    r"lv_obj_add_event_cb\(\s*[^,]+,\s*&?([A-Za-z_][\w:]*)\s*,\s*LV_EVENT_LONG_PRESSED\b")


def skip_literal(text: str, i: int) -> int:
    """Index just past the string/char literal or comment starting at i, else i."""
    if text.startswith("//", i):
        end = text.find("\n", i)
        return len(text) if end < 0 else end
    if text.startswith("/*", i):
        end = text.find("*/", i + 2)
        return len(text) if end < 0 else end + 2
    if text[i] in "\"'":
        quote = text[i]
        j = i + 1
        while j < len(text) and text[j] != quote:
            j += 2 if text[j] == "\\" else 1
        return j + 1
    return i


def brace_body(text: str, open_idx: int) -> str | None:
    """The text from the '{' at open_idx through its matching '}'."""
    depth = 0
    i = open_idx
    while i < len(text):
        j = skip_literal(text, i)
        if j != i:
            i = j
            continue
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx:i + 1]
        i += 1
    return None


def lambda_body(text: str, start: int) -> str | None:
    """The lambda whose capture list opens at text[start] == '[', parameters and body."""
    paren = text.find("(", start)
    if paren < 0:
        return None
    depth = 0
    i = paren
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    brace = text.find("{", i)
    body = brace_body(text, brace) if brace >= 0 else None
    return text[start:brace] + body if body is not None else None


def function_body(sources: dict[Path, str], name: str, near: Path | None = None) -> str | None:
    """The definition of `name` whose first parameter is an lv_event_t*, signature and
    body. The file `near` is searched first: two classes may each define a handler of
    the same name, and the registration's own file holds the one it means."""
    bare = name.split("::")[-1]
    pattern = re.compile(
        r"\b(?:\w+::)*" + re.escape(bare) +
        r"\s*\(\s*lv_event_t\s*\*\s*\w*\s*(?:,[^)]*)?\)\s*(?:const\s*)?\{")
    order = sorted(sources, key=lambda p: p != near)
    for path in order:
        text = sources[path]
        if bare not in text:  # the regex costs a scan per word; most files lack the name
            continue
        m = pattern.search(text)
        if m:
            body = brace_body(text, m.end() - 1)
            return text[m.start():m.end() - 1] + body if body is not None else None
    return None


def ends_or_declines(sources: dict[Path, str], body: str) -> bool:
    """True when the body, or a function it passes its event to, ends the press or
    says why it never does."""
    if any(m in body for m in PASS_MARKERS):
        return True
    param = re.match(r"[^{]*?lv_event_t\s*\*\s*(\w+)", body)
    event = param.group(1) if param else "e"
    for call in re.finditer(r"\b([A-Za-z_]\w*)\s*\(\s*" + re.escape(event) + r"\s*[,)]", body):
        callee = function_body(sources, call.group(1))
        if callee and any(m in callee for m in PASS_MARKERS):
            return True
    return False


def registered_body(sources: dict[Path, str], name: str) -> str | None:
    """Body of the callback registered for XML callback `name`."""
    pattern = re.compile(r'"' + re.escape(name) + r'"\s*,\s*')
    for path, text in sources.items():
        for m in pattern.finditer(text):
            rest = text[m.end():]
            if rest.startswith("["):
                return lambda_body(text, m.end())
            ident = re.match(r"&?([A-Za-z_][\w:]*)", rest)
            if ident:
                return function_body(sources, ident.group(1), path)
    return None


def xml_hold_callbacks(xml_root: Path) -> tuple[list[tuple[str, str]], list[str]]:
    """(where, callback name) for every long_pressed event_cb; plus parse errors."""
    found: list[tuple[str, str]] = []
    errors: list[str] = []
    prop_users: list[tuple[str, str, str]] = []  # (component, prop, where)
    files = sorted(xml_root.rglob("*.xml"))
    texts = {f: f.read_text(encoding="utf-8", errors="replace") for f in files}
    for f in files:
        try:
            tree = ET.parse(f)
        except ET.ParseError as e:
            errors.append(f"{f}: does not parse: {e}")
            continue
        for el in tree.iter("event_cb"):
            if el.get("trigger") != "long_pressed":
                continue
            cb = el.get("callback", "")
            if cb.startswith("$"):
                prop_users.append((f.stem, cb[1:], str(f)))
            elif cb:
                found.append((str(f), cb))
    for component, prop, where in prop_users:
        use = re.compile(r"<" + re.escape(component) + r"\b[^>]*\b" + re.escape(prop) +
                         r'="([^"$]+)"')
        names = {m.group(1) for text in texts.values() for m in use.finditer(text)}
        if not names:
            errors.append(f"{where}: no instance of <{component}> sets {prop}")
        for name in sorted(names):
            found.append((f"{where} (via {prop})", name))
    return found, errors


def check(root: Path) -> list[str]:
    src = root / "src"
    sources = {p: p.read_text(encoding="utf-8", errors="replace")
               for p in sorted(src.rglob("*.cpp"))} if src.is_dir() else {}
    problems: list[str] = []

    xml_dir = root / "ui_xml"
    if xml_dir.is_dir():
        callbacks, errors = xml_hold_callbacks(xml_dir)
        problems.extend(errors)
        for where, name in callbacks:
            body = registered_body(sources, name)
            if body is None:
                problems.append(f"{where}: cannot find the C++ handler registered as '{name}'")
            elif not ends_or_declines(sources, body):
                problems.append(f"{where}: hold handler '{name}' neither calls end_press() "
                                "nor says HOLD_DECLINES_OK")

    for path, text in sources.items():
        if "LV_EVENT_LONG_PRESSED" not in text:
            continue
        for m in CPP_REGISTRATION.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            body = function_body(sources, m.group(1), path)
            if body is None:
                problems.append(f"{path}:{line}: cannot find hold handler '{m.group(1)}'")
            elif not ends_or_declines(sources, body):
                problems.append(f"{path}:{line}: hold handler '{m.group(1)}' neither calls "
                                "end_press() nor says HOLD_DECLINES_OK")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    problems = check(args.root)
    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} hold handler(s) can leave their release firing the tap. "
              "See docs/devel/LVGL9_XML_GUIDE.md § \"Tap and hold\".")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
