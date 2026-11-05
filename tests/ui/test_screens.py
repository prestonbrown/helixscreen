# SPDX-License-Identifier: GPL-3.0-or-later

"""Golden captures for each reachable screen.

Screen tokens and their navigation come from `scripts/screenshot-recipes.sh`
(the `SCREENSHOT_RECIPE` table), so this corpus and `screenshot.sh` reach each
screen the same way. Bash sources the script and prints the table through its
own accessors; Python only splits `"navigate x; click y"` into steps.

Every capture assumes overlay/panel transitions render instantly.
`HelixApp.start()` (`helix/app.py`) seeds `animations_enabled: false` into each
instance's private config dir; if animations ever come back on by default, the
fix belongs there, not in a fixture here.
"""

from __future__ import annotations

import shlex
import subprocess
from pathlib import Path

import pytest

from conftest import BINARY
from helix.app import HelixApp
from helix.goldens import compare_images

REPO_ROOT = Path(__file__).resolve().parents[2]
RECIPES_SCRIPT = REPO_ROOT / "scripts" / "screenshot-recipes.sh"


def _load_recipes() -> dict[str, str]:
    """Source screenshot-recipes.sh and dump its recipe table.

    Goes through the script's own two accessors rather than reading its data
    variable, so the storage stays the script's business.
    """
    script = (
        f"source {shlex.quote(str(RECIPES_SCRIPT))}; "
        'for k in $(screenshot_recipe_tokens); do '
        'printf "%s\\t%s\\n" "$k" "$(screenshot_recipe_for "$k")"; done'
    )
    result = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                            check=True, cwd=REPO_ROOT)
    recipes: dict[str, str] = {}
    for line in result.stdout.splitlines():
        token, _, recipe = line.partition("\t")
        recipes[token] = recipe
    # An empty table is always a harness fault, never a real state. bash can
    # refuse a declaration, write to stderr and still exit 0, so check=True
    # alone does not catch it.
    if not recipes:
        raise RuntimeError(
            f"{RECIPES_SCRIPT} yielded no recipes — sourcing it produced nothing.\n"
            f"bash: {result.stderr.strip() or '(no stderr)'}")
    return recipes


def _steps_for(recipe: str) -> list[tuple]:
    """Turn 'navigate controls; click btn_motion' into ctl-call step tuples."""
    steps = []
    for clause in recipe.split(";"):
        parts = clause.split()
        if parts:
            steps.append(("ctl", *parts))
    return steps


_RECIPES = _load_recipes()

# Subset of scripts/screenshot-recipes.sh's tokens. Every (screen, variant) pair
# here is byte-identical across independent app boots. The rest stay out
# because their content is not a function of the boot:
#
#   - `home`, `controls`, `filament`, `fan`: the mock's `simulation_thread_`
#     (moonraker_client_mock_temperature.cpp) drifts temperatures and the motor-idle timer.
#     `freeze()` parks that thread from the moment it is called, so the value
#     it parks on is wherever the drift had reached, and `wait_idle()` has
#     nothing to gate on. `fan` is a card on the same Controls panel and shows
#     the same temperature card; `filament` also charts usage against the wall
#     clock.
#   - `console`: lines are stamped with the wall-clock time.
#   - `preflight-check`: the modal's dim backdrop is the Home panel, whose
#     temperature drift shows faintly through the scrim.
#   - `camera`: the "Connecting Camera..." spinner runs its own `lv_anim`,
#     independent of `animations_enabled`, so `freeze()` catches an arbitrary
#     arc position.
#   - `ams`: the Bypass spool icon (`ui_bypass_spool_widget.cpp`) renders about
#     300 px differently depending on whether its canvas was refreshed once or
#     twice to reach the same final colour and fill, and the external-spool
#     sync decides that count. `tests/ui/goldens/ams.png` waits for that widget
#     to render the same either way.
_SUBSET = [
    "settings", "advanced", "print-select",
    "motion", "bed-mesh", "zoffset", "macros",
]

# Extra command-line args per golden variant, one app boot per variant. The
# "" variant is the default 800x480 dark rendering and keeps the unsuffixed
# golden names; every other variant's golden is `<screen>@<variant>.png`.
_VARIANTS = {
    "": [],
    "small": ["-s", "small"],
    "large": ["-s", "large"],
    "light": ["--light"],
}

CASES = [(variant, name, _steps_for(_RECIPES[name]))
         for variant in _VARIANTS for name in _SUBSET]


def _golden_name(variant: str, name: str) -> str:
    return f"{name}@{variant}" if variant else name


@pytest.fixture
def helix_app(variant, tmp_path_factory):
    """A fresh app per case, shadowing conftest's shared instance in this module.

    A panel's own state survives `ctl reset`, so in a shared app each capture
    depends on which screens ran before it.
    """
    if not BINARY.exists():
        pytest.skip(f"{BINARY} not built - run `make -j`")
    workdir = tmp_path_factory.mktemp(f"helix-{variant or 'default'}")
    app = HelixApp(binary=BINARY,
                   socket_path=workdir / "control.sock",
                   log_path=workdir / "app.log",
                   extra_args=_VARIANTS[variant])
    with app:
        yield app


# Screens whose correct rendering depends on a one-time async event that
# `wait_idle()`/`freeze()` cannot see, keyed to a (subject, value) `wait_for()`
# can block on.
#
# `print-select`: `UsbBackendMock::start()` (usb_backend_mock.cpp) inserts a
# demo USB drive 1.5s after boot, and `PrintSelectUsbSource::on_drive_inserted()`
# sets `print_source_usb_present`, which shows the Printer/USB source selector.
# The selector-visible state is the settled one. The selector is exactly as
# tall as the view toggle beside it, so the card grid does not move when it
# appears; only the selector itself depends on this wait.
_POST_NAV_WAIT_SUBJECT = {
    "print-select": ("print_source_usb_present", 1),
}

# freeze() pauses every LVGL timer it finds armed (remote_control_server.cpp's
# handle_freeze walks lv_timer_get_next()), including a panel's own one-shot
# debounce timer if it is still pending, e.g. PrintSelectPanel::refresh_timer_,
# armed from inside a queued thumbnail-fetch callback and not itself visible to
# wait_idle() (neither UpdateQueue, HttpExecutor, nor ThumbnailProcessor work).
# A timer paused before it fires never fires, so a capture can freeze on the
# card grid's pre-refresh layout. _capture_settled() re-unfreezes between
# attempts, giving a stranded timer its next chance to run, and only accepts
# a capture once two consecutive attempts agree pixel for pixel.
_SETTLE_MAX_ATTEMPTS = 5


def _capture_settled(helix_app):
    previous = None
    for _ in range(_SETTLE_MAX_ATTEMPTS):
        helix_app.wait_idle()
        helix_app.freeze()
        try:
            image = helix_app.capture(stable=True)
        finally:
            helix_app.unfreeze()
        if previous is not None and compare_images(image, previous).matches:
            return image
        previous = image
    raise RuntimeError(
        f"screen never settled across {_SETTLE_MAX_ATTEMPTS} freeze/unfreeze "
        "cycles - two consecutive captures never matched"
    )


@pytest.mark.parametrize("variant,name,steps", CASES,
                         ids=[_golden_name(v, n) for v, n, _ in CASES])
def test_screen_matches_golden(helix_app, golden, variant, name, steps):
    for method, *args in steps:
        getattr(helix_app, method)(*args)
    wait_target = _POST_NAV_WAIT_SUBJECT.get(name)
    if wait_target:
        helix_app.wait_for(*wait_target)
    image = _capture_settled(helix_app)
    golden(image, _golden_name(variant, name))
