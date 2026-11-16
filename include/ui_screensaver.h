// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "ui_widget_ref.h"

#include "screensaver_base.h"
#include "screensaver_motion.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

/**
 * @brief Flying Toasters screensaver (After Dark, 1989)
 *
 * Toasters and toast fly diagonally across a black screen until a touch wakes the UI.
 * Every sprite's position and wing frame are computed from the time the saver has run, so
 * they keep their speed however often or unevenly the frame timer fires, and a level change
 * that changes the timer's period changes nothing on screen but the frame rate. The lowest
 * rung also flies fewer sprites.
 *
 * During a print the toast browns from light to dark as the print goes; a failed or cancelled
 * print burns it black, and a finished one has a toaster pop a toast every few seconds.
 */
class FlyingToasterScreensaver : public helix::ui::SaverBase {
  public:
    FlyingToasterScreensaver() = default;
    FlyingToasterScreensaver(const FlyingToasterScreensaver&) = delete;
    FlyingToasterScreensaver& operator=(const FlyingToasterScreensaver&) = delete;

    ScreensaverType type() const override {
        return ScreensaverType::FLYING_TOASTERS;
    }

    /// Browns the toast takes on, light to dark, as a print goes.
    static constexpr int kToastShadeCount = 6;
    /// The shade of a failed or cancelled print's toast.
    static constexpr int kToastBurnt = kToastShadeCount;

    /// Shade the toast shows: -1 in its own colours (no print), 0 to kToastShadeCount - 1
    /// light to dark, or kToastBurnt.
    int toast_shade() const {
        return m_toast_shade;
    }
    /// The toast art for `shade` (as toast_shade()), or nullptr while stopped.
    const lv_draw_buf_t* toast_art(int shade) const;
    /// The toast a toaster pops on a finished print, or nullptr while stopped.
    lv_obj_t* popped_toast() const {
        return m_pop.get();
    }

  protected:
    /// Sprites are LVGL images on the overlay; there is no canvas.
    std::optional<lv_color_format_t> canvas_format() const override {
        return std::nullopt;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override;
    uint32_t ladder_period_ms(size_t level) const override;
    /// Applies a level at once; the lowest rung also lets go of the sprites past its cap.
    void on_level_request(size_t level) override;

  private:
    // Test-only seam: reads the sprites and decoded frames so frame-time-driven motion can be
    // pinned. See tests/test_helpers/screensaver_test_access.h.
    friend class FlyingToasterScreensaverTestAccess;

    struct FlyingObject {
        helix::ui::WeakWidget img;
        bool is_toaster;
        int16_t start_x;
        int16_t start_y;
        int fly_ms;
        int delay_ms;
        // Wing flap (toasters only)
        uint8_t initial_frame;
        uint8_t flap_frame;    // frame currently shown
        uint16_t flap_step_ms; // how long each wing frame holds
        // Previous position; lv_obj_set_pos() is skipped when unchanged to avoid invalidation
        int16_t prev_x = INT16_MIN;
        int16_t prev_y = INT16_MIN;
    };

    /** @brief Spawn the first `count` flying objects with staggered positions and delays */
    void spawn_objects(size_t count);

    /** @brief Sprites flown at `level` */
    size_t sprite_limit(size_t level) const;

    /** @brief Lets go of the sprites past `limit`: hidden now, deleted on a later timer pass */
    void drop_sprites_past(size_t limit);

    /** @brief Create a single flying object */
    void create_flying_object(int start_x, int start_y, bool is_toaster, bool reverse_flap,
                              int speed_ms, int delay_ms);

    /** @brief Get image scale factor based on screen width */
    int get_scale_factor() const;

    /** @brief Pre-decode all PNG sprites into persistent RAM buffers */
    void decode_sprites();

    /** @brief Free pre-decoded sprite buffers */
    void free_sprites();

    /** @brief Tint the decoded toast into each shade's buffer */
    void build_toast_shades();

    /** @brief The buffer toast shows at `shade` (as toast_shade()) */
    lv_draw_buf_t* toast_buffer(int shade) const;

    /** @brief Points every toast at the shade the print reading calls for, when it changed */
    void apply_toast_shade();

    /** @brief Advances a finished print's pop: a toast rises out of a visible toaster and drops */
    void step_pop(uint32_t dt_ms, int screen_w, int screen_h, int obj_size);

    std::vector<FlyingObject> m_objects;
    uint32_t m_elapsed_ms = 0; // time the saver has run

    // Pre-decoded sprite buffers (avoid per-frame PNG file I/O + decompression)
    lv_draw_buf_t* m_decoded_frames[4] = {}; // toaster_0..3
    lv_draw_buf_t* m_decoded_toast = nullptr;
    lv_draw_buf_t* m_toast_shades[kToastShadeCount + 1] = {}; // light to dark, then burnt
    int m_toast_shade = -1;

    // A finished print's pop: one image, behind the flock, reused for every pop
    helix::ui::WidgetRef m_pop;
    int m_pop_from = -1;         // index of the toaster popping, -1 between pops
    uint32_t m_pop_ms = 0;       // time into the current pop
    uint32_t m_since_pop_ms = 0; // time since the last pop began
};

#endif // HELIX_ENABLE_SCREENSAVER
