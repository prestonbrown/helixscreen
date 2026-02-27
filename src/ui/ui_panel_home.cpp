// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_home.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_ams.h"
#include "ui_panel_print_status.h"
#include "ui_printer_manager_overlay.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "display_settings_manager.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "injection_point_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <memory>

using namespace helix;

HomePanel::HomePanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Initialize buffer contents with default values
    std::strcpy(status_buffer_, "Welcome to HelixScreen");

    // Subscribe to PrinterState subjects (ObserverGuard handles cleanup)
    using helix::ui::observe_int_sync;
    using helix::ui::observe_print_state;
    using helix::ui::observe_string;

    // Subscribe to print state for dynamic print card updates
    print_state_observer_ = observe_print_state<HomePanel>(
        printer_state_.get_print_state_enum_subject(), this,
        [](HomePanel* self, PrintJobState state) { self->on_print_state_changed(state); });
    print_progress_observer_ = observe_int_sync<HomePanel>(
        printer_state_.get_print_progress_subject(), this,
        [](HomePanel* self, int /*progress*/) { self->on_print_progress_or_time_changed(); });
    print_time_left_observer_ = observe_int_sync<HomePanel>(
        printer_state_.get_print_time_left_subject(), this,
        [](HomePanel* self, int /*time*/) { self->on_print_progress_or_time_changed(); });
    print_thumbnail_path_observer_ = observe_string<HomePanel>(
        printer_state_.get_print_thumbnail_path_subject(), this,
        [](HomePanel* self, const char* path) { self->on_print_thumbnail_path_changed(path); });

    spdlog::debug("[{}] Subscribed to PrinterState extruder temperature and target", get_name());
    spdlog::debug("[{}] Subscribed to PrinterState print state/progress/time/thumbnail",
                  get_name());

    // Subscribe to filament runout for idle modal
    auto& fsm = helix::FilamentSensorManager::instance();
    filament_runout_observer_ = observe_int_sync<HomePanel>(
        fsm.get_any_runout_subject(), this, [](HomePanel* self, int any_runout) {
            spdlog::debug("[{}] Filament runout subject changed: {}", self->get_name(), any_runout);
            if (any_runout == 1) {
                self->check_and_show_idle_runout_modal();
            } else {
                self->runout_modal_shown_ = false;
            }
        });
    spdlog::debug("[{}] Subscribed to filament_any_runout subject", get_name());

    // Subscribe to printer image changes for immediate refresh
    image_changed_observer_ = observe_int_sync<HomePanel>(
        helix::PrinterImageManager::instance().get_image_changed_subject(), this,
        [](HomePanel* self, int /*ver*/) {
            // Clear cache so refresh_printer_image() actually applies the new image
            self->last_printer_image_path_.clear();
            self->refresh_printer_image();
        });
}

HomePanel::~HomePanel() {
    // Deinit subjects FIRST - disconnects observers before subject memory is freed
    // This prevents crashes during lv_deinit() when widgets try to unsubscribe
    deinit_subjects();

    // Gate observers watch external subjects (capabilities, klippy_state) that may
    // already be freed. Clear unconditionally — deinit_subjects() may have been
    // skipped if subjects_initialized_ was already false from a prior call.
    helix::PanelWidgetManager::instance().clear_gate_observers("home");
    helix::PanelWidgetManager::instance().unregister_rebuild_callback("home");

    // Detach active PanelWidget instances
    for (auto& w : active_widgets_) {
        w->detach();
    }
    active_widgets_.clear();

    // Clean up timers and animations - must be deleted explicitly before LVGL shutdown
    // Check lv_is_initialized() to avoid crash during static destruction
    if (lv_is_initialized()) {
        // Stop tip fade animations (var=this, not an lv_obj_t*, so lv_obj_delete won't clean them)
        // Clear flag first so completion callbacks become no-ops if triggered synchronously
        tip_animating_ = false;
        lv_anim_delete(this, nullptr);

        if (snapshot_timer_) {
            lv_timer_delete(snapshot_timer_);
            snapshot_timer_ = nullptr;
        }
        if (tip_rotation_timer_) {
            lv_timer_delete(tip_rotation_timer_);
            tip_rotation_timer_ = nullptr;
        }

        // Free cached printer image snapshot
        if (cached_printer_snapshot_) {
            lv_draw_buf_destroy(cached_printer_snapshot_);
            cached_printer_snapshot_ = nullptr;
        }
    }
}

void HomePanel::init_subjects() {
    using helix::ui::observe_int_sync;

    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize subjects with default values
    // Note: LED state (led_state) is managed by PrinterState and already registered
    UI_MANAGED_SUBJECT_STRING(status_subject_, status_buffer_, "Welcome to HelixScreen",
                              "status_text", subjects_);

    // Network subjects (home_network_icon_state, network_label) are owned by
    // NetworkWidget and initialized via PanelWidgetManager::init_widget_subjects()
    // before this function runs. HomePanel looks them up by name when needed.

    // Printer type and host - two subjects for flexible XML layout
    UI_MANAGED_SUBJECT_STRING(printer_type_subject_, printer_type_buffer_, "", "printer_type_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(printer_host_subject_, printer_host_buffer_, "", "printer_host_text",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(printer_info_visible_, 0, "printer_info_visible", subjects_);

    // Register panel-level event callbacks BEFORE loading XML.
    // Widget-specific callbacks are self-registered in each widget's attach().
    register_xml_callbacks({
        {"print_card_clicked_cb", print_card_clicked_cb},
        {"tip_text_clicked_cb", tip_text_clicked_cb},
        {"printer_status_clicked_cb", printer_status_clicked_cb},
        {"printer_manager_clicked_cb", printer_manager_clicked_cb},
        {"ams_clicked_cb", ams_clicked_cb},
    });

    // Subscribe to AmsState slot_count to show/hide AMS indicator
    // AmsState::init_subjects() is called in main.cpp before us
    ams_slot_count_observer_ = observe_int_sync<HomePanel>(
        AmsState::instance().get_slot_count_subject(), this,
        [](HomePanel* self, int slot_count) { self->update_ams_indicator(slot_count); });

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "HomePanelSubjects", []() { get_global_home_panel().deinit_subjects(); });

    spdlog::debug("[{}] Registered subjects and event callbacks", get_name());

    // Set initial tip of the day
    update_tip_of_day();
}

void HomePanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // Release gate observers BEFORE subjects are freed — they observe external
    // subjects (capabilities, klippy_state) that may be destroyed during shutdown.
    helix::PanelWidgetManager::instance().clear_gate_observers("home");

    // SubjectManager handles all lv_subject_deinit() calls via RAII
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

void HomePanel::setup_widget_gate_observers() {
    auto& mgr = helix::PanelWidgetManager::instance();
    mgr.setup_gate_observers("home", [this]() { populate_widgets(); });
}

void HomePanel::populate_widgets() {
    lv_obj_t* container = lv_obj_find_by_name(panel_, "widget_container");
    if (!container) {
        spdlog::error("[{}] widget_container not found", get_name());
        return;
    }

    // Detach active PanelWidget instances before clearing
    for (auto& w : active_widgets_) {
        w->detach();
    }
    active_widgets_.clear();

    // Delegate generic widget creation to the manager
    active_widgets_ = helix::PanelWidgetManager::instance().populate_widgets("home", container);

    // HomePanel-specific: cache widget references for tip animation, print card, etc.
    cache_widget_references();
}

void HomePanel::cache_widget_references() {
    // Cache tip label for fade animation
    tip_label_ = lv_obj_find_by_name(panel_, "status_text_label");
    if (!tip_label_) {
        spdlog::warn("[{}] Could not find status_text_label for tip animation", get_name());
    }

    // Look up print card widgets for dynamic updates during printing
    print_card_thumb_ = lv_obj_find_by_name(panel_, "print_card_thumb");
    print_card_active_thumb_ = lv_obj_find_by_name(panel_, "print_card_active_thumb");
    print_card_label_ = lv_obj_find_by_name(panel_, "print_card_label");
}

void HomePanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::debug("[{}] Setting up...", get_name());

    // Dynamically populate status card widgets from PanelWidgetConfig
    populate_widgets();

    // Observe hardware gate subjects so widgets appear/disappear when
    // capabilities change (e.g. power devices discovered after startup).
    // Also observe klippy_state for firmware_restart conditional injection.
    setup_widget_gate_observers();

    // Register rebuild callback so settings overlay toggle changes take effect immediately
    helix::PanelWidgetManager::instance().register_rebuild_callback(
        "home", [this]() { populate_widgets(); });

    // Start tip rotation timer (60 seconds = 60000ms)
    if (!tip_rotation_timer_) {
        tip_rotation_timer_ = lv_timer_create(tip_rotation_timer_cb, 60000, this);
        spdlog::debug("[{}] Started tip rotation timer (60s interval)", get_name());
    }

    // Load printer image from config (if available)
    apply_printer_config();

    // Check initial AMS state and show indicator if AMS is already available
    // (The observer may have fired before panel_ was set during init_subjects)
    int slot_count = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    if (slot_count > 0) {
        update_ams_indicator(slot_count);
    }

    // Print card widgets are already cached by cache_widget_references() via populate_widgets()
    if (print_card_thumb_ && print_card_active_thumb_ && print_card_label_) {
        spdlog::debug("[{}] Found print card widgets for dynamic updates", get_name());

        // Check initial print state (observer may have fired before setup)
        auto state = static_cast<PrintJobState>(
            lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
        if (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED) {
            // Already printing - load thumbnail and update label
            on_print_state_changed(state);
        }
    }

    // Register plugin injection point for home panel widgets
    lv_obj_t* widget_area = lv_obj_find_by_name(panel_, "panel_widget_area");
    if (widget_area) {
        helix::plugin::InjectionPointManager::instance().register_point("panel_widget_area",
                                                                        widget_area);
        spdlog::debug("[{}] Registered injection point: panel_widget_area", get_name());
    }

    spdlog::debug("[{}] Setup complete!", get_name());
}

void HomePanel::on_activate() {
    // Resume tip rotation timer when panel becomes visible
    if (!tip_rotation_timer_) {
        tip_rotation_timer_ = lv_timer_create(tip_rotation_timer_cb, 60000, this);
        spdlog::debug("[{}] Resumed tip rotation timer", get_name());
    }

    // Re-check printer image (may have changed in settings overlay)
    refresh_printer_image();

    // Activate all behavioral widgets (network polling, power refresh, etc.)
    for (auto& w : active_widgets_) {
        w->on_activate();
    }

    // Start Spoolman polling for AMS mini status updates
    AmsState::instance().start_spoolman_polling();
}

void HomePanel::on_deactivate() {
    // Deactivate all behavioral widgets
    for (auto& w : active_widgets_) {
        w->on_deactivate();
    }

    AmsState::instance().stop_spoolman_polling();

    // Cancel pending snapshot timer (no point snapshotting while hidden)
    if (snapshot_timer_) {
        lv_timer_delete(snapshot_timer_);
        snapshot_timer_ = nullptr;
    }

    // Cancel any in-flight tip fade animations (var=this, not an lv_obj_t*)
    if (tip_animating_) {
        tip_animating_ = false;
        lv_anim_delete(this, nullptr);
    }

    // Stop tip rotation timer when panel is hidden (saves CPU)
    if (tip_rotation_timer_) {
        lv_timer_delete(tip_rotation_timer_);
        tip_rotation_timer_ = nullptr;
        spdlog::debug("[{}] Stopped tip rotation timer", get_name());
    }
}

void HomePanel::update_tip_of_day() {
    auto tip = TipsManager::get_instance()->get_random_unique_tip();

    if (!tip.title.empty()) {
        // Use animated transition if label is available and not already animating
        if (tip_label_ && !tip_animating_) {
            start_tip_fade_transition(tip);
        } else {
            // Fallback: instant update (initial load or animation in progress)
            current_tip_ = tip;
            std::snprintf(status_buffer_, sizeof(status_buffer_), "%s", tip.title.c_str());
            lv_subject_copy_string(&status_subject_, status_buffer_);
            spdlog::trace("[{}] Updated tip (instant): {}", get_name(), tip.title);
        }
    } else {
        spdlog::warn("[{}] Failed to get tip, keeping current", get_name());
    }
}

// Animation duration constants
static constexpr uint32_t TIP_FADE_DURATION_MS = 300;

void HomePanel::start_tip_fade_transition(const PrintingTip& new_tip) {
    if (!tip_label_ || tip_animating_) {
        return;
    }

    // Store the pending tip to apply after fade-out
    pending_tip_ = new_tip;
    tip_animating_ = true;

    spdlog::debug("[{}] Starting tip fade transition to: {}", get_name(), new_tip.title);

    // Skip animation if disabled - apply text immediately
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        current_tip_ = pending_tip_;
        std::snprintf(status_buffer_, sizeof(status_buffer_), "%s", pending_tip_.title.c_str());
        lv_subject_copy_string(&status_subject_, status_buffer_);
        lv_obj_set_style_opa(tip_label_, LV_OPA_COVER, LV_PART_MAIN);
        tip_animating_ = false;
        spdlog::debug("[{}] Animations disabled - applied tip instantly", get_name());
        return;
    }

    // Fade out animation
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_values(&anim, 255, 0);
    lv_anim_set_duration(&anim, TIP_FADE_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);

    // Execute callback: update opacity on each frame
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        auto* self = static_cast<HomePanel*>(var);
        if (self->tip_label_) {
            lv_obj_set_style_opa(self->tip_label_, static_cast<lv_opa_t>(value), LV_PART_MAIN);
        }
    });

    // Completion callback: apply new text and start fade-in
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* self = static_cast<HomePanel*>(a->var);
        self->apply_pending_tip();
    });

    lv_anim_start(&anim);
}

void HomePanel::apply_pending_tip() {
    // Apply the pending tip text
    current_tip_ = pending_tip_;
    std::snprintf(status_buffer_, sizeof(status_buffer_), "%s", pending_tip_.title.c_str());
    lv_subject_copy_string(&status_subject_, status_buffer_);

    spdlog::debug("[{}] Applied pending tip: {}", get_name(), pending_tip_.title);

    // Skip animation if disabled - show at full opacity immediately
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        if (tip_label_) {
            lv_obj_set_style_opa(tip_label_, LV_OPA_COVER, LV_PART_MAIN);
        }
        tip_animating_ = false;
        return;
    }

    // Fade in animation
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_values(&anim, 0, 255);
    lv_anim_set_duration(&anim, TIP_FADE_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);

    // Execute callback: update opacity on each frame
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        auto* self = static_cast<HomePanel*>(var);
        if (self->tip_label_) {
            lv_obj_set_style_opa(self->tip_label_, static_cast<lv_opa_t>(value), LV_PART_MAIN);
        }
    });

    // Completion callback: mark animation as done
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* self = static_cast<HomePanel*>(a->var);
        self->tip_animating_ = false;
    });

    lv_anim_start(&anim);
}

void HomePanel::handle_print_card_clicked() {
    // Check if a print is in progress
    if (!printer_state_.can_start_new_print()) {
        // Print in progress - show print status overlay
        spdlog::info("[{}] Print card clicked - showing print status (print in progress)",
                     get_name());

        extern PrintStatusPanel& get_global_print_status_panel();
        lv_obj_t* status_panel = get_global_print_status_panel().get_panel();
        if (status_panel) {
            NavigationManager::instance().register_overlay_instance(
                status_panel, &get_global_print_status_panel());
            NavigationManager::instance().push_overlay(status_panel);
        } else {
            spdlog::error("[{}] Print status panel not available", get_name());
        }
    } else {
        // No print in progress - navigate to print select panel
        spdlog::info("[{}] Print card clicked - navigating to print select panel", get_name());
        NavigationManager::instance().set_active(PanelId::PrintSelect);
    }
}

void HomePanel::handle_tip_text_clicked() {
    if (current_tip_.title.empty()) {
        spdlog::warn("[{}] No tip available to display", get_name());
        return;
    }

    spdlog::info("[{}] Tip text clicked - showing detail dialog", get_name());

    // Use alert helper which auto-handles OK button to close
    helix::ui::modal_show_alert(current_tip_.title.c_str(), current_tip_.content.c_str(),
                                ModalSeverity::Info);
}

void HomePanel::handle_tip_rotation_timer() {
    update_tip_of_day();
}

void HomePanel::handle_printer_status_clicked() {
    spdlog::info("[{}] Printer status icon clicked - navigating to advanced settings", get_name());

    // Navigate to advanced settings panel
    NavigationManager::instance().set_active(PanelId::Advanced);
}

void HomePanel::handle_printer_manager_clicked() {
    spdlog::info("[{}] Printer image clicked - opening Printer Manager overlay", get_name());

    auto& overlay = get_printer_manager_overlay();

    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(parent_screen_);
        NavigationManager::instance().register_overlay_instance(overlay.get_root(), &overlay);
    }

    // Push overlay onto navigation stack
    NavigationManager::instance().push_overlay(overlay.get_root());
}

void HomePanel::handle_ams_clicked() {
    spdlog::info("[{}] AMS indicator clicked - opening AMS panel overlay", get_name());

    // Open AMS panel overlay for multi-filament management
    auto& ams_panel = get_global_ams_panel();
    if (!ams_panel.are_subjects_initialized()) {
        ams_panel.init_subjects();
    }
    lv_obj_t* panel_obj = ams_panel.get_panel();
    if (panel_obj) {
        NavigationManager::instance().push_overlay(panel_obj);
    }
}

void HomePanel::apply_printer_config() {
    using helix::ui::observe_int_sync;

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[{}] apply_printer_config: Config not available", get_name());
        return;
    }

    // Update printer type in PrinterState (triggers capability cache refresh)
    std::string printer_type = config->get<std::string>(helix::wizard::PRINTER_TYPE, "");
    printer_state_.set_printer_type_sync(printer_type);

    // Update printer image
    refresh_printer_image();

    // Update printer type/host overlay
    // Always visible (even for localhost) to maintain consistent flex layout.
    // Hidden flag removes elements from flex, causing printer image to scale differently.
    std::string host = config->get<std::string>(helix::wizard::MOONRAKER_HOST, "");

    if (host.empty() || host == "127.0.0.1" || host == "localhost") {
        // Space keeps the text_small at its font height for consistent layout
        std::strncpy(printer_type_buffer_, " ", sizeof(printer_type_buffer_) - 1);
        lv_subject_copy_string(&printer_type_subject_, printer_type_buffer_);
        lv_subject_set_int(&printer_info_visible_, 1);
    } else {
        std::strncpy(printer_type_buffer_, printer_type.empty() ? "Printer" : printer_type.c_str(),
                     sizeof(printer_type_buffer_) - 1);
        std::strncpy(printer_host_buffer_, host.c_str(), sizeof(printer_host_buffer_) - 1);
        lv_subject_copy_string(&printer_type_subject_, printer_type_buffer_);
        lv_subject_copy_string(&printer_host_subject_, printer_host_buffer_);
        lv_subject_set_int(&printer_info_visible_, 1);
    }
}

void HomePanel::refresh_printer_image() {
    if (!subjects_initialized_ || !panel_)
        return;

    lv_display_t* disp = lv_display_get_default();
    int screen_width = disp ? lv_display_get_horizontal_resolution(disp) : 800;

    // Resolve the image path (lightweight string work) before touching LVGL widgets
    auto& pim = helix::PrinterImageManager::instance();
    std::string resolved_path = pim.get_active_image_path(screen_width);
    if (resolved_path.empty()) {
        // Auto-detect from printer type using PrinterImages
        Config* config = Config::get_instance();
        std::string printer_type =
            config ? config->get<std::string>(helix::wizard::PRINTER_TYPE, "") : "";
        resolved_path = PrinterImages::get_best_printer_image(printer_type);
    }

    // Skip redundant work if the image path hasn't changed
    if (resolved_path == last_printer_image_path_) {
        return;
    }
    last_printer_image_path_ = resolved_path;

    // Free old snapshot — image source is about to change
    if (cached_printer_snapshot_) {
        lv_obj_t* img = lv_obj_find_by_name(panel_, "printer_image");
        if (img) {
            // Clear source before destroying buffer it points to
            // Note: must use NULL, not "" — empty string byte 0x00 gets misclassified
            // as LV_IMAGE_SRC_VARIABLE by lv_image_src_get_type
            lv_image_set_src(img, nullptr);
            // Restore contain alignment so the original image scales correctly
            // during the ~50ms gap before the new snapshot is taken
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
        }
        lv_draw_buf_destroy(cached_printer_snapshot_);
        cached_printer_snapshot_ = nullptr;
    }

    lv_obj_t* img = lv_obj_find_by_name(panel_, "printer_image");
    if (img) {
        lv_image_set_src(img, resolved_path.c_str());
        spdlog::debug("[{}] Printer image: '{}'", get_name(), resolved_path);
    }
    schedule_printer_image_snapshot();
}

void HomePanel::schedule_printer_image_snapshot() {
    // Cancel any pending snapshot timer
    if (snapshot_timer_) {
        lv_timer_delete(snapshot_timer_);
        snapshot_timer_ = nullptr;
    }

    // Defer snapshot until after layout resolves (~50ms)
    snapshot_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<HomePanel*>(lv_timer_get_user_data(timer));
            if (self) {
                self->snapshot_timer_ = nullptr; // Timer is one-shot, about to be deleted
                self->take_printer_image_snapshot();
            }
            lv_timer_delete(timer);
        },
        50, this);
    lv_timer_set_repeat_count(snapshot_timer_, 1);
}

void HomePanel::take_printer_image_snapshot() {
    if (!panel_)
        return;

    lv_obj_t* img = lv_obj_find_by_name(panel_, "printer_image");
    if (!img)
        return;

    // Only snapshot if the widget has resolved to a non-zero size
    int32_t w = lv_obj_get_width(img);
    int32_t h = lv_obj_get_height(img);
    if (w <= 0 || h <= 0) {
        spdlog::debug("[{}] Printer image not laid out yet ({}x{}), skipping snapshot", get_name(),
                      w, h);
        return;
    }

    lv_draw_buf_t* snapshot = lv_snapshot_take(img, LV_COLOR_FORMAT_ARGB8888);
    if (!snapshot) {
        spdlog::warn("[{}] Failed to take printer image snapshot", get_name());
        return;
    }

    // Free previous snapshot if any
    if (cached_printer_snapshot_) {
        lv_draw_buf_destroy(cached_printer_snapshot_);
    }
    cached_printer_snapshot_ = snapshot;

    // Diagnostic: verify snapshot header before setting as source
    uint32_t snap_w = snapshot->header.w;
    uint32_t snap_h = snapshot->header.h;
    uint32_t snap_magic = snapshot->header.magic;
    uint32_t snap_cf = snapshot->header.cf;
    spdlog::debug("[{}] Snapshot header: magic=0x{:02x} cf={} {}x{} data={}", get_name(),
                  snap_magic, snap_cf, snap_w, snap_h, fmt::ptr(snapshot->data));

    // Swap image source to the pre-scaled snapshot buffer — LVGL blits 1:1, no scaling
    lv_image_set_src(img, cached_printer_snapshot_);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);

    spdlog::debug("[{}] Printer image snapshot cached ({}x{}, {} bytes)", get_name(), snap_w,
                  snap_h, snap_w * snap_h * 4);
}

void HomePanel::print_card_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] print_card_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_print_card_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::tip_text_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] tip_text_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_tip_text_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::printer_status_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] printer_status_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_printer_status_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::printer_manager_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] printer_manager_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_printer_manager_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::ams_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] ams_clicked_cb");
    (void)e;
    extern HomePanel& get_global_home_panel();
    get_global_home_panel().handle_ams_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::tip_rotation_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<HomePanel*>(lv_timer_get_user_data(timer));
    if (self) {
        self->handle_tip_rotation_timer();
    }
}

void HomePanel::update_ams_indicator(int /* slot_count */) {
    // AMS mini status widget auto-updates via observers bound to AmsState subjects.
    // No additional work needed here.
}

// ============================================================================
// PRINT CARD DYNAMIC UPDATES
// ============================================================================

void HomePanel::on_print_thumbnail_path_changed(const char* path) {
    if (!subjects_initialized_ || !print_card_active_thumb_) {
        return;
    }

    // Defer the image update to avoid LVGL assertion when called during render
    // (observer callbacks can fire during subject updates which may be mid-render)
    std::string path_copy = path ? path : "";
    helix::ui::async_call(
        [](void* user_data) {
            auto* self = static_cast<HomePanel*>(user_data);
            // Guard against async callback firing after display destruction
            if (!self->print_card_active_thumb_ ||
                !lv_obj_is_valid(self->print_card_active_thumb_)) {
                return;
            }

            const char* current_path =
                lv_subject_get_string(self->printer_state_.get_print_thumbnail_path_subject());

            if (current_path && current_path[0] != '\0') {
                // Thumbnail available - set it on the active print card
                lv_image_set_src(self->print_card_active_thumb_, current_path);
                spdlog::debug("[{}] Active print thumbnail updated: {}", self->get_name(),
                              current_path);
            } else {
                // No thumbnail - revert to benchy placeholder
                lv_image_set_src(self->print_card_active_thumb_,
                                 "A:assets/images/benchy_thumbnail_white.png");
                spdlog::debug("[{}] Active print thumbnail cleared", self->get_name());
            }
        },
        this);
}

void HomePanel::on_print_state_changed(PrintJobState state) {
    if (!subjects_initialized_ || !print_card_thumb_ || !print_card_label_) {
        return;
    }

    bool is_active = (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED);

    if (is_active) {
        spdlog::debug("[{}] Print active - updating card progress display", get_name());
        update_print_card_from_state(); // Update label immediately
    } else {
        spdlog::debug("[{}] Print not active - reverting card to idle state", get_name());
        reset_print_card_to_idle();
    }
}

void HomePanel::on_print_progress_or_time_changed() {
    if (!subjects_initialized_)
        return;

    update_print_card_from_state();
}

void HomePanel::update_print_card_from_state() {
    auto state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));

    // Only update if actively printing
    if (state != PrintJobState::PRINTING && state != PrintJobState::PAUSED) {
        return;
    }

    int progress = lv_subject_get_int(printer_state_.get_print_progress_subject());
    int time_left = lv_subject_get_int(printer_state_.get_print_time_left_subject());

    update_print_card_label(progress, time_left);
}

void HomePanel::update_print_card_label(int progress, int time_left_secs) {
    if (!print_card_label_) {
        return;
    }

    char buf[64];
    int hours = time_left_secs / 3600;
    int minutes = (time_left_secs % 3600) / 60;

    if (hours > 0) {
        snprintf(buf, sizeof(buf), "%d%% \u2022 %dh %02dm left", progress, hours, minutes);
    } else if (minutes > 0) {
        snprintf(buf, sizeof(buf), "%d%% \u2022 %dm left", progress, minutes);
    } else {
        snprintf(buf, sizeof(buf), "%d%% \u2022 < 1m left", progress);
    }

    lv_label_set_text(print_card_label_, buf);
}

void HomePanel::reset_print_card_to_idle() {
    // Reset idle thumbnail to benchy (active thumb is handled by observer when path clears)
    if (print_card_thumb_) {
        lv_image_set_src(print_card_thumb_, "A:assets/images/benchy_thumbnail_white.png");
    }
    if (print_card_label_) {
        lv_label_set_text(print_card_label_, lv_tr("Print Files"));
    }
}

// ============================================================================
// Filament Runout Modal
// ============================================================================

void HomePanel::check_and_show_idle_runout_modal() {
    // Grace period - don't show modal during startup
    auto& fsm = helix::FilamentSensorManager::instance();
    if (fsm.is_in_startup_grace_period()) {
        spdlog::debug("[{}] In startup grace period - skipping runout modal", get_name());
        return;
    }

    // Verify actual sensor state — callers may trigger this from stale subject values
    // during discovery races, so always re-check the authoritative sensor state
    if (!fsm.has_any_runout()) {
        spdlog::debug("[{}] No actual runout detected - skipping modal", get_name());
        return;
    }

    // Check suppression logic (AMS without bypass, wizard active, etc.)
    if (!get_runtime_config()->should_show_runout_modal()) {
        spdlog::debug("[{}] Runout modal suppressed by runtime config", get_name());
        return;
    }

    // Only show modal if not already shown
    if (runout_modal_shown_) {
        spdlog::debug("[{}] Runout modal already shown - skipping", get_name());
        return;
    }

    // Only show if printer is idle (not printing/paused)
    int print_state = lv_subject_get_int(printer_state_.get_print_state_enum_subject());
    if (print_state != static_cast<int>(PrintJobState::STANDBY) &&
        print_state != static_cast<int>(PrintJobState::COMPLETE) &&
        print_state != static_cast<int>(PrintJobState::CANCELLED)) {
        spdlog::debug("[{}] Print active (state={}) - skipping idle runout modal", get_name(),
                      print_state);
        return;
    }

    spdlog::info("[{}] Showing idle runout modal", get_name());
    show_idle_runout_modal();
    runout_modal_shown_ = true;
}

void HomePanel::trigger_idle_runout_check() {
    spdlog::debug("[{}] Triggering deferred runout check", get_name());
    runout_modal_shown_ = false; // Allow modal to show again
    check_and_show_idle_runout_modal();
}

void HomePanel::show_idle_runout_modal() {
    if (runout_modal_.is_visible()) {
        return;
    }

    // Configure callbacks for the modal buttons
    runout_modal_.set_on_load_filament([this]() {
        spdlog::info("[{}] User chose to load filament (idle)", get_name());
        NavigationManager::instance().set_active(PanelId::Filament);
    });

    runout_modal_.set_on_resume([]() {
        // Resume not applicable when idle, but modal handles this
    });

    runout_modal_.set_on_cancel_print([]() {
        // Cancel not applicable when idle, but modal handles this
    });

    runout_modal_.show(parent_screen_);
}

static std::unique_ptr<HomePanel> g_home_panel;

HomePanel& get_global_home_panel() {
    if (!g_home_panel) {
        g_home_panel = std::make_unique<HomePanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("HomePanel",
                                                         []() { g_home_panel.reset(); });
    }
    return *g_home_panel;
}
