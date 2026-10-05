// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_sensor_widget.h"

#include "ui_event_safety.h"
#include "ui_resume_dispatch.h"
#include "ui_settings_sensors.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "filament_sensor_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "print_control_buttons.h"
#include "printer_state.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix {

namespace {
/// Owns the XML-published static subjects shared by every FilamentSensorWidget.
SubjectManager s_static_subjects;
} // namespace

void register_filament_sensor_widget() {
    FilamentSensorWidget::init_static_subjects();
    register_widget_subjects("filament", FilamentSensorWidget::init_static_subjects);

    register_widget_factory(
        "filament", [](const std::string&) { return std::make_unique<FilamentSensorWidget>(); });

    lv_xml_register_event_cb(nullptr, "filament_widget_clicked_cb",
                             FilamentSensorWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "on_filament_source_selected",
                             FilamentSensorWidget::on_filament_source_selected);
}

// filament_source_picker.xml's four rows are written out with fixed
// value="0..3" attributes rather than generated from this enum, so a reordered
// FilamentTileSource would silently point every row's check icon and every tap
// at the wrong role. This breaks the build instead.
static_assert(static_cast<int>(helix::ui::FilamentTileSource::Auto) == 0 &&
                  static_cast<int>(helix::ui::FilamentTileSource::Runout) == 1 &&
                  static_cast<int>(helix::ui::FilamentTileSource::Toolhead) == 2 &&
                  static_cast<int>(helix::ui::FilamentTileSource::Entry) == 3,
              "FilamentTileSource enum order must match filament_source_picker.xml's row values");

void FilamentSensorWidget::init_static_subjects() {
    if (subjects_initialized_) {
        return;
    }
    subjects_initialized_ = true;

    // -1 = no sensor, matching FilamentSensorManager's encoding, so the tile
    // reads "hidden" until the first real value arrives rather than "empty".
    lv_subject_init_int(&tile_state_subject_, -1);
    lv_subject_init_int(&source_subject_, static_cast<int>(ui::FilamentTileSource::Auto));

    // Global scope (not the "panel_widget_filament" component scope): the
    // mirror is also read as a `subject`-type prop by the nested
    // <filament_sensor_indicator> component, which resolves that prop via
    // lv_xml_get_subject() against its OWN scope, falling back only to
    // "globals" - never to the parent component's private scope. A
    // component-scoped registration here left that lookup unresolved (8x "No
    // subject was found" warnings) despite the same-component label bindings
    // resolving it fine, since those short-circuit against the local scope
    // before ever needing the fallback. lv_xml_register_subject(nullptr, ...)
    // routes to "globals" (lib/helix-xml/src/xml/lv_xml.c:689).
    s_static_subjects.publish("filament_tile_state", &tile_state_subject_);

    // Same reasoning, same fallback trap: the check icons that bind this live in
    // filament_source_row, a different component from filament_source_picker, and
    // lv_xml_get_subject() only falls back from a component scope to "globals" -
    // never into another component's private scope
    // (lib/helix-xml/src/xml/lv_xml.c:689,757-778). Register globally or the
    // check icons silently never resolve it.
    s_static_subjects.publish("filament_tile_source", &source_subject_);

    StaticSubjectRegistry::instance().register_deinit("FilamentSensorWidget", []() {
        if (!subjects_initialized_) {
            return;
        }
        s_static_subjects.deinit_all();
        subjects_initialized_ = false;
    });
}

FilamentSensorWidget::FilamentSensorWidget()
    : TiledPanelWidget("filament", TileSizing::Content{"", "", "Loaded", false}) {}

FilamentSensorWidget::~FilamentSensorWidget() {
    detach();
}

void FilamentSensorWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Instances are recycled across grid rebuilds, so the source binding must be
    // (re)established here, not only in set_config().
    rebind_source();
}

void FilamentSensorWidget::detach() {
    source_observer_.reset();
    // A grid rebuild detaches this instance while the tap modal may still be up.
    // invalidate() below expires every callback the dialog holds, so leaving it
    // open would leave a dialog whose buttons all silently no-op — and whose
    // parent_screen_ is about to be torn down under it.
    tap_modal_.hide();
    lifetime_.invalidate();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void FilamentSensorWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    if (config.contains("source") && config["source"].is_string()) {
        source_ = ui::parse_tile_source(config["source"].get<std::string>());
    }
    spdlog::debug("[FilamentSensorWidget] Config: source={}", ui::tile_source_to_string(source_));
    rebind_source();
}

bool FilamentSensorWidget::on_edit_configure() {
    spdlog::info("[FilamentSensorWidget] Configure requested - showing source picker");
    if (source_picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return false;
    }
    lv_subject_set_int(&source_subject_, static_cast<int>(source_));
    source_picker_.show_below_widget(parent_screen_, widget_obj_,
                                     helix::ui::ContextMenu::AnchorAlign::Center);
    return false; // no rebuild - the picker saves on selection
}

void FilamentSensorWidget::on_filament_source_selected(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentSensorWidget] on_filament_source_selected");
    auto* picker = helix::ui::ContextMenu::active_as<SourcePicker>();
    if (!picker) {
        spdlog::warn("[FilamentSensorWidget] No active source picker for row click");
        return;
    }

    // user_data comes off the XML engine as a heap C-string (lv_strdup of the
    // resolved "$value" attribute, e.g. "2"), never an int - confirmed against
    // ui_panel_calibration_zoffset.cpp's identical user_data="0.005" handling.
    // Logged once here rather than asserted, since a bad row value degrades to
    // Auto instead of crashing.
    const char* value_str = static_cast<const char*>(lv_event_get_user_data(e));
    spdlog::debug("[FilamentSensorWidget] source row user_data raw='{}'",
                  value_str ? value_str : "(null)");
    const int value = value_str ? std::atoi(value_str) : 0;

    FilamentSensorWidget& self = picker->owner();
    self.source_ = static_cast<ui::FilamentTileSource>(value);
    self.config_["source"] = ui::tile_source_to_string(self.source_);
    self.save_widget_config(self.config_);
    self.rebind_source();
    lv_subject_set_int(&source_subject_, value);
    picker->hide();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentSensorWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentSensorWidget] clicked_cb");
    // Sole registration is the XML <event_cb> on the tile's root, which LVGL's
    // XML engine dispatches with user_data=NULL - recover the instance via
    // widget_obj_'s own user_data (set in attach()) instead. No parallel
    // lv_obj_add_event_cb() here: the root has no clickable child to target,
    // unlike MotionWidget's motion_button, so a second registration on the
    // same object would only double-dispatch every tap.
    auto* self = panel_widget_from_event<FilamentSensorWidget>(e);
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentSensorWidget::handle_click() {
    const int sensor_state = lv_subject_get_int(&tile_state_subject_);
    // Lifecycle, not the raw wire enum (check_raw_print_job_state). The tap
    // policy only asks whether a print is running right now; Paused keeps the
    // full modal on purpose (Load/Unload are safe then), so this is exactly
    // Printing rather than job_holds_machine().
    const bool printing = get_printer_state().get_print_lifecycle() == PrintState::Printing;

    switch (ui::decide_tap_destination(sensor_state, printing ? 1 : 0)) {
    case ui::FilamentTapDestination::None:
        spdlog::debug("[FilamentSensorWidget] Tap ignored - no sensor configured");
        return;
    case ui::FilamentTapDestination::SensorSettings:
        open_sensor_settings();
        return;
    case ui::FilamentTapDestination::ModalStatusOnly:
        show_tap_modal(/*status_only=*/true);
        return;
    case ui::FilamentTapDestination::ModalFull:
        show_tap_modal(/*status_only=*/false);
        return;
    }
}

void FilamentSensorWidget::open_sensor_settings() {
    if (!parent_screen_) {
        return;
    }
    helix::settings::get_sensor_settings_overlay().show(parent_screen_);
}

void FilamentSensorWidget::show_tap_modal(bool status_only) {
    if (!parent_screen_) {
        return;
    }
    // A deliberate tap is not a warning. Every show site states its own value —
    // see RunoutGuidanceModal::set_advisory() for why inheriting is a bug.
    tap_modal_.set_advisory(true);

    // Override the runout copy. `title` and `message` are <prop>s on
    // runout_guidance_modal.xml, and Modal::show() forwards XML attrs, so the
    // runout path's defaults are untouched. NULL-terminated, as show() expects.
    // The manual row is hidden mid-print by the XML gate, so the default
    // "What would you like to do?" copy - and a plain "Load or unload" - both
    // read as offers the dialog is not making. Say what is actually true.
    //
    // These go through lv_tr() here rather than being passed as raw tags for the
    // component's translation_tag="$title" to resolve, and that is deliberate.
    // The C++ extractor (scripts/translations/extractor.py,
    // CPP_TRANSLATABLE_PATTERNS) only scans lv_tr(), lv_label_set_text() and
    // `return "Xx…"` - a raw literal handed to an XML attribute is invisible to
    // it, so dropping lv_tr() would delete all three strings from
    // translations/*.yml on the next regeneration. It also matches the two other
    // C++ callers that fill a modal's title/message prop
    // (ui_print_exclude_object_manager.cpp, modal_show_confirmation()). The one
    // cost is that translation_tag then stores an already-translated string, so a
    // language switch while this dialog is open would not re-resolve it - and
    // that is unreachable, since changing language means leaving the home screen
    // this modal sits on.
    const char* message = status_only ? lv_tr("Filament controls are unavailable while printing.")
                                      : lv_tr("Load or unload filament.");
    const char* attrs[] = {"title", lv_tr("Filament"), "message", message, nullptr};

    if (!status_only) {
        // RunoutGuidanceModal retains these callbacks past dismissal (its own
        // header says so) and this widget is recycled across grid rebuilds, so
        // a bare `this` would be a use-after-free the next time the modal fires.
        // Main-thread only (button press) — a plain expired() check is correct
        // here; the L081 ban on bare expired() is for background threads.
        auto token = lifetime_.token();
        tap_modal_.set_on_load_filament([this, token]() {
            if (token.expired()) {
                return;
            }
            dispatch_load();
        });
        tap_modal_.set_on_unload_filament([this, token]() {
            if (token.expired()) {
                return;
            }
            dispatch_unload();
        });
        tap_modal_.set_on_purge([this, token]() {
            if (token.expired()) {
                return;
            }
            dispatch_purge();
        });

        // Paused (print_state_enum == 2) is a ModalFull destination, and at that
        // state runout_guidance_modal.xml hides the Close row and shows the
        // Cancel Print / Resume Print row instead. on_cancel()/on_tertiary()
        // null-check the callback and then hide() either way, so leaving these
        // unset gives the user a live "Resume Print" button that closes the
        // dialog and leaves the print paused — indistinguishable from success.
        // Both route through the same shared dispatches the runout guidance
        // dialog uses, so the two surfaces cannot drift.
        tap_modal_.set_on_resume([token]() {
            if (token.expired()) {
                return;
            }
            spdlog::info("[FilamentSensorWidget] User chose to resume print from the sensor tile");
            // Same path as the panel's primary Resume button and the runout
            // dialog's: pending-action UI, macro check, prepare_for_resume chain.
            helix::ui::PrintControlButtons::instance().request_resume();
        });
        tap_modal_.set_on_cancel_print([this, token]() {
            if (token.expired()) {
                return;
            }
            spdlog::info("[FilamentSensorWidget] User chose to cancel print from the sensor tile");
            // Raises a confirmation and sends nothing until it is accepted; the
            // dialog lives in dispatch_cancel_print() so this surface and the
            // runout guidance dialog cannot diverge on a destructive action.
            // This modal stays up behind it — on_tertiary() no longer hides, so
            // declining returns here — and closes only once the cancel is real.
            helix::ui::dispatch_cancel_print(get_moonraker_api(), "[FilamentSensorWidget]",
                                             [this, token]() {
                                                 if (token.expired()) {
                                                     return;
                                                 }
                                                 tap_modal_.hide();
                                             });
        });
    } else {
        // Status-only: the manual row is hidden by XML and the paused row is
        // hidden too (this destination is only chosen while PRINTING). Clear
        // rather than leave whatever a previous full show installed — the modal
        // instance is a member and retains its callbacks until overwritten, so
        // "unset" is only true on the very first show.
        tap_modal_.set_on_load_filament(RunoutGuidanceModal::Callback{});
        tap_modal_.set_on_unload_filament(RunoutGuidanceModal::Callback{});
        tap_modal_.set_on_purge(RunoutGuidanceModal::Callback{});
        tap_modal_.set_on_resume(RunoutGuidanceModal::Callback{});
        tap_modal_.set_on_cancel_print(RunoutGuidanceModal::Callback{});
    }

    tap_modal_.show(parent_screen_, attrs);
}

void FilamentSensorWidget::dispatch_load() {
    AmsBackend* backend = AmsState::instance().get_backend();
    // No slot picker on this tile either — mirrors PrintStatusWidget::dispatch_load().
    const int slot = backend ? backend->get_current_slot() : -1;
    helix::ui::execute_filament_load(backend, slot, "[FilamentSensorWidget]");
}

void FilamentSensorWidget::dispatch_unload() {
    AmsBackend* backend = AmsState::instance().get_backend();
    const int slot = backend ? backend->get_current_slot() : -1;

    // unload_target_is_loaded()'s is_current_slot arm is what makes this
    // reachable during a runout: the lane's own sensor clears while filament
    // is still at the head (#995), and #1199 deliberately keeps Unload
    // reachable there. Do not answer "is it loaded" any other way here - see
    // the doc comment on unload_target_is_loaded() for why that divergence is
    // exactly what it exists to prevent.
    bool loaded = false;
    if (backend) {
        loaded = helix::ui::unload_target_is_loaded(
            slot, backend->slot_is_actively_loaded(slot),
            backend->slot_has_filament_at_toolhead(slot),
            /*is_current_slot=*/true, backend->get_system_info().filament_loaded,
            backend->toolhead_filament_unaccounted().value_or(false));
    }

    helix::ui::execute_filament_unload(backend, slot, loaded, "[FilamentSensorWidget]");
}

void FilamentSensorWidget::dispatch_purge() {
    helix::ui::execute_filament_purge("[FilamentSensorWidget]");
}

void FilamentSensorWidget::rebind_source() {
    // Reset first: attach() runs again on every grid rebuild, and set_config()
    // can fire after attach(), so without this the observers stack up.
    source_observer_.reset();

    lv_subject_t* src = lv_xml_get_subject(nullptr, ui::tile_source_subject(source_));
    if (!src) {
        spdlog::warn("[FilamentSensorWidget] Subject '{}' not found - tile will not update",
                     ui::tile_source_subject(source_));
        return;
    }

    // Mirror the selected role subject into the one the XML binds. A runtime
    // source mux, not a compound condition, so <subject_expr> cannot express it.
    // DECLARATIVE_OK: the selection is user config, not a static relationship.
    source_observer_ = helix::ui::observe<int>(
        src, this,
        [](FilamentSensorWidget* self, int value) {
            (void)self;
            lv_subject_set_int(&tile_state_subject_, value);
        },
        FilamentSensorManager::instance().get_subjects_lifetime());
}

} // namespace helix
