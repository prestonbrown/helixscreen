// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_confetti.h"

#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace {

// Physics constants
constexpr float kGravity = 400.0f;        // Pixels per second squared
constexpr float kAirResistance = 0.98f;   // Velocity multiplier per frame
constexpr float kFadeStart = 0.7f;        // Start fading at 70% lifetime
constexpr float kParticleLifetime = 3.0f; // Seconds
constexpr int MAX_PARTICLES = 60;         // Keep particle count reasonable

// Celebration colors (vibrant rainbow)
const lv_color_t kConfettiColors[] = {
    lv_color_hex(0xFF6B6B), // Red
    lv_color_hex(0xFFE66D), // Yellow
    lv_color_hex(0x4ECDC4), // Teal
    lv_color_hex(0x45B7D1), // Blue
    lv_color_hex(0x96E6A1), // Green
    lv_color_hex(0xDDA0DD), // Plum
    lv_color_hex(0xF7DC6F), // Gold
    lv_color_hex(0xAED6F1), // Light blue
};
constexpr int kNumColors = sizeof(kConfettiColors) / sizeof(kConfettiColors[0]);

struct Particle {
    lv_obj_t* obj = nullptr;
    float x, y;           // Position
    float vx, vy;         // Velocity
    float rotation;       // Current rotation angle
    float rotation_speed; // Rotation velocity
    float life;           // Remaining lifetime (0-1)
    float base_w, base_h; // Base dimensions
};

struct ConfettiData {
    lv_obj_t* container = nullptr;
    lv_timer_t* timer = nullptr;
    std::vector<Particle> particles;
    int32_t width = 0;
    int32_t height = 0;
    std::mt19937 rng;
    uint32_t last_tick = 0;
};

static std::unordered_map<lv_obj_t*, ConfettiData*> s_registry;

ConfettiData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

float random_float(std::mt19937& rng, float min, float max) {
    std::uniform_real_distribution<float> dist(min, max);
    return dist(rng);
}

int random_int(std::mt19937& rng, int min, int max) {
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

void update_particles(ConfettiData* data) {
    if (!data || !data->container)
        return;

    uint32_t now = lv_tick_get();
    float dt = (now - data->last_tick) / 1000.0f;
    data->last_tick = now;
    dt = std::min(dt, 0.1f);

    bool any_alive = false;
    for (auto& p : data->particles) {
        if (p.life <= 0)
            continue;

        // Physics
        p.vy += kGravity * dt;
        p.vx *= kAirResistance;
        p.vy *= kAirResistance;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.rotation += p.rotation_speed * dt;
        p.life -= dt / kParticleLifetime;

        // Bounce off bottom
        if (p.y > data->height - p.base_h) {
            p.y = data->height - p.base_h;
            p.vy = -p.vy * 0.3f;
            p.vx *= 0.8f;
        }

        if (p.life <= 0) {
            // Hide dead particles
            lv_obj_add_flag(p.obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        any_alive = true;

        // Update position
        lv_obj_set_pos(p.obj, (int32_t)p.x, (int32_t)p.y);

        // Simulate rotation by varying width/height
        float rot_factor = fabsf(cosf(p.rotation));
        int32_t w = std::max((int32_t)(p.base_w * (0.4f + 0.6f * rot_factor)), (int32_t)2);
        int32_t h = std::max((int32_t)(p.base_h * (0.4f + 0.6f * (1.0f - rot_factor))), (int32_t)2);
        lv_obj_set_size(p.obj, w, h);

        // Fade out near end of life
        if (p.life < kFadeStart) {
            lv_opa_t opa = (lv_opa_t)(255 * (p.life / kFadeStart));
            lv_obj_set_style_opa(p.obj, opa, LV_PART_MAIN);
        }
    }

    // All particles dead — clean up
    if (!any_alive && data->timer) {
        lv_timer_delete(data->timer);
        data->timer = nullptr;

        // Delete the whole container (takes all particle objects with it).
        // Reparent-first via safe_delete_deferred_raw: the container's original
        // parent (an arbitrary panel) may be lv_obj_clean()'d before the async
        // delete fires, which would otherwise cascade into a freed event list.
        lv_obj_t* container = data->container;
        s_registry.erase(container);
        delete data;
        helix::ui::safe_delete_deferred_raw(container);
        spdlog::debug("[Confetti] Animation complete, cleaned up");
    }
}

void timer_cb(lv_timer_t* timer) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    ConfettiData* data = get_data(obj);
    if (data) {
        update_particles(data);
    }
}

void delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    ConfettiData* data = get_data(obj);
    if (data) {
        if (data->timer) {
            lv_timer_delete(data->timer);
            data->timer = nullptr;
        }
        s_registry.erase(obj);
        delete data;
        spdlog::trace("[Confetti] Destroyed via delete event");
    }
}

lv_obj_t* confetti_create_internal(lv_obj_t* parent) {
    // Use display resolution for sizing
    lv_display_t* disp = lv_display_get_default();
    int32_t width = lv_display_get_horizontal_resolution(disp);
    int32_t height = lv_display_get_vertical_resolution(disp);

    // Create transparent click-through container
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, width, height);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(container, LV_OBJ_FLAG_FLOATING);

    ConfettiData* data = new ConfettiData();
    data->container = container;
    data->width = width;
    data->height = height;
    data->rng = std::mt19937(std::random_device{}());
    data->last_tick = lv_tick_get();

    s_registry[container] = data;
    lv_obj_add_event_cb(container, delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[Confetti] Created {}x{} container on parent", width, height);
    return container;
}

void* confetti_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    return confetti_create_internal(parent);
}

void confetti_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);
}

} // namespace

lv_obj_t* ui_confetti_create(lv_obj_t* parent) {
    return confetti_create_internal(parent);
}

void ui_confetti_burst(lv_obj_t* confetti, int count) {
    ConfettiData* data = get_data(confetti);
    if (!data) {
        spdlog::warn("[Confetti] burst called on invalid object");
        return;
    }

    // Cap particle count for performance
    count = std::min(count, MAX_PARTICLES);

    // Spawn point (top center)
    float spawn_x = data->width / 2.0f;
    float spawn_y = data->height * 0.15f;

    data->particles.reserve(data->particles.size() + count);

    for (int i = 0; i < count; i++) {
        Particle p;

        // Create the visual object
        p.obj = lv_obj_create(data->container);
        lv_obj_remove_style_all(p.obj);
        lv_obj_remove_flag(p.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(p.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(p.obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_add_flag(p.obj, LV_OBJ_FLAG_FLOATING);

        // Random color
        lv_color_t color = kConfettiColors[random_int(data->rng, 0, kNumColors - 1)];
        lv_obj_set_style_bg_color(p.obj, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(p.obj, LV_OPA_COVER, LV_PART_MAIN);

        // Random shape via border radius
        int shape = random_int(data->rng, 0, 2);
        if (shape == 2) {
            lv_obj_set_style_radius(p.obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        } else {
            lv_obj_set_style_radius(p.obj, 2, LV_PART_MAIN);
        }

        // Size
        p.base_w = random_float(data->rng, 8, 16);
        p.base_h = (shape == 0) ? p.base_w * 0.5f : p.base_w; // Rectangles are wider
        lv_obj_set_size(p.obj, (int32_t)p.base_w, (int32_t)p.base_h);

        // Position
        p.x = spawn_x + random_float(data->rng, -60, 60);
        p.y = spawn_y + random_float(data->rng, -30, 30);
        lv_obj_set_pos(p.obj, (int32_t)p.x, (int32_t)p.y);

        // Velocity — fan out in all upward directions
        float angle = random_float(data->rng, -2.8f, -0.3f);
        float speed = random_float(data->rng, 250, 550);
        p.vx = cosf(angle) * speed * random_float(data->rng, 0.5f, 1.5f);
        p.vy = sinf(angle) * speed;

        p.rotation = random_float(data->rng, 0, 6.28f);
        p.rotation_speed = random_float(data->rng, -8.0f, 8.0f);
        p.life = 1.0f;

        data->particles.push_back(p);
    }

    // Start animation timer if not running
    if (!data->timer) {
        data->last_tick = lv_tick_get();
        data->timer = lv_timer_create(timer_cb, 16, confetti); // ~60 FPS
        spdlog::debug("[Confetti] Burst {} particles", count);
    }
}

void ui_confetti_clear(lv_obj_t* confetti) {
    ConfettiData* data = get_data(confetti);
    if (!data)
        return;

    if (data->timer) {
        lv_timer_delete(data->timer);
        data->timer = nullptr;
    }

    // Hide all particles
    for (auto& p : data->particles) {
        if (p.obj) {
            lv_obj_add_flag(p.obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    data->particles.clear();
}

void ui_confetti_init() {
    lv_xml_register_widget("ui_confetti", confetti_xml_create, confetti_apply);
    spdlog::trace("[Confetti] Registered ui_confetti widget");
}
