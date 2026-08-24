// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_listen_session.cpp
 * @brief The live measurement pipeline, driven by real Voron captures
 */

#include "../../include/belt_capture.h"
#include "../../include/belt_listen_session.h"
#include "../../include/belt_stream_client.h"
#include "../../include/pitch_estimator.h"
#include "belt_test_signals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

/// Broadband quiet bed - see belt_test_signals.h for why gravity is on X and
/// why the content must not be a handful of sinusoids. An earlier revision
/// used sin(i)/cos(1.7i)/sin(0.3i) here; that bed has a median spectral bin
/// near zero, so every bin of the learned QuietSpectrum read as contaminated,
/// and sin(0.3i) at ~3091 Hz put an artificial tone at ~147 Hz, inside the
/// 77-165 Hz search window the estimator scans.
std::vector<AccelSample> make_noise(float amplitude, size_t count, float t0, float rate_hz) {
    helix::calibration::test::Hiss rng;
    return helix::calibration::test::hiss_bed(count, amplitude, rate_hz, rng, t0);
}

/// A live-stream-shaped buffer: quiet, then the real ring-down, then quiet.
/// The noise amplitude reproduces the signal-to-noise the original capture
/// recorded in its own header, rather than an invented figure chosen to pass.
std::vector<AccelSample> splice_live_window(const Fixture& f, size_t lead_in = 1024) {
    const float sig = PluckDetector::window_rms(f.samples.data(), f.samples.size());
    // Uniform noise on three axes has a combined broadband RMS equal to its
    // amplitude (see hiss_bed), so this is the amplitude that makes the
    // spliced floor reproduce the fixture's recorded ratio. It reproduces it
    // to about 89%, not exactly: recorded_ratio was measured on the detection
    // window that triggered, while `sig` here is the RMS of the whole 500 ms
    // capture, which is quieter than its own loudest window. That gap is why
    // the two weakest captures sit below the gate once spliced.
    const float noise_amp = sig / f.recorded_ratio;

    auto out = make_noise(noise_amp, lead_in, 0.0f, f.rate_hz);
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

using helix::calibration::test::Hiss;
using helix::calibration::test::hiss_bed;

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
/// harmonic series in it - see noise_burst() in test_pluck_detector.cpp for
/// why its leading edge is a single full-scale sample.
std::vector<AccelSample> thump_stream(float amp) {
    Hiss rng;
    auto out = hiss_bed(kQuietLead + kBody, 5.0f, kRate, rng);
    Hiss burst{31337u};
    for (size_t i = kQuietLead; i < out.size(); ++i) {
        const float dt = static_cast<float>(i - kQuietLead) / kRate;
        const float env = amp * std::exp(-dt / 0.20f);
        const bool impact = (i == kQuietLead);
        out[i].x += env * (impact ? 1.0f : burst.next());
        out[i].y += env * (impact ? 1.0f : burst.next());
        out[i].z += env * (impact ? 1.0f : burst.next());
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

namespace {
namespace fs = std::filesystem;

/// Sets HELIX_BELT_CAPTURE_DIR for the test's scope and restores whatever was
/// there before (normally nothing) on the way out, so this test cannot leak
/// capture-writing into any test case that runs after it in the same shard.
class CaptureDirEnvGuard {
  public:
    explicit CaptureDirEnvGuard(const std::string& dir) {
        if (const char* prev = std::getenv("HELIX_BELT_CAPTURE_DIR")) {
            had_prev_ = true;
            prev_value_ = prev;
        }
        setenv("HELIX_BELT_CAPTURE_DIR", dir.c_str(), 1);
    }
    ~CaptureDirEnvGuard() {
        if (had_prev_) {
            setenv("HELIX_BELT_CAPTURE_DIR", prev_value_.c_str(), 1);
        } else {
            unsetenv("HELIX_BELT_CAPTURE_DIR");
        }
    }
    CaptureDirEnvGuard(const CaptureDirEnvGuard&) = delete;
    CaptureDirEnvGuard& operator=(const CaptureDirEnvGuard&) = delete;

  private:
    bool had_prev_ = false;
    std::string prev_value_;
};

size_t count_files(const fs::path& dir) {
    size_t n = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        (void)entry;
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE("HELIX_BELT_CAPTURE_DIR wires capture into a live session end to end",
          "[belt][listen][belt_capture][slow]") {
    // Proves the env-var-triggered path, not just BeltCaptureWriter's own
    // API: BeltListenSession reads belt_capture_dir() itself in its
    // constructor, so the directory has to be set before construction for
    // this to exercise anything.
    const fs::path dir =
        fs::temp_directory_path() /
        ("helix_belt_capture_e2e_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    CaptureDirEnvGuard env(dir.string());

    {
        const auto fx = load_fixture("a_belt_86hz_3.csv");
        BeltListenSession accepted_session(SPAN_MM, fx.rate_hz);
        listen(accepted_session, splice_live_window(fx));
        REQUIRE(accepted_session.accepted_count() >= 1);
    }
    {
        // weak_pluck_reject.csv never clears MIN_DETECTABLE_RATIO once
        // spliced (it was captured at 1.4x the floor), so push() returns
        // nullopt before ever reaching a capture point - nothing to write
        // and nothing wrong with that. A steady tone clears the energy gate
        // and gets resolved as NOT_A_PLUCK, which is the rejection shape
        // this test needs to see written.
        auto buf = steady_tone_stream(115.0f, 400.0f);
        BeltListenSession rejected_session(SPAN_MM, kRate);
        auto events = listen(rejected_session, buf);
        REQUIRE_FALSE(events.empty());
        REQUIRE(rejected_session.accepted_count() == 0);
    }

    REQUIRE(count_files(dir) > 0);

    bool saw_accepted_detection = false;
    bool saw_accepted_ringdown = false;
    bool saw_rejected = false;
    bool saw_quiet = false;

    for (const auto& entry : fs::directory_iterator(dir)) {
        std::ifstream in(entry.path());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();

        // Every file, whatever kind, must be a valid capture: parseable by
        // the same parser a real fixture goes through, non-empty, and
        // carrying the sample rate the session measured.
        const auto samples = parse_accel_csv(text);
        CHECK_FALSE(samples.empty());
        CHECK(parse_capture_sample_rate(text) > 0.0f);

        const std::string name = entry.path().filename().string();
        if (name.find("_ACCEPTED_detection") != std::string::npos) {
            saw_accepted_detection = true;
            CHECK(text.find("verdict=ACCEPTED") != std::string::npos);
        } else if (name.find("_ACCEPTED_ringdown") != std::string::npos) {
            saw_accepted_ringdown = true;
        } else if (name.find("_TOO_SOFT_") != std::string::npos ||
                   name.find("_NOT_A_PLUCK_") != std::string::npos) {
            saw_rejected = true;
        } else if (name.rfind("quiet_", 0) == 0) {
            saw_quiet = true;
            CHECK(text.find("verdict=") == std::string::npos);
        }
    }

    CHECK(saw_accepted_detection);
    CHECK(saw_accepted_ringdown);
    CHECK(saw_rejected);
    CHECK(saw_quiet);

    std::error_code ec;
    fs::remove_all(dir, ec);
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
    auto events = listen(s, buf);

    CHECK(peak_window_ratio(s, buf) > PluckDetector::MIN_RMS_RATIO);
    CHECK(s.accepted_count() == 0);
    CHECK_FALSE(s.committed());
    // The reason matters most here: a thump is firm, so "pluck harder" would
    // be the worst possible instruction to give.
    CHECK(std::any_of(events.begin(), events.end(),
                      [](const PluckEvent& e) { return e.reject == PluckReject::NOT_A_PLUCK; }));
    CHECK(
        std::none_of(events.begin(), events.end(), [](const PluckEvent& e) { return e.accepted; }));
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

TEST_CASE("every accept-case capture is measured at every window alignment",
          "[belt][listen][golden]") {
    // THE regression guard, and the reason it is exhaustive rather than sampled.
    //
    // A lead-in fixes the alignment between the strike and the 340-sample batch
    // grid. The live stream produces every alignment, and the checks have twice
    // turned out to be sensitive to which one:
    //
    //   - anchoring has_sharp_onset() at the loudest envelope segment instead
    //     of at the strike made the same capture read a rise of 1.16 at one
    //     alignment and 36.4 at the next, against a threshold of 3. A
    //     single-lead-in guard could not see it.
    //   - MIN_HARMONIC_CONCENTRATION at 0.30 rejected b_belt_82hz_3, the
    //     strongest capture in the set, at leads 2304-2312 and nowhere else. A
    //     nine-point sampled guard stepped straight over that band (2300, then
    //     2342) and could not see it either.
    //
    // Sampling is what let both through, so this sweeps all 340 alignments. It
    // costs a few seconds; the alternative has now twice been a wrong number
    // shipped to a user.
    //
    // The lead-in starts at DETECTION_WINDOW_SAMPLES so the window is already
    // full when the strike arrives, which is the production condition: the
    // window fills within a second of the stream starting, and the user plucks
    // long after that.
    float band_lo = 0.0f, band_hi = 0.0f;
    REQUIRE(search_window_for_span(SPAN_MM, &band_lo, &band_hi));

    struct Expect {
        const char* name;
        float hz;
    };
    for (const auto& e : {Expect{"a_belt_86hz_2.csv", 86.0f}, Expect{"a_belt_86hz_3.csv", 86.0f},
                          Expect{"b_belt_82hz_1.csv", 82.0f}, Expect{"b_belt_82hz_2.csv", 82.0f},
                          Expect{"b_belt_82hz_3.csv", 82.0f}}) {
        const auto fx = load_fixture(e.name);
        size_t measured = 0;
        size_t phases = 0;
        float worst_concentration = 1.0f;
        size_t worst_lead = 0;

        constexpr size_t kBatch = 340;
        for (size_t lead = BeltListenSession::DETECTION_WINDOW_SAMPLES;
             lead < BeltListenSession::DETECTION_WINDOW_SAMPLES + kBatch; ++lead) {
            ++phases;
            auto live = splice_live_window(fx, lead);

            BeltListenSession s(SPAN_MM, fx.rate_hz);
            REQUIRE(
                s.learn_noise_floor(std::vector<AccelSample>(live.begin(), live.begin() + 1000)));
            stream_through(s, live);

            if (s.accepted_count() == 0) {
                continue;
            }
            ++measured;
            // The read-back below pairs median_hz() (a median over every
            // accepted pluck) with last_spectrum() (the last accepted pluck's
            // PSD). They describe the same pluck only while there is exactly
            // one, which the cooldown guarantees for a single spliced strike.
            // Pin it, so a change that broke it would fail here rather than
            // silently measure a mismatched pair.
            REQUIRE(s.accepted_count() == 1);
            // Every measurement produced has to be right. A sweep that raised
            // the count by admitting wrong numbers would be worse than the
            // miss it papered over.
            if (std::abs(s.median_hz() - e.hz) > 6.0f) {
                INFO("fixture " << e.name << " lead-in " << lead << " read " << s.median_hz());
                CHECK(s.median_hz() == Catch::Approx(e.hz).margin(6.0f));
            }

            // Read back the concentration push() actually computed, from the
            // spectrum and the frequency it settled on. This is the margin on
            // MIN_HARMONIC_CONCENTRATION, and it is the quantity that had none
            // left: asserting the accept/reject outcome alone would let the
            // margin erode silently until the next alignment sweep trips.
            const float c = harmonic_concentration(s.last_spectrum(), s.median_hz(),
                                                   DEFAULT_HARMONICS, band_lo, &s.quiet_spectrum());
            if (c < worst_concentration) {
                worst_concentration = c;
                worst_lead = lead;
            }
        }

        INFO("fixture " << e.name << " measured at " << measured << " of " << phases
                        << " alignments; worst concentration " << worst_concentration
                        << " at lead-in " << worst_lead);
        CHECK(measured == phases);
        CHECK(worst_concentration >= MIN_HARMONIC_CONCENTRATION);
    }
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

/// The reference machine's situation, end to end: a fan running the whole
/// time, a structural gantry mode that dominates the broadband energy, and a
/// comparatively small belt tone.
///
/// The proportions are what make it reproducible. The fan is in the quiet
/// window too, so it raises the learned noise floor - which means a strike can
/// only clear the 9x gate on energy that is NOT in the search window. On the
/// reference machine that energy is the structural mode (roughly 38-58 Hz,
/// moving with toolhead position, and the loudest thing in the capture), and
/// with it out of the way the fan and the belt are comparable inside the
/// window. Without a dominant structural mode this case cannot be built at
/// all: a fan loud enough to outscore the belt also lifts the floor past what
/// the belt can clear.
std::vector<AccelSample> fan_and_pluck_stream(float fan_hz, float fan_amp, float belt_hz,
                                              float belt_amp, float structural_amp) {
    static const float fan_profile[4] = {1.0f, 0.5f, 0.3f, 0.2f};
    static const float belt_profile[4] = {0.708f, 1.0f, 0.200f, 0.224f};
    constexpr float kStructuralHz = 45.0f;

    Hiss rng;
    auto out = hiss_bed(kQuietLead + kBody, 5.0f, kRate, rng);
    for (size_t i = 0; i < out.size(); ++i) {
        const float t = out[i].time;
        float fan = 0.0f;
        for (int h = 0; h < 4; ++h) {
            fan += fan_profile[h] * std::sin(2.0f * static_cast<float>(M_PI) * fan_hz *
                                             static_cast<float>(h + 1) * t);
        }
        out[i].x += fan_amp * fan;
        if (i < kQuietLead) {
            continue;
        }
        const float decay = std::exp(-static_cast<float>(i - kQuietLead) / kRate / 0.20f);
        float belt = 0.0f;
        for (int h = 0; h < 4; ++h) {
            belt += belt_profile[h] * std::sin(2.0f * static_cast<float>(M_PI) * belt_hz *
                                               static_cast<float>(h + 1) * t);
        }
        out[i].x += belt_amp * decay * belt;
        out[i].x +=
            structural_amp * decay * std::sin(2.0f * static_cast<float>(M_PI) * kStructuralHz * t);
    }
    return out;
}

TEST_CASE("a fan running through the whole session does not become the answer", "[belt][listen]") {
    // The only end-to-end evidence that the quiet spectrum is wired through
    // push(). test_pitch_estimator.cpp proves the discount works on a spectrum;
    // this proves the session learns it from the same buffer it learns its
    // floor from, and hands it to both the estimator and the concentration
    // check. With a broadband quiet bed no fixture-based session test reaches
    // BACKGROUND_PROMINENCE_TOLERANCE at all, so without this the discount is
    // inert everywhere else in this file.
    //
    // 115 Hz is the reference machine's measured background peak. It sits
    // inside the 77-165 Hz search window and outscores the belt, which is what
    // produced an evening of confident, wrong, mutually contradictory readings.
    auto buf = fan_and_pluck_stream(115.0f, 120.0f, 99.0f, 120.0f, 4000.0f);
    BeltListenSession s(SPAN_MM, kRate);
    listen(s, buf);

    // The fan is genuinely prominent in what the session learned, or nothing
    // below means anything.
    REQUIRE(s.quiet_spectrum().valid());
    INFO("prominence at 115 Hz " << s.quiet_spectrum().prominence_at(115.0f));
    CHECK(s.quiet_spectrum().prominence_at(115.0f) > BACKGROUND_PROMINENCE_TOLERANCE);
    CHECK(s.quiet_spectrum().weight_at(115.0f) < 1.0f);

    REQUIRE(s.accepted_count() >= 1);
    INFO("session median " << s.median_hz());
    CHECK(s.median_hz() == Catch::Approx(99.0f).margin(4.0f));

    // The teeth: score the very spectrum the session analysed WITHOUT the quiet
    // window, and it returns the fan. Same PSD, two scorings, opposite answers -
    // so this cannot pass by the fan simply being too quiet to matter.
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(SPAN_MM, &lo, &hi));
    REQUIRE_FALSE(s.last_spectrum().empty());
    const auto blind = estimate_pitch(s.last_spectrum(), lo, hi, DEFAULT_HARMONICS, nullptr);
    REQUIRE(blind.valid);
    INFO("blind estimate " << blind.frequency_hz);
    CHECK(blind.frequency_hz == Catch::Approx(115.0f).margin(4.0f));
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
