"""Tests for ProjectRegistry and cross-file resolution."""

from __future__ import annotations

from pathlib import Path

import pytest

from helix_xml_linter.crossref import ProjectRegistry, _collect_registry_from_file
from helix_xml_linter.diagnostics import CheckType
from helix_xml_linter.linter import Linter, LinterConfig
from helix_xml_linter.schema_loader import Schema

# ── ProjectRegistry unit tests ──────────────────────────────────────────


class TestProjectRegistryMerge:
    """Tests for ProjectRegistry.merge."""

    def test_merge_consts(self) -> None:
        """Merging two registries combines const_names."""
        a = ProjectRegistry(const_names={"x", "y"})
        b = ProjectRegistry(const_names={"y", "z"})
        a.merge(b)
        assert a.const_names == {"x", "y", "z"}

    def test_merge_subjects(self) -> None:
        """Merging two registries combines subject_names."""
        a = ProjectRegistry(subject_names={"sub_a"})
        b = ProjectRegistry(subject_names={"sub_b"})
        a.merge(b)
        assert a.subject_names == {"sub_a", "sub_b"}

    def test_merge_styles(self) -> None:
        """Merging two registries combines style_names."""
        a = ProjectRegistry(style_names={"s1"})
        b = ProjectRegistry(style_names={"s2"})
        a.merge(b)
        assert a.style_names == {"s1", "s2"}

    def test_merge_event_callbacks(self) -> None:
        """Merging two registries combines event_callback_names."""
        a = ProjectRegistry(event_callback_names={"cb1"})
        b = ProjectRegistry(event_callback_names={"cb2"})
        a.merge(b)
        assert a.event_callback_names == {"cb1", "cb2"}

    def test_merge_fonts(self) -> None:
        """Merging two registries combines font_names."""
        a = ProjectRegistry(font_names={"font_a"})
        b = ProjectRegistry(font_names={"font_b"})
        a.merge(b)
        assert a.font_names == {"font_a", "font_b"}

    def test_merge_component_views(self) -> None:
        """Merging two registries combines component_view_names."""
        a = ProjectRegistry(component_view_names={"comp_a": "lv_obj"})
        b = ProjectRegistry(component_view_names={"comp_b": "lv_label"})
        a.merge(b)
        assert a.component_view_names == {"comp_a": "lv_obj", "comp_b": "lv_label"}

    def test_merge_does_not_mutate_source(self) -> None:
        """Merging does not mutate the source registry."""
        a = ProjectRegistry(const_names={"x"})
        b = ProjectRegistry(const_names={"y"})
        a.merge(b)
        assert b.const_names == {"y"}


class TestProjectRegistryFromFiles:
    """Tests for ProjectRegistry.from_files."""

    def test_collects_consts_from_globals(
        self, schema_path: Path, tmp_path: Path
    ) -> None:
        """from_files collects const names from all files."""
        xml = tmp_path / "globals.xml"
        xml.write_text(
            '<component><consts><px name="space_md" value="10"/></consts>'
            '<view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "space_md" in registry.const_names

    def test_collects_subjects_from_file(self, tmp_path: Path) -> None:
        """from_files collects subject names from all files."""
        xml = tmp_path / "subs.xml"
        xml.write_text(
            '<component><subjects><subject name="my_sub" type="string" value=""/>'
            '</subjects><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "my_sub" in registry.subject_names

    def test_collects_styles_from_file(self, tmp_path: Path) -> None:
        """from_files collects style names from all files."""
        xml = tmp_path / "styles.xml"
        xml.write_text(
            '<component><styles><style name="my_style" border_width="2"/>'
            '</styles><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "my_style" in registry.style_names

    def test_collects_fonts_from_file(self, tmp_path: Path) -> None:
        """from_files collects font names from <font> elements."""
        xml = tmp_path / "fonts.xml"
        xml.write_text(
            '<component><consts><font name="my_font" value="arial_12"/>'
            '</consts><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "my_font" in registry.font_names
        assert "my_font" in registry.const_names

    def test_registers_component_by_filename(self, tmp_path: Path) -> None:
        """Component files register their filename stem as a widget."""
        xml = tmp_path / "my_widget.xml"
        xml.write_text(
            '<component><view name="inner" extends="lv_label" width="100%">'
            '</view></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "my_widget" in registry.component_view_names
        assert registry.component_view_names["my_widget"] == "lv_label"

    def test_registers_component_without_extends(self, tmp_path: Path) -> None:
        """Component with no extends defaults to lv_obj."""
        xml = tmp_path / "plain_comp.xml"
        xml.write_text(
            '<component><view name="inner" width="100%"/>'
            '</component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "plain_comp" in registry.component_view_names
        assert registry.component_view_names["plain_comp"] == "lv_obj"

    def test_collects_font_from_fonts_section(self, tmp_path: Path) -> None:
        """Font definitions from <fonts> blocks are collected."""
        xml = tmp_path / "fonts.xml"
        xml.write_text(
            '<component><fonts><tiny_ttf name="inter_md" src_path="Inter.ttf" '
            'size="16" as_file="true"/></fonts><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "inter_md" in registry.font_names

    def test_merges_multiple_files(self, tmp_path: Path) -> None:
        """from_files merges definitions from multiple files."""
        globals_xml = tmp_path / "globals.xml"
        globals_xml.write_text(
            '<component><consts><px name="global_pad" value="8"/>'
            '</consts><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component><consts><px name="local_pad" value="4"/>'
            '</consts><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([globals_xml, user_xml])
        assert "global_pad" in registry.const_names
        assert "local_pad" in registry.const_names

    def test_str_const_tag(self, tmp_path: Path) -> None:
        """<str name="..."> is collected as a const name."""
        xml = tmp_path / "icons.xml"
        xml.write_text(
            '<component><consts><str name="icon_close" value="X"/>'
            '</consts><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "icon_close" in registry.const_names

    def test_percentage_const_tag(self, tmp_path: Path) -> None:
        """<percentage name="..."> is collected as a const name."""
        xml = tmp_path / "sizes.xml"
        xml.write_text(
            '<component><consts><percentage name="card_width" value="45%"/>'
            '</consts><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        registry = ProjectRegistry.from_files([xml])
        assert "card_width" in registry.const_names

    def test_handles_malformed_xml_gracefully(self, tmp_path: Path) -> None:
        """from_files does not crash on malformed XML."""
        xml = tmp_path / "broken.xml"
        xml.write_text("<component><consts><px name=", encoding="utf-8")
        registry = ProjectRegistry.from_files([xml])
        assert len(registry.const_names) == 0


class TestCollectRegistryFromFile:
    """Tests for _collect_registry_from_file."""

    def test_nonexistent_file_returns_empty(self, tmp_path: Path) -> None:
        """Nonexistent file returns empty registry."""
        registry = _collect_registry_from_file(tmp_path / "missing.xml")
        assert len(registry.const_names) == 0

    def test_non_component_file(self, tmp_path: Path) -> None:
        """Translation file (root=translations) is not registered as component."""
        xml = tmp_path / "en.xml"
        xml.write_text(
            '<translations languages="en">'
            '<translation tag="hello" en="Hello"/>'
            '</translations>',
            encoding="utf-8",
        )
        registry = _collect_registry_from_file(xml)
        assert "en" not in registry.component_view_names


# ── Cross-file resolution in Linter ──────────────────────────────────────


class TestCrossFileConstResolution:
    """Tests that constants from one file resolve in another."""

    def test_global_const_resolved_across_files(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Constants from globals.xml resolve in other files."""
        globals_xml = tmp_path / "globals.xml"
        globals_xml.write_text(
            '<component><consts><px name="custom_spacing" value="10"/></consts>'
            '<view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="#custom_spacing" height="50"/>'
            '</component>',
            encoding="utf-8",
        )

        # Single-file mode: should flag the const
        single_linter = Linter(schema, LinterConfig(enable_xref=True))
        single_result = single_linter.lint_file(user_xml)
        single_errors = [
            d for d in single_result.diagnostics if d.check == CheckType.UNKNOWN_CONST_REF
        ]
        assert len(single_errors) >= 1

        # Multi-file mode: should resolve the const
        registry = ProjectRegistry.from_files([globals_xml, user_xml])
        multi_linter = Linter(schema, LinterConfig(enable_xref=True), project_registry=registry)
        multi_result = multi_linter.lint_file(user_xml)
        multi_errors = [
            d for d in multi_result.diagnostics if d.check == CheckType.UNKNOWN_CONST_REF
            and "custom_spacing" in d.message
        ]
        assert len(multi_errors) == 0

    def test_style_ref_resolved_across_files(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Styles from one file resolve in another."""
        defs_xml = tmp_path / "defs.xml"
        defs_xml.write_text(
            '<component><styles><style name="my_style" border_width="2"/></styles>'
            '<view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="100%" height="50">'
            '<bind_style name="my_style"/>'
            '</view></component>',
            encoding="utf-8",
        )

        registry = ProjectRegistry.from_files([defs_xml, user_xml])
        linter = Linter(schema, LinterConfig(enable_xref=True), project_registry=registry)
        result = linter.lint_file(user_xml)
        style_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_REF
        ]
        assert len(style_errors) == 0


class TestCrossFileSubjectResolution:
    """Tests that subjects from one file resolve in another."""

    def test_subject_resolved_across_files(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Subjects from one file resolve in another."""
        defs_xml = tmp_path / "defs.xml"
        defs_xml.write_text(
            '<component><subjects><subject name="temp" type="string" value=""/>'
            '</subjects><view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="100%" height="50">'
            '<lv_label text="" bind_text="temp"/>'
            '</view></component>',
            encoding="utf-8",
        )

        registry = ProjectRegistry.from_files([defs_xml, user_xml])
        linter = Linter(schema, LinterConfig(enable_xref=True), project_registry=registry)
        result = linter.lint_file(user_xml)
        subject_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_SUBJECT_REF
        ]
        assert len(subject_errors) == 0


class TestCrossFileComponentResolution:
    """Tests that custom component widgets from one file are recognized in another."""

    def test_custom_widget_not_flagged_as_unknown(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """A component defined in another file is not flagged as unknown widget."""
        comp_xml = tmp_path / "my_card.xml"
        comp_xml.write_text(
            '<component>'
            '<view name="card" extends="lv_obj" width="100%" height="content"/>'
            '</component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="100%" height="content">'
            '<my_card width="100%" height="50"/>'
            '</view></component>',
            encoding="utf-8",
        )

        registry = ProjectRegistry.from_files([comp_xml, user_xml])
        # Register custom widgets in schema
        for name, extends in registry.component_view_names.items():
            schema.register_custom_widget(name, extends)

        linter = Linter(schema, LinterConfig(enable_xref=False), project_registry=registry)
        result = linter.lint_file(user_xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
            and d.element == "my_card"
        ]
        assert len(widget_errors) == 0

    def test_custom_widget_inherits_base_attributes(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Custom widget inheriting from lv_label allows text attribute."""
        comp_xml = tmp_path / "text_small.xml"
        comp_xml.write_text(
            '<component>'
            '<view name="text_small" extends="lv_label" width="100%" height="content"/>'
            '</component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="100%" height="content">'
            '<text_small text="Hello" long_mode="wrap"/>'
            '</view></component>',
            encoding="utf-8",
        )

        registry = ProjectRegistry.from_files([comp_xml, user_xml])
        for name, extends in registry.component_view_names.items():
            schema.register_custom_widget(name, extends)

        linter = Linter(schema, LinterConfig(enable_xref=False), project_registry=registry)
        result = linter.lint_file(user_xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
        ]
        assert len(widget_errors) == 0
        attr_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_ATTRIBUTE
            and d.element == "text_small"
        ]
        assert len(attr_errors) == 0

    def test_custom_widget_unknown_param_is_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Typos in component API params should not be silently accepted."""
        comp_xml = tmp_path / "setting_row.xml"
        comp_xml.write_text(
            '<component><api><prop name="label" type="string"/></api>'
            '<view extends="lv_label" text="$label"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component><view extends="lv_obj">'
            '<setting_row lable="Bad typo"/>'
            '</view></component>',
            encoding="utf-8",
        )

        registry = ProjectRegistry.from_files([comp_xml, user_xml])
        for name, extends in registry.component_view_names.items():
            schema.register_custom_widget(name, extends)

        linter = Linter(schema, LinterConfig(enable_xref=False), project_registry=registry)
        result = linter.lint_file(user_xml)
        attr_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_ATTRIBUTE
        ]
        assert any(d.attribute == "lable" for d in attr_errors)

    def test_custom_widget_declared_param_is_allowed(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Declared component API params are still accepted."""
        comp_xml = tmp_path / "setting_row.xml"
        comp_xml.write_text(
            '<component><api><prop name="label" type="string"/></api>'
            '<view extends="lv_label" text="$label"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component><view extends="lv_obj">'
            '<setting_row label="Good"/>'
            '</view></component>',
            encoding="utf-8",
        )

        registry = ProjectRegistry.from_files([comp_xml, user_xml])
        for name, extends in registry.component_view_names.items():
            schema.register_custom_widget(name, extends)

        linter = Linter(schema, LinterConfig(enable_xref=False), project_registry=registry)
        result = linter.lint_file(user_xml)
        attr_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_ATTRIBUTE
        ]
        assert attr_errors == []


# ── Structural tag tests ────────────────────────────────────────────────


class TestStructuralTags:
    """Tests for newly recognized structural tags."""

    def test_translation_element_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """<translation> elements are not flagged as unknown widgets."""
        xml = tmp_path / "translations.xml"
        xml.write_text(
            '<translations languages="en">'
            '<translation tag="hello" en="Hello"/>'
            '</translations>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
        ]
        assert len(widget_errors) == 0

    def test_api_element_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """<api> element inside component is not flagged."""
        xml = tmp_path / "comp.xml"
        xml.write_text(
            '<component>'
            '<api><prop name="label" type="string" default="Test"/></api>'
            '<view extends="lv_obj" width="100%" height="content"/>'
            '</component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
        ]
        assert len(widget_errors) == 0

    def test_prop_inside_api_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """<prop> inside <api> is not flagged."""
        xml = tmp_path / "comp.xml"
        xml.write_text(
            '<component>'
            '<api><prop name="label" type="string" default="Test"/></api>'
            '<view extends="lv_obj" width="100%"/>'
            '</component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
        ]
        assert len(widget_errors) == 0

    def test_percentage_const_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """<percentage> inside <consts> is not flagged as unknown widget."""
        xml = tmp_path / "sizes.xml"
        xml.write_text(
            '<component>'
            '<consts><percentage name="card_w" value="45%"/></consts>'
            '<view extends="lv_obj" width="100%"/>'
            '</component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
        ]
        assert len(widget_errors) == 0

    def test_str_const_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """<str> inside <consts> is not flagged as unknown widget."""
        xml = tmp_path / "icons.xml"
        xml.write_text(
            '<component>'
            '<consts><str name="icon_close" value="X"/></consts>'
            '<view extends="lv_obj" width="100%"/>'
            '</component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
        ]
        assert len(widget_errors) == 0

    def test_metadata_sections_not_flagged(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        """Engine metadata sections are structural, not widgets."""
        xml = tmp_path / "metadata.xml"
        xml.write_text(
            '<component>'
            '<fonts><tiny_ttf name="inter" src_path="Inter.ttf" size="16" as_file="true"/></fonts>'
            '<images><file name="logo" src_path="logo.png"/></images>'
            '<gradients><linear name="fade" start="0 0" end="100 0">'
            '<stop color="#fff" opa="255" offset="0"/></linear></gradients>'
            '<timeline name="intro"><animation target="self" prop="opa" start="0" end="255"/></timeline>'
            '<view extends="lv_obj"/>'
            '</component>',
            encoding="utf-8",
        )
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(xml)
        widget_errors = [
            d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET
        ]
        assert widget_errors == []


# ── CLI two-pass integration tests ───────────────────────────────────────


class TestCLITwoPass:
    """Tests for CLI two-pass linting."""

    def test_directory_linting_resolves_cross_file_refs(
        self, schema_path: Path, tmp_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Linting a directory resolves cross-file references."""
        from helix_xml_linter.cli import main

        globals_xml = tmp_path / "globals.xml"
        globals_xml.write_text(
            '<component><consts><px name="space_md" value="10"/></consts>'
            '<view extends="lv_obj"/></component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="#space_md" height="50"/>'
            '</component>',
            encoding="utf-8",
        )

        main(["-s", str(schema_path), str(tmp_path)])
        captured = capsys.readouterr()

        # The constant should be resolved across files
        assert "space_md" not in captured.out or "Undefined" not in captured.out

    def test_single_file_mode_preserves_behavior(
        self, schema_path: Path, tmp_path: Path
    ) -> None:
        """Single file mode still flags unresolved references."""
        from helix_xml_linter.cli import main

        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="#unknown_global" height="50"/>'
            '</component>',
            encoding="utf-8",
        )

        exit_code = main(["-s", str(schema_path), str(user_xml)])
        assert exit_code == 1  # Should have errors


class TestComponentPropTypeOverride:
    """A component's <api> prop declaration overrides the base-widget schema.

    setting_toggle_row redeclares `disabled` as a string (a subject name),
    shadowing lv_obj's boolean of the same name. The bool-value check must
    not fire on a prop the component itself re-typed — and must still fire
    on the base attribute everywhere else.
    """

    def test_retyped_prop_accepts_subject_name(
        self, schema: Schema, tmp_path: Path
    ) -> None:
        comp_xml = tmp_path / "setting_toggle_row.xml"
        comp_xml.write_text(
            '<component>'
            '<api><prop name="disabled" type="string" default=""/></api>'
            '<view name="setting_row" extends="lv_obj" width="100%" height="content"/>'
            '</component>',
            encoding="utf-8",
        )
        user_xml = tmp_path / "user.xml"
        user_xml.write_text(
            '<component>'
            '<view name="v" extends="lv_obj" width="100%" height="content">'
            '<setting_toggle_row name="row" disabled="ams_device_ops_fw_retains_spool_info"/>'
            '</view></component>',
            encoding="utf-8",
        )

        registry = ProjectRegistry.from_files([comp_xml, user_xml])
        for name, extends in registry.component_view_names.items():
            schema.register_custom_widget(name, extends)

        linter = Linter(schema, LinterConfig(enable_xref=False), project_registry=registry)
        result = linter.lint_file(user_xml)
        bool_errors = [
            d for d in result.diagnostics if d.check == CheckType.INVALID_BOOL_VALUE
        ]
        assert len(bool_errors) == 0, [d.message for d in bool_errors]

    def test_base_bool_attribute_still_validated(self, schema: Schema) -> None:
        """Without a component prop override, the base bool check fires."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"disabled": "some_subject_name"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        bool_errors = [d for d in diagnostics if d.check == CheckType.INVALID_BOOL_VALUE]
        assert len(bool_errors) == 1
        assert "disabled" in bool_errors[0].message
