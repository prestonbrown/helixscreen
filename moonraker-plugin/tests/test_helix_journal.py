# SPDX-License-Identifier: GPL-3.0-or-later
"""
The printer event journal (helix_journal.py): each event kind recorded once
with the active job, the count and age bounds, the endpoint, and the store
surviving a plugin restart.

Run with: pytest tests/test_helix_journal.py -v
"""

import json
import sys
from pathlib import Path

import pytest
import pytest_asyncio

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_journal
from helix_journal import (
    JOURNAL_EVENT,
    JOURNAL_FEATURE,
    JOURNAL_NAMESPACE,
    JournalStore,
    classify_error,
    entry_key,
    is_mcu_disconnect,
    pause_cause,
    runout_sensors,
    runout_trips,
)
from helix_print import HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, load_component
from test_helix_print import (  # noqa: F401  (fixtures are used by name)
    MockConfigHelper,
    MockDatabase,
    MockFileManager,
    MockHistory,
    MockKlippyApis,
    Moonraker093Database,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)

FIXTURE = (
    Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "journal"
    / "error_line_classes.json"
)
# Shared with tests/unit/test_printer_event_journal.cpp
REPEATS = FIXTURE.parent / "repeats.json"
REPEAT_CASES = json.loads(REPEATS.read_text())["cases"]

T0 = 1700000000.0


class Clock:
    def __init__(self, now=T0):
        self.now = now

    def __call__(self):
        return self.now


class Request:
    def __init__(self, **params):
        self.params = params

    def get_int(self, key, default=None):
        return int(self.params.get(key, default))


class KlippyState:
    """Moonraker's KlippyState enum member as klippy_started passes it."""

    def __init__(self, name, message=""):
        self.name = name.upper()
        self.message = message


def _added(job_id="00000A", filename="benchy.gcode"):
    return {"action": "added", "job": {"job_id": job_id, "filename": filename,
                                       "status": "in_progress"}}


def _finished(status, job_id="00000A", filename="benchy.gcode"):
    return {"action": "finished", "job": {"job_id": job_id, "filename": filename,
                                          "status": status}}


@pytest_asyncio.fixture
async def journal(helix_print_component, mock_server):
    sent = []
    mock_server.send_event = lambda event, *args: sent.append((event, args))
    await helix_print_component.component_init()
    j = helix_print_component.journal
    j.clock = Clock()
    j.sent = sent
    return j


def _kinds(j):
    return [e["kind"] for e in j.store.entries]


# ============================================================================
# Classification, pinned with the app by one fixture
# ============================================================================

class TestClassifier:

    def test_matches_the_shared_fixture(self):
        cases = json.loads(FIXTURE.read_text())["cases"]
        assert len(cases) > 20
        for case in cases:
            assert classify_error(case["line"]) == case["class"], case["line"]
            assert is_mcu_disconnect(case["line"]) == case["mcu_disconnect"], case["line"]

    def test_pause_cause_prefers_the_most_specific_recent_event(self):
        now = T0 + 100
        runout = {"kind": "runout", "time": now - 3}
        error = {"kind": "gcode_error", "time": now - 3}
        stale = {"kind": "runout", "time": now - 60}
        assert pause_cause([error, runout], [], now) == "runout"
        assert pause_cause([error], [], now) == "error"
        assert pause_cause([stale], [(now - 2, "pause")], now) == "command"
        assert pause_cause([stale], [(now - 60, "PAUSE")], now) == "unknown"
        assert pause_cause([], [(now - 1, "PAUSE_AT_LAYER LAYER=3")], now) == "unknown"

    def test_runout_trips_only_on_present_to_absent_while_enabled(self):
        sensors = {}
        name = "filament_switch_sensor runout"
        assert runout_trips(sensors, {name: {"filament_detected": True, "enabled": True}}) == []
        assert runout_trips(sensors, {name: {"filament_detected": False}}) == [name]
        assert runout_trips(sensors, {name: {"filament_detected": False}}) == []
        runout_trips(sensors, {name: {"filament_detected": True, "enabled": False}})
        assert runout_trips(sensors, {name: {"filament_detected": False}}) == []
        assert runout_trips(sensors, {"toolhead": {"filament_detected": False}}) == []

    def test_runout_sensors_are_the_config_defined_ones_that_pause(self):
        objects = [
            "toolhead",
            "filament_switch_sensor Runout",            # in printer.cfg, default pause
            "filament_motion_sensor encoder",           # in printer.cfg, pause on
            "filament_switch_sensor door",              # in printer.cfg, pause off
            "filament_switch_sensor spool",             # in printer.cfg, M600 instead
            "filament_switch_sensor filament_tension",  # Happy Hare's, runtime-made
        ]
        settings = {
            "filament_switch_sensor runout": {"switch_pin": "PA1"},
            "filament_motion_sensor encoder": {"pause_on_runout": True},
            "filament_switch_sensor door": {"pause_on_runout": False, "runout_gcode": "  "},
            "filament_switch_sensor spool": {"pause_on_runout": False, "runout_gcode": "M600"},
        }
        assert runout_sensors(objects, settings) == [
            "filament_switch_sensor Runout", "filament_motion_sensor encoder",
            "filament_switch_sensor spool"]
        assert runout_sensors(objects, None) == []


# ============================================================================
# Store bounds
# ============================================================================

class TestStore:

    def test_count_bound_evicts_the_oldest(self):
        store = JournalStore(max_entries=3, max_age_s=1e9)
        evicted = []
        for i in range(5):
            evicted += store.append("klippy_ready", T0 + i)[1]
        assert [e["id"] for e in store.entries] == [3, 4, 5]
        assert evicted == [1, 2]

    def test_age_bound_evicts_the_oldest(self):
        store = JournalStore(max_entries=100, max_age_s=60)
        store.append("klippy_ready", T0)
        store.append("klippy_ready", T0 + 30)
        _, evicted = store.append("klippy_ready", T0 + 70)
        assert evicted == [1]
        assert [e["time"] for e in store.entries] == [T0 + 30, T0 + 70]

    def test_query_returns_the_newest_after_since_oldest_first(self):
        store = JournalStore()
        for i in range(5):
            store.append("klippy_ready", T0 + i)
        result = store.query(since=1, limit=2)
        assert [e["id"] for e in result["entries"]] == [4, 5]
        assert result["truncated"] is True
        assert result["latest_id"] == 5
        assert store.query(since=5)["entries"] == []

    def test_ids_keep_growing_after_eviction(self):
        store = JournalStore(max_entries=1)
        store.append("klippy_ready", T0)
        entry, _ = store.append("klippy_ready", T0 + 1)
        assert entry["id"] == 2


# ============================================================================
# Each event kind, recorded once with the job attached
# ============================================================================

class TestRecording:

    @pytest.mark.asyncio
    async def test_klippy_shutdown_records_state_message_and_job(self, journal):
        await journal.on_history_changed(_added())
        journal.helix.klippy_apis.query_objects.return_value = {
            "webhooks": {"state_message": "MCU 'mcu' shutdown: Timer too close\n"}}

        await journal.on_klippy_shutdown()

        assert _kinds(journal) == ["klippy_shutdown"]
        entry = journal.store.entries[0]
        assert entry["text"] == "MCU 'mcu' shutdown: Timer too close"
        assert entry["class"] == "mcu"
        assert entry["job"] == {"job_id": "00000A", "filename": "benchy.gcode"}
        assert entry["time"] == T0

    @pytest.mark.asyncio
    async def test_lost_mcu_shutdown_is_an_mcu_disconnect(self, journal):
        journal.helix.klippy_apis.query_objects.return_value = {
            "webhooks": {"state_message": "Lost communication with MCU 'mcu'"}}

        await journal.on_klippy_shutdown()

        assert _kinds(journal) == ["mcu_disconnect"]
        assert journal.store.entries[0]["job"] is None

    @pytest.mark.asyncio
    async def test_startup_error_for_an_unreachable_mcu(self, journal):
        await journal.on_klippy_started(KlippyState("error", "mcu 'mcu': Unable to connect"))
        await journal.on_klippy_started(KlippyState("error", "Option 'foo' is not valid"))
        await journal.on_klippy_started(KlippyState("ready"))

        assert _kinds(journal) == ["mcu_disconnect"]

    @pytest.mark.asyncio
    async def test_disconnect_and_ready(self, journal):
        await journal.on_klippy_disconnect()
        await journal.on_klippy_ready()

        assert _kinds(journal) == ["klippy_disconnect", "klippy_ready"]

    @pytest.mark.asyncio
    async def test_firmware_restart_from_a_client_command(self, journal):
        await journal.on_gcode_received("G28")
        await journal.on_gcode_received("firmware_restart")

        assert _kinds(journal) == ["firmware_restart"]

    @pytest.mark.asyncio
    async def test_bang_lines_are_classified_and_others_ignored(self, journal):
        await journal.on_history_changed(_added())
        await journal.on_gcode_response("// Klipper state: Ready")
        await journal.on_gcode_response("ok")
        await journal.on_gcode_response("!! Must home axis first: 0 0 0")

        assert _kinds(journal) == ["gcode_error"]
        entry = journal.store.entries[0]
        assert entry["text"] == "Must home axis first: 0 0 0"
        assert entry["class"] == "homing"
        assert entry["job"]["job_id"] == "00000A"

    @pytest.mark.asyncio
    async def test_pause_records_its_cause(self, journal):
        await journal.on_history_changed(_added())
        await journal.on_gcode_received("PAUSE")
        await journal.on_job_state_changed(
            None, {"state": "printing"}, {"state": "paused"})
        await journal.on_job_state_changed(
            None, {"state": "paused"}, {"state": "paused"})

        assert _kinds(journal) == ["pause"]
        assert journal.store.entries[0]["detail"] == "command"
        assert journal.store.entries[0]["job"]["filename"] == "benchy.gcode"

    @pytest.mark.asyncio
    async def test_runout_trip_during_a_job(self, journal):
        apis = journal.helix.klippy_apis
        apis.get_object_list.return_value = [
            "toolhead", "filament_motion_sensor encoder", "filament_switch_sensor filament_tension"]
        apis.query_objects.return_value = {"configfile": {"settings": {
            "filament_motion_sensor encoder": {"pause_on_runout": True}}}}
        apis.subscribe_objects.return_value = {
            "filament_motion_sensor encoder": {"filament_detected": True, "enabled": True}}
        await journal.on_klippy_ready()
        subscribed = apis.subscribe_objects.call_args[0][0]
        assert subscribed == {"filament_motion_sensor encoder": ["filament_detected", "enabled"]}

        update = {"filament_motion_sensor encoder": {"filament_detected": False}}
        await journal.on_status_update(update)  # idle: nothing printing
        await journal.on_status_update({"filament_motion_sensor encoder":
                                        {"filament_detected": True}})
        await journal.on_history_changed(_added())
        await journal.on_status_update(update)

        assert _kinds(journal) == ["klippy_ready", "runout"]
        assert journal.store.entries[1]["detail"] == "encoder"
        assert journal.store.entries[1]["job"]["job_id"] == "00000A"

    @pytest.mark.asyncio
    async def test_job_end_carries_status_and_clears_the_job(self, journal):
        await journal.on_history_changed(_added())
        await journal.on_history_changed(_finished("cancelled"))
        await journal.on_gcode_response("!! Move out of range: 1 2 3")

        assert _kinds(journal) == ["job_end", "gcode_error"]
        assert journal.store.entries[0]["detail"] == "cancelled"
        assert journal.store.entries[0]["job"] == {"job_id": "00000A",
                                                   "filename": "benchy.gcode"}
        assert journal.store.entries[1]["job"] is None

    @pytest.mark.asyncio
    async def test_every_append_is_persisted_and_broadcast(self, journal):
        await journal.on_klippy_ready()

        entry = journal.store.entries[0]
        stored = journal.helix.database.namespaces[JOURNAL_NAMESPACE]
        assert stored == {entry_key(entry["id"]): entry}
        assert journal.sent == [(JOURNAL_EVENT, (entry,))]

    @pytest.mark.asyncio
    async def test_has_no_off_switch(self, journal, mock_server):
        await journal.helix.database.insert_item(
            HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, {JOURNAL_FEATURE: False})

        await journal.on_klippy_ready()

        assert _kinds(journal) == ["klippy_ready"]
        from test_helix_print import MockWebRequest
        status = await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))
        assert status["features"][JOURNAL_FEATURE]["enabled"] is True


# ============================================================================
# Bounds against the database, endpoint, restart
# ============================================================================

class TestErrorFloods:

    def test_the_fixture_set_is_there(self):
        # An empty case list would pass the parametrized test by running none.
        assert len(REPEAT_CASES) >= 7

    @pytest.mark.parametrize("case", REPEAT_CASES, ids=[c["name"] for c in REPEAT_CASES])
    def test_the_shared_repeat_cases(self, case):
        store = JournalStore()
        for a in case["appends"]:
            store.append(a["kind"], T0 + a["time"], a["text"], a["job"], a["class"],
                         a.get("detail"))
        got = []
        for e in store.entries:
            row = {"time": e["time"] - T0, "text": e["text"], "count": e.get("count", 1)}
            if "last_time" in e:
                row["last_time"] = e["last_time"] - T0
            got.append(row)
        assert got == case["expected"]

    def test_repeats_within_the_window_collapse_into_one_entry(self):
        store = JournalStore(max_entries=5)
        store.append("klippy_shutdown", T0, "Lost communication with MCU")
        for i in range(1000):
            store.append("gcode_error", T0 + 1 + i * 0.01, "Move out of range", cls="move")
        assert [e["kind"] for e in store.entries] == ["klippy_shutdown", "gcode_error"]
        flood = store.entries[1]
        assert flood["count"] == 1000
        assert flood["time"] == T0 + 1
        assert flood["last_time"] == pytest.approx(T0 + 1 + 999 * 0.01)

    def test_alternating_errors_each_keep_one_entry(self):
        store = JournalStore()
        for i in range(50):
            store.append("gcode_error", T0 + i, "A")
            store.append("gcode_error", T0 + i, "B")
        assert [(e["text"], e["count"]) for e in store.entries] == [("A", 50), ("B", 50)]

    def test_a_repeat_after_the_window_is_a_new_entry(self):
        store = JournalStore()
        store.append("gcode_error", T0, "A")
        store.append("gcode_error", T0 + helix_journal.REPEAT_WINDOW_S - 1, "A")
        store.append("gcode_error", T0 + 2 * helix_journal.REPEAT_WINDOW_S, "A")
        assert [e.get("count", 1) for e in store.entries] == [2, 1]

    def test_another_job_or_kind_is_a_new_entry(self):
        store = JournalStore()
        store.append("gcode_error", T0, "A", job={"job_id": "1"})
        store.append("gcode_error", T0, "A", job={"job_id": "2"})
        store.append("pause", T0, "A")
        store.append("pause", T0, "A")
        assert len(store.entries) == 4

    @pytest.mark.asyncio
    async def test_a_flood_is_stored_and_broadcast_once_and_its_count_reaches_the_db(
            self, journal):
        db = journal.helix.database
        writes = []
        real_insert = db.insert_item

        async def counting_insert(namespace, key, value):
            if namespace == JOURNAL_NAMESPACE:
                writes.append(dict(value))
            await real_insert(namespace, key, value)

        db.insert_item = counting_insert
        for i in range(200):
            journal.clock.now = T0 + i * 0.01
            await journal.on_gcode_response("!! Move out of range: 1 2 3")
        assert len(journal.store.entries) == 1
        assert journal.store.entries[0]["count"] == 200
        assert len(writes) <= 2
        assert len(journal.sent) == 1

        journal.clock.now = T0 + 5
        await journal.on_klippy_ready()
        flood_id = journal.store.entries[0]["id"]
        assert [w.get("count") for w in writes if w["id"] == flood_id][-1] == 200


class TestPersistence:

    @pytest.mark.asyncio
    async def test_evicted_entries_leave_the_database(self, journal):
        journal.store.max_entries = 2
        for _ in range(3):
            await journal.on_klippy_ready()

        assert sorted(journal.helix.database.namespaces[JOURNAL_NAMESPACE]) == [
            entry_key(2), entry_key(3)]

    @pytest.mark.asyncio
    async def test_endpoint_reads_since_and_limit(self, journal, mock_server):
        for _ in range(4):
            await journal.on_klippy_ready()
        handler = mock_server.endpoints["/server/helix/journal"]

        result = await handler(Request(since=1, limit=2))

        assert [e["id"] for e in result["entries"]] == [3, 4]
        assert result["truncated"] is True
        assert result["enabled"] is True

    @pytest.mark.asyncio
    @pytest.mark.parametrize("db_class", [MockDatabase, Moonraker093Database])
    async def test_survives_a_plugin_restart(self, mock_server, temp_gcodes_dir, db_class):
        db = db_class()
        mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
        mock_server.components["database"] = db
        mock_server.components["klippy_apis"] = MockKlippyApis()
        mock_server.components["history"] = MockHistory()
        config = MockConfigHelper(mock_server, {"cleanup_delay": 3600, "enabled": True})

        first = load_component(config)
        await first.component_init()
        first.journal.clock = Clock(helix_journal.time.time())
        await first.journal.on_klippy_disconnect()
        await first.journal.on_klippy_ready()
        # An entry past the age bound and one that is not an entry at all
        await db.insert_item(JOURNAL_NAMESPACE, [entry_key(0)], {
            "id": 0, "time": 1.0, "kind": "klippy_ready", "text": ""})
        await db.insert_item(JOURNAL_NAMESPACE, ["junk"], {"kind": "nonsense"})

        second = load_component(config)
        await second.component_init()

        assert [e["kind"] for e in second.journal.store.entries] == [
            "klippy_disconnect", "klippy_ready"]
        await second.journal.on_klippy_ready()
        assert second.journal.store.entries[-1]["id"] == 3
        remaining = (db.namespaces if hasattr(db, "namespaces") else db.ns)[JOURNAL_NAMESPACE]
        assert sorted(remaining) == [entry_key(1), entry_key(2), entry_key(3)]

    @pytest.mark.asyncio
    async def test_feature_reports_in_status(self, journal, mock_server):
        from test_helix_print import MockWebRequest
        status = await mock_server.endpoints["/server/helix/status"](MockWebRequest({}))

        assert status["features"][JOURNAL_FEATURE] == {
            "version": 1, "available": True, "enabled": True, "reason": None}

    @pytest.mark.asyncio
    async def test_a_restart_mid_print_picks_up_the_open_job(self, helix_print_component):
        history = helix_print_component.server.components["history"]

        class Job:
            filename = "benchy.gcode"
            status = "in_progress"

        history.current_job = Job()
        history.current_job_id = "00000B"
        await helix_print_component.component_init()

        assert helix_print_component.journal.job == {"job_id": "00000B",
                                                     "filename": "benchy.gcode"}
