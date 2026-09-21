// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "ui_timer_guard.h" // lv_timer_cancel_safe
#include "ui_utils.h"

#include "lv_draw_buf_guard.h"
#include "platform_capabilities.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <draw/lv_image_decoder_private.h>

// Sprite asset paths
static constexpr const char* TOASTER_FRAMES[] = {
    "A:assets/images/screensaver/toaster_0.png",
    "A:assets/images/screensaver/toaster_1.png",
    "A:assets/images/screensaver/toaster_2.png",
    "A:assets/images/screensaver/toaster_3.png",
};
static constexpr const char* TOAST_IMG = "A:assets/images/screensaver/toast.png";
static constexpr int NUM_TOASTER_FRAMES = sizeof(TOASTER_FRAMES) / sizeof(TOASTER_FRAMES[0]);

// CSS reference: translate(-1600px, 1600px) — fixed travel distance
static constexpr int FLIGHT_DISTANCE = 1600;

// Tick period determines frames-per-second for sprite motion and flap animation.
// Each tick traverses the active sprite list, triggering LVGL dirty-region work.
// On STANDARD hardware, 50 ms (~20 fps) looks smooth. On BASIC/EMBEDDED, drop to
// ~7 fps — still visually acceptable for a background screensaver, ~3× less work
// per wall-second. Combined with SPRITE_CAP_LOW, this keeps Klipper's CPU budget
// untouched if the user opts the screensaver back on.
static constexpr uint32_t TICK_PERIOD_STANDARD_MS = 50;
static constexpr uint32_t TICK_PERIOD_LOW_MS = 150;

// Low-tier sprite cap — hard cap on active sprites regardless of how many are
// defined in OBJECTS[]. The array is pre-ordered by delay/wave, so truncation
// keeps a representative visual mix.
static constexpr int SPRITE_CAP_LOW = 10;

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
static constexpr ObjectDef OBJECTS[] = {
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
static constexpr int NUM_OBJECTS = sizeof(OBJECTS) / sizeof(OBJECTS[0]);

int FlyingToasterScreensaver::get_scale_factor() const {
    lv_display_t* disp = lv_display_get_default();
    if (!disp)
        return 256; // 1x in LVGL 256 = 100%
    int w = lv_display_get_horizontal_resolution(disp);
    return (w > 800) ? 512 : 256; // 2x on larger displays
}

void FlyingToasterScreensaver::start() {
    if (m_active) {
        spdlog::debug("[Screensaver] Already active, ignoring start()");
        return;
    }

    spdlog::info("[Screensaver] Starting flying toasters");

    const auto caps = helix::PlatformCapabilities::detect();
    const bool low_tier = !caps.supports_animations;
    m_tick_period_ms = low_tier ? TICK_PERIOD_LOW_MS : TICK_PERIOD_STANDARD_MS;

    m_elapsed_ms = 0;
    decode_sprites();
    create_overlay();
    spawn_objects(low_tier);

    m_tick_timer = lv_timer_create(tick_cb, m_tick_period_ms, this);
    spdlog::info("[Screensaver] Flying toasters tick period = {}ms ({} tier)", m_tick_period_ms,
                 helix::platform_tier_to_string(caps.tier));

    m_active = true;
}

FlyingToasterScreensaver::~FlyingToasterScreensaver() {
    // ScreensaverManager owns these in a unique_ptr and does not stop the active
    // one before destroying it, so a screensaver torn down while running would
    // otherwise leave tick_cb armed on a freed `this`. lv_timer_cancel_safe()
    // self-guards on lv_is_initialized() and neuters rather than unlinking, which
    // is what makes it safe from a destructor and after lv_deinit has already
    // reclaimed the timer (#750, #751, #1173).
    cancel_timer();
}

void FlyingToasterScreensaver::cancel_timer() {
    if (m_tick_timer) {
        helix::ui::lv_timer_cancel_safe(m_tick_timer);
        m_tick_timer = nullptr;
    }
}

void FlyingToasterScreensaver::stop() {
    if (!m_active) {
        return;
    }

    spdlog::info("[Screensaver] Stopping flying toasters");

    cancel_timer();

    // Clear object list (LVGL objects are children of overlay, deleted with it)
    m_objects.clear();

    // Async delete — stop() runs inside lv_timer_handler (via check_display_sleep),
    // so synchronous deletion corrupts LVGL's event linked list (#316).
    // safe_delete_deferred also reparents to lv_layer_top() before the async
    // delete so a racing parent-clean can't free the overlay out from under us.
    helix::ui::safe_delete_deferred(m_overlay);

    free_sprites();

    m_active = false;
}

void FlyingToasterScreensaver::create_overlay() {
    m_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(m_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(m_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m_overlay, 0, 0);
    lv_obj_set_style_pad_all(m_overlay, 0, 0);
    lv_obj_set_style_radius(m_overlay, 0, 0);
    // Clickable to absorb wake touch (prevents it from triggering underlying UI)
    // LVGL still registers the activity for inactivity tracking
    lv_obj_add_flag(m_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m_overlay, LV_OBJ_FLAG_SCROLLABLE);
}

void FlyingToasterScreensaver::spawn_objects(bool low_tier) {
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

    const int active_count = low_tier ? std::min(NUM_OBJECTS, SPRITE_CAP_LOW) : NUM_OBJECTS;

    m_objects.reserve(active_count);

    for (int i = 0; i < active_count; i++) {
        const auto& def = OBJECTS[i];

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

    spdlog::debug("[Screensaver] Spawned {}/{} flying objects ({}x{} screen, {}px sprites, "
                  "low_tier={})",
                  m_objects.size(), NUM_OBJECTS, screen_w, screen_h, obj_size, low_tier);
}

void FlyingToasterScreensaver::create_flying_object(int start_x, int start_y, bool is_toaster,
                                                    bool reverse_flap, int speed_ms, int delay_ms) {
    if (!m_overlay)
        return;

    lv_obj_t* img = lv_image_create(m_overlay);

    // Set initial image from pre-decoded RAM buffers
    uint8_t initial_frame = reverse_flap ? 2 : 0;
    if (is_toaster) {
        lv_image_set_src(img, m_decoded_frames[initial_frame]);
    } else {
        lv_image_set_src(img, m_decoded_toast);
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

    // Pre-compute flap rate: slower flight = slower wing flap
    // 10s → every tick, 16s → every 2 ticks, 24s → every 3 ticks
    int8_t ticks_per_flap = static_cast<int8_t>(std::max(1, speed_ms / 10000));

    FlyingObject obj{};
    obj.img = img;
    obj.is_toaster = is_toaster;
    obj.start_x = static_cast<int16_t>(start_x);
    obj.start_y = static_cast<int16_t>(start_y);
    obj.fly_ms = speed_ms;
    obj.delay_ms = delay_ms;
    obj.flap_frame = initial_frame;
    obj.flap_forward = true;
    obj.flap_counter = 0;
    obj.ticks_per_flap = ticks_per_flap;
    m_objects.push_back(obj);
}

void FlyingToasterScreensaver::tick_cb(lv_timer_t* timer) {
    auto* self = static_cast<FlyingToasterScreensaver*>(lv_timer_get_user_data(timer));
    if (!self || !self->m_active)
        return;

    self->m_elapsed_ms += self->m_tick_period_ms;

    lv_display_t* disp = lv_display_get_default();
    int screen_w = disp ? lv_display_get_horizontal_resolution(disp) : 800;
    int screen_h = disp ? lv_display_get_vertical_resolution(disp) : 480;
    int obj_size = 64;
    int scale = self->get_scale_factor();
    if (scale != 256) {
        obj_size = obj_size * scale / 256;
    }

    for (auto& obj : self->m_objects) {
        if (!obj.img)
            continue;

        // Skip objects still in their start delay
        if (self->m_elapsed_ms < static_cast<uint32_t>(obj.delay_ms)) {
            continue;
        }

        // Compute position from elapsed time (replaces per-object LVGL animations)
        uint32_t local_ms = self->m_elapsed_ms - obj.delay_ms;
        uint32_t t = local_ms % static_cast<uint32_t>(obj.fly_ms);
        int32_t dx = static_cast<int32_t>(-FLIGHT_DISTANCE) * static_cast<int32_t>(t) / obj.fly_ms;
        int32_t dy = static_cast<int32_t>(FLIGHT_DISTANCE) * static_cast<int32_t>(t) / obj.fly_ms;
        auto new_x = static_cast<int16_t>(obj.start_x + dx);
        auto new_y = static_cast<int16_t>(obj.start_y + dy);

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

        obj.flap_counter++;
        if (obj.flap_counter < obj.ticks_per_flap)
            continue;
        obj.flap_counter = 0;

        // Advance frame: 0→1→2→3→2→1→0 (ping-pong)
        uint8_t prev_frame = obj.flap_frame;
        if (obj.flap_forward) {
            obj.flap_frame++;
            if (obj.flap_frame >= NUM_TOASTER_FRAMES - 1) {
                obj.flap_forward = false;
            }
        } else {
            if (obj.flap_frame == 0) {
                obj.flap_forward = true;
            } else {
                obj.flap_frame--;
            }
        }

        // Only update image source when frame actually changed (RAM buffer, no file I/O)
        if (obj.flap_frame != prev_frame) {
            lv_image_set_src(obj.img, self->m_decoded_frames[obj.flap_frame]);
        }
    }
}

void FlyingToasterScreensaver::decode_sprites() {
    // Pre-decode each unique PNG into a persistent draw buffer.
    // When used as lv_image_set_src(), LVGL treats these as LV_IMAGE_SRC_VARIABLE —
    // no file open, no PNG decompression, just direct pixel data.
    for (int i = 0; i < NUM_TOASTER_FRAMES; i++) {
        lv_image_decoder_dsc_t dsc;
        lv_result_t res = lv_image_decoder_open(&dsc, TOASTER_FRAMES[i], nullptr);
        if (res == LV_RESULT_OK && dsc.decoded) {
            m_decoded_frames[i] = lv_draw_buf_dup(dsc.decoded);
            lv_image_decoder_close(&dsc);
        } else {
            spdlog::warn("[Screensaver] Failed to pre-decode {}", TOASTER_FRAMES[i]);
            if (res == LV_RESULT_OK)
                lv_image_decoder_close(&dsc);
        }
    }

    {
        lv_image_decoder_dsc_t dsc;
        lv_result_t res = lv_image_decoder_open(&dsc, TOAST_IMG, nullptr);
        if (res == LV_RESULT_OK && dsc.decoded) {
            m_decoded_toast = lv_draw_buf_dup(dsc.decoded);
            lv_image_decoder_close(&dsc);
        } else {
            spdlog::warn("[Screensaver] Failed to pre-decode {}", TOAST_IMG);
            if (res == LV_RESULT_OK)
                lv_image_decoder_close(&dsc);
        }
    }

    spdlog::debug("[Screensaver] Pre-decoded {} toaster frames + toast sprite into RAM",
                  NUM_TOASTER_FRAMES);
}

void FlyingToasterScreensaver::free_sprites() {
    // The sprites are live image sources; a blend of the saver's last frame
    // may still be in flight when the overlay is torn down.
    for (auto& buf : m_decoded_frames) {
        helix::safe_draw_buf_destroy(buf, "toaster");
    }
    helix::safe_draw_buf_destroy(m_decoded_toast, "toast");
}

#endif // HELIX_ENABLE_SCREENSAVER
