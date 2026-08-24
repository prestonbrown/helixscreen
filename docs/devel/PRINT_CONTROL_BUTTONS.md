# Print Control Buttons

**Version:** 1.0
**Last Updated:** 2026-06-13
**Status:** Shipped (controller extraction, 2026-06-11)

---

## Overview

`PrintControlButtons` is a singleton controller that centralizes the
pause/resume and stop button logic for an active print. It owns the LVGL
subjects those buttons bind to, drives them from a pure view function, observes
print state to keep them in sync, and runs an optimistic "pending action" state
machine so a tap is acknowledged instantly while the RPC is in flight.

The print-status panel used to own this logic inline. The extraction lets the
same buttons appear in two places — the full print-status panel and a 2×1 home
widget — both binding to the same global subjects with no duplicated state.

### Key files

| File | Contents |
|------|----------|
| `include/print_control_buttons.h`, `src/ui/print_control_buttons.cpp` | The controller: owned subjects, dispatch, observer, pending-action machine |
| `include/print_control_view.h`, `src/ui/print_control_view.cpp` | `compute_control_button_view()` — the pure, LVGL-free view function |
| `src/ui/panel_widgets/control_buttons_widget.{h,cpp}` | The 2×1 home widget (declarative, no state) |
| `ui_xml/components/panel_widget_control_buttons.xml` | The widget's XML layout + bindings |
| `src/ui/ui_panel_print_status.cpp` | Delegates paused-overlay visibility to the controller |

---

## Owned Subjects

The controller owns five global subjects, registered into the global XML scope
so any layout can bind to them by name:

| Subject | Type | Initial | Purpose |
|---------|------|---------|---------|
| `print_control_primary_icon` | string | pause MDI glyph | Icon for the primary (pause/resume) button |
| `print_control_primary_label` | string | `"Pause"` | Primary button label (English, run through `lv_tr()`) |
| `print_control_primary_enabled` | int | 0 | Enable gate for the primary button |
| `print_control_stop_enabled` | int | 0 | Enable gate for the stop button |
| `print_pending_action` | int | 0 | In-flight optimistic action (`PendingAction` cast to int) |

### Ownership & lifecycle

The subjects are created in `init_subjects()`, which runs during global subject
initialization (idempotent; guarded by `subjects_initialized_`). They are
static — printer state is an app-lifetime singleton — so the observer on
`print_state_enum` uses a bare `ObserverGuard` with **no** `SubjectLifetime`
token (`print_state_enum` is a global static subject, not a dynamic one).

Cleanup self-registers with `StaticSubjectRegistry` inside `init_subjects()`,
co-locating init and teardown:

```cpp
StaticSubjectRegistry::instance().register_deinit("PrintControlButtons", []() {
    auto& self = PrintControlButtons::instance();
    self.print_state_observer_.release();   // last pre-deinit cleanup → release(), not reset()
    if (self.pending_action_timeout_)
        lv_timer_delete(self.pending_action_timeout_);
    self.subjects_.deinit_all();
    self.subjects_initialized_ = false;
});
```

The two click callbacks (`on_print_control_primary`, `on_print_control_stop`)
are registered as XML event callbacks in the same init.

---

## The Pure View Function

All of the "what should the buttons look like" logic lives in a stateless
function with **no LVGL dependency** — which makes it trivially unit-testable
(`print_control_view.cpp`):

```cpp
enum class PendingAction : int { None = 0, Pausing = 1, Resuming = 2 };

struct ControlButtonView {
    const char* primary_icon  = CONTROL_ICON_PAUSE;  // MDI glyph pointer
    const char* primary_label = "Pause";            // English; caller runs lv_tr()
    bool        primary_enabled = false;
    bool        stop_enabled    = false;
    bool        stop_retires_preparing = false;      // Stop cancels the start, not the print
};

struct ControlButtonInputs {
    helix::PrintJobState job_state = helix::PrintJobState::STANDBY;
    PrintState lifecycle = PrintState::Idle;
    bool has_preparing_job = false;
    PendingAction pending = PendingAction::None;
    bool pause_available = false;
    bool resume_available = false;
    bool cancel_available = false;
};

ControlButtonView compute_control_button_view(const ControlButtonInputs& in);
```

The inputs are a struct rather than positional parameters because the row is
gated on **two** axes - the printer's job state and the UI lifecycle - and a
defaulted trailing bool is exactly the shape that silently drops a dimension at
one call site.

Logic:

```cpp
const bool printer_has_the_job = (in.job_state == PrintJobState::PRINTING ||
                                  in.job_state == PrintJobState::PAUSED);
const bool preparing = (in.lifecycle == PrintState::Preparing);

v.stop_retires_preparing = in.has_preparing_job && !printer_has_the_job;
v.stop_enabled = v.stop_retires_preparing || (printer_has_the_job && in.cancel_available);

const bool slot_ok = (in.job_state == PrintJobState::PAUSED) ? in.resume_available
                                                             : in.pause_available;
v.primary_enabled = printer_has_the_job && !preparing &&
                    in.pending == PendingAction::None && slot_ok;

switch (in.pending) {
case PendingAction::Pausing:  v.primary_icon = CONTROL_ICON_HOURGLASS; v.primary_label = "Pausing...";  break;
case PendingAction::Resuming: v.primary_icon = CONTROL_ICON_HOURGLASS; v.primary_label = "Resuming..."; break;
case PendingAction::None:
    if (in.job_state == PrintJobState::PAUSED) { v.primary_icon = CONTROL_ICON_PLAY;  v.primary_label = "Resume"; }
    else                                       { v.primary_icon = CONTROL_ICON_PAUSE; v.primary_label = "Pause";  }
    break;
}
```

### Why the lifecycle is an input (#798)

Affordance is a function of `PrintState` alone. It must not depend on whether
pre-print work runs **in front of** the job (a host-side forced bed mesh, where
`print_stats` still describes the previous job for minutes) or **inside**
Klipper's `PRINT_START` (where the job state already reads `printing`). The user
gets the same controls either way; only the mechanism differs, and that is hidden.

Keying on `job_state` alone could not express this, and the two architectures
diverged in opposite directions:

| | Host-side pre-print | Firmware-side pre-print |
|---|---|---|
| Job state | STANDBY / COMPLETE | PRINTING |
| **Pause, before** | disabled (incidentally) | **enabled - sent PAUSE mid-macro** |
| **Cancel, before** | **disabled - the retire path was unreachable** | enabled |
| Pause, now | disabled | disabled |
| Cancel, now | enabled, retires the preparing job | enabled, routes to `AbortManager` |

`stop_retires_preparing` is computed here rather than at the call site so the
button's enablement and the handler's routing cannot disagree about which
mechanism applies. `CANCEL_PRINT` sent to an idle printer is worse than useless:
`AbortManager` reads the terminal state as proof the cancel succeeded while the
queued `start_print` still fires.

Inputs → outputs at a glance:

- **Print state + pending action** decide icon and label.
- **Macro availability** (pause/resume/cancel configured?) gates the enable
  flags. A missing macro disables the relevant button.
- **A pending action disables the primary button** even when the macro is
  available — this prevents re-triggering while an RPC is in flight, and shows
  the hourglass + transitional label.

Icon constants: `CONTROL_ICON_PAUSE`, `CONTROL_ICON_PLAY`, `CONTROL_ICON_HOURGLASS`
(MDI glyphs, defined in `print_control_view.h`).

---

## Controller Wiring

### Observer on print_state_enum

```cpp
print_state_observer_ = observe_int_sync<PrintControlButtons>(
    get_printer_state().get_print_state_enum_subject(), this,
    [](PrintControlButtons* self, int) {
        if (self->pending_action_ != PendingAction::None)
            self->clear_pending_action();  // a real transition supersedes the optimistic state
        else
            self->recompute();             // macro availability may have changed
    });
```

`recompute()` reads the current print state and the `StandardMacros`
availability, calls `compute_control_button_view()`, and copies the result into
the five subjects (`lv_subject_copy_string` for icon/label, `lv_subject_set_int`
for the flags). It is the single place subjects are written.

### Dispatch

- **Primary button** (`handle_primary_button()`): if PRINTING, validates the
  `Pause` macro, calls `start_pending_action(Pausing)`, and executes the
  `Pause` macro with an error callback that clears the pending action on
  failure. If PAUSED, calls `request_resume()`, which runs the shared
  `dispatch_prepared_resume()` (prepare-for-resume → Resume chain) and clears
  pending on failure.
- **Stop button** (`handle_stop_button()`): shows a cancel-confirmation modal,
  then hands off to `AbortManager::start_abort()` on confirm. AbortManager owns
  its own progress UI.

---

## Optimistic Pending-Action State Machine

When the user taps pause or resume, the controller immediately shows an
hourglass + "Pausing…"/"Resuming…" rather than waiting for Klipper to confirm
the state change (which can take many seconds, especially resume on auto-feed
backends).

```cpp
void PrintControlButtons::start_pending_action(PendingAction action) {
    clear_pending_action();         // supersede any in-flight action
    pending_action_ = action;

    // Backstop timer; the print-state observer is the authoritative clear.
    // Pause is normally <2s but can stretch ~20s while moves drain; resume can
    // run 40-90s of AUTO_FEEDING (heat + feed + flush) on auto-feed backends.
    const uint32_t timeout_ms = (action == PendingAction::Resuming) ? 150000u : 25000u;
    pending_action_timeout_ = lv_timer_create(/* warn + clear_pending_action */, timeout_ms, this);
    lv_timer_set_repeat_count(pending_action_timeout_, 1);
    recompute();                    // publish hourglass state to subjects
}
```

- **Set** when a pause/resume is dispatched.
- **Cleared** authoritatively when the `print_state_enum` observer sees the real
  transition (PRINTING↔PAUSED).
- **Cleared on failure** via the dispatch error callback.
- **Backstop** — if Klipper never confirms, the timer fires a warning toast and
  clears (25 s for pause, 150 s for resume).

`pending_action_` is mirrored into the `print_pending_action` subject so other
UI (the print-status panel's paused overlay) can react to the optimistic state.

---

## The 2×1 Home Widget

`ControlButtonsWidget` (`control_buttons_widget.{h,cpp}`) is a `PanelWidget`
subclass that hosts the XML component `panel_widget_control_buttons` — two
buttons side by side. It is **pure declarative**: no instance subjects, no click
routing, no view logic. Everything binds to the controller's global subjects.

```cpp
class ControlButtonsWidget : public PanelWidget {
  public:
    void        attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void        detach() override;
    const char* id() const override { return "control_buttons"; }
  private:
    lv_obj_t* widget_obj_ = nullptr;
};

void register_control_buttons_widget() {
    register_widget_factory("control_buttons",
        [](const std::string&) { return std::make_unique<ControlButtonsWidget>(); });
    // No init_subjects here — the print_control_* subjects are owned by PrintControlButtons.
}
```

XML bindings (`panel_widget_control_buttons.xml`):

- Primary button: `bind_icon="print_control_primary_icon"`,
  `bind_text="print_control_primary_label"`,
  `bind_state_if_eq subject="print_control_primary_enabled" state="disabled" ref_value="0"`,
  `event_cb clicked → on_print_control_primary`.
- Stop button: `variant="danger"`,
  `bind_state_if_eq subject="print_control_stop_enabled" state="disabled" ref_value="0"`,
  `event_cb clicked → on_print_control_stop`.

---

## Print-Status Panel Delegation

`PrintStatusPanel` no longer manages the pause/resume icon, label, or enable
state — those are owned by the controller. The panel observes the controller's
`print_pending_action` subject to drive its paused-overlay visibility
optimistically:

```cpp
pending_action_observer_ = observe_int_sync<PrintStatusPanel>(
    helix::ui::PrintControlButtons::instance().pending_action_subject(), this,
    [](PrintStatusPanel* self, int) { self->recompute_paused_overlay_visibility(); });
```

`recompute_paused_overlay_visibility()` blends the real print state with the
pending action: it shows the paused overlay while `Pausing` (even though the
state is still PRINTING) and hides it while `Resuming` (even though the state is
still PAUSED), so the tap feels instantly acknowledged.

---

## Testing

| Test file | Tags | Covers |
|-----------|------|--------|
| `tests/unit/test_print_control_view.cpp` | `[print_control_view]` | The pure view function — printing→Pause, paused→Resume(play), idle→both disabled, pending→hourglass+disabled, missing-macro→disabled. No LVGL fixture. |
| `tests/unit/test_print_control_buttons.cpp` | `[print_control][slow]` | Controller integration — subject registration, label tracks state, macro-gated enable, pending publishes hourglass, pending cleared on state arrival. |
| `tests/unit/test_print_controls_char.cpp` | `[characterization][controls][...]` | Characterization of the other print controls (light, timelapse, tune/Z-offset). |

```bash
./build/bin/helix-tests "[print_control_view]"   # fast, no LVGL
./build/bin/helix-tests "[print_control]"
```

The pure-view tests are the primary safety net: button behavior is a pure
function of (state, pending, macro availability), so the full truth table is
asserted without a display.

---

## References

- `docs/devel/PRINT_STATE_MACHINE.md` — print lifecycle states the controller observes
- `docs/devel/MACROS_PANEL.md`, `docs/devel/STANDARD_MACROS_SPEC.md` — `StandardMacros` slots that gate the buttons
