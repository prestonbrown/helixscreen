# Slot Component Designs

**Status**: Unbuilt proposals. Nothing in this document has shipped, and two of the four
original designs no longer describe anything in the tree.

**What this is**: two XML duplication patterns that are still real, plus a record of what
happened to the two that are not, plus the measured capability of the expression evaluator so
nobody re-proposes moving string formatting into XML.

**What this is not**: a plan of record. There is no scheduled work behind it.

---

## Ground truth about helix-xml

`lib/helix-xml/` is our MIT fork of the XML engine LVGL removed in 9.5. See
`HELIX_XML_FORK.md` for the fork's origin and licensing position.

### There is no slot system

The engine has the `<component-slotname>` **usage** syntax and nothing behind it. The fork
inherited it from upstream LVGL's `f38718108` ("feat(xml): add slot support", lvgl/lvgl#9193);
the current implementation is `lib/helix-xml/src/xml/lv_xml.c`, in the "If not a component
either, check if it is a slot" branch of the view element handler.

What it does: split the tag on `-`, look up the component scope, then
`lv_obj_find_by_name(state->parent, slot_name)`. That is the whole mechanism.

What is missing:

- No `<slot>` declaration. A component cannot say which slots it offers.
- No `<api>` validation. Filling a slot that does not exist is not an error at parse time.
- No default content.
- No multi-slot dispatch beyond "find a descendant with this name".
- The fallback exists only on the parse path. `lv_xml_create()` tries the widget processor,
  then the component scope, and returns NULL without ever splitting the name, so the two entry
  points into the engine disagree about whether a slot tag means anything.
- A NULL result falls through to the unknown-tag path, which logs the misleading
  "not a known widget/element/component/slot - likely an unregistered widget in a STALE
  BINARY" error. The failure mode for a typo'd slot name is a message about rebuilding.

Real slot declarations are tracked as
[helix-xml#1](https://github.com/prestonbrown/helix-xml/issues/1). Every design below needs
that first.

### What the engine does have

Two features shipped after this document was first written, and they cover most of what slots
were wanted for:

- `<repeat count="4">` / `<repeat count="a_subject">` for repeated fragments, with `$i` /
  `${i}` substitution. Reactive form rebuilds on subject change.
- `<if cond="...">` / `<else/>` structural conditionals, which build only the matching branch.
- `<subject_expr name="x" expr="a or b gt c"/>` for derived subjects.

See `LVGL9_XML_GUIDE.md` and rules 7 and 8 in the root `CLAUDE.md`. Before proposing a slot
component, check whether `<repeat>` or `<if>` already expresses it.

---

## 1. State-mapped icon switcher (still real, unbuilt)

**Where the duplication is**: `ui_xml/components/panel_widget_network.xml`

Six `<icon>` elements, each carrying its own `bind_flag_if_not_eq` against
`home_network_icon_state` with a different `ref_value`. Only `src`, `variant` and the ref value
change between them; every icon is built, and five of the six are hidden at any moment.

```xml
<icon name="net_disconnected" src="wifi_off" size="#icon_size" variant="disabled">
  <bind_flag_if_not_eq subject="home_network_icon_state" flag="hidden" ref_value="0"/>
</icon>
<icon name="net_wifi_1" src="wifi_strength_1_alert" size="#icon_size" variant="warning">
  <bind_flag_if_not_eq subject="home_network_icon_state" flag="hidden" ref_value="1"/>
</icon>
<!-- four more, states 2 through 5 -->
```

The subject is registered in `src/ui/panel_widgets/network_widget.cpp`.

**Proposal**: a `state_icon` component taking one subject and a per-state slot, so the caller
writes the icon list and nothing else. Needs real slots, since the number of states is a
property of the call site rather than the component.

**Prop-based alternative that needs no slots**: pass parallel comma-separated lists and have
the component build the icons itself.

```xml
<state_icon subject="home_network_icon_state"
            icons="wifi_off,wifi_strength_1_alert,wifi_strength_2,wifi_strength_3,wifi_strength_4,ethernet"
            variants="disabled,warning,secondary,secondary,secondary,success"/>
```

This is buildable today - it is a C++ custom widget registered with `lv_xml_register_widget`,
not an XML component - and it is the version worth doing first if the pattern ever earns the
work. It is also the only variant that could stop building all six icons up front.

## 2. Conditional row wrapper (still real, unbuilt)

**Where the duplication is**: `ui_xml/settings_hardware_overlay.xml`

Four capability-gated rows (`container_fan_settings`, `container_filament_sensors`,
`container_led_light`, `container_power_devices`), each an `lv_obj` whose only job is to carry
a `bind_flag_if_eq` over the real row.

```xml
<lv_obj name="container_filament_sensors" width="100%" style_pad_all="0" scrollable="false">
  <bind_flag_if_eq subject="filament_sensor_count" flag="hidden" ref_value="0"/>
  <setting_action_row name="row_filament_sensors" label="Sensors" .../>
</lv_obj>
```

**Proposal**: a `conditional_container` component owning the wrapper attributes and the bind,
with the row itself in a slot.

**Caveat**: `<if cond="...">` is not a substitute here. It decides at build time and builds one
branch; these gates are reactive - `filament_sensor_count` and `power_device_count` change
after discovery lands. `bind_flag_if_eq` is the correct primitive for this case per rule 2 in
the root `CLAUDE.md`. The proposal is only about not retyping the wrapper.

## 3. Z-offset dual icons (solved by a different route, no work remaining)

The original design targeted six z-offset buttons in `ui_xml/print_tune_panel.xml`, each
embedding two `<icon>` elements gated on `printer_bed_moves` so the arrow direction followed
the printer's kinematics.

That XML no longer exists. The panel now has two direction buttons that take the icon from a
subject:

```xml
<ui_button name="btn_z_farther" width="100%" icon="arrow_up" bind_icon="tune_z_farther_icon">
<ui_button name="btn_z_closer" width="100%" icon="arrow_down" bind_icon="tune_z_closer_icon">
```

`tune_z_closer_icon` and `tune_z_farther_icon` are registered in
`src/ui/ui_print_tune_overlay.cpp`. C++ decides the kinematics once and writes the icon name;
the XML binds it. No duplicated icon pairs, no slot needed, and the proposed `z_offset_button`
and `kinematics_icon` components were never created.

Worth keeping as the pattern: **a bound icon subject beats a pair of mutually-hidden icons**
wherever the choice is computed rather than per-instance.

## 4. Tune slider card (pattern gone, design obsolete)

The original design targeted `speed_card` and `flow_card` in `ui_xml/print_tune_panel.xml` -
two near-identical `ui_card` structures each wrapping an `lv_slider`. Its own heading conceded
"No Slots Needed - Props Sufficient", so it was never a slot design.

That XML no longer exists either. Speed and flow are now a `text_heading` bound to
`tune_speed_display` / `tune_flow_display` plus a row of four `ui_button` step adjusters. There
are no sliders and no cards on that panel.

Sliders inside cards do still appear in `ui_xml/retraction_settings_overlay.xml` and
`ui_xml/machine_limits_overlay.xml`. Whether those two are similar enough to share a component
is unverified - nobody has looked.

---

## Formula migration: measured, and the answer is no

An earlier version of this document carried a "String Subject Audit" proposing that C++ string
formatting (`"%d%%"`, `"%dh %02dm"`, temperature pairs) move into XML formulas, pending an
expression evaluator. The evaluator shipped. It does not support this, and it is not close.

`lib/helix-xml/src/xml/lv_xml_expr.c` is an **integer-only** expression evaluator. The lexer
recognises integer literals, identifiers, and operators. There is no string literal token, no
string type, and no function-call syntax in the grammar.

| Capability | Wanted for | Actual status |
|------------|-----------|---------------|
| Integer arithmetic `+ - * /` | all of it | Supported |
| Modulo `%` | time formatting | Supported |
| Comparison, `and` / `or` / `not` | conditionals | Supported |
| Subject references (reactive) | any binding | Supported, via `<subject_expr>` |
| String literals | `'%'`, `'h '`, `'mm'` suffixes | **Not supported** |
| String concatenation | attaching any suffix | **Not supported** |
| `pad()` / zero-padding | `%02d` minutes | **Not supported** - no functions at all |
| Ternary | heater-off fallback | **Not supported**; use `<if>` or a `cond=` bind |

So none of `print_progress_text`, `print_speed_text`, `print_flow_text`, `tune_speed_display`,
`tune_flow_display`, `tune_z_offset_display`, `print_elapsed` or `print_remaining` can move to
a formula. They format a string, and the evaluator has no strings. Adding a string type to the
evaluator is a much larger piece of work than any of the savings involved, and it would collide
with translation: `lv_tr()` owns the localised forms, and a concatenation built in XML cannot
be looked up as a translation unit.

### What to do instead: a widget that owns the formatting rule

The temperature pair is the worked example. Two string subjects, `nozzle_temp_text` and
`bed_temp_text`, formatted a current/target pair in `PrintStatusPanel`. Neither was ever bound
by any XML, so both were removed rather than migrated. The rule now lives in a widget:

```xml
<temp_display size="sm" show_target="true" hide_target_when_off="true"
              bind_current="bed_temp" bind_target="bed_target"/>
```

and in exactly one function, `format_temperature_pair()` in `src/ui/ui_temperature_utils.cpp`.
The heater-off form is an em dash, not `--`:

```cpp
char* format_temperature_pair(int current, int target, char* buffer, size_t buffer_size) {
    if (target == 0) {
        snprintf(buffer, buffer_size, "%d / —°C", current);
    } else {
        snprintf(buffer, buffer_size, "%d / %d°C", current, target);
    }
    return buffer;
}
```

Note the unit conversion. `PrinterState` stores temperatures as **decidegrees** (degrees x10),
so display divides by **10**, via `helix::ui::temperature::deci_to_degrees()`
(`include/ui_temperature_utils.h`). An earlier draft of this section taught `/100`; a formula
written from that would have rendered 210°C as 2°C.

A shared widget beats a formula on every axis that matters here: it needs no conditional
operator, keeps one implementation of the heater-off case, and translates.
