// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_panel_print_select.cpp
 * @brief Print file selection panel with file browser and metadata display
 *
 * @pattern Panel with deferred dependency propagation
 * @threading File operations may be async
 * @gotchas set_api() must propagate to file_provider_->set_api()
 */

#include "ui_panel_print_select.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_filename_utils.h"
#include "ui_fonts.h"
#include "ui_format_utils.h"
#include "ui_icon.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_preflight_check_modal.h"
#include "ui_print_select_file_sorter.h"
#include "ui_print_select_history.h"
#include "ui_print_select_path_navigator.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "connection_state.h" // For ConnectionState enum
#include "display_manager.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "gcode_parser.h" // For extract_thumbnails_from_content (USB thumbnail fallback)
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "preprint_predictor.h"
#include "print_history_manager.h"
#include "print_start_analyzer.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "thumbnail_cache.h"
#include "usb_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace helix;
using helix::gcode::strip_gcode_extension;
using helix::ui::format_filament_weight;
using helix::ui::format_layer_count;
using helix::ui::format_print_height;
using helix::ui::format_print_time;

// ============================================================================
// Global Instance
// ============================================================================

// Slurp a file into memory. Used to hand an on-disk thumbnail to the prescaler,
// which takes PNG bytes rather than a path. Returns empty on any failure — every
// caller treats that as "no thumbnail" rather than an error worth surfacing.
static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        return {};
    }
    return data;
}

static std::unique_ptr<PrintSelectPanel> g_print_select_panel;

PrintSelectPanel* get_print_select_panel(PrinterState& printer_state, IMoonrakerAPI* api) {
    if (!g_print_select_panel) {
        g_print_select_panel = std::make_unique<PrintSelectPanel>(printer_state, api);
        // Register both deinit AND destruction in one callback (consistent with other panels)
        StaticPanelRegistry::instance().register_destroy("PrintSelectPanel", []() {
            g_print_select_panel->deinit_subjects();
            g_print_select_panel.reset();
        });
    }
    return g_print_select_panel.get();
}

PrintSelectPanel& get_global_print_select_panel() {
    if (!g_print_select_panel) {
        spdlog::error(
            "[PrintSelectPanel] get_global_print_select_panel() called before panel created");
    }
    return *g_print_select_panel;
}

// ============================================================================
// Static XML Event Callbacks (registered via lv_xml_register_event_cb)
// ============================================================================

static void on_print_select_view_toggle(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().toggle_view();
}

static void on_print_select_source_printer(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().on_source_printer_clicked();
}

static void on_print_select_source_usb(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().on_source_usb_clicked();
}

// Header column sort callbacks
static void on_print_select_header_filename(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().sort_by(PrintSelectSortColumn::FILENAME);
}

static void on_print_select_header_size(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().sort_by(PrintSelectSortColumn::SIZE);
}

static void on_print_select_header_modified(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().sort_by(PrintSelectSortColumn::MODIFIED);
}

static void on_print_select_header_print_time(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().sort_by(PrintSelectSortColumn::PRINT_TIME);
}

// Detail view callbacks
static void on_print_select_print_button(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().start_print();
}

static void on_print_select_delete_button(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().show_delete_confirmation();
}

static void on_toggle_sliced_colors(lv_event_t* e) {
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    get_global_print_select_panel().forward_sliced_colors_toggle(checked);
}

static void on_print_select_detail_backdrop(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* current_target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    // Only close if clicking the backdrop itself, not child widgets
    if (target == current_target) {
        get_global_print_select_panel().hide_detail_view();
    }
}

static void on_print_detail_back_clicked(lv_event_t* e) {
    (void)e;
    get_global_print_select_panel().hide_detail_view();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

PrintSelectPanel::PrintSelectPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::trace("[{}] Constructed", get_name());
}

PrintSelectPanel::~PrintSelectPanel() {
    // Expire all outstanding lifetime tokens up front so any thumbnail/metadata
    // apply callbacks still queued on the UpdateQueue no-op on their next tick
    // instead of dereferencing this freed panel (the guard's own dtor also does
    // this, but doing it first makes cancellation deterministic at teardown start).
    lifetime_.invalidate();

    // Deinitialize subjects first to disconnect observers [L041]
    deinit_subjects();

    // Signal ThumbnailLoadContext compatibility flag
    thumbnail_alive_->store(false);

    // Remove history manager observer first (simple pointer comparison removal)
    if (history_observer_) {
        auto* history_manager = get_print_history_manager();
        if (history_manager) {
            history_manager->remove_observer(&history_observer_);
        }
    }

    // Unregister file list change notification handler
    // CRITICAL: During static destruction, MoonrakerManager may already be destroyed
    // causing the api_ pointer to reference a destroyed client. Guard by checking
    // if the global manager is still valid (it returns nullptr after destruction).
    auto* mgr = get_moonraker_manager();
    if (mgr && api_ && !filelist_handler_name_.empty()) {
        api_->unregister_method_callback("notify_filelist_changed", filelist_handler_name_);
    }

    // CRITICAL: During static destruction (app exit), LVGL may already be gone.
    // We check if LVGL is still initialized before calling any LVGL functions.
    if (lv_is_initialized()) {
        // Remove scroll event callbacks to prevent use-after-free
        if (card_view_container_) {
            lv_obj_remove_event_cb(card_view_container_, on_scroll_static);
        }
        if (list_rows_container_) {
            lv_obj_remove_event_cb(list_rows_container_, on_scroll_static);
        }

        // Delete pending timers
        if (refresh_timer_) {
            lv_timer_delete(refresh_timer_);
            refresh_timer_ = nullptr;
        }
        if (file_poll_timer_) {
            lv_timer_delete(file_poll_timer_);
            file_poll_timer_ = nullptr;
        }
    }

    // print_controller_ cleanup happens automatically via unique_ptr destructor

    // Cleanup extracted view modules (handles observer removal internally)
    if (card_view_) {
        card_view_->cleanup();
    }
    if (list_view_) {
        list_view_->cleanup();
    }
    if (detail_view_) {
        detail_view_->cleanup();
    }

    // Reset widget references - the LVGL widget tree handles widget cleanup.
    card_view_container_ = nullptr;
    list_view_container_ = nullptr;
    list_rows_container_ = nullptr;
    empty_state_container_ = nullptr;
    view_toggle_btn_ = nullptr;
    view_toggle_icon_ = nullptr;
    print_status_panel_widget_ = nullptr;

    // Guard against static destruction order fiasco (spdlog may be gone)
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[PrintSelectPanel] Destroyed");
    }
}

// ============================================================================
// PanelBase Implementation
// ============================================================================

void PrintSelectPanel::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize selected file subjects
    UI_MANAGED_SUBJECT_STRING(selected_filename_subject_, selected_filename_buffer_, "",
                              "selected_filename", subjects_);
    UI_MANAGED_SUBJECT_STRING(selected_display_filename_subject_, selected_display_filename_buffer_,
                              "", "selected_display_filename", subjects_);

    // Thumbnail uses POINTER subject (required by lv_image_bind_src)
    // Use get_default_thumbnail() for pre-rendered .bin support
    std::string default_thumb = helix::ui::PrintSelectCardView::get_default_thumbnail();
    strncpy(selected_thumbnail_buffer_, default_thumb.c_str(),
            sizeof(selected_thumbnail_buffer_) - 1);
    UI_MANAGED_SUBJECT_POINTER(selected_thumbnail_subject_, selected_thumbnail_buffer_,
                               "selected_thumbnail", subjects_);

    // Detail view thumbnail - uses cached PNG for better upscaling quality
    strncpy(selected_detail_thumbnail_buffer_, default_thumb.c_str(),
            sizeof(selected_detail_thumbnail_buffer_) - 1);
    UI_MANAGED_SUBJECT_POINTER(selected_detail_thumbnail_subject_,
                               selected_detail_thumbnail_buffer_, "selected_detail_thumbnail",
                               subjects_);
    // Publish nullptr (not the empty default buffer) so the immediate bind_src on
    // overlay creation doesn't hit LVGL's empty-buffer warning path (#990).
    lv_subject_set_pointer(&selected_detail_thumbnail_subject_,
                           thumbnail_subject_value(selected_detail_thumbnail_buffer_));

    UI_MANAGED_SUBJECT_STRING(selected_print_time_subject_, selected_print_time_buffer_, "",
                              "selected_print_time", subjects_);
    UI_MANAGED_SUBJECT_STRING(selected_filament_weight_subject_, selected_filament_weight_buffer_,
                              "", "selected_filament_weight", subjects_);
    UI_MANAGED_SUBJECT_STRING(selected_layer_count_subject_, selected_layer_count_buffer_, "",
                              "selected_layer_count", subjects_);
    UI_MANAGED_SUBJECT_STRING(selected_print_height_subject_, selected_print_height_buffer_, "",
                              "selected_print_height", subjects_);
    UI_MANAGED_SUBJECT_STRING(selected_layer_height_subject_, selected_layer_height_buffer_, "",
                              "selected_layer_height", subjects_);
    UI_MANAGED_SUBJECT_STRING(selected_filament_type_subject_, selected_filament_type_buffer_, "",
                              "selected_filament_type", subjects_);
    // Initialize detail view visibility subject (0 = hidden, 1 = visible)
    UI_MANAGED_SUBJECT_INT(detail_view_visible_subject_, 0, "detail_view_visible", subjects_);

    // Initialize view mode subject (0 = CARD, 1 = LIST) - XML bindings control container visibility
    UI_MANAGED_SUBJECT_INT(view_mode_subject_, 0, "print_select_view_mode", subjects_);

    // Initialize can print subject (1 = can print, 0 = print in progress)
    // XML binding disables print button when value is 0
    bool can_print = printer_state_.can_start_new_print();
    UI_MANAGED_SUBJECT_INT(can_print_subject_, can_print ? 1 : 0, "print_select_can_print",
                           subjects_);

    // Initialize USB source subject (before XML is parsed so bindings resolve)
    helix::ui::PrintSelectUsbSource::init_subjects();

    // Register XML event callbacks (must be done BEFORE XML is created)
    register_xml_callbacks({
        {"on_print_select_view_toggle", on_print_select_view_toggle},
        {"on_print_select_source_printer", on_print_select_source_printer},
        {"on_print_select_source_usb", on_print_select_source_usb},
        // List header sort callbacks
        {"on_print_select_header_filename", on_print_select_header_filename},
        {"on_print_select_header_size", on_print_select_header_size},
        {"on_print_select_header_modified", on_print_select_header_modified},
        {"on_print_select_header_print_time", on_print_select_header_print_time},
        // Detail view callbacks
        {"on_print_select_print_button", on_print_select_print_button},
        {"on_print_select_delete_button", on_print_select_delete_button},
        {"on_print_select_detail_backdrop", on_print_select_detail_backdrop},
        {"on_print_detail_back_clicked", on_print_detail_back_clicked},
        {"on_toggle_sliced_colors", on_toggle_sliced_colors},
    });

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void PrintSelectPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // SubjectManager handles all subject cleanup via RAII
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[PrintSelectPanel] Subjects deinitialized");
}

void PrintSelectPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] Cannot setup: panel is null", get_name());
        return;
    }

    // Find widget references
    card_view_container_ = lv_obj_find_by_name(panel_, "card_view_container");
    list_view_container_ = lv_obj_find_by_name(panel_, "list_view_container");
    list_rows_container_ = lv_obj_find_by_name(panel_, "list_rows_container");
    empty_state_container_ = lv_obj_find_by_name(panel_, "empty_state_container");
    view_toggle_btn_ = lv_obj_find_by_name(panel_, "view_toggle_btn");
    view_toggle_icon_ = lv_obj_find_by_name(panel_, "view_toggle_btn_icon");

    if (!card_view_container_ || !list_view_container_ || !list_rows_container_ ||
        !empty_state_container_ || !view_toggle_btn_ || !view_toggle_icon_) {
        spdlog::error("[{}] Failed to find required widgets", get_name());
        return;
    }

    // Register scroll event handlers for progressive loading
    lv_obj_add_event_cb(card_view_container_, on_scroll_static, LV_EVENT_SCROLL, this);
    lv_obj_add_event_cb(list_rows_container_, on_scroll_static, LV_EVENT_SCROLL, this);

    // Create and setup virtualized view modules
    auto* self = this;

    card_view_ = std::make_unique<helix::ui::PrintSelectCardView>();
    card_view_->setup(
        card_view_container_,
        // File click callback
        [self](size_t file_index) { self->handle_file_click(file_index); },
        // Metadata fetch callback
        [self](size_t start, size_t end) { self->fetch_metadata_range(start, end); });

    // Long-press on a file card → delete confirmation (card view only; list view unchanged).
    card_view_->set_on_file_long_press(
        [self](size_t file_index) { self->on_file_long_pressed(file_index); });

    list_view_ = std::make_unique<helix::ui::PrintSelectListView>();
    list_view_->setup(
        list_rows_container_,
        // File click callback
        [self](size_t file_index) { self->handle_file_click(file_index); },
        // Metadata fetch callback
        [self](size_t start, size_t end) { self->fetch_metadata_range(start, end); });

    // Note: view_toggle_btn, source buttons, and header click handlers are now in XML via
    // <event_cb>

    // Initialize USB source manager
    usb_source_ = std::make_unique<helix::ui::PrintSelectUsbSource>();
    usb_source_->setup(panel);
    // Apply pending USB manager if set_usb_manager was called before setup()
    if (pending_usb_manager_) {
        usb_source_->set_usb_manager(pending_usb_manager_);
    }
    usb_source_->set_on_source_changed([self](FileSource source) {
        if (source == FileSource::PRINTER) {
            self->refresh_files();
        }
        // USB source refresh is handled by usb_source_ internally via on_files_ready callback
    });
    usb_source_->set_on_files_ready([self](std::vector<PrintFileData>&& files) {
        // USB files have no Moonraker metadata - mark all as "fetched" to skip metadata requests
        for (auto& file : files) {
            file.metadata_fetched = true;
        }
        self->file_list_ = std::move(files);

        self->apply_sort();
        // Preserve scroll if still in the same directory (e.g., refresh after file changes)
        bool same_dir = (self->current_path_ == self->last_populated_path_);
        if (self->current_view_mode_ == PrintSelectViewMode::CARD) {
            self->populate_card_view(same_dir);
        } else {
            self->populate_list_view(same_dir);
        }
        self->last_populated_path_ = self->current_path_;
        self->update_empty_state();
    });

    // Initialize file data provider for Moonraker files
    file_provider_ = std::make_unique<helix::ui::PrintSelectFileProvider>();
    file_provider_->set_api(api_);
    file_provider_->set_on_files_ready([self, token = self->lifetime_.token()](
                                           std::vector<PrintFileData>&& files) {
        spdlog::trace("[{}] on_files_ready callback: received {} items from provider",
                      self->get_name(), files.size());
        for (size_t i = 0; i < files.size() && i < 30; ++i) {
            spdlog::trace("[{}]   file[{}]: '{}' (dir={}, modified={})", self->get_name(), i,
                          files[i].filename, files[i].is_dir, files[i].modified_timestamp);
        }
        if (files.size() > 30) {
            spdlog::trace("[{}]   ... and {} more items", self->get_name(), files.size() - 30);
        }

        // CRITICAL: Defer ALL work to main thread
        // WebSocket callbacks run on libhv thread - direct LVGL calls cause crashes
        struct FilesReadyContext {
            PrintSelectPanel* panel;
            helix::LifetimeToken token;
            std::vector<PrintFileData> files;
        };
        auto ctx =
            std::make_unique<FilesReadyContext>(FilesReadyContext{self, token, std::move(files)});

        helix::ui::queue_update<FilesReadyContext>(std::move(ctx), [](FilesReadyContext* c) {
            if (c->token.expired()) {
                spdlog::warn("[PrintSelectPanel] on_files_ready: lifetime token expired, "
                             "dropping {} files",
                             c->files.size());
                return;
            }
            auto* panel = c->panel;

            spdlog::trace("[{}] on_files_ready: applying {} files on main thread",
                          panel->get_name(), c->files.size());

            panel->refresh_guard_.release();

            // Snapshot previous file list (filename + modified time, in sorted
            // order) before replacing — used to skip repopulation when nothing
            // changed. Timestamps catch re-uploads of the same filename.
            struct FileSnapshot {
                std::string filename;
                time_t modified;
            };
            const size_t prev_count = panel->file_list_.size();
            std::vector<FileSnapshot> prev_files;
            prev_files.reserve(prev_count);
            for (const auto& f : panel->file_list_) {
                prev_files.push_back({f.filename, f.modified_timestamp});
            }

            // Preserve cached metadata from previous file list before replacing.
            // Without this, all metadata resets on every refresh, causing expensive
            // metadata re-fetches and metascans on every scroll cycle (8-15s per
            // file on AD5M). We carry forward the entire old entry for files that
            // already had metadata fetched, updating only the file listing fields
            // (size, modified time) from the fresh data.
            std::unordered_map<std::string, PrintFileData> old_state;
            for (auto& f : panel->file_list_) {
                if (f.metadata_fetched) {
                    old_state.emplace(f.filename, std::move(f));
                }
            }

            // Move data into panel (now safe - on main thread)
            panel->file_list_ = std::move(c->files);

            // Merge old metadata into new file list
            const bool retry_missing = panel->retry_missing_thumbnails_on_refresh_;
            panel->retry_missing_thumbnails_on_refresh_ = false;
            for (auto& f : panel->file_list_) {
                auto it = old_state.find(f.filename);
                if (it != old_state.end()) {
                    auto& old = it->second;
                    // Keep fresh listing data (size, modified time may have changed)
                    time_t new_modified = f.modified_timestamp;
                    size_t new_size = f.file_size_bytes;

                    if (should_carry_forward_print_file_metadata(old, new_size, retry_missing)) {
                        // File unchanged — carry forward all cached metadata
                        f = std::move(old);
                        f.modified_timestamp = new_modified;
                        f.file_size_bytes = new_size;
                    } else {
                        // Carry-forward declined (size changed OR retry-missing-
                        // thumbnail kicked in). The provider already preserved this
                        // entry's metadata_fetched=true / thumbnail_path before our
                        // decision ran, so without an explicit reset here the next
                        // fetch_metadata_range would short-circuit on the stale
                        // metadata_fetched flag and the placeholder would persist
                        // (the bug the retry-on-activate flag was meant to fix —
                        // 8dc2f8fde). Force a fresh fetch.
                        f.metadata_fetched = false;
                        f.thumbnail_path.clear();
                    }
                }
            }

            panel->apply_sort();

#if defined(HELIX_PLATFORM_ESP32)
            // Task 11 R1: cap the browsable list to the newest N files. No
            // local disk/pagination UI on this platform — an unbounded list
            // from a printer with years of prints would be an unusable
            // scroll and a PSRAM/RAM cost for card recycling metadata.
            // apply_sort() above has just guaranteed dirs-first + newest-
            // files-first, so a straight tail-truncate keeps the newest N.
            // Cap runs every refresh (needed for correctness even when
            // nothing else changed), but the toast is deferred until we know
            // below whether the list actually changed — this poll fires
            // every 5s and frequently returns identical data, and
            // ToastManager has no dedup, so toasting here unconditionally
            // would spam the same message on every poll (review finding).
            constexpr size_t ESP32_MAX_FILE_COUNT = 50;
            bool esp32_list_capped =
                cap_print_file_list_to_newest(panel->file_list_, ESP32_MAX_FILE_COUNT);
#endif

            panel->merge_history_into_file_list(); // Populate history status for each file
            panel->update_sort_indicators();

            // Skip repopulation when the file list hasn't materially changed.
            // The 5-second poll frequently returns the same data; rebuilding the
            // card view each time causes thumbnail flashing and scrambling because
            // populate() invalidates all pool indices and triggers new metadata
            // fetches that race with stale async callbacks.
            bool same_dir = (panel->current_path_ == panel->last_populated_path_);
            bool list_changed = !same_dir || panel->file_list_.size() != prev_count;
            if (!list_changed) {
                for (size_t i = 0; i < panel->file_list_.size(); i++) {
                    if (panel->file_list_[i].filename != prev_files[i].filename ||
                        panel->file_list_[i].modified_timestamp != prev_files[i].modified) {
                        list_changed = true;
                        break;
                    }
                }
            }

            if (list_changed) {
                spdlog::debug("[{}] File list changed, repopulating", panel->get_name());
#if defined(HELIX_PLATFORM_ESP32)
                if (esp32_list_capped) {
                    ToastManager::instance().show(
                        ToastSeverity::INFO,
                        lv_tr("Showing the 50 newest files. See more in the printer's web UI."));
                }
#endif
                if (panel->current_view_mode_ == PrintSelectViewMode::CARD) {
                    panel->populate_card_view(same_dir);
                } else {
                    panel->populate_list_view(same_dir);
                }
            } else {
                spdlog::trace("[{}] File list unchanged, skipping repopulation", panel->get_name());
            }
            panel->last_populated_path_ = panel->current_path_;

            panel->update_empty_state();

            // Check for pending file selection
            std::string pending;
            if (!panel->pending_file_selection_.empty()) {
                pending = panel->pending_file_selection_;
                panel->pending_file_selection_.clear();
            } else if (get_runtime_config()->select_file != nullptr) {
                static bool select_file_checked = false;
                if (!select_file_checked) {
                    pending = get_runtime_config()->select_file;
                    select_file_checked = true;
                }
            }
            if (!pending.empty()) {
                if (!panel->select_file_by_name(pending)) {
                    spdlog::warn("[{}] Pending file selection '{}' not found in file list",
                                 panel->get_name(), pending);
                }
            }

            // Fetch metadata for visible items
            int visible_start = 0, visible_end = 0;
            if (panel->current_view_mode_ == PrintSelectViewMode::CARD && panel->card_view_) {
                panel->card_view_->get_visible_range(visible_start, visible_end);
            } else if (panel->list_view_) {
                panel->list_view_->get_visible_range(visible_start, visible_end);
            }
            if (visible_end == 0 && !panel->file_list_.empty()) {
                visible_end = static_cast<int>(std::min(panel->file_list_.size(), size_t{20}));
            }
            panel->fetch_metadata_range(static_cast<size_t>(visible_start),
                                        static_cast<size_t>(visible_end));
        });
    });
    file_provider_->set_on_error([self, token = self->lifetime_.token()](const std::string& error) {
        LOG_ERROR_INTERNAL("[{}] File list refresh error: {}", self->get_name(), error);
        token.defer([self, error]() {
            self->refresh_guard_.release();
            if (dir_error_should_reset_to_root(error, self->path_navigator_.is_at_root())) {
                // The current directory no longer exists on the server (e.g. a
                // FlashForge path-doubling artifact). Retrying it would wedge the
                // panel, so fall back to root instead of looping (TJVQDCZ6).
                spdlog::warn("[{}] Current directory missing ('{}'); falling back to root",
                             self->get_name(), error);
                self->path_navigator_.reset();
                self->current_path_.clear();
                self->refresh_files(/*force=*/true);
                return;
            }
            NOTIFY_ERROR(lv_tr("Failed to refresh file list"));
        });
    });

    // Create detail view (confirmation dialog created on-demand)
    create_detail_view();

    // Register resize callback
    // Note: register_resize_callback expects a C callback, so we use a static trampoline
    // We store 'this' in a static variable since the resize system doesn't support user_data
    // This is safe because there's only one PrintSelectPanel instance
    static PrintSelectPanel* resize_self = nullptr;
    resize_self = this;
    if (auto* dm = DisplayManager::instance()) {
        dm->register_resize_callback([]() {
            if (resize_self) {
                resize_self->handle_resize();
            }
        });
    }

    // Mark panel as fully initialized (enables resize callbacks)
    panel_initialized_ = true;

    // Check CLI flag for initial list view mode (--print-select-list)
    if (get_runtime_config()->print_select_list_mode) {
        // Start in list mode instead of default card mode
        current_view_mode_ = PrintSelectViewMode::LIST;
        lv_subject_set_int(&view_mode_subject_, 1);
        ui_icon_set_source(view_toggle_icon_, "grid_view");
        spdlog::debug("[{}] Starting in list view mode (CLI flag)", get_name());
    }

    // Refresh from Moonraker when API becomes available (via set_api)
    // Don't populate anything here - wait for API connection
    if (api_) {
        refresh_files();
    } else {
        spdlog::debug("[{}] IMoonrakerAPI not available yet, waiting for set_api()", get_name());
        update_empty_state();
    }

    // Register observer on connection state to refresh files when printer connects
    // This handles the race condition where panel activates before WebSocket connection
    using helix::ui::observe_int_sync;
    lv_subject_t* connection_subject = printer_state_.get_printer_connection_state_subject();
    if (connection_subject) {
        connection_observer_ = observe_int_sync<PrintSelectPanel>(
            connection_subject, this, [](PrintSelectPanel* self, int state) {
                if (state == static_cast<int>(ConnectionState::CONNECTED)) {
                    // Always refresh on (re)connect to pick up files uploaded while
                    // disconnected. The previous guard (file_list_.empty()) silently
                    // skipped refresh after reconnects on unreliable hardware like
                    // CB1, leaving newly-uploaded files invisible (#577).
                    bool is_printer_source =
                        !self->usb_source_ || !self->usb_source_->is_usb_active();
                    if (is_printer_source) {
                        spdlog::info(
                            "[{}] Connection (re)established, refreshing file list (existing={})",
                            self->get_name(), self->file_list_.size());
                        // Force: previous RPC was lost with the old socket
                        self->refresh_files(/*force=*/true);
                    }

                    // Check USB symlink now that connection is established
                    // (moved from set_api() which runs before connection)
                    if (self->usb_source_) {
                        self->check_moonraker_usb_symlink();
                    }

                    // Update installer's websocket URL for local/remote detection
                    if (self->api_) {
                        self->plugin_installer_.set_websocket_url(self->api_->get_websocket_url());
                    }
                    // Note: Plugin detection now happens automatically in discovery flow
                    // (application.cpp). Install prompt is triggered by helix_plugin_observer_.
                }
            });
        spdlog::trace("[{}] Registered observer on connection state for auto-refresh", get_name());
    }

    // Register observer on print job state enum to enable/disable print button
    // Prevents starting a new print while one is already in progress
    // NOTE: get_print_state_enum_subject() is INT, get_print_state_subject() is STRING
    // RAW_PRINT_STATE_OK: pairs with the print_filename read below, which still
    // holds the PREVIOUS job during a preparing window.
    lv_subject_t* print_state_subject = printer_state_.get_print_state_enum_subject();
    if (print_state_subject) {
        print_state_observer_ = observe_int_sync<PrintSelectPanel>(
            print_state_subject, this,
            [](PrintSelectPanel* self, int) { self->update_print_button_state(); });
        spdlog::trace("[{}] Registered observer on print job state for print button", get_name());
    }

    // Also observe print_in_progress subject - this fires immediately when Print is tapped
    // (before Moonraker reports state change, which can take seconds)
    lv_subject_t* print_in_progress_subject = printer_state_.get_print_in_progress_subject();
    if (print_in_progress_subject) {
        print_in_progress_observer_ = observe_int_sync<PrintSelectPanel>(
            print_in_progress_subject, this,
            [](PrintSelectPanel* self, int) { self->update_print_button_state(); });
        spdlog::trace("[{}] Registered observer on print_in_progress for print button", get_name());
    }

    // Register observer on helix_plugin_installed to show install prompt when plugin not available
    // Subject uses tri-state: -1=unknown (pre-discovery), 0=not installed, 1=installed
    // Only show modal when explicitly 0 (after discovery confirms plugin is missing)
    lv_subject_t* plugin_subject = printer_state_.get_helix_plugin_installed_subject();
    if (plugin_subject) {
        helix_plugin_observer_ = observe_int_sync<PrintSelectPanel>(
            plugin_subject, this, [](PrintSelectPanel* self, int plugin_state) {
                // Only show modal when state is explicitly 0 (checked and not installed)
                // Skip if -1 (unknown/pre-discovery) or 1 (installed)
                if (plugin_state == 0 && self->plugin_installer_.should_prompt_install()) {
                    spdlog::info("[PrintSelectPanel] helix_print plugin not available, showing "
                                 "install prompt");
                    self->plugin_install_modal_.set_installer(&self->plugin_installer_);
                    self->plugin_install_modal_.show(lv_screen_active());
                }
            });
        spdlog::trace("[{}] Registered observer on helix_plugin_installed for install prompt",
                      get_name());
    }

    // Register observer on PrintHistoryManager to update file status when history changes
    // (e.g., when a print completes). PrintHistoryManager uses pointer-based observer pattern.
    auto* history_manager = get_print_history_manager();
    if (history_manager && !history_observer_) {
        history_observer_ = [this]() {
            // This runs on main thread (PrintHistoryManager uses ui_queue_update)
            spdlog::trace("[{}] History changed, merging status into file list", get_name());
            merge_history_into_file_list();
            schedule_view_refresh(); // Debounced refresh
        };
        history_manager->add_observer(&history_observer_);
        spdlog::trace("[{}] Registered observer on PrintHistoryManager for history updates",
                      get_name());
    }

    spdlog::trace("[{}] Setup complete", get_name());
}

// ============================================================================
// Public API
// ============================================================================

void PrintSelectPanel::toggle_view() {
    if (current_view_mode_ == PrintSelectViewMode::CARD) {
        // Switch to list view
        current_view_mode_ = PrintSelectViewMode::LIST;

        // Update reactive subject - XML bindings handle container visibility
        lv_subject_set_int(&view_mode_subject_, 1);

        // Update icon to show grid_view (indicates you can switch back to card view)
        ui_icon_set_source(view_toggle_icon_, "grid_view");
        spdlog::debug("[{}] Switched to list view", get_name());

        // Populate list view (initializes pool if needed)
        populate_list_view();

        // Animate list container entrance with crossfade
        animate_view_entrance(list_view_container_);

        // Animate list rows with staggered entrance (runs in parallel with container fade)
        if (list_view_) {
            list_view_->animate_entrance();
        }
    } else {
        // Switch to card view
        current_view_mode_ = PrintSelectViewMode::CARD;

        // Update reactive subject - XML bindings handle container visibility
        lv_subject_set_int(&view_mode_subject_, 0);

        // Update icon to show list (indicates you can switch to list view)
        ui_icon_set_source(view_toggle_icon_, "list");
        spdlog::debug("[{}] Switched to card view", get_name());

        // Repopulate card view
        populate_card_view();

        // Animate card container entrance with crossfade
        animate_view_entrance(card_view_container_);
    }

    update_empty_state();
}

void PrintSelectPanel::sort_by(PrintSelectSortColumn column) {
    // Convert PrintSelectSortColumn to FileSorter's SortColumn (identical enum values)
    auto sorter_column = static_cast<helix::ui::SortColumn>(static_cast<int>(column));

    // Delegate toggle logic to file_sorter_
    file_sorter_.sort_by(sorter_column);

    // Update local state from file_sorter_ (for UI code that uses these members)
    current_sort_column_ =
        static_cast<PrintSelectSortColumn>(static_cast<int>(file_sorter_.current_column()));
    current_sort_direction_ =
        static_cast<PrintSelectSortDirection>(static_cast<int>(file_sorter_.current_direction()));

    apply_sort();
    update_sort_indicators();

    // Repopulate current view
    if (current_view_mode_ == PrintSelectViewMode::CARD) {
        populate_card_view();
    } else {
        populate_list_view();
    }

    spdlog::debug("[{}] Sorted by column {}, direction {}", get_name(), static_cast<int>(column),
                  static_cast<int>(current_sort_direction_));
}

void PrintSelectPanel::set_return_to_home_on_close() {
    return_to_home_on_close_ = true;
    return_home_activation_count_ = 0;
    spdlog::debug("[{}] Will return to home when detail view closes", get_name());
}

void PrintSelectPanel::set_sort_recent() {
    // Force MODIFIED descending without toggle
    auto sorter_column =
        static_cast<helix::ui::SortColumn>(static_cast<int>(PrintSelectSortColumn::MODIFIED));
    file_sorter_.set_sort(sorter_column, helix::ui::SortDirection::DESCENDING);

    current_sort_column_ = PrintSelectSortColumn::MODIFIED;
    current_sort_direction_ = PrintSelectSortDirection::DESCENDING;

    apply_sort();
    update_sort_indicators();

    if (current_view_mode_ == PrintSelectViewMode::CARD) {
        populate_card_view();
    } else {
        populate_list_view();
    }

    // Show "Recently Printed" context banner
    if (panel_) {
        auto* banner = lv_obj_find_by_name(panel_, "context_banner");
        if (banner) {
            lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    spdlog::info("[{}] Sort set to recent (MODIFIED descending)", get_name());
}

void PrintSelectPanel::hide_context_banner() {
    if (panel_) {
        auto* banner = lv_obj_find_by_name(panel_, "context_banner");
        if (banner && !lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void PrintSelectPanel::refresh_files(bool force) {
    hide_context_banner();
    if (!file_provider_) {
        spdlog::warn("[{}] Cannot refresh files: file provider not initialized", get_name());
        return;
    }

    if (!file_provider_->is_ready()) {
        spdlog::debug("[{}] Cannot refresh files: not connected (API state: {})", get_name(),
                      api_ ? static_cast<int>(api_->get_connection_state()) : -1);
        return;
    }

    const auto acquire = refresh_guard_.try_acquire(force);
    if (acquire == helix::InFlightGuard::AcquireResult::Skipped) {
        spdlog::debug("[{}] refresh_files() skipped: previous request still in-flight", get_name());
        return;
    }
    if (acquire == helix::InFlightGuard::AcquireResult::RecoveredStuck) {
        // The prior response is presumed lost (threshold exceeded); the guard
        // has already re-based its timestamp, so fall through to a fresh request.
        spdlog::warn("[{}] refresh_files(): in-flight flag stuck for {}ms — treating prior "
                     "response as lost and retrying",
                     get_name(), refresh_guard_.stuck_threshold().count());
    }

    spdlog::trace("[{}] refresh_files() called for path='{}', existing_count={}{}", get_name(),
                  current_path_.empty() ? "/" : current_path_, file_list_.size(),
                  force ? " (forced)" : "");

    // Delegate to file provider - callbacks set in setup() will handle the results
    file_provider_->refresh_files(current_path_, file_list_);
}

void PrintSelectPanel::fetch_metadata_range(size_t start, size_t end) {
    if (!api_) {
        return;
    }

    // Clamp range to file list bounds
    start = std::min(start, file_list_.size());
    end = std::min(end, file_list_.size());

    if (start >= end) {
        return;
    }

    auto* self = this;
    size_t fetch_count = 0;

    // Capture current navigation generation to detect directory changes during async ops
    uint32_t captured_gen = nav_generation_.load();

    // Fetch metadata for files in range only (not directories, not already fetched)
    for (size_t i = start; i < end; i++) {
        if (file_list_[i].is_dir)
            continue; // Skip directories

        if (file_list_[i].metadata_fetched)
            continue; // Already fetched or in flight

        // Mark as fetched immediately to prevent duplicate requests
        file_list_[i].metadata_fetched = true;
        fetch_count++;

        const std::string filename = file_list_[i].filename;
        // Build full path for metadata request (e.g., "usb/flowrate_0.gcode")
        const std::string file_path =
            current_path_.empty() ? filename : current_path_ + "/" + filename;

        api_->files().get_file_metadata(
            file_path,
            // Metadata success callback (runs on background thread).
            // No self->member access on bg — everything member-touching is inside tok.defer
            // so the nav-generation check moves into each deferred body. (#L081 Mech-C)
            [self, i, filename, file_path, captured_gen,
             token = self->lifetime_.token()](const FileMetadata& metadata) {
                // Pure local computation — no self access.
                // Empty metadata means file hasn't been scanned yet (e.g. USB symlinks).
                bool metadata_empty = metadata.thumbnails.empty() && metadata.estimated_time == 0;

                if (metadata_empty) {
                    token.defer("PrintSelectPanel::metadata_empty_metascan", [self, i, filename,
                                                                              file_path,
                                                                              captured_gen,
                                                                              token]() {
                        if (self->nav_generation_.load() != captured_gen) {
                            return;
                        }
                        if (!self->api_) {
                            return;
                        }
                        spdlog::debug("[{}] Empty metadata for {}, triggering metascan",
                                      self->get_name(), filename);
                        self->api_->files().metascan_file(
                            file_path,
                            [self, i, filename, captured_gen, token](const FileMetadata& scanned) {
                                token.defer("PrintSelectPanel::metascan_success_process",
                                            [self, i, filename, captured_gen, scanned]() {
                                                if (self->nav_generation_.load() != captured_gen) {
                                                    return;
                                                }
                                                self->process_metadata_result(i, filename, scanned);
                                            });
                            },
                            [self, i, filename, captured_gen, token](const MoonrakerError& error) {
                                token.defer(
                                    "PrintSelectPanel::metascan_error_process",
                                    [self, i, filename, captured_gen, error]() {
                                        if (self->nav_generation_.load() != captured_gen) {
                                            return;
                                        }
                                        spdlog::debug("[{}] Metascan failed for {}: {}, "
                                                      "trying gcode extraction",
                                                      self->get_name(), filename, error.message);
                                        FileMetadata empty_meta;
                                        self->process_metadata_result(i, filename, empty_meta);
                                    });
                            });
                    });
                    return;
                }

                token.defer("PrintSelectPanel::metadata_process",
                            [self, i, filename, captured_gen, metadata]() {
                                if (self->nav_generation_.load() != captured_gen) {
                                    return;
                                }
                                self->process_metadata_result(i, filename, metadata);
                            });
            },
            // Metadata error callback. Same pattern: nav-gen check moves into the defer body.
            [self, i, filename, file_path, captured_gen,
             token = self->lifetime_.token()](const MoonrakerError& error) {
                token.defer("PrintSelectPanel::metadata_error_metascan", [self, i, filename,
                                                                          file_path, captured_gen,
                                                                          token, error]() {
                    if (self->nav_generation_.load() != captured_gen) {
                        return;
                    }
                    spdlog::debug("[{}] Failed to get metadata for {}: {} ({})", self->get_name(),
                                  filename, error.message, error.get_type_string());

                    if (!self->api_) {
                        return;
                    }
                    spdlog::debug("[{}] Triggering metascan for {} after metadata failure",
                                  self->get_name(), filename);
                    self->api_->files().metascan_file(
                        file_path,
                        [self, i, filename, captured_gen, token](const FileMetadata& scanned) {
                            token.defer("PrintSelectPanel::metadata_err_metascan_success",
                                        [self, i, filename, captured_gen, scanned]() {
                                            if (self->nav_generation_.load() != captured_gen) {
                                                return;
                                            }
                                            self->process_metadata_result(i, filename, scanned);
                                        });
                        },
                        [self, i, filename, captured_gen, token](const MoonrakerError& scan_error) {
                            token.defer("PrintSelectPanel::metadata_err_metascan_err",
                                        [self, i, filename, captured_gen, scan_error]() {
                                            if (self->nav_generation_.load() != captured_gen) {
                                                return;
                                            }
                                            spdlog::debug("[{}] Metascan also failed for {}: {}, "
                                                          "trying gcode extraction",
                                                          self->get_name(), filename,
                                                          scan_error.message);
                                            FileMetadata empty_meta;
                                            self->process_metadata_result(i, filename, empty_meta);
                                        });
                        });
                });
            },
            true // silent - don't trigger RPC_ERROR event/toast
        );
    }

    if (fetch_count > 0) {
        spdlog::trace("[{}] fetch_metadata_range({}, {}): started {} metadata requests", get_name(),
                      start, end, fetch_count);
    }
}

/**
 * @brief Process metadata result and update file list
 *
 * Extracted from fetch_metadata_range to allow reuse from metascan fallback.
 * Handles thumbnail fetching, UI updates, and detail view synchronization.
 *
 * @param i Index in file_list_
 * @param filename Filename for validation
 * @param metadata Metadata to process
 */
void PrintSelectPanel::process_metadata_result(size_t i, const std::string& filename,
                                               const FileMetadata& metadata) {
    // Extract all values (this runs on background thread - metadata is const ref)
    int print_time_minutes = static_cast<int>(metadata.estimated_time / 60.0);
    float filament_grams = static_cast<float>(metadata.filament_weight_total);
    std::string filament_type_raw = metadata.filament_type;
    std::string filament_name_raw = metadata.filament_name;

    // Parse per-tool filament types and names from semicolon-separated metadata
    // (e.g., "PLA;PLA;PETG" → ["PLA", "PLA", "PETG"])
    std::vector<std::string> filament_types;
    if (!filament_type_raw.empty()) {
        std::istringstream type_stream(filament_type_raw);
        std::string token;
        while (std::getline(type_stream, token, ';')) {
            filament_types.push_back(token);
        }
    }
    std::vector<std::string> filament_names;
    if (!filament_name_raw.empty()) {
        std::istringstream name_stream(filament_name_raw);
        std::string token;
        while (std::getline(name_stream, token, ';')) {
            filament_names.push_back(token);
        }
    }

    // Scalar fields use only the first tool's value for display
    std::string filament_type = filament_types.empty() ? "" : filament_types.front();
    std::string filament_name = filament_names.empty() ? "" : filament_names.front();

    // Copy filament colors (per-tool hex colors parsed from Moonraker)
    std::vector<std::string> filament_colors = metadata.filament_colors;
    uint32_t layer_count = metadata.layer_count;
    uint64_t gcode_end_byte = metadata.gcode_end_byte;
    double object_height = metadata.object_height;
    double layer_height = metadata.layer_height;
    std::string uuid = metadata.uuid;

    // Smart thumbnail selection: pick smallest that meets display requirements
    // This reduces download size while ensuring adequate resolution
    helix::ThumbnailTarget target = helix::ThumbnailProcessor::get_target_for_display();
    const ThumbnailInfo* best_thumb = metadata.get_best_thumbnail(target.width, target.height);
    std::string thumb_path =
        resolve_thumbnail_path(best_thumb ? best_thumb->relative_path : "", current_path_);
    spdlog::debug("[{}] Metadata thumbnails for {}: count={}, selected='{}' -> '{}'", get_name(),
                  filename, metadata.thumbnails.size(),
                  best_thumb ? best_thumb->relative_path : "(none)", thumb_path);

    // Include predicted pre-print overhead (heating, homing, bed mesh, etc.)
    // in the total time estimate so users see realistic wall-clock time
    int preprint_seconds = helix::PreprintPredictor::predicted_total_from_config();
    int total_minutes =
        print_time_minutes + (preprint_seconds + 30) / 60; // round to nearest minute

    // Format strings on background thread (uses standalone helper functions)
    std::string print_time_str = format_print_time(total_minutes);
    std::string filament_str = format_filament_weight(filament_grams);
    std::string layer_count_str = format_layer_count(layer_count);
    std::string print_height_str = format_print_height(object_height) + " " + lv_tr("tall");

    // Format layer height (e.g., "0.24 mm")
    char layer_height_buf[32];
    if (layer_height > 0.0) {
        helix::format::format_distance_mm(layer_height, 2, layer_height_buf,
                                          sizeof(layer_height_buf));
    } else {
        snprintf(layer_height_buf, sizeof(layer_height_buf), "-");
    }
    std::string layer_height_str = layer_height_buf;

    // Check if thumbnail is a local file (background thread - filesystem OK)
    bool thumb_is_local = !thumb_path.empty() && std::filesystem::exists(thumb_path);

    // CRITICAL: Dispatch file_list_ modifications to main thread to avoid race
    // conditions with populate_card_view/populate_list_view reading file_list_
    struct MetadataUpdate {
        PrintSelectPanel* panel;
        size_t index;
        std::string filename;
        int print_time_minutes;
        float filament_grams;
        std::string filament_type;
        std::string filament_name;
        std::string print_time_str;
        std::string filament_str;
        uint32_t layer_count;
        uint64_t gcode_end_byte;
        std::string layer_count_str;
        double object_height;
        std::string print_height_str;
        double layer_height;
        std::string layer_height_str;
        std::string uuid;
        std::string thumb_path;
        bool thumb_is_local;
        helix::ThumbnailTarget thumb_target;
        std::vector<std::string> filament_types;
        std::vector<std::string> filament_names;
        std::vector<std::string> filament_colors;
    };

    // process_metadata_result already runs on the main thread (every caller is
    // inside a `tok.defer(...)` body — see fetch_metadata_range). The original
    // code wrapped the apply-and-fetch step in queue_update<MetadataUpdate> to
    // marshal back to main from the bg thread that used to run this function.
    // Post-L081 sweep that's redundant: just call the apply lambda directly.
    // The MetadataUpdate struct is preserved as a parameter bag so the
    // ~260-line existing apply body (chains the guarded thumbnail fetch +
    // download_file_partial) stays unchanged.
    {
        auto d_owned = std::make_unique<MetadataUpdate>(MetadataUpdate{this,
                                                                       i,
                                                                       filename,
                                                                       print_time_minutes,
                                                                       filament_grams,
                                                                       filament_type,
                                                                       filament_name,
                                                                       print_time_str,
                                                                       filament_str,
                                                                       layer_count,
                                                                       gcode_end_byte,
                                                                       layer_count_str,
                                                                       object_height,
                                                                       print_height_str,
                                                                       layer_height,
                                                                       layer_height_str,
                                                                       uuid,
                                                                       thumb_path,
                                                                       thumb_is_local,
                                                                       target,
                                                                       std::move(filament_types),
                                                                       std::move(filament_names),
                                                                       std::move(filament_colors)});
        auto apply = [](MetadataUpdate* d) {
            auto* self = d->panel;

            // Bounds check (file_list could change during async operation)
            if (d->index >= self->file_list_.size() ||
                self->file_list_[d->index].filename != d->filename) {
                spdlog::warn("[{}] File list changed during metadata fetch for {}",
                             self->get_name(), d->filename);
                return;
            }

            // Snapshot a lifetime token while the panel is known valid on the main
            // thread. Thumbnail/gcode fetch results below are applied on a LATER
            // UpdateQueue tick; if the panel is torn down before then (e.g. user
            // navigates away mid-scroll), the deferred apply must no-op instead of
            // dereferencing the freed panel (use-after-free → glibc heap abort).
            auto panel_tok = self->lifetime_.token();

            // Same hazard, for the pieces the bg-thread callbacks below need.
            // get_name() is virtual, so calling it on a torn-down panel dispatches
            // through a freed vtable pointer — and a bare `ctx.is_valid()` ahead of
            // it does not close the window, it only narrows it (L081 Mechanism C).
            // Both of these are stable for the panel's life and the name is a
            // compile-time literal, so snapshot them here, on the main thread,
            // and let the callbacks format and fetch without touching `self`.
            const char* panel_name = self->get_name();
            auto* panel_api = self->api_;

            // Update metadata fields (now on main thread - safe!)
            self->file_list_[d->index].print_time_minutes = d->print_time_minutes;
            self->file_list_[d->index].filament_grams = d->filament_grams;
            self->file_list_[d->index].filament_type = d->filament_type;
            self->file_list_[d->index].filament_name = d->filament_name;
            self->file_list_[d->index].print_time_str = d->print_time_str;
            self->file_list_[d->index].filament_str = d->filament_str;
            self->file_list_[d->index].layer_count = d->layer_count;
            self->file_list_[d->index].gcode_end_byte = d->gcode_end_byte;
            self->file_list_[d->index].layer_count_str = d->layer_count_str;
            self->file_list_[d->index].object_height = d->object_height;
            self->file_list_[d->index].print_height_str = d->print_height_str;
            self->file_list_[d->index].layer_height = d->layer_height;
            self->file_list_[d->index].layer_height_str = d->layer_height_str;
            self->file_list_[d->index].uuid = d->uuid;
            self->file_list_[d->index].filament_types = std::move(d->filament_types);
            self->file_list_[d->index].filament_names = std::move(d->filament_names);
            self->file_list_[d->index].filament_colors = std::move(d->filament_colors);

            spdlog::trace(
                "[{}] Updated metadata for {}: {}min, {}g, {} layers, {} types, {} colors",
                self->get_name(), d->filename, d->print_time_minutes, d->filament_grams,
                d->layer_count, self->file_list_[d->index].filament_types.size(),
                self->file_list_[d->index].filament_colors.size());

            // Handle thumbnail with pre-scaling optimization
            if (!d->thumb_path.empty() && self->api_) {
                // Store original URL for detail view PNG lookup
                self->file_list_[d->index].original_thumbnail_url = d->thumb_path;

                if (d->thumb_is_local) {
                    // Local file (mock mode). Feed it through the same prescale pipeline
                    // the remote and USB paths use rather than pointing the widget at the
                    // raw PNG: the card sizes its image widget to the .bin target, so a
                    // native-size 300x300 source gets drawn at 300x300 and cropped to the
                    // widget box, showing a fragment of the model (#1208). Routing it
                    // through the prescaler also keeps --test faithful to a real printer.
                    // capture(), not create(): nav_generation_ is bumped by
                    // panel navigation, never by a thumbnail load. Incrementing
                    // it here would cancel every other in-flight card fetch.
                    ThumbnailLoadContext ctx = ThumbnailLoadContext::capture(
                        self->thumbnail_alive_, &self->nav_generation_);

                    size_t file_idx = d->index;
                    std::string filename_copy = d->filename;
                    std::string cache_key = d->thumb_path + "_local";

                    // A refresh re-runs this for files that already have their .bin, and
                    // re-reading plus re-staging every PNG to rediscover that is wasted IO.
                    const std::string& existing = self->file_list_[d->index].thumbnail_path;
                    bool already_prescaled = existing.size() > 4 &&
                                             existing.compare(existing.size() - 4, 4, ".bin") == 0;

                    std::vector<uint8_t> png_data =
                        already_prescaled ? std::vector<uint8_t>{} : read_file_bytes(d->thumb_path);
                    std::string cached_png =
                        png_data.empty() ? std::string()
                                         : get_thumbnail_cache().save_raw_png(cache_key, png_data);

                    if (cached_png.empty()) {
                        // Could not stage it for prescaling — the raw PNG still beats no
                        // thumbnail at all, even though the card will crop it.
                        self->file_list_[d->index].thumbnail_path = "A:" + d->thumb_path;
                        spdlog::debug("[{}] Using local thumbnail unscaled for {}: {}",
                                      self->get_name(), d->filename,
                                      self->file_list_[d->index].thumbnail_path);
                    } else {
                        ThumbnailRequest req;
                        req.key = cache_key;
                        req.target = helix::ThumbnailProcessor::get_target_for_display();
                        req.api = self->api_;

                        get_thumbnail_cache().fetch(
                            req, ctx,
                            [self, panel_tok, file_idx, filename_copy](const std::string& optimized,
                                                                       bool /*degraded*/) {
                                struct LocalThumbUpdate {
                                    PrintSelectPanel* panel;
                                    helix::LifetimeToken token;
                                    size_t index;
                                    std::string filename;
                                    std::string lvgl_path;
                                };
                                helix::ui::queue_update<LocalThumbUpdate>(
                                    std::make_unique<LocalThumbUpdate>(LocalThumbUpdate{
                                        self, panel_tok, file_idx, filename_copy, optimized}),
                                    [](LocalThumbUpdate* t) {
                                        if (t->token.expired()) {
                                            return;
                                        }
                                        if (t->index < t->panel->file_list_.size() &&
                                            t->panel->file_list_[t->index].filename ==
                                                t->filename) {
                                            t->panel->file_list_[t->index].thumbnail_path =
                                                t->lvgl_path;
                                            t->panel->schedule_view_refresh();
                                        }
                                    });
                            },
                            [panel_name, filename_copy](const std::string& error) {
                                // HttpExecutor worker: no `self`, so nothing to
                                // outlive us. panel_name is a literal snapshotted
                                // on the main thread.
                                spdlog::warn("[{}] Failed to prescale local thumbnail for {}: {}",
                                             panel_name, filename_copy, error);
                            });
                    }
                } else {
#if !defined(HELIX_PLATFORM_ESP32)
                    // Remote path - use semantic API for card view thumbnails
                    spdlog::debug("[{}] Fetching card thumbnail for {}: {}", self->get_name(),
                                  d->filename, d->thumb_path);

                    size_t file_idx = d->index;
                    std::string filename_copy = d->filename;
                    time_t modified_ts = self->file_list_[d->index].modified_timestamp;

                    // Create context with alive flag and nav generation for safety.
                    // capture(), not create(): nav_generation_ tracks panel
                    // navigation, and one card's fetch must not supersede the
                    // rest of the visible range.
                    ThumbnailLoadContext ctx = ThumbnailLoadContext::capture(
                        self->thumbnail_alive_, &self->nav_generation_);

                    ThumbnailRequest req;
                    req.key = d->thumb_path;
                    req.target = helix::ThumbnailProcessor::get_target_for_display();
                    req.source_modified = modified_ts;
                    req.api = self->api_;

                    get_thumbnail_cache().fetch(
                        req, ctx,
                        // Success callback - receives pre-scaled .bin path
                        [self, panel_tok, file_idx, filename_copy](const std::string& lvgl_path,
                                                                   bool /*degraded*/) {
                            struct ThumbUpdate {
                                PrintSelectPanel* panel;
                                helix::LifetimeToken token;
                                size_t index;
                                std::string filename;
                                std::string lvgl_path;
                            };
                            helix::ui::queue_update<ThumbUpdate>(
                                std::make_unique<ThumbUpdate>(ThumbUpdate{
                                    self, panel_tok, file_idx, filename_copy, lvgl_path}),
                                [](ThumbUpdate* t) {
                                    // Panel may have been destroyed between this fetch
                                    // completing and the queued apply running.
                                    if (t->token.expired()) {
                                        return;
                                    }
                                    if (t->index < t->panel->file_list_.size() &&
                                        t->panel->file_list_[t->index].filename == t->filename) {
                                        t->panel->file_list_[t->index].thumbnail_path =
                                            t->lvgl_path;
                                        spdlog::debug(
                                            "[{}] Card thumbnail for {}: {}", t->panel->get_name(),
                                            t->filename,
                                            t->panel->file_list_[t->index].thumbnail_path);
                                        t->panel->schedule_view_refresh();

                                        // Update detail view thumbnail if this file is selected
                                        // (handles metascan flow where thumbnail arrives after
                                        // metadata)
                                        if (strcmp(t->panel->selected_filename_buffer_,
                                                   t->filename.c_str()) == 0) {
                                            const auto& file = t->panel->file_list_[t->index];
                                            std::string filament_display =
                                                file.filament_name.empty() ? file.filament_type
                                                                           : file.filament_name;
                                            t->panel->set_selected_file(
                                                file.filename.c_str(), file.thumbnail_path.c_str(),
                                                file.original_thumbnail_url.c_str(),
                                                file.print_time_str.c_str(),
                                                file.filament_str.c_str(),
                                                file.layer_count_str.c_str(),
                                                file.print_height_str.c_str(),
                                                file.modified_timestamp,
                                                file.layer_height_str.c_str(),
                                                filament_display.c_str());
                                            spdlog::debug(
                                                "[{}] Updated detail view thumbnail for {}",
                                                t->panel->get_name(), t->filename);
                                        }
                                    }
                                });
                        },
                        // Error callback
                        [self, filename_copy](const std::string& error) {
                            spdlog::warn("[{}] Failed to fetch thumbnail for {}: {}",
                                         self->get_name(), filename_copy, error);
                        });
#else
                    // ESP32 (Task 11 R2): no disk thumbnail cache on this platform
                    // (Task 10 R6), so bypass ThumbnailCache/ThumbnailProcessor
                    // entirely and fetch the PNG bytes directly via the HTTP lane,
                    // decoding into a PSRAM-backed lv_image_dsc_t instead of a
                    // cache file (see esp_psram_thumbnail.h).
                    //
                    // MANDATORY threading: EspHttpLane invokes on_success/on_error
                    // directly on its own worker thread with no built-in
                    // marshaling. This callback therefore does only local
                    // byte-copy/PSRAM work on the worker thread and defers every
                    // `self`/file_list_ touch via panel_tok.defer() — `self` is
                    // captured here only to pass into that deferred lambda, never
                    // dereferenced on this thread.
                    spdlog::debug("[{}] Fetching PSRAM thumbnail for {}: {}", self->get_name(),
                                  d->filename, d->thumb_path);

                    size_t file_idx = d->index;
                    std::string filename_copy = d->filename;
                    constexpr size_t ESP32_THUMBNAIL_MAX_BYTES = 512 * 1024;

                    self->api_->transfers().download_file_partial(
                        "gcodes", d->thumb_path, ESP32_THUMBNAIL_MAX_BYTES,
                        // Success callback — runs on the EspHttpLane worker thread.
                        [self, panel_tok, file_idx, filename_copy](const std::string& png_bytes) {
                            auto thumb = helix::ui::EspPsramThumbnail::create(png_bytes);
                            if (!thumb) {
                                spdlog::warn("[PrintSelectPanel] PSRAM alloc failed for "
                                             "thumbnail: {}",
                                             filename_copy);
                                return;
                            }
                            panel_tok.defer(
                                "PrintSelectPanel::on_psram_thumbnail_fetched",
                                [self, file_idx, filename_copy,
                                 thumb = std::move(thumb)]() mutable {
                                    if (file_idx < self->file_list_.size() &&
                                        self->file_list_[file_idx].filename == filename_copy) {
                                        self->file_list_[file_idx].esp_thumbnail = std::move(thumb);
                                        self->schedule_view_refresh();
                                    }
                                });
                        },
                        // Error callback — bg thread, log only, no member access.
                        [filename_copy](const MoonrakerError& error) {
                            spdlog::debug(
                                "[PrintSelectPanel] PSRAM thumbnail fetch failed for {}: {}",
                                filename_copy, error.message);
                        });
#endif
                }
            } else if (self->api_) {
                // No thumbnail from metadata - try extracting from gcode file directly
                // This handles USB files where Moonraker can't write .thumbs directory
                // because the USB mount is read-only.

                // Skip if we already have a thumbnail for this file (avoids re-extracting
                // 100KB of gcode on every scroll cycle — major perf issue on AD5M)
                if (!self->file_list_[d->index].thumbnail_path.empty()) {
                    spdlog::debug("[{}] Skipping gcode extraction for {} (already have thumbnail)",
                                  self->get_name(), d->filename);
                } else {
                    size_t file_idx = d->index;
                    std::string filename_copy = d->filename;

                    // Build the full gcode path for download
                    std::string gcode_path = filename_copy;
                    if (!self->current_path_.empty()) {
                        gcode_path = self->current_path_ + "/" + filename_copy;
                    }

                    spdlog::debug("[{}] No thumbnail in metadata for {}, extracting from gcode",
                                  self->get_name(), gcode_path);

                    // Download first 100KB of gcode (thumbnails are always in header)
                    constexpr size_t THUMBNAIL_HEADER_SIZE = 100 * 1024;

                    // Create context for prescale callback safety. capture(),
                    // not create(): nav_generation_ belongs to panel
                    // navigation, so a gcode extraction must observe it, not
                    // advance it.
                    ThumbnailLoadContext ctx = ThumbnailLoadContext::capture(
                        self->thumbnail_alive_, &self->nav_generation_);

                    self->api_->transfers().download_file_partial(
                        "gcodes", gcode_path, THUMBNAIL_HEADER_SIZE,
                        // Success callback - extract thumbnails from gcode content
                        [self, panel_tok, panel_name, panel_api, file_idx, filename_copy,
                         gcode_path, ctx](const std::string& content) {
                            // HttpExecutor worker. The parse and the cache write below
                            // are deliberately left here — they are the expensive part
                            // and touch no panel state. What must NOT happen on this
                            // thread is a `self` dereference, so everything the body
                            // needs from the panel (name, api, lifetime) was captured
                            // by value on the main thread instead.
                            auto thumbnails =
                                helix::gcode::extract_thumbnails_from_content(content);

                            if (thumbnails.empty()) {
                                spdlog::debug("[{}] No embedded thumbnails in {}", panel_name,
                                              gcode_path);
                                return;
                            }

                            // Use the largest thumbnail (already sorted largest-first)
                            const auto& best = thumbnails[0];
                            spdlog::debug("[{}] Extracted {}x{} thumbnail ({} bytes) from {}",
                                          panel_name, best.width, best.height, best.png_data.size(),
                                          gcode_path);

                            // Save to cache using the gcode path as identifier
                            std::string cache_key = gcode_path + "_extracted";
                            std::string lvgl_path =
                                get_thumbnail_cache().save_raw_png(cache_key, best.png_data);

                            if (lvgl_path.empty()) {
                                spdlog::warn("[{}] Failed to cache extracted thumbnail for {}",
                                             panel_name, gcode_path);
                                return;
                            }

                            // Feed through prescale pipeline for .bin generation
                            // (avoids runtime 300x300→160x160 scaling on every frame)
                            ThumbnailRequest req;
                            req.key = cache_key;
                            req.target = helix::ThumbnailProcessor::get_target_for_display();
                            req.api = panel_api;

                            get_thumbnail_cache().fetch(
                                req, ctx,
                                [self, panel_tok, file_idx,
                                 filename_copy](const std::string& optimized, bool /*degraded*/) {
                                    // Everything that touches the panel happens in the
                                    // queued apply below, which re-checks the token on
                                    // the main thread.
                                    struct ExtractedThumbUpdate {
                                        PrintSelectPanel* panel;
                                        helix::LifetimeToken token;
                                        size_t index;
                                        std::string filename;
                                        std::string lvgl_path;
                                    };
                                    helix::ui::queue_update<ExtractedThumbUpdate>(
                                        std::make_unique<ExtractedThumbUpdate>(ExtractedThumbUpdate{
                                            self, panel_tok, file_idx, filename_copy, optimized}),
                                        [](ExtractedThumbUpdate* t) {
                                            // Panel may have been destroyed between the
                                            // fetch completing and this queued apply.
                                            if (t->token.expired()) {
                                                return;
                                            }
                                            if (t->index < t->panel->file_list_.size() &&
                                                t->panel->file_list_[t->index].filename ==
                                                    t->filename) {
                                                t->panel->file_list_[t->index].thumbnail_path =
                                                    t->lvgl_path;
                                                spdlog::info(
                                                    "[{}] Extracted thumbnail for {}: {}",
                                                    t->panel->get_name(), t->filename,
                                                    t->panel->file_list_[t->index].thumbnail_path);
                                                t->panel->schedule_view_refresh();
                                            }
                                        });
                                },
                                [panel_name, filename_copy](const std::string& error) {
                                    spdlog::warn(
                                        "[{}] Failed to prescale extracted thumbnail for {}: {}",
                                        panel_name, filename_copy, error);
                                });
                        },
                        // Error callback - silent fail (file might be too small or inaccessible)
                        [panel_name, gcode_path](const MoonrakerError& error) {
                            spdlog::debug("[{}] Failed to download gcode header for {}: {}",
                                          panel_name, gcode_path, error.message);
                        });
                }
            }

            // Schedule debounced view refresh
            self->schedule_view_refresh();

            // Update detail view if this file is currently selected
            if (strcmp(self->selected_filename_buffer_, d->filename.c_str()) == 0) {
                spdlog::debug("[{}] Updating detail view for selected file: {}", self->get_name(),
                              d->filename);
                // Use filament_name if available, otherwise filament_type
                const std::string& filament_display =
                    !d->filament_name.empty() ? d->filament_name : d->filament_type;
                self->set_selected_file(
                    d->filename.c_str(), self->file_list_[d->index].thumbnail_path.c_str(),
                    self->file_list_[d->index].original_thumbnail_url.c_str(),
                    d->print_time_str.c_str(), d->filament_str.c_str(), d->layer_count_str.c_str(),
                    d->print_height_str.c_str(), self->file_list_[d->index].modified_timestamp,
                    d->layer_height_str.c_str(), filament_display.c_str());
            }
        };
        apply(d_owned.get());
    }
}

void PrintSelectPanel::set_api(IMoonrakerAPI* api) {
    api_ = api;

    // Update file provider's API reference (it was created with nullptr in setup())
    if (file_provider_) {
        file_provider_->set_api(api_);
    }

    // Update detail view's dependencies (it was created with nullptr in setup())
    if (detail_view_) {
        detail_view_->set_dependencies(api_, &printer_state_);
    }

    // Update print controller's API reference
    if (print_controller_) {
        print_controller_->set_api(api_);
    }

    // Note: Don't auto-refresh here - WebSocket may not be connected yet.
    // refresh_files() has a connection check that will silently return if not connected.
    // Files will be loaded lazily via on_activate() when user navigates to this panel.
    // helix_print plugin check happens in connection observer (after connection established)
    if (api_ && panel_initialized_) {
        spdlog::debug("[{}] API set, files will load on first view", get_name());
        refresh_files(); // Will early-return if not connected
    }

    // Register for file list change notifications from Moonraker
    // This handles external uploads (OrcaSlicer, Mainsail, etc.) and file operations
    if (api_) {
        filelist_handler_name_ =
            "print_select_filelist_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        auto* self = this;
        api_->register_method_callback(
            "notify_filelist_changed", filelist_handler_name_, [self](const json& msg) {
                // Action + path only, never the raw payload. The full dump ran
                // ~344 bytes a line, and an AFC printer fires this constantly
                // (AFC rewrites AFC/AFC.var.unit on every SET_* command), so on
                // one debug bundle it burned 97 KB of a ring that has to hold
                // the whole session. Nothing downstream reads the other fields.
                std::string action = "?";
                std::string root;
                std::string path;
                if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty()) {
                    const json& p = msg["params"][0];
                    action = p.value("action", "?");
                    if (p.contains("item") && p["item"].is_object()) {
                        const json& item = p["item"];
                        root = item.value("root", "");
                        path = root + ":" + item.value("path", "");
                    }
                }

                // Roots other than "gcodes" cannot change this list, and the
                // config root churns constantly on an AFC printer. Log those at
                // debug so the ring still shows they arrived without one line
                // per 10 s of print time.
                if (!filelist_change_affects_gcodes(root)) {
                    spdlog::debug("[{}] notify_filelist_changed: {} {} (other root, ignored)",
                                  self->get_name(), action, path);
                    return;
                }
                spdlog::info("[{}] notify_filelist_changed: {} {}", self->get_name(), action, path);

                // Check if we're on the printer source (not USB)
                bool is_usb_active = self->usb_source_ && self->usb_source_->is_usb_active();
                if (!is_usb_active) {
                    // If detail view is open, just mark that files changed - will refresh on return
                    if (self->detail_view_open_) {
                        self->files_changed_while_detail_open_ = true;
                        spdlog::debug(
                            "[{}] Files changed while detail view open, deferring refresh",
                            self->get_name());
                        return;
                    }

                    // Use async call to refresh on main thread
                    helix::ui::async_call(
                        [](void* user_data) {
                            auto* panel = static_cast<PrintSelectPanel*>(user_data);
                            // Guard against async callback firing after display destruction
                            if (!panel || !panel->panel_ || !lv_obj_is_valid(panel->panel_)) {
                                return;
                            }
                            spdlog::debug("[{}] Refreshing file list due to external change",
                                          panel->get_name());
                            panel->refresh_files();
                        },
                        self);
                }
            });
        spdlog::debug("[{}] Registered for notify_filelist_changed notifications", get_name());
    }

    // Create periodic polling timer as fallback for missed WebSocket notifications (issue #33)
    // Some hardware (e.g. CB1) may not reliably deliver notify_filelist_changed.
    // Timer starts paused — on_activate()/on_deactivate() control its lifecycle.
    if (api_ && !file_poll_timer_) {
        auto* self = this;
        file_poll_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* panel = static_cast<PrintSelectPanel*>(lv_timer_get_user_data(timer));
                if (!panel->panel_ || !lv_obj_is_valid(panel->panel_)) {
                    return;
                }
                bool is_usb_active = panel->usb_source_ && panel->usb_source_->is_usb_active();
                if (!is_usb_active) {
                    spdlog::trace("[{}] Polling fallback: refreshing file list (current_count={})",
                                  panel->get_name(), panel->file_list_.size());
                    panel->refresh_files();
                }
            },
            FILE_POLL_INTERVAL_MS, self);
        lv_timer_pause(file_poll_timer_);
        spdlog::debug("[{}] Created file list polling timer ({}ms, paused)", get_name(),
                      FILE_POLL_INTERVAL_MS);
    }
}

void PrintSelectPanel::check_moonraker_usb_symlink() {
    if (!api_ || !usb_source_) {
        return;
    }

    spdlog::trace("[{}] Checking if Moonraker has USB symlink access...", get_name());

    // Query Moonraker for files in the "usb" directory
    // If it exists and has files, Klipper's mod has created a symlink
    auto* self = this;
    api_->files().list_files(
        "gcodes", "usb", false,
        [self](const std::vector<FileInfo>& files) {
            // Validate that returned files actually live under usb/ — Moonraker may
            // return the root gcodes listing instead of 404 when usb/ doesn't exist (#610)
            bool has_usb_files = false;
            for (const auto& f : files) {
                if (f.path.find("usb/") == 0) {
                    has_usb_files = true;
                    break;
                }
            }

            if (has_usb_files) {
                spdlog::info("[{}] Moonraker has USB symlink access ({} files) - hiding USB tab",
                             self->get_name(), files.size());
                if (self->usb_source_) {
                    self->usb_source_->set_moonraker_has_usb_access(true);
                }
            } else {
                spdlog::debug("[{}] Moonraker returned {} files but none under usb/ - no symlink",
                              self->get_name(), files.size());
            }
        },
        [self](const MoonrakerError& error) {
            // 404 or error means no symlink - USB tab should be available
            spdlog::debug("[{}] No Moonraker USB symlink detected ({})", self->get_name(),
                          error.message);
            // usb_source_ will show USB tab when drive is inserted
        });
}

void PrintSelectPanel::on_activate() {
    // "Print Last" flow: suppress panel flash while detail view is pending/open
    if (return_to_home_on_close_) {
        bool detail_open = detail_view_ && detail_view_->is_visible();
        bool pending = !pending_file_selection_.empty();

        if ((detail_open || pending) && ++return_home_activation_count_ <= 3) {
            // Hide panel content to prevent flash while detail view is pending/open
            if (panel_) {
                lv_obj_set_style_opa(panel_, LV_OPA_0, 0);
            }
            spdlog::debug(
                "[{}] Print Last flow: hiding panel (detail_open={}, pending={}, count={})",
                get_name(), detail_open, pending, return_home_activation_count_);
        } else {
            // Detail view closed OR safety timeout — restore opacity and go home
            if (return_home_activation_count_ > 3) {
                spdlog::warn("[{}] Print Last flow: safety timeout after {} activations",
                             get_name(), return_home_activation_count_);
                pending_file_selection_.clear();
            }
            if (panel_) {
                lv_obj_set_style_opa(panel_, LV_OPA_100, 0);
            }
            return_to_home_on_close_ = false;
            return_home_activation_count_ = 0;
            spdlog::info("[{}] Returning to home panel (Print Last flow)", get_name());
            NavigationManager::instance().set_active(PanelId::Home);
            return;
        }
    }

    // On first activation: skip refresh if files already loaded (connection observer did it)
    // On subsequent activations: refresh to pick up external changes
    bool is_usb_active = usb_source_ && usb_source_->is_usb_active();

    spdlog::debug(
        "[{}] on_activate called (first_activation={}, file_count={}, usb_active={}, api={}, "
        "was_deactivated={}, files_changed_while_detail={})",
        get_name(), first_activation_, file_list_.size(), is_usb_active, (api_ != nullptr),
        was_deactivated_, files_changed_while_detail_open_);

    // ALWAYS resume polling while panel is visible (must be before early returns)
    if (file_poll_timer_) {
        lv_timer_resume(file_poll_timer_);
        lv_timer_reset(file_poll_timer_); // Reset so first poll is a full interval from now
        spdlog::trace("[{}] File list polling resumed", get_name());
    }

    // Only skip refresh when returning from detail view within the same panel
    // and no files changed while it was open. If the panel was fully deactivated
    // (user navigated to another panel), always refresh to pick up external uploads.
    if (!first_activation_ && !file_list_.empty() && !was_deactivated_ &&
        !files_changed_while_detail_open_) {
        spdlog::debug("[{}] Returning from detail view, no file changes - skipping refresh",
                      get_name());
        files_changed_while_detail_open_ = false;
        return;
    }

    // Reset flags after checking
    files_changed_while_detail_open_ = false;
    was_deactivated_ = false;

    first_activation_ = false;

    if (!is_usb_active && api_) {
        // Printer (Moonraker) source — always refresh to pick up external uploads
        spdlog::info("[{}] Panel activated, refreshing file list", get_name());
        // Give any files with missing thumbnails one retry this visit. Handles the
        // case where Moonraker's metadata extraction failed transiently on upload
        // (JSON-RPC -32601) and later recovered — without this, metadata_fetched
        // stays true forever and the card shows the placeholder permanently.
        retry_missing_thumbnails_on_refresh_ = true;
        refresh_files();
    } else if (is_usb_active) {
        // USB source
        spdlog::info("[{}] Panel activated, refreshing USB file list", get_name());
        if (usb_source_) {
            usb_source_->refresh_files();
        }
    }
}

void PrintSelectPanel::on_deactivate() {
    // Restore opacity if we were in "Print Last" pass-through mode
    if (return_to_home_on_close_) {
        if (panel_) {
            lv_obj_set_style_opa(panel_, LV_OPA_100, 0);
        }
        return_to_home_on_close_ = false;
        return_home_activation_count_ = 0;
    }

    // A navbar switch already closes the overlay (NavigationManager tears it down
    // before calling this), so is_visible() is false and clearing the flag is enough.
    // A rebuild() calls on_deactivate() directly with no stack teardown, so a still-open
    // overlay needs an explicit hide_detail_view()/go_back() here; gating on is_visible()
    // keeps that from popping the wrong panel_stack_ entry in the navbar case.
    if (detail_view_open_) {
        if (detail_view_ && detail_view_->is_visible()) {
            hide_detail_view();
            spdlog::debug("[{}] Closed still-open detail view on deactivate", get_name());
        } else {
            detail_view_open_ = false;
            spdlog::debug("[{}] Cleared detail_view_open_ on deactivate", get_name());
        }
    }

    // Mark that the panel was fully deactivated so on_activate() knows to refresh
    was_deactivated_ = true;

    // Pause file list polling while panel is hidden — no point polling when not visible
    if (file_poll_timer_) {
        lv_timer_pause(file_poll_timer_);
        spdlog::trace("[{}] File list polling paused", get_name());
    }
}

void PrintSelectPanel::navigate_to_directory(const std::string& dirname) {
    hide_context_banner();

    // Increment generation counter to invalidate in-flight metadata callbacks
    uint32_t gen = ++nav_generation_;
    spdlog::debug("[{}] Navigation generation incremented to {} (entering {})", get_name(), gen,
                  dirname);

    path_navigator_.navigate_to(dirname);
    current_path_ = path_navigator_.current_path();

    spdlog::info("[{}] Navigating to directory: {}", get_name(), current_path_);
    refresh_files();
}

void PrintSelectPanel::navigate_up() {
    if (path_navigator_.is_at_root()) {
        spdlog::debug("[{}] Already at root, cannot navigate up", get_name());
        return;
    }

    // Increment generation counter to invalidate in-flight metadata callbacks
    uint32_t gen = ++nav_generation_;
    spdlog::debug("[{}] Navigation generation incremented to {} (going up)", get_name(), gen);

    path_navigator_.navigate_up();
    current_path_ = path_navigator_.current_path();

    spdlog::info("[{}] Navigating up to: {}", get_name(),
                 current_path_.empty() ? "/" : current_path_);
    refresh_files();
}

void PrintSelectPanel::set_selected_file(const char* filename, const char* thumbnail_src,
                                         const char* original_url, const char* print_time,
                                         const char* filament_weight, const char* layer_count,
                                         const char* print_height, time_t modified_timestamp,
                                         const char* layer_height, const char* filament_type) {
    lv_subject_copy_string(&selected_filename_subject_, filename);

    // Display filename strips .gcode extension for cleaner UI
    std::string display_name = strip_gcode_extension(filename);
    lv_subject_copy_string(&selected_display_filename_subject_, display_name.c_str());

    // Card thumbnail uses POINTER subject - copy to buffer then update pointer
    // This is the pre-scaled .bin for fast card rendering
    strncpy(selected_thumbnail_buffer_, thumbnail_src, sizeof(selected_thumbnail_buffer_) - 1);
    selected_thumbnail_buffer_[sizeof(selected_thumbnail_buffer_) - 1] = '\0';
    lv_subject_set_pointer(&selected_thumbnail_subject_, selected_thumbnail_buffer_);

    // Detail view thumbnail source selection.
    //
    // The pre-scaled card .bin (thumbnail_src) is the AUTHORITATIVE source: it is
    // the exact image the file-browser card renders successfully, so if it exists
    // the detail view is guaranteed to show something. The full-resolution PNG is
    // only an upscaling-quality ENHANCEMENT, and it is fragile: it may have been
    // evicted while the .bin survives, or may fail to decode at the large detail
    // size on memory-constrained 2D-only devices (Snapmaker U1, AD5M). On those
    // platforms the detail preview is the ONLY render (no 3D viewer to mask a
    // failed thumbnail), so a missing/undecodable PNG showed as an all-black
    // preview even though the card .bin was right there.
    //
    // Resolution order: (1) proven card .bin if present, (2) cached PNG when no
    // .bin exists yet, (3) nullptr sentinel handled by the has_real block below.
    const bool have_bin = thumbnail_src && thumbnail_src[0] != '\0' &&
                          !helix::ui::PrintSelectCardView::is_placeholder_thumbnail(thumbnail_src);
    std::string detail_src;
    if (have_bin) {
        detail_src = thumbnail_src;
        spdlog::debug("[{}] Using pre-scaled .bin for detail view: {}", get_name(), detail_src);
    } else if (original_url && original_url[0] != '\0') {
        // No card .bin yet — fall back to the cached full-res PNG so the detail
        // view still shows the model. FullPng is load-bearing here: a pre-scaled
        // lookup would answer "not cached" in exactly the case this branch
        // exists for. Pass modification timestamp to invalidate stale entries.
        ThumbnailRequest req;
        req.key = original_url;
        req.source_modified = modified_timestamp;
        req.format = ThumbnailRequest::ThumbnailFormat::FullPng;
        detail_src = get_thumbnail_cache().get_if_cached(req);
        if (!detail_src.empty()) {
            spdlog::debug("[{}] No .bin yet — using cached PNG for detail view: {}", get_name(),
                          detail_src);
        }
    }
    strncpy(selected_detail_thumbnail_buffer_, detail_src.c_str(),
            sizeof(selected_detail_thumbnail_buffer_) - 1);
    selected_detail_thumbnail_buffer_[sizeof(selected_detail_thumbnail_buffer_) - 1] = '\0';
    // Publish nullptr for the no-thumbnail sentinel: an empty buffer is misread by
    // LVGL as a VARIABLE image source and warns/fails to decode (#990). The
    // has_real block below handles the placeholder-icon/gradient UI toggle.
    lv_subject_set_pointer(&selected_detail_thumbnail_subject_,
                           thumbnail_subject_value(selected_detail_thumbnail_buffer_));

    // Toggle thumbnail image, no-thumbnail placeholder icon, and gradient background in detail view
    if (detail_view_ && detail_view_->get_widget()) {
        lv_obj_t* thumb_img = lv_obj_find_by_name(detail_view_->get_widget(), "detail_thumbnail");
        lv_obj_t* no_thumb =
            lv_obj_find_by_name(detail_view_->get_widget(), "detail_no_thumbnail_icon");
        lv_obj_t* gradient = lv_obj_find_by_name(detail_view_->get_widget(), "gradient_bg");
        bool has_real = thumbnail_src && thumbnail_src[0] != '\0' &&
                        !helix::ui::PrintSelectCardView::is_placeholder_thumbnail(thumbnail_src);
        if (has_real) {
            if (thumb_img)
                lv_obj_remove_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
            if (no_thumb)
                lv_obj_add_flag(no_thumb, LV_OBJ_FLAG_HIDDEN);
            if (gradient)
                lv_obj_set_style_image_opa(gradient, LV_OPA_COVER, 0);
        } else {
            // Hide the lv_image so nothing renders behind the cube icon,
            // and clear the buffer so LVGL doesn't hold a stale src reference.
            if (thumb_img)
                lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
            if (no_thumb)
                lv_obj_remove_flag(no_thumb, LV_OBJ_FLAG_HIDDEN);
            // Notify with nullptr, not the buffer: lv_image_src_get_type
            // classifies a buffer whose byte[0]<0x20 as LV_IMAGE_SRC_VARIABLE,
            // and decoders then read the leftover bytes past the cleared first
            // byte as if they were lv_image_dsc_t fields (SEGV in is_jpg).
            selected_detail_thumbnail_buffer_[0] = '\0';
            lv_subject_set_pointer(&selected_detail_thumbnail_subject_, nullptr);
            if (gradient)
                lv_obj_set_style_image_opa(gradient, LV_OPA_TRANSP, 0);
        }
    }

    lv_subject_copy_string(&selected_print_time_subject_, print_time);
    lv_subject_copy_string(&selected_filament_weight_subject_, filament_weight);
    lv_subject_copy_string(&selected_layer_count_subject_, layer_count);
    lv_subject_copy_string(&selected_print_height_subject_, print_height);
    lv_subject_copy_string(&selected_layer_height_subject_, layer_height ? layer_height : "");
    lv_subject_copy_string(&selected_filament_type_subject_, filament_type ? filament_type : "");

    spdlog::info("[{}] Selected file: {}", get_name(), filename);
}

void PrintSelectPanel::show_detail_view() {
    // Track that detail view is open (for smart refresh skip on return)
    detail_view_open_ = true;
    files_changed_while_detail_open_ = false;

    if (detail_view_) {
        std::string filename(selected_filename_buffer_);
        detail_view_->show(filename, current_path_, selected_filament_type_,
                           selected_filament_colors_, selected_filament_materials_,
                           selected_file_size_bytes_, selected_modified_timestamp_,
                           selected_gcode_end_byte_);
        // Update history status display in detail view
        detail_view_->update_history_status(selected_history_status_, selected_success_count_);
    }
}

void PrintSelectPanel::forward_sliced_colors_toggle(bool checked) {
    if (detail_view_) {
        detail_view_->set_prefer_sliced_colors(checked);
    }
}

void PrintSelectPanel::hide_detail_view() {
    // Clear detail view open flag (on_activate will check files_changed_while_detail_open_)
    detail_view_open_ = false;

    if (detail_view_) {
        // hide() pops the overlay via go_back(), and its only guard is
        // overlay_root_ existing — which setup() creates eagerly. Calling it
        // when the overlay was never pushed (a long-press delete never opens
        // the detail view) pops panel_stack_.back() instead: for a main-panel
        // stack that is this panel itself, and go_back()'s empty-stack
        // fallback lands on Home. is_visible() is driven by NavigationManager
        // push/pop, the same gate on_deactivate() uses for its rebuild-path
        // close.
        if (detail_view_->is_visible()) {
            detail_view_->hide();
        }
    }
}

void PrintSelectPanel::show_delete_confirmation() {
    if (!detail_view_) {
        spdlog::warn("[{}] Cannot show delete confirmation: detail_view_ not initialized",
                     get_name());
        return;
    }
    std::string filename(selected_filename_buffer_);
    detail_view_->show_delete_confirmation(filename);
}

void PrintSelectPanel::set_print_status_panel(lv_obj_t* panel) {
    print_status_panel_widget_ = panel;
    spdlog::trace("[{}] Print status panel reference set", get_name());
}

// ============================================================================
// Internal Methods
// ============================================================================

CardDimensions PrintSelectPanel::calculate_card_dimensions() {
    // Card thumbnails are pre-scaled to a fixed .bin size, so the processor has to know
    // how big a card really is — its own resolution ladder guessed too large and LVGL
    // cropped the art (#1208). Publish on every exit path, including the fallbacks.
    auto publish = [](const CardDimensions& d) {
        helix::ThumbnailProcessor::set_card_size_hint(d.card_width, d.card_height);
        return d;
    };

    if (!card_view_container_) {
        spdlog::error("[{}] Cannot calculate dimensions: container is null", get_name());
        return publish({4, 2, CARD_MIN_WIDTH, CARD_DEFAULT_HEIGHT});
    }

    lv_coord_t container_width = lv_obj_get_content_width(card_view_container_);
    // Read gap from container's XML-defined style (respects design tokens)
    // Note: style_pad_gap in XML sets both pad_row and pad_column; we read pad_column for width
    // calc
    int card_gap = lv_obj_get_style_pad_column(card_view_container_, LV_PART_MAIN);
    spdlog::trace("[{}] Container content width: {}px (MIN={}, MAX={}, GAP={})", get_name(),
                  container_width, CARD_MIN_WIDTH, CARD_MAX_WIDTH, card_gap);

    // Calculate available height from parent panel dimensions
    lv_obj_t* panel_root = lv_obj_get_parent(card_view_container_);
    if (!panel_root) {
        spdlog::error("[{}] Cannot find panel root", get_name());
        return publish({4, 2, CARD_MIN_WIDTH, CARD_DEFAULT_HEIGHT});
    }

    lv_coord_t panel_height = lv_obj_get_height(panel_root);
    lv_obj_t* top_bar = lv_obj_get_child(panel_root, 0);
    lv_coord_t top_bar_height = top_bar ? lv_obj_get_height(top_bar) : 60;
    lv_coord_t panel_gap = lv_obj_get_style_pad_row(panel_root, LV_PART_MAIN);
    lv_coord_t container_pad_top = lv_obj_get_style_pad_top(card_view_container_, LV_PART_MAIN);
    lv_coord_t container_pad_bottom =
        lv_obj_get_style_pad_bottom(card_view_container_, LV_PART_MAIN);
    lv_coord_t container_padding = container_pad_top + container_pad_bottom;
    lv_coord_t available_height = panel_height - top_bar_height - container_padding - panel_gap;

    spdlog::trace("[{}] Height calc: panel={} - top_bar={} - container_pad({}+{})={} - "
                  "panel_gap={} = available={}",
                  get_name(), panel_height, top_bar_height, container_pad_top, container_pad_bottom,
                  container_padding, panel_gap, available_height);

    CardDimensions dims;

    // Determine optimal number of rows based on available height
    dims.num_rows = (available_height >= ROW_COUNT_3_MIN_HEIGHT) ? 3 : 2;

    // Calculate card height based on rows
    // Each row takes card_height + gap (LVGL flex adds gap after each row)
    // Reserve a small bottom margin, then divide remaining height by num_rows
    int bottom_margin = card_gap / 2;
    int row_height = (available_height - bottom_margin) / dims.num_rows;
    dims.card_height = row_height - card_gap;

    int total_row_gaps = dims.num_rows * card_gap;
    int total_used = (dims.num_rows * dims.card_height) + total_row_gaps;
    spdlog::trace("[{}] Card height: row_height={} - gap={} = {}, total_used={}, remainder={}",
                  get_name(), row_height, card_gap, dims.card_height, total_used,
                  available_height - total_used);

    // Pick the column count whose card width lands nearest the size the card component
    // was designed for, rather than the count that fits the most columns.
    int best_cols = choose_card_columns(container_width, card_gap);
    if (best_cols > 0) {
        dims.num_columns = best_cols;
        dims.card_width = card_width_for_columns(container_width, card_gap, best_cols);

        spdlog::trace("[{}] Calculated card layout: {} rows x {} columns, card={}x{}", get_name(),
                      dims.num_rows, dims.num_columns, dims.card_width, dims.card_height);
        return publish(dims);
    }

    // Fallback
    dims.num_columns = container_width / (CARD_MIN_WIDTH + card_gap);
    if (dims.num_columns < 1)
        dims.num_columns = 1;
    dims.card_width = CARD_MIN_WIDTH;

    spdlog::warn("[{}] No optimal card layout found, using fallback: {} columns", get_name(),
                 dims.num_columns);
    return publish(dims);
}

void PrintSelectPanel::schedule_view_refresh() {
    // Use lv_async_call to ensure thread-safety (this may be called from WebSocket thread)
    helix::ui::async_call(
        [](void* user_data) {
            auto* self = static_cast<PrintSelectPanel*>(user_data);

            // Guard against async callback firing after display destruction
            if (!self->panel_ || !lv_obj_is_valid(self->panel_)) {
                return;
            }

            // If a timer is already pending, reset it (debounce)
            if (self->refresh_timer_) {
                lv_timer_reset(self->refresh_timer_);
                return;
            }

            // Create a one-shot timer to refresh views after debounce period
            self->refresh_timer_ = lv_timer_create(
                [](lv_timer_t* timer) {
                    auto* panel = static_cast<PrintSelectPanel*>(lv_timer_get_user_data(timer));
                    panel->refresh_timer_ = nullptr; // Clear before callback (timer auto-deletes)

                    // Guard against timer firing after display destruction
                    if (!panel->panel_ || !lv_obj_is_valid(panel->panel_)) {
                        return;
                    }

                    spdlog::trace("[{}] Debounced metadata refresh - updating visible cards only",
                                  panel->get_name());

                    // Only refresh CONTENT of currently visible cards - don't reset
                    // spacers/positions This prevents flashing when metadata/thumbnails arrive
                    // asynchronously
                    panel->refresh_visible_content();
                },
                REFRESH_DEBOUNCE_MS, self);

            // Make it a one-shot timer
            lv_timer_set_repeat_count(self->refresh_timer_, 1);
        },
        this);
}

void PrintSelectPanel::refresh_visible_content() {
    // Refresh content of currently visible cards without resetting positions
    // Delegates to extracted view modules
    if (card_view_ && card_view_->is_initialized()) {
        CardDimensions dims = calculate_card_dimensions();
        card_view_->refresh_content(file_list_, dims);
    }

    if (list_view_ && list_view_->is_initialized()) {
        list_view_->refresh_content(file_list_);
    }
}

void PrintSelectPanel::handle_scroll(lv_obj_t* container) {
    // Delegate to extracted view modules (they trigger metadata fetch via callback)
    if (container == card_view_container_ && card_view_) {
        CardDimensions dims = calculate_card_dimensions();
        card_view_->update_visible(file_list_, dims);
    } else if (container == list_rows_container_ && list_view_) {
        list_view_->update_visible(file_list_);
    }
}

void PrintSelectPanel::populate_card_view(bool preserve_scroll) {
    if (!card_view_ || !card_view_container_)
        return;

    spdlog::trace("[{}] populate_card_view() with {} files (virtualized, preserve_scroll={})",
                  get_name(), file_list_.size(), preserve_scroll);

    // Delegate to extracted card view module
    CardDimensions dims = calculate_card_dimensions();
    card_view_->populate(file_list_, dims, preserve_scroll);

    spdlog::trace("[{}] Card view populated with {} files", get_name(), file_list_.size());
}

void PrintSelectPanel::animate_view_entrance(lv_obj_t* container) {
    if (!container)
        return;

    // Skip animation if disabled - show container in final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_opa(container, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::debug("[{}] Animations disabled - showing view instantly", get_name());
        return;
    }

    // Animation constants for view transition
    constexpr int32_t FADE_DURATION_MS = 150;

    // Start container transparent
    lv_obj_set_style_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);

    // Fade in animation
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, container);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, FADE_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::debug("[{}] View entrance animation started", get_name());
}

void PrintSelectPanel::populate_list_view(bool preserve_scroll) {
    if (!list_view_ || !list_rows_container_)
        return;

    spdlog::debug("[{}] populate_list_view() with {} files (virtualized, preserve_scroll={})",
                  get_name(), file_list_.size(), preserve_scroll);

    // Delegate to extracted list view module
    list_view_->populate(file_list_, preserve_scroll);

    // Trigger entrance animation for newly visible rows (skip if preserving scroll)
    if (!preserve_scroll) {
        list_view_->animate_entrance();
    }

    spdlog::debug("[{}] List view populated with {} files", get_name(), file_list_.size());
}

void PrintSelectPanel::apply_sort() {
    file_sorter_.apply_sort(file_list_);
}

void PrintSelectPanel::merge_history_into_file_list() {
    auto* history_manager = get_print_history_manager();
    if (!history_manager) {
        spdlog::debug("[{}] No PrintHistoryManager available, skipping history merge", get_name());
        return;
    }

    // Trigger fetch if history not loaded yet
    if (!history_manager->is_loaded()) {
        spdlog::trace("[{}] History not loaded, triggering fetch", get_name());
        history_manager->fetch();
    }

    // Get currently printing filename (if any)
    std::string current_print_filename;
    // RAW_PRINT_STATE_OK: reads print_filename, which reset_for_new_print()
    // deliberately does NOT clear - so during a preparing window it still holds
    // the PREVIOUS job. Widening this would badge the wrong file rather than
    // none. Badging the committed file would mean reading preparing_job()
    // instead, which is a feature, not this migration.
    auto print_state = printer_state_.get_print_job_state();
    if (print_state == PrintJobState::PRINTING || print_state == PrintJobState::PAUSED) {
        if (auto* filename_subject = printer_state_.get_print_filename_subject()) {
            if (const char* filename = lv_subject_get_string(filename_subject);
                filename && filename[0] != '\0') {
                current_print_filename =
                    helix::ui::PrintSelectHistoryIntegration::extract_basename(filename);
            }
        }
    }

    // Delegate to history integration module
    helix::ui::PrintSelectHistoryIntegration::merge_history_into_files(
        file_list_, history_manager->get_filename_stats(), current_print_filename);

    spdlog::trace("[{}] Merged history status for {} files", get_name(), file_list_.size());
}

void PrintSelectPanel::update_empty_state() {
    if (!empty_state_container_)
        return;

    bool is_empty = file_list_.empty();

    if (is_empty) {
        lv_obj_remove_flag(empty_state_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_view_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(list_view_container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(empty_state_container_, LV_OBJ_FLAG_HIDDEN);

        if (current_view_mode_ == PrintSelectViewMode::CARD) {
            lv_obj_remove_flag(card_view_container_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(list_view_container_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(card_view_container_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(list_view_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void PrintSelectPanel::update_print_button_state() {
    // Update the can_print subject based on current print state and macro analysis
    // XML binding automatically disables button when value is 0
    bool can_print = printer_state_.can_start_new_print();

    // Also disable if macro analysis is in progress to prevent race conditions
    // where print starts before we know which skip params to use
    if (can_print && detail_view_) {
        if (auto* prep_mgr = detail_view_->get_prep_manager()) {
            if (prep_mgr->is_macro_analysis_in_progress()) {
                can_print = false;
                spdlog::trace("[{}] Print button disabled: macro analysis in progress", get_name());
            }
        }
    }

    int new_value = can_print ? 1 : 0;

    // Only update if value changed (avoid unnecessary subject notifications)
    if (lv_subject_get_int(&can_print_subject_) != new_value) {
        lv_subject_set_int(&can_print_subject_, new_value);
        spdlog::trace("[{}] Print button {} (can_start_new_print={})", get_name(),
                      can_print ? "enabled" : "disabled", can_print);
    }
}

void PrintSelectPanel::update_sort_indicators() {
    const char* header_names[] = {"header_filename", "header_size", "header_modified",
                                  "header_print_time"};
    PrintSelectSortColumn columns[] = {PrintSelectSortColumn::FILENAME, PrintSelectSortColumn::SIZE,
                                       PrintSelectSortColumn::MODIFIED,
                                       PrintSelectSortColumn::PRINT_TIME};

    // Animation constants for sort indicator transition
    constexpr int32_t FADE_DURATION_MS = 200;

    // Check if animations are enabled
    bool animations_enabled = DisplaySettingsManager::instance().get_animations_enabled();

    // Helper lambda for animated show/hide with crossfade
    auto animate_icon_visibility = [animations_enabled](lv_obj_t* icon, bool show) {
        if (!icon)
            return;

        if (show) {
            // Show icon
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);

            if (!animations_enabled) {
                // Instant show
                lv_obj_set_style_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
                return;
            }

            // Show with fade in
            lv_obj_set_style_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);

            lv_anim_t fade_in;
            lv_anim_init(&fade_in);
            lv_anim_set_var(&fade_in, icon);
            lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&fade_in, FADE_DURATION_MS);
            lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&fade_in, [](void* obj, int32_t value) {
                lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                                     LV_PART_MAIN);
            });
            lv_anim_start(&fade_in);
        } else {
            // Hide icon
            if (!animations_enabled) {
                // Instant hide
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                return;
            }

            // Hide with fade out (hide flag set in completion callback)
            bool is_visible = !lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN);
            if (is_visible) {
                lv_anim_t fade_out;
                lv_anim_init(&fade_out);
                lv_anim_set_var(&fade_out, icon);
                lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
                lv_anim_set_duration(&fade_out, FADE_DURATION_MS);
                lv_anim_set_path_cb(&fade_out, lv_anim_path_ease_in);
                lv_anim_set_exec_cb(&fade_out, [](void* obj, int32_t value) {
                    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                                         LV_PART_MAIN);
                });
                lv_anim_set_completed_cb(&fade_out, [](lv_anim_t* anim) {
                    lv_obj_add_flag(static_cast<lv_obj_t*>(anim->var), LV_OBJ_FLAG_HIDDEN);
                });
                lv_anim_start(&fade_out);
            } else {
                // Already hidden, just ensure it stays hidden
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
            }
        }
    };

    for (int i = 0; i < 4; i++) {
        char icon_up_name[64];
        char icon_down_name[64];
        snprintf(icon_up_name, sizeof(icon_up_name), "%s_icon_up", header_names[i]);
        snprintf(icon_down_name, sizeof(icon_down_name), "%s_icon_down", header_names[i]);

        lv_obj_t* icon_up = lv_obj_find_by_name(panel_, icon_up_name);
        lv_obj_t* icon_down = lv_obj_find_by_name(panel_, icon_down_name);

        if (icon_up && icon_down) {
            if (columns[i] == current_sort_column_) {
                if (current_sort_direction_ == PrintSelectSortDirection::ASCENDING) {
                    animate_icon_visibility(icon_up, true);
                    animate_icon_visibility(icon_down, false);
                } else {
                    animate_icon_visibility(icon_up, false);
                    animate_icon_visibility(icon_down, true);
                }
            } else {
                animate_icon_visibility(icon_up, false);
                animate_icon_visibility(icon_down, false);
            }
        }
    }
}

void PrintSelectPanel::create_detail_view() {
    if (detail_view_) {
        // The detail overlay lives under parent_screen_, not panel_, so it
        // survives a panel rebuild. setup() is re-entrant; only the first
        // call constructs the view.
        return;
    }
    detail_view_ = std::make_unique<helix::ui::PrintSelectDetailView>();

    // Initialize subjects BEFORE create() so XML bindings can find them [L004]
    detail_view_->init_subjects();

    // create() now returns lv_obj_t* per OverlayBase interface
    if (!detail_view_->create(parent_screen_)) {
        spdlog::error("[{}] Failed to create detail view", get_name());
        detail_view_.reset();
        return;
    }

    // Set dependencies and callbacks
    detail_view_->set_dependencies(api_, &printer_state_);
    detail_view_->set_visible_subject(&detail_view_visible_subject_);
    detail_view_->set_on_delete_confirmed([this]() { delete_file(); });

    // The detail view's tappable cards (the visible color-requirements swatch
    // card on U1, AND the embedded editable FilamentMappingCard on AFC/CFS) all
    // route their tap here so there is ONE opener and ONE modal instance for
    // every backend — the same entry the preflight "Remap…" button uses.
    detail_view_->set_on_remap_requested([this]() { open_remap_modal(); });

    // Re-enable the print button when macro analysis completes. The legacy
    // bullet-text "preprint steps" display has been replaced by the dynamic
    // PrePrintOption toggle UI in print_file_detail.xml, so no scan/analysis
    // text refresh is needed here anymore.
    if (auto* prep_mgr = detail_view_->get_prep_manager()) {
        prep_mgr->set_macro_analysis_callback(
            [this](const helix::PrintStartAnalysis& /*analysis*/) { update_print_button_state(); });
    }

    // Create and wire up print start controller
    print_controller_ = std::make_unique<helix::ui::PrintStartController>(printer_state_, api_);
    print_controller_->set_detail_view(detail_view_.get());
    print_controller_->set_can_print_subject(&can_print_subject_);
    print_controller_->set_update_print_button([this]() { update_print_button_state(); });
    print_controller_->set_hide_detail_view([this]() { hide_detail_view(); });
    print_controller_->set_show_detail_view([this]() { show_detail_view(); });
    print_controller_->set_navigate_to_print_status(
        [this]() { PrintStatusPanel::push_overlay(parent_screen_); });

    // Crash recovery: restore firmware mapping if app restarted mid-print
    print_controller_->recover_pending_remap();

    spdlog::debug("[{}] Detail view module initialized", get_name());
}

void PrintSelectPanel::hide_delete_confirmation() {
    if (detail_view_) {
        detail_view_->hide_delete_confirmation();
    }
}

void PrintSelectPanel::handle_resize() {
    if (!panel_initialized_)
        return;

    spdlog::info("[{}] Handling resize event", get_name());

    if (current_view_mode_ == PrintSelectViewMode::CARD && card_view_container_) {
        populate_card_view(true); // Preserve scroll on resize
    }

    if (detail_view_ && parent_screen_) {
        detail_view_->handle_resize(parent_screen_);
    }
}

void PrintSelectPanel::attach_card_click_handler(lv_obj_t* card, size_t file_index) {
    // Store file index in widget user_data
    lv_obj_set_user_data(card, reinterpret_cast<void*>(file_index));
    lv_obj_add_event_cb(card, on_file_clicked_static, LV_EVENT_CLICKED, this);
}

void PrintSelectPanel::attach_row_click_handler(lv_obj_t* row, size_t file_index) {
    lv_obj_set_user_data(row, reinterpret_cast<void*>(file_index));
    lv_obj_add_event_cb(row, on_file_clicked_static, LV_EVENT_CLICKED, this);
}

void PrintSelectPanel::apply_file_selection(const PrintFileData& file) {
    // For filament display, prefer filament_name if available (e.g., "PolyMaker PolyLite ABS")
    // Fallback to short filament_type (e.g., "ABS") if no name provided
    std::string filament_display =
        file.filament_name.empty() ? file.filament_type : file.filament_name;
    set_selected_file(file.filename.c_str(), file.thumbnail_path.c_str(),
                      file.original_thumbnail_url.c_str(), file.print_time_str.c_str(),
                      file.filament_str.c_str(), file.layer_count_str.c_str(),
                      file.print_height_str.c_str(), file.modified_timestamp,
                      file.layer_height_str.c_str(), filament_display.c_str());
    selected_filament_type_ = file.filament_type;
    selected_filament_colors_ = file.filament_colors;
    selected_filament_materials_ = file.filament_types;
    selected_file_size_bytes_ = file.file_size_bytes;
    selected_modified_timestamp_ = file.modified_timestamp;
    selected_gcode_end_byte_ = file.gcode_end_byte;
    selected_history_status_ = file.history_status;
    selected_success_count_ = file.success_count;
}

void PrintSelectPanel::handle_file_click(size_t file_index) {
    if (file_index >= file_list_.size()) {
        spdlog::warn("[{}] Ignoring click on stale file index {} (list size {})", get_name(),
                     file_index, file_list_.size());
        return;
    }

    const auto& file = file_list_[file_index];

    if (file.is_dir) {
        // Close detail view before navigating to prevent stale file references
        if (detail_view_open_) {
            spdlog::debug("[{}] Closing detail view before directory navigation", get_name());
            hide_detail_view();
        }

        if (file.filename == "..") {
            // Parent directory - navigate up
            navigate_up();
        } else {
            // Directory clicked - navigate into it
            navigate_to_directory(file.filename);
        }
    } else {
        // File clicked - apply selection state and open the detail view
        apply_file_selection(file);
        show_detail_view();

        // A programmatically- or off-screen-selected file (e.g. "Print Last")
        // may never have had its metadata fetched — the viewport scan only
        // covers visible cards. Its list entry is then empty, so the detail
        // view shows 0 min / 0.0 g and the no-thumbnail placeholder. Trigger the
        // fetch now; process_metadata_result() re-syncs the open detail view via
        // set_selected_file() once the metadata (and real thumbnail) land.
        if (!file_list_[file_index].metadata_fetched) {
            fetch_metadata_range(file_index, file_index + 1);
        }
    }
}

void PrintSelectPanel::on_file_long_pressed(size_t file_index) {
    if (file_index >= file_list_.size()) {
        spdlog::warn("[{}] Ignoring long-press on stale file index {} (list size {})", get_name(),
                     file_index, file_list_.size());
        return;
    }

    const auto& file = file_list_[file_index];
    if (file.is_dir) {
        // Card view already filters directories, but double-check in case the
        // list mutated between the long-press timer starting and firing.
        spdlog::trace("[{}] long-press on directory ignored: {}", get_name(), file.filename);
        return;
    }

    spdlog::info("[{}] Long-press delete requested: {}", get_name(), file.filename);

    // Apply the same selection state a normal tap would, so the existing
    // delete_file() path (which reads selected_filename_buffer_ and friends)
    // works unchanged.
    apply_file_selection(file);

    // Reuse the existing confirmation modal. The detail view's
    // show_delete_confirmation() is a thin wrapper around modal_show_confirmation()
    // and does not require the detail view to be visible.
    show_delete_confirmation();
}

void PrintSelectPanel::start_print(bool force) {
    if (!print_controller_) {
        spdlog::error("[{}] Cannot start print - controller not initialized", get_name());
        NOTIFY_ERROR(lv_tr("Cannot start print: internal error"));
        return;
    }

    // Pre-flight gate: filament checks + tools_used are only valid once the file
    // has been scanned. Readiness arrives via EITHER the visual viewer parse
    // (full platforms) OR the headless tools_used scan (runs on ALL platforms,
    // including 2D-only where the viewer skips parsing). Gating on
    // is_preflight_ready() — NOT is_gcode_loaded(), which never becomes true on
    // 2D-only and would hang the print forever (the original bug). If Print is
    // tapped before readiness, defer the attempt; run_when_preflight_ready()
    // carries a safety timeout so a stuck/failed scan can never wedge the print.
    // force=true bypasses (deferred re-entry + "Print Anyway" both call with force).
    if (!force && detail_view_ && !detail_view_->is_preflight_ready()) {
        spdlog::debug("[{}] Print tapped before pre-flight ready - deferring", get_name());
        detail_view_->run_when_preflight_ready(
            []() { get_global_print_select_panel().start_print(false); });
        NOTIFY_INFO(lv_tr("Checking filament..."));
        return;
    }

    // Pre-flight check: hard-confirm before printing when the cached
    // PreflightResult reports a definitively-empty required filament slot
    // (has_block(): any tool the gcode references maps to an empty slot).
    // Backend-agnostic — computed for ALL backends in try_extract_gcode_colors().
    // Material mismatch is advisory only (surfaced via the warning icon) and does
    // NOT block here. Bypassed when force=true (user pressed "Print Anyway").
    if (!force && detail_view_) {
        const auto& pf = detail_view_->preflight_result();
        if (pf.has_block()) {
            show_preflight_modal(pf);
            return;
        }
    }

    // Set the file to print in the controller
    // Pass extracted thumbnail path so USB/embedded thumbnails propagate to print status
    print_controller_->set_file(selected_filename_buffer_, current_path_, selected_filament_colors_,
                                selected_detail_thumbnail_buffer_);

    // Delegate to the print start controller
    print_controller_->initiate();
}

void PrintSelectPanel::show_preflight_modal(const helix::PreflightResult& pf) {
    // Heap-allocated; self-deletes in PreflightCheckModal::on_hide() (LVGL /
    // ModalStack cleanup never calls Modal::~Modal). Mirrors the spaghetti
    // detection modal's lifecycle.
    auto* modal = new helix::ui::PreflightCheckModal();
    modal->set_checks(pf);
    modal->set_on_force([]() { get_global_print_select_panel().start_print(true); });
    modal->set_on_remap([]() { get_global_print_select_panel().on_preflight_remap(); });
    modal->show(lv_screen_active());
}

void PrintSelectPanel::on_preflight_remap() {
    open_remap_modal();
}

// ============================================================================
// Unified filament remap
// ============================================================================
//
// ONE opener for every backend. The picker inputs are built UNIFORMLY:
//   - tool_info  from the cached pre-flight checks (each ToolCheck carries the
//                tool index + the gcode's intended color/material), so the modal
//                shows exactly the tools this print uses.
//   - slots      from AmsState::collect_available_slots() (canonical accessor).
//   - mappings   the current effective mappings if the card already holds any
//                (preserves in-progress AFC/CFS edits), else FilamentMapper
//                defaults.
//
// The per-backend difference is ONLY in how the chosen mapping is applied — see
// apply_remap(), dispatched on RemapStrategy. Print-start is likewise already
// strategy-dispatched (PrintStartController), so Native and SnapmakerNative both
// read the same shared card store and need no special opener.
void PrintSelectPanel::open_remap_modal() {
    if (!detail_view_) {
        spdlog::warn("[{}] Remap requested with no detail view", get_name());
        return;
    }

    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    const auto strategy = backend->get_remap_strategy();
    if (strategy == AmsBackend::RemapStrategy::None) {
        return;
    }

    // GcodeRewrite is the generic fallback for backends with no native routing
    // table: it prints a modified temp copy and relies on the HelixPrint plugin
    // to patch print history back to the original filename. No backend ships this
    // strategy today (ACE will adopt it once ACE_CHANGE_TOOL is implemented).
    // Guard BEFORE opening the modal so the user sees the actionable alert instead
    // of a picker whose Done would silently fail.
    if (strategy == AmsBackend::RemapStrategy::GcodeRewrite &&
        !printer_state_.service_has_helix_plugin()) {
        helix::ui::modal_show_alert(
            lv_tr("Remap needs the HelixPrint plugin"),
            lv_tr("Install the HelixPrint plugin in Advanced settings to remap "
                  "filament without affecting print history."),
            ModalSeverity::Info, lv_tr("OK"), nullptr, nullptr);
        return;
    }

    // Build picker inputs from the per-tool info of the tools this file uses.
    // Sourced directly from the slicer palette (Moonraker metadata) via the
    // detail view's single assembler — populated on ALL backends, including
    // those whose inline mapping card is hidden (U1 / ACE / 2D-only), where the
    // card instance's tool_info_ is empty.
    auto tool_info = detail_view_->get_used_tool_info();

    if (tool_info.empty()) {
        spdlog::warn("[{}] Remap: no tools in pre-flight result", get_name());
        NOTIFY_INFO(lv_tr("Nothing to remap"));
        return;
    }

    auto slots = AmsState::instance().collect_available_slots();

    // Seed the picker with the mappings the print will actually use. For editable
    // backends (AFC / CFS / Happy Hare / AD5X-IFS) the card already holds the
    // current/positional mappings, so reusing them preserves in-progress edits;
    // for U1 / ACE the card is hidden and get_filament_mappings() is empty, so we
    // fall back to FilamentMapper defaults (identical to the prior per-backend
    // openers).
    auto mappings = detail_view_->get_filament_mappings();
    if (mappings.empty()) {
        mappings = helix::FilamentMapper::compute_defaults(tool_info, slots);
    }

    remap_modal_.set_tool_info(tool_info);
    remap_modal_.set_available_slots(slots);
    remap_modal_.set_mappings(mappings);
    remap_modal_.set_on_mappings_updated(
        [this](std::vector<helix::ToolMapping> updated) { apply_remap(updated); });
    remap_modal_.show(lv_screen_active());
}

// Strategy-dispatched APPLY — the ONLY place the backends diverge in the UI.
void PrintSelectPanel::apply_remap(const std::vector<helix::ToolMapping>& updated) {
    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }

    switch (backend->get_remap_strategy()) {
    case AmsBackend::RemapStrategy::GcodeRewrite: {
        // Generic fallback (no backend ships it yet; ACE once ACE_CHANGE_TOOL
        // lands): rewrite the Tx / ACTIVATE_EXTRUDER / SET_GCODE_VARIABLE lines in
        // the gcode and print the modified copy via the HelixPrint plugin (history
        // stays under the original filename). Plugin presence was already guarded
        // in open_remap_modal().
        std::map<int, int> remap;
        for (const auto& m : updated) {
            if (m.tool_index < 0 || m.mapped_slot < 0) {
                continue; // auto / unmapped — leave the gcode's tool number alone
            }
            // slot index == physical head for these backends, so mapped_slot is the
            // head the gcode rewrite targets. A backend where slot != head would
            // need a slot→head translation here.
            remap[m.tool_index] = m.mapped_slot;
        }

        auto* prep = detail_view_ ? detail_view_->get_prep_manager() : nullptr;
        if (!prep) {
            spdlog::warn("[{}] Gcode remap: no prep manager", get_name());
            NOTIFY_ERROR(lv_tr("Cannot remap: internal error"));
            return;
        }

        std::string filename(selected_filename_buffer_);
        std::string file_path = current_path_.empty() ? filename : current_path_ + "/" + filename;

        spdlog::info("[{}] Applying gcode remap: {} tool(s) for {}", get_name(), remap.size(),
                     file_path);
        // The pipeline guards identity remaps internally (prints the original
        // unmodified when nothing changed).
        prep->modify_and_print_with_remap(
            file_path, remap, [this]() { PrintStatusPanel::push_overlay(parent_screen_); });
        break;
    }

    case AmsBackend::RemapStrategy::SnapmakerNative: {
        // Snapmaker U1: log each non-identity choice for on-device confirmation,
        // then push the full vector into the shared card store. The backend send
        // (SET_PRINT_EXTRUDER_MAP via build_preprint_gcode) happens at print-start
        // through PrintStartController, which reads get_effective_remap() — that
        // identity-filters, so only changed tools emit a line.
        auto default_head = [](int tool) { return (tool >= 0 && tool <= 3) ? tool : 0; };
        for (const auto& m : updated) {
            if (m.tool_index >= 0 && m.mapped_slot >= 0 &&
                m.mapped_slot != default_head(m.tool_index)) {
                spdlog::debug("[{}] U1 remap applied: tool {} -> head {}", get_name(), m.tool_index,
                              m.mapped_slot);
            }
        }
        if (detail_view_) {
            detail_view_->set_filament_mappings(updated);
        }
        break;
    }

    case AmsBackend::RemapStrategy::Native:
        // AFC / Happy Hare / CFS / AD5X-IFS / toolchanger: push the chosen
        // mappings into the shared card store. The backend send (set_tool_mapping
        // per changed tool) happens at print-start through PrintStartController,
        // which reads get_filament_mappings(). The card's on_mappings_changed
        // path re-runs the pre-flight gate so a subsequent Print clears the block.
        if (detail_view_) {
            detail_view_->set_filament_mappings(updated);
        }
        break;

    case AmsBackend::RemapStrategy::None:
        break;
    }
}

void PrintSelectPanel::delete_file() {
    std::string filename_to_delete(selected_filename_buffer_);
    auto* self = this;
    auto token = lifetime_.token();

    if (api_) {
        // Construct full path: gcodes/<current_path>/<filename>
        // Moonraker's delete_file requires the full path including root
        std::string full_path;
        if (current_path_.empty()) {
            full_path = "gcodes/" + filename_to_delete;
        } else {
            full_path = "gcodes/" + current_path_ + "/" + filename_to_delete;
        }

        spdlog::info("[{}] Deleting file: {}", get_name(), full_path);

        // Context struct for async callbacks
        struct DeleteFileContext {
            PrintSelectPanel* panel;
            helix::LifetimeToken token;
        };

        api_->files().delete_file(
            full_path,
            // Success callback - dispatch to main thread for LVGL safety
            [self, token]() {
                // === MAIN THREAD: log uses self->get_name(); inner queue_update preserved ===
                token.defer("PrintSelectPanel::delete_file_success", [self, token]() {
                    spdlog::info("[{}] File deleted successfully", self->get_name());
                    struct SuccessContext {
                        PrintSelectPanel* panel;
                        helix::LifetimeToken token;
                    };
                    auto ctx = std::make_unique<SuccessContext>(SuccessContext{self, token});
                    helix::ui::queue_update<SuccessContext>(std::move(ctx), [](SuccessContext* c) {
                        if (c->token.expired()) {
                            return;
                        }
                        c->panel->hide_delete_confirmation();
                        c->panel->hide_detail_view();
                        c->panel->refresh_files();
                    });
                });
            },
            // Error callback - dispatch to main thread for LVGL safety
            [self, token](const MoonrakerError& error) {
                // === MAIN THREAD: log uses self->get_name(); inner queue_update preserved ===
                token.defer("PrintSelectPanel::delete_file_error", [self, token, error]() {
                    LOG_ERROR_INTERNAL("[{}] File delete error: {} ({})", self->get_name(),
                                       error.message, error.get_type_string());
                    struct ErrorContext {
                        PrintSelectPanel* panel;
                        helix::LifetimeToken token;
                    };
                    auto ctx = std::make_unique<ErrorContext>(ErrorContext{self, token});
                    helix::ui::queue_update<ErrorContext>(std::move(ctx), [](ErrorContext* c) {
                        if (c->token.expired()) {
                            return;
                        }
                        NOTIFY_ERROR(lv_tr("Failed to delete file"));
                        c->panel->hide_delete_confirmation();
                    });
                });
            });
    } else {
        NOTIFY_WARNING(lv_tr("Cannot delete file: printer not connected"));
        hide_delete_confirmation();
    }
}

// ============================================================================
// Static Callbacks (trampolines)
// ============================================================================

void PrintSelectPanel::on_scroll_static(lv_event_t* e) {
    auto* self = static_cast<PrintSelectPanel*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self && target) {
        self->handle_scroll(target);
    }
}

void PrintSelectPanel::on_file_clicked_static(lv_event_t* e) {
    auto* self = static_cast<PrintSelectPanel*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self && target) {
        size_t file_index = reinterpret_cast<size_t>(lv_obj_get_user_data(target));
        self->handle_file_click(file_index);
    }
}

bool PrintSelectPanel::select_file_by_name(const std::string& filename) {
    // Search for the file in the current file list
    for (size_t i = 0; i < file_list_.size(); ++i) {
        const auto& file = file_list_[i];
        if (!file.is_dir && file.filename == filename) {
            // Found it - simulate a file click
            spdlog::info("[{}] Programmatically selecting file: {}", get_name(), filename);
            handle_file_click(i);
            return true;
        }
    }

    spdlog::warn("[{}] File not found for selection: {}", get_name(), filename);
    return false;
}

void PrintSelectPanel::set_pending_file_selection(const std::string& filename) {
    pending_file_selection_ = filename;
    spdlog::info("[{}] Set pending file selection: '{}'", get_name(), filename);
}

// ============================================================================
// USB Source Methods (delegate to usb_source_ module)
// ============================================================================

void PrintSelectPanel::on_source_printer_clicked() {
    if (!usb_source_) {
        spdlog::warn("[{}] USB source module not initialized", get_name());
        return;
    }
    usb_source_->select_printer_source();
}

void PrintSelectPanel::on_source_usb_clicked() {
    if (!usb_source_) {
        spdlog::warn("[{}] USB source module not initialized", get_name());
        return;
    }
    usb_source_->select_usb_source();
}

void PrintSelectPanel::set_usb_manager(UsbManager* manager) {
    pending_usb_manager_ = manager;
    if (usb_source_) {
        usb_source_->set_usb_manager(manager);
    }
    spdlog::trace("[{}] UsbManager set (usb_source_={})", get_name(), usb_source_ != nullptr);
}

void PrintSelectPanel::on_usb_drive_inserted() {
    if (usb_source_) {
        usb_source_->on_drive_inserted();
    }
}

void PrintSelectPanel::on_usb_drive_removed() {
    if (usb_source_) {
        usb_source_->on_drive_removed();
    }
    // Note: The usb_source_ module handles switching to Printer source if needed,
    // and the on_source_changed callback triggers refresh_files()
}
