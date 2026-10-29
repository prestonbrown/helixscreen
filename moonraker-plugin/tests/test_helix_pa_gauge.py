# SPDX-License-Identifier: GPL-3.0-or-later
"""
The Klipper companion (klippy/helix_pa_gauge.py): force-source selection,
routine profiles, the sweep G-code the fit reads, the measured cache, and a
ready callback that never raises whatever the printer lacks.

Run with: pytest tests/test_helix_pa_gauge.py -v
"""

import re
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "klippy"))

import helix_pa_gauge as gauge

K2_BOX = {
    "extrude_pos_x": "133", "extrude_pos_y": "378",
    "clean_left_pos_x": "135", "clean_pos_left_x": "160",
}


# --- fakes ------------------------------------------------------------------

class FakeGcode:
    def __init__(self):
        self.commands = {}

    def register_command(self, name, fn, desc=None):
        self.commands[name] = fn


class FakePrinter:
    def __init__(self, objects):
        self.objects = dict(objects)
        self.handlers = {}
        self.objects.setdefault("gcode", FakeGcode())

    def lookup_object(self, name, default=None):
        return self.objects.get(name, default)

    def get_reactor(self):
        return SimpleNamespace(monotonic=lambda: 0.0)

    def register_event_handler(self, event, fn):
        self.handlers[event] = fn


class FakeConfig:
    def __init__(self, printer, values=None):
        self.printer = printer
        self.values = values or {}

    def get_printer(self):
        return self.printer

    def get_name(self):
        return "helix_pa_gauge"

    def get(self, key, default=None):
        return self.values.get(key, default)

    def getfloat(self, key, default=None, **_):
        return float(self.values.get(key, default)) if key in self.values or default is not None else None

    def getint(self, key, default=None, **_):
        return int(self.values.get(key, default)) if key in self.values or default is not None else None


class FakeMcu:
    def __init__(self):
        self.lookups = []

    def alloc_command_queue(self):
        return object()

    def lookup_query_command(self, msgformat, respformat, oid=None, cq=None, **_):
        self.lookups.append((msgformat.split()[0], respformat.split()[0], oid))
        return SimpleNamespace(send=lambda args: {})


def _prtouch(pres):
    return SimpleNamespace(pres=pres)


def _motion_report(attr="dtrapqs"):
    return SimpleNamespace(**{attr: {"extruder": object()}})


def _configfile(settings):
    return SimpleNamespace(get_status=lambda eventtime: {"settings": settings})


def _ready(objects, values=None):
    printer = FakePrinter(objects)
    companion = gauge.HelixPaGauge(FakeConfig(printer, values))
    printer.handlers["klippy:ready"]()
    return companion


# --- ready callback ---------------------------------------------------------

def test_stock_k2_is_available_with_the_prtouch_source():
    mcu = FakeMcu()
    companion = _ready({
        "prtouch_v3": _prtouch(SimpleNamespace(oid=4, mcu=mcu)),
        "motion_report": _motion_report(),
        "configfile": _configfile({"box": K2_BOX}),
    })
    status = companion.get_status(0.)
    assert status["available"], status["reason"]
    assert status["source"] == "prtouch"
    assert status["profile"] == "creality_cfs"
    # Our own handles on Creality's channel, not the stock wrapper's.
    assert {name for name, _, oid in mcu.lookups} == {
        "start_prtouch_pres", "read_prtouch_pres", "stop_prtouch_pres"}
    assert all(oid == 4 for _, _, oid in mcu.lookups)


def test_creality_motion_report_names_its_map_trapqs():
    companion = _ready({
        "prtouch_v3": _prtouch(SimpleNamespace(oid=4, mcu=FakeMcu())),
        "motion_report": _motion_report("trapqs"),
        "configfile": _configfile({"box": K2_BOX}),
    })
    assert companion.get_status(0.)["available"]


BOX_CONFIG = _configfile({"box": K2_BOX})


@pytest.mark.parametrize("objects, reason", [
    ({"motion_report": _motion_report(), "configfile": BOX_CONFIG},
     "no nozzle force sensor"),
    ({"prtouch_v3": SimpleNamespace(), "motion_report": _motion_report(),
      "configfile": BOX_CONFIG},
     "no nozzle force sensor"),
    ({"prtouch_v3": _prtouch(SimpleNamespace(mcu=FakeMcu())),
      "motion_report": _motion_report(), "configfile": BOX_CONFIG},
     "no nozzle force sensor"),
    ({"prtouch_v3": _prtouch(SimpleNamespace(oid=4, mcu=FakeMcu())),
      "configfile": BOX_CONFIG},
     "no extruder motion report"),
    ({"prtouch_v3": _prtouch(SimpleNamespace(oid=4, mcu=FakeMcu())),
      "motion_report": _motion_report(), "configfile": _configfile({})},
     "purge_x"),
])
def test_ready_never_raises_and_says_why(objects, reason):
    status = _ready(objects).get_status(0.)
    assert not status["available"]
    assert reason in status["reason"]


def test_a_failing_handle_lookup_is_a_reason_not_a_crash():
    class BrokenMcu(FakeMcu):
        def lookup_query_command(self, *a, **k):
            raise RuntimeError("Unknown command: start_prtouch_pres")
    status = _ready({
        "prtouch_v3": _prtouch(SimpleNamespace(oid=4, mcu=BrokenMcu())),
        "motion_report": _motion_report(),
        "configfile": _configfile({"box": K2_BOX}),
    }).get_status(0.)
    assert not status["available"]
    assert "start_prtouch_pres" in status["reason"]


def test_upstream_load_cell_is_a_source():
    companion = _ready({
        "load_cell": SimpleNamespace(add_client=lambda cb: None),
        "motion_report": _motion_report(),
        "configfile": _configfile({}),
    }, {"purge_x": "5", "purge_y": "5", "wiggle": "14"})
    status = companion.get_status(0.)
    assert status["available"], status["reason"]
    assert status["source"] == "load_cell"
    assert status["profile"] == "generic"


# --- routine profiles -------------------------------------------------------

def test_creality_profile_reads_the_box_section():
    profile, reason = gauge.profile_from_settings({"box": K2_BOX}, {})
    assert reason is None
    assert profile["name"] == "creality_cfs"
    assert profile["purge"] == (133.0, 378.0)
    xa, xb = profile["wiggle"]
    # Inside the chute, clear of the silicone wiper.
    assert xa == 133.0 and xb < 160.0
    assert "BOX_START_PRINT" in profile["feed_start"]
    assert "BOX_END_PRINT" in profile["feed_stop"]


def test_generic_profile_needs_a_purge_position():
    profile, reason = gauge.profile_from_settings({}, {})
    assert profile is None
    assert "purge_x" in reason
    profile, reason = gauge.profile_from_settings({}, {"purge_x": "5", "purge_y": "300", "wiggle": "20"})
    assert profile["name"] == "generic"
    assert profile["wiggle"] == (5.0, 25.0)


# --- sweep G-code -----------------------------------------------------------

def test_sweep_marks_every_pa_value_the_fit_reads():
    profile, _ = gauge.profile_from_settings({"box": K2_BOX}, {})
    lines = gauge.sweep_gcode(profile, [0.04, 0.08], cycles=2)
    text = "\n".join(lines)
    for label in ("dry", "k0.04", "end0.04", "k0.08", "end0.08"):
        assert "HELIX_PA_LOG_MARK LABEL=%s" % label in text
    assert text.index("HELIX_PA_LOG_START") < text.index("LABEL=dry")
    assert lines[-1].startswith("HELIX_PA_LOG_STOP")


def test_every_logged_extrusion_moves_x():
    # Klipper applies PA only to moves with X/Y: an extrude-only logged move
    # measures nothing.
    profile, _ = gauge.profile_from_settings({"box": K2_BOX}, {})
    lines = gauge.sweep_gcode(profile, [0.04, 0.08], cycles=2)
    logged = lines[next(i for i, l in enumerate(lines) if "LABEL=dry" in l):]
    extrusions = [l for l in logged if re.match(r"G1 .*E\d", l)]
    assert extrusions
    assert all(" X" in l for l in extrusions)
    xs = {float(re.search(r"X([\d.]+)", l).group(1)) for l in extrusions}
    assert xs == set(profile["wiggle"])


def test_slow_and_fast_flow_match_the_requested_rates():
    profile, _ = gauge.profile_from_settings({"box": K2_BOX}, {})
    lines = gauge.sweep_gcode(profile, [0.04, 0.08], cycles=1, slow=2.0, fast=12.0)
    xa, xb = profile["wiggle"]
    seconds = abs(xb - xa) / profile["speed"]
    rates = set()
    for l in lines:
        m = re.match(r"G1 X[\d.]+ E([\d.]+)", l)
        if m:
            rates.add(round(float(m.group(1)) * gauge.FILAMENT_AREA / seconds, 2))
    assert rates == {2.0, 12.0}


def test_quick_sweep_straddles_the_centre():
    ks = gauge.quick_ks(0.04)
    assert len(ks) == 4
    assert min(ks) < 0.04 < max(ks)
    assert ks == sorted(ks)
    assert min(gauge.quick_ks(0.005)) > 0


# --- cache ------------------------------------------------------------------

def test_cache_round_trips_by_filament_and_temperature(tmp_path):
    cache = gauge.PaCache(str(tmp_path / "helix_pa_gauge.json"))
    assert cache.get("PLA|white", 220) is None
    cache.put("PLA|white", 220, 0.082, stamp=1.0)
    again = gauge.PaCache(str(tmp_path / "helix_pa_gauge.json"))
    assert again.get("PLA|white", 220)["k"] == 0.082
    # A different temperature is a different measurement.
    assert again.get("PLA|white", 240) is None


def test_a_corrupt_cache_file_reads_as_empty(tmp_path):
    path = tmp_path / "helix_pa_gauge.json"
    path.write_text("{not json")
    assert gauge.PaCache(str(path)).get("PLA|white", 220) is None
