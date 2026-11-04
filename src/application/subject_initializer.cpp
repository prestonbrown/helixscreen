// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "subject_initializer.h"

#include "ui_bed_drying_modal.h"
#include "ui_component_keypad.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_notification_manager.h"
#include "ui_overlay_printer_image.h"
#include "ui_overlay_retraction_settings.h"
#include "ui_overlay_timelapse_install.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_overlay_timelapse_videos.h"
#include "ui_panel_advanced.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_tool_offset.h"
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
#include "ui_stepper_resonance_cal_modal.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "abort_manager.h"
#include "active_print_media_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "bed_drying_controller.h"
#include "bed_mesh_state.h"
#include "chamber_stall_prompt.h"
#include "filament_accounting_controller.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_op_heater.h"
#include "filament_variants.h"
#include "lock_manager.h"
#include "lvgl/lvgl.h"
#include "material_settings_manager.h"
#include "panel_widget_manager.h"
#include "plr_offer_controller.h"
#include "plugin_status_refresh.h"
#include "preset_materials.h"
#include "print_completion.h"
#include "print_control_buttons.h"
#include "print_photos_controller.h"
#include "print_start_navigation.h"
#include "printer_image_manager.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "sensor_manager.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "standard_macros.h"
#include "standing_print_options_controller.h"
#if HELIX_HAS_CAMERA
#include "camera_frame.h"
#endif
#include "system/telemetry_manager.h"
#include "temperature_controller.h"
#include "temperature_service.h"
#include "timelapse_state.h"
#include "tool_state.h"
#include "usb_manager.h"
#include "xml_registration.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>

SubjectInitializer::SubjectInitializer() = default;
// PanelWidgetManager outlives this object, and the next printer's widget subjects read
// their shared resources before that printer's SubjectInitializer registers its own, so a
// slot left naming an object freed here is dereferenced on every printer switch.
SubjectInitializer::~SubjectInitializer() {
    auto& widgets = helix::PanelWidgetManager::instance();
    widgets.unregister_shared_resource<helix::IMoonrakerAPI>(m_registered_api);
    widgets.unregister_shared_resource<TemperatureService>(m_temp_control_panel.get());
    widgets.unregister_shared_resource<helix::BedDryingController>(m_bed_drying.get());
    widgets.unregister_shared_resource<helix::TemperatureController>(m_temp_controller.get());
    widgets.unregister_shared_resource<helix::ui::FilamentOpHeater>(m_filament_op_heater.get());
}

void SubjectInitializer::init_core_and_state() {
    spdlog::debug("[SubjectInitializer] Initializing core and state subjects...");

    // Phase 1: Core subjects (must be first)
    init_core_subjects();

    // Phase 2: PrinterState subjects (panels depend on these)
    init_printer_state_subjects();

    // Settings subjects next: panels built from here on observe the language
    // subject, and LVGL refuses an observer on a subject not yet initialised.
    // Needs PrinterState (the Z movement override applies there).
    helix::SettingsManager::instance().init_subjects();

    // Warm the Orca match tables on the main thread, before AmsState/backends
    // start below — otherwise the first lane_data heal parses
    // assets/filaments.json under g_orca_mutex on a WebSocket background
    // thread (see filament_variants.h warm_orca_tables()).
    filament::warm_orca_tables();

    // Same reason for the material-type table: its first use would otherwise
    // parse the asset and the user overlay on whichever thread got there first.
    filament::materials();

    // Merge user-contributed Orca type overrides from config/user_filaments.json
    // (object form, `orca_type_map` key). User entries win over shipped entries
    // in orca_match_type() resolution. Same main-thread / pre-backend window as
    // warm_orca_tables() above — see FILAMENT_MANAGEMENT.md § "User overlay format".
    filament::merge_user_orca_overrides(helix::printer::FilamentCatalog::load_user_orca_type_map());

    // Phase 3: AMS and filament sensor subjects
    init_ams_subjects();

    // Z-offset save availability. Derived from PrinterState's strategy and
    // machine-wide offset plus ToolState's per-tool dirty flag, so it registers
    // after all three, and before any panel XML binds the name.
    helix::zoffset::init_save_available_subject();

    // The active bed mesh's stats, read by BedMeshPanel's XML and the home tile. They
    // exist from here, so neither depends on the panel having been opened.
    helix::BedMeshState::instance().init_subjects();

    // Phase 4: Navigation subjects — MUST register AFTER PrinterState/AmsState
    // so that in reverse deinit order, NavigationManager cleans up its observers
    // on PrinterState subjects BEFORE PrinterState deinits those subjects.
    NavigationManager::instance().init();

    spdlog::debug("[SubjectInitializer] Core and state subjects initialized");
}

void SubjectInitializer::init_panels(helix::IMoonrakerAPI* api,
                                     const RuntimeConfig& /* runtime_config */) {
    spdlog::debug("[SubjectInitializer] Initializing panel subjects (api={})...",
                  api ? "valid" : "nullptr");

    // Phase 4: Panel subjects
    init_panel_subjects(api);
}

void SubjectInitializer::init_post(helix::IMoonrakerAPI* api, const RuntimeConfig& runtime_config) {
    spdlog::debug("[SubjectInitializer] Initializing post-panel subjects...");

    // Phase 5: Observers (depend on subjects being ready)
    init_observers(api);

    // Phase 6: Utility subjects
    init_utility_subjects();

    // Phase 7: USB manager (needs notification system)
    init_usb_manager(runtime_config);

    m_initialized = true;
    spdlog::debug("[SubjectInitializer] Initialized {} observer guards", m_observers.size());
}

void SubjectInitializer::init_core_subjects() {
    spdlog::trace("[SubjectInitializer] Initializing core subjects");
    app_globals_init_subjects();                    // Global subjects (notification subject, etc.)
    PrinterStatusIcon::instance().init_subjects();  // Printer icon state
    helix::ui::notification_init_subjects();        // Notification badge subjects
    helix::LockManager::instance().init_subjects(); // Lock screen pin_set subject
    // HomePanel observes it from its constructor, which runs in init_panels().
    helix::PrinterImageManager::instance().init_subjects();

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
    // (e.g., HomePanel observes extruder_temp_, connection_state_)
    get_printer_state().init_subjects();

    // ActivePrintMediaManager observes print_filename_ and updates print_display_filename_
    // and print_thumbnail_path_. Must be initialized after PrinterState, before panels.
    helix::init_active_print_media_manager();
}

void SubjectInitializer::init_ams_subjects() {
    spdlog::trace("[SubjectInitializer] Initializing AMS/FilamentSensor subjects");

    // ToolState subjects (tool changer state tracking) come FIRST: in mock mode
    // AmsState::init_subjects() below creates its backend and syncs it straight
    // through build_ams_topology() into ToolState::set_ams_topology(), so
    // ToolState's subjects have to exist by then. Registering first also means
    // that in reverse deinit order AmsState - the side that pushes - tears down
    // before the state it pushes into.
    helix::ToolState::instance().init_subjects();

    // Initialize AmsState subjects BEFORE panels so XML bindings can find ams_gate_count
    // Note: In mock mode, init_subjects() also creates the mock backend internally
    helix::AmsState::instance().init_subjects(true);

    // Initialize SpoolmanManager AFTER AmsState (it reads slot data from AmsState)
    SpoolmanManager::instance().init_subjects();

    // Macro-slot resolution version. Published before any panel is built so
    // surfaces that gate buttons on macro availability can observe it.
    StandardMacros::instance().init_subjects();

    // Initialize sensor manager subjects BEFORE panels so XML bindings can work
    // Note: Each manager self-registers cleanup with StaticSubjectRegistry in init_subjects()
    for (auto* m : helix::sensors::sensor_managers()) {
        m->init_subjects();
    }

    // The clog meter falls back to a filament motion sensor, whose subjects
    // exist only from here.
    helix::AmsState::instance().install_motion_sensor_observer();
}

void SubjectInitializer::init_panel_subjects(helix::IMoonrakerAPI* api) {
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

    // ConsolePanel is now lazy-initialized by AdvancedPanel (OverlayBase pattern)

    // Row handlers for advanced features
    init_screws_tilt_row_handler();
    init_input_shaper_row_handler();
    init_belt_tension_row_handler();
    init_zoffset_row_handler();
    helix::ui::init_tool_offset_row_handler();
    helix::ui::init_stepper_resonance_cal_row_handler();
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
    m_bed_mesh_panel->set_api(api);
    m_bed_mesh_panel->init_subjects();

    // Panel initialization via global instances
    // PIDCalibrationPanel: deinit handled by destructor (registered with StaticPanelRegistry)
    get_global_pid_cal_panel().init_subjects();

    get_global_zoffset_cal_panel().init_subjects();
#endif

    init_temperature_control(api);

    // Inject TemperatureService into dependent panels
    // (HomePanel widgets get it via PanelWidgetManager::shared_resource<TemperatureService>())
    get_global_controls_panel().set_temp_control_panel(m_temp_control_panel.get());
    get_global_print_status_panel().set_temp_control_panel(m_temp_control_panel.get());
    get_global_filament_panel().set_temp_control_panel(m_temp_control_panel.get());
    get_global_pid_cal_panel().set_temp_control_panel(m_temp_control_panel.get());

    // E-Stop overlay — cleanup self-registered inside init_subjects()
    EmergencyStopOverlay::instance().init_subjects();

    // AbortManager subjects — cleanup self-registered inside init_subjects()
    helix::AbortManager::instance().init_subjects();

    // ActivePrintMediaManager needs API for thumbnail loading
    if (api) {
        helix::get_active_print_media_manager().set_api(api);
    }
}

void SubjectInitializer::init_temperature_control(helix::IMoonrakerAPI* api) {
    // TemperatureController (owned by SubjectInitializer — stateless wiring, no subjects)
    m_temp_controller = std::make_unique<helix::TemperatureController>(get_printer_state(), api);
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        m_temp_controller.get());

    // The nozzle heat around every filament op. Here, not on a panel: the
    // surface that asks (a tile's dialog) is often gone before the nozzle is hot.
    m_filament_op_heater = std::make_unique<helix::ui::FilamentOpHeater>(get_printer_state());
    helix::PanelWidgetManager::instance().register_shared_resource<helix::ui::FilamentOpHeater>(
        m_filament_op_heater.get());

    // Drying on the heated bed (prestonbrown/helixscreen#1730). restore() brings
    // back a persisted spools-on-the-bed latch before any panel can move the
    // printer.
    // Under --test the run's clock follows --sim-speed, so a 12 h run plays out
    // against the mock in minutes.
    helix::BedDryingController::Clock bed_drying_clock;
    if (auto* rc = get_runtime_config(); rc && rc->is_test_mode() && rc->sim_speedup > 1.0) {
        const double speedup = rc->sim_speedup;
        const auto t0 = std::chrono::steady_clock::now();
        const long long wall0 = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        bed_drying_clock = [speedup, t0, wall0] {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return wall0 + static_cast<long long>(elapsed * speedup);
        };
    }
    m_bed_drying = std::make_unique<helix::BedDryingController>(
        get_printer_state(), api, m_temp_controller.get(), std::move(bed_drying_clock));
    m_bed_drying->init_subjects();
    m_bed_drying->set_on_ready_to_remove(helix::ui::show_bed_drying_remove_prompt);
    m_bed_drying->set_on_print_while_latched(helix::ui::show_spools_on_bed_print_alarm);
    helix::PanelWidgetManager::instance().register_shared_resource<helix::BedDryingController>(
        m_bed_drying.get());
    m_bed_drying->restore();

    // TemperatureService (owned by SubjectInitializer - destructor handles deinit_subjects)
    m_temp_control_panel = std::make_unique<TemperatureService>(get_printer_state(), api);
    m_temp_control_panel->set_controller(m_temp_controller.get());
    m_temp_control_panel->init_subjects();

    // Register shared resources on PanelWidgetManager for self-registered widget factories
    helix::PanelWidgetManager::instance().register_shared_resource<TemperatureService>(
        m_temp_control_panel.get());
    if (api) {
        helix::PanelWidgetManager::instance().register_shared_resource<helix::IMoonrakerAPI>(api);
        m_registered_api = api;
    }
}

void SubjectInitializer::init_observers(helix::IMoonrakerAPI* api) {
    spdlog::trace("[SubjectInitializer] Initializing observers");

    // Print completion notification observer
    m_observers.push_back(helix::init_print_completion_observer());
    m_observers.push_back(helix::init_preparing_exit_observer());

    // Print start navigation observer (auto-navigate to print status)
    m_observers.push_back(helix::init_print_start_navigation_observer());

    // Power-loss-recovery offer controller. Registered unconditionally — the
    // pure plr_should_offer() self-guards printers without a recovery backend.
    // Owns its own observers; RAII cleanup on SubjectInitializer teardown.
    m_plr_offer_controller = std::make_unique<helix::ui::PlrOfferController>();

    // A print held by a chamber wait while the chamber heater is not heating
    // gets a prompt with a way out; a rejection outside a print gets a toast.
    m_chamber_stall_prompt = std::make_unique<helix::ui::ChamberStallPrompt>();

    // Standing print options: kept in Moonraker for the plugin's print hooks,
    // and sent by this screen at a foreign job start when the plugin cannot.
    m_standing_print_options = std::make_unique<helix::StandingPrintOptionsController>(api);

    // Filament accounting: the spool map the plugin switches the active spool
    // from, and this screen's switch at a tool change when the plugin cannot.
    m_filament_accounting = std::make_unique<helix::FilamentAccountingController>();

    // The plugin's feature map, read again at each job start and on the
    // plugin's word, so every plugin-or-fallback choice reads it current.
    m_plugin_status_refresh = std::make_unique<helix::PluginStatusRefresh>();

    // Camera photos in history, taken by this screen when the plugin cannot.
    // A build without a camera has no snapshot to take, so the plugin takes
    // every photo and there is nothing for a controller here to do.
#if HELIX_HAS_CAMERA
    m_print_photos =
        std::make_unique<helix::PrintPhotosController>(helix::live_camera_sources().fetch);
#endif

    // Print outcome telemetry observer (records anonymous print stats when telemetry enabled)
    m_observers.push_back(TelemetryManager::instance().init_print_outcome_observer());

    // Panel usage tracking for telemetry
    // Note: Uses raw ObserverGuard rather than observe<int> because there's no
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
        auto* conn_subject = ps.network_state().get_printer_connection_state_subject();
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
        auto* klippy_subject = ps.network_state().get_klippy_state_subject();
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

    // The callback goes on before start(): the backend's thread can report a
    // drive as soon as it runs, and an event with no callback is dropped.
    {
        // Track when USB callbacks were set up - suppress toasts for drives at startup
        static auto usb_setup_time = std::chrono::steady_clock::now();

        // Capture print_select_panel and alive guard for the callback
        PrintSelectPanel* panel = m_print_select_panel;
        std::weak_ptr<bool> weak_alive = m_usb_callback_alive;

        m_usb_manager->set_drive_callback([panel, weak_alive](UsbEvent event,
                                                              const UsbDrive& drive) {
            (void)drive;

            // Suppress toast for drives detected within 3 seconds of startup
            constexpr auto kGracePeriod = std::chrono::seconds(3);
            auto now = std::chrono::steady_clock::now();
            bool within_grace_period = (now - usb_setup_time) < kGracePeriod;

            if (event == UsbEvent::DRIVE_INSERTED) {
                if (!within_grace_period) {
                    // Marshal notification to main thread — callback fires from
                    // USB backend's background thread
                    helix::ui::queue_update("SubjectInitializer::init_usb_manager",
                                            []() { NOTIFY_SUCCESS(lv_tr("USB drive connected")); });
                } else {
                    spdlog::debug("[USB] Suppressing toast for drive present at startup");
                }
                if (panel) {
                    // Marshal to main thread — callback fires from UsbBackendMock's
                    // demo thread, and panel methods touch LVGL widgets.
                    // Capture weak_alive to guard against panel destruction.
                    helix::ui::queue_update("SubjectInitializer::init_usb_manager",
                                            [panel, weak_alive]() {
                                                if (!weak_alive.expired())
                                                    panel->on_usb_drive_inserted();
                                            });
                }
            } else if (event == UsbEvent::DRIVE_REMOVED) {
                helix::ui::queue_update("SubjectInitializer::init_usb_manager",
                                        []() { NOTIFY_INFO(lv_tr("USB drive removed")); });
                if (panel) {
                    helix::ui::queue_update("SubjectInitializer::init_usb_manager",
                                            [panel, weak_alive]() {
                                                if (!weak_alive.expired())
                                                    panel->on_usb_drive_removed();
                                            });
                }
            }
        });
    }

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
}
