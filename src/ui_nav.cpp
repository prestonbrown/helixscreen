#include "ui_nav.h"
#include <cstdio>

// FontAwesome icon codes (Unicode private use area)
#define ICON_HOME        "\xEF\x80\x95"      // fa-house
#define ICON_CONTROLS    "\xEF\x87\xAE"      // fa-sliders
#define ICON_FILAMENT    "\xEF\x81\x9B"      // fa-fill-drip
#define ICON_SETTINGS    "\xEF\x80\x93"      // fa-gear
#define ICON_ADVANCED    "\xEF\x83\xB9"      // fa-ellipsis-vertical

static lv_obj_t* nav_bar = nullptr;
static lv_obj_t* content_area = nullptr;
static lv_obj_t* nav_buttons[UI_PANEL_COUNT] = {nullptr};
static ui_panel_id_t current_panel = UI_PANEL_HOME;

// Button click handler
static void nav_button_clicked(lv_event_t* e) {
    ui_panel_id_t panel_id = (ui_panel_id_t)(intptr_t)lv_event_get_user_data(e);
    ui_nav_set_active(panel_id);
}

// Create a single nav button
static lv_obj_t* create_nav_button(lv_obj_t* parent, const char* icon, ui_panel_id_t panel_id) {
    lv_obj_t* btn = lv_button_create(parent);

    // Calculate button size based on screen
    lv_coord_t screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    lv_coord_t nav_width = UI_NAV_WIDTH(screen_w);
    lv_coord_t btn_size = nav_width - (UI_NAV_PADDING * 2);

    lv_obj_set_size(btn, btn_size, btn_size);
    lv_obj_set_style_bg_color(btn, UI_COLOR_NAV_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);

    // Pressed/checked state styling
    lv_obj_set_style_bg_color(btn, UI_COLOR_PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PRIMARY, LV_STATE_CHECKED);

    // Icon label
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);  // TODO: Use FontAwesome
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(label);

    // Add click event
    lv_obj_add_event_cb(btn, nav_button_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)panel_id);

    // Enable checkable state
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);

    return btn;
}

lv_obj_t* ui_nav_create(lv_obj_t* parent) {
    lv_coord_t screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    lv_coord_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_coord_t nav_width = UI_NAV_WIDTH(screen_w);

    // Create navigation bar container
    nav_bar = lv_obj_create(parent);
    lv_obj_set_size(nav_bar, nav_width, screen_h);
    lv_obj_set_pos(nav_bar, 0, 0);
    lv_obj_set_style_bg_color(nav_bar, UI_COLOR_NAV_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nav_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(nav_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(nav_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nav_bar, UI_NAV_PADDING, LV_PART_MAIN);

    // Use flex layout - vertical, centered
    lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nav_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(nav_bar, UI_NAV_PADDING, LV_PART_MAIN);

    // Create nav buttons
    nav_buttons[UI_PANEL_HOME]     = create_nav_button(nav_bar, "🏠", UI_PANEL_HOME);
    nav_buttons[UI_PANEL_CONTROLS] = create_nav_button(nav_bar, "🎚", UI_PANEL_CONTROLS);
    nav_buttons[UI_PANEL_FILAMENT] = create_nav_button(nav_bar, "🧵", UI_PANEL_FILAMENT);
    nav_buttons[UI_PANEL_SETTINGS] = create_nav_button(nav_bar, "⚙", UI_PANEL_SETTINGS);
    nav_buttons[UI_PANEL_ADVANCED] = create_nav_button(nav_bar, "⋮", UI_PANEL_ADVANCED);

    // Create content area
    content_area = lv_obj_create(parent);
    lv_obj_set_size(content_area, screen_w - nav_width, screen_h);
    lv_obj_set_pos(content_area, nav_width, 0);
    lv_obj_set_style_bg_color(content_area, UI_COLOR_PANEL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(content_area, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content_area, 0, LV_PART_MAIN);

    // Set home as default
    ui_nav_set_active(UI_PANEL_HOME);

    return nav_bar;
}

void ui_nav_set_active(ui_panel_id_t panel_id) {
    if (panel_id >= UI_PANEL_COUNT) return;

    // Update button states
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        if (i == panel_id) {
            lv_obj_add_state(nav_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(nav_buttons[i], LV_STATE_CHECKED);
        }
    }

    current_panel = panel_id;

    // TODO: Switch panel content
    printf("Switched to panel: %d\\n", panel_id);
}

lv_obj_t* ui_nav_get_content_area(void) {
    return content_area;
}

