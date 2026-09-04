// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_image_widget.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_printer_manager_overlay.h"

#include "app_globals.h"
#include "config.h"
#include "panel_widget_registry.h"
#include "prerendered_images.h"
#include "printer_detector.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "wizard_config_paths.h"

#include <lvgl/src/misc/cache/lv_cache.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>

// Subjects owned by PrinterImageWidget module — created before XML bindings resolve
static lv_subject_t s_printer_type_subject;
static char s_printer_type_buffer[64];
static lv_subject_t s_printer_info_visible;
static bool s_subjects_initialized = false;

static void printer_image_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    // String subject for printer model name
    lv_subject_init_string(&s_printer_type_subject, s_printer_type_buffer, nullptr,
                           sizeof(s_printer_type_buffer), "");
    lv_xml_register_subject(nullptr, "printer_type_text", &s_printer_type_subject);
    SubjectDebugRegistry::instance().register_subject(&s_printer_type_subject, "printer_type_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    // String subject for hostname/IP

    // Integer subject: 0=hidden, 1=visible
    lv_subject_init_int(&s_printer_info_visible, 0);
    lv_xml_register_subject(nullptr, "printer_info_visible", &s_printer_info_visible);
    SubjectDebugRegistry::instance().register_subject(
        &s_printer_info_visible, "printer_info_visible", LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    s_subjects_initialized = true;

    // Self-register cleanup with StaticSubjectRegistry (co-located with init)
    StaticSubjectRegistry::instance().register_deinit("PrinterImageWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_printer_info_visible);
            lv_subject_deinit(&s_printer_type_subject);
            s_subjects_initialized = false;
            spdlog::trace("[PrinterImageWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[PrinterImageWidget] Subjects initialized (type + host + info_visible)");
}

namespace helix {
void register_printer_image_widget() {
    register_widget_factory(
        "printer_image", [](const std::string&) { return std::make_unique<PrinterImageWidget>(); });
    register_widget_subjects("printer_image", printer_image_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "printer_manager_clicked_cb",
                             PrinterImageWidget::printer_manager_clicked_cb);

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

    // Store this pointer for event callback recovery
    lv_obj_set_user_data(widget_obj_, this);

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

    spdlog::debug("[PrinterImageWidget] Attached");
}

void PrinterImageWidget::detach() {
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

    if (widget_obj_) {
        auto* container = lv_obj_find_by_name(widget_obj_, "printer_container");
        if (container) {
            lv_obj_set_user_data(container, nullptr);
        }
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    current_source_path_.clear();

    spdlog::debug("[PrinterImageWidget] Detached");
}

void PrinterImageWidget::on_activate() {
    // Re-check printer image and name (may have changed in printer manager overlay)
    reload_from_config();
}

void PrinterImageWidget::reload_from_config() {
    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[PrinterImageWidget] reload_from_config: Config not available");
        return;
    }

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

    // Resolve source image path
    std::string source_path;

    // Check for user-selected printer image (custom or shipped override)
    auto& pim = helix::PrinterImageManager::instance();
    source_path = pim.get_active_image_path(screen_width);

    if (source_path.empty()) {
        // Auto-detect from printer type
        Config* config = Config::get_instance();
        std::string printer_type =
            config ? config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "") : "";
        source_path = PrinterImages::get_best_printer_image(printer_type);
    }

    // The decoded copy LVGL holds is dropped on every refresh: an import can
    // rewrite an image in place, so one path can hold different pixels over time.
    // The scaled caches on disk are only stale when the path itself changes —
    // check_or_generate_cache() rebuilds one synchronously on the main thread, so
    // discarding a cache that still matches costs a visible stall per refresh.
    if (!current_source_path_.empty()) {
        lv_image_cache_drop(current_source_path_.c_str());
    }
    helix::invalidate_printer_image_cache_if_changed(current_source_path_, source_path);

    current_source_path_ = source_path;

    // Set source with CONTAIN alignment — displays immediately (with runtime scaling)
    lv_obj_t* img = lv_obj_find_by_name(widget_obj_, "printer_image");
    if (img) {
        lv_image_set_src(img, source_path.c_str());
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
        spdlog::debug("[PrinterImageWidget] Source image: '{}'", source_path);
    }

    // Schedule cache check after layout resolves
    schedule_cache_check();
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

    lv_obj_t* img = lv_obj_find_by_name(widget_obj_, "printer_image");
    if (!img)
        return;

    int32_t w = lv_obj_get_width(img);
    int32_t h = lv_obj_get_height(img);
    if (w <= 0 || h <= 0) {
        spdlog::debug("[PrinterImageWidget] Not laid out yet ({}x{}), skipping cache", w, h);
        return;
    }

    // Check if a cached .bin exists at exact widget dimensions
    std::string cache_path = helix::get_cached_printer_image_path(current_source_path_, w, h);

    // error_code overload: the ESP32 VFS reports missing paths as ENODATA,
    // which the throwing exists(p) treats as an error, not "not found".
    std::error_code cache_ec;
    if (std::filesystem::exists(cache_path, cache_ec)) {
        // Cache hit — load directly, no scaling needed
        std::string lvgl_path = "A:" + cache_path;
        lv_image_set_src(img, lvgl_path.c_str());
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        spdlog::debug("[PrinterImageWidget] Cache hit: {} ({}x{})", cache_path, w, h);
        return;
    }

    // Cache miss — generate at exact dimensions
    spdlog::debug("[PrinterImageWidget] Cache miss, generating {}x{} from '{}'", w, h,
                  current_source_path_);

    if (helix::generate_cached_printer_image(current_source_path_, w, h, cache_path)) {
        std::string lvgl_path = "A:" + cache_path;
        lv_image_set_src(img, lvgl_path.c_str());
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        spdlog::debug("[PrinterImageWidget] Cached and loaded: {} ({}x{})", cache_path, w, h);
    } else {
        // Generation failed — keep displaying with CONTAIN scaling (already set)
        spdlog::warn("[PrinterImageWidget] Cache generation failed, using scaled source");
    }
}

void PrinterImageWidget::handle_printer_manager_clicked() {
    spdlog::info("[PrinterImageWidget] Printer image clicked - opening Printer Manager overlay");

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
