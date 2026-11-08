// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_registry.h"
#include "screensaver_sim_contract.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace helix::ui {

class SimSaverTestAccess;

/**
 * @brief A saver whose scene is a simulation drawing into the canvas
 *
 * Implements every SaverBase hook from the sim (screensaver_sim_contract.h): the ladder is
 * Sim::kLevels, start() clears the sim and initializes it on the canvas at the start level,
 * each frame steps it, a level request applies at once and reaches the sim at the level
 * applied, and stop() clears it. A sim whose init() and step() take a `const ClockReading&`
 * after the level or the rng is handed print_reading(), so its scene follows the print on the
 * frame the progress card does. A saver that needs more overrides the one hook it must and
 * calls start_sim() / step_sim() from it; a sim whose init() or step() takes anything else
 * has a saver that overrides on_start() and on_frame(), and without them refuses to start.
 */
template <typename Sim> class SimSaver : public SaverBase {
  public:
    const Sim& sim() const {
        return sim_;
    }

  protected:
    size_t ladder_size() const override {
        return std::size(Sim::kLevels);
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return ladder_period(Sim::kLevels, level);
    }

    bool on_start() override {
        if constexpr (sim_inits_with_v<Sim>) {
            start_sim();
            return true;
        } else if constexpr (sim_inits_with_v<Sim, const ClockReading&>) {
            start_sim(this->print_reading());
            return true;
        } else {
            spdlog::error("[Screensaver] {} has no start for its sim", screensaver_name(type()));
            return false;
        }
    }

    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override {
        if constexpr (sim_steps_with_v<Sim>) {
            step_sim(dt_ms, dirty);
        } else if constexpr (sim_steps_with_v<Sim, const ClockReading&>) {
            step_sim(dt_ms, dirty, this->print_reading());
        }
    }

    void on_level_request(size_t level) override {
        apply_level(level);
        sim_.request_level(this->level());
    }

    void on_stop() override {
        sim_.clear();
    }

    /// Clears the sim, initializes it on the canvas at level() with `extra` after the level,
    /// and logs what it holds.
    template <typename... Extra> void start_sim(Extra&&... extra) {
        sim_.clear();
        FrameTarget frame = canvas().frame();
        init_sim(sim_, frame, rng(), level(), std::forward<Extra>(extra)...);
        if constexpr (sim_has_heap_bytes_v<Sim>) {
            spdlog::debug("[Screensaver] {} holds {} KB at {}x{}, level {}",
                          screensaver_name(type()), sim_.heap_bytes() / 1024, frame.w, frame.h,
                          level());
        } else {
            spdlog::debug("[Screensaver] {} started at {}x{}, level {}", screensaver_name(type()),
                          frame.w, frame.h, level());
        }
    }

    /// Steps the sim by `dt_ms` on the canvas, `extra` before the dirty list.
    template <typename... Extra>
    void step_sim(uint32_t dt_ms, std::vector<DirtyRect>& dirty, Extra&&... extra) {
        FrameTarget frame = canvas().frame();
        helix::ui::step_sim(sim_, dt_ms, frame, rng(), dirty, std::forward<Extra>(extra)...);
    }

    Sim sim_;

  private:
    friend class SimSaverTestAccess;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
