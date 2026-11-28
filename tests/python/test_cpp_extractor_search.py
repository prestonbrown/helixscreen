# SPDX-License-Identifier: GPL-3.0-or-later
"""What the C++ extractor of the translation sync searches.

A sync read every C++ file with seven call-site patterns and four table scans,
3.3 s for the few files in a hundred that hold any of them. Each pattern and
scan now names a literal every one of its matches holds, and a file without it
is not searched. These tests pin that skip, so a later edit cannot quietly put
the passes back.
"""

import sys
from pathlib import Path
from textwrap import dedent

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from translations import cpp_tables, extractor  # noqa: E402

# None of the literals the extractor keys on appears in it.
PLAIN = dedent("""\
    void describe(const Record& record) {
        const char* kind = "section";
        std::vector<Row> rows;
        rows.push_back(record.row);
    }
""")


def test_a_file_holding_none_of_the_calls_is_not_searched(tmp_path, monkeypatch):
    path = tmp_path / "plain.cpp"
    path.write_text(PLAIN, encoding="utf-8")
    searched = []
    real_finditer = extractor.re.finditer

    def counting_finditer(pattern, string, *args, **kwargs):
        searched.append(pattern)
        return real_finditer(pattern, string, *args, **kwargs)

    monkeypatch.setattr(extractor.re, "finditer", counting_finditer)

    assert extractor.extract_strings_from_cpp(path) == set()
    assert searched == []


def test_a_file_holding_no_table_is_not_searched(monkeypatch):
    searched = []
    spied = tuple(
        (literals, lambda content, scan=scan: searched.append(scan.__name__) or scan(content))
        for literals, scan in cpp_tables._TABLE_EXTRACTORS
    )
    monkeypatch.setattr(cpp_tables, "_TABLE_EXTRACTORS", spied)

    assert cpp_tables.extract_table_strings(PLAIN) == set()
    assert searched == []
