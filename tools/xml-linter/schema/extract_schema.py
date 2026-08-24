#!/usr/bin/env python3
"""Extract LVGL helix-xml schema from C parser source files.

Parses the C source files in the helix-xml library to extract:
- Widget attribute definitions (with type inference)
- Enum value registries
- Style property definitions (with type info)
- Special element types
- Registered widget names

Usage:
    python schema/extract_schema.py /path/to/xml/src/ -o schema/schema.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Type inference helpers
# ---------------------------------------------------------------------------


def infer_type_from_value_parser(func_call: str, widget_context: str = "") -> dict[str, Any]:
    """Infer the XML attribute type from the C function used to parse the value.

    Returns a dict like {"type": "int"}, {"type": "enum", "enum": "align"}, etc.
    widget_context: e.g. "bar", "slider", used for widget-specific enum qualification.
    """
    func_call = func_call.strip()

    # Boolean patterns
    if "lv_xml_to_bool" in func_call:
        return {"type": "bool"}

    # Color patterns
    if "lv_xml_to_color" in func_call:
        return {"type": "color"}

    # Opacity patterns
    if "lv_xml_to_opa" in func_call:
        return {"type": "opa"}

    # Size patterns (int, percentage, or "content")
    if "lv_xml_to_size" in func_call:
        return {"type": "size"}

    # Integer patterns
    if "lv_xml_atoi" in func_call:
        return {"type": "int"}

    # String patterns (direct value assignment)
    if func_call in ("value",) or func_call.startswith("value"):
        return {"type": "string"}

    # Image patterns
    if "lv_xml_get_image" in func_call:
        return {"type": "image"}

    # Font patterns
    if "lv_xml_get_font" in func_call:
        return {"type": "font"}

    # Gradient patterns
    if "lv_xml_component_get_grad" in func_call:
        return {"type": "gradient"}

    # Enum patterns - extract enum name from the converter function name
    enum_map: dict[str, str] = {
        "lv_xml_align_to_enum": "align",
        "lv_xml_dir_to_enum": "dir",
        "lv_xml_flex_flow_to_enum": "flex_flow",
        "lv_xml_flex_align_to_enum": "flex_align",
        "lv_xml_grid_align_to_enum": "grid_align",
        "lv_xml_layout_to_enum": "layout",
        "lv_xml_base_dir_to_enum": "base_dir",
        "lv_xml_text_align_to_enum": "text_align",
        "lv_xml_text_decor_to_enum": "text_decor",
        "lv_xml_scroll_snap_to_enum": "scroll_snap",
        "lv_xml_scrollbar_mode_to_enum": "scrollbar_mode",
        "lv_xml_grad_dir_to_enum": "grad_dir",
        "lv_xml_border_side_to_enum": "border_side",
        "lv_xml_blend_mode_to_enum": "blend_mode",
        "lv_xml_blur_quality_to_enum": "blur_quality",
        "lv_xml_state_to_enum": "state",
        "lv_xml_style_state_to_enum": "state",
        "lv_xml_style_part_to_enum": "part",
        "lv_xml_trigger_text_to_enum_value": "trigger",
        "lv_xml_screen_load_anim_text_to_enum_value": "screen_load_anim",
    }
    for func_name, enum_name in enum_map.items():
        if func_name in func_call:
            return {"type": "enum", "enum": enum_name}

    # Widget-specific enum converters (local static functions)
    # IMPORTANT: Check specific names BEFORE generic substrings
    # (e.g. "long_mode_text_to_enum_value" contains "mode_text_to_enum_value")
    local_enum_map: dict[str, str] = {
        "long_mode_text_to_enum_value": "label_long_mode",
        "chart_type_to_enum": "chart_type",
        "chart_update_mode_to_enum": "chart_update_mode",
        "chart_axis_to_enum": "chart_axis",
        "image_align_to_enum": "image_align",
        "imagebutton_state_to_enum": "imagebutton_state",
        "ctrl_text_to_enum_value": "buttonmatrix_ctrl",
        "table_ctrl_to_enum": "table_cell_ctrl",
        "scale_mode_to_enum": "scale_mode",
        "spangroup_overflow_to_enum": "span_overflow",
        "flag_to_enum": "flag",
    }
    for func_name, enum_name in local_enum_map.items():
        if func_name in func_call:
            return {"type": "enum", "enum": enum_name}

    # Generic patterns (must come after specific checks)
    if "orientation_text_to_enum_value" in func_call:
        if widget_context:
            return {"type": "enum", "enum": f"{widget_context}_orientation"}
        return {"type": "enum", "enum": "orientation"}

    if "mode_text_to_enum_value" in func_call:
        if widget_context:
            return {"type": "enum", "enum": f"{widget_context}_mode"}
        return {"type": "enum", "enum": "mode"}

    # Default to string
    return {"type": "string"}


# ---------------------------------------------------------------------------
# C source file parsers
# ---------------------------------------------------------------------------


def extract_lv_streq_attrs(source: str) -> list[tuple[str, str]]:
    """Extract (attr_name, value_expression) pairs from lv_streq() checks.

    Matches patterns like:
        if(lv_streq("attr_name", name))
        else if(lv_streq("attr_name", name))
    """
    results: list[tuple[str, str]] = []
    # Match: lv_streq("attr_name", name) followed by the value handler
    pattern = re.compile(
        r'lv_streq\(\s*"([^"]+)"\s*,\s*(?:name|op)\s*\)\s*\)'
        r"\s*([^{;]+?)(?:;|$)",
        re.MULTILINE,
    )
    for m in pattern.finditer(source):
        attr_name = m.group(1)
        value_expr = m.group(2).strip()
        # Skip the lv_streq check itself; get the assignment/call
        results.append((attr_name, value_expr))
    return results


def parse_widget_apply_function(
    source: str, func_name: str, widget_context: str = ""
) -> dict[str, dict[str, Any]]:
    """Parse a widget's *_apply() function to extract attributes.

    Returns {attr_name: {"type": "...", ...}}
    """
    attrs: dict[str, dict[str, Any]] = {}

    # Find the function body
    func_pattern = re.compile(
        rf"void\s+{re.escape(func_name)}\s*\([^)]*\)\s*\{{",
        re.DOTALL,
    )
    func_match = func_pattern.search(source)
    if not func_match:
        return attrs

    # Find the balanced braces block
    start = func_match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth > 0:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1

    func_body = source[start:pos]

    # Strategy 1: One-liner patterns
    # Pattern: if(lv_streq("name", name)) handler(value);
    # or: else if(lv_streq("name", name)) handler(value);
    one_liner_pattern = re.compile(
        r'(?:else\s+)?if\s*\(\s*lv_streq\(\s*"([^"]+)"\s*,\s*name\s*\)\s*\)\s*'
        r"([^{;]+?)(?:;|\{)",
        re.MULTILINE,
    )

    for m in one_liner_pattern.finditer(func_body):
        attr_name = m.group(1)
        value_expr = m.group(2).strip()

        if not value_expr:
            continue

        attr_type = infer_type_from_value_parser(value_expr, widget_context)
        attrs[attr_name] = attr_type

    # Strategy 2: Block patterns where handler is inside braces
    # if(lv_streq("name", name)) {
    #     ... handler(value);
    # }
    block_pattern = re.compile(
        r'(?:else\s+)?if\s*\(\s*lv_streq\(\s*"([^"]+)"\s*,\s*name\s*\)\s*\)\s*\{',
        re.MULTILINE,
    )

    for m in block_pattern.finditer(func_body):
        attr_name = m.group(1)
        if attr_name in attrs:
            continue  # Already captured by one-liner pattern

        block_start = m.end()
        # Find the balanced closing brace for this if block
        depth2 = 1
        bpos = block_start
        while bpos < len(func_body) and depth2 > 0:
            if func_body[bpos] == "{":
                depth2 += 1
            elif func_body[bpos] == "}":
                depth2 -= 1
            bpos += 1

        block_text = func_body[block_start:bpos]

        # Strategy 2a: Find the main setter call in the block
        # Pattern: lv_<widget>_set_<attr>(item, <value_parser>(value))
        setter_pattern = re.compile(
            r"lv_\w+\s*\(\s*item\s*,\s*([^)]*value[^)]*)\)",
        )
        setter_match = setter_pattern.search(block_text)
        if setter_match:
            value_expr = setter_match.group(1).strip()
            attr_type = infer_type_from_value_parser(value_expr, widget_context)
            attrs[attr_name] = attr_type
            continue

        # Strategy 2b: Look for value parsing functions directly
        # e.g. int32_t v = lv_xml_atoi(value); ... lv_roller_set_selected(item, v, anim);
        value_parser_pattern = re.compile(
            r"(lv_xml_\w+\([^)]*value[^)]*\))",
        )
        vp_match = value_parser_pattern.search(block_text)
        if vp_match:
            value_expr = vp_match.group(1).strip()
            attr_type = infer_type_from_value_parser(value_expr, widget_context)
            attrs[attr_name] = attr_type
            continue

        # Strategy 2c: Broader setter pattern
        setter_pattern2 = re.compile(
            r"(?:lv_\w+\s*\(\s*item\s*,\s*)([^;]*value[^;]*?)\)",
        )
        setter_match2 = setter_pattern2.search(block_text)
        if setter_match2:
            value_expr = setter_match2.group(1).strip()
            attr_type = infer_type_from_value_parser(value_expr, widget_context)
            attrs[attr_name] = attr_type
            continue

        # Default to string if we found the attr but can't parse the value
        attrs[attr_name] = {"type": "string"}

    return attrs


def parse_apply_styles(source: str) -> dict[str, dict[str, Any]]:
    """Parse apply_styles() function for style properties and their types."""
    styles: dict[str, dict[str, Any]] = {}

    # Find the apply_styles function body first
    func_pattern = re.compile(
        r"static\s+void\s+apply_styles\s*\([^)]*\)\s*\{",
        re.DOTALL,
    )
    func_match = func_pattern.search(source)
    if not func_match:
        return styles

    start = func_match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth > 0:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1

    func_body = source[start:pos]

    # Find SET_STYLE_IF entries within apply_styles only
    pattern = re.compile(
        r"(?:else\s+)?SET_STYLE_IF\(\s*(\w+)\s*,\s*([^)]+)\)",
        re.MULTILINE,
    )

    for m in pattern.finditer(func_body):
        prop_name = m.group(1)
        value_expr = m.group(2).strip()
        style_type = infer_type_from_value_parser(value_expr)
        styles[prop_name] = style_type

    # Special grid array styles
    if "style_grid_column_dsc_array" in func_body:
        styles["grid_column_dsc_array"] = {"type": "string"}
    if "style_grid_row_dsc_array" in func_body:
        styles["grid_row_dsc_array"] = {"type": "string"}

    return styles


def parse_enum_converter(source: str) -> dict[str, list[str]]:
    """Parse enum converter functions to extract enum name -> valid values."""
    enums: dict[str, list[str]] = {}

    # Match functions that convert text to enum
    # Pattern: if(lv_streq("value", txt)) return ENUM_VALUE;
    func_pattern = re.compile(
        r"static\s+\w+\s+(\w+)\s*\([^)]*const\s+char\s*\*\s*txt[^)]*\)\s*\{",
        re.DOTALL,
    )

    for func_match in func_pattern.finditer(source):
        func_name = func_match.group(1)
        func_start = func_match.end()

        # Find the function body
        depth = 1
        pos = func_start
        while pos < len(source) and depth > 0:
            if source[pos] == "{":
                depth += 1
            elif source[pos] == "}":
                depth -= 1
            pos += 1

        func_body = source[func_start:pos]

        # Extract enum string values
        values: list[str] = []
        streq_pattern = re.compile(r'lv_streq\(\s*"([^"]+)"\s*,\s*(?:txt|str)\s*\)')
        for sm in streq_pattern.finditer(func_body):
            val = sm.group(1)
            if val not in values:
                values.append(val)

        if values:
            enums[func_name] = values

    return enums


def parse_global_enum_converters(source: str) -> dict[str, list[str]]:
    """Parse global enum converter functions from lv_xml_base_types.c."""
    enums: dict[str, list[str]] = {}

    # Map function name -> enum name
    func_to_enum: dict[str, str] = {
        "lv_xml_state_to_enum": "state",
        "lv_xml_align_to_enum": "align",
        "lv_xml_dir_to_enum": "dir",
        "lv_xml_border_side_to_enum": "border_side",
        "lv_xml_grad_dir_to_enum": "grad_dir",
        "lv_xml_base_dir_to_enum": "base_dir",
        "lv_xml_text_align_to_enum": "text_align",
        "lv_xml_text_decor_to_enum": "text_decor",
        "lv_xml_scroll_snap_to_enum": "scroll_snap",
        "lv_xml_scrollbar_mode_to_enum": "scrollbar_mode",
        "lv_xml_flex_flow_to_enum": "flex_flow",
        "lv_xml_flex_align_to_enum": "flex_align",
        "lv_xml_grid_align_to_enum": "grid_align",
        "lv_xml_layout_to_enum": "layout",
        "lv_xml_blend_mode_to_enum": "blend_mode",
        "lv_xml_blur_quality_to_enum": "blur_quality",
        "lv_xml_trigger_text_to_enum_value": "trigger",
        "lv_xml_screen_load_anim_text_to_enum_value": "screen_load_anim",
        "lv_xml_style_state_to_enum": "style_state",
        "lv_xml_style_part_to_enum": "style_part",
    }

    for func_name, enum_name in func_to_enum.items():
        # Find function body
        pattern = re.compile(
            rf"\w+\s+{re.escape(func_name)}\s*\([^)]*\)\s*\{{",
            re.DOTALL,
        )
        match = pattern.search(source)
        if not match:
            continue

        start = match.end()
        depth = 1
        pos = start
        while pos < len(source) and depth > 0:
            if source[pos] == "{":
                depth += 1
            elif source[pos] == "}":
                depth -= 1
            pos += 1

        func_body = source[start:pos]

        values: list[str] = []
        streq_pattern = re.compile(r'lv_streq\(\s*"([^"]+)"\s*,\s*(?:txt|str)\s*\)')
        for sm in streq_pattern.finditer(func_body):
            val = sm.group(1)
            if val not in values:
                values.append(val)

        if values:
            enums[enum_name] = values

    return enums


def parse_registered_widgets(source: str) -> tuple[list[str], list[str]]:
    """Parse lv_xml_init() for registered widget names.

    Returns (widget_names, special_elements) where special elements
    are sub-elements like "lv_chart-series", "lv_obj-style", etc.
    """
    widgets: list[str] = []
    special: list[str] = []

    # Find lv_xml_init function
    pattern = re.compile(
        r"void\s+lv_xml_init\s*\([^)]*\)\s*\{",
        re.DOTALL,
    )
    match = pattern.search(source)
    if not match:
        return widgets, special

    start = match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth > 0:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1

    func_body = source[start:pos]

    # Find all lv_xml_register_widget("name", ...) calls
    reg_pattern = re.compile(
        r'lv_xml_register_widget\(\s*"([^"]+)"\s*,',
    )

    for m in reg_pattern.finditer(func_body):
        name = m.group(1)
        # Determine if it's a main widget or special element
        # Main widgets don't contain hyphens (or are well-known base widgets)
        # Special elements contain hyphens and represent sub-elements
        if "-" in name and not name.startswith("lv_obj-"):
            # Sub-widget like lv_chart-series, lv_tabview-tab
            special.append(name)
        elif name.startswith("lv_obj-"):
            # Utility elements like lv_obj-style, lv_obj-event_cb
            special.append(name)
        else:
            widgets.append(name)

    return widgets, special


def parse_style_props_enum(source: str) -> list[str]:
    """Parse lv_xml_style_prop_to_enum() for style property names."""
    props: list[str] = []

    pattern = re.compile(
        r"lv_xml_style_prop_to_enum\s*\([^)]*\)\s*\{",
        re.DOTALL,
    )
    match = pattern.search(source)
    if not match:
        return props

    start = match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth > 0:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1

    func_body = source[start:pos]

    streq_pattern = re.compile(r'lv_streq\(\s*(?:txt|str)\s*,\s*"([^"]+)"\s*\)')
    for m in streq_pattern.finditer(func_body):
        props.append(m.group(1))

    return props


# ---------------------------------------------------------------------------
# Widget-specific enum extraction from parser files
# ---------------------------------------------------------------------------


def extract_local_enums(source: str) -> dict[str, list[str]]:
    """Extract local enum converter functions from a parser .c file."""
    enums: dict[str, list[str]] = {}

    # Match static functions with _text_to_enum_value, _to_enum patterns
    func_pattern = re.compile(
        r"static\s+\w+\s+(\w+)\s*\([^)]*const\s+char\s*\*\s*(?:txt|str)[^)]*\)\s*\{",
        re.DOTALL,
    )

    for func_match in func_pattern.finditer(source):
        func_name = func_match.group(1)
        func_start = func_match.end()

        depth = 1
        pos = func_start
        while pos < len(source) and depth > 0:
            if source[pos] == "{":
                depth += 1
            elif source[pos] == "}":
                depth -= 1
            pos += 1

        func_body = source[func_start:pos]

        values: list[str] = []
        streq_pattern = re.compile(r'lv_streq\(\s*"([^"]+)"\s*,\s*(?:txt|str)\s*\)')
        for sm in streq_pattern.finditer(func_body):
            val = sm.group(1)
            if val not in values:
                values.append(val)

        if values:
            enums[func_name] = values

    return enums


# ---------------------------------------------------------------------------
# Main extraction pipeline
# ---------------------------------------------------------------------------


def extract_schema(xml_src_dir: Path) -> dict[str, Any]:
    """Extract the full schema from C source files."""
    schema: dict[str, Any] = {
        "version": "1.0",
        "widgets": {},
        "enums": {},
        "style_properties": {},
        "special_elements": [],
        "registered_widgets": [],
    }

    parsers_dir = xml_src_dir / "parsers"
    base_types_file = xml_src_dir / "lv_xml_base_types.c"
    main_xml_file = xml_src_dir / "lv_xml.c"
    obj_parser_file = parsers_dir / "lv_xml_obj_parser.c"

    # -----------------------------------------------------------------------
    # 1. Extract registered widgets and special elements from lv_xml_init()
    # -----------------------------------------------------------------------
    if main_xml_file.exists():
        main_source = main_xml_file.read_text(encoding="utf-8", errors="replace")
        widgets, special = parse_registered_widgets(main_source)
        schema["registered_widgets"] = widgets
        schema["special_elements"] = special

    # -----------------------------------------------------------------------
    # 2. Extract global enums from lv_xml_base_types.c
    # -----------------------------------------------------------------------
    if base_types_file.exists():
        base_source = base_types_file.read_text(encoding="utf-8", errors="replace")
        global_enums = parse_global_enum_converters(base_source)
        schema["enums"].update(global_enums)

    # -----------------------------------------------------------------------
    # 3. Extract lv_obj attributes (base widget) from lv_xml_obj_parser.c
    # -----------------------------------------------------------------------
    obj_attrs: dict[str, dict[str, Any]] = {}
    if obj_parser_file.exists():
        obj_source = obj_parser_file.read_text(encoding="utf-8", errors="replace")
        obj_attrs = parse_widget_apply_function(obj_source, "lv_xml_obj_apply")
        schema["widgets"]["lv_obj"] = {
            "attributes": obj_attrs,
        }

        # Extract style properties from apply_styles()
        style_props = parse_apply_styles(obj_source)
        schema["style_properties"] = style_props

        # Extract flag enum
        flag_enum = extract_local_enums(obj_source)
        for func_name, values in flag_enum.items():
            # Map function name to enum name
            if "flag_to_enum" in func_name:
                schema["enums"]["flag"] = values

    # -----------------------------------------------------------------------
    # 4. Extract widget-specific attributes from parser files
    # -----------------------------------------------------------------------
    # Map parser file -> widget name -> apply function name
    widget_parser_map: dict[str, list[tuple[str, str]]] = {
        "lv_xml_label_parser.c": [("lv_label", "lv_xml_label_apply")],
        "lv_xml_button_parser.c": [("lv_button", "lv_xml_button_apply")],
        "lv_xml_image_parser.c": [("lv_image", "lv_xml_image_apply")],
        "lv_xml_bar_parser.c": [("lv_bar", "lv_xml_bar_apply")],
        "lv_xml_slider_parser.c": [("lv_slider", "lv_xml_slider_apply")],
        "lv_xml_arc_parser.c": [("lv_arc", "lv_xml_arc_apply")],
        "lv_xml_chart_parser.c": [
            ("lv_chart", "lv_xml_chart_apply"),
        ],
        "lv_xml_dropdown_parser.c": [("lv_dropdown", "lv_xml_dropdown_apply")],
        "lv_xml_roller_parser.c": [("lv_roller", "lv_xml_roller_apply")],
        "lv_xml_switch_parser.c": [("lv_switch", "lv_xml_switch_apply")],
        "lv_xml_checkbox_parser.c": [("lv_checkbox", "lv_xml_checkbox_apply")],
        "lv_xml_table_parser.c": [("lv_table", "lv_xml_table_apply")],
        "lv_xml_tabview_parser.c": [("lv_tabview", "lv_xml_tabview_apply")],
        "lv_xml_textarea_parser.c": [("lv_textarea", "lv_xml_textarea_apply")],
        "lv_xml_keyboard_parser.c": [("lv_keyboard", "lv_xml_keyboard_apply")],
        "lv_xml_scale_parser.c": [("lv_scale", "lv_xml_scale_apply")],
        "lv_xml_spinbox_parser.c": [("lv_spinbox", "lv_xml_spinbox_apply")],
        "lv_xml_spinner_parser.c": [("lv_spinner", "lv_xml_spinner_apply")],
        "lv_xml_buttonmatrix_parser.c": [("lv_buttonmatrix", "lv_xml_buttonmatrix_apply")],
        "lv_xml_canvas_parser.c": [("lv_canvas", "lv_xml_canvas_apply")],
        "lv_xml_calendar_parser.c": [("lv_calendar", "lv_xml_calendar_apply")],
        "lv_xml_qrcode_parser.c": [("lv_qrcode", "lv_xml_qrcode_apply")],
        "lv_xml_imagebutton_parser.c": [("lv_imagebutton", "lv_xml_imagebutton_apply")],
        "lv_xml_spangroup_parser.c": [("lv_spangroup", "lv_xml_spangroup_apply")],
    }

    for filename, widget_entries in widget_parser_map.items():
        parser_file = parsers_dir / filename
        if not parser_file.exists():
            continue

        source = parser_file.read_text(encoding="utf-8", errors="replace")

        for widget_name, func_name in widget_entries:
            # Extract short widget name for enum qualification (e.g. "bar" from "lv_bar")
            widget_context = (
                widget_name.replace("lv_", "", 1) if widget_name.startswith("lv_") else widget_name
            )
            widget_attrs = parse_widget_apply_function(source, func_name, widget_context)

            # Extract local enum converters
            local_enums = extract_local_enums(source)
            for enum_func, values in local_enums.items():
                # Determine the enum key name
                enum_key = _local_enum_func_to_key(enum_func, widget_name)
                if enum_key and enum_key not in schema["enums"]:
                    schema["enums"][enum_key] = values

            schema["widgets"][widget_name] = {
                "inherits": "lv_obj",
                "attributes": widget_attrs,
            }

    # -----------------------------------------------------------------------
    # 5. Extract special sub-element attributes
    # -----------------------------------------------------------------------
    _extract_special_elements(parsers_dir, schema)

    return schema


def _local_enum_func_to_key(func_name: str, widget_name: str) -> str | None:
    """Map a local enum converter function name to an enum key."""
    # Direct mappings - always use these specific names
    mappings: dict[str, str] = {
        "long_mode_text_to_enum_value": "label_long_mode",
        "chart_type_to_enum": "chart_type",
        "chart_update_mode_to_enum": "chart_update_mode",
        "chart_axis_to_enum": "chart_axis",
        "image_align_to_enum": "image_align",
        "imagebutton_state_to_enum": "imagebutton_state",
        "ctrl_text_to_enum_value": "buttonmatrix_ctrl",
        "table_ctrl_to_enum": "table_cell_ctrl",
        "scale_mode_to_enum": "scale_mode",
        "spangroup_overflow_to_enum": "span_overflow",
    }

    if func_name in mappings:
        return mappings[func_name]

    # Generic pattern: orientation_text_to_enum_value -> <widget>_orientation
    # Strip the "lv_" prefix from widget name for cleaner enum keys
    short_name = widget_name.replace("lv_", "", 1)

    if "orientation_text_to_enum_value" in func_name:
        return f"{short_name}_orientation"

    if "mode_text_to_enum_value" in func_name:
        return f"{short_name}_mode"

    return None


def _extract_special_elements(parsers_dir: Path, schema: dict[str, Any]) -> None:
    """Extract attributes for special sub-elements."""
    # Chart sub-elements
    chart_file = parsers_dir / "lv_xml_chart_parser.c"
    if chart_file.exists():
        source = chart_file.read_text(encoding="utf-8", errors="replace")

        # lv_chart-series
        series_attrs = parse_widget_apply_function(source, "lv_xml_chart_series_apply")
        schema["widgets"]["lv_chart-series"] = {
            "attributes": {
                "color": {"type": "color"},
                "axis": {"type": "enum", "enum": "chart_axis"},
                **series_attrs,
            },
        }

        # lv_chart-cursor
        cursor_attrs = parse_widget_apply_function(source, "lv_xml_chart_cursor_apply")
        schema["widgets"]["lv_chart-cursor"] = {
            "attributes": {
                "color": {"type": "color"},
                "dir": {"type": "enum", "enum": "dir"},
                **cursor_attrs,
            },
        }

        # lv_chart-axis
        axis_attrs = parse_widget_apply_function(source, "lv_xml_chart_axis_apply")
        schema["widgets"]["lv_chart-axis"] = {
            "attributes": {
                "axis": {"type": "enum", "enum": "chart_axis"},
                **axis_attrs,
            },
        }

    # Table sub-elements
    table_file = parsers_dir / "lv_xml_table_parser.c"
    if table_file.exists():
        source = table_file.read_text(encoding="utf-8", errors="replace")

        # lv_table-column
        col_attrs = parse_widget_apply_function(source, "lv_xml_table_column_apply")
        schema["widgets"]["lv_table-column"] = {
            "attributes": {
                "column": {"type": "int"},
                **col_attrs,
            },
        }

        # lv_table-cell
        cell_attrs = parse_widget_apply_function(source, "lv_xml_table_cell_apply")
        schema["widgets"]["lv_table-cell"] = {
            "attributes": {
                "row": {"type": "int"},
                "column": {"type": "int"},
                **cell_attrs,
            },
        }

    # Tabview sub-elements
    tabview_file = parsers_dir / "lv_xml_tabview_parser.c"
    if tabview_file.exists():
        source = tabview_file.read_text(encoding="utf-8", errors="replace")

        # lv_tabview-tab_bar
        schema["widgets"]["lv_tabview-tab_bar"] = {
            "inherits": "lv_obj",
            "attributes": {},
        }

        # lv_tabview-tab
        schema["widgets"]["lv_tabview-tab"] = {
            "inherits": "lv_obj",
            "attributes": {
                "text": {"type": "string"},
            },
        }

        # lv_tabview-tab_button
        schema["widgets"]["lv_tabview-tab_button"] = {
            "inherits": "lv_obj",
            "attributes": {
                "index": {"type": "int"},
            },
        }

    # Dropdown sub-elements
    dropdown_file = parsers_dir / "lv_xml_dropdown_parser.c"
    if dropdown_file.exists():
        schema["widgets"]["lv_dropdown-list"] = {
            "inherits": "lv_obj",
            "attributes": {},
        }

    # Scale sub-elements
    scale_file = parsers_dir / "lv_xml_scale_parser.c"
    if scale_file.exists():
        source = scale_file.read_text(encoding="utf-8", errors="replace")
        section_attrs = parse_widget_apply_function(source, "lv_xml_scale_section_apply")
        schema["widgets"]["lv_scale-section"] = {
            "attributes": {
                **section_attrs,
            },
        }

    # Spangroup sub-elements
    spangroup_file = parsers_dir / "lv_xml_spangroup_parser.c"
    if spangroup_file.exists():
        source = spangroup_file.read_text(encoding="utf-8", errors="replace")
        span_attrs = parse_widget_apply_function(source, "lv_xml_spangroup_span_apply")
        schema["widgets"]["lv_spangroup-span"] = {
            "attributes": {
                **span_attrs,
            },
        }

    # Imagebutton sub-elements
    imagebutton_file = parsers_dir / "lv_xml_imagebutton_parser.c"
    if imagebutton_file.exists():
        for sub in ["src_left", "src_right", "src_mid"]:
            schema["widgets"][f"lv_imagebutton-{sub}"] = {
                "attributes": {
                    "state": {"type": "enum", "enum": "imagebutton_state"},
                    "src": {"type": "image"},
                },
            }

    # Calendar sub-elements
    calendar_file = parsers_dir / "lv_xml_calendar_parser.c"
    if calendar_file.exists():
        for sub in ["header_arrow", "header_dropdown"]:
            schema["widgets"][f"lv_calendar-{sub}"] = {
                "inherits": "lv_obj",
                "attributes": {},
            }

    # <subject_expr name="X" expr="..."> -- derived-subject sibling of <subject>
    # inside a component's <subjects> block (lv_xml_component.c
    # process_subject_expr_element). It's parsed directly by the <subjects>
    # element handler, not registered via lv_xml_register_widget(), so it
    # can't be auto-discovered the way lv_obj-* special elements are.
    schema["widgets"]["subject_expr"] = {
        "attributes": {
            "name": {"type": "string"},
            "expr": {"type": "string"},
        },
    }

    # <repeat count="N">...body...</repeat> -- load-time looping construct handled
    # directly in lv_xml.c's view element handlers (capture/replay), not registered
    # via lv_xml_register_widget(). `count` is a literal, a #const, or a subject name.
    schema["widgets"]["repeat"] = {
        "attributes": {
            "count": {"type": "string"},
        },
    }

    # <if cond="expr">...<else/>...</if> -- load-time conditional handled in
    # lv_xml.c view handlers (capture/replay), not via lv_xml_register_widget().
    schema["widgets"]["if"] = {
        "attributes": {
            "cond": {"type": "string"},
        },
    }
    schema["widgets"]["else"] = {
        "attributes": {},
    }

    # Both of the above are validated by parse_registered_widgets() finding a
    # lv_xml_register_widget("name", ...) call in lv_xml_init() -- neither
    # goes through that call, so is_valid_widget() (registered_widgets or
    # special_elements) would reject them as "Unknown widget type" wherever
    # they appear outside a <subjects> block (subject_expr is skipped there
    # as a definition tag, but <repeat> appears directly in view markup).
    # Register both as special_elements explicitly.
    for tag in ("subject_expr", "repeat", "if", "else"):
        if tag not in schema["special_elements"]:
            schema["special_elements"].append(tag)

    # Add special obj elements with their attributes
    _extract_obj_special_elements(schema)


def _extract_obj_special_elements(schema: dict[str, Any]) -> None:
    """Add attribute definitions for lv_obj-* special elements."""
    schema["widgets"]["lv_obj-style"] = {
        "attributes": {
            "name": {"type": "string"},
            "selector": {"type": "string"},
        },
    }

    schema["widgets"]["lv_obj-remove_style"] = {
        "attributes": {
            "name": {"type": "string"},
            "selector": {"type": "string"},
        },
    }

    schema["widgets"]["lv_obj-remove_style_all"] = {
        "attributes": {},
    }

    schema["widgets"]["lv_obj-event_cb"] = {
        "attributes": {
            "trigger": {"type": "enum", "enum": "trigger"},
            "callback": {"type": "string"},
            "user_data": {"type": "string"},
        },
    }

    schema["widgets"]["lv_obj-subject_toggle_event"] = {
        "attributes": {
            "subject": {"type": "string"},
            "trigger": {"type": "enum", "enum": "trigger"},
        },
    }

    for typ in ["int", "float", "string"]:
        schema["widgets"][f"lv_obj-subject_set_{typ}_event"] = {
            "attributes": {
                "subject": {"type": "string"},
                "trigger": {"type": "enum", "enum": "trigger"},
                "value": {"type": "string"},
            },
        }

    schema["widgets"]["lv_obj-subject_increment_event"] = {
        "attributes": {
            "subject": {"type": "string"},
            "trigger": {"type": "enum", "enum": "trigger"},
            "step": {"type": "int"},
            "min_value": {"type": "int"},
            "max_value": {"type": "int"},
            "rollover": {"type": "bool"},
        },
    }

    schema["widgets"]["lv_obj-screen_load_event"] = {
        "attributes": {
            "screen": {"type": "string"},
            "duration": {"type": "int"},
            "delay": {"type": "int"},
            "anim_type": {"type": "enum", "enum": "screen_load_anim"},
            "trigger": {"type": "enum", "enum": "trigger"},
        },
    }

    schema["widgets"]["lv_obj-screen_create_event"] = {
        "attributes": {
            "screen": {"type": "string"},
            "duration": {"type": "int"},
            "delay": {"type": "int"},
            "anim_type": {"type": "enum", "enum": "screen_load_anim"},
            "trigger": {"type": "enum", "enum": "trigger"},
        },
    }

    schema["widgets"]["lv_obj-play_timeline_event"] = {
        "attributes": {
            "target": {"type": "string"},
            "timeline": {"type": "string"},
            "delay": {"type": "int"},
            "trigger": {"type": "enum", "enum": "trigger"},
            "reverse": {"type": "bool"},
        },
    }

    # bind_style variants
    schema["widgets"]["lv_obj-bind_style"] = {
        "attributes": {
            "name": {"type": "string"},
            "subject": {"type": "string"},
            "ref_value": {"type": "int"},
            "selector": {"type": "string"},
        },
    }

    for cmp in ["eq", "not_eq", "gt", "ge", "lt", "le"]:
        schema["widgets"][f"lv_obj-bind_style_if_{cmp}"] = {
            "attributes": {
                "name": {"type": "string"},
                "subject": {"type": "string"},
                "ref_value": {"type": "int"},
                "selector": {"type": "string"},
            },
        }

    schema["widgets"]["lv_obj-bind_style_prop"] = {
        "attributes": {
            "prop": {"type": "string"},
            "subject": {"type": "string"},
            "selector": {"type": "string"},
        },
    }

    for cmp in ["eq", "not_eq", "gt", "ge", "lt", "le"]:
        schema["widgets"][f"lv_obj-bind_flag_if_{cmp}"] = {
            "attributes": {
                "subject": {"type": "string"},
                "flag": {"type": "enum", "enum": "flag"},
                "ref_value": {"type": "int"},
            },
        }

    for cmp in ["eq", "not_eq", "gt", "ge", "lt", "le"]:
        schema["widgets"][f"lv_obj-bind_state_if_{cmp}"] = {
            "attributes": {
                "subject": {"type": "string"},
                "state": {"type": "enum", "enum": "state"},
                "ref_value": {"type": "int"},
            },
        }

    # cond= expression-driven variants (lv_xml_obj_parser.c
    # lv_obj_xml_bind_flag_if_apply / bind_state_if_apply / bind_style_if_apply)
    # -- written in XML unprefixed as <bind_flag_if>/<bind_state_if>/
    # <bind_style_if>; `cond` is a free-form expression string, not enum-validated.
    schema["widgets"]["lv_obj-bind_flag_if"] = {
        "attributes": {
            "cond": {"type": "string"},
            "flag": {"type": "enum", "enum": "flag"},
            "invert": {"type": "bool"},
        },
    }

    schema["widgets"]["lv_obj-bind_state_if"] = {
        "attributes": {
            "cond": {"type": "string"},
            "state": {"type": "enum", "enum": "state"},
            "invert": {"type": "bool"},
        },
    }

    schema["widgets"]["lv_obj-bind_style_if"] = {
        "attributes": {
            "cond": {"type": "string"},
            "name": {"type": "string"},
            "selector": {"type": "string"},
            "parts": {"type": "string"},
            "invert": {"type": "bool"},
        },
    }


# ---------------------------------------------------------------------------
# C++ widget extraction
# ---------------------------------------------------------------------------


# Map from C++ filename to widget extraction info.
# Each entry: (filename, [widget_name, ...], apply_delegation)
# apply_delegation: what _apply function calls to determine inheritance
#   None -> inherits lv_obj (via lv_xml_obj_apply)
#   "lv_label" -> inherits lv_label (via lv_xml_label_apply)
#   "lv_textarea" -> inherits lv_textarea (via lv_xml_textarea_apply)

CPP_WIDGET_FILES: dict[str, dict[str, Any]] = {
    "ui_icon.cpp": {
        "widgets": ["icon"],
        "inherits": "lv_obj",
    },
    "ui_text.cpp": {
        "widgets": [
            "text_heading",
            "text_body",
            "text_muted",
            "text_small",
            "text_xs",
            "text_tiny",
        ],
        "inherits": "lv_label",
        "extra_attrs": {
            "stroke_width": {"type": "int"},
            "stroke_color": {"type": "color"},
            "stroke_opa": {"type": "opa"},
            "text_transform": {"type": "enum", "enum": "text_transform"},
        },
    },
    "ui_text.cpp:text_button": {
        "widgets": ["text_button"],
        "inherits": "lv_label",
        "extra_attrs": {},
    },
    "ui_button.cpp": {
        "widgets": ["ui_button"],
        "inherits": "lv_obj",
        "extra_attrs": {
            "focusable": {"type": "bool"},
            "variant": {"type": "enum", "enum": "button_variant"},
            "text": {"type": "string"},
            "translation_tag": {"type": "string"},
            "icon": {"type": "string"},
            "icon_size": {"type": "enum", "enum": "icon_size"},
            "icon_position": {"type": "enum", "enum": "icon_position"},
            "layout": {"type": "enum", "enum": "button_layout"},
            "bind_text": {"type": "string"},
            "bind_icon": {"type": "string"},
            "bind_op_state": {"type": "string"},
            "long_mode": {"type": "enum", "enum": "label_long_mode"},
            "label_hidden_if_bp_eq": {"type": "int"},
            "bind_text-fmt": {"type": "string"},
            "text-fmt": {"type": "string"},
        },
    },
    "ui_card.cpp": {
        "widgets": ["ui_card"],
        "inherits": "lv_obj",
    },
    "ui_spinner.cpp": {
        "widgets": ["spinner"],
        "inherits": "lv_obj",
        "extra_attrs": {
            "size": {"type": "enum", "enum": "spinner_size"},
        },
    },
    "ui_text_input.cpp": {
        "widgets": ["text_input"],
        "inherits": "lv_textarea",
        "extra_attrs": {
            "placeholder": {"type": "string"},
            "placeholder_tag": {"type": "string"},
            "max_length": {"type": "int"},
            "multiline": {"type": "bool"},
            "bind_text": {"type": "string"},
            "keyboard_hint": {"type": "enum", "enum": "keyboard_hint"},
            "show_clear_button": {"type": "bool"},
            "clear_callback": {"type": "string"},
        },
    },
    "ui_switch.cpp": {
        "widgets": ["ui_switch"],
        "inherits": "lv_obj",
        "extra_attrs": {
            "size": {"type": "enum", "enum": "switch_size"},
            "width": {"type": "int"},
            "height": {"type": "int"},
            "knob_pad": {"type": "int"},
            "checked": {"type": "bool"},
            "orientation": {"type": "enum", "enum": "switch_orientation"},
        },
    },
}

# C++ files that are simpler — they only call lv_xml_obj_apply and have
# no custom attributes beyond what we parse from their source.
CPP_SIMPLE_WIDGETS: dict[str, dict[str, Any]] = {
    "ui_carousel.cpp": {
        "widgets": ["ui_carousel"],
        "inherits": "lv_obj",
    },
    "ui_dialog.cpp": {
        "widgets": ["ui_dialog"],
        "inherits": "lv_obj",
    },
    "ui_gradient_canvas.cpp": {
        "widgets": ["ui_gradient_canvas"],
        "inherits": "lv_obj",
    },
    "ui_hsv_picker.cpp": {
        "widgets": ["ui_hsv_picker"],
        "inherits": "lv_obj",
    },
    "ui_markdown.cpp": {
        "widgets": ["ui_markdown"],
        "inherits": "lv_obj",
    },
    "ui_confetti.cpp": {
        "widgets": ["ui_confetti"],
        "inherits": "lv_obj",
    },
    "ui_notification_badge.cpp": {
        "widgets": ["notification_badge"],
        "inherits": "lv_obj",
    },
    "ui_severity_card.cpp": {
        "widgets": ["severity_card"],
        "inherits": "lv_obj",
    },
    "ui_status_pill.cpp": {
        "widgets": ["status_pill"],
        "inherits": "lv_obj",
    },
    "ui_temp_display.cpp": {
        "widgets": ["temp_display"],
        "inherits": "lv_obj",
    },
    "ui_gcode_viewer.cpp": {
        "widgets": ["gcode_viewer"],
        "inherits": "lv_obj",
    },
    "ui_bed_mesh.cpp": {
        "widgets": ["bed_mesh"],
        "inherits": "lv_obj",
    },
    "ui_ams_slot.cpp": {
        "widgets": ["ams_slot"],
        "inherits": "lv_obj",
    },
    "ui_ams_mini_status.cpp": {
        "widgets": ["ams_mini_status"],
        "inherits": "lv_obj",
    },
    "ui_spool_canvas.cpp": {
        "widgets": ["spool_canvas"],
        "inherits": "lv_obj",
        "extra_attrs": {
            "size": {"type": "int"},
        },
    },
    "ui_filament_path_canvas.cpp": {
        "widgets": ["filament_path_canvas"],
        "inherits": "lv_obj",
    },
    "ui_system_path_canvas.cpp": {
        "widgets": ["system_path_canvas"],
        "inherits": "lv_obj",
    },
    "ui_endless_spool_arrows.cpp": {
        "widgets": ["endless_spool_arrows"],
        "inherits": "lv_obj",
    },
    "ui_split_button.cpp": {
        "widgets": ["ui_split_button"],
        "inherits": "lv_obj",
        "extra_attrs": {
            "variant": {"type": "enum", "enum": "button_variant"},
            "text": {"type": "string"},
            "icon": {"type": "string"},
            "options": {"type": "string"},
            "text_format": {"type": "string"},
            "show_selection": {"type": "bool"},
            "selected": {"type": "int"},
        },
    },
    "ui_z_offset_indicator.cpp": {
        "widgets": ["z_offset_indicator"],
        "inherits": "lv_obj",
    },
}


def extract_cpp_widget_attrs(source: str) -> dict[str, dict[str, Any]]:
    """Extract attributes from a C++ widget source file.

    Scans for two patterns:
    1. lv_xml_get_value_of(attrs, "attr_name") calls
    2. strcmp(name, "attr_name") / strcmp(attrs[i], "attr_name") comparisons

    Returns {attr_name: type_info_dict}
    """
    attrs: dict[str, dict[str, Any]] = {}

    # Pattern 1: lv_xml_get_value_of(attrs, "name")
    get_value_pattern = re.compile(
        r'lv_xml_get_value_of\s*\(\s*attrs\s*,\s*"([^"]+)"\s*\)',
    )
    for m in get_value_pattern.finditer(source):
        attr_name = m.group(1)
        if attr_name not in attrs:
            attrs[attr_name] = _infer_cpp_attr_type(source, attr_name)

    # Pattern 2: strcmp(name, "attr_name") == 0 or strcmp(attrs[i], "attr_name")
    strcmp_pattern = re.compile(
        r'strcmp\s*\(\s*(?:name|attrs\[i\])\s*,\s*"([^"]+)"\s*\)'
    )
    for m in strcmp_pattern.finditer(source):
        attr_name = m.group(1)
        if attr_name not in attrs:
            attrs[attr_name] = _infer_cpp_attr_type(source, attr_name)

    return attrs


def _infer_cpp_attr_type(source: str, attr_name: str) -> dict[str, Any]:
    """Infer the type of a C++ widget attribute from context."""
    # Boolean attrs
    bool_attrs = {
        "focusable", "show_clear_button", "multiline", "dither",
        "show_selection", "show_target", "show_indicators", "wrap",
        "bypass_active", "show_bypass", "hub_only", "hide_target_when_off",
    }
    if attr_name in bool_attrs:
        return {"type": "bool"}

    # Integer attrs
    int_attrs = {
        "max_length", "sv_size", "hue_height", "gap",
        "slot_index", "fill_level", "slot_count", "active_slot",
        "unit_count", "active_unit", "slot_width", "knob_pad",
        "label_hidden_if_bp_eq", "anim_progress",
    }
    if attr_name in int_attrs or attr_name.endswith("_length") or attr_name.endswith("_count"):
        return {"type": "int"}

    # Color attrs
    if "color" in attr_name.lower():
        return {"type": "color"}

    # Known enum attrs
    enum_map: dict[str, str] = {
        "size": "_widget_size",  # Context-dependent
        "variant": "ui_variant",
        "orientation": "switch_orientation",
        "severity": "severity",
        "keyboard_hint": "keyboard_hint",
        "text_transform": "text_transform",
    }
    if attr_name in enum_map:
        return {"type": "enum", "enum": enum_map[attr_name]}

    # String attrs (default)
    return {"type": "string"}


def extract_cpp_widgets(cpp_src_dir: Path, schema: dict[str, Any]) -> None:
    """Extract C++ widget definitions from src/ui/ directory.

    Two passes:
    1. Hand-curated entries in CPP_WIDGET_FILES / CPP_SIMPLE_WIDGETS — preserves
       custom enums and extra_attrs that can't be inferred from .cpp alone.
    2. Auto-discovery for any widget registered via lv_xml_register_widget() in
       cpp_src_dir but not covered by pass 1. Inheritance is inferred from the
       apply function name in the registration call. This means adding a new
       widget no longer requires editing CPP_WIDGET_FILES to avoid
       unknown-widget false positives.
    """
    # Pass 1: hand-curated configs
    all_configs: dict[str, dict[str, Any]] = {**CPP_WIDGET_FILES, **CPP_SIMPLE_WIDGETS}

    for config_key, config in all_configs.items():
        filename = config_key.split(":")[0]
        cpp_file = cpp_src_dir / filename

        if not cpp_file.exists():
            continue

        source = cpp_file.read_text(encoding="utf-8", errors="replace")
        widget_names: list[str] = config["widgets"]
        inherits: str = config.get("inherits", "lv_obj")
        extra_attrs: dict[str, dict[str, Any]] = config.get("extra_attrs", {})

        # Parse attributes from source
        parsed_attrs = extract_cpp_widget_attrs(source)

        # Merge: extra_attrs take priority over parsed
        merged_attrs = {**parsed_attrs, **extra_attrs}

        for widget_name in widget_names:
            # Add to registered widgets
            if widget_name not in schema["registered_widgets"]:
                schema["registered_widgets"].append(widget_name)

            # Add widget schema
            widget_entry: dict[str, Any] = {
                "attributes": merged_attrs,
                "inherits": inherits,
            }

            schema["widgets"][widget_name] = widget_entry

    # Pass 2: auto-discover anything not yet covered
    _auto_discover_cpp_widgets(cpp_src_dir, schema)

    # Add C++ specific enums
    _add_cpp_enums(schema)


# Apply-function names passed to lv_xml_register_widget(), mapped to the base
# widget whose attributes the registered widget inherits. Unknown apply fns
# default to lv_obj.
_APPLY_FN_TO_INHERITS: dict[str, str] = {
    "lv_xml_obj_apply": "lv_obj",
    "lv_xml_label_apply": "lv_label",
    "lv_xml_button_apply": "lv_button",
    "lv_xml_image_apply": "lv_image",
    "lv_xml_textarea_apply": "lv_textarea",
    "lv_xml_bar_apply": "lv_bar",
    "lv_xml_slider_apply": "lv_slider",
    "lv_xml_arc_apply": "lv_arc",
}


_REGISTER_WIDGET_RE = re.compile(
    r'lv_xml_register_widget\(\s*"([^"]+)"\s*,\s*\w+\s*,\s*(\w+)\s*\)'
)


def _auto_discover_cpp_widgets(cpp_src_dir: Path, schema: dict[str, Any]) -> None:
    """Scan cpp_src_dir recursively for lv_xml_register_widget() calls.

    Any widget not already present in schema['widgets'] gets added with
    attributes parsed from its .cpp source and inheritance inferred from the
    apply function name passed to register_widget().
    """
    for cpp_file in sorted(cpp_src_dir.rglob("*.cpp")):
        try:
            source = cpp_file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        matches = list(_REGISTER_WIDGET_RE.finditer(source))
        if not matches:
            continue

        # Parse attrs once per file — multiple widgets in one file share them
        parsed_attrs = extract_cpp_widget_attrs(source)

        for m in matches:
            widget_name = m.group(1)
            apply_fn = m.group(2)

            if widget_name in schema["widgets"]:
                continue  # Already curated or already discovered

            inherits = _APPLY_FN_TO_INHERITS.get(apply_fn, "lv_obj")

            schema["widgets"][widget_name] = {
                "attributes": parsed_attrs,
                "inherits": inherits,
            }
            if widget_name not in schema["registered_widgets"]:
                schema["registered_widgets"].append(widget_name)


def _add_cpp_enums(schema: dict[str, Any]) -> None:
    """Add enum types discovered from C++ widgets."""
    cpp_enums: dict[str, list[str]] = {
        "ui_variant": [
            "none", "text", "muted", "primary", "accent", "secondary",
            "tertiary", "disabled", "success", "warning", "danger", "info",
        ],
        "button_variant": [
            "primary", "secondary", "danger", "success", "tertiary",
            "warning", "ghost", "transparent", "outline",
        ],
        "icon_size": ["xs", "sm", "md", "lg", "xl"],
        "icon_position": ["left", "right", "top", "bottom"],
        "button_layout": ["row", "column"],
        "spinner_size": ["xs", "sm", "md", "lg"],
        "switch_size": ["tiny", "small", "medium", "large"],
        "switch_orientation": ["horizontal", "vertical", "auto"],
        "keyboard_hint": ["text", "numeric"],
        "text_transform": ["uppercase"],
        "severity": ["info", "success", "warning", "error"],
        "_widget_size": ["xs", "sm", "md", "lg", "xl"],
    }
    for enum_name, values in cpp_enums.items():
        if enum_name not in schema["enums"]:
            schema["enums"][enum_name] = values


# ---------------------------------------------------------------------------
# Runtime constant extraction from XML files
# ---------------------------------------------------------------------------

# Responsive suffixes used by the theme system
# Must mirror skip_suffixes in src/ui/theme_manager.cpp:793
_RESPONSIVE_SUFFIXES = [
    "_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge",
]


# Tags that define a named constant when they have a `name` attribute. Per the
# helix-xml engine, these register a constant when they appear inside a
# <consts> block — but helixscreen's theme_manager also walks every XML file at
# startup and registers any <px>/<color>/<string> regardless of nesting, so we
# treat all occurrences as constants.
_CONST_TAGS = frozenset(
    {"px", "color", "string", "int", "percentage", "font", "tiny_ttf", "bin", "const"}
)


def extract_runtime_constants(xml_roots: list[Path]) -> set[str]:
    """Scan XML files for every constant name the runtime can resolve.

    Collects:
    - Any <px>/<color>/<string>/<int>/<percentage>/<font>/<tiny_ttf>/<bin>
      element with a `name` attribute, regardless of whether it sits inside
      <consts> (helixscreen's theme_manager harvests these globally at startup).
    - Base names with responsive suffixes stripped (e.g. `chip_height_small`
      → also adds `chip_height`).
    """
    import re
    import xml.etree.ElementTree as ET

    # Mirrors helix_xml_linter.xml_parser._preprocess_state_qualifiers:
    # rewrites attribute names like 'style_text_color:checked="..."' so
    # ElementTree doesn't reject them as unbound XML namespace prefixes.
    state_qualifier_re = re.compile(r"(\b\w+):(\w+\s*=\s*[\"'])")

    constants: set[str] = set()

    for xml_root in xml_roots:
        if not xml_root.is_dir():
            continue
        for xml_file in xml_root.rglob("*.xml"):
            try:
                raw = xml_file.read_text(encoding="utf-8")
            except OSError:
                continue
            preprocessed = state_qualifier_re.sub(r"\1__\2", raw)
            try:
                root = ET.fromstring(preprocessed)
            except ET.ParseError:
                continue
            for elem in root.iter():
                if elem.tag not in _CONST_TAGS:
                    continue
                name = elem.get("name")
                if not name:
                    continue
                constants.add(name)
                base = _strip_responsive_suffix(name)
                if base != name:
                    constants.add(base)

    return constants


def _strip_responsive_suffix(name: str) -> str:
    """Strip responsive size suffix from a constant name."""
    for suffix in _RESPONSIVE_SUFFIXES:
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return name


_CPP_REGISTER_CONST_RE = re.compile(
    r'lv_xml_register_const\s*\(\s*[^,]+,\s*"([a-zA-Z_][a-zA-Z0-9_]*)"'
)


def extract_cpp_registered_constants(cpp_src_dirs: list[Path]) -> set[str]:
    """Scan C++ source for every literal lv_xml_register_const() call site.

    Catches constants the runtime registers via C++ (responsive spacing tokens
    like nav_width / overlay_panel_width / hue_height) which would otherwise
    be flagged as unknown-const-ref.
    """
    constants: set[str] = set()
    for src_dir in cpp_src_dirs:
        if not src_dir.is_dir():
            continue
        for path in src_dir.rglob("*.cpp"):
            try:
                source = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for m in _CPP_REGISTER_CONST_RE.finditer(source):
                constants.add(m.group(1))
    return constants


def extract_theme_constants(theme_dirs: list[Path]) -> set[str]:
    """Scan theme JSON files for color and style tokens.

    Helixscreen theme JSONs have shape:
        {"dark": {"primary": "#…", "card_bg": "#…", …},
         "light": {…},
         "border_width": 1, "shadow_opa": 0, …}

    Both per-mode color keys and top-level scalar keys become runtime constants
    via theme_manager_register_static_constants().
    """
    constants: set[str] = set()
    for theme_dir in theme_dirs:
        if not theme_dir.is_dir():
            continue
        for json_path in theme_dir.rglob("*.json"):
            try:
                data = json.loads(json_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            if not isinstance(data, dict):
                continue
            for key, value in data.items():
                if isinstance(value, dict):
                    # "dark" / "light" — keys inside are color tokens
                    for color_name in value:
                        if isinstance(color_name, str):
                            constants.add(color_name)
                elif isinstance(value, (int, float, str)):
                    # Top-level scalar — itself a token (e.g. border_width)
                    if isinstance(key, str) and key != "name":
                        constants.add(key)
    return constants


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------


def main() -> None:
    """CLI entry point for schema extraction."""
    parser = argparse.ArgumentParser(
        description="Extract LVGL helix-xml schema from C parser source files.",
    )
    parser.add_argument(
        "src_dir",
        type=Path,
        help="Path to the helix-xml/src/xml/ directory containing C sources.",
    )
    parser.add_argument(
        "--cpp-src",
        type=Path,
        default=None,
        help="Path to the C++ src/ui/ directory containing custom widget files.",
    )
    parser.add_argument(
        "--xml-roots",
        type=Path,
        nargs="*",
        default=None,
        help="XML directories to scan for runtime constant names.",
    )
    parser.add_argument(
        "--theme-dirs",
        type=Path,
        nargs="*",
        default=None,
        help="Theme JSON directories to scan for color/style tokens.",
    )
    parser.add_argument(
        "--cpp-const-dirs",
        type=Path,
        nargs="*",
        default=None,
        help="C++ directories to scan recursively for lv_xml_register_const() calls.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output path for schema.json (default: stdout).",
    )

    args = parser.parse_args()

    src_dir: Path = args.src_dir
    if not src_dir.is_dir():
        print(f"Error: {src_dir} is not a directory", file=sys.stderr)
        sys.exit(2)

    schema = extract_schema(src_dir)

    # Extract C++ widgets if specified
    if args.cpp_src and args.cpp_src.is_dir():
        extract_cpp_widgets(args.cpp_src, schema)

    # Aggregate runtime constants from every source the runtime might register from
    all_runtime_consts: set[str] = set()
    if args.xml_roots:
        all_runtime_consts |= extract_runtime_constants(args.xml_roots)
    # Default: scan the cpp-src dir for register_const() if --cpp-const-dirs not given
    const_dirs = args.cpp_const_dirs or ([args.cpp_src] if args.cpp_src else [])
    if const_dirs:
        all_runtime_consts |= extract_cpp_registered_constants(const_dirs)
    if args.theme_dirs:
        all_runtime_consts |= extract_theme_constants(args.theme_dirs)
    if all_runtime_consts:
        schema["runtime_constants"] = sorted(all_runtime_consts)

    # Summary
    widget_count = len(schema["widgets"])
    enum_count = len(schema["enums"])
    style_count = len(schema["style_properties"])
    special_count = len(schema["special_elements"])
    registered_count = len(schema["registered_widgets"])
    runtime_const_count = len(schema.get("runtime_constants", []))

    total_attrs = sum(len(w.get("attributes", {})) for w in schema["widgets"].values())

    print(f"Extracted schema from {src_dir}:")
    print(f"  Widgets:         {widget_count} ({registered_count} registered)")
    print(f"  Special elements: {special_count}")
    print(f"  Total attributes: {total_attrs}")
    print(f"  Enum types:       {enum_count}")
    print(f"  Style properties: {style_count}")
    print(f"  Runtime constants: {runtime_const_count}")

    # Output
    json_str = json.dumps(schema, indent=2, ensure_ascii=False)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json_str, encoding="utf-8")
        print(f"\nSchema written to: {args.output}")
    else:
        print()
        print(json_str)


if __name__ == "__main__":
    main()
