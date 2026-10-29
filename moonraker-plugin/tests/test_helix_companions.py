# SPDX-License-Identifier: GPL-3.0-or-later
"""
Klipper companions (helix_companions.py): which Klipper-side modules to
install, remove or restart for, and the order the files change in. A
companion's config section naming a module that is not there stops Klipper
from booting, so the order is the safety property.

Run with: pytest tests/test_helix_companions.py -v
"""

import asyncio
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import helix_companions as hc

GAUGE = hc.COMPANIONS[0]
KLIPPER = "/usr/share/klipper"
FORCE = ["prtouch_v3", "extruder", "toolhead"]


def _plan(**kw):
    args = dict(enabled={"pa_gauge"}, objects=FORCE, installed={}, klipper_path=KLIPPER,
                klippy_error="", job_active=False)
    args.update(kw)
    return hc.plan_actions(hc.COMPANIONS, **args)


def test_enabled_companion_with_its_sensor_is_installed_and_waits_for_a_restart():
    actions = _plan()
    assert ("install", "helix_pa_gauge") in actions
    # A new module loads at the next Klipper restart, and that restart is
    # the user's to give.
    assert ("restart",) not in actions
    assert ("restart_pending", "helix_pa_gauge") in actions


def test_current_install_does_nothing():
    actions = _plan(installed={"helix_pa_gauge": {"files": True, "cfg": True, "current": True}})
    assert actions == []


def test_outdated_install_is_relinked():
    actions = _plan(installed={"helix_pa_gauge": {"files": True, "cfg": True, "current": False}})
    assert ("install", "helix_pa_gauge") in actions


def test_disabled_feature_removes_an_installed_companion():
    actions = _plan(enabled=set(),
                    installed={"helix_pa_gauge": {"files": True, "cfg": True, "current": True}})
    assert actions[0] == ("remove", "helix_pa_gauge")


def test_a_printer_without_the_sensor_gets_nothing_and_says_why():
    assert _plan(objects=["extruder", "toolhead"]) == [
        ("unavailable", "helix_pa_gauge", "no_force_sensor")]
    # Said whether or not the feature is switched on, so no screen offers it.
    assert _plan(enabled=set(), objects=["extruder"]) == [
        ("unavailable", "helix_pa_gauge", "no_force_sensor")]


def test_an_installed_companion_without_its_sensor_is_removed_then_reported():
    actions = _plan(objects=["extruder"],
                    installed={"helix_pa_gauge": {"files": True, "cfg": True, "current": True}})
    assert actions == [("remove", "helix_pa_gauge"), ("restart_pending", "helix_pa_gauge"),
                       ("unavailable", "helix_pa_gauge", "no_force_sensor")]


@pytest.mark.parametrize("sensor", ["load_cell", "load_cell_probe", "prtouch_v3"])
def test_every_force_sensor_kind_qualifies(sensor):
    assert ("install", "helix_pa_gauge") in _plan(objects=[sensor, "extruder"])


def test_unknown_object_list_changes_nothing():
    # Klippy not ready yet: no evidence either way, so leave files alone.
    installed = {"helix_pa_gauge": {"files": True, "cfg": True, "current": True}}
    assert _plan(objects=None, installed=installed) == []
    assert _plan(objects=None) == []


def test_unknown_klipper_path_reports_and_never_guesses():
    actions = _plan(klipper_path=None)
    assert actions == [("unavailable", "helix_pa_gauge", "klipper_path_unknown")]


def test_klipper_stopped_on_a_missing_companion_module_is_repaired_and_restarted():
    error = "Section 'helix_pa_gauge' is not a valid config section"
    actions = _plan(objects=None, klippy_error=error,
                    installed={"helix_pa_gauge": {"files": False, "cfg": True, "current": False}})
    assert actions == [("install", "helix_pa_gauge"), ("restart",)]


def test_other_klipper_errors_are_not_ours_to_restart():
    error = "Section 'cartographer' is not a valid config section"
    assert ("restart",) not in _plan(objects=None, klippy_error=error)


def test_no_restart_while_a_job_runs():
    error = "Section 'helix_pa_gauge' is not a valid config section"
    actions = _plan(objects=None, klippy_error=error, job_active=True,
                    installed={"helix_pa_gauge": {"files": False, "cfg": True, "current": False}})
    assert ("restart",) not in actions


# --- the executor's file order ----------------------------------------------

class FakeFs:
    def __init__(self, refuse_links=False):
        self.ops = []
        self.refuse_links = refuse_links

    def link(self, src, dst):
        if self.refuse_links:
            raise OSError("links not allowed here")
        self.ops.append(("link", Path(dst).name))

    def copy(self, src, dst):
        self.ops.append(("copy", Path(dst).name))

    def write(self, path, text):
        self.ops.append(("write", Path(path).name))

    def remove(self, path):
        self.ops.append(("remove", Path(path).name))


def test_install_puts_the_module_in_place_before_its_section():
    fs = FakeFs()
    hc.install(GAUGE, fs, plugin_dir="/p", klipper_path=KLIPPER, config_dir="/c")
    names = [name for _, name in fs.ops]
    assert names.index("helix_pa_gauge.cfg") > names.index("helix_pa_gauge.py")
    assert names.index("helix_pa_gauge.cfg") > names.index("helix_pa_fit.py")


def test_removal_takes_the_section_away_before_the_module():
    fs = FakeFs()
    hc.remove(GAUGE, fs, klipper_path=KLIPPER, config_dir="/c")
    names = [name for _, name in fs.ops]
    assert names[0] == "helix_pa_gauge.cfg"


def test_a_refused_link_falls_back_to_a_copy():
    fs = FakeFs(refuse_links=True)
    hc.install(GAUGE, fs, plugin_dir="/p", klipper_path=KLIPPER, config_dir="/c")
    assert ("copy", "helix_pa_gauge.py") in fs.ops


def test_section_text_names_the_companion():
    assert hc.section_text(GAUGE).splitlines()[0].strip() == "[helix_pa_gauge]"


# --- the manager, against a fake Moonraker -----------------------------------

class FakeApis:
    def __init__(self, info, objects=None, print_state="standby"):
        self.info = info
        self.objects = objects
        self.print_state = print_state
        self.gcode = []

    async def get_klippy_info(self):
        return self.info

    async def get_object_list(self):
        if self.objects is None:
            raise RuntimeError("Klippy not ready")
        return self.objects

    async def query_objects(self, objects):
        return {"print_stats": {"state": self.print_state}}

    async def run_gcode(self, script):
        self.gcode.append(script)


class FakeHelix:
    def __init__(self, apis, enabled, config_dir):
        self.klippy_apis = apis
        self._enabled = enabled
        self.file_manager = type("FM", (), {"get_directory": lambda s, n: config_dir})()
        self.changed = 0

    async def feature_enabled(self, name):
        return name in self._enabled

    def notify_features_changed(self):
        self.changed += 1


def _tree(tmp_path):
    plugin = tmp_path / "plugin"
    (plugin / "klippy").mkdir(parents=True)
    for name in GAUGE.files:
        (plugin / "klippy" / name).write_text("# %s v1\n" % name)
    klipper = tmp_path / "klipper"
    (klipper / "klippy" / "extras").mkdir(parents=True)
    config = tmp_path / "config"
    config.mkdir()
    (config / "printer.cfg").write_text("[printer]\n")
    return plugin, klipper, config


@pytest.mark.asyncio
async def test_sync_installs_on_a_printer_with_a_force_sensor(tmp_path):
    plugin, klipper, config = _tree(tmp_path)
    apis = FakeApis({"klipper_path": str(klipper), "state": "ready"}, objects=FORCE)
    helix = FakeHelix(apis, {"pa_gauge"}, str(config))
    manager = hc.CompanionManager(helix, plugin_dir=str(plugin))
    await manager.sync()
    assert (klipper / "klippy" / "extras" / "helix_pa_gauge.py").exists()
    assert (config / "helix_companions" / "helix_pa_gauge.cfg").exists()
    status = manager.status()["helix_pa_gauge"]
    assert status["restart_pending"] and status["reason"] is None
    assert apis.gcode == []


@pytest.mark.asyncio
async def test_sync_reports_a_printer_without_a_force_sensor(tmp_path):
    plugin, klipper, config = _tree(tmp_path)
    apis = FakeApis({"klipper_path": str(klipper), "state": "ready"}, objects=["extruder"])
    helix = FakeHelix(apis, {"pa_gauge"}, str(config))
    manager = hc.CompanionManager(helix, plugin_dir=str(plugin))
    await manager.sync()
    assert manager.status()["helix_pa_gauge"]["reason"] == "no_force_sensor"
    assert not (config / "helix_companions").exists()
    # The sensor appears (a firmware swap, a config change): the reason goes.
    apis.objects = FORCE
    await manager.sync()
    assert manager.status()["helix_pa_gauge"]["reason"] is None


@pytest.mark.asyncio
async def test_sync_repairs_and_restarts_klipper_stopped_on_our_section(tmp_path):
    plugin, klipper, config = _tree(tmp_path)
    (config / "helix_companions").mkdir()
    (config / "helix_companions" / "helix_pa_gauge.cfg").write_text("[helix_pa_gauge]\n")
    apis = FakeApis({"klipper_path": str(klipper), "state": "error",
                     "state_message": "Section 'helix_pa_gauge' is not a valid config section"})
    helix = FakeHelix(apis, {"pa_gauge"}, str(config))
    await hc.CompanionManager(helix, plugin_dir=str(plugin)).sync()
    assert (klipper / "klippy" / "extras" / "helix_pa_gauge.py").exists()
    assert apis.gcode == ["RESTART"]


@pytest.mark.asyncio
async def test_sync_removes_when_the_feature_is_switched_off(tmp_path):
    plugin, klipper, config = _tree(tmp_path)
    apis = FakeApis({"klipper_path": str(klipper), "state": "ready"}, objects=FORCE)
    helix = FakeHelix(apis, {"pa_gauge"}, str(config))
    manager = hc.CompanionManager(helix, plugin_dir=str(plugin))
    await manager.sync()
    helix._enabled = set()
    await manager.sync()
    assert not (config / "helix_companions" / "helix_pa_gauge.cfg").exists()
    assert not os.path.lexists(klipper / "klippy" / "extras" / "helix_pa_gauge.py")


@pytest.mark.asyncio
async def test_first_install_adds_the_include_to_printer_cfg_once(tmp_path):
    plugin, klipper, config = _tree(tmp_path)
    (config / "printer.cfg").write_text("[printer]\n")
    apis = FakeApis({"klipper_path": str(klipper), "state": "ready"}, objects=FORCE)
    manager = hc.CompanionManager(FakeHelix(apis, {"pa_gauge"}, str(config)),
                                  plugin_dir=str(plugin))
    await manager.sync()
    await manager.sync()
    text = (config / "printer.cfg").read_text()
    assert text.count(hc.INCLUDE_LINE) == 1
    assert (config / "printer.cfg.helixscreen-bak").read_text() == "[printer]\n"


@pytest.mark.asyncio
async def test_uninstall_takes_away_include_sections_and_modules(tmp_path):
    plugin, klipper, config = _tree(tmp_path)
    (config / "printer.cfg").write_text("[printer]\n")
    apis = FakeApis({"klipper_path": str(klipper), "state": "ready"}, objects=FORCE)
    await hc.CompanionManager(FakeHelix(apis, {"pa_gauge"}, str(config)),
                              plugin_dir=str(plugin)).sync()
    assert hc.uninstall_companions(config) is None
    assert (config / "printer.cfg").read_text() == "[printer]\n"
    assert not (config / "helix_companions").exists()
    assert not os.path.lexists(klipper / "klippy" / "extras" / "helix_pa_gauge.py")
    assert not os.path.lexists(klipper / "klippy" / "extras" / "helix_pa_fit.py")


def test_uninstall_with_nothing_installed_is_a_no_op(tmp_path):
    (tmp_path / "printer.cfg").write_text("[printer]\n")
    assert hc.uninstall_companions(tmp_path) is None
    assert (tmp_path / "printer.cfg").read_text() == "[printer]\n"


@pytest.mark.asyncio
async def test_a_switched_feature_syncs_when_the_status_is_next_read(tmp_path):
    plugin, klipper, config = _tree(tmp_path)
    apis = FakeApis({"klipper_path": str(klipper), "state": "ready"}, objects=FORCE)
    helix = FakeHelix(apis, set(), str(config))
    manager = hc.CompanionManager(helix, plugin_dir=str(plugin))
    assert manager.note_opt_ins({}) is not None
    await asyncio.sleep(0)
    assert manager.note_opt_ins({"photos": True}) is None
    # The user switches the gauge on: the screen writes the opt-in, then
    # reads the status, and the companion goes in without a Klippy event.
    helix._enabled = {"pa_gauge"}
    task = manager.note_opt_ins({"pa_gauge": True})
    assert task is not None
    await task
    assert (config / "helix_companions" / "helix_pa_gauge.cfg").exists()
    assert manager.note_opt_ins({"pa_gauge": True}) is None
