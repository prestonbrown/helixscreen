"""Core linting engine — orchestrates schema-driven XML validation."""

from __future__ import annotations

from pathlib import Path

from .constants import DEFINITION_TAGS, STRUCTURAL_TAGS
from .crossref import CrossRefValidator, ProjectRegistry
from .diagnostics import CheckType, Diagnostic, LintResult, Severity
from .schema_loader import AttributeSchema, Schema
from .xml_parser import ParsedElement, parse_xml_file

# Attributes that use subject references — these accept subject names as values
_BIND_ATTRIBUTES = frozenset(
    {
        "bind_text",
        "bind_checked",
        "bind_value",
        "bind_selected",
        "bind_description",
    }
)

# Prefixes for attribute categories handled specially
_STYLE_PREFIX = "style_"
_BIND_PREFIX = "bind_"
_CALLBACK_ATTR = "callback"

# Attribute name for widget extension
_EXTENDS_ATTR = "extends"


class LinterConfig:
    """Configuration for the linter engine."""

    def __init__(
        self,
        fail_on_warning: bool = False,
        quiet: bool = False,
        min_severity: Severity = Severity.INFO,
        enable_xref: bool = True,
    ) -> None:
        self.fail_on_warning = fail_on_warning
        self.quiet = quiet
        self.min_severity = min_severity
        self.enable_xref = enable_xref


class Linter:
    """Schema-driven linting engine for helix-xml files.

    Validates XML elements against a loaded schema, checking for:
    - Unknown widget types
    - Unknown attributes
    - Invalid enum values
    - Invalid boolean values
    - Unknown style properties
    - Cross-reference integrity (optional)
    """

    def __init__(self, schema: Schema, config: LinterConfig | None = None, project_registry: ProjectRegistry | None = None) -> None:
        self._schema = schema
        self._config = config or LinterConfig()
        self._xref = CrossRefValidator(schema, project_registry) if self._config.enable_xref else None
        self._project_registry = project_registry

    def lint_file(self, path: Path) -> LintResult:
        """Lint a single XML file and return a LintResult.

        Parses the file, runs all validation passes, and collects diagnostics.
        """
        elements, parse_diagnostics = parse_xml_file(path)

        all_diagnostics: list[Diagnostic] = list(parse_diagnostics)

        # Build a map from tag -> resolved widget name for extends resolution
        extends_map = self._build_extends_map(elements)

        # Tier 1: Schema validation
        for elem in elements:
            element_diagnostics = self.lint_element(elem, extends_map)
            all_diagnostics.extend(element_diagnostics)

        # Tier 2: Cross-reference validation
        if self._xref is not None:
            self._xref.collect_definitions(elements)
            xref_diagnostics = self._xref.validate_references(elements)
            all_diagnostics.extend(xref_diagnostics)

        # Filter by minimum severity
        filtered = [
            d for d in all_diagnostics if _severity_gte(d.severity, self._config.min_severity)
        ]

        return LintResult(file=path, diagnostics=filtered)

    def lint_element(
        self,
        element: ParsedElement,
        extends_map: dict[str, str] | None = None,
    ) -> list[Diagnostic]:
        """Validate a single XML element against the schema.

        Returns a list of diagnostics for any issues found.
        """
        diagnostics: list[Diagnostic] = []

        # Skip structural elements and definition elements
        if self._should_skip_element(element):
            return diagnostics

        # Resolve the widget name (may be a component that extends a real widget)
        widget_name = self._resolve_widget_name(element, extends_map)

        # Check 1: Unknown widget
        if (
            not self._schema.is_valid_widget(widget_name)
            and widget_name == element.tag
            and not self._is_known_custom_tag(element, extends_map)
        ):
            diagnostics.append(
                Diagnostic(
                    file=element.source_file,
                    line=element.line,
                    column=element.column,
                    severity=Severity.ERROR,
                    check=CheckType.UNKNOWN_WIDGET,
                    message=f"Unknown widget type: <{element.tag}>",
                    element=element.tag,
                )
            )
            return diagnostics  # No point checking attributes of unknown widgets

        # If the widget is still unknown, skip attribute validation
        if (
            widget_name not in self._schema.widgets
            and widget_name not in self._schema.special_elements
        ):
            # It might be a custom component — validate with extends base if available
            base = extends_map.get(element.tag) if extends_map else None
            if base and base in self._schema.widgets:
                widget_name = base
            else:
                # Unknown widget without base — skip attribute checks
                return diagnostics

        # Get valid attributes for this widget (with inheritance)
        valid_attrs = self._schema.get_valid_attributes(widget_name)

        # Check each attribute
        for attr_name, attr_value in element.attributes.items():
            attr_diagnostics = self._check_attribute(
                element, widget_name, valid_attrs, attr_name, attr_value
            )
            diagnostics.extend(attr_diagnostics)

        return diagnostics

    def _check_attribute(
        self,
        element: ParsedElement,
        widget_name: str,
        valid_attrs: set[str],
        attr_name: str,
        attr_value: str,
    ) -> list[Diagnostic]:
        """Validate a single attribute against the schema."""
        diagnostics: list[Diagnostic] = []

        # Skip special attributes
        if attr_name in (_EXTENDS_ATTR, "translation_tag", "name"):
            return diagnostics

        # Handle style_* attributes separately
        if attr_name.startswith(_STYLE_PREFIX):
            return self._check_style_attribute(element, attr_name, attr_value)

        # Handle bind_* attributes — these are valid on all widgets
        if attr_name.startswith(_BIND_PREFIX) and attr_name != "bind_text-fmt":
            return diagnostics  # Bind attributes are always valid, xref checks values

        # Handle bind_text-fmt — companion to bind_text
        if attr_name == "bind_text-fmt":
            return diagnostics

        # Handle callback attribute
        if attr_name == _CALLBACK_ATTR:
            return diagnostics  # Callbacks reference C functions, validated at runtime

        # Check if attribute is known for this widget
        if attr_name not in valid_attrs:
            # Also check if this is a component param (e.g. label, icon on setting_action_row)
            if self._is_component_param(element.tag, attr_name):
                return diagnostics

            diagnostics.append(
                Diagnostic(
                    file=element.source_file,
                    line=element.line,
                    column=element.column,
                    severity=Severity.ERROR,
                    check=CheckType.UNKNOWN_ATTRIBUTE,
                    message=f"Unknown attribute '{attr_name}' on <{element.tag}>",
                    element=element.tag,
                    attribute=attr_name,
                    value=attr_value,
                )
            )
            return diagnostics

        # Get the attribute schema. A component's <api> prop declaration
        # overrides the base widget's for that name: setting_toggle_row
        # redeclares `disabled` as a string (a subject name), shadowing
        # lv_obj's boolean of the same name, so the bool-value check must
        # not fire on component props the component itself re-typed.
        attr_schema = self._schema.get_attribute_schema(widget_name, attr_name)
        if self._project_registry is not None:
            declared_type = (
                self._project_registry.component_params.get(element.tag, {}).get(attr_name)
            )
            if declared_type is not None:
                attr_schema = AttributeSchema(type=declared_type)

        # Validate value type
        if attr_schema is not None:
            diagnostics.extend(
                self._check_attribute_value(element, attr_name, attr_value, attr_schema)
            )

        return diagnostics

    # Known LVGL style selector parts (LV_PART_*) and states (LV_STATE_*)
    # Used after the first `-` in style attributes like style_bg_opa-knob
    _SELECTOR_PARTS = frozenset(
        {
            "main",
            "scrollbar",
            "indicator",
            "knob",
            "selected",
            "items",
            "cursor",
        }
    )
    _SELECTOR_STATES = frozenset(
        {
            "default",
            "pressed",
            "checked",
            "scrolled",
            "focused",
            "focus_key",
            "edited",
            "hovered",
            "disabled",
            "user_1",
            "user_2",
            "user_3",
            "user_4",
        }
    )
    _VALID_SELECTORS = _SELECTOR_PARTS | _SELECTOR_STATES

    def _check_style_attribute(
        self,
        element: ParsedElement,
        attr_name: str,
        attr_value: str,
    ) -> list[Diagnostic]:
        """Validate a style_* attribute against the style property registry.

        Handles three layers of attribute name decomposition:
        1. State qualifier (``__``): ``style_text_color__checked`` → base ``style_text_color``, state ``checked``
        2. Part selector (``-``): ``style_bg_color-indicator`` → prop ``bg_color``, selector ``indicator``
        3. Combined: ``style_bg_color-indicator__checked`` → prop ``bg_color``, selector ``indicator``, state ``checked``

        The preprocessor converts ``style_text_color:checked`` → ``style_text_color__checked``.
        """
        diagnostics: list[Diagnostic] = []

        # Extract the property name after "style_"
        raw_prop = attr_name[len(_STYLE_PREFIX) :]

        # Step 1: Split on '__' to separate state qualifier from the base attribute.
        # The XML preprocessor converts ':' to '__' in attribute names.
        base_prop, _, state_qualifier = raw_prop.partition("__")

        # Validate state qualifier against known LVGL states
        if state_qualifier and state_qualifier not in self._SELECTOR_STATES:
            # Reconstruct the original colon syntax for display
            display_attr = self._format_state_attr(attr_name)
            diagnostics.append(
                Diagnostic(
                    file=element.source_file,
                    line=element.line,
                    column=element.column,
                    severity=Severity.WARNING,
                    check=CheckType.INVALID_STATE_QUALIFIER,
                    message=(
                        f"Unknown state qualifier ':{state_qualifier}' "
                        f"on attribute '{display_attr}'"
                    ),
                    element=element.tag,
                    attribute=attr_name,
                    value=attr_value,
                )
            )
            return diagnostics

        # Step 2: Handle part selector syntax on the base: style_<prop>-<selector>...
        # The C parser splits on '-' to separate base property from selectors
        # (parts like 'knob', 'indicator' and states like 'disabled', 'checked').
        prop_name, _, selector_suffix = base_prop.partition("-")
        selectors = selector_suffix.split("-") if selector_suffix else []

        if not self._schema.is_valid_style_property(prop_name):
            suggestion = self._suggest_selector_dash_attr(base_prop, state_qualifier)
            message = f"Unknown style property: style_{prop_name}"
            if suggestion:
                message += f". Did you mean '{suggestion}'?"
            diagnostics.append(
                Diagnostic(
                    file=element.source_file,
                    line=element.line,
                    column=element.column,
                    severity=Severity.ERROR,
                    check=CheckType.UNKNOWN_STYLE_PROP,
                    message=message,
                    element=element.tag,
                    attribute=attr_name,
                    value=attr_value,
                )
            )
            return diagnostics

        # Validate selector parts against known LVGL selectors
        for sel in selectors:
            if sel and sel not in self._VALID_SELECTORS:
                diagnostics.append(
                    Diagnostic(
                        file=element.source_file,
                        line=element.line,
                        column=element.column,
                        severity=Severity.WARNING,
                        check=CheckType.UNKNOWN_STYLE_PROP,
                        message=f"Unknown style selector: '{sel}' in style_{prop_name}-{selector_suffix}",
                        element=element.tag,
                        attribute=attr_name,
                        value=attr_value,
                    )
                )

        # Check enum values for style properties
        style_schema = self._schema.get_style_property_schema(prop_name)
        if style_schema and style_schema.enum:
            enum_values = self._schema.get_enum_values(style_schema.enum)
            if (
                enum_values
                and not attr_value.startswith(("#", "$"))
                and attr_value not in enum_values
            ):
                diagnostics.append(
                    Diagnostic(
                        file=element.source_file,
                        line=element.line,
                        column=element.column,
                        severity=Severity.ERROR,
                        check=CheckType.INVALID_ENUM_VALUE,
                        message=(
                            f"Invalid value '{attr_value}' for style property "
                            f"'style_{prop_name}'. Valid values: {', '.join(enum_values)}"
                        ),
                        element=element.tag,
                        attribute=attr_name,
                        value=attr_value,
                    )
                )

        return diagnostics

    def _suggest_selector_dash_attr(self, base_prop: str, state_qualifier: str) -> str:
        """Suggest dash selector syntax for attrs like ``style_arc_width_indicator``."""
        for selector in sorted(self._VALID_SELECTORS, key=len, reverse=True):
            suffix = f"_{selector}"
            if not base_prop.endswith(suffix):
                continue

            prop_name = base_prop[: -len(suffix)]
            if not prop_name or not self._schema.is_valid_style_property(prop_name):
                continue

            suggestion = f"style_{prop_name}-{selector}"
            if state_qualifier:
                suggestion += f":{state_qualifier}"
            return suggestion

        return ""

    @staticmethod
    def _format_state_attr(attr_name: str) -> str:
        """Convert internal ``__`` state qualifier back to display syntax ``:``.

        For diagnostic messages, we show the original LVGL syntax.
        """
        return attr_name.replace("__", ":", 1) if "__" in attr_name else attr_name

    def _check_attribute_value(
        self,
        element: ParsedElement,
        attr_name: str,
        attr_value: str,
        attr_schema: AttributeSchema,
    ) -> list[Diagnostic]:
        """Validate an attribute value against its schema type."""
        diagnostics: list[Diagnostic] = []

        # Skip validation for constant/parameter references
        if attr_value.startswith(("#", "$")):
            return diagnostics

        # Bool validation
        if attr_schema.type == "bool" and attr_value not in ("true", "false"):
            diagnostics.append(
                Diagnostic(
                    file=element.source_file,
                    line=element.line,
                    column=element.column,
                    severity=Severity.ERROR,
                    check=CheckType.INVALID_BOOL_VALUE,
                    message=(
                        f"Invalid boolean value '{attr_value}' for attribute "
                        f"'{attr_name}'. Expected 'true' or 'false'"
                    ),
                    element=element.tag,
                    attribute=attr_name,
                    value=attr_value,
                )
            )

        # Enum validation
        if attr_schema.type == "enum" and attr_schema.enum:
            enum_values = self._schema.get_enum_values(attr_schema.enum)
            if enum_values and attr_value not in enum_values:
                diagnostics.append(
                    Diagnostic(
                        file=element.source_file,
                        line=element.line,
                        column=element.column,
                        severity=Severity.ERROR,
                        check=CheckType.INVALID_ENUM_VALUE,
                        message=(
                            f"Invalid value '{attr_value}' for attribute '{attr_name}'. "
                            f"Valid values: {', '.join(enum_values)}"
                        ),
                        element=element.tag,
                        attribute=attr_name,
                        value=attr_value,
                    )
                )

        return diagnostics

    def _should_skip_element(self, element: ParsedElement) -> bool:
        """Return True if the element should not be validated as a widget."""
        # Skip structural XML tags (component, view, consts, etc.)
        if element.tag in STRUCTURAL_TAGS:
            return True

        # Skip definition elements (constants, styles, subjects)
        if element.tag in DEFINITION_TAGS:
            return True

        # Skip event_cb — it's a special child element, not a widget
        return element.tag == "event_cb"

    def _resolve_widget_name(
        self,
        element: ParsedElement,
        extends_map: dict[str, str] | None,
    ) -> str:
        """Resolve the actual widget name for an element.

        If the tag is a view with extends="...", resolve to the base widget.
        If the tag is already a known widget in the schema (registered via C/C++),
        use it directly — it has its own attribute definitions.
        Otherwise, check extends_map and project registry for component resolution.
        """
        if element.tag == "view" and _EXTENDS_ATTR in element.attributes:
            return element.attributes[_EXTENDS_ATTR]

        # The runtime accepts lv_obj child processors without the "lv_obj-"
        # prefix via lv_xml_widget_get_processor(), e.g. <bind_flag_if_eq>.
        prefixed_obj_tag = f"lv_obj-{element.tag}"
        if prefixed_obj_tag in self._schema.widgets:
            return prefixed_obj_tag

        # If the tag is already a known widget in the schema, use it directly.
        # This handles C++ registered widgets (like "icon") that also have
        # component XML files providing additional structure (api, consts, etc.).
        if element.tag in self._schema.widgets:
            return element.tag

        if extends_map and element.tag in extends_map:
            return extends_map[element.tag]

        # Check project-level component registry for cross-file resolution
        if self._project_registry and element.tag in self._project_registry.component_view_names:
            return self._project_registry.component_view_names[element.tag]

        return element.tag

    def _is_known_custom_tag(
        self,
        element: ParsedElement,
        extends_map: dict[str, str] | None,
    ) -> bool:
        """Check if an unknown tag is a known custom component or widget alias."""
        # Check extends map (custom components)
        if extends_map and element.tag in extends_map:
            return True

        # Check if it's in special elements from schema
        if element.tag in self._schema.special_elements:
            return True

        # Check runtime's unprefixed lv_obj child-processor alias.
        if f"lv_obj-{element.tag}" in self._schema.special_elements:
            return True

        # Check if it's in registered widgets (may be a sub-widget)
        if element.tag in self._schema.registered_widgets:
            return True

        # Structural tags handled by _should_skip_element should pass here too
        if element.tag in STRUCTURAL_TAGS:
            return True

        # Check project-level component registry (cross-file components)
        if self._project_registry and element.tag in self._project_registry.component_view_names:
            return True

        # Check if parent is a known context (consts, styles, subjects)
        return element.parent_tag in ("consts", "styles", "subjects")

    def _is_component_param(self, tag: str, attr_name: str) -> bool:
        """Check if an attribute is a known param/prop for a component tag.

        Component instances accept base-widget attributes through their
        resolved ``extends`` widget. Extra component-specific attributes must
        be declared in the component's <api> section; otherwise a typo in a
        prop name would be silently ignored by the runtime.
        """
        if self._project_registry is None:
            return False

        params = self._project_registry.component_params.get(tag)
        return bool(params is not None and attr_name in params)

    def _build_extends_map(self, elements: list[ParsedElement]) -> dict[str, str]:
        """Build a mapping from custom component names to their base widgets.

        Views with extends="lv_label" allow the view name to be treated
        as an alias for the base widget.
        """
        extends_map: dict[str, str] = {}

        for elem in elements:
            if elem.tag == "view" and _EXTENDS_ATTR in elem.attributes:
                base = elem.attributes[_EXTENDS_ATTR]
                if "name" in elem.attributes:
                    extends_map[elem.attributes["name"]] = base

        return extends_map


def _severity_gte(severity: Severity, minimum: Severity) -> bool:
    """Check if severity is at or above the minimum level."""
    order = {Severity.INFO: 0, Severity.WARNING: 1, Severity.ERROR: 2}
    return order.get(severity, 0) >= order.get(minimum, 0)
