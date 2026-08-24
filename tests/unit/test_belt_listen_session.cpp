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
#include <cstdint>
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

/// Deterministic hiss - a plain LCG, so a threshold test analyses the same
/// buffer on every run instead of a fresh random draw.
struct Hiss {
    uint32_t state = 2468u;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 23) - 1.0f;
    }
};

/// Gravity on X, matching every real capture - see make_noise() above.
std::vector<AccelSample> hiss_bed(size_t count, float amp, float rate_hz, Hiss& rng) {
    std::vector<AccelSample> out(count);
    for (size_t i = 0; i < count; ++i) {
        out[i].time = static_cast<float>(i) / rate_hz;
        out[i].x = 9810.0f + amp * rng.next();
        out[i].y = amp * rng.next();
        out[i].z = amp * rng.next();
    }
    return out;
}

constexpr float kRate = 3091.0f;
/// Long enough to learn a floor from, trigger on, and ride out one cooldown.
constexpr size_t kQuietLead = 4096;
constexpr size_t kBody = 8192;

/// Quiet, then a steady tone that simply switches on and never stops - a fan
/// spinning up, not a string being plucked.
std::vector<AccelSample> steady_tone_stream(float freq, float amp) {
    Hiss rng;
    auto out = hiss_bed(kQuietLead + kBody, 5.0f, kRate, rng);
    for (size_t i = kQuietLead; i < out.size(); ++i) {
        out[i].x += amp * std::sin(2.0f * static_cast<float>(M_PI) * freq * out[i].time);
    }
    return out;
}

/// Quiet, then energy that grows to a peak and stops - a machine winding up,
/// not a string ringing down. It ends rather than running to the end of the
/// buffer so the window does resolve: an event that is still growing when the
/// stream stops is simply never judged, which would make the test vacuous.
std::vector<AccelSample> swelling_stream(float freq, float amp) {
    Hiss rng;
    auto out = hiss_bed(kQuietLead + kBody, 5.0f, kRate, rng);
    const size_t peak = kQuietLead + kBody / 2;
    const float peak_time = out[peak - 1].time;
    for (size_t i = kQuietLead; i < peak; ++i) {
        const float env = std::exp((out[i].time - peak_time) / 0.40f);
        out[i].x += amp * env * std::sin(2.0f * static_cast<float>(M_PI) * freq * out[i].time);
    }
    return out;
}

/// Quiet, then a sharp broadband thump that decays like a pluck but has no
/// harmonic series in it.
std::vector<AccelSample> thump_stream(float amp) {
    Hiss rng;
    auto out = hiss_bed(kQuietLead + kBody, 5.0f, kRate, rng);
    Hiss burst{31337u};
    for (size_t i = kQuietLead; i < out.size(); ++i) {
        const float dt = static_cast<float>(i - kQuietLead) / kRate;
        const float env = amp * std::exp(-dt / 0.20f);
        out[i].x += env * burst.next();
        out[i].y += env * burst.next();
        out[i].z += env * burst.next();
    }
    return out;
}

/// Quiet, then a genuine exponentially decaying harmonic series.
std::vector<AccelSample> pluck_stream(float f0, float amp) {
    static const float profile[4] = {0.708f, 1.0f, 0.200f, 0.224f};
    Hiss rng;
    auto out = hiss_bed(kQuietLead + kBody, 5.0f, kRate, rng);
    for (size_t i = kQuietLead; i < out.size(); ++i) {
        const float dt = static_cast<float>(i - kQuietLead) / kRate;
        float v = 0.0f;
        for (int h = 0; h < 4; ++h) {
            v += profile[h] * std::sin(2.0f * static_cast<float>(M_PI) * f0 *
                                       static_cast<float>(h + 1) * out[i].time);
        }
        out[i].x += amp * std::exp(-dt / 0.20f) * v;
    }
    return out;
}

/// Run a synthetic buffer through a session that learned its floor from the
/// buffer's own quiet lead-in, exactly as the panel does.
std::vector<PluckEvent> listen(BeltListenSession& s, const std::vector<AccelSample>& buf) {
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(buf.begin(), buf.begin() + 1000)));
    return stream_through(s, buf);
}

/// Strength of the loudest detection window in `buf`, as the session would
/// measure it. Pins that a rejection was NOT the old energy gate doing the work.
float peak_window_ratio(const BeltListenSession& s, const std::vector<AccelSample>& buf) {
    PluckDetector det;
    det.set_noise_floor(s.noise_floor());
    float best = 0.0f;
    const size_t w = BeltListenSession::DETECTION_WINDOW_SAMPLES;
    for (size_t end = w; end <= buf.size(); end += 128) {
        best = std::max(best, det.rms_ratio(buf.data() + (end - w), w));
    }
    return best;
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
    CHECK(s.last_spectrum().empty());
}

TEST_CASE("last_spectrum holds the accepted pluck's PSD", "[belt][listen]") {
    // The live spectrum strip reads this after every accepted strike. It must
    // come from a real PSD pass, not just toggle non-empty - a rejected
    // strike must leave the previous reading in place rather than clear it,
    // since between plucks there is nothing new to show.
    const auto fx = load_fixture("a_belt_86hz_3.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    CHECK(s.last_spectrum().empty()); // nothing accepted yet

    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
    stream_through(s, live);
    REQUIRE(s.accepted_count() >= 1);

    REQUIRE_FALSE(s.last_spectrum().empty());
    // Every bin is a real (frequency, power) pair from compute_psd(), not a
    // placeholder - frequencies are strictly increasing and positive.
    for (size_t i = 1; i < s.last_spectrum().size(); ++i) {
        CHECK(s.last_spectrum()[i].first > s.last_spectrum()[i - 1].first);
    }
    CHECK(s.last_spectrum().front().first > 0.0f);

    s.reset();
    CHECK(s.last_spectrum().empty());
}

// ============================================================================
// Energy is not enough: the event has to have been a pluck
// ============================================================================

TEST_CASE("a loud steady tone is never counted as a pluck", "[belt][listen]") {
    // Three transients at 44-53x the floor were analysed and reported as
    // plucks on the reference machine, on an evening when the maintainer
    // plucked nothing. This is that event: far past the 9x gate, and not a
    // strike.
    auto buf = steady_tone_stream(115.0f, 400.0f);
    BeltListenSession s(SPAN_MM, kRate);
    auto events = listen(s, buf);

    INFO("peak window ratio " << peak_window_ratio(s, buf));
    CHECK(peak_window_ratio(s, buf) > PluckDetector::MIN_RMS_RATIO); // energy accepts it
    CHECK(s.accepted_count() == 0);
    CHECK(s.median_hz() == 0.0f);
    CHECK_FALSE(s.committed());
    // It is resolved and rejected, not merely ignored - and the reason has to
    // be the right one, or the panel tells the user to pluck harder.
    REQUIRE_FALSE(events.empty());
    for (const auto& e : events) {
        CHECK(e.reject == PluckReject::NOT_A_PLUCK);
    }
}

TEST_CASE("energy that swells instead of ringing down is not a pluck", "[belt][listen]") {
    auto buf = swelling_stream(115.0f, 400.0f);
    BeltListenSession s(SPAN_MM, kRate);
    auto events = listen(s, buf);

    CHECK(peak_window_ratio(s, buf) > PluckDetector::MIN_RMS_RATIO);
    CHECK(s.accepted_count() == 0);
    CHECK_FALSE(s.committed());
    // The swell is resolved and rejected on its shape. Its dying tail also
    // produces a TOO_SOFT event on the way out, which is honest - by then it
    // really is too soft - so the assertion is that the shape rejection
    // happened, not that it was the only thing that did.
    CHECK(std::any_of(events.begin(), events.end(),
                      [](const PluckEvent& e) { return e.reject == PluckReject::NOT_A_PLUCK; }));
    CHECK(
        std::none_of(events.begin(), events.end(), [](const PluckEvent& e) { return e.accepted; }));
}

TEST_CASE("a broadband thump is not a pluck", "[belt][listen]") {
    // A door closing or the toolhead settling after a park: real onset, real
    // decay, no harmonic series. Only the spectrum can tell it apart, which is
    // why the concentration check exists.
    auto buf = thump_stream(400.0f);
    BeltListenSession s(SPAN_MM, kRate);
    listen(s, buf);

    CHECK(peak_window_ratio(s, buf) > PluckDetector::MIN_RMS_RATIO);
    CHECK(s.accepted_count() == 0);
    CHECK_FALSE(s.committed());
}

TEST_CASE("a genuine decaying harmonic series is still accepted", "[belt][listen]") {
    // The guard on everything above: the new checks must not be satisfiable by
    // raising MIN_RMS_RATIO, so a real strike barely over the existing gate has
    // to come through.
    auto buf = pluck_stream(99.0f, 172.0f);
    BeltListenSession s(SPAN_MM, kRate);
    auto events = listen(s, buf);

    const float ratio = peak_window_ratio(s, buf);
    INFO("peak window ratio " << ratio << ", events " << events.size());
    CHECK(ratio < 20.0f); // not a landslide - this is a modest strike
    REQUIRE(s.accepted_count() >= 1);
    CHECK(s.median_hz() == Catch::Approx(99.0f).margin(4.0f));
}

TEST_CASE("every accept-case capture still passes the whole pipeline", "[belt][listen][golden]") {
    // The regression guard: these are real plucks on real hardware. A shape
    // check tuned until the synthetic cases pass but that rejects a genuine
    // capture has made the tool worse, not safer.
    //
    // a_belt_86hz_1 (recorded 9.37) is absent on purpose - splice_live_window
    // only approximates a target ratio, and a capture sitting 4% over the gate
    // lands below it once spliced. That is the energy gate, not a shape check.
    struct Expect {
        const char* name;
        float hz;
    };
    for (const auto& e : {Expect{"a_belt_86hz_2.csv", 86.0f}, Expect{"a_belt_86hz_3.csv", 86.0f},
                          Expect{"b_belt_82hz_1.csv", 82.0f}, Expect{"b_belt_82hz_2.csv", 82.0f},
                          Expect{"b_belt_82hz_3.csv", 82.0f}}) {
        const auto fx = load_fixture(e.name);
        auto live = splice_live_window(fx);

        BeltListenSession s(SPAN_MM, fx.rate_hz);
        REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
        stream_through(s, live);

        INFO("fixture " << e.name);
        REQUIRE(s.accepted_count() >= 1);
        CHECK(s.median_hz() == Catch::Approx(e.hz).margin(6.0f));
    }
}

TEST_CASE("the below-gate capture is still rejected", "[belt][listen][golden]") {
    // Recorded at 7.82x, under MIN_RMS_RATIO. It must stay a reject case - the
    // shape checks are additional evidence, never a way past the energy gate.
    const auto fx = load_fixture("b_belt_82hz_hard_case.csv");
    auto live = splice_live_window(fx);

    BeltListenSession s(SPAN_MM, fx.rate_hz);
    REQUIRE(s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
    stream_through(s, live);

    CHECK(s.accepted_count() == 0);
}

TEST_CASE("learn_noise_floor learns the quiet spectrum as well as the floor", "[belt][listen]") {
    // Same buffer, two questions: how loud is it in here, and what is already
    // ringing in here. set_noise_floor() answers only the first, and a session
    // set up that way has no background rejection.
    auto buf = pluck_stream(99.0f, 172.0f);
    BeltListenSession learned(SPAN_MM, kRate);
    REQUIRE(learned.learn_noise_floor(std::vector<AccelSample>(buf.begin(), buf.begin() + 3000)));
    CHECK(learned.quiet_spectrum().valid());

    BeltListenSession scalar_only(SPAN_MM, kRate);
    scalar_only.set_noise_floor(50.0f);
    CHECK_FALSE(scalar_only.quiet_spectrum().valid());
}
