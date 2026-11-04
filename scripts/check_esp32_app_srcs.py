#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drift gate for the ESP32 firmware app source manifest.

The firmware is a deliberately curated subset of `src/` — the "v1 Core+AMS cut"
(see firmware/helixscreen-esp32/components/helixapp/CMakeLists.txt). Whole
subsystems are gated off (camera, label printer, gcode/bed-mesh 3D, plugins,
timelapse viewer, screensaver, calibration panels, sound, mocks, the concrete
libhv client). The manifest (`app_srcs.txt`) is hand-maintained: it was
generated once from the native-audit 491-file Xtensa-compile classification and
has drifted twice since (ams_endless_spool.cpp, toolhead_homing.cpp — each
landed in main without making the manifest, and the firmware link broke ~25 min
into esp32-build CI).

Curation is unavoidable for a subset build. The fix is not to auto-glob all of
src/ (that pulls hundreds of Xtensa-incompatible files) but to make the drift
LOUD at quality-check / PR time instead of at a 25-minute link step.

WHAT IS FLAGGED
  - A `src/**/*.{cpp,c}` in NEITHER app_srcs.txt NOR app_srcs_excluded.txt
    ("undecided" — a new file, or one nobody decided on). Add it to app_srcs.txt
    (compile it on the firmware) or to the exclusion file (don't).
  - A manifest line CMake would silently DROP or mangle (see MANIFEST LINE
    FORMAT below) — the failure mode this gate exists to prevent, since the
    line looks present to a reader and is absent to the build.
  - A `src/...` line in app_srcs.txt whose file no longer exists ("stale" — the
    source was deleted/renamed but the manifest line wasn't).
  - A `src/...` entry in app_srcs_excluded.txt whose file no longer exists, or
    a `dir/` entry with no src/ files left beneath it. A rotted exclusion is
    worse than a rotted manifest line: rename a file onto a stale excluded path
    and the new file is silently auto-excluded from the firmware.
  - A file in BOTH manifest and exclusions (directly, or via a `dir/` entry).
    CMake compiles it; the exclusion file says it doesn't. One of them is a lie.

NOT FLAGGED
  - Manifest entries OUTSIDE src/ (e.g. lib/lv_markdown/*.c). The universe is
    src/ only; the manifest legitimately pulls a few lib/ sources.
  - Anything not under src/.

MANIFEST LINE FORMAT (app_srcs.txt) — dictated by CMake, not by this script
  CMakeLists.txt reads the manifest with
      file(STRINGS app_srcs.txt APP_SRCS_REL REGEX "^[^#].*\\.(cpp|c)$")
  so a line only reaches the build when it starts with a non-`#` character AND
  ends in `.cpp`/`.c`. Consequences, all of them silent at CMake time:
    - `src/a/b.cpp  # keep, AMS needs it` does NOT end in .cpp → DROPPED.
    - `src/a/b.cpp   ` (trailing space)   does NOT end in .cpp → DROPPED.
    - `  src/a/b.cpp` (leading space) matches, and CMake then builds the path
      `<repo>/ src/a/b.cpp` → a bogus source path.
  So: one bare path per line, nothing after it. Comment lines start with `#`
  (the manifest carries its whole derivation ledger that way). This gate
  rejects any other shape — it fails CLOSED, because the alternative is a
  25-minute CI link error for a line that reads as correct.

EXCLUSION FILE FORMAT (app_srcs_excluded.txt)
  Only this script reads it, so it is the permissive one:
  - One path per line; `#`-prefixed lines and trailing `# reason` are ignored.
  - A path ending in `/` excludes a whole directory recursively.
  - Any other path excludes that single file.

MODES
  (default)          check; exit 0 if clean, 1 on any finding above.
  --list             print the undecided files, one per line.
  --summary          one-line counts.
  --link             link-level check over the native build's objects (build/obj):
                     fails when a listed file references a symbol that only an
                     excluded file defines, which the source checks cannot see.
                     A symbol the firmware's own sources stub, or a reference whose
                     every mention sits in a branch the firmware does not compile,
                     is fine. Known references are ratcheted, symbol by symbol, in
                     --baseline (default scripts/esp32_link_baseline.txt).
                     Exit 2 is "no verdict": listed objects are missing, an object
                     is older than its source or a header in its depfile, or nm or
                     c++filt is not on PATH.
  --write-exclusions SEEDING/BULK-ADD tool, not the answer to routine drift.
                     Adds the currently-undecided files to the baseline,
                     compressing whole directories to dir-level entries.
                     Refuses to touch an existing baseline without --force;
                     with --force it MERGES (existing entries and their
                     hand-written reasons are preserved). Delete the file
                     first if you really want a clean re-seed.

Exit 0 when the manifest and exclusion baseline cover src/ exactly, 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "firmware/helixscreen-esp32/components/helixapp/app_srcs.txt"
DEFAULT_EXCLUSIONS = REPO_ROOT / "firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt"
DEFAULT_SRC_ROOT = REPO_ROOT / "src"
DEFAULT_OBJ_ROOT = REPO_ROOT / "build/obj"
DEFAULT_FIRMWARE_ROOT = REPO_ROOT / "firmware/helixscreen-esp32"
DEFAULT_LINK_BASELINE = REPO_ROOT / "scripts/esp32_link_baseline.txt"
LINK_CEILING_KEY = "max-edges"
SRC_SUFFIXES = (".cpp", ".c")

# The lines components/helixapp/app_srcs.cmake keeps: one .cpp/.c path, optional
# surrounding whitespace, optional trailing '# comment'. It strips the comment and the
# whitespace, so the path is what is left.
CMAKE_MANIFEST_LINE = re.compile(r"^[ \t]*[^# \t][^#]*\.(?:cpp|c)[ \t]*(?:#.*)?$")
# CMake list syntax: on a kept line it splits or merges the entry.
CMAKE_LIST_CHARS = re.compile(r"[;\[\]]")

DEFAULT_FILE_REASON = "not in the v1 Core+AMS cut"

# Each of these in a compiled file links libstdc++'s std::locale machinery (~150K of
# facets, built all at once on first use) into the image: iostreams and <regex>
# directly, <filesystem> through path's wide codecvt. core/text_io.h, helix_regex.h and
# helix_fs.h replace them. Headers reached indirectly are caught by the map check
# in scripts/check_esp32_size.py.
LOCALE_INCLUDE = re.compile(
    r"^\s*#\s*include\s*<(sstream|fstream|iostream|istream|ostream|iomanip|regex|filesystem|locale)>",
    re.M)

# The ESP32 image is built without exceptions, so each of these aborts the device on
# input the desktop build would catch: a wrongly typed or missing JSON field, text
# that is not a number, an any holding another type. try/catch and throw do not
# compile there, so the compiler flags those; these calls compile and abort.
ABORTING_CALLS = [
    (re.compile(r'\.value\(\s*"'), 'json .value("key", d) throws on a wrongly typed field',
     "json_util::safe_*(j, \"key\", d)"),
    (re.compile(r'\.at\(\s*"'), 'json .at("key") throws on a missing key',
     "find() or contains() first"),
    (re.compile(r'json::parse\([^,()]*\)'), "one-argument json::parse throws on bad input",
     "json::parse(s, nullptr, false) and is_discarded()"),
    (re.compile(r'\bstd::sto(?:i|l|ll|ul|ull|f|d|ld)\s*\('), "std::sto* throws on text that is not a number",
     "text_io::parse_leading<T>(s)"),
    (re.compile(r'\bany_cast<[^>]+>\s*\((?!\s*&)'), "value-form std::any_cast throws on another type",
     "std::any_cast<T>(&a) and a null check"),
]

# try/catch/throw do not compile without exceptions. The firmware build says so
# 25 minutes into esp32-build; this says so at commit time. A branch the firmware
# does not compile is skipped: the firmware's own compile definitions (helixapp's
# CMakeLists) decide `#if HELIX_HAS_X`, `#if defined(ESP_PLATFORM)` and
# `#if defined(__cpp_exceptions)`. A condition this cannot evaluate counts as
# compiled. Code desktop and firmware share keeps a desktop-only net inside
# `#if defined(__cpp_exceptions)`, or uses exception_policy.h.
EXCEPTION_CONSTRUCT = re.compile(r"\btry\s*\{|\bcatch\s*\(|\bthrow\b(?!_)")
STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
# Defined by the toolchain or IDF on every firmware compile, or never defined there.
FIRMWARE_TOOLCHAIN_DEFINES = {"ESP_PLATFORM": "1"}
FIRMWARE_UNDEFINED = {"__cpp_exceptions", "HELIX_ENABLE_MOCKS", "__APPLE__", "__ANDROID__"}


def native_only_flags(repo: Path, cmake_text: str) -> set[str]:
    """HELIX_* names only the native Makefile's -D sets, which the firmware never sees.

    The firmware's flags come from its CMake and sdkconfig alone, so a name the
    Makefile passes, no firmware CMake or sdkconfig file mentions and no source
    #defines is undefined there.
    """
    makefiles = [repo / "Makefile", *sorted((repo / "mk").glob("*.mk"))]
    names = {n for m in makefiles if m.is_file()
             for n in re.findall(r"-D\s*(HELIX_\w+)", m.read_text(errors="replace"))}
    firmware_config = [cmake_text] + [
        f.read_text(errors="replace") for f in (repo / "firmware").rglob("*")
        if (f.name == "CMakeLists.txt" or f.suffix == ".cmake" or f.name.startswith("sdkconfig"))
        and "build" not in f.relative_to(repo).parts and f.is_file()]
    names = {n for n in names if not any(re.search(rf"\b{n}\b", t) for t in firmware_config)}
    if not names:
        return names
    hash_defined = re.compile(r"^\s*#\s*define\s+(HELIX_\w+)", re.M)
    for d in ("include", "src", "firmware"):
        for f in (repo / d).rglob("*"):
            if f.suffix in (".h", ".hpp", ".c", ".cpp") and f.is_file():
                names -= set(hash_defined.findall(f.read_text(errors="replace")))
    return names


def firmware_defines(cmake_text: str, repo: Path | None = None) -> dict[str, str | None]:
    """NAME -> value for helixapp's PRIVATE compile definitions, plus the toolchain's.

    A name mapped to None is known to be undefined on the firmware.
    """
    defines: dict[str, str | None] = dict.fromkeys(FIRMWARE_UNDEFINED)
    if repo is not None:
        defines.update(dict.fromkeys(native_only_flags(repo, cmake_text)))
    defines.update(FIRMWARE_TOOLCHAIN_DEFINES)
    if repo is not None:
        # Every feature gate's default, which the CMake definitions below override:
        # lvgl_pch.h brings include/helix_features.h into every firmware source.
        features = repo / "include" / "helix_features.h"
        if features.is_file():
            for gate, value in re.findall(r"^#define (HELIX_HAS_\w+) (\d+)$",
                                          features.read_text(errors="replace"), re.M):
                defines[gate] = value
    m = re.search(r"target_compile_definitions\(\$\{COMPONENT_LIB\} PRIVATE(.*?)\)\s*$",
                  cmake_text, re.S | re.M)
    if m:
        for line in m.group(1).splitlines():
            for token in line.split("#", 1)[0].split():
                name, _, value = token.partition("=")
                if re.fullmatch(r"[A-Za-z_]\w*", name):
                    defines[name] = value or "1"
    return defines


def firmware_condition(directive: str, defines: dict[str, str]) -> bool | None:
    """Whether the firmware compiles the branch a #if/#ifdef/#ifndef opens; None if unknown."""
    def defined(name: str) -> bool | None:
        return defines[name] is not None if name in defines else None

    def negate(v: bool | None) -> bool | None:
        return None if v is None else not v

    m = re.fullmatch(r"(ifdef|ifndef)\s+(\w+)", directive)
    if m:
        v = defined(m.group(2))
        return v if m.group(1) == "ifdef" else negate(v)
    m = re.fullmatch(r"if\s+(!?)\s*defined\s*\(?\s*(\w+)\s*\)?", directive)
    if m:
        v = defined(m.group(2))
        return negate(v) if m.group(1) else v
    m = re.fullmatch(r"if\s+(!?)\s*(\w+)(?:\s*(==|!=)\s*(\d+))?", directive)
    if m:
        name = m.group(2)
        if name.isdigit():
            value = int(name)
        elif name in defines:
            raw = defines[name]
            value = 0 if raw is None else int(raw) if raw.lstrip("-").isdigit() else None
        else:
            return None
        if value is None:
            return None
        if m.group(3):
            v = (value == int(m.group(4))) == (m.group(3) == "==")
        else:
            v = value != 0
        return not v if m.group(1) else v
    return None


def firmware_code_lines(text: str, defines: dict[str, str | None], includes: bool = False):
    """Yield (line, code) for each line in a branch the firmware compiles.

    Code is the line with string literals blanked and comments cut. A branch
    whose condition this cannot evaluate counts as compiled. With `includes`,
    compiled #include directives are yielded too.
    """
    stack, in_block_comment = [], False
    lines = text.splitlines()
    for lineno, raw in enumerate(lines, 1):
        line = raw
        if line.lstrip().startswith("#"):
            while line.endswith("\\") and lineno < len(lines):
                line = line[:-1] + " " + lines[lineno]
                lines[lineno] = ""
                lineno += 1
        if in_block_comment:
            if "*/" not in line:
                continue
            line, in_block_comment = line.split("*/", 1)[1], False
        stripped = line.strip()
        if stripped.startswith("#"):
            directive = re.sub(r"\s+", " ", stripped[1:].split("//", 1)[0]).strip()
            # Each frame: [does the firmware compile this branch (None = unknown),
            #              has an earlier branch of this #if been compiled,
            #              could an earlier branch have been compiled].
            if directive.startswith("if"):
                cond = firmware_condition(directive, defines)
                stack.append([cond, cond is True, cond is None])
            elif directive.startswith(("elif", "else")) and stack:
                frame = stack[-1]
                cond = (firmware_condition("if" + directive[4:], defines)
                        if directive.startswith("elif") else True)
                if frame[1]:
                    frame[0] = False
                elif frame[2] and cond is not False:
                    frame[0] = None
                else:
                    frame[0] = cond
                frame[1] = frame[1] or frame[0] is True
                frame[2] = frame[2] or frame[0] is None
            elif directive.startswith("endif") and stack:
                stack.pop()
            elif (includes and directive.startswith("include")
                  and not any(frame[0] is False for frame in stack)):
                yield lineno, stripped
            continue
        code = STRING_LITERAL.sub('""', line).split("//", 1)[0]
        if "/*" in code:
            code, rest = code.split("/*", 1)
            in_block_comment = "*/" not in rest
        if any(frame[0] is False for frame in stack):
            continue
        yield lineno, code


def exception_sites(text: str, defines: dict[str, str]) -> list[tuple[int, str]]:
    """(line, construct) for each try/catch/throw in a branch the firmware compiles."""
    sites = []
    for lineno, code in firmware_code_lines(text, defines):
        m = EXCEPTION_CONSTRUCT.search(code)
        if m:
            sites.append((lineno, m.group(0).rstrip("({ ")))
    return sites


EXCLUSIONS_HEADER = [
    "# ESP32 firmware app_srcs exclusion baseline.",
    "#",
    "# Every src/**/*.cpp|.c NOT compiled by the firmware (i.e. not in",
    "# app_srcs.txt) must appear here, or scripts/check_esp32_app_srcs.py",
    "# fails. A path ending in '/' excludes a whole directory recursively.",
    "#",
    "# THE BLANKET RULE. Everything in this file is out for one reason: it is not",
    "# part of the v1 Core+AMS cut (camera, label printer, gcode/bed-mesh 3D,",
    "# plugins, timelapse viewer, screensaver, calibration panels, sound, desktop",
    "# mocks, the Linux platform backends, the concrete libhv client). An entry",
    "# carrying only the default '# not in the v1 Core+AMS cut' note is covered by",
    "# that rule and needs nothing more — writing 200 restatements of it would bury",
    "# the entries that DO have something specific to say. Add a specific trailing",
    "# '# reason' when, and only when, the real reason differs from the blanket",
    "# rule (a link constraint, a conditional CMake arm, an ESP-side replacement).",
    "#",
    "# Extend with:",
    "#   python3 scripts/check_esp32_app_srcs.py --write-exclusions --force",
    "# --force MERGES — existing entries and their hand-written reasons are kept,",
    "# only newly-undecided files are added. Delete this file first for a re-seed.",
    "",
]


@dataclass
class ExclusionEntry:
    lineno: int
    path: str
    reason: str  # trailing comment text, without the leading '#'; "" if none

    @property
    def is_dir(self) -> bool:
        return self.path.endswith("/")


@dataclass
class Findings:
    undecided: list[str] = field(default_factory=list)
    malformed: list[tuple[int, str, str]] = field(default_factory=list)  # (lineno, line, why)
    stale_manifest: list[str] = field(default_factory=list)
    stale_exclusions: list[tuple[int, str, str]] = field(default_factory=list)  # (lineno, path, why)
    overlap: list[tuple[str, str]] = field(default_factory=list)  # (file, exclusion entry)
    locale_includes: list[tuple[str, int, str]] = field(default_factory=list)  # (file, lineno, header)
    aborting_calls: list[tuple[str, int, str, str]] = field(default_factory=list)  # (file, lineno, why, fix)
    exception_constructs: list[tuple[str, int, str]] = field(default_factory=list)  # (file, lineno, construct)
    universe: set[str] = field(default_factory=set)

    def any(self) -> bool:
        return bool(self.undecided or self.malformed or self.stale_manifest
                    or self.stale_exclusions or self.overlap or self.locale_includes
                    or self.aborting_calls or self.exception_constructs)


def manifest_path(line: str) -> str:
    """The path a kept manifest line names, as app_srcs.cmake extracts it."""
    return line.split("#", 1)[0].strip()


def why_cmake_drops(line: str) -> str:
    """Human-readable reason a manifest line is not CMake-consumable."""
    if CMAKE_LIST_CHARS.search(line):
        return ("contains ';', '[' or ']', which CMake reads as list syntax and so "
                "splits or merges the entry")
    if re.search(r"\s", manifest_path(line)):
        return "the path contains whitespace, so CMake builds a path that does not exist"
    return ("does not name one .cpp/.c path — app_srcs.cmake skips it, so it is "
            "silently DROPPED from the build")


def load_manifest(path: Path) -> tuple[set[str], list[tuple[int, str, str]]]:
    """Return (paths CMake will compile, malformed lines).

    Parsed exactly the way CMakeLists.txt parses it, so anything this function
    reports as malformed is a line CMake would drop or mangle. May include
    non-src/ entries (lib/ sources are legitimate).
    """
    included: set[str] = set()
    malformed: list[tuple[int, str, str]] = []
    if not path.exists():
        return included, malformed
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.rstrip("\r")
        if not line.strip() or line.strip().startswith("#"):
            continue  # blank, or a comment line (CMake's ^[^#] rejects those)
        path = manifest_path(line)
        if (CMAKE_MANIFEST_LINE.match(line) and not CMAKE_LIST_CHARS.search(line)
                and not re.search(r"\s", path)):
            included.add(path)
        else:
            malformed.append((lineno, line, why_cmake_drops(line)))
    return included, malformed


def load_exclusions(path: Path) -> list[ExclusionEntry]:
    """Parse the baseline into entries, keeping each trailing '# reason'."""
    entries: list[ExclusionEntry] = []
    if not path.exists():
        return entries
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.rstrip("\r")
        if line.strip().startswith("#"):
            continue
        body, _, reason = line.partition("#")
        body = body.strip()
        if not body:
            continue
        entries.append(ExclusionEntry(lineno, body, reason.strip()))
    return entries


def split_exclusions(entries: list[ExclusionEntry]) -> tuple[set[str], set[str]]:
    """(file_exclusions, dir_exclusions) from parsed entries."""
    files = {e.path for e in entries if not e.is_dir}
    dirs = {e.path for e in entries if e.is_dir}
    return files, dirs


def scan_universe(src_root: Path) -> set[str]:
    """All src/**/*.{cpp,c} as forward-slash paths relative to src_root's parent.

    Relative to the PARENT (not REPO_ROOT) so the paths read ``src/...`` for both
    the real tree (src_root = <repo>/src, parent = <repo>) and an external
    fixture (src_root = <tmp>/src, parent = <tmp>) — matching how manifest and
    exclusion entries are written.
    """
    base = src_root.parent
    universe: set[str] = set()
    for p in src_root.rglob("*"):
        if p.is_file() and p.suffix in SRC_SUFFIXES:
            universe.add(str(p.relative_to(base)).replace("\\", "/"))
    return universe


def display(path: Path) -> str:
    """Repo-relative if under the repo, else the path as-is (for fixtures)."""
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def covering_exclusion(rel: str, ex_files: set[str], ex_dirs: set[str]) -> str | None:
    """The exclusion entry that covers `rel`, or None."""
    if rel in ex_files:
        return rel
    for d in sorted(ex_dirs):
        if rel.startswith(d):
            return d
    return None


def is_excluded(rel: str, ex_files: set[str], ex_dirs: set[str]) -> bool:
    return covering_exclusion(rel, ex_files, ex_dirs) is not None


def compute(manifest: Path, exclusions: Path, src_root: Path) -> Findings:
    included, malformed = load_manifest(manifest)
    entries = load_exclusions(exclusions)
    ex_files, ex_dirs = split_exclusions(entries)
    universe = scan_universe(src_root)

    undecided = sorted(
        f for f in universe
        if f not in included and not is_excluded(f, ex_files, ex_dirs)
    )
    # Stale = manifest src/ entries that no longer exist on disk.
    stale_manifest = sorted(i for i in included if i.startswith("src/") and i not in universe)

    # Stale exclusions rot invisibly (nothing links against them), and a rename
    # onto a stale excluded path silently auto-excludes the new file.
    stale_exclusions: list[tuple[int, str, str]] = []
    for e in sorted(entries, key=lambda e: e.lineno):
        if not e.path.startswith("src/"):
            continue
        if e.is_dir:
            if not any(u.startswith(e.path) for u in universe):
                stale_exclusions.append(
                    (e.lineno, e.path, "no src/ files remain beneath this directory"))
        elif e.path not in universe:
            stale_exclusions.append((e.lineno, e.path, "file no longer exists"))

    # In both files: CMake compiles it, the baseline claims it doesn't.
    overlap = []
    for f in sorted(included):
        if not f.startswith("src/"):
            continue
        cover = covering_exclusion(f, ex_files, ex_dirs)
        if cover is not None:
            overlap.append((f, cover))

    locale_includes: list[tuple[str, int, str]] = []
    aborting_calls: list[tuple[str, int, str, str]] = []
    exception_constructs: list[tuple[str, int, str]] = []
    cmake = manifest.parent / "CMakeLists.txt"
    base = src_root.parent
    defines = firmware_defines(cmake.read_text() if cmake.exists() else "", base)
    compiled = [f for f in sorted(included) if f.startswith("src/") and f in universe]
    # Every header, not only the ones a compiled file includes: any of them can
    # reach the firmware through an include, and tracing that graph here would be
    # a second, weaker compiler. Plus the firmware's own component sources, which
    # sit beside helixapp outside src/.
    extra = [p.relative_to(base).as_posix()
             for d in (base / "include", src_root) if d.is_dir()
             for p in sorted(d.rglob("*.h"))]
    if manifest.parent.name == "helixapp":
        extra += [p.relative_to(base).as_posix()
                  for p in sorted(manifest.parent.parent.glob("*/*.cpp"))
                  if p.is_relative_to(base)]
    for f in compiled + extra:
        text = (base / f).read_text(errors="replace")
        if f in compiled:
            for m in LOCALE_INCLUDE.finditer(text):
                locale_includes.append((f, text.count("\n", 0, m.start()) + 1, m.group(1)))
        for lineno, line in enumerate(text.splitlines(), 1):
            code = line.split("//", 1)[0]
            if code.lstrip().startswith("*"):
                continue
            for pattern, why, fix in ABORTING_CALLS:
                if pattern.search(code):
                    aborting_calls.append((f, lineno, why, fix))
        for lineno, construct in exception_sites(text, defines):
            exception_constructs.append((f, lineno, construct))

    return Findings(undecided=undecided, malformed=malformed,
                    stale_manifest=stale_manifest, stale_exclusions=stale_exclusions,
                    overlap=overlap, locale_includes=locale_includes,
                    aborting_calls=aborting_calls,
                    exception_constructs=exception_constructs, universe=universe)


def load_link_baseline(path: Path) -> tuple[set[tuple[str, str, str]], int | None]:
    """(entries, max-edges ceiling) from a `listed -> excluded  symbol` per-line baseline."""
    edges: set[tuple[str, str, str]] = set()
    ceiling = None
    if not path.exists():
        return edges, ceiling
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line.startswith(LINK_CEILING_KEY + ":"):
            ceiling = int(line.split(":", 1)[1])
        elif " -> " in line:
            user, _, target = line.partition(" -> ")
            definer, _, symbol = target.strip().partition(" ")
            edges.add((user.strip(), definer, symbol.strip()))
    return edges, ceiling


IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.M)


def object_path(obj_root: Path, rel: str) -> Path:
    """build/obj/<path under src/>.o, the native build's object for src/<path>."""
    return obj_root / Path(rel).relative_to("src").with_suffix(".o")


def older_than_inputs(objects: dict[str, Path], base: Path) -> list[str]:
    """Files whose object predates the source or a header its depfile names.

    A depfile input that no longer exists counts: make would rebuild that object.
    """
    mtimes: dict[Path, int | None] = {}

    def mtime(p: Path) -> int | None:
        if p not in mtimes:
            try:
                mtimes[p] = p.stat().st_mtime_ns
            except OSError:
                mtimes[p] = None
        return mtimes[p]

    stale = []
    for rel, obj in sorted(objects.items()):
        built = mtime(obj)
        inputs = [base / rel]
        depfile = obj.with_suffix(".d")
        if depfile.exists():
            rule = depfile.read_text(errors="replace").replace("\\\n", " ").split("\n", 1)[0]
            inputs += [base / d for d in rule.partition(":")[2].split()]
        for i in inputs:
            m = mtime(i)
            if m is None or built is None or m > built:
                stale.append(rel)
                break
    return stale


def nm_symbols(objects: dict[str, Path], flag: str) -> dict[str, set[str]]:
    """rel -> symbols `nm <flag>` lists for that object."""
    by_path = {str(p): rel for rel, p in objects.items()}
    out: dict[str, set[str]] = {rel: set() for rel in objects}
    paths = list(by_path)
    for i in range(0, len(paths), 500):
        res = subprocess.run(["nm", "-A", "-P", flag, *paths[i:i + 500]],
                             capture_output=True, text=True, check=True)
        for line in res.stdout.splitlines():
            obj, _, rest = line.partition(": ")
            if rest:
                out[by_path[obj]].add(rest.split()[0])
    return out


def symbol_name(demangled: str) -> list[str]:
    """`ns::Cls::fn(args) const` -> ['ns', 'Cls', 'fn']; vtables name their class."""
    name = demangled.removeprefix("vtable for ").removeprefix("typeinfo for ")
    depth = 0
    for i, c in enumerate(name):
        depth += c == "<"
        depth -= c == ">"
        if c == "(" and depth == 0:
            name = name[:i]
            break
    name = re.sub(r"\[abi:\w+\]", "", name)
    while "<" in name:
        stripped = re.sub(r"<[^<>]*>", "", name)
        if stripped == name:
            break
        name = stripped
    return name.split("::")


NOT_A_FUNCTION = {"if", "for", "while", "switch", "return", "sizeof", "catch", "defined",
                  "alignof", "decltype", "static_assert"}
CALLEE = re.compile(r"(?<![\w.>:~])((?:\w+::)*~?\w+)\s*\(")
AFTER_PARAMS = re.compile(
    r"(?:\s|\bconst\b|\bnoexcept\b|\boverride\b|\bfinal\b)*(?:[{]|:(?!:)|=\s*default\b)")
VARIABLE_DEFINITION = re.compile(r"^[ \t]*(?:(?:static|extern|const|constexpr|inline|"
                                 r"thread_local)\s+)*(?!return\b)[\w:]+(?:<[^;{]*>)?[\s*&]+"
                                 r"((?:\w+::)*\w+)\s*(?:[{=;\[])", re.M)


def defined_functions(code: str) -> set[str]:
    """Names, as written (`Cls::fn`, `fn`), of each function `code` gives a body.

    A name counts when its closing parenthesis is followed, past cv/noexcept
    qualifiers, by `{`, a constructor's `:` or `= default`. A call is followed by
    `;`, `)`, `.` or an operator instead. Namespace-scope variables count too: a
    declaration with a type before the name and an initializer or `;` after it.
    """
    out = set(VARIABLE_DEFINITION.findall(code))
    for m in CALLEE.finditer(code):
        if m.group(1).rsplit("::", 1)[-1] in NOT_A_FUNCTION:
            continue
        depth, k = 0, m.end() - 1
        while k < len(code):
            depth += {"(": 1, ")": -1}.get(code[k], 0)
            if depth == 0:
                break
            k += 1
        if AFTER_PARAMS.match(code, k + 1):
            out.add(m.group(1))
    return out


def firmware_defines_symbol(parts: list[str], defined: set[str], namespaces: set[str]) -> bool:
    """Whether a firmware source gives `parts` (from symbol_name) a body.

    A vtable or typeinfo names a class: any out-of-line member of it counts.
    """
    name = parts[-1]
    if len(parts) > 1 and f"{parts[-2]}::{name}" in defined:
        return True
    return name in defined and (len(parts) == 1 or parts[-2] in namespaces)


@dataclass
class LinkFindings:
    new: list[tuple[str, str, str]] = field(default_factory=list)  # (listed, excluded, symbol)
    stale_baseline: list[tuple[str, str, str]] = field(default_factory=list)
    over_ceiling: tuple[int, int | None] | None = None  # (entries, ceiling)
    missing_objects: list[str] = field(default_factory=list)
    stale_objects: list[str] = field(default_factory=list)
    missing_tools: list[str] = field(default_factory=list)


def compute_link(manifest: Path, exclusions: Path, src_root: Path, obj_root: Path,
                 firmware_root: Path, baseline: set[tuple[str, str, str]]) -> LinkFindings:
    """References from firmware-listed objects that only an excluded object satisfies.

    A symbol counts as supplied when a listed object defines it, or when one of the
    firmware's own sources (stubs, platform seams) gives it a body. A reference counts as live unless every mention of the name in
    the referencing source sits in a branch the firmware does not compile, or
    nothing the firmware compiles for that source names the symbol's class.
    """
    included, _ = load_manifest(manifest)
    ex_files, ex_dirs = split_exclusions(load_exclusions(exclusions))
    universe = scan_universe(src_root)
    listed = sorted(f for f in included if f in universe)
    excluded = sorted(f for f in universe
                      if f not in included and is_excluded(f, ex_files, ex_dirs))
    found = LinkFindings()
    found.missing_tools = [t for t in ("nm", "c++filt") if shutil.which(t) is None]
    if found.missing_tools:
        return found
    listed_objs = {}
    for f in listed:
        o = object_path(obj_root, f)
        if o.exists():
            listed_objs[f] = o
        else:
            found.missing_objects.append(f)
    excluded_objs = {f: object_path(obj_root, f) for f in excluded
                     if object_path(obj_root, f).exists()}

    # Only the listed objects: `make` leaves excluded files it does not link into the
    # app (the Bluetooth plugin, test-only backends) as old as the last test build.
    found.stale_objects = older_than_inputs(listed_objs, src_root.parent)
    if found.missing_objects or found.stale_objects:
        return found

    undefined = nm_symbols(listed_objs, "--undefined-only")
    supplied = {sym for syms in nm_symbols(listed_objs, "--defined-only").values()
                for sym in syms}
    owner: dict[str, str] = {}
    for f, syms in nm_symbols(excluded_objs, "--defined-only").items():
        for sym in syms:
            owner.setdefault(sym, f)
    wanted = sorted({sym for syms in undefined.values() for sym in syms
                     if sym in owner and sym not in supplied})
    if not wanted:
        found.stale_baseline = sorted(baseline)
        return found
    demangled = dict(zip(wanted, subprocess.run(
        ["c++filt"], input="\n".join(wanted), capture_output=True, text=True,
        check=True).stdout.splitlines()))

    cmake = manifest.parent / "CMakeLists.txt"
    cmake_text = cmake.read_text() if cmake.exists() else ""
    base = src_root.parent
    defines = firmware_defines(cmake_text, base)
    include_dirs = [base / d for d in re.findall(r"\$\{REPO_ROOT\}/([\w./-]+)", cmake_text)]
    firmware_defined: set[str] = set()
    firmware_namespaces: set[str] = set()
    for p in sorted(firmware_root.rglob("*")):
        if p.suffix not in SRC_SUFFIXES:
            continue
        text = p.read_text(errors="replace")
        # A single-header library defines its functions in the source that sets
        # its *_IMPLEMENTATION macro before including it.
        if re.search(r"^\s*#\s*define\s+\w+_IMPLEMENTATION\b", text, re.M):
            for header in re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, re.M):
                hits = [d / header for d in (p.parent, *include_dirs) if (d / header).is_file()]
                if hits:
                    text += "\n" + hits[0].read_text(errors="replace")
        code = "\n".join(c for _, c in firmware_code_lines(text, defines))
        firmware_defined |= defined_functions(code)
        for ns in re.findall(r"\bnamespace\s+([\w:]+)", code):
            firmware_namespaces |= set(ns.split("::"))
    firmware_classes = {d.split("::")[-2] for d in firmware_defined if "::" in d}
    header_dirs = [base / "include", *include_dirs]
    forced = [base / h for h in re.findall(r"-include\b[^$]*?\$\{REPO_ROOT\}/([\w./-]+)",
                                           cmake_text, re.S) if (base / h).is_file()]
    header_words: dict[Path, set[str]] = {}
    header_live_words: dict[Path, set[str]] = {}
    header_includes: dict[tuple[Path, bool], list[Path]] = {}

    def resolve(names, includer: Path) -> list[Path]:
        return [Path(os.path.normpath(hits[0])) for h in names
                if (hits := [d / h for d in (includer.parent, *header_dirs) if (d / h).is_file()])]

    def words_of(header: Path) -> set[str]:
        """Every identifier the header spells, in any branch."""
        if header not in header_words:
            code = STRING_LITERAL.sub('""', header.read_text(errors="replace"))
            code = re.sub(r"/\*.*?\*/|//[^\n]*", " ", code, flags=re.S)
            header_words[header] = set(IDENTIFIER.findall(code))
        return header_words[header]

    def live_words_of(header: Path) -> set[str]:
        """The identifiers in the header's branches the firmware compiles."""
        if header not in header_live_words:
            header_live_words[header] = {
                w for _, c in firmware_code_lines(header.read_text(errors="replace"), defines)
                for w in IDENTIFIER.findall(c)}
        return header_live_words[header]

    def includes_of(text: str, includer: Path, live: bool) -> list[Path]:
        names = ([h for _, c in firmware_code_lines(text, defines, includes=True)
                  for h in INCLUDE.findall(c)] if live else INCLUDE.findall(text))
        return resolve(names, includer)

    def closure(roots: list[Path], live: bool) -> set[Path]:
        """Every header `roots` reach, through compiled branches only when `live`."""
        out, todo = set(), list(roots)
        while todo:
            h = todo.pop()
            if h in out:
                continue
            out.add(h)
            if (h, live) not in header_includes:
                header_includes[(h, live)] = includes_of(h.read_text(errors="replace"), h, live)
            todo += header_includes[(h, live)]
        return out

    seen: set[tuple[str, str]] = set()
    for f in sorted(undefined):
        syms = sorted(undefined[f] & demangled.keys())
        if not syms:
            continue
        text = (base / f).read_text(errors="replace")
        words = set(IDENTIFIER.findall(text))
        live_words = {w for _, c in firmware_code_lines(text, defines)
                      for w in IDENTIFIER.findall(c)}
        src = base / f
        live_headers = closure(forced + includes_of(text, src, True), True)
        all_headers = live_headers | closure(includes_of(text, src, False), False)

        def dead(word: str) -> bool:
            """Every mention of `word` sits in a branch the firmware does not compile.

            A name the source never spells comes from an inline in a header; it is
            dead when some header mentions it and none the firmware compiles does.
            """
            if word in words:
                return word not in live_words
            return (any(word in words_of(h) for h in all_headers)
                    and not any(word in live_words_of(h) for h in live_headers))

        def unnameable(scope: str) -> bool:
            """No code or header the firmware compiles for this source spells `scope`."""
            return (scope not in live_words
                    and not any(scope in live_words_of(h) for h in live_headers))

        for sym in syms:
            parts = symbol_name(demangled[sym])
            if demangled[sym].startswith(("vtable for ", "typeinfo for ")):
                if parts[-1] in firmware_classes:
                    continue
            elif firmware_defines_symbol(parts, firmware_defined, firmware_namespaces):
                continue
            if dead(parts[-1].lstrip("~")) or (len(parts) > 1 and unnameable(parts[-2])):
                continue
            edge = (f, owner[sym], "::".join(parts))
            if edge not in seen and edge not in baseline:
                found.new.append(edge)
            seen.add(edge)
    found.stale_baseline = sorted(baseline - seen)
    return found


def report_link(f: LinkFindings) -> None:
    if f.new:
        print(f"FAIL: {len(f.new)} reference(s) from firmware-listed files to symbols only an "
              "app_srcs_excluded.txt file defines.\n      The ESP32 link has no definition "
              "for them:", file=sys.stderr)
        for user, definer, symbol in sorted(f.new):
            print(f"        {user} -> {definer}  {symbol}", file=sys.stderr)
        print("\n      Guard the call with the subsystem's HELIX_HAS_* macro, compile the "
              "defining file\n      on the firmware (move it to app_srcs.txt), or stub it in "
              "firmware/helixscreen-esp32/.", file=sys.stderr)
    if f.stale_baseline:
        print(f"FAIL: {len(f.stale_baseline)} link baseline entr(ies) no longer occur; "
              "delete them and lower max-edges:", file=sys.stderr)
        for user, definer, symbol in f.stale_baseline:
            print(f"        {user} -> {definer}  {symbol}", file=sys.stderr)
    if f.over_ceiling:
        entries, ceiling = f.over_ceiling
        print(f"FAIL: the link baseline holds {entries} entries against max-edges: {ceiling}.\n"
              "      It only shrinks; fix the reference instead of listing it.", file=sys.stderr)


def compress_dirs(undecided_set: set[str], universe: set[str]) -> dict[str, list[str]]:
    """Map each whole-undecided directory to the undecided files it covers.

    For every undecided file, walk its parent chain SHALLOWEST-first and take
    the first directory whose entire src/ file-set is undecided — so a
    fully-excluded tree collapses to one line, not one per subdirectory.
    """
    dir_covers: dict[str, list[str]] = {}
    for f in sorted(undecided_set):
        parts = f.split("/")
        best_dir = None
        for i in range(2, len(parts)):  # parts[0]=="src", dir needs >= "src/X/"
            d = "/".join(parts[:i]) + "/"
            files_under = {u for u in universe if u.startswith(d)}
            if files_under and files_under <= undecided_set:
                best_dir = d
                break  # shallowest wins
        if best_dir:
            dir_covers.setdefault(best_dir, []).append(f)
        # else: emitted per-file by the caller
    return dir_covers


def write_exclusions(undecided: list[str], universe: set[str], path: Path) -> int:
    """Merge the undecided set into `path`, preserving existing entries/reasons."""
    existing = load_exclusions(path)
    reasons = {e.path: e.reason for e in existing}
    old_files, old_dirs = split_exclusions(existing)

    undecided_set = set(undecided)
    dir_covers = compress_dirs(undecided_set, universe)
    covered: set[str] = set()
    for files in dir_covers.values():
        covered.update(files)
    new_files = {f for f in undecided_set if f not in covered}

    dirs = sorted(old_dirs | set(dir_covers))
    files = sorted(f for f in (old_files | new_files)
                   if not any(f.startswith(d) for d in dirs))

    def reason_for(entry: str) -> str:
        if entry in reasons and reasons[entry]:
            return reasons[entry]
        if entry.endswith("/"):
            n = sum(1 for u in universe if u.startswith(entry))
            return f"all {n} src/ files beneath — {DEFAULT_FILE_REASON}"
        return DEFAULT_FILE_REASON

    lines = list(EXCLUSIONS_HEADER)
    if dirs:
        lines.append("# --- whole directories excluded ---")
        lines += [f"{d}  # {reason_for(d)}" for d in dirs]
        lines.append("")
    if files:
        lines.append("# --- individual files (same dir, partially compiled) ---")
        lines += [f"{f}  # {reason_for(f)}" for f in files]
        lines.append("")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    added = len(set(dir_covers) - old_dirs) + len(new_files - old_files)
    print(f"Wrote {display(path)}: {len(dirs) + len(files)} entries "
          f"({len(dirs)} dir-level, {len(files)} file-level); "
          f"{added} newly added, {len(old_dirs) + len(old_files)} preserved.")
    return 0


def report(f: Findings) -> None:
    """Print every finding to stderr, most actionable first."""
    if f.malformed:
        print(f"FAIL: {len(f.malformed)} app_srcs.txt line(s) that CMake will NOT compile.\n"
              "      CMake reads the manifest with REGEX \"^[^#].*\\.(cpp|c)$\" — one bare\n"
              "      path per line, no leading whitespace, nothing after the suffix:",
              file=sys.stderr)
        for lineno, line, why in f.malformed:
            print(f"        line {lineno}: {line}", file=sys.stderr)
            print(f"                 ^ {why}", file=sys.stderr)
        print("\n      Put the explanation on its own '#' comment line above the path.",
              file=sys.stderr)

    if f.undecided:
        print(f"FAIL: {len(f.undecided)} src/ file(s) not decided for the ESP32 firmware build:",
              file=sys.stderr)
        for x in f.undecided:
            print(f"        {x}", file=sys.stderr)
        print("\n      Decide each one, by hand, one line:\n"
              "        1. Should the firmware compile it? Add the bare path to\n"
              "           firmware/helixscreen-esp32/components/helixapp/app_srcs.txt.\n"
              "           This is the usual answer for a new file in a subsystem that is\n"
              "           already part of the v1 Core+AMS cut.\n"
              "        2. Should it not? Add it to app_srcs_excluded.txt (a trailing\n"
              "           '# reason' only if the reason is not the blanket v1-cut rule).\n"
              "      Do NOT reach for --write-exclusions here: it answers (2) for every\n"
              "      file at once, which is the wrong answer for a file you just added.",
              file=sys.stderr)

    if f.locale_includes:
        print(f"FAIL: {len(f.locale_includes)} std::locale-pulling include(s) in firmware-compiled "
              "files.\n      Any one links ~150K of libstdc++ locale facets into the ESP32 image.",
              file=sys.stderr)
        for path, lineno, header in f.locale_includes:
            print(f"        {path}:{lineno}: <{header}>", file=sys.stderr)
        print("\n      Use core/text_io.h (streams), helix_regex.h (<regex>) or helix_fs.h "
              "(<filesystem>).", file=sys.stderr)
    if f.aborting_calls:
        print(f"FAIL: {len(f.aborting_calls)} call(s) in firmware-compiled files that abort the "
              "ESP32 image,\n      which is built without exceptions:", file=sys.stderr)
        for path, lineno, why, fix in f.aborting_calls:
            print(f"        {path}:{lineno}: {why}; use {fix}", file=sys.stderr)
    if f.exception_constructs:
        print(f"FAIL: {len(f.exception_constructs)} try/catch/throw in firmware-compiled files; "
              "the ESP32 image\n      is built without exceptions, so these do not compile "
              "there:", file=sys.stderr)
        for path, lineno, construct in f.exception_constructs:
            print(f"        {path}:{lineno}: {construct}", file=sys.stderr)
        print("\n      Keep a desktop-only net inside #if defined(__cpp_exceptions), or use "
              "exception_policy.h\n      (throw_or_abort, contain_exceptions).", file=sys.stderr)
    if f.overlap:
        print(f"FAIL: {len(f.overlap)} file(s) in BOTH app_srcs.txt and app_srcs_excluded.txt.\n"
              "      CMake compiles them; the exclusion baseline says it does not. Remove\n"
              "      whichever line is wrong:", file=sys.stderr)
        for path, cover in f.overlap:
            via = "" if path == cover else f" (via the '{cover}' directory entry)"
            print(f"        {path}{via}", file=sys.stderr)

    if f.stale_manifest:
        print(f"FAIL: {len(f.stale_manifest)} stale app_srcs.txt line(s) — src/ files that no "
              "longer exist:", file=sys.stderr)
        for x in f.stale_manifest:
            print(f"        {x}", file=sys.stderr)
        print("\n      Remove the stale line(s) from app_srcs.txt.", file=sys.stderr)

    if f.stale_exclusions:
        print(f"FAIL: {len(f.stale_exclusions)} stale app_srcs_excluded.txt entr(ies):",
              file=sys.stderr)
        for lineno, path, why in f.stale_exclusions:
            print(f"        line {lineno}: {path} — {why}", file=sys.stderr)
        print("\n      Remove them. A stale exclusion is not harmless: rename a source onto\n"
              "      one of these paths and the new file is silently excluded from the\n"
              "      firmware with nobody deciding anything.", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--exclusions", type=Path, default=DEFAULT_EXCLUSIONS)
    ap.add_argument("--src-root", type=Path, default=DEFAULT_SRC_ROOT)
    ap.add_argument("--list", action="store_true", help="print undecided files")
    ap.add_argument("--summary", action="store_true", help="one-line counts")
    ap.add_argument("--write-exclusions", action="store_true",
                    help="seed/bulk-add: merge the currently-undecided files into the "
                         "exclusion baseline. NOT the fix for a file you just added — "
                         "that belongs in app_srcs.txt.")
    ap.add_argument("--force", action="store_true",
                    help="with --write-exclusions: merge into an existing baseline "
                         "(existing entries and hand-written reasons are preserved)")
    ap.add_argument("--link", action="store_true",
                    help="check the native build's objects instead: fail on a reference "
                         "from a listed file that only an excluded file defines")
    ap.add_argument("--obj-root", type=Path, default=DEFAULT_OBJ_ROOT)
    ap.add_argument("--firmware-root", type=Path, default=DEFAULT_FIRMWARE_ROOT)
    ap.add_argument("--baseline", type=Path, default=DEFAULT_LINK_BASELINE,
                    help="with --link: the known `listed -> excluded  symbol` entries and their "
                         f"{LINK_CEILING_KEY}: ceiling")
    args = ap.parse_args()

    if args.link:
        baseline, ceiling = load_link_baseline(args.baseline)
        lf = compute_link(args.manifest, args.exclusions, args.src_root, args.obj_root,
                          args.firmware_root, baseline)
        if lf.missing_tools:
            print(f"SKIP: {' and '.join(lf.missing_tools)} not on PATH.", file=sys.stderr)
            return 2
        if lf.missing_objects:
            print(f"SKIP: {len(lf.missing_objects)} listed file(s) have no object under "
                  f"{display(args.obj_root)}; build first (make).", file=sys.stderr)
            return 2
        if lf.stale_objects:
            print(f"SKIP: {len(lf.stale_objects)} object(s) under {display(args.obj_root)} are "
                  f"older than their inputs; build first (make). First: {lf.stale_objects[0]}",
                  file=sys.stderr)
            return 2
        if ceiling is None or len(baseline) > ceiling:
            lf.over_ceiling = (len(baseline), ceiling)
        if lf.new or lf.stale_baseline or lf.over_ceiling:
            report_link(lf)
            return 1
        print(f"OK: no firmware-listed object references a symbol only an excluded file "
              f"defines beyond the {len(baseline)} baselined edge(s).")
        return 0

    f = compute(args.manifest, args.exclusions, args.src_root)

    if args.write_exclusions:
        if args.exclusions.exists() and not args.force:
            print(f"FAIL: {display(args.exclusions)} already exists — "
                  "use --force to merge the undecided files into it.", file=sys.stderr)
            return 1
        return write_exclusions(f.undecided, f.universe, args.exclusions)

    if args.summary:
        print(f"esp32 app_srcs: {len(f.undecided)} undecided, "
              f"{len(f.malformed)} malformed, "
              f"{len(f.stale_manifest)} stale manifest, "
              f"{len(f.stale_exclusions)} stale exclusions, "
              f"{len(f.overlap)} in both, "
              f"{len(f.universe)} src files total.")
        return 1 if f.any() else 0

    if f.any():
        report(f)
        if args.list:
            print()
            for x in f.undecided:
                print(x)
        return 1

    print(f"OK: every src/**/*.{{cpp,c}} ({len(f.universe)}) is in the manifest or "
          "exclusion baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
