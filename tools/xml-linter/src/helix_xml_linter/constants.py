"""Shared constants for helix-xml-linter."""

# Structural XML tags that are never linted as widgets.
# These are defined by the helix-xml component system, not by the schema.
STRUCTURAL_TAGS = frozenset(
    {
        "component",
        "view",
        "consts",
        "styles",
        "subjects",
        "gradients",
        "images",
        "fonts",
        "timeline",
        "animation",
        "include_timeline",
        "param",
        "api",
        "prop",
        "slot",
        "translation",
        "translations",
    }
)

# Tags that define constants, styles, or subjects — skip widget validation.
DEFINITION_TAGS = frozenset(
    {
        "px",
        "string",
        "color",
        "font",
        "int",
        "str",
        "percentage",
        "style",
        "subject",
        "tiny_ttf",
        "bin",
        "file",
        "linear",
        "radial",
        "conical",
        "horizontal",
        "vertical",
        "stop",
    }
)
