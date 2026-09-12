// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include <cstdint>
#include <lvgl.h>
#include <memory>
#include <vector>

/**
 * @brief Available screensaver types
 *
 * Values map directly to the settings dropdown index and persisted config value.
 */
enum class ScreensaverType : int {
    OFF = 0,
    FLYING_TOASTERS = 1,
    STARFIELD = 2,
    PIPES_3D = 3,
};

namespace helix::ui {

/**
 * @brief Row pitch in bytes lv_canvas_set_buffer() uses for a w-wide ARGB8888 canvas
 *
 * lv_canvas_set_buffer() does not take the caller's word for packing: it
 * derives an LV_DRAW_BUF_STRIDE_ALIGN-rounded stride and sizes the canvas
 * extent from it, so a buffer allocated at w * h * 4 under-runs the extent
 * whenever the stride exceeds w * 4 (prestonbrown/helixscreen#1591). The
 * canvas screensavers size their allocations and direct pixel writes from
 * this pitch so they cannot disagree with what LVGL steps rows by.
 */
inline uint32_t screensaver_canvas_stride_bytes(int32_t w) {
    return lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), LV_COLOR_FORMAT_ARGB8888);
}

} // namespace helix::ui

/**
 * @brief Abstract base class for all screensaver implementations
 *
 * Each screensaver owns its overlay and rendering resources.
 * The ScreensaverManager routes start/stop calls to the active instance.
 */
class Screensaver {
  public:
    virtual ~Screensaver() = default;

    /** @brief Create overlay and begin rendering */
    virtual void start() = 0;

    /** @brief Stop rendering and destroy overlay */
    virtual void stop() = 0;

    /** @brief Check if this screensaver is currently running */
    virtual bool is_active() const = 0;

    /** @brief Return the type identifier for this screensaver */
    virtual ScreensaverType type() const = 0;
};

/**
 * @brief Registry and router for screensaver instances
 *
 * Owns all screensaver implementations. Routes start/stop based on
 * the configured type from DisplaySettingsManager.
 */
class ScreensaverManager {
  public:
    /** @brief Must not be called before LVGL initialization */
    static ScreensaverManager& instance();

    ScreensaverManager(const ScreensaverManager&) = delete;
    ScreensaverManager& operator=(const ScreensaverManager&) = delete;

    /**
     * @brief Start the specified screensaver type
     *
     * Stops any currently active screensaver first.
     * Does nothing if type is OFF.
     */
    void start(ScreensaverType type);

    /** @brief Stop whatever screensaver is currently active */
    void stop();

    /** @brief Check if any screensaver is currently active */
    bool is_active() const;

    /** @brief Read configured screensaver type from DisplaySettingsManager */
    static ScreensaverType configured_type();

  private:
    ScreensaverManager();
    ~ScreensaverManager() = default;

    /** @brief Find screensaver instance by type, or nullptr */
    Screensaver* find(ScreensaverType type) const;

    std::vector<std::unique_ptr<Screensaver>> screensavers_;
    Screensaver* active_ = nullptr;
};

#endif // HELIX_ENABLE_SCREENSAVER
