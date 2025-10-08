#include "lvgl/lvgl.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include <SDL.h>
#include <cstdio>

// SDL window and renderer
static SDL_Window* window = nullptr;
static SDL_Renderer* renderer = nullptr;

// LVGL display and input
static lv_display_t* display = nullptr;
static lv_indev_t* indev_mouse = nullptr;

// Screen dimensions (default to medium size)
static const int SCREEN_WIDTH = UI_SCREEN_MEDIUM_W;
static const int SCREEN_HEIGHT = UI_SCREEN_MEDIUM_H;

// LVGL display flush callback
static void sdl_display_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    // In SDL mode, LVGL renders to its own buffer, we just mark it as flushed
    lv_display_flush_ready(disp);
}

// LVGL input read callback (mouse/touch)
static void sdl_mouse_read(lv_indev_t* indev, lv_indev_data_t* data) {
    int mouse_x, mouse_y;
    uint32_t mouse_state = SDL_GetMouseState(&mouse_x, &mouse_y);

    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state = (mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Initialize SDL
static bool init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init Error: %s\\n", SDL_GetError());
        return false;
    }

    window = SDL_CreateWindow(
        "GuppyScreen UI Prototype",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );

    if (!window) {
        printf("SDL_CreateWindow Error: %s\\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        printf("SDL_CreateRenderer Error: %s\\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    return true;
}

// Initialize LVGL
static bool init_lvgl() {
    lv_init();

    // Create display
    display = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!display) {
        printf("Failed to create LVGL SDL display\\n");
        return false;
    }

    // Create mouse input device
    indev_mouse = lv_sdl_mouse_create();
    if (!indev_mouse) {
        printf("Failed to create LVGL SDL mouse input\\n");
        return false;
    }

    printf("LVGL initialized: %dx%d\\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    return true;
}

// Main application
int main(int argc, char** argv) {
    printf("GuppyScreen UI Prototype\\n");
    printf("========================\\n");
    printf("Target: %dx%d\\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    printf("Nav Width: %d pixels\\n", UI_NAV_WIDTH(SCREEN_WIDTH));
    printf("\\n");

    // Initialize SDL
    if (!init_sdl()) {
        return 1;
    }

    // Initialize LVGL
    if (!init_lvgl()) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Create main screen
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, UI_COLOR_PANEL_BG, LV_PART_MAIN);

    // Create navigation bar and content area
    ui_nav_create(screen);

    printf("UI created successfully\\n");
    printf("Press Ctrl+C to exit\\n\\n");

    // Main event loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            }
        }

        // Run LVGL tasks
        lv_timer_handler();

        // Small delay to prevent 100% CPU usage
        SDL_Delay(5);
    }

    // Cleanup
    printf("Shutting down...\\n");
    lv_deinit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

