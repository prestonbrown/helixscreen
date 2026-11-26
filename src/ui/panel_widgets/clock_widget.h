// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "core/subject.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/clock_face.h"
#include "subject_managed_panel.h"

#if HELIX_ENABLE_SCREENSAVER
#include "src/ui/panel_widgets/clock_face_picker.h"
#include "src/ui/panel_widgets/clock_face_view.h"
#endif

#include <string>
#include <vector>

#include "hv/json.hpp" // JSON_HPP_OK: config_ held by value

namespace helix {

/**
 * @brief The home clock: the time as text, or one of the clock savers' faces
 *
 * Each instance (clock, clock:N) has its own Face, stored at its config's "face". A painted face
 * shows on a canvas the size of the tile where the tile is large enough for it
 * (clock_face_shown()), and Text otherwise. The size mode and whether a face shows are this
 * instance's own subjects; the time, date and uptime text are shared by every clock.
 */
class ClockWidget : public PanelWidget {
  public:
    explicit ClockWidget(const std::string& instance_id = "clock");
    ~ClockWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    void on_activate() override;
    void on_deactivate() override;
    const char* id() const override {
        return instance_id_.c_str();
    }
    /// Every instance builds the one clock component, whatever its instance suffix.
    std::string get_component_name() const override {
        return "panel_widget_clock";
    }
    const char** xml_attrs() const override {
        return const_cast<const char**>(attrs_.data());
    }
    bool has_edit_configure() const override;
    bool on_edit_configure() override;

    /// The face this instance is set to, whatever its tile shows.
    ClockFace face() const {
        return face_;
    }
    /// Sets the face, stores it in this instance's config and shows it.
    void select_face(ClockFace face);
    /// The XML names of this instance's size-mode and face-shown subjects.
    const std::string& size_subject_name() const {
        return size_subject_name_;
    }
    const std::string& face_subject_name() const {
        return face_subject_name_;
    }
#if HELIX_ENABLE_SCREENSAVER
    /// The painted face on the tile, for tests.
    const ClockFaceView& face_view() const {
        return face_view_;
    }
#endif

  private:
    /// Shows the face that fits the granted size, or Text, while the panel is active.
    void apply_face();

    std::string instance_id_;
    nlohmann::json config_ = nlohmann::json::object();
    ClockFace face_ = ClockFace::TEXT;
    bool active_ = false;

    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef parent_screen_;
    lv_timer_t* clock_timer_ = nullptr;

    SubjectManager subjects_;
    helix::Subject size_subject_{};
    helix::Subject face_subject_{};
    std::string size_subject_name_;
    std::string face_subject_name_;
    std::vector<std::string> attr_storage_;
    std::vector<const char*> attrs_;

#if HELIX_ENABLE_SCREENSAVER
    ClockFaceView face_view_;
    ClockFacePicker picker_;
#endif

    // Update all clock subjects with current time/date/uptime
    void update_clock();

    // Whether the time over the date fits this box at their widest, inside the
    // widget's own padding
    bool time_and_date_fit(int width_px, int height_px) const;

    // Static timer callback
    static void clock_timer_cb(lv_timer_t* timer);
};

} // namespace helix
