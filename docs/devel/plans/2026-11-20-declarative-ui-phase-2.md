# Declarative UI, phase 2 (1.2)

Branch `feature/declarative-1.2`, worktree `.worktrees/declarative-1.2`. Tracked under
prestonbrown/helixscreen#1140. Deleted in the change that ships the last batch.

## Why

`scripts/check_imperative_ui.py` counts one pattern: an XML widget fetched by name and then
mutated from C++. Phase 1 (#1141-#1145) brought that count to 24. The larger debt is UI that
C++ **builds**: rows, cards, chips and dialogs created with `lv_*_create`, styled with
`lv_obj_set_style_*`, wired with `lv_obj_add_event_cb`, and restyled by walking
`lv_obj_get_child` indexes. The gate cannot see any of it, because nothing was looked up by
name.

Most of it exists because the engine could not express the shape: no way to put an instance's
children inside a component's inner container, no way for a component to branch its structure
on how it was instantiated. Those gaps are fixed in helix-xml first, then the sites move.

## Engine work (lib/helix-xml)

| # | Feature | What it replaces |
|---|---------|------------------|
| E1 (done) | **Slots.** A component declares `<slot name="body" default="true"/>` in its `<api>`, naming an object in its view. An instance's object-creating children land in the default slot; `<comp-body>` targets a named one; element children (`bind_*`, `event_cb`, `style`) still apply to the instance root. `lv_xml_get_slot(instance, "body")` gives C++ the declared host. An undeclared `<comp-x>` keeps today's find-by-name behavior | C++ that fetches `*_container` and fills it; wrapper markup copied because a component could not take children in the right place |
| E2 | **Prop operands in `cond`.** `<if cond="$editing eq 1">` and `bind_*_if cond=` resolve `$prop` / `${prop}` against the component's props when the expression is captured, so a cond over props alone is static | Per-instance structure decided in C++ (edit vs summary card, type-specific fields) |
| E3 (done) | **Host widgets.** A C++ widget's create_cb calls `lv_xml_set_child_host(obj, cb, user_data)`; each object child parsed under it is created on the object `cb` returns, which `ui_carousel` uses to give each XML child a tile as a page. Routed after E1, so a slot whose target is a host widget passes its content on | Carousel pages only reachable through `ui_carousel_add_item` |

Each feature: a `lib/helix-xml/tests/cases/test_*.c` case first, `docs/BINDINGS.md` section,
then `docs/devel/LVGL9_XML_GUIDE.md` and `.claude/rules/declarative-ui.md` updates.

## Slot adoption (E1 shipped; refactor locations, highest value first)

| # | Component | Slots | Today | Change |
|---|-----------|-------|-------|--------|
| S1 (done) | `ui_xml/overlay_panel.xml` | default `overlay_content` | 77 `extends="overlay_panel"` views and 16 hand-rolled overlays each write their own `<lv_obj name="overlay_content" ...>` (93 copies); `src/ui/overlay_base.cpp` and 14 more sites find it by name | overlay_panel owns the column; every view drops its wrapper; `OverlayBase` asks `lv_xml_component_get_slot()`. One commit: adding the slot re-parents every existing wrapper |
| S2 (frame done; 62 dialogs to convert) | `ui_xml/components/dialog_frame.xml` around `ui_dialog`; `modal_dialog.xml` extends it | default `body` (`dialog_body`), `footer` (`dialog_footer`), `header_extra` | 47 dialogs rebuild header + scrolling body + button row; 8 copy `modal_dialog`'s `content_container`; C++ finds `content_container` in `action_prompt_modal.cpp`, `ui_print_select_detail_view.cpp` | one frame component; dialogs supply body and buttons. Exemplars: `modal_dialog`, `bed_mesh_rename_modal`, `debug_bundle_modal`. Guide and batches below |
| S3 (toggle, dropdown, text done) | one `setting_row` base for `setting_toggle_row`, `setting_dropdown_row`, `setting_slider_row`, `setting_action_row`, `setting_text_row` | `control`, optional `extra` | 5 near-duplicates (74-107 lines each) repeat icon, label, description and info button | each row becomes a thin `extends="setting_row"`; its control lands at the row's end, so no slot is needed. `setting_action_row` extends `lv_button` and `setting_slider_row` is a column: both stay separate |
| S4 (slot done; 14 screens to convert) | `ui_xml/header_bar.xml` | `header_content` (declared), `actions` (host `header_actions`, exposed by `overlay_panel`) | 2 slot users; ~30 `action_button_*` props re-forwarded through `overlay_panel` | screens put their own header buttons in `actions`, styled `styles.header_action`; the prop sprawl goes once every screen is converted. Exemplars: `macro_panel`, `printer_manager_overlay`. Guide and batches below |
| S5 | `components/full_page_frame.xml`, `wizard_container.xml` | `body`, `content` | C++ `FullPageHost::body()` (`src/ui/panel_widgets/full_page_host.cpp`) and the wizard step mount (`src/ui/wizard/ui_wizard.cpp`) find the container by name | first C++ users of `lv_xml_component_get_slot()` |
| S6 | `setting_group` + `setting_group_header` | `header` (title/icon props), default body | 58 group+header pairs in 19 files | the header becomes props on the group |
| S7 | `components/context_menu_card.xml` | `subheader` | 3 menus hide the divider and redraw it to fit content above it | content goes in `subheader` |
| S8 | `setting_detail_panel.xml`, `setting_macro_card.xml` | `controls`; `header`, `body` | built and filled from `ui_settings_led.cpp` | part of conversion #1 |
| S9 | `info_qr_modal`, `screws_tilt_share_modal`, `crash_report_modal` | `qr` | C++ inserts the QR into `qr_container` in each | one QR modal frame |
| S10 | `ams_device_section_detail.xml` | `actions` | `ui_cfs_device_flows.cpp`, `ui_indx_device_flows.cpp` create into `section_actions_container` | `get_slot` |
| S11 | home tile chrome (`panel_widget_fan`, `_thermistor`, `_humidity`, `_led`, `_network`, ...) | `icon`, `reading`, `caption` | 17 files repeat `styles.tile_column`/`tile_row` + `bind_tile_rung` | one `tile` component |
| S12 (done) | `ui_carousel` (C++ widget) | default (pages) | pages only through `ui_carousel_add_item` (temp stack, fan stack, power device widgets) | E3: power device and temp stack pages are XML children; fan stack stays (a page per discovered fan is a data list) |

Looked at and left: S6 (`setting_group` is a C++ widget that draws dividers between its
children; folding the header into it saves one line per group for new C++ coupling), S7
(only `spoolman_context_menu.xml` puts content above the rule, and a slot host there adds a
pad gap when both its labels are hidden), S11 (the tiles differ in root type, a `ui_button`
in some, and in how many icons they carry; what they share is two style lines).

Not slots: a widget's own internal list (`option_list`, `fan_list`, `sensor_list`, ...), filled
by its own controller.

Found tonight, for Preston: `lv_xml_to_opa()` is `atoi`, so `opa="cover"` reads as 0 and
renders transparent. Seven sites write it (`setting_macro_card.xml`, `debug_bundle_modal.xml`,
`bed_drying_modal.xml`, `chamber_dryer_modal.xml`, `crash_report_modal.xml` and two more;
`grep -rn 'opa="cover"' ui_xml`). Accepting the keyword would make all seven opaque, a visible
change, so it waits for a decision.

Measured: creating 107 `printer_type_row` components takes 2.4 ms against 0.34 ms for
the equivalent `lv_obj_create` calls (x86, test binary). The wizard caches the rows after
its first visit (#231). A callgrind profile of those 107 creates: expat's own tokenizing is
about 15% of the cost, the element handlers 79%, of which `lv_xml_obj_apply`'s linear
`lv_streq` chain over attribute names is 19%, style refreshes 13% and the semantic text
widgets 12%. A parse cache would save at most 15-20%; a hashed attribute dispatch in the
obj parser about as much again. Neither closes the gap to hand-written C++, so a list long
enough to matter on MIPS should stay cached as this one is.

Style attributes now skip lv_obj's attribute chain and the style chain compares
suffixes, which cut the same creates by 7.5% (71.9M to 66.5M instructions) and
`apply_styles` by 43%. What is left of the chain cost is widget attributes (`text`,
`src`) failing lv_obj's ~50 names before their own parser; skipping it needs a
second name list kept in step with the chain, for about 3% more.

Found while building E1: `lib/` is on `-isystem`, so `-MMD` does not track
`lib/helix-xml/src/xml/lv_xml_component_private.h`, which `src/xml_registration.cpp`,
`src/application/xml_hot_reloader.cpp` and `src/remote/remote_control_server.cpp` include.
A layout change there leaves their objects stale. The scope struct now only grows at its end;
the dependency gap itself is a follow-up.

## S2/S4 migration guide

### One dialog

A dialog extends `dialog_frame` instead of `ui_dialog`. The frame owns the card
(`width` prop, default 70%; `style_max_height="85%"`; column; no gap), the header
(`modal_header`: icon, then the title, left-aligned), a body column padded `#space_lg`
on the sides and bottom with a `#space_md` gap and capped at `#dialog_content_max`
(it scrolls past that), and a footer for the button rows.

| Prop | Meaning |
|---|---|
| `title`, `title_tag` | The title and its translation key |
| `title_subject` | A string subject that retitles it live (no default: unset binds nothing) |
| `icon`, `icon_variant` | An icon name before the title (empty = none); `primary`, `warning`, `danger`, `success`, `accent`... |
| `width` | Card width, default `70%` |
| `hide_close`, `close_callback` | An X after the title |

Widgets in the view land in the body. `<dialog_frame-footer>` takes the
`<modal_button_row>`(s); `<dialog_frame-header_extra>` takes widgets between the
title and the X. Restyle the body on its slot tag:
`<dialog_frame-body style_flex_cross_place="center" style_pad_gap="#space_sm">`.

Before (`ui_xml/bed_mesh_rename_modal.xml`):

```xml
<view name="bed_mesh_rename_modal"
      extends="ui_dialog" width="70%" height="content" align="center" flex_flow="column" style_flex_main_place="start"
      style_pad_gap="0">
  <lv_obj name="rename_form_container"
          width="100%" height="content" style_min_height="180" style_pad_left="#space_lg" style_pad_right="#space_lg"
          style_pad_top="#space_lg" style_pad_bottom="#space_lg" flex_flow="column" style_flex_cross_place="center"
          style_pad_gap="#space_md">
    <text_heading width="100%" text="Rename Mesh Profile" style_text_align="center"/>
    ...form widgets...
  </lv_obj>
  <modal_button_row primary_text="Rename" secondary_text="Cancel" primary_callback="on_bed_mesh_rename_confirm"
                    secondary_callback="on_bed_mesh_rename_cancel"/>
</view>
```

After:

```xml
<view name="bed_mesh_rename_modal"
      extends="dialog_frame" title="Rename Mesh Profile" title_tag="Rename Mesh Profile">
  <dialog_frame-body style_flex_cross_place="center">
    ...form widgets...
  </dialog_frame-body>
  <dialog_frame-footer>
    <modal_button_row primary_text="Rename" secondary_text="Cancel" primary_callback="on_bed_mesh_rename_confirm"
                      secondary_callback="on_bed_mesh_rename_cancel"/>
  </dialog_frame-footer>
</view>
```

Steps, per dialog:

1. Write down its C++ lookups first (`grep -rn '"<widget name>"' src tests`). Names
   inside the body and footer keep working; the frame names its own parts
   `dialog_title`, `dialog_icon`, `dialog_header`, `dialog_body`, `dialog_footer`.
2. The title row (an `icon` + `text_heading`, a `modal_header`, or a centered heading
   at the top of the body) becomes `title`/`title_tag`/`icon`/`icon_variant`. A centered
   title becomes left-aligned; nothing else should move. A `modal_header` with
   `hide_close="false"` maps to `hide_close`/`close_callback`.
3. The content container's children move into the view (or a `<dialog_frame-body>` tag
   carrying the attributes that differ from the frame's). A container C++ fills by
   name (`content_container`) is reached with `lv_xml_component_get_slot(dialog, nullptr)`.
4. Button rows go in `<dialog_frame-footer>`, with their bindings unchanged.
5. A `text_heading` C++ reads as the modal title must stay `dialog_title`
   (`kModalTitleWidgetName`); the frame's title already is.
6. Screenshot before and after at 800x480 and 480x272 from fresh launches; the title
   moves to the left of the header, the rest should match.

Leave a dialog on `ui_dialog` and say why when it is not a titled card: a full-screen
picker (`color_picker`), a dialog whose header is its content (QR modals, owned by
the S9 work), or one with a measured layout. Severity icons that C++ or a subject toggles
(`icon_info`/`icon_warning`/`icon_error` in `runout_guidance_modal`,
`exclude_object_modal`, `action_prompt_modal`, `recovery_fault_modal`) become the
`icon`/`icon_variant` props, which the caller sets from the severity when it creates the
dialog, as `helix::ui::ModalDialogAttrs` does for `modal_dialog`. A test that finds an
`icon_*` by name checks the shown glyph instead (`visible_glyphs()` in
`tests/unit/test_dialog_frame.cpp`).

### One header

Before (`ui_xml/macro_panel.xml`):

```xml
<view name="macro_panel"
      extends="overlay_panel" title="Macros" hide_action_button="false" action_button_text="Save"
      action_button_callback="on_macros_edit_save" action_button_hidden_subject="macros_edit_save_hidden"
      back_button_callback="on_macros_back_clicked">
```

After:

```xml
<view name="macro_panel" extends="overlay_panel" title="Macros" back_button_callback="on_macros_back_clicked">
  <overlay_panel-actions>
    <ui_button name="btn_save" height="content" text="Save" translation_tag="Save">
      <style name="styles.header_action"/>
      <bind_flag_if_eq subject="macros_edit_save_hidden" flag="hidden" ref_value="1"/>
      <event_cb trigger="clicked" callback="on_macros_edit_save"/>
    </ui_button>
  </overlay_panel-actions>
```

Prop to markup:

| Prop | Markup on the button in `actions` |
|---|---|
| `hide_action_button="false"` + `action_button_text`/`_text_tag` | `<ui_button height="content" text=".." translation_tag="..">` + `<style name="styles.header_action"/>` |
| `action_button_icon`, `_icon_size` | `icon="..." icon_size="..."` |
| `action_button_bg_color`, `_radius`, `_width`, `_height` | `style_bg_color`, `style_radius`, `width`, `height` on the button (keep `height="content"` unless a size was passed) |
| `action_button_callback` | `<event_cb trigger="clicked" callback="..."/>` |
| `action_button_hidden_subject` | `<bind_flag_if_eq subject="..." flag="hidden" ref_value="1"/>` |
| `action_button_disabled_subject` / `_disabled_cond` | `<bind_state_if_eq subject=".." state="disabled" ref_value="1"/>` / `<bind_state_if cond=".." state="disabled"/>` |
| `action_button_2_*` | a button BEFORE the primary one, `style_bg_color="#secondary"`, `style_min_width="#header_slot2_min_width"`, `label_hidden_cond` as is |
| `action_icon*` | a round button first: `width="#button_height_sm" height="#button_height_sm" style_max_height="#header_button_height"`, `<style name="styles.header_icon_button"/>` |
| `*_hidden_cond` | `<bind_flag_if cond=".." flag="hidden"/>` |

Order inside `actions` is left to right: icon button, secondary, primary. A screen using
`<header_bar>` directly (not `extends="overlay_panel"`) writes `<header_bar-actions>`.

**Name the buttons.** header_bar has no built-in action buttons, so each slot button
carries its own name (`btn_save`, `btn_clear_all`, ...) and every lookup (C++, tests, ctl
recipes in `scripts/screenshot-recipes.sh` and `tests/ui/`) uses it.

Before/after screenshots of a header must be pixel-identical.

### Batches

Paths are under `ui_xml/`; `components/` is noted. Each batch takes about a quarter of
the dialog lines and three or four headers.

| Batch | Dialogs | Headers |
|---|---|---|
| 1 | abort_progress_modal, action_prompt_modal (C++ `content_container`), ams_loading_error_modal, ams_recover_state_modal, batch_filament_modal, components/bed_check_modal, bed_drying_modal, bed_mesh_calibrate_modal, bed_mesh_save_config_modal, belt_names_modal, belt_tension_auto_modal, components/buffer_status_modal, components/camera_config_modal, chamber_dryer_modal, components/chamber_stall_modal, change_host_modal, components/clog_detection_config_modal | ams_edit_overlay, input_shaper_panel, numeric_keypad_panel, theme_preview_overlay |
| 2 | color_picker, crash_report_modal (S9), create_filament_modal, create_vendor_modal, estop_confirmation_dialog, exclude_object_modal, fan_rename_modal, favorite_macro_config_modal, components/filament_catalog_picker, filament_mapping_modal, filament_product_edit_modal, guided_load_modal, hidden_network_modal | bed_mesh_panel, led_settings_overlay, panel_belt_tension (a direct `<header_bar>`), widget_catalog_overlay |
| 3 | info_qr_modal (S9), components/ipp_print_modal, ipv4_keep_modal, job_queue_modal, klipper_recovery_dialog, layer_pause_modal, macro_param_modal, network_test_modal, plugin_install_modal, components/preflight_check_modal, print_cancel_confirm_modal, print_completion_modal, probe_accuracy_modal, probe_config_edit_modal, recovery_fault_modal, restart_prompt_dialog, runout_guidance_modal | calibration_pid_panel, motion_panel, print_status_panel |
| 4 | runout_other_head_modal, screws_tilt_share_modal (S9), shutdown_modal, components/spaghetti_detection_modal, spoolman_edit_modal, stepper_resonance_cal_modal, telemetry_info_modal, components/temp_graph_config_modal, text_entry_modal, theme_save_as_modal, toolhead_preset_config_modal, components/unclaimed_toolhead_modal, update_download_modal, update_notify_modal, wifi_password_modal | history_dashboard_panel, notification_history_panel (C++ wiring), spoolman_panel |

(S9) marks the three QR dialogs the QR-frame work also touches: settle with that session
before editing them.

## Conversion order (worst first)

From the 2026-11-20 audit. A = convertible today (or with E1-E3), B = static chrome moves,
data-driven parts stay.

| # | Site | Class |
|---|------|-------|
| 1 | `src/ui/settings/ui_settings_led.cpp`: macro device cards and edit form, auto-state rows (`#populate_macro_devices_impl`, `#rebuild_macro_edit_controls`, `#populate_auto_state_rows`, `#rebuild_contextual_controls`) | A |
| 2 | `src/ui/wizard/ui_wizard_printer_identify.cpp#populate_printer_type_list`, `#populate_vendor_tiles`, `#update_list_selection` | A |
| 3 | Three hand-built copies of `picker_option_row`: `src/ui/panel_widgets/print_status_widget.cpp` (the ConfigurePicker and NozzleToolPicker `on_created` bodies), `fan_widget.cpp` fan picker | A |
| 4 | `src/ui/components/ui_multiselect.cpp#build_row` | A |
| 5 | `src/ui/controls/ui_overlay_temp_graph.cpp#create_chips` | A |
| 6 | `src/ui/print/ui_panel_history_dashboard.cpp#update_filament_chart` (bar rows; the trend chart stays) | A |
| 7 | `src/ui/settings/ui_settings_telemetry_data.cpp` event cards | A |
| 8 | `src/ui/filament/ui_panel_ams.cpp` backend segment buttons | A |
| 9 (done) | `src/ui/panel_widgets/temp_stack_widget.cpp#attach_carousel` and `src/ui/widgets/power_device_widget.cpp#show_energy_page` declare their pages in XML; `src/ui/panel_widgets/fan_stack_widget.cpp#bind_carousel_fans` stays a data list | A/B |
| 10 | `ui_busy_overlay.cpp`, `ui_panel_home.cpp` carousel arrows, `ui_filament_mapping_modal.cpp` switch row, `ui_panel_glyphs.cpp` | A, small |
| 11 | `ui_step_progress.cpp`, `action_prompt_modal.cpp`, `src/ui/tour/tour_overlay.cpp#build_tree`, `src/ui/calibration/ui_panel_screws_tilt.cpp#create_screw_indicator`, `grid_edit_mode.cpp` chrome, `ui_exclude_object_map_view.cpp` badges, `ui_keyboard_manager.cpp` popup | B |

Left in C++ on purpose: bootstrap (`ui_fatal_error.cpp`, splash), AMS drawing and measured
tray layout, grid layout, theme application, charts, animation, games, and the files that
implement XML widgets.

Engine gaps not in E1-E3, decide later: `<repeat>` over a data list rather than a count;
nested `<repeat>`/`<if>`; derived colors (contrast text on a computed background).

## Test plan (one session owns it)

- Engine: each feature's Unity case goes red first (`make test-xml`).
- Each converted site: a characterization test of the behavior through widget names, written
  and passing **before** the conversion, unchanged after it; `make t F='[tag]'` per site.
- Finish, once for the batch: one review, one screenshot per changed screen at 800x480 and
  480x272, one `make land-gate`, push helix-xml `main` before the pin moves.
