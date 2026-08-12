// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_listen_session.cpp
 * @brief The live measurement pipeline, driven by real Voron captures
 */

#include "../../include/belt_listen_session.h"
#include "../../include/belt_stream_client.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

namespace {

constexpr float SPAN_MM = 151.0f; ///< the span the fixtures were captured at

/// A fixture plus the two numbers its header records. Both vary per file, so
/// reading them beats hardcoding: rates run 3090.5-3091.5 Hz, and the recorded
/// strengths run 1.41 to 15.08. Splicing everything at one invented ratio would
/// silently test the weakest capture at more signal than it really had.
struct Fixture {
    std::vector<AccelSample> samples;
    float rate_hz = 0.0f;
    float recorded_ratio = 0.0f;
};

float header_value(const std::string& text, const std::string& key) {
    const size_t at = text.find(key + "=");
    REQUIRE(at != std::string::npos);
    return std::stof(text.substr(at + key.size() + 1));
}

Fixture load_fixture(const std::string& name) {
    std::ifstream in("tests/fixtures/belt_plucks/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    Fixture f;
    f.samples = parse_accel_csv(text);
    f.rate_hz = header_value(text, "sample_rate_hz");
    f.recorded_ratio = header_value(text, "rms_over_noise_floor");
    REQUIRE_FALSE(f.samples.empty());
    return f;
}

/// Gravity sits on X in every one of the 8 real captures (mean ~9515 on X,
/// under 50 on Y/Z in each fixture header's own data) - the toolhead mounts
/// the accelerometer with X vertical, not Z. Spliced noise must carry gravity
/// on the same axis the real ring-down does, or the join between synthetic
/// noise and real samples is a DC step disguised as a strong transient: it
/// falsely re-triggers the gate even on the weak-pluck fixture, which never
/// actually rings.
std::vector<AccelSample> make_noise(float amplitude, size_t count, float t0, float rate_hz) {
    std::vector<AccelSample> out(count);
    for (size_t i = 0; i < count; ++i) {
        const float p = static_cast<float>(i);
        out[i].time = t0 + static_cast<float>(i) / rate_hz;
        out[i].x = 9810.0f + amplitude * std::sin(p);
        out[i].y = amplitude * std::cos(p * 1.7f);
        out[i].z = amplitude * std::sin(p * 0.3f);
    }
    return out;
}

/// A live-stream-shaped buffer: quiet, then the real ring-down, then quiet.
/// The noise amplitude reproduces the signal-to-noise the original capture
/// recorded in its own header, rather than an invented figure chosen to pass.
std::vector<AccelSample> splice_live_window(const Fixture& f) {
    const float sig = PluckDetector::window_rms(f.samples.data(), f.samples.size());
    // window_rms combines all three axes: with the same amplitude on x, y and
    // z, the combined broadband RMS is amplitude*sqrt(1.5), not amplitude
    // alone. Dividing by that factor (rather than the single-axis amp/sqrt(2)
    // conversion) is what makes the spliced noise floor reproduce the
    // fixture's own recorded ratio - verified against every fixture: peak
    // window ratio during the ring-down lands within ~2% of recorded_ratio.
    const float noise_amp = (sig / f.recorded_ratio) / 1.2247f;

    auto out = make_noise(noise_amp, 1024, 0.0f, f.rate_hz);
    const float t_join = out.back().time;
    for (size_t i = 0; i < f.samples.size(); ++i) {
        AccelSample s = f.samples[i];
        s.time = t_join + static_cast<float>(i + 1) / f.rate_hz;
        out.push_back(s);
    }
    auto tail = make_noise(noise_amp, 1024, out.back().time, f.rate_hz);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

/// Feed a buffer through the session the way the stream client would, in
/// ~340-sample batches.
std::vector<PluckEvent> stream_through(BeltListenSession& s, const std::vector<AccelSample>& buf) {
    std::vector<PluckEvent> events;
    constexpr size_t kBatch = 340;
    for (size_t off = 0; off < buf.size(); off += kBatch) {
        AccelBatch b;
        const size_t n = std::min(kBatch, buf.size() - off);
        b.samples.assign(buf.begin() + static_cast<long>(off),
                         buf.begin() + static_cast<long>(off + n));
        if (auto ev = s.push(b)) {
            events.push_back(*ev);
        }
    }
    return events;
}

} // namespace

TEST_CASE("session recovers the A belt fundamental from a real capture", "[belt][listen]") {
    // a_belt_86hz_3 (recorded 11.94) rather than _1 (9.37): _1 sits 4% above
    // the gate, and the splice only approximates a target ratio, so it belongs
    // in a boundary test rather than in the one that must reliably accept.
    const auto fx = load_fixture("a_belt_86hz_3.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));

    auto events = stream_through(s, live);
    REQUIRE_FALSE(events.empty());

    const auto accepted =
        std::count_if(events.begin(), events.end(), [](const PluckEvent& e) { return e.accepted; });
    INFO("events=" << events.size() << " accepted=" << accepted);
    REQUIRE(accepted >= 1);
    CHECK(s.accepted_count() >= 1);
    CHECK(s.median_hz() == Catch::Approx(86.0f).margin(6.0f));
}

TEST_CASE("session recovers the B belt fundamental from a real capture", "[belt][listen]") {
    const auto fx = load_fixture("b_belt_82hz_1.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
    stream_through(s, live);

    REQUIRE(s.accepted_count() >= 1);
    CHECK(s.median_hz() == Catch::Approx(82.0f).margin(6.0f));
}

TEST_CASE("session does not read the A belt an octave sharp", "[belt][listen]") {
    // The shipping bug: on this belt the 2nd harmonic dominates the
    // fundamental by 3 dB, so a largest-bin peak-pick returns 172 Hz with
    // total confidence. The whole pipeline exists to not do that.
    const auto fx = load_fixture("a_belt_86hz_2.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
    stream_through(s, live);

    // A margin-only upper bound (e.g. "< 130") is too loose to guard this: it
    // also passes a naive-peak-pick mutation that locks onto a spurious ~34 Hz
    // bin instead of the octave, because this session's ring-down comes from
    // the leading edge of a still-filling detection window, not the fixture's
    // full 500 ms in isolation. Asserting closeness to the true 86 Hz
    // fundamental catches both failure shapes.
    REQUIRE(s.accepted_count() >= 1);
    CHECK(s.median_hz() == Catch::Approx(86.0f).margin(10.0f));
}

TEST_CASE("one physical pluck is counted once", "[belt][listen]") {
    // A pluck lives in many overlapping detection windows. Without a cooldown
    // the median commits on a single strike and the count is fiction.
    const auto fx = load_fixture("a_belt_86hz_3.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
    stream_through(s, live);

    CHECK(s.accepted_count() == 1);
}

TEST_CASE("a weak strike is rejected, not measured", "[belt][listen]") {
    const auto fx = load_fixture("weak_pluck_reject.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
    stream_through(s, live);

    CHECK(s.accepted_count() == 0);
    CHECK(s.median_hz() == 0.0f);
    CHECK_FALSE(s.committed());
}

TEST_CASE("quiet stream produces nothing", "[belt][listen]") {
    constexpr float kRate = 3091.0f; // no fixture involved; any plausible rate
    auto quiet = make_noise(5.0f, 8192, 0.0f, kRate);
    BeltListenSession s(SPAN_MM, kRate);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(quiet.begin(), quiet.begin() + 1000)));
    stream_through(s, quiet);
    CHECK(s.accepted_count() == 0);
    CHECK_FALSE(s.committed());
}

TEST_CASE("the session commits only after five accepted plucks", "[belt][listen]") {
    const auto fx = load_fixture("a_belt_86hz_3.csv");
    BeltListenSession s(SPAN_MM, fx.rate_hz);
    for (int i = 0; i < 5; ++i) {
        auto live = splice_live_window(fx);
        if (i == 0) {
            REQUIRE(
                s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
        }
        CHECK_FALSE(s.committed()); // still short of five on every iteration
        stream_through(s, live);
    }
    CHECK(s.accepted_count() == 5);
    CHECK(s.committed());
    CHECK(s.median_hz() == Catch::Approx(86.0f).margin(6.0f));
}

TEST_CASE("the session keeps accepting past the commit threshold", "[belt][listen]") {
    // The user is meant to be able to keep plucking and watch the number hold.
    const auto fx = load_fixture("a_belt_86hz_3.csv");
    BeltListenSession s(SPAN_MM, fx.rate_hz);
    for (int i = 0; i < 8; ++i) {
        auto live = splice_live_window(fx);
        if (i == 0) {
            REQUIRE(
                s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
        }
        stream_through(s, live);
    }
    CHECK(s.accepted_count() == 8);
}

TEST_CASE("a discontiguous batch is not analysed", "[belt][listen]") {
    // Nonzero errors/overflows mean klippy dropped samples, so the window is
    // not a real ring-down and any frequency read from it is invented.
    const auto fx = load_fixture("a_belt_86hz_3.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));

    constexpr size_t kBatch = 340;
    for (size_t off = 0; off < live.size(); off += kBatch) {
        AccelBatch b;
        const size_t n = std::min(kBatch, live.size() - off);
        b.samples.assign(live.begin() + static_cast<long>(off),
                         live.begin() + static_cast<long>(off + n));
        b.overflows = 1;
        s.push(b);
    }
    CHECK(s.accepted_count() == 0);
}

TEST_CASE("reset clears everything", "[belt][listen]") {
    const auto fx = load_fixture("a_belt_86hz_3.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
    stream_through(s, live);
    REQUIRE(s.accepted_count() >= 1);

    s.reset();
    CHECK(s.accepted_count() == 0);
    CHECK(s.rejected_count() == 0);
    CHECK(s.median_hz() == 0.0f);
    CHECK_FALSE(s.committed());
    CHECK(s.window().empty());
}
