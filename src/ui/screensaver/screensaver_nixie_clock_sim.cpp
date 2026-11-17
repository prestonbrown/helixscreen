// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_nixie_clock_sim.h"

#include "screensaver_buffer.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_print_colors.h"
#include "screensaver_stroke_digits.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

// ---------- The tube's parts, in glyph units ----------

/// The glass and socket's inset from the card's sides.
constexpr float kInset = 0.05f;
/// Glass below the numerals before the socket starts: past the halo, so a fade never redraws
/// the socket and the AM/PM marker on it.
constexpr float kGlassBelow = 0.22f;
/// Width of the glass's lit rim.
constexpr float kRimWidth = 0.035f;
/// The halo's reach beyond the wire, and how fast it falls off.
constexpr float kHaloReach = 0.2f;
constexpr float kHaloFalloff = 0.06f;
/// The halo's brightness next to the wire, against the wire's own.
constexpr float kHaloPeak = 0.42f;
/// The anode mesh: line pitch, and how far it reaches past the numerals.
constexpr float kMeshPitch = 0.085f;
constexpr float kMeshMargin = 0.08f;
/// The socket's rounded bottom corners.
constexpr float kSocketCorner = 0.08f;
/// The neon dots between groups: radius against a unit, and their height in the numeral box.
constexpr float kDotR = 0.055f;
constexpr float kDotRows[] = {0.36f, 0.68f};

constexpr Rgb kGlassInside{9, 8, 11};
constexpr Rgb kGlassRim{64, 60, 68};
constexpr Rgb kGlassStreak{46, 46, 54};
constexpr Rgb kMesh{30, 26, 24};
/// The unlit wire numerals stacked behind the lit one.
constexpr Rgb kWire{54, 42, 34};
constexpr Rgb kSocketTop{58, 55, 60};
constexpr Rgb kSocketBottom{16, 16, 18};
constexpr Rgb kSocketEdge{96, 92, 98};
/// How far the core of a lit stroke runs from the ink toward white, per channel: the red and
/// green further than the blue, so it burns warm whatever the ink.
constexpr float kCoreLiftRg = 0.45f;
constexpr float kCoreLiftB = 0.2f;

int32_t px(float v, int32_t unit) {
    return static_cast<int32_t>(std::lround(v * static_cast<float>(unit)));
}

/// Glow for a point `d` px from a wire `half` px in half-width with a halo reaching `reach` px:
/// the wire's coverage plus a halo falling to 0 at `reach`.
float glow_at(float d, float half, float reach, float falloff) {
    const float core = edge_coverage(half - d);
    const float out = std::max(0.0f, d - half);
    const float tail = std::exp(-reach / falloff);
    const float halo =
        out >= reach ? 0.0f : kHaloPeak * (std::exp(-out / falloff) - tail) / (1.0f - tail);
    return std::min(1.0f, core + halo);
}

uint8_t saturating_add(uint8_t a, uint32_t b) {
    return static_cast<uint8_t>(std::min<uint32_t>(255, a + b));
}

/// `base` with `glow` added at `amount` (0 to 256).
Rgb add_glow(Rgb base, Rgb glow, uint32_t amount) {
    return {saturating_add(base.r, glow.r * amount >> 8),
            saturating_add(base.g, glow.g * amount >> 8),
            saturating_add(base.b, glow.b * amount >> 8)};
}

/// Card `i` of `face` in layout order: the main row, then the small row.
char face_char(const FlipFace& face, size_t i) {
    const size_t main = face.shape.card_count();
    if (i < main) {
        return face.cards[i].digit;
    }
    return i - main < kFlipMaxSubCards ? face.sub[i - main].digit : '\0';
}

bool shape_differs(const FlipFace& face, const FlipLayout& layout) {
    return face.shape != layout.shape || face.sub_shape != layout.sub_shape ||
           face.sub_colons != layout.sub_colons;
}

uint32_t to_amount(float share) {
    return static_cast<uint32_t>(std::lround(std::clamp(share, 0.0f, 1.0f) * 256.0f));
}

} // namespace

// ---------- NixieTubeArt ----------

void NixieTubeArt::build(const FlipRowMetrics& row, int32_t card_w, int32_t card_h) {
    clear();
    const CardGeometry& g = kNixieTubeGeometry;
    unit = std::max<int32_t>(1, row.unit);
    w = card_w;
    h = card_h;
    const auto u = static_cast<float>(unit);
    const int32_t gw = px(1.0f + g.stroke, unit);
    const int32_t gh = px(kStrokeDigitPathH + g.stroke, unit);
    halo = std::max<int32_t>(2, px(kHaloReach, unit));
    glow_x = row.pad_x - halo;
    glow_y = row.pad_y - halo;
    glow_w = gw + 2 * halo;
    glow_h = gh + 2 * halo;
    inset = std::max<int32_t>(1, px(kInset, unit));
    socket_y = std::min(h - 2, row.pad_y + gh + px(kGlassBelow, unit));

    base.assign(static_cast<size_t>(w) * h, Rgb{});
    const float half_stroke = g.stroke / 2.0f;

    // The glass: a dome on a straight tube, from the top inset down into the socket.
    const float glass_x1 = static_cast<float>(inset);
    const float glass_x2 = static_cast<float>(w - inset);
    const float radius = (glass_x2 - glass_x1) / 2.0f;
    const float cx = (glass_x1 + glass_x2) / 2.0f;
    const float dome_cy = static_cast<float>(inset) + radius;
    const float rim = std::max(1.5f, kRimWidth * u);
    const float streak_x = cx - 0.55f * radius;
    const float streak_w = std::max(1.0f, 0.13f * radius);
    // The anode cage around the numerals.
    const int32_t margin = px(kMeshMargin, unit);
    const int32_t mesh_x1 = row.pad_x - margin;
    const int32_t mesh_x2 = row.pad_x + gw + margin;
    const int32_t mesh_y1 = row.pad_y - margin;
    const int32_t mesh_y2 = row.pad_y + gh + margin;
    const int32_t pitch = std::max<int32_t>(3, px(kMeshPitch, unit));

    for (int32_t y = 0; y < socket_y; y++) {
        const float fy = static_cast<float>(y) + 0.5f;
        for (int32_t x = 0; x < w; x++) {
            const float fx = static_cast<float>(x) + 0.5f;
            const float dist =
                fy < dome_cy ? std::hypot(fx - cx, fy - dome_cy) : std::fabs(fx - cx);
            const float inside = radius - dist;
            const float cov = edge_coverage(inside);
            if (cov <= 0.0f) {
                continue;
            }
            Rgb c = mix_rgb(kGlassInside, kGlassRim, std::clamp(1.0f - inside / rim, 0.0f, 1.0f));
            if (x >= mesh_x1 && x <= mesh_x2 && y >= mesh_y1 && y <= mesh_y2 &&
                ((x + y) % pitch == 0 || (x - y + 4 * pitch * h) % pitch == 0)) {
                c = mix_rgb(c, kMesh, 0.85f);
            }
            if (x >= row.pad_x && x < row.pad_x + gw && y >= row.pad_y && y < row.pad_y + gh) {
                const float gx = fx / u - static_cast<float>(row.pad_x) / u - half_stroke;
                const float gy = fy / u - static_cast<float>(row.pad_y) / u - half_stroke;
                float nearest = 1e9f;
                for (int d = 0; d < 10; d++) {
                    nearest = std::min(nearest, stroke_digit_distance(d, gx, gy));
                }
                const float wire = edge_coverage((half_stroke - nearest) * u);
                c = mix_rgb(c, kWire, wire);
            }
            // The highlight down the glass's left side, fading toward the socket.
            const float across = (fx - streak_x) / streak_w;
            const float along = 1.0f - 0.6f * fy / static_cast<float>(socket_y);
            const float streak = std::exp(-across * across) * along;
            if (streak > 0.01f) {
                c = add_glow(c, kGlassStreak, to_amount(streak));
            }
            base[static_cast<size_t>(y) * w + x] = mix_rgb(Rgb{}, c, cov);
        }
    }

    // The socket: a dark metal cap with a lit top edge and rounded bottom corners.
    const int32_t corner = std::max<int32_t>(1, px(kSocketCorner, unit));
    const auto socket_h = static_cast<float>(std::max<int32_t>(1, h - socket_y));
    for (int32_t y = socket_y; y < h; y++) {
        const Rgb shade = y == socket_y ? kSocketEdge
                                        : mix_rgb(kSocketTop, kSocketBottom,
                                                  static_cast<float>(y - socket_y) / socket_h);
        for (int32_t x = inset; x < w - inset; x++) {
            // Coverage of the rounded bottom corners.
            const int32_t ccx = std::clamp(x, inset + corner, w - inset - corner - 1);
            const int32_t ccy = std::min(y, h - corner - 1);
            float cov = 1.0f;
            if (ccx != x && ccy != y) {
                cov = edge_coverage(
                    static_cast<float>(corner) -
                    std::hypot(static_cast<float>(x - ccx), static_cast<float>(y - ccy)));
            }
            base[static_cast<size_t>(y) * w + x] = mix_rgb(Rgb{}, shade, cov);
        }
    }

    for (char d = '0'; d <= '9'; d++) {
        ensure(d);
    }
}

void NixieTubeArt::ensure(char c) {
    const int i = stroke_glyph_index(c);
    if (i < 0 || glow_w <= 0 || !glow[static_cast<size_t>(i)].empty()) {
        return;
    }
    std::vector<uint8_t>& map = glow[static_cast<size_t>(i)];
    map.assign(static_cast<size_t>(glow_w) * glow_h, 0);
    const auto u = static_cast<float>(unit);
    const float half_stroke = kNixieTubeGeometry.stroke / 2.0f;
    const float half_px = half_stroke * u;
    const auto reach = static_cast<float>(halo);
    const float falloff = std::max(1.0f, kHaloFalloff * u);
    for (int32_t y = 0; y < glow_h; y++) {
        const float gy = (static_cast<float>(y - halo) + 0.5f) / u - half_stroke;
        for (int32_t x = 0; x < glow_w; x++) {
            const float gx = (static_cast<float>(x - halo) + 0.5f) / u - half_stroke;
            const float d = stroke_glyph_distance(c, gx, gy) * u;
            map[static_cast<size_t>(y) * glow_w + x] =
                unit_to_u8(glow_at(d, half_px, reach, falloff));
        }
    }
}

const uint8_t* NixieTubeArt::glow_of(char c) const {
    const int i = stroke_glyph_index(c);
    if (i < 0 || glow[static_cast<size_t>(i)].empty()) {
        return nullptr;
    }
    return glow[static_cast<size_t>(i)].data();
}

size_t NixieTubeArt::heap_bytes() const {
    size_t total = buffer_bytes(base);
    for (const std::vector<uint8_t>& g : glow) {
        total += buffer_bytes(g);
    }
    return total;
}

void NixieTubeArt::clear() {
    release_buffer(base);
    for (std::vector<uint8_t>& g : glow) {
        release_buffer(g);
    }
    unit = 0;
    w = 0;
    h = 0;
}

// ---------- NixieClockSim ----------

void NixieClockSim::init(FrameTarget& frame, size_t level, const FlipFace& face) {
    level_ = std::min(level, kNixieClockLevelCount - 1);
    frame_w_ = static_cast<int32_t>(frame.w);
    frame_h_ = static_cast<int32_t>(frame.h);
    drift_.reset();
    drift_shape_ = face.shape;
    drift_sub_shape_ = face.sub_shape;
    drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_, drift_sub_shape_,
                                         face.sub_colons, kNixieTubeGeometry),
                     frame_w_, frame_h_);
    since_cycle_ms_ = 0;
    cycle_ms_ = kNixieCycleMs;
    PixelWriter(frame).fill(kBackground);
    ink_ = print_state_color(face.mode, kNeon);
    build_ramp();
    layout_ = layout_flip_cards(frame_w_, frame_h_, face.shape, face.sub_shape, 0, 0,
                                face.sub_colons, kNixieTubeGeometry);
    build_art(face);
    for (size_t i = 0; i < anims_.size(); i++) {
        const char c = face_char(face, i);
        anims_[i] = {c, c, kNixieFadeMs};
    }
    for (size_t i = 0; i < tube_count(); i++) {
        draw_tube(frame, i);
    }
    draw_colons(frame);
}

void NixieClockSim::build_ramp() {
    for (size_t v = 0; v < ramp_.size(); v++) {
        const float t = static_cast<float>(v) / 255.0f;
        // The halo is the ink, dim; the wire's core runs hotter, toward white.
        const Rgb glow = scale_rgb_by(ink_, std::min(1.0f, t * 1.5f));
        const float hot = std::clamp((t - 0.6f) / 0.4f, 0.0f, 1.0f);
        const auto lift = [](uint8_t c, float k) {
            return to_u8(static_cast<float>(c) + (255.0f - static_cast<float>(c)) * k);
        };
        const Rgb core{lift(ink_.r, kCoreLiftRg), lift(ink_.g, kCoreLiftRg),
                       lift(ink_.b, kCoreLiftB)};
        ramp_[v] = mix_rgb(glow, core, hot * hot);
    }
}

void NixieClockSim::build_art(const FlipFace& face) {
    const DirtyRect& tube = layout_.cards[0];
    const int32_t tw = tube.x2 - tube.x1 + 1;
    const int32_t th = tube.y2 - tube.y1 + 1;
    if (art_.unit != layout_.main.unit || art_.w != tw || art_.h != th) {
        art_.build(layout_.main, tw, th);
    }
    if (layout_.sub_count > 0) {
        const DirtyRect& small = layout_.cards[layout_.card_count];
        const int32_t sw = small.x2 - small.x1 + 1;
        const int32_t sh = small.y2 - small.y1 + 1;
        if (sub_art_.unit != layout_.small.unit || sub_art_.w != sw || sub_art_.h != sh) {
            sub_art_.build(layout_.small, sw, sh);
        }
    }
    for (size_t i = 0; i < layout_.card_count; i++) {
        art_.ensure(face.cards[i].digit);
    }
    for (size_t i = 0; i < layout_.sub_count; i++) {
        sub_art_.ensure(face.sub[i].digit);
    }
}

void NixieClockSim::redraw_tube(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const {
    if (i >= tube_count()) {
        return;
    }
    draw_tube(frame, i);
    dirty.push_back(layout_.cards[i]);
}

void NixieClockSim::request_level(size_t level) {
    level_ = std::min(level, kNixieClockLevelCount - 1);
}

DirtyRect NixieClockSim::meridiem_box() const {
    if (layout_.card_count == 0 || art_.w <= 0) {
        return {};
    }
    const DirtyRect& tube = layout_.cards[0];
    const int32_t pad = std::max<int32_t>(1, art_.inset * 2);
    return {tube.x1 + pad, tube.y1 + art_.socket_y + 2, tube.x2 - pad, tube.y2 - 1};
}

DirtyRect NixieClockSim::glow_box(size_t i) const {
    if (i >= tube_count()) {
        return {};
    }
    const NixieTubeArt& art = art_of(i);
    const DirtyRect& tube = layout_.cards[i];
    const int32_t x1 = tube.x1 + std::max<int32_t>(0, art.glow_x);
    const int32_t y1 = tube.y1 + std::max<int32_t>(0, art.glow_y);
    return DirtyRect{x1, y1, std::min(tube.x2, tube.x1 + art.glow_x + art.glow_w - 1),
                     std::min(tube.y2, tube.y1 + art.glow_y + art.glow_h - 1)}
        .clipped(frame_w_, frame_h_);
}

bool NixieClockSim::fading() const {
    for (size_t i = 0; i < tube_count(); i++) {
        if (anims_[i].elapsed_ms < kNixieFadeMs) {
            return true;
        }
    }
    return false;
}

float NixieClockSim::fade_progress(size_t i) const {
    if (i >= anims_.size()) {
        return 1.0f;
    }
    return static_cast<float>(std::min(anims_[i].elapsed_ms, kNixieFadeMs)) /
           static_cast<float>(kNixieFadeMs);
}

char NixieClockSim::lit(size_t i) const {
    return i < anims_.size() ? anims_[i].to : '\0';
}

size_t NixieClockSim::heap_bytes() const {
    return art_.heap_bytes() + sub_art_.heap_bytes();
}

void NixieClockSim::clear() {
    art_.clear();
    sub_art_.clear();
    layout_ = {};
}

bool NixieClockSim::step_cycle(uint32_t dt_ms, const FlipFace& face) {
    if (!kNixieClockLevels[level_].fade || face.mode != ClockMode::CLOCK) {
        const bool was = cycling();
        cycle_ms_ = kNixieCycleMs;
        since_cycle_ms_ = 0;
        return was;
    }
    if (cycling()) {
        cycle_ms_ = std::min(cycle_ms_ + dt_ms, kNixieCycleMs);
        return true;
    }
    since_cycle_ms_ += dt_ms;
    if (since_cycle_ms_ >= kNixieCycleEveryMs && !fading()) {
        since_cycle_ms_ = 0;
        cycle_ms_ = 0;
        return true;
    }
    return false;
}

void NixieClockSim::step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
                         std::vector<DirtyRect>& dirty) {
    dirty.clear();
    const bool fade = kNixieClockLevels[level_].fade;

    if (face.shape != drift_shape_ || face.sub_shape != drift_sub_shape_) {
        drift_shape_ = face.shape;
        drift_sub_shape_ = face.sub_shape;
        drift_.set_range(flip_centred_bounds(frame_w_, frame_h_, drift_shape_, drift_sub_shape_,
                                             face.sub_colons, kNixieTubeGeometry),
                         frame_w_, frame_h_);
    }
    const bool moved = drift_.step(dt_ms);
    const Rgb ink = print_state_color(face.mode, kNeon);

    if (shape_differs(face, layout_) || moved || !(ink == ink_)) {
        // A new layout shows the face as it stands: a running fade or cycle ends here.
        cycle_ms_ = kNixieCycleMs;
        for (size_t i = 0; i < anims_.size(); i++) {
            const char c = face_char(face, i);
            anims_[i] = {c, c, kNixieFadeMs};
        }
        if (!(ink == ink_)) {
            ink_ = ink;
            build_ramp();
        }
        relayout(frame, face, dirty);
        return;
    }

    const bool cycling_now = step_cycle(dt_ms, face) && cycling();
    for (size_t i = 0; i < tube_count(); i++) {
        TubeAnim& anim = anims_[i];
        char want = face_char(face, i);
        if (cycling_now && i < layout_.card_count) {
            const uint32_t turn = cycle_ms_ / kNixieCycleStepMs + static_cast<uint32_t>(i);
            want = static_cast<char>('0' + turn % 10);
        }
        bool redraw = false;
        if (want != anim.to) {
            (i < layout_.card_count ? art_ : sub_art_).ensure(want);
            anim.from = anim.to;
            anim.to = want;
            // The cycle flashes each numeral at once; a real change fades.
            anim.elapsed_ms = fade && !cycling_now ? 0 : kNixieFadeMs;
            redraw = true;
        } else if (anim.elapsed_ms < kNixieFadeMs) {
            anim.elapsed_ms = fade ? std::min(anim.elapsed_ms + dt_ms, kNixieFadeMs) : kNixieFadeMs;
            redraw = true;
        }
        if (redraw) {
            draw_glow(frame, i);
            const DirtyRect box = glow_box(i);
            if (!box.empty()) {
                dirty.push_back(box);
            }
        }
    }
}

void NixieClockSim::relayout(FrameTarget& frame, const FlipFace& face,
                             std::vector<DirtyRect>& dirty) {
    DirtyRect area = layout_.bounds();
    PixelWriter(frame).fill_rect(area, kBackground);
    layout_ = layout_flip_cards(frame_w_, frame_h_, face.shape, face.sub_shape, drift_.dx,
                                drift_.dy, face.sub_colons, kNixieTubeGeometry);
    build_art(face);
    for (size_t i = 0; i < tube_count(); i++) {
        draw_tube(frame, i);
    }
    draw_colons(frame);
    area.add(layout_.bounds());
    area = area.clipped(frame_w_, frame_h_);
    if (!area.empty()) {
        dirty.push_back(area);
    }
}

void NixieClockSim::draw_tube(FrameTarget& frame, size_t i) const {
    const NixieTubeArt& art = art_of(i);
    const DirtyRect& tube = layout_.cards[i];
    if (art.base.empty()) {
        return;
    }
    PixelWriter writer(frame);
    const DirtyRect glow = glow_box(i);
    for (int32_t y = 0; y < art.h; y++) {
        for (int32_t x = 0; x < art.w; x++) {
            const int32_t fx = tube.x1 + x;
            const int32_t fy = tube.y1 + y;
            if (fx >= glow.x1 && fx <= glow.x2 && fy >= glow.y1 && fy <= glow.y2) {
                continue;
            }
            if (writer.contains(fx, fy)) {
                writer.put_dithered(fx, fy, art.base[static_cast<size_t>(y) * art.w + x]);
            }
        }
    }
    draw_glow(frame, i);
}

void NixieClockSim::draw_glow(FrameTarget& frame, size_t i) const {
    const NixieTubeArt& art = art_of(i);
    const DirtyRect box = glow_box(i);
    if (art.base.empty() || box.empty()) {
        return;
    }
    const DirtyRect& tube = layout_.cards[i];
    const TubeAnim& anim = anims_[i];
    const float p = fade_progress(i);
    const uint8_t* to = art.glow_of(anim.to);
    const uint8_t* from = p < 1.0f && anim.from != anim.to ? art.glow_of(anim.from) : nullptr;
    const uint32_t to_amount_v = to_amount(p);
    const uint32_t from_amount_v = to_amount(1.0f - p);
    PixelWriter writer(frame);
    for (int32_t fy = box.y1; fy <= box.y2; fy++) {
        const int32_t ty = fy - tube.y1;
        const int32_t gy = ty - art.glow_y;
        for (int32_t fx = box.x1; fx <= box.x2; fx++) {
            const int32_t tx = fx - tube.x1;
            const int32_t gx = tx - art.glow_x;
            Rgb c = art.base[static_cast<size_t>(ty) * art.w + tx];
            const size_t g = static_cast<size_t>(gy) * art.glow_w + gx;
            if (to != nullptr && to[g] != 0) {
                c = add_glow(c, ramp_[to[g]], to_amount_v);
            }
            if (from != nullptr && from[g] != 0) {
                c = add_glow(c, ramp_[from[g]], from_amount_v);
            }
            writer.put_dithered(fx, fy, c);
        }
    }
}

void NixieClockSim::draw_colons(FrameTarget& frame) const {
    PixelWriter writer(frame);
    for (size_t c = 0; c < layout_.colon_count; c++) {
        const DirtyRect& box = layout_.colons[c];
        const bool main = c < layout_.main_colon_count;
        const FlipRowMetrics& row = main ? layout_.main : layout_.small;
        const auto u = static_cast<float>(row.unit);
        const float r = std::max(1.0f, kDotR * u);
        const float reach = r * 2.2f;
        const float falloff = std::max(0.7f, r * 0.7f);
        const float gh = (kStrokeDigitPathH + kNixieTubeGeometry.stroke) * u;
        const float cx = static_cast<float>(box.x1 + box.x2 + 1) / 2.0f;
        for (const float share : kDotRows) {
            const float cy = static_cast<float>(box.y1 + row.pad_y) + gh * share;
            const auto y1 = static_cast<int32_t>(std::floor(cy - r - reach));
            const auto y2 = static_cast<int32_t>(std::ceil(cy + r + reach));
            const auto x1 = static_cast<int32_t>(std::floor(cx - r - reach));
            const auto x2 = static_cast<int32_t>(std::ceil(cx + r + reach));
            for (int32_t y = std::max(y1, box.y1); y <= std::min(y2, box.y2); y++) {
                for (int32_t x = std::max(x1, box.x1); x <= std::min(x2, box.x2); x++) {
                    const float d = std::hypot(static_cast<float>(x) + 0.5f - cx,
                                               static_cast<float>(y) + 0.5f - cy);
                    const uint8_t v = unit_to_u8(glow_at(d, r, reach, falloff));
                    if (v != 0 && writer.contains(x, y)) {
                        writer.put_dithered(x, y, ramp_[v]);
                    }
                }
            }
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
