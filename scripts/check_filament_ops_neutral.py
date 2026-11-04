#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Wall: the filament-op surfaces never ask which backend they are on. They ask
# capability questions (AmsBackend::head_filament_ops() and the rest), so a
# `get_type()` call or an `AmsType::` name in one of them fails, with no
# baseline and no escape hatch. docs/devel/FILAMENT_MANAGEMENT.md has the
# contract.
#
# Scope is the filament-op code only. buffer_status_modal, ui_ams_detail,
# ui_wizard_summary and ui_panel_ams still branch on the type and are outside it
# until they are ported; the identify wizard is the one place the type belongs.
#
# Usage:
#   ./scripts/check_filament_ops_neutral.py
#   ./scripts/check_filament_ops_neutral.py --root DIR

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SCOPE = (
    "include/filament_action_set.h",
    "src/ui/filament/filament_action_set.cpp",
    "include/filament_op_dispatch.h",
    "include/filament_op_execute.h",
    "src/ui/filament/filament_op_execute.cpp",
    "include/ui_ams_sidebar.h",
    "src/ui/filament/ui_ams_sidebar.cpp",
    "include/ui_ams_context_menu.h",
    "src/ui/filament/ui_ams_context_menu.cpp",
    "include/ui_panel_filament.h",
    "src/ui/filament/ui_panel_filament.cpp",
    "include/ui_filament_runout_handler.h",
    "src/ui/filament/ui_filament_runout_handler.cpp",
    "src/ui/filament/ui_runout_guidance_modal.cpp",
    "src/ui/panel_widgets/filament_sensor_widget.cpp",
    "src/ui/panel_widgets/print_status_widget.cpp",
    "include/ui_batch_filament_modal.h",
    "src/ui/filament/ui_batch_filament_modal.cpp",
    "src/ui/filament/ui_unload_lane_choice.cpp",
    "src/ui/filament/ui_manual_pull_prompt.cpp",
)

SHAPE = re.compile(r"\bget_type\s*\(\s*\)|\bAmsType::")
LINE_COMMENT = re.compile(r"//.*$")


def code_lines(text: str):
    """Yield (line_number, code) with comments removed."""
    in_block = False
    for number, line in enumerate(text.splitlines(), start=1):
        code = ""
        rest = line
        while rest:
            if in_block:
                end = rest.find("*/")
                if end < 0:
                    rest = ""
                    break
                rest = rest[end + 2 :]
                in_block = False
                continue
            start = rest.find("/*")
            if start < 0:
                code += rest
                break
            code += rest[:start]
            rest = rest[start + 2 :]
            in_block = True
        yield number, LINE_COMMENT.sub("", code)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = parser.parse_args()
    root = Path(args.root)

    hits = []
    for rel in SCOPE:
        path = root / rel
        if not path.is_file():
            continue
        for number, code in code_lines(path.read_text(encoding="utf-8", errors="replace")):
            if SHAPE.search(code):
                hits.append(f"{rel}:{number}: {code.strip()}")

    if hits:
        print("Filament-op code asks which backend it is on. Ask a capability instead:")
        for hit in hits:
            print(f"  {hit}")
        print(f"{len(hits)} backend-type reference(s) in filament-op code")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
