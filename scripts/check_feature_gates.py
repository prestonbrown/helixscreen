#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every HELIX_HAS_* feature gate has its default in include/helix_features.h.

An undefined identifier in `#if` evaluates to 0, so a build that forgets to pass a gate
compiles its feature out with no error. include/helix_features.h gives every gate its
default under an #ifndef guard, every build force-includes it through lvgl_pch.h, and a
build overrides only what it changes. This gate keeps that the one place:

  - a gate tested in `#if`/`#ifdef`/`defined()` in src/, include/ or the firmware
    components must be declared in the header, defined by some header for itself
    (a derived flag such as lv_conf.h's HELIX_HAS_CAMERA), or be a presence flag the
    build sets by probing the host (PROBED);
  - each default in the header sits under its own #ifndef, so -D still overrides it;
  - no other file sets a second default for a gate the header declares;
  - a build file (Makefile, mk/*.mk, the Android and firmware CMakeLists) passes no
    HELIX_HAS_* name the header does not know, which catches a misspelt override.

Exit 0 clean, 1 with findings, 2 when the header is missing (an empty declared set would
pass everything). Usage: check_feature_gates.py [--root DIR]
"""

import argparse
import re
import sys
from pathlib import Path

HEADER = "include/helix_features.h"

# Presence flags the build defines (or not) from what the host offers; tested with
# #ifdef, so they have no default to declare.
PROBED = {
    "HELIX_HAS_SOUND",
    "HELIX_HAS_TRACKER",
    "HELIX_HAS_ALSA",
    "HELIX_HAS_SYSTEMD",
    "HELIX_HAS_LIBUSB",
    "HELIX_HAS_PWM_SOUND",
    "HELIX_HAS_JZ_PWM",
    "HELIX_HAS_PCM_PIPE",
}

SOURCE_DIRS = ("src", "include", "firmware/helixscreen-esp32/components")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".mm"}
EXTRA_SOURCES = ("lv_conf.h",)
CMAKE_FILES = (
    "android/app/jni/CMakeLists.txt",
    "firmware/helixscreen-esp32/components/helixapp/CMakeLists.txt",
)

GATE = re.compile(r"\bHELIX_HAS_[A-Z0-9_]+\b")
CONDITIONAL = re.compile(r"^\s*#\s*(if|elif|ifdef|ifndef)\b(.*)")
DEFINE = re.compile(r"^\s*#\s*define\s+(HELIX_HAS_[A-Z0-9_]+)\b")
# The one form the Makefile's sed reads a default from.
MAKEFILE_FORM = re.compile(r"#define (HELIX_HAS_[A-Z0-9_]+) ([0-9]+)")
PRESENCE_TEST = re.compile(r"^\s*#\s*(?:ifdef|ifndef)\s+(HELIX_HAS_[A-Z0-9_]+)|\bdefined\s*\(?\s*(HELIX_HAS_[A-Z0-9_]+)")
IFNDEF = re.compile(r"^\s*#\s*ifndef\s+(HELIX_HAS_[A-Z0-9_]+)\s*$")
# A name a make expansion completes (-DHELIX_HAS_MDI_ICONS_$(n)) is built at parse time.
DASH_D = re.compile(r"-D(HELIX_HAS_[A-Z0-9_]+)\b(?!\$)")


def strip_line_comment(text, marker):
    return text.split(marker, 1)[0]


def source_files(root):
    for rel in SOURCE_DIRS:
        base = root / rel
        if base.is_dir():
            for path in sorted(base.rglob("*")):
                if path.suffix in SOURCE_SUFFIXES and path.is_file():
                    yield path
    for rel in EXTRA_SOURCES:
        path = root / rel
        if path.is_file():
            yield path


def previous_code_line(lines, index):
    for j in range(index - 1, -1, -1):
        stripped = lines[j].strip()
        if stripped and not stripped.startswith("//"):
            return stripped
    return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=Path(__file__).resolve().parent.parent, type=Path)
    root = parser.parse_args().root.resolve()

    header = root / HEADER
    if not header.is_file():
        print(f"feature gates: {HEADER} is missing; nothing to check against")
        return 2

    findings = []
    declared = set()
    header_lines = header.read_text(errors="replace").splitlines()
    for i, line in enumerate(header_lines):
        m = DEFINE.match(line)
        if not m:
            continue
        name = m.group(1)
        if not MAKEFILE_FORM.fullmatch(line):
            findings.append(f"{HEADER}:{i + 1}: write `#define {name} <digits>` with nothing "
                            f"after it; the Makefile reads only that form")
        declared.add(name)
        guard = IFNDEF.match(previous_code_line(header_lines, i))
        if not guard or guard.group(1) != name:
            findings.append(f"{HEADER}:{i + 1}: {name}'s default needs its own #ifndef "
                            f"{name} guard, or a build's -D cannot override it")
    if not declared:
        print(f"feature gates: {HEADER} declares no HELIX_HAS_* gate")
        return 2

    self_defined = set()
    uses = []
    for path in source_files(root):
        rel = path.relative_to(root).as_posix()
        if rel == HEADER:
            continue
        lines = path.read_text(errors="replace").splitlines()
        for i, line in enumerate(lines):
            m = DEFINE.match(line)
            if m:
                name = m.group(1)
                if name in declared:
                    findings.append(f"{rel}:{i + 1}: defines {name} again; its default lives "
                                    f"in {HEADER} and a build overrides it with -D")
                else:
                    self_defined.add(name)
                continue
            c = CONDITIONAL.match(line)
            if c:
                for groups in PRESENCE_TEST.findall(strip_line_comment(line, "//")):
                    name = groups[0] or groups[1]
                    if name in declared:
                        findings.append(f"{rel}:{i + 1}: tests whether {name} is defined, "
                                        f"which every build makes true; test it with #if")
                for name in GATE.findall(strip_line_comment(c.group(2), "//")):
                    uses.append((rel, i + 1, name))

    known = declared | self_defined | PROBED
    for rel, lineno, name in uses:
        if name not in known:
            findings.append(f"{rel}:{lineno}: {name} is tested but has no default; "
                            f"declare it in {HEADER}")

    build_files = [root / "Makefile", *sorted((root / "mk").glob("*.mk"))]
    for path in build_files:
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        for i, line in enumerate(path.read_text(errors="replace").splitlines()):
            for name in DASH_D.findall(strip_line_comment(line, "#")):
                if name not in known:
                    findings.append(f"{rel}:{i + 1}: passes {name}, which no source declares")
    for relpath in CMAKE_FILES:
        path = root / relpath
        if not path.is_file():
            continue
        for i, line in enumerate(path.read_text(errors="replace").splitlines()):
            for name in GATE.findall(strip_line_comment(line, "#")):
                if name not in known:
                    findings.append(f"{relpath}:{i + 1}: passes {name}, which no source declares")

    # A presence flag is tested with #ifdef, so passing it as =0 still turns it on.
    zero = re.compile(r"\b(" + "|".join(sorted(PROBED)) + r")\s*=\s*0\b")
    for path in [*build_files, *(root / rel for rel in CMAKE_FILES)]:
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        for i, line in enumerate(path.read_text(errors="replace").splitlines()):
            for name in zero.findall(strip_line_comment(line, "#")):
                findings.append(f"{rel}:{i + 1}: {name}=0 still counts as defined to #ifdef; "
                                f"leave a presence flag out to turn it off")

    if findings:
        print(f"feature gates: {len(findings)} finding(s)")
        for f in findings:
            print(f"  {f}")
        return 1
    print(f"feature gates: {len(declared)} declared in {HEADER}, every tested gate has a default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
