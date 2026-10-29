# SPDX-License-Identifier: GPL-3.0-or-later
"""
Klipper companions (helix_companions.py): which Klipper-side modules to
install, remove or restart for, and the order the files change in. A
companion's config section naming a module that is not there stops Klipper
from booting, so the order is the safety property.

Run with: pytest tests/test_helix_companions.py -v
"""

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


def test_a_printer_without_the_sensor_gets_nothing():
    assert _plan(objects=["extruder", "toolhead"]) == []


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
