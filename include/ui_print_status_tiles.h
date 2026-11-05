// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_print_status_tiles.h
 * @brief The print status printer tile's chips and the job tile's checklist,
 *        as subjects.
 *
 * Every chip's content comes from the pure decisions in print_status_chips.h,
 * the checklist rows from preprint_checklist.h, and the chips' places from the
 * shared CalloutPresenter, so this class only gathers readings, measures and
 * copies results into subjects the XML binds.
 *
 * Updates are coalesced: any number of request_update() calls in one frame
 * lead to one chip pass and one callout layout on the next timer tick.
 *
 * @threading Main thread only.
 */

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "callout_presenter.h"
#include "preprint_checklist.h"
#include "print_status_chips.h"
#include "printer_image_regions.h"
#include "src/ui/panel_widgets/exact_size_image.h"
#include "subject_managed_panel.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {
class PrinterState;
}

namespace helix::ui {

/// The printer tile's chips, in the order their subjects are named.
enum class PrintStatusChip {
    Nozzle = 0,
    Bed,
    Chamber,
    PartFan,
    HotendFan,
    AuxFan,
    Light,
    SpeedFlow,
    OtherTools,
};
inline constexpr size_t kPrintStatusChipCount = 9;

/// The `<key>` in `ps_chip_<key>_*`, `ps_line_<key>` and `ps_glow_<key>`.
[[nodiscard]] const char* print_status_chip_key(PrintStatusChip chip);

class PrintStatusTiles {
  public:
    /// The printer picture the tile shows. `regions` is null for an untagged
    /// image or a custom photo, which docks every chip.
    struct Image {
        std::string path;
        int natural_w = 0;
        int natural_h = 0;
        const ImageRegions* regions = nullptr;
    };

    /// What the panel knows that the printer state does not.
    struct Host {
        std::function<lv_obj_t*()> root;
        /// Past pre-print: a heater leaving its band warns instead of working.
        std::function<bool()> printing;
        std::function<int()> speed_pct;
        std::function<int()> flow_pct;
        /// The speed/flow chip's text; required, the chip has no other source.
        std::function<std::string()> speed_text;
    };

    PrintStatusTiles(PrinterState& printer_state, Host host);
    ~PrintStatusTiles();

    PrintStatusTiles(const PrintStatusTiles&) = delete;
    PrintStatusTiles& operator=(const PrintStatusTiles&) = delete;

    /// Register every ps_* subject in `subjects` and start watching the
    /// readings the panel does not already forward.
    void init_subjects(SubjectManager& subjects);
    /// Stop watching; call before the subjects are freed.
    void deinit();

    /// The fans the panel classified; empty names have no chip.
    void set_fans(const std::string& part, const std::string& hotend, const std::string& aux);

    /// Schedule one chip pass and layout for the next tick.
    void request_update();
    /// Run the pending update now.
    void update_now();
    /// Re-read the collector's checklist and publish the rows.
    void refresh_checklist();
    /// The widget tree went away; forget its pointers and the pending update.
    void on_tree_destroyed();

    /// Where checklist input comes from; nullopt while there is no collector.
    void set_checklist_source(std::function<std::optional<PreprintChecklistInput>()> source) {
        checklist_source_ = std::move(source);
    }
    /// Where the printer picture comes from.
    void set_image_source(std::function<Image()> source) {
        image_source_ = std::move(source);
    }
    /// The picture the printer image manager displays, the default source.
    void use_displayed_image() {
        image_source_ = [this]() { return current_image(); };
        image_stale_ = true;
    }

    [[nodiscard]] int layout_count() const {
        return layout_count_;
    }
    /// What the last layout pass decided, chip rects included.
    [[nodiscard]] const CalloutLayout& last_layout() const {
        return last_layout_;
    }

  private:
    static constexpr size_t kChipSubjectCount = 7;
    struct ChipSubjects {
        lv_subject_t shown{};
        lv_subject_t text{};
        lv_subject_t tone{};
        lv_subject_t glow{};
        lv_subject_t progress{};
        lv_subject_t line{};
        lv_subject_t state{};
        char text_buf[96] = "";
    };
    struct StepSubjects {
        lv_subject_t name{};
        lv_subject_t time{};
        lv_subject_t status{};
        char name_buf[48] = "";
        char time_buf[16] = "";
    };
    /// Where a heater started climbing, for its progress bar.
    struct HeatStart {
        int target = 0;
        int start = 0;
    };

    void cancel_update_timer();
    /// `labeled`: fan chips name their role ("Part 30%").
    std::array<ChipContent, kPrintStatusChipCount> decide_chips(bool show_power, bool labeled);
    void publish(PrintStatusChip chip, const ChipContent& c, bool shown, bool glow, bool line);
    /// The active tool's badge ("T1") on a multi-tool printer, empty otherwise.
    std::string tool_badge() const;
    ChipContent heater(PrintStatusChip chip, bool show_power);
    int heat_start(PrintStatusChip chip, int current, int target);
    /// Every head but the printing one, read for the other-tools chip.
    std::vector<OtherToolReading> other_tool_readings();
    /// Watches every extruder's reading and target, so a parked head's pre-heat redraws.
    void watch_extruders();
    ChipContent fan(PrintStatusChip chip, bool labeled);
    Image current_image() const;

    PrinterState& printer_state_;
    Host host_;
    AsyncLifetimeGuard lifetime_;
    CalloutPresenter presenter_;
    /// The printer picture's 1:1 cached copy for the size the layout gave it.
    ExactSizeImage exact_image_;

    std::array<ChipSubjects, kPrintStatusChipCount> chips_{};
    std::array<std::string, kChipSubjectCount * kPrintStatusChipCount> chip_names_{};
    lv_subject_t image_src_{};
    char image_src_buf_[256] = "";
    lv_subject_t callout_mode_{};
    lv_subject_t toolhead_merged_{};
    lv_subject_t toolhead_text_{};
    char toolhead_text_buf_[48] = "";

    std::array<StepSubjects, kPreprintChecklistMaxRows> steps_{};
    std::array<std::string, 3 * kPreprintChecklistMaxRows> step_names_{};
    lv_subject_t step_count_{};
    lv_subject_t step_position_text_{};
    lv_subject_t step_current_name_{};
    lv_subject_t step_current_time_{};
    char step_position_buf_[32] = "";
    char step_current_name_buf_[48] = "";
    char step_current_time_buf_[16] = "";

    std::array<HeatStart, 3> heat_starts_{};
    /// Keyed by extruder name, for the other-tools chip's bar.
    std::unordered_map<std::string, HeatStart> other_heat_starts_;
    std::string part_fan_;
    std::string hotend_fan_;
    std::string aux_fan_;
    /// The last layout's mode: heater duty is sized for it.
    CalloutMode last_mode_ = CalloutMode::ImageOnly;

    std::function<std::optional<PreprintChecklistInput>()> checklist_source_;
    std::function<Image()> image_source_;
    /// The displayed image's path, resolved again only after one of
    /// image_observers_ fires: resolving reads the filesystem and config.
    mutable std::string image_path_;
    mutable bool image_stale_ = true;
    mutable std::string natural_size_path_;
    std::vector<ObserverGuard> image_observers_;
    mutable int natural_w_ = 0;
    mutable int natural_h_ = 0;

    lv_timer_t* update_timer_ = nullptr;
    int layout_count_ = 0;
    CalloutLayout last_layout_;
    bool subjects_ready_ = false;

    ObserverGuard led_version_observer_;
    ObserverGuard led_controllable_observer_;
    ObserverGuard tool_badge_observer_;
    ObserverGuard tool_badge_text_observer_;
    ObserverGuard nozzle_power_observer_;
    ObserverGuard bed_power_observer_;
    ObserverGuard chamber_power_observer_;

    // Lifetimes before observers: members destroy in reverse, so each guard
    // goes while its lifetime still says whether the subject lives.
    struct ExtruderWatch {
        SubjectLifetime temp_lifetime;
        SubjectLifetime target_lifetime;
        ObserverGuard temp;
        ObserverGuard target;
    };
    std::vector<ExtruderWatch> extruder_watches_;
    ObserverGuard extruder_version_observer_;
    ObserverGuard tools_version_observer_;
};

} // namespace helix::ui
