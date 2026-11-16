# SPDX-License-Identifier: GPL-3.0-or-later

"""wait_idle must actually gate on async work, not just return immediately."""

import re

import pytest

from helix.app import HelixCtlError


def test_wait_idle_returns_idle_on_a_quiet_screen(helix_app):
    helix_app.navigate("home")
    result = helix_app.wait_idle()
    assert result["idle"] is True
    assert result["waited_ms"] >= 0


def test_wait_idle_after_navigation_makes_the_panel_queryable(helix_app):
    # Without wait_idle this is the classic race: navigate returns as soon as
    # the panel is created, before its subjects populate.
    helix_app.navigate("controls")
    helix_app.wait_idle()
    listing = helix_app.ls()
    names = {w["name"] for w in listing["widgets"] if w.get("name")}
    assert "btn_motion" in names


def test_wait_idle_reports_which_counter_was_busy_on_timeout(helix_app):
    # With timeout=0.0 the raise itself is unconditional: `last` is seeded
    # non-idle so the two-consecutive-samples rule can never pass on the
    # first (only) sample, and the deadline has already elapsed by the time
    # it's checked. That makes `pytest.raises` alone a tautology — it would
    # pass even if wait_idle always embedded a hardcoded "update_queue=0" in
    # its message. The property actually worth testing is that the embedded
    # counter is a REAL nonzero reading, not just present-looking text.
    #
    # Racing a burst of subject changes (via `scenario printing`/`idle`)
    # against this process's own subprocess-spawn + socket round trip cannot
    # win that reading reliably. The burst shows up in
    # UpdateQueue.pending_count() for at most one drain period (33 ms), because
    # process_pending() drains the ENTIRE queue each time, so there is no way
    # to widen that window from the subject side, and under load spawn latency
    # alone exceeds it. No amount of retrying fixes a per-attempt probability
    # that collapses exactly when the machine is busy enough to make spawning
    # slow.
    #
    # The "http_busy" mock scenario sidesteps the race instead of tuning it:
    # it submits a 2-second synthetic job to HttpExecutor::fast(), whose
    # inflight() counter is incremented at submission and only decremented
    # on job completion (see core/http_executor.h). That gives wait_idle a
    # multi-hundred-millisecond, load-insensitive window — 2s comfortably
    # exceeds subprocess-spawn latency even under heavy load — so a single
    # sample is enough; no retry loop needed.
    counter_pattern = re.compile(r"http=[1-9]\d*")
    try:
        helix_app.ctl("scenario", "http_busy")
        with pytest.raises(HelixCtlError) as exc:
            helix_app.wait_idle(timeout=0.0)
        message = exc.value.message
        assert counter_pattern.search(message), (
            f"wait_idle did not report a genuinely nonzero http counter "
            f"(message: {message!r}) — either the http_busy scenario didn't "
            "submit its job, or counter reporting regressed to always "
            "showing zero"
        )
    finally:
        # The submitted job keeps running for its fixed 2s regardless of
        # what this test does. Wait it out here rather than leaving it
        # dangling — this is a session-scoped shared instance, and the next
        # test to call wait_idle shouldn't inherit a surprise busy http=1.
        helix_app.wait_idle(timeout=5.0)


def test_wait_idle_reports_thumbnail_counter_busy_on_timeout(helix_app):
    # Same property as the http counter above, for ThumbnailProcessor's own
    # worker pool. "thumbnail_busy" submits a synthetic 2s task directly to
    # the pool via submit_test_task(), sidestepping real PNG decode timing
    # for the same load-insensitive window http_busy uses.
    counter_pattern = re.compile(r"thumbnail=[1-9]\d*")
    try:
        helix_app.ctl("scenario", "thumbnail_busy")
        with pytest.raises(HelixCtlError) as exc:
            helix_app.wait_idle(timeout=0.0)
        message = exc.value.message
        assert counter_pattern.search(message), (
            f"wait_idle did not report a genuinely nonzero thumbnail counter "
            f"(message: {message!r}), either the thumbnail_busy scenario didn't "
            "submit its task, or counter reporting regressed to always "
            "showing zero"
        )
    finally:
        helix_app.wait_idle(timeout=5.0)
