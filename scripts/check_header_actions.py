#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Gate: a header's actions slot holds header_action and nothing else.
#
# <header_bar-actions> and <overlay_panel-actions> are where a screen puts its header
# buttons, and ui_xml/header_action.xml owns how every one of them looks: height,
# circle or pill, fill by role, collapse on small screens. So:
#
#   errors (exit 1)
#     - an element in the slot other than <header_action> or a bind_* that acts on
#       the slot itself (motion_panel hides its whole slot below the small tier)
#     - a child of a header_action other than bind_* and event_cb
#     - a header_action attribute outside its API (name, icon, text, text_tag, role,
#       collapse, callback, hidden): a width, height or color is the component's call
#     - a role other than primary / neutral / danger, a collapse other than
#       auto / never, or a header_action with neither icon nor text
#   warnings (exit 0)
#     - more than 3 actions in one header, or more than one primary
#
# A header_action outside a slot (the AMS panels' own header rows) gets the same
# attribute and child checks.
#
# Usage:
#   ./scripts/check_header_actions.py              # ui_xml/ under the repo
#   ./scripts/check_header_actions.py --root DIR   # DIR/ui_xml

from __future__ import annotations

import argparse
import sys
import xml.parsers.expat
from pathlib import Path

SLOT_TAGS = {"header_bar-actions", "overlay_panel-actions"}
ACTION = "header_action"
ACTION_ATTRS = {"name", "icon", "text", "text_tag", "role", "collapse", "callback", "hidden"}
ROLES = {"primary", "neutral", "danger"}
COLLAPSE = {"auto", "never"}
ACTION_CHILDREN_EXTRA = {"event_cb"}
MAX_ACTIONS = 3


def is_binding(tag: str) -> bool:
    return tag.startswith("bind_")


class Checker:
    def __init__(self, path: Path, rel: str) -> None:
        self.rel = rel
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.stack: list[str] = []
        self.slots: list[dict] = []  # open slots: {"line", "actions", "primaries"}
        self.parser = xml.parsers.expat.ParserCreate()
        self.parser.StartElementHandler = self.start
        self.parser.EndElementHandler = self.end
        self.path = path

    def where(self) -> str:
        return f"{self.rel}:{self.parser.CurrentLineNumber}"

    def start(self, tag: str, attrs: dict[str, str]) -> None:
        parent = self.stack[-1] if self.stack else None
        if parent in SLOT_TAGS and tag != ACTION and not is_binding(tag):
            self.errors.append(
                f"{self.where()}: <{tag}> in <{parent}>: the actions slot takes only "
                f"<header_action> (and bind_* on the slot itself)")
        if parent == ACTION and not (is_binding(tag) or tag in ACTION_CHILDREN_EXTRA):
            self.errors.append(
                f"{self.where()}: <{tag}> inside <header_action>: only bind_* and "
                f"event_cb children act on it")
        if tag == ACTION:
            self.check_action(attrs, in_slot=parent in SLOT_TAGS)
        if tag in SLOT_TAGS:
            self.slots.append({"line": self.parser.CurrentLineNumber, "actions": 0,
                               "primaries": 0, "tag": tag})
        self.stack.append(tag)

    def end(self, tag: str) -> None:
        self.stack.pop()
        if tag in SLOT_TAGS:
            slot = self.slots.pop()
            here = f"{self.rel}:{slot['line']}"
            if slot["actions"] > MAX_ACTIONS:
                self.warnings.append(
                    f"{here}: <{tag}> has {slot['actions']} actions; a header holds "
                    f"{MAX_ACTIONS} at most before the title starves")
            if slot["primaries"] > 1:
                self.warnings.append(
                    f"{here}: <{tag}> has {slot['primaries']} primary actions; a screen "
                    f"has one main action")

    def check_action(self, attrs: dict[str, str], in_slot: bool) -> None:
        name = attrs.get("name", "<unnamed>")
        for attr in sorted(set(attrs) - ACTION_ATTRS):
            self.errors.append(
                f"{self.where()}: header_action {name} sets {attr}=: its look belongs to "
                f"ui_xml/header_action.xml (props: {', '.join(sorted(ACTION_ATTRS))})")
        role = attrs.get("role", "neutral")
        if role not in ROLES:
            self.errors.append(f"{self.where()}: header_action {name} role=\"{role}\" is "
                               f"not one of {', '.join(sorted(ROLES))}")
        collapse = attrs.get("collapse", "auto")
        if collapse not in COLLAPSE:
            self.errors.append(f"{self.where()}: header_action {name} collapse=\"{collapse}\" "
                               f"is not one of {', '.join(sorted(COLLAPSE))}")
        if not attrs.get("icon") and not attrs.get("text"):
            self.errors.append(f"{self.where()}: header_action {name} has neither icon nor text")
        if in_slot and self.slots:
            self.slots[-1]["actions"] += 1
            if role == "primary":
                self.slots[-1]["primaries"] += 1

    def run(self) -> None:
        try:
            self.parser.Parse(self.path.read_bytes(), True)
        except xml.parsers.expat.ExpatError as exc:
            self.errors.append(f"{self.rel}: not parseable XML ({exc})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args()

    errors: list[str] = []
    warnings: list[str] = []
    ui = args.root / "ui_xml"
    for path in sorted(ui.rglob("*.xml")):
        rel = path.relative_to(args.root).as_posix()
        if rel.startswith("ui_xml/translations/") or rel == "ui_xml/header_action.xml":
            continue
        checker = Checker(path, rel)
        checker.run()
        errors += checker.errors
        warnings += checker.warnings

    for w in warnings:
        print(f"warning: {w}")
    for e in errors:
        print(f"error: {e}")
    if errors:
        print(f"{len(errors)} header action error(s): see ui_xml/header_action.xml and "
              f"docs/devel/LVGL9_XML_GUIDE.md § \"Slots: filling a component from outside\"")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
