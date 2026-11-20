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

| File | Purpose |
|---|---|
| `ui_xml/retraction_settings_overlay.xml` | Layout, styling, bindings |
| `include/ui_overlay_retraction_settings.h` | Class declaration, subject + widget references |
| `src/ui/settings/ui_overlay_retraction_settings.cpp` | Lifecycle, event handlers, G-code send |

You'll also touch other files to wire it into the app:

- `src/application/subject_initializer.cpp` — hand the overlay its API and run `init_subjects()` at boot
- `ui_xml/settings_printing_overlay.xml` — add a row that opens the overlay
- `src/ui/settings/ui_settings_printing.cpp` — the row's click callback, which calls `show()`
- (optional) translation YAML if you added user-visible strings

Let's walk through each.

### Step 1: The XML layout (`ui_xml/retraction_settings_overlay.xml`)

The top of every overlay XML looks like this:

```xml
<component>
  <view name="retraction_settings_overlay"
        extends="overlay_panel"
        title="Retraction Settings"
        bg_color="#screen_bg">
    <overlay_panel-overlay_content style_pad_all="#space_lg" scrollable="true"
                                   flex_flow="column" style_pad_gap="#space_lg">
      <!-- sections go here -->
    </overlay_panel-overlay_content>
  </view>
</component>
```

Notes:

- **`extends="overlay_panel"`** — inherits the standard overlay chrome: title bar, back button, backdrop. You almost always want this.
- **`title="..."`** — the English text, which is also its own translation key (see below).
- **No `width`** — overlay width is not yours to set. `NavigationManager::push_overlay()` decides it from how the user reached the overlay: full width inside Settings, gapped when it is a tool opened over something else. See `include/overlay_class.h`.
- **`#space_lg`, `#screen_bg`** — design tokens. Never hardcode pixel spacing or hex colors.
- **`<overlay_panel-overlay_content>`** — fills `overlay_panel`'s content slot, the area under the header that grows to the bottom. The base owns that object; the tag's attributes give it the layout, padding and scrolling this screen wants, and its children are your content. Widgets written straight in the view land there too. Buttons pinned under the content go in `<overlay_panel-overlay_footer>`.

Inside the content area, each setting is structured consistently (abbreviated from the real file):

```xml
<lv_obj name="retract_length_section"
        width="100%" height="content" style_pad_all="0"
        scrollable="false" flex_flow="column" style_pad_gap="#space_sm">

  <!-- Top row: label on left, current value on right -->
  <lv_obj width="100%" height="content" style_pad_all="0"
          flex_flow="row" style_flex_main_place="space_between"
          style_flex_cross_place="center">
    <lv_obj height="content" flex_flow="column" style_pad_gap="#space_xxs">
      <text_body text="Retract Length"/>
      <text_small text="0.4-2mm direct drive, 4-6mm bowden"/>
    </lv_obj>
    <setting_value_field name="retract_length_field"
                         value_subject="retract_length_display"
                         callback="on_retraction_field_clicked" user_data="0"/>
  </lv_obj>

  <!-- Slider row -->
  <lv_obj width="100%" height="#input_height" flex_flow="row" style_flex_cross_place="center">
    <text_small width="30" text="0"/>
    <lv_slider name="retract_length_slider" flex_grow="1"
               min_value="0" max_value="600" value="80">
      <event_cb trigger="value_changed" callback="on_retraction_setting_changed"/>
    </lv_slider>
    <text_small width="40" text="6mm"/>
  </lv_obj>
</lv_obj>
<divider_horizontal/>
```

Things worth noticing:

- **`value_subject="retract_length_display"`** — the value field's text comes from a subject named `retract_length_display`. The C++ side owns that subject and writes to it when the slider moves. Tapping the field calls `on_retraction_field_clicked` and opens a numeric keypad.
- **`<event_cb trigger="value_changed" callback="on_retraction_setting_changed"/>`** — when the slider value changes, call the C++ function registered under the name `on_retraction_setting_changed`. **No `lv_obj_add_event_cb()` in C++ — ever.**
- **Callback naming** — the convention is `on_<component>_<action>`. Generic names like `on_changed` will collide. (See lesson **L039**.)
- **A literal `text=` is translated as written.** The text is its own translation key, so no `translation_tag` is needed; write one only when the key differs from the text. (See `TRANSLATION_SYSTEM.md`.)
- **`<divider_horizontal/>`** — a semantic widget that applies the standard divider style. Don't roll your own with `lv_obj` + `border_width`.

### Step 2: The class declaration (`include/ui_overlay_retraction_settings.h`)

The header declares a class that inherits `OverlayBase` (`include/overlay_base.h`):

```cpp
class RetractionSettingsOverlay : public OverlayBase {
  public:
    explicit RetractionSettingsOverlay(helix::IMoonrakerAPI* api = nullptr);
    ~RetractionSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;     // builds the tree, caches widgets
    const char* get_name() const override { return "Retraction Settings"; }
    const char* xml_component() const override { return "retraction_settings_overlay"; }
    bool destroy_on_close() const override { return true; }
    void on_activate() override;

  private:
    void send_retraction_settings();
    void update_display_labels();
    void sync_from_printer_state();

    // Widget pointers: cached in create(), cleared in on_ui_destroyed()
    lv_obj_t* enable_switch_ = nullptr;
    lv_obj_t* retract_length_slider_ = nullptr;
    // ... etc

    SubjectManager subjects_;                // RAII cleanup for the subjects below
    helix::Subject retract_length_display_;  // label text, written by C++
    char retract_length_buf_[16];            // backing buffer for the string subject
    // ... etc

    helix::IMoonrakerAPI* api_ = nullptr;
    bool syncing_from_state_ = false;
};

// Process-lifetime accessor; the object is built on first use.
inline RetractionSettingsOverlay& get_global_retraction_settings() {
    return helix::lazy_global<RetractionSettingsOverlay>("RetractionSettingsOverlay", nullptr);
}
```

Patterns to internalize:

- **Inherit `OverlayBase`.** It provides `lifetime_` (AsyncLifetimeGuard), `overlay_root_`, the lifecycle hooks and `show()`. Don't reinvent overlay chrome. A behaviour-free overlay is just a name and an XML component.
- **Cache widget pointers in `create()`.** Don't re-lookup in event handlers. With `destroy_on_close()` the tree is freed whenever the overlay is popped, so `on_ui_destroyed()` must drop every cached pointer.
- **`SubjectManager subjects_`** is the RAII wrapper that deinitializes the subjects on destruction.
- **Handlers reach the instance through the accessor.** LVGL callbacks are C function pointers, so the registered lambdas call `get_global_retraction_settings()`. The object lives for the process, but its widget tree exists only while the overlay is open, so an overlay the user never opens costs no widgets.

### Step 3: The implementation (`src/ui/settings/ui_overlay_retraction_settings.cpp`)

The interesting parts:

**3a. Subjects (`init_subjects()`)**

```cpp
void RetractionSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(retract_length_display_, retract_length_buf_, "0.00mm",
                                  "retract_length_display", subjects_);
        // ... same for the other three display subjects ...
    });
}
```

`UI_MANAGED_SUBJECT_STRING` creates a string subject, registers it globally under the given name, and hands it to `subjects_` for cleanup. The XML's `value_subject="retract_length_display"` resolves to it. `init_subjects()` runs before the XML is created, so bindings resolve.

**3b. Callbacks (`register_callbacks()`)**

```cpp
void RetractionSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_retraction_setting_changed",
         [](lv_event_t*) {
             auto& overlay = get_global_retraction_settings();
             overlay.update_display_labels();
             if (!overlay.syncing_from_state_) {
                 overlay.send_retraction_settings();
             }
         }},
        // ... enabled switch, tappable value fields ...
    });
}
```

Every `callback="..."` string in the XML must be registered by name before the XML is parsed. A name that does not match a registered one makes the event silently do nothing, a common first-contribution trap.

**3c. Widget creation (`create()`)**

```cpp
lv_obj_t* RetractionSettingsOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, xml_component())) {
        return nullptr;
    }
    enable_switch_ = helix::ui::find_required(overlay_root_, "retraction_enabled_switch", get_name());
    retract_length_slider_ = helix::ui::find_required(overlay_root_, "retract_length_slider", get_name());
    // ... etc
    return overlay_root_;
}
```

`create_overlay_from_xml()` parses your XML component into a real widget tree. Then `find_required()` (a name lookup that logs when the widget is missing) locates the widgets C++ must read or set. The `name="..."` attributes in your XML are the contract.

**Never use `lv_obj_get_child()`** — it's index-based and fragile. If someone reorders the XML, your code breaks silently.

**3d. The lifecycle hooks**

```cpp
void RetractionSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    if (returning_from_keypad_) { returning_from_keypad_ = false; return; }
    sync_from_printer_state();
}
```

`on_activate()` runs every time the overlay becomes visible. For a settings overlay this is where you pull the current values from `PrinterState` and seed the UI (here, except when returning from the overlay's own numeric keypad, which would discard the value just typed). `on_deactivating(reason)` runs when it is dismissed; use it to stop timers and polling.

**3e. The feedback-loop guard**

`sync_from_printer_state()` sets `syncing_from_state_ = true` around its `lv_slider_set_value()` calls. Without the flag, each of those would fire `value_changed`, which would send a G-code, which the printer would echo back, which would set the sliders again: a feedback loop. Flag-guard any handler whose widget you also write programmatically.

### Step 4: Wire it into the app

The XML component itself needs no registration: `create_overlay_from_xml()` loads `ui_xml/retraction_settings_overlay.xml` on first use.

**4a. Give it its API and subjects at boot** — `src/application/subject_initializer.cpp`:

```cpp
init_global_retraction_settings(api);
get_global_retraction_settings().init_subjects();
```

Every overlay's setup is called here during app startup; follow the pattern of the siblings.

**4b. Add the row that opens it** — `ui_xml/settings_printing_overlay.xml`:

```xml
<lv_obj name="container_firmware_retraction" width="100%" style_pad_all="0" scrollable="false">
  <bind_flag_if_eq subject="printer_has_firmware_retraction" flag="hidden" ref_value="0"/>
  <setting_action_row name="row_firmware_retraction"
                      label="Retraction Settings" icon="arrow_up_right"
                      description="Configure G10/G11 firmware retraction"
                      callback="on_retraction_row_clicked"/>
</lv_obj>
```

The `<bind_flag_if_eq>` wrapper hides the row on printers that don't support firmware retraction: this is how you conditionally expose features without a runtime C++ check.

**4c. Open it from the row's callback** — `src/ui/settings/ui_settings_printing.cpp`:

```cpp
{"on_retraction_row_clicked",
 [](lv_event_t*) {
     get_global_retraction_settings().show(get_printing_settings_overlay().parent_screen_);
 }},
```

`OverlayBase::show()` builds the tree if needed, registers the lifecycle and pushes the overlay through `NavigationManager`; with `destroy_on_close()` it also registers the callback that frees the tree on close.

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

src/ui/settings/
  ui_overlay_retraction_settings.cpp     ← your impl
  ui_settings_printing.cpp               ← edit to add the row's callback

src/
  application/subject_initializer.cpp    ← edit to call init_global_*()
```

That's the full shape of a real settings overlay contribution. If you're stuck, find the closest sibling `ui_overlay_*.cpp` or `ui_settings_*.cpp`, open its files side by side with these, and diff the structure.

---

## Part 2: Scaling up — how AMS is organized

The AMS (Automatic Material System) panel is what a full-subsystem contribution looks like. You won't write one of these on your first PR, but knowing how it's decomposed helps you read it when you need to touch adjacent code, and it's the shape to aim for if you're proposing something big.

AMS is a large C++ panel (`src/ui/filament/ui_panel_ams.cpp`) plus a family of `ui_xml/ams_*.xml` files and a feature-tokens file, `ams_tokens.xml`. It's not monolithic — it's a set of small components that compose:

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

One nuance: these particular subjects come from a static array owned by a singleton (`AmsState`), so they live for the whole app and need no `SubjectLifetime` token — the header comments say so explicitly. If your per-item subjects are owned by something shorter-lived (a backend, a panel), you must pair each observer with the owner's `SubjectLifetime` — see [THREADING.md](THREADING.md) and the `core/observer_factory.h` helpers.

### 4. Multi-backend abstraction

AMS isn't one system — it's AMS (Bambu), AFC (Box Turtle), Happy Hare, ACE (Anycubic), AD5X IFS, CFS (Creality), and Tool Changers, all surfaced through the same UI. The abstraction lives in `AmsState`, which normalizes all backends to a common shape. The panel knows only the normalized shape.

If you're adding a new filament backend, the pattern is: implement a backend class under `src/printer/` that publishes the same subjects `AmsState` expects. The existing panel picks it up for free. See `docs/devel/FILAMENT_MANAGEMENT.md` for the deep dive.

### 5. Modals and overlays are peers, not children

`ams_loading_error_modal.xml` and `ams_edit_overlay.xml` are standalone classes (`AmsLoadingErrorModal : Modal` and `AmsEditOverlay : OverlayBase` respectively). The main AMS panel launches them through `OverlayBase::show()` and `Modal` (`ComponentModal::show_owned()` for a plain component) — it doesn't own their widgets. This keeps each concern bounded.

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
make t F='[tag]'                     # build, then run the tests for one area
make full-test-run                   # C++ suite plus the bats shell suite
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
on..." index into the 17-chapter architecture guide.
