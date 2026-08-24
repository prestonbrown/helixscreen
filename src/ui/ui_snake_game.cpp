// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_snake_game.cpp
 * @brief Snake easter egg - filament tube edition
 *
 * Grid-based Snake game with interpolated rendering at ~60fps.
 * Snake body drawn as 3D filament tubes (shadow/body/highlight layers).
 * Food drawn as spool boxes using ui_draw_spool_box().
 * Input via swipe gestures (touchscreen) or D-pad overlay (SDL/desktop).
 */

#include "ui_snake_game.h"

#include "ui_spool_drawing.h"
#include "ui_utils.h"

#include "config.h"
#include "display_backend.h"
#include "display_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "sound_manager.h"
#include "sound_theme.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <vector>

namespace helix {

// ============================================================================
// CONSTANTS
// ============================================================================

static constexpr int32_t CELL_SIZE = 20;
static constexpr uint32_t INITIAL_TICK_MS = 150;
static constexpr uint32_t MIN_TICK_MS = 70;
static constexpr int SPEED_UP_INTERVAL = 5;
static constexpr uint32_t RENDER_TICK_MS = 16; // ~60fps render timer
static constexpr uint32_t DT_CLAMP_MS = 200;   // Spiral-of-death clamp

// Config key for persisted high score (non-obvious name)
static constexpr const char* HIGH_SCORE_KEY = "/display/frame_counter";

// Death animation timing (ms)
static constexpr uint32_t DEATH_FLASH_DURATION = 50;
static constexpr uint32_t DEATH_SHRINK_DURATION = 200;
static constexpr uint32_t DEATH_CARD_TIME = 300;
static constexpr uint32_t DEATH_INPUT_READY_TIME = 600;
static constexpr uint32_t TRACKER_FADE_MS = 500;

// Tracker music asset
static constexpr const char* SNAKE_MUSIC_PATH = "assets/sounds/elysium.mod";

// Filament colors for snake body (random at game start)
static constexpr uint32_t FILAMENT_COLORS[] = {
    0xED1C24, // Red
    0x00A651, // Green
    0x2E3192, // Blue
    0xFFF200, // Yellow
    0xF7941D, // Orange
    0x92278F, // Purple
    0x00AEEF, // Cyan
    0xEC008C, // Magenta
    0x8DC63F, // Lime
    0xF15A24, // Vermillion
};
static constexpr int NUM_FILAMENT_COLORS =
    static_cast<int>(sizeof(FILAMENT_COLORS) / sizeof(FILAMENT_COLORS[0]));

// Food spool colors (random per food item)
static constexpr uint32_t FOOD_COLORS[] = {
    0xFF6B35, // Tangerine
    0x00D2FF, // Sky blue
    0xFFD700, // Gold
    0xFF1493, // Deep pink
    0x7FFF00, // Chartreuse
    0xDA70D6, // Orchid
};
static constexpr int NUM_FOOD_COLORS =
    static_cast<int>(sizeof(FOOD_COLORS) / sizeof(FOOD_COLORS[0]));

// Speed tier border colors
static constexpr uint32_t TIER_COLORS[] = {
    0x666666, // Neutral gray (tier 0)
    0x00A651, // Green (tier 1)
    0xFFF200, // Yellow (tier 2)
    0xF7941D, // Orange (tier 3)
    0xED1C24, // Red (tier 4+)
};
static constexpr int NUM_TIER_COLORS =
    static_cast<int>(sizeof(TIER_COLORS) / sizeof(TIER_COLORS[0]));

// ============================================================================
// TYPES
// ============================================================================

enum class Direction { UP, DOWN, LEFT, RIGHT };

enum class InputMode { SWIPE, DPAD };

struct GridPos {
    int x;
    int y;
    bool operator==(const GridPos& o) const {
        return x == o.x && y == o.y;
    }
    bool operator!=(const GridPos& o) const {
        return !(*this == o);
    }
};

struct Particle {
    float x;
    float y;
    float vx;
    float vy;
    float life;     // Remaining life in seconds
    float max_life; // Starting life for opacity calc
    lv_color_t color;
    bool active;
};

// ============================================================================
// STATE STRUCTS
// ============================================================================

struct GridState {
    int cols = 0;
    int rows = 0;
    int offset_x = 0;
    int offset_y = 0;
    std::vector<GridPos> free_cells;

    void rebuild_free_cells(const std::deque<GridPos>& snake) {
        free_cells.clear();
        free_cells.reserve(cols * rows);
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                GridPos p{x, y};
                if (std::find(snake.begin(), snake.end(), p) == snake.end()) {
                    free_cells.push_back(p);
                }
            }
        }
    }

    void remove_cell(const GridPos& p) {
        for (size_t i = 0; i < free_cells.size(); i++) {
            if (free_cells[i] == p) {
                free_cells[i] = free_cells.back();
                free_cells.pop_back();
                return;
            }
        }
    }

    void add_cell(const GridPos& p) {
        free_cells.push_back(p);
    }
};

struct GameState {
    std::deque<GridPos> snake;
    std::deque<GridPos> prev_snake; // Snapshot before last logic tick
    Direction direction = Direction::RIGHT;
    Direction prev_direction = Direction::RIGHT;
    int score = 0;
    int high_score = 0;
    GridPos food = {0, 0};
    lv_color_t food_color = {};
    lv_color_t snake_color = {};
    uint32_t tick_ms = INITIAL_TICK_MS;
    int speed_tier = 0;
    bool game_over = false;
    bool game_started = false;
};

struct RenderState {
    float interp = 0.0f;
    float tick_accumulator = 0.0f;
    uint32_t last_render_ms = 0;
    float food_pulse_phase = 0.0f;

    // Head squash effect
    bool squash_active = false;
    uint32_t squash_start_ms = 0;

    // Eat particles
    std::array<Particle, 8> particles = {};

    // Death animation
    uint32_t death_start_ms = 0;
    bool death_input_ready = false;
};

struct InputState {
    InputMode mode = InputMode::SWIPE;
    // 2-deep direction queue
    Direction queue[2] = {Direction::RIGHT, Direction::RIGHT};
    int queue_count = 0;

    // Swipe tracking
    lv_point_t touch_start = {0, 0};
    bool swipe_handled = false;

    void push_direction(Direction dir, Direction current_dir) {
        // Determine what the effective direction would be after applying queued inputs
        Direction check_against = current_dir;
        if (queue_count > 0) {
            check_against = queue[queue_count - 1];
        }
        // 180-degree reversal prevention
        if ((dir == Direction::UP && check_against == Direction::DOWN) ||
            (dir == Direction::DOWN && check_against == Direction::UP) ||
            (dir == Direction::LEFT && check_against == Direction::RIGHT) ||
            (dir == Direction::RIGHT && check_against == Direction::LEFT)) {
            return;
        }
        if (queue_count < 2) {
            queue[queue_count++] = dir;
        }
    }

    bool pop_direction(Direction& out) {
        if (queue_count == 0)
            return false;
        out = queue[0];
        // Shift down
        queue[0] = queue[1];
        queue_count--;
        return true;
    }
};

// ============================================================================
// GAME STATE (anonymous namespace)
// ============================================================================

namespace {

// UI objects
lv_obj_t* g_overlay = nullptr;
lv_obj_t* g_game_area = nullptr;
lv_obj_t* g_score_label = nullptr;
lv_obj_t* g_gameover_label = nullptr;
lv_obj_t* g_close_btn = nullptr;
lv_timer_t* g_render_timer = nullptr;

// D-pad buttons (only created in DPAD mode)
lv_obj_t* g_dpad_up = nullptr;
lv_obj_t* g_dpad_down = nullptr;
lv_obj_t* g_dpad_left = nullptr;
lv_obj_t* g_dpad_right = nullptr;

// Organized state
GridState g_grid;
GameState g_game;
RenderState g_render;
InputState g_input;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

void init_game();
void game_logic_tick();
void render_tick(lv_timer_t* timer);
void draw_cb(lv_event_t* e);
void touch_cb(lv_event_t* e);
void input_cb(lv_event_t* e);
void close_cb(lv_event_t* e);
void place_food();
void update_score_label();
void show_game_over();
void create_overlay();
void destroy_overlay();
void detect_input_mode();
void spawn_eat_particles(int32_t px, int32_t py, lv_color_t color);
void update_particles(float dt);

// ============================================================================
// TUBE DRAWING
// ============================================================================

/// Draw a flat line segment (base primitive for tube layers)
void draw_flat_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                    lv_color_t color, int32_t width) {
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = width;
    line_dsc.p1.x = x1;
    line_dsc.p1.y = y1;
    line_dsc.p2.x = x2;
    line_dsc.p2.y = y2;
    line_dsc.round_start = true;
    line_dsc.round_end = true;
    lv_draw_line(layer, &line_dsc);
}

/// Draw a 3D tube segment between two points (shadow/body/highlight layers)
void draw_tube_segment(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       lv_color_t color, int32_t width) {
    // Shadow: wider, darker
    int32_t shadow_extra = LV_MAX(2, width / 2);
    lv_color_t shadow_color = ui_color_darken(color, 35);
    draw_flat_line(layer, x1, y1, x2, y2, shadow_color, width + shadow_extra);

    // Body: main tube surface
    draw_flat_line(layer, x1, y1, x2, y2, color, width);

    // Highlight: narrower, lighter, offset toward top-left
    int32_t hl_width = LV_MAX(1, width * 2 / 5);
    lv_color_t hl_color = ui_color_lighten(color, 44);

    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    int32_t off_amount = width / 4 + 1;

    if (dx == 0) {
        offset_x = off_amount; // Vertical: highlight right
    } else if (dy == 0) {
        offset_y = -off_amount; // Horizontal: highlight up
    }

    draw_flat_line(layer, x1 + offset_x, y1 + offset_y, x2 + offset_x, y2 + offset_y, hl_color,
                   hl_width);
}

// ============================================================================
// GRID HELPERS
// ============================================================================

/// Convert grid position to pixel center coordinates
void grid_to_pixel(const GridPos& pos, int32_t& px, int32_t& py) {
    px = g_grid.offset_x + pos.x * CELL_SIZE + CELL_SIZE / 2;
    py = g_grid.offset_y + pos.y * CELL_SIZE + CELL_SIZE / 2;
}

/// Interpolate between two grid positions and return pixel coords
void lerp_grid_to_pixel(const GridPos& from, const GridPos& to, float t, int32_t& px, int32_t& py) {
    float fx = static_cast<float>(from.x) + (static_cast<float>(to.x - from.x)) * t;
    float fy = static_cast<float>(from.y) + (static_cast<float>(to.y - from.y)) * t;
    px = g_grid.offset_x + static_cast<int32_t>(fx * CELL_SIZE + CELL_SIZE / 2);
    py = g_grid.offset_y + static_cast<int32_t>(fy * CELL_SIZE + CELL_SIZE / 2);
}

/// Pick a random filament color
lv_color_t random_filament_color() {
    return lv_color_hex(FILAMENT_COLORS[rand() % NUM_FILAMENT_COLORS]);
}

/// Pick a random food color
lv_color_t random_food_color() {
    return lv_color_hex(FOOD_COLORS[rand() % NUM_FOOD_COLORS]);
}

// ============================================================================
// INPUT MODE DETECTION
// ============================================================================

void detect_input_mode() {
    auto* dm = DisplayManager::instance();
    if (dm && dm->backend() && dm->backend()->type() == DisplayBackendType::SDL) {
        g_input.mode = InputMode::DPAD;
    } else {
        g_input.mode = InputMode::SWIPE;
    }
    spdlog::debug("[SnakeGame] Input mode: {}", g_input.mode == InputMode::DPAD ? "DPAD" : "SWIPE");
}

// ============================================================================
// PARTICLES
// ============================================================================

void spawn_eat_particles(int32_t px, int32_t py, lv_color_t color) {
    int spawned = 0;
    for (auto& p : g_render.particles) {
        if (!p.active && spawned < 8) {
            p.active = true;
            p.x = static_cast<float>(px);
            p.y = static_cast<float>(py);
            p.vx = static_cast<float>((rand() % 241) - 120); // -120..120
            p.vy = static_cast<float>((rand() % 241) - 120);
            p.life = 0.3f;
            p.max_life = 0.3f;
            p.color = color;
            spawned++;
        }
    }
}

void update_particles(float dt) {
    for (auto& p : g_render.particles) {
        if (!p.active)
            continue;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.life -= dt;
        if (p.life <= 0.0f) {
            p.active = false;
        }
    }
}

// ============================================================================
// GAME LOGIC
// ============================================================================

// ============================================================================
// SOUND
// ============================================================================

#ifdef HELIX_HAS_SOUND
SoundDefinition make_sfx(std::vector<SoundStep> steps) {
    SoundDefinition def;
    def.name = "snake_sfx";
    def.steps = std::move(steps);
    return def;
}

void play_sfx_eat() {
    auto& sm = SoundManager::instance();
    if (!sm.has_backend())
        return;
    SoundStep s;
    s.freq_hz = 440;
    s.duration_ms = 80;
    s.wave = Waveform::SQUARE;
    s.velocity = 0.6f;
    s.sweep = {"freq", 880};
    s.envelope = {2, 10, 0.5f, 30};
    sm.play(make_sfx({s}), SoundPriority::UI);
}

void play_sfx_die() {
    auto& sm = SoundManager::instance();
    if (!sm.has_backend())
        return;
    SoundStep s;
    s.freq_hz = 220;
    s.duration_ms = 200;
    s.wave = Waveform::SAW;
    s.velocity = 0.8f;
    s.sweep = {"freq", 55};
    s.envelope = {5, 30, 0.4f, 80};
    sm.play(make_sfx({s}), SoundPriority::UI);
}

void play_sfx_speedup() {
    auto& sm = SoundManager::instance();
    if (!sm.has_backend())
        return;
    // C5→E5→G5 arpeggio
    SoundStep s1, s2, s3;
    s1.freq_hz = 523;
    s1.duration_ms = 50;
    s1.wave = Waveform::SQUARE;
    s1.velocity = 0.5f;
    s1.envelope = {2, 10, 0.6f, 15};
    s2 = s1;
    s2.freq_hz = 659;
    s3 = s1;
    s3.freq_hz = 784;
    sm.play(make_sfx({s1, s2, s3}), SoundPriority::UI);
}

void play_sfx_start() {
    auto& sm = SoundManager::instance();
    if (!sm.has_backend())
        return;
    // C4→G4 rising interval
    SoundStep s1, s2;
    s1.freq_hz = 262;
    s1.duration_ms = 60;
    s1.wave = Waveform::SQUARE;
    s1.velocity = 0.5f;
    s1.envelope = {2, 10, 0.6f, 20};
    s2 = s1;
    s2.freq_hz = 392;
    sm.play(make_sfx({s1, s2}), SoundPriority::UI);
}
#else
void play_sfx_eat() {}
void play_sfx_die() {}
void play_sfx_speedup() {}
void play_sfx_start() {}
#endif

void start_music() {
#ifdef HELIX_HAS_TRACKER
    auto& sm = SoundManager::instance();
    if (sm.has_backend()) {
        sm.play_file(SNAKE_MUSIC_PATH, SoundPriority::UI);
    }
#endif
}

void stop_music() {
#ifdef HELIX_HAS_TRACKER
    auto& sm = SoundManager::instance();
    if (sm.is_tracker_playing()) {
        sm.stop_tracker();
    }
#endif
}

void fade_music() {
#ifdef HELIX_HAS_TRACKER
    auto& sm = SoundManager::instance();
    if (sm.is_tracker_playing()) {
        sm.fade_out_tracker(TRACKER_FADE_MS);
    }
#endif
}

// ============================================================================
// HIGH SCORE
// ============================================================================

void load_high_score() {
    auto* cfg = Config::get_instance();
    if (cfg) {
        g_game.high_score = cfg->get<int>(HIGH_SCORE_KEY, 0);
    }
    spdlog::debug("[SnakeGame] Loaded high score: {}", g_game.high_score);
}

void save_high_score() {
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set(HIGH_SCORE_KEY, g_game.high_score);
        cfg->save();
    }
    spdlog::info("[SnakeGame] Saved new high score: {}", g_game.high_score);
}

void init_game() {
    g_game.snake.clear();
    g_game.prev_snake.clear();
    g_game.direction = Direction::RIGHT;
    g_game.prev_direction = Direction::RIGHT;
    g_game.game_over = false;
    g_game.game_started = true;
    g_game.score = 0;
    g_game.tick_ms = INITIAL_TICK_MS;
    g_game.speed_tier = 0;

    // Random snake color
    g_game.snake_color = random_filament_color();

    // Start snake in center, 3 segments long
    int start_x = g_grid.cols / 2;
    int start_y = g_grid.rows / 2;
    for (int i = 2; i >= 0; i--) {
        g_game.snake.push_back({start_x - i, start_y});
    }
    g_game.prev_snake = g_game.snake;

    // Build free cell list
    g_grid.rebuild_free_cells(g_game.snake);

    // Reset input queue
    g_input.queue_count = 0;
    g_input.swipe_handled = false;

    // Reset render state
    g_render.interp = 0.0f;
    g_render.tick_accumulator = 0.0f;
    g_render.last_render_ms = lv_tick_get();
    g_render.food_pulse_phase = 0.0f;
    g_render.squash_active = false;
    g_render.death_start_ms = 0;
    g_render.death_input_ready = false;
    for (auto& p : g_render.particles) {
        p.active = false;
    }

    place_food();
    update_score_label();

    // Hide game over label
    if (g_gameover_label) {
        lv_obj_add_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN);
    }

    play_sfx_start();
    start_music();
}

void place_food() {
    // Win detection
    if (g_grid.free_cells.empty()) {
        g_game.game_over = true;
        spdlog::info("[SnakeGame] Snake filled the grid - you win!");

        if (g_game.score > g_game.high_score) {
            g_game.high_score = g_game.score;
            save_high_score();
        }

        if (g_gameover_label) {
            const std::string msg =
                fmt::format(lv_tr("YOU WIN!\nScore: {}\nTap to play again"), g_game.score);
            lv_label_set_text(g_gameover_label, msg.c_str());
            lv_obj_remove_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN);
        }
        update_score_label();
        return;
    }

    // O(1) food placement from free_cells
    int idx = rand() % static_cast<int>(g_grid.free_cells.size());
    g_game.food = g_grid.free_cells[idx];
    g_game.food_color = random_food_color();
}

void game_logic_tick() {
    if (g_game.game_over || !g_game.game_started) {
        return;
    }

    // Snapshot current state for interpolation
    g_game.prev_snake = g_game.snake;
    g_game.prev_direction = g_game.direction;

    // Pop direction from input queue
    Direction new_dir;
    if (g_input.pop_direction(new_dir)) {
        g_game.direction = new_dir;

        // Trigger head squash on direction change
        if (new_dir != g_game.prev_direction) {
            g_render.squash_active = true;
            g_render.squash_start_ms = lv_tick_get();
        }
    }

    // Calculate new head position
    GridPos head = g_game.snake.back();
    GridPos new_head = head;

    switch (g_game.direction) {
    case Direction::UP:
        new_head.y--;
        break;
    case Direction::DOWN:
        new_head.y++;
        break;
    case Direction::LEFT:
        new_head.x--;
        break;
    case Direction::RIGHT:
        new_head.x++;
        break;
    }

    // Wall collision
    if (new_head.x < 0 || new_head.x >= g_grid.cols || new_head.y < 0 ||
        new_head.y >= g_grid.rows) {
        g_game.game_over = true;
        show_game_over();
        return;
    }

    // Self collision
    if (std::find(g_game.snake.begin(), g_game.snake.end(), new_head) != g_game.snake.end()) {
        g_game.game_over = true;
        show_game_over();
        return;
    }

    // Move snake
    g_game.snake.push_back(new_head);
    g_grid.remove_cell(new_head);

    // Check food collision
    if (new_head == g_game.food) {
        g_game.score++;
        update_score_label();
        play_sfx_eat();

        // Spawn eat particles at food pixel position
        int32_t fpx, fpy;
        grid_to_pixel(g_game.food, fpx, fpy);
        spawn_eat_particles(fpx, fpy, g_game.food_color);

        place_food();

        // Speed up periodically
        if (g_game.score % SPEED_UP_INTERVAL == 0 && g_game.tick_ms > MIN_TICK_MS) {
            g_game.tick_ms -= 10;
            g_game.speed_tier = g_game.score / SPEED_UP_INTERVAL;
            play_sfx_speedup();
        }
    } else {
        // Remove tail (no growth) - add freed cell back
        GridPos tail = g_game.snake.front();
        g_game.snake.pop_front();
        g_grid.add_cell(tail);
    }
}

void render_tick(lv_timer_t* /*timer*/) {
    uint32_t now = lv_tick_get();
    uint32_t raw_dt = now - g_render.last_render_ms;
    g_render.last_render_ms = now;

    // Clamp dt to avoid spiral of death after pause/debug
    float dt = static_cast<float>(LV_MIN(raw_dt, DT_CLAMP_MS)) / 1000.0f;

    // Update food pulse animation
    g_render.food_pulse_phase += dt * 2.0f * 3.14159265f * 2.0f; // 2Hz
    if (g_render.food_pulse_phase > 6.28318f) {
        g_render.food_pulse_phase -= 6.28318f;
    }

    // Update particles
    update_particles(dt);

    // Update death animation state
    if (g_game.game_over && g_render.death_start_ms > 0) {
        uint32_t death_elapsed = now - g_render.death_start_ms;
        if (death_elapsed >= DEATH_INPUT_READY_TIME && !g_render.death_input_ready) {
            g_render.death_input_ready = true;
        }
    }

    // Accumulate time and run game logic ticks
    if (!g_game.game_over && g_game.game_started) {
        g_render.tick_accumulator += static_cast<float>(LV_MIN(raw_dt, DT_CLAMP_MS));

        while (g_render.tick_accumulator >= static_cast<float>(g_game.tick_ms)) {
            g_render.tick_accumulator -= static_cast<float>(g_game.tick_ms);
            game_logic_tick();
            if (g_game.game_over) {
                g_render.tick_accumulator = 0.0f;
                break;
            }
        }

        // Calculate interpolation factor
        if (g_game.tick_ms > 0) {
            g_render.interp = g_render.tick_accumulator / static_cast<float>(g_game.tick_ms);
            g_render.interp = LV_CLAMP(0.0f, g_render.interp, 1.0f);
        }
    }

    // Trigger redraw
    if (g_game_area) {
        lv_obj_invalidate(g_game_area);
    }
}

void update_score_label() {
    if (g_score_label) {
        char buf[48];
        const std::string text =
            g_game.high_score > 0
                ? fmt::format(lv_tr("Score: {}  |  Best: {}"), g_game.score, g_game.high_score)
                : fmt::format(lv_tr("Score: {}"), g_game.score);
        snprintf(buf, sizeof(buf), "%s", text.c_str());
        lv_label_set_text(g_score_label, buf);
    }
}

void show_game_over() {
    bool new_high = g_game.score > g_game.high_score && g_game.score > 0;
    if (new_high) {
        g_game.high_score = g_game.score;
        save_high_score();
    }

    spdlog::info("[SnakeGame] Game over! Score: {} | Best: {}{}", g_game.score, g_game.high_score,
                 new_high ? " (NEW!)" : "");

    // Start death animation
    g_render.death_start_ms = lv_tick_get();
    g_render.death_input_ready = false;

    play_sfx_die();
    fade_music();

    // Update score label to reflect new high score
    update_score_label();
}

// ============================================================================
// DRAWING
// ============================================================================

void draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_obj_t* obj = lv_event_get_current_target_obj(e);

    lv_area_t obj_area;
    lv_obj_get_coords(obj, &obj_area);

    // Dark background fill
    {
        lv_draw_rect_dsc_t bg_dsc;
        lv_draw_rect_dsc_init(&bg_dsc);
        bg_dsc.bg_color = lv_color_hex(0x0a0a0a);
        bg_dsc.bg_opa = LV_OPA_COVER;
        bg_dsc.radius = 4;

        lv_area_t bg_area = {
            obj_area.x1 + g_grid.offset_x - 2,
            obj_area.y1 + g_grid.offset_y - 2,
            obj_area.x1 + g_grid.offset_x + g_grid.cols * CELL_SIZE + 1,
            obj_area.y1 + g_grid.offset_y + g_grid.rows * CELL_SIZE + 1,
        };
        lv_draw_rect(layer, &bg_dsc, &bg_area);
    }

    // Subtle grid lines
    {
        lv_color_t grid_color = lv_color_hex(0x1a1a1a);
        // Vertical lines
        for (int x = 0; x <= g_grid.cols; x++) {
            int32_t px = obj_area.x1 + g_grid.offset_x + x * CELL_SIZE;
            int32_t y1 = obj_area.y1 + g_grid.offset_y;
            int32_t y2 = y1 + g_grid.rows * CELL_SIZE;
            lv_draw_line_dsc_t line_dsc;
            lv_draw_line_dsc_init(&line_dsc);
            line_dsc.color = grid_color;
            line_dsc.width = 1;
            line_dsc.p1.x = px;
            line_dsc.p1.y = y1;
            line_dsc.p2.x = px;
            line_dsc.p2.y = y2;
            lv_draw_line(layer, &line_dsc);
        }
        // Horizontal lines
        for (int y = 0; y <= g_grid.rows; y++) {
            int32_t py = obj_area.y1 + g_grid.offset_y + y * CELL_SIZE;
            int32_t x1 = obj_area.x1 + g_grid.offset_x;
            int32_t x2 = x1 + g_grid.cols * CELL_SIZE;
            lv_draw_line_dsc_t line_dsc;
            lv_draw_line_dsc_init(&line_dsc);
            line_dsc.color = grid_color;
            line_dsc.width = 1;
            line_dsc.p1.x = x1;
            line_dsc.p1.y = py;
            line_dsc.p2.x = x2;
            line_dsc.p2.y = py;
            lv_draw_line(layer, &line_dsc);
        }
    }

    // Speed tier border
    {
        int tier_idx = LV_MIN(g_game.speed_tier, NUM_TIER_COLORS - 1);
        lv_draw_rect_dsc_t border_dsc;
        lv_draw_rect_dsc_init(&border_dsc);
        border_dsc.bg_opa = LV_OPA_TRANSP;
        border_dsc.border_color = lv_color_hex(TIER_COLORS[tier_idx]);
        border_dsc.border_opa = LV_OPA_COVER;
        border_dsc.border_width = 2;
        border_dsc.radius = 4;

        lv_area_t border_area = {
            obj_area.x1 + g_grid.offset_x - 2,
            obj_area.y1 + g_grid.offset_y - 2,
            obj_area.x1 + g_grid.offset_x + g_grid.cols * CELL_SIZE + 1,
            obj_area.y1 + g_grid.offset_y + g_grid.rows * CELL_SIZE + 1,
        };
        lv_draw_rect(layer, &border_dsc, &border_area);
    }

    if (!g_game.game_started) {
        return;
    }

    // Determine if we can interpolate
    bool can_interp = (g_game.prev_snake.size() == g_game.snake.size()) && !g_game.game_over;
    float t = can_interp ? g_render.interp : 1.0f;

    // Draw food as spool box with pulse
    {
        int32_t fx, fy;
        grid_to_pixel(g_game.food, fx, fy);
        fx += obj_area.x1;
        fy += obj_area.y1;

        float pulse = sinf(g_render.food_pulse_phase) * 2.0f;
        int32_t food_r = CELL_SIZE / 4 + static_cast<int32_t>(pulse);
        if (food_r < 2)
            food_r = 2;

        ui_draw_spool_box(layer, fx, fy, g_game.food_color, true, food_r);
    }

    // Death animation state
    uint32_t death_elapsed = 0;
    bool death_shrinking = false;
    float death_shrink_progress = 0.0f;
    if (g_game.game_over && g_render.death_start_ms > 0) {
        death_elapsed = lv_tick_get() - g_render.death_start_ms;
        if (death_elapsed > DEATH_FLASH_DURATION &&
            death_elapsed <= DEATH_FLASH_DURATION + DEATH_SHRINK_DURATION) {
            death_shrinking = true;
            death_shrink_progress = static_cast<float>(death_elapsed - DEATH_FLASH_DURATION) /
                                    static_cast<float>(DEATH_SHRINK_DURATION);
        }
    }

    // Draw snake body as tube segments
    lv_color_t body_color = g_game.game_over ? lv_color_hex(0xCC2222) : g_game.snake_color;
    int32_t tube_width = CELL_SIZE * 2 / 3;
    int snake_len = static_cast<int>(g_game.snake.size());

    for (int i = 1; i < snake_len; i++) {
        int32_t x1, y1, x2, y2;

        if (can_interp && i < static_cast<int>(g_game.prev_snake.size())) {
            lerp_grid_to_pixel(g_game.prev_snake[i - 1], g_game.snake[i - 1], t, x1, y1);
            lerp_grid_to_pixel(g_game.prev_snake[i], g_game.snake[i], t, x2, y2);
        } else {
            grid_to_pixel(g_game.snake[i - 1], x1, y1);
            grid_to_pixel(g_game.snake[i], x2, y2);
        }

        x1 += obj_area.x1;
        y1 += obj_area.y1;
        x2 += obj_area.x1;
        y2 += obj_area.y1;

        bool is_head = (i == snake_len - 1);

        // Tail taper: last 3 segments taper in width
        int from_tail = i; // segment index from tail (0 = tail end)
        int32_t seg_width = tube_width;
        if (from_tail < 3) {
            // Taper from 40% to 100% over 3 segments
            float taper = 0.4f + 0.6f * (static_cast<float>(from_tail) / 3.0f);
            seg_width = static_cast<int32_t>(static_cast<float>(tube_width) * taper);
            if (seg_width < 4)
                seg_width = 4;
        }

        if (is_head) {
            // Head squash effect
            int32_t head_width = tube_width + 2;
            if (g_render.squash_active) {
                uint32_t squash_elapsed = lv_tick_get() - g_render.squash_start_ms;
                if (squash_elapsed < 100) {
                    // 15% wider during squash
                    head_width = static_cast<int32_t>(static_cast<float>(head_width) * 1.15f);
                } else {
                    g_render.squash_active = false;
                }
            }
            seg_width = head_width;
        }

        // Death shrink: segments shrink from tail to head
        if (death_shrinking) {
            float seg_progress =
                static_cast<float>(i) / static_cast<float>(LV_MAX(snake_len - 1, 1));
            // Tail shrinks first (invert progress relative to segment)
            float shrink = 1.0f - death_shrink_progress * (1.0f - seg_progress);
            if (shrink < 0.0f)
                shrink = 0.0f;
            seg_width = static_cast<int32_t>(static_cast<float>(seg_width) * shrink);
            if (seg_width < 1)
                seg_width = 1;
        }

        lv_color_t c = is_head ? ui_color_lighten(body_color, 20) : body_color;
        draw_tube_segment(layer, x1, y1, x2, y2, c, seg_width);
    }

    // Draw eyes on snake head
    if (g_game.snake.size() >= 2) {
        int32_t hx, hy;
        if (can_interp && g_game.prev_snake.size() >= 2) {
            lerp_grid_to_pixel(g_game.prev_snake.back(), g_game.snake.back(), t, hx, hy);
        } else {
            grid_to_pixel(g_game.snake.back(), hx, hy);
        }
        hx += obj_area.x1;
        hy += obj_area.y1;

        int32_t eye_offset = CELL_SIZE / 4;
        int32_t ex1 = hx, ey1 = hy, ex2 = hx, ey2 = hy;

        switch (g_game.direction) {
        case Direction::UP:
        case Direction::DOWN:
            ex1 = hx - eye_offset;
            ex2 = hx + eye_offset;
            ey1 = ey2 = hy + (g_game.direction == Direction::UP ? -eye_offset / 2 : eye_offset / 2);
            break;
        case Direction::LEFT:
        case Direction::RIGHT:
            ey1 = hy - eye_offset;
            ey2 = hy + eye_offset;
            ex1 = ex2 =
                hx + (g_game.direction == Direction::LEFT ? -eye_offset / 2 : eye_offset / 2);
            break;
        }

        // White circles
        lv_draw_arc_dsc_t eye_dsc;
        lv_draw_arc_dsc_init(&eye_dsc);
        eye_dsc.width = 3;
        eye_dsc.start_angle = 0;
        eye_dsc.end_angle = 360;
        eye_dsc.color = lv_color_white();
        eye_dsc.radius = 3;

        eye_dsc.center.x = ex1;
        eye_dsc.center.y = ey1;
        lv_draw_arc(layer, &eye_dsc);

        eye_dsc.center.x = ex2;
        eye_dsc.center.y = ey2;
        lv_draw_arc(layer, &eye_dsc);

        // Black pupils
        eye_dsc.color = lv_color_black();
        eye_dsc.radius = 2;
        eye_dsc.width = 2;

        eye_dsc.center.x = ex1;
        eye_dsc.center.y = ey1;
        lv_draw_arc(layer, &eye_dsc);

        eye_dsc.center.x = ex2;
        eye_dsc.center.y = ey2;
        lv_draw_arc(layer, &eye_dsc);
    }

    // Draw eat particles
    for (const auto& p : g_render.particles) {
        if (!p.active)
            continue;
        float opacity = p.life / p.max_life;
        lv_draw_arc_dsc_t pdsc;
        lv_draw_arc_dsc_init(&pdsc);
        pdsc.color = p.color;
        pdsc.radius = 3;
        pdsc.width = 3;
        pdsc.start_angle = 0;
        pdsc.end_angle = 360;
        pdsc.opa = static_cast<lv_opa_t>(opacity * 255.0f);
        pdsc.center.x = obj_area.x1 + static_cast<int32_t>(p.x);
        pdsc.center.y = obj_area.y1 + static_cast<int32_t>(p.y);
        lv_draw_arc(layer, &pdsc);
    }

    // Death white flash overlay
    if (g_game.game_over && g_render.death_start_ms > 0 && death_elapsed < DEATH_FLASH_DURATION) {
        lv_draw_rect_dsc_t flash_dsc;
        lv_draw_rect_dsc_init(&flash_dsc);
        flash_dsc.bg_color = lv_color_white();
        flash_dsc.bg_opa = LV_OPA_20;
        flash_dsc.radius = 4;

        lv_area_t flash_area = {
            obj_area.x1 + g_grid.offset_x - 2,
            obj_area.y1 + g_grid.offset_y - 2,
            obj_area.x1 + g_grid.offset_x + g_grid.cols * CELL_SIZE + 1,
            obj_area.y1 + g_grid.offset_y + g_grid.rows * CELL_SIZE + 1,
        };
        lv_draw_rect(layer, &flash_dsc, &flash_area);
    }

    // Show game over label after DEATH_CARD_TIME (fade in over ~200ms)
    if (g_game.game_over && g_render.death_start_ms > 0 && death_elapsed >= DEATH_CARD_TIME) {
        if (g_gameover_label && lv_obj_has_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN)) {
            bool new_high = g_game.score > 0 &&
                            g_game.score >= g_game.high_score; // already saved in show_game_over
            const std::string msg =
                new_high
                    ? fmt::format(lv_tr("NEW HIGH SCORE!\n{}\nTap to play again"), g_game.score)
                    : fmt::format(lv_tr("Game Over!\nScore: {}\nTap to restart"), g_game.score);
            lv_label_set_text(g_gameover_label, msg.c_str());
            lv_obj_set_style_opa(g_gameover_label, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_remove_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_gameover_label) {
            uint32_t fade_elapsed = death_elapsed - DEATH_CARD_TIME;
            uint32_t fade_duration = DEATH_INPUT_READY_TIME - DEATH_CARD_TIME; // ~300ms
            float fade =
                LV_MIN(1.0f, static_cast<float>(fade_elapsed) / static_cast<float>(fade_duration));
            lv_obj_set_style_opa(g_gameover_label, static_cast<lv_opa_t>(LV_OPA_COVER * fade),
                                 LV_PART_MAIN);
        }
    }
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

void dpad_cb(lv_event_t* e) {
    if (g_game.game_over) {
        if (g_render.death_input_ready) {
            init_game();
        }
        return;
    }
    auto* btn = lv_event_get_target_obj(e);
    if (btn == g_dpad_up)
        g_input.push_direction(Direction::UP, g_game.direction);
    else if (btn == g_dpad_down)
        g_input.push_direction(Direction::DOWN, g_game.direction);
    else if (btn == g_dpad_left)
        g_input.push_direction(Direction::LEFT, g_game.direction);
    else if (btn == g_dpad_right)
        g_input.push_direction(Direction::RIGHT, g_game.direction);
}

void touch_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &g_input.touch_start);
        g_input.swipe_handled = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (g_input.swipe_handled || g_game.game_over) {
            return;
        }

        lv_point_t current;
        lv_indev_get_point(indev, &current);

        int32_t dx = current.x - g_input.touch_start.x;
        int32_t dy = current.y - g_input.touch_start.y;
        int32_t abs_dx = LV_ABS(dx);
        int32_t abs_dy = LV_ABS(dy);

        constexpr int32_t SWIPE_THRESHOLD = 12;
        if (abs_dx < SWIPE_THRESHOLD && abs_dy < SWIPE_THRESHOLD) {
            return;
        }

        if (abs_dx > abs_dy) {
            g_input.push_direction(dx > 0 ? Direction::RIGHT : Direction::LEFT, g_game.direction);
        } else {
            g_input.push_direction(dy > 0 ? Direction::DOWN : Direction::UP, g_game.direction);
        }
        g_input.swipe_handled = true;
        // Reset touch origin for chained swipes
        lv_indev_get_point(indev, &g_input.touch_start);
    } else if (code == LV_EVENT_RELEASED) {
        if (!g_input.swipe_handled) {
            lv_point_t end;
            lv_indev_get_point(indev, &end);

            int32_t dx = end.x - g_input.touch_start.x;
            int32_t dy = end.y - g_input.touch_start.y;
            int32_t abs_dx = LV_ABS(dx);
            int32_t abs_dy = LV_ABS(dy);

            constexpr int32_t SWIPE_THRESHOLD = 12;
            if (abs_dx >= SWIPE_THRESHOLD || abs_dy >= SWIPE_THRESHOLD) {
                if (!g_game.game_over) {
                    if (abs_dx > abs_dy) {
                        g_input.push_direction(dx > 0 ? Direction::RIGHT : Direction::LEFT,
                                               g_game.direction);
                    } else {
                        g_input.push_direction(dy > 0 ? Direction::DOWN : Direction::UP,
                                               g_game.direction);
                    }
                }
            } else if (g_game.game_over && g_render.death_input_ready) {
                // Tap to restart after death animation completes
                init_game();
                if (g_game_area) {
                    lv_obj_invalidate(g_game_area);
                }
            }
        }
        g_input.swipe_handled = false;
    }
}

void input_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);

        if (key == LV_KEY_ESC) {
            // Defer destruction — never safe_delete() during input event processing
            lv_async_call([](void*) { SnakeGame::hide(); }, nullptr);
            return;
        }

        if (g_game.game_over) {
            if (g_render.death_input_ready) {
                init_game();
                if (g_game_area) {
                    lv_obj_invalidate(g_game_area);
                }
            }
            return;
        }

        switch (key) {
        case LV_KEY_UP:
            g_input.push_direction(Direction::UP, g_game.direction);
            break;
        case LV_KEY_DOWN:
            g_input.push_direction(Direction::DOWN, g_game.direction);
            break;
        case LV_KEY_LEFT:
            g_input.push_direction(Direction::LEFT, g_game.direction);
            break;
        case LV_KEY_RIGHT:
            g_input.push_direction(Direction::RIGHT, g_game.direction);
            break;
        default:
            break;
        }
    }
}

void close_cb(lv_event_t* /*e*/) {
    // Defer destruction — never safe_delete() during input event processing
    lv_async_call([](void*) { SnakeGame::hide(); }, nullptr);
}

// ============================================================================
// D-PAD CREATION
// ============================================================================

// MDI chevron codepoints for D-pad arrows (use DPAD_ prefix to avoid
// collision with ICON_CHEVRON_* macros from ui_fonts.h)
static constexpr const char* DPAD_CHEVRON_UP = "\xF3\xB0\x85\x83";    // F0143
static constexpr const char* DPAD_CHEVRON_DOWN = "\xF3\xB0\x85\x80";  // F0140
static constexpr const char* DPAD_CHEVRON_LEFT = "\xF3\xB0\x85\x81";  // F0141
static constexpr const char* DPAD_CHEVRON_RIGHT = "\xF3\xB0\x85\x82"; // F0142

lv_obj_t* create_dpad_button(lv_obj_t* parent, const char* icon_utf8, lv_align_t align,
                             int32_t x_ofs, int32_t y_ofs) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 48, 48);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(btn, dpad_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, icon_utf8);
    lv_obj_set_style_text_font(label, theme_manager_get_font("icon_font_sm"), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, LV_OPA_50, LV_PART_MAIN);
    lv_obj_center(label);

    return btn;
}

void create_dpad(lv_obj_t* parent) {
    // D-pad at bottom center of game area
    int32_t cx = 0;
    int32_t base_y = -20;

    g_dpad_up = create_dpad_button(parent, DPAD_CHEVRON_UP, LV_ALIGN_BOTTOM_MID, cx, base_y - 100);
    g_dpad_down = create_dpad_button(parent, DPAD_CHEVRON_DOWN, LV_ALIGN_BOTTOM_MID, cx, base_y);
    g_dpad_left =
        create_dpad_button(parent, DPAD_CHEVRON_LEFT, LV_ALIGN_BOTTOM_MID, cx - 52, base_y - 50);
    g_dpad_right =
        create_dpad_button(parent, DPAD_CHEVRON_RIGHT, LV_ALIGN_BOTTOM_MID, cx + 52, base_y - 50);
}

// ============================================================================
// OVERLAY LIFECYCLE
// ============================================================================

void create_overlay() {
    if (g_overlay) {
        spdlog::warn("[SnakeGame] Overlay already exists");
        return;
    }

    spdlog::info("[SnakeGame] Launching snake game!");

    load_high_score();
    srand(static_cast<unsigned>(time(nullptr)));
    detect_input_mode();

    // Opaque backdrop on top layer
    lv_obj_t* parent = lv_layer_top();
    g_overlay = lv_obj_create(parent);
    lv_obj_set_size(g_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(g_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_overlay, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(g_overlay, 4, LV_PART_MAIN);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* content = g_overlay;

    // === Header row (score + close button) ===
    lv_obj_t* header = lv_obj_create(content);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Score label
    g_score_label = lv_label_create(header);
    lv_obj_set_style_text_color(g_score_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_score_label, theme_manager_get_font("font_heading"), LV_PART_MAIN);
    lv_label_set_text(g_score_label, "Score: 0");

    // Close button (X)
    g_close_btn = lv_button_create(header);
    lv_obj_set_size(g_close_btn, 36, 36);
    lv_obj_set_style_bg_color(g_close_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_close_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_close_btn, 18, LV_PART_MAIN);
    lv_obj_add_event_cb(g_close_btn, close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* close_label = lv_label_create(g_close_btn);
    lv_label_set_text(close_label, "\xF3\xB0\x85\x96"); // MDI close/xmark (F0156)
    lv_obj_set_style_text_color(close_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_label, theme_manager_get_font("icon_font_sm"), LV_PART_MAIN);
    lv_obj_center(close_label);

    // === Game area ===
    g_game_area = lv_obj_create(content);
    lv_obj_set_style_bg_opa(g_game_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_game_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_game_area, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(g_game_area, 1);
    lv_obj_set_width(g_game_area, LV_PCT(100));
    lv_obj_remove_flag(g_game_area, LV_OBJ_FLAG_SCROLLABLE);

    // Calculate grid dimensions from available space
    lv_coord_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screen_h = lv_display_get_vertical_resolution(nullptr);

    lv_coord_t avail_w = screen_w - 24;
    lv_coord_t avail_h = screen_h - 64;

    g_grid.cols = avail_w / CELL_SIZE;
    g_grid.rows = avail_h / CELL_SIZE;
    g_grid.offset_x = (avail_w - g_grid.cols * CELL_SIZE) / 2;
    g_grid.offset_y = (avail_h - g_grid.rows * CELL_SIZE) / 2;

    spdlog::debug("[SnakeGame] Grid: {}x{} cells, offset: ({}, {})", g_grid.cols, g_grid.rows,
                  g_grid.offset_x, g_grid.offset_y);

    // Register custom draw callback
    lv_obj_add_event_cb(g_game_area, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    // Register touch input
    lv_obj_add_flag(g_game_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_game_area, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(g_game_area, touch_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g_game_area, touch_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(g_game_area, touch_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(g_game_area, input_cb, LV_EVENT_KEY, nullptr);

    // Add to default group for keyboard input
    lv_group_t* group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, g_game_area);
        lv_group_focus_obj(g_game_area);
    }

    // === Game over overlay label ===
    g_gameover_label = lv_label_create(content);
    lv_obj_set_style_text_color(g_gameover_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_gameover_label, theme_manager_get_font("font_heading"),
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(g_gameover_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(g_gameover_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_gameover_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(g_gameover_label, LV_OBJ_FLAG_CLICKABLE);

    // Create D-pad if in DPAD mode
    if (g_input.mode == InputMode::DPAD) {
        create_dpad(g_game_area);
    }

    // Bring overlay to front
    lv_obj_move_foreground(g_overlay);

    // Initialize game state
    init_game();

    // Start render timer
    g_render.last_render_ms = lv_tick_get();
    g_render_timer = lv_timer_create(render_tick, RENDER_TICK_MS, nullptr);

    spdlog::info("[SnakeGame] Game started! Grid: {}x{}, input: {}", g_grid.cols, g_grid.rows,
                 g_input.mode == InputMode::DPAD ? "DPAD" : "SWIPE");
}

void destroy_overlay() {
    if (g_render_timer) {
        lv_timer_delete(g_render_timer);
        g_render_timer = nullptr;
    }

    // Remove from focus group before deletion
    if (g_game_area) {
        lv_group_t* group = lv_group_get_default();
        if (group) {
            lv_group_remove_obj(g_game_area);
        }
    }

    // Clean up overlay
    helix::ui::safe_delete(g_overlay);
    g_game_area = nullptr;
    g_score_label = nullptr;
    g_gameover_label = nullptr;
    g_close_btn = nullptr;
    g_dpad_up = nullptr;
    g_dpad_down = nullptr;
    g_dpad_left = nullptr;
    g_dpad_right = nullptr;

    // Reset state
    g_game.snake.clear();
    g_game.prev_snake.clear();
    g_game.game_started = false;
    g_game.game_over = false;
    g_grid.free_cells.clear();
    g_input.swipe_handled = false;
    g_input.queue_count = 0;

    stop_music();
    spdlog::info("[SnakeGame] Game closed");
}

} // anonymous namespace

// ============================================================================
// PUBLIC API
// ============================================================================

void SnakeGame::show() {
    if (g_overlay) {
        spdlog::debug("[SnakeGame] Already visible");
        return;
    }
    create_overlay();
}

void SnakeGame::hide() {
    destroy_overlay();
}

bool SnakeGame::is_visible() {
    return g_overlay != nullptr;
}

} // namespace helix
