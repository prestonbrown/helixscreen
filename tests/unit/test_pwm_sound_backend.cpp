// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/pwm_sound_backend_test_access.h"
#include "pwm_sound_backend.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sched.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

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

// ============================================================================
// PCM render loop (virtual clock)
// ============================================================================

namespace {

/// Virtual monotonic clock. wait_until() jumps `now` to the deadline instead
/// of sleeping, so a whole park/resume cycle runs in milliseconds of real
/// time. arm_jump() schedules a one-shot forward jump that fires inside the
/// next read() — the catch-up test uses it to simulate a scheduling stall.
/// wait_until() also records every deadline it receives; the pacing test
/// asserts on them after the render thread is joined.
class VirtualClock {
  public:
    /// Virtual time starts here — far from machine-uptime-scale CLOCK_MONOTONIC
    /// values, so a schedule base wrongly derived from the real clock is
    /// trivially distinguishable.
    static constexpr int64_t START_NS = 1000000000LL;

    int64_t read() {
        if (jump_armed_.exchange(false)) {
            now_ += jump_ns_;
        }
        return now_;
    }

    void wait_until(int64_t deadline) {
        if (deadline > now_) {
            now_ = deadline;
        }
        deadlines_.push_back(deadline);
        deadline_count_.fetch_add(1, std::memory_order_release);
    }

    void arm_jump(int64_t ns) {
        jump_ns_ = ns;
        jump_armed_.store(true);
    }

    /// Deadlines received so far (safe to poll from the test thread).
    int64_t deadline_count() const {
        return deadline_count_.load(std::memory_order_acquire);
    }

    /// Full deadline log — only valid after the render thread is joined.
    const std::vector<int64_t>& deadlines() const {
        return deadlines_;
    }

  private:
    // now_/deadlines_ are only touched from the render thread (both seams run
    // there); the test thread only arms the jump and reads the counters.
    int64_t now_ = START_NS;
    int64_t jump_ns_ = 0;
    std::atomic<bool> jump_armed_{false};
    std::vector<int64_t> deadlines_;
    std::atomic<int64_t> deadline_count_{0};
};

/// Mock sysfs + initialized backend with the virtual clock and a shrunken
/// park threshold installed BEFORE the render thread starts.
///
/// Any shared state a source lambda captures by reference must be declared
/// BEFORE the harness: the destructor clears the render source (joining the
/// render thread) and runs before those locals die.
struct PwmVirtualRun {
    std::string base;
    std::unique_ptr<PWMSoundBackend> backend;
    VirtualClock clock;

    explicit PwmVirtualRun(int park_threshold) : base(create_mock_sysfs(0, 6)) {
        backend = std::make_unique<PWMSoundBackend>(base, 0, 6);
        REQUIRE(backend->initialize());
        PWMSoundBackendTestAccess::set_park_silent_buffers(*backend, park_threshold);
        PWMSoundBackendTestAccess::set_now_fn(*backend, [this] { return clock.read(); });
        PWMSoundBackendTestAccess::set_wait_until_fn(
            *backend, [this](int64_t deadline) { clock.wait_until(deadline); });
    }

    ~PwmVirtualRun() {
        backend->clear_render_source();
        cleanup_mock_sysfs(base);
    }
};

/// Poll pred() every 1 ms of real time until true or timeout.
bool wait_for(const std::function<bool()>& pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace

TEST_CASE("render loop parks after sustained silence", "[sound][pwm]") {
    PwmVirtualRun run(2);
    run.backend->set_render_source(
        [](float* buf, size_t frames, int) { std::memset(buf, 0, frames * sizeof(float)); });

    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::parked(*run.backend); }));

    // Park drops both duty and enable: the channel is fully off, not held
    // at a mid-scale duty.
    REQUIRE(read_sysfs_file(run.base + "/pwmchip0/pwm6/enable") == "0");
    REQUIRE(read_sysfs_file(run.base + "/pwmchip0/pwm6/duty_cycle") == "0");
}

TEST_CASE("parked loop stops writing duty", "[sound][pwm]") {
    PwmVirtualRun run(2);
    run.backend->set_render_source(
        [](float* buf, size_t frames, int) { std::memset(buf, 0, frames * sizeof(float)); });

    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::parked(*run.backend); }));

    // 20 ms of real time is thousands of virtual park polls; the probe path
    // must not touch duty_cycle.
    uint64_t w1 = PWMSoundBackendTestAccess::duty_writes(*run.backend);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uint64_t w2 = PWMSoundBackendTestAccess::duty_writes(*run.backend);
    REQUIRE(w2 == w1);
}

TEST_CASE("parked loop resumes on non-silent buffer", "[sound][pwm]") {
    std::atomic<bool> silent{true}; // declared before run: outlives the join
    PwmVirtualRun run(2);
    run.backend->set_render_source([&silent](float* buf, size_t frames, int) {
        float v = silent.load() ? 0.0f : 0.5f;
        for (size_t i = 0; i < frames; i++) {
            buf[i] = v;
        }
    });

    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::parked(*run.backend); }));

    silent.store(false);
    REQUIRE(wait_for([&] { return !PWMSoundBackendTestAccess::parked(*run.backend); }));

    // Resume re-enables the channel and duty writes continue.
    REQUIRE(read_sysfs_file(run.base + "/pwmchip0/pwm6/enable") == "1");
    uint64_t w1 = PWMSoundBackendTestAccess::duty_writes(*run.backend);
    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::duty_writes(*run.backend) > w1; }));
}

TEST_CASE("non-zero buffer resets the silence run", "[sound][pwm]") {
    std::atomic<int> full_buffers{0}; // 512-frame source calls (probes excluded)
    std::atomic<bool> gate_open{false};
    PwmVirtualRun run(3);
    run.backend->set_render_source([&](float* buf, size_t frames, int) {
        if (frames != static_cast<size_t>(PWMSoundBackend::PCM_RENDER_BUFFER_FRAMES)) {
            std::memset(buf, 0, frames * sizeof(float)); // probes stay silent
            return;
        }
        // Pattern: z, z, NON-ZERO, z, z, z... The non-zero buffer must
        // restart the run, so parking needs 3 zeros AFTER it, not 3 total.
        int n = full_buffers.fetch_add(1);
        float v = (n == 2) ? 0.5f : 0.0f;
        for (size_t i = 0; i < frames; i++) {
            buf[i] = v;
        }
        if (n == 5) {
            // Hold inside the 6th buffer's render call: buffers 0-4 are fully
            // processed (run == 2), and buffer 5 cannot complete or park
            // while gated, so the mid-run assertions below are deterministic.
            while (!gate_open.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    });

    REQUIRE(wait_for([&] { return full_buffers.load() >= 6; }));
    // z, z, nz, z, z: the run restarted at the non-zero buffer and now
    // stands at 2 — no park, even though 4 of the last 5 buffers were silent.
    REQUIRE(PWMSoundBackendTestAccess::silent_buffer_run(*run.backend) == 2);
    REQUIRE_FALSE(PWMSoundBackendTestAccess::parked(*run.backend));

    gate_open.store(true);
    // The 6th buffer completes a fresh run of 3 consecutive zeros.
    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::parked(*run.backend); }));
}

TEST_CASE("catch-up drops samples instead of bursting", "[sound][pwm]") {
    std::atomic<int> full_buffers{0};
    // Duty writes per full buffer. Element k is finalized by source call k+1;
    // only the render thread touches the vector until the test joins it.
    std::vector<uint64_t> buffer_writes;
    uint64_t writes_at_start = 0;
    PwmVirtualRun run(8); // non-zero content never parks
    PWMSoundBackend* backend = run.backend.get();
    run.backend->set_render_source([&](float* buf, size_t frames, int) {
        if (frames != static_cast<size_t>(PWMSoundBackend::PCM_RENDER_BUFFER_FRAMES)) {
            return;
        }
        int n = full_buffers.fetch_add(1);
        for (size_t i = 0; i < frames; i++) {
            buf[i] = 0.5f;
        }
        if (n > 0) {
            buffer_writes.push_back(PWMSoundBackendTestAccess::duty_writes(*backend) -
                                    writes_at_start);
        }
        writes_at_start = PWMSoundBackendTestAccess::duty_writes(*backend);
        if (n == 2) {
            // Arm a +10-sample-interval jump: it fires on the render thread's
            // next now_fn() call, mid-buffer — a scheduling stall.
            run.clock.arm_jump(10 * (1000000000LL / PWMSoundBackend::PCM_SAMPLE_RATE));
        }
    });

    REQUIRE(wait_for([&] { return full_buffers.load() >= 5; }));
    run.backend->clear_render_source();
    // Finalize the last buffer's delta now that the loop is joined.
    buffer_writes.push_back(PWMSoundBackendTestAccess::duty_writes(*run.backend) - writes_at_start);

    REQUIRE(buffer_writes.size() >= 4);
    // Normal pacing writes every sample of the buffer.
    REQUIRE(buffer_writes[0] == 512);
    REQUIRE(buffer_writes[1] == 512);
    // The stalled buffer drops the missed samples instead of bursting them.
    REQUIRE(buffer_writes[2] < 512);
    REQUIRE(buffer_writes[2] > 480); // bounded drop: ~10 samples, not the buffer
    // And the loop keeps running normally afterwards.
    REQUIRE(buffer_writes[3] == 512);
}

TEST_CASE("steady playback paces from the virtual clock's base", "[sound][pwm]") {
    PwmVirtualRun run(8); // non-silent content never parks
    run.backend->set_render_source([](float* buf, size_t frames, int) {
        for (size_t i = 0; i < frames; i++) {
            buf[i] = 0.5f;
        }
    });

    const int64_t interval = 1000000000LL / PWMSoundBackend::PCM_SAMPLE_RATE;
    REQUIRE(wait_for([&] { return run.clock.deadline_count() >= 24; }));
    run.backend->clear_render_source();

    const std::vector<int64_t>& deadlines = run.clock.deadlines();
    REQUIRE(deadlines.size() >= 24);
    // The schedule base must come from the injected clock (virtual start +
    // one sample interval for the first deadline). A CLOCK_MONOTONIC base
    // sits at machine-uptime scale — far outside this window.
    REQUIRE(deadlines[0] - VirtualClock::START_NS <= 2 * interval);
    // Steady pacing: every consecutive deadline is exactly one sample
    // interval after the previous one.
    for (size_t i = 1; i < deadlines.size(); i++) {
        REQUIRE(deadlines[i] - deadlines[i - 1] == interval);
    }
}

TEST_CASE("a render source that fills nothing parks - stale buffer content is not audio",
          "[sound][pwm]") {
    std::atomic<int> full_buffers{0};
    PwmVirtualRun run(2);
    run.backend->set_render_source([&full_buffers](float* buf, size_t frames, int) {
        if (frames != static_cast<size_t>(PWMSoundBackend::PCM_RENDER_BUFFER_FRAMES)) {
            return; // probe calls: write nothing
        }
        // First buffer: real audio. Every later buffer: return without
        // touching the buffer — only the loop's pre-clear makes those silent;
        // without it the stale non-zero content would play (and never park).
        if (full_buffers.fetch_add(1) == 0) {
            for (size_t i = 0; i < frames; i++) {
                buf[i] = 0.5f;
            }
        }
    });

    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::parked(*run.backend); }));
}

TEST_CASE("render thread applies SCHED_IDLE", "[sound][pwm]") {
    PwmVirtualRun run(8);
    run.backend->set_render_source([](float* buf, size_t frames, int) {
        for (size_t i = 0; i < frames; i++) {
            buf[i] = 0.25f;
        }
    });

    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::duty_writes(*run.backend) > 0; }));

    // CHECK, not REQUIRE: an unprivileged sandbox may refuse SCHED_IDLE; the
    // loop logs that and continues at SCHED_OTHER (policy stays -1).
    CHECK(PWMSoundBackendTestAccess::applied_sched_policy(*run.backend) == SCHED_IDLE);
}

// ============================================================================
// PCM render loop (real clock) — [slow], excluded from make test-run
// ============================================================================

TEST_CASE("render loop paces near 8 kHz with real clock", "[sound][pwm][slow]") {
    auto base = create_mock_sysfs(0, 6);
    PWMSoundBackend backend(base, 0, 6);
    REQUIRE(backend.initialize());

    backend.set_render_source([](float* buf, size_t frames, int sr) {
        for (size_t i = 0; i < frames; i++) {
            buf[i] = 0.4f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) /
                                     static_cast<float>(sr));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    uint64_t writes = PWMSoundBackendTestAccess::duty_writes(backend);
    backend.clear_render_source();

    // 0.5 s at 8 kHz = 4000 writes. Far undershooting means over-sleeping;
    // overshooting means the deadline pacing broke and the loop bursts.
    REQUIRE(writes >= 2500);
    REQUIRE(writes <= 9000);

    cleanup_mock_sysfs(base);
}

TEST_CASE("clear_render_source joins promptly while parked", "[sound][pwm][slow]") {
    auto base = create_mock_sysfs(0, 6);
    PWMSoundBackend backend(base, 0, 6);
    REQUIRE(backend.initialize());
    PWMSoundBackendTestAccess::set_park_silent_buffers(backend, 2);

    backend.set_render_source(
        [](float* buf, size_t frames, int) { std::memset(buf, 0, frames * sizeof(float)); });

    // Real clock: 2 silent buffers of 64 ms each before the park lands.
    REQUIRE(wait_for([&] { return PWMSoundBackendTestAccess::parked(backend); }, 5000));

    auto t0 = std::chrono::steady_clock::now();
    backend.clear_render_source();
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    // Parked polls sleep 10 ms per cycle; the join must not wait out a buffer.
    REQUIRE(elapsed_ms < 250);

    cleanup_mock_sysfs(base);
}
