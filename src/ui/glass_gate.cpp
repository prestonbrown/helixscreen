// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "glass_gate.h"

#include "ui_timer_guard.h"

#include "board_fingerprint.h"
#include "config.h"
#include "core/observer_factory.h"
#include "display_backend.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "glass_painter.h"
#include "glass_shading.h"
#include "helix_version.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h" // disp->screens: LVGL has no screen iterator
#include "platform_capabilities.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace helix::ui {

GlassGateVerdict decide_glass_gate(const GlassGateFacts& f) {
    if (f.env_force) {
        return *f.env_force ? GlassGateVerdict::Pass : GlassGateVerdict::Fail;
    }
    if (f.color_depth < 24 || !f.style_effects_allowed || f.software_rotated ||
        !f.multi_stop_gradients) {
        return GlassGateVerdict::Fail;
    }
    if (f.stored && f.stored->version == f.version && f.stored->board == f.board) {
        return f.stored->passed ? GlassGateVerdict::Pass : GlassGateVerdict::Fail;
    }
    return GlassGateVerdict::NeedsProbe;
}

std::optional<bool> glass_env_force() {
    const char* v = std::getenv("HELIX_GLASS");
    if (v == nullptr) {
        return std::nullopt;
    }
    if (std::strcmp(v, "1") == 0) {
        return true;
    }
    if (std::strcmp(v, "0") == 0) {
        return false;
    }
    spdlog::warn("[Glass] Ignoring HELIX_GLASS='{}': expected 1 or 0", v);
    return std::nullopt;
}

std::optional<GlassProbeRecord> parse_glass_probe(const nlohmann::json* node) {
    if (node == nullptr || !node->is_object()) {
        return std::nullopt;
    }
    const auto passed = node->find("passed");
    const auto frame_ms = node->find("frame_ms");
    const auto key = parse_measurement_key(*node);
    if (passed == node->end() || !passed->is_boolean() || frame_ms == node->end() ||
        !frame_ms->is_number() || !key) {
        return std::nullopt;
    }
    GlassProbeRecord r;
    r.passed = passed->get<bool>();
    r.frame_ms = frame_ms->get<double>();
    r.version = key->version;
    r.board = key->board;
    return r;
}

nlohmann::json glass_probe_json(const GlassProbeRecord& r) {
    nlohmann::json node{{"passed", r.passed}, {"frame_ms", r.frame_ms}};
    write_measurement_key(node, {r.board, r.version});
    return node;
}

std::optional<GlassProbeRecord> load_glass_probe(const helix::Config& c) {
    return parse_glass_probe(c.try_get_json(kGlassProbeStorePath));
}

void save_glass_probe(helix::Config& c, const GlassProbeRecord& r) {
    c.set<nlohmann::json>(kGlassProbeStorePath, glass_probe_json(r));
    c.save();
}

namespace {

constexpr int kProbeRepetitions = 5;
constexpr int kProbeButtons = 12;
constexpr int32_t kProbeButtonW = 140;
constexpr int32_t kProbeButtonH = 46;
constexpr int32_t kProbeRadius = 10;

/// One glass rect as a live frame draws it: its drop shadow, then its gradient fill.
void fill_glass(lv_layer_t* layer, const lv_area_t& area, GlassFamily family, lv_color_t base,
                const GlassShadow& shadow) {
    lv_draw_box_shadow_dsc_t sh;
    lv_draw_box_shadow_dsc_init(&sh);
    sh.radius = kProbeRadius;
    sh.width = shadow.width;
    sh.ofs_y = shadow.offset_y;
    sh.color = glass_shadow_color(base, true);
    sh.opa = shadow.dark_opa;
    lv_draw_box_shadow(layer, &sh, &area);

    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.radius = kProbeRadius;
    dsc.opa = LV_OPA_COVER;
    dsc.color = base;
    glass_gradient(&dsc.grad, family, base, true, false);
    lv_draw_fill(layer, &dsc, &area);
}

void draw_probe_frame(lv_obj_t* canvas, int32_t width, int32_t height) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    const lv_area_t whole{0, 0, width - 1, height - 1};
    fill_glass(&layer, whole, GlassFamily::Surface, lv_color_hex(0x2E3440), kGlassSurfaceShadow);
    const int32_t cols = std::max<int32_t>(1, width / (kProbeButtonW + 8));
    for (int i = 0; i < kProbeButtons; ++i) {
        const int32_t x = (i % cols) * (kProbeButtonW + 8);
        const int32_t y = (i / cols) * (kProbeButtonH + 8);
        const lv_area_t btn{x, y, x + kProbeButtonW - 1, y + kProbeButtonH - 1};
        fill_glass(&layer, btn, GlassFamily::Raised, lv_color_hex(0x5E81AC), kGlassButtonShadow);
    }
    lv_canvas_finish_layer(canvas, &layer);
}

} // namespace

double measure_glass_frame_ms(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    lv_display_t* disp = lv_display_get_default();
    const lv_color_format_t cf = disp ? lv_display_get_color_format(disp) : LV_COLOR_FORMAT_NATIVE;
    lv_draw_buf_t* buf = lv_draw_buf_create(static_cast<uint32_t>(width),
                                            static_cast<uint32_t>(height), cf, LV_STRIDE_AUTO);
    if (buf == nullptr) {
        spdlog::warn("[Glass] Probe could not allocate a {}x{} buffer", width, height);
        return 0;
    }
    // A hidden canvas on the system layer: the layer round-trip needs an object, and a hidden
    // one never reaches the display.
    lv_obj_t* canvas = lv_canvas_create(lv_layer_sys());
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: offscreen probe canvas
    lv_canvas_set_draw_buf(canvas, buf);

    std::array<double, kProbeRepetitions> ms{};
    for (double& m : ms) {
        const auto start = std::chrono::steady_clock::now();
        draw_probe_frame(canvas, width, height);
        m = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);

    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

// ============================================================================
// The running gate
// ============================================================================

namespace {

struct GlassController {
    bool started = false;
    bool effective = false;
    GlassGateFacts facts;
    GlassMeasureFn measure;
    helix::ui::LvglTimerGuard probe_timer;
    ObserverGuard available_observer;
    ObserverGuard setting_observer;
};

GlassController& controller() {
    static GlassController c;
    return c;
}

const char* verdict_name(GlassGateVerdict v) {
    switch (v) {
    case GlassGateVerdict::Pass:
        return "pass";
    case GlassGateVerdict::Fail:
        return "fail";
    case GlassGateVerdict::NeedsProbe:
        break;
    }
    return "needs probe";
}

void update_effective() {
    auto& dsm = helix::DisplaySettingsManager::instance();
    const bool on = dsm.get_glass_available() && dsm.get_glass_effects();
    if (on != controller().effective) {
        spdlog::info("[Glass] Effective glass {} (available {}, setting {})", on ? "on" : "off",
                     dsm.get_glass_available(), dsm.get_glass_effects());
    }
    glass_set_effective(on);
}

lv_obj_tree_walk_res_t attach_one(lv_obj_t* obj, void* on) {
    if (on != nullptr) {
        glass_attach(obj);
    } else {
        glass_detach(obj);
    }
    return LV_OBJ_TREE_WALK_NEXT;
}

/// Every widget on every screen and layer of the default display.
void attach_every_widget(bool on) {
    lv_display_t* disp = lv_display_get_default();
    if (disp == nullptr) {
        return;
    }
    void* arg = on ? reinterpret_cast<void*>(1) : nullptr;
    for (uint32_t i = 0; i < disp->screen_cnt; ++i) {
        lv_obj_tree_walk(disp->screens[i], attach_one, arg);
    }
    for (lv_obj_t* layer : {lv_display_get_layer_bottom(disp), lv_display_get_layer_top(disp),
                            lv_display_get_layer_sys(disp)}) {
        if (layer != nullptr) {
            lv_obj_tree_walk(layer, attach_one, arg);
        }
    }
}

void run_probe(lv_timer_t*) {
    GlassController& c = controller();
    c.probe_timer.release();
    lv_display_t* disp = lv_display_get_default();
    const int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : 0;
    const int32_t h = disp ? lv_display_get_vertical_resolution(disp) : 0;
    GlassProbeRecord r;
    r.frame_ms = c.measure ? c.measure(w, h) : 0;
    r.passed = r.frame_ms > 0 && r.frame_ms <= kGlassFrameBudgetMs;
    r.version = c.facts.version;
    r.board = c.facts.board;
    spdlog::info("[Glass] Probe at {}x{}: {:.2f} ms per frame against {:.1f} ms, {}", w, h,
                 r.frame_ms, kGlassFrameBudgetMs, r.passed ? "pass" : "fail");
    if (helix::Config* config = helix::Config::get_instance()) {
        save_glass_probe(*config, r);
    }
    helix::DisplaySettingsManager::instance().set_glass_available(r.passed);
}

} // namespace

GlassGateFacts glass_gate_facts_now() {
    const helix::PlatformCapabilities caps = helix::PlatformCapabilities::detect();
    GlassGateFacts f;
    // A 32 bpp build on a 16 bpp framebuffer renders RGB565: the display's format is the depth.
    lv_display_t* disp = lv_display_get_default();
    f.color_depth =
        disp ? lv_color_format_get_size(lv_display_get_color_format(disp)) * 8 : LV_COLOR_DEPTH;
    f.multi_stop_gradients = !LV_USE_DRAW_NANOVG;
    f.style_effects_allowed = helix::full_style_effects_allowed(caps.tier);
    std::string backend;
    if (DisplayManager* dm = DisplayManager::instance()) {
        f.software_rotated = dm->is_software_rotated();
        if (const DisplayBackend* b = dm->backend()) {
            backend = display_backend_key(b->type(), b->is_gpu_accelerated());
        }
    }
    f.env_force = glass_env_force();
    if (const helix::Config* config = helix::Config::get_instance()) {
        f.stored = load_glass_probe(*config);
    }
    f.version = helix_version_full();
    f.board = board_fingerprint(current_board_facts(caps, backend));
    return f;
}

void glass_gate_start() {
    glass_gate_start(glass_gate_facts_now(), measure_glass_frame_ms);
}

void glass_gate_start(const GlassGateFacts& facts, GlassMeasureFn measure) {
    glass_gate_stop();
    GlassController& c = controller();
    c.started = true;
    c.facts = facts;
    c.measure = std::move(measure);

    auto& dsm = helix::DisplaySettingsManager::instance();
    const GlassGateVerdict verdict = decide_glass_gate(facts);
    spdlog::info("[Glass] Gate {} on {} ({}{})", verdict_name(verdict), facts.board, facts.version,
                 facts.env_force ? ", HELIX_GLASS override" : "");
    dsm.set_glass_available(verdict == GlassGateVerdict::Pass);

    // DisplaySettingsManager is a process-lifetime singleton, so `this` for the observers is
    // the controller, which outlives it.
    c.available_observer = helix::ui::observe<int>(
        dsm.subject_glass_available(), &c, [](GlassController*, int) { update_effective(); },
        dsm.get_subjects_lifetime(), helix::ui::Dispatch::Immediate);
    c.setting_observer = helix::ui::observe<int>(
        dsm.subject_glass_effects(), &c, [](GlassController*, int) { update_effective(); },
        dsm.get_subjects_lifetime(), helix::ui::Dispatch::Immediate);

    if (verdict == GlassGateVerdict::NeedsProbe) {
        lv_timer_t* t = lv_timer_create(run_probe, kGlassProbeDelayMs, nullptr);
        lv_timer_set_repeat_count(t, 1);
        c.probe_timer.reset(t);
    }
}

void glass_gate_stop() {
    GlassController& c = controller();
    c.probe_timer.reset();
    c.available_observer.reset();
    c.setting_observer.reset();
    if (c.started) {
        glass_set_effective(false);
    }
    c.started = false;
}

void glass_set_effective(bool on) {
    GlassController& c = controller();
    if (c.effective == on) {
        return;
    }
    c.effective = on;
    ThemeManager::instance().set_glass(on);
    attach_every_widget(on);
}

bool glass_effective() {
    return controller().effective;
}

void glass_on_object_created(lv_obj_t* obj) {
    if (controller().effective) {
        glass_attach(obj);
    }
}

} // namespace helix::ui
