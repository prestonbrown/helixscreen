// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_image_widget.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_printer_manager_overlay.h"

#include "app_globals.h"
#include "config.h"
#include "panel_widget_registry.h"
#include "printer_detector.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstring>

// Subjects owned by PrinterImageWidget module — created before XML bindings resolve
static lv_subject_t s_printer_type_subject;
static char s_printer_type_buffer[64];
static lv_subject_t s_printer_host_subject;
static char s_printer_host_buffer[64];
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
    lv_subject_init_string(&s_printer_host_subject, s_printer_host_buffer, nullptr,
                           sizeof(s_printer_host_buffer), "");
    lv_xml_register_subject(nullptr, "printer_host_text", &s_printer_host_subject);
    SubjectDebugRegistry::instance().register_subject(&s_printer_host_subject, "printer_host_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

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
            lv_subject_deinit(&s_printer_host_subject);
            lv_subject_deinit(&s_printer_type_subject);
            s_subjects_initialized = false;
            spdlog::trace("[PrinterImageWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[PrinterImageWidget] Subjects initialized (type + host + info_visible)");
}

namespace helix {
void register_printer_image_widget() {
    register_widget_factory("printer_image",
                            []() { return std::make_unique<PrinterImageWidget>(); });
    register_widget_subjects("printer_image", printer_image_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "printer_manager_clicked_cb",
                             PrinterImageWidget::printer_manager_clicked_cb);
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
    // Cancel any pending snapshot timer
    if (lv_is_initialized() && snapshot_timer_) {
        lv_timer_delete(snapshot_timer_);
        snapshot_timer_ = nullptr;
    }

    // Destroy cached snapshot
    if (cached_printer_snapshot_) {
        if (lv_is_initialized() && widget_obj_ && lv_obj_is_valid(widget_obj_)) {
            lv_obj_t* img = lv_obj_find_by_name(widget_obj_, "printer_image");
            if (img) {
                lv_image_set_src(img, nullptr);
            }
        }
        lv_draw_buf_destroy(cached_printer_snapshot_);
        cached_printer_snapshot_ = nullptr;
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

    spdlog::debug("[PrinterImageWidget] Detached");
}

void PrinterImageWidget::on_activate() {
    // Re-check printer image (may have changed in settings overlay)
    refresh_printer_image();
}

void PrinterImageWidget::reload_from_config() {
    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[PrinterImageWidget] reload_from_config: Config not available");
        return;
    }

    // Update printer type in PrinterState (triggers capability cache refresh)
    std::string printer_type = config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    get_printer_state().set_printer_type_sync(printer_type);

    // Update printer image
    refresh_printer_image();

    // Update printer type/host overlay
    // Always visible (even for localhost) to maintain consistent flex layout.
    // Hidden flag removes elements from flex, causing printer image to scale differently.
    std::string host = config->get<std::string>(config->df() + helix::wizard::MOONRAKER_HOST, "");

    if (host.empty() || host == "127.0.0.1" || host == "localhost") {
        // Space keeps the text_small at its font height for consistent layout
        lv_subject_copy_string(&s_printer_type_subject, " ");
        lv_subject_set_int(&s_printer_info_visible, 1);
    } else {
        const char* type_str = printer_type.empty() ? "Printer" : printer_type.c_str();
        lv_subject_copy_string(&s_printer_type_subject, type_str);
        lv_subject_copy_string(&s_printer_host_subject, host.c_str());
        lv_subject_set_int(&s_printer_info_visible, 1);
    }
}

void PrinterImageWidget::refresh_printer_image() {
    if (!widget_obj_)
        return;

    // Free old snapshot — image source is about to change
    if (cached_printer_snapshot_) {
        lv_obj_t* img = lv_obj_find_by_name(widget_obj_, "printer_image");
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

    lv_display_t* disp = lv_display_get_default();
    int screen_width = disp ? lv_display_get_horizontal_resolution(disp) : 800;

    // Check for user-selected printer image (custom or shipped override)
    auto& pim = helix::PrinterImageManager::instance();
    std::string custom_path = pim.get_active_image_path(screen_width);
    if (!custom_path.empty()) {
        lv_obj_t* img = lv_obj_find_by_name(widget_obj_, "printer_image");
        if (img) {
            lv_image_set_src(img, custom_path.c_str());
            spdlog::debug("[PrinterImageWidget] User-selected printer image: '{}'", custom_path);
        }
        schedule_printer_image_snapshot();
        return;
    }

    // Auto-detect from printer type using PrinterImages
    Config* config = Config::get_instance();
    std::string printer_type =
        config ? config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "") : "";
    std::string image_path = PrinterImages::get_best_printer_image(printer_type);
    lv_obj_t* img = lv_obj_find_by_name(widget_obj_, "printer_image");
    if (img) {
        lv_image_set_src(img, image_path.c_str());
        spdlog::debug("[PrinterImageWidget] Printer image: '{}' for '{}'", image_path,
                      printer_type);
    }
    schedule_printer_image_snapshot();
}

void PrinterImageWidget::schedule_printer_image_snapshot() {
    // Cancel any pending snapshot timer
    if (snapshot_timer_) {
        lv_timer_delete(snapshot_timer_);
        snapshot_timer_ = nullptr;
    }

    // Defer snapshot until after layout resolves (~50ms)
    snapshot_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<PrinterImageWidget*>(lv_timer_get_user_data(timer));
            if (self) {
                self->snapshot_timer_ = nullptr; // Timer is one-shot, about to be deleted
                self->take_printer_image_snapshot();
            }
            lv_timer_delete(timer);
        },
        50, this);
    lv_timer_set_repeat_count(snapshot_timer_, 1);
}

void PrinterImageWidget::take_printer_image_snapshot() {
    if (!widget_obj_)
        return;

    lv_obj_t* img = lv_obj_find_by_name(widget_obj_, "printer_image");
    if (!img)
        return;

    // Only snapshot if the widget has resolved to a non-zero size
    int32_t w = lv_obj_get_width(img);
    int32_t h = lv_obj_get_height(img);
    if (w <= 0 || h <= 0) {
        spdlog::debug("[PrinterImageWidget] Printer image not laid out yet ({}x{}), skipping "
                      "snapshot",
                      w, h);
        return;
    }

    lv_draw_buf_t* snapshot = lv_snapshot_take(img, LV_COLOR_FORMAT_ARGB8888);
    if (!snapshot) {
        spdlog::warn("[PrinterImageWidget] Failed to take printer image snapshot");
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
    spdlog::debug("[PrinterImageWidget] Snapshot header: magic=0x{:02x} cf={} {}x{} data={}",
                  snap_magic, snap_cf, snap_w, snap_h, fmt::ptr(snapshot->data));

    // Swap image source to the pre-scaled snapshot buffer — LVGL blits 1:1, no scaling
    lv_image_set_src(img, cached_printer_snapshot_);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);

    spdlog::debug("[PrinterImageWidget] Printer image snapshot cached ({}x{}, {} bytes)", snap_w,
                  snap_h, snap_w * snap_h * 4);
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
