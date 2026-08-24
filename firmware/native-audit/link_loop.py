#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task 3 automated link loop. Repeatedly builds the slice; after each failed
# link, maps undefined symbols to defining src/ files (Linux-build symbol
# index) and auto-appends the safe ones to components/helixapp/app_srcs.txt:
#
#   auto-add:  Task 2 sweep bucket A or B, or C-rtti (RTTI is enabled now),
#              or unswept paths that don't match the platform/network denylist
#   stop for a human: bucket D, C-other, denylist paths (src/api/* Moonraker
#              seam → audit_moonraker_stub.cpp, display/application bootstrap,
#              bluetooth, network), or a compile (non-link) error
#
# Usage: python3 link_loop.py [max_rounds]

import csv
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

AUDIT = Path(__file__).resolve().parent
REPO = AUDIT.parents[1]
SRCS_TXT = AUDIT / "components/helixapp/app_srcs.txt"
SYMINDEX = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(
    "/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/026652d2-07d5-4dc3-a081-f411dad812d0/scratchpad/symindex.tsv")

DENY = re.compile(
    r"src/(api/|network/|bluetooth/|main\.cpp|app_globals\.cpp|"
    r"print/thumbnail_processor\.cpp|plugin/plugin_manager\.cpp|rendering/gcode_data_source\.cpp|"  # hv/hthreadpool.h (libhv thread pool)
    r"application/(application|display_manager|"
    r"input_manager|panel_factory|moonraker_manager)\.cpp)")

buckets = {}
with open(AUDIT / "audit_sweep_results_pass2.csv") as f:
    for row in csv.DictReader(f):
        buckets[row["file"]] = (row["bucket"], row["blocking"])


def auto_addable(src: str):
    if DENY.search(src):
        return False, "denylist (platform/network seam)"
    b = buckets.get(src)
    if b is None:
        return True, "unswept, not denylisted — optimistic add"
    bucket, blocking = b
    if bucket in ("A", "B"):
        return True, f"sweep {bucket}"
    if bucket == "C" and "rtti" in blocking:
        return True, "sweep C-rtti (RTTI enabled)"
    return False, f"sweep {bucket}: {blocking}"


def build(round_no: int) -> str:
    log = AUDIT / f"build/link_loop_round{round_no}.log"
    p = subprocess.run(
        ["bash", "-c",
         f"source ~/Code/esp-idf/export.sh >/dev/null 2>&1 && cd {AUDIT} && idf.py build"],
        capture_output=True, text=True)
    log.write_text(p.stdout + p.stderr)
    if p.returncode == 0:
        return "OK", log
    return "FAIL", log


def undefined_sources(log_text: str):
    undef = sorted(set(re.findall(r"undefined reference to `([^']+)'", log_text)))
    mangled = [s for s in undef if s.startswith("_Z")]
    demap = {}
    if mangled:
        out = subprocess.run(["c++filt"], input="\n".join(mangled),
                             capture_output=True, text=True).stdout.splitlines()
        demap = dict(zip(mangled, out))
    undef = [demap.get(s, s) for s in undef]

    index = defaultdict(set)
    src_cache = {}

    def obj_to_src(obj: str):
        # Linux objects mostly mirror src/, but generated/vendored objects
        # (fonts, images) live at repo root — probe candidates on disk.
        if obj in src_cache:
            return src_cache[obj]
        stem = obj.replace("build/obj/", "", 1).rsplit(".o", 1)[0]
        src = None
        for cand in (f"src/{stem}.cpp", f"{stem}.cpp", f"{stem}.c", f"src/{stem}.c"):
            if (REPO / cand).is_file():
                src = cand
                break
        src_cache[obj] = src
        return src

    for line in open(SYMINDEX, errors="replace"):
        try:
            sym, obj = line.rstrip("\n").split("\t")
        except ValueError:
            continue
        src = obj_to_src(obj)
        if src:
            index[sym.split("(")[0].strip()].add(src)

    by_src, unresolved = defaultdict(list), []
    for sym in undef:
        hits = index.get(sym.split("(")[0].strip())
        if hits:
            for s in hits:
                by_src[s].append(sym)
        else:
            unresolved.append(sym)
    return by_src, unresolved


def main():
    max_rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    current = set(l.strip() for l in SRCS_TXT.read_text().splitlines()
                  if l.strip() and not l.startswith("#"))

    for rnd in range(1, max_rounds + 1):
        print(f"=== round {rnd}: {len(current)} sources ===", flush=True)
        status, log = build(rnd)
        text = log.read_text(errors="replace")
        if status == "OK":
            print("LINKED. Slice builds clean.")
            return 0
        if "undefined reference" not in text:
            errs = [l for l in text.splitlines() if " error:" in l][:8]
            print("COMPILE ERROR (not link) — human needed:")
            for e in errs:
                print("  " + e[:220])
            return 2

        by_src, unresolved = undefined_sources(text)
        added, blocked = [], []
        # ui_test_utils defines test doubles for symbols the real TUs own —
        # never a slice candidate.
        for src in sorted(by_src):
            if "tests/" in src:
                continue
            if src in current:
                continue
            ok, why = auto_addable(src)
            if ok:
                added.append((src, why))
            else:
                blocked.append((src, why, by_src[src][:3]))

        for src, why in added:
            current.add(src)
            with open(SRCS_TXT, "a") as f:
                f.write(f"{src}\n")
            print(f"  + {src}  [{why}]")

        if blocked:
            print("\nBLOCKED — human decision needed (stub vs add vs override):")
            for src, why, syms in blocked:
                print(f"  ! {src}  [{why}]")
                for s in syms:
                    print(f"      {s[:130]}")
        if unresolved:
            print(f"\n{len(unresolved)} symbols not in Linux index (stub candidates):")
            for s in unresolved[:30]:
                print(f"  ? {s[:150]}")
        if blocked or (unresolved and not added):
            return 3
        if not added:
            print("No progress possible; stopping.")
            return 4
    print("Round limit reached.")
    return 5


if __name__ == "__main__":
    sys.exit(main())
