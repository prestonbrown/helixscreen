# Helix-XML Linter

A schema-driven XML linter for helix-xml — HelixScreen's declarative XML UI system, forked from LVGL's XML engine before v9.5 removed it from core (see `docs/devel/HELIX_XML_FORK.md`).

> **Origin:** Vendored from [GhostTypes/helix-xml-linter](https://github.com/GhostTypes/helix-xml-linter) @ `2124093` (2026-05-03) under MIT license. Adapted for in-tree use in helixscreen; further development happens here.

> **Status:** Beta. Tested against the full helixscreen `ui_xml/` corpus and a synthetic test suite (see `tests/`). API and CLI are stable; new check types may be added.

> **In-tree usage:** Run via `make lint-xml` from the repo root. The linter is pure Python (stdlib only) — no install required.

## Why

The helix-xml engine silently ignores attributes it doesn't recognize. A typo like:

```xml
<lv_button stlye_bg_color="#primary"/>   <!-- 'stlye' instead of 'style' -->
<lv_label align="center_top"/>             <!-- not a valid align value -->
<lv_obj style_bg_color:cheked="#blue"/>    <!-- 'cheked' instead of 'checked' -->
```

…parses cleanly and ships broken UI. There's no compile error, no runtime warning — the attribute is just dropped.

This linter parses XML against a schema extracted directly from the helix-xml C parser sources, catching these silent failures before they ship.

## Installation

```bash
pip install -e ".[dev]"
```

## Usage

```bash
# Lint one or more files
helix-xml-lint ui_xml/panel.xml ui_xml/modal.xml

# Lint entire directory
helix-xml-lint ui_xml/

# Use custom schema
helix-xml-lint --schema custom_schema.json ui_xml/

# Output formats
helix-xml-lint --format text ui_xml/panel.xml      # human-readable (default)
helix-xml-lint --format json ui_xml/panel.xml      # JSON diagnostics
helix-xml-lint --format github ui_xml/panel.xml    # GitHub Actions annotations

# Severity filtering
helix-xml-lint --severity error ui_xml/            # only errors, suppress warnings

# Regenerate schema from C sources
helix-xml-lint extract-schema /path/to/helix-xml/src/xml/ -o schema/schema.json
```

### Example output

```
ui_xml/panel.xml:14:5: error: unknown attribute 'stlye_bg_color' on <lv_button> [unknown-attribute]
ui_xml/panel.xml:22:9: error: invalid enum value 'center_top' for attribute 'align' on <lv_label> [invalid-enum-value]
ui_xml/panel.xml:31:5: warning: unknown state qualifier 'cheked' in 'style_bg_color:cheked' [invalid-state-qualifier]

3 file(s) checked, 2 error(s), 1 warning(s)
```

Format is `file:line:col: severity: message [check-type]`.

## How It Works

1. **Schema Extraction** — Parses C parser source files (`lv_xml_*.c`) to extract widget attributes, enum values, style properties, and registered widgets into `schema.json`.
2. **Linting** — Validates XML files against the schema, checking for unknown attributes, invalid enum values, unknown widgets, and more.
3. **Cross-references** — Validates references between XML elements (constants, styles, subjects, events).

The bundled schema targets LVGL 9.5 / helix-xml as used by helixscreen. When helix-xml updates, re-run `extract-schema` against the new C sources to regenerate `schema/schema.json`.

## Checks

Tiers reflect detection difficulty and confidence:
- **Tier 1** — Single-element checks against the schema. High confidence, mostly errors.
- **Tier 2** — Cross-reference checks across files (constants, styles, subjects, fonts). Warnings.
- **Tier 3** — Semantic checks requiring context (required attrs, component params).

| Tier | Check | Description |
|------|-------|-------------|
| 1 | `unknown-attribute` | Attribute name not in widget's valid set |
| 1 | `unknown-widget` | Element tag not a registered widget |
| 1 | `invalid-enum-value` | Value doesn't match valid enum options |
| 1 | `invalid-bool-value` | Value is not `true`/`false` for boolean attrs |
| 1 | `unknown-style-prop` | `style_*` attribute is not a recognized style property |
| 1 | `invalid-state-qualifier` | Unknown state after `:` in style attribute (e.g. `style_text_color:cheked`) |
| 1 | `xml-parse-error` | Malformed XML that cannot be parsed |
| 2 | `unknown-const-ref` | `#name` reference not defined |
| 2 | `unknown-style-ref` | Style block references undefined style |
| 2 | `unknown-subject-ref` | Subject reference is undefined |
| 2 | `unknown-font-ref` | Font reference is undefined |
| 3 | `missing-required` | Required attribute missing |
| 3 | `component-param-mismatch` | Component used with undeclared params |

## Notes

### State qualifier syntax

LVGL supports state-qualified style attributes using `:` in the attribute name:

```xml
<lv_obj style_bg_color:checked="#primary" style_text_color:disabled="#gray"/>
```

The linter preprocesses these before parsing (replacing `:` with `__`) and validates the qualifier against known LVGL states (`checked`, `pressed`, `focused`, `disabled`, etc.). Typos like `style_bg_color:cheked` surface as `invalid-state-qualifier` warnings.

Part selectors (`-indicator`, `-knob`, etc.) and combined syntax (`style_bg_color-indicator:checked`) are also supported.
