// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_canvas.h"
#include "screensaver_clock.h"
#include "screensaver_frame.h"
#include "screensaver_frame_timer.h"
#include "screensaver_overlay.h"
#include "screensaver_print_card.h"
#include "screensaver_sim_contract.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace helix::ui {

class SaverTestAccess;

/**
 * @brief Where a saver reads the time and the print each frame
 *
 * Unset reads saver_clock_now(), the time of day with no print. The manager reaches it through
 * Screensaver::clock_input_sink() and points it at the app's print.
 */
class ClockInputSink {
  public:
    using InputSource = std::function<ClockInput()>;

    virtual ~ClockInputSink() = default;

    void set_input_source(InputSource source) {
        source_ = std::move(source);
    }

  protected:
    ClockInput read_input() const {
        return source_ ? source_() : saver_clock_now();
    }

  private:
    InputSource source_;
};

/**
 * @brief Sequences the parts every screensaver runs on
 *
 * start() creates the overlay, the canvas when the saver has one, seeds the random sequence,
 * sets the start level, calls on_start() and starts the frame timer at the level's period.
 * Each frame calls on_frame() and invalidates the dirty areas it returns. stop() cancels the
 * timer, hides and frees the canvas, queues the overlay for deletion and calls on_stop().
 * A saver implements the on_* hooks and its ladder, most expensive level first (a table of
 * rows with a period_ms reads through ladder_period()). A level change also moves the display
 * refresh while ScreensaverManager holds it (RefreshPeriodHold::follow). A saver that runs a
 * simulation derives from SimSaver (screensaver_sim_saver.h), which implements the hooks.
 */
class SaverBase : public Screensaver, public ClockInputSink {
  public:
    /// Every saver reads the time and the print, whether or not its scene shows them.
    ClockInputSink* clock_input_sink() override {
        return this;
    }
    void start() final;
    void stop() final;
    bool is_active() const final {
        return active_;
    }

    /// Level the saver runs at.
    size_t level() const {
        return level_;
    }
    size_t level_count() const {
        return ladder_size();
    }

    /// Frame period, in ms, of `level`, clamped to the ladder. Level 0 runs at
    /// HELIX_SCREENSAVER_REFR_PERIOD_MS (RefreshPeriodHold::period()) when one is configured.
    uint32_t level_period_ms(size_t level) const;

    /// Level the next start() runs at, clamped to the ladder.
    void set_start_level(size_t level);

    /// Asks a running saver to run at `level`, clamped to the ladder. Ignored while stopped.
    void request_level(size_t level);

    /// Whether the print progress card is on screen now.
    bool print_card_shown() const {
        return card_.shown;
    }
    /// What the card shows, or would show once visible.
    const PrintCardFace& print_card_face() const {
        return card_.face;
    }
    /// The card's canvas object, or nullptr for a saver with its own print display.
    lv_obj_t* print_card_obj() const {
        return card_.canvas.obj();
    }

  protected:
    SaverBase() = default;

    /// Canvas format the saver draws in, or nullopt for a saver without a canvas.
    virtual std::optional<lv_color_format_t> canvas_format() const = 0;
    /// Sets up the scene. The overlay, the canvas, the seeded random sequence and level() are
    /// ready. Returning false refuses to start and releases everything.
    virtual bool on_start() = 0;
    /// Advances the scene by `dt_ms` and adds the canvas areas it changed to `dirty` (empty on
    /// entry).
    virtual void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) = 0;
    /// Releases scene state once the timer, canvas and overlay are gone.
    virtual void on_stop() = 0;
    /// Number of quality levels, at least 1.
    virtual size_t ladder_size() const = 0;
    /// Frame period, in ms, of `level` (below ladder_size()).
    virtual uint32_t ladder_period_ms(size_t level) const = 0;
    /// A level request for the running saver. The default applies it at once, which suits
    /// motion that is a function of time; a saver with a natural break calls apply_level() there.
    virtual void on_level_request(size_t level);

    /// Runs at `level` (clamped) from now on: sets level(), the frame timer's period and, while
    /// the display refresh is held, the refresh period.
    void apply_level(size_t level);

    SaverOverlay& overlay() {
        return overlay_;
    }
    SaverCanvas& canvas() {
        return canvas_;
    }
    std::minstd_rand& rng() {
        return rng_;
    }
    int32_t screen_w() const {
        return screen_w_;
    }
    int32_t screen_h() const {
        return screen_h_;
    }

  private:
    friend class SaverTestAccess;

    void run_frame(uint32_t dt_ms);
    /// Creates the progress card above the scene for a saver the registry marks CARD.
    void start_print_card();
    /// Steps the card's reading and drift, then shows, moves or repaints it as they changed.
    void step_print_card(uint32_t dt_ms);
    void stop_print_card();

    /// The progress card, for savers with no print display of their own.
    struct PrintCard {
        bool enabled = false;
        bool shown = false;
        bool painted = false;
        PrintCardLayout layout;
        DirtyRect home;
        ClockState clock;
        ClockDrift drift;
        PrintCardFace face;
        PrintCardPainter painter;
        SaverCanvas canvas;
    };

    bool active_ = false;
    size_t level_ = 0;
    size_t start_level_ = 0;
    int32_t screen_w_ = 0;
    int32_t screen_h_ = 0;
    SaverOverlay overlay_;
    SaverCanvas canvas_;
    SaverFrameTimer timer_;
    std::vector<DirtyRect> dirty_;
    // Owned random sequence, seeded in start(); a fixed seed makes a run replay exactly.
    std::minstd_rand rng_;
    std::optional<uint32_t> fixed_seed_;
    PrintCard card_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
