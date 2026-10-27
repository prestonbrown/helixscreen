// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_status_tiles.h"

#include "ui_event_safety.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "led/led_controller.h"
#include "light_button_config.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_manager.h"
#include "observer_factory.h"
#include "panel_widgets/callout_chip.h"
#include "print_start_collector.h"
#include "print_start_phase_label.h"
#include "printer_image_manager.h"
#include "printer_state.h"
#include "tool_state.h"
#include "tune_controller.h"

#include <lvgl/src/misc/cache/lv_cache.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix::ui {

namespace {

constexpr uint32_t kUpdateDelayMs = 16;

struct ChipSpec {
    CalloutKind kind;
    const char* key;
    const char* icon;
};

// Indexed by PrintStatusChip.
constexpr ChipSpec kChips[kPrintStatusChipCount] = {
    {CalloutKind::Nozzle, "nozzle", "heater"},
    {CalloutKind::Bed, "bed", "radiator"},
    {CalloutKind::Chamber, "chamber", "fridge_industrial"},
    {CalloutKind::Fan, "part_fan", "fan"},
    {CalloutKind::HotendFan, "hotend_fan", "fan"},
    {CalloutKind::AuxFan, "aux_fan", "fan"},
    {CalloutKind::Light, "light", "lightbulb_on"},
    {CalloutKind::SpeedFlow, "speed_flow", "speed"},
};

constexpr size_t index_of(PrintStatusChip c) {
    return static_cast<size_t>(c);
}

bool is_heater(PrintStatusChip c) {
    return c == PrintStatusChip::Nozzle || c == PrintStatusChip::Bed ||
           c == PrintStatusChip::Chamber;
}

int subject_int_or_zero(lv_subject_t* s) {
    return s ? lv_subject_get_int(s) : 0;
}

void set_int(lv_subject_t* s, int v) {
    if (lv_subject_get_int(s) != v) {
        lv_subject_set_int(s, v);
    }
}

void set_string(lv_subject_t* s, char* buf, size_t size, const std::string& v) {
    if (v == buf) {
        return;
    }
    std::snprintf(buf, size, "%s", v.c_str());
    lv_subject_copy_string(s, buf);
}

bool lined_or_pinned(CalloutMode m) {
    return m == CalloutMode::BothSides || m == CalloutMode::OneSide || m == CalloutMode::Pinned;
}

/// A fan chip names the role its slot fills, whichever fan fills it.
const char* fan_chip_label(PrintStatusChip c) {
    const FanRole role = c == PrintStatusChip::PartFan     ? FanRole::Part
                         : c == PrintStatusChip::HotendFan ? FanRole::Hotend
                                                           : FanRole::Aux;
    return lv_tr(fan_role_label(role, FanLabelLength::Chip));
}

std::string step_time(const PreprintStepRow& row) {
    if (row.seconds < 0) {
        return {};
    }
    return helix::format::duration(row.seconds);
}

} // namespace

const char* print_status_chip_key(PrintStatusChip chip) {
    return kChips[index_of(chip)].key;
}

PrintStatusTiles::PrintStatusTiles(PrinterState& printer_state, Host host)
    : printer_state_(printer_state), host_(std::move(host)),
      exact_image_(
          "PrintStatusTiles",
          [this]() -> lv_obj_t* {
              lv_obj_t* root = host_.root ? host_.root() : nullptr;
              return root ? lv_obj_find_by_name(root, "ps_printer_image") : nullptr;
          },
          // The XML binds the picture's src, so the chosen copy goes through it.
          [this](lv_obj_t*, const std::string& path) {
              set_string(&image_src_, image_src_buf_, sizeof(image_src_buf_), path);
          }) {
    checklist_source_ = []() -> std::optional<PreprintChecklistInput> {
        auto* mgr = get_moonraker_manager();
        auto collector = mgr ? mgr->print_start_collector() : nullptr;
        if (!collector) {
            return std::nullopt;
        }
        return collector->checklist_input();
    };
    use_displayed_image();
}

PrintStatusTiles::~PrintStatusTiles() {
    cancel_update_timer();
    lifetime_.invalidate();
}

void PrintStatusTiles::init_subjects(SubjectManager& subjects) {
    if (subjects_ready_) {
        return;
    }
    for (size_t i = 0; i < kPrintStatusChipCount; ++i) {
        ChipSubjects& c = chips_[i];
        const std::string key = kChips[i].key;
        std::string* names = &chip_names_[i * kChipSubjectCount];
        names[0] = "ps_chip_" + key + "_shown";
        names[1] = "ps_chip_" + key + "_text";
        names[2] = "ps_chip_" + key + "_tone";
        names[3] = "ps_glow_" + key;
        names[4] = "ps_chip_" + key + "_progress";
        names[5] = "ps_line_" + key;
        names[6] = "ps_chip_" + key + "_state";
        lv_subject_init_int(&c.shown, 0);
        lv_subject_init_string(&c.text, c.text_buf, nullptr, sizeof(c.text_buf), "");
        lv_subject_init_int(&c.tone, static_cast<int>(ChipTone::Normal));
        lv_subject_init_int(&c.glow, 0);
        lv_subject_init_int(&c.progress, -1);
        lv_subject_init_int(&c.line, 0);
        lv_subject_init_int(&c.state, 0);
        lv_subject_t* each[] = {&c.shown,    &c.text, &c.tone, &c.glow,
                                &c.progress, &c.line, &c.state};
        for (size_t n = 0; n < kChipSubjectCount; ++n) {
            subjects.publish(names[n].c_str(), each[n]);
        }
    }
    lv_subject_init_string(&image_src_, image_src_buf_, nullptr, sizeof(image_src_buf_), "");
    subjects.publish("ps_printer_image_src", &image_src_);
    lv_subject_init_int(&callout_mode_, static_cast<int>(CalloutMode::ImageOnly));
    subjects.publish("ps_callout_mode", &callout_mode_);
    lv_subject_init_int(&toolhead_merged_, 0);
    subjects.publish("ps_toolhead_merged", &toolhead_merged_);
    lv_subject_init_string(&toolhead_text_, toolhead_text_buf_, nullptr, sizeof(toolhead_text_buf_),
                           "");
    subjects.publish("ps_chip_toolhead_text", &toolhead_text_);

    for (size_t i = 0; i < kPreprintChecklistMaxRows; ++i) {
        StepSubjects& s = steps_[i];
        std::string* names = &step_names_[i * 3];
        names[0] = "ps_step_" + std::to_string(i) + "_name";
        names[1] = "ps_step_" + std::to_string(i) + "_time";
        names[2] = "ps_step_" + std::to_string(i) + "_status";
        lv_subject_init_string(&s.name, s.name_buf, nullptr, sizeof(s.name_buf), "");
        lv_subject_init_string(&s.time, s.time_buf, nullptr, sizeof(s.time_buf), "");
        lv_subject_init_int(&s.status, static_cast<int>(PreprintStepStatus::Upcoming));
        subjects.publish(names[0].c_str(), &s.name);
        subjects.publish(names[1].c_str(), &s.time);
        subjects.publish(names[2].c_str(), &s.status);
    }
    lv_subject_init_int(&step_count_, 0);
    subjects.publish("ps_step_count", &step_count_);
    lv_subject_init_string(&step_position_text_, step_position_buf_, nullptr,
                           sizeof(step_position_buf_), "");
    subjects.publish("ps_step_position_text", &step_position_text_);
    lv_subject_init_string(&step_current_name_, step_current_name_buf_, nullptr,
                           sizeof(step_current_name_buf_), "");
    subjects.publish("ps_step_current_name", &step_current_name_);
    lv_subject_init_string(&step_current_time_, step_current_time_buf_, nullptr,
                           sizeof(step_current_time_buf_), "");
    subjects.publish("ps_step_current_time", &step_current_time_);
    subjects_ready_ = true;

    const auto update = [](PrintStatusTiles* self, int) { self->request_update(); };
    auto& leds = helix::led::LedController::instance();
    led_version_observer_ = observe<int>(leds.get_led_state_version_subject(), this, update,
                                         leds.get_subjects_lifetime());
    led_controllable_observer_ = observe<int>(leds.get_led_controllable_subject(), this, update,
                                              leds.get_subjects_lifetime());
    auto& tools = ToolState::instance();
    tool_badge_observer_ = observe<int>(tools.get_show_tool_badge_subject(), this, update,
                                        tools.get_subjects_lifetime());
    tool_badge_text_observer_ = observe<const char*>(
        tools.get_tool_badge_text_subject(), this,
        [](PrintStatusTiles* self, const char*) { self->request_update(); },
        tools.get_subjects_lifetime());
    const SubjectLifetime ps = printer_state_.get_subjects_lifetime();
    nozzle_power_observer_ =
        observe<int>(printer_state_.get_heater_power_subject(HeaterType::Nozzle), this, update, ps);
    bed_power_observer_ =
        observe<int>(printer_state_.get_heater_power_subject(HeaterType::Bed), this, update, ps);
    chamber_power_observer_ = observe<int>(
        printer_state_.get_heater_power_subject(HeaterType::Chamber), this, update, ps);

    // The same inputs the home printer image follows.
    const auto image_changed = [](PrintStatusTiles* self, int) {
        self->image_stale_ = true;
        self->request_update();
    };
    image_observers_.clear();
    auto& images = PrinterImageManager::instance();
    image_observers_.push_back(observe<int>(images.get_image_changed_subject(), this, image_changed,
                                            images.get_subjects_lifetime()));
    image_observers_.push_back(observe<const char*>(
        printer_state_.profile_state().get_printer_type_subject(), this,
        [](PrintStatusTiles* self, const char*) {
            self->image_stale_ = true;
            self->request_update();
        },
        ps));
    for (const auto& in : PrinterImageManager::detection_inputs()) {
        image_observers_.push_back(observe<int>(in.subject, this, image_changed, in.lifetime));
    }
}

void PrintStatusTiles::deinit() {
    cancel_update_timer();
    // A generation landing after this would write a subject being torn down.
    exact_image_.cancel();
    led_version_observer_.reset();
    led_controllable_observer_.reset();
    tool_badge_observer_.reset();
    tool_badge_text_observer_.reset();
    nozzle_power_observer_.reset();
    bed_power_observer_.reset();
    chamber_power_observer_.reset();
    image_observers_.clear();
    subjects_ready_ = false;
}

void PrintStatusTiles::set_fans(const std::string& part, const std::string& hotend,
                                const std::string& aux) {
    part_fan_ = part;
    hotend_fan_ = hotend;
    aux_fan_ = aux;
    request_update();
}

void PrintStatusTiles::cancel_update_timer() {
    lv_timer_cancel_safe(update_timer_);
    update_timer_ = nullptr;
}

void PrintStatusTiles::request_update() {
    if (!subjects_ready_ || update_timer_) {
        return;
    }
    // Deferred: a burst of readings lays out once, and sizes are read only
    // after the frame has laid the tile out.
    update_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusTiles] update_timer");
            auto* self = static_cast<PrintStatusTiles*>(lv_timer_get_user_data(timer));
            if (self) {
                self->update_timer_ = nullptr;
                self->update_now();
            }
            lv_timer_delete(timer);
            LVGL_SAFE_EVENT_CB_END();
        },
        kUpdateDelayMs, this);
    lv_timer_set_repeat_count(update_timer_, 1);
}

void PrintStatusTiles::on_tree_destroyed() {
    cancel_update_timer();
    exact_image_.cancel();
    exact_image_.forget_displayed();
    last_mode_ = CalloutMode::ImageOnly;
}

int PrintStatusTiles::heat_start(PrintStatusChip chip, int current, int target) {
    HeatStart& h = heat_starts_[index_of(chip)];
    if (target != h.target) {
        h.target = target;
        h.start = current;
    }
    return h.start;
}

std::string PrintStatusTiles::tool_badge() const {
    auto& tools = ToolState::instance();
    if (subject_int_or_zero(tools.get_show_tool_badge_subject()) == 0) {
        return {};
    }
    const char* t = lv_subject_get_string(tools.get_tool_badge_text_subject());
    return t ? t : "";
}

ChipContent PrintStatusTiles::heater(PrintStatusChip chip, bool show_power) {
    auto& temps = printer_state_.temperature_state();
    bool present = true;
    int current = 0, target = 0;
    HeaterType type = HeaterType::Nozzle;
    ChamberMode mode = ChamberMode::Heating;
    std::string badge;
    switch (chip) {
    case PrintStatusChip::Nozzle: {
        current = subject_int_or_zero(temps.get_active_extruder_temp_subject());
        target = subject_int_or_zero(temps.get_active_extruder_target_subject());
        badge = tool_badge();
        break;
    }
    case PrintStatusChip::Bed:
        type = HeaterType::Bed;
        current = subject_int_or_zero(temps.get_bed_temp_subject());
        target = subject_int_or_zero(temps.get_bed_target_subject());
        break;
    default: {
        type = HeaterType::Chamber;
        auto& caps = printer_state_.capabilities_state();
        present = subject_int_or_zero(caps.subject(Capability::HasChamber)) != 0;
        current = subject_int_or_zero(temps.get_chamber_temp_subject());
        // The effective setpoint: a parked chamber holds its target on the fan.
        target = subject_int_or_zero(temps.get_chamber_effective_target_subject());
        mode = static_cast<ChamberMode>(subject_int_or_zero(temps.get_chamber_mode_subject()));
        break;
    }
    }
    const auto status = temperature::classify_heater_status(
        current, target, subject_int_or_zero(printer_state_.get_heater_power_subject(type)), mode);
    const bool printing = host_.printing && host_.printing();
    return heater_chip(present, current, target, status, printing, show_power, badge,
                       heat_start(chip, current, target));
}

ChipContent PrintStatusTiles::fan(PrintStatusChip chip, bool labeled) {
    const std::string& name = chip == PrintStatusChip::PartFan     ? part_fan_
                              : chip == PrintStatusChip::HotendFan ? hotend_fan_
                                                                   : aux_fan_;
    if (name.empty()) {
        return {};
    }
    auto& fans = printer_state_.fan_state();
    bool automatic = false;
    for (const auto& f : fans.get_fans()) {
        if (f.object_name == name) {
            automatic = !f.is_controllable;
            break;
        }
    }
    return fan_chip(kChips[index_of(chip)].kind, fan_chip_label(chip), true,
                    subject_int_or_zero(fans.get_fan_speed_subject(name)), automatic, labeled);
}

std::array<ChipContent, kPrintStatusChipCount> PrintStatusTiles::decide_chips(bool show_power,
                                                                              bool labeled) {
    std::array<ChipContent, kPrintStatusChipCount> out;
    for (auto c : {PrintStatusChip::Nozzle, PrintStatusChip::Bed, PrintStatusChip::Chamber}) {
        out[index_of(c)] = heater(c, show_power);
    }
    for (auto c : {PrintStatusChip::PartFan, PrintStatusChip::HotendFan, PrintStatusChip::AuxFan}) {
        out[index_of(c)] = fan(c, labeled);
    }
    out[index_of(PrintStatusChip::Light)] =
        light_chip(subject_int_or_zero(
                       helix::led::LedController::instance().get_led_controllable_subject()) != 0,
                   helix::home_light_buttons_lit());
    out[index_of(PrintStatusChip::SpeedFlow)] =
        speed_flow_chip(host_.speed_pct ? host_.speed_pct() : 100,
                        host_.flow_pct ? host_.flow_pct() : 100, host_.speed_text());
    return out;
}

void PrintStatusTiles::publish(PrintStatusChip chip, const ChipContent& c, bool shown, bool glow,
                               bool line) {
    ChipSubjects& s = chips_[index_of(chip)];
    set_string(&s.text, s.text_buf, sizeof(s.text_buf), c.text);
    set_int(&s.tone, static_cast<int>(c.tone));
    set_int(&s.progress, is_heater(chip) ? c.progress_pct : -1);
    set_int(&s.state, is_heater(chip) ? c.status_state : 0);
    set_int(&s.glow, glow ? 1 : 0);
    set_int(&s.line, line ? 1 : 0);
    set_int(&s.shown, shown ? 1 : 0);
}

PrintStatusTiles::Image PrintStatusTiles::current_image() const {
    Image img;
    if (image_stale_) {
        // An import can rewrite the image under the same path, so the decoded
        // copy and the size read from it go too.
        if (!image_path_.empty()) {
            lv_image_cache_drop(image_path_.c_str());
        }
        natural_size_path_.clear();
        lv_display_t* disp = lv_display_get_default();
        image_path_ = PrinterImageManager::instance().get_displayed_image_path(
            disp ? lv_display_get_horizontal_resolution(disp) : 800);
        image_stale_ = false;
    }
    img.path = image_path_;
    if (img.path.empty()) {
        return img;
    }
    if (natural_size_path_ != img.path) {
        lv_image_header_t hdr;
        if (lv_image_decoder_get_info(img.path.c_str(), &hdr) != LV_RESULT_OK) {
            return img;
        }
        natural_size_path_ = img.path;
        natural_w_ = static_cast<int>(hdr.w);
        natural_h_ = static_cast<int>(hdr.h);
    }
    img.natural_w = natural_w_;
    img.natural_h = natural_h_;
    img.regions = lookup_image_regions(printer_image_region_key(img.path), natural_w_, natural_h_);
    return img;
}

void PrintStatusTiles::update_now() {
    cancel_update_timer();
    if (!subjects_ready_) {
        return;
    }

    auto contents = decide_chips(false, true);
    // The same chips with their labels left off, for a layout that sheds them.
    auto bare = decide_chips(false, false);
    lv_obj_t* root = host_.root ? host_.root() : nullptr;
    lv_obj_t* area = root ? lv_obj_find_by_name(root, "ps_printer_area") : nullptr;
    std::array<lv_obj_t*, kPrintStatusChipCount> chip_objs{};
    lv_obj_t* probe = nullptr;
    for (size_t i = 0; i < kPrintStatusChipCount; ++i) {
        chip_objs[i] =
            area ? lv_obj_find_by_name(area, ("ps_chip_" + std::string(kChips[i].key)).c_str())
                 : nullptr;
        if (!probe && chip_objs[i]) {
            probe = chip_objs[i];
        }
    }

    if (!area || !probe) {
        // No printer tile in this tree: the chips publish their content alone.
        for (size_t i = 0; i < kPrintStatusChipCount; ++i) {
            const auto chip = static_cast<PrintStatusChip>(i);
            publish(chip, contents[i], contents[i].shown, false, false);
        }
        return;
    }

    const CalloutChipMetrics m = measure_callout_chip(probe);
    // A rewrite under the same path needs the picture shown and checked again.
    if (image_stale_) {
        exact_image_.forget_displayed();
    }
    const Image image = image_source_ ? image_source_() : Image{};

    // What each chip shows decides the mode, at representative readings so a
    // digit coming or going never moves the picture: a heater heating or
    // holding, a fan at full speed, labeled and not, speed and flow at 100%.
    const std::string badge = tool_badge();
    const auto worst = [&](PrintStatusChip c, bool labeled) -> std::string {
        switch (c) {
        case PrintStatusChip::Nozzle:
            return heater_budget_text(badge);
        case PrintStatusChip::Bed:
        case PrintStatusChip::Chamber:
            return heater_budget_text({});
        case PrintStatusChip::PartFan:
        case PrintStatusChip::HotendFan:
        case PrintStatusChip::AuxFan:
            return fan_chip(kChips[index_of(c)].kind, fan_chip_label(c), true, 100, false, labeled)
                .text;
        case PrintStatusChip::Light:
            return {};
        case PrintStatusChip::SpeedFlow:
            return {}; // measured from its ChipContent, lead value included
        }
        return {};
    };
    const auto is_fan = [](PrintStatusChip c) {
        return c == PrintStatusChip::PartFan || c == PrintStatusChip::HotendFan ||
               c == PrintStatusChip::AuxFan;
    };
    const PrimaryFanSlots fan_slots = printer_state_.fan_state().possible_primary_fan_slots();
    const auto in_budget = [&](PrintStatusChip c) {
        switch (c) {
        case PrintStatusChip::Nozzle:
        case PrintStatusChip::Bed:
        case PrintStatusChip::SpeedFlow:
            return true;
        case PrintStatusChip::Chamber: {
            auto& caps = printer_state_.capabilities_state();
            return subject_int_or_zero(caps.subject(Capability::HasChamber)) != 0;
        }
        // What the printer's fans can fill, not which fan fills a slot now: a
        // fan moving between slots must not change the mode.
        case PrintStatusChip::PartFan:
            return fan_slots.part;
        case PrintStatusChip::HotendFan:
            return fan_slots.hotend;
        case PrintStatusChip::AuxFan:
            return fan_slots.aux;
        case PrintStatusChip::Light:
            return contents[index_of(c)].shown;
        }
        return false;
    };

    // A heater chip ends in its heater_status glyph; room for the widest one
    // keeps its width the same whatever state it shows.
    int status_glyph = 0;
    for (const char* g : {"fire", "check", "cooldown", "alpha_a_circle"}) {
        status_glyph = std::max(status_glyph, m.icon_px(g));
    }
    std::array<int, kPrintStatusChipCount> icons{}, budget_w{}, budget_bare_w{};
    // The worst case of a chip budgeted from its ChipContent; its lines set the chip's height.
    std::array<std::string, kPrintStatusChipCount> budget_text;
    int column_w = 0;
    const auto width_of = [&](size_t i, const ChipContent& c) {
        return m.chip_w(icons[i], c.text);
    };
    for (size_t i = 0; i < kPrintStatusChipCount; ++i) {
        const auto chip = static_cast<PrintStatusChip>(i);
        icons[i] = m.icon_px(kChips[i].icon) + (is_heater(chip) ? m.col_gap + status_glyph : 0);
        if (!in_budget(chip)) {
            budget_w[i] = 0;
        } else if (chip == PrintStatusChip::SpeedFlow) {
            const ChipContent b = speed_flow_budget_chip();
            budget_w[i] = width_of(i, b);
            budget_text[i] = b.text;
        } else {
            budget_w[i] = m.chip_w(icons[i], worst(chip, true));
        }
        if (in_budget(chip) && is_fan(chip)) {
            budget_bare_w[i] = m.chip_w(icons[i], worst(chip, false));
        }
        column_w = std::max(column_w, budget_w[i]);
    }

    // Heater duty rides along only where the last layout put chips in a column
    // and the wider text still fits it.
    const auto with_power = decide_chips(true, true);
    for (auto c : {PrintStatusChip::Nozzle, PrintStatusChip::Bed, PrintStatusChip::Chamber}) {
        const size_t i = index_of(c);
        if (with_power[i].text != contents[i].text &&
            heater_power_fits(last_mode_, m.chip_w(icons[i], with_power[i].text), column_w)) {
            contents[i] = with_power[i];
        }
    }

    const bool printing = host_.printing && host_.printing();
    CalloutPresenter::Parts parts;
    parts.area = area;
    parts.image = lv_obj_find_by_name(area, "ps_printer_image");
    for (size_t i = 0; i < kPrintStatusChipCount; ++i) {
        const auto chip = static_cast<PrintStatusChip>(i);
        CalloutPresenter::Chip part;
        part.kind = kChips[i].kind;
        part.chip = chip_objs[i];
        part.line = lv_obj_find_by_name(area, ("ps_line_" + std::string(kChips[i].key)).c_str());
        part.glow = lv_obj_find_by_name(area, ("ps_glow_" + std::string(kChips[i].key)).c_str());
        part.in_budget = in_budget(chip);
        part.active = contents[i].shown;
        part.expendable = contents[i].expendable;
        part.budget_expendable = phase_budget_expendable(part.kind, printing);
        part.glowing = contents[i].glow;
        // Everything beside the chip's own label, which bounds the label's width.
        part.icons = icons[i];
        part.w_budget = budget_w[i];
        part.w_full = part.active ? width_of(i, contents[i]) : 0;
        if (is_fan(chip)) {
            part.w_budget_unlabeled = budget_bare_w[i];
            part.w_full_unlabeled = part.active ? m.chip_w(icons[i], bare[i].text) : 0;
        }
        // Pinned chips sit on their parts and carry no labels.
        const ChipContent& b = bare[i];
        part.w_compact = part.active ? compact_callout_chip_w(probe, b.text, icons[i]) : 0;
        if (const int lines = text_line_count(budget_text[i]); lines > 1) {
            part.h = m.chip_h_for(budget_text[i]);
            part.h_compact = compact_callout_chip_h(
                probe, contents[index_of(PrintStatusChip::Light)].shown, lines);
        }
        parts.chips.push_back(part);
    }
    const std::string fan_pct =
        std::to_string(subject_int_or_zero(
            part_fan_.empty() ? nullptr
                              : printer_state_.fan_state().get_fan_speed_subject(part_fan_))) +
        "%";
    const std::string toolhead_text =
        toolhead_chip_text(contents[index_of(PrintStatusChip::Nozzle)].text, fan_pct);
    if (lv_obj_t* th_obj = lv_obj_find_by_name(area, "ps_chip_toolhead")) {
        CalloutPresenter::Chip th;
        th.kind = CalloutKind::Toolhead;
        th.chip = th_obj;
        th.w_compact = compact_callout_chip_w(probe, toolhead_text, m.col_gap + status_glyph);
        parts.toolhead = th;
    }

    CalloutPresenter::Config cfg;
    cfg.chip_h = m.chip_h;
    cfg.compact_chip_h =
        compact_callout_chip_h(probe, contents[index_of(PrintStatusChip::Light)].shown);
    cfg.gap = theme_manager_get_spacing("space_xs");
    cfg.min_line = theme_manager_get_spacing("space_md");
    cfg.leader_min_distance = theme_manager_get_spacing("space_lg");

    const CalloutLayout out =
        presenter_.apply(parts, image.regions, image.natural_w, image.natural_h, cfg);
    ++layout_count_;
    last_layout_ = out;
    // After the layout sized the picture, so the copy is cut for its rect.
    exact_image_.apply(image.path);

    const bool glows = image.regions && lined_or_pinned(out.mode);
    for (size_t i = 0; i < kPrintStatusChipCount; ++i) {
        const auto chip = static_cast<PrintStatusChip>(i);
        const CalloutKind kind = kChips[i].kind;
        const bool dropped =
            std::find(out.dropped.begin(), out.dropped.end(), kind) != out.dropped.end();
        bool line = false;
        for (const auto& c : out.chips) {
            if (c.kind == kind) {
                line = c.has_line;
            }
        }
        const ChipContent& shown = out.unlabeled && is_fan(chip) ? bare[i] : contents[i];
        publish(chip, shown, shown.shown && !dropped, glows && shown.glow, line);
    }
    set_string(&toolhead_text_, toolhead_text_buf_, sizeof(toolhead_text_buf_), toolhead_text);
    set_int(&toolhead_merged_, out.toolhead_merged ? 1 : 0);
    set_int(&callout_mode_, static_cast<int>(out.mode));

    if (out.mode != last_mode_) {
        // Heater duty was sized for the previous mode.
        last_mode_ = out.mode;
        request_update();
    }
}

void PrintStatusTiles::refresh_checklist() {
    if (!subjects_ready_) {
        return;
    }
    std::vector<PreprintStepRow> rows;
    if (checklist_source_) {
        if (auto in = checklist_source_()) {
            rows = build_preprint_checklist(*in);
        }
    }
    const size_t count = std::min(rows.size(), kPreprintChecklistMaxRows);
    for (size_t i = 0; i < kPreprintChecklistMaxRows; ++i) {
        StepSubjects& s = steps_[i];
        if (i < count) {
            set_string(&s.name, s.name_buf, sizeof(s.name_buf),
                       print_start_phase_step_label(rows[i].phase));
            set_string(&s.time, s.time_buf, sizeof(s.time_buf), step_time(rows[i]));
            set_int(&s.status, static_cast<int>(rows[i].status));
        } else {
            set_string(&s.name, s.name_buf, sizeof(s.name_buf), "");
            set_string(&s.time, s.time_buf, sizeof(s.time_buf), "");
            set_int(&s.status, static_cast<int>(PreprintStepStatus::Upcoming));
        }
    }
    set_int(&step_count_, static_cast<int>(count));

    const PreprintStepPosition pos = preprint_step_position(rows);
    std::string position, name, time;
    if (pos.index > 0) {
        position = fmt::format(lv_tr("Step {} of {}"), pos.index, pos.count);
        const PreprintStepRow& current = rows[static_cast<size_t>(pos.index - 1)];
        name = print_start_phase_step_label(current.phase);
        time = step_time(current);
    }
    set_string(&step_position_text_, step_position_buf_, sizeof(step_position_buf_), position);
    set_string(&step_current_name_, step_current_name_buf_, sizeof(step_current_name_buf_), name);
    set_string(&step_current_time_, step_current_time_buf_, sizeof(step_current_time_buf_), time);
}

} // namespace helix::ui
