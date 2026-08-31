// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_HAS_ALSA

#include "alsa_sound_backend.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE("ALSASoundBackend::mono_to_stereo interleaves L=R", "[sound][alsa]") {
    std::vector<float> mono = {0.1f, 0.5f, -0.3f, 1.0f};
    std::vector<float> stereo(mono.size() * 2);

    ALSASoundBackend::mono_to_stereo(mono.data(), stereo.data(), mono.size());

    REQUIRE(stereo[0] == Catch::Approx(0.1f));
    REQUIRE(stereo[1] == Catch::Approx(0.1f));
    REQUIRE(stereo[2] == Catch::Approx(0.5f));
    REQUIRE(stereo[3] == Catch::Approx(0.5f));
    REQUIRE(stereo[4] == Catch::Approx(-0.3f));
    REQUIRE(stereo[5] == Catch::Approx(-0.3f));
    REQUIRE(stereo[6] == Catch::Approx(1.0f));
    REQUIRE(stereo[7] == Catch::Approx(1.0f));
}

TEST_CASE("ALSASoundBackend::mono_to_stereo empty input produces empty output", "[sound][alsa]") {
    std::vector<float> stereo(16, 0.0f);

    ALSASoundBackend::mono_to_stereo(nullptr, stereo.data(), 0);

    // Output buffer should be untouched (0 frames written)
    for (auto v : stereo) {
        REQUIRE(v == 0.0f);
    }
}

TEST_CASE("ALSASoundBackend::float_to_s16 normal range", "[sound][alsa]") {
    std::vector<float> src = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
    std::vector<int16_t> dst(src.size());

    ALSASoundBackend::float_to_s16(src.data(), dst.data(), src.size());

    REQUIRE(dst[0] == 0);
    REQUIRE(dst[1] >= 16381);
    REQUIRE(dst[1] <= 16385);
    REQUIRE(dst[2] <= -16381);
    REQUIRE(dst[2] >= -16385);
    REQUIRE(dst[3] == 32767);
    REQUIRE(dst[4] == -32767);
}

TEST_CASE("ALSASoundBackend::float_to_s16 boundary values", "[sound][alsa]") {
    std::vector<float> src = {1.0f, -1.0f, 0.0f};
    std::vector<int16_t> dst(src.size());

    ALSASoundBackend::float_to_s16(src.data(), dst.data(), src.size());

    REQUIRE(dst[0] == 32767);
    REQUIRE(dst[1] == -32767);
    REQUIRE(dst[2] == 0);
}

TEST_CASE("ALSASoundBackend::float_to_s16 clamps out-of-range values", "[sound][alsa]") {
    std::vector<float> src = {1.5f, -1.5f, 2.0f, -100.0f};
    std::vector<int16_t> dst(src.size());

    ALSASoundBackend::float_to_s16(src.data(), dst.data(), src.size());

    REQUIRE(dst[0] == 32767);
    REQUIRE(dst[1] == -32767);
    REQUIRE(dst[2] == 32767);
    REQUIRE(dst[3] == -32767);
}

TEST_CASE("ALSASoundBackend::float_to_s16 silence stays silent", "[sound][alsa]") {
    std::vector<float> src(8, 0.0f);
    std::vector<int16_t> dst(src.size());

    ALSASoundBackend::float_to_s16(src.data(), dst.data(), src.size());

    for (auto v : dst) {
        REQUIRE(v == 0);
    }
}

// ============================================================================
// suspend()/resume() — idle parking + the resume handoff.
//
// Regression (v0.99.114, 08f49c420): resume() was fire-and-forget, so the
// sequencer's step clock started while the render thread was still parked.
// Notes published in that gap were overwritten un-rendered — short sounds
// never played, longer ones lost their beginning. resume() must not return
// until the render thread has completed a render pass.
//
// All tests use the ALSA "null" PCM plugin: a real in-process device with no
// hardware dependency, so the threading paths run against genuine snd_pcm_*
// calls deterministically.
// ============================================================================

namespace {

/// Render-source probe: counts passes through the render loop. Every pass
/// sleeps before setting the flag so a pass can never complete between
/// resume() returning and the assertion reading the flag — the assertion is
/// about ordering, not scheduling speed.
class RenderProbe {
  public:
    void operator()(float*, size_t, int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        passes_.fetch_add(1, std::memory_order_relaxed);
        rendered_.store(true, std::memory_order_relaxed);
    }
    void reset() {
        rendered_.store(false, std::memory_order_relaxed);
    }
    bool rendered() const {
        return rendered_.load(std::memory_order_relaxed);
    }
    uint32_t passes() const {
        return passes_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<uint32_t> passes_{0};
    std::atomic<bool> rendered_{false};
};

/// The resume() contract is a BOUNDED synchronous handoff: production waits up
/// to 100ms for the render thread to ack one full pass, then deliberately
/// degrades to the old asynchronous resume rather than stall the sequencer on
/// a wedged render thread (ALSASoundBackend::resume()). Asserting that the
/// handoff always landed over-asserts — under a starved runner (96 shards) a
/// render pass can legitimately outlast the bound. What distinguishes the
/// fire-and-forget bug this pins (v0.99.114, 08f49c420) is resume() returning
/// with neither the ack nor the bound spent.
bool handoff_satisfied(const RenderProbe& probe, std::chrono::steady_clock::time_point t0) {
    if (probe.rendered())
        return true;
    // 80ms floor under the 100ms bound: wait_for never wakes before its
    // deadline without the predicate holding, and caller-side wake latency
    // only adds to the measured elapsed.
    return std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(80);
}

/// Wait for the render thread to actually stop calling the render source.
///
/// suspend() only sets a flag; the render thread drops the device when it next
/// observes it, so parking is asynchronous and no fixed sleep is ever correct.
/// A 50ms nap was close enough to right that it passed in isolation and failed
/// intermittently under 96-shard parallelism, where the thread could still land
/// one pass after the snapshot (the assertion read `4 == 3`).
///
/// Returns the settled pass count, or std::nullopt if the thread never went
/// quiet inside the timeout — which is the real regression this guards.
std::optional<uint32_t>
wait_until_parked(const RenderProbe& probe,
                  std::chrono::milliseconds quiet_window = std::chrono::milliseconds(50),
                  std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const uint32_t before = probe.passes();
        std::this_thread::sleep_for(quiet_window);
        if (probe.passes() == before)
            return before;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("ALSASoundBackend::resume() blocks until a render pass completed", "[sound][alsa]") {
    ALSASoundBackend backend;
    REQUIRE(backend.initialize("null"));

    RenderProbe probe;
    backend.set_render_source([&probe](float* buf, size_t n, int sr) { probe(buf, n, sr); });

    // Let the render thread run, then park it as the sequencer does when idle.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    backend.suspend();

    // Must be genuinely parked before probe.reset(), or a still-in-flight pass
    // sets rendered_ after the reset and the assertion below passes for the
    // wrong reason.
    REQUIRE(wait_until_parked(probe).has_value());

    probe.reset();
    const auto handoff_t0 = std::chrono::steady_clock::now();
    backend.resume();

    // The whole regression: the sequencer starts its step clock the instant
    // resume() returns. If the render thread is still parked at that moment,
    // notes published meanwhile are overwritten un-rendered — unless the
    // bound expired, which is the documented degrade, not the bug.
    REQUIRE(handoff_satisfied(probe, handoff_t0));

    backend.clear_render_source();
    backend.shutdown();
}

TEST_CASE("ALSASoundBackend::suspend() parks the render thread", "[sound][alsa]") {
    ALSASoundBackend backend;
    REQUIRE(backend.initialize("null"));

    RenderProbe probe;
    backend.set_render_source([&probe](float* buf, size_t n, int sr) { probe(buf, n, sr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    backend.suspend();

    const auto after_park = wait_until_parked(probe);
    REQUIRE(after_park.has_value()); // suspend() never quiesced the render thread

    // And it must STAY parked, not merely pause between periods.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(probe.passes() == *after_park);

    // And the park must not strand the device: the handoff still works.
    probe.reset();
    const auto handoff_t0 = std::chrono::steady_clock::now();
    backend.resume();
    REQUIRE(handoff_satisfied(probe, handoff_t0));

    backend.clear_render_source();
    backend.shutdown();
}

TEST_CASE("ALSASoundBackend::resume() handoff survives repeated suspend/resume cycles",
          "[sound][alsa]") {
    ALSASoundBackend backend;
    REQUIRE(backend.initialize("null"));

    RenderProbe probe;
    backend.set_render_source([&probe](float* buf, size_t n, int sr) { probe(buf, n, sr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < 50; ++i) {
        backend.suspend();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        probe.reset();
        const auto handoff_t0 = std::chrono::steady_clock::now();
        backend.resume();
        REQUIRE(handoff_satisfied(probe, handoff_t0));
    }

    backend.clear_render_source();
    backend.shutdown();
}

// ============================================================================
// Short sounds vs the ALSA start threshold (prestonbrown/helixscreen#1337)
//
// start_threshold is set to (buffer_size - period_size), derived from the
// NEGOTIATED buffer. Real hardware returned 1024/8192 where we asked for
// 256/2048, making the threshold 7168 frames = 162.5 ms at 44.1 kHz. Most UI
// sounds are shorter than that, so the stream never leaves PREPARED. Draining
// a PREPARED stream is a no-op, and the prepare() on the next resume discards
// the queue - the sound is silently swallowed. The park path must therefore
// start such a stream before draining it.
// ============================================================================

TEST_CASE("ALSASoundBackend: queued-but-unstarted stream is started before drain",
          "[sound][alsa]") {
    // PREPARED with audio queued is exactly the short-sound case: drain alone
    // would discard it.
    CHECK(ALSASoundBackend::needs_start_before_drain(SND_PCM_STATE_PREPARED, true));
}

TEST_CASE("ALSASoundBackend: nothing queued needs no start before drain", "[sound][alsa]") {
    // PREPARED with an empty queue: starting would only risk an immediate
    // underrun, and there is nothing to play out.
    CHECK_FALSE(ALSASoundBackend::needs_start_before_drain(SND_PCM_STATE_PREPARED, false));
}

TEST_CASE("ALSASoundBackend: an already-running stream is not restarted", "[sound][alsa]") {
    // A sound long enough to cross the threshold already started; drain plays
    // its tail out on its own. Restarting a RUNNING stream is an error.
    CHECK_FALSE(ALSASoundBackend::needs_start_before_drain(SND_PCM_STATE_RUNNING, true));
    CHECK_FALSE(ALSASoundBackend::needs_start_before_drain(SND_PCM_STATE_XRUN, true));
    CHECK_FALSE(ALSASoundBackend::needs_start_before_drain(SND_PCM_STATE_SETUP, true));
}

#endif // HELIX_HAS_ALSA
