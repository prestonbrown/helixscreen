/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ui_panel_home.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "tips_manager.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cstdlib>

static lv_obj_t* home_panel = nullptr;

// Widget references for direct updates
static lv_obj_t* network_icon_label = nullptr;
static lv_obj_t* network_text_label = nullptr;
static lv_obj_t* light_icon_label = nullptr;

// Subjects for reactive data binding
static lv_subject_t status_subject;
static lv_subject_t temp_subject;
static lv_subject_t network_icon_subject;
static lv_subject_t network_label_subject;
static lv_subject_t network_color_subject;
static lv_subject_t light_icon_color_subject;

static char status_buffer[512];  // Larger buffer for tips (title + content)
static char temp_buffer[32];
static char network_icon_buffer[8];
static char network_label_buffer[32];
static char network_color_buffer[16];

static bool subjects_initialized = false;
static bool light_on = false;
static network_type_t current_network = NETWORK_WIFI;

// Tip of the day rotation
static lv_timer_t* tip_rotation_timer = nullptr;
static PrintingTip current_tip;  // Store full tip for dialog display
static lv_obj_t* tip_detail_dialog = nullptr;  // Modal dialog reference

// Forward declarations
static void light_toggle_event_cb(lv_event_t* e);
static void print_card_clicked_cb(lv_event_t* e);
static void tip_text_clicked_cb(lv_event_t* e);
static void network_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
static void light_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
static void tip_rotation_timer_cb(lv_timer_t* timer);
static void update_tip_of_day();

void ui_panel_home_init_subjects() {
    if (subjects_initialized) {
        spdlog::warn("Home panel subjects already initialized");
        return;
    }

    spdlog::debug("Initializing home panel subjects");

    // Initialize subjects with default values
    lv_subject_init_string(&status_subject, status_buffer, NULL, sizeof(status_buffer), "Welcome to HelixScreen");
    lv_subject_init_string(&temp_subject, temp_buffer, NULL, sizeof(temp_buffer), "30 °C");
    lv_subject_init_string(&network_icon_subject, network_icon_buffer, NULL, sizeof(network_icon_buffer), ICON_WIFI);
    lv_subject_init_string(&network_label_subject, network_label_buffer, NULL, sizeof(network_label_buffer), "Wi-Fi");
    lv_subject_init_string(&network_color_subject, network_color_buffer, NULL, sizeof(network_color_buffer), "0xff4444");
    lv_subject_init_color(&light_icon_color_subject, lv_color_hex(0x909090));  // Muted gray for "off" state

    // Register subjects globally so XML can bind to them
    lv_xml_register_subject(NULL, "status_text", &status_subject);
    lv_xml_register_subject(NULL, "temp_text", &temp_subject);
    lv_xml_register_subject(NULL, "network_icon", &network_icon_subject);
    lv_xml_register_subject(NULL, "network_label", &network_label_subject);
    lv_xml_register_subject(NULL, "network_color", &network_color_subject);
    lv_xml_register_subject(NULL, "light_icon_color", &light_icon_color_subject);

    // Register event callbacks BEFORE loading XML
    lv_xml_register_event_cb(NULL, "light_toggle_cb", light_toggle_event_cb);
    lv_xml_register_event_cb(NULL, "print_card_clicked_cb", print_card_clicked_cb);
    lv_xml_register_event_cb(NULL, "tip_text_clicked_cb", tip_text_clicked_cb);

    subjects_initialized = true;
    spdlog::debug("Registered subjects: status_text, temp_text, network_icon, network_label, network_color, light_icon_color");
    spdlog::debug("Registered event callbacks: light_toggle_cb, print_card_clicked_cb, tip_text_clicked_cb");

    // Set initial tip of the day
    update_tip_of_day();
}

void ui_panel_home_setup_observers(lv_obj_t* panel) {
    if (!subjects_initialized) {
        spdlog::error("Subjects not initialized! Call ui_panel_home_init_subjects() first!");
        return;
    }

    home_panel = panel;

    // Use LVGL 9's name-based widget lookup - resilient to layout changes
    network_icon_label = lv_obj_find_by_name(home_panel, "network_icon");
    network_text_label = lv_obj_find_by_name(home_panel, "network_label");
    light_icon_label = lv_obj_find_by_name(home_panel, "light_icon");

    if (!network_icon_label || !network_text_label || !light_icon_label) {
        spdlog::error("Failed to find named widgets (net_icon={}, net_label={}, light={})",
                      static_cast<void*>(network_icon_label),
                      static_cast<void*>(network_text_label),
                      static_cast<void*>(light_icon_label));
        return;
    }

    // Get screen dimensions for responsive sizing
    lv_display_t* display = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(display);

    // 1. Set responsive printer image size and SCALE (not just crop/pad)
    lv_obj_t* printer_image = lv_obj_find_by_name(home_panel, "printer_image");
    if (printer_image) {
        int32_t printer_size;
        uint16_t zoom_level;  // 256 = 100%, 128 = 50%, 512 = 200%

        // Calculate printer image size and zoom based on screen height
        if (screen_height <= UI_SCREEN_TINY_H) {
            printer_size = 150;  // Tiny screens (480x320)
            zoom_level = 96;     // 37.5% zoom to scale 400px -> 150px
        } else if (screen_height <= UI_SCREEN_SMALL_H) {
            printer_size = 250;  // Small screens (800x480)
            zoom_level = 160;    // 62.5% zoom to scale 400px -> 250px
        } else if (screen_height <= UI_SCREEN_MEDIUM_H) {
            printer_size = 300;  // Medium screens (1024x600)
            zoom_level = 192;    // 75% zoom to scale 400px -> 300px
        } else {
            printer_size = 400;  // Large screens (1280x720+)
            zoom_level = 256;    // 100% zoom (original size)
        }

        lv_obj_set_width(printer_image, printer_size);
        lv_obj_set_height(printer_image, printer_size);
        lv_image_set_scale(printer_image, zoom_level);  // Actually scale the image
        spdlog::debug("Set printer image: size={}px, zoom={} ({}%) for screen height {}",
                      printer_size, zoom_level, (zoom_level * 100) / 256, screen_height);
    } else {
        spdlog::warn("Printer image not found - size not adjusted");
    }

    // 2. Set responsive info card icon sizes
    const lv_font_t* info_icon_font;
    if (screen_height <= UI_SCREEN_TINY_H) {
        info_icon_font = &fa_icons_24;  // Tiny: 24px icons
    } else if (screen_height <= UI_SCREEN_SMALL_H) {
        info_icon_font = &fa_icons_24;  // Small: 24px icons
    } else {
        info_icon_font = &fa_icons_48;  // Medium/Large: 48px icons
    }

    lv_obj_t* temp_icon = lv_obj_find_by_name(home_panel, "temp_icon");
    if (temp_icon) lv_obj_set_style_text_font(temp_icon, info_icon_font, 0);

    if (network_icon_label) lv_obj_set_style_text_font(network_icon_label, info_icon_font, 0);
    if (light_icon_label) lv_obj_set_style_text_font(light_icon_label, info_icon_font, 0);

    // 3. Set responsive status text font for tiny screens
    if (screen_height <= UI_SCREEN_TINY_H) {
        lv_obj_t* status_text = lv_obj_find_by_name(home_panel, "status_text_label");
        if (status_text) {
            lv_obj_set_style_text_font(status_text, UI_FONT_HEADING, 0);  // Smaller font for tiny
            spdlog::debug("Set status text to UI_FONT_HEADING for tiny screen");
        }
    }

    int icon_size = (info_icon_font == &fa_icons_24) ? 24 : (info_icon_font == &fa_icons_32) ? 32 : 48;
    spdlog::debug("Set info card icons to {}px for screen height {}", icon_size, screen_height);

    // Add observers to watch subjects and update widgets
    lv_subject_add_observer(&network_icon_subject, network_observer_cb, nullptr);
    lv_subject_add_observer(&network_label_subject, network_observer_cb, nullptr);
    lv_subject_add_observer(&network_color_subject, network_observer_cb, nullptr);
    lv_subject_add_observer(&light_icon_color_subject, light_observer_cb, nullptr);

    // Apply initial light icon color (observers only fire on *changes*, not initial state)
    if (light_icon_label) {
        lv_color_t initial_color = lv_subject_get_color(&light_icon_color_subject);
        lv_obj_set_style_img_recolor(light_icon_label, initial_color, LV_PART_MAIN);
        lv_obj_set_style_img_recolor_opa(light_icon_label, 255, LV_PART_MAIN);
        spdlog::debug("Applied initial light icon color");
    }

    spdlog::debug("Home panel observers set up successfully");
}

lv_obj_t* ui_panel_home_create(lv_obj_t* parent) {
    spdlog::debug("Creating home panel");

    if (!subjects_initialized) {
        spdlog::error("Subjects not initialized! Call ui_panel_home_init_subjects() first!");
        return nullptr;
    }

    // Create the XML component (will bind to subjects automatically)
    home_panel = (lv_obj_t*)lv_xml_create(parent, "home_panel", nullptr);
    if (!home_panel) {
        spdlog::error("Failed to create home_panel from XML");
        return nullptr;
    }

    // Setup observers
    ui_panel_home_setup_observers(home_panel);

    // Start tip rotation timer (60 seconds = 60000ms)
    if (!tip_rotation_timer) {
        tip_rotation_timer = lv_timer_create(tip_rotation_timer_cb, 60000, NULL);
        spdlog::info("[Home] Started tip rotation timer (60s interval)");
    }

    spdlog::debug("XML home_panel created successfully with reactive observers");
    return home_panel;
}

void ui_panel_home_update(const char* status_text, int temp) {
    // Update subjects - all bound widgets update automatically
    if (status_text) {
        lv_subject_copy_string(&status_subject, status_text);
        spdlog::debug("Updated status_text subject to: {}", status_text);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d °C", temp);
    lv_subject_copy_string(&temp_subject, buf);
    spdlog::debug("Updated temp_text subject to: {}", buf);
}

void ui_panel_home_set_network(network_type_t type) {
    current_network = type;

    switch (type) {
        case NETWORK_WIFI:
            lv_subject_copy_string(&network_icon_subject, ICON_WIFI);
            lv_subject_copy_string(&network_label_subject, "Wi-Fi");
            lv_subject_copy_string(&network_color_subject, "0xff4444");  // Primary color
            break;
        case NETWORK_ETHERNET:
            lv_subject_copy_string(&network_icon_subject, ICON_ETHERNET);
            lv_subject_copy_string(&network_label_subject, "Ethernet");
            lv_subject_copy_string(&network_color_subject, "0xff4444");  // Primary color
            break;
        case NETWORK_DISCONNECTED:
            lv_subject_copy_string(&network_icon_subject, ICON_WIFI_SLASH);
            lv_subject_copy_string(&network_label_subject, "Disconnected");
            lv_subject_copy_string(&network_color_subject, "0x909090");  // Text secondary
            break;
    }
    spdlog::debug("Updated network status to type {}", static_cast<int>(type));
}

void ui_panel_home_set_light(bool is_on) {
    light_on = is_on;

    if (is_on) {
        // Light is on - show bright yellow/white
        lv_subject_set_color(&light_icon_color_subject, lv_color_hex(0xFFD700));
    } else {
        // Light is off - show muted gray
        lv_subject_set_color(&light_icon_color_subject, lv_color_hex(0x909090));
    }
    spdlog::debug("Updated light state to: {}", is_on ? "ON" : "OFF");
}

bool ui_panel_home_get_light_state() {
    return light_on;
}

static void light_toggle_event_cb(lv_event_t* e) {
    (void)e;  // Unused parameter

    spdlog::info("Light button clicked");

    // Toggle the light state
    ui_panel_home_set_light(!light_on);

    // TODO: Add callback to send command to Klipper
    spdlog::debug("Light toggled: {}", light_on ? "ON" : "OFF");
}

static void print_card_clicked_cb(lv_event_t* e) {
    (void)e;  // Unused parameter

    spdlog::info("Print card clicked - navigating to print select panel");

    // Navigate to print select panel
    ui_nav_set_active(UI_PANEL_PRINT_SELECT);
}

// Observer callback for network icon/label/color changes
static void network_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)observer;  // Unused parameter
    (void)subject;   // Unused - we read subjects directly, not from callback param

    if (!network_icon_label || !network_text_label) {
        return;
    }

    // Update network icon text
    const char* icon = lv_subject_get_string(&network_icon_subject);
    if (icon) {
        lv_label_set_text(network_icon_label, icon);
    }

    // Update network label text
    const char* label = lv_subject_get_string(&network_label_subject);
    if (label) {
        lv_label_set_text(network_text_label, label);
    }

    // Network icon/label colors are now handled by theme
    // Color changes would be managed through state changes if needed

    spdlog::trace("Network observer updated widgets");
}

// Observer callback for light icon color changes
static void light_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)observer;  // Unused parameter

    if (!light_icon_label) {
        return;
    }

    // Update light icon color using image recolor (Material Design icons are monochrome)
    lv_color_t color = lv_subject_get_color(subject);
    lv_obj_set_style_img_recolor(light_icon_label, color, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(light_icon_label, 255, LV_PART_MAIN);

    spdlog::trace("Light observer updated icon color");
}

// Helper function to update tip of the day
static void update_tip_of_day() {
    auto tip = TipsManager::get_instance()->get_random_unique_tip();

    if (!tip.title.empty()) {
        // Store full tip for dialog display
        current_tip = tip;

        snprintf(status_buffer, sizeof(status_buffer), "%s", tip.title.c_str());
        lv_subject_copy_string(&status_subject, status_buffer);
        spdlog::info("[Home] Updated tip: {}", tip.title);
    } else {
        spdlog::warn("[Home] Failed to get tip, keeping current");
    }
}

// Timer callback for tip rotation (runs every 60 seconds)
static void tip_rotation_timer_cb(lv_timer_t* timer) {
    (void)timer;  // Unused parameter
    update_tip_of_day();
}

// Event handler for tip text click - shows modal with full tip
static void tip_text_clicked_cb(lv_event_t* e) {
    (void)e;  // Unused parameter

    if (current_tip.title.empty()) {
        spdlog::warn("[Home] No tip available to display");
        return;
    }

    spdlog::info("[Home] Tip text clicked - showing detail dialog");

    // Create dialog with current tip data
    const char* attrs[] = {
        "title", current_tip.title.c_str(),
        "content", current_tip.content.c_str(),
        NULL
    };

    lv_obj_t* screen = lv_screen_active();
    tip_detail_dialog = (lv_obj_t*)lv_xml_create(screen, "tip_detail_dialog", attrs);

    if (!tip_detail_dialog) {
        spdlog::error("[Home] Failed to create tip detail dialog from XML");
        return;
    }

    // Wire up Ok button to close dialog
    lv_obj_t* btn_ok = lv_obj_find_by_name(tip_detail_dialog, "btn_ok");
    if (btn_ok) {
        lv_obj_add_event_cb(btn_ok, [](lv_event_t* e) {
            (void)e;
            if (tip_detail_dialog) {
                lv_obj_delete(tip_detail_dialog);
                tip_detail_dialog = nullptr;
                spdlog::debug("[Home] Tip dialog closed via Ok button");
            }
        }, LV_EVENT_CLICKED, nullptr);
    }

    // Backdrop click to close
    lv_obj_add_event_cb(tip_detail_dialog, [](lv_event_t* e) {
        lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
        lv_obj_t* current_target = (lv_obj_t*)lv_event_get_current_target(e);
        // Only handle if clicking the backdrop itself (not a child)
        if (target == current_target && tip_detail_dialog) {
            lv_obj_delete(tip_detail_dialog);
            tip_detail_dialog = nullptr;
            spdlog::debug("[Home] Tip dialog closed via backdrop click");
        }
    }, LV_EVENT_CLICKED, nullptr);

    // Bring to foreground
    lv_obj_move_foreground(tip_detail_dialog);
    spdlog::debug("[Home] Tip dialog shown: {}", current_tip.title);
}
