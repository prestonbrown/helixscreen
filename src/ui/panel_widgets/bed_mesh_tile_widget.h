// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/bed_mesh_tile_layout.h"

#include <functional>
#include <optional>

namespace helix {

class PrinterState;

/// Home widget for the active bed mesh. Reads BedMeshState (which exists from connect),
/// measures what each rung would draw, decides the rung with decide_bed_mesh_tile_layout()
/// and publishes the verdict and its texts as subjects that panel_widget_bed_mesh_tile.xml
/// binds. The heatmap is a static, non-interactive <bed_mesh> fed when the mesh changes. A
/// tap opens the Bed Mesh panel; with no mesh loaded and nothing printing, the Full rung
/// adds Calibrate and Load profile.
class BedMeshTileWidget : public PanelWidget {
  public:
    explicit BedMeshTileWidget(PrinterState& printer_state);
    ~BedMeshTileWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_deactivate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "bed_mesh_tile";
    }

    static void clicked_cb(lv_event_t* e);

    /// Home the toolhead if it is not, then start a mesh calibration into the default
    /// profile. Does nothing unless the Calibrate button is on screen and no job holds
    /// the machine.
    void calibrate();
    /// A calibration was sent and neither finished, failed nor produced a new mesh yet.
    [[nodiscard]] bool calibrating() const {
        return calibrating_;
    }
    /// Open the Bed Mesh panel (the tile's tap, and Load profile's). Does nothing in a build
    /// without the panel.
    void open_panel();

    /// The last verdict, for tests.
    [[nodiscard]] const BedMeshTileVerdict& verdict() const {
        return verdict_;
    }

  private:
    struct LiveState;
    LiveState read_live() const;
    BedMeshTileVerdict decide(int width_px, int height_px, const LiveState& live) const;
    void publish_verdict();
    void publish_texts(const LiveState& live);
    void on_live_changed();
    void on_mesh_changed();
    void set_calibrating(bool on);
    void feed_map(const LiveState& live);
    lv_obj_t* find_map() const;

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    BedMeshTileVerdict verdict_;
    /// The state the verdict was decided for; a change re-decides, because the content
    /// being measured changed.
    bool decided_loaded_ = false;
    bool decided_print_active_ = false;
    int decided_profile_count_ = 0;
    /// The mesh version the canvas was last fed, so an unchanged mesh costs no render.
    std::optional<int> fed_version_;
    /// The build volume version the canvas bounds were fed from.
    std::optional<int> bounds_version_;
    /// The render mode last applied to the canvas.
    std::optional<bool> applied_3d_;
    /// The canvas dropped its static 3D render for 2D after an overrun and stays there.
    bool static_3d_banned_ = false;
    bool calibrating_ = false;
    /// The home panel is covered; mesh updates wait for it to come back.
    bool panel_hidden_ = false;

    ObserverGuard version_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard profile_count_observer_;
    ObserverGuard build_volume_observer_;
    ObserverGuard language_observer_;

    // Declared last so it is torn down first, expiring every token the observers
    // captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_bed_mesh_tile_widget();

/// Replaces what a tap opens, so a test can see that one happened. Pass an empty function to
/// restore the real panel.
void set_bed_mesh_tile_open_hook_for_test(std::function<void()> hook);

/// Forces whether the Bed Mesh panel counts as built; pass nullopt to read the build.
void set_bed_mesh_tile_panel_available_for_test(std::optional<bool> available);

/// Forces the full-tier answer a test cares about; pass nullopt to read the platform tier.
void set_bed_mesh_tile_full_tier_for_test(std::optional<bool> full_tier);

} // namespace helix
