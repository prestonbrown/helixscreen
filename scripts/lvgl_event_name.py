#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a crash record's LVGL event_code the way the crash worker does.

lv_event_code_t numbering changes between LVGL releases, so a code means
nothing without the LVGL the crashing binary was built on. The crash worker
(server/crash-worker/src/index.ts) holds both tables: LVGL_9_5_EVENT_NAMES,
frozen, for records without lvgl_version, and CURRENT_LVGL_EVENT_NAMES,
generated from lib/lvgl, for "9.6" and later. This reads those same tables, so
a local triage and the filed issue name the same event.

    lvgl_event_name.py <event_code> [lvgl_version]

Prints the LV_EVENT_* name, or nothing when the code is not in the table.
"""

import re
import sys
from pathlib import Path

WORKER = Path(__file__).resolve().parent.parent / "server" / "crash-worker" / "src" / "index.ts"

# lv_event_get_code() masks this flag off; a preprocess dispatch records it in e->code.
PREPROCESS_FLAG = 0x8000


def load_table(source, name):
    """The {code: NAME} map of the TS object literal `const <name> ... = { ... };`."""
    m = re.search(r"const " + name + r"\b[^=]*=\s*\{(.*?)\n\};", source, re.S)
    if not m:
        raise SystemExit(f"lvgl_event_name: {name} not found in {WORKER}")
    return {int(code): label for code, label in re.findall(r'^\s*(\d+):\s*"([A-Z0-9_]+)"', m.group(1), re.M)}


def table_name_for(lvgl_version):
    m = re.match(r"(\d+)\.(\d+)", lvgl_version or "")
    if not m:
        return "LVGL_9_5_EVENT_NAMES"
    major, minor = int(m.group(1)), int(m.group(2))
    return "CURRENT_LVGL_EVENT_NAMES" if (major, minor) >= (9, 6) else "LVGL_9_5_EVENT_NAMES"


def event_name(code, lvgl_version, source=None):
    if source is None:
        source = WORKER.read_text()
    table = load_table(source, table_name_for(lvgl_version))
    label = table.get(code & ~PREPROCESS_FLAG)
    return f"LV_EVENT_{label}" if label else ""


def main(argv):
    if len(argv) < 2 or not argv[1].isdigit():
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    print(event_name(int(argv[1]), argv[2] if len(argv) > 2 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
