// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_image_widget.h"

#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_icon_codepoints.h"
#include "ui_overlay_temp_graph.h"
#include "ui_printer_manager_overlay.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "callout_chip.h"
#include "callout_presenter.h"
#include "config.h"
#include "core/http_executor.h"
#include "core/observer_factory.h"
#include "display_settings_manager.h"
#include "grid_layout.h"
#include "helix_fs.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "led_widget.h"
#include "light_button_config.h"
#include "lock_manager.h"
#include "panel_widget_registry.h"
#include "prerendered_images.h"
#include "printer_detector.h"
#include "printer_image_manager.h"
#include "printer_image_regions.h"
#include "printer_images.h"
#include "printer_state.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/fan_spin_animation.h"
#include "widget_subjects.h"
#include "wizard_config_paths.h"

#include <lvgl/src/misc/cache/lv_cache.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// Subjects owned by PrinterImageWidget module — created before XML bindings resolve
static lv_subject_t s_printer_type_subject;
static char s_printer_type_buffer[64];
static lv_subject_t s_printer_info_visible;

// Live callout subjects (printer image overlay chips), driven from PrinterState
// by PrinterImageWidget::update_callouts() and apply_callout_layout().
static lv_subject_t s_printer_callout_mode;
static lv_subject_t s_callout_toolhead_merged;
static lv_subject_t s_callout_nozzle_shown;
static lv_subject_t s_callout_bed_shown;
static lv_subject_t s_callout_chamber_shown;
static lv_subject_t s_callout_fan_shown;
static lv_subject_t s_callout_light_shown;
static lv_subject_t s_callout_light_on;
static lv_subject_t s_callout_bed_heating;
static lv_subject_t s_callout_nozzle_text;
static char s_callout_nozzle_text_buf[32];
static lv_subject_t s_callout_bed_text;
static char s_callout_bed_text_buf[32];
static lv_subject_t s_callout_chamber_text;
static char s_callout_chamber_text_buf[32];
static lv_subject_t s_callout_fan_text;
static char s_callout_fan_text_buf[32];
static lv_subject_t s_callout_toolhead_text;
static char s_callout_toolhead_text_buf[32];

static helix::WidgetSubjects s_subjects{"PrinterImageWidgetSubjects"};

namespace {

/// The callout leader lines, indexed by CalloutKind (Nozzle..Light).
constexpr const char* kLineNames[] = {"callout_line_nozzle", "callout_line_bed",
                                      "callout_line_chamber", "callout_line_fan",
                                      "callout_line_light"};

constexpr lv_opa_t kGlowOpaLow = 30;
constexpr lv_opa_t kGlowOpaHigh = 70;
constexpr uint32_t kGlowPulseMs = 900;

void glow_opa_anim_cb(void* glow, int32_t opa) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(glow), static_cast<lv_opa_t>(opa), 0);
}

/// Writes a chip's text, compact while the chips are pinned. True on a change.
bool publish_callout_text(lv_subject_t* text, const std::string& full) {
    const bool pinned =
        lv_subject_get_int(&s_printer_callout_mode) == static_cast<int>(helix::CalloutMode::Pinned);
    const std::string t = pinned ? helix::ui::compact_callout_text(full) : full;
    if (t == lv_subject_get_string(text))
        return false;
    lv_subject_copy_string(text, t.c_str());
    return true;
}

/// The text chips, indexed like PrinterImageWidget::callout_full_text_.
struct CalloutTextSubject {
    helix::CalloutKind kind;
    lv_subject_t* subject;
};
const CalloutTextSubject kCalloutTexts[] = {
    {helix::CalloutKind::Nozzle, &s_callout_nozzle_text},
    {helix::CalloutKind::Bed, &s_callout_bed_text},
    {helix::CalloutKind::Chamber, &s_callout_chamber_text},
    {helix::CalloutKind::Fan, &s_callout_fan_text},
    {helix::CalloutKind::Toolhead, &s_callout_toolhead_text},
};

} // namespace

static void printer_image_widget_init_subjects() {
    s_subjects.ensure_init([] {
        // String subject for printer model name
        lv_subject_init_string(&s_printer_type_subject, s_printer_type_buffer, nullptr,
                               sizeof(s_printer_type_buffer), "");
        s_subjects.publish("printer_type_text", &s_printer_type_subject);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_printer_type_subject, "printer_type_text",
                                     LV_SUBJECT_TYPE_STRING);

        // String subject for hostname/IP

        // Integer subject: 0=hidden, 1=visible
        lv_subject_init_int(&s_printer_info_visible, 0);
        s_subjects.publish("printer_info_visible", &s_printer_info_visible);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_printer_info_visible, "printer_info_visible",
                                     LV_SUBJECT_TYPE_INT);

        // Live callout subjects. Every int starts at 0 (CalloutMode::ImageOnly for
        // the mode subject, "not shown" for the rest) so the callout layer and
        // every chip parse hidden, matching the widget's default idle state. A
        // callout_<kind>_shown int is 0 hidden, 1 active, 2 residual (a heater off
        // but still hot, its chip text greyed).
        static const struct {
            lv_subject_t* subject;
            const char* name;
        } int_subjects[] = {
            {&s_printer_callout_mode, "printer_callout_mode"},
            {&s_callout_toolhead_merged, "callout_toolhead_merged"},
            {&s_callout_nozzle_shown, "callout_nozzle_shown"},
            {&s_callout_bed_shown, "callout_bed_shown"},
            {&s_callout_chamber_shown, "callout_chamber_shown"},
            {&s_callout_fan_shown, "callout_fan_shown"},
            {&s_callout_light_shown, "callout_light_shown"},
            {&s_callout_light_on, "callout_light_on"},
            {&s_callout_bed_heating, "callout_bed_heating"},
        };
        for (const auto& s : int_subjects) {
            // 0 doubles as CalloutMode::ImageOnly for the mode subject and as
            // "not shown"/"not merged"/"not heating" for the rest.
            lv_subject_init_int(s.subject, 0);
            s_subjects.publish(s.name, s.subject);
            HELIX_SUBJECT_DEBUG_REGISTER(s.subject, s.name, LV_SUBJECT_TYPE_INT);
        }

        static const struct {
            lv_subject_t* subject;
            char* buf;
            size_t buf_size;
            const char* name;
        } text_subjects[] = {
            {&s_callout_nozzle_text, s_callout_nozzle_text_buf, sizeof(s_callout_nozzle_text_buf),
             "callout_nozzle_text"},
            {&s_callout_bed_text, s_callout_bed_text_buf, sizeof(s_callout_bed_text_buf),
             "callout_bed_text"},
            {&s_callout_chamber_text, s_callout_chamber_text_buf,
             sizeof(s_callout_chamber_text_buf), "callout_chamber_text"},
            {&s_callout_fan_text, s_callout_fan_text_buf, sizeof(s_callout_fan_text_buf),
             "callout_fan_text"},
            {&s_callout_toolhead_text, s_callout_toolhead_text_buf,
             sizeof(s_callout_toolhead_text_buf), "callout_toolhead_text"},
        };
        for (const auto& s : text_subjects) {
            lv_subject_init_string(s.subject, s.buf, nullptr, s.buf_size, "");
            s_subjects.publish(s.name, s.subject);
            HELIX_SUBJECT_DEBUG_REGISTER(s.subject, s.name, LV_SUBJECT_TYPE_STRING);
        }

        spdlog::debug(
            "[PrinterImageWidget] Subjects initialized (type + host + info_visible + callouts)");
    });
}

namespace helix {
void register_printer_image_widget() {
    register_widget_factory(
        "printer_image", [](const std::string&) { return std::make_unique<PrinterImageWidget>(); });
    register_widget_subjects("printer_image", printer_image_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "printer_manager_clicked_cb",
                             PrinterImageWidget::printer_manager_clicked_cb);
    lv_xml_register_event_cb(nullptr, "printer_callout_nozzle_cb",
                             PrinterImageWidget::printer_callout_nozzle_cb);
    lv_xml_register_event_cb(nullptr, "printer_callout_bed_cb",
                             PrinterImageWidget::printer_callout_bed_cb);
    lv_xml_register_event_cb(nullptr, "printer_callout_chamber_cb",
                             PrinterImageWidget::printer_callout_chamber_cb);
    lv_xml_register_event_cb(nullptr, "printer_callout_fan_cb",
                             PrinterImageWidget::printer_callout_fan_cb);
    lv_xml_register_event_cb(nullptr, "printer_callout_light_cb",
                             PrinterImageWidget::printer_callout_light_cb);
    lv_xml_register_event_cb(nullptr, "printer_callout_light_hold_cb",
                             PrinterImageWidget::printer_callout_light_hold_cb);

    // Prune old cached printer images on startup
    prune_printer_image_cache();
}
} // namespace helix

using namespace helix;

PrinterImageWidget::PrinterImageWidget() = default;

PrinterImageWidget::~PrinterImageWidget() {
    detach();
}

void PrinterImageWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // A raw lv_obj_delete() of the home page container gives the widget no
    // detach(), and the cache-generation continuation would then walk a freed
    // tree looking for the image child.
    install_delete_hook(widget_obj_);

    // Set user_data on the printer_container child (where event_cb is registered in XML)
    // so the callback can recover this widget instance via lv_obj_get_user_data()
    auto* container = lv_obj_find_by_name(widget_obj_, "printer_container");
    if (container) {
        lv_obj_set_user_data(container, this);
        // Pressed feedback: dim on touch
        lv_obj_set_style_opa(container, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Load printer image and info from config
    reload_from_config();

    // attach() runs on every rebuild of a recycled instance, so re-arming here
    // keeps the observer alive across home-panel rebuilds.
    printer_type_observer_ = helix::ui::observe<const char*>(
        get_printer_state().profile_state().get_printer_type_subject(), this,
        [](PrinterImageWidget* w, const char* /*type*/) { w->schedule_image_refresh(); },
        get_printer_state().get_subjects_lifetime());

    hardware_observers_.clear();
    for (const auto& in : helix::PrinterImageManager::detection_inputs()) {
        hardware_observers_.push_back(helix::ui::observe<int>(
            in.subject, this,
            [](PrinterImageWidget* w, int /*value*/) { w->refresh_if_detected_image_changed(); },
            in.lifetime));
    }

    // A recycled instance's mode is the last tree's; hide the layer until the
    // timer has positioned this tree's chips.
    lv_subject_set_int(&s_printer_callout_mode, static_cast<int>(CalloutMode::ImageOnly));
    arm_callout_observers();
    schedule_callout_layout();

    spdlog::debug("[PrinterImageWidget] Attached");
}

void PrinterImageWidget::detach() {
    // Drop the type observer first: any handler it has queued on the
    // UpdateQueue holds a weak alive token that this reset expires, so a
    // deferred refresh can't land on a detached tree.
    printer_type_observer_.reset();
    hardware_observers_.clear();
    callout_observers_.clear();
    bed_temp_lt_.reset();
    bed_target_lt_.reset();
    chamber_temp_lt_.reset();
    chamber_target_lt_.reset();
    // Safe with or without a live tree: each animator drops its icon when that
    // icon is deleted, so an unbind after the tree died touches no widget.
    nozzle_binder_.unbind();
    bed_binder_.unbind();
    chamber_binder_.unbind();
    if (widget_obj_) {
        helix::ui::fan_spin_stop(lv_obj_find_by_name(widget_obj_, "callout_fan_icon"));
        // The lines point into this instance's arrays, which die with it; a
        // tree that outlives the instance must not keep drawing from them.
        for (const char* n : kLineNames) {
            if (lv_obj_t* line = lv_obj_find_by_name(widget_obj_, n))
                lv_line_set_points(line, nullptr, 0);
        }
    }
    set_glow_pulse(false);
    callout_glow_pulsing_ = false; // a dead tree took its pulse with it
    cancel_callout_timer();

    // Cancel any pending timers. detach() runs from the destructor, so cancelling
    // here makes the deferred timers lifetime-safe (main-thread work — no
    // AsyncLifetimeGuard needed).
    if (lv_is_initialized() && refresh_timer_) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
    if (lv_is_initialized() && cache_timer_) {
        lv_timer_delete(cache_timer_);
        cache_timer_ = nullptr;
    }

    // Expire the cache-generation continuation before the tree it targets goes
    // away. The worker keeps running to completion and still writes its entry, so
    // the next attach finds a cache hit.
    exact_image_.cancel();

    uninstall_delete_hook();

    if (widget_obj_) {
        auto* container = lv_obj_find_by_name(widget_obj_, "printer_container");
        if (container) {
            lv_obj_set_user_data(container, nullptr);
        }
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    current_source_path_.clear();

    spdlog::debug("[PrinterImageWidget] Detached");
}

void PrinterImageWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: expire the guard and drop pointers only.
    // The deferred timers below re-read widget_obj_ and bail on null, and the
    // cache continuation is skipped by its expired token, so neither reaches the
    // freed tree. A later detach() must not call lv_obj_set_user_data() on it
    // either, which is why widget_obj_ is cleared here rather than there.
    // The callout observers stay until detach()/attach(): their handler reads
    // widget_obj_ before any widget call, the icon binders' animators drop
    // their icons as the children die, and LVGL deletes each icon's fan spin
    // with the icon, so nothing here reaches the freed tree.
    exact_image_.cancel();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void PrinterImageWidget::on_activate() {
    // Re-check printer image and name (may have changed in printer manager overlay)
    reload_from_config();
}

void PrinterImageWidget::reload_from_config() {
    Config* config = Config::get_instance();

    // Update printer type in PrinterState (triggers capability cache refresh)
    std::string printer_type =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    get_printer_state().set_printer_type_sync(printer_type);

    // Update printer image — DEFERRED. refresh_printer_image() calls
    // lv_image_set_inner_align(), which forces lv_obj_update_layout and cascades
    // into the parent grid's grid_update. reload_from_config() runs from attach()
    // and on_activate(), both reachable from a panel rebuild; forcing layout on a
    // mid-rebuild grid walked the freed descriptor off the heap end (#983/#1025).
    // Defer the image work to a later tick so it never runs inside the rebuild.
    schedule_image_refresh();

    // Update printer info overlay
    // Always visible to maintain consistent flex layout (hidden flag removes from flex).
    std::string host = config->get<std::string>(config->df() + helix::wizard::MOONRAKER_HOST, "");

    // Show printer name if set, otherwise fall back to printer type, then " " for layout
    std::string display_name = helix::get_printer_display_name(" ");
    lv_subject_copy_string(&s_printer_type_subject, display_name.c_str());

    if (!host.empty() && host != "127.0.0.1" && host != "localhost") {
    }

    lv_subject_set_int(&s_printer_info_visible, 1);
}

void PrinterImageWidget::refresh_printer_image() {
    if (!widget_obj_)
        return;

    lv_display_t* disp = lv_display_get_default();
    int screen_width = disp ? lv_display_get_horizontal_resolution(disp) : 800;

    const std::string source_path =
        helix::PrinterImageManager::instance().get_displayed_image_path(screen_width);

    // LVGL keys its decoded copy on the path alone, and an import can rewrite an
    // image in place under that same path, so the decoded copy and the natural
    // size read from it are dropped on every refresh. The scaled entries on disk
    // need no such sweep: their names carry the source's mtime and size, so the
    // entry holding the old pixels is never named again.
    if (!current_source_path_.empty()) {
        lv_image_cache_drop(current_source_path_.c_str());
    }
    natural_size_path_.clear();
    // A new source, or new pixels under the same path, can have other tagged
    // points and another aspect.
    schedule_callout_layout();

    if (current_source_path_ != source_path)
        exact_image_.forget_displayed();
    current_source_path_ = source_path;

    if (!exact_image_.show_cached(source_path)) {
        // No exact-size copy for this size yet. The tier image costs a decode plus a
        // CONTAIN scale on every paint and is replaced as soon as one is generated,
        // so this path is the first display at a given size, not the steady state.
        exact_image_.show_scaled(source_path);
    }

    // Schedule cache check after layout resolves
    schedule_cache_check();
}

void PrinterImageWidget::refresh_if_detected_image_changed() {
    // Hardware subjects change on every connect; redrawing an unchanged image
    // would drop its decoded copy for nothing.
    if (!widget_obj_ || helix::PrinterImageManager::instance().get_displayed_image_path(
                            printer_images::current_screen_width()) == current_source_path_)
        return;
    schedule_image_refresh();
}

void PrinterImageWidget::schedule_image_refresh() {
    if (refresh_timer_) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }

    refresh_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            // Guard the C/C++ boundary: refresh_printer_image() sets the image
            // source and forces an alignment-driven layout; an exception escaping
            // into lv_timer_handler() (C) would terminate the process.
            LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] refresh_timer");
            auto* self = static_cast<PrinterImageWidget*>(lv_timer_get_user_data(timer));
            if (self) {
                self->refresh_timer_ = nullptr;
                self->refresh_printer_image();
            }
            lv_timer_delete(timer);
            LVGL_SAFE_EVENT_CB_END();
        },
        1, this);
    lv_timer_set_repeat_count(refresh_timer_, 1);
}

void PrinterImageWidget::schedule_cache_check() {
    if (cache_timer_) {
        lv_timer_delete(cache_timer_);
        cache_timer_ = nullptr;
    }

    cache_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            // Guard the C/C++ boundary: check_or_generate_cache() decodes and resizes
            // images, allocating large pixel buffers that can throw std::bad_alloc on a
            // 32-bit target. An exception escaping into lv_timer_handler() (C) would
            // terminate the process. Cache generation is best-effort, so on failure we
            // keep the already-displayed scaled source image.
            LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] cache_timer");
            auto* self = static_cast<PrinterImageWidget*>(lv_timer_get_user_data(timer));
            if (self) {
                self->cache_timer_ = nullptr;
                self->check_or_generate_cache();
            }
            lv_timer_delete(timer);
            LVGL_SAFE_EVENT_CB_END();
        },
        50, this);
    lv_timer_set_repeat_count(cache_timer_, 1);
}

void PrinterImageWidget::check_or_generate_cache() {
    if (!widget_obj_ || current_source_path_.empty())
        return;
    exact_image_.generate_if_missing(current_source_path_);
}

void PrinterImageWidget::handle_printer_manager_clicked() {
    spdlog::info("[PrinterImageWidget] Printer image clicked - opening Printer Manager overlay");

    get_printer_manager_overlay().show(parent_screen_);
}

void PrinterImageWidget::printer_manager_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] printer_manager_clicked_cb");

    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<PrinterImageWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_printer_manager_clicked();
    } else {
        spdlog::warn(
            "[PrinterImageWidget] printer_manager_clicked_cb: could not recover widget instance");
    }

    LVGL_SAFE_EVENT_CB_END();
}

namespace {

/// The light chip switches a real light, so a touch that reaches it behind the
/// lock screen (a resting finger on wake) does nothing.
bool light_chip_locked() {
    if (!helix::LockManager::instance().is_locked())
        return false;
    spdlog::debug("[PrinterImageWidget] light chip touch ignored: screen locked");
    return true;
}

} // namespace

PrinterImageWidget* PrinterImageWidget::widget_from_chip(lv_event_t* e) {
    // chip -> callout_layer -> printer_container, whose user_data attach() set.
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_obj_t* layer = chip ? lv_obj_get_parent(chip) : nullptr;
    lv_obj_t* container = layer ? lv_obj_get_parent(layer) : nullptr;
    auto* self =
        container ? static_cast<PrinterImageWidget*>(lv_obj_get_user_data(container)) : nullptr;
    if (!self) {
        spdlog::warn("[PrinterImageWidget] callout event: could not recover widget instance");
    }
    return self;
}

void PrinterImageWidget::route_callout_click(lv_event_t* e, CalloutKind kind) {
    // The home panel makes every descendant bubble, so an unstopped chip tap also
    // reaches printer_container and opens Printer Manager over the chip's control.
    // Only CLICKED stops here; PRESSED and LONG_PRESSED still reach grid edit mode,
    // except on the light chip, whose hold has its own action.
    lv_event_stop_bubbling(e);
    if (auto* self = widget_from_chip(e)) {
        self->record_interaction();
        self->handle_callout_clicked(kind);
    }
}

void PrinterImageWidget::printer_callout_nozzle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] printer_callout_nozzle_cb");
    route_callout_click(e, CalloutKind::Nozzle);
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageWidget::printer_callout_bed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] printer_callout_bed_cb");
    route_callout_click(e, CalloutKind::Bed);
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageWidget::printer_callout_chamber_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] printer_callout_chamber_cb");
    route_callout_click(e, CalloutKind::Chamber);
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageWidget::printer_callout_fan_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] printer_callout_fan_cb");
    route_callout_click(e, CalloutKind::Fan);
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageWidget::printer_callout_light_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] printer_callout_light_cb");
    route_callout_click(e, CalloutKind::Light);
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageWidget::printer_callout_light_hold_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] printer_callout_light_hold_cb");
    // A hold on the bulb opens the light's controls rather than grid edit mode,
    // and ends the press so its release does not also toggle the light.
    lv_event_stop_bubbling(e);
    auto* self = widget_from_chip(e);
    if (light_chip_locked())
        return; // inside the SAFE_EVENT_CB try block; END closes it below
    if (self) {
        helix::ui::end_press(e);
        self->record_interaction();
        open_led_control_overlay(self->parent_screen_,
                                 helix::led::LedController::instance().chamber_light());
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageWidget::handle_callout_clicked(CalloutKind kind) {
    switch (kind) {
    case CalloutKind::Nozzle:
    case CalloutKind::Toolhead:
        get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_);
        break;
    case CalloutKind::Bed:
        get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
        break;
    case CalloutKind::Chamber:
        get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Chamber, parent_screen_);
        break;
    case CalloutKind::Fan:
        open_fan_control_overlay(parent_screen_);
        break;
    case CalloutKind::Light: {
        if (light_chip_locked())
            break;
        const auto result = toggle_lights(
            helix::led::LedController::instance().light_targets(std::string()), "PrinterImage");
        // The dark bulb shows while the state is unknown, so it takes the flash.
        if (result.unconfirmed && widget_obj_)
            flash_light_icon(lv_obj_find_by_name(widget_obj_, "callout_light_dark"));
        break;
    }
    case CalloutKind::HotendFan:
    case CalloutKind::AuxFan:
    case CalloutKind::SpeedFlow:
    case CalloutKind::OtherTools:
        break; // the home widget has no such chips
    }
}

// ---------------------------------------------------------------------------
// Live callouts
// ---------------------------------------------------------------------------

namespace {

lv_subject_t* printer_has_led_subject() {
    // A capability subject with no PrinterState accessor, owned by
    // PrinterState's capabilities_state_ (so it shares get_subjects_lifetime()).
    return lv_xml_get_subject(nullptr, "printer_has_led");
}

int read_int_or_zero(lv_subject_t* s) {
    return s ? lv_subject_get_int(s) : 0;
}

/// The light chip shows, lit or dark, whenever a chamber light can be switched.
bool light_chip_capable() {
    return read_int_or_zero(printer_has_led_subject()) &&
           !helix::led::LedController::instance().chamber_light().empty();
}

} // namespace

void PrinterImageWidget::arm_callout_observers() {
    callout_observers_.clear();
    auto& ps = get_printer_state();
    const auto on_change = [](PrinterImageWidget* w, int) { w->update_callouts(); };
    const SubjectLifetime life = ps.get_subjects_lifetime();
    for (lv_subject_t* s : {ps.temperature_state().get_active_extruder_temp_subject(),
                            ps.temperature_state().get_active_extruder_target_subject(),
                            ps.fan_state().get_fan_speed_subject()}) {
        callout_observers_.push_back(helix::ui::observe<int>(s, this, on_change, life));
    }
    auto& leds = helix::led::LedController::instance();
    callout_observers_.push_back(helix::ui::observe<int>(leds.get_led_state_version_subject(), this,
                                                         on_change, leds.get_subjects_lifetime()));
    // A capability joins the budget, which decides the mode, whether or not any
    // chip text changes with it.
    const auto on_capability = [](PrinterImageWidget* w, int) {
        w->update_callouts();
        w->schedule_callout_layout();
    };
    for (lv_subject_t* s :
         {printer_has_led_subject(), ps.capabilities_state().subject(Capability::HasChamber)})
        callout_observers_.push_back(helix::ui::observe<int>(s, this, on_capability, life));
    const auto observe_dynamic = [&](lv_subject_t* s, SubjectLifetime& lt) {
        callout_observers_.push_back(helix::ui::observe<int>(s, this, on_change, lt));
    };
    observe_dynamic(ps.temperature_state().get_bed_temp_subject(bed_temp_lt_), bed_temp_lt_);
    observe_dynamic(ps.temperature_state().get_bed_target_subject(bed_target_lt_), bed_target_lt_);
    observe_dynamic(ps.temperature_state().get_chamber_temp_subject(chamber_temp_lt_),
                    chamber_temp_lt_);
    observe_dynamic(ps.temperature_state().get_chamber_effective_target_subject(chamber_target_lt_),
                    chamber_target_lt_);
    auto& display = DisplaySettingsManager::instance();
    callout_observers_.push_back(helix::ui::observe<int>(
        display.subject_animations_enabled(), this, on_change, display.get_subjects_lifetime()));
    // The nozzle glyph draws the tool number beside it on a multi-tool printer,
    // which widens the nozzle chip. Looked up by the names the badge binds, so
    // where ToolState never registered them there is no badge.
    const SubjectLifetime tools_life = ToolState::instance().get_subjects_lifetime();
    callout_observers_.push_back(helix::ui::observe<int>(
        lv_xml_get_subject(nullptr, "show_tool_badge"), this,
        [](PrinterImageWidget* w, int) { w->schedule_callout_layout(); }, tools_life));
    callout_observers_.push_back(helix::ui::observe<const char*>(
        lv_xml_get_subject(nullptr, "tool_badge_text"), this,
        [](PrinterImageWidget* w, const char*) { w->schedule_callout_layout(); }, tools_life));

    const auto chip = [&](const char* name) { return lv_obj_find_by_name(widget_obj_, name); };
    callout_spin_pct_ = -1;        // fresh icons: the next update sets their spin
    callout_glow_pulsing_ = false; // fresh glow: nothing animates it yet
    nozzle_binder_.bind(chip("callout_chip_nozzle"), ps, HeaterType::Nozzle);
    bed_binder_.bind(chip("callout_chip_bed"), ps, HeaterType::Bed);
    chamber_binder_.bind(chip("callout_chip_chamber"), ps, HeaterType::Chamber);
    update_callouts();
}

void PrinterImageWidget::update_callouts() {
    using namespace helix::ui::temperature;
    auto& ps = get_printer_state();
    bool changed = false;
    const auto set_text = [&](CalloutKind k, lv_subject_t* text, const std::string& t) {
        callout_full_text_[static_cast<size_t>(k)] = t;
        changed |= publish_callout_text(text, t);
    };
    // A shown subject reads 0 hidden, 1 active, 2 residual (off but still hot).
    const auto publish = [&](lv_subject_t* shown, int value, CalloutKind k, lv_subject_t* text,
                             const std::string& t) {
        if (lv_subject_get_int(shown) != value) {
            lv_subject_set_int(shown, value);
            changed = true;
        }
        if (text)
            set_text(k, text, t);
    };
    // A heater shows while it has a target, and after that, greyed, while it is
    // still hot enough to burn. The chip reads exactly what the temperature widgets read.
    const auto heater = [&](int cur, int tgt, bool capable, lv_subject_t* shown, CalloutKind k,
                            lv_subject_t* text) {
        const int value = !capable ? 0 : tgt > 0 ? 1 : is_residual_hot(cur) ? 2 : 0;
        publish(shown, value, k, text, value ? heater_display(cur, tgt).temp : std::string());
    };

    heater(read_int_or_zero(ps.temperature_state().get_active_extruder_temp_subject()),
           read_int_or_zero(ps.temperature_state().get_active_extruder_target_subject()), true,
           &s_callout_nozzle_shown, CalloutKind::Nozzle, &s_callout_nozzle_text);
    const int bed_cur = read_int_or_zero(ps.temperature_state().get_bed_temp_subject());
    const int bed_tgt = read_int_or_zero(ps.temperature_state().get_bed_target_subject());
    heater(bed_cur, bed_tgt, true, &s_callout_bed_shown, CalloutKind::Bed, &s_callout_bed_text);
    const int bed_heating = heater_display(bed_cur, bed_tgt).state == HeatState::Heating ? 1 : 0;
    lv_subject_set_int(&s_callout_bed_heating, bed_heating);
    heater(read_int_or_zero(ps.temperature_state().get_chamber_temp_subject()),
           read_int_or_zero(ps.temperature_state().get_chamber_effective_target_subject()),
           read_int_or_zero(ps.capabilities_state().subject(Capability::HasChamber)) != 0,
           &s_callout_chamber_shown, CalloutKind::Chamber, &s_callout_chamber_text);

    const int fan = read_int_or_zero(ps.fan_state().get_fan_speed_subject());
    char fan_buf[8];
    snprintf(fan_buf, sizeof(fan_buf), "%d%%", fan);
    publish(&s_callout_fan_shown, fan > 0 ? 1 : 0, CalloutKind::Fan, &s_callout_fan_text,
            fan > 0 ? fan_buf : "");
    const bool light_capable = light_chip_capable();
    publish(&s_callout_light_shown, light_capable ? 1 : 0, CalloutKind::Light, nullptr, {});
    // Lit or dark is the same chip size, so it needs no relayout.
    const int light_on = light_capable && helix::led::chamber_light_on() ? 1 : 0;
    if (lv_subject_get_int(&s_callout_light_on) != light_on)
        lv_subject_set_int(&s_callout_light_on, light_on);
    set_text(CalloutKind::Toolhead, &s_callout_toolhead_text,
             ui::toolhead_chip_text(callout_full_text_[static_cast<size_t>(CalloutKind::Nozzle)],
                                    fan_buf));

    const bool animate = DisplaySettingsManager::instance().get_animations_enabled();
    set_glow_pulse(bed_heating && animate);
    // Restarting a spin resets its rotation, so only a speed or preference
    // change touches it, never a temperature tick.
    const int spin = animate ? fan : 0;
    if (widget_obj_ && spin != callout_spin_pct_) {
        callout_spin_pct_ = spin;
        lv_obj_t* icon = lv_obj_find_by_name(widget_obj_, "callout_fan_icon");
        if (spin > 0)
            helix::ui::fan_spin_start(icon, spin);
        else
            helix::ui::fan_spin_stop(icon);
    }
    if (changed)
        schedule_callout_layout();
}

void PrinterImageWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                         int /*height_px*/) {
    callout_colspan_ = colspan;
    callout_rowspan_ = rowspan;
    schedule_callout_layout();
}

void PrinterImageWidget::cancel_callout_timer() {
    helix::ui::lv_timer_cancel_safe(callout_timer_);
    callout_timer_ = nullptr;
}

void PrinterImageWidget::schedule_callout_layout() {
    cancel_callout_timer();
    // Deferred like the cache check: sizes are read only after the grid has laid
    // the widget out, and nothing here may force layout during a rebuild (#983).
    callout_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageWidget] callout_timer");
            auto* self = static_cast<PrinterImageWidget*>(lv_timer_get_user_data(timer));
            if (self) {
                self->callout_timer_ = nullptr;
                self->apply_callout_layout();
            }
            lv_timer_delete(timer);
            LVGL_SAFE_EVENT_CB_END();
        },
        50, this);
    lv_timer_set_repeat_count(callout_timer_, 1);
}

void PrinterImageWidget::apply_callout_layout() {
    if (!widget_obj_)
        return;
    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "printer_container");
    if (!container)
        return;
    const auto image_only = [this] {
        lv_subject_set_int(&s_printer_callout_mode, static_cast<int>(CalloutMode::ImageOnly));
        place_printer_image(nullptr);
    };

    helix::ui::CalloutPresenter::Config cfg;
    cfg.single_cell = callout_colspan_ <= GridLayout::TRACKS_PER_CELL &&
                      callout_rowspan_ <= GridLayout::TRACKS_PER_CELL;
    if (lv_obj_get_content_width(container) <= 0 || lv_obj_get_content_height(container) <= 0) {
        image_only(); // the next size change reschedules
        return;
    }

    if (natural_size_path_ != current_source_path_) {
        lv_image_header_t hdr;
        if (current_source_path_.empty() ||
            lv_image_decoder_get_info(current_source_path_.c_str(), &hdr) != LV_RESULT_OK) {
            image_only();
            return;
        }
        natural_size_path_ = current_source_path_;
        natural_w_ = static_cast<int>(hdr.w);
        natural_h_ = static_cast<int>(hdr.h);
    }
    const ImageRegions* r = lookup_image_regions(printer_image_region_key(current_source_path_),
                                                 natural_w_, natural_h_);

    lv_obj_t* probe = lv_obj_find_by_name(widget_obj_, "callout_chip_bed");
    if (!probe)
        return;
    const helix::ui::CalloutChipMetrics metrics = helix::ui::measure_callout_chip(probe);
    const auto icon_px = [&](const char* name) { return metrics.icon_px(name); };
    const auto chip_w = [&](int icons, const std::string& text) {
        return metrics.chip_w(icons, text);
    };
    // Pinned chips drop their icons and set chip_text in font_xs (the
    // printer_callout_mode binding in panel_widget_printer_image.xml).
    const auto compact_w = [&](int icons, const std::string& text) {
        return helix::ui::compact_callout_chip_w(probe, text, icons);
    };
    cfg.chip_h = metrics.chip_h;
    cfg.compact_chip_h =
        helix::ui::compact_callout_chip_h(probe, lv_subject_get_int(&s_callout_light_shown));
    cfg.gap = theme_manager_get_spacing("space_xs");
    cfg.min_line = theme_manager_get_spacing("space_md");

    const std::string widest_heater = helix::ui::temperature::heater_display(9990, 9990).temp;
    // The tool badge sits in the nozzle glyph's own row, after its column gap,
    // in the font the badge label renders; hidden on a single-tool printer.
    int nozzle_icons = icon_px("heater");
    if (lv_obj_t* nozzle = lv_obj_find_by_name(widget_obj_, "callout_chip_nozzle")) {
        lv_obj_t* badge = lv_obj_find_by_name(nozzle, "tool_badge");
        if (badge && !lv_obj_has_flag(badge, LV_OBJ_FLAG_HIDDEN)) {
            nozzle_icons +=
                lv_obj_get_style_pad_column(lv_obj_get_parent(badge), LV_PART_MAIN) +
                helix::ui::measure_text_px(lv_label_get_text(badge),
                                           lv_obj_get_style_text_font(badge, LV_PART_MAIN));
        }
    }
    const auto text = [this](CalloutKind k) { return callout_full_text_[static_cast<size_t>(k)]; };
    auto& ps = get_printer_state();

    struct Chip {
        CalloutKind kind;
        const char* name;
        lv_subject_t* shown;
        int icons;
        std::string now;   ///< what it shows
        std::string worst; ///< the widest it can show
        bool capable;
    };
    const Chip chips[] = {
        {CalloutKind::Nozzle, "callout_chip_nozzle", &s_callout_nozzle_shown, nozzle_icons,
         text(CalloutKind::Nozzle), widest_heater, true},
        {CalloutKind::Bed, "callout_chip_bed", &s_callout_bed_shown, icon_px("radiator"),
         text(CalloutKind::Bed), widest_heater, true},
        {CalloutKind::Chamber, "callout_chip_chamber", &s_callout_chamber_shown,
         icon_px("fridge_industrial"), text(CalloutKind::Chamber), widest_heater,
         read_int_or_zero(ps.capabilities_state().subject(Capability::HasChamber)) != 0},
        {CalloutKind::Fan, "callout_chip_fan", &s_callout_fan_shown, icon_px("fan"),
         text(CalloutKind::Fan), "100%", true},
        {CalloutKind::Light,
         "callout_chip_light",
         &s_callout_light_shown,
         icon_px("lightbulb_on"),
         {},
         {},
         light_chip_capable()},
    };
    helix::ui::CalloutPresenter::Parts parts;
    parts.area = container;
    parts.image = lv_obj_find_by_name(widget_obj_, "printer_image");
    for (const Chip& c : chips) {
        const auto k = static_cast<size_t>(c.kind);
        helix::ui::CalloutPresenter::Chip part;
        part.kind = c.kind;
        part.chip = lv_obj_find_by_name(widget_obj_, c.name);
        part.line =
            k < std::size(kLineNames) ? lv_obj_find_by_name(widget_obj_, kLineNames[k]) : nullptr;
        if (c.kind == CalloutKind::Bed)
            part.glow = lv_obj_find_by_name(widget_obj_, "callout_bed_glow");
        part.in_budget = c.capable;
        part.active = lv_subject_get_int(c.shown) != 0;
        part.icons = c.icons;
        part.w_budget = c.capable ? chip_w(c.icons, c.worst) : 0;
        part.w_full = part.active ? chip_w(c.icons, c.now) : 0;
        part.w_compact = part.active ? compact_w(c.icons, c.now) : 0;
        parts.chips.push_back(part);
    }
    {
        helix::ui::CalloutPresenter::Chip th;
        th.kind = CalloutKind::Toolhead;
        th.chip = lv_obj_find_by_name(widget_obj_, "callout_chip_toolhead");
        th.w_compact = compact_w(0, text(CalloutKind::Toolhead));
        parts.toolhead = th;
    }
    const CalloutLayout out = presenter_.apply(parts, r, natural_w_, natural_h_, cfg);

    lv_subject_set_int(&s_callout_toolhead_merged, out.toolhead_merged ? 1 : 0);
    lv_subject_set_int(&s_printer_callout_mode, static_cast<int>(out.mode));
    for (const auto& t : kCalloutTexts)
        publish_callout_text(t.subject, callout_full_text_[static_cast<size_t>(t.kind)]);
    if (presenter_.image_changed())
        schedule_image_refresh();
}

void PrinterImageWidget::place_printer_image(const CalloutRect* moved) {
    lv_obj_t* img = widget_obj_ ? lv_obj_find_by_name(widget_obj_, "printer_image") : nullptr;
    // The exact-size copy on screen was cut for the old rect. The refresh
    // relayouts, and that relayout finds the rect unchanged, so this cannot loop.
    if (presenter_.place_image(img, moved))
        schedule_image_refresh();
}

void PrinterImageWidget::set_glow_pulse(bool on) {
    lv_obj_t* glow = widget_obj_ ? lv_obj_find_by_name(widget_obj_, "callout_bed_glow") : nullptr;
    if (!glow || on == callout_glow_pulsing_)
        return;
    callout_glow_pulsing_ = on;
    // The anim runs on the glow object, so LVGL deletes it with the object.
    lv_anim_delete(glow, glow_opa_anim_cb);
    if (!on)
        return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, glow);
    lv_anim_set_values(&a, kGlowOpaLow, kGlowOpaHigh);
    lv_anim_set_duration(&a, kGlowPulseMs);
    lv_anim_set_playback_duration(&a, kGlowPulseMs);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, glow_opa_anim_cb);
    lv_anim_start(&a);
}
