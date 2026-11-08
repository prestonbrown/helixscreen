// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "ui_utils.h"

#include "lv_draw_buf_guard.h"
#include "screensaver_image.h"
#include "screensaver_print_colors.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>

using helix::ui::ClockMode;
using helix::ui::DirtyRect;
using helix::ui::screensaver::flap_frame_at;
using helix::ui::screensaver::flight_pos_at;
using helix::ui::screensaver::FlightPos;

// Sprite asset paths
static constexpr const char* kToasterFrames[] = {
    "A:assets/images/screensaver/toaster_0.png",
    "A:assets/images/screensaver/toaster_1.png",
    "A:assets/images/screensaver/toaster_2.png",
    "A:assets/images/screensaver/toaster_3.png",
};
static constexpr const char* kToastImg = "A:assets/images/screensaver/toast.png";
static constexpr int kNumToasterFrames = sizeof(kToasterFrames) / sizeof(kToasterFrames[0]);

// Each shade multiplies the toast's own colours, light to dark, as a print goes; the last is
// a failed print's burnt toast. Animation content, not UI chrome, so not theme tokens.
static constexpr helix::ui::Rgb kToastShadeTints[] = {
    {255, 236, 200}, {238, 200, 150}, {214, 160, 104}, {184, 122, 72},
    {150, 92, 52},   {116, 68, 38},   {44, 36, 32},
};
static_assert(std::size(kToastShadeTints) == FlyingToasterScreensaver::kToastShadeCount + 1);

// A finished print pops a toast this often, and each pop rises and falls over this long.
static constexpr uint32_t kToastPopPeriodMs = 3000;
static constexpr uint32_t kToastPopMs = 1200;
// Peak height of a pop, as a fraction of the sprite.
static constexpr float kToastPopRise = 0.9f;

// CSS reference: translate(-1600px, 1600px) — fixed travel distance
static constexpr int kFlightDistance = 1600;

// Each wing frame holds this long on a 10 s or 16 s flight, and twice as long on a
// 24 s flight: slower flight, slower flap.
static constexpr int kFlapStepMs = 50;

// Object definition matching the exact CSS classes and positions.
// CSS uses right/top percentages. Negative right = off-screen to the right.
// Negative top = off-screen above.
//
// Timing classes from CSS:
//   t1: 10s,  0s delay, alternate
//   t2: 16s,  0s delay, alternate-reverse
//   t3: 24s,  0s delay, alternate
//   t4: 10s,  5s delay, alternate
//   t5: 24s,  4s delay, alternate-reverse
//   t6: 24s,  8s delay, alternate
//   t7: 24s, 12s delay, alternate-reverse
//   t8: 24s, 16s delay, alternate
//   t9: 24s, 20s delay, alternate-reverse
//   tst1: 10s, 0s delay (toast)
//   tst2: 16s, 0s delay (toast)
//   tst3: 24s, 0s delay (toast)
//   tst4: 24s, 12s delay (toast)

struct ObjectDef {
    float right_pct; // CSS right percentage (negative = off-screen right)
    float top_pct;   // CSS top percentage (negative = off-screen above)
    bool is_toaster;
    bool reverse_flap; // alternate-reverse direction
    int fly_ms;        // flight animation duration
    int delay_ms;      // flight start delay
};

// Exact objects from the CSS HTML, in order
static constexpr ObjectDef kObjects[] = {
    // First group
    {-0.02f, -0.17f, true, false, 10000, 0}, // t1 p6
    {0.10f, -0.19f, true, false, 24000, 0},  // t3 p7
    {0.20f, -0.18f, false, false, 10000, 0}, // tst1 p8
    {0.30f, -0.20f, true, false, 24000, 0},  // t3 p9
    {0.50f, -0.18f, true, false, 10000, 0},  // t1 p11
    {0.60f, -0.20f, true, false, 24000, 0},  // t3 p12
    {-0.17f, 0.10f, true, true, 16000, 0},   // t2 p13
    {-0.19f, 0.20f, false, false, 24000, 0}, // tst3 p14
    {-0.23f, 0.50f, false, false, 16000, 0}, // tst2 p16
    {-0.25f, 0.70f, true, false, 10000, 0},  // t1 p17
    {0.10f, -0.20f, false, false, 16000, 0}, // tst2 p19
    {0.20f, -0.36f, false, false, 24000, 0}, // tst3 p20
    {0.30f, -0.24f, true, true, 16000, 0},   // t2 p21
    {-0.26f, 0.10f, false, false, 10000, 0}, // tst1 p24
    {0.40f, -0.33f, true, false, 10000, 0},  // t1 p22
    {-0.29f, 0.50f, false, false, 16000, 0}, // tst2 p26
    {0.10f, -0.56f, true, false, 10000, 0},  // t1 p28
    {0.30f, -0.60f, false, false, 16000, 0}, // tst2 p30
    {-0.46f, 0.10f, true, true, 16000, 0},   // t2 p31
    {-0.56f, 0.20f, true, false, 10000, 0},  // t1 p32
    {-0.49f, 0.30f, false, false, 24000, 0}, // tst3 p33

    // Wave 1: t4 (fast delayed) — 10s, 5s delay
    {0.00f, -0.46f, true, false, 10000, 5000}, // t4 p27
    {0.40f, -0.21f, true, false, 10000, 5000}, // t4 p10
    {-0.36f, 0.30f, true, false, 10000, 5000}, // t4 p25
    {0.20f, -0.49f, true, false, 10000, 5000}, // t4 p29

    // Wave 2: t5 — 24s, 4s delay, alternate-reverse
    {-0.21f, 0.30f, true, true, 24000, 4000}, // t5 p15
    {0.00f, -0.26f, true, true, 24000, 4000}, // t5 p18
    {0.40f, -0.33f, true, true, 24000, 4000}, // t5 p22

    // Wave 3: t6 — 24s, 8s delay, alternate
    {-0.02f, -0.17f, true, false, 24000, 8000}, // t6 p6
    {0.50f, -0.18f, true, false, 24000, 8000},  // t6 p11
    {-0.21f, 0.30f, true, false, 24000, 8000},  // t6 p15
    {0.10f, -0.20f, true, false, 24000, 8000},  // t6 p19
    {0.60f, -0.40f, true, false, 24000, 8000},  // t6 p23

    // Delayed toast: tst4 — 24s, 12s delay
    {0.40f, -0.21f, false, false, 24000, 12000}, // tst4 p10
    {0.60f, -0.40f, false, false, 24000, 12000}, // tst4 p23
    {-0.21f, 0.30f, false, false, 24000, 12000}, // tst4 p15

    // Wave 4: t7 — 24s, 12s delay, alternate-reverse
    {0.10f, -0.19f, true, true, 24000, 12000}, // t7 p7
    {0.60f, -0.20f, true, true, 24000, 12000}, // t7 p12
    {-0.23f, 0.50f, true, true, 24000, 12000}, // t7 p16
    {0.20f, -0.36f, true, true, 24000, 12000}, // t7 p20
    {-0.26f, 0.10f, true, true, 24000, 12000}, // t7 p24

    // Wave 5: t8 — 24s, 16s delay, alternate
    {0.20f, -0.18f, true, false, 24000, 16000}, // t8 p8
    {-0.17f, 0.10f, true, false, 24000, 16000}, // t8 p13
    {-0.25f, 0.70f, true, false, 24000, 16000}, // t8 p17
    {-0.36f, 0.30f, true, false, 24000, 16000}, // t8 p25

    // Wave 6: t9 — 24s, 20s delay, alternate-reverse
    {-0.19f, 0.20f, true, true, 24000, 20000}, // t9 p14
    {0.00f, -0.26f, true, true, 24000, 20000}, // t9 p18
    {0.30f, -0.24f, true, true, 24000, 20000}, // t9 p21
    {-0.29f, 0.50f, true, true, 24000, 20000}, // t9 p26
};
static constexpr int NUM_OBJECTS = sizeof(kObjects) / sizeof(kObjects[0]);

namespace helix::ui {
namespace {

struct ToasterLevel {
    uint32_t period_ms; // frame period in ms
    int sprites;        // sprites flown, a prefix of kObjects[]
};

// The ladder, most expensive level first. Every visible sprite costs dirty-region work each
// frame, and a halved rate that still flies every sprite saves little, so the rungs past half
// rate thin the flock rather than slow it again. kObjects[] is ordered by delay and wave, so a
// prefix keeps a representative mix.
constexpr ToasterLevel kToasterLevels[] = {
    {kSaverFastPeriod, NUM_OBJECTS},
    {33, NUM_OBJECTS},
    {33, 25},
    {33, 10},
};

} // namespace
} // namespace helix::ui

using helix::ui::kToasterLevels;

size_t FlyingToasterScreensaver::ladder_size() const {
    return std::size(kToasterLevels);
}

uint32_t FlyingToasterScreensaver::ladder_period_ms(size_t level) const {
    return helix::ui::ladder_period(kToasterLevels, level);
}

int FlyingToasterScreensaver::get_scale_factor() const {
    lv_display_t* disp = lv_display_get_default();
    if (!disp)
        return 256; // 1x in LVGL 256 = 100%
    int w = lv_display_get_horizontal_resolution(disp);
    return (w > 800) ? 512 : 256; // 2x on larger displays
}

bool FlyingToasterScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting flying toasters");

    m_elapsed_ms = 0;
    decode_sprites();
    build_toast_shades();
    m_toast_shade = -1;
    if (print_reading().mode != ClockMode::CLOCK) {
        apply_toast_shade();
    }

    // DECLARATIVE_OK: a C++-built sprite, created first so a pop rises from behind its toaster
    m_pop = lv_image_create(overlay().obj());
    lv_image_set_src(m_pop, toast_buffer(m_toast_shade));
    if (get_scale_factor() != 256) {
        lv_image_set_scale(m_pop, get_scale_factor());
    }
    lv_obj_set_floating(m_pop, true);
    lv_obj_set_clickable(m_pop, false);
    lv_obj_set_hidden(m_pop, true);
    m_pop_from = -1;
    m_pop_ms = 0;
    m_since_pop_ms = kToastPopPeriodMs; // a finished print pops at once

    spawn_objects(sprite_limit(level()));
    return true;
}

void FlyingToasterScreensaver::on_stop() {
    // The sprites are children of the overlay the base has already queued for deletion; they
    // let go of the buffers before those are freed.
    for (auto& obj : m_objects) {
        if (obj.img) {
            lv_image_set_src(obj.img, nullptr);
        }
    }
    if (m_pop) {
        lv_image_set_src(m_pop, nullptr);
    }
    m_pop = nullptr;
    m_objects.clear();
    free_sprites();
}

void FlyingToasterScreensaver::on_level_request(size_t level) {
    apply_level(level);
    drop_sprites_past(sprite_limit(this->level()));
}

size_t FlyingToasterScreensaver::sprite_limit(size_t level) const {
    return static_cast<size_t>(std::min(
        NUM_OBJECTS, kToasterLevels[std::min(level, std::size(kToasterLevels) - 1)].sprites));
}

void FlyingToasterScreensaver::drop_sprites_past(size_t limit) {
    if (m_objects.size() <= limit) {
        return;
    }
    // Each sprite is hidden now and freed on the next timer pass.
    for (size_t i = limit; i < m_objects.size(); i++) {
        helix::ui::safe_delete_deferred(m_objects[i].img);
    }
    m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(limit), m_objects.end());
}

void FlyingToasterScreensaver::spawn_objects(size_t count) {
    lv_display_t* disp = lv_display_get_default();
    if (!disp)
        return;

    int screen_w = lv_display_get_horizontal_resolution(disp);
    int screen_h = lv_display_get_vertical_resolution(disp);
    int obj_size = 64;
    int scale = get_scale_factor();
    if (scale != 256) {
        obj_size = obj_size * scale / 256;
    }

    const int active_count = std::min(NUM_OBJECTS, static_cast<int>(count));

    m_objects.reserve(active_count);

    for (int i = 0; i < active_count; i++) {
        const auto& def = kObjects[i];

        // Convert CSS right/top percentages to LVGL left/top coordinates.
        // CSS: right: R% means element's right edge is R% from container right.
        //   Negative right = off-screen to the right.
        //   left = screen_w * (1.0 - right_pct) - obj_size
        // CSS: top: T% means element's top edge is T% from container top.
        //   Negative top = above the viewport.
        //   y = screen_h * top_pct
        int start_x = static_cast<int>(screen_w * (1.0f - def.right_pct)) - obj_size;
        int start_y = static_cast<int>(screen_h * def.top_pct);

        create_flying_object(start_x, start_y, def.is_toaster, def.reverse_flap, def.fly_ms,
                             def.delay_ms);
    }

    spdlog::debug("[Screensaver] Spawned {}/{} flying objects ({}x{} screen, {}px sprites)",
                  m_objects.size(), NUM_OBJECTS, screen_w, screen_h, obj_size);
}

void FlyingToasterScreensaver::create_flying_object(int start_x, int start_y, bool is_toaster,
                                                    bool reverse_flap, int speed_ms, int delay_ms) {
    if (!overlay().obj())
        return;

    lv_obj_t* img = lv_image_create(overlay().obj());

    // Set initial image from pre-decoded RAM buffers
    uint8_t initial_frame = reverse_flap ? 2 : 0;
    if (is_toaster) {
        lv_image_set_src(img, m_decoded_frames[initial_frame]);
    } else {
        lv_image_set_src(img, toast_buffer(m_toast_shade));
    }

    // Scale on larger displays
    int scale = get_scale_factor();
    if (scale != 256) {
        lv_image_set_scale(img, scale);
    }

    // Position absolutely (floating, out of any layout)
    lv_obj_add_flag(img, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(img, start_x, start_y);

    FlyingObject obj{};
    obj.img = img;
    obj.is_toaster = is_toaster;
    obj.start_x = static_cast<int16_t>(start_x);
    obj.start_y = static_cast<int16_t>(start_y);
    obj.fly_ms = speed_ms;
    obj.delay_ms = delay_ms;
    obj.initial_frame = initial_frame;
    obj.flap_frame = initial_frame;
    // 10 s and 16 s flights hold each wing frame kFlapStepMs, 24 s flights twice that
    obj.flap_step_ms = static_cast<uint16_t>(kFlapStepMs * std::max(1, speed_ms / 10000));
    m_objects.push_back(obj);
}

// Sprites are LVGL images, which invalidate their own old and new areas when they move or
// change frame, so a frame adds nothing to the dirty list.
void FlyingToasterScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& /*dirty*/) {
    m_elapsed_ms += dt_ms;

    lv_display_t* disp = lv_display_get_default();
    int screen_w = disp ? lv_display_get_horizontal_resolution(disp) : 800;
    int screen_h = disp ? lv_display_get_vertical_resolution(disp) : 480;
    int obj_size = 64;
    int scale = get_scale_factor();
    if (scale != 256) {
        obj_size = obj_size * scale / 256;
    }

    apply_toast_shade();

    for (auto& obj : m_objects) {
        if (!obj.img)
            continue;

        // Position is a function of elapsed time, never of how often the timer fired
        const FlightPos pos = flight_pos_at(m_elapsed_ms, obj.start_x, obj.start_y, obj.fly_ms,
                                            obj.delay_ms, kFlightDistance);

        // Skip objects still in their start delay
        if (!pos.started) {
            continue;
        }
        auto new_x = static_cast<int16_t>(pos.x);
        auto new_y = static_cast<int16_t>(pos.y);

        // Hide objects that are entirely off-screen (LVGL skips hidden objects in render)
        bool on_screen =
            (new_x + obj_size > 0 && new_x < screen_w && new_y + obj_size > 0 && new_y < screen_h);
        if (!on_screen) {
            if (!lv_obj_has_flag(obj.img, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(obj.img, LV_OBJ_FLAG_HIDDEN);
            obj.prev_x = new_x;
            obj.prev_y = new_y;
            continue;
        }
        if (lv_obj_has_flag(obj.img, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(obj.img, LV_OBJ_FLAG_HIDDEN);

        if (new_x != obj.prev_x || new_y != obj.prev_y) {
            lv_obj_set_pos(obj.img, new_x, new_y);
            obj.prev_x = new_x;
            obj.prev_y = new_y;
        }

        // Flap wing frames (toasters only)
        if (!obj.is_toaster)
            continue;

        // Wing frame cycles 0→1→2→3→2→1 on elapsed time
        const uint8_t frame =
            flap_frame_at(m_elapsed_ms, obj.delay_ms, obj.flap_step_ms, obj.initial_frame);

        // Only update image source when frame actually changed (RAM buffer, no file I/O)
        if (frame != obj.flap_frame) {
            obj.flap_frame = frame;
            lv_image_set_src(obj.img, m_decoded_frames[frame]);
        }
    }

    step_pop(dt_ms, screen_w, screen_h, obj_size);
}

// DECLARATIVE_OK: per-frame print state on C++-built sprites
void FlyingToasterScreensaver::apply_toast_shade() {
    const helix::ui::ClockReading& reading = print_reading();
    int shade = -1;
    if (reading.mode == ClockMode::FAILED || reading.mode == ClockMode::CANCELLED) {
        shade = kToastBurnt;
    } else if (reading.mode != ClockMode::CLOCK) {
        shade = std::clamp(reading.progress_pct * kToastShadeCount / 100, 0, kToastShadeCount - 1);
    }
    if (shade == m_toast_shade) {
        return;
    }
    m_toast_shade = shade;
    lv_draw_buf_t* art = toast_buffer(shade);
    for (auto& obj : m_objects) {
        if (obj.img && !obj.is_toaster) {
            lv_image_set_src(obj.img, art);
        }
    }
    if (m_pop) {
        lv_image_set_src(m_pop, art);
    }
}

// DECLARATIVE_OK: per-frame motion of a C++-built sprite
void FlyingToasterScreensaver::step_pop(uint32_t dt_ms, int screen_w, int screen_h, int obj_size) {
    if (!m_pop) {
        return;
    }
    const auto end_pop = [this] {
        m_pop_from = -1;
        lv_obj_set_hidden(m_pop, true);
    };
    if (print_reading().mode != ClockMode::DONE) {
        end_pop();
        m_since_pop_ms = kToastPopPeriodMs;
        return;
    }

    m_since_pop_ms += dt_ms;
    if (m_pop_from >= 0) {
        m_pop_ms += dt_ms;
    } else if (m_since_pop_ms >= kToastPopPeriodMs) {
        // The first toaster wholly on screen pops; with none, the next frame looks again.
        for (size_t i = 0; i < m_objects.size(); i++) {
            const FlyingObject& obj = m_objects[i];
            if (obj.is_toaster && obj.img && !lv_obj_is_hidden(obj.img) && obj.prev_x >= 0 &&
                obj.prev_y >= 0 && obj.prev_x + obj_size <= screen_w &&
                obj.prev_y + obj_size <= screen_h) {
                m_pop_from = static_cast<int>(i);
                m_pop_ms = 0;
                m_since_pop_ms = 0;
                break;
            }
        }
    }
    if (m_pop_from < 0) {
        return;
    }

    const auto from = static_cast<size_t>(m_pop_from);
    if (m_pop_ms >= kToastPopMs || from >= m_objects.size() || !m_objects[from].img ||
        lv_obj_is_hidden(m_objects[from].img)) {
        end_pop();
        return;
    }
    // Up and back down on a parabola, riding along with its toaster.
    const float t = static_cast<float>(m_pop_ms) / static_cast<float>(kToastPopMs);
    const int rise =
        static_cast<int>(kToastPopRise * static_cast<float>(obj_size) * 4.0f * t * (1.0f - t));
    const FlyingObject& toaster = m_objects[from];
    if (rise <= 0) {
        lv_obj_set_hidden(m_pop, true);
        return;
    }
    lv_obj_set_pos(m_pop, toaster.prev_x, toaster.prev_y - rise);
    lv_obj_set_hidden(m_pop, false);
}

void FlyingToasterScreensaver::decode_sprites() {
    // Pre-decode each unique PNG into a persistent draw buffer.
    // When used as lv_image_set_src(), LVGL treats these as LV_IMAGE_SRC_VARIABLE —
    // no file open, no PNG decompression, just direct pixel data.
    for (int i = 0; i < kNumToasterFrames; i++) {
        m_decoded_frames[i] = helix::ui::saver_decode_image(kToasterFrames[i], "toaster frame");
    }
    m_decoded_toast = helix::ui::saver_decode_image(kToastImg, "toast");

    spdlog::debug("[Screensaver] Pre-decoded {} toaster frames + toast sprite into RAM",
                  kNumToasterFrames);
}

void FlyingToasterScreensaver::free_sprites() {
    // The sprites are live image sources; a blend of the saver's last frame
    // may still be in flight when the overlay is torn down.
    for (auto& buf : m_decoded_frames) {
        helix::safe_draw_buf_destroy(buf, "toaster");
    }
    for (auto& buf : m_toast_shades) {
        helix::safe_draw_buf_destroy(buf, "toast_shade");
    }
    helix::safe_draw_buf_destroy(m_decoded_toast, "toast");
}

void FlyingToasterScreensaver::build_toast_shades() {
    if (!m_decoded_toast || !helix::ui::saver_rows_mappable(m_decoded_toast->header.cf)) {
        return;
    }
    const auto rows = static_cast<int32_t>(m_decoded_toast->header.h);
    for (size_t s = 0; s < std::size(m_toast_shades); s++) {
        m_toast_shades[s] = lv_draw_buf_dup(m_decoded_toast);
        if (!m_toast_shades[s]) {
            continue;
        }
        const helix::ui::Rgb tint = kToastShadeTints[s];
        helix::ui::saver_map_rows(*m_decoded_toast, *m_toast_shades[s], 0, rows,
                                  [tint](lv_color32_t p) {
                                      p.red = static_cast<uint8_t>(p.red * tint.r / 255);
                                      p.green = static_cast<uint8_t>(p.green * tint.g / 255);
                                      p.blue = static_cast<uint8_t>(p.blue * tint.b / 255);
                                      return p;
                                  });
    }
}

lv_draw_buf_t* FlyingToasterScreensaver::toast_buffer(int shade) const {
    if (shade < 0 || shade > kToastBurnt || !m_toast_shades[shade]) {
        return m_decoded_toast;
    }
    return m_toast_shades[shade];
}

const lv_draw_buf_t* FlyingToasterScreensaver::toast_art(int shade) const {
    return toast_buffer(shade);
}

#endif // HELIX_ENABLE_SCREENSAVER
