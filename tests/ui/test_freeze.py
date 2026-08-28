# SPDX-License-Identifier: GPL-3.0-or-later

"""freeze must stop motion without severing the control channel."""

import time


def test_freeze_then_unfreeze_keeps_the_channel_alive(helix_app):
    # The regression this guards: a global lv_timer_enable(false) stops the
    # UpdateQueue timer that execute_on_ui_thread depends on, so every command
    # after freeze times out — including unfreeze.
    helix_app.freeze()
    try:
        assert helix_app.ctl("ping") == "pong"
        assert helix_app.current()["panel"] is not None
    finally:
        helix_app.unfreeze()
    assert helix_app.ctl("ping") == "pong"


def test_freeze_reports_pausing_at_least_one_timer(helix_app):
    helix_app.navigate("home")
    helix_app.wait_idle()
    result = helix_app.freeze()
    try:
        assert result["frozen"] is True
        assert result["timers_paused"] >= 1
    finally:
        helix_app.unfreeze()


def test_unfreeze_resumes_exactly_what_freeze_paused(helix_app):
    frozen = helix_app.freeze()
    thawed = helix_app.unfreeze()
    assert thawed["timers_resumed"] == frozen["timers_paused"]


def test_freeze_twice_does_not_lose_track_of_the_paused_set(helix_app):
    # A naive re-scan on a second freeze() would find every timer already
    # paused, track none of them as newly-paused, and so overwrite the
    # server's bookkeeping with an empty set — orphaning the originally
    # paused timers, which unfreeze() could then never resume.
    first = helix_app.freeze()
    try:
        second = helix_app.freeze()
        assert second["frozen"] is True
        assert second["timers_paused"] == first["timers_paused"]
    finally:
        thawed = helix_app.unfreeze()
    assert thawed["timers_resumed"] == first["timers_paused"]


def test_unfreeze_without_freeze_is_a_noop(helix_app):
    # A defensive `finally: unfreeze()` (as several tests here use) must not
    # raise or misbehave if freeze() itself never ran.
    result = helix_app.unfreeze()
    assert result == {"frozen": False, "timers_resumed": 0}


def test_unfreeze_actually_resumes_a_paused_timer(helix_app):
    # The other tests in this file only check arithmetic (timers_resumed ==
    # timers_paused) — a regression where lv_timer_resume() silently became a
    # no-op, or resumed the wrong pointer, would still pass every one of them.
    # perf_history_tick is a real lv_timer (MockPerformanceSource, 1s period,
    # src/system/mock_performance_source.cpp) driving a subject readable over
    # ctl — genuinely paused by freeze's skip-list walk (it is neither the
    # UpdateQueue processor nor the display refresh timer) and genuinely
    # resumed by unfreeze, so its value is direct behavioral proof rather than
    # a count the server reports about itself.
    #
    # Two different shapes here. "Still advancing after unfreeze" is a
    # positive assertion, so it's event-driven: perf_history_tick increments
    # by exactly 1 per tick (PerformanceState::update — `tick = current + 1`),
    # so the exact next value is known up front and wait_for's exact-match
    # semantics fit it precisely. That resolves the instant the tick actually
    # fires instead of guessing how long to sleep, so it is *faster* than a
    # fixed sleep in the common case and immune to the flake a fixed margin
    # has under load (this used to sleep 2.2s against the 1s period and
    # failed once under concurrent load).
    #
    # "Stays flat while frozen" is the opposite shape — asserting an absence
    # has no event to wait for, so it still needs a real wall-clock wait.
    # 1.3s (one tick period plus a modest margin) is enough: if freeze()
    # failed to actually pause the timer, it would have ticked within that
    # window regardless.
    helix_app.wait_idle()
    before = helix_app.get("perf_history_tick")["value"]

    helix_app.freeze()
    try:
        time.sleep(1.3)
        during = helix_app.get("perf_history_tick")["value"]
        assert during == before, "perf_history_tick advanced while frozen"
    finally:
        helix_app.unfreeze()

    helix_app.wait_for("perf_history_tick", during + 1, timeout=5)
    after = helix_app.get("perf_history_tick")["value"]
    assert after > during, "perf_history_tick did not resume advancing after unfreeze"
