# 01 - Declarative UI

HelixScreen builds every screen the same way: an XML file describes the widget tree, C++ owns the data, and named reactive slots ("subjects") connect the two. A small XML engine - `lib/helix-xml/`, our own MIT-licensed fork of the engine LVGL removed in 9.5 - parses those files at runtime and instantiates real LVGL widgets from them, so editing a layout never requires recompiling. The contract to hold in your head is one line: **data lives in C++, appearance lives in XML, subjects connect them.**

The engine resolves three questions at runtime: *what widgets exist* (C++ registration), *what components exist* (XML files, registered on first use), and *where bindings find their data* (scope-ordered subject lookup). Everything else in this chapter is those three answers plus the lint gate that keeps new code declarative.

```mermaid
flowchart TD
    subgraph XMLFILES["ui_xml/ - runtime-loaded files"]
        GLOBALS["globals.xml<br/>subjects, consts, theme tokens"]
        LAYOUTS["panels, overlays, modals<br/>+ components/ fragments"]
    end

    subgraph BOOT["Boot registration"]
        WIDGETS["C++ widget files<br/>call lv_xml_register_widget()"]
        REG["src/xml_registration.cpp<br/>styles.xml eagerly; every other<br/>component on first use"]
        SUBJ["SubjectInitializer<br/>registers C++ data as named subjects"]
    end

    subgraph RUNTIME["Runtime - navigation drives creation"]
        CREATE["lv_xml_create(parent, 'panel_name')<br/>widget table hit, else component template"]
        LOOKUP["bind_text / bind_flag_if_eq / event_cb<br/>subject lookup: component scope, then globals"]
    end

    GLOBALS --> REG
    LAYOUTS --> REG
    WIDGETS --> CREATE
    REG --> CREATE
    CREATE --> LOOKUP
    SUBJ --> LOOKUP
```

## Key files

| File | Role |
|------|------|
| [`src/xml_registration.cpp`](../../../src/xml_registration.cpp) | Registers `styles.xml` at boot and installs the first-use component loader; registers global event callbacks |
| [`src/application/application.cpp`](../../../src/application/application.cpp) | Startup ordering: widgets → XML components → subjects → root layout |
| [`src/application/session_wiring.cpp`](../../../src/application/session_wiring.cpp) | `create_app_layout()`: the single `lv_xml_create` of the root layout |
| [`src/application/subject_initializer.cpp`](../../../src/application/subject_initializer.cpp) | Creates subjects and publishes C++ state into them |
| [`src/application/xml_hot_reloader.cpp`](../../../src/application/xml_hot_reloader.cpp) | `HELIX_HOT_RELOAD` live re-registration and active-view rebuild |
| [`ui_xml/globals.xml`](../../../ui_xml/globals.xml) | The `globals` scope: consts, theme tokens, XML-declared subjects |
| [`ui_xml/overlay_panel.xml`](../../../ui_xml/overlay_panel.xml) | Reusable wrapper component most overlay panels extend |
| [`ui_xml/history_dashboard_panel.xml`](../../../ui_xml/history_dashboard_panel.xml) | A representative panel: consts, styles, bindings, event callbacks |
| [`ui_xml/app_layout.xml`](../../../ui_xml/app_layout.xml) | Root layout, created once at startup |
| [`src/ui/ui_card.cpp`](../../../src/ui/ui_card.cpp) | Representative custom widget (one of the `lv_xml_register_widget` files) |
| [`include/ui_callback_helpers.h`](../../../include/ui_callback_helpers.h) | `register_xml_callbacks()`: the table every panel uses to publish its XML event callbacks |
| [`include/ui/ui_widget_helpers.h`](../../../include/ui/ui_widget_helpers.h) | `find_required()` / `find_optional()`: the sanctioned way C++ looks up a named widget |
| `lib/helix-xml/src/xml/lv_xml.c` | Engine core: create dispatch, subject lookup, binding elements |
| `lib/helix-xml/src/xml/lv_xml_component.c` | Component registration, scopes, instantiation |
| `lib/helix-xml/src/xml/lv_xml_expr.c` | Integer expression evaluator behind `cond="..."` and `<subject_expr>` |
| [`scripts/check_imperative_ui.py`](../../../scripts/check_imperative_ui.py) | The ratchet gate counting imperative-UI violations |

## How it works

Four mechanics carry the load: the file-to-widget pipeline (including live editing), binding resolution, the shared subject namespace, and the custom-widget layer where app code meets the engine.

### From XML file to live widget

At boot, `Application` runs the phases in a fixed order (phase numbers and lines from [`src/application/application.cpp`](../../../src/application/application.cpp)):

1. Phase 7 - `register_widgets()` (`src/application/application.cpp#register_widgets`): registers the first wave of C++ widget types so the engine knows tags like `ui_card` and `ui_dialog`.
2. Phase 8a - translations, before any UI exists.
3. Phase 8b - rotation probe and layout-manager init, so per-display XML variant directories are known.
4. Phase 8c - `register_xml_components()` (`src/application/application.cpp#register_xml_components`): registers `styles.xml`, installs the first-use loader, and registers the second wave of widgets (`ui_button`, `ui_text_input`, `ui_markdown`, ...) and the app-wide event callbacks.
5. Phase 9a - subject initialization, so every binding can resolve.
6. Phase 10 - `PrinterSession::init_ui()` calls `create_app_layout()` (`src/application/session_wiring.cpp#create_app_layout`), whose `lv_xml_create(screen, "app_layout", nullptr)` instantiates the root layout.

`register_xml_components()` in [`src/xml_registration.cpp#register_xml_components`](../../../src/xml_registration.cpp) registers only `styles.xml` eagerly (its theme-token consts resolve at registration, after theme init) and installs a component loader with `lv_xml_set_component_loader()` ([`src/xml_registration.cpp#register_xml_on_first_use`](../../../src/xml_registration.cpp)). Whenever `lv_xml_component_get_scope()` misses a name (every tag in a view, every `extends=` base, every `lv_xml_create()` and every C++ scope lookup goes through it), the loader registers `ui_xml/<name>.xml` or else `ui_xml/components/<name>.xml`, so no component needs a registration line and none has to be registered before the one that nests or extends it. A name with no file fails loudly: `lv_xml_create` returns NULL and logs an error. The path resolves through `LayoutManager::resolve_xml_path()` ([`src/layout_manager.cpp#resolve_xml_path`](../../../src/layout_manager.cpp)) - which prefers a per-display variant subdirectory when one is active - prefixes an LVGL filesystem drive letter, and hands it to `lv_xml_register_component_from_file()`. The result is a growing table of component templates: named XML fragments like

```xml
<component>
  <consts>
    <!-- Layout dimensions for side-by-side stats + chart -->
    <percentage name="stats_section_width" value="55%"/>
    <percentage name="chart_section_width" value="43%"/>
  </consts>
  <view name="history_dashboard_panel" extends="overlay_panel" title="Print History">
    <!-- child widgets, bindings, event callbacks -->
  </view>
</component>
```

(excerpted from [`ui_xml/history_dashboard_panel.xml`](../../../ui_xml/history_dashboard_panel.xml); comments removed). A template is not yet widgets - just a parsed definition waiting to be instantiated.

Creation happens later, on demand. When navigation needs a panel, its owner calls `lv_xml_create(parent, "history_dashboard_panel", attrs)` ([`src/ui/ui_panel_history_dashboard.cpp#create`](../../../src/ui/ui_panel_history_dashboard.cpp)). The engine (`lib/helix-xml/src/xml/lv_xml.c#lv_xml_create`) first looks the name up in the widget-processor table - the built-in `lv_label`/`lv_slider` types plus our custom `ui_*` widgets. If that misses, it looks up a registered component scope and instantiates the template: recursively creating child widgets, applying attributes, and resolving bindings as it goes.

Components compose. A panel's `<view extends="overlay_panel">` inherits a wrapper template ([`ui_xml/overlay_panel.xml`](../../../ui_xml/overlay_panel.xml), loaded on first use) instead of a bare `lv_obj`; the `extends` link is resolved at instantiation time through the same widget/component tables (`lib/helix-xml/src/xml/lv_xml_component.c#extended_proc`). A component file may also declare `<consts>` - named values visible to that component's bindings and styles - which is where per-panel colors and sizes live when they are not global theme tokens.

Widget naming follows a three-level precedence, set in the engine at `lib/helix-xml/src/xml/lv_xml.c#lv_xml_create`: an explicit `name="..."` at the instantiation site wins; otherwise a `name` the component set on its own `<view>` root is kept; otherwise the object gets a default `<component>_#`. An instance-site name that displaces a `<view>` name logs a one-time warning (`lib/helix-xml/src/xml/lv_xml.c#value_of_name`).

Because components are plain files resolved by name, live editing works.

**Nothing above is compiled in.** `ui_xml/` files are read from disk on first use, so an XML edit takes effect on the next launch with no `make` needed. With hot reload on you do not even relaunch: `XmlHotReloader` ([`src/application/xml_hot_reloader.cpp`](../../../src/application/xml_hot_reloader.cpp)) polls `ui_xml/` every 500 ms on a background thread, well-formedness-checks changed files with expat (no LVGL state touched off the main thread), re-registers changed components, and rebuilds the active panel/overlay/modal in place via `NavigationManager::rebuild_active_views()`. Invalid XML - mid-write truncation, syntax errors - is silently skipped; the existing UI stays live and the next poll retries. Hot reload defaults ON for native dev builds and OFF for cross-compiled release builds; `HELIX_HOT_RELOAD={0,1}` overrides either way ([`src/system/runtime_config.cpp#hot_reload_enabled`](../../../src/system/runtime_config.cpp)). Components whose scopes C++ extends after registration are exempt - `globals`, plus every component marked with `helix::keep_xml_component_registered()` ([`src/xml_registration.cpp`](../../../src/xml_registration.cpp)): `color_picker`, `ams_edit_overlay` and the wizard scopes - because a fresh registration of those would lose theme tokens and breakpoint constants. Editing [`globals.xml`](../../../ui_xml/globals.xml) therefore still needs a relaunch.

One trap follows directly from runtime loading: the XML and the binary can drift. XML referencing a widget that this binary never registered produces an unknown-element path, not a build error (`lib/helix-xml/src/xml/lv_xml.c#lv_xml_create`). If a layout change "does nothing", confirm the binary actually contains the C++ side of what the XML uses.

### How bindings reach data

Two binding vocabularies exist, and both end at the same lookup.

- **Attributes on real widgets**: `bind_text="my_subject"` on a label, `bind_value` on a slider, `bind_style` for reactive styling. The attribute is applied when the widget is created and subscribes the widget to the subject:

  ```xml
  <text_small name="status_message" width="100%" bind_text="power_status" text="" style_text_align="center"/>
  ```

- **Standalone binding elements** placed as children of the widget they target - flag/state/style conditionals with comparison variants:

  ```xml
  <bind_flag_if_eq subject="buf_show_espooler" flag="hidden" ref_value="0"/>
  <bind_style_if_ge name="pad_standard" subject="ui_breakpoint" ref_value="1"/>
  ```

  (The first is from [`ui_xml/power_panel.xml`](../../../ui_xml/power_panel.xml). `bind_style*` targets a named style instead of a raw property - the style itself is declared elsewhere in the file. The two binding elements are verbatim from [`ui_xml/components/buffer_status_modal.xml#hh_section`](../../../ui_xml/components/buffer_status_modal.xml) and [`ui_xml/setting_group_header.xml#"<bind_style_if_ge name=\"pad_standard\" subject=\"ui_breakpoint\" ref_value=\"1\"/>"`](../../../ui_xml/setting_group_header.xml).)

  These are implemented as pseudo-widgets registered by the engine itself (`lib/helix-xml/src/xml/lv_xml.c#lv_xml_init` - names like `lv_obj-bind_flag_if_eq`), which is why they appear in the widget table but never in layouts as `lv_*` tags. The tree uses them by the hundreds; `<bind_flag_if_eq>` and `bind_text=` are the two you will meet first.

Every binding resolves its subject through `lv_xml_get_subject()` (`lib/helix-xml/src/xml/lv_xml.c#lv_xml_get_subject`): first the current component's scope, then the global `globals` scope. A miss is not an error - the engine logs `No subject was found with name "..."` at WARN and the binding stays dead, which surfaces as a blank or frozen widget rather than a crash. That log line is the first thing to grep for when a bound value does not show up.

Events flow the other direction through the same registration idea: XML declares `<event_cb trigger="clicked" callback="on_thing_clicked"/>`, and C++ publishes the implementation by name. Panels and overlays do that with one table per class, `register_xml_callbacks({{"name", handler}, ...})` ([`include/ui_callback_helpers.h#register_xml_callbacks`](../../../include/ui_callback_helpers.h)), from their `register_callbacks()` before their XML is first created. An entry takes a static function or a captureless lambda; a lambda runs inside the exception guard, which logs the callback's name, and reaches its owner through a `get_global_*()` accessor or the binding's `user_data`, since it cannot capture. The handful of app-wide callbacks register in [`src/xml_registration.cpp#register_xml_components`](../../../src/xml_registration.cpp).

Structural conditionals avoid building both branches: `<if cond="expr">...</if>` / `<else>` builds only the matching side, and `<repeat count="4">` clones a fragment with the loop index available as bare `$i` or embedded `${i}` (`lib/helix-xml/src/xml/lv_xml.c#resolve_params` and `lib/helix-xml/src/xml/lv_xml.c#resolve_params/"Embedded"`). A `count` that names a subject rebuilds the fragment when that subject changes. Compound conditions stay in XML too - `<subject_expr name="x" expr="a or b gt c"/>` derives a new subject from existing ones via the integer-only evaluator in `lib/helix-xml/src/xml/lv_xml_expr.c`. Do not hand-write a C++ observer to combine subjects; the evaluator already does it.

**A preset button, end to end.** One button in [`ui_xml/temp_graph_overlay.xml`](../../../ui_xml/temp_graph_overlay.xml) exercises every vocabulary above at once:

```xml
<ui_button name="preset_1" width="48%" bind_text="preset_material_0_name">
  <event_cb trigger="clicked" callback="on_temp_graph_preset_clicked"/>
</ui_button>
```

- `ui_button` is a custom widget, one of the files calling `lv_xml_register_widget`, registered by `ui_button_init()` from `register_xml_components()`.
- `bind_text="preset_material_0_name"` resolves at creation against the globals scope. The subject is registered from C++ in [`src/system/preset_materials.cpp`](../../../src/system/preset_materials.cpp):

  ```cpp
  static constexpr const char* NAME_SUBJECTS[PRESET_COUNT] = {
      "preset_material_0_name", "preset_material_1_name", "preset_material_2_name",
      "preset_material_3_name"};
  // ...
  lv_subject_init_string(&s.name_subjects[i], s.name_bufs[i].data(), nullptr,
                         s.name_bufs[i].size(), s.name_bufs[i].data());
  s.subjects.publish(NAME_SUBJECTS[i], &s.name_subjects[i]);
  ```

  (verbatim from [`src/system/preset_materials.cpp#init_subjects`](../../../src/system/preset_materials.cpp)). `SubjectManager::publish()` ([`include/subject_managed_panel.h#publish`](../../../include/subject_managed_panel.h)) registers the name in the current XML scope, globals at boot, and remembers it so teardown can withdraw the name before freeing the subject. When preset names load from settings, C++ writes the subject once and every bound button across every panel updates.

- `<event_cb ... callback="on_temp_graph_preset_clicked"/>` resolves the name against C++ registrations, here the overlay's own table in [`src/ui/ui_overlay_temp_graph.cpp#register_callbacks`](../../../src/ui/ui_overlay_temp_graph.cpp):

  ```cpp
  register_xml_callbacks({
      {"on_temp_graph_preset_clicked", on_temp_graph_preset_clicked},
      {"on_temp_graph_custom_clicked", on_temp_graph_custom_clicked},
  });
  ```

  The handler at [`src/ui/ui_overlay_temp_graph.cpp#on_temp_graph_preset_clicked`](../../../src/ui/ui_overlay_temp_graph.cpp) receives the click with the button's user data.

Three files, no direct references between them. The XML names a subject and a callback; C++ publishes both by name; the engine ties them at instantiation. This is the shape essentially every interactive element in the app takes.

The overlay those three files produce, as the user meets it - the preset buttons bottom-right are the excerpt's `preset_1`/`preset_2`/`preset_3` widgets, their labels ("PLA", "PETG", "ABS") arriving through the `preset_material_*` subjects that [`preset_materials.cpp`](../../../src/system/preset_materials.cpp) registers (the faulted chamber card in the right column carries the shared banner from [`components/chamber_fault_banner.xml`](../../../ui_xml/components/chamber_fault_banner.xml)):

<img src="../../images/screenshot-temp-graph-overlay.png" alt="The temperature graph overlay: chart, current/target card, preset buttons, and the faulted chamber card in the right column" width="800"/>

### The globals scope: where subjects live

The `globals` component is not a screen. Its scope is the app-wide namespace every binding falls back to. [`ui_xml/globals.xml`](../../../ui_xml/globals.xml) declares the theme-token consts (`<color>`, `<px>`, `<str>`) and a first wave of XML-owned subjects. The bulk of the namespace is registered from C++ after that, by the `init_subjects()` of every state class: `INIT_SUBJECT_*` macros, `SubjectManager::publish()`, or a bare `lv_xml_register_subject(nullptr, name, &subject)` where a null scope means globals ([`src/application/subject_initializer.cpp`](../../../src/application/subject_initializer.cpp) sequences them; chapter 02 has the macros).

Components can declare subjects of their own in a `<subjects>` block (e.g. [`ui_xml/hidden_network_modal.xml#hidden_ssid`](../../../ui_xml/hidden_network_modal.xml)); those live in the component scope and shadow same-named globals - see the gotcha below.

The ownership split matters at teardown: XML-declared subjects are owned by the scope and die with it, while C++-registered subjects are *borrowed* - the scope stores the pointer but never frees it (the hot reloader relies on this; see `lib/helix-xml/src/xml/lv_xml.c#lv_xml_unregister_subject` for the same split in explicit unregistration). Practically: declare a subject in [`globals.xml`](../../../ui_xml/globals.xml) when only XML writes it, register from C++ when C++ owns the storage.

### Custom widgets and the engine that runs them

The widget-processor table is not only built-ins. About thirty-five files under `src/` call `lv_xml_register_widget()` to teach the engine new tags - the visual vocabulary of the app: `ui_card`, `ui_button`, `ui_dialog`, `ui_icon`, `ui_markdown`, `ui_spinner`, `ui_switch`, `ui_text_input`, `ui_temp_display`, `ui_carousel`, `helix_sparkline`, canvas widgets like `ui_bed_mesh` and `ui_gcode_viewer`, and more (full list: `rg -l 'lv_xml_register_widget' src/`). Each file pairs a *create* handler (runs once per instance, sets defaults) with an *apply* handler (runs on attribute application, may run again). [`src/ui/ui_card.cpp`](../../../src/ui/ui_card.cpp) is the cleanest example and a tour stop below.

The engine those files register into is not LVGL's code. `lib/helix-xml/` is a permanent hard fork of the XML engine LVGL removed from core in v9.5. It is MIT-licensed, lives in its own repository (prestonbrown/helix-xml), and upstream is us: there is no LVGL-side upstream to track. Engine changes are committed directly in the submodule, then the bumped pointer is committed here; the `patches/*.patch` workflow applies only to third-party submodules, never to this one. Because LVGL now sells an XML-based product (LVGL Pro / SquareLine), there is a clean-room rule for anything their commercial offering also has - read [`HELIX_XML_FORK.md`](../HELIX_XML_FORK.md) (listed below) before touching the engine.

## Patterns & gotchas

- **The rules are a ratchet.** New code must be fully declarative - no `lv_obj_add_event_cb()`, no `lv_label_set_text()` on XML-owned widgets, no imperative visibility flips, no C++ styling. The tree still carries a few hundred known violations, tracked in prestonbrown/helixscreen#1140. Run `scripts/check_imperative_ui.py --list` to see them by category (visibility, text, style, event). The count may fall, never rise. Existing imperative sites are debt, not precedent. Chapter 15 covers the payoff plan.
- **Name every component instantiation, and look widgets up by name.** Instance names beat `<view>` names, but a `<view name>` is shared by *every* instance of the component - fine for a singleton panel, useless for a repeated row. C++ finds a widget with `helix::ui::find_required(root, name, owner)` when the XML must contain it (a miss logs once, and aborts under strict UI checks) or `find_optional(root, name)` when it may be absent (inside `<if>`, omitted by a layout variant, plugin XML) ([`include/ui/ui_widget_helpers.h#find_required`](../../../include/ui/ui_widget_helpers.h)). [`scripts/check_required_names.py`](../../../scripts/check_required_names.py) checks every literal `find_required` name against each layout variant of the component. Child-index access breaks the moment a layout changes.
- **XML callbacks are table entries, not member pointers.** A `register_xml_callbacks` lambda cannot capture (a `static_assert` says so); per-instance state comes from `lv_event_get_user_data()` or the owner's `get_global_*()` accessor. `event_user_int()`, `event_checked()` and `event_selected()` in the same header read the common payloads.
- **A dead binding is a WARN, not an error.** Misspelled subject names surface as `No subject was found with name` in the log. Debug with `-vv` and grep for that line before suspecting the data layer.
- **Inline style attributes override `bind_style`.** If a widget has both `style_bg_color="..."` and a `bind_style` targeting the same property, use two `bind_style` elements instead - the inline attribute wins and the binding looks broken.
- **Component-local subjects shadow globals.** Lookup checks the component scope first (`lib/helix-xml/src/xml/lv_xml.c#lv_xml_get_subject`), so a component-local `<subject name="...">` declaration (see [`ui_xml/hidden_network_modal.xml#hidden_ssid`](../../../ui_xml/hidden_network_modal.xml)) with the same name as a globals subject silently wins inside that component. If a bound value is stuck at a strange default, check for a same-named local declaration before blaming the C++ side.
- **Choose visibility mechanics by cost.** `<bind_flag_if_eq flag="hidden">` toggles an *already-built* subtree - right for cheap show/hide. `<if cond="...">` builds only the matching branch - right when the subtree is expensive to create (a whole card, an alternate layout). Do not build both and hide one.
- **Expression word forms, not operators.** `cond` and `<subject_expr>` use word forms (`or`, `and`, `gt`, `lt`) because `&&` and `<` need XML escaping; the evaluator is integer-only, so string formatting stays in C++ formatters.
- **C++ may not touch LVGL from background threads, and subject writes count.** `lv_subject_set_*()` fires observers that call widget APIs. Background code routes through `ui_queue_update()`. Chapter 3 and [`THREADING.md`](../THREADING.md) own the details; the rule is absolute here because the XML engine's observers run on whatever thread sets the subject.
- **Custom widgets are the sanctioned escape hatch.** The files calling `lv_xml_register_widget` implement widget *types*; there is no XML beneath them to bind to, so imperative code inside them is correct by definition (see [`src/ui/ui_card.cpp#ui_card_xml_create`](../../../src/ui/ui_card.cpp)). If your genuinely un-declarative site does not fit an escape hatch, annotate it `// DECLARATIVE_OK: <reason>` so audits skip it.

## Going deeper

- [`../LVGL9_XML_GUIDE.md`](../LVGL9_XML_GUIDE.md) - the full XML syntax: every widget, flex layout, styles, observer cleanup in DELETE handlers, structural conditionals, `<repeat>`.
- [`../LVGL9_XML_ATTRIBUTES_REFERENCE.md`](../LVGL9_XML_ATTRIBUTES_REFERENCE.md) - complete attribute reference, binding and style properties per widget.
- [`../HELIX_XML_FORK.md`](../HELIX_XML_FORK.md) - fork origin and licensing, why there is no upstream, the clean-room rule, feature-gap analysis vs LVGL's commercial engine.
- [`../UI_CONTRIBUTOR_GUIDE.md`](../UI_CONTRIBUTOR_GUIDE.md) - breakpoints, design tokens, colors: the layout-level conventions this chapter does not cover.
- [`../CONTRIBUTOR_GOTCHAS.md`](../CONTRIBUTOR_GOTCHAS.md) - symptom-indexed silent-failure traps ("if you see X, you forgot Y") for XML, translations, and subjects.
- [`../THREADING.md`](../THREADING.md) - the threading rules that bind every subject write (chapter 3 summarizes; this is the source of truth).

## Guided code tour

Read in this order; about 25 minutes total.

1. [`ui_xml/temp_graph_overlay.xml#temp_graph_overlay`](../../../ui_xml/temp_graph_overlay.xml) - a whole live overlay in one file. Notice the orthogonal `<styles>` pairs driven by `bind_style_if cond=`, the structural `<if cond="ui_is_portrait eq 0">` branches for landscape and portrait, `bind_text` on the `preset_1` buttons, and the shared component instantiation (`<chamber_fault_banner/>`).
2. [`ui_xml/overlay_panel.xml`](../../../ui_xml/overlay_panel.xml) - the wrapper component those panels extend: positioning, header, and the content slot convention.
3. [`src/xml_registration.cpp#register_xml`](../../../src/xml_registration.cpp) - the `register_xml()` helper: path resolution and the LVGL drive-letter prefix. Then `register_xml_on_first_use()` in the same file, the loader every other component goes through.
4. [`src/application/application.cpp#register_widgets`](../../../src/application/application.cpp) - `register_widgets()` (the first wave of C++ widget registrations), then `src/application/application.cpp#register_xml_components` `register_xml_components()` and its hot-reloader wiring, and finally `src/application/session_wiring.cpp#create_app_layout`, the single `lv_xml_create` that instantiates the root layout. This is the whole boot ordering in four stops.
5. `lib/helix-xml/src/xml/lv_xml.c#lv_xml_create` - `lv_xml_create`: widget-processor table first, component scope second. Then `lib/helix-xml/src/xml/lv_xml.c#lv_xml_create/"Set a default indexed name"` - the name-precedence rules and the default `<component>_#` fallback.
6. `lib/helix-xml/src/xml/lv_xml.c#lv_xml_get_subject` - `lv_xml_get_subject`: the component-scope-then-globals walk, and the WARN on miss you will grep for.
7. `lib/helix-xml/src/xml/lv_xml.c#lv_xml_init` - the `bind_*` pseudo-widget registrations; the entire binding-element vocabulary in ~25 lines.
8. [`src/ui/ui_card.cpp#ui_card_xml_create`](../../../src/ui/ui_card.cpp) - one custom widget end to end: the create handler (runs once), theme style attach, and `ui_card_register()` at [`src/ui/ui_card.cpp#ui_card_register`](../../../src/ui/ui_card.cpp) calling `lv_xml_register_widget("ui_card", ...)`.
9. [`src/application/xml_hot_reloader.cpp`](../../../src/application/xml_hot_reloader.cpp) - the non-reloadable component list and why; read down through the polling loop for the expat pre-check and the silent-skip behavior.
