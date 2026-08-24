// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_console.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_global_panel_helper.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_overlay_console_settings.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "console_filter_engine.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

// ============================================================================
// Global Instance
// ============================================================================

DEFINE_GLOBAL_PANEL(ConsolePanel, g_console_panel, get_global_console_panel)

// ============================================================================
// HTML Span Parsing (for AFC/Happy Hare colored output)
// ============================================================================

namespace {

// HTML span tag delimiters used by AFC/Happy Hare plugins (Mainsail-style)
constexpr const char SPAN_OPEN[] = "<span class=";
constexpr size_t SPAN_OPEN_LEN = sizeof(SPAN_OPEN) - 1; // 12
constexpr const char SPAN_CLOSE[] = "</span>";
constexpr size_t SPAN_CLOSE_LEN = sizeof(SPAN_CLOSE) - 1; // 7

struct TextSegment {
    std::string text;
    std::string color_class; // empty = default, "success", "info", "warning", "error"
};

/**
 * @brief Extract a color class name from a span's class attribute
 *
 * Maps "success--text" -> "success", "info--text" -> "info", etc.
 * Returns empty string for unrecognized classes.
 */
std::string extract_color_class(const std::string& class_attr) {
    static constexpr std::pair<const char*, const char*> mappings[] = {
        {"success--text", "success"},
        {"info--text", "info"},
        {"warning--text", "warning"},
        {"error--text", "error"},
    };
    for (const auto& [pattern, name] : mappings) {
        if (class_attr.find(pattern) != std::string::npos) {
            return name;
        }
    }
    return {};
}

/**
 * @brief Check if a message contains HTML spans we can parse
 *
 * Looks for Mainsail-style spans from AFC/Happy Hare plugins:
 * <span class=success--text>LOADED</span>
 */
bool contains_html_spans(const std::string& message) {
    return message.find(SPAN_OPEN) != std::string::npos &&
           (message.find("success--text") != std::string::npos ||
            message.find("info--text") != std::string::npos ||
            message.find("warning--text") != std::string::npos ||
            message.find("error--text") != std::string::npos);
}

/**
 * @brief Parse HTML span tags into text segments with color classes
 *
 * Parses Mainsail-style spans: <span class=XXX--text>content</span>
 * Returns vector of segments, each with text and optional color class.
 */
std::vector<TextSegment> parse_html_spans(const std::string& message) {
    std::vector<TextSegment> segments;

    size_t pos = 0;
    const size_t len = message.size();

    while (pos < len) {
        size_t span_start = message.find(SPAN_OPEN, pos);

        if (span_start == std::string::npos) {
            std::string remaining = message.substr(pos);
            if (!remaining.empty()) {
                segments.push_back({std::move(remaining), {}});
            }
            break;
        }

        // Add any text before the span as a plain segment
        if (span_start > pos) {
            segments.push_back({message.substr(pos, span_start - pos), {}});
        }

        // Find the class value (ends at >)
        size_t class_start = span_start + SPAN_OPEN_LEN;
        size_t class_end = message.find('>', class_start);

        if (class_end == std::string::npos) {
            // Malformed - add rest as plain text
            segments.push_back({message.substr(span_start), {}});
            break;
        }

        std::string color_class =
            extract_color_class(message.substr(class_start, class_end - class_start));

        // Find the closing </span>
        size_t content_start = class_end + 1;
        size_t span_close = message.find(SPAN_CLOSE, content_start);

        if (span_close == std::string::npos) {
            // No closing tag - add rest as colored text
            segments.push_back({message.substr(content_start), color_class});
            break;
        }

        std::string content = message.substr(content_start, span_close - content_start);
        if (!content.empty()) {
            segments.push_back({std::move(content), std::move(color_class)});
        }

        pos = span_close + SPAN_CLOSE_LEN;
    }

    return segments;
}

/**
 * @brief Format a Unix timestamp as HH:MM:SS local time
 *
 * @param timestamp Unix timestamp (> 0), or 0 to use current time
 * @return Formatted string "HH:MM:SS " (with trailing space)
 */
std::string format_timestamp(double timestamp) {
    time_t t;
    if (timestamp > 0.0) {
        t = static_cast<time_t>(timestamp);
    } else {
        t = std::time(nullptr);
    }
    struct tm tm_buf {};
    localtime_r(&t, &tm_buf);
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d ", tm_buf.tm_hour, tm_buf.tm_min,
                  tm_buf.tm_sec);
    return std::string(buf);
}

} // namespace

// ============================================================================
// Constructor
// ============================================================================

ConsolePanel::ConsolePanel() {
    spdlog::trace("[{}] Constructor", get_name());
}

ConsolePanel::~ConsolePanel() {
    deinit_subjects();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void ConsolePanel::init_subjects() {
    init_subjects_guarded([this]() {
        // Initialize status subject for reactive binding
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, lv_tr("Loading history..."),
                                  "console_status", subjects_);
        // Status label visibility (1 = visible, 0 = hidden)
        UI_MANAGED_SUBJECT_INT(status_visible_subject_, 1, "console_status_visible", subjects_);
        // Entry presence (1 = has entries, 0 = empty/show empty state)
        UI_MANAGED_SUBJECT_INT(has_entries_subject_, 0, "console_has_entries", subjects_);

        // Seed filter flags from SettingsManager and observe future changes.
        // SettingsManager subjects are static (singleton-owned) — no SubjectLifetime needed.
        auto& sm = helix::SettingsManager::instance();
        filter_temps_ = sm.get_console_filter_temps();
        filter_firmware_noise_ = sm.get_console_filter_firmware_noise();

        filter_temps_observer_ = helix::ui::observe_int_sync(
            sm.subject_console_filter_temps(), this,
            [](ConsolePanel* self, int v) { self->filter_temps_ = (v != 0); });
        filter_firmware_observer_ = helix::ui::observe_int_sync(
            sm.subject_console_filter_firmware_noise(), this,
            [](ConsolePanel* self, int v) { self->filter_firmware_noise_ = (v != 0); });
    });
}

void ConsolePanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // Drop observers before subjects (subjects are static so order is mainly cosmetic here).
    filter_temps_observer_.reset();
    filter_firmware_observer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

void ConsolePanel::rebuild_firmware_filter() {
    // Always rebuild on activate so user-edited pattern lists take effect when
    // the user returns from the settings overlay. Cost is trivial — a few
    // dozen prefix-string copies plus optional regex compilation.
    const std::string& printer = get_printer_state().get_printer_type();
    firmware_filter_.clear();

    auto preset = PrinterDetector::get_console_filter_patterns(printer);
    auto& sm = helix::SettingsManager::instance();
    const auto user_remove = sm.get_console_filter_user_remove();
    for (const auto& spec : preset) {
        if (std::find(user_remove.begin(), user_remove.end(), spec) != user_remove.end()) {
            continue; // User chose to drop this preset entry.
        }
        firmware_filter_.add(spec);
    }
    firmware_filter_.add_all(sm.get_console_filter_user_add());
    spdlog::debug("[{}] Firmware filter rebuilt for '{}': {} patterns", get_name(), printer,
                  firmware_filter_.size());
}

// ============================================================================
// Callback Registration
// ============================================================================

void ConsolePanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    // Register XML event callbacks for send, clear, and filter buttons
    register_xml_callbacks({
        {"on_console_send_clicked",
         [](lv_event_t* /*e*/) {
             spdlog::debug("[Console] Send button clicked");
             get_global_console_panel().send_gcode_command();
         }},
        {"on_console_clear_clicked",
         [](lv_event_t* /*e*/) {
             spdlog::debug("[Console] Clear button clicked");
             helix::ui::modal_show_confirmation(
                 lv_tr("Clear Console?"), lv_tr("This will remove all entries from the display."),
                 ModalSeverity::Info, lv_tr("Clear"),
                 [](lv_event_t* /*e*/) {
                     Modal::hide(Modal::get_top());
                     get_global_console_panel().clear_display();
                 },
                 nullptr, nullptr);
         }},
        {"on_console_settings_clicked",
         [](lv_event_t* /*e*/) {
             spdlog::debug("[Console] Settings gear clicked - opening overlay");
             auto& overlay = get_global_console_settings();
             auto* root = overlay.get_root();
             if (!root) {
                 root = overlay.create(lv_screen_active());
             }
             if (root) {
                 NavigationManager::instance().register_overlay_instance(root, &overlay);
                 NavigationManager::instance().push_overlay(root);
             }
         }},
    });

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* ConsolePanel::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, "console_panel")) {
        return nullptr;
    }

    // Find widget references
    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (overlay_content) {
        console_container_ = lv_obj_find_by_name(overlay_content, "console_container");
        empty_state_ =
            console_container_ ? lv_obj_find_by_name(console_container_, "empty_state") : nullptr;
        status_label_ = lv_obj_find_by_name(overlay_content, "status_message");

        // Find the input row and get the text input
        lv_obj_t* input_row = lv_obj_find_by_name(overlay_content, "input_row");
        if (input_row) {
            gcode_input_ = lv_obj_find_by_name(input_row, "gcode_input");
            if (gcode_input_) {
                spdlog::debug("[{}] Found gcode_input textarea", get_name());
            }
        }
    }

    if (!console_container_) {
        spdlog::error("[{}] console_container not found!", get_name());
        return nullptr;
    }

    // Track user scroll position for smart auto-scroll behavior.
    // When user scrolls up to read history, auto-scroll is paused.
    lv_obj_add_event_cb(
        console_container_,
        [](lv_event_t* e) {
            auto* panel = static_cast<ConsolePanel*>(lv_event_get_user_data(e));
            auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
            int32_t scroll_bottom = lv_obj_get_scroll_bottom(obj);
            // At bottom when remaining scroll distance is negligible (within 5px)
            panel->user_scrolled_up_ = (scroll_bottom > 5);
        },
        LV_EVENT_SCROLL, this);

    if (!gcode_input_) {
        spdlog::warn("[{}] gcode_input not found - input disabled", get_name());
    } else {
        // Enter key submits the command (LV_EVENT_READY fires on Enter
        // in a one-line textarea)
        lv_obj_add_event_cb(
            gcode_input_,
            [](lv_event_t* e) {
                auto* panel = static_cast<ConsolePanel*>(lv_event_get_user_data(e));
                panel->send_gcode_command();
            },
            LV_EVENT_READY, this);

        // Up/Down arrow keys navigate command history
        lv_obj_add_event_cb(
            gcode_input_,
            [](lv_event_t* e) {
                auto* panel = static_cast<ConsolePanel*>(lv_event_get_user_data(e));
                uint32_t key = lv_event_get_key(e);

                if (key == LV_KEY_UP) {
                    if (panel->command_history_.empty()) {
                        return;
                    }
                    // Save current input on first press into history
                    if (panel->history_index_ == -1) {
                        const char* cur = lv_textarea_get_text(panel->gcode_input_);
                        panel->saved_input_ = cur ? cur : "";
                    }
                    // Move to older command (increment index)
                    int next = panel->history_index_ + 1;
                    if (next < static_cast<int>(panel->command_history_.size())) {
                        panel->history_index_ = next;
                        lv_textarea_set_text(
                            panel->gcode_input_,
                            panel->command_history_[static_cast<size_t>(next)].c_str());
                    }
                } else if (key == LV_KEY_DOWN) {
                    if (panel->history_index_ < 0) {
                        return;
                    }
                    // Move to newer command (decrement index)
                    int next = panel->history_index_ - 1;
                    if (next >= 0) {
                        panel->history_index_ = next;
                        lv_textarea_set_text(
                            panel->gcode_input_,
                            panel->command_history_[static_cast<size_t>(next)].c_str());
                    } else {
                        // Restore saved input
                        panel->history_index_ = -1;
                        lv_textarea_set_text(panel->gcode_input_, panel->saved_input_.c_str());
                    }
                }
            },
            LV_EVENT_KEY, this);
    }

    // Show timestamps on medium+ screens. Gate on the constrained axis (min of
    // width/height): the timestamp column is horizontal, so a narrow portrait
    // screen should hide it even though its height is large.
    int32_t resp_res = responsive_dimension(lv_display_get_default());
    show_timestamps_ = (resp_res > UI_BREAKPOINT_SMALL_MAX);
    spdlog::debug("[{}] Timestamps {} (min_dim={})", get_name(),
                  show_timestamps_ ? "enabled" : "disabled", resp_res);

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void ConsolePanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Reset state for fresh activation
    user_scrolled_up_ = false;
    history_index_ = -1;
    saved_input_.clear();

    // Refresh the firmware-noise filter for the active printer (cheap if unchanged).
    rebuild_firmware_filter();

    // Refresh history when panel becomes visible
    fetch_history();
    // Subscribe to real-time updates
    subscribe_to_gcode_responses();
}

void ConsolePanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Unsubscribe from real-time updates
    unsubscribe_from_gcode_responses();

    // Call base class
    OverlayBase::on_deactivate();
}

void ConsolePanel::on_ui_destroyed() {
    console_container_ = nullptr;
    empty_state_ = nullptr;
    status_label_ = nullptr;
    gcode_input_ = nullptr;
}

// ============================================================================
// Data Loading
// ============================================================================

void ConsolePanel::fetch_history() {
    // I5: Prevent concurrent fetches (rapid activate/deactivate cycles). A
    // healthy in-flight fetch short-circuits here; a stuck one (response lost
    // >30s ago) falls through and is recovered by try_acquire() below.
    if (fetch_guard_.active() && !fetch_guard_.is_stuck()) {
        spdlog::debug("[{}] Fetch already in flight, skipping", get_name());
        return;
    }

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No IMoonrakerAPI available", get_name());
        std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Not connected to printer"));
        lv_subject_copy_string(&status_subject_, status_buf_);
        update_visibility();
        return;
    }

    if (fetch_guard_.try_acquire() == helix::InFlightGuard::AcquireResult::RecoveredStuck) {
        spdlog::warn("[{}] fetch in-flight flag stuck for {}ms — treating prior response as lost "
                     "and retrying",
                     get_name(), fetch_guard_.stuck_threshold().count());
    }

    // Update status while loading
    std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Loading..."));
    lv_subject_copy_string(&status_subject_, status_buf_);

    // Request gcode history from Moonraker
    // CRITICAL: Callbacks run on libhv background thread - defer LVGL work to main thread
    // and guard with lifetime_ token to prevent use-after-free if panel is destroyed
    api->get_gcode_store(
        FETCH_COUNT,
        [this, token = lifetime_.token()](const std::vector<GcodeStoreEntry>& entries) {
            spdlog::info("[Console] Received {} gcode entries", entries.size());

            // Convert to our entry format on background thread (no LVGL calls)
            std::vector<GcodeEntry> converted;
            converted.reserve(entries.size());

            for (const auto& entry : entries) {
                GcodeEntry e;
                e.message = entry.message;
                e.timestamp = entry.time;
                e.type = (entry.type == "command") ? GcodeEntry::Type::COMMAND
                                                   : GcodeEntry::Type::RESPONSE;
                e.is_error = is_error_message(entry.message);
                converted.push_back(e);
            }

            // Defer LVGL operations to main thread; defer has its own atomic
            // expiration check on the main thread (avoids L081 Mechanism C noise).
            auto entries_ptr = std::make_shared<std::vector<GcodeEntry>>(std::move(converted));
            token.defer("ConsolePanel::fetch_done", [this, entries_ptr]() {
                fetch_guard_.release();
                populate_entries(*entries_ptr);
            });
        },
        [this, token = lifetime_.token()](const MoonrakerError& err) {
            spdlog::error("[Console] Failed to fetch gcode store: {}", err.message);

            token.defer("ConsolePanel::fetch_error", [this]() {
                fetch_guard_.release();
                std::snprintf(status_buf_, sizeof(status_buf_), "%s",
                              lv_tr("Failed to load history"));
                lv_subject_copy_string(&status_subject_, status_buf_);
                update_visibility();
            });
        });
}

void ConsolePanel::populate_entries(const std::vector<GcodeEntry>& entries) {
    clear_entries();

    // Keep only the most recent MAX_ENTRIES entries (input is oldest-first)
    size_t start = (entries.size() > MAX_ENTRIES) ? entries.size() - MAX_ENTRIES : 0;
    for (size_t i = start; i < entries.size(); i++) {
        entries_.push_back(entries[i]);
        // Stamp identity here rather than at the call site: this is the only other
        // door into entries_ besides add_entry(), and an unstamped entry is silently
        // untappable rather than loudly broken.
        entries_.back().id = next_entry_id_++;
    }

    for (const auto& entry : entries_) {
        create_entry_widget(entry);
    }

    // Clear "Loading..." status — fetch succeeded regardless of entry count
    status_buf_[0] = '\0';

    update_visibility();
    scroll_to_bottom();
}

void ConsolePanel::create_entry_widget(const GcodeEntry& entry) {
    if (!console_container_) {
        return;
    }

    const lv_font_t* font = theme_manager_get_font("font_mono");
    if (!font) {
        font = theme_manager_get_font("font_small");
    }

    const bool is_command = (entry.type == GcodeEntry::Type::COMMAND);

    // Color based on entry type: errors red, responses green, commands default
    auto entry_color = [&]() -> lv_color_t {
        if (entry.is_error) {
            return theme_manager_get_color("danger");
        }
        if (entry.type == GcodeEntry::Type::RESPONSE) {
            return theme_manager_get_color("success");
        }
        return theme_manager_get_color("text");
    };

    // Resolve a span color class name to a theme color, falling back to entry color
    auto resolve_span_color = [&](const std::string& color_class) -> lv_color_t {
        // "error" class maps to "danger" theme token; others map directly
        if (color_class == "error") {
            return theme_manager_get_color("danger");
        }
        if (!color_class.empty()) {
            return theme_manager_get_color(color_class.c_str());
        }
        return entry_color();
    };

    bool has_html = contains_html_spans(entry.message);

    // Tap-to-paste: a sent command can be tapped to refill the input field.
    // Applied to BOTH render paths below — show_timestamps_ and HTML spans decide
    // which one runs at runtime, so wiring only one silently breaks the feature
    // when a user toggles timestamps or firmware emits coloured output.
    //
    // lv_obj_add_event_cb() rather than a declarative XML <event_cb> because these
    // are streaming log lines: variable count, two render shapes, up to MAX_ENTRIES
    // of them, rebuilt as responses arrive. <repeat> would rebuild the whole list
    // on every incoming line.
    auto make_tappable = [&](lv_obj_t* widget) {
        if (!is_command || entry.id == 0) {
            return; // Responses stay inert; an unstamped entry can't be resolved.
        }
        lv_obj_add_flag(widget, LV_OBJ_FLAG_CLICKABLE);
        // Identity travels as an integer VALUE, not a pointer. Nothing to free and
        // nothing to dangle when this widget is pruned — a recycled widget address
        // carrying a stale pointer is the #924/#1111 footgun.
        lv_obj_set_user_data(widget, reinterpret_cast<void*>(static_cast<uintptr_t>(entry.id)));
        lv_obj_add_event_cb(widget, on_entry_clicked, LV_EVENT_CLICKED, nullptr);
        // Touch has no hover, so the pressed state is the only tap confirmation.
        lv_obj_set_style_bg_color(widget, theme_manager_get_color("card_bg"), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(widget, LV_OPA_COVER, LV_STATE_PRESSED);
    };

    // Use spangroup when timestamps or HTML spans need mixed colors
    if (show_timestamps_ || has_html) {
        lv_obj_t* spangroup = lv_spangroup_create(console_container_);
        lv_obj_set_width(spangroup, LV_PCT(100));
        lv_obj_set_style_text_font(spangroup, font, 0);

        // Helper to add a colored span
        auto add_span = [&](const char* text, lv_color_t color) {
            lv_span_t* span = lv_spangroup_add_span(spangroup);
            lv_span_set_text(span, text);
            lv_style_set_text_color(lv_span_get_style(span), color);
        };

        if (show_timestamps_) {
            std::string ts = format_timestamp(entry.timestamp);
            add_span(ts.c_str(), theme_manager_get_color("text_muted"));
        }

        if (is_command) {
            add_span("> ", theme_manager_get_color("text"));
        }

        if (has_html) {
            for (const auto& seg : parse_html_spans(entry.message)) {
                add_span(seg.text.c_str(), resolve_span_color(seg.color_class));
            }
        } else {
            add_span(entry.message.c_str(), entry_color());
        }

        lv_spangroup_refresh(spangroup);
        make_tappable(spangroup);
    } else {
        // No timestamps, no HTML: plain label (fastest path)
        lv_obj_t* label = lv_label_create(console_container_);
        if (is_command) {
            std::string display_text = "> " + entry.message;
            lv_label_set_text(label, display_text.c_str());
        } else {
            lv_label_set_text(label, entry.message.c_str());
        }
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_style_text_color(label, entry_color(), 0);
        lv_obj_set_style_text_font(label, font, 0);
        make_tappable(label);
    }
}

const ConsolePanel::GcodeEntry*
ConsolePanel::find_entry_by_id(const std::deque<GcodeEntry>& entries, uint64_t id) {
    if (id == 0) {
        return nullptr; // Reserved for "never stamped" — must never match.
    }
    for (const auto& entry : entries) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

void ConsolePanel::paste_entry(uint64_t id) {
    if (!gcode_input_) {
        return;
    }

    const GcodeEntry* entry = find_entry_by_id(entries_, id);
    if (!entry) {
        // Normal outcome when a tap races a burst of responses that pruned the
        // entry out from under the widget. Leave the input alone.
        spdlog::debug("[{}] Tapped entry {} no longer in buffer, ignoring", get_name(), id);
        return;
    }

    // entry->message is the bare command; the "> " prefix is added at render time.
    lv_textarea_set_text(gcode_input_, entry->message.c_str());

    // Neutralise the readline browse cursor. Without this a later Up-arrow resumes
    // a stale walk from wherever history_index_ was left, and a Down-arrow past the
    // newest restores a saved_input_ that no longer relates to anything on screen.
    history_index_ = -1;
    saved_input_.clear();

    spdlog::info("[{}] Pasted command to input: {}", get_name(), entry->message);
}

void ConsolePanel::on_entry_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[Console] on_entry_clicked");

    auto* widget = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (widget) {
        auto id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(widget)));
        get_global_console_panel().paste_entry(id);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void ConsolePanel::clear_entries() {
    entries_.clear();

    if (!console_container_) {
        return;
    }

    // Delete all children except the empty_state_ widget (which lives inside
    // the container to share its dark background).
    // Re-parent the empty state, clean the container, then re-parent it back.
    // This avoids synchronous lv_obj_delete during child list iteration which
    // corrupts LVGL's event linked list (lv_event_mark_deleted crashes).
    if (empty_state_) {
        lv_obj_set_parent(empty_state_, lv_obj_get_parent(console_container_));
        lv_obj_add_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
    }
    helix::ui::safe_clean_children(console_container_);
    if (empty_state_) {
        lv_obj_set_parent(empty_state_, console_container_);
        lv_obj_remove_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ConsolePanel::scroll_to_bottom() {
    if (console_container_) {
        user_scrolled_up_ = false;
        lv_obj_scroll_to_y(console_container_, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

bool ConsolePanel::is_error_message(const std::string& message) {
    if (message.size() >= 2 && message[0] == '!' && message[1] == '!') {
        return true;
    }

    // Case-insensitive check for "error" at start (covers "Error:", "ERROR:", etc.)
    if (message.size() >= 5) {
        auto ci_eq = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        };
        return std::equal(message.begin(), message.begin() + 5, "error", ci_eq);
    }

    return false;
}

bool ConsolePanel::is_temp_message(const std::string& message) {
    if (message.empty()) {
        return false;
    }

    // Temperature status messages look like:
    // "ok T:210.5 /210.0 B:60.2 /60.0"
    // "T:210.5 /210.0 B:60.2 /60.0"
    // "ok B:60.0 /60.0 T0:210.0 /210.0"

    // Check for "T:" or "B:" followed immediately by a digit, with "/" somewhere after
    size_t t_pos = message.find("T:");
    size_t b_pos = message.find("B:");

    auto check_temp_pattern = [&](size_t pos) -> bool {
        if (pos == std::string::npos)
            return false;
        // Require digit immediately after the colon (e.g. "T:210" not "T: see docs")
        size_t val_start = pos + 2; // skip "T:" or "B:"
        if (val_start < message.size() &&
            std::isdigit(static_cast<unsigned char>(message[val_start]))) {
            // Also require "/" somewhere after the pattern (target temp separator)
            size_t slash_pos = message.find('/', val_start);
            return slash_pos != std::string::npos;
        }
        return false;
    };

    return check_temp_pattern(t_pos) || check_temp_pattern(b_pos);
}

void ConsolePanel::update_visibility() {
    // Drive visibility via subjects — XML bindings handle show/hide
    lv_subject_set_int(&has_entries_subject_, entries_.empty() ? 0 : 1);
    lv_subject_copy_string(&status_subject_, status_buf_);
    lv_subject_set_int(&status_visible_subject_, status_buf_[0] != '\0' ? 1 : 0);
}

// ============================================================================
// Real-time G-code Response Streaming
// ============================================================================

void ConsolePanel::subscribe_to_gcode_responses() {
    if (is_subscribed_) {
        return;
    }

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::debug("[{}] Cannot subscribe - no API", get_name());
        return;
    }

    // Generate unique handler name
    static std::atomic<uint64_t> s_handler_id{0};
    gcode_handler_name_ = "console_panel_" + std::to_string(++s_handler_id);

    // Register for notify_gcode_response notifications
    // Guard with lifetime token in case panel is destroyed before unsubscribe
    api->register_method_callback("notify_gcode_response", gcode_handler_name_,
                                  [this, token = lifetime_.token()](const nlohmann::json& msg) {
                                      if (token.expired())
                                          return;
                                      on_gcode_response(msg);
                                  });

    is_subscribed_ = true;
    spdlog::debug("[{}] Subscribed to notify_gcode_response (handler: {})", get_name(),
                  gcode_handler_name_);
}

void ConsolePanel::unsubscribe_from_gcode_responses() {
    if (!is_subscribed_) {
        return;
    }

    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        api->unregister_method_callback("notify_gcode_response", gcode_handler_name_);
        spdlog::debug("[{}] Unsubscribed from notify_gcode_response", get_name());
    }

    is_subscribed_ = false;
    gcode_handler_name_.clear();
}

void ConsolePanel::on_gcode_response(const nlohmann::json& msg) {
    // Parse notify_gcode_response format: {"method": "...", "params": ["line"]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    // I4: Type-check params[0] before extracting string reference
    if (!msg["params"][0].is_string()) {
        return;
    }
    const std::string& line = msg["params"][0].get_ref<const std::string&>();

    // Skip empty lines and common noise
    if (line.empty() || line == "ok") {
        return;
    }

    // Build entry on background thread (no LVGL calls, only pure C++).
    // is_temp_message / is_error_message are pure functions, safe on any thread.
    const bool is_temp = is_temp_message(line);

    GcodeEntry entry;
    entry.message = line;
    entry.timestamp = 0.0; // Real-time entries don't have timestamps
    entry.type = GcodeEntry::Type::RESPONSE;
    entry.is_error = is_error_message(line);

    // CRITICAL: Defer LVGL operations to main thread via token.defer
    // WebSocket callbacks run on libhv thread - direct LVGL calls cause crashes.
    // Use token.defer() (not lifetime_.defer()) to avoid TOCTOU race (#707).
    // Filtering decisions also happen on the main thread so the engine's pattern
    // vector is mutated and read on the same thread (no lock required).
    auto tok = lifetime_.token();
    tok.defer("ConsolePanel::gcode_entry", [this, entry = std::move(entry), is_temp]() {
        if (filter_temps_ && is_temp) {
            return;
        }
        if (filter_firmware_noise_ && firmware_filter_.should_filter(entry.message)) {
            return;
        }
        add_entry(entry);
    });
}

void ConsolePanel::add_entry(const GcodeEntry& entry) {
    // Add to deque
    entries_.push_back(entry);
    entries_.back().id = next_entry_id_++;

    // Enforce max size (remove oldest)
    while (entries_.size() > MAX_ENTRIES && console_container_) {
        entries_.pop_front();
        // Remove oldest entry widget (first child that isn't empty_state_)
        lv_obj_t* first_child = lv_obj_get_child(console_container_, 0);
        if (first_child == empty_state_) {
            first_child = lv_obj_get_child(console_container_, 1);
        }
        helix::ui::safe_delete(first_child);
    }

    // Create widget for the STORED entry, not the caller's copy — the caller's still
    // has id 0 and would build an untappable widget. Pruning above only pops the
    // front, so back() is still the entry just added.
    create_entry_widget(entries_.back());

    // Update visibility state
    update_visibility();

    // Smart auto-scroll: only scroll if user hasn't scrolled up manually
    if (!user_scrolled_up_) {
        scroll_to_bottom();
    }
}

void ConsolePanel::send_gcode_command() {
    if (!gcode_input_) {
        spdlog::warn("[{}] Cannot send - no input field", get_name());
        return;
    }

    // Get text from input
    const char* text = lv_textarea_get_text(gcode_input_);
    if (!text || text[0] == '\0') {
        spdlog::debug("[{}] Empty command, ignoring", get_name());
        return;
    }

    std::string command(text);
    spdlog::info("[{}] Sending G-code: {}", get_name(), command);

    // Add to command history (newest first) and reset browsing state
    command_history_.push_front(command);
    if (command_history_.size() > MAX_HISTORY) {
        command_history_.pop_back();
    }
    history_index_ = -1;
    saved_input_.clear();

    // Clear the input field immediately
    lv_textarea_set_text(gcode_input_, "");

    // Add command to console display
    GcodeEntry cmd_entry;
    cmd_entry.message = command;
    cmd_entry.timestamp = 0.0;
    cmd_entry.type = GcodeEntry::Type::COMMAND;
    cmd_entry.is_error = false;
    add_entry(cmd_entry);

    // Send via IMoonrakerAPI with error feedback
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        api->execute_gcode(command, nullptr, // success: no-op, response comes via WS subscription
                           [token = lifetime_.token()](const MoonrakerError& err) {
                               if (token.expired())
                                   return;
                               NOTIFY_ERROR(lv_tr("Failed to send command: {}"), err.message);
                           });
    } else {
        spdlog::warn("[{}] No IMoonrakerAPI available", get_name());
    }
}

void ConsolePanel::clear_display() {
    spdlog::debug("[{}] Clearing console display", get_name());
    clear_entries();
    update_visibility();
}
