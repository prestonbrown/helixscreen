# Your First Contribution

A walk through what a real HelixScreen contribution looks like, using code already in the repo. No invented "Hello World" — you'll learn by reading a feature that shipped and understanding why each piece is there.

---

## What contributions actually look like

HelixScreen has a fixed set of top-level navigation panels — `home`, `print-select`, `controls`, `filament`, `settings`, `advanced`. **These don't get added to.** If you think you need a new top-level panel, step back — you almost certainly want an overlay, a modal, or a settings subpage instead.

Realistic contribution shapes, ranked by scope:

| Contribution | Typical scope | Examples |
|---|---|---|
| **Theme / design token tweak** | Pure XML + token edits | New dark variant, spacing adjustment |
| **Translation** | YAML + regenerated artifacts | Add a new language, fix a string |
| **Layout fix at a breakpoint** | Pure XML | 480x320 clipping, portrait wrapping |
| **Printer database entry** | JSON only | Add support for a new printer model |
| **Settings overlay** | XML + C++ class inheriting `OverlayBase` | Retraction Settings, Barcode Scanner |
| **Feature modal** | XML + `Modal` subclass | Plugin Install, Bed Mesh Rename |
| **Component widget** | XML component + optional C++ custom widget | `ui_card`, `filament_sensor_indicator` |
| **Full feature subsystem** | Multiple XML components + C++ panel + state backend | AMS, Filament panel, Macros panel |

This doc focuses on the **settings overlay** tier (the most common real contribution shape) with a pattern tour of the full-feature tier at the end.

---

## Part 1: The walkthrough — Retraction Settings Overlay

The retraction settings overlay is about as small as a real contribution gets: four sliders, a toggle, and a G-code send. It lives in three files:

| File | Lines | Purpose |
|---|---|---|
| `ui_xml/retraction_settings_overlay.xml` | 175 | Layout, styling, bindings |
| `include/ui_overlay_retraction_settings.h` | 162 | Class declaration, subject + widget references |
| `src/ui/ui_overlay_retraction_settings.cpp` | 293 | Lifecycle, event handlers, G-code send |

You'll also touch four *other* files to wire it into the app:

- `src/xml_registration.cpp` — register the XML component
- `src/application/subject_initializer.cpp` — construct the global instance at boot
- `ui_xml/settings_printing_overlay.xml` — add a row that opens the overlay
- (optional) translation YAML if you added user-visible strings

Let's walk through each.

### Step 1: The XML layout (`ui_xml/retraction_settings_overlay.xml`)

The top of every overlay XML looks like this:

```xml
<component>
  <view name="retraction_settings_overlay"
        extends="overlay_panel"
        title="Retraction Settings"
        title_tag="Retraction Settings"
        bg_color="#screen_bg">
    <lv_obj name="overlay_content" width="100%" flex_grow="1"
            style_pad_all="#space_lg" scrollable="true"
            flex_flow="column" style_pad_gap="#space_lg">
      <!-- sections go here -->
    </lv_obj>
  </view>
</component>
```

Notes:

- **`extends="overlay_panel"`** — inherits the standard overlay chrome: title bar, back button, backdrop. You almost always want this.
- **`title_tag="..."`** — the translation key. `title` is the English source; `title_tag` is what gets looked up in other languages. Both must be present for user-visible strings.
- **No `width`** — overlay width is not yours to set. `NavigationManager::push_overlay()` decides it from how the user reached the overlay: full width inside Settings, gapped when it is a tool opened over something else. See `include/overlay_class.h`.
- **`#space_lg`, `#screen_bg`** — design tokens. Never hardcode pixel spacing or hex colors.
- **Inner `<lv_obj name="overlay_content">`** — the scrollable content area. The `overlay_panel` base provides the frame; you provide what's inside.

Inside the content area, each setting is structured consistently (lines 40–66 of the real file, slightly abbreviated):

```xml
<lv_obj name="retract_length_section"
        width="100%" height="content" style_pad_all="0"
        scrollable="false" flex_flow="column" style_pad_gap="#space_sm">

  <!-- Top row: label on left, current value on right -->
  <lv_obj width="100%" height="content" style_pad_all="0"
          flex_flow="row" style_flex_main_place="space_between"
          style_flex_cross_place="center">
    <lv_obj height="content" flex_flow="column" style_pad_gap="#space_xxs">
      <text_body text="Retract Length" translation_tag="Retract Length"/>
      <text_small text="0.4-2mm direct drive, 4-6mm bowden"
                  translation_tag="0.4-2mm direct drive, 4-6mm bowden"/>
    </lv_obj>
    <text_body name="retract_length_label" bind_text="retract_length_display"/>
  </lv_obj>

  <!-- Slider row -->
  <lv_obj width="100%" height="50" flex_flow="row" style_flex_cross_place="center">
    <text_small width="30" text="0"/>
    <lv_slider name="retract_length_slider" flex_grow="1"
               min_value="0" max_value="600" value="80">
      <event_cb trigger="value_changed" callback="on_retraction_setting_changed"/>
    </lv_slider>
    <text_small width="40" text="6mm" translation_tag="6mm"/>
  </lv_obj>
</lv_obj>
<divider_horizontal/>
```

Things worth noticing:

- **`bind_text="retract_length_display"`** — this label's text comes from a subject named `retract_length_display`. The C++ side owns that subject and writes to it when the slider moves.
- **`<event_cb trigger="value_changed" callback="on_retraction_setting_changed"/>`** — when the slider value changes, call the C++ function registered under the name `on_retraction_setting_changed`. **No `lv_obj_add_event_cb()` in C++ — ever.**
- **Callback naming** — the convention is `on_<component>_<action>`. Generic names like `on_changed` will collide. (See lesson **L039**.)
- **`translation_tag` on every user-visible string.** Exception: product names, material codes, universal terms. (See `CONTRIBUTOR_GOTCHAS.md`.)
- **`<divider_horizontal/>`** — a semantic widget that applies the standard divider style. Don't roll your own with `lv_obj` + `border_width`.

### Step 2: The class declaration (`include/ui_overlay_retraction_settings.h`)

The header declares a class that inherits `OverlayBase`:

```cpp
class RetractionSettingsOverlay : public OverlayBase {
  public:
    explicit RetractionSettingsOverlay(IMoonrakerAPI* api);
    ~RetractionSettingsOverlay() override;

    // OverlayBase virtuals
    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    [[nodiscard]] const char* get_name() const override {
        return "Retraction Settings";
    }
    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;

  private:
    void send_retraction_settings();
    void update_display_labels();
    void sync_from_printer_state();

    static void on_enabled_changed(lv_event_t* e);
    static void on_setting_changed(lv_event_t* e);

    // Widget pointers — cached in create(), used by event handlers
    lv_obj_t* enable_switch_ = nullptr;
    lv_obj_t* retract_length_slider_ = nullptr;
    // ... etc

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // Subjects that C++ writes to (label text)
    lv_subject_t retract_length_display_;
    // ... etc

    // Backing buffers for string subjects
    char retract_length_buf_[16];
    // ... etc

    IMoonrakerAPI* api_ = nullptr;
    bool syncing_from_state_ = false;
};

// Global accessor — constructed once by subject_initializer at boot
// (IMoonrakerAPI is the consumer-facing interface — panels never name the
// concrete MoonrakerAPI class)
RetractionSettingsOverlay& get_global_retraction_settings();
void init_global_retraction_settings(IMoonrakerAPI* api);
```

Patterns to internalize:

- **Inherit `OverlayBase`.** It provides `lifetime_` (AsyncLifetimeGuard), `overlay_root_`, and virtual lifecycle hooks. Don't reinvent overlay chrome.
- **Cache widget pointers in `create()`.** Don't re-lookup in event handlers.
- **`SubjectManager subjects_`** — RAII wrapper for cleanup. When the overlay is destroyed, `subjects_.deinit_all()` handles it.
- **Static event handlers.** LVGL callbacks are C-style function pointers. You'll need a way to reach the instance from inside them — the retraction overlay uses a global accessor, but see the note below on why that's legacy.

> **Note on the singleton pattern:** The `get_global_retraction_settings()` + `init_global_retraction_settings()` shape you see here — static `std::unique_ptr` constructed at boot, lives for the app lifetime — is a **legacy pattern**. It exists because most overlays were written when the codebase was smaller and boot-time construction was simpler. **Going forward, new overlays should prefer dynamic allocation: construct on open, destroy on close.** Two reasons: (1) memory isn't wasted on overlays the user never opens, critical on small devices like AD5M (14–20MB RAM budget), and (2) if multiple instances ever become useful, nothing in the design blocks that. If you're writing a new overlay, don't reach for the global accessor shape reflexively — instantiate from the row-click handler, hand ownership to `NavigationManager`, let it destroy on pop. See "Going forward" at the end of this section for the sketch.

### Step 3: The implementation (`src/ui/ui_overlay_retraction_settings.cpp`)

The interesting parts:

**3a. Initialization (`init_subjects()`)**

```cpp
void RetractionSettingsOverlay::init_subjects() {
    // Create the subjects the XML binds to
    UI_MANAGED_SUBJECT_STRING(retract_length_display_, retract_length_buf_, "0.0mm",
                              "retract_length_display", subjects_);
    // ... same for the other three sliders ...

    // Register the event callbacks the XML references by name
    lv_xml_register_event_cb(nullptr, "on_retraction_row_clicked",
                             on_retraction_row_clicked);
    lv_xml_register_event_cb(nullptr, "on_retraction_enabled_changed",
                             on_enabled_changed);
    lv_xml_register_event_cb(nullptr, "on_retraction_setting_changed",
                             on_setting_changed);
}
```

Two things happen here:

1. **Subject creation.** `UI_MANAGED_SUBJECT_STRING` creates a string subject, registers it globally under the given name, and tells `subjects_` to clean it up on destruction. The XML's `bind_text="retract_length_display"` resolves to this subject.
2. **Callback registration.** Every string in `<event_cb callback="...">` must be registered with `lv_xml_register_event_cb()` *before* the XML is parsed. If the callback string doesn't match a registered name, the event silently does nothing — a common first-time mistake. (See `CONTRIBUTOR_GOTCHAS.md`.)

**3b. Widget creation (`create()`)**

```cpp
lv_obj_t* RetractionSettingsOverlay::create(lv_obj_t* parent) {
    overlay_root_ = (lv_obj_t*)lv_xml_create(parent,
                                             get_xml_component_name(),
                                             nullptr);
    if (!overlay_root_) return nullptr;

    enable_switch_ = lv_obj_find_by_name(overlay_root_, "retraction_enabled_switch");
    retract_length_slider_ = lv_obj_find_by_name(overlay_root_, "retract_length_slider");
    // ... etc

    return overlay_root_;
}
```

`lv_xml_create()` parses your XML component into a real widget tree. After that, use `lv_obj_find_by_name()` to locate widgets you need to poke at from C++ (sliders for reading values, switches for setting checked state). The `name="..."` attributes in your XML are what make this lookup work — give a `name` to anything you'll touch from C++.

**Never use `lv_obj_get_child()`** — it's index-based and fragile. If someone reorders the XML, your code breaks silently.

**3c. The lifecycle hooks**

```cpp
void RetractionSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    sync_from_printer_state();
}
```

`on_activate()` runs every time the overlay becomes visible. For a settings overlay, this is where you pull the current values from `PrinterState` and seed the UI. `on_deactivate()` runs when it's dismissed — clean up timers, stop polling, etc.

**3d. Event handlers**

```cpp
void RetractionSettingsOverlay::on_setting_changed(lv_event_t* /*e*/) {
    auto& overlay = get_global_retraction_settings();
    overlay.update_display_labels();

    if (overlay.syncing_from_state_) return;
    overlay.send_retraction_settings();
}
```

Notice the `syncing_from_state_` guard. Without it, calling `lv_slider_set_value()` inside `sync_from_printer_state()` would fire `value_changed`, which would then send a G-code, which the printer would echo back, which would... it's a feedback loop. Flag-guard sync paths whenever the source-of-truth is external.

**3e. The "open me" callback**

```cpp
static void on_retraction_row_clicked(lv_event_t* /*e*/) {
    if (!g_retraction_settings) return;

    if (!g_retraction_settings_panel) {
        g_retraction_settings_panel =
            g_retraction_settings->create(lv_display_get_screen_active(nullptr));
        NavigationManager::instance().register_overlay_instance(
            g_retraction_settings_panel, g_retraction_settings.get());
    }

    NavigationManager::instance().push_overlay(g_retraction_settings_panel);
}
```

Lazy-create on first open (fast boot), reuse thereafter. `NavigationManager::push_overlay()` handles animation, backdrop, and the activation lifecycle.

### Step 4: Wire it into the app

Four one-line edits:

**4a. Register the XML component** — `src/xml_registration.cpp`:

```cpp
register_xml("retraction_settings_overlay.xml");
```

Without this, `lv_xml_create("retraction_settings_overlay", ...)` returns null. Put it near similar overlays, not at random.

**4b. Construct the global instance at boot** — `src/application/subject_initializer.cpp`:

```cpp
init_global_retraction_settings(api);
```

This is where every overlay's `init_global_*()` gets called during app startup. Order can matter if overlays depend on each other — follow the pattern of siblings.

**4c. Add the row that opens it** — `ui_xml/settings_printing_overlay.xml`:

```xml
<lv_obj name="container_firmware_retraction" width="100%" style_pad_all="0" scrollable="false">
  <bind_flag_if_eq subject="printer_has_firmware_retraction" flag="hidden" ref_value="0"/>
  <setting_action_row name="row_firmware_retraction"
                      label="Retraction Settings"
                      label_tag="Retraction Settings"
                      icon="arrow_up_right"
                      description="Configure G10/G11 firmware retraction"
                      description_tag="Configure G10/G11 firmware retraction"
                      callback="on_retraction_row_clicked"/>
</lv_obj>
```

The `<bind_flag_if_eq>` wrapper hides the row on printers that don't support firmware retraction — this is how you conditionally expose features without a runtime C++ check.

**4d. Run it**

```bash
make -j
./build/bin/helix-screen --test -vv
```

Navigate to Settings → Printing → Retraction Settings. If nothing appears: check `CONTRIBUTOR_GOTCHAS.md`.

### The full shape

```
ui_xml/
  retraction_settings_overlay.xml        ← your layout
  settings_printing_overlay.xml          ← edit to add the row

include/
  ui_overlay_retraction_settings.h       ← your class

src/ui/
  ui_overlay_retraction_settings.cpp     ← your impl

src/
  xml_registration.cpp                   ← edit to register_xml()
  application/subject_initializer.cpp    ← edit to call init_global_*()
```

That's the full shape of a real settings overlay contribution. Every other overlay in `src/ui/ui_overlay_*.cpp` and every `ui_settings_*.cpp` follows this same pattern. If you're stuck, find the closest sibling, open its three files side-by-side, and diff against yours.

### Going forward: dynamic overlays

The retraction overlay is the pattern the codebase *has*. Here's the pattern it's moving *toward* — use this shape for new overlays:

**What changes:**

- **No global accessor, no `init_global_*`.** Drop the `static std::unique_ptr<...> g_instance` at the top of the .cpp file and the call site in `subject_initializer.cpp`.
- **Construct from the row-click handler.** When the user taps the row that opens your overlay, instantiate on the spot and hand ownership to `NavigationManager`.
- **Destroy on pop.** When the overlay is dismissed, the NavigationManager destroys the instance. Next open creates a fresh one.
- **Event handlers need an instance pointer, not a global lookup.** The cleanest path is to capture `this` in the `<event_cb>` registration by using an instance method or a small adapter, so handlers don't need `get_global_*()` at all. Non-static member callbacks registered per-instance are preferred over static free functions that reach into globals.

**What stays the same:**

- Inheriting `OverlayBase`.
- The `init_subjects()` / `create()` / `on_activate()` / `on_deactivate()` / `cleanup()` lifecycle.
- The XML structure — this is purely a C++ pattern change.
- `SubjectManager` for subject cleanup.

**Why this matters:**

1. **Memory.** On AD5M (14–20MB RAM budget) and similar small devices, an overlay the user never opens shouldn't occupy any bytes. Lazy-global saved boot time but pays a permanent memory tax.
2. **Multi-instance becomes possible.** Nothing in the design forces one-instance-per-app. If two contexts ever want the same overlay shape with different data, dynamic allocation supports it; the global pattern doesn't.
3. **Teardown is simpler.** No `StaticPanelRegistry::register_destroy` dance at shutdown. The overlay destroys when popped, full stop.

This pattern isn't yet established in code — if you're writing a new overlay, you get to set the precedent. Flag it in your PR and Preston will help shape the exact API. Migrating the existing singleton overlays is tracked as post-1.0 work (see `ROADMAP.md` § "Planned post-1.0 refactoring").

---

## Part 2: Scaling up — how AMS is organized

The AMS (Automatic Material System) panel is what a full-subsystem contribution looks like. You won't write one of these on your first PR, but knowing how it's decomposed helps you read it when you need to touch adjacent code, and it's the shape to aim for if you're proposing something big.

AMS spans roughly 1700 lines of C++ (`src/ui/ui_panel_ams.cpp`) and 13 XML files (12 components plus a feature-tokens file, `ams_tokens.xml`). It's not monolithic — it's a set of small components that compose:

| XML component | Role |
|---|---|
| `ams_panel.xml` | Root panel — title bar, unit carousel, active tool display |
| `ams_unit_card.xml` | Rendering of a single AMS unit (e.g., one AMS Lite) |
| `ams_slot_view.xml` | One filament slot inside a unit |
| `ams_current_tool.xml` | "Currently loaded" display at the top |
| `ams_device_section_detail.xml` | Detail pane showing humidity, temp, etc. |
| `ams_device_operations.xml` | Action buttons (load, unload, purge) |
| `ams_edit_overlay.xml` | Edit filament assigned to a slot |
| `ams_context_menu.xml` | Long-press context menu |
| `ams_selector_menu.xml` | Selector context menu (switch active lane) |
| `ams_loading_error_modal.xml` | Error state dialog |
| `ams_environment_overlay.xml` | Humidity / temp details |
| `ams_overview_panel.xml` | Summary view for multi-unit setups |

Patterns worth internalizing from AMS:

### 1. One XML component per visual primitive

The unit card, slot, operations panel, and current tool are each their own `.xml` file with their own `<component>` block. This means they can be:

- Instantiated from multiple places (the current tool display reuses the same look as a slot).
- Styled and laid out independently at different breakpoints.
- Tested and iterated on without re-rendering the whole panel.

If you're composing a feature, **every distinct visual piece should be its own XML component**. The small cost of an extra file is paid back the first time you want to reuse or restyle it.

### 2. State lives in a backend class, not the panel

AMS state (which units exist, what filament is in each slot, humidity readings) lives in `AmsState` (`src/printer/ams_state.cpp`) — a singleton updated by the Moonraker WebSocket thread. The panel observes subjects published by `AmsState` and renders reactively. The panel never stores "the truth" — it only displays what the state says.

This separation is what lets the panel be destroyed and recreated without the app losing its AMS knowledge.

### 3. Dynamic collections rebuild their observers

AMS units can appear and disappear at runtime (plug in a second unit, a firmware update changes the layout). The panel keeps one `ObserverGuard` member per panel-lifetime subject, and for the per-slot subjects — one set per lane, alive only as long as that slot exists — a rebuildable vector (`include/ui_panel_ams.h`):

```cpp
// Panel-lifetime subjects: one named guard each
ObserverGuard slots_version_observer_;
ObserverGuard slot_count_observer_;

// Per-slot subjects: guards rebuilt whenever the slot count changes
std::vector<ObserverGuard> slot_path_observers_;
```

When the set of units changes, `slot_path_observers_` is `clear()`ed first, then re-populated with one `ObserverGuard` per live slot as the slot widgets are recreated (`ui_panel_ams.cpp` — `clear()` at both teardown and rebuild, `push_back` per slot). Guards are RAII, so dropping the old vector elements un-observes the dead slots automatically. This is the pattern for any per-item observer collection: **the observer's lifetime is the item widget's lifetime.**

One nuance: these particular subjects come from a static array owned by a singleton (`AmsState`), so they live for the whole app and need no `SubjectLifetime` token — the header comments say so explicitly. If your per-item subjects are owned by something shorter-lived (a backend, a panel), you must pair each observer with the owner's `SubjectLifetime` — see [THREADING.md](THREADING.md) and the `observer_factory.h` helpers.

### 4. Multi-backend abstraction

AMS isn't one system — it's AMS (Bambu), AFC (Box Turtle), Happy Hare, ACE (Anycubic), AD5X IFS, CFS (Creality), and Tool Changers, all surfaced through the same UI. The abstraction lives in `AmsState`, which normalizes all backends to a common shape. The panel knows only the normalized shape.

If you're adding a new filament backend, the pattern is: implement a backend class under `src/printer/` that publishes the same subjects `AmsState` expects. The existing panel picks it up for free. See `docs/devel/FILAMENT_MANAGEMENT.md` for the deep dive.

### 5. Modals and overlays are peers, not children

`ams_loading_error_modal.xml` and `ams_edit_overlay.xml` are standalone classes (`AmsLoadingErrorModal : Modal` and `AmsEditOverlay : OverlayBase` respectively). The main AMS panel launches them via `NavigationManager::push_overlay()` or `Modal::show()` — it doesn't own their widgets. This keeps each concern bounded.

---

## Part 3: The workflow

### Branch

Follow GitHub naming: `feature/<short-name>` for new features, `fix/<short-name>` for bugs.

```bash
git switch -c feature/my-contribution
```

For multi-file or risky changes, use a worktree — the threshold is in
[DEVELOPMENT.md](DEVELOPMENT.md) § "Worktrees":

```bash
scripts/setup-worktree.sh feature/my-contribution
```

### Build and test

```bash
make -j                              # build binary only
./build/bin/helix-screen --test -vv  # run with mock printer, verbose logs
make test-run                        # build and run the full test suite
```

For UI iteration without rebuilding after every XML edit — hot reload is ON by default for native builds, so just run:

```bash
./build/bin/helix-screen --test -vv
# edit XML → save → active panel rebuilds in place within ~500ms
```

Test at multiple breakpoints before submitting. At minimum: `-s 480x320`, `-s 800x480`, `-s 1024x600`. See `UI_CONTRIBUTOR_GUIDE.md` § Screen Breakpoints.

### Commit style

- Subject line: `type(scope): summary` — e.g., `feat(retraction): add settings overlay`.
- For bug fixes, cite the GitHub issue when one already exists: `fix(scope): thing (prestonbrown/helixscreen#123)`. Don't file an issue just to have one to cite — a commit without an issue reference is complete on its own.
- Keep commits focused. One logical change per commit.

### Before you submit

Work through `CONTRIBUTOR_GOTCHAS.md` § "Before You Submit". It's a pre-flight checklist for the silent-failure traps.

### PR

Open a PR on `prestonbrown/helixscreen` — that's how we coordinate changes, discuss tradeoffs, and keep a paper trail of decisions. Describe what the change does and why, and link any related issues. Screenshots of UI changes at multiple breakpoints go a long way.

---

## Further reading

Once the retraction overlay makes sense, these are the next docs to read in order:

| Read next | Why |
|---|---|
| `CONTRIBUTOR_GOTCHAS.md` | The "if you see X, you forgot Y" reference for silent failures |
| `UI_CONTRIBUTOR_GUIDE.md` | Deeper on breakpoints, tokens, semantic widgets |
| `LVGL9_XML_GUIDE.md` | Full XML attribute and widget reference |
| `MODAL_SYSTEM.md` | If your contribution is a modal rather than an overlay |
| `DEVELOPER_QUICK_REFERENCE.md` | Code patterns for specific scenarios |
| `TRANSLATION_SYSTEM.md` | When you add user-visible strings |

And if you hit a wall, the fastest debug is: find the closest-shaped sibling in `src/ui/` and diff your code against it.

## Next: pick your subsystem

You've seen one overlay end-to-end and toured a full subsystem. The rest of the
codebase is organized the same way — one subsystem per chapter, about an hour
each to read. Find yours:

→ **[architecture/README.md](architecture/README.md)** — the "I want to work
on..." index into the 15-chapter architecture guide.
