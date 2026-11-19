#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Route the app\x27s LTO output objects to PSRAM .bss in the generated sections.ld.

The helixapp mapping in components/helixapp/linker.lf selects statics by archive
name. LTO objects are named *.ltrans.o and belong to no archive, so without this
they fall into internal .dram0.bss, which the WiFi driver needs. Only helixapp is
compiled with -flto, so every LTO object is helixapp code.
"""
import sys

EXCLUDE = "EXCLUDE_FILE(*libhelixapp.a "
EXCLUDE_LTO = "EXCLUDE_FILE(*.ltrans.o *libhelixapp.a "
EXT_RAM_RULE = " *(.ext_ram.bss .ext_ram.bss.*)\n"
LTO_RULE = " *.ltrans.o(.bss .bss.* COMMON)\n"


def patch(text: str) -> str:
    start = text.index(".dram0.bss (NOLOAD)")
    end = text.index("_bss_end", start)
    head = text[start:end]
    if EXCLUDE_LTO not in head:
        if EXCLUDE not in head:
            raise SystemExit("sections.ld: no helixapp exclusion in .dram0.bss")
        text = text[:start] + head.replace(EXCLUDE, EXCLUDE_LTO) + text[end:]
    if LTO_RULE not in text:
        if text.count(EXT_RAM_RULE) != 1:
            raise SystemExit("sections.ld: .ext_ram.bss rule not found exactly once")
        text = text.replace(EXT_RAM_RULE, EXT_RAM_RULE + LTO_RULE)
    return text


if __name__ == "__main__":
    path = sys.argv[1]
    with open(path, encoding="utf-8") as f:
        original = f.read()
    patched = patch(original)
    if patched != original:
        with open(path, "w", encoding="utf-8") as f:
            f.write(patched)
