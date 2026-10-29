# SPDX-License-Identifier: GPL-3.0-or-later
"""Include lines the plugin adds to printer.cfg.

A feature that needs Klipper to read files the plugin writes adds two lines
to printer.cfg: a marker naming who added it, and an [include <dir>/*.cfg].
Both are matched on their stripped text, so whitespace never defeats a
removal and a user's own include of the same directory in any other spelling
is never taken for ours. Each feature passes its own pair, so removing one
never touches another's.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Callable, Optional, Tuple

import helix_gcode_reader

# printer.cfg's single rolling backup, beside the file.
BACKUP_SUFFIX = ".helixscreen-bak"


def include_present(text: str, line: str) -> bool:
    """True when the include is there, whosever hand added it."""
    return any(l.strip() == line for l in text.splitlines())


def strip_include(text: str, marker: str, line: str) -> Tuple[str, int]:
    """`text` without the marker and include, and how many lines went."""
    lines = text.splitlines(True)
    kept = [l for l in lines if l.strip() not in (marker, line)]
    return "".join(kept), len(lines) - len(kept)


def add_include(text: str, marker: str, line: str) -> Optional[str]:
    """`text` with the marker and include appended, or None when the include
    is already there. A marker left behind by a hand-removed include is
    dropped first, so the file never carries two."""
    if include_present(text, line):
        return None
    if any(l.strip() == marker for l in text.splitlines()):
        text, _ = strip_include(text, marker, line)
    prefix = text if text == "" or text.endswith("\n") else text + "\n"
    return prefix + marker + "\n" + line + "\n"


def write_change(path: Path, old_text: str, new_text: str,
                 replace: Callable[[str, str], None] = os.replace) -> None:
    """Back `path` up to its single rolling backup beside it, then write
    `new_text` atomically. The backup is the text the caller read, so it is
    the file as it stood; a failure at any point leaves `path` as it was, and
    a half-written config never exists."""
    backup = Path(path).with_name(Path(path).name + BACKUP_SUFFIX)
    with open(str(backup), "wb") as f:
        f.write(old_text.encode("utf-8"))
        f.flush()
        os.fsync(f.fileno())
    helix_gcode_reader.atomic_write_text(str(path), new_text, replace=replace)
