# SPDX-License-Identifier: GPL-3.0-or-later

"""Stable capture and widget cropping."""

import os
from pathlib import Path

import pytest
from PIL import Image

from helix.app import HelixApp, HelixCtlError

# Mirrors conftest.py's BINARY: this file's own HELIX_MOCK_AUTO_PRINT test boots
# a private instance with boot-time flags (--sim-speed) the shared fixtures
# don't parametrize, so it can't just request `fresh_helix_app`.
_REPO_ROOT = Path(__file__).resolve().parents[2]
_BINARY = Path(os.environ.get("HELIX_UI_BINARY", str(_REPO_ROOT / "build" / "bin" / "helix-screen")))


def test_stable_capture_is_reproducible_when_frozen(helix_app, tmp_path):
    # Deliberately "settings", not "home". Home carries a live temperature
    # readout driven by moonraker_client_mock.cpp's temperature_simulation_loop
    # — a perpetual sine wave on a raw background thread, invisible to
    # freeze() (LVGL timers/animations only). Targeting Home here used to make
    # this test intermittently assert something we've documented as
    # impossible: two frozen captures of a screen that never stops changing
    # matching byte-for-byte. Confirmed via a controlled A/B under heavy load
    # (identical logic, identical load, only the screen differs): Home failed,
    # Settings passed 20/20. See test_screens.py's deferred-screens comment for
    # the full list of screens this applies to — don't retarget this back to
    # one of them.
    helix_app.navigate("settings")
    helix_app.wait_idle()
    helix_app.freeze()
    try:
        a = helix_app.capture(stable=True)
        b = helix_app.capture(stable=True)
    finally:
        helix_app.unfreeze()
    assert a.tobytes() == b.tobytes(), "two frozen captures of the same screen differ"


def test_target_crop_is_smaller_than_the_full_screen(helix_app):
    helix_app.navigate("controls")
    helix_app.wait_idle()
    helix_app.freeze()
    try:
        full = helix_app.capture(stable=True)
        cropped = helix_app.capture(target="btn_motion", stable=True)
    finally:
        helix_app.unfreeze()
    assert cropped.width < full.width or cropped.height < full.height
    assert cropped.width > 0 and cropped.height > 0


def test_crop_matches_the_widgets_reported_geometry(helix_app):
    helix_app.navigate("controls")
    helix_app.wait_idle()
    g = helix_app.geom("btn_motion")["widgets"][0]
    helix_app.freeze()
    try:
        cropped = helix_app.capture(target="btn_motion", stable=True)
    finally:
        helix_app.unfreeze()
    assert (cropped.width, cropped.height) == (g["w"], g["h"])


def test_crop_reads_from_the_widgets_actual_position_not_just_its_size(helix_app):
    # The size-only assertions above would still pass if the crop read the
    # right *dimensions* from the wrong *offset* (e.g. a swapped x/y, or a
    # crop anchored at the screen origin instead of the widget's own
    # position). Prove the pixel content is right, not just its shape: crop
    # the full-screen capture in PIL using geom's x/y/w/h (screen-absolute,
    # like capture_frame()'s own crop_to) and byte-compare against the
    # server's own --target crop.
    helix_app.navigate("controls")
    helix_app.wait_idle()
    g = helix_app.geom("btn_motion")["widgets"][0]
    helix_app.freeze()
    try:
        full = helix_app.capture(stable=True)
        cropped = helix_app.capture(target="btn_motion", stable=True)
    finally:
        helix_app.unfreeze()
    expected = full.crop((g["x"], g["y"], g["x"] + g["w"], g["y"] + g["h"]))
    assert cropped.tobytes() == expected.tobytes()


def test_stable_times_out_with_an_actionable_message_on_a_genuinely_moving_screen(tmp_path):
    # Every other --stable test in this file calls freeze() first, so the
    # server's poll loop matches on its very first repeated hash — the
    # multi-sample branch and the timeout throw are never actually executed,
    # only inspected by reading the code. Drive a screen that keeps
    # genuinely repainting on its own: a live mock print with animations
    # re-enabled (`HelixApp.start()` seeds each instance's private config dir
    # from config/settings-test.json, whose `animations_enabled: false` is
    # what needs overriding here — see the seeding comment in helix/app.py
    # for why that seeding step exists at all).
    # PrintStatusPanel restarts a ~300ms ease-out progress-bar animation on
    # every mock physics tick (~250ms, independent of --sim-speed), so
    # pixels never hold still for 3 consecutive 16ms samples — this
    # reliably drives the timeout branch rather than merely asserting it
    # exists.
    #
    # Boots its own instance (HELIX_MOCK_AUTO_PRINT env + --sim-speed) since
    # this needs boot-time flags the shared `helix_app`/`fresh_helix_app`
    # fixtures don't support, and dirties env/settings state a shared
    # instance shouldn't carry into other tests.
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built — run `make -j`")

    env_before = os.environ.get("HELIX_MOCK_AUTO_PRINT")
    os.environ["HELIX_MOCK_AUTO_PRINT"] = "1"
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log",
                       extra_args=["--sim-speed", "8"])
        with app:
            app.set("settings_animations_enabled", 1)
            app.wait_for("print_progress", 1, timeout=15)
            with pytest.raises(HelixCtlError) as exc:
                app.screenshot(str(tmp_path / "moving.png"), stable=True)
            message = exc.value.message
            assert "never stabilized" in message, message
            assert "freeze" in message, message
    finally:
        if env_before is None:
            os.environ.pop("HELIX_MOCK_AUTO_PRINT", None)
        else:
            os.environ["HELIX_MOCK_AUTO_PRINT"] = env_before


def test_png_path_matches_the_active_screens_dimensions(helix_app, tmp_path):
    # `img.width > 0 and img.height > 0` holds for any PNG the encoder emits,
    # a 1x1 included, so it cannot see a crop or scale regression in the
    # untargeted full-screen path. Measure against the active screen's own
    # geometry instead: "@s" resolves to lv_screen_active() (resolve_path in
    # src/remote/widget_resolution.cpp) and its w/h come from
    # lv_obj_get_coords(), not from the capture, so the two are independent.
    screen = helix_app.geom("@s")["widgets"][0]
    out = tmp_path / "shot.png"
    helix_app.screenshot(str(out))
    with Image.open(out) as img:
        assert (img.width, img.height) == (screen["w"], screen["h"])
