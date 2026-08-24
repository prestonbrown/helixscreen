// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_component_keypad.h"
#include "ui_heater_icon_binder.h"
#include "ui_observer_guard.h"
#include "ui_panel_base.h"
#include "ui_print_tune_overlay.h"
#include "ui_temperature_utils.h"

#include "async_lifetime_guard.h"
#include "config.h"
#include "operation_timeout_guard.h"
#include "standard_macros.h"
#include "subject_managed_panel.h"
#include "ui/position_observer_bundle.h"
#include "ui/temperature_observer_bundle.h"
#include "ui/ui_modal_guard.h"

#include <memory>
#include <optional>

// Forward declaration
class TemperatureService;
namespace helix {
class TemperatureController;
class LedWidget;
namespace ui {
struct ControlsPanelTestAccess; // test-only friend (tests/test_helpers/)
} // namespace ui
} // namespace helix

/**
 * @file ui_panel_controls.h
 * @brief Controls Panel V2 - Dashboard with 5 smart cards
 *
 * A card-based dashboard providing quick access to printer controls with
 * live data display. Uses proper reactive XML event_cb bindings.
 *
 * ## V2 Layout (3+1 Grid):
 * - Row 1: Quick Actions | Temperatures | Cooling
 * - Row 2: Calibration & Tools (centered)
 *
 * ## Key Features:
 * - Combined nozzle + bed temperature card with dual progress bars
 * - Quick Actions: Home buttons (All/XY/Z) + configurable macro slots
 * - Cooling: Part fan hero slider + secondary fans list
 * - Calibration: Bed mesh, Z-offset, screws, motor disable
 *
 * ## Event Binding Pattern:
 * - Button event handlers: XML `event_cb` + `lv_xml_register_event_cb()`
 * - Card background clicks: Manual `lv_obj_add_event_cb()` with user_data
 * - Observer callbacks: RAII ObserverGuard for automatic cleanup
 *
 * @see PanelBase for base class documentation
 * @see ui_nav for overlay navigation
 */
class ControlsPanel : public PanelBase {
  public:
    /**
     * @brief Construct ControlsPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState
     * @param api Pointer to IMoonrakerAPI (may be nullptr)
     */
    ControlsPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    ~ControlsPanel() override;

    /**
     * @brief Set reference to TemperatureService for temperature sub-screens
     *
     * Must be called before setup() if temperature panels should work.
     * @param temp_panel Pointer to TemperatureService instance
     */
    void set_temp_control_panel(TemperatureService* temp_panel);

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize subjects and register XML event callbacks
     *
     * Registers all V2 dashboard subjects for reactive data binding
     * and registers XML event_cb handlers for buttons.
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Calls lv_subject_deinit() on all local lv_subject_t members.
     * Must be called before lv_deinit() to prevent dangling observers.
     */
    void deinit_subjects();

    /**
     * @brief Setup the controls panel with card navigation handlers
     *
     * Wires up card background click handlers for navigation to full panels.
     * All button handlers are already wired via XML event_cb in init_subjects().
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen (needed for overlay panel creation)
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Controls Panel";
    }
    const char* get_xml_component_name() const override {
        return "controls_panel";
    }

    /**
     * @brief Called when panel becomes visible
     *
     * Refreshes the secondary fans list to handle cases where fan discovery
     * completed after initial setup or when switching between connections.
     */
    void on_activate() override;
    void on_deactivate() override;

  private:
    // Test-only access to private secondary-fan lifetime/observer internals.
    friend struct helix::ui::ControlsPanelTestAccess;

    //
    // === Panel Active State (observer suspension) ===
    //

    bool active_ = false; ///< True when panel is visible; observer callbacks skip work when false

    /// Force-refresh all display values (called on activate to catch up on missed changes)
    void refresh_all_displays();

    //
    // === Dependencies ===
    //

    TemperatureService* temp_control_panel_ = nullptr;

    /// Convenience accessor: the temp controller via the temp service, or
    /// nullptr if no service is wired up. Centralizes the null-guard the
    /// temperature command sites share.
    helix::TemperatureController* controller() const;

    //
    // === Configurable Macro Buttons (StandardMacros integration) ===
    //

    std::optional<StandardMacroSlot> macro_1_slot_; ///< Slot for macro button 1
    std::optional<StandardMacroSlot> macro_2_slot_; ///< Slot for macro button 2

    /**
     * @brief Refresh macro button labels and visibility
     *
     * Called after StandardMacros config changes to update button text
     * and hide buttons for empty slots.
     */
    void refresh_macro_buttons();

    //
    // === Subject Manager (RAII cleanup) ===
    //

    SubjectManager subjects_; ///< RAII subject manager - auto-deinits all subjects

    //
    // === V2 Dashboard Subjects (for XML bind_text/bind_value) ===
    //

    // Nozzle label (dynamic: "Nozzle:" or "Nozzle N:" for multi-tool)
    lv_subject_t nozzle_label_subject_{};
    char nozzle_label_buf_[32] = {};
    ObserverGuard active_tool_observer_;
    void update_nozzle_label();

    // Nozzle temperature display
    lv_subject_t nozzle_temp_subject_{};
    char nozzle_temp_buf_[32] = {};
    lv_subject_t nozzle_pct_subject_{};
    lv_subject_t nozzle_status_subject_{};
    char nozzle_status_buf_[16] = {};

    // Bed temperature display
    lv_subject_t bed_temp_subject_{};
    char bed_temp_buf_[32] = {};
    lv_subject_t bed_pct_subject_{};
    lv_subject_t bed_status_subject_{};
    char bed_status_buf_[16] = {};

    // Chamber temperature display
    lv_subject_t chamber_status_subject_{};
    char chamber_status_buf_[16] = {};

    // Heating icon animators (nozzle/bed/chamber), bound from the panel's own
    // container so lv_obj_find_by_name() cannot pick up another panel's
    // same-named icon. Each binder owns its own temperature observers.
    helix::ui::HeaterIconBinder nozzle_icon_binder_;
    helix::ui::HeaterIconBinder bed_icon_binder_;
    helix::ui::HeaterIconBinder chamber_icon_binder_;

    // Fan speed display
    lv_subject_t fan_speed_subject_{};
    char fan_speed_buf_[16] = {};
    lv_subject_t fan_pct_subject_{};
    uint32_t last_fan_slider_input_ = 0; // Tick of last slider interaction (suppression window)

    // Macro button subjects for declarative binding.
    //
    // Two independent gates, because a macro button has three states, not two:
    //   *_visible   0 = nothing is assigned to this slot, do not render it
    //   *_available 0 = rendered but not usable — the slot resolves to a macro
    //                   the connected printer does not define
    // A slot the user configured against a macro this printer lacks stays
    // visible and goes disabled, so the button that stopped working is still
    // where they left it instead of silently vanishing.
    lv_subject_t macro_1_visible_{};
    lv_subject_t macro_2_visible_{};
    lv_subject_t macro_1_available_{};
    lv_subject_t macro_2_available_{};
    lv_subject_t macro_1_name_{};
    lv_subject_t macro_2_name_{};
    char macro_1_name_buf_[64] = {};
    char macro_2_name_buf_[64] = {};

    //
    // === Cached Values (for display update efficiency) ===
    //

    int cached_extruder_temp_ = 0;
    int cached_extruder_target_ = 0;
    int cached_bed_temp_ = 0;
    int cached_bed_target_ = 0;
    int cached_chamber_temp_ = 0; ///< Chamber current temperature (decidegrees)
    int cached_chamber_target_ =
        0; ///< Raw chamber heater target (decidegrees) — used for keypad seed only
    int cached_chamber_effective_target_ =
        0; ///< Canonical display target (decidegrees): effective target per chamber_mode
    int cached_chamber_mode_ = 0; ///< ChamberMode int (Off=0/Heating=1/Maintaining=2)

    // Temperature limits for keypad
    int nozzle_max_temp_ = 500;  ///< Nozzle max temperature (°C) from heater_generic config
    int bed_max_temp_ = 150;     ///< Bed max temperature (°C) from heater_bed config
    int chamber_max_temp_ = 150; ///< Chamber max temperature (°C)

    //
    // === Observer Guards (RAII cleanup) ===
    //

    /// @brief Temperature observer bundle (nozzle + bed temps)
    helix::ui::TemperatureObserverBundle<ControlsPanel> temp_observers_;
    ObserverGuard macros_version_observer_; // Macro slot resolution changed
    ObserverGuard fan_observer_;
    ObserverGuard fans_version_observer_;      // Multi-fan list changes
    ObserverGuard temp_sensor_count_observer_; // Temp sensor list changes
    SubjectLifetime chamber_temp_lifetime_;    // Lifetime token for chamber temp subject
    SubjectLifetime chamber_target_lifetime_;  // Lifetime token for raw heater target subject
    SubjectLifetime
        chamber_effective_target_lifetime_; // Lifetime token for effective target subject
    SubjectLifetime chamber_mode_lifetime_; // Lifetime token for chamber mode subject
    ObserverGuard chamber_temp_observer_;   // Chamber temperature observer
    ObserverGuard chamber_target_observer_; // Chamber raw heater target observer (keypad seed)
    ObserverGuard chamber_effective_target_observer_; // Chamber effective target observer (status)
    ObserverGuard chamber_mode_observer_;             // Chamber M141 control mode observer

    bool fans_rebuild_pending_ = false; ///< Coalesces rapid fans_version observer notifications
    bool temps_rebuild_pending_ =
        false; ///< Coalesces rapid temp_sensor_count observer notifications
    helix::AsyncLifetimeGuard
        lifetime_; ///< Guards deferred callbacks from accessing destroyed panel

    //
    // === Lazily-Created Child Panels ===
    //

    lv_obj_t* motion_panel_ = nullptr;
    lv_obj_t* fan_control_panel_ = nullptr;
    lv_obj_t* bed_mesh_panel_ = nullptr;
    lv_obj_t* zoffset_panel_ = nullptr;
    lv_obj_t* screws_panel_ = nullptr;

    /// LED quick-toggle for the Calibration & Tools grid cell. Reuses the same
    /// LedWidget that drives the home-dashboard light widget (stateful bulb icon
    /// reflecting on/off + brightness + LED color; tap toggles). Present only
    /// when an LED strip is controllable (cell hidden via led_controllable).
    std::unique_ptr<helix::LedWidget> led_widget_;

    //
    // === Modal Dialog State ===
    //

    helix::ui::ModalGuard motors_confirmation_dialog_;
    helix::ui::ModalGuard save_z_offset_confirmation_dialog_;
    helix::ui::ModalGuard macro_run_confirmation_dialog_;
    OperationTimeoutGuard operation_guard_;

    /// Guards against a double-click race on Save Z-Offset.
    ///
    /// A bounded timeout rather than a bare bool: SAVE_CONFIG restarts Klipper,
    /// and MoonrakerClient::notify_klippy_disconnected() calls
    /// tracker_.cleanup_all(), which drops the pending RPC — so neither the
    /// success nor the error callback ever fires and a plain flag stayed latched
    /// until app restart, leaving the Save button dead. The guard self-clears.
    OperationTimeoutGuard save_z_offset_guard_;

    /// Covers Z_OFFSET_APPLY_PROBE + SAVE_CONFIG plus the Klipper restart, with
    /// headroom for stock code that chains a second config write (Creality K2 +
    /// CFS writes CFS Tn_data via CXSAVE_CONFIG ~50s later).
    static constexpr uint32_t SAVE_Z_OFFSET_TIMEOUT_MS = 90000;

    size_t pending_macro_run_index_ = 0; ///< Slot index awaiting run confirmation

    //
    // === Dynamic UI Containers ===
    //

    lv_obj_t* secondary_fans_list_ = nullptr; // Container for dynamic fan rows

    /// @brief Info for a secondary fan row for reactive speed updates
    struct SecondaryFanRow {
        std::string object_name;
        lv_obj_t* speed_label = nullptr;
    };
    std::vector<SecondaryFanRow> secondary_fan_rows_;    ///< Tracked for reactive updates
    std::vector<ObserverGuard> secondary_fan_observers_; ///< Per-fan speed observers
    /// Lifetime tokens for the dynamic per-fan speed subjects observed above. Per-fan
    /// subjects are destroyed/recreated on fan rediscovery; what makes the guards safe is
    /// that each token is handed to observe_int_sync(), not where the token is stored.
    /// These are copies of shared_ptrs PrinterFanState owns — it signals subject death by
    /// writing *token = false — so declaring them after the observers (destroyed first) is
    /// equivalent to declaring them before. Kept index-aligned with
    /// secondary_fan_observers_. See docs/devel/THREADING.md § 5.
    std::vector<SubjectLifetime> secondary_fan_lifetimes_;
    uint32_t fan_populate_gen_ = 0; ///< Incremented on each populate; stale callbacks skip

    lv_obj_t* secondary_temps_list_ = nullptr; // Container for dynamic temp sensor rows

    /// @brief Info for a secondary temperature sensor row for reactive temp updates
    struct SecondaryTempRow {
        std::string klipper_name; // e.g., "temperature_sensor mcu_temp"
        lv_obj_t* temp_label = nullptr;
    };
    std::vector<SecondaryTempRow> secondary_temp_rows_;   ///< Tracked for reactive updates
    std::vector<ObserverGuard> secondary_temp_observers_; ///< Per-sensor temp observers
    uint32_t temp_populate_gen_ = 0; ///< Incremented on each populate; stale callbacks skip

    //
    // === Z-Offset Banner (reactive binding - no widget caching needed) ===
    //

    lv_subject_t z_offset_delta_display_subject_{}; // Formatted delta string (e.g., "+0.05mm")
    char z_offset_delta_display_buf_[32] = {};
    ObserverGuard pending_z_offset_observer_; // Observer to update display when delta changes

    //
    // === Homing Status Subjects (for bind_style visual feedback) ===
    //

    lv_subject_t x_homed_{};            // 1 if X is homed (for position indicator)
    lv_subject_t y_homed_{};            // 1 if Y is homed (for position indicator)
    lv_subject_t xy_homed_{};           // 1 if X and Y are homed
    lv_subject_t z_homed_{};            // 1 if Z is homed
    lv_subject_t all_homed_{};          // 1 if all axes are homed
    ObserverGuard homed_axes_observer_; // Observer for PrinterState::homed_axes_

    //
    // === Position Display Subjects (for Position card) ===
    //

    lv_subject_t controls_pos_x_subject_{};
    lv_subject_t controls_pos_y_subject_{};
    lv_subject_t controls_pos_z_subject_{};
    char controls_pos_x_buf_[32] = {};
    char controls_pos_y_buf_[32] = {};
    char controls_pos_z_buf_[32] = {};
    helix::ui::PositionObserverBundle<ControlsPanel> pos_observers_;

    //
    // === Z-Offset Live Tuning ===
    //

    char controls_z_offset_buf_[16] = {};
    lv_subject_t controls_z_offset_subject_{};
    ObserverGuard gcode_z_offset_observer_;
    // ZMOD zeroes the live offset outside a print, so the row has to re-pick its
    // source when either the persisted value or the print state changes.
    ObserverGuard persisted_z_offset_observer_;
    ObserverGuard persisted_z_offset_valid_observer_;
    ObserverGuard z_offset_print_active_observer_;

    //
    // === Speed/Flow Override Subjects ===
    //

    lv_subject_t speed_override_subject_{};
    lv_subject_t flow_override_subject_{};
    char speed_override_buf_[16] = {};
    char flow_override_buf_[16] = {};
    ObserverGuard speed_factor_observer_;
    // Note: Flow factor observer uses extrude_factor from helix::PrinterState

    //
    // === Macro Slots 3 & 4 ===
    //

    std::optional<StandardMacroSlot> macro_3_slot_;
    std::optional<StandardMacroSlot> macro_4_slot_;
    lv_subject_t macro_3_visible_{};
    lv_subject_t macro_4_visible_{};
    lv_subject_t macro_3_available_{};
    lv_subject_t macro_4_available_{};
    lv_subject_t macro_3_name_{};
    lv_subject_t macro_4_name_{};
    char macro_3_name_buf_[64] = {};
    char macro_4_name_buf_[64] = {};
    lv_subject_t macro_header_visible_{};

  public:
    // === Leveling Commands (shared with MotionPanel) ===
    void handle_qgl();
    void handle_z_tilt();

  private:
    //
    // === Private Helpers ===
    //

    void setup_card_handlers();
    void register_observers();

    // Display update helpers
    void update_nozzle_temp_display();
    void update_bed_temp_display();
    void update_chamber_temp_display();
    void update_fan_display();
    void populate_secondary_fans();  // Build fan list from helix::PrinterState
    void populate_secondary_temps(); // Build temp sensor list from TemperatureSensorManager
    void update_z_offset_delta_display(int delta_microns); // Format delta for banner

    // Z-Offset save handler
    void handle_save_z_offset();
    void handle_save_z_offset_confirm();
    void handle_save_z_offset_cancel();

    //
    // === V2 Card Click Handlers (navigation to full panels) ===
    //

    void handle_quick_actions_clicked();
    void handle_nozzle_temp_clicked();
    void handle_bed_temp_clicked();
    void handle_chamber_temp_clicked();
    void handle_cooling_clicked();
    void handle_secondary_fans_clicked();
    void handle_secondary_temps_clicked();
    void handle_nozzle_target_edit();
    void handle_bed_target_edit();
    void handle_chamber_target_edit();
    void handle_custom_nozzle_confirmed(float value);
    void handle_custom_bed_confirmed(float value);
    void handle_custom_chamber_confirmed(float value);

    /**
     * @brief Show a temperature keypad dialog for a heater zone.
     *
     * @tparam Handler  Pointer-to-member for confirmed callback
     * @param title     Title shown in the keypad
     * @param cached_target   Current target in decidegrees
     * @param default_initial Default initial °C when target is 0
     * @param max_temp        Maximum allowed temperature in °C
     */
    template <void (ControlsPanel::*Handler)(float)>
    void show_temperature_keypad(const char* title, int cached_target, int default_initial,
                                 int max_temp);

    //
    // === Quick Action Button Handlers ===
    //

    void handle_home_all();
    void handle_home_x();
    void handle_home_y();
    void handle_home_xy();
    void handle_home_z();

    /**
     * @brief Execute a macro by slot index (0-3)
     *
     * Consolidates duplicate logic from handle_macro_1/2/3/4.
     * @param index Macro button index (0=macro_1, 1=macro_2, etc.)
     */
    void execute_macro(size_t index);

    /**
     * @brief Actually run a configured macro slot (bypasses confirmation)
     *
     * Called by execute_macro() directly or from the confirmation callback.
     */
    void do_execute_macro(size_t index);

    /**
     * @brief Update a single macro button's visibility and label
     *
     * Used by refresh_macro_buttons() to update each button.
     * @param macros Reference to StandardMacros instance
     * @param slot Optional slot for this button (nullopt = hide)
     * @param visible_subject Subject controlling visibility binding
     * @param available_subject Subject controlling the disabled-state binding
     * @param name_subject Subject controlling label text binding
     * @param button_num Button number for debug logging (1-4)
     */
    void update_macro_button(StandardMacros& macros, const std::optional<StandardMacroSlot>& slot,
                             lv_subject_t& visible_subject, lv_subject_t& available_subject,
                             lv_subject_t& name_subject, int button_num);

    //
    // === Speed/Flow Override Handlers ===
    //

    void handle_speed_up();
    void handle_speed_down();
    void handle_flow_up();
    void handle_flow_down();
    void update_speed_display();
    void update_flow_display();

    //
    // === Z-Offset Control Handlers ===
    //

    void handle_zoffset_tune(); ///< Open Print Tune overlay for live Z-offset tuning
    /// Reformat the Z-offset row from whichever reading is currently truthful.
    /// Takes no value: the choice between Klipper's live offset and the
    /// firmware-persisted one depends on three subjects, so every caller would
    /// otherwise have to repeat the selection. See
    /// helix::zoffset::displayed_z_offset_microns().
    void update_controls_z_offset_display();

    //
    // === Fan Slider Handler ===
    //

    void handle_fan_slider_changed(int value);

    //
    // === Calibration & Motors Handlers ===
    //

    void handle_motors_clicked();
    void handle_motors_confirm();
    void handle_motors_cancel();
    void handle_calibration_bed_mesh();
    void handle_calibration_zoffset();
    void handle_calibration_screws();
    void handle_calibration_motors();

    //
    // === V2 Card Click Trampolines (manual wiring with user_data) ===
    //

    static void on_quick_actions_clicked(lv_event_t* e);
    static void on_nozzle_temp_clicked(lv_event_t* e);
    static void on_bed_temp_clicked(lv_event_t* e);
    static void on_chamber_temp_clicked(lv_event_t* e);
    static void on_cooling_clicked(lv_event_t* e);
    static void on_secondary_fans_clicked(lv_event_t* e);
    static void on_secondary_temps_clicked(lv_event_t* e);
    static void on_nozzle_target_edit(lv_event_t* e);
    static void on_bed_target_edit(lv_event_t* e);
    static void on_chamber_target_edit(lv_event_t* e);
    static void on_motors_confirm(lv_event_t* e);
    static void on_motors_cancel(lv_event_t* e);
    static void on_save_z_offset_confirm(lv_event_t* e);
    static void on_save_z_offset_cancel(lv_event_t* e);

    //
    // === Calibration Button Trampolines (XML event_cb - global accessor) ===
    //

    static void on_calibration_bed_mesh(lv_event_t* e);
    static void on_calibration_zoffset(lv_event_t* e);
    static void on_calibration_screws(lv_event_t* e);
    static void on_calibration_motors(lv_event_t* e);

    //
    // === V2 Button Trampolines (XML event_cb - global accessor) ===
    //

    static void on_home_all(lv_event_t* e);
    static void on_home_x(lv_event_t* e);
    static void on_home_y(lv_event_t* e);
    static void on_home_xy(lv_event_t* e);
    static void on_home_z(lv_event_t* e);
    static void on_qgl(lv_event_t* e);
    static void on_z_tilt(lv_event_t* e);
    static void on_macro(lv_event_t* e);
    static void on_fan_slider_changed(lv_event_t* e);
    static void on_save_z_offset(lv_event_t* e);
    static void on_speed_up(lv_event_t* e);
    static void on_speed_down(lv_event_t* e);
    static void on_flow_up(lv_event_t* e);
    static void on_flow_down(lv_event_t* e);

    //
    // === Z-Offset Trampolines (XML event_cb - global accessor) ===
    //

    static void on_zoffset_tune(lv_event_t* e);

    void subscribe_to_secondary_fan_speeds();
    void update_secondary_fan_speed(const std::string& object_name, int speed_pct);

    void subscribe_to_secondary_temp_subjects();
    void update_secondary_temp(const std::string& klipper_name, int decidegrees);
};

// ============================================================================
// TEMPLATE DEFINITIONS (must be in header)
// ============================================================================

template <void (ControlsPanel::*Handler)(float)>
void ControlsPanel::show_temperature_keypad(const char* title, int cached_target,
                                            int default_initial, int max_temp) {
    spdlog::debug("[{}] Opening {} keypad", get_name(), title);

    int initial_deci = cached_target > 0 ? cached_target
                                         : helix::ui::temperature::degrees_to_deci(default_initial);
    ui_keypad_config_t config = {
        .initial_value = static_cast<float>(helix::ui::temperature::deci_to_degrees(initial_deci)),
        .min_value = 0.0f,
        .max_value = static_cast<float>(max_temp),
        .title_label = lv_tr(title),
        .unit_label = "°C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback =
            [](float value, void* user_data) {
                auto* self = static_cast<ControlsPanel*>(user_data);
                if (self) {
                    (self->*Handler)(value);
                }
            },
        .user_data = this};

    ui_keypad_show(&config);
}

// Global instance accessor (needed by main.cpp and XML event_cb trampolines)
ControlsPanel& get_global_controls_panel();
