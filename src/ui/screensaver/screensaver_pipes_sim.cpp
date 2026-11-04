// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes_sim.h"

#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>

using helix::ui::screensaver::random_below;
using helix::ui::screensaver::random_int;
using helix::ui::screensaver::shuffle_in_place;

namespace helix::ui {

// ============================================================================
// Grid and growth rules
// ============================================================================

PipeGridDims pipes_grid_dims(int32_t w, int32_t h) {
    const int32_t long_side = std::max(w, h);
    const int32_t short_side = std::min(w, h);
    const int other = long_side > 0
                          ? std::max(1, static_cast<int>(static_cast<int64_t>(kPipesNodesLong) *
                                                         short_side / long_side))
                          : 1;
    if (w >= h) {
        return {kPipesNodesLong, other, kPipesNodesLong};
    }
    return {other, kPipesNodesLong, kPipesNodesLong};
}

PipeNode pipe_step(PipeNode n, PipeDir d) {
    switch (d) {
    case PipeDir::POS_X:
        n.x++;
        break;
    case PipeDir::NEG_X:
        n.x--;
        break;
    case PipeDir::POS_Y:
        n.y++;
        break;
    case PipeDir::NEG_Y:
        n.y--;
        break;
    case PipeDir::POS_Z:
        n.z++;
        break;
    case PipeDir::NEG_Z:
        n.z--;
        break;
    case PipeDir::NONE:
        break;
    }
    return n;
}

PipeDir pipe_opposite(PipeDir d) {
    if (d == PipeDir::NONE) {
        return PipeDir::NONE;
    }
    // Each axis's pair is adjacent: POS then NEG.
    return static_cast<PipeDir>(static_cast<uint8_t>(d) ^ 1u);
}

void PipesGrid::reset(PipeGridDims dims) {
    dims_ = dims;
    const size_t n =
        static_cast<size_t>(std::max(0, dims.x)) * std::max(0, dims.y) * std::max(0, dims.z);
    taken_.assign(n, 0);
    empty_count_ = n;
}

void PipesGrid::release() {
    release_buffer(taken_);
    dims_ = {};
    empty_count_ = 0;
}

bool PipesGrid::in_bounds(PipeNode n) const {
    return n.x >= 0 && n.y >= 0 && n.z >= 0 && n.x < dims_.x && n.y < dims_.y && n.z < dims_.z;
}

size_t PipesGrid::index(PipeNode n) const {
    return (static_cast<size_t>(n.z) * dims_.y + n.y) * dims_.x + n.x;
}

bool PipesGrid::empty(PipeNode n) const {
    return in_bounds(n) && taken_[index(n)] == 0;
}

void PipesGrid::take(PipeNode n) {
    if (!in_bounds(n)) {
        return;
    }
    uint8_t& cell = taken_[index(n)];
    if (cell == 0) {
        cell = 1;
        empty_count_--;
    }
}

int PipesGrid::empty_run(PipeNode from, PipeDir d, int limit) const {
    int run = 0;
    PipeNode n = from;
    while (run < limit) {
        n = pipe_step(n, d);
        if (!empty(n)) {
            break;
        }
        run++;
    }
    return run;
}

bool PipesGrid::random_empty(std::minstd_rand& rng, PipeNode& out) const {
    if (empty_count_ == 0) {
        return false;
    }
    const int count = static_cast<int>(taken_.size());
    for (int tries = 2 * count; tries > 0; tries--) {
        const PipeNode n{random_below(rng, dims_.x), random_below(rng, dims_.y),
                         random_below(rng, dims_.z)};
        if (empty(n)) {
            out = n;
            return true;
        }
    }
    for (int z = 0; z < dims_.z; z++) {
        for (int y = 0; y < dims_.y; y++) {
            for (int x = 0; x < dims_.x; x++) {
                if (empty({x, y, z})) {
                    out = {x, y, z};
                    return true;
                }
            }
        }
    }
    return false;
}

int pipes_choose_weight_straight(std::minstd_rand& rng) {
    if (random_below(rng, 20) == 0) {
        return random_int(rng, 25, kPipesMaxWeightStraight);
    }
    return random_int(rng, 1, 4);
}

PipeJoint pipes_choose_joint(std::minstd_rand& rng) {
    // Any one value of the draw: the original tests against a fixed number.
    if (random_below(rng, kPipesTeapotOdds) == 153) {
        return PipeJoint::TEAPOT;
    }
    return random_below(rng, 3) == 0 ? PipeJoint::BALL : PipeJoint::ELBOW;
}

PipeDir pipes_choose_direction(const PipesGrid& grid, PipeNode pos, PipeDir last,
                               int weight_straight, std::minstd_rand& rng) {
    const bool straight_empty = last != PipeDir::NONE && grid.empty(pipe_step(pos, last));
    if (weight_straight >= kPipesMaxWeightStraight && straight_empty) {
        return last;
    }
    std::array<PipeDir, kPipeDirCount> turns{};
    std::array<int, kPipeDirCount> runs{};
    int turn_count = 0;
    for (int i = 0; i < kPipeDirCount; i++) {
        const auto d = static_cast<PipeDir>(i);
        if (d == last || !grid.empty(pipe_step(pos, d))) {
            continue;
        }
        turns[static_cast<size_t>(turn_count)] = d;
        runs[static_cast<size_t>(turn_count)] = grid.empty_run(pos, d, kPipesTurnLookahead);
        turn_count++;
    }
    const int w = straight_empty ? weight_straight : 0;
    if (w + turn_count == 0) {
        return PipeDir::NONE;
    }
    if (random_below(rng, w + turn_count) < w) {
        return last;
    }
    const int best = *std::max_element(runs.begin(), runs.begin() + turn_count);
    std::array<PipeDir, kPipeDirCount> ties{};
    int tie_count = 0;
    for (int i = 0; i < turn_count; i++) {
        if (runs[static_cast<size_t>(i)] == best) {
            ties[static_cast<size_t>(tie_count++)] = turns[static_cast<size_t>(i)];
        }
    }
    return ties[static_cast<size_t>(random_below(rng, tie_count))];
}

// ============================================================================
// Materials and lighting
// ============================================================================

namespace {

/// A degenerate direction lights and turns as no direction at all.
PipeVec3 normalized(PipeVec3 v) {
    return normalized_or(v, {});
}

/// Light-model ambient (1.0) plus the light's own ambient (0.1).
constexpr float kAmbientScale = 1.1f;

} // namespace

PipeVec3 pipes_light_dir() {
    return normalized({90.0f, 90.0f, 150.0f});
}

PipeVec3 pipes_half_vector() {
    const PipeVec3 l = pipes_light_dir();
    return normalized({l.x, l.y, l.z + 1.0f});
}

Rgb pipes_shade(const PipeMaterial& m, PipeVec3 n) {
    const float nl = dot(n, pipes_light_dir());
    const float nh = std::max(0.0f, dot(n, pipes_half_vector()));
    const float diffuse = std::max(0.0f, nl);
    const float spec = nl > 0.0f ? std::pow(nh, m.shininess) : 0.0f;
    std::array<uint8_t, 3> c{};
    for (size_t i = 0; i < 3; i++) {
        const float v = kAmbientScale * m.ka[i] + m.kd[i] * diffuse + m.ks[i] * spec;
        c[i] = unit_to_u8(v);
    }
    return {c[0], c[1], c[2]};
}

// ============================================================================
// One screen of pipes
// ============================================================================

namespace {

PipePiece joint_piece(PipePieceKind kind, PipeVec3 at, uint8_t material) {
    PipePiece p;
    p.kind = kind;
    p.at = at;
    p.material = material;
    return p;
}

PipePiece cylinder_piece(PipeVec3 at, PipeDir dir, float length, uint8_t material) {
    PipePiece p;
    p.kind = PipePieceKind::CYLINDER;
    p.at = at;
    p.dir = dir;
    p.length = length;
    p.material = material;
    return p;
}

constexpr float kShortPipe = kPipesPitch - 2.0f * kPipesRadius;

} // namespace

PipeVec3 pipes_node_world(PipeNode n, PipeGridDims dims) {
    const auto axis = [](int i, int dim) {
        return (static_cast<float>(i) - static_cast<float>(dim - 1) * 0.5f) * kPipesPitch;
    };
    return {axis(n.x, dims.x), axis(n.y, dims.y), axis(n.z, dims.z)};
}

PipeVec3 pipes_dir_vec(PipeDir d) {
    switch (d) {
    case PipeDir::POS_X:
        return {1, 0, 0};
    case PipeDir::NEG_X:
        return {-1, 0, 0};
    case PipeDir::POS_Y:
        return {0, 1, 0};
    case PipeDir::NEG_Y:
        return {0, -1, 0};
    case PipeDir::POS_Z:
        return {0, 0, 1};
    case PipeDir::NEG_Z:
        return {0, 0, -1};
    case PipeDir::NONE:
        break;
    }
    return {};
}

void pipes_emit_start(PipeNode node, PipeDir first, PipeGridDims dims, uint8_t material,
                      std::vector<PipePiece>& out) {
    const PipeVec3 c = pipes_node_world(node, dims);
    out.push_back(joint_piece(PipePieceKind::BALL, c, material));
    out.push_back(
        cylinder_piece(c + pipes_dir_vec(first) * kPipesRadius, first, kShortPipe, material));
}

void pipes_emit_move(PipeNode node, PipeDir last, PipeDir next, PipeJoint joint, PipeGridDims dims,
                     uint8_t material, std::vector<PipePiece>& out) {
    const PipeVec3 c = pipes_node_world(node, dims);
    const PipeVec3 d = pipes_dir_vec(next);
    if (next == last) {
        out.push_back(cylinder_piece(c + d * -kPipesRadius, next, kPipesPitch, material));
        return;
    }
    switch (joint) {
    case PipeJoint::ELBOW: {
        PipePiece elbow = joint_piece(PipePieceKind::ELBOW, c, material);
        elbow.from = last;
        elbow.dir = next;
        out.push_back(elbow);
        break;
    }
    case PipeJoint::BALL:
        out.push_back(joint_piece(PipePieceKind::BALL, c, material));
        break;
    case PipeJoint::TEAPOT:
        out.push_back(joint_piece(PipePieceKind::TEAPOT, c, material));
        break;
    }
    out.push_back(cylinder_piece(c + d * kPipesRadius, next, kShortPipe, material));
}

void pipes_emit_end(PipeNode node, PipeGridDims dims, uint8_t material,
                    std::vector<PipePiece>& out) {
    out.push_back(joint_piece(PipePieceKind::BALL, pipes_node_world(node, dims), material));
}

void PipesScreen::begin(PipeGridDims dims, std::minstd_rand& rng, std::vector<PipePiece>& out) {
    grid_.reset(dims);
    pipes_ = {};
    joints_ = {};
    straight_ = 0;
    started_ = 0;
    max_pipes_ = kPipesPerScreen;
    const int n = std::min(kPipesPerScreen, kPipesActive);
    counted_ = n;
    for (size_t i = 0; i < static_cast<size_t>(n); i++) {
        if (!start_pipe(i, rng, out)) {
            pipes_[i].alive = false;
            max_pipes_ = counted_;
        }
    }
}

bool PipesScreen::start_pipe(size_t i, std::minstd_rand& rng, std::vector<PipePiece>& out) {
    PipeState& p = pipes_[i];
    PipeNode node;
    if (!grid_.random_empty(rng, node)) {
        return false;
    }
    grid_.take(node);
    p = {};
    p.alive = true;
    p.pos = node;
    p.material = static_cast<uint8_t>(random_below(rng, static_cast<int>(kPipeMaterialCount)));
    p.weight_straight = pipes_choose_weight_straight(rng);
    p.last = static_cast<PipeDir>(random_below(rng, kPipeDirCount));
    started_++;
    const PipeDir first = pipes_choose_direction(grid_, node, p.last, p.weight_straight, rng);
    if (first == PipeDir::NONE) {
        out.push_back(
            joint_piece(PipePieceKind::TEAPOT, pipes_node_world(node, grid_.dims()), p.material));
        p.stuck = true;
        return true;
    }
    pipes_emit_start(node, first, grid_.dims(), p.material, out);
    p.pos = pipe_step(node, first);
    grid_.take(p.pos);
    p.last = first;
    return true;
}

void PipesScreen::grow_pipe(PipeState& p, std::minstd_rand& rng, std::vector<PipePiece>& out) {
    const PipeDir next = pipes_choose_direction(grid_, p.pos, p.last, p.weight_straight, rng);
    if (next == PipeDir::NONE) {
        pipes_emit_end(p.pos, grid_.dims(), p.material, out);
        p.stuck = true;
        return;
    }
    PipeJoint joint = PipeJoint::ELBOW;
    if (next != p.last) {
        joint = pipes_choose_joint(rng);
        joints_[static_cast<size_t>(joint)]++;
    } else {
        straight_++;
    }
    pipes_emit_move(p.pos, p.last, next, joint, grid_.dims(), p.material, out);
    p.pos = pipe_step(p.pos, next);
    grid_.take(p.pos);
    p.last = next;
    p.segment_count++;
}

bool PipesScreen::step(std::minstd_rand& rng, std::vector<PipePiece>& out) {
    for (size_t i = 0; i < pipes_.size(); i++) {
        PipeState& p = pipes_[i];
        if (!p.alive || !p.stuck) {
            continue;
        }
        if (++counted_ > max_pipes_) {
            p.alive = false;
        } else if (!start_pipe(i, rng, out)) {
            p.alive = false;
            max_pipes_ = counted_;
        }
    }
    for (PipeState& p : pipes_) {
        if (p.alive && !p.stuck) {
            grow_pipe(p, rng, out);
        }
    }
    return !finished();
}

bool PipesScreen::finished() const {
    return std::none_of(pipes_.begin(), pipes_.end(), [](const PipeState& p) { return p.alive; });
}

void PipesScreen::release() {
    grid_.release();
    pipes_ = {};
    counted_ = 0;
    started_ = 0;
    joints_ = {};
    straight_ = 0;
}

// ============================================================================
// Meshes
// ============================================================================

namespace {

/// Bend segments of an elbow.
constexpr int kElbowSegments = 6;
/// Latitude bands of a ball.
constexpr int kBallStacks = 8;

/// Joins ring `a` to ring `b` (kPipesSlices vertices each, b ahead of a along the tube) with
/// triangles facing out when each ring runs counter-clockwise about the direction of travel.
void join_rings(uint16_t a, uint16_t b, std::vector<uint16_t>& tris) {
    for (int k = 0; k < kPipesSlices; k++) {
        const int k1 = (k + 1) % kPipesSlices;
        const auto a0 = static_cast<uint16_t>(a + k);
        const auto a1 = static_cast<uint16_t>(a + k1);
        const auto b0 = static_cast<uint16_t>(b + k);
        const auto b1 = static_cast<uint16_t>(b + k1);
        tris.insert(tris.end(), {a0, a1, b1, a0, b1, b0});
    }
}

float slice_angle(int k) {
    return static_cast<float>(k) * 2.0f * kSaverPi / static_cast<float>(kPipesSlices);
}

} // namespace

float pipes_ball_radius() {
    return std::sqrt(2.0f) * kPipesRadius / std::cos(kSaverPi / static_cast<float>(kPipesSlices));
}

void pipes_build_cylinder(PipeMesh& out) {
    out.verts.clear();
    out.tris.clear();
    for (int ring = 0; ring < 2; ring++) {
        for (int k = 0; k < kPipesSlices; k++) {
            const float c = std::cos(slice_angle(k));
            const float s = std::sin(slice_angle(k));
            out.verts.push_back(
                {{kPipesRadius * c, kPipesRadius * s, static_cast<float>(ring)}, {c, s, 0.0f}});
        }
    }
    join_rings(0, kPipesSlices, out.tris);
}

void pipes_build_ball(PipeMesh& out) {
    out.verts.clear();
    out.tris.clear();
    const float radius = pipes_ball_radius();
    // Poles on the z axis, rings of kPipesSlices between them.
    out.verts.push_back({{0, 0, -radius}, {0, 0, -1}});
    for (int stack = 1; stack < kBallStacks; stack++) {
        const float polar = kSaverPi * static_cast<float>(stack) / static_cast<float>(kBallStacks);
        const float z = -std::cos(polar);
        const float rr = std::sin(polar);
        for (int k = 0; k < kPipesSlices; k++) {
            const PipeVec3 n{rr * std::cos(slice_angle(k)), rr * std::sin(slice_angle(k)), z};
            out.verts.push_back({{n.x * radius, n.y * radius, n.z * radius}, n});
        }
    }
    const auto top = static_cast<uint16_t>(out.verts.size());
    out.verts.push_back({{0, 0, radius}, {0, 0, 1}});
    for (int k = 0; k < kPipesSlices; k++) {
        const auto k1 = static_cast<uint16_t>(1 + (k + 1) % kPipesSlices);
        out.tris.insert(out.tris.end(), {0, k1, static_cast<uint16_t>(1 + k)});
    }
    for (int stack = 1; stack + 1 < kBallStacks; stack++) {
        join_rings(static_cast<uint16_t>(1 + (stack - 1) * kPipesSlices),
                   static_cast<uint16_t>(1 + stack * kPipesSlices), out.tris);
    }
    const auto last = static_cast<uint16_t>(1 + (kBallStacks - 2) * kPipesSlices);
    for (int k = 0; k < kPipesSlices; k++) {
        const auto k1 = static_cast<uint16_t>(last + (k + 1) % kPipesSlices);
        out.tris.insert(out.tris.end(), {static_cast<uint16_t>(last + k), k1, top});
    }
}

void pipes_build_elbow(PipeMesh& out) {
    out.verts.clear();
    out.tris.clear();
    const float r = kPipesRadius;
    for (int seg = 0; seg <= kElbowSegments; seg++) {
        const float phi = 0.5f * kSaverPi * static_cast<float>(seg) / kElbowSegments;
        // Centreline round (0, r, -r); the ring's frame is x and e2, with x cross e2 the
        // direction of travel.
        const PipeVec3 centre{0.0f, r - r * std::cos(phi), -r + r * std::sin(phi)};
        const PipeVec3 e2{0.0f, std::cos(phi), -std::sin(phi)};
        for (int k = 0; k < kPipesSlices; k++) {
            const float c = std::cos(slice_angle(k));
            const float s = std::sin(slice_angle(k));
            const PipeVec3 n{c, s * e2.y, s * e2.z};
            out.verts.push_back({{centre.x + r * n.x, centre.y + r * n.y, centre.z + r * n.z}, n});
        }
    }
    for (int seg = 0; seg < kElbowSegments; seg++) {
        join_rings(static_cast<uint16_t>(seg * kPipesSlices),
                   static_cast<uint16_t>((seg + 1) * kPipesSlices), out.tris);
    }
}

namespace {

/// The fixed axis-aligned pair across `d`, with x cross y = d.
void cylinder_basis(PipeDir d, PipeVec3& x, PipeVec3& y) {
    switch (d) {
    case PipeDir::POS_X:
        x = {0, 1, 0};
        y = {0, 0, 1};
        return;
    case PipeDir::NEG_X:
        x = {0, 1, 0};
        y = {0, 0, -1};
        return;
    case PipeDir::POS_Y:
        x = {0, 0, 1};
        y = {1, 0, 0};
        return;
    case PipeDir::NEG_Y:
        x = {0, 0, 1};
        y = {-1, 0, 0};
        return;
    case PipeDir::POS_Z:
        x = {1, 0, 0};
        y = {0, 1, 0};
        return;
    case PipeDir::NEG_Z:
    case PipeDir::NONE:
        x = {1, 0, 0};
        y = {0, -1, 0};
        return;
    }
}

} // namespace

PipeXform pipes_piece_xform(const PipePiece& piece) {
    PipeXform xf;
    xf.t = piece.at;
    switch (piece.kind) {
    case PipePieceKind::CYLINDER: {
        cylinder_basis(piece.dir, xf.x, xf.y);
        const PipeVec3 d = pipes_dir_vec(piece.dir);
        xf.z = {d.x * piece.length, d.y * piece.length, d.z * piece.length};
        break;
    }
    case PipePieceKind::ELBOW:
        xf.z = pipes_dir_vec(piece.from);
        xf.y = pipes_dir_vec(piece.dir);
        xf.x = cross(xf.y, xf.z);
        break;
    case PipePieceKind::BALL:
    case PipePieceKind::TEAPOT:
        xf.x = {1, 0, 0};
        xf.y = {0, 1, 0};
        xf.z = {0, 0, 1};
        break;
    }
    return xf;
}

PipeVec3 pipes_xform_point(const PipeXform& xf, PipeVec3 p) {
    return {p.x * xf.x.x + p.y * xf.y.x + p.z * xf.z.x + xf.t.x,
            p.x * xf.x.y + p.y * xf.y.y + p.z * xf.z.y + xf.t.y,
            p.x * xf.x.z + p.y * xf.y.z + p.z * xf.z.z + xf.t.z};
}

// ============================================================================
// Renderer
// ============================================================================

namespace {

constexpr int kLutAd = 256;    ///< ambient + diffuse entries over n.L in [0, 1]
constexpr int kLutSpec = 1024; ///< specular entries over n.H in [0, 1]
constexpr size_t kLutStride = 3 * (kLutAd + kLutSpec);

/// Nearer than this a triangle is not drawn (nothing in the grid comes close).
constexpr float kNearDepth = 0.5f;

uint16_t lut_value(float v) {
    return static_cast<uint16_t>(std::lround(std::max(0.0f, v) * 255.0f));
}

} // namespace

void PipesRenderer::init(uint32_t w, uint32_t h, uint8_t scale) {
    w_ = w;
    h_ = h;
    set_scale(scale);
    pipes_build_cylinder(cylinder_);
    pipes_build_ball(ball_);
    pipes_build_elbow(elbow_);
    pipes_build_teapot(teapot_);
    const size_t most = std::max(
        {cylinder_.verts.size(), ball_.verts.size(), elbow_.verts.size(), teapot_.verts.size()});
    screen_.assign(most, ScreenVertex{});
    lut_.assign(kLutStride * kPipeMaterialCount, 0);
    for (size_t m = 0; m < kPipeMaterialCount; m++) {
        const PipeMaterial& mat = kPipeMaterials[m];
        uint16_t* base = lut_.data() + m * kLutStride;
        for (size_t c = 0; c < 3; c++) {
            uint16_t* ad = base + c * kLutAd;
            uint16_t* sp = base + 3 * kLutAd + c * kLutSpec;
            for (int i = 0; i < kLutAd; i++) {
                const float nl = static_cast<float>(i) / (kLutAd - 1);
                ad[i] = lut_value(kAmbientScale * mat.ka[c] + mat.kd[c] * nl);
            }
            for (int i = 0; i < kLutSpec; i++) {
                const float nh = static_cast<float>(i) / (kLutSpec - 1);
                sp[i] = lut_value(mat.ks[c] * std::pow(nh, mat.shininess));
            }
        }
    }
}

void PipesRenderer::release() {
    for (PipeMesh* m : {&cylinder_, &ball_, &elbow_, &teapot_}) {
        release_buffers(m->verts, m->tris);
    }
    release_buffers(inv_depth_, screen_, lut_);
    w_ = h_ = iw_ = ih_ = 0;
}

size_t PipesRenderer::heap_bytes() const {
    return buffer_bytes(inv_depth_, screen_, lut_) + cylinder_.heap_bytes() + ball_.heap_bytes() +
           elbow_.heap_bytes() + teapot_.heap_bytes();
}

void PipesRenderer::set_scale(uint8_t scale) {
    scale_ = std::max<uint8_t>(1, scale);
    iw_ = static_cast<uint32_t>(scaled_extent(static_cast<int32_t>(w_), scale_));
    ih_ = static_cast<uint32_t>(scaled_extent(static_cast<int32_t>(h_), scale_));
    focal_ = static_cast<float>(ih_) * 0.5f;
    inv_depth_.assign(static_cast<size_t>(iw_) * ih_, 0.0f);
}

void PipesRenderer::set_view(float yrot_deg) {
    const float rad = yrot_deg * kSaverPi / 180.0f;
    cos_y_ = std::cos(rad);
    sin_y_ = std::sin(rad);
}

void PipesRenderer::clear_depth() {
    std::fill(inv_depth_.begin(), inv_depth_.end(), 0.0f);
}

PipeVec3 PipesRenderer::to_eye(PipeVec3 p) const {
    return {p.x * cos_y_ + p.z * sin_y_, p.y, -p.x * sin_y_ + p.z * cos_y_ - kPipesViewDist};
}

bool PipesRenderer::project(PipeVec3 p, float& sx, float& sy) const {
    const PipeVec3 e = to_eye(p);
    const float d = -e.z;
    if (d < kNearDepth) {
        return false;
    }
    const auto s = static_cast<float>(scale_);
    sx = (static_cast<float>(iw_) * 0.5f + focal_ * e.x / d) * s;
    sy = (static_cast<float>(ih_) * 0.5f - focal_ * e.y / d) * s;
    return true;
}

const PipeMesh& PipesRenderer::mesh_for(PipePieceKind kind) const {
    switch (kind) {
    case PipePieceKind::CYLINDER:
        return cylinder_;
    case PipePieceKind::ELBOW:
        return elbow_;
    case PipePieceKind::BALL:
        return ball_;
    case PipePieceKind::TEAPOT:
        break;
    }
    return teapot_;
}

DirtyRect PipesRenderer::draw(const FrameTarget& frame, const PipePiece& piece) {
    DirtyRect box;
    if (iw_ == 0 || ih_ == 0 || inv_depth_.empty()) {
        return box;
    }
    const PipeMesh& mesh = mesh_for(piece.kind);
    const PipeXform xf = pipes_piece_xform(piece);
    // Normals turn with the frame's axes; only a cylinder's z is scaled, and its normals have
    // no z part.
    const PipeVec3 nz = normalized(xf.z);
    float min_x = 1e30f;
    float min_y = 1e30f;
    float max_x = -1e30f;
    float max_y = -1e30f;
    for (size_t i = 0; i < mesh.verts.size(); i++) {
        const PipeVertex& v = mesh.verts[i];
        const PipeVec3 e = to_eye(pipes_xform_point(xf, v.p));
        const PipeVec3 wn{v.n.x * xf.x.x + v.n.y * xf.y.x + v.n.z * nz.x,
                          v.n.x * xf.x.y + v.n.y * xf.y.y + v.n.z * nz.y,
                          v.n.x * xf.x.z + v.n.y * xf.y.z + v.n.z * nz.z};
        ScreenVertex& sv = screen_[i];
        sv.n = {wn.x * cos_y_ + wn.z * sin_y_, wn.y, -wn.x * sin_y_ + wn.z * cos_y_};
        const float d = -e.z;
        if (d < kNearDepth) {
            sv.inv_depth = -1.0f;
            continue;
        }
        sv.inv_depth = 1.0f / d;
        sv.x = static_cast<float>(iw_) * 0.5f + focal_ * e.x * sv.inv_depth;
        sv.y = static_cast<float>(ih_) * 0.5f - focal_ * e.y * sv.inv_depth;
        min_x = std::min(min_x, sv.x);
        max_x = std::max(max_x, sv.x);
        min_y = std::min(min_y, sv.y);
        max_y = std::max(max_y, sv.y);
    }
    if (min_x > max_x) {
        return box;
    }
    const uint16_t* lut = lut_.data() + (piece.material % kPipeMaterialCount) * kLutStride;
    for (size_t t = 0; t + 2 < mesh.tris.size(); t += 3) {
        const ScreenVertex& a = screen_[mesh.tris[t]];
        const ScreenVertex& b = screen_[mesh.tris[t + 1]];
        const ScreenVertex& c = screen_[mesh.tris[t + 2]];
        if (a.inv_depth < 0.0f || b.inv_depth < 0.0f || c.inv_depth < 0.0f) {
            continue;
        }
        raster_triangle(frame, a, b, c, lut);
    }
    // Every write lands inside the vertices' box, padded a pixel for the sampling.
    const auto clamp_x = [&](float v) {
        return static_cast<int32_t>(std::clamp(v, -1.0f, static_cast<float>(iw_)));
    };
    const auto clamp_y = [&](float v) {
        return static_cast<int32_t>(std::clamp(v, -1.0f, static_cast<float>(ih_)));
    };
    const int32_t ix1 = std::max(0, clamp_x(std::floor(min_x)) - 1);
    const int32_t iy1 = std::max(0, clamp_y(std::floor(min_y)) - 1);
    const int32_t ix2 = std::min(static_cast<int32_t>(iw_) - 1, clamp_x(std::ceil(max_x)) + 1);
    const int32_t iy2 = std::min(static_cast<int32_t>(ih_) - 1, clamp_y(std::ceil(max_y)) + 1);
    if (ix2 < ix1 || iy2 < iy1) {
        return box;
    }
    const int32_t s = scale_;
    box.add(ix1 * s, iy1 * s, std::min(static_cast<int32_t>(frame.w) - 1, (ix2 + 1) * s - 1),
            std::min(static_cast<int32_t>(frame.h) - 1, (iy2 + 1) * s - 1));
    return box;
}

void PipesRenderer::raster_triangle(const FrameTarget& frame, const ScreenVertex& a,
                                    const ScreenVertex& b0, const ScreenVertex& c0,
                                    const uint16_t* lut) {
    // Counter-clockwise in eye space (y up) is clockwise on screen (y down): a front face has
    // negative screen area. Swapping two vertices makes it positive for the edge tests.
    const float area = (b0.x - a.x) * (c0.y - a.y) - (b0.y - a.y) * (c0.x - a.x);
    if (area >= 0.0f) {
        return;
    }
    const ScreenVertex& b = c0;
    const ScreenVertex& c = b0;
    const float inv_area = -1.0f / area;

    const int32_t x1 = std::max(0, static_cast<int32_t>(std::floor(std::min({a.x, b.x, c.x}))));
    const int32_t y1 = std::max(0, static_cast<int32_t>(std::floor(std::min({a.y, b.y, c.y}))));
    const int32_t x2 = std::min(static_cast<int32_t>(iw_) - 1,
                                static_cast<int32_t>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int32_t y2 = std::min(static_cast<int32_t>(ih_) - 1,
                                static_cast<int32_t>(std::ceil(std::max({a.y, b.y, c.y}))));
    if (x2 < x1 || y2 < y1) {
        return;
    }

    // Edge function of edge (p, q), positive inside. A pixel centre on a shared edge passes
    // both triangles' tests; the second fails the depth test, so it is drawn once.
    struct Edge {
        float dx;
        float dy;
    };
    const auto make_edge = [](const ScreenVertex& p, const ScreenVertex& q) {
        return Edge{q.x - p.x, q.y - p.y};
    };
    const Edge e0 = make_edge(b, c); // weight of a
    const Edge e1 = make_edge(c, a); // weight of b
    const Edge e2 = make_edge(a, b); // weight of c
    const auto eval = [](const Edge& e, const ScreenVertex& p, float x, float y) {
        return e.dx * (y - p.y) - e.dy * (x - p.x);
    };

    const PipeVec3 light = pipes_light_dir();
    const PipeVec3 half = pipes_half_vector();
    const uint16_t* ad[3] = {lut, lut + kLutAd, lut + 2 * kLutAd};
    const uint16_t* sp[3] = {lut + 3 * kLutAd, lut + 3 * kLutAd + kLutSpec,
                             lut + 3 * kLutAd + 2 * kLutSpec};
    PixelWriter out(frame);
    const int32_t s = scale_;
    for (int32_t py = y1; py <= y2; py++) {
        const float fy = static_cast<float>(py) + 0.5f;
        float w0 = eval(e0, b, static_cast<float>(x1) + 0.5f, fy);
        float w1 = eval(e1, c, static_cast<float>(x1) + 0.5f, fy);
        float w2 = eval(e2, a, static_cast<float>(x1) + 0.5f, fy);
        float* depth_row = inv_depth_.data() + static_cast<size_t>(py) * iw_;
        for (int32_t px = x1; px <= x2; px++, w0 -= e0.dy, w1 -= e1.dy, w2 -= e2.dy) {
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }
            const float l0 = w0 * inv_area;
            const float l1 = w1 * inv_area;
            const float l2 = w2 * inv_area;
            const float inv_depth = l0 * a.inv_depth + l1 * b.inv_depth + l2 * c.inv_depth;
            if (inv_depth <= depth_row[px]) {
                continue;
            }
            depth_row[px] = inv_depth;
            PipeVec3 n{l0 * a.n.x + l1 * b.n.x + l2 * c.n.x, l0 * a.n.y + l1 * b.n.y + l2 * c.n.y,
                       l0 * a.n.z + l1 * b.n.z + l2 * c.n.z};
            n = normalized(n);
            const float nl = dot(n, light);
            const size_t il = unit_to_u8(nl);
            const auto ih = static_cast<size_t>(
                std::lround(std::clamp(dot(n, half), 0.0f, 1.0f) * (kLutSpec - 1)));
            uint8_t rgb[3];
            for (size_t ch = 0; ch < 3; ch++) {
                const int v = ad[ch][il] + (nl > 0.0f ? sp[ch][ih] : 0);
                rgb[ch] = static_cast<uint8_t>(std::min(255, v));
            }
            const Rgb colour{rgb[0], rgb[1], rgb[2]};
            out.fill_rect_dithered({px * s, py * s, (px + 1) * s - 1, (py + 1) * s - 1}, colour);
        }
    }
}

// ============================================================================
// Dissolve
// ============================================================================

void PipesDissolve::init(uint32_t w, uint32_t h) {
    w_ = w;
    h_ = h;
    block_px_ = std::max(1, static_cast<int>(std::min(w, h)) / kPipesDissolveDiv);
    const auto b = static_cast<uint32_t>(block_px_);
    cols_ = static_cast<int>((w + b - 1) / b);
    const uint32_t rows = (h + b - 1) / b;
    order_.resize(static_cast<size_t>(cols_) * rows);
    for (size_t i = 0; i < order_.size(); i++) {
        order_[i] = static_cast<uint32_t>(i);
    }
    cleared_ = 0;
    elapsed_ms_ = 0;
    active_ = false;
}

void PipesDissolve::release() {
    release_buffer(order_);
    cleared_ = 0;
    active_ = false;
}

void PipesDissolve::begin(std::minstd_rand& rng) {
    shuffle_in_place(rng, order_.begin(), order_.end());
    cleared_ = 0;
    elapsed_ms_ = 0;
    active_ = !order_.empty();
}

void PipesDissolve::step(uint32_t dt_ms, const FrameTarget& frame, std::vector<DirtyRect>& dirty) {
    if (!active_) {
        return;
    }
    elapsed_ms_ += dt_ms;
    const size_t n = order_.size();
    const auto due = static_cast<size_t>(
        std::min<uint64_t>(n, static_cast<uint64_t>(n) * elapsed_ms_ / kPipesDissolveMs));
    PixelWriter out(frame);
    const auto w = static_cast<int32_t>(std::min(w_, frame.w));
    const auto h = static_cast<int32_t>(std::min(h_, frame.h));
    for (; cleared_ < due; cleared_++) {
        const uint32_t block = order_[cleared_];
        const int32_t x1 = static_cast<int32_t>(block % static_cast<uint32_t>(cols_)) * block_px_;
        const int32_t y1 = static_cast<int32_t>(block / static_cast<uint32_t>(cols_)) * block_px_;
        const int32_t x2 = std::min(w, x1 + block_px_) - 1;
        const int32_t y2 = std::min(h, y1 + block_px_) - 1;
        const DirtyRect r{x1, y1, x2, y2};
        out.fill_rect(r, {0, 0, 0});
        if (!r.empty()) {
            dirty.push_back(r);
        }
    }
    if (cleared_ >= n) {
        active_ = false;
    }
}

// ============================================================================
// PipesSim
// ============================================================================

void PipesSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level) {
    level_ = std::min(level, kPipesLevelCount - 1);
    renderer_.init(frame.w, frame.h, kPipesLevels[level_].scale);
    dissolve_.init(frame.w, frame.h);
    pieces_.clear();
    pieces_.reserve(4 * kPipesActive * kPipesMaxStepsPerTick);
    yrot_deg_ = 0.0f;
    screens_ = 0;
    std::vector<DirtyRect> unused;
    start_screen(frame, rng, unused);
}

void PipesSim::start_screen(const FrameTarget& frame, std::minstd_rand& rng,
                            std::vector<DirtyRect>& dirty) {
    const uint8_t scale = kPipesLevels[level_].scale;
    if (renderer_.scale() != scale) {
        renderer_.set_scale(scale);
    }
    const PipeGridDims dims =
        pipes_grid_dims(static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h));
    renderer_.clear_depth();
    renderer_.set_view(yrot_deg_);
    pieces_.clear();
    screen_.begin(dims, rng, pieces_);
    draw_pieces(frame, dirty);
    steps_.reset();
    phase_ = PipesPhase::GROWING;
    screens_++;
}

void PipesSim::draw_pieces(const FrameTarget& frame, std::vector<DirtyRect>& dirty) {
    for (const PipePiece& p : pieces_) {
        const DirtyRect box = renderer_.draw(frame, p);
        if (!box.empty()) {
            dirty.push_back(box);
        }
    }
    pieces_.clear();
}

void PipesSim::step(uint32_t dt_ms, const FrameTarget& frame, std::minstd_rand& rng,
                    std::vector<DirtyRect>& dirty) {
    if (phase_ == PipesPhase::GROWING) {
        const uint32_t due = steps_.steps_due(dt_ms, kPipesStepMs, kPipesMaxStepsPerTick);
        for (uint32_t i = 0; i < due; i++) {
            const bool more = screen_.step(rng, pieces_);
            draw_pieces(frame, dirty);
            if (!more) {
                // The steps still due belong to the finished screen; the dissolve starts on
                // the next frame's time.
                dissolve_.begin(rng);
                phase_ = PipesPhase::DISSOLVING;
                return;
            }
        }
        return;
    }
    dissolve_.step(dt_ms, frame, dirty);
    if (dissolve_.active()) {
        return;
    }
    yrot_deg_ = std::fmod(yrot_deg_ + kPipesYrotStepDeg, 360.0f);
    start_screen(frame, rng, dirty);
}

void PipesSim::request_level(size_t level) {
    level_ = std::min(level, kPipesLevelCount - 1);
}

void PipesSim::clear() {
    screen_.release();
    renderer_.release();
    dissolve_.release();
    release_buffer(pieces_);
    phase_ = PipesPhase::GROWING;
    steps_.reset();
}

size_t PipesSim::heap_bytes() const {
    return screen_.grid().heap_bytes() + renderer_.heap_bytes() + dissolve_.heap_bytes() +
           buffer_bytes(pieces_);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
