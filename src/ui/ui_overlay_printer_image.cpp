// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_overlay_printer_image.cpp
 * @brief Implementation of PrinterImageOverlay
 *
 * Displays shipped and custom printer images in a left-list + right-preview layout.
 * List rows are created from the setting_action_row XML component. The preview panel
 * on the right is driven by subjects for declarative binding (bind_src, bind_text,
 * bind_flag_if_eq).
 */

#include "ui_overlay_printer_image.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "config.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "prerendered_images.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "static_panel_registry.h"
#include "usb_manager.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>
#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<PrinterImageOverlay> g_printer_image_overlay;

PrinterImageOverlay& get_printer_image_overlay() {
    if (!g_printer_image_overlay) {
        g_printer_image_overlay = std::make_unique<PrinterImageOverlay>();
        StaticPanelRegistry::instance().register_destroy("PrinterImageOverlay",
                                                         []() { g_printer_image_overlay.reset(); });
    }
    return *g_printer_image_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrinterImageOverlay::PrinterImageOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

PrinterImageOverlay::~PrinterImageOverlay() {
    // SubjectManager handles subject deinit via RAII
    if (subjects_initialized_) {
        deinit_subjects_base(subjects_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void PrinterImageOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // USB section visibility subject (0=hidden, 1=visible)
    UI_MANAGED_SUBJECT_INT(usb_visible_subject_, 0, "printer_image_usb_visible", subjects_);

    // USB status text subject
    UI_MANAGED_SUBJECT_STRING(usb_status_subject_, usb_status_buf_, "", "printer_image_usb_status",
                              subjects_);

    // Preview panel subjects
    UI_MANAGED_SUBJECT_POINTER(preview_src_subject_, preview_src_buf_, "printer_image_preview_src",
                               subjects_);
    UI_MANAGED_SUBJECT_STRING(preview_name_subject_, preview_name_buf_, "",
                              "printer_image_preview_name", subjects_);
    UI_MANAGED_SUBJECT_INT(has_preview_subject_, 0, "printer_image_has_preview", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void PrinterImageOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_printer_image_auto_detect", on_auto_detect);
    lv_xml_register_event_cb(nullptr, "on_printer_image_card_clicked", on_image_card_clicked);
    lv_xml_register_event_cb(nullptr, "on_printer_image_usb_clicked", on_usb_image_clicked);
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* PrinterImageOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "printer_image_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void PrinterImageOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Register for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack (on_activate will populate lists)
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void PrinterImageOverlay::set_usb_manager(UsbManager* manager) {
    usb_manager_ = manager;
    spdlog::debug("[{}] USB manager set ({})", get_name(), manager ? "valid" : "null");
}

void PrinterImageOverlay::refresh_custom_images() {
    populate_custom_images();
    std::string active_id = helix::PrinterImageManager::instance().get_active_image_id();
    update_selection_indicator(active_id);
}

void PrinterImageOverlay::on_activate() {
    OverlayBase::on_activate();

    populate_shipped_images();
    populate_custom_images();
    scan_usb_drives();

    // Highlight the currently active image
    std::string active_id = helix::PrinterImageManager::instance().get_active_image_id();
    update_selection_indicator(active_id);

    // Show preview for current selection
    if (!active_id.empty()) {
        std::string display_name = active_id;
        auto colon_pos = active_id.find(':');
        if (colon_pos != std::string::npos) {
            display_name = active_id.substr(colon_pos + 1);
        }
        std::string preview_path = get_preview_path_for_id(active_id);
        update_preview(active_id, display_name, preview_path);
    } else {
        // Show the auto-detected image as preview
        std::string auto_path;
        Config* config = Config::get_instance();
        if (config) {
            std::string printer_type =
                config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
            auto_path = PrinterImages::get_best_printer_image(printer_type);
        }
        update_preview("", "Auto-Detect", auto_path);
    }
}

void PrinterImageOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// LIST ROW CREATION
// ============================================================================

lv_obj_t* PrinterImageOverlay::create_list_row(lv_obj_t* parent, const std::string& image_id,
                                               const std::string& display_name,
                                               const char* callback_name) {
    const char* attrs[] = {"label_text", display_name.c_str(), "callback", callback_name, nullptr};
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(parent, "printer_image_list_item", attrs));
    if (!row) {
        spdlog::warn("[{}] Failed to create list row for {}", get_name(), image_id);
        return nullptr;
    }

    // Store image_id in user_data (freed on row delete)
    // TODO: lv_obj user_data is safe here ONLY because printer_image_list_item
    // extends lv_button (which doesn't claim user_data). If the XML component
    // ever extends a custom widget like severity_card that uses user_data
    // internally, this will silently collide. Consider event callback user_data
    // or a C++ side map instead. See L069.
    char* id_copy = strdup(image_id.c_str());
    if (!id_copy) {
        spdlog::error("[{}] Failed to allocate memory for image id", get_name());
        return row;
    }
    lv_obj_set_user_data(row, id_copy);

    // Free strdup'd user_data when LVGL destroys the row
    // (acceptable exception to declarative UI rule for cleanup)
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* obj = lv_event_get_current_target_obj(e);
            void* data = lv_obj_get_user_data(obj);
            if (data) {
                free(data);
            }
        },
        LV_EVENT_DELETE, nullptr);

    return row;
}

// ============================================================================
// PREVIEW
// ============================================================================

void PrinterImageOverlay::update_preview(const std::string& /*image_id*/,
                                         const std::string& display_name,
                                         const std::string& preview_path) {
    // Update preview name via subject binding
    lv_subject_copy_string(&preview_name_subject_, display_name.c_str());

    if (!preview_path.empty()) {
        // Copy path to buffer and update pointer subject
        strncpy(preview_src_buf_, preview_path.c_str(), sizeof(preview_src_buf_) - 1);
        preview_src_buf_[sizeof(preview_src_buf_) - 1] = '\0';
        lv_subject_set_pointer(&preview_src_subject_, preview_src_buf_);
        lv_subject_set_int(&has_preview_subject_, 1);
    } else {
        lv_subject_set_int(&has_preview_subject_, 0);
    }
}

std::string PrinterImageOverlay::get_preview_path_for_id(const std::string& image_id) {
    if (image_id.rfind("shipped:", 0) == 0) {
        std::string name = image_id.substr(8);
        return get_prerendered_printer_path(name, 480);
    }
    if (image_id.rfind("custom:", 0) == 0) {
        auto custom_images = helix::PrinterImageManager::instance().get_custom_images();
        for (const auto& img : custom_images) {
            if (img.id == image_id)
                return img.preview_path;
        }
    }
    return "";
}

// ============================================================================
// LIST POPULATION
// ============================================================================

void PrinterImageOverlay::populate_shipped_images() {
    if (!overlay_root_) {
        return;
    }

    lv_obj_t* list = lv_obj_find_by_name(overlay_root_, "shipped_images_list");
    if (!list) {
        spdlog::warn("[{}] shipped_images_list not found", get_name());
        return;
    }

    lv_obj_clean(list);

    auto images = helix::PrinterImageManager::instance().get_shipped_images();
    spdlog::debug("[{}] Populating {} shipped images", get_name(), images.size());

    for (const auto& img : images) {
        create_list_row(list, img.id, img.display_name, "on_printer_image_card_clicked");
    }
}

void PrinterImageOverlay::populate_custom_images() {
    if (!overlay_root_) {
        return;
    }

    lv_obj_t* list = lv_obj_find_by_name(overlay_root_, "custom_images_list");
    if (!list) {
        spdlog::warn("[{}] custom_images_list not found", get_name());
        return;
    }

    lv_obj_clean(list);

    // Auto-import any raw PNG/JPEG files dropped into the custom_images directory
    helix::PrinterImageManager::instance().auto_import_raw_images();

    auto valid_images = helix::PrinterImageManager::instance().get_custom_images();
    auto invalid_images = helix::PrinterImageManager::instance().get_invalid_custom_images();
    spdlog::debug("[{}] Populating {} custom images ({} invalid)", get_name(), valid_images.size(),
                  invalid_images.size());

    // Merge valid and invalid into one list sorted by display_name
    struct CustomEntry {
        const helix::PrinterImageManager::ImageInfo* info;
        bool valid;
    };
    std::vector<CustomEntry> merged;
    merged.reserve(valid_images.size() + invalid_images.size());
    for (const auto& img : valid_images)
        merged.push_back({&img, true});
    for (const auto& img : invalid_images)
        merged.push_back({&img, false});
    std::sort(merged.begin(), merged.end(), [](const CustomEntry& a, const CustomEntry& b) {
        return a.info->display_name < b.info->display_name;
    });

    for (const auto& entry : merged) {
        lv_obj_t* row = create_list_row(list, entry.info->id, entry.info->display_name,
                                        "on_printer_image_card_clicked");
        if (row && !entry.valid) {
            lv_obj_add_state(row, LV_STATE_DISABLED);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void PrinterImageOverlay::update_selection_indicator(const std::string& active_id) {
    if (!overlay_root_) {
        return;
    }

    auto update_list = [&](const char* list_name) {
        lv_obj_t* list = lv_obj_find_by_name(overlay_root_, list_name);
        if (!list)
            return;

        uint32_t count = lv_obj_get_child_count(list);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* child = lv_obj_get_child(list, static_cast<int32_t>(i));
            auto* id = static_cast<const char*>(lv_obj_get_user_data(child));
            if (id && std::string(id) == active_id) {
                lv_obj_add_state(child, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(child, LV_STATE_CHECKED);
            }
        }
    };

    update_list("shipped_images_list");
    update_list("custom_images_list");
}

// ============================================================================
// USB IMPORT
// ============================================================================

void PrinterImageOverlay::scan_usb_drives() {
    if (!usb_manager_ || !usb_manager_->is_running()) {
        lv_subject_set_int(&usb_visible_subject_, 0);
        return;
    }

    auto drives = usb_manager_->get_drives();
    if (drives.empty()) {
        lv_subject_set_int(&usb_visible_subject_, 0);
        return;
    }

    lv_subject_set_int(&usb_visible_subject_, 1);
    spdlog::debug("[{}] Found {} USB drive(s), scanning first: {}", get_name(), drives.size(),
                  drives[0].mount_path);
    populate_usb_images(drives[0].mount_path);
}

void PrinterImageOverlay::populate_usb_images(const std::string& mount_path) {
    lv_obj_t* list = lv_obj_find_by_name(overlay_root_, "usb_images_list");
    if (!list) {
        spdlog::warn("[{}] usb_images_list not found", get_name());
        return;
    }

    lv_obj_clean(list);

    auto image_paths = helix::PrinterImageManager::instance().scan_for_images(mount_path);
    spdlog::debug("[{}] Found {} importable images on USB", get_name(), image_paths.size());

    if (image_paths.empty()) {
        lv_subject_copy_string(&usb_status_subject_, lv_tr("No images found on USB drive"));
        return;
    }

    lv_subject_copy_string(&usb_status_subject_, "");

    for (const auto& path : image_paths) {
        std::string filename = std::filesystem::path(path).filename().string();

        // USB rows use a different callback (import behavior vs select)
        // and store the full path as image_id for the import handler
        create_list_row(list, path, filename, "on_printer_image_usb_clicked");
    }
}

void PrinterImageOverlay::handle_usb_import(const std::string& source_path) {
    std::string filename = std::filesystem::path(source_path).filename().string();
    spdlog::info("[{}] Importing USB image: {}", get_name(), filename);

    // Update status via subject binding
    std::string msg = "Importing " + filename + "...";
    lv_subject_copy_string(&usb_status_subject_, msg.c_str());

    // import_image_async() currently runs synchronously, but the callback is wrapped
    // in helix::ui::queue_update() for safety in case the implementation becomes truly async
    helix::PrinterImageManager::instance().import_image_async(
        source_path, [filename](helix::PrinterImageManager::ImportResult result) {
            helix::ui::queue_update([result = std::move(result), filename]() {
                auto& overlay = get_printer_image_overlay();

                if (result.success) {
                    spdlog::info("[Printer Image] USB import success: {}", result.id);
                    lv_subject_copy_string(&overlay.usb_status_subject_, "");
                    overlay.refresh_custom_images();
                    overlay.handle_image_selected(result.id);
                    NOTIFY_SUCCESS("Imported {}", filename);
                } else {
                    spdlog::warn("[Printer Image] USB import failed: {}", result.error);
                    lv_subject_copy_string(&overlay.usb_status_subject_, result.error.c_str());
                    NOTIFY_WARNING("Import failed: {}", result.error);
                }
            });
        });
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void PrinterImageOverlay::handle_auto_detect() {
    spdlog::info("[{}] Auto-detect selected", get_name());
    helix::PrinterImageManager::instance().set_active_image("");
    update_selection_indicator("");

    // Show the auto-detected image as preview
    std::string auto_path;
    Config* config = Config::get_instance();
    if (config) {
        std::string printer_type =
            config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
        auto_path = PrinterImages::get_best_printer_image(printer_type);
    }
    update_preview("", "Auto-Detect", auto_path);
    NOTIFY_INFO("Printer image set to auto-detect");
}

void PrinterImageOverlay::handle_image_selected(const std::string& image_id) {
    if (image_id.rfind("invalid:", 0) == 0) {
        spdlog::warn("[{}] Ignoring selection of invalid image: {}", get_name(), image_id);
        return;
    }
    spdlog::info("[{}] Image selected: {}", get_name(), image_id);
    helix::PrinterImageManager::instance().set_active_image(image_id);
    update_selection_indicator(image_id);

    // Extract display name from ID for notification and preview
    std::string display_name = image_id;
    auto colon_pos = image_id.find(':');
    if (colon_pos != std::string::npos) {
        display_name = image_id.substr(colon_pos + 1);
    }

    // Update preview panel
    std::string preview_path = get_preview_path_for_id(image_id);
    update_preview(image_id, display_name, preview_path);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void PrinterImageOverlay::on_auto_detect(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageOverlay] on_auto_detect");
    get_printer_image_overlay().handle_auto_detect();
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageOverlay::on_image_card_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageOverlay] on_image_card_clicked");
    // Get the row root (current_target = obj with the handler = setting_action_row view)
    auto* row = lv_event_get_current_target_obj(e);
    auto* id = static_cast<const char*>(lv_obj_get_user_data(row));
    if (id) {
        get_printer_image_overlay().handle_image_selected(std::string(id));
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterImageOverlay::on_usb_image_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterImageOverlay] on_usb_image_clicked");
    // Get the row root (current_target = obj with the handler = setting_action_row view)
    auto* row = lv_event_get_current_target_obj(e);
    auto* path = static_cast<const char*>(lv_obj_get_user_data(row));
    if (path) {
        get_printer_image_overlay().handle_usb_import(std::string(path));
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
