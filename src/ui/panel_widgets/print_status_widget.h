// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_heater_icon_binder.h"
#include "ui_job_queue_modal.h"
#include "ui_observer_guard.h"
#include "ui_runout_guidance_modal.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#if defined(HELIX_PLATFORM_ESP32)
#include "esp_psram_thumbnail.h"
#endif
#include "print_history_manager.h"
#include "print_lifecycle_state.h" // PrintState, job_holds_machine
#include "printer_temperature_state.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace helix {

class PrinterState;
enum class PrintJobState;

// Forward-declared friend that gives unit tests access to private static
// subjects without exposing them on the production API (see [L065]).
class PrintStatusWidgetTestAccess;

class PrintStatusWidget : public PanelWidget {
    friend class PrintStatusWidgetTestAccess;

  public:
    PrintStatusWidget();
    ~PrintStatusWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    /// Factory-registration key. Exposed so callers scanning a heterogeneous
    /// widget list can match on id() and static_cast, instead of dynamic_cast —
    /// the firmware builds -fno-rtti.
    static constexpr const char* WIDGET_ID = "print_status";

    /// One row of the nozzle-tool picker: what it reads, and the Klipper
    /// extruder object it pins the temperature display to.
    struct NozzleToolOption {
        std::string label;
        std::string extruder_name;
    };

    /// The nozzle rows the picker should offer, ordered by extruder index.
    ///
    /// Sourced from the extruders PrinterTemperatureState actually discovered,
    /// never from a tool count: an AMS expands ToolState's tool list to one
    /// entry per filament slot, so deriving names from that count offered
    /// "extruder1".."extruder15" on a 4-port AD5X with one hotend - names no
    /// Klipper object answers to, every one of which the formatter then refused.
    [[nodiscard]] static std::vector<NozzleToolOption> build_nozzle_tool_options(
        const std::unordered_map<std::string, helix::ExtruderInfo>& extruders);

    /// Adopt a nozzle pin from the picker. Returns false when the formatter
    /// cannot bind the named extruder, in which case the widget records the
    /// "auto" fallback it actually applied rather than the rejected name.
    bool apply_nozzle_tool_override(const std::string& tool_key);

    const char* id() const override {
        return WIDGET_ID;
    }

    // Configuration
    void set_config(const nlohmann::json& config) override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    /// Re-check runout condition after wizard completion
    void trigger_idle_runout_check();

    /// XML event callback — opens print status panel or file browser
    static void print_card_clicked_cb(lv_event_t* e);

    /// Library row callbacks
    static void library_files_cb(lv_event_t* e);
    static void library_last_cb(lv_event_t* e);
    static void library_recent_cb(lv_event_t* e);
    static void library_queue_cb(lv_event_t* e);

    /// XML event callbacks — layout selector in configure picker
    static void print_status_layout_library_cb(lv_event_t* e);
    static void print_status_layout_detailed_cb(lv_event_t* e);

    /// XML event callback — chevron tap on nozzle temp opens tool picker
    static void print_status_nozzle_chevron_cb(lv_event_t* e);

    /// XML event callbacks — temp slot taps in the detailed-active footer
    /// open the global temp graph overlay with the matching heater mode.
    static void on_print_status_nozzle_temp_clicked(lv_event_t* e);
    static void on_print_status_bed_temp_clicked(lv_event_t* e);
    static void on_print_status_chamber_temp_clicked(lv_event_t* e);

    /// Registry of live (attached) widget instances for use-after-free prevention
    static std::unordered_set<PrintStatusWidget*>& live_instances();

    /// Parent screen used as overlay/modal anchor (e.g., temp graph overlay).
    lv_obj_t* get_parent_screen() const {
        return parent_screen_;
    }

    /// Arc thickness tier (0..4 → 4/6/8/10/12 px). Driven by C++ when the arc
    /// resizes; consumed by XML bind_style entries in print_status_detailed_active.
    /// Public so the static helper in the .cpp can publish to it without a friend.
    static inline lv_subject_t arc_thickness_tier_subject_{};

    // Test accessors (always compiled — used by unit tests)
    const std::string& layout_style_for_test() const {
        return layout_style_;
    }
    const std::string& nozzle_tool_override_for_test() const {
        return nozzle_tool_override_;
    }
    const nlohmann::json& config_for_test() const {
        return config_;
    }
    static lv_subject_t* layout_effective_subject_for_test() {
        return &layout_effective_subject_;
    }
    static lv_subject_t* show_filament_active_subject_for_test() {
        return &show_filament_active_subject_;
    }
    static lv_subject_t* view_subject_for_test() {
        return &view_subject_;
    }
    // Test-only — drive on_print_state_changed without a real PrinterState change.
    void on_print_state_changed_for_test(PrintState state) {
        on_print_state_changed(state);
    }
    // Test-only — destroy the singleton formatter regardless of refcount. Used
    // by test fixtures that reset PrinterState between cases so the next
    // formatter's observers bind to fresh subjects rather than freed memory.
    static void destroy_formatter_for_test() {
        s_formatter_.reset();
        s_formatter_refcount_ = 0;
    }

    // Test-only — instantiate the formatter without needing a real attach()
    static void ensure_formatter_for_test() {
        // Widget-static subjects (print_status_multi_tool, layout_effective, etc.)
        // are normally registered in the PrintStatusWidget ctor. Tests construct
        // only the formatter, so make sure those subjects are alive too — the
        // formatter writes to multi_tool_subject_ and tests assert on it.
        init_static_subjects();
        acquire_formatter();
    }
    // Initializes the widget's static-inline subjects + their StaticSubjectRegistry
    // deinit callback. Idempotent — guarded by *_initialized_ flags inside.
    // Called from the ctor in production, AND from ensure_formatter_for_test so
    // tests that only construct the formatter still get the subjects.
    static void init_static_subjects();
    // Take a reference on the shared DetailedFormatter, building it if this is
    // the first. Shared by the ctor and ensure_formatter_for_test() so both go
    // through the same replacement ordering (see the definition).
    static void acquire_formatter();

    // Floor at zero: destroy_formatter_for_test() zeroes the count outright, so a
    // release_ that follows one would otherwise drive the count negative and leave
    // the next acquire unable to recognise itself as the first.
    static void release_formatter_for_test() {
        if (s_formatter_refcount_ > 0 && --s_formatter_refcount_ == 0) {
            s_formatter_.reset();
        }
    }
    // Test-only — forward nozzle override to active formatter; no-op if none alive
    static void set_nozzle_tool_override_for_test(const std::string& override_name) {
        if (s_formatter_)
            s_formatter_->set_nozzle_tool_override(override_name);
    }

  private:
    /// Layout selector plus the Show Sections checkbox list, raised from edit
    /// mode. Every control applies live, so both the Done button and a tap on
    /// the backdrop commit the card rather than discarding it.
    class ConfigurePicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(ConfigurePicker)

      public:
        explicit ConfigurePicker(PrintStatusWidget& owner) : owner_(owner) {}

        /// Adopt a layout style, repaint the selector and apply it to the live
        /// widget behind the card. Driven by the two XML layout buttons.
        void select_layout(const char* style);

        /// Read the checkbox rows back into the widget's config, save, and apply
        /// the new visibility. Idempotent, so every toggle can call it.
        void apply_state();

      protected:
        const char* xml_component_name() const override {
            return "print_status_configure_picker";
        }
        /// 30% of the screen, clamped so the option rows stay readable on a 480px
        /// panel and the card does not sprawl on a 1024px one.
        CardWidth card_width() const override {
            return {30, 160, 240};
        }
        void on_created(lv_obj_t* backdrop) override;
        /// A tap outside the card commits the toggles and re-gates the widget.
        /// on_close_clicked() inherits this, so Done takes the same path.
        void on_backdrop_clicked() override;

      private:
        /// Paint the selected layout button with the primary accent and hide the
        /// Show Sections group in Detailed mode. Visuals only - no checkbox read,
        /// no save, so opening the card cannot rewrite the config.
        void apply_visuals();

        PrintStatusWidget& owner_;
    };

    /// Single-select list of the printer's tools, raised by a tap on the nozzle
    /// readout in the detailed-active footer. Picking a row pins the temperature
    /// display to that tool; a tap outside it chooses nothing.
    class NozzleToolPicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(NozzleToolPicker)

      public:
        explicit NozzleToolPicker(PrintStatusWidget& owner) : owner_(owner) {}

      protected:
        const char* xml_component_name() const override {
            return "print_status_nozzle_tool_picker";
        }
        /// The card is width="content" in XML but its option_list is width="100%",
        /// which cannot resolve against a still-empty content area (L082). The base
        /// applies this policy before on_created() builds the rows, so they size
        /// against a real width instead of collapsing to zero.
        CardWidth card_width() const override {
            return {30, 160, 240};
        }
        void on_created(lv_obj_t* backdrop) override;

      private:
        /// What a row needs to act on a tap: which tool it names, and the picker
        /// that owns it. Heap-allocated per row, hung off the row's user_data and
        /// freed by that row's own LV_EVENT_DELETE handler.
        struct RowPayload {
            NozzleToolPicker* picker;
            std::string tool_key;
        };

        PrintStatusWidget& owner_;
    };

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Cached widget references (looked up after XML creation)
    lv_obj_t* print_card_thumb_ = nullptr;          // Idle state thumbnail
    lv_obj_t* print_card_active_thumb_ = nullptr;   // Active print thumbnail
    lv_obj_t* print_card_layout_ = nullptr;         // Row/column layout container
    lv_obj_t* print_card_thumb_wrap_ = nullptr;     // Thumbnail wrapper
    lv_obj_t* print_card_info_ = nullptr;           // Info section (filename/progress)
    lv_obj_t* print_card_printing_ = nullptr;       // Active state container (preparing + printing)
    lv_obj_t* print_card_preparing_info_ = nullptr; // Preparing info section

    // Library idle state widgets
    lv_obj_t* print_card_idle_ = nullptr;          // Full library idle card
    lv_obj_t* print_card_idle_compact_ = nullptr;  // Compact idle card (1x2)
    lv_obj_t* print_card_thumb_compact_ = nullptr; // Compact thumbnail
    lv_obj_t* library_row_last_ = nullptr;         // Print Last row (for graying out)
    lv_obj_t* compact_row_last_ = nullptr;         // Compact Print Last row (for graying out)

    // Detailed-layout state containers (visibility managed by C++)
    lv_obj_t* print_card_idle_detailed_ = nullptr;     // Detailed idle hero
    lv_obj_t* print_card_printing_detailed_ = nullptr; // Detailed active body

    // Size-dependent subject for XML bindings (1 = column/2x2 mode, 0 = row/wide)
    static inline lv_subject_t column_mode_subject_;
    static inline bool column_mode_subject_initialized_ = false;

    // Physical width band (0=compact, 1=normal, 2=wide) derived from width_px in
    // on_size_changed — see panel_widget_size.h. Exposed to XML so library_body's
    // per-tier gap (panel_widget_print_status.xml bind_style entries) can react to
    // it; the raw pixel value isn't meaningful to XML the way a small band enum is.
    static inline lv_subject_t width_band_subject_;
    static inline bool width_band_subject_initialized_ = false;

    // Per-element visibility subjects — 1 = hidden, 0 = visible. XML binds via
    // <bind_flag_if_eq ... ref_value="1"/>. apply_visibility_config() computes
    // each value from show_* config + breakpoint + job queue count and writes
    // these subjects; C++ no longer toggles LV_OBJ_FLAG_HIDDEN directly.
    // Initial values (0 visible, 1 hidden for queue) are safe defaults for the
    // first XML parse; apply_visibility_config() re-derives them on attach().
    static inline lv_subject_t title_hidden_subject_;
    static inline lv_subject_t files_hidden_subject_;
    static inline lv_subject_t last_hidden_subject_;
    static inline lv_subject_t recent_hidden_subject_;
    static inline lv_subject_t queue_hidden_subject_;
    static inline lv_subject_t actions_hidden_subject_;
    static inline bool visibility_subjects_initialized_ = false;

    // Detailed-layout subjects (static inline — shared across all widget instances)
    static inline lv_subject_t layout_effective_subject_{}; // after width gating
    // Combined gate: (width band == wide) AND (filament_used > 0). Avoids the
    // phantom-row gap when filament hasn't started extruding yet.
    static inline lv_subject_t show_filament_active_subject_{};
    static inline lv_subject_t multi_tool_subject_{}; // 1 when tool_count > 1
    // Resolved thumbnail path for the detailed-idle hero; written by
    // reset_print_card_to_idle alongside the lv_image_set_src calls on the
    // Library-mode thumbs, so all three idle thumbnails share the same source.
    // The initializer here is never observed — init_static_subjects()
    // overwrites the buffer with the asset-root-resolved benchy path before
    // lv_subject_init_string publishes it. A static array needs a constant
    // initializer, so the accessor cannot be called from this line.
    static inline char idle_thumb_path_buf_[512] = "A:assets/images/benchy_thumbnail_white.png";
    static inline lv_subject_t idle_thumb_path_subject_{};
    // Single subject driving visibility of all five card-body siblings:
    //   0 = idle_library_full   (print_card_idle)
    //   1 = idle_library_compact (print_card_idle_compact)
    //   2 = idle_detailed       (print_card_idle_detailed)
    //   3 = active_library      (print_card_layout)
    //   4 = active_detailed     (print_card_printing_detailed)
    // C++ sets this; XML binds with bind_flag_if_not_eq per sibling.
    static inline lv_subject_t view_subject_{};
    static inline bool detailed_subjects_initialized_ = false;

    // Compact mode and state tracking
    bool is_active_ = false; // job_holds_machine() (drives view_subject_)
    bool is_compact_ = false;
    bool is_column_ = false;
    bool last_print_available_ = false;
    // Cached granted pixel size, for picker-dismiss re-gating (regate_after_configure
    // re-runs on_size_changed after a layout_style change, and needs the widget's last
    // known real size — colspan/rowspan are no longer read).
    int last_width_px_ = 0;
    int last_height_px_ = 0;

    // PrinterState reference for subject access
    PrinterState& printer_state_;

    // Observers (RAII cleanup via ObserverGuard)
    ObserverGuard print_state_observer_;
    ObserverGuard print_thumbnail_path_observer_;
#if defined(HELIX_PLATFORM_ESP32)
    ObserverGuard print_psram_thumb_observer_; ///< Ditto, via the PSRAM generation counter
    /// PSRAM-resident thumbnail currently shown in print_card_active_thumb_.
    /// There is no cache file on this platform, so this shared_ptr is what keeps
    /// the image src's buffer alive. Main-thread only (its destructor drops the
    /// LVGL image cache entry).
    std::shared_ptr<helix::ui::EspPsramThumbnail> esp_thumbnail_;
#endif
    ObserverGuard filament_runout_observer_;
    ObserverGuard job_queue_count_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard breakpoint_observer_;

    // Guards async thumbnail callbacks and history observer from use-after-free
    helix::AsyncLifetimeGuard lifetime_;

    // Supersedes in-flight idle thumbnail loads. Bumped by every
    // reset_print_card_to_idle() through ThumbnailLoadContext, so a load started
    // for the previous history head can no longer land on top of a newer one.
    // Per-instance, unlike the static-inline subjects above: two dashboard
    // widgets resolve independently, and a shared counter would have each
    // cancelling the other's fetch.
    std::atomic<uint32_t> idle_thumb_generation_{0};

    // Thermal tint for the detailed-active heater icons. Plain by-value members
    // of THIS instance — never on the shared/refcounted s_formatter_ below.
    // Dashboard widget instances are recycled by the panel manager: attach A ->
    // detach A -> attach B, and A's deferred LV_EVENT_DELETE fires after B has
    // taken over. Per-instance ownership is what keeps that safe, since each
    // animator's delete callback carries its own `this` (see the equivalent
    // guard for the progress arc in attach_arc(), below).
    helix::ui::HeaterIconBinder nozzle_icon_binder_;
    helix::ui::HeaterIconBinder bed_icon_binder_;
    helix::ui::HeaterIconBinder chamber_icon_binder_;

    // History observer for updating idle thumbnail when history loads
    helix::HistoryChangedCallback history_changed_cb_;

    // Filament runout modal
    RunoutGuidanceModal runout_modal_;
    bool runout_modal_shown_ = false;

    // Job queue
    helix::JobQueueModal job_queue_modal_;

    // First-instance formatter singleton — owns observers + formatted subjects for Detailed layout.
    // Created on the first widget attach, destroyed on the last detach.
    class DetailedFormatter {
      public:
        DetailedFormatter();
        ~DetailedFormatter();
        DetailedFormatter(const DetailedFormatter&) = delete;
        DetailedFormatter& operator=(const DetailedFormatter&) = delete;
        DetailedFormatter(DetailedFormatter&&) = delete;
        DetailedFormatter& operator=(DetailedFormatter&&) = delete;

        /// Override is "" or "auto" for auto-tracking; else extruder name like "extruder1".
        /// Silently falls back to auto if the named subject doesn't resolve.
        // Returns true if the override resolved to a real subject. False
        // when override_name was non-empty/non-"auto" but the per-tool subject
        // didn't exist (formatter falls back to "auto"); callers should clear
        // their cached override / persisted config to match.
        bool set_nozzle_tool_override(const std::string& override_name);

        /// Called by widget attach() to hand the arc widget to the formatter
        void attach_arc(lv_obj_t* arc);

        /// Re-fit the arc to a square sized from its parent column (call from on_size_changed)
        void resize_arc();

      private:
        ObserverGuard arc_value_observer_;
        lv_obj_t* arc_widget_ = nullptr;
        std::string current_nozzle_override_ = "auto";

        SubjectManager subjects_;

        // Buffers backing string subjects
        char layer_text_buf_[64];       // "Layer ~9999 / 9999 (123.4mm)", translated
        char time_text_buf_[40];        // "12h 34m / 99h 99m"
        char filament_text_buf_[32];    // "1234.5m / 9999.9m"
        char nozzle_text_buf_[32];      // "265 / 270°C" — kept for tool_override test
        char nozzle_tool_label_buf_[8]; // "T0", "T9"
        char idle_filename_buf_[160];
        char idle_when_buf_[64]; // "Completed 2 hours ago"
        char idle_meta_buf_[64]; // "12.4m filament • 4h 12m"

        // String + int subjects (XML-registered)
        lv_subject_t layer_text_subject_;
        lv_subject_t time_text_subject_;
        lv_subject_t filament_text_subject_;
        lv_subject_t nozzle_text_subject_;
        lv_subject_t nozzle_tool_label_subject_;
        // Proxy temp subjects (decidegrees, int) so the temp_display widget in the
        // detailed XML follows the pinned tool when nozzle_tool_override is set.
        // The formatter's nozzle_temp/target observers re-bind on pin change and
        // copy the source value into these proxies; XML binds temp_display to them.
        lv_subject_t nozzle_current_subject_;
        lv_subject_t nozzle_target_subject_;
        lv_subject_t idle_filename_subject_;
        lv_subject_t idle_when_subject_;
        lv_subject_t idle_meta_subject_;
        lv_subject_t idle_has_last_subject_;

        // Print-state observers (wired in constructor, RAII cleanup via ObserverGuard)
        ObserverGuard layer_current_observer_;
        ObserverGuard layer_total_observer_;
        ObserverGuard elapsed_observer_;
        ObserverGuard time_left_observer_;
        ObserverGuard filament_used_observer_;

        // Nozzle temp observers — paired SubjectLifetimes per [L084] for per-tool pinning.
        // Lifetimes declared before observers so they destruct AFTER observers in ~dtor
        // (the observer's cleanup queries the lifetime; reverse order = UAF on dead lifetime).
        // In auto mode the static active_extruder subjects are observed, lifetimes unused.
        SubjectLifetime nozzle_temp_lifetime_;
        SubjectLifetime nozzle_target_lifetime_;
        ObserverGuard nozzle_temp_observer_;
        ObserverGuard nozzle_target_observer_;

        /// Bound to ToolState's tools_version subject, not tool_count: the badge
        /// gate counts extruders, and a spool/status refresh can change the
        /// extruder mapping without moving the tool count.
        ObserverGuard tools_version_observer_;
        ObserverGuard active_tool_observer_;

        void update_layer_text();
        void update_time_text();
        void update_filament_text();
        void update_nozzle_text();
        void update_multi_tool();
        void update_tool_label();
        void update_idle_fields();

        helix::HistoryChangedCallback history_cb_;
    };

    static inline std::unique_ptr<DetailedFormatter> s_formatter_;
    static inline int s_formatter_refcount_ = 0;

    // Print card update methods
    [[nodiscard]] std::string get_last_print_thumbnail_path() const;
    /// Source gcode mtime for the history entry get_last_print_thumbnail_path()
    /// resolved from, or 0 when history is unavailable or Moonraker omitted it.
    /// Feeds ThumbnailRequest::source_modified so a re-slice under the same
    /// filename invalidates the cached render instead of being served forever.
    [[nodiscard]] time_t get_last_print_source_modified() const;
    void handle_print_card_clicked();
    void on_print_state_changed(PrintState state);
    void on_print_thumbnail_path_changed(const char* path);
#if defined(HELIX_PLATFORM_ESP32)
    /// Pull the current PSRAM thumbnail from PrinterState, hold a reference,
    /// and point print_card_active_thumb_ at its descriptor. Main thread only;
    /// no-op when the widget is unattached or nothing has been fetched yet.
    /// Called from the generation observer AND from attach(), because widget
    /// instances are recycled and a fresh attach must re-apply the image.
    void apply_esp_psram_thumbnail();
#endif
    void reset_print_card_to_idle();
    // Publish one resolved idle thumbnail everywhere it is shown: the two
    // imperative Library-mode thumbs and idle_thumb_path_subject_, which the
    // detailed-idle hero reads through bind_src. A member rather than a lambda
    // inside reset_print_card_to_idle() so the async fetch completion publishes
    // to all three too, instead of leaving the hero on the placeholder.
    void set_thumb_on_widgets(const char* src);
    // Schedule reset_print_card_to_idle() on the next LVGL tick via lv_async_call.
    // Required when called from UpdateQueue::process_pending() contexts (subject
    // observers, token.defer bodies): reset_print_card_to_idle() does synchronous
    // lv_image_set_src which cascades into lv_obj_update_layout up to the page
    // grid; if populate_page is mid-rebuild, grid_update reads freed track data
    // and SIGSEGVs (J2URYGSM AD5M / SY6JLLKJ / FFATPQWB Pi5).
    void defer_reset_print_card_to_idle();
    // Schedule the active-print thumbnail write on the next LVGL tick. Same
    // reasoning as defer_reset_print_card_to_idle() above — the observer body
    // runs inside UpdateQueue::process_pending(), and lv_image_set_src there
    // cascades into lv_obj_update_layout across a page grid populate_page may
    // still be rebuilding. The path is copied because the subject can publish
    // again before the tick fires.
    void defer_apply_active_thumbnail(const char* path);
    void update_idle_compact_mode();
    void update_active_layout_mode();
    // Apply the imperative print-card row/column flex layout for is_column_.
    // Called from on_size_changed and attach() (recycled-instance re-sync, #1109).
    void apply_card_layout();
    void update_last_print_availability();

    // Library action handlers
    void handle_library_files();
    void handle_library_last();
    void handle_library_recent();
    void handle_library_queue();
    void update_job_queue_row_visibility();

    // Filament runout handling
    void check_and_show_idle_runout_modal();
    void show_idle_runout_modal();

    /**
     * @brief Dispatch the idle runout dialog's "Load filament" action.
     *
     * The fourth surface on the shared plan_load() ladder — AMS backend, then
     * the configured LOAD_FILAMENT macro, then raw gcode. It used to
     * set_active(PanelId::Filament) instead, which navigated out from under the
     * dialog and left the user to find the Load button themselves.
     *
     * Same two constraints as FilamentRunoutHandler, and for the same reasons:
     * ParamPolicy::Suppress (a parameter modal would stack on top of this live
     * dialog, whose observers keep firing underneath it), and a refusal never
     * navigates. Main thread only — reached from the modal's button callback
     * after its LifetimeToken check.
     */
    void dispatch_load();

    // Configuration state
    nlohmann::json config_;
    std::string layout_style_ = "library";      // "library" | "detailed"
    std::string nozzle_tool_override_ = "auto"; // "auto" | extruder name
    bool show_title_ = true;
    bool show_print_files_ = true;
    bool show_reprint_last_ = true;
    bool show_recent_prints_ = true;
    bool show_job_queue_ = true;

    void show_configure_picker();
    void apply_visibility_config();
    void recompute_actions_visibility();
    // Re-run width gating against the last granted size, so a layout_style change
    // made in the configure picker reaches the visible widget the moment the card
    // closes.
    void regate_after_configure();
    // Recompute the view subject from (is_active_, layout_style_, is_compact_).
    // Drives bind_flag_if_not_eq on the five card-body siblings.
    void update_view_subject();

    void show_nozzle_tool_picker(lv_obj_t* anchor);

    // The two context menus this widget raises
    ConfigurePicker configure_picker_{*this};
    NozzleToolPicker nozzle_picker_{*this};
};

} // namespace helix
