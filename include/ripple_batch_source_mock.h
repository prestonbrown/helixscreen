// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mock printer's accelerometer for a motor ripple sweep. Each speed
// script the mock runs makes every live source emit that speed's capture
// before the script completes, the way a real stream fills while the moves
// run. Ripple follows k x belt speed / 0.8 mm through a resonance; the
// compensation enable macro scales it down.

#include "live_sweep_preview.h"
#include "motor_ripple_types.h"

#include <cstdint>
#include <string>

namespace helix::calibration::ripple {

inline constexpr float kMockRippleResonanceHz = 216.0f;
inline constexpr float kMockRippleQ = 8.0f;
inline constexpr float kMockRipplePeakMmS2 = 2500.0f;
inline constexpr float kMockRippleFloorMmS2 = 40.0f;
inline constexpr float kMockRippleCompensated = 0.25f;
inline constexpr float kMockRippleElectricalMm = 0.8f;
inline constexpr float kMockRippleCaptureS = 2.5f;

class MockRippleBatchSource final : public IAccelBatchSource {
  public:
    explicit MockRippleBatchSource(float sample_rate_hz);
    ~MockRippleBatchSource() override;
    MockRippleBatchSource(const MockRippleBatchSource&) = delete;
    MockRippleBatchSource& operator=(const MockRippleBatchSource&) = delete;

    bool start(const std::string& socket_path, const std::string& accel_chip,
               BatchCallback on_batch, ErrorCallback on_error) override;
    void stop() override;
    [[nodiscard]] float sample_rate_hz() const override {
        return rate_;
    }

    /// Amplitude the mock gives harmonic `k` at `belt_speed`.
    [[nodiscard]] static float ripple_amplitude(float belt_speed, int k, bool compensated);

    /// One speed's capture, straight to the batch callback. mock_ripple_on_script() calls it.
    void emit(float belt_speed, bool compensated);

  private:
    float rate_;
    float t_ = 0.0f;
    uint32_t seed_ = 1;
    BatchCallback on_batch_;
};

/// The mock's gcode handler calls this with every script it runs.
void mock_ripple_on_script(const std::string& script);

/// HELIX_MOCK_ACCEL_RATE, else 3200 Hz.
[[nodiscard]] float mock_ripple_sample_rate();

} // namespace helix::calibration::ripple
