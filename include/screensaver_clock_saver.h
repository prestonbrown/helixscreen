// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_buffer.h"
#include "screensaver_clock.h"
#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_sim_saver.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace helix::ui {

/**
 * @brief A SimSaver whose sim draws a clock: the time, the print, and text over the scene
 *
 * `State` decides what the clock reads (ClockState, or a state that wraps it and exposes
 * face()); the sim draws that face: start() and each frame read the input, step the state and
 * pass clock_face() to the sim after the level or the rng. After the sim draws, the saver
 * puts its text back over what the frame rewrote through draw_clock_text(), with `under`
 * holding those boxes (empty at start). reset_clock_text() forgets the saver's labels, after
 * the sim is initialized at start and once it is cleared at stop.
 */
template <typename Sim, typename State = ClockState> class ClockSaver : public SimSaver<Sim> {
  public:
    const State& clock() const {
        return clock_;
    }

  protected:
    /// What the sim draws: the state's face() when it has one, else its reading().
    decltype(auto) clock_face() const {
        if constexpr (has_face<State>::value) {
            return clock_.face();
        } else {
            return clock_.reading();
        }
    }

    bool on_start() override {
        clock_.reset();
        clock_.step(0, this->read_input());
        this->start_sim(clock_face());
        under_.clear();
        under_.reserve(kSaverMaxDirtyAreas);
        text_.reset();
        reset_clock_text();
        std::vector<DirtyRect> dirty;
        draw_clock_text(under_, dirty);
        return true;
    }

    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override {
        clock_.step(dt_ms, this->read_input());
        this->step_sim(dt_ms, dirty, clock_face());
        under_.assign(dirty.begin(), dirty.end());
        draw_clock_text(under_, dirty);
    }

    void on_stop() override {
        SimSaver<Sim>::on_stop();
        release_buffer(under_);
        text_.reset();
        reset_clock_text();
    }

    /// Draws the saver's text where it changed or where a box in `under` crossed it, adding
    /// what it drew to `dirty`.
    virtual void draw_clock_text(const std::vector<DirtyRect>& under,
                                 std::vector<DirtyRect>& dirty) = 0;
    /// Forgets what the saver's labels drew.
    virtual void reset_clock_text() = 0;

    /// The reading's text lines, built again only when they change.
    const ClockLines& clock_lines_now() {
        return text_.get(clock_reading());
    }

    /// The reading the state holds; needs State::reading().
    const ClockReading& clock_reading() const {
        return clock_.reading();
    }

    State clock_;

  private:
    template <typename S, typename = void> struct has_face : std::false_type {};
    template <typename S>
    struct has_face<S, std::void_t<decltype(std::declval<const S&>().face())>> : std::true_type {};

    std::vector<DirtyRect> under_;
    ClockTextCache text_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
