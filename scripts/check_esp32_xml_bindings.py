#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: an XML binding is registered in a file and branch the firmware compiles.

The ESP32 firmware ships all of ui_xml/ but compiles only the sources in
app_srcs.txt, with its own HELIX_HAS_* set. A binding whose only registration
sits in an excluded file, or behind a gate the firmware compiles out, resolves
to nothing on the device: a bound label shows its placeholder text, a hidden-if
flag never applies, a tapped row does nothing. Nothing fails on the desktop,
where every registration is compiled.

Flagged: a name XML references that some src/ or include/ file registers, but
no file the firmware compiles registers in a branch the firmware compiles.
"Compiled" is app_srcs.txt, include/, and the firmware components' own sources,
with #if branches evaluated against the helixapp CMake definitions. A name
nothing registers anywhere is check_orphan_callbacks.py's business (or is built
at runtime) and is not counted here.

Outside the gate: a registration that is compiled but never called. This reads
where registrations are written, not whether the firmware's boot path reaches
them, so an init_subjects() that app_boot.cpp never calls passes here and
binds to nothing on the device.

Fix a finding by registering the name on the firmware: a stub with the
platform's real value (0 hides a row bound hidden-if-0), or a callback that
gives the tap feedback. A RATCHET keyed on names: --baseline lists accepted
debt (bindings inside panels the firmware never opens).

Also a hard failure, with no baseline: firmware-shipped XML (ui_xml/ less micro/,
with ui_xml_overrides/ winning) using a widget tag whose LV_USE_* the firmware's
lv_conf.h sets to 0. helix-xml registers each tag under its LV_USE_*, so the device
would only log "Couldn't create widget" and leave the element out. The tag-to-option
map is read from helix-xml's own registration table.

Usage:
  check_esp32_xml_bindings.py                    # count
  check_esp32_xml_bindings.py --list             # every finding
  check_esp32_xml_bindings.py --baseline FILE [--write-baseline]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_orphan_callbacks as callbacks  # noqa: E402
import check_orphan_subjects as subjects  # noqa: E402
from check_esp32_app_srcs import firmware_code_lines, firmware_defines, load_manifest  # noqa: E402

FIRMWARE = Path("firmware/helixscreen-esp32")
HELIXAPP = FIRMWARE / "components/helixapp"


def firmware_files(root: Path) -> list[Path]:
    manifest, _ = load_manifest(root / HELIXAPP / "app_srcs.txt")
    files = {root / rel for rel in manifest if (root / rel).is_file()}
    files.update((root / "include").rglob("*.h"))
    for comp in (root / FIRMWARE / "components").glob("helix*"):
        files.update(f for f in comp.rglob("*") if f.suffix in (".cpp", ".h"))
    return sorted(files)


def compiled_text_reader(root: Path):
    """Read a file with every line the firmware compiles out blanked."""
    cmake = (root / HELIXAPP / "CMakeLists.txt").read_text(errors="replace")
    defines = firmware_defines(cmake, root)

    def read(path: Path) -> str:
        text = path.read_text(errors="replace")
        live = {n for n, _ in firmware_code_lines(text, defines)}
        return "\n".join(line if n in live else ""
                         for n, line in enumerate(text.splitlines(), 1))
    return read


def findings(root: Path) -> dict[str, str]:
    """'kind:name' -> where the desktop registers it."""
    fw = firmware_files(root)
    read = compiled_text_reader(root)
    out = {}

    cb_all = callbacks.collect_registrations(root)
    cb_fw = callbacks.collect_registrations(root, fw, read)
    for name in callbacks.collect_xml_refs(root):
        if name in cb_all and name not in cb_fw:
            out[f"callback:{name}"] = ", ".join(sorted(cb_all[name]))

    subj_all = subjects.collect_registrations(root)[0]
    subj_fw = subjects.collect_registrations(root, fw, read)[0]
    for name in subjects.collect_xml_refs(root):
        if name in subj_all and name not in subj_fw:
            out[f"subject:{name}"] = ", ".join(sorted(subj_all[name]))
    return out


def registered_widgets(lv_xml_c: str) -> dict[str, list[str]]:
    """helix-xml's widget tag -> every LV_USE_* option its registration sits under."""
    tags: dict[str, list[str]] = {}
    guards: list[str | None] = []
    for line in lv_xml_c.splitlines():
        stripped = line.strip()
        if stripped.startswith("#if"):
            m = re.match(r"#if\s+(LV_USE_\w+)\s*$", stripped)
            guards.append(m.group(1) if m else None)
        elif stripped.startswith("#endif"):
            if guards:
                guards.pop()
        else:
            m = re.search(r'lv_xml_register_widget\("([\w-]+)"', stripped)
            options = [g for g in guards if g]
            if m and options:
                tags[m.group(1)] = options
    return tags


def firmware_xml_files(root: Path) -> dict[Path, Path]:
    """The XML the firmware ships, by staged path: ui_xml/ less micro/ and translations/,
    with ui_xml_overrides/ replacing the file at the same path."""
    files = {}
    ui_xml = root / "ui_xml"
    for src in sorted(ui_xml.rglob("*.xml")):
        rel = src.relative_to(ui_xml)
        if rel.parts[0] not in ("micro", "translations"):
            files[rel] = src
    overrides = root / FIRMWARE / "ui_xml_overrides"
    if overrides.is_dir():
        for src in sorted(overrides.rglob("*.xml")):
            files[src.relative_to(overrides)] = src
    return files


def disabled_widget_uses(root: Path) -> list[str]:
    """Each firmware-shipped use of a widget tag the firmware's lv_conf.h compiles out."""
    conf = root / FIRMWARE / "components/helixcore/lv_conf.h"
    lv_xml_c = root / "lib/helix-xml/src/xml/lv_xml.c"
    if not conf.is_file() or not lv_xml_c.is_file():
        return []
    disabled = set(re.findall(r"^\s*#define\s+(LV_USE_\w+)\s+0\b", conf.read_text(), re.M))
    tags = registered_widgets(lv_xml_c.read_text())
    if not tags:
        return [f"{lv_xml_c}: no lv_xml_register_widget under an LV_USE_* guard; cannot judge"]
    off = {tag: option for tag, options in tags.items() for option in options if option in disabled}
    uses = []
    for rel, src in firmware_xml_files(root).items():
        for tag in re.findall(r"<([\w-]+)", src.read_text(encoding="utf-8", errors="replace")):
            if tag in off:
                uses.append(f"{rel}: <{tag}> needs {off[tag]}, which the firmware lv_conf.h sets to 0")
    return sorted(set(uses))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args()

    uses = disabled_widget_uses(args.repo_root.resolve())
    if uses:
        print(f"❌ ESP32 XML: {len(uses)} use(s) of a widget the firmware compiles out:")
        for use in uses:
            print(f"     {use}")
        return 1

    found = findings(args.repo_root.resolve())
    if args.list:
        for k in sorted(found):
            print(f"{k}  ({found[k]})")

    if args.write_baseline:
        if not args.baseline:
            print("--write-baseline needs --baseline PATH")
            return 2
        header = ("# XML bindings the desktop registers and the ESP32 firmware does not.\n"
                  "# Accepted debt, shrink-only: register the name on the firmware, then\n"
                  "# re-run scripts/check_esp32_xml_bindings.py with --write-baseline.\n")
        args.baseline.write_text(header + "".join(k + "\n" for k in sorted(found)))
        print(f"wrote {args.baseline}: {len(found)} findings")
        return 0

    if not args.baseline:
        print(f"firmware-unregistered XML bindings: {len(found)}")
        return 0

    accepted = callbacks.read_baseline(args.baseline)
    new = sorted(set(found) - accepted)
    if new:
        print(f"❌ ESP32 XML bindings: {len(new)} registered only off-firmware, "
              f"not in {args.baseline}:")
        for k in new:
            print(f"     {k}  ({found[k]})")
        return 1
    gone = sorted(accepted - set(found))
    if gone:
        print(f"✅ ESP32 XML bindings: {len(found)} accepted, {len(gone)} fewer than "
              f"{args.baseline}; re-run with --write-baseline to hold the gain "
              f"({', '.join(gone)}).")
        return 0
    print(f"✅ ESP32 XML bindings: {len(found)} findings, all accepted debt in {args.baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
