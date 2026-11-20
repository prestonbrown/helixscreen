# SPDX-License-Identifier: GPL-3.0-or-later
"""Shipped Klipper config survives Klipper's own reader.

Klipper's configfile._parse_config cuts every line at its first '#' before
configparser sees it, quoted or not. A '#' inside a Jinja expression or a
quoted string therefore truncates the template mid-token, the macro fails to
compile, and Klipper halts at startup with the whole helper pack.
"""

from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SHIPPED = sorted((REPO_ROOT / "assets" / "config").glob("*.cfg"))


def klipper_reader_cuts(line):
    """The text Klipper drops from a line that holds code, or "" when nothing."""
    body = line.lstrip()
    if not body or body.startswith("#"):
        return ""
    pos = line.find("#")
    if pos < 0:
        return ""
    kept = line[:pos]
    if "{" in kept or '"' in kept or "'" in kept:
        return line[pos:]
    return ""


def test_shipped_configs_exist():
    assert SHIPPED, "assets/config/*.cfg moved; point this test at the shipped macro files"


@pytest.mark.parametrize("cfg", SHIPPED, ids=lambda p: p.name)
def test_no_hash_inside_jinja_or_strings(cfg):
    cut = [
        f"{cfg.name}:{n}: {line.strip()}"
        for n, line in enumerate(cfg.read_text().splitlines(), 1)
        if klipper_reader_cuts(line)
    ]
    assert not cut, (
        "Klipper cuts these lines at '#', breaking the template; spell it \"\\x23\":\n"
        + "\n".join(cut)
    )


def test_reader_cut_is_detected():
    assert klipper_reader_cuts("    {% set m = raw|replace('#', '') %}")
    assert not klipper_reader_cuts("    # a comment with '#' in it")
    assert not klipper_reader_cuts("variable_x: 1  # trailing comment")
