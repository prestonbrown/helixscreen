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
gives the tap feedback. --exempt names the screens the firmware never opens,
one staged ui_xml/ path per line: a finding every one of whose bindings sits in
a listed screen is exempt, and any binding elsewhere fails. A listed path the
firmware does not ship fails too, so the list cannot outlive its screens.

Also a hard failure, with no exemption: firmware-shipped XML (ui_xml/ less micro/,
with ui_xml_overrides/ winning) using a widget tag whose LV_USE_* the firmware's
lv_conf.h sets to 0. helix-xml registers each tag under its LV_USE_*, so the device
would only log "Couldn't create widget" and leave the element out. The tag-to-option
map is read from helix-xml's own registration table.

Usage:
  check_esp32_xml_bindings.py                    # count
  check_esp32_xml_bindings.py --list             # every finding, with where it is bound
  check_esp32_xml_bindings.py --exempt FILE      # fail on a finding outside the listed screens
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


def findings(root: Path) -> dict[str, tuple[str, set[str]]]:
    """'kind:name' -> (where the desktop registers it, where the firmware binds it).

    A binding site is a shipped XML file by staged path, or a firmware-compiled
    C++ file naming a callback through a component prop."""
    fw = firmware_files(root)
    fw_rel = {str(f.relative_to(root)) for f in fw}
    read = compiled_text_reader(root)
    shipped = firmware_xml_files(root)
    staged = {str(src.relative_to(root)): str(rel) for rel, src in shipped.items()}
    out = {}

    cb_all = callbacks.collect_registrations(root)
    cb_fw = callbacks.collect_registrations(root, fw, read)
    for name, files in callbacks.collect_xml_refs(root, list(shipped.values())).items():
        sites = {staged.get(f, f) for f in files if f in staged or f in fw_rel}
        if sites and name in cb_all and name not in cb_fw:
            out[f"callback:{name}"] = (", ".join(sorted(cb_all[name])), sites)

    subj_all = subjects.collect_registrations(root)[0]
    subj_fw = subjects.collect_registrations(root, fw, read)[0]
    subj_sites: dict[str, set[str]] = {}
    for rel, src in shipped.items():
        for name in subjects.collect_xml_refs(root, [src], ignore_attrs=("name",)):
            subj_sites.setdefault(name, set()).add(str(rel))
    for name, sites in subj_sites.items():
        if name in subj_all and name not in subj_fw:
            out[f"subject:{name}"] = (", ".join(sorted(subj_all[name])), sites)
    return out


def read_exempt(path: Path) -> set[str]:
    return {ln.strip() for ln in path.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}


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
    ap.add_argument("--exempt", type=Path, help="screens the firmware never opens")
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args()

    uses = disabled_widget_uses(args.repo_root.resolve())
    if uses:
        print(f"❌ ESP32 XML: {len(uses)} use(s) of a widget the firmware compiles out:")
        for use in uses:
            print(f"     {use}")
        return 1

    root = args.repo_root.resolve()
    found = findings(root)
    if args.list:
        for k in sorted(found):
            registered, sites = found[k]
            print(f"{k}  (registered: {registered}; bound in: {', '.join(sorted(sites))})")

    if not args.exempt:
        print(f"firmware-unregistered XML bindings: {len(found)}")
        return 0

    exempt = read_exempt(args.exempt)
    shipped = {str(rel) for rel in firmware_xml_files(root)}
    stale = sorted(exempt - shipped)
    failed = False
    if stale:
        print(f"❌ ESP32 XML bindings: {len(stale)} screen(s) in {args.exempt} "
              f"the firmware does not ship; remove them:")
        for rel in stale:
            print(f"     {rel}")
        failed = True
    live = {k: (reg, sites - exempt) for k, (reg, sites) in found.items() if sites - exempt}
    if live:
        print(f"❌ ESP32 XML bindings: {len(live)} registered only off-firmware, bound "
              f"outside the screens {args.exempt} exempts:")
        for k in sorted(live):
            registered, sites = live[k]
            print(f"     {k}  (registered: {registered}; bound in: {', '.join(sorted(sites))})")
        failed = True
    if failed:
        return 1
    print(f"✅ ESP32 XML bindings: {len(found)} off-firmware, all inside the "
          f"{len(exempt)} screens {args.exempt} exempts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
