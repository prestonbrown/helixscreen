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
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "firmware/helixscreen-esp32/components/helixapp/app_srcs.txt"
DEFAULT_EXCLUSIONS = REPO_ROOT / "firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt"
DEFAULT_SRC_ROOT = REPO_ROOT / "src"
SRC_SUFFIXES = (".cpp", ".c")

# The exact shape CMake's `REGEX "^[^#].*\.(cpp|c)$"` accepts AND that yields a
# usable path: no leading whitespace (which CMake keeps, producing `<repo>/ src/…`),
# nothing after the suffix (which makes the regex miss the line entirely).
CMAKE_MANIFEST_LINE = re.compile(r"^[^#\s].*\.(?:cpp|c)$")

DEFAULT_FILE_REASON = "not in the v1 Core+AMS cut"

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
    universe: set[str] = field(default_factory=set)

    def any(self) -> bool:
        return bool(self.undecided or self.malformed or self.stale_manifest
                    or self.stale_exclusions or self.overlap)


def why_cmake_drops(line: str) -> str:
    """Human-readable reason a manifest line is not CMake-consumable."""
    stripped = line.strip()
    if line != line.lstrip():
        return ("leading whitespace — CMake keeps it and builds the path "
                "'<repo>/ " + stripped + "'")
    if "#" in line:
        return ("trailing '#' comment — CMake's REGEX requires the line to END in "
                ".cpp/.c, so this line is silently DROPPED from the build")
    if line != line.rstrip():
        return ("trailing whitespace — CMake's REGEX requires the line to END in "
                ".cpp/.c, so this line is silently DROPPED from the build")
    return ("does not end in .cpp/.c — CMake's REGEX skips it, so it is silently "
            "DROPPED from the build")


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
        if CMAKE_MANIFEST_LINE.match(line):
            included.add(line)
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

    return Findings(undecided=undecided, malformed=malformed,
                    stale_manifest=stale_manifest, stale_exclusions=stale_exclusions,
                    overlap=overlap, universe=universe)


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
    args = ap.parse_args()

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
