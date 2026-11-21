#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: overlay width is decided at push time, never in XML.
#
# Background (#1178). theme_manager registers two overlay widths:
#
#   overlay_width_transient    = screen - nav - space_lg
#   overlay_width_destination  = screen - nav
#
# They are not spacing options. A destination occludes the backdrop entirely —
# it is a place you park, and its drill-downs are part of it. A transient layer
# leaves the backdrop showing at the leading edge — you opened it over something
# and will return.
#
# Which one an overlay gets depends on HOW THE USER REACHED IT, not on the
# overlay itself. fan_control_overlay is a transient layer when opened from
# Controls and a drill-down when opened from Settings > Fans. power_panel and
# settings_sensors have the same split. No static `width=` attribute can be
# correct for those, so NavigationManager::push_overlay() resolves the class
# against the live stack and writes the width.
#
# Before the gate existed, authors picked a constant by hand. 20 panels ended up
# gapped and 36 full with no stated rule, `overlay_panel.xml` defaulted to the
# minority variant so new panels silently inherited the wrong look, and
# console_settings_overlay rendered 16px WIDER than the console_panel it was
# pushed from.
#
# WHAT IS FLAGGED
#   1. Any XML naming #overlay_width_destination. Destination-ness is declared
#      in C++ by IPanelLifecycle::is_destination(), never in markup.
#   2. A navigation width constant passed as `width=` to an element that
#      extends overlay_panel. The component already bakes in the transient
#      width; push_overlay upgrades it. Hand-picking either constant there is
#      the exact mistake that produced #1178.
#
# NOT FLAGGED
#   - Hand-rolled overlay roots (extends="lv_obj" with align="right_mid") using
#     width="#overlay_width_transient". Those predate the overlay_panel
#     component and still need a born-width, because setup() runs before the
#     push and several of them measure their own layout there. They may name
#     the transient constant and nothing else.
#   - A deliberate custom width on an overlay_panel — widget_catalog_overlay is
#     width="70%" so the home grid stays visible behind it. Neither navigation
#     class applies. Such an overlay MUST call
#     NavigationManager::set_overlay_width_unmanaged() or push_overlay will
#     overwrite the width it chose.

import argparse
import re
import subprocess
import sys
from pathlib import Path

from path_scope import in_skipped_dir
from staged_content import staged_files

SCAN_DIRS = ('ui_xml',)

# android/app/src/main/assets/ui_xml/ is a gradle copy of ui_xml/
# (android/app/build.gradle: from('../../ui_xml')), not a source of truth.
SKIP_PARTS = ('android', 'build', '.worktrees')

DESTINATION_CONST = 'overlay_width_destination'

# <foo ... extends="overlay_panel" ...> spanning newlines, up to the closing >
OVERLAY_PANEL_ELEMENT = re.compile(
    r'<[A-Za-z_][\w.-]*\b[^>]*?extends="overlay_panel"[^>]*?>', re.DOTALL)
# width= naming either navigation class. A custom width (70%, a px literal) is
# legal on an overlay_panel and is covered by set_overlay_width_unmanaged().
CLASS_WIDTH_ATTR = re.compile(
    r'\bwidth\s*=\s*"#overlay_width_(?:transient|destination)"')


def line_of(text: str, index: int) -> int:
    return text.count('\n', 0, index) + 1


def scan_source(path: str, text: str) -> list[tuple[str, int, str]]:
    """Return (path, line, message) for each violation. `path` is only carried
    into the report - `text` is already sourced, from disk or the index."""
    out: list[tuple[str, int, str]] = []

    for m in re.finditer(re.escape(DESTINATION_CONST), text):
        out.append((
            path, line_of(text, m.start()),
            f'names #{DESTINATION_CONST}. Destination width is declared in C++ '
            'by overriding IPanelLifecycle::is_destination(), not in XML.'))

    for m in OVERLAY_PANEL_ELEMENT.finditer(text):
        w = CLASS_WIDTH_ATTR.search(m.group(0))
        if w:
            out.append((
                path, line_of(text, m.start() + w.start()),
                'hand-picks a navigation width class on an overlay_panel. '
                'push_overlay() resolves it from how the user got here; drop the '
                'width attribute.'))

    return out


def collect_files(args) -> list[tuple[str, str]]:
    """Yield (path, text) for every file to scan, content already sourced.

    --staged-only reads each staged XML file's INDEX content - what the
    commit will contain, not whatever the working tree holds right now.
    """
    if args.files:
        out = []
        for f in args.files:
            try:
                out.append((f, Path(f).read_text(encoding='utf-8')))
            except (OSError, UnicodeDecodeError):
                continue
        return out

    if args.staged_only:
        return [(p, t) for p, t in staged_files(suffixes=('.xml',))
                if not in_skipped_dir(p, '.', SKIP_PARTS)]

    files = []
    for d in SCAN_DIRS:
        for p in sorted(Path(d).rglob('*.xml')):
            if in_skipped_dir(p, '.', SKIP_PARTS):
                continue
            try:
                files.append((str(p), p.read_text(encoding='utf-8')))
            except (OSError, UnicodeDecodeError):
                continue
    return files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help=f'XML files to check (default: scan {SCAN_DIRS[0]}/)')
    ap.add_argument('--staged-only', action='store_true', help='Check only staged XML files')
    args = ap.parse_args()

    if not args.files:
        root = subprocess.run(['git', 'rev-parse', '--show-toplevel'],
                              capture_output=True, text=True, check=False).stdout.strip()
        if root:
            import os
            os.chdir(root)

    findings: list[tuple[str, int, str]] = []
    for path, text in collect_files(args):
        findings += scan_source(path, text)

    if not findings:
        return 0

    print('Overlay width must be resolved at push time, not set in XML (#1178).\n')
    for path, line, msg in sorted(findings):
        print(f'  {path}:{line}: {msg}')
    print(f'\n{len(findings)} violation(s). See include/overlay_class.h.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
