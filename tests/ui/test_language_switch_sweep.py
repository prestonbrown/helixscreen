# SPDX-License-Identifier: GPL-3.0-or-later

"""Every screen re-translates its labels on a live language switch.

XML re-translates what it bound through translation_tag; text C++ set on a
label (or a tag C++ then overwrote) stays in the language it was written in.
Panels and most overlays are built once and kept, so a label that misses the
switch still reads English after it. This walks every screen the screenshot
recipes reach, in English, switches to Russian the way a user does, walks them
again, and reports each label whose English text is a translation key but
still reads English.
"""

from __future__ import annotations

import re
import shlex
from pathlib import Path

import pytest
import yaml

from helix.app import HelixCtlError

REPO_ROOT = Path(__file__).resolve().parents[2]
RECIPES = REPO_ROOT / "scripts" / "screenshot-recipes.sh"
RUSSIAN = REPO_ROOT / "translations" / "ru.yml"
LANGUAGES = REPO_ROOT / "src" / "system" / "system_settings_manager.cpp"

# Edit-mode callouts drive raw pointer gestures, not screens of their own.
SKIP = {"callouts-2x2", "callouts-4x2", "callouts-untagged"}


WORD = re.compile(r"[A-Za-z][A-Za-z'-]+")

# Keys whose text stays as-is inside composed labels: units, material and
# brand names that a label carries as data.
ALLOW = {"min", "mm", "PLA", "PETG", "ABS", "ASA", "TPU", "OK"}

TEXT_TYPES = {"label", "dropdown"}

# Widgets that show data: file, spool and AMS slot (spool) names, material and macro names, paths, the
# licenses text, LED and temperature sensor names from the printer's config, and the title, text and
# buttons a macro's action prompt supplies.
DATA_PATH = re.compile(
    r"licenses_text|row_install_root|loaded_material|macro_label|macro_desc|"
    r"filename_label|spool_name|ams_slot_view\[\d+\]/name_label|led_tab_\d+/0/tab_label|"
    r"temp_sensors_list/sensor_row(\[\d+\])?/0/0/name_label|"
    r"action_prompt_modal/.*(dialog_title|content_container|button_container|footer_container)")

# Fan names generated from the mock printer's config object names.
CONFIG_NAMES = {"Controller Fan", "Nevermore Fan"}


def _ru_index() -> int:
    """Position of "ru" in SystemSettingsManager's language list."""
    codes = re.search(r"kLanguageCodes\[\] = \{([^}]*)\}", LANGUAGES.read_text()).group(1)
    return re.findall(r'"(\w+)"', codes).index("ru")


def _translatable(en: str, ru: dict) -> tuple[str, str] | None:
    """The longest phrase of ``en`` that Russian translates, whole text first.

    An uppercase-transformed label shows its key uppercased, so a whole
    uppercase text also matches its key case-insensitively.
    """
    if ru.get(en) and ru[en] != en:
        return en, ru[en]
    if en.isupper():
        for key, want in ru.items():
            if want and want != key and key.upper() == en:
                return en, want
    words = WORD.findall(en)
    for n in range(len(words), 0, -1):
        for i in range(len(words) - n + 1):
            phrase = " ".join(words[i:i + n])
            want = ru.get(phrase)
            if phrase not in ALLOW and want and want != phrase:
                return phrase, want
    return None


def _recipes() -> list[tuple[str, list[list[str]]]]:
    text = RECIPES.read_text()
    body = text.split('SCREENSHOT_RECIPES="', 1)[1].split('"', 1)[0]
    recipes = []
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        token, _, steps = line.partition(" ")
        if token in SKIP:
            continue
        recipes.append((token, [shlex.split(s) for s in steps.split(";") if s.strip()]))
    return recipes


def _run(app, steps: list[list[str]]) -> bool:
    app.reset()
    app.wait_idle()
    for step in steps:
        try:
            app.ctl(*step)
        except HelixCtlError:
            return False  # not reachable on this mock; nothing to compare
        app.wait_idle()
    return True


def _labels(app) -> dict[str, str]:
    out = {}
    for widget in app.ls().get("widgets", []):
        if widget.get("type") not in TEXT_TYPES:
            continue
        try:
            out[widget["path"]] = app.text(widget["path"])
        except HelixCtlError:
            continue
    return out


@pytest.mark.slow
def test_every_screen_retranslates_on_a_language_switch(fresh_helix_app):
    app = fresh_helix_app
    ru = yaml.safe_load(RUSSIAN.read_text())["translations"]

    before = {}
    unreachable = []
    for token, steps in _recipes():
        if _run(app, steps):
            before[token] = _labels(app)
        else:
            unreachable.append(token)

    app.reset()
    app.navigate("settings")
    app.wait_idle()
    app.click("row_language_time")
    app.wait_idle()
    app.ctl("set_value", "row_language", _ru_index())
    app.wait_idle()

    stale = []
    checked = 0
    for token, steps in _recipes():
        if token not in before or not _run(app, steps):
            continue
        after = _labels(app)
        for path, en in before[token].items():
            if DATA_PATH.search(path) or en in CONFIG_NAMES:
                continue
            hit = _translatable(en, ru) if en.strip() else None
            if not hit:
                continue
            phrase, want = hit
            checked += 1
            now = after.get(path)
            if now is None:
                continue  # not on screen after the switch: rebuilt, nothing stale
            # An uppercase label must show its translation uppercased too.
            if en.isupper() and phrase == en:
                if now != want.upper():
                    stale.append(f"{token}: {path}: {en!r} reads {now!r}, want {want.upper()!r}")
                continue
            # Still exactly the English text is the stale signature; a different
            # rendering of the right translation (a count beside it) is not.
            # A composed label ("Tool 2", "Heat to 200°C") counts when any phrase
            # of it has a translation.
            if now == en:
                stale.append(f"{token}: {path}: {en!r} unchanged; {phrase!r} -> {want!r}")

    report = "\n".join(stale)
    print(f"\n{checked} translatable labels checked, {len(stale)} stale; "
          f"unreachable on this mock: {', '.join(unreachable) or 'none'}\n{report}")
    assert checked > 0
    assert not stale, f"{len(stale)} stale labels:\n{report}"
