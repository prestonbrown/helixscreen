// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "favorite_macro_widget.h"

#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "device_display_name.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_param_modal.h"
#include "moonraker_api.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <regex>
#include <set>

namespace helix {
void register_favorite_macro_widgets() {
    register_widget_factory("favorite_macro_1", []() {
        return std::make_unique<FavoriteMacroWidget>("favorite_macro_1");
    });
    register_widget_factory("favorite_macro_2", []() {
        return std::make_unique<FavoriteMacroWidget>("favorite_macro_2");
    });
}
} // namespace helix

namespace {
// File-local helper: get the shared PanelWidgetConfig instance
helix::PanelWidgetConfig& get_widget_config_ref() {
    static helix::PanelWidgetConfig config("home", *helix::Config::get_instance());
    config.load();
    return config;
}
// File-local helper: single shared MacroParamModal instance.
// Using one instance avoids s_active_instance_ stomping when two widget slots
// both try to open param modals (the old code had two separate static locals).
helix::MacroParamModal& get_shared_param_modal() {
    static helix::MacroParamModal modal;
    return modal;
}
} // namespace

using namespace helix;

// ============================================================================
// parse_macro_params — extract Klipper gcode_macro parameters from template
// ============================================================================

std::vector<MacroParam> helix::parse_macro_params(const std::string& gcode_template) {
    std::vector<MacroParam> result;
    std::set<std::string> seen;

    // Match params.NAME, params['NAME'], params["NAME"]
    // Optional trailing |default(VALUE) or | default(VALUE)
    std::regex param_re(
        R"RE(params\.([A-Za-z_][A-Za-z0-9_]*)|params\['([A-Za-z_][A-Za-z0-9_]*)'\]|params\["([A-Za-z_][A-Za-z0-9_]*)"\])RE");

    auto it = std::sregex_iterator(gcode_template.begin(), gcode_template.end(), param_re);
    auto end = std::sregex_iterator();

    for (; it != end; ++it) {
        const auto& match = *it;

        // Extract name from whichever group matched
        std::string name;
        if (match[1].matched)
            name = match[1].str();
        else if (match[2].matched)
            name = match[2].str();
        else if (match[3].matched)
            name = match[3].str();

        // Normalize to uppercase
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        // Skip duplicates
        if (seen.count(name)) {
            continue;
        }
        seen.insert(name);

        // Try to extract |default(VALUE) after the match
        std::string default_value;
        auto suffix_start = match.suffix().first;
        auto suffix_end = gcode_template.cend();
        std::string suffix(suffix_start, suffix_end);

        // Look for |default(...) or | default(...) immediately after
        std::regex default_re(R"(^\s*\|\s*default\(([^)]*)\))");
        std::smatch default_match;
        if (std::regex_search(suffix, default_match, default_re)) {
            default_value = default_match[1].str();
            // Strip surrounding quotes from string defaults
            if (default_value.size() >= 2) {
                char first = default_value.front();
                char last = default_value.back();
                if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
                    default_value = default_value.substr(1, default_value.size() - 2);
                }
            }
        }

        result.push_back({name, default_value});
    }

    return result;
}

// ============================================================================
// FavoriteMacroWidget
// ============================================================================

FavoriteMacroWidget* FavoriteMacroWidget::s_active_picker_ = nullptr;

FavoriteMacroWidget::FavoriteMacroWidget(const std::string& widget_id) : widget_id_(widget_id) {}

FavoriteMacroWidget::~FavoriteMacroWidget() {
    detach();
}

void FavoriteMacroWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);
    }

    // Register XML event callbacks
    lv_xml_register_event_cb(nullptr, "favorite_macro_1_clicked_cb", clicked_1_cb);
    lv_xml_register_event_cb(nullptr, "favorite_macro_1_long_press_cb", long_press_1_cb);
    lv_xml_register_event_cb(nullptr, "favorite_macro_2_clicked_cb", clicked_2_cb);
    lv_xml_register_event_cb(nullptr, "favorite_macro_2_long_press_cb", long_press_2_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_picker_backdrop_cb", picker_backdrop_cb);

    // Cache label pointers from XML
    icon_label_ = lv_obj_find_by_name(widget_obj_, "fav_macro_icon");
    name_label_ = lv_obj_find_by_name(widget_obj_, "fav_macro_name");

    // Load saved macro from config
    load_config();
    update_display();

    spdlog::debug("[FavoriteMacroWidget] Attached {} (macro: {})", widget_id_,
                  macro_name_.empty() ? "none" : macro_name_);
}

void FavoriteMacroWidget::detach() {
    *alive_ = false;
    dismiss_macro_picker();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    icon_label_ = nullptr;
    name_label_ = nullptr;

    spdlog::debug("[FavoriteMacroWidget] Detached");
}

void FavoriteMacroWidget::handle_clicked() {
    if (macro_name_.empty()) {
        // No macro assigned — open picker to configure
        spdlog::info("[FavoriteMacroWidget] {} clicked (unconfigured) - showing picker",
                     widget_id_);
        show_macro_picker();
    } else {
        // Execute assigned macro
        spdlog::info("[FavoriteMacroWidget] {} clicked - executing {}", widget_id_, macro_name_);
        fetch_and_execute();
    }
}

void FavoriteMacroWidget::handle_long_press() {
    spdlog::info("[FavoriteMacroWidget] {} long-pressed - showing picker", widget_id_);
    show_macro_picker();
}

MoonrakerAPI* FavoriteMacroWidget::get_api() const {
    return get_moonraker_api();
}

void FavoriteMacroWidget::update_display() {
    if (name_label_) {
        if (macro_name_.empty()) {
            lv_label_set_text(name_label_, lv_tr("Configure"));
        } else {
            std::string display = helix::get_display_name(macro_name_, helix::DeviceType::MACRO);
            lv_label_set_text(name_label_, display.c_str());
        }
    }

    if (icon_label_) {
        // Show play icon when configured, settings icon when not
        const char* icon_name = macro_name_.empty() ? "cog" : "play";
        const char* codepoint = ui_icon::lookup_codepoint(icon_name);
        if (codepoint) {
            lv_label_set_text(icon_label_, codepoint);
        }
    }
}

void FavoriteMacroWidget::load_config() {
    auto& wc = get_widget_config_ref();
    auto config = wc.get_widget_config(widget_id_);
    if (config.contains("macro") && config["macro"].is_string()) {
        macro_name_ = config["macro"].get<std::string>();
        spdlog::debug("[FavoriteMacroWidget] Loaded config: {}={}", widget_id_, macro_name_);
    }
}

void FavoriteMacroWidget::save_config() {
    nlohmann::json config;
    config["macro"] = macro_name_;

    auto& wc = get_widget_config_ref();
    wc.set_widget_config(widget_id_, config);

    spdlog::debug("[FavoriteMacroWidget] Saved config: {}={}", widget_id_, macro_name_);
}

void FavoriteMacroWidget::fetch_and_execute() {
    MoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[FavoriteMacroWidget] No API available");
        return;
    }

    // If params are already cached, use them directly
    if (params_cached_) {
        if (cached_params_.empty()) {
            // No params — execute directly
            execute_with_params({});
        } else if (parent_screen_) {
            // Has params — show modal (guard against widget destruction while modal is open)
            std::weak_ptr<bool> weak_alive = alive_;
            get_shared_param_modal().show_for_macro(
                parent_screen_, macro_name_, cached_params_,
                [this, weak_alive](const std::map<std::string, std::string>& values) {
                    if (weak_alive.expired())
                        return;
                    execute_with_params(values);
                });
        }
        return;
    }

    // Query macro template to detect parameters
    std::string object_name = "gcode_macro " + macro_name_;
    nlohmann::json params;
    params["objects"] = nlohmann::json::object();
    params["objects"][object_name] = nlohmann::json::array({"gcode"});

    std::weak_ptr<bool> weak_alive = alive_;
    std::string macro_name_copy = macro_name_;

    api->get_client().send_jsonrpc(
        "printer.objects.query", params,
        [this, weak_alive, macro_name_copy, object_name](nlohmann::json response) {
            if (weak_alive.expired())
                return;

            std::string gcode_template;
            try {
                if (response.contains("result") && response["result"].contains("status") &&
                    response["result"]["status"].contains(object_name) &&
                    response["result"]["status"][object_name].contains("gcode")) {
                    gcode_template =
                        response["result"]["status"][object_name]["gcode"].get<std::string>();
                }
            } catch (const std::exception& e) {
                spdlog::warn("[FavoriteMacroWidget] Failed to parse template for {}: {}",
                             macro_name_copy, e.what());
            }

            // Parse and cache params (on UI thread via queue)
            auto parsed = parse_macro_params(gcode_template);
            ui::queue_update(
                [this, weak_alive, parsed = std::move(parsed), macro_name_copy]() mutable {
                    if (weak_alive.expired())
                        return;

                    cached_params_ = std::move(parsed);
                    params_cached_ = true;

                    spdlog::debug("[FavoriteMacroWidget] Cached {} params for {}",
                                  cached_params_.size(), macro_name_copy);

                    if (cached_params_.empty()) {
                        execute_with_params({});
                    } else if (parent_screen_) {
                        // Guard against widget destruction while modal is open
                        get_shared_param_modal().show_for_macro(
                            parent_screen_, macro_name_, cached_params_,
                            [this, weak_alive](const std::map<std::string, std::string>& values) {
                                if (weak_alive.expired())
                                    return;
                                execute_with_params(values);
                            });
                    }
                });
        },
        [macro_name_copy](const MoonrakerError& err) {
            spdlog::warn("[FavoriteMacroWidget] Failed to query template for {}: {}",
                         macro_name_copy, err.message);
        });
}

void FavoriteMacroWidget::execute_with_params(const std::map<std::string, std::string>& params) {
    MoonrakerAPI* api = get_api();
    if (!api) {
        return;
    }

    // Build gcode command: MACRO_NAME PARAM1=value1 PARAM2=value2
    std::string gcode = macro_name_;
    for (const auto& [key, value] : params) {
        gcode += " " + key + "=" + value;
    }

    spdlog::info("[FavoriteMacroWidget] Executing: {}", gcode);

    // Capture copies — callbacks fire from WebSocket thread after widget may be destroyed
    std::string macro_name_copy = macro_name_;
    api->execute_gcode(
        gcode,
        [macro_name_copy]() {
            spdlog::info("[FavoriteMacroWidget] {} executed successfully", macro_name_copy);
        },
        [macro_name_copy](const MoonrakerError& err) {
            spdlog::error("[FavoriteMacroWidget] {} failed: {}", macro_name_copy, err.message);
        });
}

void FavoriteMacroWidget::show_macro_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    // Dismiss any other widget's picker before opening ours
    if (s_active_picker_ && s_active_picker_ != this) {
        s_active_picker_->dismiss_macro_picker();
    }

    MoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[FavoriteMacroWidget] No API available for macro picker");
        return;
    }

    const auto& macros = api->hardware().macros();
    if (macros.empty()) {
        spdlog::warn("[FavoriteMacroWidget] No macros available");
        return;
    }

    // Create picker from XML
    picker_backdrop_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "favorite_macro_picker", nullptr));
    if (!picker_backdrop_) {
        spdlog::error("[FavoriteMacroWidget] Failed to create picker from XML");
        return;
    }

    // Find the macro_list container
    lv_obj_t* macro_list = lv_obj_find_by_name(picker_backdrop_, "macro_list");
    if (!macro_list) {
        spdlog::error("[FavoriteMacroWidget] macro_list not found in picker XML");
        helix::ui::safe_delete(picker_backdrop_);
        return;
    }

    // Resolve responsive spacing tokens
    auto get_token = [](const char* name, int fallback) {
        const char* s = lv_xml_get_const(nullptr, name);
        return s ? std::atoi(s) : fallback;
    };
    int space_xs = get_token("space_xs", 4);
    int space_sm = get_token("space_sm", 6);
    int space_md = get_token("space_md", 10);

    // Cap list height at 2/3 of screen
    int screen_h = lv_obj_get_height(parent_screen_);
    lv_obj_set_style_max_height(macro_list, screen_h * 2 / 3, 0);

    // Sort macros alphabetically for display
    std::vector<std::string> sorted_macros(macros.begin(), macros.end());
    std::sort(sorted_macros.begin(), sorted_macros.end());

    // Populate macro rows
    for (const auto& macro : sorted_macros) {
        bool is_selected = (macro == macro_name_);
        std::string display = helix::get_display_name(macro, helix::DeviceType::MACRO);

        lv_obj_t* row = lv_obj_create(macro_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // Highlight selected row
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);

        // Macro display name
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, display.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);

        // Store macro name for click handler
        auto* macro_name_copy = new std::string(macro);
        lv_obj_set_user_data(row, macro_name_copy);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] macro_row_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
                if (!name_ptr)
                    return;

                if (FavoriteMacroWidget::s_active_picker_) {
                    std::string selected = *name_ptr;
                    FavoriteMacroWidget::s_active_picker_->select_macro(selected);
                    FavoriteMacroWidget::s_active_picker_->dismiss_macro_picker();
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    s_active_picker_ = this;

    // Self-clearing delete callback — if LVGL deletes picker_backdrop_ via parent
    // deletion (e.g., user navigates away), clear our pointer to prevent dangling access
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* e) {
            auto* self = static_cast<FavoriteMacroWidget*>(lv_event_get_user_data(e));
            if (self) {
                // Clean up heap-allocated strings before LVGL frees the tree
                lv_obj_t* backdrop = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                lv_obj_t* macro_list = lv_obj_find_by_name(backdrop, "macro_list");
                if (macro_list) {
                    uint32_t count = lv_obj_get_child_count(macro_list);
                    for (uint32_t i = 0; i < count; ++i) {
                        lv_obj_t* row = lv_obj_get_child(macro_list, i);
                        auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
                        delete name_ptr;
                        lv_obj_set_user_data(row, nullptr);
                    }
                }
                self->picker_backdrop_ = nullptr;
                if (s_active_picker_ == self) {
                    s_active_picker_ = nullptr;
                }
            }
        },
        LV_EVENT_DELETE, this);

    // Position the context menu card near the widget
    lv_obj_t* card = lv_obj_find_by_name(picker_backdrop_, "context_menu");
    if (card && widget_obj_) {
        int screen_w = lv_obj_get_width(parent_screen_);

        lv_area_t widget_area;
        lv_obj_get_coords(widget_obj_, &widget_area);

        int card_w = std::clamp(screen_w * 3 / 10, 160, 240);
        lv_obj_set_width(card, card_w);
        int card_x = (widget_area.x1 + widget_area.x2) / 2 - card_w / 2;
        int card_y = widget_area.y2 + space_xs;
        int max_card_h = screen_h * 2 / 3;

        // Clamp to screen bounds
        if (card_x < space_md)
            card_x = space_md;
        if (card_x + card_w > screen_w - space_md)
            card_x = screen_w - card_w - space_md;
        if (card_y + max_card_h > screen_h - space_md) {
            card_y = widget_area.y1 - max_card_h - space_xs;
            if (card_y < space_md)
                card_y = space_md;
        }

        lv_obj_set_pos(card, card_x, card_y);
    }

    spdlog::debug("[FavoriteMacroWidget] Picker shown with {} macros", sorted_macros.size());
}

void FavoriteMacroWidget::dismiss_macro_picker() {
    if (!picker_backdrop_) {
        return;
    }

    // Clean up heap-allocated macro name strings (only if object is still valid —
    // parent screen deletion auto-frees children, leaving stale pointers)
    if (lv_obj_is_valid(picker_backdrop_)) {
        lv_obj_t* macro_list = lv_obj_find_by_name(picker_backdrop_, "macro_list");
        if (macro_list) {
            uint32_t count = lv_obj_get_child_count(macro_list);
            for (uint32_t i = 0; i < count; ++i) {
                lv_obj_t* row = lv_obj_get_child(macro_list, i);
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
                delete name_ptr;
                lv_obj_set_user_data(row, nullptr);
            }
        }
    }

    helix::ui::safe_delete(picker_backdrop_);
    s_active_picker_ = nullptr;

    spdlog::debug("[FavoriteMacroWidget] Picker dismissed");
}

void FavoriteMacroWidget::select_macro(const std::string& name) {
    macro_name_ = name;
    params_cached_ = false; // Invalidate param cache for new macro
    cached_params_.clear();

    update_display();
    save_config();

    spdlog::info("[FavoriteMacroWidget] {} selected macro: {}", widget_id_, name);
}

// ============================================================================
// Static event callbacks
// ============================================================================

void FavoriteMacroWidget::clicked_1_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] clicked_1_cb");
    auto* widget = panel_widget_from_event<FavoriteMacroWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroWidget::long_press_1_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] long_press_1_cb");
    auto* widget = panel_widget_from_event<FavoriteMacroWidget>(e);
    if (widget) {
        widget->handle_long_press();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroWidget::clicked_2_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] clicked_2_cb");
    auto* widget = panel_widget_from_event<FavoriteMacroWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroWidget::long_press_2_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] long_press_2_cb");
    auto* widget = panel_widget_from_event<FavoriteMacroWidget>(e);
    if (widget) {
        widget->handle_long_press();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroWidget::picker_backdrop_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] picker_backdrop_cb");
    (void)e;
    if (s_active_picker_) {
        s_active_picker_->dismiss_macro_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}
