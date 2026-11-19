// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tube_router.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

using namespace helix::ui::tuberoute;
using helix::ui::pathgeo::FilamentPath;
using helix::ui::pathgeo::path_length;
using helix::ui::pathgeo::path_point_at;

namespace {

// ---- Scenarios ------------------------------------------------------------

// Buses with their own storage, so a scenario can be returned by value.
struct Scenario {
    std::string name;
    std::vector<std::vector<PathPoint>> src;
    std::vector<std::vector<PathPoint>> dst;
    std::vector<Bus> buses;
    RouterConfig cfg;

    void add(std::vector<PathPoint> s, std::vector<PathPoint> d) {
        src.push_back(std::move(s));
        dst.push_back(std::move(d));
    }
    const Bus* bind() {
        buses.clear();
        for (size_t i = 0; i < src.size(); ++i)
            buses.push_back(Bus{src[i].data(), dst[i].data(), (int)src[i].size()});
        return buses.data();
    }
    int bus_count() const {
        return (int)src.size();
    }
    int trace_count() const {
        int n = 0;
        for (auto& s : src)
            n += (int)s.size();
        return n;
    }
};

std::vector<PathPoint> row(float x0, float step, int n, float y) {
    std::vector<PathPoint> v;
    for (int i = 0; i < n; ++i)
        v.push_back({x0 + step * (float)i, y});
    return v;
}

std::vector<PathPoint> entries(float cx, float y, int n, float pitch) {
    std::vector<PathPoint> v((size_t)n);
    spread_entries(cx, y, n, pitch, v.data());
    return v;
}

RouterConfig base_cfg() {
    RouterConfig c;
    c.clearance = 12.0f;
    c.fillet_r = 8.0f;
    c.lead_in = 10.0f;
    c.lead_out = 10.0f;
    return c;
}

Scenario bt4_hub() {
    Scenario s{"bt4_hub", {}, {}, {}, base_cfg()};
    s.add(row(200, 60, 4, 40), entries(290, 300, 4, 14));
    return s;
}

Scenario bt8_narrow_hub() {
    Scenario s{"bt8_narrow_hub", {}, {}, {}, base_cfg()};
    s.add(row(100, 60, 8, 40), entries(310, 400, 8, 12));
    return s;
}

Scenario two_units_shared_hub() {
    Scenario s{"two_units_shared_hub", {}, {}, {}, base_cfg()};
    s.cfg.group_gap = 24.0f;
    auto hub = entries(320, 380, 8, 14);
    s.add(row(100, 40, 4, 40), {hub.begin(), hub.begin() + 4});
    s.add(row(420, 40, 4, 40), {hub.begin() + 4, hub.end()});
    return s;
}

Scenario four_spread_heads() {
    Scenario s{"four_spread_heads", {}, {}, {}, base_cfg()};
    s.add(row(300, 30, 4, 40), {{120, 380}, {280, 380}, {440, 380}, {600, 380}});
    return s;
}

Scenario mixed_direct_and_hub() {
    Scenario s{"mixed_direct_and_hub", {}, {}, {}, base_cfg()};
    s.add(row(200, 40, 2, 40), {{100, 400}, {170, 400}});
    s.add(row(280, 40, 2, 40), entries(400, 300, 2, 14));
    return s;
}

Scenario fleet12_hub() {
    Scenario s{"fleet12_hub", {}, {}, {}, base_cfg()};
    s.cfg.group_gap = 20.0f;
    auto hub = entries(400, 440, 12, 12);
    for (int u = 0; u < 3; ++u)
        s.add(row(60.0f + 280.0f * (float)u, 40, 4, 40),
              {hub.begin() + 4 * u, hub.begin() + 4 * (u + 1)});
    return s;
}

// ---- Independent checks -----------------------------------------------------

// Points every half pixel along the filleted path the caller draws.
std::vector<PathPoint> samples(const Trace& t, float fillet_r) {
    FilamentPath p;
    trace_to_path(t, fillet_r, p);
    std::vector<PathPoint> v;
    float len = path_length(p);
    for (float d = 0.0f; d <= len; d += 0.5f)
        v.push_back(path_point_at(p, d));
    v.push_back(path_point_at(p, len));
    return v;
}

// Least distance between two traces, to within a quarter pixel.
float trace_gap(const Trace& a, const Trace& b, float fillet_r) {
    auto sa = samples(a, fillet_r);
    auto sb = samples(b, fillet_r);
    float best = 1e9f;
    for (auto& p : sa)
        for (auto& q : sb)
            best = std::min(best, std::hypot(p.x - q.x, p.y - q.y));
    return best;
}

float min_gap(const std::vector<Trace>& t, float fillet_r) {
    float best = 1e9f;
    for (size_t i = 0; i < t.size(); ++i)
        for (size_t j = i + 1; j < t.size(); ++j)
            best = std::min(best, trace_gap(t[i], t[j], fillet_r));
    return best;
}

// Every leg vertical, horizontal or at the trace's slope.
bool octilinear(const Trace& t) {
    for (int i = 0; i + 1 < t.count; ++i) {
        float dx = t.pts[i + 1].x - t.pts[i].x;
        float dy = t.pts[i + 1].y - t.pts[i].y;
        if (std::fabs(dx) < 1e-4f || std::fabs(dy) < 1e-4f)
            continue;
        if (std::fabs(std::fabs(dy / dx) - t.slope) > 1e-3f || t.slope <= 0.0f)
            return false;
    }
    return true;
}

// X where the polyline crosses height @p y, or NAN when it does not.
float x_at(const Trace& t, float y) {
    for (int i = 0; i + 1 < t.count; ++i) {
        PathPoint a = t.pts[i], b = t.pts[i + 1];
        if (y < std::min(a.y, b.y) || y > std::max(a.y, b.y) || a.y == b.y)
            continue;
        return a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y);
    }
    return NAN;
}

// At every shared height, trace i stays left of trace i + 1.
bool keeps_order(const std::vector<Trace>& t) {
    for (float y = 0.0f; y < 500.0f; y += 1.0f) {
        float prev = -1e9f;
        for (auto& tr : t) {
            float x = x_at(tr, y);
            if (std::isnan(x))
                continue;
            if (x <= prev)
                return false;
            prev = x;
        }
    }
    return true;
}

// The diagonal leg's line offset across a 45-degree ribbon: x - y for a
// rightward jog, x + y for a leftward one.
float diag_offset(const Trace& t) {
    REQUIRE(t.count == 4);
    float dir = t.pts[2].x > t.pts[1].x ? 1.0f : -1.0f;
    return t.pts[1].x - dir * t.pts[1].y;
}

struct Routed {
    RouteStatus status;
    RouteReport report;
    std::vector<Trace> traces;
};

Routed route(Scenario& s) {
    Routed r;
    r.traces.resize((size_t)s.trace_count());
    r.status = plan_routes(s.bind(), s.bus_count(), s.cfg, r.traces.data(), (int)r.traces.size(),
                           &r.report);
    return r;
}

void check_well_formed(Scenario& s, const Routed& r) {
    INFO(s.name);
    REQUIRE(r.status == RouteStatus::Ok);
    int k = 0;
    for (size_t b = 0; b < s.src.size(); ++b) {
        for (size_t l = 0; l < s.src[b].size(); ++l, ++k) {
            const Trace& t = r.traces[(size_t)k];
            CHECK(t.bus == (int)b);
            CHECK(t.lane == (int)l);
            CHECK(t.pts[0].x == s.src[b][l].x);
            CHECK(t.pts[0].y == s.src[b][l].y);
            CHECK(t.pts[t.count - 1].y == s.dst[b][l].y);
            CHECK(std::fabs(t.pts[t.count - 1].x - s.dst[b][l].x) < 0.5f);
            CHECK(t.bends() <= 2);
            CHECK(octilinear(t));
        }
    }
    CHECK(keeps_order(r.traces));
    CHECK(min_gap(r.traces, s.cfg.fillet_r) >= s.cfg.clearance - 0.3f);
}

} // namespace

// ---- Properties on every scenario -------------------------------------------

TEST_CASE("tube router: every scenario routes octilinear, ordered and clear", "[tube_router]") {
    Scenario all[] = {bt4_hub(),           bt8_narrow_hub(),       two_units_shared_hub(),
                      four_spread_heads(), mixed_direct_and_hub(), fleet12_hub()};
    for (auto& s : all) {
        Routed r = route(s);
        check_well_formed(s, r);
        CHECK(r.report.slope == 1.0f);
    }
}

TEST_CASE("tube router: a lane over its sink drops straight with no bends", "[tube_router]") {
    Scenario s = four_spread_heads();
    s.dst[0][1] = {330, 380}; // lane 1 sits right over its head
    Routed r = route(s);
    REQUIRE(r.status == RouteStatus::Ok);
    CHECK(r.traces[1].count == 2);
    CHECK(r.traces[1].bends() == 0);
    CHECK(r.traces[0].bends() == 2);
}

// ---- Ribbon pitch -----------------------------------------------------------

TEST_CASE("tube router: one bus's parallel jogs sit exactly one bus pitch apart", "[tube_router]") {
    Scenario s = bt8_narrow_hub();
    s.cfg.bus_pitch = 16.0f;
    Routed r = route(s);
    REQUIRE(r.status == RouteStatus::Ok);
    // Lanes 0-3 jog right, 4-7 jog left; within each side the diagonals are
    // parallel lines whose offsets differ by pitch * sqrt(2).
    const float want = 16.0f * std::sqrt(2.0f);
    for (int i = 0; i < 3; ++i)
        CHECK(std::fabs(diag_offset(r.traces[(size_t)i + 1]) - diag_offset(r.traces[(size_t)i])) ==
              Catch::Approx(want).margin(0.01));
    for (int i = 4; i < 7; ++i)
        CHECK(std::fabs(diag_offset(r.traces[(size_t)i + 1]) - diag_offset(r.traces[(size_t)i])) ==
              Catch::Approx(want).margin(0.01));
}

TEST_CASE("tube router: neighboring buses keep the group gap between their jogs", "[tube_router]") {
    // Two units jogging the same way into one hub at the far right.
    Scenario s{"two_units_one_side", {}, {}, {}, base_cfg()};
    s.cfg.group_gap = 30.0f;
    auto hub = entries(400, 400, 8, 14);
    s.add(row(60, 40, 4, 40), {hub.begin(), hub.begin() + 4});
    s.add(row(260, 40, 4, 40), {hub.begin() + 4, hub.end()});
    Routed r = route(s);
    check_well_formed(s, r);
    REQUIRE(r.report.slope == 1.0f);
    const float within = 12.0f * std::sqrt(2.0f);
    const float across = 30.0f * std::sqrt(2.0f);
    CHECK(diag_offset(r.traces[1]) - diag_offset(r.traces[0]) ==
          Catch::Approx(within).margin(0.01));
    CHECK(diag_offset(r.traces[4]) - diag_offset(r.traces[3]) ==
          Catch::Approx(across).margin(0.01));
}

// ---- Alignment ----------------------------------------------------------------

TEST_CASE("tube router: alignment puts the jog under the source or over the sink",
          "[tube_router]") {
    PathPoint src[] = {{100, 0}};
    PathPoint dst[] = {{140, 200}};
    Bus bus{src, dst, 1};
    RouterConfig c;
    c.clearance = 10.0f;
    c.fillet_r = 0.0f;
    c.lead_in = 5.0f;
    c.lead_out = 7.0f;
    Trace t[1];

    c.align = 0.0f;
    REQUIRE(plan_routes(&bus, 1, c, t, 1) == RouteStatus::Ok);
    CHECK(t[0].pts[1].y == Catch::Approx(5.0f));
    CHECK(t[0].pts[2].y == Catch::Approx(45.0f));

    c.align = 1.0f;
    REQUIRE(plan_routes(&bus, 1, c, t, 1) == RouteStatus::Ok);
    CHECK(t[0].pts[2].y == Catch::Approx(193.0f));
    CHECK(t[0].pts[1].y == Catch::Approx(153.0f));
}

// ---- Fallbacks and failures ----------------------------------------------------

TEST_CASE("tube router: a short canvas falls back to the second slope, then horizontal",
          "[tube_router]") {
    // One lane jogging 100 px with 70 px of height: 45 degrees needs 100 + leads.
    PathPoint src[] = {{100, 0}};
    PathPoint dst[] = {{200, 70}};
    Bus bus{src, dst, 1};
    RouterConfig c;
    c.clearance = 10.0f;
    c.fillet_r = 0.0f;
    c.lead_in = 5.0f;
    c.lead_out = 5.0f;
    Trace t[1];
    RouteReport rep;

    c.allow_horizontal = false;
    CHECK(plan_routes(&bus, 1, c, t, 1, &rep) == RouteStatus::NoRoom);
    CHECK(rep.required_bottom == Catch::Approx(110.0f));

    c.alt_slope = 0.5f; // 50 px of jog height fits in 60
    REQUIRE(plan_routes(&bus, 1, c, t, 1, &rep) == RouteStatus::Ok);
    CHECK(rep.slope == 0.5f);
    CHECK(octilinear(t[0]));

    c.alt_slope = 0.0f;
    c.allow_horizontal = true;
    REQUIRE(plan_routes(&bus, 1, c, t, 1, &rep) == RouteStatus::Ok);
    CHECK(rep.slope == 0.0f);
    CHECK(t[0].pts[1].y == t[0].pts[2].y);
}

TEST_CASE("tube router: horizontal jogs in one bus stack a pitch apart", "[tube_router]") {
    Scenario s = bt4_hub();
    for (auto& p : s.dst[0])
        p.y = 120.0f; // too shallow for 45 degrees
    s.cfg.align = 0.0f;
    Routed r = route(s);
    check_well_formed(s, r);
    REQUIRE(r.report.slope == 0.0f);
    // Lanes 0 and 1 jog right: lane 1 (ahead) turns first, lane 0 one pitch below.
    CHECK(r.traces[0].pts[1].y - r.traces[1].pts[1].y == Catch::Approx(12.0f));
    CHECK(r.traces[3].pts[1].y - r.traces[2].pts[1].y == Catch::Approx(12.0f));
}

TEST_CASE("tube router: entries closer than the clearance fail and name the pair",
          "[tube_router]") {
    Scenario s = bt4_hub();
    s.dst[0] = entries(290, 300, 4, 8); // pitch 8 under a 12 px clearance
    Routed r = route(s);
    CHECK(r.status == RouteStatus::Clearance);
    CHECK(r.report.pair_a >= 0);
    CHECK(r.report.pair_b == r.report.pair_a + 1);
}

TEST_CASE("tube router: sinks too high for any slope report NoRoom", "[tube_router]") {
    Scenario s = bt4_hub();
    s.cfg.allow_horizontal = false;
    for (auto& p : s.dst[0])
        p.y = 100.0f;
    Routed r = route(s);
    CHECK(r.status == RouteStatus::NoRoom);
    CHECK(r.report.required_bottom > 100.0f);
}

TEST_CASE("tube router: crossed or malformed input is refused", "[tube_router]") {
    Scenario s = bt4_hub();
    std::swap(s.dst[0][1], s.dst[0][2]);
    CHECK(route(s).status == RouteStatus::OutOfOrder);

    Scenario u = bt4_hub();
    std::swap(u.src[0][0], u.src[0][1]);
    CHECK(route(u).status == RouteStatus::OutOfOrder);

    Scenario up = bt4_hub();
    up.dst[0][0].y = 10.0f; // above its source
    CHECK(route(up).status == RouteStatus::BadInput);

    Scenario small = bt4_hub();
    Trace t[3];
    CHECK(plan_routes(small.bind(), 1, small.cfg, t, 3) == RouteStatus::BadInput);
    CHECK(plan_routes(nullptr, 0, small.cfg, t, 3) == RouteStatus::BadInput);
}

// ---- Height query ----------------------------------------------------------------

TEST_CASE("tube router: span of one jog is lead in + jog + lead out", "[tube_router]") {
    PathPoint src[] = {{100, 20}};
    PathPoint dst[] = {{140, 0}}; // only x is read
    Bus bus{src, dst, 1};
    RouterConfig c;
    c.clearance = 10.0f;
    c.fillet_r = 0.0f;
    c.lead_in = 5.0f;
    c.lead_out = 7.0f;
    CHECK(required_span(&bus, 1, c) == Catch::Approx(52.0f));
    c.slope = 0.5f;
    CHECK(required_span(&bus, 1, c) == Catch::Approx(32.0f));
}

TEST_CASE("tube router: the span query agrees with the planner", "[tube_router]") {
    Scenario all[] = {bt4_hub(), bt8_narrow_hub(), two_units_shared_hub(), fleet12_hub()};
    for (auto& s : all) {
        INFO(s.name);
        s.cfg.allow_horizontal = false;
        float span = required_span(s.bind(), s.bus_count(), s.cfg);
        REQUIRE(span > 0.0f);
        for (auto& d : s.dst)
            for (auto& p : d)
                p.y = 40.0f + span + 0.5f;
        CHECK(route(s).status == RouteStatus::Ok);
        for (auto& d : s.dst)
            for (auto& p : d)
                p.y = 40.0f + span - 2.0f;
        CHECK(route(s).status == RouteStatus::NoRoom);
    }
}

TEST_CASE("tube router: the span grows with clearance, lanes and a narrower hub", "[tube_router]") {
    auto span_of = [](int lanes, float src_step, float hub_pitch, float clearance) {
        auto s = row(100, src_step, lanes, 0);
        auto d = entries(100 + 0.5f * src_step * (float)(lanes - 1), 0, lanes, hub_pitch);
        Bus bus{s.data(), d.data(), lanes};
        RouterConfig c = base_cfg();
        c.clearance = clearance;
        return required_span(&bus, 1, c);
    };
    // Sources 20 apart: past about 14 px of clearance the ribbon has to stagger.
    float prev = 0.0f;
    for (float c = 8.0f; c <= 18.0f; c += 2.0f) {
        float s = span_of(6, 20.0f, 18.0f, c);
        CHECK(s >= prev - 0.01f);
        prev = s;
    }
    CHECK(span_of(6, 20.0f, 18.0f, 18.0f) > span_of(6, 20.0f, 18.0f, 8.0f) + 1.0f);
    prev = 0.0f;
    for (int n = 2; n <= 10; ++n) {
        float s = span_of(n, 50.0f, 14.0f, 12.0f);
        CHECK(s > prev);
        prev = s;
    }
    prev = 0.0f;
    for (float pitch = 40.0f; pitch >= 12.0f; pitch -= 4.0f) {
        float s = span_of(6, 50.0f, pitch, 12.0f);
        CHECK(s >= prev - 0.01f);
        prev = s;
    }
    CHECK(span_of(6, 50.0f, 12.0f, 12.0f) > span_of(6, 50.0f, 40.0f, 12.0f));
}

TEST_CASE("tube router: span query refuses sources closer than the clearance", "[tube_router]") {
    auto s = row(100, 8, 3, 0);
    auto d = row(200, 20, 3, 0);
    Bus bus{s.data(), d.data(), 3};
    RouterConfig c = base_cfg();
    CHECK(required_span(&bus, 1, c) < 0.0f);
}

// ---- Debug render ----------------------------------------------------------------

namespace {

struct Canvas {
    int w, h;
    std::vector<unsigned char> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 3, 0) {
        for (size_t i = 0; i < px.size(); i += 3) {
            px[i] = 30;
            px[i + 1] = 32;
            px[i + 2] = 38;
        }
    }
    void disc(float cx, float cy, float r, const unsigned char* c) {
        for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); ++y)
            for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); ++x) {
                if (x < 0 || y < 0 || x >= w || y >= h)
                    continue;
                if (std::hypot((float)x + 0.5f - cx, (float)y + 0.5f - cy) > r)
                    continue;
                std::copy(c, c + 3, &px[((size_t)y * w + x) * 3]);
            }
    }
    void rect(float x1, float y1, float x2, float y2, const unsigned char* c) {
        for (int y = std::max(0, (int)y1); y < std::min(h, (int)y2); ++y)
            for (int x = std::max(0, (int)x1); x < std::min(w, (int)x2); ++x)
                std::copy(c, c + 3, &px[((size_t)y * w + x) * 3]);
    }
};

void write_cb(void* ctx, void* data, int size) {
    std::fwrite(data, 1, (size_t)size, static_cast<FILE*>(ctx));
}

void render(Scenario& s, const std::string& dir) {
    Routed r = route(s);
    Canvas cv(800, 480);
    const unsigned char halo[3] = {12, 12, 16};
    const unsigned char bore[3] = {235, 235, 235};
    const unsigned char box[3] = {70, 74, 86};
    const unsigned char palette[4][3] = {
        {230, 90, 80}, {80, 170, 230}, {120, 200, 110}, {230, 190, 70}};
    float tube = s.cfg.clearance * 0.6f;
    for (auto& srcs : s.src)
        for (auto& p : srcs)
            cv.rect(p.x - 14, p.y - 30, p.x + 14, p.y, box);
    for (auto& dsts : s.dst)
        for (auto& p : dsts)
            cv.rect(p.x - 10, p.y, p.x + 10, p.y + 24, box);
    if (r.status == RouteStatus::Ok) {
        for (int pass = 0; pass < 3; ++pass)
            for (auto& t : r.traces) {
                auto pts = samples(t, s.cfg.fillet_r);
                for (auto& p : pts) {
                    if (pass == 0)
                        cv.disc(p.x, p.y, tube * 0.5f + 2.0f, halo);
                    else if (pass == 1)
                        cv.disc(p.x, p.y, tube * 0.5f, palette[t.bus % 4]);
                    else
                        cv.disc(p.x, p.y, 0.8f, bore);
                }
            }
    }
    std::string path = dir + "/" + s.name + ".png";
    FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    stbi_write_png_to_func(write_cb, f, cv.w, cv.h, 3, cv.px.data(), cv.w * 3);
    std::fclose(f);
    INFO(path << " status " << (int)r.status);
    CHECK(r.status == RouteStatus::Ok);
}

} // namespace

// Writes the six example scenarios as PNGs for eyeballing:
//   HELIX_TUBE_ROUTER_RENDER_DIR=/some/dir build/bin/helix-tests '[tube_router_render]'
TEST_CASE("tube router: render example scenarios", "[.][tube_router_render]") {
    const char* dir = std::getenv("HELIX_TUBE_ROUTER_RENDER_DIR");
    if (!dir)
        SKIP("HELIX_TUBE_ROUTER_RENDER_DIR not set");
    Scenario all[] = {bt4_hub(),           bt8_narrow_hub(),       two_units_shared_hub(),
                      four_spread_heads(), mixed_direct_and_hub(), fleet12_hub()};
    for (auto& s : all)
        render(s, dir);
}
