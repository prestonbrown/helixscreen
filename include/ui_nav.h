#pragma once

#include "lvgl/lvgl.h"
#include "ui_theme.h"

// Navigation panel IDs
typedef enum {
    UI_PANEL_HOME,
    UI_PANEL_CONTROLS,
    UI_PANEL_FILAMENT,
    UI_PANEL_SETTINGS,
    UI_PANEL_ADVANCED,
    UI_PANEL_COUNT
} ui_panel_id_t;

// Create the navigation bar (left side vertical bar)
lv_obj_t* ui_nav_create(lv_obj_t* parent);

// Set active panel (highlights nav button, switches content)
void ui_nav_set_active(ui_panel_id_t panel_id);

// Get the content area where panels should be displayed
lv_obj_t* ui_nav_get_content_area(void);

