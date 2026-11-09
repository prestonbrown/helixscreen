# SPDX-License-Identifier: GPL-3.0-or-later
"""A per-test watchdog for the plugin suite.

A test whose setup, call or teardown runs past HELIX_PLUGIN_TEST_TIMEOUT_S
(default 300, 0 turns it off) ends the run: the watchdog prints the test's id
and every thread's stack, then exits 1. A hang in the event loop's teardown
leaves the last line of -v output naming the test before it, so the id
printed here is the one to read. pytest-timeout does the same, but is not in
every venv the gate runs from.
"""

import os
import sys
import threading
import time
import traceback

import pytest

TIMEOUT_ENV = "HELIX_PLUGIN_TEST_TIMEOUT_S"
DEFAULT_TIMEOUT_S = 300.0


def _limit() -> float:
    try:
        return float(os.environ.get(TIMEOUT_ENV, DEFAULT_TIMEOUT_S))
    except ValueError:
        return DEFAULT_TIMEOUT_S


def _expire(item: pytest.Item, limit: float) -> None:
    capman = item.config.pluginmanager.getplugin("capturemanager")
    if capman is not None:
        capman.suspend_global_capture(in_=True)
    out = sys.stdout
    out.write("\n%s TIMEOUT: %s ran past %.0f s (%s); stacks:\n"
              % (time.strftime("%H:%M:%S"), item.nodeid, limit, TIMEOUT_ENV))
    names = {t.ident: t.name for t in threading.enumerate()}
    for ident, frame in sys._current_frames().items():
        out.write("\n--- thread %s ---\n" % names.get(ident, ident))
        out.write("".join(traceback.format_stack(frame)))
    out.flush()
    os._exit(1)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_protocol(item: pytest.Item, nextitem: object):
    limit = _limit()
    if limit <= 0:
        yield
        return
    timer = threading.Timer(limit, _expire, (item, limit))
    timer.daemon = True
    timer.start()
    try:
        yield
    finally:
        timer.cancel()
