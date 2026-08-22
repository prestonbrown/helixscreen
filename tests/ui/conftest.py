# SPDX-License-Identifier: GPL-3.0-or-later

"""Fixtures for the helixctl-driven UI tests."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))  # make `helix` importable

from helix.app import HelixApp  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
# HELIX_UI_BINARY lets a test that copies this conftest elsewhere (see
# test_diagnostics.py's pytester sub-run) point back at the real binary —
# REPO_ROOT above is computed from *this* file's location, which is wrong
# once the file has been copied into a temp directory.
BINARY = Path(os.environ.get("HELIX_UI_BINARY", str(REPO_ROOT / "build" / "bin" / "helix-screen")))

# moonraker_client_mock_files.cpp's scan_mock_gcode_files() + server.files.list
# mock report each fixture's REAL on-disk mtime via stat(), and print-select
# sorts on it. Git checkout doesn't preserve original commit timestamps, so
# without this, which files land on the first screen (and their order) varies
# by checkout/clone/machine — print-select's golden was never reproducible
# on a fresh clone by construction, not flaky. Newest first, matching the
# order the committed golden was captured under.
#
# Pinning fixes the ORDER, not the COUNT. The card view renders every file in
# a scrolling 4-across grid, so adding a fixture here can add a row and shorten
# the scrollbar thumb even when it sorts last and no card is visibly different
# — a ~200px golden diff confined to the scrollbar column. Adding or removing
# any file in this list means regenerating the print-select golden:
#   pytest 'tests/ui/test_screens.py::test_screen_matches_golden[print-select]' \
#       --accept-goldens
_TEST_GCODE_DIR = REPO_ROOT / "assets" / "test_gcodes"
_TEST_GCODE_MTIME_ORDER = [
    "xyz-10mm-calibration-cube.gcode",
    "vaso_voronoi_V2_abajo.gcode",
    "u1_4color_ring.gcode",
    "stand_s.gcode",
    "ssr_heat_sink_orca.gcode",
    "exclude_object_test.gcode",
    "calicat_calico.gcode",
    "Weighted Baseplate 3x2.gcode",
    "Torture_Calibration_Cube_New.gcode",
    "SimpleCuraTest.gcode",
    "Benchbin_MK4_MMU3.gcode",
    "3DBenchy.gcode",
    "Night Spirit_v1_2_og.gcode",
    "Low poly vase v1.1 flat top.gcode",
    "ECC_0.4_stand_PLA0.2_2h42m.gcode",
    # Added by 6ba20a707 as extra mock gcode; nothing references them by name.
    # Pinned oldest so they sort onto the last row rather than displacing the
    # cards the golden was captured under. Moving either up is a deliberate
    # golden update.
    "eiffel_final_PLA_2h42m.gcode",
    "2022-big-ben-by-miniworld3d_ABS_36m21s.gcode",
]


@pytest.fixture(scope="session", autouse=True)
def _pin_test_gcode_mtimes():
    """Normalize assets/test_gcodes/*.gcode mtimes before any test boots an app.

    Idempotent (sets a fixed timestamp, doesn't touch content) and runs once
    per session before the first app boots. The only side effect is these 15
    files' mtimes changing on disk — scripts/screenshot.sh and the doc
    pipeline also read this directory, but neither depends on a *specific*
    mtime (only content), so this is safe to run unconditionally rather than
    gating it on which test actually needs print-select.
    """
    # An unpinned .gcode keeps its checkout mtime, which is newer than every
    # pinned one, so it silently takes card 0 and print-select tests fail on a
    # card-identity assertion that says nothing about the real cause. Fail here
    # instead, naming the file and the fix.
    on_disk = {p.name for p in _TEST_GCODE_DIR.glob("*.gcode")}
    unpinned = sorted(on_disk - set(_TEST_GCODE_MTIME_ORDER))
    if unpinned:
        raise AssertionError(
            f"unpinned gcode fixture(s) in assets/test_gcodes: {unpinned}. "
            "Add each to _TEST_GCODE_MTIME_ORDER above - at the END to keep it out "
            "of print-select's visible top 10, or higher up as a deliberate golden "
            "update. Left unpinned it sorts newest and displaces card 0.")

    base = 1_700_000_000  # arbitrary fixed epoch; only the relative order matters
    for i, name in enumerate(reversed(_TEST_GCODE_MTIME_ORDER)):
        path = _TEST_GCODE_DIR / name
        if not path.exists():
            continue
        ts = base + i
        os.utime(path, (ts, ts))


@pytest.fixture(scope="session")
def helix_app(tmp_path_factory):
    """One app instance shared by the whole session.

    Session scope because a boot costs ~2s and a full corpus run is hundreds of
    tests. Tests that dirty global state should request `fresh_helix_app`.
    """
    if not BINARY.exists():
        pytest.skip(f"{BINARY} not built — run `make -j`")
    workdir = tmp_path_factory.mktemp("helix-session")
    app = HelixApp(binary=BINARY,
                   socket_path=workdir / "control.sock",
                   log_path=workdir / "app.log")
    with app:
        yield app


@pytest.fixture
def fresh_helix_app(tmp_path):
    """A private instance for a single test. Use when a test dirties global state."""
    if not BINARY.exists():
        pytest.skip(f"{BINARY} not built — run `make -j`")
    app = HelixApp(binary=BINARY,
                   socket_path=tmp_path / "control.sock",
                   log_path=tmp_path / "app.log")
    with app:
        yield app


@pytest.fixture(autouse=True)
def clean_screen(request):
    """Reset to a known screen before each test that uses the shared instance.

    Autouse so a test that navigates somewhere cannot leak that state into the
    next one. Tests using `fresh_helix_app` get a new process and skip this.
    """
    if "helix_app" not in request.fixturenames:
        return
    app = request.getfixturevalue("helix_app")
    app.reset()
    app.wait_idle()


ARTIFACT_ROOT = Path(os.environ.get("HELIX_UI_ARTIFACTS", "ui-artifacts"))


@pytest.hookimpl(hookwrapper=True, tryfirst=True)
def pytest_runtest_makereport(item, call):
    """Stash each phase's report on the item so fixtures can see the outcome."""
    outcome = yield
    report = outcome.get_result()
    setattr(item, f"rep_{report.when}", report)


@pytest.fixture
def artifacts(request):
    """A directory for this test's diagnostics. Populated only if the test fails.

    Resolves whichever app fixture the failing test actually requested —
    `fresh_helix_app` first (a test requesting both wants its private
    instance), falling back to the shared `helix_app`. A test that requests
    `artifacts` without either app fixture gets a clean no-op teardown rather
    than an error.
    """
    app = None
    for name in ("fresh_helix_app", "helix_app"):
        if name in request.fixturenames:
            app = request.getfixturevalue(name)
            break

    target = ARTIFACT_ROOT / request.node.name
    yield target

    failed = any(
        getattr(request.node, f"rep_{phase}", None) is not None
        and getattr(request.node, f"rep_{phase}").failed
        for phase in ("setup", "call", "teardown")
    )
    if not failed or app is None:
        return

    target.mkdir(parents=True, exist_ok=True)
    # Each dump is independently best-effort: the app may be wedged or dead, and
    # losing the screenshot must not cost us the log.
    try:
        app.screenshot(str((target / "screen.png").resolve()))
    except Exception as exc:  # noqa: BLE001 - diagnostics must never mask the real failure
        (target / "screen.png.error").write_text(str(exc))
    try:
        (target / "app.log").write_text("\n".join(app.log(200)))
    except Exception:  # noqa: BLE001
        (target / "app.log").write_text(app._log_tail_from_file(200))
    try:
        state = [f"current: {app.current()}", "", f"ls: {app.ls()}"]
        (target / "state.txt").write_text("\n".join(state))
    except Exception as exc:  # noqa: BLE001
        (target / "state.txt").write_text(f"state dump failed: {exc}")

    print(f"\n[ui-artifacts] failure diagnostics written to {target}")


GOLDENS_DIR = Path(__file__).parent / "goldens"


def pytest_addoption(parser):
    parser.addoption(
        "--accept-goldens", action="store_true", default=False,
        help="Overwrite golden images with the current rendering. Review the diff first.",
    )


@pytest.fixture
def golden(request):
    """Assert an image matches its golden. Name defaults to the test's name."""
    from helix.goldens import assert_golden

    accept = request.config.getoption("--accept-goldens")

    def _check(image, name: str | None = None) -> None:
        assert_golden(image, name or request.node.name,
                      goldens_dir=GOLDENS_DIR,
                      artifacts_dir=ARTIFACT_ROOT / request.node.name,
                      accept=accept)

    return _check
