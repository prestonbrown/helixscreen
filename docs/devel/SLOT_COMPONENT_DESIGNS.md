# Slot Component Designs

**Status**: Unbuilt proposals. Nothing in this document has shipped.

**What this is**: two XML duplication patterns that are still real, one pattern that already
removes the need for a third, and the measured capability of the expression evaluator so
nobody re-proposes moving string formatting into XML.

**What this is not**: a plan of record. There is no scheduled work behind it.

---

## Ground truth about helix-xml

`lib/helix-xml/` is our MIT fork of the XML engine LVGL removed in 9.5. See
`HELIX_XML_FORK.md` for the fork's origin and licensing position.

### Slots

Components declare slots in `<api>` (`<slot name="body" default="true"/>`): an instance's
widget children land in the default slot, `<component-slot>` fills a named one, and C++
reaches a host with `lv_xml_component_get_slot()`. Semantics:
`lib/helix-xml/docs/BINDINGS.md` § "Slots".

### What the engine does have

Alongside slots:

- `<repeat count="4">` / `<repeat count="a_subject">` for repeated fragments, with `$i` /
  `${i}` substitution. Reactive form rebuilds on subject change.
- `<if cond="...">` / `<else/>` structural conditionals, which build only the matching branch.
- `<subject_expr name="x" expr="a or b gt c"/>` for derived subjects.

See `LVGL9_XML_GUIDE.md` and rules 2 and 8 in `.claude/rules/declarative-ui.md`. Before proposing a slot
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

This is buildable today (it is a C++ custom widget registered with `lv_xml_register_widget`,
not an XML component), and it is the version worth doing first if the pattern ever earns the
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
branch; these gates are reactive: `filament_sensor_count` and `power_device_count` change
after discovery lands. `bind_flag_if_eq` is the correct primitive for this case per rule 2 in
`.claude/rules/declarative-ui.md`. The proposal is only about not retyping the wrapper.

## 3. Bound icon subject (the pattern that needs no slot)

The tune overlay's two Z-offset direction buttons take their icon from a subject:

```xml
<ui_button name="btn_z_farther" width="100%" icon="arrow_up" bind_icon="tune_z_farther_icon">
<ui_button name="btn_z_closer" width="100%" icon="arrow_down" bind_icon="tune_z_closer_icon">
```

`tune_z_closer_icon` and `tune_z_farther_icon` are registered in
`src/ui/print/ui_print_tune_overlay.cpp`. C++ decides the kinematics once and writes the icon name;
the XML binds it. **A bound icon subject beats a pair of mutually-hidden icons** wherever the
choice is computed rather than per-instance.

Sliders still repeat in `ui_xml/retraction_settings_overlay.xml` and
`ui_xml/machine_limits_overlay.xml`. Whether those are similar enough to share a component is
unverified.

---

## Formula migration: measured, and the answer is no

Moving C++ string formatting (`"%d%%"`, `"%dh %02dm"`, temperature pairs) into XML formulas is
not possible with the expression evaluator, and it is not close.

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
| `pad()` / zero-padding | `%02d` minutes | **Not supported**: no functions at all |
| Ternary | heater-off fallback | **Not supported**; use `<if>` or a `cond=` bind |

So none of `print_progress_text`, `print_speed_text`, `print_flow_text`, `tune_speed_display`,
`tune_flow_display`, `tune_z_offset_display`, `print_elapsed` or `print_remaining` can move to
a formula. They format a string, and the evaluator has no strings. Adding a string type to the
evaluator is a much larger piece of work than any of the savings involved, and it would collide
with translation: `lv_tr()` owns the localised forms, and a concatenation built in XML cannot
be looked up as a translation unit.

### What to do instead: a widget that owns the formatting rule

The temperature pair is the worked example. The rule lives in a widget:

```xml
<temp_display size="sm" show_target="true" hide_target_when_off="true"
              bind_current="bed_temp" bind_target="bed_target"/>
```

and in exactly one function, `format_temperature_pair()` in
`src/ui/controls/ui_temperature_utils.cpp`, which converts to the display unit and renders a
heater that is off (target 0 or below) with a dash in place of the target.

`PrinterState` stores temperatures as **decidegrees** (degrees x10), so display divides by
**10**, via `helix::temperature_rules::deci_to_degrees()` (`include/temperature_rules.h`).

A shared widget beats a formula on every axis that matters here: it needs no conditional
operator, keeps one implementation of the heater-off case, and translates.
