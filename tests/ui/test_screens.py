# SPDX-License-Identifier: GPL-3.0-or-later

"""Golden captures for each reachable screen.

Screen tokens and their navigation are sourced from `scripts/screenshot-recipes.sh`
(the `SCREENSHOT_RECIPE` table) rather than re-transcribed here, so this corpus
can't drift from the table `screenshot.sh`/`screenshot-all.sh` already use to
reach each screen. This isn't really "parsing bash from Python": bash itself
sources the file and dumps its own associative array, so the only thing done
here in Python is splitting `"navigate x; click y"` into steps. That's more
robust than hand-rolling a parser for bash's quoting/comment syntax, and it
means a renamed or added recipe token shows up here automatically instead of
needing a second edit that someone eventually forgets to make.

Overlay/panel transitions must render instantly, not animate, or `freeze()`
can catch one mid-slide (see `_SUBSET`'s comment below for the details this
corpus depends on). That used to require a local `settings_animations_enabled`
override in this file; it's now guaranteed by `HelixApp.start()` itself
(`helix/app.py` writes `animations_enabled: false` into each instance's
private config dir before boot, via a literal minimal seed — not a copy of
the repo's own gitignored, machine-specific `config/settings-test.json`,
which is what let this regress silently on a fresh checkout, see
`docs/devel/UI_TESTING.md` § "Golden corpus scope"), so no
per-test workaround remains here — if animations ever come back on by
default, the right fix is back in `helix/app.py`, not a re-added fixture in
this file.
"""

from __future__ import annotations

import shlex
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
RECIPES_SCRIPT = REPO_ROOT / "scripts" / "screenshot-recipes.sh"


def _load_recipes() -> dict[str, str]:
    """Source screenshot-recipes.sh and dump its recipe table.

    Goes through the script's own two accessors rather than reading its data
    variable, so the storage stays the script's business. It used to poke
    ``${!SCREENSHOT_RECIPE[@]}`` directly, which meant this broke the moment
    that array did.
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
    # An empty table is always a harness fault, never a real state — and it
    # used to surface as `KeyError: 'settings'` at module scope, a hundred
    # lines from the cause. (bash 3.2 on macOS hit `declare -gA`, wrote an
    # error to stderr and still exited 0, so check=True saw success.)
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

# Subset of scripts/screenshot-recipes.sh's ~38 tokens. Chosen to prove the
# mechanism on a spread that's actually stable, not just plausible-looking:
# every one of these was verified byte-identical across at least 6 independent
# app boots (not just 3 quick frames within one capture) before being kept.
#
# Deliberately left out of THIS pass — not dropped silently, see the task-10
# report for full evidence — because they carry content that `freeze()`
# cannot pin down:
#
#   - `home`, `controls`, `filament`, `fan`: the mock backend's
#     `simulation_thread_` (moonraker_client_mock.cpp) drifts nozzle/bed/
#     chamber temps and the motor-idle timer on its own raw background
#     thread, invisible to `freeze()` (which only pauses LVGL timers/
#     animations) and to `wait_idle()` (which only tracks UpdateQueue/
#     HttpExecutor). `fan` looked stable in quick back-to-back checks but
#     failed across independent boots once — it's `card_cooling` on the same
#     Controls panel, not a separate overlay, so it shows the same
#     temperature card. `filament` additionally renders a usage chart with a
#     real-wall-clock x-axis (e.g. "9:40 PM"). This is exactly the gap the
#     design spec's `wait_idle` source table already names ("Mock backends
#     ... Mock mode adds nondeterminism") — a real hole in the determinism
#     story for any screen with a live numeric readout, not a bug in this
#     test.
#   - `console`: the gcode console echoes lines stamped with the real
#     wall-clock time the mock print ran, so its content is never the same
#     twice.
#   - `preflight-check`: the modal's dim backdrop is the Home panel, which
#     inherits the same temperature-jitter problem faintly through the scrim.
#   - `camera`: the "Connecting Camera..." state's spinner animates via its
#     own always-running `lv_anim` (independent of `settings_animations_enabled`,
#     the same category of issue the design spec flags for the print-select
#     loading spinner), so `freeze()` catches it at a different arc position
#     each time — confirmed as a small (~15px) but real diff across runs.
#   - `ams`: the "Bypass" spool icon's custom canvas fill graphic
#     (`ui_bypass_spool_widget.cpp`) renders 322 px (0.08%) differently than
#     the committed golden, isolated to that one icon's curved edge. This one
#     took two passes to root-cause — recorded in full because the bisection
#     was the expensive part and shouldn't have to be redone.
#
#     First pass wrongly concluded "rasterizer precision" after disproving an
#     async-race hypothesis (`Application::sync_external_spool` populates the
#     spool assignment via a queued UI-thread callback, not synchronously at
#     boot — but `ams_external_spool_color` already reads the synced value,
#     `1710638`/mock spool #1's "Jet Black" PLA, within ~1-2s of boot, well
#     before any capture, so there's no race window) and a weight-driven-fill
#     hypothesis (`fill_level` is a hardcoded `0.75` whenever a spool is
#     assigned, not derived from `remaining_weight_g` at all). Both correctly
#     disproven, but "not those two, so it must be the renderer" was a leap
#     the evidence didn't support — flagged from outside and worth taking
#     seriously rather than defended.
#
#     Second pass: booting with the exact `settings-test.json` the golden was
#     originally captured under (before this suite stopped copying that
#     gitignored file — see `HelixApp`'s docstring) reproduces the golden at
#     **0 diff**. So it IS config-state after all — just not the color.
#     Bisecting `printers.default` down to find which key:
#
#       | seed contents                                          | diff (px) |
#       |---------------------------------------------------------|-----------|
#       | full `printers.default`, minus `filament_sensors`       | 0         |
#       | full `printers.default`, minus `filament` entirely      | 322       |
#       | `filament.external_spool: {assigned: true}` only        | 322       |
#       | `...{assigned: true, spoolman_id: 1}` (matches mock)     | 567       |
#       | full `filament.external_spool` (assigned, spoolman_id,  | 0         |
#       |   color_rgb, material, spool_name, weights — all        |           |
#       |   matching the synced identity)                         |           |
#
#     `ams_external_spool_color` reads identically (`1710638`) in every one
#     of these — so the color was never the variable. The actual mechanism:
#     `AmsState::set_external_spool_info()`'s sync guard skips re-fetching
#     when `existing->spoolman_id` already matches. A seed with the full
#     block pre-populated makes the sync skip — the bypass widget's
#     `refresh_bypass_display()` runs exactly ONCE, synchronously, with final
#     values. An empty/partial seed makes the sync proceed — the widget
#     builds once with default/empty values, then a SECOND
#     `refresh_bypass_display()` fires once the async callback lands, ending
#     at the identical final color (`1710638`) and fill (`0.75`) either way.
#     Despite that, the twice-refreshed canvas differs from the
#     once-refreshed one by 322 px at the edge. Checked for stale
#     compositing (a redraw that doesn't clear before repainting) as the
#     obvious explanation for a refresh-count-dependent result — ruled out:
#     `ui_spool_canvas.cpp`'s redraw calls `lv_canvas_fill_bg(..., LV_OPA_TRANSP)`
#     unconditionally on every pass, before either draw. Not root-caused
#     further than that.
#
#     The golden CAN be made to reproduce byte-identically — pre-populate
#     `printers.default.filament.external_spool` in `_TEST_SEED_SETTINGS`
#     with the exact synced identity (assigned, spoolman_id=1,
#     color_rgb=1710638, material="PLA", spool_name="Polymaker PLA - Jet
#     Black", the weights). This was deliberately NOT done: it passes only
#     because it makes the app skip the second `refresh_bypass_display()`
#     call, not because the harness needs that specific spool identity — it
#     would test less than the suite tests today, and it's fake business
#     data standing in for a rendering setting, which is exactly the seed's
#     line ("only what a fresh install needs") from ballooning back into
#     accumulated fixture state. If a future reader finds this same
#     "fix" — don't apply it without addressing the double-refresh first.
#
#     Net: this is a genuine, if purely cosmetic, application defect — a
#     widget that renders 322 px differently depending on how many times it
#     was refreshed to reach the *same* final state, not on what that state
#     is. Worth its own bug report (ask Preston/whoever triages next); not
#     filed as part of this pass. `tests/ui/goldens/ams.png` is left in
#     place, untouched, for whenever the widget gets fixed.
#
# Kept: every base panel except the temp-bearing ones above, a representative
# handful of overlays reached through their real click handlers (each a
# full-screen replacement with no backdrop bleed-through), and `print-select`,
# which needs the extra `wait_for()` step in `_POST_NAV_WAIT_SUBJECT` below
# before it's safe to capture.
_SUBSET = [
    "settings", "advanced", "print-select",
    "motion", "bed-mesh", "zoffset", "macros",
]

SCREENS = [(name, _steps_for(_RECIPES[name])) for name in _SUBSET]

# Screens whose correct rendering depends on a one-time async event that
# `wait_idle()`/`freeze()` cannot see, keyed to a (subject, value) `wait_for()`
# can block on.
#
# `print-select`: `UsbBackendMock::start()` (usb_backend_mock.cpp) spawns a
# background thread that inserts a demo USB drive exactly 1.5s after boot — a
# fixed delay, not open-ended jitter. `PrintSelectUsbSource::on_drive_inserted()`
# (fixed in 232985fed — Rule #2, no more imperative `lv_obj_*_flag` calls here)
# now writes `print_source_usb_present`, which `wait_for()` can block on
# directly. A capture taken before 1.5s has elapsed since boot catches the
# source-selector row still hidden (content occupies the space instead,
# shifted up); one taken after shows the row. The row-visible state is the
# correct, final one — the mock USB drive is present from boot in every real
# sense, just reported with a startup latency — and is what got reviewed and
# approved; the row-hidden state is a race loss, not an alternate rendering.
# Confirmed via matching log lines ("Source selector configured (hidden
# until USB drive inserted)" at boot, "USB drive inserted - showing source
# selector" ~1.5s later): 6+ independent boots, each captured only once
# `wait_for` confirms the subject, produced byte-identical images, matching
# (byte-for-byte) the original human-approved candidate — reconfirmed after
# switching from a client-side `geom()` poll to this `wait_for()` call.
_POST_NAV_WAIT_SUBJECT = {
    "print-select": ("print_source_usb_present", 1),
}


@pytest.mark.parametrize("name,steps", SCREENS, ids=[s[0] for s in SCREENS])
def test_screen_matches_golden(helix_app, golden, name, steps):
    for method, *args in steps:
        getattr(helix_app, method)(*args)
    wait_target = _POST_NAV_WAIT_SUBJECT.get(name)
    if wait_target:
        helix_app.wait_for(*wait_target)
    helix_app.wait_idle()
    helix_app.freeze()
    try:
        image = helix_app.capture(stable=True)
    finally:
        helix_app.unfreeze()
    golden(image, name)
