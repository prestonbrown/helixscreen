// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_analog_paint.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

constexpr float kDegToRad = 0.017453292f;
constexpr float kRadToDeg = 57.29578f;

/// Angle of (dx, dy) from the centre, degrees clockwise from 12 o'clock, 0 to 360.
float angle_of(float dx, float dy) {
    float a = std::atan2(dx, -dy) * kRadToDeg;
    return a < 0.0f ? a + 360.0f : a;
}

/// `a` wrapped into 0 to 360.
float wrap360(float a) {
    a = std::fmod(a, 360.0f);
    return a < 0.0f ? a + 360.0f : a;
}

/// Signed distance, in px at radius `dist`, from the angular span `deg0` + `span` (clockwise):
/// negative inside, measured along the arc to the nearer end.
float angular_distance(float theta, float deg0, float span, float dist) {
    if (span >= 360.0f) {
        return -1e6f;
    }
    const float rel = wrap360(theta - deg0);
    float deg;
    if (rel <= span) {
        deg = -std::min(rel, span - rel);
    } else {
        deg = std::min(rel - span, 360.0f - rel);
    }
    return deg * kDegToRad * std::max(dist, 1.0f);
}

} // namespace

DirtyRect apaint_box(float x1, float y1, float x2, float y2, float pad) {
    return {static_cast<int32_t>(std::floor(std::min(x1, x2) - pad)),
            static_cast<int32_t>(std::floor(std::min(y1, y2) - pad)),
            static_cast<int32_t>(std::ceil(std::max(x1, x2) + pad)),
            static_cast<int32_t>(std::ceil(std::max(y1, y2) + pad))};
}

DirtyRect apaint_segment_box(APt a, APt b, float pad) {
    return apaint_box(a.x, a.y, b.x, b.y, pad);
}

APainter::APainter(const FrameTarget& frame, const DirtyRect& clip)
    : frame_(frame),
      clip_(clip.clipped(static_cast<int32_t>(frame.w), static_cast<int32_t>(frame.h))),
      writer_(frame) {}

template <typename Cov>
void APainter::shade(float x1, float y1, float x2, float y2, Rgb color, float alpha, Cov&& cov) {
    if (clip_.empty() || alpha <= 0.0f) {
        return;
    }
    DirtyRect box = apaint_box(x1, y1, x2, y2, 1.0f);
    box = {std::max(box.x1, clip_.x1), std::max(box.y1, clip_.y1), std::min(box.x2, clip_.x2),
           std::min(box.y2, clip_.y2)};
    if (box.empty()) {
        return;
    }
    for (int32_t y = box.y1; y <= box.y2; y++) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int32_t x = box.x1; x <= box.x2; x++) {
            const float c = cov(static_cast<float>(x) + 0.5f, py);
            if (c <= 0.0f) {
                continue;
            }
            const float a = c * alpha;
            if (a >= 0.999f) {
                writer_.put_dithered(x, y, color);
            } else {
                writer_.blend_over(x, y, color, a);
            }
        }
    }
}

void APainter::disc(APt c, float r, Rgb color, const AEdge& e) {
    const float reach = r + e.grow + e.soft;
    shade(c.x - reach, c.y - reach, c.x + reach, c.y + reach, color, e.alpha,
          [&](float px, float py) { return edge_cover(std::hypot(px - c.x, py - c.y) - r, e); });
}

void APainter::ring(APt c, float r_in, float r_out, Rgb color, const AEdge& e) {
    arc(c, r_in, r_out, 0.0f, 360.0f, color, e);
}

void APainter::arc(APt c, float r_in, float r_out, float deg0, float deg1, Rgb color,
                   const AEdge& e) {
    const float span = deg1 - deg0;
    if (span <= 0.0f) {
        return;
    }
    const float reach = r_out + e.grow + e.soft;
    shade(c.x - reach, c.y - reach, c.x + reach, c.y + reach, color, e.alpha,
          [&](float px, float py) {
              const float dx = px - c.x;
              const float dy = py - c.y;
              const float dist = std::sqrt(dx * dx + dy * dy);
              const float dr = std::max(r_in - dist, dist - r_out);
              if (span >= 360.0f) {
                  return edge_cover(dr, e);
              }
              if (dr - e.grow > e.soft) {
                  return 0.0f;
              }
              const float da = angular_distance(angle_of(dx, dy), deg0, span, dist);
              return edge_cover(std::max(dr, da), e);
          });
}

void APainter::wedge(APt c, float r, float deg0, float deg1, Rgb color, const AEdge& e) {
    const float span = deg1 - deg0;
    if (span <= 0.0f) {
        return;
    }
    const float reach = r + e.grow + e.soft;
    shade(c.x - reach, c.y - reach, c.x + reach, c.y + reach, color, e.alpha,
          [&](float px, float py) {
              const float dx = px - c.x;
              const float dy = py - c.y;
              const float dist = std::sqrt(dx * dx + dy * dy);
              const float dr = dist - r;
              const float da = angular_distance(angle_of(dx, dy), deg0, span, dist);
              return edge_cover(std::max(dr, da), e);
          });
}

void APainter::segment(APt a, APt b, float w, Rgb color, bool round, const AEdge& e) {
    if (!round) {
        taper(a, b, w, w, color, e);
        return;
    }
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float len2 = std::max(vx * vx + vy * vy, 1e-6f);
    const float pad = w * 0.5f + e.grow + e.soft;
    shade(std::min(a.x, b.x) - pad, std::min(a.y, b.y) - pad, std::max(a.x, b.x) + pad,
          std::max(a.y, b.y) + pad, color, e.alpha, [&](float px, float py) {
              const float t = std::clamp(((px - a.x) * vx + (py - a.y) * vy) / len2, 0.0f, 1.0f);
              const float d = std::hypot(px - (a.x + vx * t), py - (a.y + vy * t)) - w * 0.5f;
              return edge_cover(d, e);
          });
}

void APainter::taper(APt a, APt b, float w0, float w1, Rgb color, const AEdge& e) {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float len = std::max(std::sqrt(vx * vx + vy * vy), 1e-3f);
    const float ux = vx / len;
    const float uy = vy / len;
    const float pad = std::max(w0, w1) * 0.5f + e.grow + e.soft;
    shade(std::min(a.x, b.x) - pad, std::min(a.y, b.y) - pad, std::max(a.x, b.x) + pad,
          std::max(a.y, b.y) + pad, color, e.alpha, [&](float px, float py) {
              const float rx = px - a.x;
              const float ry = py - a.y;
              const float u = rx * ux + ry * uy;
              const float v = std::fabs(-rx * uy + ry * ux);
              const float t = std::clamp(u / len, 0.0f, 1.0f);
              const float hw = (w0 + (w1 - w0) * t) * 0.5f;
              const float d = std::max(v - hw, std::max(-u, u - len));
              return edge_cover(d, e);
          });
}

void APainter::radial_disc(APt c, float r, Rgb inner, Rgb outer, float fx, float fy) {
    if (clip_.empty()) {
        return;
    }
    const APt focus{c.x + fx * r, c.y + fy * r};
    const float reach = r + 1.0f;
    DirtyRect box = apaint_box(c.x - reach, c.y - reach, c.x + reach, c.y + reach, 0.0f);
    box = {std::max(box.x1, clip_.x1), std::max(box.y1, clip_.y1), std::min(box.x2, clip_.x2),
           std::min(box.y2, clip_.y2)};
    const float span = r * (1.0f + std::hypot(fx, fy));
    for (int32_t y = box.y1; y <= box.y2; y++) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int32_t x = box.x1; x <= box.x2; x++) {
            const float px = static_cast<float>(x) + 0.5f;
            const float cov = edge_cover(std::hypot(px - c.x, py - c.y) - r, AEdge{});
            if (cov <= 0.0f) {
                continue;
            }
            const float t = std::min(1.0f, std::hypot(px - focus.x, py - focus.y) / span);
            const Rgb col = mix_rgb(inner, outer, t);
            if (cov >= 0.999f) {
                writer_.put_dithered(x, y, col);
            } else {
                writer_.blend_over(x, y, col, cov);
            }
        }
    }
}

void APainter::steel_ring(APt c, float r_in, float r_out, Rgb light, Rgb dark) {
    if (clip_.empty()) {
        return;
    }
    const float reach = r_out + 1.0f;
    DirtyRect box = apaint_box(c.x - reach, c.y - reach, c.x + reach, c.y + reach, 0.0f);
    box = {std::max(box.x1, clip_.x1), std::max(box.y1, clip_.y1), std::min(box.x2, clip_.x2),
           std::min(box.y2, clip_.y2)};
    for (int32_t y = box.y1; y <= box.y2; y++) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int32_t x = box.x1; x <= box.x2; x++) {
            const float px = static_cast<float>(x) + 0.5f;
            const float dx = px - c.x;
            const float dy = py - c.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float cov = edge_cover(std::max(r_in - dist, dist - r_out), AEdge{});
            if (cov <= 0.0f) {
                continue;
            }
            // Brightest facing the top left (315 degrees), with a second, smaller sheen
            // opposite, as a turned bezel catches the light twice.
            const float rel = (angle_of(dx, dy) - 315.0f) * kDegToRad;
            const float k = 0.5f + 0.4f * std::cos(rel) + 0.1f * std::cos(2.0f * rel);
            const Rgb col = mix_rgb(dark, light, std::clamp(k, 0.0f, 1.0f));
            if (cov >= 0.999f) {
                writer_.put_dithered(x, y, col);
            } else {
                writer_.blend_over(x, y, col, cov);
            }
        }
    }
}

void APainter::trail(APt c, float r_in, float r_out, float deg_head, float span, Rgb color,
                     float alpha) {
    if (span <= 0.0f || alpha <= 0.0f) {
        return;
    }
    // The trail's box: the band's points at the head, the tail and every quarter between.
    DirtyRect box;
    for (int i = 0; i <= 8; i++) {
        const float deg = deg_head - span * static_cast<float>(i) / 8.0f;
        for (const float r : {r_in, r_out}) {
            const APt p = polar(c.x, c.y, r, deg);
            box.add(apaint_box(p.x, p.y, p.x, p.y, 2.0f));
        }
    }
    shade(static_cast<float>(box.x1), static_cast<float>(box.y1), static_cast<float>(box.x2),
          static_cast<float>(box.y2), color, alpha, [&](float px, float py) {
              const float dx = px - c.x;
              const float dy = py - c.y;
              const float dist = std::sqrt(dx * dx + dy * dy);
              const float radial = edge_cover(std::max(r_in - dist, dist - r_out), AEdge{});
              if (radial <= 0.0f) {
                  return 0.0f;
              }
              const float behind = wrap360(deg_head - angle_of(dx, dy));
              if (behind > span) {
                  return 0.0f;
              }
              const float f = 1.0f - behind / span;
              return radial * f * f;
          });
}

void APainter::round_rect(float x1, float y1, float x2, float y2, float radius, Rgb color,
                          const AEdge& e) {
    const float cx = (x1 + x2) * 0.5f;
    const float cy = (y1 + y2) * 0.5f;
    const float hx = (x2 - x1) * 0.5f;
    const float hy = (y2 - y1) * 0.5f;
    const float pad = e.grow + e.soft;
    shade(x1 - pad, y1 - pad, x2 + pad, y2 + pad, color, e.alpha, [&](float px, float py) {
        const float qx = std::fabs(px - cx) - hx + radius;
        const float qy = std::fabs(py - cy) - hy + radius;
        const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
        const float d = outside + std::min(std::max(qx, qy), 0.0f) - radius;
        return edge_cover(d, e);
    });
}

void APainter::fill(Rgb color, float alpha) {
    shade(static_cast<float>(clip_.x1), static_cast<float>(clip_.y1), static_cast<float>(clip_.x2),
          static_cast<float>(clip_.y2), color, alpha, [](float, float) { return 1.0f; });
}

void APainter::coverage(const uint8_t* cov, int32_t w, int32_t h, int32_t x, int32_t y, Rgb color,
                        float alpha) {
    for (int32_t gy = 0; gy < h; gy++) {
        const int32_t fy = y + gy;
        if (fy < clip_.y1 || fy > clip_.y2) {
            continue;
        }
        for (int32_t gx = 0; gx < w; gx++) {
            const int32_t fx = x + gx;
            const uint8_t c = cov[static_cast<size_t>(gy) * static_cast<size_t>(w) + gx];
            if (c == 0 || fx < clip_.x1 || fx > clip_.x2) {
                continue;
            }
            writer_.blend_over(fx, fy, color, alpha * static_cast<float>(c) / 255.0f);
        }
    }
}

} // namespace helix::ui
