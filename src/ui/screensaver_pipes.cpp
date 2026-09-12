// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes.h"

#include "ui_timer_guard.h" // lv_timer_cancel_safe
#include "ui_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

static constexpr uint32_t TICK_PERIOD_MS = 100;
static constexpr float PIPE_RADIUS = 0.22f; // Reference uses 0.2
static constexpr float JOINT_RADIUS =
    PIPE_RADIUS * 1.5f;                      // Reference: ballJointRadius = pipeRadius * 1.5
static constexpr float FOV_DEGREES = 45.0f;  // Reference: PerspectiveCamera(45, ...)
static constexpr float CAM_DISTANCE = 28.0f; // Reference: camera at distance 14 from ±10 grid
static constexpr float PI_F = 3.14159265f;

// ---------- Vector math helpers ----------

static float vec3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vec3_cross(float out[3], const float a[3], const float b[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void vec3_normalize(float v[3]) {
    float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0.0001f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

// ---------- Color palette ----------

static constexpr int NUM_PIPE_COLORS = 10;

static lv_color_t get_pipe_color(int index) {
    static const uint8_t palette[][3] = {
        {230, 50, 50},  // red
        {50, 200, 50},  // green
        {70, 70, 240},  // blue
        {230, 220, 40}, // yellow
        {40, 210, 210}, // cyan
        {210, 50, 210}, // magenta
        {240, 140, 20}, // orange
        {140, 230, 20}, // lime
        {30, 140, 240}, // sky blue
        {240, 40, 140}, // hot pink
    };
    int i = index % NUM_PIPE_COLORS;
    return lv_color_make(palette[i][0], palette[i][1], palette[i][2]);
}

// ---------- Camera ----------

void PipesScreensaver::setup_camera() {
    // Randomize camera angle each reset (reference: 50% head-on, 50% random rotation)
    float angle = (static_cast<float>(rand()) / RAND_MAX) * 2.0f * PI_F;
    float elevation = 0.3f + (static_cast<float>(rand()) / RAND_MAX) * 0.4f;

    cam_pos_[0] = CAM_DISTANCE * std::cos(elevation) * std::cos(angle);
    cam_pos_[1] = CAM_DISTANCE * std::sin(elevation);
    cam_pos_[2] = CAM_DISTANCE * std::cos(elevation) * std::sin(angle);

    // Forward = normalize(origin - camera_pos)
    cam_fwd_[0] = -cam_pos_[0];
    cam_fwd_[1] = -cam_pos_[1];
    cam_fwd_[2] = -cam_pos_[2];
    vec3_normalize(cam_fwd_);

    // Right = normalize(cross(forward, world_up))
    float world_up[3] = {0, 1, 0};
    vec3_cross(cam_right_, cam_fwd_, world_up);
    vec3_normalize(cam_right_);

    // Up = cross(right, forward)
    vec3_cross(cam_up_, cam_right_, cam_fwd_);
    vec3_normalize(cam_up_);

    // Focal length from FOV
    focal_ = static_cast<float>(std::min(screen_w_, screen_h_)) /
             (2.0f * std::tan(FOV_DEGREES * 0.5f * PI_F / 180.0f));
}

bool PipesScreensaver::project(float wx, float wy, float wz, int& sx, int& sy, float& depth) const {
    float d[3] = {wx - cam_pos_[0], wy - cam_pos_[1], wz - cam_pos_[2]};

    float vz = vec3_dot(d, cam_fwd_);
    if (vz < 0.1f)
        return false; // Behind camera

    float vx = vec3_dot(d, cam_right_);
    float vy = vec3_dot(d, cam_up_);

    sx = screen_w_ / 2 + static_cast<int>(vx * focal_ / vz);
    sy = screen_h_ / 2 - static_cast<int>(vy * focal_ / vz);
    depth = vz;

    return true;
}

// ---------- Lifecycle ----------

void PipesScreensaver::start() {
    if (active_) {
        spdlog::debug("[Screensaver] Pipes already active, ignoring start()");
        return;
    }

    spdlog::info("[Screensaver] Starting 3D pipes");

    lv_display_t* disp = lv_display_get_default();
    if (!disp) {
        spdlog::warn("[Screensaver] No display available, cannot start pipes");
        return;
    }

    screen_w_ = lv_display_get_horizontal_resolution(disp);
    screen_h_ = lv_display_get_vertical_resolution(disp);

    // Create black overlay on lv_layer_top() — absorbs touch input
    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // Create canvas as child of overlay
    canvas_ = lv_canvas_create(overlay_);
    lv_obj_set_size(canvas_, screen_w_, screen_h_);
    lv_obj_set_pos(canvas_, 0, 0);

    // Allocate ARGB8888 draw buffer at LVGL's row stride: lv_canvas_set_buffer()
    // steps rows by the aligned stride and lv_canvas_fill_bg() writes the full
    // extent immediately, so a tightly-packed w * h * 4 allocation under-runs it.
    size_t buf_size =
        static_cast<size_t>(helix::ui::screensaver_canvas_stride_bytes(screen_w_)) * screen_h_;
    draw_buf_ = static_cast<uint8_t*>(lv_malloc(buf_size));
    if (!draw_buf_) {
        spdlog::error("[Screensaver] Failed to allocate {}KB draw buffer for pipes",
                      buf_size / 1024);
        helix::ui::safe_delete_deferred(overlay_);
        canvas_ = nullptr; // deleted as child of overlay
        return;
    }
    draw_buf_size_ = buf_size;

    lv_canvas_set_buffer(canvas_, draw_buf_, screen_w_, screen_h_, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);

    // Seed RNG, setup camera, initialize grid
    srand(static_cast<unsigned>(time(nullptr)));
    setup_camera();
    reset_grid();

    // Start 2 pipes initially (reference starts 1-3)
    color_index_ = 0;
    for (int i = 0; i < 2; i++) {
        start_new_pipe(pipes_[i]);
    }

    // Create tick timer
    timer_ = lv_timer_create(tick_timer_cb, TICK_PERIOD_MS, this);

    active_ = true;
    spdlog::debug("[Screensaver] Pipes started ({}x{}, perspective camera)", screen_w_, screen_h_);
}

PipesScreensaver::~PipesScreensaver() {
    // ScreensaverManager owns these in a unique_ptr and does not stop the active
    // one before destroying it, so a screensaver torn down while running would
    // otherwise leave tick_timer_cb armed on a freed `this`. lv_timer_cancel_safe()
    // self-guards on lv_is_initialized() and neuters rather than unlinking, which
    // is what makes it safe from a destructor and after lv_deinit has already
    // reclaimed the timer (#750, #751, #1173).
    cancel_timer();
}

void PipesScreensaver::cancel_timer() {
    if (timer_) {
        helix::ui::lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }
}

void PipesScreensaver::stop() {
    if (!active_) {
        return;
    }

    spdlog::info("[Screensaver] Stopping 3D pipes");

    cancel_timer();

    // Free draw buffer BEFORE deleting overlay — the canvas (child of overlay)
    // may access the buffer during deletion
    if (draw_buf_) {
        lv_free(draw_buf_);
        draw_buf_ = nullptr;
    }

    // Async delete — stop() runs from the main loop's wake path which can be
    // adjacent to lv_timer_handler ticks, and synchronous deletion of an
    // overlay carrying child widgets risks corrupting LVGL's event linked
    // list (#316). Matches FlyingToaster and Starfield screensavers, which
    // both use safe_delete_deferred() for the same reason.
    if (overlay_) {
        helix::ui::safe_delete_deferred(overlay_);
        canvas_ = nullptr; // deleted as child of overlay
    }

    for (auto& p : pipes_)
        p.alive = false;
    active_ = false;
}

// ---------- Grid ----------

void PipesScreensaver::reset_grid() {
    std::memset(grid_, 0, sizeof(grid_));
    total_segments_ = 0;
    color_index_ = 0;
}

void PipesScreensaver::start_new_pipe(ActivePipe& pipe) {
    // Find a random empty cell (like reference: randomIntegerVector3WithinBox)
    for (int attempt = 0; attempt < 100; attempt++) {
        GridPos pos;
        pos.x = rand() % GRID_DIM;
        pos.y = rand() % GRID_DIM;
        pos.z = rand() % GRID_DIM;

        if (!grid_[pos.x][pos.y][pos.z]) {
            pipe.pos = pos;
            grid_[pos.x][pos.y][pos.z] = true;
            pipe.dir = static_cast<Direction>(rand() % 6);
            pipe.color = get_pipe_color(color_index_++);
            pipe.shadow_color = lv_color_mix(pipe.color, lv_color_black(), LV_OPA_50);
            pipe.highlight_color = lv_color_mix(pipe.color, lv_color_white(), LV_OPA_40);
            pipe.segment_count = 0;
            pipe.alive = true;
            pipe.has_prev_dir = false;

            // Draw initial ball joint (reference: makeBallJoint at start position)
            float wx = static_cast<float>(pos.x - GRID_OFFSET);
            float wy = static_cast<float>(pos.y - GRID_OFFSET);
            float wz = static_cast<float>(pos.z - GRID_OFFSET);
            int sx, sy;
            float depth;
            if (project(wx, wy, wz, sx, sy, depth)) {
                lv_layer_t layer;
                lv_canvas_init_layer(canvas_, &layer);
                draw_joint(&layer, sx, sy, depth, pipe);
                lv_canvas_finish_layer(canvas_, &layer);
            }
            return;
        }
    }
    pipe.alive = false;
}

// ---------- Tick ----------

void PipesScreensaver::tick_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<PipesScreensaver*>(lv_timer_get_user_data(timer));
    if (!self || !self->active_)
        return;
    self->tick();
}

void PipesScreensaver::tick() {
    if (!canvas_)
        return;

    // Reset when grid is full
    if (total_segments_ > MAX_SEGMENTS) {
        lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
        reset_grid();
        setup_camera(); // New random camera angle on reset
        for (auto& p : pipes_)
            p.alive = false;
        for (int i = 0; i < 2; i++) {
            start_new_pipe(pipes_[i]);
        }
        return;
    }

    // Single layer session for all pipe growth this tick
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);

    // Grow each active pipe
    int alive_count = 0;
    for (auto& pipe : pipes_) {
        if (!pipe.alive)
            continue;
        alive_count++;

        if (!grow_pipe(pipe, &layer)) {
            pipe.alive = false;
        }
    }

    lv_canvas_finish_layer(canvas_, &layer);

    // Start new pipes when old ones die (reference: multiple pipes simultaneously)
    if (alive_count < MAX_ACTIVE_PIPES) {
        for (auto& pipe : pipes_) {
            if (!pipe.alive) {
                start_new_pipe(pipe);
                break;
            }
        }
    }
}

// ---------- Growth ----------

bool PipesScreensaver::grow_pipe(ActivePipe& pipe, lv_layer_t* layer) {
    Direction try_dir = pipe.dir;

    // Reference: chance(1/2) && lastDirectionVector ? continue straight : random direction
    if (pipe.has_prev_dir && rand() % 2 == 0) {
        // Continue straight (50%)
        try_dir = pipe.dir;
    } else {
        // Random direction — pick random axis + sign (reference: chooseFrom("xyz") + [+1,-1])
        try_dir = static_cast<Direction>(rand() % 6);
    }

    // Try the chosen direction
    GridPos np = next_pos(pipe.pos, try_dir);
    if (in_bounds(np) && !grid_[np.x][np.y][np.z]) {
        grid_[np.x][np.y][np.z] = true;

        float wx1 = static_cast<float>(pipe.pos.x - GRID_OFFSET);
        float wy1 = static_cast<float>(pipe.pos.y - GRID_OFFSET);
        float wz1 = static_cast<float>(pipe.pos.z - GRID_OFFSET);
        float wx2 = static_cast<float>(np.x - GRID_OFFSET);
        float wy2 = static_cast<float>(np.y - GRID_OFFSET);
        float wz2 = static_cast<float>(np.z - GRID_OFFSET);

        int sx1, sy1, sx2, sy2;
        float d1, d2;

        if (project(wx1, wy1, wz1, sx1, sy1, d1) && project(wx2, wy2, wz2, sx2, sy2, d2)) {
            // Ball joint at direction change (reference: makeBallJoint)
            if (pipe.has_prev_dir && try_dir != pipe.dir) {
                draw_joint(layer, sx1, sy1, d1, pipe);
            }

            draw_segment(layer, sx1, sy1, sx2, sy2, (d1 + d2) * 0.5f, pipe);
        }

        pipe.dir = try_dir;
        pipe.has_prev_dir = true;
        pipe.pos = np;
        pipe.segment_count++;
        total_segments_++;
        return true;
    }

    // Fallback: try all 6 directions (reference just returns, but we're at lower tick rate)
    for (int d = 0; d < 6; d++) {
        auto candidate = static_cast<Direction>(d);
        GridPos cnp = next_pos(pipe.pos, candidate);
        if (in_bounds(cnp) && !grid_[cnp.x][cnp.y][cnp.z]) {
            grid_[cnp.x][cnp.y][cnp.z] = true;

            float wx1 = static_cast<float>(pipe.pos.x - GRID_OFFSET);
            float wy1 = static_cast<float>(pipe.pos.y - GRID_OFFSET);
            float wz1 = static_cast<float>(pipe.pos.z - GRID_OFFSET);
            float wx2 = static_cast<float>(cnp.x - GRID_OFFSET);
            float wy2 = static_cast<float>(cnp.y - GRID_OFFSET);
            float wz2 = static_cast<float>(cnp.z - GRID_OFFSET);

            int sx1, sy1, sx2, sy2;
            float depth1, depth2;

            if (project(wx1, wy1, wz1, sx1, sy1, depth1) &&
                project(wx2, wy2, wz2, sx2, sy2, depth2)) {
                if (pipe.has_prev_dir && candidate != pipe.dir) {
                    draw_joint(layer, sx1, sy1, depth1, pipe);
                }

                draw_segment(layer, sx1, sy1, sx2, sy2, (depth1 + depth2) * 0.5f, pipe);
            }

            pipe.dir = candidate;
            pipe.has_prev_dir = true;
            pipe.pos = cnp;
            pipe.segment_count++;
            total_segments_++;
            return true;
        }
    }

    return false;
}

// ---------- Drawing ----------

void PipesScreensaver::draw_segment(lv_layer_t* layer, int sx1, int sy1, int sx2, int sy2,
                                    float depth, const ActivePipe& pipe) {
    // Pipe thickness scales with depth (perspective foreshortening)
    float projected_radius = PIPE_RADIUS * focal_ / depth;
    int thickness = std::max(3, static_cast<int>(projected_radius * 2.0f));

    // 3-layer cylinder shading using thick lines that follow the actual segment direction.
    // Square ends (no rounding) so consecutive straight segments connect seamlessly.
    int inset1 = std::max(1, thickness / 5);
    int inset2 = std::max(2, thickness / 3);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 0;
    dsc.round_end = 0;
    dsc.p1.x = sx1;
    dsc.p1.y = sy1;
    dsc.p2.x = sx2;
    dsc.p2.y = sy2;

    // Shadow (outer) — full thickness
    dsc.color = pipe.shadow_color;
    dsc.width = thickness;
    lv_draw_line(layer, &dsc);

    // Base color (middle)
    dsc.color = pipe.color;
    dsc.width = std::max(1, thickness - 2 * inset1);
    lv_draw_line(layer, &dsc);

    // Highlight strip (inner)
    dsc.color = pipe.highlight_color;
    dsc.width = std::max(1, thickness - 2 * inset2);
    lv_draw_line(layer, &dsc);
}

void PipesScreensaver::draw_joint(lv_layer_t* layer, int sx, int sy, float depth,
                                  const ActivePipe& pipe) {
    // Ball joint radius scales with depth (reference: SphereGeometry(ballJointRadius, 8, 8))
    float projected_radius = JOINT_RADIUS * focal_ / depth;
    int ball_r = std::max(3, static_cast<int>(projected_radius));

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;

    lv_area_t area;

    // Outer shadow sphere
    dsc.bg_color = pipe.shadow_color;
    dsc.radius = ball_r;
    area.x1 = sx - ball_r;
    area.y1 = sy - ball_r;
    area.x2 = sx + ball_r;
    area.y2 = sy + ball_r;
    lv_draw_rect(layer, &dsc, &area);

    // Middle base color
    int mid_r = ball_r * 3 / 4;
    dsc.bg_color = pipe.color;
    dsc.radius = mid_r;
    area.x1 = sx - mid_r;
    area.y1 = sy - mid_r;
    area.x2 = sx + mid_r;
    area.y2 = sy + mid_r;
    lv_draw_rect(layer, &dsc, &area);

    // Highlight spot (offset upper-left for 3D specular effect)
    int hi_r = std::max(2, ball_r / 3);
    int offset = ball_r / 4;
    dsc.bg_color = pipe.highlight_color;
    dsc.radius = hi_r;
    area.x1 = sx - offset - hi_r;
    area.y1 = sy - offset - hi_r;
    area.x2 = sx - offset + hi_r;
    area.y2 = sy - offset + hi_r;
    lv_draw_rect(layer, &dsc, &area);
}

// ---------- Helpers ----------

PipesScreensaver::GridPos PipesScreensaver::next_pos(GridPos pos, Direction dir) const {
    switch (dir) {
    case Direction::POS_X:
        pos.x += 1;
        break;
    case Direction::NEG_X:
        pos.x -= 1;
        break;
    case Direction::POS_Y:
        pos.y += 1;
        break;
    case Direction::NEG_Y:
        pos.y -= 1;
        break;
    case Direction::POS_Z:
        pos.z += 1;
        break;
    case Direction::NEG_Z:
        pos.z -= 1;
        break;
    }
    return pos;
}

bool PipesScreensaver::in_bounds(GridPos pos) const {
    return pos.x >= 0 && pos.x < GRID_DIM && pos.y >= 0 && pos.y < GRID_DIM && pos.z >= 0 &&
           pos.z < GRID_DIM;
}

#endif // HELIX_ENABLE_SCREENSAVER
