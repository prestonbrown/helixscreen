// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_mesh_tile_widget.h"

#include "ui_bed_mesh.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_panel_bed_mesh.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "bed_mesh_calibration_plan.h"
#include "bed_mesh_probe_temp.h"
#include "bed_mesh_state.h"
#include "bed_mesh_stats.h"
#include "helix_psram_attr.h"
#include "i18n_plural.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "platform_capabilities.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "standard_macros.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "toolhead_homing.h"
#include "value_status.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using helix::ui::copy_string_if_changed;
using helix::ui::measure_text_px;
using helix::ui::set_int_if_changed;
using helix::ui::widest_word_px;

namespace {

// The verdict and texts the widget publishes, read by panel_widget_bed_mesh_tile.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
HELIX_PSRAM_BSS lv_subject_t s_rung_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_map_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_range_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_empty_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_details_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_scale_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_footer_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_calibrate_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_load_subject{};
HELIX_PSRAM_BSS lv_subject_t s_calibrating_subject{};
HELIX_PSRAM_BSS lv_subject_t s_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_range_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_extent_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_scale_low_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_scale_high_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_count_text_subject{};
HELIX_PSRAM_BSS char s_range_text_buf[16];
HELIX_PSRAM_BSS char s_extent_text_buf[32];
HELIX_PSRAM_BSS char s_scale_low_text_buf[16];
HELIX_PSRAM_BSS char s_scale_high_text_buf[16];
HELIX_PSRAM_BSS char s_count_text_buf[32];
helix::WidgetSubjects s_subjects{"BedMeshTileWidgetSubjects"};
// The instance bound to the live tile. Only it publishes and acts: a transient instance
// (a fits_at probe, a rebuild's outgoing widget) must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::BedMeshTileWidget> s_tile;
std::optional<bool> s_full_tier_override;
std::optional<bool> s_panel_available_override;
std::function<void()> s_open_hook;

void bed_mesh_tile_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Cell),
                               "bed_mesh_tile_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_map_subject, 0, "bed_mesh_tile_show_map", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_range_subject, 0, "bed_mesh_tile_show_range", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_empty_subject, 0, "bed_mesh_tile_show_empty", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_details_subject, 0, "bed_mesh_tile_show_details", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_scale_subject, 0, "bed_mesh_tile_show_scale", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_footer_subject, 0, "bed_mesh_tile_show_footer", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_calibrate_subject, 0, "bed_mesh_tile_show_calibrate",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_load_subject, 0, "bed_mesh_tile_show_load", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_calibrating_subject, 0, "bed_mesh_tile_calibrating", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_status_subject, 0, "bed_mesh_tile_status", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_range_text_subject, s_range_text_buf, "",
                                  "bed_mesh_tile_range_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_extent_text_subject, s_extent_text_buf, "",
                                  "bed_mesh_tile_extent_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_scale_low_text_subject, s_scale_low_text_buf, "",
                                  "bed_mesh_tile_scale_low_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_scale_high_text_subject, s_scale_high_text_buf, "",
                                  "bed_mesh_tile_scale_high_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_count_text_subject, s_count_text_buf, "",
                                  "bed_mesh_tile_count_text", s_subjects);
    });
}

std::string profile_count_text(int count) {
    char buf[48];
    std::snprintf(buf, sizeof(buf),
                  helix::ui::tr_plural("1 profile", "%d profiles", static_cast<uint64_t>(count)),
                  count);
    return buf;
}

bool platform_is_full_tier() {
    if (s_full_tier_override)
        return *s_full_tier_override;
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    return tier && static_cast<helix::PlatformTier>(lv_subject_get_int(tier)) ==
                       helix::PlatformTier::STANDARD;
}

constexpr bool kCanvasAvailable = HELIX_HAS_BED_MESH_3D != 0;

#if defined(HELIX_PLATFORM_ESP32)
constexpr bool kPanelBuilt = false;
#else
constexpr bool kPanelBuilt = true;
#endif

bool panel_is_available() {
    return s_panel_available_override.value_or(kPanelBuilt);
}

struct Fonts {
    const lv_font_t* xs;
    const lv_font_t* small;
    const lv_font_t* body;
    const lv_font_t* icon_xs;
    const lv_font_t* icon_lg;
};

Fonts load_fonts() {
    return {theme_manager_get_font("font_xs"), theme_manager_get_font("font_small"),
            theme_manager_get_font("font_body"), theme_manager_get_font("icon_font_xs"),
            theme_manager_get_font("icon_font_lg")};
}

using helix::line_px;

/// The state a rung's content depends on.
struct MeasureState {
    bool loaded = false;
    bool print_active = false;
    bool panel_available = true;
    int profile_count = 0;
    std::string profile_name;
};

/// The minimum content box each rung draws in. Mirrors panel_widget_bed_mesh_tile.xml:
/// spacing is the XML's pad_row (space_xxs) and pad_column (space_xs). The size-class
/// rungs (Wide, Full, Large) also hold to the per-tier bands, which compare against the
/// widget's whole extent, so the chrome is added back.
helix::RungNeeds measure_needs(const MeasureState& m, const helix::ContentChrome& chrome) {
    using helix::rung_box;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);
    constexpr int kSlackPx = 1;
    const int button_px = theme_manager_get_spacing("button_height_sm");

    const int icon_w = measure_text_px(helix::ui::icon::lookup_codepoint("grid_large"), f.icon_xs);
    const int icon_h = line_px(f.icon_xs);
    const int icon_lg_w = measure_text_px(helix::ui::icon::lookup_codepoint("grid_off"), f.icon_lg);
    const int icon_lg_h = line_px(f.icon_lg);
    const int xs_h = line_px(f.xs);
    const int small_h = line_px(f.small);
    const int body_h = line_px(f.body);

    // The smallest heatmap that still reads as a grid; without the renderer a glyph
    // stands in for it.
    const int map_min = kCanvasAvailable ? 2 * xs_h + 2 : icon_h;

    // The widest values the labels can show, so the rung does not change as the numbers do.
    const int range2_w = measure_text_px(helix::format_mesh_range(8.88f, 2), f.xs);
    const int range3_xs_w = measure_text_px(helix::format_mesh_range(8.888f, 3), f.xs);
    const int range3_small_w = measure_text_px(helix::format_mesh_range(8.888f, 3), f.small);
    const std::string empty_text = lv_tr("No mesh loaded");
    const int empty_w = measure_text_px(empty_text, f.small);
    const int widest_word_w = widest_word_px(empty_text, f.small);

    helix::RungNeeds needs;
    if (!m.loaded) {
        // Nothing to draw but a glyph, and from Cell up what it means.
        needs.glance = rung_box(icon_w + tight, icon_h + kSlackPx);
        // Strip and Column would draw what Glance does, so they stay unoffered.
        // The text wraps at a Cell, so it needs its widest word across and two lines down.
        needs.cell = rung_box(std::max<int>(icon_lg_w, widest_word_w) + comfort,
                              icon_lg_h + row_gap + 2 * small_h + comfort + kSlackPx);
        needs.wide = rung_box(icon_lg_w + col_gap + empty_w + comfort,
                              std::max<int>(icon_lg_h, small_h) + comfort + kSlackPx);
        int full_w = needs.cell.w;
        int full_h = needs.cell.h;
        if (!m.print_active) {
            // The buttons stack, each as wide as its label in the body face plus padding.
            const int button_label_pad = 2 * theme_manager_get_spacing("space_lg");
            const int calibrate_w = measure_text_px(lv_tr("Calibrate"), f.body) + button_label_pad;
            const int load_w = measure_text_px(lv_tr("Load Profile"), f.body) + button_label_pad;
            const bool offers_load = m.profile_count > 0 && m.panel_available;
            const int buttons = offers_load ? 2 : 1;
            full_w =
                std::max<int>({full_w, calibrate_w + comfort, offers_load ? load_w + comfort : 0});
            full_h += buttons * (row_gap + button_px);
        }
        needs.full = rung_box(full_w, full_h);
    } else {
        needs.glance =
            rung_box(std::max<int>(icon_w, range2_w) + tight, icon_h + row_gap + xs_h + kSlackPx);
        needs.strip = rung_box(map_min + col_gap + range3_xs_w + tight,
                               std::max<int>(map_min, xs_h) + kSlackPx);
        needs.column = rung_box(std::max<int>(map_min, range3_xs_w) + tight,
                                map_min + row_gap + xs_h + kSlackPx);
        // A square-ish cell holds a wider heatmap than a half-cell column does, which is
        // what separates Cell from Column.
        needs.cell =
            rung_box(map_min + 3 * comfort, map_min + row_gap + small_h + comfort + kSlackPx);

        const std::string range_unit = helix::format_mesh_range(8.888f, 3) + " mm";
        const int name_cap_w = measure_text_px("MMMMMMMM", f.small);
        const int name_w = std::min<int>(measure_text_px(m.profile_name, f.small), name_cap_w);
        const int details_w =
            std::max<int>({name_w, measure_text_px(range_unit, f.body),
                           measure_text_px(helix::format_mesh_extent(-8.88f, 8.88f), f.xs),
                           measure_text_px("88x88 points", f.xs)});
        const int details_h = small_h + body_h + 2 * xs_h + 3 * row_gap;
        needs.wide = rung_box(map_min + col_gap + details_w + comfort,
                              std::max<int>(map_min, details_h) + comfort + kSlackPx);

        const int scale_h = std::max<int>(theme_manager_get_spacing("space_xs"), xs_h);
        const int footer_h = small_h + xs_h;
        const int footer_w = name_w + col_gap + measure_text_px(range_unit, f.small);
        const int scale_w =
            2 * measure_text_px(helix::format_mesh_extent(-8.88f, 8.88f), f.xs) / 2 + 6 * col_gap;
        needs.full =
            rung_box(std::max<int>(footer_w, scale_w) + comfort,
                     2 * map_min + 2 * row_gap + (kCanvasAvailable ? scale_h + row_gap : 0) +
                         footer_h + comfort + kSlackPx);
    }

    // Size classes: 2 columns for Wide, 2x2 for Full, 3x3 for Large.
    const int w_normal = helix::widget_size::w_normal() - chrome.w;
    const int w_wide = helix::widget_size::w_wide() - chrome.w;
    const int h_tall = helix::widget_size::h_tall() - chrome.h;
    const int h_taller = helix::widget_size::h_taller() - chrome.h;
    needs.wide.w = std::max<int>(needs.wide.w, w_normal);
    needs.full.w = std::max<int>(needs.full.w, w_normal);
    needs.full.h = std::max<int>(needs.full.h, h_tall);
    // The Large rung differs from Full only by the 3D render on a full-tier board, so it
    // is offered at 3x3 and up regardless of tier.
    needs.large =
        rung_box(std::max<int>(needs.full.w, w_wide), std::max<int>(needs.full.h, h_taller));
    return needs;
}

} // namespace

namespace helix {

struct BedMeshTileWidget::LiveState {
    MeasureState measure;
    MeshStats stats;
    bool full_tier = false;
};

BedMeshTileWidget::BedMeshTileWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

BedMeshTileWidget::~BedMeshTileWidget() {
    detach();
}

BedMeshTileWidget::LiveState BedMeshTileWidget::read_live() const {
    auto& state = BedMeshState::instance();
    LiveState s;
    s.stats = state.stats();
    s.measure.loaded = s.stats.loaded;
    s.measure.print_active =
        lv_subject_get_int(printer_state_.print_state().get_print_active_subject()) != 0;
    s.measure.panel_available = panel_is_available();
    s.measure.profile_count = state.profile_count();
    s.measure.profile_name = state.profile().name;
    s.full_tier = platform_is_full_tier();
    return s;
}

BedMeshTileVerdict BedMeshTileWidget::decide(int width_px, int height_px,
                                             const LiveState& live) const {
    // The tree's own padding where it exists, and before it does (the load path asks
    // fits_at then) the root's. measure_needs() counts the gaps between the body's parts.
    const ContentChrome chrome = content_padding_or_estimate(root(), "space_xxs", "space_xxs");
    BedMeshTileInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.mesh_loaded = live.measure.loaded;
    in.print_active = live.measure.print_active;
    in.full_tier = live.full_tier;
    in.canvas_available = kCanvasAvailable;
    in.panel_available = live.measure.panel_available;
    in.profile_count = live.measure.profile_count;
    in.needs = measure_needs(live.measure, chrome);
    const auto& n = in.needs;
    spdlog::trace("[BedMeshTileWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{} l{}x{} loaded={} printing={}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h, n.large.w, n.large.h, in.mesh_loaded, in.print_active);
    return decide_bed_mesh_tile_layout(in);
}

void BedMeshTileWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    auto& mesh_state = BedMeshState::instance();
    const auto token = lifetime_.token();
    const auto on_live = [token](BedMeshTileWidget* self, int) {
        if (token.expired())
            return;
        self->on_live_changed();
    };
    version_observer_ = ui::observe<int>(
        mesh_state.get_version_subject(), this,
        [token](BedMeshTileWidget* self, int) {
            if (token.expired())
                return;
            self->on_mesh_changed();
        },
        mesh_state.get_subjects_lifetime());
    print_active_observer_ =
        ui::observe<int>(printer_state_.print_state().get_print_active_subject(), this, on_live,
                         printer_state_.get_subjects_lifetime());
    profile_count_observer_ = ui::observe<int>(mesh_state.get_profile_count_subject(), this,
                                               on_live, mesh_state.get_subjects_lifetime());
    // The tile's text is measured in C++, so a new language re-takes the verdict.
    language_observer_ = ui::observe_language_change(this, [](BedMeshTileWidget* self) {
        BedMeshState::instance().retranslate();
        self->relayout_for_granted_size();
        self->publish_texts(self->read_live());
    });
    // The bed bounds arrive after connect; a mesh fed before them is fed again then.
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        bounds_version_ = lv_subject_get_int(api->get_build_volume_version_subject());
        build_volume_observer_ = ui::observe<int>(
            api->get_build_volume_version_subject(), this,
            [token](BedMeshTileWidget* self, int version) {
                if (token.expired() || self->bounds_version_ == version)
                    return;
                self->bounds_version_ = version;
                self->fed_version_.reset();
                self->feed_map(self->read_live());
            },
            subject_never_freed());
    }

    if (lv_obj_t* map = find_map()) {
        ui_bed_mesh_set_static_mode(map, true);
        ui_bed_mesh_set_zero_plane_visible(map, false);
    }
    fed_version_.reset();
    applied_3d_.reset();
    panel_hidden_ = false;
    set_calibrating(false);

    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    publish_texts(read_live());
}

void BedMeshTileWidget::detach() {
    lifetime_.invalidate();
    version_observer_.reset();
    print_active_observer_.reset();
    profile_count_observer_.reset();
    build_volume_observer_.reset();
    language_observer_.reset();
    if (BedMeshTileWidget* displaced = s_tile.release(this)) {
        displaced->fed_version_.reset();
        displaced->applied_3d_.reset();
        displaced->relayout_for_granted_size();
        displaced->publish_texts(displaced->read_live());
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

lv_obj_t* BedMeshTileWidget::find_map() const {
    return root() ? lv_obj_find_by_name(root(), "bed_mesh_tile_map") : nullptr;
}

void BedMeshTileWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                        int height_px) {
    const LiveState live = read_live();
    verdict_ = decide(width_px, height_px, live);
    decided_loaded_ = live.measure.loaded;
    decided_print_active_ = live.measure.print_active;
    decided_profile_count_ = live.measure.profile_count;
    if (!s_tile.is_owner(this))
        return;

    publish_verdict();
    publish_texts(live);
    feed_map(live);

    spdlog::debug("[BedMeshTileWidget] on_size_changed {}x{} rung={} map={} range={} empty={} "
                  "details={} scale={} footer={} calibrate={} load={} 3d={} fits={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.show_map,
                  verdict_.show_range, verdict_.show_empty_text, verdict_.show_details,
                  verdict_.show_scale, verdict_.show_footer, verdict_.show_calibrate,
                  verdict_.show_load_profile, verdict_.render_3d, verdict_.fits);
}

bool BedMeshTileWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content. The loaded state's Glance is the
    // larger, so one probe decides.
    LiveState probe;
    probe.measure.loaded = true;
    return decide(width_px, height_px, probe).fits;
}

void BedMeshTileWidget::publish_verdict() {
    lv_subject_set_int(&s_rung_subject, static_cast<int>(verdict_.rung));
    set_int_if_changed(&s_show_map_subject, verdict_.show_map ? 1 : 0);
    set_int_if_changed(&s_show_range_subject, verdict_.show_range ? 1 : 0);
    set_int_if_changed(&s_show_empty_subject, verdict_.show_empty_text ? 1 : 0);
    set_int_if_changed(&s_show_details_subject, verdict_.show_details ? 1 : 0);
    set_int_if_changed(&s_show_scale_subject, verdict_.show_scale ? 1 : 0);
    set_int_if_changed(&s_show_footer_subject, verdict_.show_footer ? 1 : 0);
    set_int_if_changed(&s_show_calibrate_subject, verdict_.show_calibrate ? 1 : 0);
    set_int_if_changed(&s_show_load_subject, verdict_.show_load_profile ? 1 : 0);
}

void BedMeshTileWidget::publish_texts(const LiveState& live) {
    if (!s_tile.is_owner(this))
        return;
    const MeshStats& st = live.stats;
    copy_string_if_changed(
        &s_range_text_subject,
        std::string(st.loaded ? format_mesh_range(st.range, verdict_.decimals) : std::string())
            .c_str());
    copy_string_if_changed(
        &s_extent_text_subject,
        std::string(st.loaded ? format_mesh_extent(st.min_z, st.max_z) : std::string()).c_str());
    char low[16] = "";
    char high[16] = "";
    if (st.loaded) {
        std::snprintf(low, sizeof(low), "%.2f", static_cast<double>(st.min_z));
        std::snprintf(high, sizeof(high), "%.2f", static_cast<double>(st.max_z));
    }
    copy_string_if_changed(&s_scale_low_text_subject, low);
    copy_string_if_changed(&s_scale_high_text_subject, high);
    copy_string_if_changed(&s_count_text_subject,
                           std::string(profile_count_text(live.measure.profile_count)).c_str());
    lv_subject_set_int(&s_status_subject,
                       st.loaded ? static_cast<int>(status::mesh_flatness_status(st.range)) : 0);
}

void BedMeshTileWidget::on_live_changed() {
    if (panel_hidden_)
        return;
    const LiveState live = read_live();
    if (!panel_hidden_ && (live.measure.loaded != decided_loaded_ ||
                           live.measure.print_active != decided_print_active_ ||
                           live.measure.profile_count != decided_profile_count_)) {
        relayout_for_granted_size();
    }
    publish_texts(live);
}

void BedMeshTileWidget::on_activate() {
    panel_hidden_ = false;
    // Whatever changed while the panel was covered is applied once, now.
    relayout_for_granted_size();
    publish_texts(read_live());
}

void BedMeshTileWidget::on_deactivate() {
    panel_hidden_ = true;
}

void BedMeshTileWidget::set_calibrating(bool on) {
    calibrating_ = on;
    if (s_tile.is_owner(this))
        set_int_if_changed(&s_calibrating_subject, on ? 1 : 0);
}

void BedMeshTileWidget::on_mesh_changed() {
    // A new mesh is the calibration's result.
    set_calibrating(false);
    if (panel_hidden_)
        return;
    // A different mesh can change what the rungs need (loaded or not, the profile name's
    // width), so the verdict is decided again before the canvas is fed.
    relayout_for_granted_size();
    const LiveState live = read_live();
    publish_texts(live);
    if (s_tile.is_owner(this))
        feed_map(live);
}

void BedMeshTileWidget::feed_map(const LiveState& live) {
    if (!verdict_.show_map || !live.stats.loaded)
        return;
    lv_obj_t* map = find_map();
    if (!map)
        return;

    // The canvas drops to 2D for good after a slow 3D render; the tile stops asking for 3D.
    if (!static_3d_banned_ && ui_bed_mesh_static_3d_banned(map)) {
        static_3d_banned_ = true;
        spdlog::info("[BedMeshTileWidget] Static 3D render overran its budget; drawing 2D");
    }
    const bool want_3d = verdict_.render_3d && !static_3d_banned_;
    if (applied_3d_ != want_3d) {
        ui_bed_mesh_set_render_mode(map, want_3d ? BedMeshRenderMode::Force3D
                                                 : BedMeshRenderMode::Force2D);
        applied_3d_ = want_3d;
        fed_version_.reset();
    }
    auto& mesh_state = BedMeshState::instance();
    const int version = lv_subject_get_int(mesh_state.get_version_subject());
    if (fed_version_ == version)
        return;

    const BedMeshProfile& profile = mesh_state.profile();
    const std::vector<std::vector<float>> matrix = mean_normalized_matrix(profile, live.stats);
    std::vector<const float*> rows;
    rows.reserve(matrix.size());
    for (const auto& row : matrix)
        rows.push_back(row.data());
    if (!ui_bed_mesh_set_data(map, rows.data(), live.stats.rows, live.stats.cols))
        return;

    // The bed outline comes from the build volume; before it is known, the probed area
    // stands in for it so the mesh still fills the canvas.
    double mesh_x0 = profile.mesh_min[0], mesh_x1 = profile.mesh_max[0];
    double mesh_y0 = profile.mesh_min[1], mesh_y1 = profile.mesh_max[1];
    double bed_x0 = mesh_x0, bed_x1 = mesh_x1, bed_y0 = mesh_y0, bed_y1 = mesh_y1;
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        const auto& bed = api->hardware().build_volume();
        if (bed.x_max > bed.x_min && bed.y_max > bed.y_min) {
            bed_x0 = bed.x_min;
            bed_x1 = bed.x_max;
            bed_y0 = bed.y_min;
            bed_y1 = bed.y_max;
        }
    }
    ui_bed_mesh_set_bounds(map, bed_x0, bed_x1, bed_y0, bed_y1, mesh_x0, mesh_x1, mesh_y0, mesh_y1);
    ui_bed_mesh_set_z_display_offset(map, static_cast<double>(live.stats.mean_z));
    fed_version_ = version;
}

void BedMeshTileWidget::open_panel() {
    if (!panel_is_available())
        return;
    if (s_open_hook) {
        s_open_hook();
        return;
    }
#if !defined(HELIX_PLATFORM_ESP32)
    helix::ui::open_bed_mesh_panel(parent_screen_);
#endif
}

void BedMeshTileWidget::calibrate() {
    if (!verdict_.show_calibrate || !s_tile.is_owner(this) || calibrating_)
        return;
    const bool blocked =
        lv_subject_get_int(printer_state_.print_state().get_machine_motion_blocked_subject()) != 0;
    IMoonrakerAPI* api = get_moonraker_api();
    if (blocked || !api)
        return;

    // The same ladder the panel plans from (configured > shipped > detected), into the
    // profile Klipper loads at startup.
    const auto& info = StandardMacros::instance().get(StandardMacroSlot::BedMesh);
    const int bed_temp_c = bed_mesh::probe_bed_temp_c(
        lv_subject_get_int(printer_state_.temperature_state().get_bed_target_subject()),
        lv_subject_get_int(printer_state_.temperature_state().get_bed_temp_subject()));
    const auto plan = bed_mesh::plan_calibration(info, bed_mesh::kDefaultProfile, bed_temp_c);

    set_calibrating(true);
    ensure_homed_then(
        api, lifetime_,
        [this, api, plan]() {
            spdlog::info("[BedMeshTileWidget] Calibrating with: {}", plan.command);
            // The error arrives on the WebSocket thread; the toast is main-thread only.
            api->advanced().start_bed_mesh_calibrate(
                IAdvancedAPI::BedMeshCommand{plan.command, plan.self_prepares, plan.shipped},
                [](int, int) {}, []() {},
                lifetime_.bg_cb(
                    "BedMeshTileWidget::calibrate_error", [this](const MoonrakerError& err) {
                        set_calibrating(false);
                        spdlog::warn("[BedMeshTileWidget] Calibration failed: {}", err.message);
                        NOTIFY_ERROR(lv_tr("Bed mesh calibration failed: {}"), err.message);
                    }));
        },
        [this](const MoonrakerError& err) {
            set_calibrating(false);
            spdlog::warn("[BedMeshTileWidget] Homing before calibration failed: {}", err.message);
            NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.message);
        });
}

void BedMeshTileWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BedMeshTileWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<BedMeshTileWidget>(e)) {
        self->record_interaction();
        self->open_panel();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void register_bed_mesh_tile_widget() {
    register_widget_factory("bed_mesh_tile", [](const std::string&) {
        return std::make_unique<BedMeshTileWidget>(get_printer_state());
    });
    register_widget_subjects("bed_mesh_tile", bed_mesh_tile_init_subjects);

    lv_xml_register_event_cb(nullptr, "bed_mesh_tile_clicked_cb", BedMeshTileWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "bed_mesh_tile_calibrate_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[BedMeshTileWidget] calibrate_cb");
        // A button tap is not a tap on the tile, which opens the panel.
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->calibrate();
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "bed_mesh_tile_load_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[BedMeshTileWidget] load_cb");
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->open_panel();
        LVGL_SAFE_EVENT_CB_END();
    });
}

void set_bed_mesh_tile_open_hook_for_test(std::function<void()> hook) {
    s_open_hook = std::move(hook);
}

void set_bed_mesh_tile_panel_available_for_test(std::optional<bool> available) {
    s_panel_available_override = available;
}

void set_bed_mesh_tile_full_tier_for_test(std::optional<bool> full_tier) {
    s_full_tier_override = full_tier;
}

} // namespace helix
