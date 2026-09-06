---
paths:
  - "src/ui/**/*"
  - "ui_xml/**/*"
  - "include/ui_*.h"
---
# Declarative UI

**DATA in C++, APPEARANCE in XML, Subjects connect them.**

**Absolute for new code.** The tree still carries a few hundred sites that break these
rules (`scripts/check_imperative_ui.py --summary`): some deliberate pragmatism from when
the XML engine could not express what was needed, some plain mistakes that got through
review. Both are debt, tracked in prestonbrown/helixscreen#1140 and being ported.
**Existing imperative code is not precedent.** Do not imitate a nearby site because it is
there, and do not opportunistically refactor one as a side effect of an unrelated change.
The gate ratchets: the count may fall, never rise.

| # | Rule | ❌ NEVER | ✅ ALWAYS |
|---|------|----------|----------|
| 1 | **NO lv_obj_add_event_cb()** | `lv_obj_add_event_cb(btn, cb)` | XML `<event_cb trigger="clicked" callback="name"/>` + `lv_xml_register_event_cb()` |
| 2 | **NO imperative visibility** | `lv_obj_add_flag(obj, HIDDEN)` | XML `<bind_flag_if_eq subject="state" flag="hidden" ref_value="0"/>` for cheap show/hide of an already-built subtree. `<if cond="X">…<else/>…</if>` is the structural sibling: use it when the *creation* itself is expensive (a whole card, an alternate layout); it builds only the matching branch. See `docs/devel/LVGL9_XML_GUIDE.md` § "Structural conditionals with `<if>` / `<else>`" |
| 3 | **NO lv_label_set_text** | `lv_label_set_text(lbl, val)` | Subject binding: `<text_body bind_text="my_subject"/>` |
| 4 | **NO C++ styling** | `lv_obj_set_style_bg_color()` | XML: `style_bg_color="#card_bg"` |
| 5 | **NO manual LVGL cleanup** | `lv_display_delete()`, `lv_group_delete()` | Just `lv_deinit()` - handles everything |
| 6 | **bind_style priority** | `style_bg_color` + `bind_style` | Inline attrs override - use TWO bind_styles |
| 7 | **NO C++ derived subject for compound conditions** | Hand-written observer that combines 2+ subjects (`a \|\| b > c`) | XML `<subject_expr name="x" expr="a or b gt c"/>` or inline `cond="a or b gt c"` on `bind_flag_if`/`bind_state_if`/`bind_style_if` (word forms; `&&`/`<` need XML escaping) |
| 8 | **NO C++ create-and-wire loop for repeated fragments** | `for(int i=0;i<n;i++) { lv_obj_create(...); ... }` in C++ | XML `<repeat count="4">…$i…</repeat>` (fixed) or `<repeat count="a_subject">…${i}…</repeat>` (reactive rebuild). See `docs/devel/LVGL9_XML_GUIDE.md` § "Repeating fragments with `<repeat>`". Measured layout, computed callbacks and data population still belong in C++; `<repeat>` only replaces the widget-creation loop |

**Structural exceptions - C++ is correct here, permanently:**

| Case | Why |
|------|-----|
| Custom XML widget implementations (the files calling `lv_xml_register_widget`) | The file *is* the widget; there is no XML beneath it to bind to |
| `LV_EVENT_DELETE` cleanup, draw hooks (`DRAW_MAIN`/`DRAW_POST`), `SIZE_CHANGED`, gestures/scroll | No declarative equivalent exists |
| Measured layout and computed fonts (`decide_nozzle_layout()`, breakpoint fonts) | Depends on runtime pixel measurement (rule 8) |
| Widgets created in C++ (`lv_*_create`): canvas and procedural rendering | Never had an XML layer |
| Per-item payload on generated collections | `lv_obj_set_user_data()` on a `ui_button` overwrites `UiButtonData*` (`src/ui/temperature_service.cpp#setup_panel`) |
| `helix-screen ctl` remote control (`remote_control_server.cpp`) | Its job is reaching into an arbitrary live widget tree on command |
| CLI stdout (`cli_args.cpp`, `detect_printer_cmd.cpp`, `helix_splash.cpp`) | stdout *is* the product there; spdlog is for logging |
| Widget pool recycling, chart data, animations | Churn or per-frame data that a subject would not model |

Genuinely un-declarative site? Annotate it: `// DECLARATIVE_OK: <reason>`.

## Design Tokens (MANDATORY)

| Category | ❌ WRONG | ✅ CORRECT |
|----------|----------|-----------|
| **Colors** | `lv_color_hex(0xE0E0E0)` | `theme_manager_get_color("card_bg")` |
| **Spacing** | `style_pad_all="12"` | `style_pad_all="#space_md"` |
| **Typography** | `<lv_label style_text_font="...">` | `<text_heading>`, `<text_body>`, `<text_small>` |

`theme_manager_get_color()` for tokens, `theme_manager_parse_hex_color()` for hex strings
only (NOT tokens). `scripts/check_hardcoded_pixels.py` ratchets raw pixel literals.
