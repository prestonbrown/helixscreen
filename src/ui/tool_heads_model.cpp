// SPDX-License-Identifier: GPL-3.0-or-later
#include "tool_heads_model.h"

#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "helix/ui/text_metrics.h"
#include "helix_psram_attr.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/tool_switcher_layout.h"
#include "static_subject_registry.h"
#include "target_tool.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>

namespace helix::ui {

namespace {

using xml::IndexedSubjectPool;

// Per head, indexed by head: tool_switcher_seg_<i>_label and so on.
IndexedSubjectPool s_seg_label{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48, "_label"};
IndexedSubjectPool s_seg_sub{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48, "_sub"};
IndexedSubjectPool s_seg_color{"tool_switcher_seg", IndexedSubjectPool::Type::Color, 0, "_color"};
IndexedSubjectPool s_seg_has_color{"tool_switcher_seg", IndexedSubjectPool::Type::Int, 0,
                                   "_has_color"};
// A multi-colour spool's colours (CSV) and the names its finish is read from, for the band.
IndexedSubjectPool s_seg_colors{"tool_switcher_seg", IndexedSubjectPool::Type::String, 96,
                                "_colors"};
IndexedSubjectPool s_seg_finish{"tool_switcher_seg", IndexedSubjectPool::Type::String, 160,
                                "_finish"};
// The head's temperature whatever the label form.
IndexedSubjectPool s_seg_temp{"tool_switcher_seg", IndexedSubjectPool::Type::String, 16, "_temp"};
// The head's label, second line and look in its picker cell.
IndexedSubjectPool s_seg_pick{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48, "_pick"};
IndexedSubjectPool s_seg_pick_sub{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48,
                                  "_pick_sub"};
IndexedSubjectPool s_seg_pick_state{"tool_switcher_seg", IndexedSubjectPool::Type::Int, 0,
                                    "_pick_state"};
// ToolSwitcherCellState, with the mounted head or the target tool's highlighted.
IndexedSubjectPool s_seg_state{"tool_switcher_seg", IndexedSubjectPool::Type::Int, 0, "_state"};
IndexedSubjectPool s_seg_target_state{"tool_switcher_seg", IndexedSubjectPool::Type::Int, 0,
                                      "_target_state"};
// The head's label in a target row, one pool per row surface so each fits
// its own width: the Filament panel's and the temperature graph's.
IndexedSubjectPool s_seg_row{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48, "_row"};
IndexedSubjectPool s_seg_row_graph{"tool_switcher_seg", IndexedSubjectPool::Type::String, 48,
                                   "_row_graph"};

struct RowPool {
    const char* key;
    IndexedSubjectPool* pool;
};
const RowPool s_row_pools[] = {{"_row", &s_seg_row}, {"_row_graph", &s_seg_row_graph}};
// The material named on the band, and the text colour that reads on its fill.
IndexedSubjectPool s_seg_material{"tool_switcher_seg", IndexedSubjectPool::Type::String, 24,
                                  "_material"};
IndexedSubjectPool s_seg_band_text{"tool_switcher_seg", IndexedSubjectPool::Type::Color, 0,
                                   "_band_text"};

IndexedSubjectPool* const s_pools[] = {
    &s_seg_label,    &s_seg_sub,        &s_seg_color,    &s_seg_has_color,
    &s_seg_colors,   &s_seg_finish,     &s_seg_temp,     &s_seg_pick,
    &s_seg_pick_sub, &s_seg_pick_state, &s_seg_state,    &s_seg_target_state,
    &s_seg_row,      &s_seg_row_graph,  &s_seg_material, &s_seg_band_text};

const HeadFilament kNoFilament{};

/// A compact row cell's height.
constexpr int kCompactRowCellPx = 40;

/// Heads at or above this read hot (decidegrees).
constexpr int kHotDeci = 1500;

void set_color_if_changed(lv_subject_t* subject, uint32_t rgb) {
    if ((lv_color_to_u32(lv_subject_get_color(subject)) & 0xFFFFFF) != rgb) {
        lv_subject_set_color(subject, lv_color_hex(rgb));
    }
}

int resolve_px_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// A head's filament from its own slot. An empty slot keeps reporting the
/// colour of whatever was in it, and the default grey is no colour at all, so
/// neither counts as known: the band is omitted rather than drawn grey.
HeadFilament read_filament(const ToolInfo& tool) {
    HeadFilament out;
    if (tool.backend_index < 0 || tool.backend_slot < 0)
        return out;
    const AmsBackend* backend = AmsState::instance().get_backend(tool.backend_index);
    if (!backend)
        return out;
    const SlotInfo slot = backend->get_slot_info(tool.backend_slot);
    if (slot.status == SlotStatus::EMPTY)
        return out;
    out.has_color = slot.color_rgb != kAmsDefaultSlotColor;
    out.color_rgb = slot.color_rgb;
    out.colors = slot.multi_color_hexes;
    out.finish = filament_finish_name(slot.finish());
    out.material = slot.material;
    return out;
}

/// Text that reads on a band filled with @p rgb.
uint32_t band_text_rgb(uint32_t rgb) {
    return lv_color_to_u32(theme_manager_get_readable_on(lv_color_hex(rgb))) & 0xFFFFFF;
}

} // namespace

ToolHeadsModel& ToolHeadsModel::instance() {
    static ToolHeadsModel s_instance;
    return s_instance;
}

void ToolHeadsModel::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(heads_subject_, 0, "tool_switcher_heads", subjects_);
    // A row cell is one touch target tall; its band is the plain strip.
    UI_MANAGED_SUBJECT_INT(row_band_h_subject_,
                           tool_switcher_strip_px(resolve_px_token("touch_target_min", 48)),
                           "tool_row_band_h", subjects_);
    // Always 0: a row cell holds one line and never disables.
    UI_MANAGED_SUBJECT_INT(row_off_subject_, 0, "tool_row_off", subjects_);
    // A row cell's size: flex shares the row's width, so the width stays 0;
    // the height is one touch target.
    UI_MANAGED_SUBJECT_INT(row_cell_w_subject_, 0, "tool_row_cell_w",
                           subjects_); // SUBJECT_OK: bound as ${size}_w by tool_switcher_segment
    UI_MANAGED_SUBJECT_INT(row_cell_h_subject_, resolve_px_token("touch_target_min", 48),
                           "tool_row_cell_h",
                           subjects_); // SUBJECT_OK: bound as ${size}_h by tool_switcher_segment
    // The compact cell a short portrait column uses, a little under a touch
    // target, as the Filament panel's portrait rows are.
    UI_MANAGED_SUBJECT_INT(row_compact_cell_w_subject_, 0, "tool_row_compact_cell_w",
                           subjects_); // SUBJECT_OK: bound as ${size}_w by tool_switcher_segment
    UI_MANAGED_SUBJECT_INT(row_compact_cell_h_subject_, kCompactRowCellPx,
                           "tool_row_compact_cell_h",
                           subjects_); // SUBJECT_OK: bound as ${size}_h by tool_switcher_segment
    // Every slot a cell can bind exists before any XML is parsed.
    for (IndexedSubjectPool* pool : s_pools) {
        pool->ensure_size(kPoolSize);
    }
    subjects_initialized_ = true;
    lv_xml_register_event_cb(nullptr, "tool_target_row_resized", on_row_resized);
    StaticSubjectRegistry::instance().register_deinit(
        "ToolHeadsModel", []() { ToolHeadsModel::instance().deinit_subjects(); });
}

void ToolHeadsModel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    active_tool_observer_.reset();
    tools_observer_.reset();
    slots_observer_.reset();
    extruders_observer_.reset();
    ams_action_observer_.reset();
    target_observer_.reset();
    numbering_observer_.reset();
    head_temps_.clear();
    filament_.clear();
    printer_state_ = nullptr;
    for (IndexedSubjectPool* pool : s_pools) {
        pool->reclaim();
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void ToolHeadsModel::attach(PrinterState& printer_state) {
    init_subjects();
    printer_state_ = &printer_state;
    printer_lifetime_ = printer_state.temperature_state().get_subjects_lifetime();
    auto& tools = ToolState::instance();
    auto republish = [](ToolHeadsModel* self, int) { self->publish(); };
    active_tool_observer_ = observe<int>(tools.get_active_tool_subject(), this, republish,
                                         tools.get_subjects_lifetime());
    tools_observer_ = observe<int>(
        tools.get_tools_version_subject(), this,
        [](ToolHeadsModel* self, int) {
            self->rebuild_head_temps();
            self->publish();
        },
        tools.get_subjects_lifetime());
    // A slot's colour or material recolours its head; a changer fault marks
    // every head but the mounted one.
    slots_observer_ = observe<int>(AmsState::instance().get_slots_version_subject(), this,
                                   republish, AmsState::instance().get_subjects_lifetime());
    ams_action_observer_ = observe<int>(AmsState::instance().get_ams_action_subject(), this,
                                        republish, AmsState::instance().get_subjects_lifetime());
    // Extruders discovered after the tools hand the heads their temperatures.
    extruders_observer_ = observe<int>(
        printer_state.temperature_state().get_extruder_version_subject(), this,
        [](ToolHeadsModel* self, int) {
            self->rebuild_head_temps();
            self->publish();
        },
        printer_state.temperature_state().get_subjects_lifetime());
    auto& target = TargetTool::instance();
    target.init_subjects();
    target_observer_ = observe<int>(
        target.subject(), this, [](ToolHeadsModel* self, int) { self->publish_states(); },
        target.get_subjects_lifetime());
    // The display numbering re-renders on a language change.
    numbering_observer_ =
        observe_language_change(this, [](ToolHeadsModel* self) { self->publish(); });
    rebuild_head_temps();
    publish();
}

int ToolHeadsModel::head_count() const {
    return static_cast<int>(ToolState::instance().physical_heads().size());
}

ToolInfo ToolHeadsModel::head_tool(int i) const {
    // The mounted tool on the mounted head, else the head's lowest. It keeps
    // that tool's G-code name, the one a tap sends, and is numbered as the
    // head, so a hub-fed head reads "Tool 6" over "T8" rather than after its lane.
    const auto& ts = ToolState::instance();
    const auto& heads = ts.physical_heads();
    const int tool = heads[static_cast<size_t>(i)].representative_tool(ts.active_tool_index());
    ToolInfo info = ts.tools()[static_cast<size_t>(tool)];
    info.index = i;
    return info;
}

int ToolHeadsModel::mounted_head() const {
    const auto& ts = ToolState::instance();
    const int active = ts.active_tool_index();
    if (active == -1)
        return -1;
    const int head = head_index_for_tool(ts.physical_heads(), active);
    return head >= 0 ? head : head_count();
}

const HeadFilament& ToolHeadsModel::filament(int i) const {
    return i >= 0 && i < static_cast<int>(filament_.size()) ? filament_[static_cast<size_t>(i)]
                                                            : kNoFilament;
}

std::string ToolHeadsModel::head_temp_text(int i) const {
    if (i < 0 || i >= static_cast<int>(head_temps_.size()) || !head_temps_[i].known ||
        !ToolState::instance().head_reads_heater(i))
        return {};
    char buf[16];
    return temperature::format_temperature(temperature::deci_to_degrees(head_temps_[i].deci), buf,
                                           sizeof(buf));
}

bool ToolHeadsModel::head_hot(int i) const {
    return i >= 0 && i < static_cast<int>(head_temps_.size()) && head_temps_[i].known &&
           ToolState::instance().head_reads_heater(i) && head_temps_[i].deci >= kHotDeci;
}

bool ToolHeadsModel::changer_fault() {
    // Only a machine whose heads dock can have one out of place; a shared
    // toolhead's filament-system error says nothing about the heads.
    const AmsBackend* backend = AmsState::instance().get_backend();
    return backend && backend->fault_concerns_carriage() &&
           lv_subject_get_int(AmsState::instance().get_ams_action_subject()) ==
               static_cast<int>(AmsAction::ERROR);
}

std::pair<std::string, std::string> ToolHeadsModel::picker_lines(int i, ToolLabelForm form) const {
    const ToolInfo tool = head_tool(i);
    const std::string code = active_tool_label(tool.name, tool.index, ToolLabelForm::Code);
    const std::string temp = head_temp_text(i);
    if (form == ToolLabelForm::Code)
        return {code, temp};
    std::string sub = code;
    if (!temp.empty())
        sub = fmt::format(lv_tr("{} · {}"), sub, temp);
    return {active_tool_name_label(tool.name, tool.index), sub};
}

std::pair<std::string, std::string> ToolHeadsModel::target_lines(int i, bool names) const {
    return {row_label(i, names), head_temp_text(i)};
}

std::string ToolHeadsModel::row_label(int i, bool names) const {
    const ToolInfo tool = head_tool(i);
    return names ? active_tool_name_label(tool.name, tool.index)
                 : tool_number_or_name(tool.name, tool.index);
}

void ToolHeadsModel::fit_row_labels(const std::string& key, int cell_w) {
    for (size_t r = 0; r < std::size(s_row_pools); ++r) {
        if (key == s_row_pools[r].key) {
            row_cell_w_[r] = cell_w;
            fit_row_pool(r);
            return;
        }
    }
    spdlog::warn("[ToolHeadsModel] No row label pool '{}'", key);
}

void ToolHeadsModel::fit_row_pool(size_t r) {
    if (!subjects_initialized_)
        return;
    IndexedSubjectPool& pool = *s_row_pools[r].pool;
    const int cell_w = row_cell_w_[r];
    const int heads = std::min(head_count(), static_cast<int>(pool.size()));
    const lv_font_t* body = theme_manager_get_font("font_body");
    const int room = cell_w - 2 * resolve_px_token("space_xs", 4);
    // The name where every head's fits, so the row never mixes "Tool 1" and "2".
    bool names = cell_w > 0;
    for (int i = 0; i < heads && names; ++i) {
        names = text_width(row_label(i, true).c_str(), body) <= room;
    }
    for (int i = 0; i < heads; ++i) {
        copy_string_if_changed(pool.at(i), row_label(i, names).c_str());
    }
}

void ToolHeadsModel::on_row_resized(lv_event_t* e) {
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* key = static_cast<const char*>(lv_event_get_user_data(e));
    lv_obj_t* cell = row ? lv_obj_get_child(row, 0) : nullptr;
    if (!cell || !key) {
        return;
    }
    instance().fit_row_labels(key, lv_obj_get_width(cell));
}

IndexedSubjectPool& ToolHeadsModel::label_pool() {
    return s_seg_label;
}
IndexedSubjectPool& ToolHeadsModel::sub_pool() {
    return s_seg_sub;
}
IndexedSubjectPool& ToolHeadsModel::pick_pool() {
    return s_seg_pick;
}
IndexedSubjectPool& ToolHeadsModel::pick_sub_pool() {
    return s_seg_pick_sub;
}
IndexedSubjectPool& ToolHeadsModel::pick_state_pool() {
    return s_seg_pick_state;
}
IndexedSubjectPool& ToolHeadsModel::state_pool() {
    return s_seg_state;
}
IndexedSubjectPool& ToolHeadsModel::target_state_pool() {
    return s_seg_target_state;
}

int ToolHeadsModel::add_listener(std::function<void()> on_heads, std::function<void(int)> on_temp) {
    const int id = next_listener_++;
    listeners_.push_back({id, std::move(on_heads), std::move(on_temp)});
    return id;
}

void ToolHeadsModel::remove_listener(int id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [id](const Listener& l) { return l.id == id; }),
                     listeners_.end());
}

void ToolHeadsModel::notify_heads() {
    // A listener may remove itself: walk a copy.
    const auto listeners = listeners_;
    for (const auto& l : listeners) {
        if (l.on_heads)
            l.on_heads();
    }
}

void ToolHeadsModel::rebuild_head_temps() {
    head_temps_.clear();
    if (!printer_state_ || !printer_lifetime_ || !*printer_lifetime_)
        return;
    const auto& all_heads = ToolState::instance().physical_heads();
    head_temps_.resize(all_heads.size());
    for (size_t i = 0; i < all_heads.size(); ++i) {
        if (!all_heads[i].extruder_name)
            continue;
        HeadTemp& head = head_temps_[i];
        lv_subject_t* subject = printer_state_->temperature_state().get_extruder_temp_subject(
            *all_heads[i].extruder_name, head.lifetime);
        if (!subject)
            continue;
        head.deci = lv_subject_get_int(subject);
        head.known = true;
        head.observer = observe<int>(
            subject, this,
            [i](ToolHeadsModel* self, int deci) {
                if (i >= self->head_temps_.size())
                    return;
                self->head_temps_[i].deci = deci;
                self->publish_head_temp(static_cast<int>(i));
            },
            head.lifetime);
    }
}

void ToolHeadsModel::publish() {
    if (!subjects_initialized_)
        return;
    const int heads = head_count();
    for (IndexedSubjectPool* pool : s_pools) {
        pool->ensure_size(static_cast<size_t>(heads));
    }
    filament_.assign(static_cast<size_t>(heads), HeadFilament{});
    for (int i = 0; i < heads; ++i) {
        const ToolInfo tool = head_tool(i);
        HeadFilament& f = filament_[static_cast<size_t>(i)];
        f = read_filament(tool);
        copy_string_if_changed(s_seg_temp.at(i), head_temp_text(i).c_str());
        set_int_if_changed(s_seg_has_color.at(i), f.has_color ? 1 : 0);
        if (f.has_color) {
            set_color_if_changed(s_seg_color.at(i), f.color_rgb);
            set_color_if_changed(s_seg_band_text.at(i), band_text_rgb(f.color_rgb));
        }
        copy_string_if_changed(s_seg_colors.at(i), f.colors.c_str());
        copy_string_if_changed(s_seg_finish.at(i), f.finish.c_str());
        copy_string_if_changed(s_seg_material.at(i), f.material.c_str());
    }
    for (size_t r = 0; r < std::size(s_row_pools); ++r) {
        fit_row_pool(r);
    }
    publish_states();
    set_int_if_changed(&heads_subject_, heads);
    notify_heads();
}

void ToolHeadsModel::publish_states() {
    if (!subjects_initialized_)
        return;
    const int heads = std::min(head_count(), static_cast<int>(s_seg_state.size()));
    const int active = mounted_head();
    const int target = TargetTool::instance().head();
    const bool fault = changer_fault();
    for (int i = 0; i < heads; ++i) {
        set_int_if_changed(s_seg_state.at(i),
                           static_cast<int>(tool_switcher_cell_state(i, active, fault)));
        set_int_if_changed(s_seg_target_state.at(i),
                           static_cast<int>(tool_switcher_cell_state(i, target, fault)));
    }
}

void ToolHeadsModel::publish_head_temp(int i) {
    if (!subjects_initialized_ || i < 0 || i >= head_count() ||
        static_cast<size_t>(i) >= s_seg_temp.size())
        return;
    copy_string_if_changed(s_seg_temp.at(i), head_temp_text(i).c_str());
    const auto listeners = listeners_;
    for (const auto& l : listeners) {
        if (l.on_temp)
            l.on_temp(i);
    }
}

} // namespace helix::ui
