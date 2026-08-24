// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_stack_widget.h"

#include "ui_carousel.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fan_arc_resize.h"
#include "ui_fan_control_overlay.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_fan_state.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "ui/fan_spin_animation.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {
void register_fan_stack_widget() {
    register_widget_factory("fan_stack", [](const std::string& instance_id) {
        auto& ps = get_printer_state();
        return std::make_unique<FanStackWidget>(instance_id, ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "on_fan_stack_clicked", FanStackWidget::on_fan_stack_clicked);
}
} // namespace helix

namespace {

// Fan-related icons available in the font
static const char* const FAN_ICONS[] = {
    // clang-format off
    "fan",       "fan_off",    "cooldown",   "heat_wave",
    // clang-format on
};
static constexpr size_t FAN_ICON_COUNT = std::size(FAN_ICONS);
static constexpr int ICON_CELL_SIZE = 36;
static constexpr const char* DEFAULT_FAN_ICON = "fan";

/// Resolve a responsive spacing token to pixels, with a fallback.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// Apply highlight styling to an icon grid cell.
void apply_icon_cell_highlight(lv_obj_t* cell, bool selected) {
    if (selected) {
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, theme_manager_get_color("primary"), 0);
        lv_obj_set_style_bg_opa(cell, 20, 0);
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("primary"), 0);
    } else {
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, 0, 0);
    }
}

} // namespace

using namespace helix;

FanStackWidget::FanStackWidget(const std::string& instance_id, PrinterState& printer_state)
    : instance_id_(instance_id), printer_state_(printer_state) {}

FanStackWidget::~FanStackWidget() {
    detach();
}

void FanStackWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    if (config.contains("fan") && config["fan"].is_string()) {
        selected_fan_ = config["fan"].get<std::string>();
    }
    if (config.contains("icon") && config["icon"].is_string()) {
        icon_name_ = config["icon"].get<std::string>();
    }
    spdlog::debug("[FanStackWidget] Config: {} fan={} icon={}", instance_id_,
                  selected_fan_.empty() ? "(auto)" : selected_fan_,
                  icon_name_.empty() ? "fan (default)" : icon_name_);
}

std::string FanStackWidget::get_component_name() const {
    if (is_carousel_mode()) {
        return "panel_widget_fan_carousel";
    }
    return "panel_widget_fan_stack";
}

bool FanStackWidget::on_edit_configure() {
    spdlog::info("[FanStackWidget] {} configure requested - showing picker", instance_id_);
    show_fan_picker();
    return false;
}

bool FanStackWidget::is_carousel_mode() const {
    if (config_.contains("display_mode") && config_["display_mode"].is_string()) {
        return config_["display_mode"].get<std::string>() == "carousel";
    }
    return false;
}

void FanStackWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    lv_obj_set_user_data(widget_obj_, this);

    // Pressed feedback: dim widget on touch
    lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);

    if (is_carousel_mode()) {
        attach_carousel(widget_obj);
    } else {
        attach_stack(widget_obj);
    }
}

void FanStackWidget::attach_stack(lv_obj_t* /*widget_obj*/) {
    // Cache label, name, and icon pointers
    part_label_ = lv_obj_find_by_name(widget_obj_, "fan_stack_part_speed");
    hotend_label_ = lv_obj_find_by_name(widget_obj_, "fan_stack_hotend_speed");
    aux_label_ = lv_obj_find_by_name(widget_obj_, "fan_stack_aux_speed");
    aux_row_ = lv_obj_find_by_name(widget_obj_, "fan_stack_aux_row");
    part_icon_ = lv_obj_find_by_name(widget_obj_, "fan_stack_part_icon");
    hotend_icon_ = lv_obj_find_by_name(widget_obj_, "fan_stack_hotend_icon");
    aux_icon_ = lv_obj_find_by_name(widget_obj_, "fan_stack_aux_icon");

    // Set initial text — text_small is a registered widget so XML inner content
    // isn't reliably applied. Observers update with real values on next tick.
    for (auto* label : {part_label_, hotend_label_, aux_label_}) {
        if (label)
            lv_label_set_text(label, "0%");
    }

    // Set rotation pivots on icons (center of 16px icon)
    for (auto* icon : {part_icon_, hotend_icon_, aux_icon_})
        set_icon_pivot(icon);

    setup_common_observers([this]() { refresh_all_animations(); }, [this]() { bind_fans(); });
    bind_fans();

    spdlog::debug("[FanStackWidget] Attached stack (animations={})", animations_enabled_);
}

void FanStackWidget::attach_carousel(lv_obj_t* widget_obj) {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj, "fan_carousel");
    if (!carousel) {
        spdlog::error("[FanStackWidget] Could not find fan_carousel in XML");
        return;
    }

    setup_common_observers(
        [this]() {
            for (auto& page : carousel_pages_)
                update_fan_animation(page.fan_icon, page.arc ? lv_arc_get_value(page.arc) : 0);
        },
        [this]() { bind_carousel_fans(); });
    bind_carousel_fans();

    spdlog::debug("[FanStackWidget] Attached carousel");
}

void FanStackWidget::detach() {
    lifetime_.invalidate();
    picker_.hide();

    // Freeze queue, drain pending deferred callbacks, THEN tear down observers
    // and animations. Without the freeze, the WebSocket thread can enqueue new
    // callbacks between drain() and pointer cleanup → use-after-free.
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();

        part_observer_.reset();
        hotend_observer_.reset();
        aux_observer_.reset();
        version_observer_.reset();
        anim_settings_observer_.reset();
        carousel_observers_.clear();

        // Cancel running animations — just delete the anim, don't touch
        // the object's style (it's about to be destroyed by lv_obj_clean).
        for (auto* icon : {part_icon_, hotend_icon_, aux_icon_})
            if (icon)
                lv_anim_delete(icon, helix::ui::fan_spin_anim_cb);
        for (auto& page : carousel_pages_)
            if (page.fan_icon)
                lv_anim_delete(page.fan_icon, helix::ui::fan_spin_anim_cb);
        carousel_pages_.clear();
    }

    if (widget_obj_)
        lv_obj_set_user_data(widget_obj_, nullptr);
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    fan_control_panel_ = nullptr;
    part_label_ = nullptr;
    hotend_label_ = nullptr;
    aux_label_ = nullptr;
    aux_row_ = nullptr;
    part_icon_ = nullptr;
    hotend_icon_ = nullptr;
    aux_icon_ = nullptr;

    spdlog::debug("[FanStackWidget] Detached");
}

void FanStackWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    // Size adaptation only applies to stack mode
    if (!widget_obj_ || is_carousel_mode())
        return;

    // Size tiers, decided from physical pixels rather than grid span so the
    // same authored layout reads correctly on every panel:
    //   compact: xs fonts, single-letter labels (P, H, C)
    //   bigger:  sm fonts, resolved display names from PrinterFanState
    //
    // Width-only: each row is icon + name + speed laid out horizontally
    // (panel_widget_fan_stack.xml), so a resolved name ("Hotend") only ever
    // competes for room along the width axis. Extra height centers the same
    // three rows with more breathing space but never widens one, so height
    // carries no information about whether a resolved name fits.
    //
    // The band is per-tier: what has to fit is text, and font_small runs
    // 10..26px across the tiers, so a width that holds "Part Cooling Fan" at
    // Small does not hold it at XXLarge.
    bool bigger = (width_px >= widget_size::w_normal());

    const char* font_token = bigger ? "font_small" : "font_xs";
    const lv_font_t* text_font = theme_manager_get_font(font_token);
    if (!text_font)
        return;

    // Icon font: xs=16px, sm=24px
    const lv_font_t* icon_font = bigger ? &mdi_icons_24 : &mdi_icons_16;

    // Apply text font to all speed labels
    for (auto* label : {part_label_, hotend_label_, aux_label_}) {
        if (label)
            lv_obj_set_style_text_font(label, text_font, 0);
    }

    // Apply icon font to fan icons. The icon widget (ui_icon_xml_create,
    // ui_icon.cpp) is itself an lv_label — there is no child glyph object to
    // reach through.
    for (auto* icon : {part_icon_, hotend_icon_, aux_icon_}) {
        if (icon)
            lv_obj_set_style_text_font(icon, icon_font, 0);
    }

    // Name labels: 1x1 = single letter, 2x1+ = resolved display name
    struct NameMapping {
        const char* obj_name;
        const char* compact;        // 1x1: single letter
        const std::string* display; // 2x1+: resolved fan display name
        const char* fallback;       // fallback if display name empty
    };
    const NameMapping name_map[] = {
        {"fan_stack_part_name", "P", &part_display_name_, "Part"},
        {"fan_stack_hotend_name", "H", &hotend_display_name_, "Hotend"},
        {"fan_stack_aux_name", "C", &aux_display_name_, "Chamber"},
    };
    for (const auto& m : name_map) {
        lv_obj_t* lbl = lv_obj_find_by_name(widget_obj_, m.obj_name);
        if (lbl) {
            lv_obj_set_style_text_font(lbl, text_font, 0);
            if (bigger) {
                const char* text = m.display->empty() ? m.fallback : m.display->c_str();
                lv_label_set_text(lbl, lv_tr(text));
            } else {
                lv_label_set_text(lbl, lv_tr(m.compact));
            }
        }
    }

    // Center the content block when the widget is wider than 1x.
    // Each row is LV_SIZE_CONTENT so it shrink-wraps its text.
    // Setting cross_place to CENTER on the flex-column parent centers
    // the rows horizontally, but that causes ragged left edges.
    // Instead: keep rows at SIZE_CONTENT and set the parent's
    // cross_place to CENTER — but use a uniform min_width on all rows
    // so they share the same left edge.
    const char* row_names[] = {"fan_stack_part_row", "fan_stack_hotend_row", "fan_stack_aux_row"};
    if (bigger) {
        // First pass: set rows to content width and measure the widest
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row) {
                lv_obj_set_width(row, LV_SIZE_CONTENT);
                lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_START, 0);
            }
        }
        lv_obj_update_layout(widget_obj_);

        int max_w = 0;
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
                int w = lv_obj_get_width(row);
                if (w > max_w)
                    max_w = w;
            }
        }

        // Second pass: set all rows to the same width (widest row)
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row)
                lv_obj_set_width(row, max_w);
        }
    } else {
        // 1x1: center content within each row (labels are short: P, H, C)
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row) {
                lv_obj_set_width(row, LV_PCT(100));
                lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_CENTER, 0);
            }
        }
    }

    spdlog::debug("[FanStackWidget] on_size_changed {}x{} -> font {}", colspan, rowspan,
                  font_token);
}

void FanStackWidget::bind_fans() {
    // Reset existing per-fan observers
    part_observer_.reset();
    hotend_observer_.reset();
    aux_observer_.reset();

    part_fan_name_.clear();
    hotend_fan_name_.clear();
    aux_fan_name_.clear();

    part_speed_ = 0;
    hotend_speed_ = 0;
    aux_speed_ = 0;

    const auto& fans = printer_state_.get_fans();
    if (fans.empty()) {
        spdlog::debug("[FanStackWidget] No fans discovered yet");
        return;
    }

    auto primary = printer_state_.get_fan_state().classify_primary_fans();
    part_fan_name_ = primary.part;
    hotend_fan_name_ = primary.hotend;
    aux_fan_name_ = primary.aux;

    // Resolve display names from the discovered fan list (helper picks by
    // object_name, but the widget still wants the human-readable label).
    part_display_name_.clear();
    hotend_display_name_.clear();
    aux_display_name_.clear();
    for (const auto& fan : fans) {
        if (fan.object_name == part_fan_name_)
            part_display_name_ = fan.display_name;
        else if (fan.object_name == hotend_fan_name_)
            hotend_display_name_ = fan.display_name;
        else if (fan.object_name == aux_fan_name_)
            aux_display_name_ = fan.display_name;
    }

    // Bind part fan
    part_observer_ = bind_fan_observer(part_fan_name_, [this](int speed) {
        part_speed_ = speed;
        update_label(part_label_, speed);
        update_fan_animation(part_icon_, speed);
    });

    // Bind hotend fan
    hotend_observer_ = bind_fan_observer(hotend_fan_name_, [this](int speed) {
        hotend_speed_ = speed;
        update_label(hotend_label_, speed);
        update_fan_animation(hotend_icon_, speed);
    });

    // Bind aux fan (hide row if none)
    if (!aux_fan_name_.empty() && aux_row_)
        lv_obj_remove_flag(aux_row_, LV_OBJ_FLAG_HIDDEN);
    else if (aux_row_)
        lv_obj_add_flag(aux_row_, LV_OBJ_FLAG_HIDDEN);

    aux_observer_ = bind_fan_observer(aux_fan_name_, [this](int speed) {
        aux_speed_ = speed;
        update_label(aux_label_, speed);
        update_fan_animation(aux_icon_, speed);
    });

    spdlog::debug("[FanStackWidget] Bound fans: part='{}' hotend='{}' aux='{}'", part_fan_name_,
                  hotend_fan_name_, aux_fan_name_);
}

void FanStackWidget::bind_carousel_fans() {
    if (!widget_obj_)
        return;

    // Guard against re-entrancy: drain() below can process a pending
    // version_observer_ callback which calls bind_carousel_fans() again.
    // The inner call would create widgets that the outer call then destroys
    // via lv_obj_clean(), leaving stale pointers in carousel_pages_.
    if (rebuilding_carousel_)
        return;
    rebuilding_carousel_ = true;
    ++carousel_gen_;

    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "fan_carousel");
    if (!carousel) {
        rebuilding_carousel_ = false;
        return;
    }

    // Freeze the update queue while tearing down observers and widgets to
    // prevent the WebSocket thread from enqueuing callbacks for destroyed objects.
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        part_observer_.reset();
        hotend_observer_.reset();
        aux_observer_.reset();
        carousel_observers_.clear();

        // Drain pending callbacks BEFORE clearing pages/widgets — pending
        // callbacks may reference carousel_pages_ entries or their LVGL objects.
        // The freeze prevents new callbacks from being enqueued during cleanup.
        helix::ui::UpdateQueue::instance().drain();

        for (auto& page : carousel_pages_) {
            if (page.fan_icon)
                helix::ui::fan_spin_stop(page.fan_icon);
        }
        carousel_pages_.clear();

        // Clear existing carousel pages (the carousel may have pages from a previous bind).
        // Drain BEFORE clean to process any pending callbacks that reference these widgets.
        // The freeze prevents new callbacks from being enqueued during the clean.
        helix::ui::UpdateQueue::instance().drain();
        auto* state_ptr = ui_carousel_get_state(carousel);
        if (state_ptr && state_ptr->scroll_container) {
            helix::ui::safe_clean_children(state_ptr->scroll_container);
            state_ptr->real_tiles.clear();
            ui_carousel_rebuild_indicators(carousel);
        }
    }

    const auto& fans = printer_state_.get_fans();

    // When no fans are discovered yet (e.g. disconnected), use placeholder
    // entries so the carousel still shows arc widgets at 0%.
    struct FanEntry {
        std::string display_name;
        std::string object_name;
        int speed_percent;
        bool is_controllable;
    };
    std::vector<FanEntry> entries;
    if (fans.empty()) {
        entries.push_back({lv_tr("Part"), "", 0, false});
        entries.push_back({"Hotend", "", 0, false});
        spdlog::debug("[FanStackWidget] Carousel: no fans discovered, using placeholders");
    } else {
        for (const auto& fan : fans) {
            std::string short_name = fan.display_name;
            auto pos = short_name.find(" Fan");
            if (pos != std::string::npos && short_name.size() > 4)
                short_name.erase(pos, 4);
            entries.push_back(
                {short_name, fan.object_name, fan.speed_percent, fan.is_controllable});
        }
    }

    const lv_font_t* xs_font = theme_manager_get_font("font_xs");
    lv_color_t text_muted = theme_manager_get_color("text_muted");

    for (const auto& entry : entries) {
        // Thin wrapper page: column layout with arc core + tiny name label
        lv_obj_t* page = lv_obj_create(lv_scr_act());
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_pad_bottom(page, theme_manager_get_spacing("space_lg"), 0);
        lv_obj_set_style_pad_gap(page, 0, 0);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_cross_place(page, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        // Create the core arc widget (no card chrome, no buttons)
        char val_str[16];
        snprintf(val_str, sizeof(val_str), "%d", entry.speed_percent);
        const char* attrs[] = {"initial_value", val_str, nullptr};
        lv_obj_t* arc_core = static_cast<lv_obj_t*>(lv_xml_create(page, "fan_arc_core", attrs));
        if (!arc_core) {
            spdlog::error("[FanStackWidget] lv_xml_create('fan_arc_core') returned NULL for '{}'",
                          entry.display_name);
            lv_obj_delete_async(page);
            continue;
        }

        // XML component root views don't propagate their name attribute to
        // lv_obj_set_name(), but fan_arc_resize_to_fit() needs to find
        // "dial_container" by name. Set it explicitly.
        lv_obj_set_name_static(arc_core, "dial_container");

        // fan_arc_core uses token-based sizing for card contexts; carousel
        // needs it to fill the tile instead.
        lv_obj_set_size(arc_core, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_grow(arc_core, 1);

        lv_obj_t* name_lbl = lv_label_create(page);
        lv_label_set_text(name_lbl, entry.display_name.c_str());
        lv_obj_set_style_text_color(name_lbl, text_muted, 0);
        if (xs_font)
            lv_obj_set_style_text_font(name_lbl, xs_font, 0);

        // Cache arc, label, and icon pointers for observer updates
        CarouselPage cp;
        cp.arc = lv_obj_find_by_name(arc_core, "dial_arc");
        cp.speed_label = lv_obj_find_by_name(arc_core, "speed_label");
        cp.fan_icon = lv_obj_find_by_name(arc_core, "fan_icon");
        cp.object_name = entry.object_name;
        cp.is_controllable = entry.is_controllable;

        // Shrink speed label font for compact display
        if (xs_font && cp.speed_label)
            lv_obj_set_style_text_font(cp.speed_label, xs_font, 0);

        set_icon_pivot(cp.fan_icon);

        // Shrink knob for compact carousel display
        if (cp.arc) {
            lv_obj_set_style_pad_all(cp.arc, theme_manager_get_spacing("space_xxs"), LV_PART_KNOB);
        }

        // Auto-controlled fans: hide knob, disable arc interaction
        if (!entry.is_controllable && cp.arc) {
            lv_obj_remove_flag(cp.arc, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(cp.arc, LV_OPA_TRANSP, LV_PART_KNOB);
            lv_obj_set_style_shadow_width(cp.arc, 0, LV_PART_KNOB);
            lv_obj_set_style_outline_width(cp.arc, 0, LV_PART_KNOB);
        }

        // Make whole page clickable → open fan control overlay (when the user
        // taps somewhere that isn't the arc itself: label, icon, empty area).
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(page, this);
        lv_obj_add_event_cb(
            page,
            [](lv_event_t* e) {
                auto* self = static_cast<FanStackWidget*>(lv_event_get_user_data(e));
                if (self)
                    self->handle_clicked();
            },
            LV_EVENT_CLICKED, this);

        ui_carousel_add_item(carousel, page);

        size_t page_idx = carousel_pages_.size();
        carousel_pages_.push_back(cp);

        // In-place arc control: the arc's VALUE_CHANGED does not bubble (LVGL
        // default), so dragging the knob adjusts speed without also triggering
        // the page-level overlay-open handler.
        if (entry.is_controllable && cp.arc && !entry.object_name.empty()) {
            lv_obj_set_user_data(cp.arc, this);
            lv_obj_add_event_cb(cp.arc, on_carousel_arc_value_changed, LV_EVENT_VALUE_CHANGED,
                                reinterpret_cast<void*>(static_cast<uintptr_t>(page_idx)));
        }

        // Observe fan speed → update arc value + label text + spin animation
        // (skip for placeholders with no object_name)
        if (entry.object_name.empty())
            continue;

        auto gen = carousel_gen_;
        auto obs = bind_fan_observer(entry.object_name, [this, page_idx, gen](int speed) {
            if (gen != carousel_gen_ || page_idx >= carousel_pages_.size())
                return;
            auto& cp = carousel_pages_[page_idx];

            // Don't let stale telemetry overwrite the user's optimistic value
            // during the Moonraker round-trip (mirrors FanDial).
            constexpr uint32_t POST_INPUT_SUPPRESSION_MS = 400;
            if (cp.last_user_input_ms != 0 &&
                (lv_tick_get() - cp.last_user_input_ms) < POST_INPUT_SUPPRESSION_MS)
                return;

            if (cp.arc) {
                cp.syncing = true;
                lv_arc_set_value(cp.arc, speed);
                cp.syncing = false;
            }
            if (cp.speed_label) {
                char buf[8];
                lv_label_set_text(cp.speed_label,
                                  lv_tr(helix::format::format_fan_speed(speed, buf, sizeof(buf))));
            }
            update_fan_animation(cp.fan_icon, speed);
        });
        if (obs)
            carousel_observers_.push_back(std::move(obs));
    }

    // Attach auto-resize AFTER all pages are reparented into the carousel.
    // Doing it inside the loop above would trigger an initial resize before the
    // carousel layout is finalized, resulting in 0x0 container dimensions and
    // the arc collapsing to MIN_ARC_SIZE.
    lv_obj_update_layout(carousel);
    auto* state = ui_carousel_get_state(carousel);
    if (state && state->scroll_container) {
        uint32_t child_count = lv_obj_get_child_count(state->scroll_container);
        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* child = lv_obj_get_child(state->scroll_container, static_cast<int32_t>(i));
            if (child) {
                helix::ui::fan_arc_attach_auto_resize(child);
            }
        }
    }

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[FanStackWidget] Carousel bound {} fan pages", page_count);

    rebuilding_carousel_ = false;
}

void FanStackWidget::set_icon_pivot(lv_obj_t* icon) {
    if (icon) {
        lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);
    }
}

ObserverGuard FanStackWidget::bind_fan_observer(const std::string& fan_name,
                                                std::function<void(int speed)> on_update) {
    if (fan_name.empty())
        return {};

    SubjectLifetime lifetime;
    lv_subject_t* subject = printer_state_.get_fan_speed_subject(fan_name, lifetime);
    if (!subject)
        return {};

    auto token = lifetime_.token();
    auto guard = helix::ui::observe_int_sync<FanStackWidget>(
        subject, this,
        [token, on_update](FanStackWidget* /*self*/, int speed) {
            if (token.expired())
                return;
            on_update(speed);
        },
        lifetime);

    // Read current value immediately — the deferred observer initial fire
    // is dropped when populate_widgets() freezes the update queue.
    on_update(lv_subject_get_int(subject));
    return guard;
}

void FanStackWidget::setup_common_observers(std::function<void()> on_anim_changed,
                                            std::function<void()> on_fans_version) {
    animations_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();

    auto token = lifetime_.token();
    anim_settings_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        DisplaySettingsManager::instance().subject_animations_enabled(), this,
        [token, on_anim_changed](FanStackWidget* self, int enabled) {
            if (token.expired())
                return;
            self->animations_enabled_ = (enabled != 0);
            on_anim_changed();
        },
        DisplaySettingsManager::instance().get_subjects_lifetime());

    version_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        printer_state_.get_fans_version_subject(), this,
        [token, on_fans_version](FanStackWidget* /*self*/, int /*version*/) {
            if (token.expired())
                return;
            on_fans_version();
        },
        printer_state_.get_subjects_lifetime());
}

void FanStackWidget::update_label(lv_obj_t* label, int speed_pct) {
    if (!label)
        return;

    char buf[8];
    helix::format::format_percent(speed_pct, buf, sizeof(buf));
    lv_label_set_text(label, buf);
}

void FanStackWidget::update_fan_animation(lv_obj_t* icon, int speed_pct) {
    if (!icon)
        return;

    if (!animations_enabled_ || speed_pct <= 0) {
        helix::ui::fan_spin_stop(icon);
    } else {
        helix::ui::fan_spin_start(icon, speed_pct);
    }
}

void FanStackWidget::refresh_all_animations() {
    update_fan_animation(part_icon_, part_speed_);
    update_fan_animation(hotend_icon_, hotend_speed_);
    update_fan_animation(aux_icon_, aux_speed_);
}

void FanStackWidget::on_carousel_arc_value_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FanStackWidget] on_carousel_arc_value_changed");
    auto* arc = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!arc)
        return;
    auto* self = static_cast<FanStackWidget*>(lv_obj_get_user_data(arc));
    if (!self)
        return;
    auto page_idx = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (page_idx >= self->carousel_pages_.size())
        return;
    auto& cp = self->carousel_pages_[page_idx];

    // Skip events generated by our own observer-driven lv_arc_set_value().
    if (cp.syncing)
        return;

    cp.last_user_input_ms = lv_tick_get();
    int speed = lv_arc_get_value(arc);

    // Optimistic local reflection before the Moonraker echo arrives.
    if (cp.speed_label) {
        char buf[8];
        lv_label_set_text(cp.speed_label,
                          lv_tr(helix::format::format_fan_speed(speed, buf, sizeof(buf))));
    }
    self->update_fan_animation(cp.fan_icon, speed);

    self->send_carousel_fan_speed(cp.object_name, speed);
    LVGL_SAFE_EVENT_CB_END();
}

void FanStackWidget::send_carousel_fan_speed(const std::string& object_name, int speed_percent) {
    if (object_name.empty())
        return;
    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[FanStackWidget] Cannot send fan speed - no API connection");
        NOTIFY_WARNING(lv_tr("No printer connection"));
        return;
    }

    spdlog::trace("[FanStackWidget] Setting carousel fan '{}' to {}%", object_name, speed_percent);

    // Optimistic PrinterState update so sibling consumers refresh immediately;
    // mirrors FanControlOverlay::send_fan_speed.
    printer_state_.update_fan_speed(object_name, static_cast<double>(speed_percent) / 100.0);

    api->set_fan_speed(
        object_name, static_cast<double>(speed_percent), []() {},
        [object_name](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Fan control failed: {}"), err.user_message());
        });
}

void FanStackWidget::handle_clicked() {
    spdlog::debug("[FanStackWidget] Clicked - opening fan control overlay");

    if (!fan_control_panel_ && parent_screen_) {
        auto& overlay = get_fan_control_overlay();

        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();
        overlay.set_api(get_moonraker_api());

        fan_control_panel_ = overlay.create(parent_screen_);
        if (!fan_control_panel_) {
            spdlog::error("[FanStackWidget] Failed to create fan control overlay");
            return;
        }
        NavigationManager::instance().register_overlay_instance(fan_control_panel_, &overlay);
    }

    if (fan_control_panel_) {
        auto& overlay = get_fan_control_overlay();
        overlay.set_api(get_moonraker_api());
        // Re-register before every push: navbar switches clear overlay_instances_,
        // so a cached panel loses its registration after the user leaves and returns.
        // register_overlay_instance is idempotent, so this is safe on first open too.
        NavigationManager::instance().register_overlay_instance(fan_control_panel_, &overlay);
        NavigationManager::instance().push_overlay(fan_control_panel_);
    }
}

void FanStackWidget::on_fan_stack_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FanStackWidget] on_fan_stack_clicked");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<FanStackWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->record_interaction();
        self->handle_clicked();
    } else {
        spdlog::warn("[FanStackWidget] on_fan_stack_clicked: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FanStackWidget::show_fan_picker() {
    if (picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    // The card hangs under the widget tile, centred on it, flipping above when the
    // tile sits low on the screen.
    picker_.show_below_widget(parent_screen_, widget_obj_,
                              helix::ui::ContextMenu::AnchorAlign::Center);
}

void FanStackWidget::ConfigurePicker::on_created(lv_obj_t* backdrop) {
    int space_sm = resolve_space_token("space_sm", 6);

    // --- Display mode rows ---
    lv_obj_t* mode_list = lv_obj_find_by_name(backdrop, "mode_list");
    if (!mode_list) {
        spdlog::error("[FanStackWidget] mode_list not found in picker XML");
        return;
    }

    bool is_carousel = owner_.is_carousel_mode();
    const char* mode_labels[] = {"Stack", "Carousel"};
    const char* mode_values[] = {"stack", "carousel"};
    for (int i = 0; i < 2; ++i) {
        bool is_selected = (i == 0) ? !is_carousel : is_carousel;

        lv_obj_t* row = lv_obj_create(mode_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);
        if (is_selected) {
            lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), 0);
        }
        lv_obj_set_style_bg_color(row, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, lv_tr(mode_labels[i]));
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_set_user_data(row, new RowPayload{this, mode_values[i]});

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FanStackWidget] mode_row_cb");
                auto* target = lv_event_get_current_target_obj(ev);
                auto* payload = static_cast<RowPayload*>(lv_obj_get_user_data(target));
                if (!payload)
                    return;

                // Copy the mode: hide() takes the row - and this payload - with it.
                std::string mode = payload->value;
                ConfigurePicker* picker = payload->picker;
                FanStackWidget& owner = picker->owner_;
                if (mode == "carousel") {
                    owner.config_["display_mode"] = "carousel";
                } else {
                    owner.config_.erase("display_mode");
                }
                spdlog::info("[FanStackWidget] {} display_mode -> {}", owner.instance_id_, mode);
                owner.save_fan_config();
                picker->hide();
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FanStackWidget] mode_row_delete_cb");
                auto* target = lv_event_get_current_target_obj(ev);
                delete static_cast<RowPayload*>(lv_obj_get_user_data(target));
                lv_obj_set_user_data(target, nullptr);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_DELETE, nullptr);
    }

    // --- Icon grid ---
    lv_obj_t* icon_grid = lv_obj_find_by_name(backdrop, "icon_grid");
    if (!icon_grid) {
        spdlog::error("[FanStackWidget] icon_grid not found in picker XML");
        return;
    }

    std::string effective_icon = owner_.icon_name_.empty() ? DEFAULT_FAN_ICON : owner_.icon_name_;

    for (size_t i = 0; i < FAN_ICON_COUNT; ++i) {
        lv_obj_t* cell = lv_obj_create(icon_grid);
        lv_obj_set_size(cell, ICON_CELL_SIZE, ICON_CELL_SIZE);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(cell, 0, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);

        // Pressed feedback
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        apply_icon_cell_highlight(cell, FAN_ICONS[i] == effective_icon);

        // Icon glyph
        const char* cp = ui_icon::lookup_codepoint(FAN_ICONS[i]);
        if (cp) {
            lv_obj_t* icon = lv_label_create(cell);
            lv_label_set_text(icon, cp);
            lv_obj_set_style_text_font(icon, &mdi_icons_24, 0);
            lv_obj_set_style_text_color(icon, theme_manager_get_color("text"), 0);
            lv_obj_center(icon);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        lv_obj_set_user_data(cell, new RowPayload{this, FAN_ICONS[i]});

        lv_obj_add_event_cb(
            cell,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FanStackWidget] icon_cell_cb");
                auto* target = lv_event_get_current_target_obj(e);
                auto* payload = static_cast<RowPayload*>(lv_obj_get_user_data(target));
                if (!payload)
                    return;
                // The card stays up after an icon change, so the payload outlives
                // this call and the name can be passed straight through.
                payload->picker->owner_.select_icon(payload->value);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_add_event_cb(
            cell,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FanStackWidget] icon_cell_delete_cb");
                auto* target = lv_event_get_current_target_obj(e);
                delete static_cast<RowPayload*>(lv_obj_get_user_data(target));
                lv_obj_set_user_data(target, nullptr);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_DELETE, nullptr);
    }

    spdlog::debug("[FanStackWidget] Picker built with {} icons", FAN_ICON_COUNT);
}

// DECLARATIVE_OK: the grid cells are created in C++, so their selection border has
// no XML layer to bind to.
void FanStackWidget::ConfigurePicker::refresh_icon_highlights() {
    lv_obj_t* backdrop = menu();
    if (!backdrop) {
        return;
    }
    lv_obj_t* icon_grid = lv_obj_find_by_name(backdrop, "icon_grid");
    if (!icon_grid) {
        return;
    }

    std::string effective = owner_.icon_name_.empty() ? DEFAULT_FAN_ICON : owner_.icon_name_;
    uint32_t grid_count = lv_obj_get_child_count(icon_grid);
    for (uint32_t i = 0; i < grid_count; ++i) {
        lv_obj_t* cell = lv_obj_get_child(icon_grid, i);
        auto* payload = cell ? static_cast<RowPayload*>(lv_obj_get_user_data(cell)) : nullptr;
        if (payload) {
            apply_icon_cell_highlight(cell, payload->value == effective);
        }
    }
}

void FanStackWidget::select_fan(const std::string& object_name) {
    selected_fan_ = object_name;
    save_fan_config();
    picker_.hide();
    spdlog::info("[FanStackWidget] {} selected fan: {}", instance_id_, object_name);
}

void FanStackWidget::select_icon(const std::string& name) {
    icon_name_ = (name == DEFAULT_FAN_ICON) ? "" : name;
    save_fan_config();
    picker_.refresh_icon_highlights();

    spdlog::info("[FanStackWidget] {} selected icon: {}", instance_id_,
                 icon_name_.empty() ? "fan (default)" : icon_name_);
}

void FanStackWidget::save_fan_config() {
    nlohmann::json config = config_;
    if (!selected_fan_.empty())
        config["fan"] = selected_fan_;
    if (!icon_name_.empty())
        config["icon"] = icon_name_;
    save_widget_config(config);
    spdlog::debug("[FanStackWidget] Saved config: {} fan={} icon={}", instance_id_,
                  selected_fan_.empty() ? "(auto)" : selected_fan_,
                  icon_name_.empty() ? DEFAULT_FAN_ICON : icon_name_);
}
