# SPDX-License-Identifier: GPL-3.0-or-later
"""
The on-load rename of legacy marker blocks to the hidden signal macro names
(helix_signal_migration.py, #2006).

Run with: pytest tests/test_helix_signal_migration.py -v
"""

import asyncio
import logging
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_print  # noqa: E402  (loads the siblings by name)
import helix_signal_migration as migration  # noqa: E402
from helix_print import SIBLING_LOAD_ERRORS, load_component  # noqa: E402
from test_helix_print import (  # noqa: E402,F401  (fixtures are used by name)
    MockConfigHelper,
    MockDatabase,
    MockHistory,
    MockKlippyApis,
    MockServer,
    MockWebRequest,
    helix_print_component,
    mock_server,
    temp_gcodes_dir,
)

BEGIN = "# <<< HELIX_TRACKING v2 >>>"
END = "# <<< /HELIX_TRACKING >>>"

INSTRUMENTED = (
    "[gcode_macro PRINT_START]\n"
    "gcode:\n"
    "    G28\n"
    f"    {BEGIN}\n"
    "    HELIX_PHASE_HOMING\n"
    f"    {END}\n"
    "    M109 S{EXTRUDER_TEMP}\n"
    f"    {BEGIN}\n"
    "    HELIX_READY\n"
    f"    {END}\n"
)
RENAMED = INSTRUMENTED.replace("    HELIX_", "    _HELIX_")

NEEDED = ("_HELIX_PHASE_HOMING", "_HELIX_READY")


def settings_with(*names):
    return {f"gcode_macro {name.lower()}": {"gcode": "\nRESPOND"} for name in names}


class ConfigFileManager:
    def __init__(self, config_dir):
        self.config_dir = str(config_dir)

    def get_directory(self, name):
        return self.config_dir if name == "config" else ""


class FakeHelix:
    """The slice of HelixPrint the migration uses."""

    def __init__(self, config_dir, settings=None):
        self.server = MockServer()
        self.klippy_apis = MockKlippyApis()
        self.klippy_apis.query_objects.return_value = {
            "configfile": {"settings": settings if settings is not None else {}}
        }
        self.database = MockDatabase()
        self.file_manager = ConfigFileManager(config_dir)
        self.features_changed = 0

    def notify_features_changed(self):
        self.features_changed += 1


@pytest.fixture
def config_dir(tmp_path):
    path = tmp_path / "config"
    path.mkdir()
    (path / "printer.cfg").write_text(INSTRUMENTED)
    return path


@pytest.fixture(autouse=True)
def not_firmware_managed(monkeypatch):
    monkeypatch.setattr(migration, "firmware_managed", lambda root="": False)


def backups(config_dir):
    return sorted(config_dir.glob("printer.cfg.bak.*"))


def make(config_dir, settings=None, enabled=True):
    helix = FakeHelix(config_dir, settings)
    return helix, migration.HelixSignalMigration(helix, enabled)


# ============================================================================
# The gate
# ============================================================================


class TestMissingHiddenNames:
    def test_lowercased_klipper_keys_match_uppercased_names(self):
        settings = settings_with("_HELIX_READY", "_HELIX_PHASE_HOMING")

        assert migration.missing_hidden_names(settings, NEEDED) == []

    def test_missing_names_are_listed_sorted(self):
        settings = settings_with("_HELIX_READY")

        assert migration.missing_hidden_names(
            settings, ["_HELIX_READY", "_HELIX_PHASE_QGL", "_HELIX_PHASE_HOMING"]
        ) == ["_HELIX_PHASE_HOMING", "_HELIX_PHASE_QGL"]

    def test_the_visible_name_does_not_stand_in_for_the_hidden_one(self):
        settings = settings_with("HELIX_READY")

        assert migration.missing_hidden_names(settings, ["_HELIX_READY"]) == ["_HELIX_READY"]

    def test_unreadable_settings_miss_every_name(self):
        assert migration.missing_hidden_names(None, NEEDED) == sorted(NEEDED)


# ============================================================================
# run_once
# ============================================================================


class TestRunOnce:
    @pytest.mark.asyncio
    async def test_waits_while_klipper_lacks_the_hidden_names(self, config_dir):
        _, mig = make(config_dir, settings_with("HELIX_READY", "HELIX_PHASE_HOMING"))

        report = await mig.run_once()

        assert report["state"] == "waiting_for_pack"
        assert report["missing"] == sorted(NEEDED)
        assert (config_dir / "printer.cfg").read_text() == INSTRUMENTED
        assert backups(config_dir) == []

    @pytest.mark.asyncio
    async def test_a_failed_configfile_query_waits(self, config_dir):
        helix, mig = make(config_dir)
        helix.klippy_apis.query_objects.side_effect = RuntimeError("klippy down")

        report = await mig.run_once()

        assert report["state"] == "waiting_for_pack"
        assert (config_dir / "printer.cfg").read_text() == INSTRUMENTED

    @pytest.mark.asyncio
    async def test_switched_off_writes_nothing_and_logs_would_rename(self, config_dir, caplog):
        _, mig = make(config_dir, settings_with(*NEEDED), enabled=False)

        with caplog.at_level(logging.INFO):
            report = await mig.run_once()

        assert report["state"] == "disabled"
        assert report["planned"] == [{"path": "printer.cfg", "blocks": 2}]
        assert (config_dir / "printer.cfg").read_text() == INSTRUMENTED
        assert backups(config_dir) == []
        assert ("HelixPrint: signal macro migration: would rename 2 call(s) in 1 file(s): "
                "printer.cfg (2)") in caplog.messages

    @pytest.mark.asyncio
    async def test_firmware_managed_reports_only(self, config_dir, monkeypatch, caplog):
        monkeypatch.setattr(migration, "firmware_managed", lambda root="": True)
        _, mig = make(config_dir, settings_with(*NEEDED))

        with caplog.at_level(logging.INFO):
            report = await mig.run_once()

        assert report["state"] == "firmware_managed"
        assert (config_dir / "printer.cfg").read_text() == INSTRUMENTED
        assert backups(config_dir) == []
        assert any("would rename 2 call(s)" in m for m in caplog.messages)

    @pytest.mark.asyncio
    async def test_renames_once_every_name_is_loaded(self, config_dir, caplog):
        helix, mig = make(config_dir, settings_with(*NEEDED))

        with caplog.at_level(logging.INFO):
            report = await mig.run_once()

        assert report["state"] == "done"
        assert (config_dir / "printer.cfg").read_text() == RENAMED
        [backup] = backups(config_dir)
        assert backup.read_text() == INSTRUMENTED
        assert report["edited"] == [{"path": "printer.cfg", "backup": str(backup), "blocks": 2}]
        assert report["missing"] == []
        assert isinstance(report["at"], float)
        assert helix.database.namespaces[migration.DB_NAMESPACE][migration.DB_KEY] == report
        assert helix.features_changed == 1
        assert ("HelixPrint: signal macro migration: renaming 2 call(s) in 1 file(s): "
                "printer.cfg (2)") in caplog.messages

    @pytest.mark.asyncio
    async def test_a_second_run_finds_nothing_and_keeps_the_done_report(self, config_dir):
        helix, mig = make(config_dir, settings_with(*NEEDED))
        done = await mig.run_once()

        second = await mig.run_once()

        assert second["state"] == "nothing"
        assert len(backups(config_dir)) == 1
        assert mig.report() == done
        assert helix.database.namespaces[migration.DB_NAMESPACE][migration.DB_KEY] == done

    @pytest.mark.asyncio
    async def test_a_tree_with_nothing_to_rename_is_nothing(self, tmp_path):
        (tmp_path / "printer.cfg").write_text("[gcode_macro PRINT_START]\ngcode:\n    G28\n")
        helix, mig = make(tmp_path)

        report = await mig.run_once()

        assert report["state"] == "nothing"
        helix.klippy_apis.query_objects.assert_not_called()

    @pytest.mark.asyncio
    async def test_two_klippy_ready_at_once_edit_once(self, config_dir):
        _, mig = make(config_dir, settings_with(*NEEDED))

        await asyncio.gather(mig.on_klippy_ready(), mig.on_klippy_ready())

        assert (config_dir / "printer.cfg").read_text() == RENAMED
        assert len(backups(config_dir)) == 1

    @pytest.mark.asyncio
    async def test_a_concurrent_write_skips_the_file_until_the_next_run(
        self, config_dir, monkeypatch
    ):
        cfg = config_dir / "printer.cfg"
        _, mig = make(config_dir, settings_with(*NEEDED))
        concurrent = INSTRUMENTED + "; saved by Fluidd\n"
        real_chmod = os.chmod
        raced = {"done": False}

        def chmod_then_race(path, mode, *args, **kwargs):
            if ".helix-tracking-strip-" in str(path) and not raced["done"]:
                raced["done"] = True
                cfg.write_text(concurrent)
            return real_chmod(path, mode, *args, **kwargs)

        monkeypatch.setattr(os, "chmod", chmod_then_race)

        first = await mig.run_once()

        assert first["state"] == "needs_attention"
        assert [entry["path"] for entry in first["skipped"]] == ["printer.cfg"]
        assert cfg.read_text() == concurrent

        second = await mig.run_once()

        assert second["state"] == "done"
        assert cfg.read_text() == RENAMED + "; saved by Fluidd\n"

    @pytest.mark.asyncio
    async def test_an_anomaly_alone_needs_attention(self, tmp_path):
        broken = f"[gcode_macro X]\ngcode:\n    {BEGIN}\n    HELIX_READY\n"
        (tmp_path / "printer.cfg").write_text(broken)
        _, mig = make(tmp_path, settings_with(*NEEDED))

        report = await mig.run_once()

        assert report["state"] == "needs_attention"
        assert report["skipped"][0]["path"] == "printer.cfg"
        assert (tmp_path / "printer.cfg").read_text() == broken

    @pytest.mark.asyncio
    async def test_the_report_survives_a_restart(self, config_dir):
        helix, mig = make(config_dir, settings_with(*NEEDED))
        done = await mig.run_once()

        restarted = migration.HelixSignalMigration(helix, True)
        await restarted.load()

        assert restarted.report() == done

    @pytest.mark.asyncio
    async def test_the_endpoint_serves_the_report(self, config_dir):
        helix, mig = make(config_dir, settings_with(*NEEDED))
        mig.start()
        done = await mig.run_once()

        served = await helix.server.endpoints["/server/helix/signal_migration"](
            MockWebRequest({})
        )

        assert served == done
        assert mig.on_klippy_ready in helix.server.event_handlers["server:klippy_ready"]


# ============================================================================
# The feature entry
# ============================================================================


async def _features(component, server):
    status = await server.endpoints["/server/helix/status"](MockWebRequest({}))
    return status["features"]


def _component(mock_server, temp_gcodes_dir, options):
    from test_helix_print import MockFileManager

    mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
    mock_server.components["database"] = MockDatabase()
    mock_server.components["klippy_apis"] = MockKlippyApis()
    mock_server.components["history"] = MockHistory()
    return load_component(MockConfigHelper(mock_server, dict(
        {"cleanup_delay": 3600, "enabled": True}, **options)))


class TestFeatureEntry:
    @pytest.mark.asyncio
    async def test_on_by_default(self, mock_server, temp_gcodes_dir):
        component = _component(mock_server, temp_gcodes_dir, {})
        await component.component_init()

        entry = (await _features(component, mock_server))[migration.SIGNAL_MIGRATION_FEATURE]

        assert entry == {"version": migration.SIGNAL_MIGRATION_FEATURE_VERSION,
                         "available": True, "enabled": True, "reason": None}
        assert component.features[migration.SIGNAL_MIGRATION_FEATURE].acts is True
        assert "/server/helix/signal_migration" in mock_server.endpoints

    @pytest.mark.asyncio
    async def test_the_option_switches_it_off(self, mock_server, temp_gcodes_dir):
        component = _component(mock_server, temp_gcodes_dir,
                               {migration.CONFIG_OPTION: False})
        await component.component_init()

        entry = (await _features(component, mock_server))[migration.SIGNAL_MIGRATION_FEATURE]

        assert entry["available"] is True
        assert entry["enabled"] is False

    @pytest.mark.asyncio
    async def test_plugin_features_does_not_switch_it(self, mock_server, temp_gcodes_dir):
        component = _component(mock_server, temp_gcodes_dir, {})
        await component.component_init()
        await component.database.insert_item(
            helix_print.HELIX_SCREEN_NAMESPACE, helix_print.PLUGIN_FEATURES_KEY,
            {migration.SIGNAL_MIGRATION_FEATURE: False},
        )

        entry = (await _features(component, mock_server))[migration.SIGNAL_MIGRATION_FEATURE]

        assert entry["enabled"] is True

    @pytest.mark.asyncio
    async def test_a_missing_strip_module_leaves_the_rest_running(
        self, mock_server, temp_gcodes_dir, monkeypatch
    ):
        monkeypatch.setattr(helix_print, "strip_phase_tracking", None)
        monkeypatch.setitem(SIBLING_LOAD_ERRORS, "strip_phase_tracking", "module_missing")
        component = _component(mock_server, temp_gcodes_dir, {})
        await component.component_init()

        features = await _features(component, mock_server)

        entry = features[migration.SIGNAL_MIGRATION_FEATURE]
        assert entry["available"] is False
        assert entry["reason"] == "module_missing"
        assert features["event_journal"]["available"] is True
        assert "/server/helix/signal_migration" not in mock_server.endpoints

    def test_the_plugin_version_moved_past_the_last_release(self):
        assert helix_print.PLUGIN_VERSION == "1.5.0"
