# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/catch2_shard_tests.py.

The shard diagnostics write this file so a failing shard can be replayed with
`helix-tests --input-file`. Catch2 splits a spec on commas and reads [] and *
structurally, so a name carrying any of them has to arrive escaped or the
replay silently selects a different set of tests than the shard ran.
"""

import io
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from catch2_shard_tests import escape, main  # noqa: E402


def _run(xml: str, monkeypatch) -> list[str]:
    monkeypatch.setattr(sys, "stdin", io.StringIO(xml))
    out = io.StringIO()
    monkeypatch.setattr(sys, "stdout", out)
    assert main() == 0
    return out.getvalue().splitlines()


def _listing(*names: str) -> str:
    cases = "".join(f"<TestCase><Name>{n}</Name></TestCase>" for n in names)
    return f'<?xml version="1.0"?><MatchingTests>{cases}</MatchingTests>'


@pytest.mark.parametrize(
    "raw,escaped",
    [
        ("plain name", "plain name"),
        ("width alone, and stream start/stop", "width alone\\, and stream start/stop"),
        ("tool_switcher: compact, pill-row", "tool_switcher: compact\\, pill-row"),
        ("a [bracketed] name", "a \\[bracketed\\] name"),
        ("a * star", "a \\* star"),
        ("back\\slash", "back\\\\slash"),
    ],
)
def test_escapes_every_spec_metacharacter(raw, escaped):
    assert escape(raw) == escaped


def test_backslash_is_escaped_before_the_escapes_it_would_swallow():
    # Escaping the comma first and the backslash second would turn "a,b" into
    # "a\\,b", where the backslash escapes itself and the comma splits again.
    assert escape("a,b") == "a\\,b"
    assert escape("a\\,b") == "a\\\\\\,b"


def test_one_line_per_test_case(monkeypatch):
    lines = _run(_listing("first", "second", "third"), monkeypatch)
    assert lines == ["first", "second", "third"]


def test_tags_and_wrapped_names_cannot_leak_into_the_count(monkeypatch):
    # The whole reason for reading XML: the human listing indents tags under
    # each name and wraps long ones, so its line count is not a test count.
    long_name = "a name long enough that the human listing would wrap it across lines"
    lines = _run(_listing(long_name, "short"), monkeypatch)
    assert lines == [long_name, "short"]


def test_xml_entities_are_decoded(monkeypatch):
    lines = _run(_listing("a &amp; b &lt;c&gt;"), monkeypatch)
    assert lines == ["a & b <c>"]


def test_empty_listing_is_not_an_error(monkeypatch):
    assert _run("", monkeypatch) == []


def test_unparseable_listing_reports_failure(monkeypatch):
    monkeypatch.setattr(sys, "stdin", io.StringIO("<MatchingTests"))
    monkeypatch.setattr(sys, "stdout", io.StringIO())
    assert main() == 1
