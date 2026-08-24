#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if a platform's font sources can't satisfy its compiled-in font guards.

Two halves have to agree and nothing makes them:

  * asset_manager.cpp / cjk_font_manager.cpp reference tier faces behind
    `#if HELIX_MAX_FONT_TIER >= N`. That is a THRESHOLD.
  * mk/fonts.mk assembles TIER_FONT_SRCS from the platform's declared
    FONT_TIERS. That is a SET.
  * mk/cross.mk derives HELIX_MAX_FONT_TIER from the HIGHEST declared tier.

A platform that skips a middle tier makes those disagree. k2 declares
"large xlarge", so its max is 5, so `>= 3` compiles a reference to
noto_sans_26 -- but "medium" is not in its set, so FONTS_MEDIUM is not in its
sources and the link fails with an undefined reference.

Nothing catches that on x86: the guards are all true there, and only the
release matrix cross-builds k2, so the break would surface long after the
commit that caused it. Hence a gate.

Checks every platform in mk/cross.mk: for each guarded tier N, if the
platform's max tier is >= N, tier N's faces must be in its sources.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FONTS_MK = REPO / "mk" / "fonts.mk"
CROSS_MK = REPO / "mk" / "cross.mk"
ASSET_MGR = REPO / "src" / "application" / "asset_manager.cpp"
CJK_MGR = REPO / "src" / "system" / "cjk_font_manager.cpp"

TIER_ORDER = ["micro", "tiny", "small", "medium", "large", "xlarge", "xxlarge"]
TIER_INDEX = {name: i for i, name in enumerate(TIER_ORDER)}


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="ignore")


def parse_tier_lists(text: str) -> dict[str, set[str]]:
    """FONTS_<TIER> := ... -> {tier: {face, ...}} (face = basename, no .c)."""
    out: dict[str, set[str]] = {}
    for tier in TIER_ORDER:
        m = re.search(rf"^FONTS_{tier.upper()}\s*:=(.*?)(?=^\S|\Z)", text, re.M | re.S)
        if not m:
            continue
        out[tier] = set(re.findall(r"fonts/([A-Za-z0-9_]+)\.c", m.group(1)))
    return out


def parse_core(text: str) -> set[str]:
    m = re.search(r"^FONTS_CORE\s*:=(.*?)(?=^\S|\Z)", text, re.M | re.S)
    return set(re.findall(r"fonts/([A-Za-z0-9_]+)\.c", m.group(1))) if m else set()


def parse_platform_tiers(text: str) -> dict[str, list[str]]:
    """Map platform -> declared FONT_TIERS list, walking cross.mk in order."""
    platforms: dict[str, list[str]] = {}
    current = None
    for line in text.splitlines():
        m = re.search(r"PLATFORM_TARGET\),([A-Za-z0-9_-]+)\)", line)
        if m:
            current = m.group(1)
        m = re.match(r"\s*FONT_TIERS\s*:=\s*(.+?)\s*$", line)
        if m and current:
            platforms[current] = m.group(1).split()
    return platforms


def guarded_tiers(*sources: Path) -> set[int]:
    """Tier numbers referenced as `#if HELIX_MAX_FONT_TIER >= N`."""
    found: set[int] = set()
    for src in sources:
        if src.exists():
            found.update(int(n) for n in re.findall(
                r"#if\s+HELIX_MAX_FONT_TIER\s*>=\s*(\d+)", read(src)))
    return found


def parse_selection_rules(text: str) -> dict[str, set[str]]:
    """Read the ACTUAL selection conditions out of mk/fonts.mk.

    Returns {tier: {tier names that trigger it}} from blocks shaped like

        ifneq ($(filter medium large xlarge xxlarge,$(FONT_TIERS)),)
            TIER_FONT_SRCS += $(FONTS_MEDIUM)
        endif

    Parsing the real condition (rather than reimplementing what it ought to be)
    is the whole point: a gate that models the intent instead of the file cannot
    fail when the file drifts from the intent, which is the exact bug this
    exists to catch.
    """
    rules: dict[str, set[str]] = {}
    for cond, tier in re.findall(
        r"ifneq\s*\(\$\(filter\s+([^,]+),\$\(FONT_TIERS\)\),\)\s*\n"
        r"\s*TIER_FONT_SRCS\s*\+=\s*\$\(FONTS_([A-Z]+)\)",
        text,
    ):
        rules[tier.lower()] = set(cond.split())
    return rules


def selected_sources(declared: list[str], tiers: dict[str, set[str]], core: set[str],
                     rules: dict[str, set[str]]) -> set[str]:
    """Reproduce TIER_FONT_SRCS using the conditions read from mk/fonts.mk."""
    if declared == ["all"]:
        return core.union(*tiers.values())
    picked = set(core)
    for tier, faces in tiers.items():
        trigger = rules.get(tier)
        if trigger and trigger.intersection(declared):
            picked |= faces
    return picked


def main() -> int:
    fonts_txt = read(FONTS_MK)
    tiers = parse_tier_lists(fonts_txt)
    core = parse_core(fonts_txt)
    platforms = parse_platform_tiers(read(CROSS_MK))
    rules = parse_selection_rules(fonts_txt)
    guards = guarded_tiers(ASSET_MGR, CJK_MGR)

    if not platforms or not tiers or not guards or not rules:
        print("check_font_tier_coverage: could not parse inputs "
              f"(platforms={len(platforms)} tiers={len(tiers)} rules={len(rules)} guards={sorted(guards)})",
              file=sys.stderr)
        return 2

    failures = []
    for plat, declared in sorted(platforms.items()):
        max_idx = 6 if declared == ["all"] else max(
            (TIER_INDEX[t] for t in declared if t in TIER_INDEX), default=6)
        have = selected_sources(declared, tiers, core, rules)
        for n in sorted(guards):
            if max_idx < n:
                continue  # guard is false here; faces legitimately absent
            missing = tiers.get(TIER_ORDER[n], set()) - have
            if missing:
                failures.append(
                    f"  {plat}: FONT_TIERS='{' '.join(declared)}' -> max tier {max_idx}, "
                    f"so `#if HELIX_MAX_FONT_TIER >= {n}` compiles, but tier "
                    f"'{TIER_ORDER[n]}' faces are absent from its sources: "
                    f"{', '.join(sorted(missing))}")

    if failures:
        print("Font tier coverage FAILED — these platforms would not link:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("\nFix: mk/fonts.mk must select a tier when the platform's MAX tier is >= it,\n"
              "not only when that tier is named in FONT_TIERS.", file=sys.stderr)
        return 1

    print(f"✅ Font tier coverage: {len(platforms)} platforms satisfy guards "
          f"{sorted(guards)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
