// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "subject_initializer.h"

#include "ui_component_keypad.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_fan_control_overlay.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_notification_manager.h"
#include "ui_overlay_console_settings.h"
#include "ui_overlay_printer_image.h"
#include "ui_overlay_retraction_settings.h"
#include "ui_overlay_timelapse_install.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_overlay_timelapse_videos.h"
#include "ui_panel_advanced.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_console.h"
#include "ui_panel_controls.h"
#include "ui_panel_filament.h"
#include "ui_panel_history_dashboard.h"
#include "ui_panel_history_list.h"
#include "ui_panel_home.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_motion.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"
#include "ui_panel_screws_tilt.h"
#include "ui_panel_settings.h"
#include "ui_panel_spoolman.h"
#include "ui_printer_status_icon.h"
#include "ui_probe_overlay.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "abort_manager.h"
#include "accel_sensor_manager.h"
#include "active_print_media_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "color_sensor_manager.h"
#include "filament_catalog.h"
#include "filament_op_router.h"
#include "filament_sensor_manager.h"
#include "filament_variants.h"
#include "humidity_sensor_manager.h"
#include "led/ui_led_control_overlay.h"
#include "lock_manager.h"
#include "lvgl/lvgl.h"
#include "material_settings_manager.h"
#include "panel_widget_manager.h"
#include "plr_offer_controller.h"
#include "preset_materials.h"
#include "print_completion.h"
#include "print_control_buttons.h"
#include "print_start_navigation.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "standard_macros.h"
#include "system/telemetry_manager.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "temperature_service.h"
#include "timelapse_state.h"
#include "tool_state.h"
#include "usb_manager.h"
#include "width_sensor_manager.h"
#include "xml_registration.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace {

// The one live installation of helix::ui::HomeConfirmPrompter (Task 8). Every
// other caller of set_home_confirm_prompter() is a test.
//
// Built on the Modal INSTANCE API (helix::ui::Modal subclass), not the static
// modal_show_confirmation() factory used by the first cut of this code. The
// static factory's dialog is created via the plain Modal::show("modal_dialog", ...)
// path, and on that path Modal::show() unconditionally wires the backdrop-tap
// and ESC handlers with a null Modal* user_data (ui_modal.cpp's
// backdrop_click_cb/esc_key_cb "static modal" branch) -- both just call
// Modal::hide(dialog) directly and never touch the confirm/cancel
// lv_event_cb_t at all. Since AmsBackendAfc/AmsBackendToolChanger arm their
// optimistic AmsAction *before* calling ensure_homed_then(), and
// ensure_homed_then()'s unhomed branch now always returns success() once it
// decides to prompt (no more synchronous failure return for `!result` to
// catch), a backdrop-tap or ESC dismissal that never resolves either callback
// left those two backends permanently stuck busy -- ToolChanger has no
// stuck-action watchdog at all, so that was an unrecoverable lockout short of
// an app restart.
//
// The instance API's on_hide() hook, by contrast, fires on every exit path:
// Ok/Cancel buttons and ESC all route through on_ok()/on_cancel() (which
// default to hide()), and backdrop-tap calls hide() directly. HomeConfirmModal
// uses on_hide() as a fallback net -- resolve() below is idempotent, so
// whichever path got there first (button, ESC, or the backdrop-tap fallback)
// is the only one that fires a callback.
class HomeConfirmModal : public Modal {
  public:
    HomeConfirmModal(std::function<void()> on_confirm, std::function<void()> on_cancel)
        : on_confirm_(std::move(on_confirm)), on_cancel_(std::move(on_cancel)) {}

    const char* get_name() const override {
        return "HomeConfirm"; // i18n: do not translate, internal modal identifier
    }
    const char* component_name() const override {
        return "modal_dialog";
    }

  protected:
    void on_show() override {
        wire_ok_button();
        wire_cancel_button();
    }

    void on_ok() override {
        resolve(on_confirm_);
        Modal::on_ok(); // hides
    }

    void on_cancel() override {
        resolve(on_cancel_);
        Modal::on_cancel(); // hides
    }

    void on_hide() override {
        // Fallback net: only fires if neither on_ok() nor on_cancel() already
        // resolved this dialog -- i.e. it was dismissed by backdrop-tap (which
        // calls hide() directly, bypassing on_cancel()) or any other exit that
        // skips both. Every dismissal must land the backend at IDLE the same
        // way the Cancel button does, so the fallback treats it as cancel.
        resolve(on_cancel_);
        // Heap-allocated, self-deleting instance (same idiom as
        // InfoQrModal::on_hide()) -- deferred via async_call so the delete
        // doesn't run synchronously inside the exit-animation machinery.
        helix::ui::async_call([](void* data) { delete static_cast<HomeConfirmModal*>(data); },
                              this);
    }

  public:
    // Same fallback-as-cancel contract as on_hide(), for the one path that
    // never reaches it: show() itself failing (no active screen). Called by
    // install_home_confirm_prompter() right before deleting an unshown
    // instance -- without this, a failed show() would silently drop both
    // callbacks and wedge the backend exactly like the bug this class fixes.
    void resolve_unshown_as_cancelled() {
        resolve(on_cancel_);
    }

  private:
    void resolve(std::function<void()>& cb) {
        if (resolved_) {
            return;
        }
        resolved_ = true;
        if (cb) {
            cb();
        }
    }

    std::function<void()> on_confirm_;
    std::function<void()> on_cancel_;
    bool resolved_ = false;
};

/// Installs the modal-backed HomeConfirmPrompter used by every real run of
/// the app. Must run after DisplayManager has created the LVGL display (so
/// lv_screen_active() and the modal subsystem exist by the time a user
/// action later triggers the callback) -- true for every SubjectInitializer
/// entry point, since Application::init_display() (Phase 4) runs well before
/// Application::init_core_subjects()/init_panels()/init_ui() (Phases 9+),
/// which is what calls into SubjectInitializer at all. Installing here (not
/// e.g. inside AmsSubscriptionBackend) keeps the AMS backend layer free of
/// any LVGL/modal dependency -- it only ever calls the seam in
/// filament_op_router.h.
void install_home_confirm_prompter() {
    helix::ui::set_home_confirm_prompter(
        [](std::function<void()> on_confirm, std::function<void()> on_cancel) {
            // Same subjects the static modal_show_confirmation() helper
            // configures (severity/button text) -- "modal_dialog" is a shared
            // XML component bound to these, regardless of which API shows it.
            // modal_configure() leaves a null cancel_text's subject untouched
            // (stale from whichever dialog configured it last), unlike
            // modal_show_confirmation() which defaults it -- pass it explicitly.
            helix::ui::modal_configure(ModalSeverity::Warning, /*show_cancel=*/true,
                                       lv_tr("Home & Continue"), lv_tr("Cancel"));
            const char* attrs[] = {
                "title", lv_tr("Home printer first?"), "message",
                lv_tr("The printer is not homed. Continuing will home all axes, moving the "
                      "toolhead. Make sure the bed is clear."),
                nullptr};
            auto* modal = new HomeConfirmModal(std::move(on_confirm), std::move(on_cancel));
            // show(lv_obj_t*, const char**) -- the instance overload (parent is
            // ignored internally; it always uses lv_screen_active()). A bare
            // nullptr is ambiguous against the static show(const char*, const
            // char**) overload in the same class, hence the explicit cast.
            if (!modal->show(static_cast<lv_obj_t*>(nullptr), attrs)) {
                spdlog::error("[SubjectInitializer] Failed to show home-confirm modal");
                modal->resolve_unshown_as_cancelled();
                delete modal;
            }
        });
}

} // namespace

SubjectInitializer::SubjectInitializer() = default;
SubjectInitializer::~SubjectInitializer() {
    // Invalidate alive guard BEFORE UsbManager destruction — prevents queued
    // USB callbacks from accessing freed PrintSelectPanel
    *m_usb_callback_alive = false;
}

void SubjectInitializer::init_core_and_state() {
    spdlog::debug("[SubjectInitializer] Initializing core and state subjects...");

    // Phase 1: Core subjects (must be first)
    init_core_subjects();

    // Phase 2: PrinterState subjects (panels depend on these)
    init_printer_state_subjects();

    // Warm the Orca match tables on the main thread, before AmsState/backends
    // start below — otherwise the first lane_data heal parses
    // assets/filaments.json under g_orca_mutex on a WebSocket background
    // thread (see filament_variants.h warm_orca_tables()).
    filament::warm_orca_tables();

    // Merge user-contributed Orca type overrides from config/user_filaments.json
    // (object form, `orca_type_map` key). User entries win over shipped entries
    // in orca_match_type() resolution. Same main-thread / pre-backend window as
    // warm_orca_tables() above — see FILAMENT_MANAGEMENT.md § "User overlay format".
    filament::merge_user_orca_overrides(helix::printer::FilamentCatalog::load_user_orca_type_map());

    // Phase 3: AMS and filament sensor subjects
    init_ams_subjects();

    // Phase 4: Navigation subjects — MUST register AFTER PrinterState/AmsState
    // so that in reverse deinit order, NavigationManager cleans up its observers
    // on PrinterState subjects BEFORE PrinterState deinits those subjects.
    NavigationManager::instance().init();

    spdlog::debug("[SubjectInitializer] Core and state subjects initialized");
}

void SubjectInitializer::init_panels(IMoonrakerAPI* api,
                                     const RuntimeConfig& /* runtime_config */) {
    spdlog::debug("[SubjectInitializer] Initializing panel subjects (api={})...",
                  api ? "valid" : "nullptr");

    // Phase 4: Panel subjects
    init_panel_subjects(api);
}

void SubjectInitializer::init_post(const RuntimeConfig& runtime_config) {
    spdlog::debug("[SubjectInitializer] Initializing post-panel subjects...");

    // Phase 5: Observers (depend on subjects being ready)
    init_observers();

    // Phase 6: Utility subjects
    init_utility_subjects();

    // Phase 7: USB manager (needs notification system)
    init_usb_manager(runtime_config);

    // Phase 8: Install the real "home printer first?" confirmation prompter
    // (Task 8) -- see install_home_confirm_prompter() above for why this
    // placement is safe. No test ever reaches this: HelixTestFixture's
    // reset_all() doesn't touch the prompter slot, and no test calls
    // SubjectInitializer::init_post(), so the ~4600 existing tests keep
    // seeing the default (no-prompter, proceed-immediately) behaviour.
    install_home_confirm_prompter();

    m_initialized = true;
    spdlog::debug("[SubjectInitializer] Initialized {} observer guards", m_observers.size());
}

void SubjectInitializer::init_core_subjects() {
    spdlog::trace("[SubjectInitializer] Initializing core subjects");
    app_globals_init_subjects();                    // Global subjects (notification subject, etc.)
    PrinterStatusIcon::instance().init_subjects();  // Printer icon state
    helix::ui::notification_init_subjects();        // Notification badge subjects
    helix::LockManager::instance().init_subjects(); // Lock screen pin_set subject

    // Quick-preset material name/temperature subjects. MUST be here in core
    // init: the filament panel, the three temp panels and the temp graph
    // overlay all bind these by name, so they have to exist before ANY panel
    // XML is created. MaterialSettingsManager::init() is idempotent and already
    // ran via SettingsManager, but call it explicitly so preset identity is
    // loaded even if subject init is driven directly by a test.
    helix::MaterialSettingsManager::instance().init();
    helix::presets::init_subjects();
}

void SubjectInitializer::init_printer_state_subjects() {
    spdlog::trace("[SubjectInitializer] Initializing PrinterState subjects");
    // PrinterState must be initialized BEFORE panels that observe its subjects
    // (e.g., HomePanel observes led_state_, extruder_temp_, connection_state_)
    get_printer_state().init_subjects();

    // ActivePrintMediaManager observes print_filename_ and updates print_display_filename_
    // and print_thumbnail_path_. Must be initialized after PrinterState, before panels.
    helix::init_active_print_media_manager();
}

void SubjectInitializer::init_ams_subjects() {
    spdlog::trace("[SubjectInitializer] Initializing AMS/FilamentSensor subjects");

    // Initialize AmsState subjects BEFORE panels so XML bindings can find ams_gate_count
    // Note: In mock mode, init_subjects() also creates the mock backend internally
    AmsState::instance().init_subjects(true);

    // Initialize SpoolmanManager AFTER AmsState (it reads slot data from AmsState)
    SpoolmanManager::instance().init_subjects();

    // Initialize ToolState subjects (tool changer state tracking)
    helix::ToolState::instance().init_subjects();

    // Macro-slot resolution version. Published before any panel is built so
    // surfaces that gate buttons on macro availability can observe it.
    StandardMacros::instance().init_subjects();

    // Initialize sensor manager subjects BEFORE panels so XML bindings can work
    // Note: Each manager self-registers cleanup with StaticSubjectRegistry in init_subjects()
    helix::FilamentSensorManager::instance().init_subjects();
    helix::sensors::HumiditySensorManager::instance().init_subjects();
    helix::sensors::WidthSensorManager::instance().init_subjects();
    helix::sensors::ProbeSensorManager::instance().init_subjects();
    helix::sensors::AccelSensorManager::instance().init_subjects();
    helix::sensors::ColorSensorManager::instance().init_subjects();
    helix::sensors::TemperatureSensorManager::instance().init_subjects();
}

void SubjectInitializer::init_panel_subjects(IMoonrakerAPI* api) {
    spdlog::trace("[SubjectInitializer] Initializing panel subjects");

    // Initialize widget-owned subjects before any panel XML is created.
    // Widgets like NetworkWidget register subjects that XML bindings reference.
    helix::PanelWidgetManager::instance().init_widget_subjects();

    // Basic panels - these use PanelBase which stores API
    // Cleanup self-registered inside each panel's init_subjects()
    get_global_home_panel().init_subjects();
    if (api)
        get_global_home_panel().set_api(api);

    // Controls, Filament, Settings panels: deinit handled by destructor
    // (registered with StaticPanelRegistry in their get_global_* functions)
    get_global_controls_panel().init_subjects();
    if (api)
        get_global_controls_panel().set_api(api);
    get_global_filament_panel().init_subjects();
    if (api)
        get_global_filament_panel().set_api(api);
    get_global_settings_panel().init_subjects();
    if (api)
        get_global_settings_panel().set_api(api);

    // Advanced panel family
    init_global_advanced_panel(get_printer_state(), api);
    get_global_advanced_panel().init_subjects();

    // SpoolmanPanel uses lazy initialization via get_global_spoolman_panel()
    // and is initialized on first access in AdvancedPanel::handle_spoolman_clicked()

    // HistoryDashboardPanel is now lazy-initialized (OverlayBase pattern)
    // HistoryListPanel is now lazy-initialized by HistoryDashboardPanel (OverlayBase pattern)

    // Timelapse state (event-driven, not a panel)
    helix::TimelapseState::instance().init_subjects();

    // Settings overlays
    init_global_timelapse_settings(api);
    get_global_timelapse_settings().init_subjects();

    init_global_timelapse_install(api);
    get_global_timelapse_install().init_subjects();

#if !defined(HELIX_PLATFORM_ESP32)
    // Timelapse videos overlay is excluded from the ESP32 v1 Core+AMS cut: its
    // TU is not compiled and the accessor is a link stub over uninitialized
    // storage. init_subjects() is pure-virtual, so calling it here would
    // dispatch through a null vtable (LoadProhibited). Nothing on ESP navigates
    // to it, so skip init. (Timelapse settings/install above are real on ESP.)
    init_global_timelapse_videos(api);
    get_global_timelapse_videos().init_subjects();
#endif

    init_global_retraction_settings(api);
    get_global_retraction_settings().init_subjects();

    init_global_console_settings();
    get_global_console_settings().init_subjects();

    // Fan control overlay (opened from Controls panel secondary fans list)
    init_fan_control_overlay(get_printer_state());
    get_fan_control_overlay().init_subjects();

    // LED control overlay (opened from Home panel light long-press)
    init_led_control_overlay(get_printer_state());

    // ConsolePanel is now lazy-initialized by AdvancedPanel (OverlayBase pattern)

    // Row handlers for advanced features
    init_screws_tilt_row_handler();
    init_input_shaper_row_handler();
    init_belt_tension_row_handler();
    init_zoffset_row_handler();
    init_zoffset_event_callbacks();
    init_probe_row_handler();

    // Wizard and keypad — cleanup self-registered inside init_subjects()
    ui_wizard_init_subjects();
    ui_keypad_init_subjects();

    // PrinterStatusIcon and StatusBar subjects — cleanup self-registered inside init_subjects()

    // Note: PrintSelectPanel registers its own deinit+destroy callback in get_print_select_panel()
    m_print_select_panel = get_print_select_panel(get_printer_state(), api);
    m_print_select_panel->init_subjects();
    if (api)
        m_print_select_panel->set_api(api);

    // Shared print-control buttons own the renamed pause/resume/stop subjects +
    // shared callbacks; register before the print-status panel (and any home
    // widget) XML binds them.
    auto& print_controls = helix::ui::PrintControlButtons::instance();
    if (api)
        print_controls.set_api(api);
    print_controls.init_subjects();

    m_print_status_panel = &get_global_print_status_panel();
    if (api)
        m_print_status_panel->set_api(api);
    m_print_status_panel->init_subjects();

    // Motion panel: deinit handled by destructor
    // (registered with StaticPanelRegistry in their get_global_* functions)
    m_motion_panel = &get_global_motion_panel();
    m_motion_panel->init_subjects();

#if !defined(HELIX_PLATFORM_ESP32)
    // Bed-mesh and calibration (PID / Z-offset) panels are excluded from the
    // ESP32 v1 Core+AMS cut: their TUs are not compiled and the accessors are
    // link stubs over uninitialized storage. init_subjects() is pure-virtual,
    // so calling it on that storage dispatches through a null vtable
    // (LoadProhibited). Nothing on ESP navigates to them; skip boot init.
    // m_bed_mesh_panel stays nullptr — its getter is never called on ESP.
    m_bed_mesh_panel = &get_global_bed_mesh_panel();
    m_bed_mesh_panel->init_subjects();

    // Panel initialization via global instances
    // PIDCalibrationPanel: deinit handled by destructor (registered with StaticPanelRegistry)
    get_global_pid_cal_panel().init_subjects();

    get_global_zoffset_cal_panel().init_subjects();
#endif

    // TemperatureController (owned by SubjectInitializer — stateless wiring, no subjects)
    m_temp_controller = std::make_unique<helix::TemperatureController>(get_printer_state(), api);
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        m_temp_controller.get());

    // TemperatureService (owned by SubjectInitializer - destructor handles deinit_subjects)
    m_temp_control_panel = std::make_unique<TemperatureService>(get_printer_state(), api);
    m_temp_control_panel->set_controller(m_temp_controller.get());
    m_temp_control_panel->init_subjects();

    // Inject TemperatureService into dependent panels
    // (HomePanel widgets get it via PanelWidgetManager::shared_resource<TemperatureService>())
    get_global_controls_panel().set_temp_control_panel(m_temp_control_panel.get());
    get_global_print_status_panel().set_temp_control_panel(m_temp_control_panel.get());
    get_global_filament_panel().set_temp_control_panel(m_temp_control_panel.get());
    get_global_pid_cal_panel().set_temp_control_panel(m_temp_control_panel.get());

    // Register shared resources on PanelWidgetManager for self-registered widget factories
    helix::PanelWidgetManager::instance().register_shared_resource<TemperatureService>(
        m_temp_control_panel.get());
    if (api) {
        helix::PanelWidgetManager::instance().register_shared_resource<IMoonrakerAPI>(api);
    }

    // E-Stop overlay — cleanup self-registered inside init_subjects()
    EmergencyStopOverlay::instance().init_subjects();

    // AbortManager subjects — cleanup self-registered inside init_subjects()
    helix::AbortManager::instance().init_subjects();

    // ActivePrintMediaManager needs API for thumbnail loading
    if (api) {
        helix::get_active_print_media_manager().set_api(api);
    }
}

void SubjectInitializer::init_observers() {
    spdlog::trace("[SubjectInitializer] Initializing observers");

    // Print completion notification observer
    m_observers.push_back(helix::init_print_completion_observer());
    m_observers.push_back(helix::init_preparing_exit_observer());

    // Print start navigation observer (auto-navigate to print status)
    m_observers.push_back(helix::init_print_start_navigation_observer());

    // Power-loss-recovery offer controller. Registered unconditionally — the
    // pure plr_should_offer() self-guards non-Snapmaker printers (pl_env_valid
    // stays false on every other backend). Owns its own observers; RAII cleanup
    // on SubjectInitializer teardown.
    m_plr_offer_controller = std::make_unique<helix::ui::PlrOfferController>();

    // Print outcome telemetry observer (records anonymous print stats when telemetry enabled)
    m_observers.push_back(TelemetryManager::instance().init_print_outcome_observer());

    // Panel usage tracking for telemetry
    // Note: Uses raw ObserverGuard rather than observe_int_sync because there's no
    // Panel* context — SubjectInitializer is not a Panel subclass.
    {
        auto* panel_subject = NavigationManager::instance().get_active_panel_subject();
        if (panel_subject) {
            m_observers.push_back(ObserverGuard(
                panel_subject,
                [](lv_observer_t* /*obs*/, lv_subject_t* subj) {
                    int panel_id = lv_subject_get_int(subj);
                    std::string name;
                    switch (static_cast<helix::PanelId>(panel_id)) {
                    case helix::PanelId::Home:
                        name = "home";
                        break;
                    case helix::PanelId::PrintSelect:
                        name = "print_select";
                        break;
                    case helix::PanelId::Controls:
                        name = "controls";
                        break;
                    case helix::PanelId::Filament:
                        name = "filament";
                        break;
                    case helix::PanelId::Settings:
                        name = "settings";
                        break;
                    case helix::PanelId::Advanced:
                        name = "advanced";
                        break;
                    default:
                        name = "unknown";
                        break;
                    }
                    TelemetryManager::instance().notify_panel_changed(name);
                },
                nullptr));
        }
    }

    // Connection stability tracking for telemetry
    {
        auto& ps = get_printer_state();

        // Connection state observer — safe without mutex because these subjects
        // are only updated via ui_queue_update() (fires on LVGL/main thread).
        auto* conn_subject = ps.get_printer_connection_state_subject();
        if (conn_subject) {
            m_observers.push_back(ObserverGuard(
                conn_subject,
                [](lv_observer_t* /*obs*/, lv_subject_t* subj) {
                    int state = lv_subject_get_int(subj);
                    TelemetryManager::instance().notify_connection_state_changed(state);
                },
                nullptr));
        }

        // Klippy state observer
        auto* klippy_subject = ps.get_klippy_state_subject();
        if (klippy_subject) {
            m_observers.push_back(ObserverGuard(
                klippy_subject,
                [](lv_observer_t* /*obs*/, lv_subject_t* subj) {
                    int state = lv_subject_get_int(subj);
                    TelemetryManager::instance().notify_klippy_state_changed(state);
                },
                nullptr));
        }
    }
}

void SubjectInitializer::init_utility_subjects() {
    spdlog::trace("[SubjectInitializer] Initializing utility subjects");
    ui_notification_init();
}

void SubjectInitializer::init_usb_manager(const RuntimeConfig& runtime_config) {
    spdlog::trace("[SubjectInitializer] Initializing USB manager");

    m_usb_manager = std::make_unique<UsbManager>(runtime_config.should_mock_usb());
    if (m_usb_manager->start()) {
        spdlog::debug("[SubjectInitializer] USB Manager started (mock={})",
                      runtime_config.should_mock_usb());
        if (m_print_select_panel) {
            m_print_select_panel->set_usb_manager(m_usb_manager.get());
        }
        // Also provide USB manager to printer image overlay
        helix::settings::get_printer_image_overlay().set_usb_manager(m_usb_manager.get());
    } else {
        spdlog::info(
            "[SubjectInitializer] USB Manager not started (not available on this platform)");
    }

    // Set up USB drive event notifications
    if (m_usb_manager) {
        // Track when USB callbacks were set up - suppress toasts for drives at startup
        static auto usb_setup_time = std::chrono::steady_clock::now();

        // Capture print_select_panel and alive guard for the callback
        PrintSelectPanel* panel = m_print_select_panel;
        std::weak_ptr<bool> weak_alive = m_usb_callback_alive;

        m_usb_manager->set_drive_callback([panel, weak_alive](UsbEvent event,
                                                              const UsbDrive& drive) {
            (void)drive;

            // Suppress toast for drives detected within 3 seconds of startup
            constexpr auto GRACE_PERIOD = std::chrono::seconds(3);
            auto now = std::chrono::steady_clock::now();
            bool within_grace_period = (now - usb_setup_time) < GRACE_PERIOD;

            if (event == UsbEvent::DRIVE_INSERTED) {
                if (!within_grace_period) {
                    // Marshal notification to main thread — callback fires from
                    // USB backend's background thread
                    helix::ui::queue_update([]() { NOTIFY_SUCCESS(lv_tr("USB drive connected")); });
                } else {
                    spdlog::debug("[USB] Suppressing toast for drive present at startup");
                }
                if (panel) {
                    // Marshal to main thread — callback fires from UsbBackendMock's
                    // demo thread, and panel methods touch LVGL widgets.
                    // Capture weak_alive to guard against panel destruction.
                    helix::ui::queue_update([panel, weak_alive]() {
                        if (!weak_alive.expired())
                            panel->on_usb_drive_inserted();
                    });
                }
            } else if (event == UsbEvent::DRIVE_REMOVED) {
                helix::ui::queue_update([]() { NOTIFY_INFO(lv_tr("USB drive removed")); });
                if (panel) {
                    helix::ui::queue_update([panel, weak_alive]() {
                        if (!weak_alive.expired())
                            panel->on_usb_drive_removed();
                    });
                }
            }
        });
        // Note: Demo drives are now auto-added by UsbBackendMock::start() after 1.5s delay
    }
}
