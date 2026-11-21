"""Tests for crossref module — Tier 2 cross-reference validation."""

from __future__ import annotations

from pathlib import Path

from helix_xml_linter.crossref import CrossRefValidator, ProjectRegistry
from helix_xml_linter.diagnostics import CheckType
from helix_xml_linter.linter import Linter, LinterConfig
from helix_xml_linter.schema_loader import Schema
from helix_xml_linter.xml_parser import parse_xml_file


class TestCrossRefConstRefs:
    """Tests for constant reference validation."""

    def test_undefined_const_ref_detected(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test undefined constant references are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(bad_refs_xml)
        const_errors = [d for d in result.diagnostics if d.check == CheckType.UNKNOWN_CONST_REF]
        assert len(const_errors) >= 1
        assert any("unknown_const" in d.message for d in const_errors)

    def test_defined_const_not_flagged(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test defined constants are not flagged."""
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(bad_refs_xml)
        const_errors = [
            d
            for d in result.diagnostics
            if d.check == CheckType.UNKNOWN_CONST_REF and "known_pad" in d.message
        ]
        assert len(const_errors) == 0

    def test_percentage_const_resolves_in_single_file(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Single-file xref recognizes all const tag variants."""
        xml = tmp_path / "percentage.xml"
        xml.write_text(
            '<component><consts><percentage name="card_width" value="45%"/></consts>'
            '<view extends="lv_obj" width="#card_width"/></component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(xml)
        const_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_CONST_REF
        ]
        assert const_errors == []


class TestCrossRefStyleRefs:
    """Tests for style reference validation."""

    def test_undefined_style_ref_detected(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test undefined style references are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(bad_refs_xml)
        style_errors = [d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_REF]
        assert len(style_errors) >= 1

    def test_defined_style_not_flagged(self, schema: Schema, valid_basic_xml: Path) -> None:
        """Test defined styles are not flagged."""
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(valid_basic_xml)
        style_errors = [d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_REF]
        assert len(style_errors) == 0

    def test_prefixed_bind_style_ref_detected(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Schema-canonical lv_obj-bind_style elements are xref-checked."""
        xml = tmp_path / "bad_prefixed_style.xml"
        xml.write_text(
            '<component><subjects><subject name="known_subject" type="int" value="0"/></subjects>'
            '<view extends="lv_obj">'
            '<lv_obj-bind_style name="missing_style" subject="known_subject" ref_value="1"/>'
            '</view></component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(xml)
        style_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_REF
        ]
        assert any("missing_style" in d.message for d in style_errors)

    def test_param_style_ref_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """A `$param` style name resolves at instantiation time, not statically.

        zone_tab binds `name="$selected_style"`; the linter cannot know which
        style the instantiating element passes, so the reference is skipped —
        the same allowance subject references already get.
        """
        xml = tmp_path / "param_style.xml"
        xml.write_text(
            '<component><subjects><subject name="tab_active" type="int" value="0"/></subjects>'
            '<styles><style name="tab_on" bg_opa="255"/></styles>'
            '<view extends="lv_obj">'
            '<bind_style_if_eq name="$selected_style" subject="tab_active" ref_value="1"/>'
            '</view></component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(xml)
        style_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_REF
        ]
        assert style_errors == []

    def test_composed_param_style_ref_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """A style name composed from a prop (header_action's `role_${role}`)."""
        xml = tmp_path / "composed_style.xml"
        xml.write_text(
            '<component><api><prop name="role" type="string" default="neutral"/></api>'
            '<styles><style name="role_neutral" bg_opa="255"/></styles>'
            '<view extends="lv_obj"><style name="role_${role}"/></view></component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(xml)
        style_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_REF
        ]
        assert style_errors == []

    def test_dotted_style_ref_resolves_across_files(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """component.style references resolve through project registry."""
        defs_xml = tmp_path / "theme.xml"
        defs_xml.write_text(
            '<component><styles><style name="badge_info" border_width="1"/></styles>'
            '<view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component><view extends="lv_obj">'
            '<bind_style name="theme.badge_info"/>'
            '</view></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([defs_xml, user_xml])
        linter = Linter(schema, LinterConfig(enable_xref=True), project_registry=registry)
        result = linter.lint_file(user_xml)
        style_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_REF
        ]
        assert style_errors == []


class TestCrossRefSubjectRefs:
    """Tests for subject reference validation."""

    def test_undefined_subject_ref_detected(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test undefined subject references are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(bad_refs_xml)
        subject_warnings = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_SUBJECT_REF
        ]
        assert len(subject_warnings) >= 1

    def test_defined_subject_not_flagged(self, schema: Schema, valid_refs_xml: Path) -> None:
        """Test defined subjects are not flagged."""
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(valid_refs_xml)
        subject_warnings = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_SUBJECT_REF
        ]
        assert len(subject_warnings) == 0

    def test_composed_indexed_name_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """A ${...} composed subject name (runtime-resolved) is not flagged."""
        xml = tmp_path / "composed.xml"
        xml.write_text(
            "<component><view extends='lv_obj'>"
            "<lv_label bind_text='demo_${i}_v'/>"
            "</view></component>",
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(xml)
        subject_warnings = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_SUBJECT_REF
        ]
        assert subject_warnings == []

    def test_plain_undefined_subject_still_flagged_alongside_composed(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """A brace-free undefined subject still warns even next to a ${...} name."""
        xml = tmp_path / "mixed.xml"
        xml.write_text(
            "<component><view extends='lv_obj'>"
            "<lv_label bind_text='demo_${i}_v'/>"
            "<lv_label bind_text='no_such_subject'/>"
            "</view></component>",
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=True))
        result = linter.lint_file(xml)
        subject_warnings = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_SUBJECT_REF
        ]
        assert len(subject_warnings) == 1
        assert subject_warnings[0].value == "no_such_subject"


class TestCrossRefDefinitions:
    """Tests for definition collection."""

    def test_collects_const_names(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test that constant names are collected."""
        elements, _ = parse_xml_file(bad_refs_xml)
        xref = CrossRefValidator(schema)
        xref.collect_definitions(elements)
        assert "known_pad" in xref.definitions.const_names

    def test_collects_style_names(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test that style names are collected."""
        elements, _ = parse_xml_file(bad_refs_xml)
        xref = CrossRefValidator(schema)
        xref.collect_definitions(elements)
        assert "known_style" in xref.definitions.style_names

    def test_collects_subject_names(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test that subject names are collected."""
        elements, _ = parse_xml_file(bad_refs_xml)
        xref = CrossRefValidator(schema)
        xref.collect_definitions(elements)
        assert "known_subject" in xref.definitions.subject_names

    def test_collects_event_callback_names(self, schema: Schema, valid_refs_xml: Path) -> None:
        """Test that event callback names are collected."""
        elements, _ = parse_xml_file(valid_refs_xml)
        xref = CrossRefValidator(schema)
        xref.collect_definitions(elements)
        assert "on_submit" in xref.definitions.event_callback_names

    def test_collects_component_view_names(self, schema: Schema, valid_basic_xml: Path) -> None:
        """Test that view names are collected as component names."""
        elements, _ = parse_xml_file(valid_basic_xml)
        xref = CrossRefValidator(schema)
        xref.collect_definitions(elements)
        assert "my_view" in xref.definitions.component_names


class TestCrossRefXrefDisabled:
    """Tests for xref-disabled mode."""

    def test_no_xref_no_subject_errors(self, schema: Schema, bad_refs_xml: Path) -> None:
        """Test that with xref disabled, no cross-reference diagnostics appear."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(bad_refs_xml)
        xref_diags = [
            d
            for d in result.diagnostics
            if d.check
            in (
                CheckType.UNKNOWN_CONST_REF,
                CheckType.UNKNOWN_STYLE_REF,
                CheckType.UNKNOWN_SUBJECT_REF,
            )
        ]
        assert len(xref_diags) == 0
