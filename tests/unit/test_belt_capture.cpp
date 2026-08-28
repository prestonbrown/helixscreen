// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_capture.cpp
 * @brief BeltCaptureWriter - the raw-sample capture path for hardware fixtures
 *
 * Every threshold in PluckDetector/pitch_estimator was measured against eight
 * captures from one machine on one evening. This is the instrument that
 * collects the next round, so the two things that matter most are: the
 * format matches tests/fixtures/belt_plucks/ exactly (a capture must drop
 * straight in as a fixture), and a real fixture survives an unaltered
 * round-trip through it.
 */

#include "../../include/belt_capture.h"
#include "../../include/belt_tension_types.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

namespace fs = std::filesystem;

namespace {

/// Two samples with binary-exact fractions (halves and quarters), so %.6f/
/// %.4f formatting has no rounding ambiguity to reason about - the point of
/// these tests is the format, not floating-point printing edge cases.
std::vector<AccelSample> two_exact_samples() {
    return {
        {0.0f, 100.5f, -50.25f, 9800.125f},
        {0.5f, 101.0f, -49.75f, 9799.875f},
    };
}

} // namespace

// ============================================================================
// Step 1: render_capture() - pure, exact bytes
// ============================================================================

TEST_CASE("render_capture writes the exact three-comment-line format",
          "[belt_tension][belt_capture]") {
    CaptureVerdict v;
    v.accepted = true;
    v.reject = PluckReject::NONE;
    v.rms_ratio = 11.94f;
    v.onset_rise = 34.21f;
    v.decay_end_ratio = 0.18f;
    v.harmonic_concentration = 0.412f;
    v.estimate_hz = 86.03f;
    v.median_hz = 85.5f;

    const std::string text =
        render_capture(two_exact_samples(), 3091.2f, CaptureBufferKind::RINGDOWN, v, 151.0f);

    const std::string expected =
        "# HelixScreen belt pluck capture - ring-down, 151mm span\n"
        "# ADXL345 on toolhead, steppers energized, ring-down window only\n"
        "# sample_rate_hz=3091.2 rms_over_noise_floor=11.94 verdict=ACCEPTED "
        "onset_rise=34.21 decay_end_ratio=0.180 harmonic_concentration=0.412 "
        "estimate_hz=86.0 median_hz=85.5\n"
        "#time,accel_x,accel_y,accel_z\n"
        "0.000000,100.5000,-50.2500,9800.1250\n"
        "0.500000,101.0000,-49.7500,9799.8750\n";

    CHECK(text == expected);
}

TEST_CASE("render_capture labels the detection window differently from a ring-down",
          "[belt_tension][belt_capture]") {
    CaptureVerdict v;
    v.accepted = true;
    v.rms_ratio = 11.94f;

    const std::string text = render_capture(two_exact_samples(), 3091.2f,
                                            CaptureBufferKind::DETECTION_WINDOW, v, 151.0f);

    CHECK(text.find("detection window") != std::string::npos);
    CHECK(text.find("ring-down") == std::string::npos);
}

TEST_CASE("render_capture writes n/a for verdict fields the event never reached",
          "[belt_tension][belt_capture]") {
    // A TOO_SOFT rejection never gets far enough to have a harmonic
    // concentration or, in this constructed case, a decay ratio either -
    // exactly the shape a real "cleared the energy gate but nothing else"
    // event produces.
    CaptureVerdict v;
    v.accepted = false;
    v.reject = PluckReject::TOO_SOFT;
    v.rms_ratio = 4.5f;
    v.onset_rise = 2.1f; // this one WAS evaluated - only some fields are absent

    const std::string text = render_capture({{0.0f, 1.0f, 2.0f, 3.0f}}, 3200.0f,
                                            CaptureBufferKind::DETECTION_WINDOW, v, 150.0f);

    CHECK(text.find("verdict=TOO_SOFT") != std::string::npos);
    CHECK(text.find("onset_rise=2.10") != std::string::npos);
    CHECK(text.find("decay_end_ratio=n/a") != std::string::npos);
    CHECK(text.find("harmonic_concentration=n/a") != std::string::npos);
}

TEST_CASE("render_capture reports NOT_A_PLUCK for a shape rejection",
          "[belt_tension][belt_capture]") {
    CaptureVerdict v;
    v.accepted = false;
    v.reject = PluckReject::NOT_A_PLUCK;
    v.rms_ratio = 15.0f;

    const std::string text = render_capture({{0.0f, 1.0f, 2.0f, 3.0f}}, 3200.0f,
                                            CaptureBufferKind::DETECTION_WINDOW, v, 150.0f);

    CHECK(text.find("verdict=NOT_A_PLUCK") != std::string::npos);
}

TEST_CASE("render_capture for a QUIET buffer carries no verdict fields",
          "[belt_tension][belt_capture]") {
    const std::string text = render_capture({{0.0f, 1.0f, 2.0f, 3.0f}}, 3200.0f,
                                            CaptureBufferKind::QUIET, CaptureVerdict{}, 150.0f);

    CHECK(text.find("# sample_rate_hz=3200.0\n") != std::string::npos);
    CHECK(text.find("verdict=") == std::string::npos);
    CHECK(text.find("quiet window") != std::string::npos);
}

TEST_CASE("render_capture's header round-trips through parse_accel_csv unchanged",
          "[belt_tension][belt_capture]") {
    const auto samples = two_exact_samples();
    CaptureVerdict v;
    v.accepted = true;
    v.rms_ratio = 11.94f;

    const std::string text =
        render_capture(samples, 3091.2f, CaptureBufferKind::RINGDOWN, v, 151.0f);
    const auto parsed = parse_accel_csv(text);

    REQUIRE(parsed.size() == samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        CHECK(parsed[i].time == samples[i].time);
        CHECK(parsed[i].x == samples[i].x);
        CHECK(parsed[i].y == samples[i].y);
        CHECK(parsed[i].z == samples[i].z);
    }
}

// ============================================================================
// Step 2: round-trip against a real fixture
// ============================================================================

TEST_CASE("a real fixture survives render_capture unchanged",
          "[belt_tension][belt_capture][golden]") {
    std::ifstream in("tests/fixtures/belt_plucks/a_belt_86hz_3.csv");
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string original_text = ss.str();

    const auto original_samples = parse_accel_csv(original_text);
    REQUIRE(original_samples.size() == 1545);
    const float rate = parse_capture_sample_rate(original_text);
    REQUIRE(rate > 0.0f);

    CaptureVerdict v;
    v.accepted = true;
    v.rms_ratio = 11.94f; // matches the fixture's own header; not load-bearing here

    const std::string rendered =
        render_capture(original_samples, rate, CaptureBufferKind::RINGDOWN, v, 151.0f);
    const auto round_tripped = parse_accel_csv(rendered);

    REQUIRE(round_tripped.size() == original_samples.size());
    for (size_t i = 0; i < original_samples.size(); ++i) {
        INFO("sample " << i);
        CHECK(round_tripped[i].time == original_samples[i].time);
        CHECK(round_tripped[i].x == original_samples[i].x);
        CHECK(round_tripped[i].y == original_samples[i].y);
        CHECK(round_tripped[i].z == original_samples[i].z);
    }
}

TEST_CASE("parse_capture_sample_rate reads the fixture's own header",
          "[belt_tension][belt_capture]") {
    std::ifstream in("tests/fixtures/belt_plucks/a_belt_86hz_3.csv");
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();

    CHECK(parse_capture_sample_rate(ss.str()) == Catch::Approx(3091.2f));
}

TEST_CASE("parse_capture_sample_rate returns 0 when the key is missing",
          "[belt_tension][belt_capture]") {
    CHECK(parse_capture_sample_rate("# no rate here\n0,1,2,3\n") == 0.0f);
    CHECK(parse_capture_sample_rate("") == 0.0f);
}

// ============================================================================
// BeltCaptureWriter - directory gating and file output
// ============================================================================

namespace {

/// RAII temp directory, same pattern as InputShaperCacheTestFixture.
class TempDirFixture {
  public:
    TempDirFixture() {
        dir_ = fs::temp_directory_path() /
               ("helix_belt_capture_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
    }
    ~TempDirFixture() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    [[nodiscard]] const fs::path& dir() const {
        return dir_;
    }

  private:
    fs::path dir_;
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_files(const fs::path& dir) {
    size_t n = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        (void)entry;
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE("BeltCaptureWriter with an empty directory is disabled and writes nothing",
          "[belt_tension][belt_capture]") {
    BeltCaptureWriter writer("", 151.0f);
    CHECK_FALSE(writer.enabled());

    CaptureVerdict v;
    v.accepted = true;
    // Neither call should throw or touch the filesystem - there's no
    // directory to touch, and enabled() being false is the whole contract.
    writer.write_event(two_exact_samples(), nullptr, 3200.0f, v);
    writer.write_quiet(two_exact_samples(), 3200.0f);
}

TEST_CASE("BeltCaptureWriter writes the detection window and ring-down as separate files",
          "[belt_tension][belt_capture]") {
    TempDirFixture tmp;
    BeltCaptureWriter writer(tmp.dir().string(), 151.0f);
    REQUIRE(writer.enabled());

    const auto detection = two_exact_samples();
    const std::vector<AccelSample> ringdown = {{1.0f, 5.0f, 6.0f, 7.0f}};

    CaptureVerdict v;
    v.accepted = true;
    v.rms_ratio = 12.3f;
    v.estimate_hz = 86.0f;
    writer.write_event(detection, &ringdown, 3091.2f, v);

    REQUIRE(count_files(tmp.dir()) == 2);

    fs::path detection_file, ringdown_file;
    for (const auto& entry : fs::directory_iterator(tmp.dir())) {
        const std::string name = entry.path().filename().string();
        if (name.find("_detection.csv") != std::string::npos) {
            detection_file = entry.path();
        } else if (name.find("_ringdown.csv") != std::string::npos) {
            ringdown_file = entry.path();
        }
    }
    REQUIRE_FALSE(detection_file.empty());
    REQUIRE_FALSE(ringdown_file.empty());

    // The two files hold different buffers - the whole point of writing them
    // separately - so their data rows must differ, and each must parse back
    // to the buffer it was given.
    const auto parsed_detection = parse_accel_csv(read_file(detection_file));
    const auto parsed_ringdown = parse_accel_csv(read_file(ringdown_file));
    REQUIRE(parsed_detection.size() == detection.size());
    REQUIRE(parsed_ringdown.size() == ringdown.size());
    CHECK(parsed_detection[0].x == detection[0].x);
    CHECK(parsed_ringdown[0].x == ringdown[0].x);

    CHECK(read_file(detection_file).find("detection window") != std::string::npos);
    CHECK(read_file(ringdown_file).find("ring-down") != std::string::npos);
}

TEST_CASE("BeltCaptureWriter writes only the detection window when no ring-down was extracted",
          "[belt_tension][belt_capture]") {
    TempDirFixture tmp;
    BeltCaptureWriter writer(tmp.dir().string(), 151.0f);

    CaptureVerdict v;
    v.accepted = false;
    v.reject = PluckReject::TOO_SOFT;
    v.rms_ratio = 4.0f;
    writer.write_event(two_exact_samples(), nullptr, 3200.0f, v);

    REQUIRE(count_files(tmp.dir()) == 1);
}

TEST_CASE("BeltCaptureWriter sequence numbers keep same-second events from colliding",
          "[belt_tension][belt_capture]") {
    TempDirFixture tmp;
    BeltCaptureWriter writer(tmp.dir().string(), 151.0f);

    CaptureVerdict v;
    v.accepted = false;
    v.reject = PluckReject::NOT_A_PLUCK;
    v.rms_ratio = 5.0f;

    // Two events back to back - realistic when a rejection immediately
    // precedes an accept, both resolved within the same wall-clock second.
    writer.write_event(two_exact_samples(), nullptr, 3200.0f, v);
    writer.write_event(two_exact_samples(), nullptr, 3200.0f, v);

    REQUIRE(count_files(tmp.dir()) == 2);

    bool saw_0000 = false, saw_0001 = false;
    for (const auto& entry : fs::directory_iterator(tmp.dir())) {
        const std::string name = entry.path().filename().string();
        saw_0000 |= name.find("event_0000_") != std::string::npos;
        saw_0001 |= name.find("event_0001_") != std::string::npos;
    }
    CHECK(saw_0000);
    CHECK(saw_0001);
}

TEST_CASE("BeltCaptureWriter creates its destination directory if missing",
          "[belt_tension][belt_capture]") {
    TempDirFixture tmp;
    const fs::path nested = tmp.dir() / "does" / "not" / "exist";
    REQUIRE_FALSE(fs::exists(nested));

    BeltCaptureWriter writer(nested.string(), 151.0f);
    CHECK(fs::exists(nested));

    CaptureVerdict v;
    v.accepted = true;
    writer.write_event(two_exact_samples(), nullptr, 3200.0f, v);
    CHECK(count_files(nested) == 1);
}

TEST_CASE("BeltCaptureWriter writes the quiet buffer with no verdict",
          "[belt_tension][belt_capture]") {
    TempDirFixture tmp;
    BeltCaptureWriter writer(tmp.dir().string(), 151.0f);

    writer.write_quiet(two_exact_samples(), 3200.0f);

    REQUIRE(count_files(tmp.dir()) == 1);
    for (const auto& entry : fs::directory_iterator(tmp.dir())) {
        const std::string name = entry.path().filename().string();
        CHECK(name.find("quiet_0000") != std::string::npos);
        const std::string content = read_file(entry.path());
        CHECK(content.find("verdict=") == std::string::npos);
        CHECK(parse_accel_csv(content).size() == 2);
    }
}
