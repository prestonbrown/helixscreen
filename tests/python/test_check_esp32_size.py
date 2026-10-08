# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/check_esp32_size.py: the budget and the std::locale map check."""
import json
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "check_esp32_size.py"

# The archive-member section of a GNU ld map, in the shape the ESP-IDF link writes.
MAP_WITH_LOCALE = """\
Archive member included to satisfy reference by file (symbol)

/x/libstdc++.a(locale_init.o)
                              /x/libstdc++.a(ios.o) (_ZNSt6localeC1Ev)
/x/libstdc++.a(ios.o)         /x/libstdc++.a(fs_path.o) (_ZNSt8ios_baseC2Ev)
/x/libstdc++.a(fs_path.o)
                              esp-idf/helixapp/libhelixapp.a(filament_catalog.cpp.obj) (_ZNSt10filesystem7__cxx114path5_ListC1Ev)

Discarded input sections
"""

MAP_WITHOUT_LOCALE = """\
Archive member included to satisfy reference by file (symbol)

/x/libstdc++.a(string-inst.o)
                              esp-idf/helixapp/libhelixapp.a(app_boot.cpp.obj) (_ZNKSt7__cxx1112basic_string)

Discarded input sections
"""


def run(tmp_path, size, budget, map_text=None, **ceilings):
    binp = tmp_path / "app.bin"
    binp.write_bytes(b"\0" * size)
    budgetp = tmp_path / "budget.json"
    budgetp.write_text(json.dumps({"app_max_bytes": budget, **ceilings}))
    args = [sys.executable, str(SCRIPT), str(binp), str(budgetp)]
    if map_text is not None:
        mapp = tmp_path / "app.map"
        mapp.write_text(map_text)
        args.append(str(mapp))
    return subprocess.run(args, capture_output=True, text=True)


def test_under_budget_passes(tmp_path):
    assert run(tmp_path, 100, 200).returncode == 0


def test_over_budget_fails(tmp_path):
    r = run(tmp_path, 300, 200)
    assert r.returncode == 1
    assert "exceeds budget" in r.stderr


def test_linked_locale_fails_and_names_the_chain(tmp_path):
    r = run(tmp_path, 100, 200, MAP_WITH_LOCALE)
    assert r.returncode == 1
    assert "locale_init.o <- ios.o" in r.stderr
    assert "fs_path.o <- filament_catalog.cpp.obj" in r.stderr


def test_map_without_locale_passes(tmp_path):
    assert run(tmp_path, 100, 200, MAP_WITHOUT_LOCALE).returncode == 0


# A link with exceptions on: the personality routine pulled by an object compiled
# with them, and IDF's .eh_frame output section.
MAP_WITH_EH = """\
Archive member included to satisfy reference by file (symbol)

/x/libstdc++.a(eh_personality.o)
                              esp-idf/helixapp/libhelixapp.a(config.cpp.obj) (__gxx_personality_v0)

Discarded input sections

.eh_frame       0x3c5eb9f4    0x6c264
"""

MAP_EMPTY_EH_FRAME = MAP_WITHOUT_LOCALE + """
.eh_frame       0x3c5eb9f4    0x0
"""


def test_linked_exception_support_fails_and_names_the_object(tmp_path):
    r = run(tmp_path, 100, 200, MAP_WITH_EH)
    assert r.returncode == 1
    assert "eh_personality.o <- config.cpp.obj (__gxx_personality_v0)" in r.stderr
    assert ".eh_frame output section: 442980 bytes" in r.stderr


def test_no_exception_support_and_an_empty_eh_frame_pass(tmp_path):
    assert run(tmp_path, 100, 200, MAP_WITHOUT_LOCALE).returncode == 0
    assert run(tmp_path, 100, 200, MAP_EMPTY_EH_FRAME).returncode == 0


# Internal-DRAM output sections as the ESP-IDF link map lays them out: the
# output section's size on its own line, then each input section with its size
# and object, long names wrapping onto the next line.
def dram_map(bss_size, data_size=0x100):
    return f"""\
Archive member included to satisfy reference by file (symbol)

Discarded input sections

.dram0.data     0x3fc98300     {data_size:#x}
 .data.small    0x3fc98300       0x40 esp-idf/helixapp/libhelixapp.a(small.cpp.obj)

.dram0.bss      0x3fc9d280     {bss_size:#x}
 .bss.app_elf_sha256_str
                0x3fc9d280        0xa esp-idf/esp_app_format/libesp_app_format.a(esp_app_desc.c.obj)
 .bss.lv_global 0x3fc9d290      0x64c esp-idf/lvgl/liblvgl.a(lv_init.c.obj)
 .bss._ZZN5helix2ui5fpath12_GLOBAL__N_112plan_scratchEvE4plan
                0x3fcb90f0     0x37d0 esp-idf/helixapp/libhelixapp.a(ui_filament_path_topology.cpp.obj)

.ext_ram.bss    0x3c0f0000     0x4000
 .ext_ram.bss.big
                0x3c0f0000     0x9000 esp-idf/helixapp/libhelixapp.a(huge.cpp.obj)
"""


def test_internal_dram_under_its_ceilings_passes(tmp_path):
    r = run(tmp_path, 100, 200, dram_map(0x4000), dram_bss_max_bytes=0x5000,
            dram_data_max_bytes=0x200)
    assert r.returncode == 0, r.stderr


def test_internal_dram_over_its_ceiling_fails_and_names_the_largest_statics(tmp_path):
    r = run(tmp_path, 100, 200, dram_map(0x5800), dram_bss_max_bytes=0x5000,
            dram_data_max_bytes=0x200)
    assert r.returncode == 1
    assert ".dram0.bss is 22528 bytes, 2048 over" in r.stderr
    lines = r.stderr.splitlines()
    largest = next(i for i, l in enumerate(lines) if "plan_scratch" in l)
    assert "14288" in lines[largest] and "ui_filament_path_topology.cpp.obj" in lines[largest]
    # Ordered by size, and only this section's: PSRAM statics are not listed.
    assert largest < next(i for i, l in enumerate(lines) if "lv_global" in l)
    assert "huge.cpp.obj" not in r.stderr
    assert "HELIX_PSRAM_BSS" in r.stderr


def test_internal_dram_data_has_its_own_ceiling(tmp_path):
    r = run(tmp_path, 100, 200, dram_map(0x4000, data_size=0x300), dram_bss_max_bytes=0x5000,
            dram_data_max_bytes=0x200)
    assert r.returncode == 1
    assert ".dram0.data is 768 bytes" in r.stderr


def test_a_budget_without_dram_ceilings_checks_only_the_image(tmp_path):
    assert run(tmp_path, 100, 200, dram_map(0x50000)).returncode == 0
