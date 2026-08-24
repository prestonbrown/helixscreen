// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_glyphs.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"

#include "app_globals.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <lvgl/lvgl.h>
#include <spdlog/spdlog.h>

#include <memory>

using namespace helix;

// MDI icon font (48px for good visibility in debug panel). Non-const: the
// canonical declaration in ui_fonts.h dropped const so embedded targets can
// populate the symbol at runtime from a .bin face.
extern lv_font_t mdi_icons_48;

/**
 * @brief Create a single icon display item
 *
 * @param parent Parent container for the item
 * @param icon Icon mapping from ui_icon::ICON_MAP
 * @return lv_obj_t* The created item container
 */
static lv_obj_t* create_icon_item(lv_obj_t* parent, const ui_icon::IconMapping& icon) {
    // Container for this icon item
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(item, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(item, 8, 0);
    lv_obj_set_style_radius(item, 8, 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_border_color(item, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_border_opa(item, LV_OPA_50, 0);

    // Flex row layout: [Icon] Name
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(item, 16, 0);

    // Icon label - use MDI font with UTF-8 codepoint
    lv_obj_t* icon_label = lv_label_create(item);
    lv_label_set_text(icon_label, icon.codepoint);
    lv_obj_set_style_text_color(icon_label, theme_manager_get_color("text"), 0);
    lv_obj_set_style_text_font(icon_label, &mdi_icons_48, 0);
    lv_obj_set_width(icon_label, 56); // Fixed width for alignment

    // Name label
    lv_obj_t* name_label = lv_label_create(item);
    lv_label_set_text(name_label, icon.name);
    lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);
    lv_obj_set_style_text_font(name_label, &noto_sans_16, 0);
    lv_obj_set_flex_grow(name_label, 1);

    return item;
}

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
    // Update icon count in header
    lv_obj_t* count_label = lv_obj_find_by_name(panel_, "glyph_count_label");
    if (count_label) {
        char count_text[32];
        snprintf(count_text, sizeof(count_text), "%zu icons", ui_icon::ICON_MAP_SIZE);
        lv_label_set_text(count_label, count_text);
    }

    // Find the scrollable content container
    // It's the second child of the main container (after header)
    lv_obj_t* main_container = lv_obj_get_child(panel_, 0);
    if (!main_container) {
        spdlog::error("[{}] Failed to find main container", get_name());
        return;
    }

    lv_obj_t* content_area = lv_obj_get_child(main_container, 1); // Second child (index 1)
    if (!content_area) {
        spdlog::error("[{}] Failed to find content area", get_name());
        return;
    }

    // Add all MDI icon items to the content area
    spdlog::debug("[{}] Adding {} MDI icon items to content area", get_name(),
                  ui_icon::ICON_MAP_SIZE);
    for (size_t i = 0; i < ui_icon::ICON_MAP_SIZE; i++) {
        create_icon_item(content_area, ui_icon::ICON_MAP[i]);
    }

    // Force layout update to ensure scrolling works correctly
    lv_obj_update_layout(panel_);

    spdlog::info("[{}] Setup complete with {} MDI icons", get_name(), ui_icon::ICON_MAP_SIZE);
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

static std::unique_ptr<GlyphsPanel> g_glyphs_panel;

GlyphsPanel& get_global_glyphs_panel() {
    if (!g_glyphs_panel) {
        g_glyphs_panel = std::make_unique<GlyphsPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("GlyphsPanel",
                                                         []() { g_glyphs_panel.reset(); });
    }
    return *g_glyphs_panel;
}

// Legacy create wrapper (test panel - still used by main.cpp)
lv_obj_t* ui_panel_glyphs_create(lv_obj_t* parent) {
    auto& panel = get_global_glyphs_panel();
    if (!panel.are_subjects_initialized()) {
        panel.init_subjects();
    }

    lv_obj_t* glyphs_panel =
        static_cast<lv_obj_t*>(lv_xml_create(parent, panel.get_xml_component_name(), nullptr));
    if (glyphs_panel) {
        panel.setup(glyphs_panel, nullptr);
    }
    return glyphs_panel;
}
