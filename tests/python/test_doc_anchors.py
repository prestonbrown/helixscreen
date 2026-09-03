# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/doc_anchors.py.

Covers the citation grammar, the region resolver's refuse-to-guess contract,
and the per-language definition scanners.
"""

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from doc_anchors import (  # noqa: E402
    Citation,
    Segment,
    format_citation,
    parse_citation,
)


def test_path_only_citation_has_no_segments():
    c = parse_citation("src/printer/printer_state.cpp")
    assert c.path == "src/printer/printer_state.cpp"
    assert c.segments == ()


def test_single_identifier_segment():
    c = parse_citation("src/printer/printer_state.cpp#update_from_status")
    assert c.path == "src/printer/printer_state.cpp"
    assert c.segments == (Segment("update_from_status", False),)


def test_nested_identifier_segments():
    c = parse_citation("include/ui_nav_manager.h#PanelRequest/overlay_root")
    assert c.segments == (
        Segment("PanelRequest", False),
        Segment("overlay_root", False),
    )


def test_snippet_segment():
    c = parse_citation('src/application/application.cpp#instance/"shutdown_requested"')
    assert c.segments == (
        Segment("instance", False),
        Segment("shutdown_requested", True),
    )


def test_slash_inside_a_snippet_does_not_split_it():
    c = parse_citation('mk/cross.mk#PLATFORM_TARGET/"lib/lvgl"')
    assert c.segments == (
        Segment("PLATFORM_TARGET", False),
        Segment("lib/lvgl", True),
    )


def test_escaped_quote_inside_a_snippet():
    c = parse_citation(r'src/a.cpp#f/"says \"hi\""')
    assert c.segments[1] == Segment('says "hi"', True)


def test_qualified_identifier_segment_keeps_colons():
    c = parse_citation("src/ui/led_widget.cpp#helix::ui/attach")
    assert c.segments[0] == Segment("helix::ui", False)


@pytest.mark.parametrize(
    "text",
    [
        "src/printer/printer_state.cpp",
        "src/printer/printer_state.cpp#update_from_status",
        "include/ui_nav_manager.h#PanelRequest/overlay_root",
        'src/application/application.cpp#instance/"shutdown_requested"',
        r'src/a.cpp#f/"says \"hi\""',
    ],
)
def test_format_round_trips_parse(text):
    assert format_citation(parse_citation(text)) == text


def test_empty_fragment_is_an_error():
    with pytest.raises(ValueError):
        parse_citation("src/a.cpp#")


def test_unterminated_snippet_is_an_error():
    with pytest.raises(ValueError):
        parse_citation('src/a.cpp#f/"never closed')


from doc_anchors import (  # noqa: E402
    Ambiguous,
    NotFound,
    Region,
    _CPP_FUNC_JOIN_LIMIT,
    block_end,
    definitions,
    resolve_segments,
)

CPP = """\
namespace helix {

class PanelRequest {
  public:
    lv_obj_t* overlay_root;
    void reset();
};

void PanelRequest::reset() {
    overlay_root = nullptr;
}

void instance() {
    bool shutdown_requested = false;
    if (shutdown_requested) {
        return;
    }
}

}  // namespace helix
""".split("\n")


def test_identifier_resolves_to_its_definition():
    r = resolve_segments(CPP, parse_citation("a.cpp#instance").segments, ".cpp")
    assert CPP[r.start].strip() == "void instance() {"


def test_nested_segment_resolves_inside_its_parent():
    segs = parse_citation("a.h#PanelRequest/overlay_root").segments
    r = resolve_segments(CPP, segs, ".h")
    assert CPP[r.start].strip() == "lv_obj_t* overlay_root;"


def test_snippet_segment_resolves_inside_its_parent():
    segs = parse_citation('a.cpp#instance/"shutdown_requested = false"').segments
    r = resolve_segments(CPP, segs, ".cpp")
    assert CPP[r.start].strip() == "bool shutdown_requested = false;"


def test_a_snippet_scoped_to_a_parent_ignores_matches_elsewhere():
    # "reset" appears both as a member declaration and as a definition; scoping
    # to the class picks out exactly one without the resolver guessing.
    segs = parse_citation("a.h#PanelRequest/reset").segments
    r = resolve_segments(CPP, segs, ".h")
    assert CPP[r.start].strip() == "void reset();"


def test_zero_matches_raises_not_found():
    with pytest.raises(NotFound):
        resolve_segments(CPP, parse_citation("a.cpp#no_such_thing").segments, ".cpp")


def test_two_matches_raises_ambiguous_with_candidates():
    dupe = ["void f() {", "}", "void f() {", "}"]
    with pytest.raises(Ambiguous) as excinfo:
        resolve_segments(dupe, parse_citation("a.cpp#f").segments, ".cpp")
    assert len(excinfo.value.candidates) == 2


def test_ambiguous_snippet_raises_rather_than_taking_the_first():
    segs = parse_citation('a.cpp#instance/"shutdown_requested"').segments
    with pytest.raises(Ambiguous):
        resolve_segments(CPP, segs, ".cpp")


def test_no_segments_resolves_to_the_whole_file():
    r = resolve_segments(CPP, (), ".cpp")
    assert r == Region(0, len(CPP))


def _resolve_region(text, citation, ext):
    lines = text.split("\n")
    return lines, resolve_segments(lines, parse_citation(citation).segments, ext)


def _resolve(text, citation, ext):
    lines, r = _resolve_region(text, citation, ext)
    return lines[r.start].strip()


def test_cpp_class_then_member():
    assert _resolve(
        "\n".join(CPP), "a.h#PanelRequest/overlay_root", ".h"
    ) == "lv_obj_t* overlay_root;"


def test_cpp_namespace_qualifies_a_free_function():
    src = "namespace helix {\nvoid attach() {\n}\n}\n"
    assert _resolve(src, "a.cpp#helix/attach", ".cpp") == "void attach() {"


def test_cpp_define():
    src = "#define HELIX_MAX 4\n"
    assert _resolve(src, "a.h#HELIX_MAX", ".h") == "#define HELIX_MAX 4"


def test_xml_name_attribute():
    src = '<view>\n  <lv_obj name="carousel_host">\n  </lv_obj>\n</view>\n'
    assert _resolve(src, "a.xml#carousel_host", ".xml") == '<lv_obj name="carousel_host">'


def test_xml_nested_names_scope():
    src = (
        '<view>\n'
        '  <lv_obj name="outer">\n'
        '    <lv_label name="inner"/>\n'
        '  </lv_obj>\n'
        '  <lv_label name="inner"/>\n'
        '</view>\n'
    )
    # Two widgets are named "inner"; scoping to "outer" resolves without guessing.
    assert _resolve(src, "a.xml#outer/inner", ".xml") == '<lv_label name="inner"/>'


def test_xml_self_closed_and_paired_siblings_resolve():
    src = (
        '<view>\n'
        '  <lv_obj name="a">\n'
        '    <lv_obj name="b"/>\n'
        '    <lv_obj name="c">\n'
        '    </lv_obj>\n'
        '  </lv_obj>\n'
        '</view>\n'
    )
    # "a" holds one self-closed sibling and one paired sibling with the same
    # tag name. Checking the END boundary matters here, not just the start
    # line: a self-closed element's region is exactly one line, and a wrong
    # end for "b" would swallow "c" (or vice versa) while the start line -
    # found by a name search, not by the end computation - stays correct.
    lines, r_a = _resolve_region(src, "a.xml#a", ".xml")
    assert lines[r_a.start].strip() == '<lv_obj name="a">'
    assert (r_a.start, r_a.end) == (1, 6)

    lines, r_b = _resolve_region(src, "a.xml#a/b", ".xml")
    assert lines[r_b.start].strip() == '<lv_obj name="b"/>'
    assert (r_b.start, r_b.end) == (2, 3)

    lines, r_c = _resolve_region(src, "a.xml#a/c", ".xml")
    assert lines[r_c.start].strip() == '<lv_obj name="c">'
    assert (r_c.start, r_c.end) == (3, 5)


def test_make_variable():
    src = "TIER_FONT_SRCS := $(FONTS_ALL)\n"
    assert _resolve(src, "fonts.mk#TIER_FONT_SRCS", ".mk") == "TIER_FONT_SRCS := $(FONTS_ALL)"


def test_make_target():
    src = "regen-doc-links: regen-doc-anchors\n\techo hi\n"
    assert _resolve(src, "tools.mk#regen-doc-links", ".mk").startswith("regen-doc-links:")


def test_python_nested_def_scopes():
    src = "class A:\n    def run(self):\n        pass\n\ndef run():\n    pass\n"
    assert _resolve(src, "a.py#A/run", ".py") == "def run(self):"


def test_shell_function():
    src = "qc_doc_refs() {\n  echo hi\n}\n"
    assert _resolve(src, "quality-checks.sh#qc_doc_refs", ".sh") == "qc_doc_refs() {"


def test_bats_test_name():
    src = '@test "temp files use unit helpers" {\n  true\n}\n'
    assert _resolve(
        src, 'a.bats#"temp files use unit helpers"', ".bats"
    ).startswith("@test")


def test_markdown_heading():
    src = "# Top\n\ntext\n\n## Console sink\n\nmore\n"
    assert _resolve(src, 'LOGGING.md#"Console sink"', ".md") == "## Console sink"


def test_markdown_heading_slug_matches_a_title_case_heading():
    src = "# Top\n\ntext\n\n## Contributing\n\nmore\n"
    assert _resolve(src, "a.md#contributing", ".md") == "## Contributing"


def test_markdown_heading_slug_hyphenates_spaces():
    src = "# Top\n\ntext\n\n## Console sink\n\nmore\n"
    assert _resolve(src, "a.md#console-sink", ".md") == "## Console sink"


def test_markdown_lowercase_single_word_heading_is_not_ambiguous():
    # A heading whose literal text already equals its own slug (lowercase, no
    # spaces) must resolve once, not raise Ambiguous from being registered
    # under the same name twice.
    src = "# Top\n\ntext\n\n## sound\n\nmore\n"
    assert _resolve(src, "a.md#sound", ".md") == "## sound"


def test_markdown_heading_slug_collision_raises_ambiguous():
    src = "## Foo Bar\n\ntext\n\n## foo-bar\n\nmore\n"
    with pytest.raises(Ambiguous):
        _resolve(src, "a.md#foo-bar", ".md")


def test_markdown_heading_slug_collision_quoted_literal_disambiguates():
    src = "## Foo Bar\n\ntext\n\n## foo-bar\n\nmore\n"
    assert _resolve(src, 'a.md#"Foo Bar"', ".md") == "## Foo Bar"


def test_bare_segment_cannot_contain_spaces():
    # Heading and test-name anchors use the quoted form; a bare segment is a
    # single identifier, so prose with spaces must be quoted to resolve.
    with pytest.raises(ValueError):
        parse_citation("a.md#Console sink")


def test_json_key():
    src = '{\n  "printers": {\n    "ad5m": 1\n  }\n}\n'
    assert _resolve(src, "db.json#printers", ".json") == '"printers": {'


def test_unknown_extension_falls_back_to_cpp_scanner():
    src = "void f() {\n}\n"
    assert _resolve(src, "a.inc#f", ".inc") == "void f() {"


def test_cpp_decl_ignores_statements_with_no_leading_type():
    lines = [
        "void f() {",
        '    spdlog::info("temp: {}", t);',
        "    counter_ = 0;",
        "}",
    ]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # Neither line declares anything: the first is a qualified call, the
    # second an assignment to an existing member. Only "f" is a definition.
    assert names == ["f"]


def test_cpp_decl_rejects_control_flow_statement():
    lines = ["void g() {", "    return foo(bar);", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # "return" is not a type, so "foo" is not a declaration.
    assert names == ["g"]


def test_cpp_decl_resolves_using_and_typedef():
    lines = [
        "using Callback = std::function<void(bool)>;",
        "typedef int MyInt;",
        "void f();",
    ]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # "using" and "typedef" introduce a declaration, not a statement - they
    # must not be caught by the same leading-keyword rejection as "return".
    assert names == ["Callback", "MyInt", "f"]


def test_cpp_decl_rejects_friend_declaration():
    # Real shape from include/application.h:88 - `friend class X;` declares
    # nothing named X in this scope, unlike `class X {` opening one.
    lines = ["friend class ApplicationTestAccess;", "void f();"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["f"]

    lines = ["friend struct ApplicationTestAccess;", "void f();"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["f"]


def test_cpp_decl_rejects_other_non_declaration_keyword_leaders():
    lines = [
        "default: foo();",
        "co_return foo(bar);",
        "return foo(bar);",
        "counter_ = 0;",
        'spdlog::info("x", t);',
        "void f();",
    ]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # None of the five lines above declares anything; only "f" is real.
    assert names == ["f"]


def test_block_end_ignores_braces_inside_string_literal():
    lines = ["void g() {", '    return "opening only {";', "}"]
    assert block_end(lines, 0) == 3


def test_cpp_func_rejects_call_through_instance_accessor():
    lines = ["helix::MemoryMonitor::instance().set_hang_callback([](uint32_t stalled_ms) {"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # "instance"'s own parens close immediately, but a member-call dot
    # follows before any brace opens a body - it is not a definition.
    assert names == []


def test_cpp_func_rejects_registration_call_with_lambda_argument():
    lines = ["lv_obj_add_event_cb(btn, [](lv_event_t* e) {"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # The lambda argument's own parens leave the call's parens unclosed on
    # this line, so the brace belongs to the lambda, not to a definition.
    assert names == []


def test_cpp_func_rejects_algorithm_call_with_lambda_argument():
    lines = ["std::sort(v.begin(), v.end(), [](int a, int b) {"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == []


def test_cpp_func_accepts_qualified_member_definition():
    lines = ["void PanelRequest::reset() {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["reset"]


def test_cpp_func_accepts_free_function_definition():
    lines = ["void f() {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["f"]


def test_cpp_func_accepts_const_qualified_member_definition():
    lines = ["static int g(int a) const {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["g"]


def test_cpp_func_accepts_trailing_return_type():
    lines = ["auto f() -> int {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["f"]


def test_cpp_func_accepts_ctor_with_single_member_initializer():
    lines = ["UsbManager::UsbManager(bool force_mock) : force_mock_(force_mock) {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["UsbManager"]


def test_cpp_func_accepts_ctor_with_multiple_member_initializers():
    lines = [
        "WiFiManager::WiFiManager() : scan_timer_(nullptr), scan_pending_(false) {",
        "}",
    ]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["WiFiManager"]


def test_cpp_func_accepts_ctor_with_short_member_initializer():
    lines = ["Foo::Foo() : a_(1) {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["Foo"]


def test_cpp_func_accepts_ctor_with_qualifier_before_initializer_list():
    lines = ["Foo::Foo() noexcept : a_(1) {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["Foo"]


def test_cpp_func_reject_shapes_still_reject_beside_ctor_support():
    lines = ["helix::MemoryMonitor::instance().set_hang_callback([](uint32_t stalled_ms) {"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == []

    lines = ["lv_obj_add_event_cb(btn, [](lv_event_t* e) {"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == []

    lines = ["std::sort(v.begin(), v.end(), [](int a, int b) {"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == []


def test_cpp_func_prefix_rejects_a_call_argument_that_looks_like_a_name():
    lines = ["foo(bar) baz() {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # "foo(bar)" is a call, not a definition. Its own parens land in the
    # prefix before "baz": rejecting any paren there is what keeps "baz"
    # from being picked up as if it were a name following a type.
    assert names == []


def test_cpp_func_rejects_case_label_with_call_expression():
    lines = ["case foo(): {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # "case foo():" has the same shape as a constructor's empty
    # initializer list ("Foo() : {"), but "case" is a statement keyword,
    # not a name being defined.
    assert names == []


def test_cpp_func_rejects_case_label_with_identifier_expression():
    lines = ["case kMax: {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == []


def test_cpp_func_rejects_default_label():
    lines = ["default: bar();", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == []


def test_cpp_func_leading_keyword_gate_still_accepts_every_prior_shape():
    for src, expected in (
        ("void f() {", "f"),
        ("static int g(int a) const {", "g"),
        ("auto f() -> int {", "f"),
        ("UsbManager::UsbManager(bool force_mock) : force_mock_(force_mock) {", "UsbManager"),
        ("Foo::Foo() noexcept : a_(1) {", "Foo"),
        ("Foo::Foo() : base::Thing(1) {", "Foo"),
    ):
        lines = [src, "}"]
        names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
        assert names == [expected], src


def test_cpp_func_accepts_a_signature_wrapped_before_its_closing_paren():
    lines = [
        "void PrinterState::update_from_status(const json& state, double eventtime,",
        "                                      bool from_cached_snapshot) {",
        "}",
    ]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["update_from_status"]


def test_cpp_func_accepts_a_signature_whose_brace_lands_on_the_next_line():
    lines = ["void f(int a, int b)", "{", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["f"]


def test_cpp_func_accepts_a_wrapped_ctor_with_member_initializer_list():
    lines = [
        "WiFiManager::WiFiManager(bool force_mock)",
        "    : force_mock_(force_mock), scan_pending_(false) {",
        "}",
    ]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == ["WiFiManager"]


def test_cpp_func_wrapped_region_starts_at_the_signatures_first_line():
    lines = [
        "void f(int a,",
        "       int b) {",
        "    return;",
        "}",
    ]
    # The region a citation resolves to is the whole definition, so it opens
    # on the line carrying the name - not on the line that happens to hold
    # the brace.
    assert definitions(lines, Region(0, len(lines)), ".cpp") == [("f", Region(0, 4))]


def test_cpp_func_join_is_bounded_by_the_limit():
    def signature_spanning(gap):
        return ["void f(", *["    int a,"] * gap, "    int z) {", "}"]

    within = signature_spanning(_CPP_FUNC_JOIN_LIMIT - 2)
    beyond = signature_spanning(_CPP_FUNC_JOIN_LIMIT + 5)
    assert [n for n, _ in definitions(within, Region(0, len(within)), ".cpp")] == ["f"]
    # Parens that never balance inside the limit end the candidate there
    # rather than running the join to the end of the file.
    assert [n for n, _ in definitions(beyond, Region(0, len(beyond)), ".cpp")] == []


def test_cpp_func_blank_line_does_not_join_into_the_next_signature():
    lines = ["void foo() {", "}", "", "void bar() {", "}"]
    # "bar" belongs to line 3 alone. A blank line that joined forward would
    # register it a second time at line 2, turning a sound citation into an
    # ambiguity - and pinning the region proves which line each one owns.
    assert definitions(lines, Region(0, len(lines)), ".cpp") == [
        ("foo", Region(0, 2)),
        ("bar", Region(3, 5)),
    ]


def test_cpp_func_comment_line_does_not_join_into_the_next_signature():
    lines = ["// Refreshes the cached snapshot.", "void bar() {", "}"]
    # A comment strips to an empty line, which is the blank-line case with
    # nothing to signal it.
    assert definitions(lines, Region(0, len(lines)), ".cpp") == [("bar", Region(1, 3))]


def test_cpp_func_rejects_a_call_whose_lambda_opens_on_the_next_line():
    lines = [
        "lv_obj_add_event_cb(btn,",
        "                    [](lv_event_t* e) {",
        "                    }, LV_EVENT_CLICKED, nullptr);",
    ]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # The call's own parens close only after the lambda body, with no brace
    # left behind them - the same reasoning as the one-line form, applied to
    # a candidate that spans lines.
    assert names == []


def test_cpp_func_rejects_multi_line_forms_of_the_lambda_call_shapes():
    for src in (
        [
            "helix::MemoryMonitor::instance().set_hang_callback(",
            "    [](uint32_t stalled_ms) {",
            "    });",
        ],
        ["std::sort(v.begin(), v.end(),", "          [](int a, int b) {", "          });"],
    ):
        names = [name for name, _ in definitions(src, Region(0, len(src)), ".cpp")]
        assert names == [], src


_CPP_NON_DEFINITION_LINES = [
    "helix::MemoryMonitor::instance().set_hang_callback([](uint32_t stalled_ms) {",
    "lv_obj_add_event_cb(btn, [](lv_event_t* e) {",
    "std::sort(v.begin(), v.end(), [](int a, int b) {",
    "case foo(): {",
    "default: bar();",
    "return foo(bar);",
    "counter_ = 0;",
    'spdlog::info("temp: {}", t);',
    "friend class Foo;",
]


@pytest.mark.parametrize("shape", _CPP_NON_DEFINITION_LINES)
def test_cpp_non_definition_line_does_not_join_into_a_following_definition(shape):
    lines = [shape, "", "void real_one() {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    # A line that defines nothing must stay defining nothing when the real
    # signature below it is in joining range: borrowing that name would pin
    # the citation to the wrong line.
    assert names == ["real_one"], shape


@pytest.mark.parametrize(
    "shape,expected",
    [
        ("void f() {", "f"),
        ("static int g(int a) const {", "g"),
        ("auto f() -> int {", "f"),
        ("UsbManager::UsbManager(bool force_mock) : force_mock_(force_mock) {", "UsbManager"),
        ("Foo::Foo() noexcept : a_(1) {", "Foo"),
        ("using Callback = int;", "Callback"),
        ("lv_obj_t* overlay_root;", "overlay_root"),
    ],
)
def test_cpp_definition_shapes_survive_a_following_signature_in_joining_range(shape, expected):
    lines = [shape, "}", "", "void real_one() {", "}"]
    names = [name for name, _ in definitions(lines, Region(0, len(lines)), ".cpp")]
    assert names == [expected, "real_one"], shape

import subprocess

from doc_anchors import resolve  # noqa: E402

SCRIPT = REPO_ROOT / "scripts" / "doc_anchors.py"


def _repo_lines(rel_path):
    return (REPO_ROOT / rel_path).read_text(encoding="utf-8").split("\n")


def test_wrapped_signature_resolves_against_the_repo_itself():
    # The module docstring offers this citation as its first example, so it
    # has to resolve in this tree and not only in a fixture. Its signature
    # wraps, so resolving it is what exercises the multi-line join.
    path = "src/printer/printer_state.cpp"
    line = resolve(f"{path}#update_from_status", repo_root=REPO_ROOT)
    lines = _repo_lines(path)
    text = lines[line - 1]
    assert text.startswith("void PrinterState::update_from_status(")
    assert not text.rstrip().endswith("{"), "this signature must wrap to be a join test"
    assert [i for i, t in enumerate(lines, 1)
            if t.startswith("void PrinterState::update_from_status(")] == [line]


def test_overload_set_with_a_wrapped_member_is_ambiguous_in_the_repo_itself():
    # Both overloads must be visible. When only the single-line one is, the
    # citation answers confidently with that line instead of refusing, which
    # is a wrong answer rather than an error.
    path = "src/printer/ams_backend.cpp"
    with pytest.raises(Ambiguous) as excinfo:
        resolve(f"{path}#create", repo_root=REPO_ROOT)
    hits = sorted(r.start + 1 for r in excinfo.value.candidates)
    lines = _repo_lines(path)
    assert len(hits) == 2, hits
    assert all("AmsBackend::create(" in lines[n - 1] for n in hits), hits
    assert lines[hits[0] - 1].rstrip().endswith("{")
    assert not lines[hits[1] - 1].rstrip().endswith("{")


def test_resolve_returns_a_one_based_line(tmp_path):
    (tmp_path / "a.cpp").write_text("// header\nvoid f() {\n}\n", encoding="utf-8")
    assert resolve("a.cpp#f", repo_root=tmp_path) == 2


def test_resolve_path_only_returns_line_one(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    assert resolve("a.cpp", repo_root=tmp_path) == 1


def test_resolve_missing_file_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        resolve("nope.cpp#f", repo_root=tmp_path)


def test_resolve_cli_prints_path_and_line():
    out = subprocess.run(
        [sys.executable, str(SCRIPT), "--resolve", "scripts/doc_anchors.py#resolve"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert out.startswith("scripts/doc_anchors.py:")
    assert int(out.rsplit(":", 1)[1]) > 0


def test_resolve_cli_exits_nonzero_on_ambiguity(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\nvoid f() {\n}\n", encoding="utf-8")
    p = subprocess.run(
        [sys.executable, str(SCRIPT), "--resolve", "a.cpp#f"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    assert p.returncode != 0
    assert "matches lines" in (p.stdout + p.stderr)


def test_resolve_cli_reports_missing_file_without_repeating_the_path(tmp_path):
    p = subprocess.run(
        [sys.executable, str(SCRIPT), "--resolve", "nope.cpp"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    assert p.returncode != 0
    assert (p.stdout + p.stderr).count("nope.cpp") == 1


from doc_anchors import check, iter_citations  # noqa: E402


def test_iter_citations_skips_fenced_code_blocks(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text(
        "prose `src/a.cpp#f` here\n"
        "```\n"
        "`src/b.cpp#g`\n"
        "```\n"
        "and `src/c.cpp#h`\n",
        encoding="utf-8",
    )
    found = [c for _, _, c in iter_citations([doc])]
    assert found == ["src/a.cpp#f", "src/c.cpp#h"]


def test_tilde_fence_does_not_close_a_backtick_fence(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text(
        "prose `src/a.cpp#f` here\n"
        "```\n"
        "~~~\n"
        "`src/b.cpp#g`\n"
        "```\n"
        "and `src/c.cpp#h`\n",
        encoding="utf-8",
    )
    found = [c for _, _, c in iter_citations([doc])]
    assert found == ["src/a.cpp#f", "src/c.cpp#h"]


def test_longer_closing_fence_still_closes(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text(
        "prose `src/a.cpp#f` here\n"
        "```\n"
        "`src/b.cpp#g`\n"
        "````\n"
        "and `src/c.cpp#h`\n",
        encoding="utf-8",
    )
    found = [c for _, _, c in iter_citations([doc])]
    assert found == ["src/a.cpp#f", "src/c.cpp#h"]


def test_unclosed_fence_produces_a_problem(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text("prose\n```\nsome code, never closed\n", encoding="utf-8")
    problems = []
    citations = iter_citations([doc], problems=problems)
    assert citations == []
    assert len(problems) == 1
    assert "fence" in problems[0]


def test_check_reports_an_unclosed_fence(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text("prose\n```\nsome code, never closed\n", encoding="utf-8")
    findings = check([doc], repo_root=tmp_path)
    assert len(findings) == 1
    assert "fence" in findings[0].lower()


def test_check_reports_a_missing_name(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#nope`\n", encoding="utf-8")
    findings = check([doc], repo_root=tmp_path)
    assert len(findings) == 1
    assert "nope" in findings[0]


def test_check_reports_ambiguity(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\nvoid f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f`\n", encoding="utf-8")
    findings = check([doc], repo_root=tmp_path)
    assert len(findings) == 1
    assert "matches lines" in findings[0]


def test_check_is_silent_when_everything_resolves(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f`\n", encoding="utf-8")
    assert check([doc], repo_root=tmp_path) == []


def test_check_cli_exits_zero_even_with_findings(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    (tmp_path / "d.md").write_text("see `a.cpp#nope`\n", encoding="utf-8")
    p = subprocess.run(
        [sys.executable, str(SCRIPT), "--check", "d.md"],
        cwd=tmp_path, capture_output=True, text=True,
    )
    assert p.returncode == 0
    assert "nope" in p.stdout


def test_check_reports_a_malformed_citation(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#1bad`\n", encoding="utf-8")
    findings = check([doc], repo_root=tmp_path)
    assert len(findings) == 1
    assert "malformed" in findings[0].lower()


def test_check_cli_survives_a_malformed_citation(tmp_path):
    (tmp_path / "d.md").write_text("see `a.cpp#1bad`\n", encoding="utf-8")
    p = subprocess.run(
        [sys.executable, str(SCRIPT), "--check", "d.md"],
        cwd=tmp_path, capture_output=True, text=True,
    )
    assert p.returncode == 0
    assert "malformed" in p.stdout.lower()


def test_check_cli_survives_a_target_that_does_not_exist(tmp_path):
    p = subprocess.run(
        [sys.executable, str(SCRIPT), "--check", "ghost.md"],
        cwd=tmp_path, capture_output=True, text=True,
    )
    assert p.returncode == 0
    assert "ghost.md" in p.stdout


@pytest.mark.skipif(os.getuid() == 0, reason="root ignores file permission bits")
def test_check_cli_survives_an_unreadable_file(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f`\n", encoding="utf-8")
    doc.chmod(0)
    try:
        p = subprocess.run(
            [sys.executable, str(SCRIPT), "--check", "d.md"],
            cwd=tmp_path, capture_output=True, text=True,
        )
    finally:
        doc.chmod(0o644)
    assert p.returncode == 0
    assert "d.md" in p.stdout


def test_check_resolves_a_citation_relative_to_the_citing_doc(tmp_path):
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    (docs_dir / "sibling.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = docs_dir / "d.md"
    doc.write_text("see `sibling.cpp#f`\n", encoding="utf-8")
    assert check([doc], repo_root=tmp_path) == []


def test_check_resolves_a_citation_relative_to_the_citing_docs_parent_dir(tmp_path):
    docs_dir = tmp_path / "docs"
    devel_dir = docs_dir / "devel"
    devel_dir.mkdir(parents=True)
    (docs_dir / "sibling.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = devel_dir / "d.md"
    doc.write_text("see `sibling.cpp#f`\n", encoding="utf-8")
    assert check([doc], repo_root=tmp_path) == []


def test_resolve_prefers_repo_root_over_doc_relative_on_a_name_collision(tmp_path):
    (tmp_path / "a.cpp").write_text("// header\nvoid root_only() {\n}\n", encoding="utf-8")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    (docs_dir / "a.cpp").write_text("void root_only() {\n}\n", encoding="utf-8")
    line = resolve("a.cpp#root_only", repo_root=tmp_path, relative_to=docs_dir)
    assert line == 2


def test_resolve_without_relative_to_does_not_widen_to_doc_relative_paths(tmp_path):
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    (docs_dir / "sibling.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    with pytest.raises(FileNotFoundError):
        resolve("sibling.cpp#f", repo_root=tmp_path)


from doc_anchors import render  # noqa: E402


def test_render_expands_citations_to_path_and_line(tmp_path):
    (tmp_path / "a.cpp").write_text("// header\nvoid f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f` for details\n", encoding="utf-8")
    out = tmp_path / "pinned"
    assert render([doc], out, repo_root=tmp_path) == 1
    text = (out / "d.md").read_text(encoding="utf-8")
    assert "`a.cpp:2`" in text
    assert "#L2" in text


def test_render_leaves_an_unresolvable_citation_alone(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#nope`\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    assert "`a.cpp#nope`" in (out / "d.md").read_text(encoding="utf-8")


def test_render_does_not_touch_fenced_examples(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("```\n`a.cpp#f`\n```\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    assert "`a.cpp#f`" in (out / "d.md").read_text(encoding="utf-8")


import re  # noqa: E402


def test_render_link_resolves_for_a_repo_root_relative_citation(tmp_path):
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    (src_dir / "a.cpp").write_text("// header\nvoid f() {\n}\n", encoding="utf-8")
    doc_dir = tmp_path / "docs" / "devel"
    doc_dir.mkdir(parents=True)
    doc = doc_dir / "d.md"
    doc.write_text("see `src/a.cpp#f`\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    dest = out / "docs" / "devel" / "d.md"
    m = re.search(r"\]\(([^)]+)#L2\)", dest.read_text(encoding="utf-8"))
    assert m
    assert os.path.isfile(os.path.join(os.path.dirname(dest), m.group(1)))


def test_render_link_resolves_for_a_doc_relative_citation(tmp_path):
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    (docs_dir / "sibling.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = docs_dir / "d.md"
    doc.write_text("see `sibling.cpp#f`\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    dest = out / "docs" / "d.md"
    m = re.search(r"\]\(([^)]+)#L1\)", dest.read_text(encoding="utf-8"))
    assert m
    assert os.path.isfile(os.path.join(os.path.dirname(dest), m.group(1)))


def test_render_link_text_uses_the_citations_own_path_unchanged(tmp_path):
    (tmp_path / "a.cpp").write_text("// header\nvoid f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f`\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    text = (out / "d.md").read_text(encoding="utf-8")
    assert "[`a.cpp:2`](" in text


def test_render_skips_a_doc_whose_pinned_path_would_escape_out_dir(tmp_path):
    repo_root = tmp_path / "repo"
    repo_root.mkdir()
    outside_doc = tmp_path / "outside" / "d.md"
    outside_doc.parent.mkdir()
    outside_doc.write_text("no citations here\n", encoding="utf-8")
    out = repo_root / "pinned"
    problems = []
    written = render([outside_doc], out, repo_root=repo_root, problems=problems)
    assert written == 0
    assert problems
    assert not (repo_root / "outside").exists()


def test_render_rebases_a_doc_to_doc_link_two_directories_deep(tmp_path):
    (tmp_path / "OTHER.md").write_text("# Other\n", encoding="utf-8")
    doc_dir = tmp_path / "docs" / "devel"
    doc_dir.mkdir(parents=True)
    doc = doc_dir / "d.md"
    doc.write_text("see [other](../../OTHER.md)\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    dest = out / "docs" / "devel" / "d.md"
    m = re.search(r"\]\(([^)]+)\)", dest.read_text(encoding="utf-8"))
    assert m
    assert os.path.isfile(os.path.join(os.path.dirname(dest), m.group(1)))


def test_render_rebases_an_image_link(tmp_path):
    (tmp_path / "diagram.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    doc_dir = tmp_path / "docs" / "devel"
    doc_dir.mkdir(parents=True)
    doc = doc_dir / "d.md"
    doc.write_text("![diagram](../../diagram.png)\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    dest = out / "docs" / "devel" / "d.md"
    m = re.search(r"!\[diagram\]\(([^)]+)\)", dest.read_text(encoding="utf-8"))
    assert m
    assert os.path.isfile(os.path.join(os.path.dirname(dest), m.group(1)))


def test_render_leaves_a_url_mailto_and_bare_fragment_untouched(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text(
        "see [ext](https://example.com/page) "
        "and [me](mailto:person@example.com) "
        "and [here](#section)\n",
        encoding="utf-8",
    )
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    text = (out / "d.md").read_text(encoding="utf-8")
    assert "[ext](https://example.com/page)" in text
    assert "[me](mailto:person@example.com)" in text
    assert "[here](#section)" in text


def test_render_rebased_link_keeps_its_fragment(tmp_path):
    (tmp_path / "OTHER.md").write_text("# Other\n", encoding="utf-8")
    doc_dir = tmp_path / "docs"
    doc_dir.mkdir()
    doc = doc_dir / "d.md"
    doc.write_text("see [other](../OTHER.md#section)\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    text = (out / "docs" / "d.md").read_text(encoding="utf-8")
    m = re.search(r"\]\(([^)]+)\)", text)
    assert m
    assert m.group(1).endswith("#section")
    target_path = m.group(1).rsplit("#", 1)[0]
    assert os.path.isfile(os.path.join(str(out / "docs"), target_path))


def test_render_does_not_touch_a_fenced_markdown_link(tmp_path):
    (tmp_path / "OTHER.md").write_text("# Other\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("```\n[other](OTHER.md)\n```\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    assert "[other](OTHER.md)" in (out / "d.md").read_text(encoding="utf-8")


def test_format_quotes_a_name_that_is_not_a_bare_identifier():
    # A markdown heading is a name, not an identifier: it carries spaces and
    # punctuation, so it round-trips only when quoted.
    heading = "Fix 2 — Persist last-known seated slot (RC2 + floor)"
    c = Citation(path="d.md", segments=(Segment(heading, False),))
    text = format_citation(c)
    assert parse_citation(text).segments[0].text == heading


def test_format_leaves_a_bare_identifier_unquoted():
    c = Citation(path="a.cpp", segments=(Segment("update_from_status", False),))
    assert format_citation(c) == "a.cpp#update_from_status"
