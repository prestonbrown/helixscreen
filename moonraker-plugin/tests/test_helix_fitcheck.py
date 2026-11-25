# SPDX-License-Identifier: GPL-3.0-or-later
"""
File-fits-printer check (helix_fitcheck.py): the checker and the printer limits
pinned by the table the screen's checker reads too, and the opt-in hold on a
slicer's upload-and-print.

Run with: pytest tests/test_helix_fitcheck.py -v
"""

import asyncio
import json
import logging
import shutil
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_fitcheck
from helix_fitcheck import (
    FIT_CHECK_FEATURE,
    REFUSED_EVENT,
    check_fit,
    fit_file_from_record,
    machine_limits,
    machine_with_live_nozzles,
    may_autostart,
    nozzle_volume_type,
)
from helix_print import HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, load_component
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockConfigHelper,
    MockDatabase,
    MockFileManager,
    MockHistory,
    MockKlippy,
    MockKlippyApis,
    mock_server,
    temp_gcodes_dir,
)

# Shared with tests/unit/test_fit_check.cpp
FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "fit_check" / "cases.json"
CASES = json.loads(FIXTURE.read_text())

SETTINGS = {
    "printer": {"kinematics": "corexy"},
    "stepper_x": {"position_min": 0.0, "position_max": 220.0},
    "stepper_y": {"position_min": 0.0, "position_max": 220.0},
    "stepper_z": {"position_min": 0.0, "position_max": 250.0},
    "extruder": {"max_temp": 300.0, "nozzle_diameter": 0.4},
    "heater_bed": {"max_temp": 110.0},
}


def _record(x_max=200.0, hotend=240.0, tools=(0,)):
    return {
        "schema": 1,
        "tools": [{"tool": t} for t in tools],
        "bounds": {
            "print": {"min": [10.0, 10.0, 0.2], "max": [x_max, 200.0, 20.0]},
            "travel": {"min": [0.0, 0.0, 0.2], "max": [x_max, 200.0, 30.0]},
            "max_hotend_temp": hotend, "max_bed_temp": 60.0,
        },
        "header": {"printer_model": "Voron 0.2", "nozzle_diameters": [0.4],
                   "bed": None, "filament_types": ["PLA"]},
    }


# ============================================================================
# The shared table
# ============================================================================


@pytest.mark.parametrize("case", CASES["machines"], ids=[c["name"] for c in CASES["machines"]])
def test_machine_limits_match_the_table(case):
    limits = machine_limits(case["settings"])
    if "live" in case:
        limits = machine_with_live_nozzles(limits, case["live"])
    assert limits == case["machine"]


@pytest.mark.parametrize("case", CASES["checks"], ids=[c["name"] for c in CASES["checks"]])
def test_findings_match_the_table(case):
    assert check_fit(case["machine"], case["file"]) == case["findings"]


def test_the_table_covers_every_kind_and_both_kinematics_families():
    severities = {f["kind"]: f["severity"] for c in CASES["checks"] for f in c["findings"]}
    assert severities["printer_model_mismatch"] == "note"
    assert severities["nozzle_mismatch"] == "warn"
    flow = {(f["detail"]["file"], f["severity"]) for c in CASES["checks"] for f in c["findings"]
            if f["kind"] == "nozzle_flow_mismatch"}
    assert flow == {("high_flow", "warn"), ("standard", "warn")}
    kinds = set(severities)
    assert kinds == {"out_of_bounds", "temp_over_max", "too_many_tools", "nozzle_mismatch",
                     "nozzle_flow_mismatch", "printer_model_mismatch"}
    assert any("live" in c for c in CASES["machines"])
    kinematics = {c["machine"]["kinematics"] for c in CASES["machines"]}
    assert {"delta", "corexy", "cartesian"} <= kinematics


@pytest.mark.parametrize("form,held", [
    ({"root": "gcodes", "filename": "a.gcode", "print": "true"}, True),
    ({"filename": "a.UFP", "print": "true"}, True),
    ({"root": "gcodes", "filename": "a.gcode", "print": "false"}, False),
    ({"root": "config", "filename": "a.gcode", "print": "true"}, False),
    ({"root": "gcodes", "filename": "a.gcode.3mf", "print": "true"}, False),
    ({"root": "gcodes", "filename": "notes.txt", "print": "true"}, False),
    ("not a form", False),
])
def test_which_uploads_are_held(form, held):
    exts = (".gcode", ".g", ".gco", ".ufp", ".nc", ".3mf")
    assert helix_fitcheck.holds_upload(form, exts) is held


def test_limits_of_something_that_is_not_settings_are_unknown():
    limits = machine_limits(None)
    assert limits["axis_max"] == [None, None, None]
    assert limits["tool_count"] is None


@pytest.mark.parametrize("text,kind", [
    ("standard", "standard"), ("Standard", "standard"), (" STANDARD ", "standard"),
    ("high_flow", "high_flow"), ("High Flow", "high_flow"), ("HighFlow", "high_flow"),
    ("high-flow", "high_flow"), ("", ""), ("hardened", ""), ("Standard HF", ""), (None, ""),
    (3, ""),
])
def test_a_nozzles_flow_type_reads_the_same_in_every_spelling(text, kind):
    assert nozzle_volume_type(text) == kind


def test_live_nozzles_of_something_that_is_not_a_list_change_nothing():
    limits = machine_limits(SETTINGS)
    assert machine_with_live_nozzles(limits, None) == limits
    assert machine_with_live_nozzles(limits, {"extruder": {}}) == limits


# ============================================================================
# The record, and the decision
# ============================================================================


def test_file_facts_come_from_the_records_bounds_header_and_tools():
    facts = fit_file_from_record(_record(tools=(1, 0)))
    assert facts["travel"]["max"][0] == 200.0
    assert facts["print"]["min"][2] == 0.2
    assert facts["max_hotend_temp"] == 240.0
    assert facts["max_bed_temp"] == 60.0
    assert facts["tools"] == [0, 1]
    assert facts["nozzle_diameters"] == [0.4]
    assert facts["nozzle_volume_types"] == []
    assert facts["printer_model"] == "Voron 0.2"


@pytest.mark.parametrize("written,read", [
    (["Standard", "High Flow"], ["standard", "high_flow"]),
    (["Turbo", None], []),          # none this check knows: as if the file named none
    ("Standard", []),               # not a list
])
def test_file_facts_read_the_records_nozzle_flow_types(written, read):
    record = _record()
    record["header"]["nozzle_volume_types"] = written
    assert fit_file_from_record(record)["nozzle_volume_types"] == read


def test_a_real_analysis_record_feeds_the_checker(tmp_path):
    """The record helix_analysis writes is the shape fit_file_from_record reads."""
    from helix_analysis import analyze_file
    fixtures = Path(__file__).parent / "fixtures" / "analysis"
    gc = tmp_path / "gcodes"
    gc.mkdir()
    shutil.copy(str(fixtures / "no_define.gcode"), str(gc / "no_define.gcode"))
    record = analyze_file(gc, "no_define.gcode", {}).record
    facts = fit_file_from_record(record)
    travel_max_x = record["bounds"]["travel"]["max"][0]
    assert facts["travel"]["max"][0] == travel_max_x
    limits = machine_limits(dict(SETTINGS, stepper_x={"position_min": 0.0,
                                                      "position_max": travel_max_x - 1.0}))
    found = check_fit(limits, facts)
    assert [(f["kind"], f["detail"]["axis"]) for f in found] == [("out_of_bounds", "x")]
    limits["axis_max"][0] = travel_max_x
    assert check_fit(limits, facts) == []


def test_file_facts_of_a_record_without_bounds_check_nothing():
    facts = fit_file_from_record({"schema": 1})
    assert check_fit(machine_limits(SETTINGS), facts) == []


BLOCK = [{"kind": "out_of_bounds", "severity": "block", "detail": {}}]
WARN = [{"kind": "nozzle_mismatch", "severity": "warn", "detail": {}}]
NOTE = [{"kind": "printer_model_mismatch", "severity": "note", "detail": {}}]


@pytest.mark.parametrize("enabled,findings,starts", [
    (False, BLOCK, True),   # switched off: the slicer's start stands
    (True, None, True),     # could not be checked: started unchecked
    (True, [], True),
    (True, WARN, True),     # a warning never holds a start
    (True, NOTE, True),     # nor does a note
    (True, BLOCK, False),
    (True, WARN + BLOCK, False),
])
def test_may_autostart(enabled, findings, starts):
    assert may_autostart(enabled, findings) is starts


# ============================================================================
# The upload hook
# ============================================================================


class UploadFileManager(MockFileManager):
    """finalize_upload in Moonraker's shape: stores the file, and starts it
    when the form asks for a print."""

    def __init__(self, gcodes_path):
        super().__init__(gcodes_path)
        self.calls = []

    async def finalize_upload(self, form_args):
        self.calls.append(dict(form_args))
        path = form_args["filename"]
        return {"item": {"path": path, "root": "gcodes"}, "action": "create_file",
                "print_started": form_args.get("print") == "true", "print_queued": False}


@pytest.fixture
def component(mock_server, temp_gcodes_dir):
    mock_server.components["file_manager"] = UploadFileManager(temp_gcodes_dir)
    mock_server.components["database"] = MockDatabase()
    mock_server.components["klippy_connection"] = MockKlippy()
    mock_server.components["klippy_apis"] = MockKlippyApis()
    mock_server.components["history"] = MockHistory()
    apis = mock_server.components["klippy_apis"]
    apis.printer_state = "standby"
    apis.printing_file = ""
    apis.settings_error = None
    apis.extruders = {}  # what each extruder reports, by name
    apis.extruder_error = None

    async def query_objects(objects):
        out = {}
        if "configfile" in objects:
            if apis.settings_error is not None:
                raise apis.settings_error
            out["configfile"] = {"settings": SETTINGS}
        if "print_stats" in objects:
            out["print_stats"] = {"state": apis.printer_state, "filename": apis.printing_file}
        for name in objects:
            if name.startswith("extruder"):
                if apis.extruder_error is not None:
                    raise apis.extruder_error
                status = apis.extruders.get(name, {})
                # Klipper answers a field its extruder lacks with null.
                out[name] = {field: status.get(field) for field in objects[name]}
        return out
    apis.query_objects = AsyncMock(side_effect=query_objects)
    mock_server.send_event = MagicMock()
    return load_component(MockConfigHelper(mock_server, {
        "temp_dir": ".helix_temp", "symlink_dir": ".helix_print",
        "cleanup_delay": 3600, "enabled": True,
        # These cases pin the .gcode upload path; a .3mf upload goes to the converter's hook.
        "start_3mf_on_upload": False,
    }))


async def _setup(component, opt_in=True, record=None):
    await component.component_init()
    if opt_in is not None:
        await component.database.insert_item(HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY,
                                             {FIT_CHECK_FEATURE: opt_in})
    component.analysis.record_for = AsyncMock(return_value=record)


async def _upload(component, filename="part.gcode", start=True):
    form = {"root": "gcodes", "filename": filename, "tmp_file_path": "/tmp/x",
            "print": "true" if start else "false", "current_user": {"username": "orca"}}
    return await component.file_manager.finalize_upload(form)


class TestUploadHold:

    @pytest.mark.asyncio
    async def test_the_feature_is_off_until_opted_into(self, component):
        await _setup(component, opt_in=None)
        assert await component.feature_enabled(FIT_CHECK_FEATURE) is False
        status = await component._features_status()
        assert status[FIT_CHECK_FEATURE]["available"] is True
        assert status[FIT_CHECK_FEATURE]["enabled"] is False

    @pytest.mark.asyncio
    async def test_switched_off_the_upload_starts_as_moonraker_would(self, component):
        await _setup(component, opt_in=False, record=_record(x_max=400.0))
        result = await _upload(component)
        fm = component.file_manager
        assert fm.calls[-1]["print"] == "true"
        assert result["print_started"] is True
        component.analysis.record_for.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_a_blocking_finding_keeps_the_file_and_starts_nothing(self, component,
                                                                        mock_server):
        await _setup(component, record=_record(x_max=221.0))
        result = await _upload(component)
        stored = component.file_manager.calls[-1]
        assert "print" not in stored
        assert stored["filename"] == "part.gcode"
        assert result["print_started"] is False and result["print_queued"] is False
        component.klippy_apis.start_print.assert_not_awaited()
        event, payload = mock_server.send_event.call_args.args
        assert event == REFUSED_EVENT
        assert payload["filename"] == "part.gcode"
        assert [f["kind"] for f in payload["findings"]] == ["out_of_bounds"]

    @pytest.mark.asyncio
    async def test_a_file_that_fits_is_started(self, component):
        await _setup(component, record=_record(x_max=219.0))
        result = await _upload(component)
        assert "print" not in component.file_manager.calls[-1]
        component.klippy_apis.start_print.assert_awaited_once_with(
            "part.gcode", user={"username": "orca"})
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_a_hotend_over_max_temp_is_held(self, component):
        await _setup(component, record=_record(hotend=301.0))
        result = await _upload(component)
        assert result["print_started"] is False
        component.klippy_apis.start_print.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_high_flow_gcode_for_a_standard_nozzle_is_not_held(self, component):
        """A flow-type mismatch only warns: the printer's own filament mapping
        can put file tool 0 on another head, so it never holds a start."""
        record = _record()
        record["header"]["nozzle_volume_types"] = ["High Flow"]
        await _setup(component, record=record)
        component.klippy_apis.extruders = {
            "extruder": {"nozzle_diameter": 0.4, "nozzle_volume_type": "standard"}}
        result = await _upload(component)
        assert result["print_started"] is True
        component.klippy_apis.query_objects.assert_any_await(
            {"extruder": ["nozzle_diameter", "nozzle_volume_type"]})

    @pytest.mark.asyncio
    async def test_a_printer_reporting_no_nozzle_type_holds_nothing_for_it(self, component):
        """Stock Klipper: the query answers null, and the config's nozzle stands."""
        record = _record()
        record["header"]["nozzle_volume_types"] = ["High Flow"]
        await _setup(component, record=record)
        result = await _upload(component)
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_a_failed_extruder_query_still_checks_the_config(self, component, caplog):
        await _setup(component, record=_record(x_max=221.0))
        component.klippy_apis.extruder_error = RuntimeError("timeout")
        with caplog.at_level(logging.INFO):
            result = await _upload(component)
        assert result["print_started"] is False
        assert "the extruder query failed: timeout" in caplog.text

    @pytest.mark.asyncio
    async def test_extra_tools_do_not_hold_a_start(self, component):
        """The plugin cannot see a filament system's T commands."""
        await _setup(component, record=_record(tools=(0, 1, 2, 3)))
        result = await _upload(component)
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_a_file_without_an_analysis_starts_unchecked(self, component, caplog):
        await _setup(component, record=None)
        with caplog.at_level(logging.INFO):
            result = await _upload(component)
        assert "starting unchecked: no analysis within" in caplog.text
        assert result["print_started"] is True
        (path, wait), _ = component.analysis.record_for.await_args
        assert path == "part.gcode"
        # The deadline counts from the hook's entry, so storing the file used some of it.
        assert 40.0 < wait <= helix_fitcheck.RECORD_WAIT_S

    @pytest.mark.asyncio
    async def test_unreadable_printer_limits_start_unchecked(self, component, caplog):
        await _setup(component, record=_record(x_max=400.0))
        component.klippy_apis.settings_error = RuntimeError("klippy down")
        with caplog.at_level(logging.INFO):
            result = await _upload(component)
        assert result["print_started"] is True
        assert "starting unchecked: configfile query failed: klippy down" in caplog.text

    @pytest.mark.asyncio
    async def test_an_upload_without_a_print_passes_through(self, component):
        await _setup(component, record=_record(x_max=400.0))
        await _upload(component, start=False)
        await _upload(component, filename="part.3mf")
        assert component.file_manager.calls[0]["print"] == "false"
        assert component.file_manager.calls[1]["print"] == "true"
        component.analysis.record_for.assert_not_awaited()

    def test_the_hold_ends_before_a_proxy_gives_up(self):
        # nginx's default proxy_read_timeout is 60 s
        assert helix_fitcheck.RECORD_WAIT_S <= 45.0

    @pytest.mark.asyncio
    async def test_a_busy_printer_without_an_analysis_starts_or_queues_at_once(self, component):
        await _setup(component, record=None)
        component.klippy_apis.printer_state = "printing"
        component.klippy_apis.printing_file = "other.gcode"
        await _upload(component)
        component.analysis.record_for.assert_awaited_once_with("part.gcode", 0.0)
        component.klippy_apis.start_print.assert_awaited_once()

    @pytest.mark.asyncio
    async def test_a_busy_printer_checks_an_analysis_already_there(self, component,
                                                                   mock_server):
        job_queue = MagicMock(queued_jobs={})
        job_queue.queue_job = AsyncMock()
        mock_server.components["job_queue"] = job_queue
        mock_server.components["file_manager"].queue_gcodes = True
        await _setup(component, record=_record(x_max=400.0))
        component.klippy_apis.printer_state = "printing"
        component.klippy_apis.printing_file = "other.gcode"
        result = await _upload(component)
        component.analysis.record_for.assert_awaited_once_with("part.gcode", 0.0)
        component.klippy_apis.start_print.assert_not_awaited()
        job_queue.queue_job.assert_not_awaited()
        assert result["print_queued"] is False

    @pytest.mark.asyncio
    async def test_a_held_analysis_queue_starts_at_once(self, component):
        await _setup(component, record=_record(x_max=400.0))
        component.analysis.queue_held = lambda: True
        result = await _upload(component)
        component.analysis.record_for.assert_not_awaited()
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_a_file_already_printing_is_not_started_again(self, component):
        await _setup(component, record=_record())
        component.klippy_apis.printer_state = "printing"
        component.klippy_apis.printing_file = "part.gcode"
        result = await _upload(component)
        component.klippy_apis.start_print.assert_not_awaited()
        assert result["print_started"] is False and result["print_queued"] is False

    @pytest.mark.asyncio
    async def test_a_file_already_queued_is_not_started_or_queued_again(self, component,
                                                                       mock_server):
        job_queue = MagicMock(queued_jobs={"0001": SimpleNamespace(filename="part.gcode")})
        job_queue.queue_job = AsyncMock()
        mock_server.components["job_queue"] = job_queue
        mock_server.components["file_manager"].queue_gcodes = True
        await _setup(component, record=_record())
        result = await _upload(component)
        component.klippy_apis.start_print.assert_not_awaited()
        job_queue.queue_job.assert_not_awaited()
        assert result["print_started"] is False and result["print_queued"] is False

    @pytest.mark.asyncio
    @pytest.mark.parametrize("name", ["part.ufp", "part.nc", "part.GCO"])
    async def test_every_extension_moonraker_prints_is_held(self, component, name):
        await _setup(component, record=_record(x_max=400.0))
        result = await _upload(component, filename=name)
        assert "print" not in component.file_manager.calls[-1]
        component.analysis.record_for.assert_awaited_once()
        assert result["print_started"] is False

    @pytest.mark.asyncio
    async def test_a_check_that_raises_starts_unchecked(self, component, caplog):
        await _setup(component)
        component.analysis.record_for = AsyncMock(side_effect=RuntimeError("db gone"))
        with caplog.at_level(logging.INFO):
            result = await _upload(component)
        assert result["print_started"] is True
        assert "the check failed (RuntimeError('db gone'))" in caplog.text

    @pytest.mark.asyncio
    @pytest.mark.parametrize("state", ["complete", "cancelled", "standby", "error"])
    async def test_a_reupload_of_a_finished_print_starts(self, component, state):
        """Klipper keeps print_stats.filename after a print ends."""
        await _setup(component, record=_record())
        component.klippy_apis.printer_state = state
        component.klippy_apis.printing_file = "part.gcode"
        result = await _upload(component)
        component.klippy_apis.start_print.assert_awaited_once()
        assert result["print_started"] is True

    @pytest.mark.asyncio
    async def test_two_uploads_of_one_path_start_it_once(self, component):
        await _setup(component, record=_record())
        apis = component.klippy_apis

        async def start_print(path, user=None):
            await asyncio.sleep(0)
            apis.printer_state, apis.printing_file = "printing", path
        apis.start_print = AsyncMock(side_effect=start_print)
        first, second = await asyncio.gather(_upload(component), _upload(component))
        apis.start_print.assert_awaited_once()
        assert sorted([first["print_started"], second["print_started"]]) == [False, True]
        assert not first["print_queued"] and not second["print_queued"]

    @pytest.mark.asyncio
    async def test_the_hold_counts_from_the_hooks_entry(self, component, monkeypatch):
        monkeypatch.setattr(helix_fitcheck, "RECORD_WAIT_S", 1.0)
        await _setup(component, record=None)
        fm = component.file_manager
        real = fm.calls

        async def slow_finalize(form_args):
            real.append(dict(form_args))
            await asyncio.sleep(0.3)
            return {"item": {"path": form_args["filename"], "root": "gcodes"},
                    "action": "create_file", "print_started": False, "print_queued": False}
        component.fit_check._original_finalize = slow_finalize
        await _upload(component)
        (_path, wait), _ = component.analysis.record_for.await_args
        assert wait <= 0.75

    @pytest.mark.asyncio
    async def test_a_cancelled_check_never_starts(self, component):
        """A cancellation (Moonraker shutting down) is not a failed check that
        starts the print. CancelledError is a BaseException from Python 3.8 on,
        so the fail-open `except Exception` handlers let it through."""
        await _setup(component)
        component.analysis.record_for = AsyncMock(side_effect=asyncio.CancelledError())
        with pytest.raises(asyncio.CancelledError):
            await _upload(component)
        component.klippy_apis.start_print.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_close_puts_moonrakers_upload_back(self, component, mock_server):
        original = mock_server.components["file_manager"].finalize_upload
        await _setup(component)
        assert component.file_manager.finalize_upload != original
        await component.close()
        assert component.file_manager.finalize_upload == original

    @pytest.mark.asyncio
    async def test_the_converter_asks_before_starting(self, component):
        converter = MagicMock(active=True, start_gate=None)
        converter.close = AsyncMock()
        component.converter = converter
        component._start_3mf_converter = AsyncMock()
        await _setup(component, record=_record(x_max=400.0))
        assert converter.start_gate == component.fit_check.may_start
        assert converter.hold_wanted == component.fit_check.hold_wanted
        assert await converter.hold_wanted() is True
        assert await converter.start_gate("plate_1.gcode") is False
        component.analysis.record_for.return_value = _record()
        assert await converter.start_gate("plate_1.gcode") is True
        await component.close()
        assert converter.start_gate is None
        assert converter.hold_wanted is None

    @pytest.mark.asyncio
    async def test_unavailable_without_file_analysis(self, component, monkeypatch):
        import helix_print
        monkeypatch.setattr(helix_print, "helix_analysis", None)
        await component.component_init()
        status = await component._features_status()
        assert status[FIT_CHECK_FEATURE]["available"] is False
        assert status[FIT_CHECK_FEATURE]["reason"] == "needs_file_analysis"
