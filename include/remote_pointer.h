// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <atomic>
#include <cstdint>

namespace helix::remote {

/**
 * @brief A `ctl`-driven pointer input device.
 *
 * Registers a second LVGL pointer indev whose position and button state are set
 * explicitly instead of read from hardware. This lets `helix-screen ctl` drive real
 * press / move / release sequences through **LVGL's own input pipeline**, so
 * long-press timing, scroll-versus-click arbitration, gesture recognition and
 * button-matrix slide detection all behave exactly as they do under a finger.
 *
 * This exists because `ctl click` is `lv_obj_send_event(obj, LV_EVENT_CLICKED)` — a
 * synthesized widget-level event with no input device and no coordinates behind it.
 * That is fine for "press this button", but it cannot exercise anything gestural:
 * a drag, a long-press, a slide onto a popover, or a tap that must not fire while a
 * list is scrolling. Code reading `lv_indev_active()` sees nothing at all.
 *
 * The device coexists with the real SDL/evdev pointer; LVGL supports multiple
 * pointer indevs and reads each on its own timer. It is only created when a `ctl`
 * pointer command first arrives, so instances without remote control never register
 * it.
 *
 * **Threading:** `ensure_created()` and `set_state()` must run on the UI thread
 * (LVGL is not thread-safe). `read_count()` is safe from any thread — callers on
 * the transport thread poll it to wait until LVGL has actually consumed a state
 * change, since indevs are sampled on a timer rather than synchronously.
 */
/// How long `ctl long_press` should hold the pointer down, given the configured
/// long-press threshold in ms.
///
/// Overshooting the threshold is the whole job: LVGL starts counting from the
/// sample that first reports the press, not from the moment the command ran, so a
/// hold of exactly the threshold races the indev timer and intermittently lands a
/// plain click instead. The margin covers one sampling period plus the scheduling
/// slop of a loaded test machine.
///
/// Clamped at the bottom so a nonsense setting cannot produce a hold that could
/// never register as a long press.
constexpr int32_t pointer_long_press_hold_ms(int32_t long_press_time_ms) {
    constexpr int32_t MARGIN = 250;
    constexpr int32_t FLOOR = 300;
    const int32_t base = long_press_time_ms > 0 ? long_press_time_ms : 500;
    const int32_t hold = base + MARGIN;
    return hold < FLOOR + MARGIN ? FLOOR + MARGIN : hold;
}

class RemotePointer {
  public:
    static RemotePointer& instance();

    RemotePointer(const RemotePointer&) = delete;
    RemotePointer& operator=(const RemotePointer&) = delete;

    /**
     * @brief Create the LVGL input device if it does not exist yet.
     * @return false when there is no default display to attach to
     * @note UI thread only. Idempotent.
     */
    bool ensure_created();

    /**
     * @brief Set pointer position and button state.
     * @note UI thread only. The change is not observed until LVGL next reads the
     *       device — poll read_count() to wait for that.
     */
    void set_state(int32_t x, int32_t y, bool pressed);

    /// Times LVGL has sampled this device. Safe from any thread.
    [[nodiscard]] uint64_t read_count() const;

    [[nodiscard]] bool is_created() const;

    /// Current state, for diagnostics. Safe from any thread.
    [[nodiscard]] int32_t x() const;
    [[nodiscard]] int32_t y() const;
    [[nodiscard]] bool pressed() const;

  private:
    RemotePointer() = default;

    static void read_cb(lv_indev_t* indev, lv_indev_data_t* data);

    lv_indev_t* indev_ = nullptr;
    std::atomic<int32_t> x_{0};
    std::atomic<int32_t> y_{0};
    std::atomic<bool> pressed_{false};
    std::atomic<uint64_t> reads_{0};
};

} // namespace helix::remote
