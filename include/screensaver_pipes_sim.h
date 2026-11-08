// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_buffer.h"
#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_motion.h"
#include "screensaver_vec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_pipes_sim.h
 * @brief The 3D Pipes: node grid, pipe growth, a z-buffered software rasterizer, and the dissolve
 *
 * No LVGL, so every part steps and renders without a display. World units follow the original
 * NT OpenGL sample: nodes 7 apart, pipe radius 1, camera 75 back with a 90 degree vertical FOV.
 */

namespace helix::ui {

/// One rung of the pipes ladder.
struct PipesLevel {
    uint32_t period_ms;
    /// Rasterizes at (canvas / scale); each internal pixel fills scale x scale canvas pixels.
    uint8_t scale;
};

/// The ladder, most expensive level first. Rasterizing is a small part of a frame's cost, so
/// the last rung saves mostly by its longer period; it also quarters the raster work.
inline constexpr PipesLevel kPipesLevels[] = {
    {kSaverFastPeriod, 1},
    {33, 1},
    {100, 2},
};
inline constexpr size_t kPipesLevelCount = std::size(kPipesLevels);

inline constexpr int kPipesNodesLong = 15;     ///< nodes along the long screen axis (NUM_DIV - 1)
inline constexpr float kPipesPitch = 7.0f;     ///< node spacing (divSize)
inline constexpr float kPipesRadius = 1.0f;    ///< pipe radius: 1/7 of the pitch
inline constexpr float kPipesViewDist = 75.0f; ///< camera distance from the grid centre (-zTrans)
inline constexpr float kPipesYrotStepDeg = 9.73156f; ///< view turn per new screen
inline constexpr int kPipesSlices = 16;              ///< facets round a pipe; a multiple of 4
inline constexpr uint32_t kPipesStepMs = 100;        ///< one growth step per pipe
inline constexpr uint32_t kPipesMaxStepsPerTick = 3;
/// Growth steps a call while a print screen catches up to a print it opened on, paused or
/// ended, so the screen fills over a few dozen frames instead of in one.
inline constexpr uint32_t kPipesFastForwardSteps = 60;
inline constexpr int kPipesActive =
    1; ///< pipes growing at once: the original's single-pipe default
inline constexpr int kPipesPerScreen = 5; ///< NORMAL_PIPE_COUNT
inline constexpr int kPipesMaxWeightStraight = 100;
inline constexpr int kPipesTeapotOdds = 1000;
inline constexpr int kPipesTurnLookahead = 2;
inline constexpr uint32_t kPipesDissolveMs = 2000;
inline constexpr int kPipesDissolveDiv = 32; ///< dissolve block = min(w, h) / this
/// The finished print's teapot: emerald, the green a finished print reads in everywhere.
inline constexpr uint8_t kPipesDoneTeapotMaterial = 0;

enum class PipeDir : uint8_t { POS_X, NEG_X, POS_Y, NEG_Y, POS_Z, NEG_Z, NONE };
inline constexpr int kPipeDirCount = 6;

enum class PipeJoint : uint8_t { ELBOW, BALL, TEAPOT };

struct PipeNode {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const PipeNode& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct PipeGridDims {
    int x = 0;
    int y = 0;
    int z = 0;
};

/// kPipesNodesLong along the long screen axis, the other by aspect (at least 1), depth equal
/// to the long axis: 800x480 gives 15 x 9 x 15, 480x800 gives 9 x 15 x 15.
PipeGridDims pipes_grid_dims(int32_t w, int32_t h);

PipeNode pipe_step(PipeNode n, PipeDir d);
PipeDir pipe_opposite(PipeDir d);

/// Which nodes are taken.
class PipesGrid {
  public:
    void reset(PipeGridDims dims);
    void release();
    PipeGridDims dims() const {
        return dims_;
    }
    bool in_bounds(PipeNode n) const;
    /// In bounds and not taken.
    bool empty(PipeNode n) const;
    void take(PipeNode n);
    size_t empty_count() const {
        return empty_count_;
    }
    /// Contiguous empty nodes from `from` (exclusive) along `d`, at most `limit`.
    int empty_run(PipeNode from, PipeDir d, int limit) const;
    /// A random empty node, or false when none is left (random tries, then a scan).
    bool random_empty(std::minstd_rand& rng, PipeNode& out) const;
    size_t heap_bytes() const {
        return buffer_bytes(taken_);
    }

  private:
    size_t index(PipeNode n) const;

    PipeGridDims dims_;
    size_t empty_count_ = 0;
    std::vector<uint8_t> taken_;
};

/// A pipe's straight weight: 1 + rand(4), or 1 time in 20 rand in [25, 100].
int pipes_choose_weight_straight(std::minstd_rand& rng);

/// A turn's joint: a teapot 1 time in kPipesTeapotOdds, else a ball 1 time in 3, else an elbow.
PipeJoint pipes_choose_joint(std::minstd_rand& rng);

/**
 * @brief Where a pipe at `pos`, last heading `last`, goes next; NONE when every neighbour is taken
 *
 * Straight (when empty) carries `weight_straight`, each empty other neighbour 1; weight
 * kPipesMaxWeightStraight always goes straight. A turn goes to whichever empty turn has
 * the longest empty run up to kPipesTurnLookahead, ties broken at random. Marks nothing.
 */
PipeDir pipes_choose_direction(const PipesGrid& grid, PipeNode pos, PipeDir last,
                               int weight_straight, std::minstd_rand& rng);

using PipeVec3 = SaverVec3;

/// An OpenGL fixed-function material: ambient, diffuse, specular reflectance and the GL
/// shininess exponent (the sample's specExp x 128).
struct PipeMaterial {
    float ka[3];
    float kd[3];
    float ks[3];
    float shininess;
};

/// The sample's 16 "good" teapot materials (COMMON/MATERIAL.C), in its goodMaterials order.
inline constexpr PipeMaterial kPipeMaterials[] = {
    {{0.0215f, 0.1745f, 0.0215f},
     {0.07568f, 0.61424f, 0.07568f},
     {0.633f, 0.727811f, 0.633f},
     0.6f * 128.0f}, // emerald
    {{0.135f, 0.2225f, 0.1575f},
     {0.54f, 0.89f, 0.63f},
     {0.316228f, 0.316228f, 0.316228f},
     0.1f * 128.0f}, // jade
    {{0.25f, 0.20725f, 0.20725f},
     {1.0f, 0.829f, 0.829f},
     {0.296648f, 0.296648f, 0.296648f},
     0.088f * 128.0f}, // pearl
    {{0.1745f, 0.01175f, 0.01175f},
     {0.61424f, 0.04136f, 0.04136f},
     {0.727811f, 0.626959f, 0.626959f},
     0.6f * 128.0f}, // ruby
    {{0.1f, 0.18725f, 0.1745f},
     {0.396f, 0.74151f, 0.69102f},
     {0.297254f, 0.30829f, 0.306678f},
     0.1f * 128.0f}, // turquoise
    {{0.329412f, 0.223529f, 0.027451f},
     {0.780392f, 0.568627f, 0.113725f},
     {0.992157f, 0.941176f, 0.807843f},
     0.21794872f * 128.0f}, // brass
    {{0.2125f, 0.1275f, 0.054f},
     {0.714f, 0.4284f, 0.18144f},
     {0.393548f, 0.271906f, 0.166721f},
     0.2f * 128.0f}, // bronze
    {{0.19125f, 0.0735f, 0.0225f},
     {0.7038f, 0.27048f, 0.0828f},
     {0.256777f, 0.137622f, 0.086014f},
     0.1f * 128.0f}, // copper
    {{0.24725f, 0.1995f, 0.0745f},
     {0.75164f, 0.60648f, 0.22648f},
     {0.628281f, 0.555802f, 0.366065f},
     0.4f * 128.0f}, // gold
    {{0.19225f, 0.19225f, 0.19225f},
     {0.50754f, 0.50754f, 0.50754f},
     {0.508273f, 0.508273f, 0.508273f},
     0.4f * 128.0f}, // silver
    {{0.0f, 0.1f, 0.06f},
     {0.0f, 0.50980392f, 0.50980392f},
     {0.50196078f, 0.50196078f, 0.50196078f},
     0.25f * 128.0f}, // cyan plastic
    {{0.0f, 0.0f, 0.0f},
     {0.55f, 0.55f, 0.55f},
     {0.7f, 0.7f, 0.7f},
     0.25f * 128.0f},                                                             // white plastic
    {{0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f}, {0.6f, 0.6f, 0.5f}, 0.25f * 128.0f}, // yellow plastic
    {{0.0f, 0.05f, 0.05f},
     {0.4f, 0.5f, 0.5f},
     {0.04f, 0.7f, 0.7f},
     0.078125f * 128.0f}, // cyan rubber
    {{0.0f, 0.05f, 0.0f},
     {0.4f, 0.5f, 0.4f},
     {0.04f, 0.7f, 0.04f},
     0.078125f * 128.0f}, // green rubber
    {{0.05f, 0.05f, 0.05f},
     {0.5f, 0.5f, 0.5f},
     {0.7f, 0.7f, 0.7f},
     0.078125f * 128.0f}, // white rubber
};
inline constexpr size_t kPipeMaterialCount = std::size(kPipeMaterials);

/// Eye-space light direction, normalize(90, 90, 150): fixed to the camera, so the highlight
/// sits in the same place on every piece.
PipeVec3 pipes_light_dir();
/// Eye-space half vector for an infinite viewer, normalize(L + (0, 0, 1)).
PipeVec3 pipes_half_vector();

/**
 * @brief GL fixed-function colour of `m` at unit eye-space normal `n`
 *
 * 1.1 ka (light-model ambient 1.0 plus light ambient 0.1) + kd max(0, n.L)
 * + ks max(0, n.H)^shininess where n.L > 0, each channel clamped to 1 and rounded to 0..255.
 */
Rgb pipes_shade(const PipeMaterial& m, PipeVec3 n);

enum class PipePieceKind : uint8_t { CYLINDER, ELBOW, BALL, TEAPOT };

/// One drawn primitive, in world units.
struct PipePiece {
    PipePieceKind kind = PipePieceKind::CYLINDER;
    /// CYLINDER: the start of its axis. Otherwise the joint's node centre.
    PipeVec3 at;
    /// CYLINDER: axis direction. ELBOW: the heading out.
    PipeDir dir = PipeDir::POS_X;
    /// ELBOW: the heading in.
    PipeDir from = PipeDir::POS_X;
    /// CYLINDER: axis length (kPipesPitch long, kPipesPitch - 2 r short).
    float length = 0.0f;
    uint8_t material = 0;
};

/// A pipe as the screen tracks it.
struct PipeState {
    PipeNode pos;
    PipeDir last = PipeDir::POS_X;
    int weight_straight = 1;
    uint8_t material = 0;
    int segment_count = 0; ///< growth steps taken since its start
    bool alive = false;
    bool stuck = false;

    bool operator==(const PipeState& o) const {
        return pos == o.pos && last == o.last && weight_straight == o.weight_straight &&
               material == o.material && segment_count == o.segment_count && alive == o.alive &&
               stuck == o.stuck;
    }
};

/// World position of node `n`'s centre for a grid of `dims`, the grid centred on the origin.
PipeVec3 pipes_node_world(PipeNode n, PipeGridDims dims);

/// Unit vector of `d` (NONE gives zero).
PipeVec3 pipes_dir_vec(PipeDir d);

/// Pieces a pipe starting at `node` heading `first` draws: a ball cap at the node and a short
/// cylinder from r ahead of it.
void pipes_emit_start(PipeNode node, PipeDir first, PipeGridDims dims, uint8_t material,
                      std::vector<PipePiece>& out);
/// Pieces one growth step from `node` draws: straight on, a long cylinder from r behind the
/// node; a turn, `joint` at the node (an ELBOW piece from `last` to `next`, a BALL, or a
/// TEAPOT) then a short cylinder from r ahead.
void pipes_emit_move(PipeNode node, PipeDir last, PipeDir next, PipeJoint joint, PipeGridDims dims,
                     uint8_t material, std::vector<PipePiece>& out);
/// The ball cap a stuck pipe ends with at `node`.
void pipes_emit_end(PipeNode node, PipeGridDims dims, uint8_t material,
                    std::vector<PipePiece>& out);

/**
 * @brief One screen of pipes, without pixels
 *
 * begin() starts kPipesActive pipes: one grows at a time, as in the original's default. Each step
 * first replaces the pipes that got stuck (a new pipe at a random empty node while fewer than
 * kPipesPerScreen have been counted, else the slot dies; a start with no empty node left caps the
 * count there), then grows every live pipe one node. The screen is finished once no slot is live.
 */
class PipesScreen {
  public:
    void begin(PipeGridDims dims, std::minstd_rand& rng, std::vector<PipePiece>& out);
    /// From now on the screen runs until the grid is full: no pipe limit, and the only teapot is
    /// the one a caller places (a start with nowhere to go and a teapot joint draw balls).
    void fill_grid();
    bool fills_grid() const {
        return fills_;
    }
    /// @return false once the screen is finished
    bool step(std::minstd_rand& rng, std::vector<PipePiece>& out);
    void release();

    const PipesGrid& grid() const {
        return grid_;
    }
    /// The grid, for tests that build a crowded screen by hand.
    PipesGrid& grid() {
        return grid_;
    }
    const std::array<PipeState, kPipesActive>& pipes() const {
        return pipes_;
    }
    /// Pipes counted toward kPipesPerScreen, the starting ones included.
    int pipes_counted() const {
        return counted_;
    }
    int pipes_started() const {
        return started_;
    }
    bool finished() const;
    /// Turn joints drawn since begin(), indexed by PipeJoint. Start and end caps are not joints.
    const std::array<uint32_t, 3>& joint_counts() const {
        return joints_;
    }
    /// Growth steps that went straight on since begin().
    uint32_t straight_steps() const {
        return straight_;
    }

  private:
    /// Starts slot `i` at a random empty node; false when none is left.
    bool start_pipe(size_t i, std::minstd_rand& rng, std::vector<PipePiece>& out);
    void grow_pipe(PipeState& p, std::minstd_rand& rng, std::vector<PipePiece>& out);

    PipesGrid grid_;
    std::array<PipeState, kPipesActive> pipes_{};
    int counted_ = 0;
    int started_ = 0;
    int max_pipes_ = kPipesPerScreen;
    bool fills_ = false;
    std::array<uint32_t, 3> joints_{};
    uint32_t straight_ = 0;
};

/// A mesh vertex in the piece's own frame.
struct PipeVertex {
    PipeVec3 p;
    PipeVec3 n; ///< unit, outward
};

struct PipeMesh {
    std::vector<PipeVertex> verts;
    std::vector<uint16_t> tris; ///< three indices per triangle, counter-clockwise from outside

    size_t heap_bytes() const {
        return buffer_bytes(verts, tris);
    }
};

/// Unit cylinder along +z from 0 to 1, radius kPipesRadius, kPipesSlices facets, open ends.
void pipes_build_cylinder(PipeMesh& out);
/// Sphere of radius sqrt(2) r / cos(pi / kPipesSlices), so it covers a pipe end r from its centre.
void pipes_build_ball(PipeMesh& out);
/// Quarter torus, bend radius = tube radius = r: enters at (0, 0, -r) heading +z, leaves at
/// (0, r, 0) heading +y; the bend centre is (0, r, -r).
void pipes_build_elbow(PipeMesh& out);
/// The Newell teapot (freeglut's patch data), scaled 0.5 x 2.5 r, z-up data turned y-up and
/// dropped 1.5 data units: x in [-3.75, 4.41], y in [-1.875, 2.0625].
void pipes_build_teapot(PipeMesh& out);

/// Radius of a ball joint or cap.
float pipes_ball_radius();

/// Mesh frame to world: p_world = p.x * x + p.y * y + p.z * z + t.
struct PipeXform {
    PipeVec3 x;
    PipeVec3 y;
    PipeVec3 z;
    PipeVec3 t;
};
/**
 * @brief Where a piece's mesh goes
 *
 * CYLINDER: z = dir scaled by length, x and y the fixed axis-aligned basis of dir's axis.
 * ELBOW: z = from, y = dir, x = y cross z. BALL and TEAPOT: world axes. t = piece.at.
 */
PipeXform pipes_piece_xform(const PipePiece& piece);
/// `p` carried from mesh frame to world by `xf`.
PipeVec3 pipes_xform_point(const PipeXform& xf, PipeVec3 p);

/**
 * @brief Draws pieces into a frame with a z-buffer and the fixed light
 *
 * Perspective with focal length (internal height / 2), the view turned `yrot` about +y and
 * pushed kPipesViewDist back. Each triangle interpolates inverse depth and the eye-space
 * normal; a pixel is written only where it is nearer than what the buffer holds, so draw
 * order never changes the picture. Shading reads per-material tables built by init().
 */
class PipesRenderer {
  public:
    /// Sizes the depth buffer for a `w` x `h` canvas drawn in `scale` x `scale` blocks, and
    /// builds the meshes and lighting tables.
    void init(uint32_t w, uint32_t h, uint8_t scale);
    /// Draws in `scale` x `scale` blocks from now on, with an empty depth buffer. A coarser
    /// scale fits in the buffer already held; only a finer one allocates.
    void set_scale(uint8_t scale);
    void release();
    void set_view(float yrot_deg);
    /// Empties the depth buffer; the frame itself is the dissolve's to clear.
    void clear_depth();
    /// Draws `piece` and returns the canvas box it may have written (empty when off screen).
    DirtyRect draw(const FrameTarget& frame, const PipePiece& piece);
    /// Canvas position of world point `p`; false behind the near plane.
    bool project(PipeVec3 p, float& sx, float& sy) const;
    /// Distance of world point `p` in front of the camera.
    float depth(PipeVec3 p) const;
    uint8_t scale() const {
        return scale_;
    }
    size_t heap_bytes() const;

  private:
    /// A mesh vertex carried to the screen: internal pixels, inverse depth, eye normal.
    struct ScreenVertex {
        float x;
        float y;
        float inv_depth;
        PipeVec3 n;
    };

    const PipeMesh& mesh_for(PipePieceKind kind) const;
    /// World point to eye space: turned by the view, then pushed back.
    PipeVec3 to_eye(PipeVec3 p) const;
    void raster_triangle(const FrameTarget& frame, const ScreenVertex& a, const ScreenVertex& b,
                         const ScreenVertex& c, const uint16_t* lut);

    uint32_t w_ = 0;
    uint32_t h_ = 0;
    uint8_t scale_ = 1;
    uint32_t iw_ = 0; ///< internal width, ceil(w / scale)
    uint32_t ih_ = 0;
    float focal_ = 0.0f;
    float cos_y_ = 1.0f;
    float sin_y_ = 0.0f;
    std::vector<float> inv_depth_;
    PipeMesh cylinder_;
    PipeMesh ball_;
    PipeMesh elbow_;
    PipeMesh teapot_;
    std::vector<ScreenVertex> screen_;
    std::vector<uint16_t> lut_; ///< per material: ambient + diffuse by n.L, specular by n.H
};

/// Clears the frame to black in random blocks over kPipesDissolveMs of frame time.
class PipesDissolve {
  public:
    /// Cuts a `w` x `h` frame into blocks of max(1, min(w, h) / kPipesDissolveDiv) pixels.
    void init(uint32_t w, uint32_t h);
    void release();
    /// Shuffles the block order and starts the clock.
    void begin(std::minstd_rand& rng);
    /// Clears the blocks due by now and dirties each.
    void step(uint32_t dt_ms, const FrameTarget& frame, std::vector<DirtyRect>& dirty);
    bool active() const {
        return active_;
    }
    size_t cleared() const {
        return cleared_;
    }
    size_t block_count() const {
        return order_.size();
    }
    int block_px() const {
        return block_px_;
    }
    size_t heap_bytes() const {
        return buffer_bytes(order_);
    }

  private:
    uint32_t w_ = 0;
    uint32_t h_ = 0;
    int block_px_ = 1;
    int cols_ = 0;
    std::vector<uint32_t> order_;
    size_t cleared_ = 0;
    uint32_t elapsed_ms_ = 0;
    bool active_ = false;
};

enum class PipesPhase : uint8_t { GROWING, DISSOLVING };

/// How the print a print screen shows has ended.
enum class PipesEnd : uint8_t { NONE, DONE, FAILED };

/**
 * @brief The whole saver: screens of pipes, each dissolved before the next
 *
 * With no print, growth runs one step per kPipesStepMs of frame time, at most
 * kPipesMaxStepsPerTick per call. A finished screen drops the steps still due and dissolves;
 * when the dissolve ends the view turns kPipesYrotStepDeg, a pending level's scale takes
 * effect, and a new screen starts, its first growth step a full step later.
 *
 * A print gets one screen (a print screen) that fills as the print goes: growth runs, at most
 * kPipesMaxStepsPerTick steps a call, only while the grid's taken share is below the print's
 * progress (carried between whole percents at the countdown's rate; paused, only up to the
 * reported progress). The node nearest the camera in the middle of the view stays free for the
 * teapot a finished print sets there once the rest of the grid is full; a failed or cancelled
 * print stops growing and greys the frame once. A print screen begun on a paused or ended print
 * catches up to it at kPipesFastForwardSteps steps a call, then shows the end. A print beginning
 * over a screen without one, a new print after an end, or no print after one dissolves the screen
 * first.
 */
class PipesSim {
  public:
    static constexpr const auto& kLevels = kPipesLevels;

    /// Starts the first screen at yrot 0 on a frame that is already black, for the print
    /// `print` reads.
    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const ClockReading& print);
    void step(uint32_t dt_ms, const FrameTarget& frame, std::minstd_rand& rng,
              const ClockReading& print, std::vector<DirtyRect>& dirty);
    /// The level's scale applies from the next screen.
    void request_level(size_t level);
    /// Releases every buffer.
    void clear();

    PipesPhase phase() const {
        return phase_;
    }
    const PipesScreen& screen() const {
        return screen_;
    }
    const PipesRenderer& renderer() const {
        return renderer_;
    }
    const PipesDissolve& dissolve() const {
        return dissolve_;
    }
    float yrot_deg() const {
        return yrot_deg_;
    }
    uint32_t screens_started() const {
        return screens_;
    }
    /// True while the screen belongs to a print.
    bool print_screen() const {
        return print_screen_;
    }
    PipesEnd print_end() const {
        return end_;
    }
    /// True once the end is on screen: the teapot drawn, or the frame greyed.
    bool end_drawn() const {
        return end_drawn_;
    }
    /// The node a finished print's teapot sits on; false when none is reserved.
    bool teapot_node(PipeNode& out) const {
        out = teapot_node_;
        return teapot_reserved_;
    }
    /// Nodes the screen's grid has taken, the teapot's reserved node included.
    size_t nodes_taken() const;
    size_t node_count() const;
    size_t heap_bytes() const;

  private:
    void start_screen(const FrameTarget& frame, std::minstd_rand& rng, const ClockReading& print,
                      std::vector<DirtyRect>& dirty);
    void draw_pieces(const FrameTarget& frame, std::vector<DirtyRect>& dirty);
    /// True when `print` needs a different kind of screen than the one showing.
    bool needs_new_screen(const ClockReading& print) const;
    void step_print(uint32_t dt_ms, const FrameTarget& frame, std::minstd_rand& rng,
                    const ClockReading& print, std::vector<DirtyRect>& dirty);
    /// Grows at most `max_steps` steps while fewer than `target` nodes are taken; true once
    /// `target` is reached or the grid is full.
    bool grow_to(size_t target, uint32_t max_steps, const FrameTarget& frame, std::minstd_rand& rng,
                 std::vector<DirtyRect>& dirty);
    /// Takes the teapot's node: the empty node nearest the camera in the middle of the view.
    void reserve_teapot_node(const FrameTarget& frame);
    /// Records how `mode` ended the print and draws it once it can: the teapot after the grid
    /// fills (one capped burst of growth a call, unless `max_steps` says more), or the grey.
    void follow_end(ClockMode mode, uint32_t max_steps, const FrameTarget& frame,
                    std::minstd_rand& rng, std::vector<DirtyRect>& dirty);

    PipesScreen screen_;
    PipesRenderer renderer_;
    PipesDissolve dissolve_;
    PipesPhase phase_ = PipesPhase::GROWING;
    float yrot_deg_ = 0.0f;
    size_t level_ = 0;
    uint32_t screens_ = 0;
    screensaver::StepAccumulator steps_;
    std::vector<PipePiece> pieces_;
    bool print_screen_ = false;
    PipesEnd end_ = PipesEnd::NONE;
    bool end_drawn_ = false;
    bool teapot_reserved_ = false;
    /// True while a screen begun on a paused or ended print grows toward it at
    /// kPipesFastForwardSteps a call.
    bool catching_up_ = false;
    PipeNode teapot_node_;
    float fill_ = 0.0f; ///< share of the grid the running print's screen should have taken
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
