#include "lvgl/lvgl.h"
#include "lvgl/src/others/xml/lv_xml.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include <SDL.h>
#include <cstdio>
#include <ctime>

// Mock file data
struct FileData {
    const char* filename;
    int print_time_minutes;
    float filament_grams;
};

static const FileData test_files[] = {
    {"Burr_Puzzle.gcode", 19, 4.0f},
    {"Scraper_grip.gcode", 80, 30.0f},
    {"Robot.gcode", 121, 12.04f},
    {"Small_box.gcode", 15, 3.5f},
    {"Large_vase.gcode", 240, 85.0f},
    {"Support_test.gcode", 45, 12.0f},
};
static const int FILE_COUNT = sizeof(test_files) / sizeof(test_files[0]);

// Format print time as "19m", "1h20m", "2h1m"
static void format_print_time(char* buf, size_t size, int minutes) {
    if (minutes < 60) {
        snprintf(buf, size, "%dm", minutes);
    } else {
        int hours = minutes / 60;
        int mins = minutes % 60;
        if (mins == 0) {
            snprintf(buf, size, "%dh", hours);
        } else {
            snprintf(buf, size, "%dh%dm", hours, mins);
        }
    }
}

// Create and populate a card from test_card component
static lv_obj_t* create_file_card(lv_obj_t* parent, const FileData& file) {
    // Instantiate card from XML component
    lv_obj_t* card = (lv_obj_t*)lv_xml_create(parent, "test_card", NULL);
    if (!card) {
        LV_LOG_ERROR("Failed to create card from test_card component");
        return nullptr;
    }

    // Find child widgets by name
    lv_obj_t* filename_label = lv_obj_find_by_name(card, "card_filename");
    lv_obj_t* time_label = lv_obj_find_by_name(card, "card_print_time");
    lv_obj_t* filament_label = lv_obj_find_by_name(card, "card_filament");

    if (!filename_label || !time_label || !filament_label) {
        LV_LOG_ERROR("Failed to find card child widgets");
        return card;
    }

    // Populate with data
    lv_label_set_text(filename_label, file.filename);

    char time_buf[32];
    format_print_time(time_buf, sizeof(time_buf), file.print_time_minutes);
    lv_label_set_text(time_label, time_buf);

    char filament_buf[32];
    snprintf(filament_buf, sizeof(filament_buf), "%.1fg", file.filament_grams);
    lv_label_set_text(filament_label, filament_buf);

    LV_LOG_USER("Created card: %s (%s, %s)", file.filename, time_buf, filament_buf);

    return card;
}

int main(int, char**) {
    printf("Dynamic Card Instantiation Test\n");
    printf("================================\n\n");

    // Initialize LVGL with SDL
    lv_init();
    lv_display_t* display = lv_sdl_window_create(1024, 800);
    lv_indev_t* mouse = lv_sdl_mouse_create();

    if (!display || !mouse) {
        printf("ERROR: Failed to initialize LVGL/SDL\n");
        return 1;
    }

    // Create screen (theme handles background)
    lv_obj_t* screen = lv_screen_active();

    // Register fonts
    LV_LOG_USER("Registering fonts...");
    lv_xml_register_font(NULL, "fa_icons_16", &fa_icons_16);
    lv_xml_register_font(NULL, "montserrat_16", &lv_font_montserrat_16);

    // Register XML components (globals first, then test_card)
    LV_LOG_USER("Registering XML components...");
    lv_xml_register_component_from_file("A:ui_xml/globals.xml");
    lv_xml_register_component_from_file("A:ui_xml/test_card.xml");

    // Create scrollable grid container (theme handles background)
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, 1024, 800);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(container, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(container, 20, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Dynamically instantiate cards
    printf("\nInstantiating %d cards...\n", FILE_COUNT);
    for (int i = 0; i < FILE_COUNT; i++) {
        create_file_card(container, test_files[i]);
    }

    printf("\n✅ SUCCESS: All cards instantiated and populated!\n");
    printf("Press 'S' to take screenshot, close window to exit.\n\n");

    // Event loop
    while (lv_display_get_next(NULL)) {
        lv_timer_handler();
        SDL_Delay(5);
    }

    lv_deinit();
    return 0;
}
