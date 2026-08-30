// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pwm_sound_backend.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using Catch::Approx;

// ============================================================================
// Helpers — temp directory sysfs mock
// ============================================================================

/// Create a fake sysfs PWM directory structure under a temp path.
/// Returns the base_path (caller must clean up).
///
/// Creates: <base>/pwmchip<chip>/pwm<channel>/{period,duty_cycle,enable}
static std::string create_mock_sysfs(int chip = 0, int channel = 6) {
    // Use mkdtemp for a unique temp directory
    std::string tmpl = "/tmp/pwm_test_XXXXXX";
    char* dir = mkdtemp(tmpl.data());
    REQUIRE(dir != nullptr);

    std::string base(dir);
    std::string pwm_dir =
        base + "/pwmchip" + std::to_string(chip) + "/pwm" + std::to_string(channel);

    // Create directory hierarchy
    std::string mkdir_cmd = "mkdir -p " + pwm_dir;
    REQUIRE(system(mkdir_cmd.c_str()) == 0);

    // Create the sysfs control files with initial values
    std::ofstream(pwm_dir + "/period") << "0";
    std::ofstream(pwm_dir + "/duty_cycle") << "0";
    std::ofstream(pwm_dir + "/enable") << "0";

    return base;
}

/// Read the contents of a sysfs mock file as a string
static std::string read_sysfs_file(const std::string& path) {
    std::ifstream f(path);
    std::string content;
    std::getline(f, content);
    return content;
}

/// Clean up a mock sysfs directory
static void cleanup_mock_sysfs(const std::string& base) {
    std::string cmd = "rm -rf " + base;
    system(cmd.c_str());
}

// ============================================================================
// Sysfs path construction
// ============================================================================

TEST_CASE("PWM backend constructs correct channel path", "[sound][pwm]") {
    PWMSoundBackend backend("/sys/class/pwm", 0, 6);
    REQUIRE(backend.channel_path() == "/sys/class/pwm/pwmchip0/pwm6");
}

TEST_CASE("PWM backend path works with different chip/channel", "[sound][pwm]") {
    PWMSoundBackend backend("/sys/class/pwm", 2, 3);
    REQUIRE(backend.channel_path() == "/sys/class/pwm/pwmchip2/pwm3");
}

TEST_CASE("PWM backend path works with custom base path", "[sound][pwm]") {
    PWMSoundBackend backend("/tmp/fake_sysfs", 1, 0);
    REQUIRE(backend.channel_path() == "/tmp/fake_sysfs/pwmchip1/pwm0");
}

// ============================================================================
// Frequency to period conversion
// ============================================================================

TEST_CASE("freq_to_period_ns converts 440 Hz correctly", "[sound][pwm]") {
    // 1e9 / 440 = 2272727.27... → 2272727
    uint32_t period = PWMSoundBackend::freq_to_period_ns(440.0f);
    REQUIRE(period == 2272727);
}

TEST_CASE("freq_to_period_ns converts 1000 Hz correctly", "[sound][pwm]") {
    uint32_t period = PWMSoundBackend::freq_to_period_ns(1000.0f);
    REQUIRE(period == 1000000);
}

TEST_CASE("freq_to_period_ns converts 20000 Hz correctly", "[sound][pwm]") {
    uint32_t period = PWMSoundBackend::freq_to_period_ns(20000.0f);
    REQUIRE(period == 50000);
}

TEST_CASE("freq_to_period_ns returns 0 for zero frequency", "[sound][pwm]") {
    uint32_t period = PWMSoundBackend::freq_to_period_ns(0.0f);
    REQUIRE(period == 0);
}

TEST_CASE("freq_to_period_ns returns 0 for negative frequency", "[sound][pwm]") {
    uint32_t period = PWMSoundBackend::freq_to_period_ns(-100.0f);
    REQUIRE(period == 0);
}

TEST_CASE("freq_to_period_ns handles A4 tuning frequency", "[sound][pwm]") {
    // 1e9 / 440 = 2272727 ns (within rounding)
    uint32_t period = PWMSoundBackend::freq_to_period_ns(440.0f);
    // Allow +-1 for rounding
    REQUIRE(period >= 2272726);
    REQUIRE(period <= 2272728);
}

// ============================================================================
// Waveform duty cycle mapping
// ============================================================================

TEST_CASE("Square wave maps to 50% duty ratio", "[sound][pwm]") {
    float ratio = PWMSoundBackend::waveform_duty_ratio(Waveform::SQUARE);
    REQUIRE(ratio == Approx(0.50f));
}

TEST_CASE("Saw wave maps to 25% duty ratio", "[sound][pwm]") {
    float ratio = PWMSoundBackend::waveform_duty_ratio(Waveform::SAW);
    REQUIRE(ratio == Approx(0.25f));
}

TEST_CASE("Triangle wave maps to 35% duty ratio", "[sound][pwm]") {
    float ratio = PWMSoundBackend::waveform_duty_ratio(Waveform::TRIANGLE);
    REQUIRE(ratio == Approx(0.35f));
}

TEST_CASE("Sine wave maps to 40% duty ratio", "[sound][pwm]") {
    float ratio = PWMSoundBackend::waveform_duty_ratio(Waveform::SINE);
    REQUIRE(ratio == Approx(0.40f));
}

// ============================================================================
// Capability flags
// ============================================================================

TEST_CASE("PWM backend reports correct capabilities", "[sound][pwm]") {
    PWMSoundBackend backend;

    // PWM can't do real waveform synthesis — only approximates via duty cycle
    REQUIRE_FALSE(backend.supports_waveforms());

    // PWM has amplitude control via duty cycle scaling
    REQUIRE(backend.supports_amplitude());

    // PWM can't do DSP filters
    REQUIRE_FALSE(backend.supports_filter());

    // Sysfs is slower than audio buffer — needs larger tick
    REQUIRE(backend.min_tick_ms() == Approx(2.0f));
}

// ============================================================================
// PCM constants
// ============================================================================

TEST_CASE("PCM sample rate is 8 kHz", "[sound][pwm]") {
    // Piezo roll-off is ~3-4 kHz; rendering faster buys no audible content
    // while halving the time each duty-cycle write gets.
    REQUIRE(PWMSoundBackend::PCM_SAMPLE_RATE == 8000);
}

TEST_CASE("PCM carrier frequency stays at 62.5 kHz", "[sound][pwm]") {
    REQUIRE(PWMSoundBackend::PCM_CARRIER_HZ == 62500);
}

TEST_CASE("PCM pacing constants are pinned", "[sound][pwm]") {
    REQUIRE(PWMSoundBackend::PCM_RENDER_BUFFER_FRAMES == 512);
    REQUIRE(PWMSoundBackend::PCM_PARK_SILENT_BUFFERS == 8);
    REQUIRE(PWMSoundBackend::PCM_PARK_POLL_NS == 10000000LL);
    REQUIRE(PWMSoundBackend::PCM_CATCHUP_MAX_SAMPLES == 2);
    REQUIRE(PWMSoundBackend::PCM_SPIN_BUDGET_NS == 20000LL);
}

TEST_CASE("park_probe_frames derives frames from poll interval and rate", "[sound][pwm]") {
    // PCM_PARK_POLL_NS * PCM_SAMPLE_RATE / 1e9 = 10000000 * 8000 / 1e9 = 80
    REQUIRE(PWMSoundBackend::park_probe_frames() == 80);
}

// ============================================================================
// PCM pacing statics
// ============================================================================

TEST_CASE("samples_behind measures whole-sample lateness", "[sound][pwm]") {
    const int64_t base = 1000000000000LL;
    const int64_t interval = 125000LL; // 8 kHz
    const int64_t deadline = base + 40 * interval;

    REQUIRE(PWMSoundBackend::samples_behind(deadline, base, 40, interval) == 0);
    REQUIRE(PWMSoundBackend::samples_behind(deadline + 1, base, 40, interval) == 0);
    REQUIRE(PWMSoundBackend::samples_behind(deadline + interval - 1, base, 40, interval) == 0);
    REQUIRE(PWMSoundBackend::samples_behind(deadline + interval, base, 40, interval) == 1);
    REQUIRE(PWMSoundBackend::samples_behind(deadline + 3 * interval, base, 40, interval) == 3);
}

TEST_CASE("samples_behind floors rather than truncates when ahead of schedule", "[sound][pwm]") {
    const int64_t base = 1000000000000LL;
    const int64_t interval = 125000LL; // 8 kHz
    const int64_t deadline = base + 40 * interval;

    // 2.5 intervals ahead: floor(-2.5) = -3 (truncation would say -2)
    REQUIRE(PWMSoundBackend::samples_behind(deadline - 2 * interval - interval / 2, base, 40,
                                            interval) == -3);
    // One ns past a whole two intervals ahead: floor(-(2i+1)/i) = -3
    REQUIRE(PWMSoundBackend::samples_behind(base - 2 * interval - 1, base, 0, interval) == -3);
}

TEST_CASE("resync_sample_index clamps to zero and floors", "[sound][pwm]") {
    const int64_t base = 1000000000000LL;
    const int64_t interval = 125000LL; // 8 kHz

    REQUIRE(PWMSoundBackend::resync_sample_index(base - 1, base, interval) == 0);
    REQUIRE(PWMSoundBackend::resync_sample_index(base, base, interval) == 0);
    REQUIRE(PWMSoundBackend::resync_sample_index(base + 7 * interval, base, interval) == 7);
    REQUIRE(PWMSoundBackend::resync_sample_index(base + 8 * interval - 1, base, interval) == 7);
}

// ============================================================================
// initialize() / shutdown() lifecycle
// ============================================================================

/// Create a fake sysfs pwmchip directory whose channel is NOT exported.
/// Returns the base_path (caller must clean up).
///
/// Creates: <base>/pwmchip<chip>/{export,npwm} — no pwm<channel> directory.
///
/// A plain-file mock cannot reproduce the kernel materializing pwm<channel>
/// in response to the export write, so initialize() still fails on this mock.
/// Tests against it pin the export write itself, not the materialization.
///
/// `export` is seeded with "0" so an assertion of a written channel number
/// distinguishes "backend wrote it" from "file was never touched".
static std::string create_mock_sysfs_unexported(int chip = 0) {
    std::string tmpl = "/tmp/pwm_test_XXXXXX";
    char* dir = mkdtemp(tmpl.data());
    REQUIRE(dir != nullptr);

    std::string base(dir);
    std::string chip_dir = base + "/pwmchip" + std::to_string(chip);

    std::string mkdir_cmd = "mkdir -p " + chip_dir;
    REQUIRE(system(mkdir_cmd.c_str()) == 0);

    std::ofstream(chip_dir + "/export") << "0";
    std::ofstream(chip_dir + "/npwm") << "16";

    return base;
}

TEST_CASE("PWM backend initializes with valid sysfs paths", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);

    PWMSoundBackend backend(base, 0, 6);
    REQUIRE(backend.initialize());

    cleanup_mock_sysfs(base);
}

TEST_CASE("PWM backend fails to initialize with missing sysfs paths", "[sound][pwm]") {
    PWMSoundBackend backend("/tmp/nonexistent_pwm_path_12345", 0, 6);
    REQUIRE_FALSE(backend.initialize());
}

// ============================================================================
// Channel auto-export on initialize()
// ============================================================================

TEST_CASE("initialize writes channel number to export when channel missing", "[sound][pwm]") {
    auto base = create_mock_sysfs_unexported(0);

    PWMSoundBackend backend(base, 0, 6);
    // The mock never materializes pwm6, so the outcome stays failure — but the
    // export write must have happened.
    REQUIRE_FALSE(backend.initialize());
    REQUIRE(read_sysfs_file(base + "/pwmchip0/export") == "6");

    cleanup_mock_sysfs(base);
}

TEST_CASE("initialize does not touch export when channel exists", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);

    // Sentinel: if initialize() writes to export, this gets clobbered
    std::ofstream(base + "/pwmchip0/export") << "42";

    PWMSoundBackend backend(base, 0, 6);
    REQUIRE(backend.initialize());
    REQUIRE(read_sysfs_file(base + "/pwmchip0/export") == "42");

    cleanup_mock_sysfs(base);
}

TEST_CASE("initialize tolerates unwritable export when channel already present", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);

    // A directory at the export path makes any open-for-write fail EISDIR
    // deterministically, even running as root.
    std::string mkdir_cmd = "mkdir -p " + base + "/pwmchip0/export";
    REQUIRE(system(mkdir_cmd.c_str()) == 0);

    PWMSoundBackend backend(base, 0, 6);
    REQUIRE(backend.initialize());

    cleanup_mock_sysfs(base);
}

TEST_CASE("initialize returns false when no pwmchip exists", "[sound][pwm]") {
    std::string tmpl = "/tmp/pwm_test_XXXXXX";
    char* dir = mkdtemp(tmpl.data());
    REQUIRE(dir != nullptr);
    std::string base(dir);

    PWMSoundBackend backend(base, 0, 6);
    REQUIRE_FALSE(backend.initialize());

    cleanup_mock_sysfs(base);
}

TEST_CASE("PWM backend shutdown disables PWM output", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // Play a tone to enable PWM
    backend.set_tone(440.0f, 1.0f, 0.5f);
    REQUIRE(backend.is_enabled());

    // Shutdown should disable
    backend.shutdown();
    REQUIRE_FALSE(backend.is_enabled());

    // Verify sysfs file says disabled
    std::string enable_path = base + "/pwmchip0/pwm6/enable";
    REQUIRE(read_sysfs_file(enable_path) == "0");

    cleanup_mock_sysfs(base);
}

// ============================================================================
// set_tone() writes correct sysfs values
// ============================================================================

TEST_CASE("set_tone writes period to sysfs", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    backend.set_tone(1000.0f, 1.0f, 0.5f);

    // 1000 Hz → 1000000 ns period
    REQUIRE(read_sysfs_file(pwm_dir + "/period") == "1000000");

    cleanup_mock_sysfs(base);
}

TEST_CASE("set_tone writes duty_cycle to sysfs", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // Square wave (default), amplitude 1.0 → duty = period * 0.50
    // At 1000 Hz, period = 1000000, duty = 500000
    backend.set_tone(1000.0f, 1.0f, 0.5f);

    REQUIRE(read_sysfs_file(pwm_dir + "/duty_cycle") == "500000");

    cleanup_mock_sysfs(base);
}

TEST_CASE("set_tone enables PWM output", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    REQUIRE_FALSE(backend.is_enabled());

    backend.set_tone(440.0f, 1.0f, 0.5f);

    REQUIRE(backend.is_enabled());
    REQUIRE(read_sysfs_file(pwm_dir + "/enable") == "1");

    cleanup_mock_sysfs(base);
}

TEST_CASE("set_tone with amplitude scaling adjusts duty cycle", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // Square wave, amplitude 0.5 → duty = period * 0.50 * 0.5 = period * 0.25
    // At 1000 Hz, period = 1000000, duty = 250000
    backend.set_tone(1000.0f, 0.5f, 0.5f);

    REQUIRE(read_sysfs_file(pwm_dir + "/duty_cycle") == "250000");

    cleanup_mock_sysfs(base);
}

TEST_CASE("set_tone with zero amplitude disables PWM", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // First enable
    backend.set_tone(440.0f, 1.0f, 0.5f);
    REQUIRE(backend.is_enabled());

    // Zero amplitude → should disable
    backend.set_tone(440.0f, 0.0f, 0.5f);
    REQUIRE_FALSE(backend.is_enabled());
    REQUIRE(read_sysfs_file(pwm_dir + "/enable") == "0");

    cleanup_mock_sysfs(base);
}

TEST_CASE("set_tone with zero frequency disables PWM", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    backend.set_tone(440.0f, 1.0f, 0.5f);
    REQUIRE(backend.is_enabled());

    backend.set_tone(0.0f, 1.0f, 0.5f);
    REQUIRE_FALSE(backend.is_enabled());

    cleanup_mock_sysfs(base);
}

// ============================================================================
// silence() behavior
// ============================================================================

TEST_CASE("silence disables PWM output", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    backend.set_tone(440.0f, 1.0f, 0.5f);
    REQUIRE(backend.is_enabled());

    backend.silence();

    REQUIRE_FALSE(backend.is_enabled());
    REQUIRE(read_sysfs_file(pwm_dir + "/enable") == "0");

    cleanup_mock_sysfs(base);
}

TEST_CASE("silence is safe to call when already silent", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // Should not crash or error
    backend.silence();
    backend.silence();

    REQUIRE_FALSE(backend.is_enabled());

    cleanup_mock_sysfs(base);
}

// ============================================================================
// Waveform switching affects duty cycle
// ============================================================================

TEST_CASE("set_waveform changes duty cycle on next set_tone", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // Default is square (50% duty)
    backend.set_tone(1000.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/duty_cycle") == "500000");

    // Switch to saw (25% duty)
    backend.set_waveform(Waveform::SAW);
    backend.set_tone(1000.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/duty_cycle") == "250000");

    // Switch to triangle (35% duty)
    backend.set_waveform(Waveform::TRIANGLE);
    backend.set_tone(1000.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/duty_cycle") == "350000");

    // Switch to sine (40% duty)
    backend.set_waveform(Waveform::SINE);
    backend.set_tone(1000.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/duty_cycle") == "400000");

    cleanup_mock_sysfs(base);
}

// ============================================================================
// Enable/disable sequencing — avoid redundant writes
// ============================================================================

TEST_CASE("Repeated set_tone does not re-write enable if already enabled", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    backend.set_tone(440.0f, 1.0f, 0.5f);
    REQUIRE(backend.is_enabled());

    // Write something else to the enable file to detect if it gets rewritten
    std::ofstream(pwm_dir + "/enable") << "42";

    // Second set_tone should NOT rewrite enable (already enabled)
    backend.set_tone(880.0f, 1.0f, 0.5f);

    // If the backend skipped the enable write, the file still says "42"
    REQUIRE(read_sysfs_file(pwm_dir + "/enable") == "42");

    cleanup_mock_sysfs(base);
}

// ============================================================================
// Frequency changes update period correctly
// ============================================================================

TEST_CASE("Changing frequency updates period in sysfs", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    backend.set_tone(440.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/period") == "2272727");

    backend.set_tone(880.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/period") == "1136363");

    cleanup_mock_sysfs(base);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("set_tone before initialize does not crash", "[sound][pwm]") {
    PWMSoundBackend backend("/tmp/nonexistent", 0, 6);

    // Should not crash — just a no-op since not initialized
    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.silence();

    REQUIRE_FALSE(backend.is_enabled());
}

TEST_CASE("PWM backend handles very high frequency", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // 20 kHz → period = 50000 ns
    backend.set_tone(20000.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/period") == "50000");

    cleanup_mock_sysfs(base);
}

TEST_CASE("PWM backend handles very low frequency", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // 20 Hz → period = 50000000 ns
    backend.set_tone(20.0f, 1.0f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/period") == "50000000");

    cleanup_mock_sysfs(base);
}

TEST_CASE("PWM backend amplitude clamped to 0-1 range", "[sound][pwm]") {
    auto base = create_mock_sysfs(0, 6);
    std::string pwm_dir = base + "/pwmchip0/pwm6";

    PWMSoundBackend backend(base, 0, 6);
    backend.initialize();

    // Amplitude > 1.0 should be clamped to 1.0
    // Square wave at 1000 Hz: period=1000000, duty = 1000000 * 0.5 * clamp(1.5,0,1) = 500000
    backend.set_tone(1000.0f, 1.5f, 0.5f);
    REQUIRE(read_sysfs_file(pwm_dir + "/duty_cycle") == "500000");

    cleanup_mock_sysfs(base);
}
