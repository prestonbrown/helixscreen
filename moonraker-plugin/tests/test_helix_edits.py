# SPDX-License-Identifier: GPL-3.0-or-later
"""
Server-side G-code edits (helix_edits.py): the edit grammar against the golden
fixtures the app's C++ suite also reads, the parser's refusals, and
print_edited end to end through the linked-print path.

Run with: pytest tests/test_helix_edits.py -v
"""

import io
import json
import subprocess
from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_edits
from helix_edits import (
    EditError,
    EditRefused,
    apply_file,
    apply_plan,
    parse_edits,
    remap_line,
    tool_index_for_line,
)
from helix_print import HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockWebRequest,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)

# Shared with tests/unit/test_gcode_edit_list.cpp
GOLDEN_DIR = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "gcode_edits"
GOLDEN_CASES = sorted(p.name for p in GOLDEN_DIR.iterdir() if p.is_dir())

PRINT_EDITED = "/server/helix/print_edited"


def _apply_bytes(data: bytes, edits) -> bytes:
    out = io.BytesIO()
    apply_plan(io.BytesIO(data), out, parse_edits(edits))
    return out.getvalue()


def _edit(line, action, expect="", **args):
    return {"line_number": line, "expect_raw_line": expect, "action": action, "args": args}


# ============================================================================
# Golden fixtures
# ============================================================================

class TestGoldenFixtures:

    def test_the_fixture_set_is_there(self):
        # An empty glob would pass every parametrized case by running none.
        assert len(GOLDEN_CASES) >= 10
        assert "tool_remap" in GOLDEN_CASES and "expect_mismatch_refuses" in GOLDEN_CASES

    @pytest.mark.parametrize("name", GOLDEN_CASES)
    def test_case(self, name):
        case = GOLDEN_DIR / name
        spec = json.loads((case / "edits.json").read_text())
        data = (case / "input.gcode").read_bytes()
        if spec.get("refused"):
            with pytest.raises(EditRefused):
                _apply_bytes(data, spec["edits"])
            return
        assert _apply_bytes(data, spec["edits"]) == (case / "expected.gcode").read_bytes()

    @pytest.mark.parametrize("name", GOLDEN_CASES)
    def test_case_file_to_file(self, name, tmp_path):
        case = GOLDEN_DIR / name
        spec = json.loads((case / "edits.json").read_text())
        dst = tmp_path / "out.gcode"
        plan = parse_edits(spec["edits"])
        if spec.get("refused"):
            with pytest.raises(EditRefused):
                apply_file(str(case / "input.gcode"), str(dst), plan)
            assert not dst.exists()
            return
        apply_file(str(case / "input.gcode"), str(dst), plan)
        assert dst.read_bytes() == (case / "expected.gcode").read_bytes()


# ============================================================================
# Applying, beyond the fixtures
# ============================================================================

class TestApply:

    def test_a_later_edit_for_the_same_line_wins(self):
        out = _apply_bytes(b"A\nB\n", [
            _edit(2, "comment_out"), _edit(2, "replace", gcode="C"),
        ])
        assert out == b"A\nC\n"

    def test_the_copy_past_the_last_edit_is_verbatim(self):
        tail = b"".join(b"G1 X%d\n" % i for i in range(5000))
        out = _apply_bytes(b"PRINT_START\n" + tail, [_edit(1, "delete", "PRINT_START")])
        assert out == tail

    def test_deleting_every_line_leaves_an_empty_file(self):
        assert _apply_bytes(b"A\nB\n", [_edit(1, "delete", end_line=2)]) == b""

    def test_an_empty_file_stays_empty(self):
        assert _apply_bytes(b"", [_edit(1, "comment_out")]) == b""

    def test_an_empty_list_copies_the_file(self):
        assert _apply_bytes(b"A\r\nB", []) == b"A\r\nB"

    def test_a_refusal_comes_from_the_first_stale_line(self):
        with pytest.raises(EditRefused) as e:
            _apply_bytes(b"A\nB\nC\n", [_edit(3, "delete", "X"), _edit(2, "delete", "Y")])
        assert e.value.line_number == 2

    def test_non_ascii_text_round_trips_as_utf8(self):
        out = _apply_bytes("A\nM117 Düse\n".encode(), [
            _edit(2, "comment_out", "M117 Düse", comment="Aus"),
        ])
        assert out == "A\n; M117 Düse  ; [HelixScreen: Aus]\n".encode()


class TestToolRemapRules:

    @pytest.mark.parametrize("line,expected", [
        (b"T12", (12, 1, 3)),
        (b"  T3 ; comment", (3, 3, 4)),
        (b"T0 PURGE=1 X_Y=2", (0, 1, 2)),
        (b"T0 X1", None),
        (b"T0X1", None),
        (b"TURN", None),
        (b"T", None),
        (b"T100001", None),
        (b"T0 =1", None),
    ])
    def test_tool_index_for_line(self, line, expected):
        assert tool_index_for_line(line) == expected

    @pytest.mark.parametrize("line,expected", [
        (b"SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1 TEMP=200", b"SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=0 TEMP=200"),
        (b"SM_PRINT_FLOW_CALIBRATE EXTRUDER=x", b"SM_PRINT_FLOW_CALIBRATE EXTRUDER=x"),
        (b"M109 S200 T1", b"M109 S200 T0"),
        (b"M104 S200 T2 T0", b"M104 S200 T2 T0"),
        (b"M104 S200 ;T0", b"M104 S200 ;T0"),
        (b"START tool=1 initial_tool=0", b"START tool=0 initial_tool=1"),
        (b"START TOOL=T0", b"START TOOL=T0"),
        (b"START\tTOOL=0", b"START\tTOOL=1"),
        (b"; START TOOL=0", b"; START TOOL=0"),
        (b"START ; TOOL=0", b"START ; TOOL=0"),
        (b"TOOL=0", b"TOOL=0"),
    ])
    def test_remap_line(self, line, expected):
        assert remap_line(line, {0: 1, 1: 0}) == expected


# ============================================================================
# Parsing
# ============================================================================

class TestParse:

    @pytest.mark.parametrize("edits", [
        "not a list",
        ["not an object"],
        [_edit(1, "explode")],
        [{"line_number": 1, "action": "replace", "args": []}],
        [_edit(1, "replace")],
        [_edit(1, "inject_after", gcode=7)],
        [_edit(1, "delete", gcode="X")],
        [_edit(0, "delete")],
        [_edit(True, "delete")],
        [_edit("1", "delete")],
        [_edit(3, "delete", end_line=2)],
        [{"line_number": 1, "expect_raw_line": None, "action": "delete", "args": {}}],
        [_edit(0, "tool_remap", map={})],
        [_edit(0, "tool_remap", map={"T0": 1})],
        [_edit(0, "tool_remap", map={"0": -1})],
        [_edit(0, "tool_remap", map={"0": "1"})],
        [_edit(1, "tool_remap", map={"0": 1})],
        [_edit(False, "tool_remap", map={"0": 1})],
        [_edit(0, "tool_remap", "T0", map={"0": 1})],
        [_edit(0, "tool_remap", map={"0": 1}), _edit(1, "delete")],
    ])
    def test_malformed_lists_are_refused(self, edits):
        with pytest.raises(EditError):
            parse_edits(edits)

    def test_a_range_stays_one_span(self):
        plan = parse_edits([_edit(2, "comment_out", "X", end_line=4)])
        assert [(s.first, s.last) for s in plan.spans] == [(2, 4)]
        assert plan.spans[0].edit.expect == b"X"

    def test_a_single_line_edit_takes_its_own_line(self):
        plan = parse_edits([_edit(2, "comment_out"), _edit(5, "replace", gcode="Y")])
        assert [(s.first, s.last) for s in plan.spans] == [(2, 2), (5, 5)]

    def test_the_largest_range_costs_no_more_than_a_small_one(self):
        plan = parse_edits([_edit(1, "delete", end_line=helix_edits.MAX_LINE_NUMBER)])
        assert [(s.first, s.last) for s in plan.spans] == [(1, helix_edits.MAX_LINE_NUMBER)]

    @pytest.mark.parametrize("edits", [
        [_edit(helix_edits.MAX_LINE_NUMBER + 1, "delete")],
        [_edit(1, "delete", end_line=helix_edits.MAX_LINE_NUMBER + 1)],
        [_edit(1, "delete")] * (helix_edits.MAX_EDITS + 1),
    ])
    def test_out_of_bounds_lists_are_refused(self, edits):
        with pytest.raises(EditError):
            parse_edits(edits)

    def test_the_edit_cap_itself_is_accepted(self):
        assert len(parse_edits([_edit(1, "delete")] * helix_edits.MAX_EDITS).spans) == 1

    def test_a_later_edit_inside_a_range_wins_its_line(self):
        out = _apply_bytes(b"1\n2\n3\n4\n5\n", [
            _edit(2, "comment_out", end_line=4), _edit(3, "replace", gcode="R"),
        ])
        assert out == b"1\n; 2\nR\n; 4\n5\n"

    def test_a_later_range_covers_an_earlier_line_edit(self):
        out = _apply_bytes(b"1\n2\n3\n4\n5\n", [
            _edit(3, "replace", gcode="R"), _edit(2, "delete", end_line=4),
        ])
        assert out == b"1\n5\n"

    def test_overlapping_ranges_resolve_per_line(self):
        out = _apply_bytes(b"1\n2\n3\n4\n5\n6\n", [
            _edit(1, "comment_out", end_line=4), _edit(3, "delete", end_line=5),
        ])
        assert out == b"; 1\n; 2\n6\n"


# ============================================================================
# The worker process
# ============================================================================

class TestWorker:

    def _run(self, tmp_path, data, edits):
        src, dst = tmp_path / "in.gcode", tmp_path / "out.gcode"
        src.write_bytes(data)
        done = subprocess.run(helix_edits.worker_command(str(src), str(dst)),
                              input=json.dumps(edits).encode(), stdout=subprocess.PIPE,
                              timeout=60)
        assert done.returncode == 0
        return helix_edits.decode_worker_output(done.stdout), dst

    def test_the_worker_writes_what_the_in_process_pass_does(self, tmp_path):
        edits = [_edit(2, "comment_out", "B", comment="x"), _edit(3, "delete")]
        changed, dst = self._run(tmp_path, b"A\nB\nC\nD\n", edits)
        assert changed == 2
        assert dst.read_bytes() == _apply_bytes(b"A\nB\nC\nD\n", edits)

    def test_a_stale_line_comes_back_refused_with_nothing_written(self, tmp_path):
        with pytest.raises(EditRefused) as e:
            self._run(tmp_path, b"A\nB\n", [_edit(2, "delete", "X")])
        assert e.value.line_number == 2
        assert not (tmp_path / "out.gcode").exists()

    @pytest.mark.parametrize("data", [b"", b"Traceback", b'{"changed": "x"}', b'{"refused": true}'])
    def test_unreadable_output_is_a_failure(self, data):
        with pytest.raises(helix_edits.WorkerFailed):
            helix_edits.decode_worker_output(data)


# ============================================================================
# print_edited
# ============================================================================

def _request(**params):
    base = {
        "original_filename": "benchy.gcode",
        "temp_file_path": ".helix_temp/modified_1_benchy.gcode",
        "edits": [_edit(2, "comment_out", "BED_MESH_CALIBRATE", comment="Disabled Bed mesh")],
        "modifications": ["bed_mesh_disabled"],
    }
    base.update(params)
    return MockWebRequest(base)


@pytest.fixture
def gcodes(temp_gcodes_dir):
    root = Path(temp_gcodes_dir)
    (root / "benchy.gcode").write_bytes(b"G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n")
    return root


class TestPrintEdited:

    @pytest.mark.asyncio
    async def test_status_reports_the_feature_on_by_default(self, helix_print_component,
                                                            mock_server):
        await helix_print_component.component_init()
        status = await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))
        assert status["features"]["server_edits"] == {
            "version": helix_edits.SERVER_EDITS_FEATURE_VERSION, "available": True,
            "enabled": True, "reason": None,
        }

    @pytest.mark.asyncio
    async def test_edits_and_starts_the_copy_under_the_original_name(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request())

        copy = gcodes / ".helix_temp" / "modified_1_benchy.gcode"
        assert copy.read_bytes() == (
            b"G28\n; BED_MESH_CALIBRATE  ; [HelixScreen: Disabled Bed mesh]\nG1 X0 Y0\n"
        )
        assert not (gcodes / ".helix_temp" / "modified_1_benchy.gcode.part").exists()
        assert result["original_filename"] == "benchy.gcode"
        assert result["print_filename"] == ".helix_print/full/benchy.gcode"
        assert result["lines_changed"] == 1
        assert (gcodes / result["print_filename"]).resolve() == copy.resolve()
        helix_print_component.klippy_apis.run_gcode.assert_awaited_once_with(
            'SDCARD_PRINT_FILE FILENAME=".helix_print/full/benchy.gcode"'
        )
        info = helix_print_component.active_prints[result["print_filename"]]
        assert info.modifications == ["bed_mesh_disabled"]

    @pytest.mark.asyncio
    async def test_the_job_is_marked_print_edited(self, helix_print_component,
                                                  mock_server, gcodes):
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request())
        await helix_print_component._on_history_changed({
            "action": "added",
            "job": {"filename": result["print_filename"], "job_id": "00003A",
                    "start_time": 5.0},
        })
        assert helix_print_component.helix_initiated(
            "00003A", result["print_filename"], 5.0
        ) == "print_edited"

    @pytest.mark.asyncio
    async def test_a_failed_start_withdraws_the_marker_and_the_copy(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        helix_print_component.klippy_apis.run_gcode.side_effect = RuntimeError("klippy down")
        with pytest.raises(Exception, match="Failed to start print"):
            await mock_server.endpoints[PRINT_EDITED](_request())
        assert helix_print_component.job_markers.pending_count() == 0
        assert not (gcodes / ".helix_temp" / "modified_1_benchy.gcode").exists()

    @pytest.mark.asyncio
    async def test_a_stale_line_refuses_without_writing_or_starting(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        request = _request(edits=[
            _edit(1, "comment_out", "G28"), _edit(2, "comment_out", "QUAD_GANTRY_LEVEL"),
        ])
        with pytest.raises(Exception, match="409.*line 2"):
            await mock_server.endpoints[PRINT_EDITED](request)
        assert list((gcodes / ".helix_temp").iterdir()) == []
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()
        assert helix_print_component.job_markers.pending_count() == 0

    @pytest.mark.asyncio
    async def test_a_malformed_list_is_a_400(self, helix_print_component, mock_server, gcodes):
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="400.*Invalid edit list"):
            await mock_server.endpoints[PRINT_EDITED](_request(edits=[_edit(1, "explode")]))
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_a_plugin_features_opt_out_is_ignored(self, helix_print_component,
                                                         mock_server, gcodes):
        await helix_print_component.component_init()
        await helix_print_component.database.insert_item(
            HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, {"server_edits": False}
        )
        await mock_server.endpoints[PRINT_EDITED](_request())
        assert (gcodes / ".helix_temp" / "modified_1_benchy.gcode").exists()

    @pytest.mark.asyncio
    async def test_paths_outside_gcodes_are_refused(self, helix_print_component,
                                                    mock_server, gcodes):
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="400"):
            await mock_server.endpoints[PRINT_EDITED](_request(temp_file_path="../x.gcode"))
        with pytest.raises(Exception, match="not found"):
            await mock_server.endpoints[PRINT_EDITED](_request(original_filename="nope.gcode"))
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    @pytest.mark.parametrize("temp", ["benchy.gcode", "other.gcode", "sub/x.gcode"])
    async def test_the_copy_never_overwrites_a_user_file(self, helix_print_component,
                                                         mock_server, gcodes, temp):
        before = (gcodes / "benchy.gcode").read_bytes()
        (gcodes / "other.gcode").write_bytes(b"OTHER\n")
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="400"):
            await mock_server.endpoints[PRINT_EDITED](_request(temp_file_path=temp))
        assert (gcodes / "benchy.gcode").read_bytes() == before
        assert (gcodes / "other.gcode").read_bytes() == b"OTHER\n"
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_an_existing_copy_is_never_overwritten(self, helix_print_component,
                                                         mock_server, gcodes):
        """A staged copy that exists may be the file a print is running."""
        staged = gcodes / ".helix_temp" / "modified_1_benchy.gcode"
        staged.parent.mkdir(exist_ok=True)
        staged.write_bytes(b"PRINTING\n")
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="409"):
            await mock_server.endpoints[PRINT_EDITED](_request())
        assert staged.read_bytes() == b"PRINTING\n"
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_the_copy_takes_the_originals_mode(self, helix_print_component,
                                                     mock_server, gcodes):
        """A Klipper running as another user still reads the copy it prints."""
        (gcodes / "benchy.gcode").chmod(0o664)
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request())
        assert (gcodes / result["temp_filename"]).stat().st_mode & 0o777 == 0o664

    @pytest.mark.asyncio
    async def test_two_requests_for_one_copy_never_publish_a_mix(
        self, helix_print_component, mock_server, gcodes
    ):
        """Both pass the existence check while their workers run; only the
        first to finish may publish, whole, and the other is refused."""
        import asyncio
        await helix_print_component.component_init()
        a = _request(edits=[_edit(1, "replace", "G28", gcode="A" * 20000)])
        b = _request(edits=[_edit(1, "replace", "G28", gcode="B" * 20000)])
        results = await asyncio.gather(mock_server.endpoints[PRINT_EDITED](a),
                                       mock_server.endpoints[PRINT_EDITED](b),
                                       return_exceptions=True)
        refused = [r for r in results if isinstance(r, Exception)]
        assert len(refused) == 1 and "409" in str(refused[0])
        body = (gcodes / ".helix_temp" / "modified_1_benchy.gcode").read_bytes()
        assert body.split(b"\n")[0] in (b"A" * 20000, b"B" * 20000)
        assert sorted(p.name for p in (gcodes / ".helix_temp").iterdir()) == \
            ["modified_1_benchy.gcode"]

    @pytest.mark.asyncio
    async def test_without_a_path_the_copy_goes_to_the_temp_dir(
        self, helix_print_component, mock_server, gcodes
    ):
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request(temp_file_path=""))
        assert result["temp_filename"].startswith(".helix_temp/modified_")
        assert result["temp_filename"].endswith("_benchy.gcode")
        assert (gcodes / result["temp_filename"]).is_file()

    @pytest.mark.asyncio
    async def test_the_rewrite_runs_outside_moonrakers_process(
        self, helix_print_component, mock_server, gcodes, monkeypatch
    ):
        def in_process(*args):
            raise AssertionError("the per-line pass ran inside Moonraker")

        monkeypatch.setattr(helix_edits, "apply_file", in_process)
        monkeypatch.setattr(helix_edits, "apply_plan", in_process)
        await helix_print_component.component_init()
        result = await mock_server.endpoints[PRINT_EDITED](_request())
        assert result["lines_changed"] == 1
        assert b"[HelixScreen: Disabled Bed mesh]" in (gcodes / result["temp_filename"]).read_bytes()

    @pytest.mark.asyncio
    async def test_a_crashing_worker_is_a_500_and_leaves_nothing(
        self, helix_print_component, mock_server, gcodes, monkeypatch, tmp_path
    ):
        script = tmp_path / "crash.py"
        script.write_text("import sys\nsys.exit(3)\n")
        monkeypatch.setattr(helix_edits, "worker_command",
                            lambda src, dst: [sys.executable, str(script)])
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="500"):
            await mock_server.endpoints[PRINT_EDITED](_request())
        assert list((gcodes / ".helix_temp").iterdir()) == []
        helix_print_component.klippy_apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_a_missing_module_reports_the_feature_unavailable(
        self, helix_print_component, mock_server, monkeypatch
    ):
        import helix_print
        monkeypatch.setattr(helix_print, "helix_edits", None)
        monkeypatch.setitem(helix_print.SIBLING_LOAD_ERRORS, "helix_edits", "module_missing")
        await helix_print_component.component_init()
        status = await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))
        assert status["features"]["server_edits"]["available"] is False
        assert status["features"]["server_edits"]["reason"] == "module_missing"
        assert PRINT_EDITED not in mock_server.endpoints
