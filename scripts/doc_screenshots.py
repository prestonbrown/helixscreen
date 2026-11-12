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

import os
import re
import shlex
import subprocess
import tempfile
import time
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


# --- capture -----------------------------------------------------------------


class CaptureError(Exception):
    """An app entry that could not be captured, named by its path."""

    def __init__(self, entry_path: str, reason: str):
        super().__init__(f"{entry_path}: {reason}")


@dataclass(frozen=True)
class Box:
    x: int
    y: int
    w: int
    h: int


def _anchor_point(box: Box, at: str) -> tuple:
    if at == "center":
        return (box.x + box.w // 2, box.y + box.h // 2)
    x = box.x if at.endswith("left") else box.x + box.w
    y = box.y if at.startswith("top") else box.y + box.h
    return (x, y)


def marker_points(entry_path: str, anchors: dict, crop: Optional[Box], markers) -> list:
    """(n, x, y) per marker, in the output image's pixels (relative to @p crop)."""
    points = []
    for m in markers:
        box = anchors.get(m.widget)
        if box is None:
            raise CaptureError(entry_path, f"marker {m.n}: widget '{m.widget}' is not on screen")
        x, y = _anchor_point(box, m.at)
        if crop is not None:
            x, y = x - crop.x, y - crop.y
        points.append((m.n, x, y))
    return points


def draw_markers(img, points, radius: int = 14):
    """Numbered circles: an accent fill with a white ring and white digits, which
    read on the light and the dark theme alike."""
    from PIL import ImageDraw, ImageFont

    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", int(radius * 1.2))
    except OSError:
        font = ImageFont.load_default()
    for n, x, y in points:
        x = min(max(x, radius + 2), img.width - radius - 2)
        y = min(max(y, radius + 2), img.height - radius - 2)
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=(230, 81, 0),
                     outline=(255, 255, 255), width=3)
        draw.text((x, y), str(n), fill=(255, 255, 255), font=font, anchor="mm")
    return img


def _box(geom: dict) -> Box:
    return Box(int(geom["x"]), int(geom["y"]), int(geom["w"]), int(geom["h"]))


def entry_steps(entry: Entry) -> list:
    """The ctl commands that bring the app to the entry's screen."""
    if entry.recipe:
        out = subprocess.run(
            ["bash", "-c", 'source "$1" && screenshot_recipe_for "$2"', "_", str(RECIPES_SCRIPT),
             entry.recipe],
            capture_output=True, text=True, check=True,
        )
        return [s.strip() for s in out.stdout.split(";") if s.strip()]
    return list(entry.steps)


def capture(entry: Entry, session, out_root: Path) -> Path:
    """Drive @p session to the entry's screen and write its image under @p out_root."""
    for step in entry_steps(entry):
        session.ctl(*shlex.split(step))
    with tempfile.TemporaryDirectory() as tmp:
        raw = Path(tmp) / "shot.png"
        session.ctl("screenshot", str(raw), "--stable")
        from PIL import Image

        img = Image.open(raw).convert("RGB")
        img.load()
    crop = None
    if entry.crop:
        try:
            c = _box(session.ctl("geom", entry.crop))
        except Exception as err:  # the ctl error names the widget
            raise CaptureError(entry.path, f"crop widget '{entry.crop}': {err}") from err
        x0, y0 = max(c.x - entry.margin, 0), max(c.y - entry.margin, 0)
        x1 = min(c.x + c.w + entry.margin, img.width)
        y1 = min(c.y + c.h + entry.margin, img.height)
        crop = Box(x0, y0, x1 - x0, y1 - y0)
        img = img.crop((x0, y0, x1, y1))
    anchors = {}
    for m in entry.markers:
        try:
            anchors[m.widget] = _box(session.ctl("geom", m.widget))
        except Exception:
            pass  # marker_points names the missing widget
    draw_markers(img, marker_points(entry.path, anchors, crop, entry.markers))
    out = Path(out_root) / entry.path
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    return out


class AppSession:
    """A headless --test instance with its own socket and config dir."""

    def __init__(self, env: dict, size: tuple, binary: Path = REPO_ROOT / "build/bin/helix-screen"):
        self.binary = Path(binary)
        if not self.binary.exists():
            raise SystemExit(f"{self.binary} is missing: build the app first (make)")
        self.env = env
        self.size = size
        self.tmp = Path(tempfile.mkdtemp(prefix="doc-shots-"))
        self.sock = self.tmp / "helix.sock"

    def __enter__(self):
        env = dict(os.environ)
        env.update(self.env)
        env.update({"HELIX_HEADLESS": "1", "SDL_VIDEODRIVER": "dummy", "HELIX_HOT_RELOAD": "0",
                    "HELIX_CONFIG_DIR": str(self.tmp / "config")})
        (self.tmp / "config").mkdir()
        w, h = self.size
        self.log = open(self.tmp / "app.log", "w")
        self.proc = subprocess.Popen(
            [str(self.binary), "--test", "--skip-wizard", "-s", f"{w}x{h}", "--remote-socket",
             str(self.sock)], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(120):
            if self.sock.exists() and self._try("ping"):
                time.sleep(2)
                return self
            time.sleep(0.5)
        self.__exit__(None, None, None)
        raise SystemExit(f"the app did not answer on {self.sock}; log: {self.tmp / 'app.log'}")

    def _try(self, *args) -> bool:
        r = subprocess.run([str(self.binary), "ctl", "-s", str(self.sock), *args],
                           capture_output=True, text=True)
        return r.returncode == 0

    def ctl(self, *args) -> dict:
        import json

        r = subprocess.run([str(self.binary), "ctl", "-s", str(self.sock), *args],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError((r.stderr or r.stdout).strip())
        try:
            return json.loads(r.stdout) if r.stdout.strip() else {}
        except ValueError:
            return {}

    def __exit__(self, *exc):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


def main(argv=None) -> int:
    import argparse
    import fnmatch

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--only", help="glob over entry paths")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)
    entries = load_manifest()
    if args.only:
        entries = [e for e in entries if fnmatch.fnmatch(e.path, args.only)]
    if args.list:
        for e in entries:
            print(f"{e.kind:6} {e.path}")
        return 0
    return run(entries, AppSession, REPO_ROOT)


def run(entries, session_factory, out_root: Path) -> int:
    """Capture every app entry; one session per (env, size) run. Prints counts."""
    captured = failed = skipped = 0
    session = None
    key = None
    try:
        for e in entries:
            if e.kind == "manual":
                skipped += 1
                continue
            k = (tuple(sorted(e.env.items())), e.size)
            if k != key:
                if session is not None:
                    session.__exit__(None, None, None)
                session = session_factory(e.env, e.size).__enter__()
                key = k
            try:
                session.ctl("navigate", "home")
                capture(e, session, out_root)
                captured += 1
                print(f"ok      {e.path}")
            except (CaptureError, RuntimeError) as err:
                failed += 1
                print(f"FAILED  {e.path}: {err}")
    finally:
        if session is not None:
            session.__exit__(None, None, None)
    print(f"captured {captured}, failed {failed}, skipped {skipped} (manual)")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
