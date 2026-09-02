#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drift gate for the ESP32 native-audit `overrides/` forks of src/ files.

`firmware/native-audit/overrides/` holds hand-maintained COPIES of ten src/
files. `components/helixapp/app_srcs.txt` lists them as `overrides/<name>.cpp`
and CMake compiles those in place of the repo-relative `src/...` path, so the
override REPLACES its twin for that build (see that CMakeLists.txt: an
`^overrides/` line resolves under firmware/native-audit/, everything else under
the repo root).

Each fork exists for a narrow, real reason — on Xtensa `int32_t` is `long`, so
`std::max(1u, some_uint32)` has no deducible template argument and needs an
explicit one; and the renderer's whole-file `#if HELIX_HAS_GCODE_VIEWER` guard
is stripped so `idf.py size-components` can attribute its bytes. That is the
entire justified divergence. Everything ELSE that differs is drift.

WHY THIS GATE EXISTS (#1427)
  Nothing else can catch this. The audit tree is not a `make` target, CI never
  compiles it, and no test links it — so a fork can sit years behind its twin
  and every gate in the repo stays green. Meanwhile each fix that lands in
  src/ and not in the fork is a bug SILENTLY REINTRODUCED on the ESP32 build.
  A v0.99.118..HEAD sweep found three such forks at once: the prime-tower fit
  fix, the thumbnail-parity framing fix, and the stale-ghost cache discard had
  all landed in src/rendering/gcode_layer_renderer.cpp and none in the fork;
  grid_edit_mode.cpp still wrote the bare `.enabled = false` whose missing
  coordinate clear is the origin of the #1414 bad on-disk layouts.

WHAT IS FLAGGED
  - An override line that differs from its src/ twin without a nearby
    `AUDIT OVERRIDE` marker, in excess of the per-file baseline. That is the
    drift case: src/ moved and the fork did not.
  - An override with no src/ twin, or with an ambiguous one (two src/ files
    share the basename). Neither can be checked, and an unverifiable fork is
    the same liability as a stale one.
  - A baseline entry for an override that no longer exists.

WHAT IS REPORTED BUT DOES NOT FAIL
  - An override that is now byte-identical to its twin. Its divergence was
    absorbed upstream (src/ adopted the explicit template arguments verbatim),
    so the file is a no-op fork that can only re-drift. The fix is to delete it
    and repoint its app_srcs.txt line at the `src/...` path — but that edits
    the manifest, so this gate names it and leaves the decision to a human.
  - A file whose unmarked drift is BELOW its baseline. Ratchet the baseline
    down; the gate prints the line to change.

WHY A RATCHET AND NOT A HARD ZERO
  Five of the ten forks are already hundreds of lines behind (theme_manager.cpp
  and filament_slot_override_store.cpp by ~1000 each). A hard-zero gate would
  be red on arrival, and a gate that is red on arrival gets switched off. The
  baseline freezes today's debt per file and fails the moment any file's drift
  GROWS — which is exactly the event this gate exists to catch, since drift
  grows only when a commit touches a twin and skips its fork. The counts may
  fall, never rise. Same shape as check_imperative_ui.py's ratchet.

WHY MARKERS AND NOT A COMMIT-LOG COMPARISON
  The sweep that FOUND this compared `git log <range> -- <fork>` against
  `git log <range> -- <twin>`. That is a fine way to find drift once and a poor
  permanent gate: it can never go green over historical commits, it cannot tell
  a fork resynced in a later commit from one still stale, and it is defeated by
  a squash or a rebase. Comparing CONTENT is strictly stronger — it is true or
  false about the tree as it stands, self-heals the moment a fork is resynced,
  and needs no range. The commit log is still the best DIAGNOSTIC, so a failure
  prints the twin's commits since the fork was last touched: that list is the
  set of changes to port.

DIVERGENCE MARKERS
  A divergence is deliberate when `AUDIT OVERRIDE` appears on a line of the
  HUNK it belongs to. Scoping is per-hunk, not per-line and not by radius: one
  comment covers the multi-line edit it explains, and it cannot reach past that
  hunk to bless an unrelated change further down the file. A hunk that only
  DELETES (something src/ has and the fork drops) has no fork line of its own,
  so it is marked by a comment left standing in its place - which is how the
  stripped `#endif // HELIX_HAS_GCODE_VIEWER` is declared. Same idiom as
  `// DECLARATIVE_OK:` and `// VENDOR_OK:` elsewhere in the tree: the reason
  lives at the site.

MODES
  (default)         check; exit 0 if no file exceeds its baseline, 1 otherwise.
  --list            print every unmarked differing line, with file and lineno.
  --summary         one line per override.
  --write-baseline  freeze today's counts into the baseline file. Use when
                    resyncing a fork (counts fall) — never to silence a rise.

Exit 0 when no override's unmarked drift exceeds its baseline, 1 otherwise.
"""

from __future__ import annotations

import argparse
import difflib
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OVERRIDES = REPO_ROOT / "firmware/native-audit/overrides"
DEFAULT_SRC_ROOT = REPO_ROOT / "src"
DEFAULT_BASELINE = REPO_ROOT / "scripts/esp32_override_drift_baseline.txt"

MARKER = "AUDIT OVERRIDE"

BASELINE_HEADER = [
    "# ESP32 native-audit override drift baseline.",
    "#",
    "# Each line is the number of UNMARKED differing lines between",
    "# firmware/native-audit/overrides/<name> and its src/ twin — i.e. the",
    "# drift that carries no 'AUDIT OVERRIDE' explanation at the site.",
    "#",
    "# The counts may FALL, never RISE. A rise means a commit touched a src/",
    "# file and skipped its fork, which silently reintroduces on the ESP32",
    "# build every bug that commit fixed (#1427).",
    "#",
    "# To lower an entry: resync the fork against its twin, keeping only the",
    "# divergence that carries an AUDIT OVERRIDE marker, then regenerate with",
    "#   python3 scripts/check_esp32_override_drift.py --write-baseline",
    "#",
    "# Format:  <unmarked-differing-lines> <override basename>",
    "",
]


@dataclass
class Finding:
    """One unmarked differing region, and how many lines it is worth."""
    lineno: int   # 1-based, on the override side
    text: str
    weight: int


@dataclass
class OverrideResult:
    name: str
    twin: str | None = None            # repo-relative path, None when unpaired
    ambiguous: list[str] = field(default_factory=list)
    unmarked: list[Finding] = field(default_factory=list)
    marked: int = 0
    identical: bool = False
    baseline: int = 0

    @property
    def count(self) -> int:
        return sum(f.weight for f in self.unmarked)

    @property
    def unpaired(self) -> bool:
        return self.twin is None

    @property
    def exceeds(self) -> bool:
        return self.count > self.baseline

    @property
    def below(self) -> bool:
        return self.count < self.baseline


def find_twin(name: str, src_root: Path) -> tuple[str | None, list[str]]:
    """Locate the src/ file an override shadows, by basename.

    app_srcs.txt lists only the `overrides/<name>` path — it does not record
    which src/ file the override replaces — so basename is the only mapping
    available. Ambiguity is reported rather than guessed: picking one of two
    same-named files would silently check the fork against the wrong twin.
    """
    base = src_root.parent
    hits = sorted(
        str(p.relative_to(base)).replace("\\", "/")
        for p in src_root.rglob(name)
        if p.is_file()
    )
    if len(hits) == 1:
        return hits[0], []
    return None, hits


def classify(twin_lines: list[str], fork_lines: list[str]) -> tuple[list[Finding], int]:
    """Split the fork's differing lines into (unmarked, marked-count).

    Marking is scoped to the HUNK, not to a radius. A radius is the wrong tool:
    make it wide enough to cover a multi-line edit and it also lets one marker
    bless an unrelated change further down a small file, which is precisely the
    silent pass this gate exists to prevent.

      replace / insert  the hunk has fork-side lines, so it is marked when any
                        ONE of them carries the marker — a multi-line
                        intentional edit needs one comment, not one per line.
      delete            the twin has lines the fork does not, so there is no
                        fork line inside the hunk to mark. Only the lines
                        immediately either side of the deletion point count, so
                        a comment left standing in place of the removed block
                        marks it (the stripped `#endif // HELIX_HAS_GCODE_VIEWER`)
                        while a marker elsewhere in the file does not.
    """
    unmarked: list[Finding] = []
    marked = 0
    sm = difflib.SequenceMatcher(None, twin_lines, fork_lines, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue

        if tag == "delete":
            # WEIGHT IS THE TWIN-LINE SPAN, not 1. difflib collapses adjacent
            # deleted lines into a single opcode, so counting the hunk once
            # would let src/ append five more lines to a block the fork already
            # lacks without the total moving - drift growing while the gate
            # stays green, the one failure this must not have.
            neighbours = [j for j in (j1 - 1, j1) if 0 <= j < len(fork_lines)]
            if any(MARKER in fork_lines[j] for j in neighbours):
                marked += i2 - i1
            else:
                unmarked.append(Finding(
                    j1 + 1,
                    f"(twin lines {i1 + 1}-{i2} not present in the override)",
                    i2 - i1,
                ))
            continue

        # "replace" / "insert": the hunk's own fork-side lines decide it, and
        # the weight counts both sides so a fork that DROPS half a replaced
        # block still moves the total.
        hunk = list(range(j1, j2))
        weight = max(i2 - i1, j2 - j1)
        if any(MARKER in fork_lines[j] for j in hunk):
            marked += weight
        else:
            per_line = [Finding(j + 1, fork_lines[j].rstrip("\n"), 1) for j in hunk]
            # Give the hunk its full weight even when the fork side is shorter.
            if per_line:
                per_line[-1].weight += weight - len(per_line)
            unmarked.extend(per_line)
    return unmarked, marked


def load_baseline(path: Path) -> dict[str, int]:
    counts: dict[str, int] = {}
    if not path.exists():
        return counts
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        num, _, name = line.partition(" ")
        name = name.strip()
        if name:
            counts[name] = int(num)
    return counts


def twin_commits_since_fork(fork_rel: str, twin_rel: str) -> list[str]:
    """Commits touching the twin since the fork was last touched.

    Diagnostics only — this is the list of changes to port, and it is exactly
    the sweep that found the problem in the first place. Returns [] outside a
    git checkout so the gate still works on a fixture tree.
    """
    def git(*args: str) -> str:
        return subprocess.run(["git", "-C", str(REPO_ROOT), *args],
                              capture_output=True, text=True).stdout.strip()

    last = git("log", "-1", "--format=%H", "--", fork_rel)
    if not last:
        return []
    out = git("log", "--oneline", f"{last}..HEAD", "--", twin_rel)
    return [l for l in out.splitlines() if l]


def compute(overrides_dir: Path, src_root: Path, baseline: Path) -> list[OverrideResult]:
    base_counts = load_baseline(baseline)
    results: list[OverrideResult] = []
    for fork in sorted(overrides_dir.glob("*.c*")):
        if fork.suffix not in (".cpp", ".c"):
            continue
        r = OverrideResult(name=fork.name, baseline=base_counts.get(fork.name, 0))
        twin_rel, hits = find_twin(fork.name, src_root)
        if twin_rel is None:
            r.ambiguous = hits
            results.append(r)
            continue
        r.twin = twin_rel
        twin_lines = (src_root.parent / twin_rel).read_text(encoding="utf-8").splitlines(True)
        fork_lines = fork.read_text(encoding="utf-8").splitlines(True)
        if twin_lines == fork_lines:
            r.identical = True
            results.append(r)
            continue
        r.unmarked, r.marked = classify(twin_lines, fork_lines)
        results.append(r)
    return results


def stale_baseline_entries(results: list[OverrideResult], baseline: Path) -> list[str]:
    present = {r.name for r in results}
    return sorted(n for n in load_baseline(baseline) if n not in present)


def write_baseline(results: list[OverrideResult], path: Path) -> int:
    lines = list(BASELINE_HEADER)
    for r in sorted(results, key=lambda r: r.name):
        lines.append(f"{r.count} {r.name}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    total = sum(r.count for r in results)
    print(f"Wrote {path}: {len(results)} overrides, {total} unmarked differing lines total.")
    return 0


def report(results: list[OverrideResult], stale: list[str], overrides_dir: Path) -> None:
    rising = [r for r in results if r.exceeds]
    unpaired = [r for r in results if r.unpaired]

    if rising:
        print(f"FAIL: {len(rising)} ESP32 override(s) drifted further from their src/ twin.\n"
              "      A src/ file moved and its hand-maintained fork did not, so every fix in\n"
              "      those commits is silently reintroduced as a bug on the ESP32 build — the\n"
              "      audit tree is not a make target, so nothing else will ever catch it.",
              file=sys.stderr)
        for r in rising:
            print(f"\n      {overrides_dir.name}/{r.name}  vs  {r.twin}", file=sys.stderr)
            print(f"        {r.count} unmarked differing lines, baseline {r.baseline} "
                  f"(+{r.count - r.baseline})", file=sys.stderr)
            fork_rel = f"firmware/native-audit/overrides/{r.name}"
            commits = twin_commits_since_fork(fork_rel, r.twin)
            if commits:
                print("        commits on the twin since the fork was last touched — "
                      "this is what to port:", file=sys.stderr)
                for c in commits[:15]:
                    print(f"          {c}", file=sys.stderr)
                if len(commits) > 15:
                    print(f"          ... and {len(commits) - 15} more", file=sys.stderr)
        print("\n      Port the missing changes into the override, keeping only the divergence\n"
              "      that carries an 'AUDIT OVERRIDE' comment at the site, then ratchet the\n"
              "      baseline DOWN with --write-baseline. Do not raise the baseline to pass.",
              file=sys.stderr)

    if unpaired:
        print(f"\nFAIL: {len(unpaired)} override(s) cannot be paired with a src/ twin:",
              file=sys.stderr)
        for r in unpaired:
            if r.ambiguous:
                print(f"        {r.name} — {len(r.ambiguous)} src/ files share this basename: "
                      f"{', '.join(r.ambiguous)}", file=sys.stderr)
            else:
                print(f"        {r.name} — no src/ file with this basename", file=sys.stderr)
        print("\n      An override nothing can be compared against is as much a liability as a\n"
              "      stale one. If the twin was renamed, rename the override to match; if it\n"
              "      was deleted, delete the override and its app_srcs.txt line.", file=sys.stderr)

    if stale:
        print(f"\nFAIL: {len(stale)} baseline entr(ies) for overrides that no longer exist:",
              file=sys.stderr)
        for n in stale:
            print(f"        {n}", file=sys.stderr)
        print("\n      Regenerate with --write-baseline.", file=sys.stderr)


def advisories(results: list[OverrideResult]) -> None:
    """Non-failing notes: redundant forks, and baselines that can ratchet down."""
    identical = [r for r in results if r.identical]
    if identical:
        print(f"NOTE: {len(identical)} override(s) are now byte-identical to their src/ twin:")
        for r in identical:
            print(f"        {r.name} == {r.twin}")
        print("      Their divergence was absorbed upstream, so the fork is a no-op that can\n"
              "      only re-drift. Delete the file and repoint its app_srcs.txt line at the\n"
              "      src/ path. Left in place, it is a bug waiting to be reintroduced.")

    lowerable = [r for r in results if r.below]
    if lowerable:
        print(f"NOTE: {len(lowerable)} baseline entr(ies) can ratchet down "
              "(--write-baseline):")
        for r in lowerable:
            print(f"        {r.name}: {r.count} unmarked (baseline {r.baseline})")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--overrides", type=Path, default=DEFAULT_OVERRIDES)
    ap.add_argument("--src-root", type=Path, default=DEFAULT_SRC_ROOT)
    ap.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    ap.add_argument("--list", action="store_true",
                    help="print every unmarked differing line")
    ap.add_argument("--summary", action="store_true", help="one line per override")
    ap.add_argument("--write-baseline", action="store_true",
                    help="freeze today's counts. Use when RESYNCING a fork (counts fall), "
                         "never to silence a rise.")
    args = ap.parse_args()

    if not args.overrides.is_dir():
        # No forks at all is the goal state, and git does not track an empty
        # directory, so a fresh checkout simply has nothing here. Only a
        # baseline still naming forks makes this a failure.
        stale = sorted(load_baseline(args.baseline))
        if stale:
            print("FAIL: baseline names overrides that no longer exist:", file=sys.stderr)
            for name in stale:
                print(f"      {name}", file=sys.stderr)
            print("      Drop those rows with --write-baseline.", file=sys.stderr)
            return 1
        print("OK: no ESP32 overrides; src/ is compiled directly.")
        return 0

    results = compute(args.overrides, args.src_root, args.baseline)
    stale = stale_baseline_entries(results, args.baseline)

    if args.write_baseline:
        return write_baseline(results, args.baseline)

    if args.summary:
        for r in sorted(results, key=lambda r: r.name):
            if r.unpaired:
                state = "NO TWIN" if not r.ambiguous else "AMBIGUOUS TWIN"
            elif r.identical:
                state = "identical to twin (redundant fork)"
            else:
                state = f"{r.count} unmarked / {r.marked} marked, baseline {r.baseline}"
            print(f"  {r.name}: {state}")

    failed = any(r.exceeds or r.unpaired for r in results) or bool(stale)

    if failed:
        report(results, stale, args.overrides)
        if args.list:
            print(file=sys.stderr)
            for r in results:
                for f in r.unmarked:
                    print(f"{args.overrides.name}/{r.name}:{f.lineno}: {f.text}",
                          file=sys.stderr)
        return 1

    advisories(results)
    checked = [r for r in results if not r.unpaired]
    print(f"OK: {len(checked)} ESP32 override(s) within their drift baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
