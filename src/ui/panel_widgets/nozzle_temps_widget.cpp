// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_temps_widget.h"

#include "ui_callback_helpers.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_overlay_temp_graph.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/subject.h"
#include "core/temp_unit_subject.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "helix/xml/indexed_subject_pool.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "nozzle_layout.h"
#include "observe_language.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "subject_managed_panel.h"
#include "system/subject_registry.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace {

// The published layout verdict, read by bind_flag_if / bind_style_if in
// nozzle_temp_row.xml, nozzle_temp_bed_row.xml and panel_widget_nozzle_temps.xml.
// Module-level (not members): the verdict belongs to the widget KIND, rows
// read the CURRENT values the moment they are created, and registration rides
// SubjectInitializer's panel-subject phase so the subjects exist before any
// XML that binds them is parsed. Values mirror decide_nozzle_layout()'s
// output: label_mode is NozzleLabelMode's int (0=none 1=number 2=short
// 3=long), columns is 1 or 2, compact is 0 or 1.
HELIX_PSRAM_BSS helix::Subject s_label_mode_subject{};
HELIX_PSRAM_BSS helix::Subject s_columns_subject{};
HELIX_PSRAM_BSS helix::Subject s_compact_font_subject{};
helix::WidgetSubjects s_subjects{"NozzleTempsWidgetSubjects"};

// The value each row shows, published for the row XML to bind: its current
// and target halves as text, and the heat state that colors the current one
// (HeatState's own value: 0 off, 1 heating, 2 at temperature, 3 cooling).
// The bed row reads nozzle_bed_*; extruder row i reads the i-th slot of each
// pool, nozzle_row_<i>_temp_text and so on. Registered here, with the layout
// subjects, so they exist before any row that binds them is parsed.
constexpr size_t kRowTextBytes = 32;
HELIX_PSRAM_BSS helix::Subject s_bed_temp_text_subject{};
HELIX_PSRAM_BSS char s_bed_temp_text_buf[kRowTextBytes];
HELIX_PSRAM_BSS helix::Subject s_bed_target_text_subject{};
HELIX_PSRAM_BSS char s_bed_target_text_buf[kRowTextBytes];
HELIX_PSRAM_BSS helix::Subject s_bed_heat_subject{};
helix::xml::IndexedSubjectPool s_row_temp_text{
    "nozzle_row", helix::xml::IndexedSubjectPool::Type::String, kRowTextBytes, "_temp_text"};
helix::xml::IndexedSubjectPool s_row_target_text{
    "nozzle_row", helix::xml::IndexedSubjectPool::Type::String, kRowTextBytes, "_target_text"};
helix::xml::IndexedSubjectPool s_row_heat{"nozzle_row", helix::xml::IndexedSubjectPool::Type::Int,
                                          0, "_heat"};

void nozzle_temps_widget_init_subjects() {
    s_subjects.ensure_init(
        [] {
            s_label_mode_subject.init_int(
                static_cast<int>(helix::NozzleLabelMode::Long)); // the degenerate default
            s_subjects.publish("nozzle_row_label_mode", &s_label_mode_subject);
            s_columns_subject.init_int(1);
            s_subjects.publish("nozzle_row_columns", &s_columns_subject);
            s_compact_font_subject.init_int(0);
            s_subjects.publish("nozzle_row_compact", &s_compact_font_subject);
            s_bed_temp_text_subject.init_string(s_bed_temp_text_buf, "");
            s_subjects.publish("nozzle_bed_temp_text", &s_bed_temp_text_subject);
            s_bed_target_text_subject.init_string(s_bed_target_text_buf, "");
            s_subjects.publish("nozzle_bed_target_text", &s_bed_target_text_subject);
            s_bed_heat_subject.init_int(static_cast<int>(helix::ui::temperature::HeatState::Off));
            s_subjects.publish("nozzle_bed_heat", &s_bed_heat_subject);
        },
        [] {
            s_row_temp_text.reclaim();
            s_row_target_text.reclaim();
            s_row_heat.reclaim();
        });
}

/// The two strings a row's value shows: the current temperature and, when a
/// target is set, the target half; "off" otherwise. Composed once, shared by
/// the code that renders it (publish_row) and the code that measures
/// it (on_size_changed) — if the two ever drift, the ladder decides on widths
/// the row does not draw.
std::pair<std::string, std::string> value_halves(int temp_deci, int target_deci) {
    char num_buf[16];
    helix::temperature_rules::format_temp_number(
        helix::temperature_rules::deci_to_degrees_f(temp_deci), num_buf, sizeof(num_buf));
    std::string current = std::string(num_buf) + "\xC2\xB0";
    if (target_deci <= 0)
        return {current, lv_tr("off")};
    helix::temperature_rules::format_temp_number(
        helix::temperature_rules::deci_to_degrees_f(target_deci), num_buf, sizeof(num_buf));
    return {current, "/ " + std::string(num_buf) + "\xC2\xB0"};
}

/// A reading's heat state, as the row XML numbers the tone it binds: HeatState's
/// own value (0 off, 1 heating, 2 at temperature, 3 cooling), from the one
/// classifier every heater color comes from.
int heat_state_of(int temp_deci, int target_deci) {
    return static_cast<int>(helix::ui::temperature::heater_display(temp_deci, target_deci).state);
}

/// A live subject's int reading; 0 for a subject that is absent or not up.
int reading_or_zero(lv_subject_t* subject) {
    return helix::subject_is_live(subject) ? lv_subject_get_int(subject) : 0;
}

} // namespace

namespace helix {

void register_nozzle_temps_widget() {
    register_widget_factory("nozzle_temps", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<NozzleTempsWidget>(ps);
    });
    register_widget_subjects("nozzle_temps", nozzle_temps_widget_init_subjects);
    lv_xml_register_event_cb(nullptr, "nozzle_temps_row_clicked_cb",
                             NozzleTempsWidget::extruder_row_clicked_cb);
    lv_xml_register_event_cb(nullptr, "nozzle_temps_bed_clicked_cb",
                             NozzleTempsWidget::bed_row_clicked_cb);
}

} // namespace helix

std::vector<std::string> helix::distinct_extruder_names(const std::vector<ToolInfo>& tools,
                                                        bool tools_are_lanes) {
    std::vector<std::string> result;
    for (const auto& head : physical_heads(tools, false, tools_are_lanes)) {
        if (head.extruder_name)
            result.push_back(*head.extruder_name);
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
    // freed subjects, crashing in lv_observer_delete during the next clear_rows().
    //
    // Safety is provided by: (1) weak_alive in observe<int> context (expires when
    // version_observer_ is reset in detach()), (2) rebuilding_ re-entrancy guard,
    // (3) initial_version skip for the attach-time callback.
    int initial_version =
        lv_subject_get_int(printer_state_.temperature_state().get_extruder_version_subject());
    version_observer_ = helix::ui::observe<int>(
        printer_state_.temperature_state().get_extruder_version_subject(), this,
        [initial_version](NozzleTempsWidget* self, int version) {
            if (version == initial_version)
                return; // Skip initial callback — rows already built in attach()
            self->rebuild_rows();
        },
        printer_state_.get_subjects_lifetime());

    auto& tool_state = ToolState::instance();
    // A lane rig orders and names its rows by head, which a tool list rebuild moves.
    if (tool_state.subjects_initialized()) {
        const int initial_tools = lv_subject_get_int(tool_state.get_tools_version_subject());
        tools_version_observer_ = helix::ui::observe<int>(
            tool_state.get_tools_version_subject(), this,
            [initial_tools](NozzleTempsWidget* self, int version) {
                if (version != initial_tools)
                    self->rebuild_rows();
            },
            tool_state.get_subjects_lifetime());
    }
    named_mounted_tool_ = lv_subject_get_int(tool_state.get_active_tool_subject());
    mounted_tool_observer_ = helix::ui::observe<int>(
        tool_state.get_active_tool_subject(), this,
        [](NozzleTempsWidget* self, int tool) {
            if (tool == self->named_mounted_tool_ || !ToolState::instance().heads_share_heater())
                return;
            self->named_mounted_tool_ = tool;
            self->rebuild_rows();
        },
        tool_state.get_subjects_lifetime());

    // "off" and the unit are formatted into the value labels, and the value width
    // decides the layout, so a new language or unit re-formats and re-measures.
    language_observer_ = helix::ui::observe_language_change(
        this, [](NozzleTempsWidget* self) { self->reformat_values(); });
    temp_unit_observer_ = helix::ui::observe_temp_unit_change(
        this, [](NozzleTempsWidget* self) { self->reformat_values(); });
    // Each row's number rung and name spell the tool in the printer's tool number style.
    auto& settings = helix::SettingsManager::instance();
    tool_number_style_observer_ = helix::ui::observe_int_change(
        settings.subject_tool_number_style(), settings.get_subjects_lifetime(), this,
        [](NozzleTempsWidget* self) {
            if (self->widget_obj_)
                self->rebuild_rows();
        });

    spdlog::debug("[NozzleTempsWidget] Attached with {} extruder rows", extruder_rows_.size());
}

void NozzleTempsWidget::reformat_values() {
    if (!widget_obj_ || !s_subjects.initialized())
        return;
    for (size_t i = 0; i < extruder_rows_.size(); ++i)
        publish_row(extruder_rows_[i], i);
    publish_bed(cached_bed_temp_, cached_bed_target_);
    relayout_for_granted_size();
}

void NozzleTempsWidget::publish_row(const ExtruderRow& row, size_t index) {
    // The pools are reclaimed when the widget's subjects are torn down, which
    // can precede the rebuild that drops the rows still listed.
    if (index >= s_row_temp_text.size())
        return;
    const auto [current, target] = value_halves(row.cached_temp, row.cached_target);
    // A pool subject keeps no previous value, so an unchanged text is filtered
    // here; its bound label would otherwise be rewritten on every reading.
    helix::ui::copy_string_if_changed(s_row_temp_text.at(index), current.c_str());
    helix::ui::copy_string_if_changed(s_row_target_text.at(index), target.c_str());
    s_row_heat.set_int(index, heat_state_of(row.cached_temp, row.cached_target));
}

void NozzleTempsWidget::publish_bed(int temp_deci, int target_deci) {
    if (!s_subjects.initialized())
        return;
    const auto [current, target] = value_halves(temp_deci, target_deci);
    helix::ui::copy_string_if_changed(&s_bed_temp_text_subject, current.c_str());
    helix::ui::copy_string_if_changed(&s_bed_target_text_subject, target.c_str());
    helix::ui::set_int_if_changed(&s_bed_heat_subject, heat_state_of(temp_deci, target_deci));
}

void NozzleTempsWidget::observe_reading(lv_subject_t* subject, lv_observer_cb_t cb, lv_obj_t* row) {
    // LVGL refuses an observer on a subject that is not live.
    if (helix::subject_is_live(subject))
        lv_subject_add_observer_obj(subject, cb, row, this);
}

void NozzleTempsWidget::observe_row_sources(ExtruderRow& row) {
    if (!row.row_obj)
        return;
    auto& temps = printer_state_.temperature_state();
    // Each observer's target is the row object. That makes it a widget
    // observer, which lv_subject_hold_obj_observers() skips while the display
    // sleeps and releases onto the latest reading, and LVGL deletes it with
    // the row or with a subject that is reset. It notifies at once, from the
    // subject's current value.
    observe_reading(temps.get_extruder_temp_subject(row.name), current_reading_cb, row.row_obj);
    observe_reading(temps.get_extruder_target_subject(row.name), target_reading_cb, row.row_obj);
}

void NozzleTempsWidget::observe_bed_sources() {
    if (!bed_row_)
        return;
    auto& temps = printer_state_.temperature_state();
    observe_reading(temps.get_bed_temp_subject(), current_reading_cb, bed_row_);
    observe_reading(temps.get_bed_target_subject(), target_reading_cb, bed_row_);
}

void NozzleTempsWidget::current_reading_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<NozzleTempsWidget*>(lv_observer_get_user_data(observer));
    self->on_reading(static_cast<lv_obj_t*>(lv_observer_get_target(observer)), false,
                     lv_subject_get_int(subject));
}

void NozzleTempsWidget::target_reading_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<NozzleTempsWidget*>(lv_observer_get_user_data(observer));
    self->on_reading(static_cast<lv_obj_t*>(lv_observer_get_target(observer)), true,
                     lv_subject_get_int(subject));
}

void NozzleTempsWidget::on_reading(lv_obj_t* owner, bool is_target, int value) {
    // The bed row and the extruder rows are told apart by the object the
    // observer was registered on.
    ExtruderRow* row = nullptr;
    size_t index = 0;
    if (owner != bed_row_) {
        for (size_t i = 0; i < extruder_rows_.size(); ++i) {
            if (extruder_rows_[i].row_obj == owner) {
                row = &extruder_rows_[i];
                index = i;
                break;
            }
        }
        if (!row)
            return; // a row this widget no longer lists
    }
    int& temp = row ? row->cached_temp : cached_bed_temp_;
    int& target = row ? row->cached_target : cached_bed_target_;

    const bool had_target = target > 0;
    (is_target ? target : temp) = value;
    if (row)
        publish_row(*row, index);
    else
        publish_bed(cached_bed_temp_, cached_bed_target_);

    // The ladder budgets the value's two shapes, target set and target off,
    // and a tile sized under one shape must re-decide under the other: setting
    // a target widens every row past the rung chosen while idle. The
    // transition is the only moment the value's WIDTH CLASS changes, so it is
    // the only re-decide. It runs at once, in the step the reading lands in:
    // queued for a later pass, it lets a frame draw the wide value in the
    // narrow rung. A release from the display-sleep hold is the exception. It
    // delivers the held readings one observer at a time, and the ladder must
    // measure both halves of the value once all of them ran, so the re-decide
    // waits while any held observer is still to come; the last one finds none
    // and re-decides at once.
    if (is_target && had_target != (value > 0)) {
        if (lv_subject_obj_observers_held_count() > 0) {
            lifetime_.defer("NozzleTempsWidget::relayout", [this] { relayout_for_granted_size(); });
        } else {
            relayout_for_granted_size();
        }
    }
}

void NozzleTempsWidget::detach() {
    lifetime_.invalidate();
    version_observer_.reset();
    language_observer_.reset();
    temp_unit_observer_.reset();
    tool_number_style_observer_.reset();
    mounted_tool_observer_.reset();
    tools_version_observer_.reset();
    clear_rows();
    uninstall_delete_hook();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void NozzleTempsWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event, before LVGL deletes the children:
    // expire the pending deferred callbacks and drop the cached pointers only.
    // The reading observers are registered on the row objects, so they go with
    // the rows a moment later; every other observer checks its token first, so
    // a drained callback no-ops instead of touching the dead tree.
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

    // Drop each row's reading observers while its object is still alive: the
    // rows are cleaned asynchronously, and an observer that outlived a
    // detached widget would call back into it. A row whose tree was already
    // deleted raw carries none: they went with it.
    if (lv_is_initialized()) {
        for (auto& row : extruder_rows_) {
            if (row.row_obj)
                lv_obj_remove_from_subject(row.row_obj, nullptr);
        }
        if (bed_row_)
            lv_obj_remove_from_subject(bed_row_, nullptr);
    }
    extruder_rows_.clear();

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

    // The rows bind subjects that exist only while the widget's own are up. A
    // rebuild that lands between their teardown and the next init, as after a
    // printer switch, would refill the row pools with no teardown left to
    // reclaim them.
    if (!s_subjects.initialized()) {
        rebuilding_ = false;
        return;
    }

    auto& temps = printer_state_.temperature_state();

    // One row per PHYSICAL extruder. Multiplexing backends (AFC BoxTurtle, Happy
    // Hare, ERCF) expose one logical tool per spool lane (T0..Tn) that all feed a
    // single extruder; without the collapse a 4-lane unit shows its one nozzle
    // temp four times. A true toolchanger maps each tool to a distinct extruder,
    // so every nozzle still gets its own row.
    const std::vector<std::string> extruder_names = distinct_extruder_names(
        ToolState::instance().tools(), ToolState::instance().tools_are_lanes());

    // A binding to a name that does not resolve while its row is parsed is
    // dropped, so every subject a row binds exists, and already holds the
    // row's value, before the row does.
    s_row_temp_text.ensure_size(extruder_names.size());
    s_row_target_text.ensure_size(extruder_names.size());
    s_row_heat.ensure_size(extruder_names.size());
    extruder_rows_.reserve(extruder_names.size());

    for (const auto& extruder_name : extruder_names) {
        // The number rung counts the rows built so far, so the row is named
        // before it joins them.
        ExtruderRow row;
        row.name = extruder_name;
        name_row(row);
        row.cached_temp = reading_or_zero(temps.get_extruder_temp_subject(extruder_name));
        row.cached_target = reading_or_zero(temps.get_extruder_target_subject(extruder_name));

        extruder_rows_.push_back(std::move(row));
        ExtruderRow& built = extruder_rows_.back();
        const size_t index = extruder_rows_.size() - 1;
        publish_row(built, index);
        create_extruder_row(container, built, index);
        observe_row_sources(built);
    }

    // Bed row at the end
    cached_bed_temp_ = reading_or_zero(temps.get_bed_temp_subject());
    cached_bed_target_ = reading_or_zero(temps.get_bed_target_subject());
    publish_bed(cached_bed_temp_, cached_bed_target_);
    create_bed_row(container);
    observe_bed_sources();

    rebuilding_ = false;
    spdlog::debug("[NozzleTempsWidget] Rebuilt with {} extruder rows + bed", extruder_rows_.size());

    // These rows are new objects. on_size_changed() decides one column or two, and
    // long labels or short, by measuring against the granted width; rows created
    // after that decision would otherwise keep the XML default and overlap. On a
    // toolchanger the tools arrive over the network, so this rebuild routinely
    // runs after the size pass.
    relayout_for_granted_size();
}

using helix::ui::measure_text_px;

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
        // same composer publish_row() renders from — measurement and
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

void NozzleTempsWidget::name_row(ExtruderRow& row) const {
    auto& tool_state = ToolState::instance();

    // Short label: the tool's physical position (e.g. "Tool 1"); falls back to
    // the klipper extruder name when no tool is mapped (multi-extruder, no
    // toolchanger).
    std::string short_name = tool_state.display_label_for_extruder(row.name);
    if (short_name.empty())
        short_name = tool_state.heads_share_heater() ? tool_state.nozzle_label() : row.name;

    // Long label: prefer the user-friendly "Nozzle N" from PrinterTemperatureState
    // when the tool's gcode identity is just the default Tn pattern. For
    // toolchangers with viesturz-named tools (e.g. "Left", "Right"), the
    // configured tool name is already meaningful — keep it.
    std::string gcode_name = tool_state.tool_name_for_extruder(row.name);
    std::string long_name = gcode_name.empty() ? row.name : gcode_name;
    if (tool_state.heads_share_heater()) {
        // One heater, several nozzles: the row is the mounted nozzle, named
        // exactly as every other nozzle readout names it.
        long_name = tool_state.nozzle_label();
    } else if (helix::ui::is_generated_tool_name(gcode_name) ||
               (gcode_name.empty() && tool_state.tools_are_lanes())) {
        if (std::string name = helix::ui::temperature::nozzle_display_name(
                printer_state_.temperature_state(), row.name);
            !name.empty())
            long_name = std::move(name);
    }

    row.short_name = std::move(short_name);
    row.long_name = std::move(long_name);
    // The number rung: the tool's number in the printer's style ("1" or "T0"),
    // the one label that fits where no spelling does and still tells four icon
    // rows apart.
    // Lane tools name no head, so a lane printer's row is its head's number.
    row.number_name =
        tool_state.tools_are_lanes()
            ? helix::ui::head_number_text(helix::ui::active_tool_number_style(),
                                          tool_state.head_identity_for_extruder(row.name))
            : helix::ui::tool_number_text(helix::ui::active_tool_number_style(), gcode_name,
                                          static_cast<int>(extruder_rows_.size()));
}

void NozzleTempsWidget::create_extruder_row(lv_obj_t* container, ExtruderRow& row, size_t index) {
    // Create row from XML template. All three spellings are written once as
    // data, and publish_row() filled the value's subjects before this parse;
    // which one shows, the row width and the font are bound off the layout
    // subjects, so the row reads the CURRENT verdict the moment it is
    // created — a late rebuild cannot disagree with its siblings.
    const std::string index_text = std::to_string(index);
    const char* attrs[] = {
        "tool_name",   row.long_name.c_str(),   "tool_short", row.short_name.c_str(),
        "tool_number", row.number_name.c_str(), "index",      index_text.c_str(),
        nullptr};
    lv_obj_t* row_obj = static_cast<lv_obj_t*>(lv_xml_create(container, "nozzle_temp_row", attrs));
    if (!row_obj) {
        spdlog::error("[NozzleTempsWidget] lv_xml_create('nozzle_temp_row') returned NULL for '{}'",
                      row.name);
        return;
    }

    row.row_obj = row_obj;
    row.label_long = helix::ui::find_required(row_obj, "tool_label_long", "NozzleTempsWidget");
    row.label_short = helix::ui::find_required(row_obj, "tool_label_short", "NozzleTempsWidget");
    row.label_number = helix::ui::find_required(row_obj, "tool_label_number", "NozzleTempsWidget");
    row.temp_label = helix::ui::find_required(row_obj, "temp_label", "NozzleTempsWidget");
    row.target_label = helix::ui::find_required(row_obj, "target_label", "NozzleTempsWidget");
}

void NozzleTempsWidget::extruder_row_clicked_cb(lv_event_t* e) {
    auto* self = panel_widget_from_event<NozzleTempsWidget>(e);
    const auto index = helix::ui::event_user_int(e);
    if (self && index) {
        self->handle_extruder_row_clicked(*index);
    }
}

void NozzleTempsWidget::bed_row_clicked_cb(lv_event_t* e) {
    auto* self = panel_widget_from_event<NozzleTempsWidget>(e);
    if (self && self->parent_screen_) {
        get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, self->parent_screen_);
    }
}

void NozzleTempsWidget::handle_extruder_row_clicked(int index) {
    if (!parent_screen_) {
        return;
    }
    // On a multi-head printer each row is one tool; the graph opens on it.
    std::string extruder;
    if (ToolState::instance().has_multiple_toolheads() && index >= 0 &&
        static_cast<size_t>(index) < extruder_rows_.size()) {
        extruder = extruder_rows_[static_cast<size_t>(index)].name;
    }
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_, extruder);
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
    bed_icon_ = helix::ui::find_required(row_obj, "bed_icon", "NozzleTempsWidget");
    bed_temp_label_ = helix::ui::find_required(row_obj, "bed_temp_label", "NozzleTempsWidget");
    bed_target_label_ = helix::ui::find_required(row_obj, "bed_target_label", "NozzleTempsWidget");
}
