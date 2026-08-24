"""Cross-reference validator — validates references to constants, styles, subjects, and events."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .diagnostics import CheckType, Diagnostic, Severity
from .schema_loader import Schema
from .xml_parser import ParsedElement, parse_xml_file

# Pattern matching hex color literals like #FFFFFF, #bf616a, #fff
_HEX_COLOR_RE = re.compile(r"^#[0-9a-fA-F]{3,8}$")

# Pattern matching hex template placeholders like #RRGGBB, #AARRGGBB
# These use uppercase letters outside the hex range (R, G, B, etc.) as format templates
_HEX_TEMPLATE_RE = re.compile(r"^#[A-Z]{3,8}$")

_CONST_DEFINITION_TAGS = frozenset({"px", "string", "color", "font", "int", "str", "percentage"})
_FONT_DEFINITION_TAGS = frozenset({"font", "tiny_ttf", "bin"})
_STYLE_REF_TAGS = frozenset(
    {
        "style",
        "remove_style",
        "bind_style",
        "bind_style_if_eq",
        "bind_style_if_not_eq",
        "bind_style_if_gt",
        "bind_style_if_ge",
        "bind_style_if_lt",
        "bind_style_if_le",
        "lv_obj-style",
        "lv_obj-remove_style",
        "lv_obj-bind_style",
        "lv_obj-bind_style_if_eq",
        "lv_obj-bind_style_if_not_eq",
        "lv_obj-bind_style_if_gt",
        "lv_obj-bind_style_if_ge",
        "lv_obj-bind_style_if_lt",
        "lv_obj-bind_style_if_le",
    }
)


def _is_hex_color(value: str) -> bool:
    """Check if a value is a hex color literal like #FFFFFF or #0xff0000."""
    return value.startswith("#0x") or bool(_HEX_COLOR_RE.match(value))


def _is_hex_template(value: str) -> bool:
    """Check if a value is a hex color template placeholder like #RRGGBB.

    These are not actual color values or constant references — they are
    template format strings used in documentation/examples.
    """
    return bool(_HEX_TEMPLATE_RE.match(value)) and not _is_hex_color(value)


@dataclass
class ProjectRegistry:
    """Shared definition registry across all files in a project.

    Collected during Pass 1 so that Pass 2 linting can resolve references
    that span multiple XML files (e.g. constants from globals.xml).
    """

    const_names: set[str] = field(default_factory=set)
    subject_names: set[str] = field(default_factory=set)
    style_names: set[str] = field(default_factory=set)
    event_callback_names: set[str] = field(default_factory=set)
    font_names: set[str] = field(default_factory=set)
    component_view_names: dict[str, str] = field(default_factory=dict)
    # component tag (from filename stem) -> extends widget name

    component_params: dict[str, dict[str, str]] = field(default_factory=dict)
    # component tag -> {prop name -> declared type ("string", "bool", "int", ...)}.
    # A component's <api> prop declaration overrides the base widget's schema
    # for that attribute (e.g. setting_toggle_row's `disabled` is a subject
    # name, shadowing lv_obj's boolean of the same name).

    @classmethod
    def from_files(cls, paths: list[Path]) -> ProjectRegistry:
        """Scan all files and merge definitions into a single registry."""
        registry = cls()
        for path in paths:
            file_registry = _collect_registry_from_file(path)
            registry.merge(file_registry)
        return registry

    def merge(self, other: ProjectRegistry) -> None:
        """Merge another registry's definitions into this one."""
        self.const_names |= other.const_names
        self.subject_names |= other.subject_names
        self.style_names |= other.style_names
        self.event_callback_names |= other.event_callback_names
        self.font_names |= other.font_names
        self.component_view_names.update(other.component_view_names)
        for comp_tag, params in other.component_params.items():
            if comp_tag in self.component_params:
                self.component_params[comp_tag] |= params
            else:
                self.component_params[comp_tag] = dict(params)


@dataclass
class Definitions:
    """Collected definitions from an XML file for cross-reference validation."""

    const_names: set[str] = field(default_factory=set)
    style_names: set[str] = field(default_factory=set)
    subject_names: set[str] = field(default_factory=set)
    event_callback_names: set[str] = field(default_factory=set)
    font_names: set[str] = field(default_factory=set)
    component_names: set[str] = field(default_factory=set)


class CrossRefValidator:
    """Validates cross-references between XML element definitions and references.

    Collects definitions of constants, styles, subjects, and events from
    an XML file, then validates that all references point to existing
    definitions.  When a *ProjectRegistry* is provided, definitions from
    other files in the project are also consulted so that cross-file
    references (e.g. constants defined in globals.xml) resolve correctly.
    """

    def __init__(self, schema: Schema, project_registry: ProjectRegistry | None = None) -> None:
        self._schema = schema
        self._definitions = Definitions()
        self._project_registry = project_registry

    def collect_definitions(self, elements: list[ParsedElement]) -> None:
        """Scan elements and collect all named definitions.

        Populates the internal definitions registry for later reference checking.
        Uses parent information for correct scoping.
        """
        self._definitions = Definitions()
        self._collect_from_tree(elements)

    def _collect_from_tree(self, elements: list[ParsedElement]) -> None:
        """Collect definitions using parent information for correct scoping."""
        for elem in elements:
            # Constants: children of <consts>
            if (
                elem.parent_tag == "consts"
                and "name" in elem.attributes
                and elem.tag in _CONST_DEFINITION_TAGS
            ):
                self._definitions.const_names.add(elem.attributes["name"])
                if elem.tag in _FONT_DEFINITION_TAGS:
                    self._definitions.font_names.add(elem.attributes["name"])

            # Fonts section: <fonts><tiny_ttf name="...">, <bin name="...">
            if elem.parent_tag == "fonts" and elem.tag in _FONT_DEFINITION_TAGS and "name" in elem.attributes:
                self._definitions.font_names.add(elem.attributes["name"])

            # Images section: image sources can be referenced by name.
            if elem.parent_tag == "images" and "name" in elem.attributes:
                self._definitions.const_names.add(elem.attributes["name"])

            # Style blocks: children of <styles>
            if elem.parent_tag == "styles" and elem.tag == "style" and "name" in elem.attributes:
                self._definitions.style_names.add(elem.attributes["name"])

            # Subjects: direct children of <subjects>
            if (
                elem.tag == "subject"
                and elem.parent_tag == "subjects"
                and "name" in elem.attributes
            ):
                self._definitions.subject_names.add(elem.attributes["name"])

            # Event callbacks
            if elem.tag == "event_cb" and "callback" in elem.attributes:
                self._definitions.event_callback_names.add(elem.attributes["callback"])

            # Component/view names
            if elem.tag == "view" and "name" in elem.attributes:
                self._definitions.component_names.add(elem.attributes["name"])

    def validate_references(self, elements: list[ParsedElement]) -> list[Diagnostic]:
        """Validate all cross-references in the given elements.

        Returns diagnostics for any unresolved references.
        """
        diagnostics: list[Diagnostic] = []

        for elem in elements:
            diagnostics.extend(self._check_element_references(elem))

        return diagnostics

    def _check_element_references(self, elem: ParsedElement) -> list[Diagnostic]:
        """Check cross-references for a single element."""
        diagnostics: list[Diagnostic] = []

        for attr_name, attr_value in elem.attributes.items():
            # Constant references: #name (but not hex color literals or template placeholders)
            if attr_value.startswith("#") and not _is_hex_color(attr_value) and not _is_hex_template(attr_value):
                const_name = attr_value[1:]
                if (
                    const_name not in self._definitions.const_names
                    and not self._is_project_const(const_name)
                    and not self._schema.is_valid_constant(const_name)
                ):
                    diagnostics.append(
                        Diagnostic(
                            file=elem.source_file,
                            line=elem.line,
                            column=elem.column,
                            severity=Severity.ERROR,
                            check=CheckType.UNKNOWN_CONST_REF,
                            message=f"Undefined constant reference: #{const_name}",
                            element=elem.tag,
                            attribute=attr_name,
                            value=attr_value,
                        )
                    )

            # Subject references from bind_* attributes. The subject="..."
            # element attribute is intentionally not checked yet because many
            # project subjects are registered by C++ runtime code and are not
            # represented in schema.json.
            if _attribute_references_subject(attr_name):
                subject_diag = self._check_subject_reference(elem, attr_name, attr_value)
                if subject_diag is not None:
                    diagnostics.append(subject_diag)

            # Callback references in event_cb elements are C functions —
            # they are recorded during collection but not validated here
            # since they're external to the XML.

            # Font references in style_text_font
            if attr_name == "style_text_font" and attr_value.startswith("#"):
                font_name = attr_value[1:]
                if (
                    font_name not in self._definitions.font_names
                    and not self._is_project_font(font_name)
                    and not self._schema.is_valid_constant(font_name)
                ):
                    diagnostics.append(
                        Diagnostic(
                            file=elem.source_file,
                            line=elem.line,
                            column=elem.column,
                            severity=Severity.WARNING,
                            check=CheckType.UNKNOWN_FONT_REF,
                            message=f"Unknown font reference: #{font_name}",
                            element=elem.tag,
                            attribute=attr_name,
                            value=attr_value,
                        )
                    )

        # Check style block references (<bind_style>, <lv_obj-style>, aliases).
        if elem.tag in _STYLE_REF_TAGS and "name" in elem.attributes:
            style_name = elem.attributes["name"]
            if not self._style_exists(style_name):
                diagnostics.append(
                    Diagnostic(
                        file=elem.source_file,
                        line=elem.line,
                        column=elem.column,
                        severity=Severity.ERROR,
                        check=CheckType.UNKNOWN_STYLE_REF,
                        message=f"Undefined style reference: {style_name}",
                        element=elem.tag,
                        attribute="name",
                        value=style_name,
                    )
                )

        return diagnostics

    def _is_project_const(self, name: str) -> bool:
        """Check if a name is defined in the project-level constant registry."""
        return (
            self._project_registry is not None
            and name in self._project_registry.const_names
        )

    def _is_project_subject(self, name: str) -> bool:
        """Check if a name is defined in the project-level subject registry."""
        return (
            self._project_registry is not None
            and name in self._project_registry.subject_names
        )

    def _is_project_style(self, name: str) -> bool:
        """Check if a name is defined in the project-level style registry."""
        return (
            self._project_registry is not None
            and name in self._project_registry.style_names
        )

    def _style_exists(self, raw_name: str) -> bool:
        """Return True if a style reference resolves locally or project-wide."""
        style_name = _local_style_name(raw_name)
        return (
            style_name in self._definitions.style_names
            or self._is_project_style(style_name)
            or self._is_project_style(raw_name)
        )

    def _check_subject_reference(
        self, elem: ParsedElement, attr_name: str, attr_value: str
    ) -> Diagnostic | None:
        """Validate a subject reference and return a diagnostic if unresolved."""
        # Skip constant references, component params, and empty default placeholders.
        if not attr_value or attr_value.startswith(("#", "$")):
            return None

        # Skip embedded ${name} composition (e.g. bind_text="demo_${i}_v"). The
        # composed subject name is only known at runtime once the <repeat> loop
        # index / component params are spliced in, so it cannot be statically
        # resolved — the C++ side is responsible for registering the indexed
        # subjects. A plain (brace-free) name still falls through and is checked.
        if "${" in attr_value:
            return None

        if (
            attr_value in self._definitions.subject_names
            or self._is_project_subject(attr_value)
            or self._schema.is_valid_subject(attr_value)
        ):
            return None

        return Diagnostic(
            file=elem.source_file,
            line=elem.line,
            column=elem.column,
            severity=Severity.WARNING,
            check=CheckType.UNKNOWN_SUBJECT_REF,
            message=f"Unknown subject reference: {attr_value}",
            element=elem.tag,
            attribute=attr_name,
            value=attr_value,
        )

    def _is_project_font(self, name: str) -> bool:
        """Check if a name is defined in the project-level font registry."""
        return (
            self._project_registry is not None
            and name in self._project_registry.font_names
        )

    @property
    def definitions(self) -> Definitions:
        """Access the collected definitions (for testing/debugging)."""
        return self._definitions


def _collect_registry_from_file(path: Path) -> ProjectRegistry:
    """Parse a single file and extract definitions into a ProjectRegistry.

    This is used during Pass 1 to build a project-wide definition index.
    Parse errors are silently skipped — they'll be reported during Pass 2.
    """
    registry = ProjectRegistry()

    elements, _parse_diagnostics = parse_xml_file(path)
    if not elements:
        return registry

    # Determine if this file is a component (root tag is "component")
    root = elements[0] if elements else None
    is_component = root is not None and root.tag == "component"

    for elem in elements:
        # Constants: children of <consts> with a name attribute
        if (
            elem.parent_tag == "consts"
            and "name" in elem.attributes
            and elem.tag in _CONST_DEFINITION_TAGS
        ):
            registry.const_names.add(elem.attributes["name"])
            if elem.tag in _FONT_DEFINITION_TAGS:
                registry.font_names.add(elem.attributes["name"])

        # Fonts section: <fonts><tiny_ttf name="...">, <bin name="...">
        if elem.parent_tag == "fonts" and elem.tag in _FONT_DEFINITION_TAGS and "name" in elem.attributes:
            registry.font_names.add(elem.attributes["name"])

        # Images section: image names are valid # references for image attrs.
        if elem.parent_tag == "images" and "name" in elem.attributes:
            registry.const_names.add(elem.attributes["name"])

        # Style blocks: children of <styles>
        if elem.parent_tag == "styles" and elem.tag == "style" and "name" in elem.attributes:
            registry.style_names.add(elem.attributes["name"])
            registry.style_names.add(f"{path.stem}.{elem.attributes['name']}")

        # Subjects: direct children of <subjects>
        if elem.tag == "subject" and elem.parent_tag == "subjects" and "name" in elem.attributes:
            registry.subject_names.add(elem.attributes["name"])

        # Event callbacks
        if elem.tag == "event_cb" and "callback" in elem.attributes:
            registry.event_callback_names.add(elem.attributes["callback"])

    # For component files, register the filename stem as a custom widget
    if is_component:
        component_tag = path.stem
        prop_types: dict[str, str] = {}

        # Collect <prop> names+types from <api> section (type defaults to
        # "string", matching the runtime's prop resolution)
        for elem in elements:
            if elem.tag == "prop" and elem.parent_tag == "api" and "name" in elem.attributes:
                prop_types[elem.attributes["name"]] = elem.attributes.get("type", "string")

        if prop_types:
            registry.component_params[component_tag] = prop_types

        # Find the primary <view> (direct child of <component>) with an extends attr
        for elem in elements:
            if elem.tag == "view" and elem.parent_tag == "component" and "extends" in elem.attributes:
                extends_widget = elem.attributes["extends"]
                registry.component_view_names[component_tag] = extends_widget
                break
        else:
            # Component with no extends — still register it so it's not flagged as unknown
            registry.component_view_names[component_tag] = "lv_obj"

    return registry


def _attribute_references_subject(attr_name: str) -> bool:
    """Return True if an attribute value is interpreted as a subject name."""
    return attr_name in {
        "bind_text",
        "bind_checked",
        "bind_value",
        "bind_selected",
        "bind_description",
    }


def _local_style_name(raw_name: str) -> str:
    """Strip an optional component prefix from a style reference."""
    return raw_name.rsplit(".", 1)[-1]
