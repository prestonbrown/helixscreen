// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rotation_probe.h"

#include "config.h"
#include "display_backend.h"
#include "display_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix {

namespace {

constexpr lv_display_rotation_t kRotations[] = {LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90,
                                                LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270};
constexpr int kRotationDegrees[] = {0, 90, 180, 270};
constexpr int kNumRotations = 4;
/// Give up after this many full sweeps (e.g. an uncalibrated resistive touchscreen that
/// cannot register taps) rather than loop forever.
constexpr int kMaxCycles = 3;

} // namespace

RotationProbe::RotationProbe(RotationProbeHost host)
    : m_host(std::move(host)), m_tap_latch(!m_host.is_sdl) {
    // The subjects exist before the screen that binds to them is created.
    UI_MANAGED_SUBJECT_STRING(m_main_subject, m_main_buf, "", "rotation_probe_main", m_subjects);
    UI_MANAGED_SUBJECT_STRING(m_help_subject, m_help_buf, "", "rotation_probe_help", m_subjects);
    UI_MANAGED_SUBJECT_STRING(m_subtitle_subject, m_subtitle_buf, "", "rotation_probe_subtitle",
                              m_subjects);
}

RotationProbe::~RotationProbe() = default;

void RotationProbe::set_subtitle(Phase phase, int rot_deg, int secs, int step, int steps) {
    char buf[128];
    if (phase == Phase::Scan) {
        snprintf(buf, sizeof(buf), lv_tr("Testing rotation: %d\xc2\xb0 (%d/%d) - %ds remaining"),
                 rot_deg, step + 1, steps, secs);
    } else {
        snprintf(buf, sizeof(buf), lv_tr("Rotation: %d\xc2\xb0 - %ds remaining (or wait to retry)"),
                 rot_deg, secs);
    }
    lv_subject_copy_string(&m_subtitle_subject, buf);
}

void RotationProbe::show_screen(Phase phase, int rot_deg, int step, int steps,
                                uint32_t timeout_ms) {
    if (phase == Phase::Scan) {
        lv_subject_copy_string(&m_main_subject,
                               lv_tr("Tap anywhere if this text is right-side up"));
        lv_subject_copy_string(&m_help_subject,
                               lv_tr("HelixScreen is detecting your display orientation"));
    } else {
        lv_subject_copy_string(&m_main_subject, lv_tr("Tap again to confirm this orientation"));
        lv_subject_copy_string(&m_help_subject, "");
    }
    set_subtitle(phase, rot_deg, static_cast<int>(timeout_ms / 1000), step, steps);

    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    const char* attrs[] = {"bg_color", phase == Phase::Scan ? "#1a1a2e" : "#1a2e1a", nullptr};
    lv_xml_create(scr, "rotation_probe_screen", attrs);
}

// Samples the pointer once and feeds the latch. The read callback is looked up per call,
// because the backend may replace the pointer device while the probe is running.
void RotationProbe::poll_pointer() {
    lv_indev_t* pointer = m_host.pointer;
    lv_indev_read_cb_t read_cb = pointer ? lv_indev_get_read_cb(pointer) : nullptr;
    if (!read_cb) {
        return;
    }
    lv_indev_data_t data = {};
    read_cb(pointer, &data);
    m_tap_latch.feed(data);
}

// Polls until the contact lifts (or the deadline passes), so a held finger cannot carry into
// the next screen.
void RotationProbe::drain_until_release() {
    const uint32_t release_deadline = DisplayManager::get_ticks() + 2000; // 2s max
    while (DisplayManager::get_ticks() < release_deadline) {
        lv_timer_handler();
        DisplayManager::delay(10);
        lv_indev_t* pointer = m_host.pointer;
        lv_indev_read_cb_t read_cb = pointer ? lv_indev_get_read_cb(pointer) : nullptr;
        if (!read_cb) {
            break;
        }
        lv_indev_data_t release_data = {};
        read_cb(pointer, &release_data);
        if (release_data.state == LV_INDEV_STATE_RELEASED) {
            break;
        }
    }
}

// Mini event loop that watches for a tap. Returns immediately on a confirmed tap.
bool RotationProbe::wait_for_tap(Phase phase, uint32_t timeout_ms, int rot_deg, int step,
                                 int steps) {
    const uint32_t start = DisplayManager::get_ticks();
    int last_sec = -1;

    // Drop anything latched by the previous screen, and re-baseline the coordinate so the
    // position the last tap left behind cannot read as a fresh one. A contact still down here
    // (a screen that timed out mid-press) is drained to its release rather than counted as a
    // tap on this screen.
    m_tap_latch.reset();
    poll_pointer();
    if (m_tap_latch.consume()) {
        spdlog::debug("[RotationProbe] contact still down at {}° entry, waiting for release",
                      rot_deg);
        drain_until_release();
        m_tap_latch.reset();
    }

    // A tap detected here is drained to its release before returning, so a finger still down
    // does not carry into the next screen as a phantom.
    auto accept_tap = [&]() {
        spdlog::info("[RotationProbe] tap detected at {}° ({})", rot_deg,
                     m_tap_latch.from_collapsed_read() ? "recovered from collapsed read"
                                                       : "press observed");
        m_tap_latch.consume();
        drain_until_release();
        m_tap_latch.reset();
    };

    while (true) {
        const uint32_t elapsed = DisplayManager::get_ticks() - start;
        if (elapsed >= timeout_ms) {
            return false;
        }

        // Sample either side of lv_timer_handler(): whatever it costs on this hardware, a tap
        // that lands during it is still seen on the very next sample rather than after
        // another full poll interval.
        poll_pointer();
        if (m_tap_latch.latched()) {
            accept_tap();
            return true;
        }

        lv_timer_handler();
        DisplayManager::delay(10);

        poll_pointer();
        if (m_tap_latch.latched()) {
            accept_tap();
            return true;
        }

        const int remaining_sec = static_cast<int>((timeout_ms - elapsed + 999) / 1000);
        if (remaining_sec != last_sec) {
            set_subtitle(phase, rot_deg, remaining_sec, step, steps);
            last_sec = remaining_sec;
        }
    }
}

int RotationProbe::run() {
    // The component is registered by path rather than through LayoutManager: the probe runs
    // before the layout is resolved and before the rest of the XML components are registered.
    if (lv_xml_register_component_from_file("A:ui_xml/rotation_probe_screen.xml") != LV_RESULT_OK) {
        spdlog::error("[RotationProbe] Failed to register rotation_probe_screen");
    }

    // LVGL's automatic input processing is off for the probe: it reads the touch device
    // directly, and letting lv_timer_handler() read it too would consume evdev events and
    // cost taps.
    lv_indev_t* pointer = m_host.pointer;
    lv_indev_enable(pointer, false);

    // Each rotation resizes the screen, and the registered theme/layout refresh runs inside
    // the lv_timer_handler() call the tap poll makes every iteration: seconds of it on a slow
    // panel, during which no touch sample is taken. The confirmed rotation is re-applied at
    // the end and the caller refreshes the theme and layout once the probe returns.
    m_host.suspend_resize_fanout(true);

    int confirmed_rotation = -1;
    int cycle = 0;

    // Loop until the user confirms a rotation. On real hardware the wrong rotation renders
    // unreadable text, so the user can only tap the correct one.
    while (confirmed_rotation < 0 && cycle < kMaxCycles) {
        cycle++;
        for (int i = 0; i < kNumRotations; i++) {
            // SDL cannot rotate (DIRECT render mode); everywhere else the rendering actually
            // changes on screen and the backend adjusts touch coordinates.
            if (!m_host.is_sdl) {
                m_host.settle_rotation(kRotations[i]);
            }

            spdlog::info("[RotationProbe] testing {}° ({}x{})", kRotationDegrees[i],
                         lv_display_get_horizontal_resolution(nullptr),
                         lv_display_get_vertical_resolution(nullptr));

            show_screen(Phase::Scan, kRotationDegrees[i], i, kNumRotations, m_host.scan_timeout_ms);
            if (!wait_for_tap(Phase::Scan, m_host.scan_timeout_ms, kRotationDegrees[i], i,
                              kNumRotations)) {
                continue;
            }

            spdlog::info("[RotationProbe] {}° tapped, confirming...", kRotationDegrees[i]);
            show_screen(Phase::Confirm, kRotationDegrees[i], i, kNumRotations,
                        m_host.confirm_timeout_ms);
            if (wait_for_tap(Phase::Confirm, m_host.confirm_timeout_ms, kRotationDegrees[i], i,
                             kNumRotations)) {
                confirmed_rotation = kRotationDegrees[i];
                spdlog::info("[RotationProbe] {}° confirmed!", confirmed_rotation);
                break;
            }
            spdlog::info("[RotationProbe] {}° not confirmed, continuing scan", kRotationDegrees[i]);
        }
    }

    if (confirmed_rotation < 0) {
        spdlog::warn("[RotationProbe] no confirmation after {} cycles, defaulting to 0°",
                     kMaxCycles);
        confirmed_rotation = 0;
    }

    Config* cfg = Config::get_instance();
    cfg->set("/display/rotation_probed", true);
    cfg->set("/display/rotate", confirmed_rotation);
    cfg->save();
    spdlog::info("[RotationProbe] saved: {}°", confirmed_rotation);

    if (!m_host.is_sdl) {
        m_host.settle_rotation(degrees_to_lv_rotation(confirmed_rotation));
    }

    // The loop above runs lv_timer_handler() repeatedly, so an unplug mid-probe can null the
    // pointer before this line; lv_indev_enable(NULL, true) would enable every indev instead
    // of doing nothing.
    if (m_host.pointer) {
        lv_indev_enable(m_host.pointer, true);
    }

    // lv_obj_clean() only removes children. The probe leaves the screen's own style alone, but
    // clearing it keeps the theme in charge of normal UI init whatever ran before.
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_local_style_prop(scr, LV_STYLE_BG_COLOR, LV_PART_MAIN);
    lv_obj_remove_local_style_prop(scr, LV_STYLE_BG_OPA, LV_PART_MAIN);

    m_host.suspend_resize_fanout(false);
    return confirmed_rotation;
}

} // namespace helix
