# SPDX-License-Identifier: GPL-3.0-or-later
"""
Filament accounting (helix_filament.py): the plugin switches Moonraker's
active spool at each tool or lane change for printers whose filament system
does not, and keeps a per-job record of what was charged to which spool.

Run with: pytest tests/test_helix_filament.py -v
"""

import asyncio
import json
from pathlib import Path
from typing import Any, List, Optional

import pytest

import sys
sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_filament
from helix_filament import (
    ACTION_CLEAR,
    ACTION_SET,
    ACTIVE_KEY,
    FILAMENT_FEATURE,
    FILAMENT_NAMESPACE,
    JOB_KEY_PREFIX,
    LAST_KEY,
    SPOOL_MAP_KEY,
    Feed,
    UsageLedger,
    active_spool_owner,
    feeding,
    parse_spool_map,
    signal_key,
    spool_switch,
)
from helix_print import HELIX_AUX_PROVIDER, HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockWebRequest,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)

CASES = json.loads(
    (Path(__file__).parents[2] / "tests" / "fixtures" / "filament_accounting" / "cases.json")
    .read_text())

T0 = 1700000000.25


class MockSpoolman:
    """Moonraker's spoolman component as this module uses it: `spool_id` and
    set_active_spool()."""

    def __init__(self, spool_id: Optional[int] = None) -> None:
        self.spool_id = spool_id
        self.calls: List[Optional[int]] = []

    def set_active_spool(self, spool_id: Optional[int]) -> None:
        self.calls.append(spool_id)
        self.spool_id = spool_id


def _feed(tool, spool, lane=None, gpm=2.98):
    return {"tool": tool, "lane": tool if lane is None else lane, "spool_id": spool,
            "grams_per_mm": gpm}


def _extruder_map(manages=False, macros=False, spools=(11, 12), printer=False):
    """A two-tool toolchanger: each tool its own extruder."""
    return {
        "version": 1,
        "manages_active_spool": manages,
        "printer_switches_active_spool": printer,
        "macros_set_active_spool": macros,
        "signals": [{
            "object": "toolhead", "path": ["extruder"],
            "values": {"extruder": _feed(0, spools[0]), "extruder1": _feed(1, spools[1])},
        }],
    }


def _status(extruder="extruder", e=0.0, state="printing", filename="part.gcode"):
    return {"toolhead": {"extruder": extruder, "position": [0, 0, 0, e]},
            "print_stats": {"state": state, "filename": filename}}


async def _history(mock_server, payload):
    await asyncio.gather(
        *(h(payload) for h in mock_server.event_handlers["history:history_changed"]))


def _added(job_id, filename="part.gcode", start_time=T0):
    return {"action": "added",
            "job": {"job_id": job_id, "filename": filename, "start_time": start_time}}


def _finished(job_id, status="completed", filename="part.gcode", end_time=T0 + 600,
              start_time=T0):
    return {"action": "finished",
            "job": {"job_id": job_id, "filename": filename, "status": status,
                    "start_time": start_time, "end_time": end_time}}


async def _update(mock_server, status):
    for h in mock_server.event_handlers["server:status_update"]:
        await h(status)


async def _ready(mock_server, status):
    mock_server.components["klippy_apis"].subscribe_objects.return_value = status
    mock_server.components["klippy_apis"].query_objects.return_value = {
        k: v for k, v in status.items() if k != "print_stats"}
    for h in mock_server.event_handlers["server:klippy_ready"]:
        await h()


async def _setup(component, mock_server, *, opt_in=None, spool_map=None, active=11,
                 status=None):
    spoolman = MockSpoolman(active)
    mock_server.components["spoolman"] = spoolman
    await component.component_init()
    db = component.database
    if opt_in is not None:
        await db.insert_item(HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY,
                             {FILAMENT_FEATURE: opt_in})
    if spool_map is not None:
        await db.insert_item(HELIX_SCREEN_NAMESPACE, SPOOL_MAP_KEY, spool_map)
    await _ready(mock_server, status or _status())
    return spoolman


# ============================================================================
# The shared rules: one case table, read by this suite and the C++ suite
# ============================================================================


@pytest.mark.parametrize("case", CASES["ownership"], ids=lambda c: c["name"])
def test_ownership_table(case):
    assert active_spool_owner(case["spoolman"], case["manages_active_spool"],
                              case["printer_switches_active_spool"],
                              case["macros_set_active_spool"]) == case["owner"]


@pytest.mark.parametrize("case", CASES["keys"], ids=lambda c: repr(c["value"]))
def test_signal_key_table(case):
    assert signal_key(case["value"]) == case["key"]


@pytest.mark.parametrize("case", CASES["switch"], ids=lambda c: c["name"])
def test_switch_table(case):
    feed = None if case["feed"] is None else Feed(spool_id=case["feed"]["spool_id"])
    expect = case["expect"]
    want = None
    if expect is not None:
        want = (ACTION_SET, expect["spool_id"]) if expect["action"] == "set" \
            else (ACTION_CLEAR, None)
    assert spool_switch(feed, case["active"], case["mapped"]) == want


# ============================================================================
# The record the screen writes
# ============================================================================


class TestSpoolMap:
    def test_absent_or_other_version_is_none(self):
        assert parse_spool_map(None) is None
        assert parse_spool_map({"version": 2, "signals": []}) is None
        assert parse_spool_map("x") is None

    def test_malformed_signals_are_dropped_alone(self):
        spool_map = parse_spool_map({"version": 1, "signals": [
            {"object": "toolhead", "path": [], "values": {}},
            {"object": "", "path": ["extruder"], "values": {}},
            {"object": "toolhead", "path": ["extruder"], "values": "x"},
            {"object": "save_variables", "path": ["variables", "active_tool"],
             "values": {"0": _feed(0, 5), "1": "not a feed"}},
        ]})
        assert len(spool_map.signals) == 1
        assert list(spool_map.signals[0].values) == ["0"]
        assert spool_map.manages_active_spool is False

    def test_first_mapped_signal_wins(self):
        spool_map = parse_spool_map({"version": 1, "signals": [
            {"object": "box", "path": ["T1", "filament"], "values": {"A": _feed(0, 5, lane=0)}},
            {"object": "box", "path": ["T2", "filament"], "values": {"B": _feed(1, 6, lane=5)}},
        ]})
        key, feed = feeding(spool_map, {"box": {"T1": {"filament": "None"},
                                                "T2": {"filament": "B"}}})
        assert key == "1:B"
        assert (feed.tool, feed.lane, feed.spool_id) == (1, 5, 6)
        key, feed = feeding(spool_map, {"box": {"T1": {"filament": "None"}}})
        assert key == "0:None" and feed is None

    def test_subscription_names_each_signal_field(self):
        spool_map = parse_spool_map({"version": 1, "signals": [
            {"object": "save_variables", "path": ["variables", "active_tool"], "values": {}},
            {"object": "box", "path": ["T1", "filament"], "values": {}},
            {"object": "box", "path": ["T2", "filament"], "values": {}},
        ]})
        assert spool_map.objects() == {"save_variables": ["variables"], "box": ["T1", "T2"]}


# ============================================================================
# The ledger charges what Moonraker charges
# ============================================================================


class TestUsageLedger:
    def test_retracts_are_not_charged_twice(self):
        ledger = UsageLedger()
        ledger.baseline(0.0, "extruder")
        ledger.attribute(0, 0, 11, 3.0)
        for e in (10.0, 8.0, 9.0, 12.0):
            ledger.position(e, None)
        assert ledger.spools() == [
            {"tool": 0, "lane": 0, "spool_id": 11, "mm": 12.0, "grams": 36.0}]

    def test_an_extruder_change_rebases_without_a_charge(self):
        ledger = UsageLedger()
        ledger.baseline(100.0, "extruder")
        ledger.attribute(0, 0, 11, None)
        ledger.position(105.0, None)
        ledger.attribute(1, 1, 12, None)
        ledger.position(0.0, "extruder1")
        ledger.position(7.5, None)
        assert [(s["tool"], s["spool_id"], s["mm"], s["grams"]) for s in ledger.spools()] == [
            (0, 11, 5.0, None), (1, 12, 7.5, None)]

    def test_a_spool_change_rebases_at_the_last_position(self):
        ledger = UsageLedger()
        ledger.baseline(0.0, "extruder")
        ledger.attribute(0, 0, 11, None)
        ledger.position(4.0, None)
        ledger.position(3.0, None)          # a retract: high-water mark stays 4
        ledger.attribute(0, 1, 12, None)    # the lane changed: re-based at 3
        ledger.position(6.0, None)
        assert [s["mm"] for s in ledger.spools()] == [4.0, 3.0]

    def test_state_round_trips(self):
        ledger = UsageLedger()
        ledger.baseline(0.0, "extruder")
        ledger.attribute(2, 7, 40, 2.5)
        ledger.position(2.0, None)
        again = UsageLedger.from_state(json.loads(json.dumps(ledger.to_state())))
        assert again.spools() == ledger.spools()
        assert again.label == (2, 7, 40)


# ============================================================================
# The module against Moonraker
# ============================================================================


class TestFilamentAccounting:
    @pytest.mark.asyncio
    async def test_feature_is_on_with_no_plugin_features_record(
            self, helix_print_component, mock_server):
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map())
        entry = (await helix_print_component._features_status())[FILAMENT_FEATURE]
        assert entry["available"] is True and entry["enabled"] is True
        assert entry["version"] == helix_filament.FILAMENT_FEATURE_VERSION

    @pytest.mark.asyncio
    async def test_without_moonraker_spoolman_it_is_unavailable_and_records_nothing(
            self, helix_print_component, mock_server):
        await helix_print_component.component_init()
        await _ready(mock_server, _status())
        entry = (await helix_print_component._features_status())[FILAMENT_FEATURE]
        assert entry["available"] is False and entry["enabled"] is False
        assert entry["reason"] == "no_spoolman"
        await _history(mock_server, _added("000009"))
        await _history(mock_server, _finished("000009"))
        assert await helix_print_component.database.get_item(
            FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000009") is None

    @pytest.mark.asyncio
    async def test_plugin_features_false_turns_it_off(self, helix_print_component, mock_server):
        await _setup(helix_print_component, mock_server, opt_in=False, spool_map=_extruder_map())
        entry = (await helix_print_component._features_status())[FILAMENT_FEATURE]
        assert entry["available"] is True and entry["enabled"] is False

    @pytest.mark.asyncio
    async def test_two_tool_print_switches_at_each_tool_change_only(
            self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, spool_map=_extruder_map(),
                                active=40)
        await _history(mock_server, _added("000001"))
        assert spoolman.calls == [11]          # the job's start asserts T0's spool
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 10.0]}})
        await _update(mock_server, {"toolhead": {"position": [5, 5, 0, 20.0]}})
        assert spoolman.calls == [11]
        await _update(mock_server, {"toolhead": {"extruder": "extruder1",
                                                 "position": [0, 0, 0, 0.0]}})
        assert spoolman.calls == [11, 12]
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 30.0]}})
        await _update(mock_server, {"toolhead": {"extruder": "extruder",
                                                 "position": [0, 0, 0, 20.0]}})
        assert spoolman.calls == [11, 12, 11]
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 25.0]}})
        assert spoolman.calls == [11, 12, 11]

        await _history(mock_server, _finished("000001"))
        record = await helix_print_component.database.get_item(
            FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000001")
        assert record["status"] == "completed"
        assert [(s["tool"], s["spool_id"], s["mm"]) for s in record["spools"]] == [
            (0, 11, 25.0), (1, 12, 30.0)]
        assert record["spools"][1]["grams"] == round(30.0 * 2.98, 2)
        assert await helix_print_component.database.get_item(
            FILAMENT_NAMESPACE, ACTIVE_KEY) is None

    @pytest.mark.asyncio
    @pytest.mark.parametrize("manages,macros,printer",
                             [(True, False, False), (False, True, False), (False, False, True)],
                             ids=["backend switches (AFC, Happy Hare push)", "macros switch",
                                  "firmware switches (U1 SpoolLink)"])
    async def test_a_printer_that_switches_itself_sees_no_call(
            self, helix_print_component, mock_server, manages, macros, printer):
        spoolman = await _setup(helix_print_component, mock_server,
                                spool_map=_extruder_map(manages, macros, printer=printer),
                                active=40)
        await _history(mock_server, _added("000002"))
        await _update(mock_server, {"toolhead": {"extruder": "extruder1",
                                                 "position": [0, 0, 0, 5.0]}})
        await _update(mock_server, {"toolhead": {"extruder": "extruder",
                                                 "position": [0, 0, 0, 5.0]}})
        assert spoolman.calls == []
        # Still recorded: what Moonraker charged, labelled with the feeding tool.
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 9.0]}})
        await _history(mock_server, _finished("000002"))
        record = await helix_print_component.database.get_item(
            FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000002")
        assert [(s["tool"], s["spool_id"], s["mm"]) for s in record["spools"]] == [(0, 40, 4.0)]

    @pytest.mark.asyncio
    async def test_opted_out_does_nothing(self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, opt_in=False,
                                spool_map=_extruder_map(), active=40)
        await _history(mock_server, _added("000003"))
        await _update(mock_server, {"toolhead": {"extruder": "extruder1",
                                                 "position": [0, 0, 0, 5.0]}})
        await _history(mock_server, _finished("000003"))
        assert spoolman.calls == []
        assert await helix_print_component.database.get_item(
            FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000003") is None

    @pytest.mark.asyncio
    async def test_no_record_no_spoolman_no_switch(self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, spool_map=None, active=40)
        await _history(mock_server, _added("000004"))
        await _update(mock_server, {"toolhead": {"extruder": "extruder1"}})
        assert spoolman.calls == []

    @pytest.mark.asyncio
    async def test_reconnect_mid_print_does_not_switch_to_a_stale_spool(
            self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, spool_map=_extruder_map(),
                                active=11)
        await _history(mock_server, _added("000005"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 10.0]}})
        for h in mock_server.event_handlers["server:klippy_disconnect"]:
            result = h()
            if asyncio.iscoroutine(result):
                await result
        # Klippy is back with extruder1 mounted, a value carried over the
        # outage: it only sets the baseline.
        await _ready(mock_server, _status("extruder1", e=50.0))
        assert spoolman.calls == []
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 52.0]}})
        # Changes seen on this connection switch.
        await _update(mock_server, {"toolhead": {"extruder": "extruder",
                                                 "position": [0, 0, 0, 52.0]}})
        await _update(mock_server, {"toolhead": {"extruder": "extruder1",
                                                 "position": [0, 0, 0, 52.0]}})
        assert spoolman.calls == [12]
        await _history(mock_server, _finished("000005"))
        record = await helix_print_component.database.get_item(
            FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000005")
        # Nothing charged across the outage: 10 before it, 2 after.
        assert sum(s["mm"] for s in record["spools"]) == 12.0

    @pytest.mark.asyncio
    async def test_an_unlinked_lane_clears_a_mapped_spool(self, helix_print_component,
                                                          mock_server):
        spoolman = await _setup(helix_print_component, mock_server,
                                spool_map=_extruder_map(spools=(11, None)), active=11)
        await _history(mock_server, _added("000006"))
        await _update(mock_server, {"toolhead": {"extruder": "extruder1"}})
        assert spoolman.calls == [None]

    @pytest.mark.asyncio
    async def test_save_variables_signal(self, helix_print_component, mock_server):
        indx = {"version": 1, "manages_active_spool": False, "macros_set_active_spool": False,
                "signals": [{"object": "save_variables", "path": ["variables", "active_tool"],
                             "values": {"0": _feed(0, 21), "1": _feed(1, 22)}}]}
        status = _status()
        status["save_variables"] = {"variables": {"active_tool": 0}}
        spoolman = await _setup(helix_print_component, mock_server, spool_map=indx, active=21,
                                status=status)
        await _history(mock_server, _added("000007"))
        assert spoolman.calls == []
        await _update(mock_server, {"save_variables": {"variables": {"active_tool": 1}}})
        assert spoolman.calls == [22]
        calls = mock_server.components["klippy_apis"].subscribe_objects.call_args_list
        assert any(c.args[0].get("save_variables") == ["variables"] for c in calls)

    @pytest.mark.asyncio
    async def test_no_switch_while_idle(self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, spool_map=_extruder_map(),
                                active=11, status=_status(state="standby"))
        await _update(mock_server, {"toolhead": {"extruder": "extruder1"}})
        assert spoolman.calls == []

    @pytest.mark.asyncio
    async def test_plr_resume_continues_the_interrupted_record(
            self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, spool_map=_extruder_map(),
                                active=11)
        await _history(mock_server, _added("000008"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 40.0]}})
        await _update(mock_server, {"toolhead": {"extruder": "extruder1",
                                                 "position": [0, 0, 0, 0.0]}})
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 3.0]}})
        assert spoolman.calls == [12]
        # The power is cut: no finish. The plugin starts the resumed part.
        helix_print_component.mark_helix_initiated("part.gcode", "plr_resume")
        await _ready(mock_server, _status("extruder1", e=0.0))
        await _history(mock_server, _added("000009", start_time=T0 + 9000))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 2.0]}})
        await _history(mock_server, _finished("000009"))
        db = helix_print_component.database
        resumed = await db.get_item(FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000009")
        assert resumed["continues"] == "000008"
        assert [(s["tool"], s["mm"]) for s in resumed["spools"]] == [(0, 40.0), (1, 5.0)]
        interrupted = await db.get_item(FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000008")
        assert interrupted["status"] is None
        assert [(s["tool"], s["mm"]) for s in interrupted["spools"]] == [(0, 40.0), (1, 3.0)]

    @pytest.mark.asyncio
    async def test_plr_resume_after_a_restart_reads_the_last_job(
            self, helix_print_component, mock_server):
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        db = helix_print_component.database
        ledger = UsageLedger()
        ledger.attribute(0, 0, 11, None)
        ledger.entries.append([0, 0, 11, 15.0, None])
        await db.insert_item(FILAMENT_NAMESPACE, LAST_KEY,
                             {"job_id": "00000A", "filename": "part.gcode", "continues": None,
                              "ledger": ledger.to_state()})
        # Moonraker restarted after the power cut: what this module had stored.
        await db.insert_item(FILAMENT_NAMESPACE, helix_filament.LAST_ADDED_KEY, "00000A")
        await helix_print_component.filament.load()
        helix_print_component.mark_helix_initiated("part.gcode", "plr_resume")
        await _history(mock_server, _added("00000B"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 1.0]}})
        await _history(mock_server, _finished("00000B"))
        record = await db.get_item(FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "00000B")
        assert record["continues"] == "00000A"
        assert [s["mm"] for s in record["spools"]] == [16.0]

    @pytest.mark.asyncio
    async def test_a_restart_mid_print_keeps_counting(self, helix_print_component, mock_server):
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        db = helix_print_component.database
        ledger = UsageLedger()
        ledger.attribute(0, 0, 11, 2.0)
        ledger.entries.append([0, 0, 11, 15.0, 2.0])
        await db.insert_item(FILAMENT_NAMESPACE, ACTIVE_KEY,
                             {"job_id": "00000C", "filename": "part.gcode", "continues": None,
                              "ledger": ledger.to_state()})
        await helix_print_component.filament.load()
        await _ready(mock_server, _status(e=100.0))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 104.0]}})
        await _history(mock_server, _finished("00000C"))
        record = await db.get_item(FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "00000C")
        assert record["spools"] == [
            {"tool": 0, "lane": 0, "spool_id": 11, "mm": 19.0, "grams": 38.0}]

    @pytest.mark.asyncio
    async def test_open_ledger_persists_on_a_cadence(self, helix_print_component, mock_server):
        now = [0.0]
        helix_filament_clock = lambda: now[0]  # noqa: E731
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        helix_print_component.filament.clock = helix_filament_clock
        await _history(mock_server, _added("00000D"))
        db = helix_print_component.database
        now[0] = 1.0
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 3.0]}})
        stored = await db.get_item(FILAMENT_NAMESPACE, ACTIVE_KEY)
        assert stored["ledger"]["entries"] == []
        now[0] = helix_filament.PERSIST_INTERVAL_S + 1.0
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 4.0]}})
        stored = await db.get_item(FILAMENT_NAMESPACE, ACTIVE_KEY)
        assert stored["ledger"]["entries"][0][3] == 4.0

    @pytest.mark.asyncio
    async def test_endpoint_serves_the_open_and_finished_record(
            self, helix_print_component, mock_server):
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        handler = mock_server.endpoints["/server/helix/filament_usage"]
        await _history(mock_server, _added("00000E"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 2.0]}})
        live = await handler(MockWebRequest({"job_id": "00000E"}))
        assert live["enabled"] is True
        assert live["record"]["status"] == "in_progress"
        assert live["record"]["spools"][0]["mm"] == 2.0
        await _history(mock_server, _finished("00000E"))
        done = await handler(MockWebRequest({"job_id": "00000E"}))
        assert done["record"]["status"] == "completed"
        none = await handler(MockWebRequest({"job_id": "FFFFFF"}))
        assert none["record"] is None
        with pytest.raises(Exception):
            await handler(MockWebRequest({}))

    @pytest.mark.asyncio
    async def test_records_are_pruned_to_the_newest(self, helix_print_component, mock_server,
                                                     monkeypatch):
        monkeypatch.setattr(helix_filament, "MAX_JOB_RECORDS", 2)
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        for i, job_id in enumerate(("000011", "000012", "000013")):
            await _history(mock_server, _added(job_id, start_time=T0 + i))
            await _history(mock_server, _finished(job_id, end_time=T0 + 100 * i))
        ns = mock_server.components["database"].namespaces[FILAMENT_NAMESPACE]
        assert sorted(k for k in ns if k.startswith(JOB_KEY_PREFIX)) == [
            JOB_KEY_PREFIX + "000012", JOB_KEY_PREFIX + "000013"]

class TestHistoryAuxiliaryData:
    @pytest.mark.asyncio
    async def test_photos_modifications_and_spools_coexist(
            self, helix_print_component, mock_server):
        """Every feature rewrites a finished history row through
        patch_history_job, each replacing only its own entry."""
        import helix_photos
        from helix_print import PrintInfo
        history = mock_server.components["history"]

        async def save_job(job, job_id=None):
            history.save_job_calls.append((job, job_id))
            row = history.jobs[history._key(job_id)]
            row["filename"] = job.filename
            row["auxiliary_data"] = job.auxiliary_data

        history.save_job = save_job
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        spoolman_entry = {"provider": "spoolman", "name": "spool_ids", "value": [11]}
        history.add_job("00002A", filename=".helix_print/x.gcode", start_time=T0,
                        auxiliary_data=[spoolman_entry])

        # The photos feature links its end photo, through its own path.
        photo_record = helix_photos.with_photo(
            helix_photos.new_record("00002A", ".helix_print/x.gcode", T0),
            helix_photos.KIND_END, ".helix_photos/00002A_end.jpg", 1, T0, "plugin")
        await helix_print_component.database.insert_item(
            helix_photos.PHOTOS_NAMESPACE, ["00002A"], photo_record)
        job_row = {"job_id": "00002A", "start_time": T0}
        await helix_print_component.photos._link_in_history(job_row)
        # This feature records the job's spools when history finishes it.
        await _history(mock_server, _added("00002A", filename=".helix_print/x.gcode"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 5.0]}})
        await _history(mock_server, _finished("00002A", filename=".helix_print/x.gcode"))
        # The linked print's rename adds its modifications entry.
        info = PrintInfo("benchy.gcode", ".helix_temp/x.gcode", ".helix_print/x.gcode",
                         ["skip"], 0.0)
        info.job_id = "00002A"
        await helix_print_component._patch_history_entry(info, "complete")
        # Each feature again: its own entry is replaced, never stacked.
        await helix_print_component.photos._link_in_history(job_row)

        aux = history.jobs[history._key("00002A")]["auxiliary_data"]
        names = [(e["provider"], e["name"]) for e in aux]
        assert sorted(names) == sorted([
            ("spoolman", "spool_ids"), (HELIX_AUX_PROVIDER, "photos"),
            (HELIX_AUX_PROVIDER, "filament_spools"), (HELIX_AUX_PROVIDER, "modifications")])
        spools = next(e for e in aux if e["name"] == "filament_spools")["value"]
        assert spools[0]["mm"] == 5.0 and spools[0]["spool_id"] == 11

    @pytest.mark.asyncio
    async def test_a_reused_job_id_is_not_written(self, helix_print_component, mock_server):
        history = mock_server.components["history"]
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        history.add_job("00002B", start_time=T0 + 999, auxiliary_data=[])
        await _history(mock_server, _added("00002B"))
        await _history(mock_server, _finished("00002B"))
        assert history.save_job_calls == []


class TestReviewFindings:
    @pytest.mark.asyncio
    async def test_a_map_written_after_ready_is_subscribed_at_job_start(
            self, helix_print_component, mock_server):
        indx = {"version": 1, "manages_active_spool": False,
                "printer_switches_active_spool": False, "macros_set_active_spool": False,
                "signals": [{"object": "save_variables", "path": ["variables", "active_tool"],
                             "values": {"0": _feed(0, 21), "1": _feed(1, 22)}}]}
        spoolman = await _setup(helix_print_component, mock_server, spool_map=None, active=21)
        apis = mock_server.components["klippy_apis"]
        # The screen writes the map once it sees the plugin, after Klippy is ready.
        await helix_print_component.database.insert_item(HELIX_SCREEN_NAMESPACE, SPOOL_MAP_KEY,
                                                         indx)
        apis.subscribe_objects.return_value = {"save_variables": {"variables": {"active_tool": 0}}}
        await _history(mock_server, _added("000020"))
        assert any(c.args[0].get("save_variables") == ["variables"]
                   for c in apis.subscribe_objects.call_args_list)
        await _update(mock_server, {"save_variables": {"variables": {"active_tool": 1}}})
        assert spoolman.calls == [22]

    @pytest.mark.asyncio
    async def test_a_relink_mid_print_counts_at_the_next_change(
            self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, spool_map=_extruder_map(),
                                active=11)
        await _history(mock_server, _added("000021"))
        db = helix_print_component.database
        # T1 runs out; the user links a new spool to it mid-print.
        await db.insert_item(HELIX_SCREEN_NAMESPACE, SPOOL_MAP_KEY,
                             _extruder_map(spools=(11, 45)))
        await _update(mock_server, {"toolhead": {"extruder": "extruder1"}})
        assert spoolman.calls == [45]
        await _update(mock_server, {"toolhead": {"extruder": "extruder"}})
        # Then unlinks it: the next change to T1 clears instead of setting 45.
        await db.insert_item(HELIX_SCREEN_NAMESPACE, SPOOL_MAP_KEY,
                             _extruder_map(spools=(11, None)))
        await _update(mock_server, {"toolhead": {"extruder": "extruder1"}})
        assert spoolman.calls == [45, 11, None]

    @pytest.mark.asyncio
    async def test_a_quick_change_and_back_ends_on_the_feeding_spool(
            self, helix_print_component, mock_server):
        spoolman = await _setup(helix_print_component, mock_server, spool_map=_extruder_map(),
                                active=11)
        await _history(mock_server, _added("000022"))
        gate = asyncio.Event()
        real_enabled = helix_print_component.feature_enabled

        async def slow_enabled(name):
            await gate.wait()
            return await real_enabled(name)

        helix_print_component.feature_enabled = slow_enabled
        to_t1 = asyncio.ensure_future(
            _update(mock_server, {"toolhead": {"extruder": "extruder1"}}))
        await asyncio.sleep(0)
        back_to_t0 = asyncio.ensure_future(
            _update(mock_server, {"toolhead": {"extruder": "extruder"}}))
        await asyncio.sleep(0)
        gate.set()
        await asyncio.gather(to_t1, back_to_t0)
        assert spoolman.spool_id == 11

    @pytest.mark.asyncio
    async def test_a_resume_does_not_continue_an_unrelated_job(
            self, helix_print_component, mock_server):
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        db = helix_print_component.database
        ledger = UsageLedger()
        ledger.entries.append([0, 0, 11, 15.0, None])
        await db.insert_item(FILAMENT_NAMESPACE, LAST_KEY,
                             {"job_id": "000009", "filename": "other.gcode", "continues": None,
                              "ledger": ledger.to_state()})
        helix_print_component.mark_helix_initiated("part.gcode", "plr_resume")
        await _history(mock_server, _added("000023"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 1.0]}})
        await _history(mock_server, _finished("000023"))
        record = await db.get_item(FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000023")
        assert record["continues"] is None
        assert [s["mm"] for s in record["spools"]] == [1.0]

    @pytest.mark.asyncio
    async def test_a_reused_job_id_does_not_answer_with_the_old_record(
            self, helix_print_component, mock_server):
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        handler = mock_server.endpoints["/server/helix/filament_usage"]
        await _history(mock_server, _added("000024"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 2.0]}})
        await _history(mock_server, _finished("000024"))
        same = await handler(MockWebRequest({"job_id": "000024", "start_time": T0}))
        assert same["record"]["spools"][0]["mm"] == 2.0
        reused = await handler(MockWebRequest({"job_id": "000024", "start_time": T0 + 5000}))
        assert reused["record"] is None


class TestQidiResume:
    @pytest.mark.asyncio
    async def test_a_qidi_resume_continues_the_interrupted_record(
            self, helix_print_component, mock_server):
        """QIDI's resume prints its .cache copy under an absolute path, with no
        original name on the marker; the interrupted job is the one history
        added before it."""
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        await _history(mock_server, _added("000030", filename="benchy.gcode"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 12.0]}})
        cache_name = "/home/mks/printer_data/gcodes/.cache/benchy.gcode"
        helix_print_component.mark_helix_initiated(cache_name, "plr_resume")
        await _ready(mock_server, _status(e=0.0, filename=cache_name))
        await _history(mock_server, _added("000031", filename=cache_name,
                                           start_time=T0 + 9000))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 3.0]}})
        await _history(mock_server, _finished("000031", filename=cache_name,
                                              start_time=T0 + 9000))
        record = await helix_print_component.database.get_item(
            FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000031")
        assert record["continues"] == "000030"
        assert [s["mm"] for s in record["spools"]] == [15.0]

    @pytest.mark.asyncio
    async def test_a_resume_after_an_uncounted_job_starts_fresh(
            self, helix_print_component, mock_server):
        """The interrupted job ran with the feature off: the last counted job
        is older and unrelated, so the resume starts its own record."""
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        db = helix_print_component.database
        await _history(mock_server, _added("000032", filename="old.gcode"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 7.0]}})
        await _history(mock_server, _finished("000032", filename="old.gcode"))
        await db.insert_item(HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY,
                             {FILAMENT_FEATURE: False})
        await _history(mock_server, _added("000033", filename="benchy.gcode"))
        await db.insert_item(HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY,
                             {FILAMENT_FEATURE: True})
        helix_print_component.mark_helix_initiated("benchy.gcode", "plr_resume")
        await _history(mock_server, _added("000034", filename="benchy.gcode",
                                           start_time=T0 + 9000))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 8.0]}})
        await _history(mock_server, _finished("000034", filename="benchy.gcode",
                                              start_time=T0 + 9000))
        record = await db.get_item(FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000034")
        assert record["continues"] is None
        assert [s["mm"] for s in record["spools"]] == [1.0]

    @pytest.mark.asyncio
    async def test_a_resume_of_a_job_this_module_never_saw_starts_fresh(
            self, helix_print_component, mock_server):
        """The interrupted job's start was missed (Moonraker restarting, plugin
        off): the last job seen is an older one, but PLR names the job it
        resumed, so the older record is not continued."""
        await _setup(helix_print_component, mock_server, spool_map=_extruder_map(), active=11)
        db = helix_print_component.database
        await _history(mock_server, _added("000040", filename="benchy.gcode"))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 9.0]}})
        # Job 000041 printed while this module was not running; PLR snapshotted it.
        helix_print_component.plr.job = {"job_id": "000041", "filename": "benchy.gcode"}
        helix_print_component.mark_helix_initiated("benchy.gcode", "plr_resume")
        await _history(mock_server, _added("000042", filename="benchy.gcode",
                                           start_time=T0 + 9000))
        await _update(mock_server, {"toolhead": {"position": [0, 0, 0, 12.0]}})
        await _history(mock_server, _finished("000042", filename="benchy.gcode",
                                              start_time=T0 + 9000))
        record = await db.get_item(FILAMENT_NAMESPACE, JOB_KEY_PREFIX + "000042")
        assert record["continues"] is None
        assert [s["mm"] for s in record["spools"]] == [3.0]
