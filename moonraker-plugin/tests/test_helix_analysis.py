# SPDX-License-Identifier: GPL-3.0-or-later
"""
File analysis (helix_analysis.py): the record each slicer's output yields,
the per-field source order, the layer index it writes for power-loss
recovery and the screen, invalidation, and the cost guard.

Fixtures in tests/fixtures/analysis are small files in each slicer's own
header and footer shapes: OrcaSlicer (two tools, EXCLUDE_OBJECT_DEFINE, an
embedded thumbnail, M600), PrusaSlicer (`; printing object` labels), Cura
(`;LAYER:n`, `;MESH:`), a .gcode.3mf plate converted output (`; CHANGE_LAYER`),
M486 labels, and a file with no DEFINE, labels or footer.

Run with: pytest tests/test_helix_analysis.py -v
"""

import asyncio
import io
import json
import os
import shutil
import sys
import threading
import types
from pathlib import Path
from typing import Any, Dict, List

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import helix_analysis as analysis
import helix_gcode_reader as reader
from helix_analysis import (
    HelixAnalysis,
    analyze_file,
    convex_hull,
    merge_tools,
    parse_object_define,
    record_current,
)

FIXTURES = Path(__file__).parent / "fixtures" / "analysis"


@pytest.fixture
def gc(tmp_path):
    root = tmp_path / "gcodes"
    root.mkdir()
    for f in FIXTURES.glob("*.gcode"):
        shutil.copy(str(f), str(root / f.name))
    return root


def run(gc, name, metadata=None):
    return analyze_file(gc, name, metadata or {})


# ============================================================================
# Records per slicer
# ============================================================================


class TestOrca:
    def test_tools_come_from_the_footer(self, gc):
        r = run(gc, "orca_two_tool.gcode").record
        assert [(t["tool"], t["mm"], t["grams"], t["color"], t["material"]) for t in r["tools"]] \
            == [(0, 120.5, 0.36, "#FF8800", "PLA"), (1, 40.25, 0.12, "#0000FF", "PETG")]
        assert all(set(t["sources"].values()) == {"footer"} for t in r["tools"])

    def test_header_bounds_and_temps(self, gc):
        r = run(gc, "orca_two_tool.gcode").record
        assert r["slicer"] == "OrcaSlicer 2.3.1"
        assert r["header"] == {
            "printer_model": "Voron 2.4 250", "nozzle_diameters": [0.4, 0.4],
            "nozzle_volume_types": [],
            "bed": {"min": [0.0, 0.0], "max": [250.0, 210.0], "max_height": 220.0},
            "filament_types": ["PLA", "PETG"],
        }
        # The purge line is printed, so it is inside the print bounds.
        assert r["bounds"]["print"] == {"min": [5.0, 5.0, 0.2], "max": [100.0, 25.0, 0.6]}
        assert r["bounds"]["max_hotend_temp"] == 240.0
        assert r["bounds"]["max_bed_temp"] == 60.0

    def test_objects_from_define_lines(self, gc):
        objects = run(gc, "orca_two_tool.gcode").record["objects"]
        assert objects["source"] == "define"
        assert [o["name"] for o in objects["list"]] == ["cube_id_0_copy_0", "cyl_id_1_copy_0"]
        assert objects["list"][0]["center"] == [20.0, 20.0]
        assert objects["list"][1]["polygon"] == [[55, 15], [65, 15], [65, 25], [55, 25]]

    def test_layers_and_pauses(self, gc):
        r = run(gc, "orca_two_tool.gcode").record
        assert r["layers"] == {"count": 3, "index": ".helix_index/orca_two_tool.gcode.layers.jsonl"}
        assert r["pauses"] == 1

    def test_embedded_thumbnail_is_used_when_metadata_has_none(self, gc):
        out = run(gc, "orca_two_tool.gcode")
        assert analysis.png_size(out.thumbnail_png) == (32, 24)  # the larger one

    def test_no_thumbnail_when_metadata_lists_one(self, gc):
        out = run(gc, "orca_two_tool.gcode", {"thumbnails": [{"width": 32}]})
        assert out.thumbnail_png is None


class TestPrusa:
    def test_objects_from_printing_object_labels(self, gc):
        r = run(gc, "prusa_labels.gcode").record
        assert r["objects"]["source"] == "label"
        a, b = r["objects"]["list"]
        assert a["name"] == "part_a id:0 copy 0"
        assert a["polygon"] == [[30.0, 40.0], [42.0, 40.0], [42.0, 52.0], [30.0, 52.0]]
        assert a["center"] == [36.0, 46.0]
        assert b["center"] == [86.0, 46.0]

    def test_header_reads_bed_shape_and_palette_fallback(self, gc):
        r = run(gc, "prusa_labels.gcode").record
        assert r["header"]["bed"] == {"min": [0.0, 0.0], "max": [250.0, 210.0],
                                      "max_height": 210.0}
        # printer_settings_id is in this file too: the model is printer_model's
        assert r["header"]["printer_model"] == "MK4"
        assert r["header"]["nozzle_diameters"] == [0.6]
        assert r["header"]["nozzle_volume_types"] == []  # PrusaSlicer writes none
        # extruder_colour is empty, so filament_colour answers
        assert r["tools"][0]["color"] == "#00FF00"

    def test_unknown_start_position_stays_out_of_the_bounds(self, gc):
        # The first move names only Z: X/Y are wherever the printer was.
        r = run(gc, "prusa_labels.gcode").record
        assert r["bounds"]["travel"]["min"][:2] == [30.0, 40.0]


class TestCura:
    def test_header_and_meters(self, gc):
        r = run(gc, "cura_mesh.gcode").record
        assert r["slicer"] == "Cura 5.3.1"
        assert r["header"]["printer_model"] == "Creality Ender-3"
        assert r["header"]["nozzle_diameters"] == [0.4]
        assert r["header"]["bed"] is None
        assert r["tools"][0]["mm"] == 50.0
        assert r["tools"][0]["sources"]["mm"] == "footer"

    def test_layers_and_mesh_labels(self, gc):
        r = run(gc, "cura_mesh.gcode").record
        assert r["layers"]["count"] == 3
        assert r["objects"]["source"] == "label"
        assert [o["name"] for o in r["objects"]["list"]] == ["cube.stl"]
        assert r["objects"]["list"][0]["polygon"] == [[10.0, 10.0], [30.0, 10.0], [30.0, 30.0],
                                                      [10.0, 30.0]]

    def test_thumbnail_is_generated_when_the_file_has_none(self, gc):
        png = run(gc, "cura_mesh.gcode").thumbnail_png
        assert png.startswith(b"\x89PNG")
        assert analysis.png_size(png) == (analysis.THUMB_SIZE, analysis.THUMB_SIZE)


class TestConverted3mf:
    def test_change_layer_markers_and_tools(self, gc):
        r = run(gc, "bambu_3mf_plate.gcode").record
        assert r["slicer"] == "BambuStudio 01.09.07.52"
        assert r["layers"]["count"] == 2
        assert [(t["tool"], t["color"]) for t in r["tools"]] == [(0, "#FFFFFF"), (1, "#000000")]
        assert r["header"]["printer_model"] == "Bambu Lab X1 Carbon"
        assert r["objects"] == {"source": "none", "list": []}


class TestM486:
    def test_named_labels_with_computed_polygons(self, gc):
        r = run(gc, "m486_objects.gcode").record
        assert r["objects"]["source"] == "label"
        assert [(o["name"], o["center"]) for o in r["objects"]["list"]] \
            == [("Box A", [12.5, 12.5]), ("Box B", [44.0, 14.0])]
        assert r["objects"]["list"][1]["polygon"] == [[40.0, 10.0], [48.0, 10.0], [48.0, 18.0],
                                                      [40.0, 18.0]]


class TestNoDefine:
    def test_everything_from_the_scan(self, gc):
        r = run(gc, "no_define.gcode").record
        assert r["objects"] == {"source": "none", "list": []}
        assert r["slicer"] is None
        (tool,) = r["tools"]
        assert tool["tool"] == 0
        assert tool["mm"] == pytest.approx(2.64)
        assert tool["sources"] == {"mm": "scan", "grams": None, "color": None, "material": None}
        assert r["bounds"]["print"] == {"min": [50.0, 50.0, 0.25], "max": [60.0, 60.0, 0.5]}


# ============================================================================
# Per-field source order
# ============================================================================


class TestMergeTools:
    FOOTER = {"mm": [100.0], "grams": [], "color": [], "material": ["PLA"]}

    def test_footer_then_metadata_then_scan_per_field(self):
        md = analysis._metadata_tools({
            "filament_weights": [3.0], "filament_weight_total": 3.0,
            "filament_colors": ["#112233"], "filament_type": "PETG",
        })
        (t,) = merge_tools([0], self.FOOTER, md, {0: 99.0})
        assert (t["mm"], t["grams"], t["color"], t["material"]) == (100.0, 3.0, "#112233", "PLA")
        assert t["sources"] == {"mm": "footer", "grams": "metadata", "color": "metadata",
                                "material": "footer"}

    def test_metadata_weights_that_do_not_add_up_are_not_grams(self):
        md = analysis._metadata_tools({"filament_weights": [1234.0],
                                       "filament_weight_total": 3.0})
        (t,) = merge_tools([0], {}, md, {})
        assert t["grams"] is None

    def test_metadata_total_length_answers_only_a_one_tool_file(self):
        md = analysis._metadata_tools({"filament_total": 500.0})
        one = merge_tools([0], {}, md, {})
        two = merge_tools([0, 1], {}, md, {0: 10.0, 1: 20.0})
        assert one[0]["mm"] == 500.0 and one[0]["sources"]["mm"] == "metadata"
        assert [t["mm"] for t in two] == [10.0, 20.0]
        assert {t["sources"]["mm"] for t in two} == {"scan"}

    def test_zero_amount_falls_through(self):
        (t,) = merge_tools([1], {"mm": [5.0, 0.0]}, {}, {1: 7.5})
        assert t["mm"] == 7.5 and t["sources"]["mm"] == "scan"

    def test_used_tools_come_from_the_footer_amounts_first(self):
        footer = {"grams": [0.0, 1.0, 0.0], "mm": []}
        assert analysis.tools_used(footer, {0: 5.0}, {0, 1}) == {1}
        assert analysis.tools_used({}, {0: 5.0, 2: 0.0}, {0, 2}) == {0}
        assert analysis.tools_used({}, {}, {3}) == {3}


# ============================================================================
# Pure helpers
# ============================================================================


def test_convex_hull_drops_interior_points():
    pts = [(0, 0), (2, 0), (2, 2), (0, 2), (1, 1), (1, 0)]
    assert convex_hull(pts) == [[0, 0], [2, 0], [2, 2], [0, 2]]


def test_running_hull_folds_batches():
    hull = analysis._Hull()
    hull.BATCH = 3
    for x, y in [(0, 0), (1, 1), (4, 0), (4, 4), (2, 2), (0, 4)]:
        hull.add(x, y)
    assert hull.result() == [[0, 0], [4, 0], [4, 4], [0, 4]]


def test_parse_object_define():
    d = parse_object_define("EXCLUDE_OBJECT_DEFINE NAME=a CENTER=1.5,2 POLYGON=[[0,0],[3,0],[3,4]]")
    assert d == {"name": "a", "center": [1.5, 2.0], "polygon": [[0.0, 0.0], [3.0, 0.0], [3.0, 4.0]]}
    assert parse_object_define("EXCLUDE_OBJECT_DEFINE CENTER=1,2") is None


def test_orcas_nozzle_flow_types_are_read_as_written():
    from helix_analysis import header_from_config, parse_config_comment
    assert parse_config_comment(b"; nozzle_volume_type = Standard,High Flow") \
        == ("nozzle_volume_type", "Standard,High Flow")
    header = header_from_config({"nozzle_volume_type": "Standard,High Flow"})
    assert header["nozzle_volume_types"] == ["Standard", "High Flow"]
    assert header_from_config({})["nozzle_volume_types"] == []


def test_a_preset_name_is_never_the_printer_model():
    from helix_analysis import header_from_config
    assert header_from_config({"printer_settings_id": "My Voron"})["printer_model"] is None
    assert header_from_config({"printer_settings_id": "My Voron",
                               "printer_model": "Voron 0.2"})["printer_model"] == "Voron 0.2"


@pytest.mark.parametrize("path,ok", [
    ("a.gcode", True), ("sub/b.GCO", True), ("c.g", True), ("d.gcode.3mf", False),
    (".helix_temp/x.gcode", False), ("sub/.hidden.gcode", False), ("notes.txt", False),
])
def test_is_analyzable(path, ok):
    assert analysis.is_analyzable(path) is ok


def test_record_current_compares_size_and_mtime():
    rec = {"schema": analysis.RECORD_SCHEMA, "file_size": 10, "file_mtime": 5.0}
    assert record_current(rec, 10, 5.0)
    assert not record_current(rec, 10, 6.0)
    assert not record_current(rec, 11, 5.0)
    assert not record_current(dict(rec, schema=0), 10, 5.0)
    assert not record_current(None, 10, 5.0)


def test_thumbnail_relpath_is_moonrakers_layout():
    assert analysis.thumbnail_relpath("sub/part.gcode", 300, 300) == "sub/.thumbs/part-300x300.png"


# ============================================================================
# The layer index sidecar
# ============================================================================


class TestLayerIndex:
    def entries(self, gc, name):
        run(gc, name)
        lines = [json.loads(l) for l in
                 open(str(reader.layer_index_path(gc, name))).read().splitlines()]
        return lines[0], [l for l in lines if "layer" in l], lines

    def test_the_index_is_complete_for_the_files_identity(self, gc):
        run(gc, "orca_two_tool.gcode")
        identity = reader.file_identity(gc / "orca_two_tool.gcode")
        summary = reader.read_layer_index(
            reader.layer_index_path(gc, "orca_two_tool.gcode"), identity)
        assert summary is not None and summary.complete
        assert summary.last.layer == 3

    def test_each_start_resumes_a_scan_to_the_same_end_state(self, gc):
        name = "orca_two_tool.gcode"
        run(gc, name)
        data = (gc / name).read_bytes()
        whole = reader.scan_to_offset(io.BytesIO(data), len(data))
        _header, starts, _ = self.entries(gc, name)
        assert len(starts) == 3
        for s in starts:
            resumed = reader.scan_to_offset(io.BytesIO(data), len(data), s["offset"],
                                            reader.ModalState.from_dict(s["state"]))
            assert resumed.state_at_head == whole.state_at_head

    def test_starts_carry_tools_print_z_and_feature(self, gc):
        _h, starts, _ = self.entries(gc, "orca_two_tool.gcode")
        assert [s["tools"] for s in starts] == [[0], [0, 1], [0, 1]]
        assert [s["print_z"] for s in starts] == [0.2, 0.4, 0.6]
        # The section in effect at the marker is the one the layer opens in.
        assert starts[0]["feature"] == "Custom"
        assert starts[1]["feature"] == "Inner wall"
        assert starts[1]["state"]["active_tool"] == 0

    def test_progress_lines_hold_pauses_and_m73(self, gc):
        _h, _s, lines = self.entries(gc, "orca_two_tool.gcode")
        (record,) = [l for l in lines if "progress_lines" in l]
        texts = [t for _o, t in record["progress_lines"]]
        assert "M600" in texts
        assert texts[0] == "M73 P0 R10"
        assert "SET_PRINT_STATS_INFO CURRENT_LAYER=2" in texts
        data = (gc / "orca_two_tool.gcode").read_bytes()
        for offset, text in record["progress_lines"]:
            assert data[offset:offset + len(text)].decode() == text

    def test_a_follower_index_in_place_is_replaced_whole(self, gc):
        name = "no_define.gcode"
        path = reader.layer_index_path(gc, name)
        identity = reader.file_identity(gc / name)
        partial = reader.LayerIndexWriter(path, identity)
        partial.append(1, 30, reader.ModalState())
        run(gc, name)
        partial.append(2, 99, reader.ModalState())  # lands in the replaced file
        partial.close()
        summary = reader.read_layer_index(path, identity)
        assert summary.complete and summary.last.layer == 2 and summary.last.offset != 99


# ============================================================================
# The component
# ============================================================================


class ServerError(Exception):
    def __init__(self, message, status_code=400):
        super().__init__(message)
        self.status_code = status_code


class FakeServer:
    def __init__(self):
        self.endpoints: Dict[str, Any] = {}
        self.handlers: Dict[str, List[Any]] = {}
        self.notifications: List[str] = []
        self.events: List[Any] = []

    def register_endpoint(self, path, methods, handler):
        self.endpoints[path] = handler

    def register_event_handler(self, event, handler):
        self.handlers.setdefault(event, []).append(handler)

    def register_notification(self, name):
        self.notifications.append(name)

    def send_event(self, event, data):
        self.events.append((event, data))

    def error(self, message, code=400):
        return ServerError(message, code)


class FakeDatabase:
    def __init__(self):
        self.ns: Dict[str, Dict[str, Any]] = {}

    async def insert_item(self, namespace, key, value):
        assert isinstance(key, list) and len(key) == 1
        self.ns.setdefault(namespace, {})[key[0]] = value

    async def get_item(self, namespace, key=None, default=None):
        items = self.ns.get(namespace)
        if key is None:
            return dict(items) if items is not None else default
        return items.get(key[0], default) if items else default

    async def delete_item(self, namespace, key):
        self.ns.get(namespace, {}).pop(key[0], None)


class FakeMetadataStorage:
    def __init__(self):
        self.entries: Dict[str, Dict[str, Any]] = {}

    def get(self, key, default=None):
        return self.entries.get(key, default)

    def insert(self, key, value):
        self.entries[key] = value


class FakeFileManager:
    def __init__(self):
        self.gcode_metadata = FakeMetadataStorage()

    def get_file_metadata(self, path):
        return self.gcode_metadata.get(path, {})


class FakeKlippyApis:
    def __init__(self, state="standby"):
        self.state = state

    async def query_objects(self, objects):
        return {"print_stats": {"state": self.state}}


class FakeHelix:
    def __init__(self, gc):
        self.server = FakeServer()
        self.gc_path = gc
        self.enabled = True
        self.database = FakeDatabase()
        self.file_manager = FakeFileManager()
        self.klippy_apis = FakeKlippyApis()
        self.feature_on = True

    async def feature_enabled(self, name):
        assert name == analysis.ANALYSIS_FEATURE
        return self.feature_on


class FakeRequest:
    def __init__(self, params):
        self.params = params

    def get_str(self, key, default=None):
        return self.params.get(key, default)


async def make(gc, cpu_count=4, state="standby", files=None):
    if files is not None:
        for f in list(gc.iterdir()):
            if f.is_file() and f.name not in files:
                f.unlink()
    helix = FakeHelix(gc)
    helix.klippy_apis.state = state
    comp = HelixAnalysis(helix, cpu_count=cpu_count)
    comp.register()
    await comp.start()
    return helix, comp


async def ask(comp, filename):
    return await comp.handle_analysis(FakeRequest({"filename": filename}))


async def settle(comp):
    await comp.wait_idle()
    await asyncio.sleep(0)


class TestComponent:
    @pytest.mark.asyncio
    async def test_backfill_writes_a_record_per_file_and_notifies(self, gc):
        helix, comp = await make(gc)
        await settle(comp)
        stored = helix.database.ns[analysis.DB_NAMESPACE]
        assert sorted(stored) == sorted(p.name for p in FIXTURES.glob("*.gcode"))
        assert ("helix:analysis_ready", {"filename": "cura_mesh.gcode"}) in helix.server.events
        assert helix.server.notifications == [analysis.READY_EVENT]
        reply = await ask(comp, "cura_mesh.gcode")
        assert reply["status"] == "ready"
        assert reply["analysis"]["layers"]["count"] == 3

    @pytest.mark.asyncio
    async def test_generated_thumbnail_is_published_and_listed_in_metadata(self, gc):
        (gc / "sub").mkdir()
        shutil.move(str(gc / "cura_mesh.gcode"), str(gc / "sub" / "cura_mesh.gcode"))
        helix = FakeHelix(gc)
        helix.file_manager.gcode_metadata.entries["sub/cura_mesh.gcode"] = {"size": 1}
        comp = HelixAnalysis(helix, cpu_count=4)
        comp.register()
        await comp.start()
        await settle(comp)
        thumb = comp.records["sub/cura_mesh.gcode"]["thumbnail"]
        assert thumb["relative_path"] == "sub/.thumbs/cura_mesh-300x300.png"
        assert (gc / thumb["relative_path"]).read_bytes().startswith(b"\x89PNG")
        listed = helix.file_manager.gcode_metadata.entries["sub/cura_mesh.gcode"]["thumbnails"]
        assert listed == [{"width": 300, "height": 300, "size": thumb["size"],
                           "relative_path": ".thumbs/cura_mesh-300x300.png"}]

    @pytest.mark.asyncio
    async def test_a_thumbnail_is_published_through_the_shared_atomic_write(
            self, gc, monkeypatch):
        written = []
        real = reader.atomic_write_bytes
        monkeypatch.setattr(reader, "atomic_write_bytes",
                            lambda path, data, **k: written.append(path) or real(path, data, **k))
        helix, comp = await make(gc, files={"cura_mesh.gcode"})
        await settle(comp)
        thumb = comp.records["cura_mesh.gcode"]["thumbnail"]
        assert written == [str(gc / thumb["relative_path"])]
        assert not [p for p in (gc / ".thumbs").iterdir() if p.name.endswith(".tmp")]

    @pytest.mark.asyncio
    async def test_a_changed_file_is_pending_until_reanalyzed(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        assert (await ask(comp, "no_define.gcode"))["status"] == "ready"
        path = gc / "no_define.gcode"
        path.write_bytes(path.read_bytes() + b"G1 X70 Y70 E9\n")
        assert (await ask(comp, "no_define.gcode"))["status"] == "pending"
        await settle(comp)
        reply = await ask(comp, "no_define.gcode")
        assert reply["status"] == "ready"
        assert reply["analysis"]["bounds"]["print"]["max"][:2] == [70.0, 70.0]

    @pytest.mark.asyncio
    async def test_modify_event_reanalyzes(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        before = len(helix.server.events)
        path = gc / "no_define.gcode"
        path.write_bytes(path.read_bytes() + b"; more\n")
        await comp.on_filelist_changed({"action": "modify_file",
                                        "item": {"root": "gcodes", "path": "no_define.gcode"}})
        await settle(comp)
        assert len(helix.server.events) == before + 1
        assert comp.records["no_define.gcode"]["file_size"] == path.stat().st_size

    @pytest.mark.asyncio
    async def test_paused_while_printing_on_one_core(self, gc):
        helix, comp = await make(gc, cpu_count=1, state="printing", files={"no_define.gcode"})
        await asyncio.sleep(0.05)
        assert comp.records == {}
        assert (await ask(comp, "no_define.gcode"))["status"] == "pending"
        await asyncio.sleep(0.05)
        assert comp.records == {}
        await comp.on_job_state_changed(None, {}, {"state": "complete"})
        await settle(comp)
        assert "no_define.gcode" in comp.records

    @pytest.mark.asyncio
    async def test_runs_while_printing_on_a_multi_core_host(self, gc):
        helix, comp = await make(gc, cpu_count=4, state="printing", files={"no_define.gcode"})
        await settle(comp)
        assert "no_define.gcode" in comp.records

    @pytest.mark.asyncio
    async def test_switched_off_answers_disabled_and_drops_the_queue(self, gc):
        helix = FakeHelix(gc)
        helix.feature_on = False
        comp = HelixAnalysis(helix, cpu_count=4)
        comp.register()
        await comp.start()
        await settle(comp)
        assert comp.records == {}
        assert (await ask(comp, "no_define.gcode"))["status"] == "disabled"
        helix.feature_on = True
        assert (await ask(comp, "no_define.gcode"))["status"] == "pending"
        await settle(comp)
        assert set(comp.records) == {p.name for p in FIXTURES.glob("*.gcode")}

    @pytest.mark.asyncio
    async def test_bad_requests(self, gc):
        helix, comp = await make(gc, files=set())
        with pytest.raises(ServerError) as e:
            await ask(comp, "../etc/passwd.gcode")
        assert e.value.status_code == 400
        with pytest.raises(ServerError) as e:
            await ask(comp, "missing.gcode")
        assert e.value.status_code == 404

    @pytest.mark.asyncio
    async def test_delete_drops_record_index_and_generated_thumbnail(self, gc):
        helix, comp = await make(gc, files={"cura_mesh.gcode"})
        await settle(comp)
        record = comp.records["cura_mesh.gcode"]
        index = reader.layer_index_path(gc, "cura_mesh.gcode")
        thumb = gc / record["thumbnail"]["relative_path"]
        assert Path(index).exists() and thumb.exists()
        (gc / "cura_mesh.gcode").unlink()
        await comp.on_filelist_changed({"action": "delete_file",
                                        "item": {"root": "gcodes", "path": "cura_mesh.gcode"}})
        assert comp.records == {}
        assert analysis.DB_NAMESPACE not in helix.database.ns \
            or "cura_mesh.gcode" not in helix.database.ns[analysis.DB_NAMESPACE]
        assert not Path(index).exists() and not thumb.exists()

    @pytest.mark.asyncio
    async def test_move_keeps_the_record_and_index(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        (gc / "sub").mkdir()
        shutil.move(str(gc / "no_define.gcode"), str(gc / "sub" / "moved.gcode"))
        events = len(helix.server.events)
        await comp.on_filelist_changed({
            "action": "move_file", "item": {"root": "gcodes", "path": "sub/moved.gcode"},
            "source_item": {"root": "gcodes", "path": "no_define.gcode"}})
        await settle(comp)
        assert set(comp.records) == {"sub/moved.gcode"}
        assert len(helix.server.events) == events  # no second analysis
        assert comp.records["sub/moved.gcode"]["layers"]["index"] \
            == ".helix_index/sub/moved.gcode.layers.jsonl"
        assert Path(reader.layer_index_path(gc, "sub/moved.gcode")).exists()

    @pytest.mark.asyncio
    async def test_records_survive_a_restart_without_reanalysis(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        again = HelixAnalysis(helix, cpu_count=4)
        helix.server.events.clear()
        await again.start()
        await settle(again)
        assert helix.server.events == []
        assert (await ask(again, "no_define.gcode"))["status"] == "ready"

    @pytest.mark.asyncio
    async def test_records_of_vanished_files_are_dropped_at_startup(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode", "cura_mesh.gcode"})
        await settle(comp)
        (gc / "cura_mesh.gcode").unlink()
        again = HelixAnalysis(helix, cpu_count=4)
        await again.start()
        await settle(again)
        assert set(again.records) == {"no_define.gcode"}
        assert set(helix.database.ns[analysis.DB_NAMESPACE]) == {"no_define.gcode"}


class TestRecordFor:
    """record_for(): what the fit check waits on before an upload may start."""

    @pytest.mark.asyncio
    async def test_a_current_record_answers_at_once(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        record = await comp.record_for("no_define.gcode", 0.01)
        assert record is comp.records["no_define.gcode"]

    @pytest.mark.asyncio
    async def test_a_changed_file_is_analyzed_first_and_answered(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode", "cura_mesh.gcode"})
        await settle(comp)
        path = gc / "no_define.gcode"
        path.write_bytes(path.read_bytes() + b"G1 X71 Y71 E9\n")
        comp.enqueue("cura_mesh.gcode")
        record = await comp.record_for("no_define.gcode", 30.0)
        assert record is not None
        assert record["bounds"]["print"]["max"][:2] == [71.0, 71.0]
        assert not comp._waiters

    @pytest.mark.asyncio
    async def test_a_held_queue_answers_none_at_the_timeout(self, gc):
        helix, comp = await make(gc, cpu_count=1, state="printing", files={"no_define.gcode"})
        assert await comp.record_for("no_define.gcode", 0.05) is None
        assert not comp._waiters

    @pytest.mark.asyncio
    async def test_a_file_deleted_while_waiting_answers_none(self, gc):
        helix, comp = await make(gc, cpu_count=1, state="printing", files={"no_define.gcode"})
        waiting = asyncio.ensure_future(comp.record_for("no_define.gcode", 30.0))
        await asyncio.sleep(0)
        await comp.forget("no_define.gcode")
        assert await waiting is None

    @pytest.mark.asyncio
    async def test_a_missing_file_answers_none(self, gc):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        assert await comp.record_for("nope.gcode", 30.0) is None


def test_checked_in_sidecar_is_what_analysis_writes(gc):
    """tests/unit/test_gcode_layer_index_sidecar.cpp builds the screen's index
    from this file; it must stay what the analyzer writes for the fixture."""
    run(gc, "orca_two_tool.gcode")
    written = open(reader.layer_index_path(gc, "orca_two_tool.gcode")).read().splitlines()
    pinned = (FIXTURES / "orca_two_tool.layers.jsonl").read_text().splitlines()
    assert len(written) == len(pinned)
    header_w, header_p = json.loads(written[0]), json.loads(pinned[0])
    assert header_w["file_size"] == header_p["file_size"]
    assert header_w["file_hash"] == header_p["file_hash"]
    assert [json.loads(l) for l in written[1:]] == [json.loads(l) for l in pinned[1:]]


# ============================================================================
# The worker process
# ============================================================================


def fake_worker(tmp_path, body):
    """A worker command running @p body (Python) instead of the analyzer;
    it appends a line to calls.log each time it starts."""
    script = tmp_path / "fake_worker.py"
    log = tmp_path / "calls.log"
    script.write_text("import sys, time\nopen(%r, 'a').write('x\\n')\n%s\n" % (str(log), body))
    return [sys.executable, str(script)], log


def calls(log):
    return len(log.read_text().splitlines()) if log.exists() else 0


class TestWorker:
    def test_the_worker_answers_what_the_in_process_pass_does(self, gc):
        import subprocess
        argv = analysis.worker_command(gc, "orca_two_tool.gcode")
        assert argv[-2:] == [str(gc), "orca_two_tool.gcode"]
        done = subprocess.run(argv, input=b"{}", stdout=subprocess.PIPE, timeout=60)
        assert done.returncode == 0
        out = analysis.decode_output(done.stdout)
        local = run(gc, "orca_two_tool.gcode")
        for key in ("analyzed_at",):
            out.record.pop(key)
            local.record.pop(key)
        assert out.record == local.record
        assert out.thumbnail_png == local.thumbnail_png

    def test_unreadable_output_is_a_failure(self):
        with pytest.raises(analysis.WorkerFailed):
            analysis.decode_output(b"Traceback ...")
        with pytest.raises(analysis.WorkerFailed):
            analysis.decode_output(json.dumps({"record": {"schema": 99}}).encode())

    @pytest.mark.asyncio
    async def test_a_crashing_worker_leaves_the_file_pending_and_retries_once(
            self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "sys.exit(3)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        assert calls(log) == 2
        assert comp.records == {}
        assert (await ask(comp, "no_define.gcode"))["status"] == "failed"
        await settle(comp)
        assert calls(log) == 2
        # A new version of the file is tried again, with the real worker.
        monkeypatch.undo()
        path = gc / "no_define.gcode"
        path.write_bytes(path.read_bytes() + b"; changed\n")
        assert (await ask(comp, "no_define.gcode"))["status"] == "pending"
        await settle(comp)
        assert (await ask(comp, "no_define.gcode"))["status"] == "ready"

    @pytest.mark.asyncio
    async def test_a_killed_worker_counts_as_a_failed_attempt(self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "import os, signal\nos.kill(os.getpid(), signal.SIGKILL)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        assert calls(log) == 2
        assert "no_define.gcode" not in comp.records

    @pytest.mark.asyncio
    async def test_a_worker_past_its_timeout_is_killed(self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "time.sleep(60)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        monkeypatch.setattr(analysis, "worker_timeout", lambda size: 0.3)
        helix, comp = await make(gc, files={"no_define.gcode"})
        await asyncio.wait_for(settle(comp), 10)
        assert calls(log) == 2
        assert not comp._runner.running

    @pytest.mark.asyncio
    async def test_shutdown_kills_the_worker(self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "time.sleep(60)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix, comp = await make(gc, files={"no_define.gcode"})
        for _ in range(100):
            if comp._runner.running and calls(log):
                break
            await asyncio.sleep(0.02)
        proc = comp._runner.proc
        assert proc is not None and proc.returncode is None
        await asyncio.wait_for(comp.close(), 5)
        assert proc.returncode is not None

    @pytest.mark.asyncio
    async def test_a_print_starting_on_one_core_kills_the_worker_and_requeues(
            self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "time.sleep(60)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix, comp = await make(gc, cpu_count=1, files={"no_define.gcode"})
        for _ in range(100):
            if comp._runner.running and calls(log):
                break
            await asyncio.sleep(0.02)
        proc = comp._runner.proc
        monkeypatch.undo()
        await comp.on_job_state_changed(None, {}, {"state": "printing"})
        await asyncio.wait_for(proc.wait(), 5)
        await asyncio.sleep(0.05)
        assert comp.records == {}
        assert "no_define.gcode" in comp._queue
        await comp.on_job_state_changed(None, {}, {"state": "complete"})
        await settle(comp)
        assert "no_define.gcode" in comp.records


class TestRobustness:

    def test_an_unterminated_thumbnail_does_not_swallow_later_comments(self):
        c = analysis.AnalysisCollector(want_thumbnail=True)
        c.comment(None, b"; thumbnail begin 32x32 100", 0)
        c.comment(None, b"; iVBORw0KGgo=", 0)
        c.comment(None, b";TYPE:WALL-OUTER", 0)
        c.comment(None, b"; nozzle_diameter = 0.4", 0)
        assert c.feature == "WALL-OUTER"
        assert c.config.get("nozzle_diameter") == "0.4"
        assert c.thumbnails == []

    def test_a_thumbnail_past_the_cap_is_dropped_as_it_grows(self, monkeypatch):
        monkeypatch.setattr(analysis, "MAX_EMBEDDED_THUMB_B64", 100)
        c = analysis.AnalysisCollector(want_thumbnail=True)
        c.comment(None, b"; thumbnail begin 32x32 100", 0)
        for _ in range(10):
            c.comment(None, b"; " + b"A" * 40, 0)
        assert c._thumb is None
        c.comment(None, b"; thumbnail end", 0)
        assert c.thumbnails == []

    def test_a_whole_thumbnail_still_reads(self):
        c = analysis.AnalysisCollector(want_thumbnail=True)
        c.comment(None, b"; thumbnail begin 32x32 8", 0)
        c.comment(None, b"; iVBORw0K", 0)
        c.comment(None, b"; Ggo=", 0)
        c.comment(None, b"; thumbnail end", 0)
        assert c.thumbnails == [("PNG", 32, 32, "iVBORw0KGgo=")]

    def test_an_absurd_move_is_clipped_to_the_grid(self):
        import time
        fp = analysis.Footprint()
        started = time.monotonic()
        fp.segment(0.0, 0.0, 0.0, 0.0, 0.2, 0)
        fp.segment(0.0, 0.0, 1e9, 0.0, 0.2, 0)
        fp.segment(-1e12, -1e12, 1e12, 1e12, 0.2, 0)
        assert time.monotonic() - started < 1.0
        n = analysis.FOOTPRINT_CELLS
        row = n // 2
        assert all(fp._height[row * n + x] for x in range(n // 2, n))
        assert all(fp._height[k * n + k] for k in range(n))

    def test_a_move_entirely_off_the_grid_marks_nothing(self):
        fp = analysis.Footprint()
        fp.segment(0.0, 0.0, 0.0, 0.0, 0.2, 0)
        before = bytes(fp._height)
        fp.segment(1e6, 1e6, 2e6, 2e6, 0.2, 0)
        assert bytes(fp._height) == before

    @pytest.mark.asyncio
    async def test_a_file_deleted_during_its_pass_leaves_no_record(self, gc, monkeypatch):
        helix, comp = await make(gc, files=set())
        real = comp._run_worker

        async def delete_meanwhile(path, metadata, size):
            out = await real(path, metadata, size)
            (gc / path).unlink()
            return out

        monkeypatch.setattr(comp, "_run_worker", delete_meanwhile)
        shutil.copy(str(FIXTURES / "no_define.gcode"), str(gc / "no_define.gcode"))
        await comp.on_filelist_changed({"action": "create_file",
                                        "item": {"root": "gcodes", "path": "no_define.gcode"}})
        await settle(comp)
        assert comp.records == {}
        assert analysis.DB_NAMESPACE not in helix.database.ns or \
            helix.database.ns[analysis.DB_NAMESPACE] == {}
        assert not os.path.exists(reader.layer_index_path(gc, "no_define.gcode"))
        assert not [e for e in helix.server.events if e[0] == analysis.READY_EVENT]

    @pytest.mark.asyncio
    async def test_a_file_changed_during_its_pass_is_analyzed_again(self, gc, monkeypatch):
        helix, comp = await make(gc, files=set())
        real = comp._run_worker
        changed = []

        async def change_once(path, metadata, size):
            out = await real(path, metadata, size)
            if not changed:
                changed.append(1)
                with open(str(gc / path), "ab") as f:
                    f.write(b"; edited\n")
            return out

        monkeypatch.setattr(comp, "_run_worker", change_once)
        shutil.copy(str(FIXTURES / "no_define.gcode"), str(gc / "no_define.gcode"))
        await comp.on_filelist_changed({"action": "create_file",
                                        "item": {"root": "gcodes", "path": "no_define.gcode"}})
        await settle(comp)
        record = comp.records["no_define.gcode"]
        assert record["file_size"] == (gc / "no_define.gcode").stat().st_size

    @pytest.mark.asyncio
    async def test_a_failing_backfill_is_logged(self, gc, caplog):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)

        async def broken():
            raise RuntimeError("scan exploded")

        comp.backfill = broken
        comp._stale = True
        with open(str(gc / "no_define.gcode"), "ab") as f:
            f.write(b"; changed\n")
        await ask(comp, "no_define.gcode")
        await asyncio.sleep(0.01)
        assert "scan exploded" in caplog.text

    @pytest.mark.asyncio
    async def test_the_prune_spares_the_file_being_analyzed(self, gc, monkeypatch):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        tmp = reader.layer_index_path(gc, "busy.gcode") + ".tmp"
        open(tmp, "w").write("{}\n")
        comp._analyzing = "busy.gcode"
        assert tmp in [p for p in comp.protected_indexes()] or \
            reader.layer_index_path(gc, "busy.gcode") in comp.protected_indexes()
        await comp.backfill()
        assert os.path.exists(tmp)
        comp._analyzing = None
        await comp.backfill()
        assert not os.path.exists(tmp)


class NsItemsOnlyDatabase(FakeDatabase):
    """A database whose get_item cannot read a whole namespace, only ns_items."""

    async def get_item(self, namespace, key=None, default=None):
        if key is None:
            raise KeyError(namespace)
        return await super().get_item(namespace, key, default)

    async def ns_items(self, namespace):
        return list(self.ns.get(namespace, {}).items())


async def restart(gc, database, files=None):
    """A second component on the same database, as after a Moonraker restart."""
    helix = FakeHelix(gc)
    helix.database = database
    comp = HelixAnalysis(helix, cpu_count=4)
    comp.register()
    await comp.start()
    return helix, comp


class TestPersistence:
    @pytest.mark.asyncio
    async def test_records_load_where_only_ns_items_reads_a_namespace(
            self, gc, tmp_path, monkeypatch):
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        db = NsItemsOnlyDatabase()
        db.ns = helix.database.ns
        argv, log = fake_worker(tmp_path, "sys.exit(3)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix2, comp2 = await restart(gc, db)
        await settle(comp2)
        assert calls(log) == 0
        assert (await ask(comp2, "no_define.gcode"))["status"] == "ready"

    @pytest.mark.asyncio
    async def test_a_file_that_failed_twice_stays_failed_across_a_restart(
            self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "sys.exit(3)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        assert calls(log) == 2
        helix2, comp2 = await restart(gc, helix.database)
        await settle(comp2)
        assert calls(log) == 2
        assert (await ask(comp2, "no_define.gcode"))["status"] == "failed"
        await settle(comp2)
        assert calls(log) == 2

    @pytest.mark.asyncio
    async def test_a_changed_file_drops_its_stored_failure(self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "sys.exit(3)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        monkeypatch.undo()
        path = gc / "no_define.gcode"
        path.write_bytes(path.read_bytes() + b"; changed\n")
        helix2, comp2 = await restart(gc, helix.database)
        await settle(comp2)
        assert (await ask(comp2, "no_define.gcode"))["status"] == "ready"
        assert helix.database.ns.get(analysis.DB_FAILURES_NAMESPACE, {}) == {}

    @pytest.mark.asyncio
    async def test_a_deleted_file_drops_its_stored_failure(self, gc, tmp_path, monkeypatch):
        argv, log = fake_worker(tmp_path, "sys.exit(3)")
        monkeypatch.setattr(analysis, "worker_command", lambda root, rel: argv)
        helix, comp = await make(gc, files={"no_define.gcode"})
        await settle(comp)
        assert helix.database.ns[analysis.DB_FAILURES_NAMESPACE]
        (gc / "no_define.gcode").unlink()
        await comp.on_filelist_changed({"action": "delete_file",
                                        "item": {"root": "gcodes", "path": "no_define.gcode"}})
        assert helix.database.ns[analysis.DB_FAILURES_NAMESPACE] == {}


@pytest.mark.asyncio
async def test_startup_prunes_stale_indexes_but_not_the_ones_recovery_uses(gc):
    import os
    stale = reader.layer_index_path(gc, "gone.gcode")
    used = reader.layer_index_path(gc, "printing_copy.gcode")
    for path in (stale, used):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w").write('{"format": 1}\n')
    helix = FakeHelix(gc)
    helix.plr = types.SimpleNamespace(protected_index_paths=lambda: [used])
    comp = HelixAnalysis(helix, cpu_count=4)
    comp.register()
    await comp.start()
    await settle(comp)
    assert not os.path.exists(stale)
    assert os.path.exists(used)


# ============================================================================
# Pressure advance set by the file
# ============================================================================

class TestSetsPressureAdvance:
    """A slicer filament profile with pressure advance enabled sends
    SET_PRESSURE_ADVANCE itself, which replaces a measured value for the job."""

    def _write(self, gc, name, body):
        (gc / name).write_text("G28\nG1 X10 Y10 F3000\n" + body + "G1 X20 E1 F1200\n")
        return run(gc, name).record

    def test_a_file_that_sets_advance(self, gc):
        r = self._write(gc, "pa.gcode", "SET_PRESSURE_ADVANCE ADVANCE=0.045\n")
        assert r["sets_pressure_advance"] is True

    def test_smooth_time_alone_is_not_an_advance(self, gc):
        r = self._write(gc, "smooth.gcode", "SET_PRESSURE_ADVANCE SMOOTH_TIME=0.04\n")
        assert r["sets_pressure_advance"] is False

    def test_a_file_that_leaves_it_alone(self, gc):
        assert self._write(gc, "plain.gcode", "")["sets_pressure_advance"] is False
