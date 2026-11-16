// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/sweep_preview_fakes.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "live_sweep_preview.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using namespace helix::calibration::test;

namespace {

constexpr double kLeadS = 1.5;

SweepAxisGain peak_gain(float peak_hz) {
    return [peak_hz](float f) {
        const float d = (f - peak_hz) / 3.8f;
        const float level = 800.0f * std::sqrt(0.02f + 1.0f / (1.0f + d * d));
        return std::array<float, 3>{0.5f * level, 0.5f * level, 0.1f * level};
    };
}

SweepSynthSpec spec_for(SweepPlan plan) {
    SweepSynthSpec spec;
    spec.plan = plan;
    spec.drive_start_s = 1.7;
    spec.noise_mm_s2 = 30.0f;
    return spec;
}

LiveSweepTarget target_for(SweepPlan plan) {
    return LiveSweepTarget{"/tmp/k.sock", "adxl345", plan};
}

void drain() {
    helix::ui::UpdateQueue::instance().drain();
}

/// Plays a synthetic sweep into whatever source the preview attached,
/// with console hints leading the motion by kLeadS.
struct SweepPlayer {
    SweepSynthSpec spec;
    SweepSynth synth;

    explicit SweepPlayer(SweepSynthSpec s) : spec(s), synth(s, peak_gain(15.0f)) {}

    float hint_at(double t) const {
        const double f =
            spec.plan.freq_start_hz + spec.plan.hz_per_sec * (t + kLeadS - spec.drive_start_s);
        return std::floor(std::min(static_cast<float>(f), spec.plan.freq_end_hz));
    }

    /// One 300-sample batch; returns false once the source is gone.
    bool step(LiveSweepPreview& preview, SourceLog& log, bool drain_after = true) {
        const double t = synth.time_s();
        if (t + kLeadS >= spec.drive_start_s) {
            preview.note_console_frequency(hint_at(t));
        }
        AccelBatch batch;
        batch.samples = synth.next(300);
        if (log.last == nullptr) {
            // Before the first console line nothing is attached yet.
            return preview.state() == LiveSweepPreview::State::ARMED;
        }
        log.last->push(batch);
        if (drain_after) {
            drain();
        }
        return true;
    }

    void play_until(LiveSweepPreview& preview, SourceLog& log, double until_s) {
        while (synth.time_s() < until_s && step(preview, log)) {
        }
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview attaches on the first console line",
                 "[sweep_preview][session]") {
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    preview.arm(target_for(plan), [](const std::vector<SweepPoint>&) {});
    CHECK(preview.state() == LiveSweepPreview::State::ARMED);
    CHECK(log.created == 0);

    preview.note_console_frequency(NAN);
    preview.note_console_frequency(0.0f);
    preview.note_console_frequency(-3.0f);
    CHECK(preview.state() == LiveSweepPreview::State::ARMED);
    CHECK(log.created == 0);

    preview.note_console_frequency(5.0f);
    CHECK(preview.state() == LiveSweepPreview::State::STREAMING);
    CHECK(log.created == 1);
    CHECK(log.started == 1);
    CHECK(log.socket_path == "/tmp/k.sock");
    CHECK(log.accel_chip == "adxl345");
    CHECK(log.plan.freq_start_hz == 5.0f);
    CHECK(log.plan.freq_end_hz == 135.0f);
    CHECK(log.plan.hz_per_sec == 1.0f);

    preview.stop();
    CHECK(preview.state() == LiveSweepPreview::State::IDLE);
    CHECK(log.stopped == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview creates nothing unless armed",
                 "[sweep_preview][session]") {
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    preview.note_console_frequency(7.0f);
    CHECK(log.created == 0);
    CHECK(preview.state() == LiveSweepPreview::State::IDLE);

    preview.arm(target_for({5.0f, 135.0f, 1.0f}), [](const std::vector<SweepPoint>&) {});
    preview.stop();
    preview.note_console_frequency(7.0f);
    CHECK(log.created == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview delivers every point and stops itself",
                 "[sweep_preview][session]") {
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    const SweepPlan plan{5.0f, 25.0f, 1.0f};
    std::vector<size_t> sizes;
    std::vector<SweepPoint> last;
    preview.arm(target_for(plan), [&](const std::vector<SweepPoint>& points) {
        sizes.push_back(points.size());
        last = points;
    });

    SweepPlayer player(spec_for(plan));
    player.play_until(preview, log, 1.7 + 20.0 + 2.0);

    REQUIRE_FALSE(sizes.empty());
    CHECK(std::is_sorted(sizes.begin(), sizes.end()));
    REQUIRE(last.size() == 21);
    for (size_t i = 0; i < last.size(); ++i) {
        CHECK(last[i].freq_hz == static_cast<float>(5 + i));
    }
    CHECK(preview.state() == LiveSweepPreview::State::DONE);
    CHECK(log.stopped == 1);
    CHECK(preview.points().size() == 21);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview::stop drops points still queued",
                 "[sweep_preview][session]") {
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    const SweepPlan plan{5.0f, 25.0f, 1.0f};
    int calls = 0;
    preview.arm(target_for(plan), [&](const std::vector<SweepPoint>&) { ++calls; });

    SweepPlayer player(spec_for(plan));
    player.play_until(preview, log, 8.0);
    const int calls_before = calls;
    const size_t delivered = preview.points().size();
    REQUIRE(calls_before > 0);

    // Points finish in these batches but are not drained before stop().
    for (int i = 0; i < 30; ++i) {
        REQUIRE(player.step(preview, log, false));
    }
    preview.stop();
    drain();
    CHECK(calls == calls_before);
    CHECK(preview.points().size() == delivered);
    CHECK(preview.state() == LiveSweepPreview::State::IDLE);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview::stop from inside on_points is final",
                 "[sweep_preview][session]") {
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    const SweepPlan plan{5.0f, 25.0f, 1.0f};
    int calls = 0;
    preview.arm(target_for(plan), [&](const std::vector<SweepPoint>&) {
        ++calls;
        preview.stop();
    });

    SweepPlayer player(spec_for(plan));
    player.play_until(preview, log, 1.7 + 20.0 + 2.0);
    CHECK(calls == 1);
    CHECK(preview.state() == LiveSweepPreview::State::IDLE);
    CHECK(log.stopped == 1);
    CHECK(log.created == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview ends on a source error",
                 "[sweep_preview][session]") {
    helix::LogCapture capture;
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    const SweepPlan plan{5.0f, 25.0f, 1.0f};
    preview.arm(target_for(plan), [](const std::vector<SweepPoint>&) {});

    SweepPlayer player(spec_for(plan));
    player.play_until(preview, log, 8.0);
    const size_t delivered = preview.points().size();
    REQUIRE(delivered > 0);
    REQUIRE(log.last != nullptr);
    log.last->fail("subscribe failed");
    drain();
    CHECK(preview.state() == LiveSweepPreview::State::DONE);
    CHECK(log.stopped == 1);
    CHECK(preview.points().size() == delivered);
    CHECK(capture.count_containing("subscribe failed") == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview::finish stops after the grace",
                 "[sweep_preview][session]") {
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    preview.arm(target_for(plan), [](const std::vector<SweepPoint>&) {});

    SweepPlayer player(spec_for(plan));
    player.play_until(preview, log, 1.7 + 35.0); // the drive reaches 40 Hz
    REQUIRE(preview.state() == LiveSweepPreview::State::STREAMING);
    preview.finish();
    const double finish_at = player.synth.time_s();
    // One batch past the grace: the first batch after finish() sets the deadline.
    player.play_until(preview, log, finish_at + kPreviewFinishGraceS - 0.3);
    CHECK(preview.state() == LiveSweepPreview::State::STREAMING);
    player.play_until(preview, log, finish_at + kPreviewFinishGraceS + 0.3);
    CHECK(preview.state() == LiveSweepPreview::State::DONE);
    CHECK(log.stopped == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview re-arms cleanly", "[sweep_preview][session]") {
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    const SweepPlan plan{5.0f, 25.0f, 1.0f};
    preview.arm(target_for(plan), [](const std::vector<SweepPoint>&) {});
    SweepPlayer player(spec_for(plan));
    player.play_until(preview, log, 8.0);
    REQUIRE_FALSE(preview.points().empty());

    preview.arm(target_for(plan), [](const std::vector<SweepPoint>&) {});
    CHECK(log.stopped == 1);
    CHECK(preview.points().empty());
    CHECK(preview.state() == LiveSweepPreview::State::ARMED);
    CHECK(log.created == 1);
    preview.note_console_frequency(5.0f);
    CHECK(log.created == 2);
    CHECK(preview.state() == LiveSweepPreview::State::STREAMING);
}

TEST_CASE_METHOD(LVGLTestFixture, "HELIX_SWEEP_PREVIEW_OFF keeps the preview idle",
                 "[sweep_preview][session]") {
    helix::ScopedEnv off("HELIX_SWEEP_PREVIEW_OFF", "1");
    helix::LogCapture capture;
    SourceLog log;
    LiveSweepPreview preview;
    preview.set_source_factory(manual_factory(log));
    preview.arm(target_for({5.0f, 25.0f, 1.0f}), [](const std::vector<SweepPoint>&) {});
    CHECK(preview.state() == LiveSweepPreview::State::IDLE);
    preview.note_console_frequency(5.0f);
    CHECK(log.created == 0);
    preview.arm(target_for({5.0f, 25.0f, 1.0f}), [](const std::vector<SweepPoint>&) {});
    CHECK(capture.count_containing("HELIX_SWEEP_PREVIEW_OFF") == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LiveSweepPreview drops a real thread's points after stop",
                 "[sweep_preview][session][slow]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    auto factory = [plan](const SweepPlan&) -> std::unique_ptr<IAccelBatchSource> {
        return std::make_unique<LoopBatchSource>(spec_for(plan), peak_gain(40.0f));
    };

    SECTION("stop") {
        LiveSweepPreview preview;
        preview.set_source_factory(factory);
        int calls = 0;
        preview.arm(target_for(plan), [&](const std::vector<SweepPoint>&) { ++calls; });
        // A fixed hint keeps 5..8.5 Hz in the search grid; the lock then holds.
        preview.note_console_frequency(8.0f);
        REQUIRE(preview.state() == LiveSweepPreview::State::STREAMING);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (calls == 0 && std::chrono::steady_clock::now() < deadline) {
            drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(calls > 0);
        preview.stop();
        const int after_stop = calls;
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            drain();
        }
        CHECK(calls == after_stop);
    }

    SECTION("destruction while streaming") {
        auto preview = std::make_unique<LiveSweepPreview>();
        preview->set_source_factory(factory);
        preview->arm(target_for(plan), [](const std::vector<SweepPoint>&) {});
        preview->note_console_frequency(8.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        preview.reset();
        drain();
        SUCCEED("destroyed while streaming");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "query_live_sweep_facts asks the mock printer",
                 "[sweep_preview][session]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api(client, get_printer_state());
    std::optional<LiveSweepFacts> facts;
    int calls = 0;
    query_live_sweep_facts(api, [&](LiveSweepFacts f) {
        ++calls;
        facts = std::move(f);
    });
    for (int i = 0; i < 50 && !facts; ++i) {
        process_lvgl(10);
    }
    REQUIRE(facts.has_value());
    CHECK(calls == 1);
    CHECK(facts->socket_path == "/tmp/helix-mock-klippy-" + std::to_string(getpid()) + ".sock");
    CHECK(facts->socket_reachable);
    CHECK(facts->cfg.accel_chip == "adxl345");
    CHECK(facts->cfg.from_printer);
}
