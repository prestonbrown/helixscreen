#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if the ESP32 app image exceeds its budget, if its internal-DRAM static
data outgrows its ceiling, or if the link pulled in libstdc++'s std::locale
machinery or any exception-handling support. Usage:
   check_esp32_size.py build/helixscreen_esp32.bin firmware/helixscreen-esp32/size_budget.json \\
       [build/helixscreen_esp32.map]"""
import json
import os
import re
import sys

# locale_init.o builds every standard facet (char and wchar_t, both string ABIs),
# ~150K, the first time anything touches std::locale: iostreams, <regex>,
# std::filesystem. app_srcs.txt files are linted for those includes; this catches
# the ones that arrive through a header or a library.
LOCALE_MEMBER = "locale_init.o"

# The image is built without exceptions (sdkconfig.defaults). Re-enabling them
# costs ~1.3MB and still fits the budget, so the size check alone would not
# notice: the personality routine is linked only for code compiled with
# exceptions, and IDF emits an .eh_frame output section only when they are on.
EH_MEMBER = "eh_personality.o"
EH_FRAME_SECTION = re.compile(r"^\.eh_frame\s+0x[0-9a-f]+\s+0x([0-9a-f]+)", re.M)


def inclusion_chain(map_text: str, member: str) -> list[str]:
    """Why the linker pulled `member`, from the map's archive-member section."""
    head = map_text.split("Discarded input sections", 1)[0].splitlines()
    why = {}
    for i, line in enumerate(head):
        # ld writes the referencer on the member's own line when the member name
        # is short, and on the next line otherwise.
        m = re.match(r"^(\S+\.a\(([^)]+)\))(\s+.*)?$", line)
        if not m:
            continue
        rest = m.group(3) if m.group(3) and m.group(3).strip() else \
            (head[i + 1] if i + 1 < len(head) else "")
        r = re.match(r"^\s+(.*?)\s+\((.*)\)$", rest)
        if r:
            ref = re.search(r"\(([^)]+)\)$", r.group(1))
            why[m.group(2)] = (ref.group(1) if ref else r.group(1).split("/")[-1], r.group(2))
    chain, seen = [], set()
    while member in why and member not in seen:
        seen.add(member)
        ref, sym = why[member]
        chain.append(f"{member} <- {ref or 'the linker command line'} ({sym})")
        member = ref
    return chain


# Internal-DRAM output sections. Static data placed here is taken from the
# internal heap before any task runs, and the WiFi driver allocates its RX
# buffers there at start-up: 14KB of statics is enough to leave it unable to
# associate. A large static belongs in PSRAM (HELIX_PSRAM_BSS) or the heap.
DRAM_SECTIONS = {".dram0.bss": "dram_bss_max_bytes", ".dram0.data": "dram_data_max_bytes"}
LARGEST_SHOWN = 8


def section_size(map_text: str, name: str):
    m = re.search(rf"^{re.escape(name)}\s+0x[0-9a-f]+\s+0x([0-9a-f]+)", map_text, re.M)
    return int(m.group(1), 16) if m else None


def largest_inputs(map_text: str, name: str, count: int) -> list[tuple[int, str, str]]:
    """The biggest input sections of an output section, as (size, section, object)."""
    start = re.search(rf"^{re.escape(name)}\s", map_text, re.M)
    if not start:
        return []
    end = re.compile(r"^\.\S", re.M).search(map_text, start.end())
    body = map_text[start.end():end.start() if end else len(map_text)]
    found = []
    for m in re.finditer(r"^ (\S+)\s*\n?\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)", body, re.M):
        size = int(m.group(2), 16)
        if size:
            obj = re.search(r"\(([^)]+)\)$", m.group(3))
            found.append((size, m.group(1), obj.group(1) if obj else m.group(3).split("/")[-1]))
    return sorted(found, reverse=True)[:count]


def check_dram(map_text: str, budget: dict) -> bool:
    ok = True
    for name, key in DRAM_SECTIONS.items():
        ceiling = budget.get(key)
        size = section_size(map_text, name)
        if ceiling is None or size is None:
            continue
        print(f"esp32 {name}: {size} bytes / ceiling {ceiling}")
        if size <= ceiling:
            continue
        print(f"FAIL: {name} is {size} bytes, {size - ceiling} over its {ceiling}-byte ceiling. "
              "Internal-DRAM statics shrink the heap the WiFi driver starts in. Largest:",
              file=sys.stderr)
        for isize, section, obj in largest_inputs(map_text, name, LARGEST_SHOWN):
            print(f"        {isize:7d}  {section}  ({obj})", file=sys.stderr)
        print("      Move a large static to PSRAM (HELIX_PSRAM_BSS, helix_psram_attr.h) or the "
              "heap. Raise the ceiling in size_budget.json only after checking internal free at "
              "boot.", file=sys.stderr)
        ok = False
    return ok


def main() -> int:
    bin_path, budget_path = sys.argv[1], sys.argv[2]
    size = os.path.getsize(bin_path)
    budgets = json.load(open(budget_path))
    budget = budgets["app_max_bytes"]
    pct = 100.0 * size / budget
    print(f"esp32 image: {size} bytes / budget {budget} ({pct:.1f}%)")
    failed = False
    if size > budget:
        print("FAIL: image exceeds budget", file=sys.stderr)
        failed = True
    if len(sys.argv) > 3:
        chain = inclusion_chain(open(sys.argv[3], errors="replace").read(), LOCALE_MEMBER)
        if chain:
            print("FAIL: the image links libstdc++'s std::locale machinery (~150K). "
                  "Inclusion chain, innermost first:", file=sys.stderr)
            for step in chain:
                print(f"        {step}", file=sys.stderr)
            print("      Replace the stream, <regex> or std::filesystem use at the end of the "
                  "chain with text_io.h, helix_regex.h or helix_fs.h.", file=sys.stderr)
            failed = True
        map_text = open(sys.argv[3], errors="replace").read()
        if not check_dram(map_text, budgets):
            failed = True
        eh_chain = inclusion_chain(map_text, EH_MEMBER)
        eh_frame = EH_FRAME_SECTION.search(map_text)
        if eh_chain or (eh_frame and int(eh_frame.group(1), 16) > 0):
            print("FAIL: the image links exception-handling support; the firmware is built "
                  "without exceptions.", file=sys.stderr)
            for step in eh_chain:
                print(f"        {step}", file=sys.stderr)
            if eh_frame:
                print(f"        .eh_frame output section: {int(eh_frame.group(1), 16)} bytes",
                      file=sys.stderr)
            print("      Check CONFIG_COMPILER_CXX_EXCEPTIONS in sdkconfig.defaults and the "
                  "object at the end of the chain.", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
