// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_types.h"
#include "overlay_base.h"
#include "thumbnail_cache.h"

#include <set>
#include <string>
#include <vector>

struct TimelapseCardDimensions {
    int card_width;
    int card_height;
};

class TimelapseVideosOverlay : public OverlayBase {
  public:
    explicit TimelapseVideosOverlay(helix::IMoonrakerAPI* api);

    lv_obj_t* create(lv_obj_t* parent) override;
    void register_callbacks() override;

    [[nodiscard]] const char* get_name() const override {
        return "Timelapse Videos";
    }
    [[nodiscard]] const char* xml_component() const override {
        return "timelapse_videos_overlay";
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;

    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Play one video through the detected player
     *
     * Downloads first when Moonraker is remote. Without a player (no mpv or
     * ffplay on the host) this logs and returns; can_play() says so up front.
     */
    void play_video(const std::string& filename);

    [[nodiscard]] bool can_play() const {
        return can_play_;
    }

  private:
    struct VideoEntry {
        std::string filename;
        std::string file_info; // "12.4 MB · Mar 12"
        uint64_t size = 0;
        double modified = 0.0;
    };

    void fetch_frame_info();
    void fetch_timelapse_root();
    void fetch_video_list();
    TimelapseCardDimensions calculate_card_dimensions();
    void populate_video_grid(const std::vector<FileInfo>& files);
    void load_thumbnail_for_card(lv_obj_t* card, const std::string& filename,
                                 const std::set<std::string>& available_files);
    void clear_video_grid();

    void detect_playback_capability();
    /// Adopt Moonraker's "timelapse" root from a server.files.roots answer.
    void apply_timelapse_root(const std::vector<FileRoot>& roots);
    /// Where a same-host player opens `filename`: under the reported root,
    /// or the stock data_path layout until one is reported.
    [[nodiscard]] std::string local_video_path(const std::string& filename) const;

    void confirm_delete(const std::string& filename);

    static void on_card_clicked(lv_event_t* e);
    static void on_card_long_pressed(lv_event_t* e);

    friend struct TimelapseVideosOverlayTestAccess;

    helix::IMoonrakerAPI* api_;
    std::vector<VideoEntry> videos_;
    bool can_play_ = false;
    std::string player_command_;
    bool is_local_moonraker_ = false;
    std::string timelapse_root_; ///< Moonraker's "timelapse" root on disk, when reported

    helix::AsyncLifetimeGuard thumb_lifetime_;

    lv_obj_t* video_grid_container_ = nullptr;
    lv_obj_t* video_grid_empty_ = nullptr;

    std::string pending_delete_filename_;
    helix::ui::WidgetRef delete_confirmation_dialog_;

    // Cached gradient buffer (shared across all video cards)
    lv_draw_buf_t* cached_gradient_ = nullptr;
    int32_t cached_gradient_w_ = 0;
    int32_t cached_gradient_h_ = 0;
    bool cached_gradient_dark_ = true;

    void ensure_gradient_cache(int32_t card_width, int32_t card_height);
};

TimelapseVideosOverlay& get_global_timelapse_videos();
void init_global_timelapse_videos(helix::IMoonrakerAPI* api);
void open_timelapse_videos();

namespace helix::ui {

/**
 * @brief Open the timelapse browser on a specific video and play it
 *
 * The browser is pushed first so the user lands on the library either way;
 * playback then starts when a player exists, and a toast says so when none
 * does. A no-op (with a log line) on builds without the viewer.
 */
void open_timelapse_video(const std::string& filename);

/// False on builds compiled without the viewer (HELIX_HAS_TIMELAPSE_VIEWER=0),
/// where every open_* entry point is inert.
bool timelapse_viewer_available();

} // namespace helix::ui
