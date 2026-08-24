#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task 3 link-loop helper: map "undefined reference" symbols from an IDF build
# log to the src/ files that define them, using a symbol index built from the
# LINUX build's objects:
#
#   for o in $(find build/obj -name '*.o'); do
#     nm -C --defined-only "$o" | awk -v f="$o" \
#       '$2 ~ /[TWVBDR]/ {s=$3; for(i=4;i<=NF;i++) s=s" "$i; print s "\t" f}'
#   done > symindex.tsv
#
# Output: source files ranked by how many missing symbols they'd satisfy —
# the top entries are the next APP_SRCS candidates (or stub candidates, if
# they're platform/network-bound per the Task 2 sweep buckets).
#
# Usage: python3 resolve_undefined.py <link.log> <symindex.tsv>

import re
import subprocess
import sys
from collections import defaultdict

log = open(sys.argv[1], errors="replace").read()
undef = sorted(set(re.findall(r"undefined reference to `([^']+)'", log)))

# The Xtensa ld prints mangled names; the nm -C index is demangled.
mangled = [s for s in undef if s.startswith("_Z")]
if mangled:
    out = subprocess.run(["c++filt"], input="\n".join(mangled),
                         capture_output=True, text=True).stdout.splitlines()
    demap = dict(zip(mangled, out))
    undef = sorted(set(demap.get(s, s) for s in undef))

# Qualified-name key (strip parameter list): tolerant of nm-vs-ld demangle
# formatting differences; overload merging is fine for file mapping.
def key(sym):
    return sym.split("(")[0].strip()

index = defaultdict(set)
for line in open(sys.argv[2], errors="replace"):
    try:
        sym, obj = line.rstrip("\n").split("\t")
    except ValueError:
        continue
    src = obj.replace("build/obj/", "src/").rsplit(".o", 1)[0] + ".cpp"
    index[key(sym)].add(src)

by_src = defaultdict(list)
unresolved = []
for sym in undef:
    hits = index.get(key(sym))
    if hits:
        for src in hits:
            by_src[src].append(sym)
    else:
        unresolved.append(sym)

print(f"{len(undef)} unique undefined symbols\n")
print("== defining source files (add to APP_SRCS or stub) ==")
for src, syms in sorted(by_src.items(), key=lambda kv: -len(kv[1])):
    print(f"{len(syms):>4}  {src}")
    for s in syms[:3]:
        print(f"        {s[:110]}")
print(f"\n== {len(unresolved)} not found in Linux build index ==")
for s in unresolved[:40]:
    print(f"  {s[:130]}")
