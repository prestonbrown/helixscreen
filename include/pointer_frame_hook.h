// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "drm_rotation_strategy.h"
#include "indev_delete_watch.h"
#include "indev_read_hook.h"
#include "input_device_scanner.h"
#include "touch_calibration.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/// What one pointer sample needs before LVGL's rotation step reads it
enum class PointerTransform {
    None,  ///< The sample is already in the frame LVGL rotates from
    Plane, ///< Turn it with the scanout plane, which LVGL's own rotation knows nothing about
    UndoLvglRotation, ///< Hand LVGL the point its own rotation carries back onto the sample
};

/**
 * @brief Decide what a sample from a @p kind device needs
 *
 * LVGL turns every pointer sample it reads by its own display rotation, so it
 * expects samples on the unrotated display. A scanout plane that owns the
 * rotation leaves LVGL's at zero and turns everything LVGL draws, so at most
 * one of the two angles is non-zero.
 *
 * A touch panel reports where on the panel it was touched. Under LVGL's
 * rotation that is what LVGL expects; under a plane its samples have to turn
 * with the plane.
 *
 * A relative pointer's driver accumulates motion into the position the user
 * is moving it across on the picture. Under a plane that position turns along
 * with the cursor drawn at it; under LVGL's rotation, LVGL would turn it a
 * second time.
 *
 * @param kind           what the device is, from its own capabilities
 * @param plane_degrees  angle a scanout plane presents the picture at; 0 when none does
 * @param lvgl_degrees   angle LVGL rotates the display by; 0 when it does not
 */
inline PointerTransform pointer_transform_for(input::PointerKind kind, int plane_degrees,
                                              int lvgl_degrees) {
    if (kind == input::PointerKind::PanelAbsolute) {
        return plane_degrees != 0 ? PointerTransform::Plane : PointerTransform::None;
    }
    return lvgl_degrees != 0 ? PointerTransform::UndoLvglRotation : PointerTransform::None;
}

/**
 * @brief One read callback that hands LVGL each pointer device's samples in its frame
 *
 * Fronts every pointer device a backend opens, each recorded with the kind of
 * device it is, and applies pointer_transform_for() to each sample after that
 * device's own driver has produced it. The kind comes from the device, never
 * from which backend member holds it.
 *
 * A device can be deleted while the hook fronts it: lv_evdev deletes its own
 * device when a read fails, as it does when the device is unplugged.
 * IndevDeleteWatch listens for each device's LV_EVENT_DELETE, clears the
 * caller's owner_slot (if install() was given one) and then runs
 * forget_device() so restore_all() and the destructor only ever touch devices
 * that are alive. install() optionally takes the caller's own raw pointer to
 * that same indev (e.g. a backend's pointer_/touch_/mouse_ member), cleared
 * the moment LVGL deletes the device so the caller never reads through it
 * afterward. Main thread only, like every indev.
 */
class PointerFrameHook {
  public:
    /// A relative device's accumulated position before its driver bounds it by
    /// the unrotated display; false when there is no reading yet
    using RawPosition = std::function<bool(int& x, int& y)>;

    /// Reads an lv_evdev device's last position before its driver's bound
    using LastRawReader = bool (*)(lv_indev_t* indev, int* x, int* y);

#if LV_USE_EVDEV
    static constexpr LastRawReader kEvdevLastRaw = lv_evdev_get_last_raw;
#else
    static constexpr LastRawReader kEvdevLastRaw = nullptr;
#endif

    PointerFrameHook() = default;
    PointerFrameHook(const PointerFrameHook&) = delete;
    PointerFrameHook& operator=(const PointerFrameHook&) = delete;

    /// Hands back every device still alive and stops listening to it. A device
    /// still wrapped by the callback after this reads nothing.
    ~PointerFrameHook() {
        restore_all();
    }

    /**
     * @brief Put the hook in front of @p indev, a @p kind device
     *
     * @param raw_position  for a relative device, its position before the driver's
     *                      bound; without one the driver's own point is used
     * @param owner_slot    the caller's own raw pointer to @p indev (e.g. a backend's
     *                      `pointer_`/`touch_`/`mouse_` member), cleared the moment
     *                      LVGL deletes the device so the caller never reads through
     *                      it afterward
     * @return true when this call put the hook in front of @p indev
     */
    bool install(lv_indev_t* indev, input::PointerKind kind, RawPosition raw_position = {},
                 lv_indev_t** owner_slot = nullptr) {
        if (!hook_.install(indev)) {
            return false;
        }
        devices_.push_back({indev, kind, std::move(raw_position)});
        deletes_.watch(indev, owner_slot, &on_device_deleted, this);
        s_active = this;
        return true;
    }

    /**
     * @brief Put the hook in front of @p indev, classified from the device it was opened on
     *
     * @param device_path      the path the driver opened; see input::pointer_kind_for_device()
     * @param opened_by_evdev  @p indev is an lv_evdev device, whose position before the
     *                         driver's bound can be read back. Any other driver's data is
     *                         never read as lv_evdev's.
     * @param owner_slot       see the other install() overload
     * @param sysfs_base       where the device's capabilities are read from
     * @param evdev_last_raw   how an lv_evdev device's position is read back
     * @return the kind @p indev was hooked as, or nullopt when this call did not hook it
     */
    std::optional<input::PointerKind> install(lv_indev_t* indev, const std::string& device_path,
                                              bool opened_by_evdev,
                                              lv_indev_t** owner_slot = nullptr,
                                              const std::string& sysfs_base = "/sys/class/input",
                                              LastRawReader evdev_last_raw = kEvdevLastRaw) {
        const input::PointerKind kind = input::pointer_kind_for_device(device_path, sysfs_base);
        RawPosition raw_position;
        if (opened_by_evdev && evdev_last_raw != nullptr) {
            raw_position = [indev, evdev_last_raw](int& x, int& y) {
                return evdev_last_raw(indev, &x, &y);
            };
        }
        if (!install(indev, kind, std::move(raw_position), owner_slot)) {
            return std::nullopt;
        }
        return kind;
    }

    /**
     * @brief Record the angle a scanout plane presents the picture at
     *
     * @param degrees  0 when no plane rotates
     * @param panel_w  native panel width, before rotation
     * @param panel_h  native panel height, before rotation
     */
    void set_plane_rotation(int degrees, int32_t panel_w, int32_t panel_h) {
        plane_degrees_ = degrees;
        panel_w_ = panel_w;
        panel_h_ = panel_h;
    }

    /**
     * @brief Turn a touch-panel point the way the active hook turns @p indev's samples
     *
     * For points a driver reports outside the read callback, such as the extra
     * fingers of a multi-touch gesture. Only the plane turn applies: LVGL's own
     * rotation leaves panel samples for its rotation step, which the caller runs.
     * Unchanged when no hook fronts @p indev.
     */
    static PointerXY map_panel_point(const lv_indev_t* indev, PointerXY panel_point) {
        if (s_active == nullptr) {
            return panel_point;
        }
        const Device* device = s_active->find(indev);
        if (device == nullptr || pointer_transform_for(device->kind, s_active->plane_degrees_, 0) !=
                                     PointerTransform::Plane) {
            return panel_point;
        }
        return s_active->turn_with_plane(panel_point);
    }

    /// Whether the hook fronts @p indev. Compares the pointer only, so a deleted
    /// device can be asked about.
    bool fronts(const lv_indev_t* indev) const {
        return hook_.fronts(indev);
    }

    /// Hand every device back the callback the hook replaced, then forget them.
    void restore_all() {
        deletes_.forget_all();
        hook_.restore_all();
        devices_.clear();
        if (s_active == this) {
            s_active = nullptr;
        }
    }

  private:
    struct Device {
        lv_indev_t* indev;
        input::PointerKind kind;
        RawPosition raw_position;
    };

    /// IndevDeleteWatch always calls this on delete; it first clears the
    /// owner_slot install() was given, if any, but only while that slot
    /// still named @p indev.
    static void on_device_deleted(void* ctx, const lv_indev_t* indev) {
        static_cast<PointerFrameHook*>(ctx)->forget_device(indev);
    }

    /// Drop @p indev from the hook without reading or writing it.
    void forget_device(const lv_indev_t* indev) {
        hook_.forget(indev);
        devices_.erase(
            std::remove_if(devices_.begin(), devices_.end(),
                           [indev](const Device& device) { return device.indev == indev; }),
            devices_.end());
    }

    static void read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
        if (s_active != nullptr) {
            s_active->read(indev, data);
        }
    }

    void read(lv_indev_t* indev, lv_indev_data_t* data) const {
        const Device* device = find(indev);
        if (device == nullptr || !hook_.read_original(indev, data)) {
            return;
        }

        lv_display_t* disp = lv_indev_get_display(indev);
        const int lvgl_degrees =
            disp != nullptr ? static_cast<int>(lv_display_get_rotation(disp)) * 90 : 0;

        switch (pointer_transform_for(device->kind, plane_degrees_, lvgl_degrees)) {
        case PointerTransform::None:
            return;
        case PointerTransform::Plane: {
            if (panel_w_ <= 0 || panel_h_ <= 0) {
                return;
            }
            const PointerXY raw{data->point.x, data->point.y};
            const PointerXY turned = turn_with_plane(raw);
            data->point.x = turned.x;
            data->point.y = turned.y;
            if (is_touch_debug_enabled() && data->state == LV_INDEV_STATE_PRESSED) {
                spdlog::warn("[TouchDebug] plane_rotate {}°: raw=({},{}) -> screen=({},{}) "
                             "panel={}x{}",
                             plane_degrees_, raw.x, raw.y, turned.x, turned.y, panel_w_, panel_h_);
            }
            return;
        }
        case PointerTransform::UndoLvglRotation: {
            PointerXY position{data->point.x, data->point.y};
            int raw_x = 0;
            int raw_y = 0;
            if (device->raw_position && device->raw_position(raw_x, raw_y)) {
                position = {raw_x, raw_y};
            }
            // The driver bounds the position by the unrotated display, which at
            // 90 and 270 degrees is not the picture the pointer moves across.
            position.x =
                std::clamp<int32_t>(position.x, 0, lv_display_get_horizontal_resolution(disp) - 1);
            position.y =
                std::clamp<int32_t>(position.y, 0, lv_display_get_vertical_resolution(disp) - 1);
            const PointerXY handed = unrotate_pointer_for_display(
                position, lvgl_degrees, lv_display_get_original_horizontal_resolution(disp),
                lv_display_get_original_vertical_resolution(disp));
            data->point.x = handed.x;
            data->point.y = handed.y;
            return;
        }
        }
    }

    /// A panel sample turned with the scanout plane; unchanged until the panel size is known.
    PointerXY turn_with_plane(PointerXY panel_point) const {
        if (panel_w_ <= 0 || panel_h_ <= 0) {
            return panel_point;
        }
        return rotate_pointer_for_plane(panel_point, plane_degrees_, panel_w_, panel_h_);
    }

    const Device* find(const lv_indev_t* indev) const {
        for (const Device& device : devices_) {
            if (device.indev == indev) {
                return &device;
            }
        }
        return nullptr;
    }

    IndevReadHook hook_{read_cb};
    std::vector<Device> devices_;
    IndevDeleteWatch deletes_;
    int plane_degrees_ = 0;
    int32_t panel_w_ = 0;
    int32_t panel_h_ = 0;

    /// LVGL stores only a function pointer, so the callback reaches the hook
    /// that installed last. One backend drives the display at a time.
    inline static PointerFrameHook* s_active =
        nullptr; // ACTIVE_INSTANCE_OK: indev callback context
};

} // namespace helix
