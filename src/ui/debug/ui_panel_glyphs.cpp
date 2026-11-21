// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_glyphs.h"

#include "ui_icon_codepoints.h"
#include "ui_row_text.h"

#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"

#include <lvgl/lvgl.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

using namespace helix;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

GlyphsPanel::GlyphsPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // GlyphsPanel doesn't use PrinterState or IMoonrakerAPI, but we accept
    // them for interface consistency with other panels
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void GlyphsPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // GlyphsPanel has no subjects to initialize
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (none required)", get_name());
}

void GlyphsPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Populate the glyphs content
    populate_glyphs();
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void GlyphsPanel::populate_glyphs() {
    lv_obj_t* glyph_list = lv_xml_component_get_slot(panel_, "glyph_list");
    if (!glyph_list) {
        spdlog::error("[{}] glyph_list slot missing", get_name());
        return;
    }

    spdlog::debug("[{}] Adding {} MDI icon items", get_name(), helix::ui::icon::kIconMapSize);
    for (size_t i = 0; i < helix::ui::icon::kIconMapSize; i++) {
        const auto& icon = helix::ui::icon::kIconMap[i];
        auto* item = static_cast<lv_obj_t*>(lv_xml_create(glyph_list, "glyph_item", nullptr));
        if (!item) {
            continue;
        }
        // Raw glyph bytes and map names: set after creation, never as props.
        helix::ui::set_row_label_text(item, "glyph_label", icon.codepoint);
        helix::ui::set_row_label_text(item, "glyph_name", icon.name);
    }

    // Force layout update to ensure scrolling works correctly
    lv_obj_update_layout(panel_);

    spdlog::info("[{}] Setup complete with {} MDI icons", get_name(),
                 helix::ui::icon::kIconMapSize);
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

GlyphsPanel& get_global_glyphs_panel() {
    return helix::lazy_global<GlyphsPanel>("GlyphsPanel", get_printer_state(), nullptr);
}

// Legacy create wrapper (test panel - still used by main.cpp)
lv_obj_t* ui_panel_glyphs_create(lv_obj_t* parent) {
    auto& panel = get_global_glyphs_panel();
    if (!panel.are_subjects_initialized()) {
        panel.init_subjects();
    }

    const std::string count_text = fmt::format("{} icons", helix::ui::icon::kIconMapSize);
    const char* attrs[] = {"icon_count_text", count_text.c_str(), nullptr};
    lv_obj_t* glyphs_panel =
        static_cast<lv_obj_t*>(lv_xml_create(parent, panel.get_xml_component_name(), attrs));
    if (glyphs_panel) {
        panel.setup(glyphs_panel, nullptr);
    }
    return glyphs_panel;
}
