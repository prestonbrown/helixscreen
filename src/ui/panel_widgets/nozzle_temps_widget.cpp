// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_temps_widget.h"

#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_overlay_temp_graph.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "display_numbering.h"
#include "lvgl/src/misc/lv_text_private.h" // lv_text_get_width, lv_text_attributes_t
#include "lvgl/src/others/translation/lv_translation.h"
#include "nozzle_layout.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace {

// The published layout verdict, read by bind_flag_if / bind_style_if in
// nozzle_temp_row.xml, nozzle_temp_bed_row.xml and panel_widget_nozzle_temps.xml.
// Module-level (not members): the verdict belongs to the widget KIND, rows
// read the CURRENT values the moment they are created, and registration rides
// SubjectInitializer's panel-subject phase so the subjects exist before any
// XML that binds them is parsed. Values mirror decide_nozzle_layout()'s
// output: label_mode is NozzleLabelMode's int (0=none 1=number 2=short
// 3=long), columns is 1 or 2, compact is 0 or 1.
lv_subject_t s_label_mode_subject;
lv_subject_t s_columns_subject;
lv_subject_t s_compact_font_subject;
bool s_subjects_initialized = false;

void nozzle_temps_widget_init_subjects() {
    if (s_subjects_initialized)
        return;

    lv_subject_init_int(&s_label_mode_subject,
                        static_cast<int>(helix::NozzleLabelMode::Long)); // the degenerate default
    lv_xml_register_subject(nullptr, "nozzle_row_label_mode", &s_label_mode_subject);
    lv_subject_init_int(&s_columns_subject, 1);
    lv_xml_register_subject(nullptr, "nozzle_row_columns", &s_columns_subject);
    lv_subject_init_int(&s_compact_font_subject, 0);
    lv_xml_register_subject(nullptr, "nozzle_row_compact", &s_compact_font_subject);
    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("NozzleTempsWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_compact_font_subject);
            lv_subject_deinit(&s_columns_subject);
            lv_subject_deinit(&s_label_mode_subject);
            s_subjects_initialized = false;
        }
    });
}

/// The two strings a row's value shows: the current temperature and, when a
/// target is set, the target half; "off" otherwise. Composed once, shared by
/// the code that renders it (update_row_display) and the code that measures
/// it (on_size_changed) — if the two ever drift, the ladder decides on widths
/// the row does not draw.
std::pair<std::string, std::string> value_halves(int temp_deci, int target_deci) {
    char num_buf[16];
    helix::ui::temperature::format_temp_number(helix::ui::temperature::deci_to_degrees_f(temp_deci),
                                               num_buf, sizeof(num_buf));
    std::string current = std::string(num_buf) + "\xC2\xB0";
    if (target_deci <= 0)
        return {current, lv_tr("off")};
    helix::ui::temperature::format_temp_number(
        helix::ui::temperature::deci_to_degrees_f(target_deci), num_buf, sizeof(num_buf));
    return {current, "/ " + std::string(num_buf) + "\xC2\xB0"};
}

} // namespace

namespace helix {

void register_nozzle_temps_widget() {
    register_widget_factory("nozzle_temps", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<NozzleTempsWidget>(ps);
    });
    register_widget_subjects("nozzle_temps", nozzle_temps_widget_init_subjects);
}

} // namespace helix

std::vector<std::string> helix::distinct_extruder_names(const std::vector<ToolInfo>& tools) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& tool : tools) {
        if (!tool.extruder_name)
            continue;
        if (seen.insert(*tool.extruder_name).second)
            result.push_back(*tool.extruder_name);
    }
    return result;
}

using namespace helix;

NozzleTempsWidget::NozzleTempsWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

NozzleTempsWidget::~NozzleTempsWidget() {
    detach();
}

void NozzleTempsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    install_delete_hook(widget_obj);

    rebuild_rows();

    // Observe extruder version changes to rebuild rows when tools are discovered.
    // Capture current version to skip the initial immediate callback — rows already built.
    //
    // NOTE: Do NOT gate this on lifetime_.token(). The lifetime is invalidated on
    // every rebuild_rows() → clear_rows() call, which would make the version observer
    // a one-shot: the first rebuild expires the token, and subsequent version changes
    // are silently ignored (#782). This left the widget with stale rows pointing to
    // freed subjects, crashing in lv_observer_remove during the next clear_rows().
    //
    // Safety is provided by: (1) weak_alive in observe_int_sync context (expires when
    // version_observer_ is reset in detach()), (2) rebuilding_ re-entrancy guard,
    // (3) initial_version skip for the attach-time callback.
    int initial_version = lv_subject_get_int(printer_state_.get_extruder_version_subject());
    version_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
        printer_state_.get_extruder_version_subject(), this,
        [initial_version](NozzleTempsWidget* self, int version) {
            if (version == initial_version)
                return; // Skip initial callback — rows already built in attach()
            self->rebuild_rows();
        },
        printer_state_.get_subjects_lifetime());

    spdlog::debug("[NozzleTempsWidget] Attached with {} extruder rows", extruder_rows_.size());
}

void NozzleTempsWidget::detach() {
    lifetime_.invalidate();
    version_observer_.reset();
    clear_rows();
    uninstall_delete_hook();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void NozzleTempsWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: expire the pending deferred observer
    // callbacks and drop the cached pointers only. The observers themselves
    // stay registered on their (still live) subjects until detach() or the
    // destructor resets them — every callback checks its token first, so a
    // drained apply no-ops instead of writing to the freed row labels.
    lifetime_.invalidate();
    forget_row_widgets();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void NozzleTempsWidget::forget_row_widgets() {
    for (auto& row : extruder_rows_) {
        row.row_obj = nullptr;
        row.label_long = nullptr;
        row.label_short = nullptr;
        row.label_number = nullptr;
        row.temp_label = nullptr;
        row.target_label = nullptr;
    }
    bed_row_ = nullptr;
    bed_icon_ = nullptr;
    bed_temp_label_ = nullptr;
    bed_target_label_ = nullptr;
}

void NozzleTempsWidget::clear_rows() {
    // Invalidate lifetime to expire all pending deferred observer callbacks.
    // This replaces drain() which caused re-entrant process_pending() crashes
    // when SensorState::set_sensors() → drain() → rebuild_rows() → clear_rows()
    // → drain() ran while subjects were in a half-torn-down state (#732).
    // Freeze prevents new callbacks from being queued during cleanup.
    lifetime_.invalidate();
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();

    ++rebuild_gen_;

    // Release lifetime tokens BEFORE destroying observers. The ObserverGuard
    // holds a weak_ptr to the SubjectLifetime shared_ptr. If the dynamic subject
    // was already destroyed (reconnection), the source's shared_ptr is gone —
    // but our row's copy keeps the weak_ptr alive. Resetting our copy first
    // lets the weak_ptr expire, so ObserverGuard::reset() safely skips
    // lv_observer_remove() on the freed observer. (#673, #698)
    for (auto& row : extruder_rows_) {
        row.temp_lifetime.reset();
        row.target_lifetime.reset();
        row.temp_observer.reset();
        row.target_observer.reset();
    }
    extruder_rows_.clear();
    // Bed subjects are also destroyed during deinit_subjects() — need lifetime
    // tokens to prevent lv_observer_remove() on freed subjects (#734)
    bed_temp_lifetime_.reset();
    bed_target_lifetime_.reset();
    bed_temp_observer_.reset();
    bed_target_observer_.reset();

    auto* container =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "nozzle_temps_container") : nullptr;
    if (container)
        helix::ui::safe_clean_children(container);

    forget_row_widgets();
    cached_bed_temp_ = 0;
    cached_bed_target_ = 0;
}

void NozzleTempsWidget::rebuild_rows() {
    // Guard against re-entrancy from deferred observer callbacks that may
    // trigger version changes during the rebuild cycle (#723, #724, #725).
    if (rebuilding_)
        return;
    rebuilding_ = true;

    clear_rows();

    auto* container =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "nozzle_temps_container") : nullptr;
    if (!container) {
        spdlog::warn("[NozzleTempsWidget] Container not found in XML");
        rebuilding_ = false;
        return;
    }

    auto token = lifetime_.token();

    // One row per PHYSICAL extruder. Multiplexing backends (AFC BoxTurtle, Happy
    // Hare, ERCF) expose one logical tool per spool lane (T0..Tn) that all feed a
    // single extruder; without the collapse a 4-lane unit shows its one nozzle
    // temp four times. A true toolchanger maps each tool to a distinct extruder,
    // so every nozzle still gets its own row.
    for (const auto& extruder_name : distinct_extruder_names(ToolState::instance().tools())) {
        ExtruderRow row;
        row.name = extruder_name;
        create_extruder_row(container, row);

        // Observe per-extruder temp subject with lifetime token
        lv_subject_t* temp_subj =
            printer_state_.get_extruder_temp_subject(row.name, row.temp_lifetime);
        lv_subject_t* target_subj =
            printer_state_.get_extruder_target_subject(row.name, row.target_lifetime);

        if (temp_subj) {
            row.cached_temp = lv_subject_get_int(temp_subj);
            auto* temp_lbl = row.temp_label;
            auto* target_lbl = row.target_label;
            row.temp_observer = helix::ui::observe_int_sync<NozzleTempsWidget>(
                temp_subj, this,
                [token, idx = extruder_rows_.size(), temp_lbl, target_lbl](NozzleTempsWidget* self,
                                                                           int temp) {
                    if (token.expired())
                        return;
                    if (idx < self->extruder_rows_.size()) {
                        self->extruder_rows_[idx].cached_temp = temp;
                        self->update_row_display(temp_lbl, target_lbl, temp,
                                                 self->extruder_rows_[idx].cached_target, false);
                    }
                },
                row.temp_lifetime);
        }

        if (target_subj) {
            row.cached_target = lv_subject_get_int(target_subj);
            auto* temp_lbl = row.temp_label;
            auto* target_lbl = row.target_label;
            row.target_observer = helix::ui::observe_int_sync<NozzleTempsWidget>(
                target_subj, this,
                [token, idx = extruder_rows_.size(), temp_lbl, target_lbl](NozzleTempsWidget* self,
                                                                           int target) {
                    if (token.expired())
                        return;
                    if (idx < self->extruder_rows_.size()) {
                        const bool had_target = self->extruder_rows_[idx].cached_target > 0;
                        self->extruder_rows_[idx].cached_target = target;
                        self->update_row_display(temp_lbl, target_lbl,
                                                 self->extruder_rows_[idx].cached_temp, target,
                                                 false);
                        // The ladder budgets the value's two shapes — target
                        // set and target off — and a tile sized under one
                        // shape must re-decide under the other: setting a
                        // target widens every row past the rung chosen while
                        // idle. The transition is the only moment the value's
                        // WIDTH CLASS changes, so it is the only re-decide.
                        if (had_target != (target > 0))
                            self->relayout_for_granted_size();
                    }
                },
                row.target_lifetime);
        }

        // Initial display update
        update_row_display(row.temp_label, row.target_label, row.cached_temp, row.cached_target,
                           false);

        extruder_rows_.push_back(std::move(row));
    }

    // Bed row at the end
    create_bed_row(container);

    // Bed subjects are destroyed during deinit_subjects() — use lifetime tokens (#734)
    lv_subject_t* bed_temp_subj = printer_state_.get_bed_temp_subject(bed_temp_lifetime_);
    lv_subject_t* bed_target_subj = printer_state_.get_bed_target_subject(bed_target_lifetime_);

    if (bed_temp_subj) {
        cached_bed_temp_ = lv_subject_get_int(bed_temp_subj);
        bed_temp_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
            bed_temp_subj, this,
            [token](NozzleTempsWidget* self, int temp) {
                if (token.expired())
                    return;
                self->cached_bed_temp_ = temp;
                self->update_row_display(self->bed_temp_label_, self->bed_target_label_, temp,
                                         self->cached_bed_target_, true);
            },
            bed_temp_lifetime_);
    }

    if (bed_target_subj) {
        cached_bed_target_ = lv_subject_get_int(bed_target_subj);
        bed_target_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
            bed_target_subj, this,
            [token](NozzleTempsWidget* self, int target) {
                if (token.expired())
                    return;
                const bool had_target = self->cached_bed_target_ > 0;
                self->cached_bed_target_ = target;
                self->update_row_display(self->bed_temp_label_, self->bed_target_label_,
                                         self->cached_bed_temp_, target, true);
                if (had_target != (target > 0))
                    self->relayout_for_granted_size();
            },
            bed_target_lifetime_);
    }

    update_row_display(bed_temp_label_, bed_target_label_, cached_bed_temp_, cached_bed_target_,
                       true);

    rebuilding_ = false;
    spdlog::debug("[NozzleTempsWidget] Rebuilt with {} extruder rows + bed", extruder_rows_.size());

    // These rows are new objects. on_size_changed() decides one column or two, and
    // long labels or short, by measuring against the granted width; rows created
    // after that decision would otherwise keep the XML default and overlap. On a
    // toolchanger the tools arrive over the network, so this rebuild routinely
    // runs after the size pass.
    relayout_for_granted_size();
}

namespace {

// Pixel width of a UTF-8 string in the given font. lv_text_get_width
// dereferences its attributes argument, so a zeroed attributes block (no
// recolor, zero letter/line space, unbounded width) is required — NULL crashes.
int measure_text_px(const char* txt, const lv_font_t* font) {
    if (!txt || !font)
        return 0;
    lv_text_attributes_t attrs;
    lv_text_attributes_init(&attrs);
    attrs.letter_space = 0;
    attrs.max_width = LV_COORD_MAX;
    return lv_text_get_width(txt, LV_TEXT_LEN_MAX, font, &attrs);
}

} // namespace

void NozzleTempsWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    if (!widget_obj_)
        return;

    auto* container = lv_obj_find_by_name(widget_obj_, "nozzle_temps_container");
    if (!container)
        return;

    const int pad_x = theme_manager_get_spacing("space_xs"); // root style_pad_all per side
    const int avail_px = width_px - 2 * pad_x;

    // Pre-layout / degenerate width: publish the same single-column long-label
    // default decide_nozzle_layout() returns, rather than dividing by an
    // unknown width.
    if (width_px <= 0 || avail_px <= 0) {
        lv_subject_set_int(&s_label_mode_subject, static_cast<int>(NozzleLabelMode::Long));
        lv_subject_set_int(&s_columns_subject, 1);
        lv_subject_set_int(&s_compact_font_subject, 0);
        spdlog::debug("[NozzleTempsWidget] on_size_changed {}x{} avail={} (pre-layout fallback)",
                      colspan, rowspan, avail_px);
        return;
    }

    // text_small renders in font_small (ui_text.cpp#ui_text_small_create), so that
    // is the font a row's text has to be measured in. font_xs is the compact rung.
    const lv_font_t* normal_font = theme_manager_get_font("font_small");
    const lv_font_t* compact_font = theme_manager_get_font("font_xs");

    // Every row leads with a size="xs" icon, which draws from the MDI font rather
    // than the text font and so keeps its width when the text shrinks. The bed's
    // glyph shares the row geometry, so the wider of the two sets the leading box.
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_xs");
    const int icon_px =
        std::max(measure_text_px(helix::ui::icon::lookup_codepoint("heater"), icon_font),
                 measure_text_px(helix::ui::icon::lookup_codepoint("radiator"), icon_font));

    const int icon_label_gap = theme_manager_get_spacing("space_xs"); // tool_left_group pad_column
    const int label_floor = theme_manager_get_spacing("space_xl");    // tool_label style_min_width
    const int label_value_gap = theme_manager_get_spacing("space_sm");
    // Breathing room so a measured fit is never an exact fit: text
    // measurement and rendered width disagree by a pixel or three, and a row
    // budgeted to the last pixel renders as an overlap.
    const int comfort_margin = theme_manager_get_spacing("space_md");
    const int bare_margin = theme_manager_get_spacing("space_xxs");
    const int gap_px = theme_manager_get_spacing("space_md"); // gap between two side-by-side rows

    // What a whole row costs in one font. The label box never shrinks below its
    // style_min_width, so a spelling narrower than the floor buys nothing; and a
    // hidden label is dropped from the flex row along with its gap, which is why
    // the icon rung is the icon plus the value and nothing between them. The
    // number rung has no floor to respect — its label carries no min_width.
    auto row_widths = [&](const lv_font_t* font) {
        int widest_long_px = 0;
        int widest_short_px = 0;
        int widest_number_px = 0;
        for (const auto& row : extruder_rows_) {
            widest_long_px = std::max(widest_long_px, measure_text_px(row.long_name.c_str(), font));
            widest_short_px =
                std::max(widest_short_px, measure_text_px(row.short_name.c_str(), font));
            widest_number_px =
                std::max(widest_number_px, measure_text_px(row.number_name.c_str(), font));
        }
        // "1" is the narrowest number any row can show, so the rung's width is
        // honest even before the first tool is discovered.
        widest_number_px = std::max(widest_number_px, measure_text_px("1", font));
        const int bed_label_px = measure_text_px(lv_tr("Bed"), font);
        widest_long_px = std::max({widest_long_px, bed_label_px, label_floor});
        widest_short_px = std::max({widest_short_px, bed_label_px, label_floor});

        // The value measured as the strings the rows actually show, via the
        // same composer update_row_display renders from — measurement and
        // rendering cannot drift apart. The floor is the off state.
        const int value_group_pad = theme_manager_get_spacing("space_xxs");
        auto halves_px = [&](int temp_deci, int target_deci) {
            const auto [current, target] = value_halves(temp_deci, target_deci);
            const int current_px = measure_text_px(current.c_str(), font);
            return std::pair{current_px, measure_text_px(target.c_str(), font)};
        };
        auto [floor_current, floor_target] = halves_px(0, 0);
        int value_px = floor_current + value_group_pad + floor_target;
        int current_only_px = floor_current;
        for (const auto& row : extruder_rows_) {
            const auto [current_px, target_px] = halves_px(row.cached_temp, row.cached_target);
            value_px = std::max(value_px, current_px + value_group_pad + target_px);
            current_only_px = std::max(current_only_px, current_px);
        }
        {
            const auto [current_px, target_px] = halves_px(cached_bed_temp_, cached_bed_target_);
            value_px = std::max(value_px, current_px + value_group_pad + target_px);
            current_only_px = std::max(current_only_px, current_px);
        }

        NozzleRowWidths w;
        // The comfort margin belongs to the spelling rungs: text measurement
        // and rendered width disagree by a few pixels, and a spelling
        // budgeted to the last pixel renders as an overlap. The number and
        // icon rungs are the degradation rungs: the row HIDES the target half
        // there (one tap away in the graph overlay), so their width budget is
        // the current half alone plus the minimal slack — which is what keeps
        // a 2-unit column in the normal font while a print is running.
        const int text_tail = label_value_gap + value_px + comfort_margin;
        const int bare_tail = label_value_gap + current_only_px + bare_margin;
        w.long_px = icon_px + icon_label_gap + widest_long_px + text_tail;
        w.short_px = icon_px + icon_label_gap + widest_short_px + text_tail;
        w.number_px = icon_px + icon_label_gap + widest_number_px + bare_tail;
        w.icon_px = icon_px + bare_tail;
        return w;
    };

    const NozzleRowWidths normal = row_widths(normal_font);
    const NozzleRowWidths compact = row_widths(compact_font);

    // Per-line heights at each font: the base is the title plus the tile's
    // vertical padding plus one render-rounding pixel (the rendered tree
    // lands a pixel over the arithmetic, and a tier chosen on a stack that
    // fits to the exact pixel scrolls by it); a row line is its font's line
    // height plus the inter-row pad. The ladder folds in the column count —
    // two columns wrap the rows, so a wide short tile keeps the normal font.
    const int avail_h = height_px - 2 * pad_x;
    auto row_line_px = [&](const lv_font_t* font) {
        const int line =
            std::max(lv_font_get_line_height(font), lv_font_get_line_height(icon_font));
        return line + theme_manager_get_spacing("space_xxs"); // container pad_row
    };
    const NozzleStackHeights stack{lv_font_get_line_height(normal_font) +
                                       theme_manager_get_spacing("space_xxs") + 1,
                                   row_line_px(normal_font), row_line_px(compact_font)};

    const NozzleLayoutDecision decision = decide_nozzle_layout(
        avail_px, avail_h, gap_px, normal, compact, static_cast<int>(extruder_rows_.size()), stack);

    // The whole verdict goes out as subjects; nozzle_temp_row.xml,
    // nozzle_temp_bed_row.xml and panel_widget_nozzle_temps.xml bind every
    // appearance (which labels are hidden, the row widths, the container flow,
    // the compact font) off them. Rows created later read the current values
    // at creation, so a late rebuild can never disagree with its siblings.
    lv_subject_set_int(&s_label_mode_subject, static_cast<int>(decision.label_mode));
    lv_subject_set_int(&s_columns_subject, decision.columns);
    lv_subject_set_int(&s_compact_font_subject, decision.use_compact_font ? 1 : 0);

    spdlog::debug("[NozzleTempsWidget] on_size_changed {}x{} avail={}x{} cols={} mode={} "
                  "compact={} widths n[l{}] s[{}#{}i{}] c[l{} s{} #{} i{}] stack b{} rn{} rc{}",
                  colspan, rowspan, avail_px, avail_h, decision.columns,
                  static_cast<int>(decision.label_mode), decision.use_compact_font, normal.long_px,
                  normal.short_px, normal.number_px, normal.icon_px, compact.long_px,
                  compact.short_px, compact.number_px, compact.icon_px, stack.base_px,
                  stack.row_normal_px, stack.row_compact_px);
}

void NozzleTempsWidget::create_extruder_row(lv_obj_t* container, ExtruderRow& row) {
    auto& tool_state = ToolState::instance();

    // Short label: the tool's physical position (e.g. "Tool 1"); falls back to
    // the klipper extruder name when no tool is mapped (multi-extruder, no
    // toolchanger).
    std::string short_name = tool_state.display_label_for_extruder(row.name);
    if (short_name.empty())
        short_name = row.name;

    // Long label: prefer the user-friendly "Nozzle N" from PrinterTemperatureState
    // when the tool's gcode identity is just the default Tn pattern. For
    // toolchangers with viesturz-named tools (e.g. "Left", "Right"), the
    // configured tool name is already meaningful — keep it.
    std::string gcode_name = tool_state.tool_name_for_extruder(row.name);
    std::string long_name = gcode_name.empty() ? row.name : gcode_name;
    if (helix::ui::is_generated_tool_name(gcode_name)) {
        const auto& exts = printer_state_.temperature_state().extruders();
        auto it = exts.find(row.name);
        if (it != exts.end() && !it->second.display_name.empty())
            long_name = it->second.display_name;
    }

    row.short_name = std::move(short_name);
    row.long_name = std::move(long_name);
    // The number rung: the 1-based display number, the one label that fits
    // where no spelling does and still tells four icon rows apart.
    row.number_name = helix::ui::lane_number_text(static_cast<int>(extruder_rows_.size()));

    // Create row from XML template. All three spellings are written once as
    // data; which one shows, the row width and the font are bound off the
    // layout subjects, so the row reads the CURRENT verdict the moment it is
    // created — a late rebuild cannot disagree with its siblings.
    const char* attrs[] = {
        "tool_name",   row.long_name.c_str(),   "tool_short", row.short_name.c_str(),
        "tool_number", row.number_name.c_str(), nullptr};
    lv_obj_t* row_obj = static_cast<lv_obj_t*>(lv_xml_create(container, "nozzle_temp_row", attrs));
    if (!row_obj) {
        spdlog::error("[NozzleTempsWidget] lv_xml_create('nozzle_temp_row') returned NULL for '{}'",
                      row.name);
        return;
    }

    row.row_obj = row_obj;
    row.label_long = lv_obj_find_by_name(row_obj, "tool_label_long");
    row.label_short = lv_obj_find_by_name(row_obj, "tool_label_short");
    row.label_number = lv_obj_find_by_name(row_obj, "tool_label_number");
    row.temp_label = lv_obj_find_by_name(row_obj, "temp_label");
    row.target_label = lv_obj_find_by_name(row_obj, "target_label");

    // Belt-and-suspenders: clip (never wrap) the value labels so even a
    // pathologically narrow tile keeps "220° / 230°" on one line. The XML
    // value_group (content width, no flex_grow) already prevents the wrap;
    // this guards against a degenerate measurement.
    if (row.temp_label)
        lv_label_set_long_mode(row.temp_label, LV_LABEL_LONG_MODE_CLIP);
    if (row.target_label)
        lv_label_set_long_mode(row.target_label, LV_LABEL_LONG_MODE_CLIP);

    // Tap row → open nozzle temp graph overlay
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* screen = parent_screen_;
    lv_obj_add_event_cb(
        row_obj,
        [](lv_event_t* e) {
            auto* scr = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            if (scr) {
                get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, scr);
            }
        },
        LV_EVENT_CLICKED, screen);
}

void NozzleTempsWidget::create_bed_row(lv_obj_t* container) {
    // Create bed row from XML template — divider, layout, colors are all declarative
    lv_obj_t* row_obj =
        static_cast<lv_obj_t*>(lv_xml_create(container, "nozzle_temp_bed_row", nullptr));
    if (!row_obj) {
        spdlog::error("[NozzleTempsWidget] lv_xml_create('nozzle_temp_bed_row') returned NULL");
        return;
    }

    bed_row_ = row_obj;
    bed_icon_ = lv_obj_find_by_name(row_obj, "bed_icon");
    bed_temp_label_ = lv_obj_find_by_name(row_obj, "bed_temp_label");
    bed_target_label_ = lv_obj_find_by_name(row_obj, "bed_target_label");

    // Clip rather than wrap the bed value labels (see create_extruder_row).
    if (bed_temp_label_)
        lv_label_set_long_mode(bed_temp_label_, LV_LABEL_LONG_MODE_CLIP);
    if (bed_target_label_)
        lv_label_set_long_mode(bed_target_label_, LV_LABEL_LONG_MODE_CLIP);

    // Tap bed row → open bed temp graph overlay
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* screen = parent_screen_;
    lv_obj_add_event_cb(
        row_obj,
        [](lv_event_t* e) {
            auto* scr = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            if (scr) {
                get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, scr);
            }
        },
        LV_EVENT_CLICKED, screen);
}

void NozzleTempsWidget::update_row_display(lv_obj_t* temp_label, lv_obj_t* target_label,
                                           int temp_deci, int target_deci, bool is_bed) {
    if (!temp_label || !target_label)
        return;

    auto result = helix::ui::temperature::heater_display(temp_deci, target_deci);

    // Current temp with color coding (green=at-temp, red=heating, blue=cooling, gray=off)
    const auto [current_text, target_text] = value_halves(temp_deci, target_deci);
    lv_label_set_text(temp_label, current_text.c_str());
    lv_obj_set_style_text_color(temp_label, result.color, LV_PART_MAIN);

    // Keep the bed icon tint in lockstep with the temp-label color (same
    // thresholds, same inputs) so they can never disagree.
    if (is_bed && bed_icon_) {
        const char* variant = helix::ui::temperature::get_heating_state_variant(
            helix::ui::temperature::deci_to_degrees(temp_deci),
            helix::ui::temperature::deci_to_degrees(target_deci));
        helix::ui::icon::set_variant(bed_icon_, variant);
    }

    lv_label_set_text(target_label, target_text.c_str());
}
