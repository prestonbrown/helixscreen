# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/subject_codemod.py: the rewrite onto helix::Subject is exact, leaves
what it cannot prove for a hand edit, never touches comments or strings, and is idempotent."""

import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("subject_codemod", REPO_ROOT / "scripts" / "subject_codemod.py")
codemod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(codemod)


def rw(src):
    out, res = codemod.rewrite(src)
    return out, [why for _, why in res.manual]


def body(out):
    return "\n".join(ln for ln in out.split("\n") if ln != codemod.INCLUDE_LINE)


def test_storage_declarations_become_helix_subject():
    src = (
        "struct X {\n"
        "    lv_subject_t count_;\n"
        "    lv_subject_t rows_[4];\n"
        "    lv_subject_t view = {};\n"
        "    std::array<lv_subject_t, 3> cells_{};\n"
        "    std::vector<lv_subject_t> lanes;\n"
        "    std::unique_ptr<lv_subject_t> dyn;\n"
        "};\n"
        "auto p = std::make_unique<lv_subject_t>();\n"
        "std::map<std::string, lv_subject_t> by_name;\n"
        "auto rows = std::make_unique<lv_subject_t[]>(4);\n"
        "lv_subject_t a, b;\n"
    )
    out, manual = rw(src)
    assert body(out) == (
        "struct X {\n"
        "    helix::Subject count_;\n"
        "    helix::Subject rows_[4];\n"
        "    helix::Subject view = {};\n"
        "    std::array<helix::Subject, 3> cells_{};\n"
        "    std::vector<helix::Subject> lanes;\n"
        "    std::unique_ptr<helix::Subject> dyn;\n"
        "};\n"
        "auto p = std::make_unique<helix::Subject>();\n"
        "std::map<std::string, helix::Subject> by_name;\n"
        "auto rows = std::make_unique<helix::Subject[]>(4);\n"
        "helix::Subject a, b;\n"
    )
    assert manual == []


def test_pointers_and_references_to_lv_subject_t_are_kept():
    src = "lv_subject_t* get() { return &count_; }\nvoid f(lv_subject_t* s, const lv_subject_t& r);\n"
    out, manual = rw(src)
    assert out == src
    assert manual == []


def test_init_and_deinit_calls_become_members():
    src = (
        "lv_subject_init_int(&count_, 0);\n"
        "lv_subject_init_string(&label_, label_buf_, nullptr, sizeof(label_buf_), \"x, y\");\n"
        "lv_subject_init_color(&obj.swatch_, lv_color_hex(0x112233));\n"
        "lv_subject_init_int(&rows_[f(i, j)], g(1, 2));\n"
        "lv_subject_init_int(info.temp_subject.get(), -1);\n"
        "lv_subject_deinit(&count_);\n"
        "lv_subject_deinit(subject_ptr);\n"
        "lv_subject_copy_string(&label_, \"z\");\n"
    )
    out, manual = rw(src)
    assert body(out) == (
        "count_.init_int(0);\n"
        "label_.init_string(label_buf_, nullptr, sizeof(label_buf_), \"x, y\");\n"
        "obj.swatch_.init_color(lv_color_hex(0x112233));\n"
        "rows_[f(i, j)].init_int(g(1, 2));\n"
        "info.temp_subject->init_int(-1);\n"
        "count_.reset();\n"
        "helix::subject_reset(subject_ptr);\n"
        "lv_subject_set_string(&label_, \"z\");\n"
    )
    assert manual == []


def test_a_reference_parameter_the_file_inits_through_is_converted():
    src = (
        "static void publish(lv_subject_t& subject, int v) {\n"
        "    lv_subject_init_int(&subject, v);\n"
        "}\n"
        "static int read(const lv_subject_t& other);\n"
    )
    out, _ = rw(src)
    assert "static void publish(helix::Subject& subject, int v) {" in out
    assert "subject.init_int(v);" in out
    assert "static int read(const lv_subject_t& other);" in out


def test_what_it_cannot_prove_is_listed_and_left_alone():
    src = (
        "lv_subject_init_int(subject, 0);\n"
        "lv_subject_init_group(&g, list, 2);\n"
        "auto* s = new lv_subject_t{};\n"
        "size_t n = sizeof(lv_subject_t);\n"
    )
    out, manual = rw(src)
    assert "lv_subject_init_int(subject, 0);" in out
    assert "lv_subject_init_group(&g, list, 2);" in out
    assert "new lv_subject_t{}" in out
    assert len(manual) == 4


def test_pointer_arithmetic_over_subject_storage_is_flagged():
    src = (
        "lv_subject_t rows_[4];\n"
        "lv_subject_t* at(int i) { return &rows_[0] + i; }\n"
        "int index_of(lv_subject_t* s) { return static_cast<int>(s - &rows_[0]); }\n"
        "bool same(lv_subject_t* s) { return s == &rows_[1] && s != nullptr; }\n"
    )
    _, manual = rw(src)
    flagged = [m for m in manual if "pointer arithmetic" in m or "subtraction" in m]
    assert len(flagged) == 2
    assert any("return &rows_[0] + i" in m for m in flagged)
    assert any("s - &rows_[0]" in m for m in flagged)


def test_comments_and_strings_are_never_rewritten():
    src = (
        "// lv_subject_init_int(&count_, 0); lv_subject_t old_;\n"
        "const char* doc = \"lv_subject_deinit(&count_)\";\n"
    )
    out, manual = rw(src)
    assert out == src
    assert manual == []


def test_the_include_is_added_once_after_the_quoted_includes():
    src = '#include "a.h"\n#include "b.h"\n\n#include <vector>\n\nlv_subject_t x_;\n'
    out, _ = rw(src)
    assert out == '#include "a.h"\n#include "b.h"\n#include "core/subject.h"\n\n#include <vector>\n\nhelix::Subject x_;\n'


def test_the_rewrite_is_idempotent():
    src = '#include "a.h"\nlv_subject_t x_;\nvoid f() { lv_subject_init_int(&x_, 1); lv_subject_deinit(&x_); }\n'
    once, _ = rw(src)
    twice, manual = rw(once)
    assert twice == once
    assert manual == []
