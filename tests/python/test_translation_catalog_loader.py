# SPDX-License-Identifier: GPL-3.0-or-later
"""The catalog loader of check_translation_format_specifiers.py.

It reads with libyaml when the interpreter has it and with PyYAML's own
scanner otherwise, so what it returns must not depend on which one ran.
"""

import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_translation_format_specifiers as check  # noqa: E402

# The shapes a catalog takes, and the ones two YAML readers could read apart:
# block scalars, every quoting style, escapes, folded text, comments, anchors,
# and keys that look like something other than strings.
CATALOG = """\
# a comment
translations:
  plain key: Plain value
  'single: quoted': 'it''s here'
  "double quoted": "line one\\nline two \\u00e9 \\x41"
  block: |
    first
      indented
  folded: >-
    folded
    text
  list: [a, b, c]
  nested:
    inner: value
  anchored: &a shared
  aliased: *a
  unicode: 中文 Русский °C
  '12': twelve
  'true': a string key
  percent: '%d of %d'
  braces: '{0} of {1}'
  empty:
  tilde: ~
"""


def test_the_catalog_loader_reads_what_safe_load_reads(tmp_path):
    path = tmp_path / "ru.yml"
    path.write_text(CATALOG, encoding="utf-8")
    expected = yaml.safe_load(CATALOG)

    assert check._load_catalog(path) == expected


def test_an_empty_catalog_is_an_empty_mapping(tmp_path):
    path = tmp_path / "empty.yml"
    path.write_text("# nothing here\n", encoding="utf-8")

    assert check._load_catalog(path) == {}
