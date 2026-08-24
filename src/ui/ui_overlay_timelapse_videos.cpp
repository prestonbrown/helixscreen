// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_timelapse_videos.h"

#include "helix_plugin_installer.h"
#include "host_identity.h"

#if HELIX_HAS_TIMELAPSE_VIEWER

#include "ui_callback_helpers.h"
#include "ui_format_utils.h"
#include "ui_gradient_canvas.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"
#include "timelapse_state.h"
#include "timelapse_thumbnailer.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Default path for timelapse files relative to $HOME.
// TODO: Query Moonraker's data_path config if an API becomes available.
static constexpr const char* TIMELAPSE_DATA_SUBPATH = "printer_data/timelapse/";

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<TimelapseVideosOverlay> g_timelapse_videos;
static lv_obj_t* g_timelapse_videos_panel = nullptr;

TimelapseVideosOverlay& get_global_timelapse_videos() {
    if (!g_timelapse_videos) {
        spdlog::error(
            "[Timelapse Videos] get_global_timelapse_videos() called before initialization!");
        throw std::runtime_error("TimelapseVideosOverlay not initialized");
    }
    return *g_timelapse_videos;
}

void init_global_timelapse_videos(IMoonrakerAPI* api) {
    if (g_timelapse_videos) {
        spdlog::warn("[Timelapse Videos] TimelapseVideosOverlay already initialized, skipping");
        return;
    }
    g_timelapse_videos = std::make_unique<TimelapseVideosOverlay>(api);
    StaticPanelRegistry::instance().register_destroy("TimelapseVideosOverlay", []() {
        if (g_timelapse_videos_panel) {
            NavigationManager::instance().unregister_overlay_instance(g_timelapse_videos_panel);
        }
        g_timelapse_videos_panel = nullptr;
        g_timelapse_videos.reset();
    });
    spdlog::trace("[Timelapse Videos] TimelapseVideosOverlay initialized");
}

void open_timelapse_videos() {
    spdlog::debug("[Timelapse Videos] Opening timelapse videos overlay");

    if (!g_timelapse_videos) {
        spdlog::error("[Timelapse Videos] Global instance not initialized!");
        return;
    }

    // Lazy-create the panel
    if (!g_timelapse_videos_panel) {
        spdlog::debug("[Timelapse Videos] Creating timelapse videos panel...");
        g_timelapse_videos_panel =
            g_timelapse_videos->create(lv_display_get_screen_active(nullptr));

        if (g_timelapse_videos_panel) {
            NavigationManager::instance().register_overlay_instance(g_timelapse_videos_panel,
                                                                    g_timelapse_videos.get());
            spdlog::debug("[Timelapse Videos] Panel created and registered");
        } else {
            spdlog::error("[Timelapse Videos] Failed to create timelapse_videos_overlay");
            return;
        }
    }

    // Show the overlay - NavigationManager will call on_activate()
    NavigationManager::instance().push_overlay(g_timelapse_videos_panel);
}

// ============================================================================
// CONSTRUCTOR
// ============================================================================

TimelapseVideosOverlay::TimelapseVideosOverlay(IMoonrakerAPI* api) : api_(api) {
    spdlog::debug("[{}] Constructor", get_name());
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void TimelapseVideosOverlay::init_subjects() {
    spdlog::debug("[{}] init_subjects()", get_name());

    // Register XML callbacks for the render button
    register_xml_callbacks({
        {"on_timelapse_render_now", on_render_now},
    });
}

lv_obj_t* TimelapseVideosOverlay::create(lv_obj_t* parent) {
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "timelapse_videos_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] create() - finding widgets", get_name());

    video_grid_container_ = lv_obj_find_by_name(overlay_root_, "video_grid_container");
    video_grid_empty_ = lv_obj_find_by_name(overlay_root_, "video_grid_empty");

    spdlog::debug("[{}] Widgets found: grid_container={} grid_empty={}", get_name(),
                  video_grid_container_ != nullptr, video_grid_empty_ != nullptr);

    return overlay_root_;
}

void TimelapseVideosOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] on_activate() - fetching video list", get_name());
    detect_playback_capability();
    fetch_frame_info();
    fetch_video_list();

    // Register render-complete callback to auto-refresh the video list
    // when a new timelapse render finishes (picks up new video + companion thumbnail)
    auto tok = lifetime_.token();
    helix::TimelapseState::instance().set_on_render_complete(
        [this, tok](const std::string& filename) {
            if (tok.expired())
                return;
            spdlog::info("[Timelapse Videos] Render complete for '{}', refreshing list", filename);
            tok.defer([this]() { fetch_video_list(); });
        });
}

void TimelapseVideosOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
    clear_video_grid();

    // Unregister render-complete callback to avoid dangling references
    helix::TimelapseState::instance().set_on_render_complete(nullptr);

    spdlog::debug("[{}] on_deactivate()", get_name());
}

void TimelapseVideosOverlay::cleanup() {
    if (cached_gradient_) {
        lv_draw_buf_destroy(cached_gradient_);
        cached_gradient_ = nullptr;
        cached_gradient_w_ = 0;
        cached_gradient_h_ = 0;
    }
    spdlog::debug("[{}] cleanup()", get_name());
    OverlayBase::cleanup();
}

void TimelapseVideosOverlay::ensure_gradient_cache(int32_t card_width, int32_t card_height) {
    bool dark = theme_manager_is_dark_mode();
    if (cached_gradient_ && cached_gradient_w_ == card_width && cached_gradient_h_ == card_height &&
        cached_gradient_dark_ == dark) {
        return;
    }
    if (cached_gradient_) {
        lv_draw_buf_destroy(cached_gradient_);
    }
    cached_gradient_ = ui_gradient_canvas_create_buf(card_width, card_height, dark, 0);
    cached_gradient_w_ = card_width;
    cached_gradient_h_ = card_height;
    cached_gradient_dark_ = dark;
}

// ============================================================================
// VIDEO LIST FETCHING
// ============================================================================

void TimelapseVideosOverlay::fetch_frame_info() {
    if (!api_)
        return;

    auto tok = lifetime_.token();
    api_->timelapse().get_last_frame_info(
        [tok](const LastFrameInfo& info) {
            if (tok.expired())
                return;
            helix::ui::queue_update([info]() {
                auto& tl = helix::TimelapseState::instance();
                lv_subject_set_int(tl.get_frame_count_subject(), info.frame_count);
                spdlog::debug("[Timelapse Videos] Frame info: {} frames", info.frame_count);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[Timelapse Videos] Failed to get frame info: {}", err.message);
        });
}

void TimelapseVideosOverlay::fetch_video_list() {
    if (!api_) {
        spdlog::debug("[{}] No API available", get_name());
        return;
    }

    auto tok = lifetime_.token();

    api_->files().list_files(
        "timelapse", "", false,
        [this, tok](const std::vector<FileInfo>& files) {
            if (tok.expired())
                return;
            tok.defer([this, files]() { populate_video_grid(files); });
        },
        [](const MoonrakerError& error) {
            spdlog::error("[Timelapse Videos] Failed to fetch video list: {}", error.message);
        });
}

// ============================================================================
// VIDEO GRID MANAGEMENT
// ============================================================================

// Card sizing constants (same as print select)
static constexpr int CARD_MIN_WIDTH = 140;
static constexpr int CARD_MAX_WIDTH = 230;
static constexpr int CARD_DEFAULT_HEIGHT = 220;

TimelapseCardDimensions TimelapseVideosOverlay::calculate_card_dimensions() {
    TimelapseCardDimensions dims{CARD_MIN_WIDTH, CARD_DEFAULT_HEIGHT};

    if (!video_grid_container_ || !overlay_root_) {
        spdlog::debug("[{}] Cannot calculate dimensions: container or root is null", get_name());
        return dims;
    }

    // Force layout so dimensions are computed
    lv_obj_update_layout(overlay_root_);

    // Get overlay total height
    lv_coord_t overlay_height = lv_obj_get_height(overlay_root_);

    // Get header height (first child of overlay_root, from overlay_panel)
    lv_obj_t* header = lv_obj_get_child(overlay_root_, 0);
    lv_coord_t header_height = header ? lv_obj_get_height(header) : 56;

    // Get overlay_content padding
    lv_obj_t* content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    lv_coord_t content_pad_top = content ? lv_obj_get_style_pad_top(content, LV_PART_MAIN) : 0;
    lv_coord_t content_pad_bottom =
        content ? lv_obj_get_style_pad_bottom(content, LV_PART_MAIN) : 0;

    // Check if render section is visible and get its height
    lv_obj_t* render_section = lv_obj_find_by_name(overlay_root_, "render_section");
    lv_coord_t render_height = 0;
    if (render_section && !lv_obj_has_flag(render_section, LV_OBJ_FLAG_HIDDEN)) {
        render_height = lv_obj_get_height(render_section);
    }

    // Grid container top padding
    lv_coord_t grid_pad_top = lv_obj_get_style_pad_top(video_grid_container_, LV_PART_MAIN);

    // Content row gap (between render section and grid)
    lv_coord_t content_gap = content ? lv_obj_get_style_pad_row(content, LV_PART_MAIN) : 0;

    // Available height for video grid
    lv_coord_t available_height = overlay_height - header_height - content_pad_top -
                                  content_pad_bottom - render_height - grid_pad_top - content_gap;

    // Card gap from grid container
    int card_gap = lv_obj_get_style_pad_row(video_grid_container_, LV_PART_MAIN);

    // Calculate card height for 2 rows
    int bottom_margin = card_gap / 2;
    int row_height = (available_height - bottom_margin) / 2;
    dims.card_height = row_height - card_gap;

    // Clamp to reasonable range
    dims.card_height = std::max(dims.card_height, 120);
    dims.card_height = std::min(dims.card_height, 280);

    spdlog::debug("[{}] Height calc: overlay={} - header={} - content_pad({}+{}) - "
                  "render={} - grid_pad={} - gap={} = available={}, card_height={}",
                  get_name(), overlay_height, header_height, content_pad_top, content_pad_bottom,
                  render_height, grid_pad_top, content_gap, available_height, dims.card_height);

    // Calculate card width: try column counts from 10 down to 1
    lv_coord_t container_width = lv_obj_get_content_width(video_grid_container_);
    int col_gap = lv_obj_get_style_pad_column(video_grid_container_, LV_PART_MAIN);

    for (int cols = 10; cols >= 1; cols--) {
        int total_gaps = (cols - 1) * col_gap;
        int card_width = (container_width - total_gaps) / cols;

        if (card_width >= CARD_MIN_WIDTH && card_width <= CARD_MAX_WIDTH) {
            dims.card_width = card_width;
            spdlog::debug("[{}] Card layout: {} columns, card={}x{}", get_name(), cols,
                          dims.card_width, dims.card_height);
            return dims;
        }
    }

    // Fallback
    dims.card_width = CARD_MIN_WIDTH;
    spdlog::debug("[{}] Card layout fallback: card={}x{}", get_name(), dims.card_width,
                  dims.card_height);
    return dims;
}

void TimelapseVideosOverlay::populate_video_grid(const std::vector<FileInfo>& files) {
    clear_video_grid();
    videos_.clear();

    // Filter to video files only and build entries
    for (const auto& file : files) {
        if (!helix::timelapse::is_video_file(file.filename)) {
            continue;
        }
        VideoEntry entry;
        entry.filename = file.filename;
        entry.size = file.size;
        entry.modified = file.modified;
        entry.file_info = helix::ui::format_file_size(static_cast<size_t>(file.size)) +
                          " \xC2\xB7 " +
                          helix::ui::format_short_date(static_cast<time_t>(file.modified));
        videos_.push_back(std::move(entry));
    }

    // Sort newest first
    std::sort(videos_.begin(), videos_.end(),
              [](const VideoEntry& a, const VideoEntry& b) { return a.modified > b.modified; });

    spdlog::info("[{}] Found {} timelapse videos", get_name(), videos_.size());

    if (videos_.empty()) {
        // Show empty state, hide grid
        if (video_grid_container_) {
            lv_obj_add_flag(video_grid_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (video_grid_empty_) {
            lv_obj_remove_flag(video_grid_empty_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Show grid, hide empty state
    if (video_grid_container_) {
        lv_obj_remove_flag(video_grid_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (video_grid_empty_) {
        lv_obj_add_flag(video_grid_empty_, LV_OBJ_FLAG_HIDDEN);
    }

    if (!video_grid_container_)
        return;

    // Calculate responsive card dimensions to fit 2 rows on screen
    lv_obj_update_layout(overlay_root_);
    auto dims = calculate_card_dimensions();

    // Build set of available files for companion thumbnail lookup
    std::set<std::string> available_files;
    for (const auto& file : files) {
        available_files.insert(file.filename);
    }

    // Invalidate stale thumbnail callbacks from previous grid population
    thumb_lifetime_.invalidate();

    for (const auto& video : videos_) {
        const char* attrs[] = {"filename", video.filename.c_str(), "file_info",
                               video.file_info.c_str(), nullptr};

        lv_obj_t* card = static_cast<lv_obj_t*>(
            lv_xml_create(video_grid_container_, "timelapse_video_card", attrs));

        if (!card) {
            spdlog::warn("[{}] Failed to create card for '{}'", get_name(), video.filename);
            continue;
        }

        // Apply responsive card dimensions
        lv_obj_set_width(card, dims.card_width);
        lv_obj_set_height(card, dims.card_height);
        lv_obj_set_style_flex_grow(card, 0, LV_PART_MAIN);

        // Apply shared gradient buffer to card
        ensure_gradient_cache(dims.card_width, dims.card_height);
        lv_obj_t* gradient_bg = lv_obj_find_by_name(card, "gradient_bg");
        if (gradient_bg && cached_gradient_) {
            lv_image_set_src(gradient_bg, cached_gradient_);
        }

        // Store filename in user_data for click handler (owned copy, freed on
        // LV_EVENT_DELETE by the helper). L069: the card root is a ui_card, i.e.
        // a plain lv_obj_create'd object — the XML engine never touches
        // user_data — so the slot is ours. On allocation failure the card simply
        // carries no filename and its handlers no-op, instead of crashing.
        if (!helix::ui::set_owned_user_string(card, video.filename)) {
            spdlog::warn("[{}] Could not attach filename to card for '{}'", get_name(),
                         video.filename);
        }

        // NOTE: lv_obj_add_event_cb used here (not XML event_cb) because each dynamically
        // created card needs per-instance user_data (filename) that XML bindings can't provide.
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(card, on_card_long_pressed, LV_EVENT_LONG_PRESSED, this);

        // Show/hide play overlay based on playback capability
        lv_obj_t* play_overlay = lv_obj_find_by_name(card, "play_overlay");
        if (play_overlay) {
            if (can_play_) {
                lv_obj_remove_flag(play_overlay, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(play_overlay, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Load thumbnail (companion image) or show placeholder
        load_thumbnail_for_card(card, video.filename, available_files);
    }
}

void TimelapseVideosOverlay::load_thumbnail_for_card(lv_obj_t* card, const std::string& filename,
                                                     const std::set<std::string>& available_files) {
    lv_obj_t* thumbnail = lv_obj_find_by_name(card, "thumbnail");
    lv_obj_t* no_thumb_icon = lv_obj_find_by_name(card, "no_thumbnail_icon");

    // Use the companion filename (e.g., "video.thumb.jpg") as the Moonraker path
    // within the "timelapse" root for ThumbnailCache
    auto companion = helix::timelapse::companion_filename(filename);

    // Check if companion thumbnail file exists in the timelapse directory listing
    if (available_files.find(companion) == available_files.end()) {
        // No companion thumbnail available -- show placeholder
        spdlog::debug("[{}] No companion thumbnail '{}' for '{}'", get_name(), companion, filename);
        if (no_thumb_icon) {
            lv_obj_remove_flag(no_thumb_icon, LV_OBJ_FLAG_HIDDEN);
        }
        if (thumbnail) {
            lv_obj_add_flag(thumbnail, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Build the relative path for Moonraker download: "timelapse/<companion>"
    // ThumbnailCache's fetch goes through api->download_thumbnail, which expects
    // a relative path under ".thumbnails/". For timelapse files, we use the
    // transfers API directly since these aren't gcode thumbnails.

    // Use the cache key from TimelapseThumbnailer as the cache identifier
    auto cache_key = helix::timelapse::cache_key(filename);
    auto target = helix::ThumbnailProcessor::get_target_for_display(helix::ThumbnailSize::Card);

    // Check if already cached (synchronous, fast path). The request-shaped
    // lookup is the same query as get_if_optimized(key, target) — timelapse
    // companions have no Moonraker mtime to validate against, so
    // source_modified stays 0.
    ThumbnailRequest req;
    req.key = cache_key;
    req.target = target;

    auto& cache = get_thumbnail_cache();
    std::string cached = cache.get_if_cached(req);
    if (!cached.empty()) {
        spdlog::debug("[{}] Thumbnail cache hit for '{}'", get_name(), filename);
        if (thumbnail) {
            lv_image_set_src(thumbnail, cached.c_str());
            lv_obj_remove_flag(thumbnail, LV_OBJ_FLAG_HIDDEN);
        }
        if (no_thumb_icon) {
            lv_obj_add_flag(no_thumb_icon, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Not cached -- download the companion file, pre-scale, and update
    if (!api_) {
        if (no_thumb_icon) {
            lv_obj_remove_flag(no_thumb_icon, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Show placeholder while loading
    if (no_thumb_icon) {
        lv_obj_remove_flag(no_thumb_icon, LV_OBJ_FLAG_HIDDEN);
    }

    std::string filename_copy = filename;

    // Download the companion thumbnail via the timelapse file root, keyed by the
    // timelapse-specific cache key so the pre-scaled .bin lands under it.
    // The companion .jpg is accessible at "timelapse/<companion>" via Moonraker's
    // file download endpoint. The cache's own fetch cannot be used here: it goes
    // through download_thumbnail, which prefixes ".thumbnails/". For timelapse
    // companions we need a direct download approach.

    // Download companion to a temp location, then save to cache and pre-scale
    std::string dest_path = app_get_runtime_dir() + "/helix_timelapse_thumb_" + companion;
    auto tok = lifetime_.token();
    auto thumb_tok = thumb_lifetime_.token();

    api_->transfers().download_file_to_path(
        "timelapse", companion, dest_path,
        [this, tok, thumb_tok, dest_path, cache_key, target,
         filename_copy](const std::string& /*path*/) {
            if (tok.expired() || thumb_tok.expired())
                return;

            // Read the downloaded file into memory for ThumbnailProcessor
            FILE* f = fopen(dest_path.c_str(), "rb");
            if (!f) {
                spdlog::warn("[Timelapse Videos] Failed to open downloaded thumbnail: {}",
                             dest_path);
                return;
            }

            fseek(f, 0, SEEK_END);
            auto fsize = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
                fclose(f);
                unlink(dest_path.c_str());
                return;
            }

            std::vector<uint8_t> data(static_cast<size_t>(fsize));
            size_t read = fread(data.data(), 1, data.size(), f);
            fclose(f);
            unlink(dest_path.c_str());

            if (read != data.size())
                return;

            // Pre-scale via ThumbnailProcessor (runs on worker thread)
            helix::ThumbnailProcessor::instance().process_async(
                data, cache_key, target,
                [this, tok, thumb_tok, filename_copy](const std::string& lvbin_path) {
                    if (tok.expired() || thumb_tok.expired())
                        return;

                    thumb_tok.defer([this, filename_copy, lvbin_path]() {
                        if (!video_grid_container_)
                            return;

                        // Find the card for this filename by scanning children
                        uint32_t count = lv_obj_get_child_count(video_grid_container_);
                        for (uint32_t i = 0; i < count; i++) {
                            lv_obj_t* child =
                                lv_obj_get_child(video_grid_container_, static_cast<int32_t>(i));
                            const char* stored_name = helix::ui::get_owned_user_string(child);
                            if (stored_name && filename_copy == stored_name) {
                                lv_obj_t* thumb = lv_obj_find_by_name(child, "thumbnail");
                                lv_obj_t* icon = lv_obj_find_by_name(child, "no_thumbnail_icon");
                                if (thumb) {
                                    lv_image_set_src(thumb, lvbin_path.c_str());
                                    lv_obj_remove_flag(thumb, LV_OBJ_FLAG_HIDDEN);
                                }
                                if (icon) {
                                    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                                }
                                spdlog::debug("[Timelapse Videos] Thumbnail loaded for '{}'",
                                              filename_copy);
                                break;
                            }
                        }
                    });
                },
                [filename_copy](const std::string& error) {
                    spdlog::warn("[Timelapse Videos] Failed to process thumbnail for '{}': {}",
                                 filename_copy, error);
                });
        },
        [companion](const MoonrakerError& error) {
            spdlog::debug("[Timelapse Videos] Failed to download companion '{}': {}", companion,
                          error.message);
        });
}

void TimelapseVideosOverlay::clear_video_grid() {
    if (video_grid_container_) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::safe_clean_children(video_grid_container_);
    }
    videos_.clear();
}

// ============================================================================
// PLAYBACK CAPABILITY
// ============================================================================

void TimelapseVideosOverlay::detect_playback_capability() {
    // Cache detection results: player availability doesn't change at runtime
    static bool detected = false;
    static bool cached_can_play = false;
    static std::string cached_player;

    if (!detected) {
        detected = true;

        // Check for available players (only once per process)
        FILE* pipe = popen("which mpv 2>/dev/null", "r");
        if (pipe) {
            char buf[256];
            if (fgets(buf, sizeof(buf), pipe) != nullptr) {
                cached_player = "mpv";
                cached_can_play = true;
            }
            pclose(pipe);
        }

        if (!cached_can_play) {
            pipe = popen("which ffplay 2>/dev/null", "r");
            if (pipe) {
                char buf[256];
                if (fgets(buf, sizeof(buf), pipe) != nullptr) {
                    cached_player = "ffplay";
                    cached_can_play = true;
                }
                pclose(pipe);
            }
        }
    }

    can_play_ = cached_can_play;
    player_command_ = cached_player;

    // Check if we're running on the same host as Moonraker (may change between
    // connections). Both halves are shared: extract_host_from_websocket_url()
    // already handles the [::1] bracket form this parse got wrong, and
    // is_moonraker_on_same_host() answers for a host named by hostname or LAN IP,
    // not just a loopback literal — playback works whenever the file is on this
    // machine, however the host happens to be spelled.
    if (api_) {
        const std::string host = helix::extract_host_from_websocket_url(api_->get_websocket_url());
        // Empty means we could not name the host at all; that is not evidence of
        // locality, and is_moonraker_on_same_host() reads "" as the localhost
        // default.
        is_local_moonraker_ = !host.empty() && helix::is_moonraker_on_same_host(host);
    }

    spdlog::debug("[{}] Playback capability: can_play={} player='{}' local={}", get_name(),
                  can_play_, player_command_, is_local_moonraker_);
}

// ============================================================================
// VIDEO PLAYBACK
// ============================================================================

/// Launch a child process via double-fork to prevent zombie processes.
/// The grandchild is adopted by init so no waitpid is needed long-term.
static void spawn_detached(const std::vector<std::string>& args) {
    if (args.empty())
        return;

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("[Timelapse Videos] fork() failed for video playback");
        return;
    }
    if (pid == 0) {
        // First child: fork again and exit immediately
        pid_t pid2 = fork();
        if (pid2 < 0)
            _exit(127);
        if (pid2 > 0)
            _exit(0); // First child exits; grandchild continues

        // Grandchild: exec the player
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    // Parent: reap the first child (exits immediately)
    int status = 0;
    waitpid(pid, &status, 0);
}

void TimelapseVideosOverlay::play_video(const std::string& filename) {
    if (!can_play_ || player_command_.empty()) {
        spdlog::warn("[{}] No video player available", get_name());
        return;
    }

    if (is_local_moonraker_) {
        // Local: construct path directly
        std::string home = getenv("HOME") ? getenv("HOME") : "/root";
        std::string path = home + "/" + TIMELAPSE_DATA_SUBPATH + filename;
        auto args = helix::timelapse::build_player_args(player_command_, path);
        spdlog::info("[{}] Playing local video: {} {}", get_name(), args[0], path);
        spawn_detached(args);
    } else {
        // Remote: download to /tmp then play
        if (!api_)
            return;

        // Ensure temp directory exists
        std::string timelapse_dir = app_get_runtime_dir() + "/helix_timelapse";
        mkdir(timelapse_dir.c_str(), 0755);

        auto tok = lifetime_.token();
        std::string player = player_command_;

        spdlog::info("[{}] Downloading remote video '{}' for playback", get_name(), filename);

        std::string dest_path = timelapse_dir + "/" + filename;

        api_->transfers().download_file_to_path(
            "timelapse", filename, dest_path,
            [this, tok, dest_path, player](const std::string& /*path*/) {
                if (tok.expired())
                    return;
                tok.defer([this, dest_path, player]() {
                    auto args = helix::timelapse::build_player_args(player, dest_path);
                    spdlog::info("[{}] Playing downloaded video: {} {}", get_name(), args[0],
                                 dest_path);
                    spawn_detached(args);
                });
            },
            [](const MoonrakerError& error) {
                spdlog::error("[Timelapse Videos] Failed to download video: {}", error.message);
            });
    }
}

// ============================================================================
// DELETE CONFIRMATION
// ============================================================================

void TimelapseVideosOverlay::confirm_delete(const std::string& filename) {
    pending_delete_filename_ = filename;

    std::string message = fmt::format(lv_tr("Delete {}?"), filename);

    delete_confirmation_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("Delete Video"), message.c_str(), ModalSeverity::Warning, lv_tr("Delete"),
        on_delete_confirmed, on_delete_cancelled, this);
}

void TimelapseVideosOverlay::on_delete_confirmed(lv_event_t* e) {
    auto* self = static_cast<TimelapseVideosOverlay*>(lv_event_get_user_data(e));
    if (!self || !g_timelapse_videos)
        return;

    // Hide the dialog
    if (self->delete_confirmation_dialog_) {
        helix::ui::modal_hide(self->delete_confirmation_dialog_);
        self->delete_confirmation_dialog_ = nullptr;
    }

    if (!self->api_ || self->pending_delete_filename_.empty())
        return;

    std::string full_path = "timelapse/" + self->pending_delete_filename_;
    auto tok = self->lifetime_.token();

    spdlog::info("[Timelapse Videos] Deleting video: {}", self->pending_delete_filename_);

    self->api_->files().delete_file(
        full_path,
        [self, tok]() {
            if (tok.expired())
                return;
            tok.defer([self]() {
                spdlog::info("[Timelapse Videos] Video deleted, refreshing list");
                self->fetch_video_list();
            });
        },
        [](const MoonrakerError& error) {
            spdlog::error("[Timelapse Videos] Failed to delete video: {}", error.message);
        });

    self->pending_delete_filename_.clear();
}

void TimelapseVideosOverlay::on_delete_cancelled(lv_event_t* e) {
    auto* self = static_cast<TimelapseVideosOverlay*>(lv_event_get_user_data(e));
    if (!self)
        return;

    if (self->delete_confirmation_dialog_) {
        helix::ui::modal_hide(self->delete_confirmation_dialog_);
        self->delete_confirmation_dialog_ = nullptr;
    }
    self->pending_delete_filename_.clear();
}

// ============================================================================
// STATIC EVENT HANDLERS
// ============================================================================

void TimelapseVideosOverlay::on_render_now(lv_event_t* /*e*/) {
    if (!g_timelapse_videos || !g_timelapse_videos->api_) {
        spdlog::warn("[Timelapse Videos] Render requested but no API available");
        return;
    }

    spdlog::info("[Timelapse Videos] Rendering timelapse...");
    g_timelapse_videos->api_->timelapse().render_timelapse(
        []() { spdlog::info("[Timelapse Videos] Render started successfully"); },
        [](const MoonrakerError& error) {
            spdlog::error("[Timelapse Videos] Render failed: {}", error.message);
        });
}

void TimelapseVideosOverlay::on_card_clicked(lv_event_t* e) {
    auto* self = static_cast<TimelapseVideosOverlay*>(lv_event_get_user_data(e));
    if (!self)
        return;

    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* filename = helix::ui::get_owned_user_string(target);
    if (!filename)
        return;

    spdlog::debug("[Timelapse Videos] Card clicked: {}", filename);

    if (self->can_play_) {
        self->play_video(filename);
    }
}

void TimelapseVideosOverlay::on_card_long_pressed(lv_event_t* e) {
    auto* self = static_cast<TimelapseVideosOverlay*>(lv_event_get_user_data(e));
    if (!self)
        return;

    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* filename = helix::ui::get_owned_user_string(target);
    if (!filename)
        return;

    spdlog::debug("[Timelapse Videos] Card long-pressed: {}", filename);
    self->confirm_delete(filename);
}

#else // !HELIX_HAS_TIMELAPSE_VIEWER

// Compiled-out build (HELIX_HAS_TIMELAPSE_VIEWER=0): no video list/download/
// playback UI on this target — capture-control (settings, render, save-frames)
// stays available via ITimelapseAPI, only the viewing overlay is stubbed.
// Player-process spawning (fork/exec) and HTTP video transfer code are absent.

#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

static std::unique_ptr<TimelapseVideosOverlay> g_timelapse_videos_stub;

TimelapseVideosOverlay& get_global_timelapse_videos() {
    return *g_timelapse_videos_stub;
}

void init_global_timelapse_videos(IMoonrakerAPI* api) {
    if (g_timelapse_videos_stub) {
        return;
    }
    g_timelapse_videos_stub = std::make_unique<TimelapseVideosOverlay>(api);
    StaticPanelRegistry::instance().register_destroy("TimelapseVideosOverlay",
                                                     []() { g_timelapse_videos_stub.reset(); });
    spdlog::debug("[Timelapse Videos] Compiled out (HELIX_HAS_TIMELAPSE_VIEWER=0)");
}

void open_timelapse_videos() {
    spdlog::debug("[Timelapse Videos] Compiled out (HELIX_HAS_TIMELAPSE_VIEWER=0); ignoring");
}

TimelapseVideosOverlay::TimelapseVideosOverlay(IMoonrakerAPI* api) : api_(api) {}

void TimelapseVideosOverlay::init_subjects() {}

lv_obj_t* TimelapseVideosOverlay::create(lv_obj_t*) {
    return nullptr;
}

void TimelapseVideosOverlay::on_activate() {
    OverlayBase::on_activate();
}

void TimelapseVideosOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

void TimelapseVideosOverlay::cleanup() {
    OverlayBase::cleanup();
}

#endif // HELIX_HAS_TIMELAPSE_VIEWER
