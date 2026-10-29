# SPDX-License-Identifier: GPL-3.0-or-later
"""
Pressure advance from a nozzle force log (klippy/helix_pa_fit.py), against runs
captured on a stock Creality K2 Plus: PLA at 220 C, the X wiggle over the purge
chute, slow/fast flow steps at each tested PA value.

Run with: pytest tests/test_helix_pa_fit.py -v
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "klippy"))

import helix_pa_fit as fit

FIXTURES = str(Path(__file__).parent / "fixtures" / "pa_gauge")


def _run(name):
    return fit.load(FIXTURES, name)


def test_wiggle_run_measures_the_crossing():
    result = fit.analyse(*_run("run4"))
    assert result["reliable"], result["problems"]
    assert 0.078 <= result["fitted"] <= 0.088


def test_extrude_only_run_is_rejected():
    # Klipper applies PA only to moves with X/Y, so this run's balance does
    # not move with PA: no crossing, no result.
    result = fit.analyse(*_run("run2"))
    assert not result["reliable"]
    assert result["fitted"] is None


def test_decode_glitches_do_not_move_the_result():
    pressure, moves, marks = _run("run4")
    clean_k = fit.analyse(pressure, moves, marks)["fitted"]
    glitched = list(pressure)
    injected = range(500, len(glitched), 997)
    for i in injected:
        t, _ = glitched[i]
        glitched[i] = (t, 0.0 if i % 2 else -6.5e6)
    result = fit.analyse(glitched, moves, marks)
    assert result["reliable"], result["problems"]
    assert result["glitches"] >= len(injected)
    assert result["fitted"] == pytest.approx(clean_k, abs=0.002)


def test_clean_keeps_a_brief_pressure_overshoot():
    # Too much PA shows as a short overshoot after a flow step, shorter than
    # half the median window and far from the running median. It is the
    # evidence the fit scores, so it must survive.
    samples = [(i * 0.001, -1.02e6) for i in range(200)]
    for i in range(100, 108):
        samples[i] = (i * 0.001, -1.02e6 + 100000)
    assert len(fit.clean(samples)) == len(samples)


def test_clean_drops_zeros_and_wild_values():
    samples = [(i * 0.001, -1.10e6) for i in range(200)]
    samples[50] = (0.05, 0.0)
    samples[120] = (0.12, -6.5e6)
    kept = fit.clean(samples)
    assert len(kept) == 198
    assert all(v == -1.10e6 for _, v in kept)


def test_a_log_without_flow_steps_reports_instead_of_raising():
    pressure, _, marks = _run("run4")
    result = fit.analyse(pressure, [], marks)
    assert not result["reliable"]
    assert result["fitted"] is None
    assert result["problems"]
