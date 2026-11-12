#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The doc-image manifest (docs/images/doc-screenshots.yml) and its capture.

Every image the docs show has one entry: an `app` entry the headless app can
regenerate (a recipe token from scripts/screenshot-recipes.sh or explicit
`helix-screen ctl` steps, an optional crop to a named widget and numbered
markers anchored to named widgets), or a `manual` entry naming where the image
comes from and how to recapture it.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml

REPO_ROOT = Path(__file__).resolve().parents[1]
RECIPES_SCRIPT = REPO_ROOT / "scripts" / "screenshot-recipes.sh"
MANIFEST = REPO_ROOT / "docs" / "images" / "doc-screenshots.yml"

KINDS = ("app", "manual")
ANCHORS = ("center", "top_left", "top_right", "bottom_left", "bottom_right")
APP_FIELDS = ("recipe", "steps", "env", "size", "crop", "margin", "markers")
DEFAULT_SIZE = (800, 480)
DEFAULT_MARGIN = 12


class ManifestError(Exception):
    """A manifest entry that cannot be used, named by its path."""

    def __init__(self, entry_path: str, reason: str):
        super().__init__(f"{entry_path}: {reason}")
        self.entry_path = entry_path
        self.reason = reason


@dataclass(frozen=True)
class Marker:
    n: int
    widget: str
    at: str = "top_right"


@dataclass(frozen=True)
class Entry:
    path: str
    kind: str
    recipe: Optional[str] = None
    steps: tuple = ()
    env: dict = field(default_factory=dict)
    size: tuple = DEFAULT_SIZE
    crop: Optional[str] = None
    margin: int = DEFAULT_MARGIN
    markers: tuple = ()
    source: Optional[str] = None
    recapture: Optional[str] = None


def recipe_tokens(script: Path = RECIPES_SCRIPT) -> list[str]:
    """Every token in the screenshot recipe table, in file order."""
    out = subprocess.run(
        ["bash", "-c", 'source "$1" && screenshot_recipe_tokens', "_", str(script)],
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def _parse_size(path: str, raw) -> tuple:
    if raw is None:
        return DEFAULT_SIZE
    m = re.fullmatch(r"(\d+)x(\d+)", str(raw))
    if not m:
        raise ManifestError(path, f"size '{raw}' is not WxH")
    return (int(m.group(1)), int(m.group(2)))


def _parse_markers(path: str, raw) -> tuple:
    markers = []
    seen = set()
    for item in raw or []:
        if not isinstance(item, dict) or "n" not in item or "widget" not in item:
            raise ManifestError(path, "a marker needs n and widget")
        n = int(item["n"])
        if n in seen:
            raise ManifestError(path, f"marker {n} appears twice")
        seen.add(n)
        at = item.get("at", "top_right")
        if at not in ANCHORS:
            raise ManifestError(path, f"marker {n} anchor '{at}' is not one of {', '.join(ANCHORS)}")
        markers.append(Marker(n=n, widget=str(item["widget"]), at=at))
    return tuple(markers)


def _parse_entry(raw: dict, tokens: set) -> Entry:
    path = raw.get("path")
    if not path:
        raise ManifestError("<entry>", "missing path")
    kind = raw.get("kind")
    if kind not in KINDS:
        raise ManifestError(path, f"kind '{kind}' is not one of {', '.join(KINDS)}")
    if kind == "manual":
        stray = [f for f in APP_FIELDS if f in raw]
        if stray:
            raise ManifestError(path, f"a manual entry carries app fields: {', '.join(stray)}")
        for need in ("source", "recapture"):
            if not raw.get(need):
                raise ManifestError(path, f"a manual entry needs {need}")
        return Entry(path=path, kind=kind, source=raw["source"], recapture=raw["recapture"])

    recipe, steps = raw.get("recipe"), raw.get("steps")
    if recipe and steps:
        raise ManifestError(path, "give a recipe or steps, not both")
    if not recipe and not steps:
        raise ManifestError(path, "an app entry needs a recipe or steps")
    if recipe and recipe not in tokens:
        raise ManifestError(path, f"recipe token '{recipe}' is not in scripts/screenshot-recipes.sh")
    return Entry(
        path=path,
        kind=kind,
        recipe=recipe,
        steps=tuple(str(s) for s in steps or ()),
        env={str(k): str(v) for k, v in (raw.get("env") or {}).items()},
        size=_parse_size(path, raw.get("size")),
        crop=raw.get("crop"),
        margin=int(raw.get("margin", DEFAULT_MARGIN)),
        markers=_parse_markers(path, raw.get("markers")),
    )


def load_manifest(manifest: Path = MANIFEST, tokens: Optional[list] = None) -> list[Entry]:
    """Every entry of @p manifest, validated; raises ManifestError on the first bad one."""
    data = yaml.safe_load(Path(manifest).read_text()) or {}
    known = set(tokens if tokens is not None else recipe_tokens())
    entries = []
    seen = set()
    for raw in data.get("images") or []:
        entry = _parse_entry(raw, known)
        if entry.path in seen:
            raise ManifestError(entry.path, "duplicate path")
        seen.add(entry.path)
        entries.append(entry)
    return entries


if __name__ == "__main__":
    for e in load_manifest():
        print(f"{e.kind:6} {e.path}")
