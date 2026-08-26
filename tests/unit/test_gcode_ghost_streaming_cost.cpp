// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_ghost_streaming_cost.cpp
 * @brief What the background ghost pass actually costs a streamed file.
 *
 * The policy in gcode_ghost_sampling.h is unit-tested on its own. This is the
 * other half: driving the real GCodeLayerRenderer ghost thread against a real
 * GCodeStreamingController over a real file on disk, and counting the reads it
 * causes. The bug being pinned was never in the arithmetic — it was that the
 * pass called get_layer_segments() once per layer of the file, so the cost was
 * a whole second parse no matter what any constant said.
 *
 * The device this was found on is a K2 Plus: dual-core Cortex-A7, 488MB, a
 * 133MB file. None of that is reproducible here, and wall-clock on a desktop
 * says nothing about it. What IS reproducible, and is the quantity that made
 * the device unusable, is the number of seek-and-parses the pass performs —
 * hardware-independent, and the thing the fix changes. These measure that
 * directly by counting read_range() calls through an injected data source.
 */

#include "../test_helpers/gcode_layer_renderer_test_access.h"
#include "gcode_data_source.h"
#include "gcode_ghost_sampling.h"
#include "gcode_layer_renderer.h"
#include "gcode_streaming_controller.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::gcode;

namespace {

/// Layers in the synthetic file. Far short of the ~5000 the 133MB K2 file
/// carries, but well past any plausible sampling budget, which is what makes
/// the difference between "bounded" and "one per layer" unambiguous.
constexpr int SYNTH_LAYERS = 2000;

/// Moves per layer — enough that a layer is a real parse rather than one line.
constexpr int MOVES_PER_LAYER = 40;

/// Canvas the ghost renders into. 320 rows is representative of the preview on
/// a 480x800 panel, and is what sets the sampling budget.
constexpr int CANVAS_W = 400;
constexpr int CANVAS_H = 320;

/// A G-code file with a known layer count, shaped like a sliced print: a Z
/// move opening each layer, then extrusions that trace a square.
std::string make_layered_gcode(int layers, int moves_per_layer) {
    std::ostringstream out;
    out << "; synthetic ghost-cost fixture\nG28\nG90\nM82\n";
    double e = 0.0;
    for (int layer = 0; layer < layers; ++layer) {
        out << "G1 Z" << (0.2 * (layer + 1)) << " F1200\n";
        for (int move = 0; move < moves_per_layer; ++move) {
            const double x = 20.0 + (move % 10) * 6.0;
            const double y = 20.0 + ((move / 10) % 4) * 6.0;
            e += 0.05;
            out << "G1 X" << x << " Y" << y << " E" << e << " F1800\n";
        }
    }
    out << "; end\n";
    return out.str();
}

class TempFile {
  public:
    explicit TempFile(const std::string& content) {
        char tmpl[] = "/tmp/helix_ghost_cost_XXXXXX";
        int fd = mkstemp(tmpl);
        REQUIRE(fd != -1);
        close(fd);
        path_ = tmpl;
        std::ofstream out(path_, std::ios::binary);
        out << content;
    }
    ~TempFile() {
        std::remove(path_.c_str());
    }
    const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
};

/// Wraps FileDataSource and counts what the layer cache asks it for. The index
/// build reads through indexable_file_path() rather than this, so after a reset
/// the counters describe layer loads only.
class CountingDataSource : public GCodeDataSource {
  public:
    explicit CountingDataSource(const std::string& path) : inner_(path) {}

    std::vector<char> read_range(uint64_t offset, uint32_t length) override {
        reads_.fetch_add(1);
        bytes_.fetch_add(length);
        return inner_.read_range(offset, length);
    }
    uint64_t file_size() const override {
        return inner_.file_size();
    }
    bool supports_range_requests() const override {
        return inner_.supports_range_requests();
    }
    std::string source_name() const override {
        return inner_.source_name();
    }
    bool is_valid() const override {
        return inner_.is_valid();
    }
    std::string indexable_file_path() const override {
        return inner_.indexable_file_path();
    }

    void reset() {
        reads_.store(0);
        bytes_.store(0);
    }
    size_t reads() const {
        return reads_.load();
    }
    uint64_t bytes() const {
        return bytes_.load();
    }

  private:
    FileDataSource inner_;
    std::atomic<size_t> reads_{0};
    std::atomic<uint64_t> bytes_{0};
};

/// Opens the fixture for streaming and reports the live counter alongside.
struct StreamingFixture {
    TempFile file;
    CountingDataSource* counter = nullptr;
    GCodeStreamingController controller;

    StreamingFixture() : file(make_layered_gcode(SYNTH_LAYERS, MOVES_PER_LAYER)) {
        auto source = std::make_unique<CountingDataSource>(file.path());
        counter = source.get();
        REQUIRE(controller.open_source(std::move(source)));
        REQUIRE(controller.is_open());
    }
};

} // namespace

TEST_CASE("ghost pass over a streamed file does not read one layer per layer",
          "[gcode][ghost][streaming][cost]") {
    StreamingFixture fx;

    const int layer_count = static_cast<int>(fx.controller.get_layer_count());
    // The fixture is only meaningful if the index found roughly the layers it
    // was built with, and enough of them to exceed any sampling budget.
    REQUIRE(layer_count > ghost_sample_budget(CANVAS_H) * 2);

    GCodeLayerRenderer renderer;
    renderer.set_canvas_size(CANVAS_W, CANVAS_H);
    renderer.set_streaming_controller(&fx.controller);
    renderer.set_view_mode(ViewMode::FRONT);

    // Everything before this point — the index build in particular — is not what
    // is being measured.
    fx.controller.wait_for_prefetch_idle();
    fx.counter->reset();

    GCodeLayerRendererTestAccess::run_ghost_pass(renderer);
    fx.controller.wait_for_prefetch_idle();

    CHECK(GCodeLayerRendererTestAccess::ghost_completed(renderer));

    const size_t reads = fx.counter->reads();
    const auto plan = plan_ghost_sampling(layer_count, /*streaming=*/true, CANVAS_H);
    INFO("layers=" << layer_count << " reads=" << reads << " planned=" << plan.count
                   << " budget=" << ghost_sample_budget(CANVAS_H));

    // The regression: one seek-and-parse per layer of the file, plus the
    // neighbours each one warmed. Anything near the layer count is that bug.
    CHECK(reads < static_cast<size_t>(layer_count));

    // A layer may take more than one read_range() to cover, so this is not a
    // strict equality with the plan — but it must stay within a small multiple
    // of it, and nowhere near one-per-layer.
    CHECK(reads <= static_cast<size_t>(plan.count) * 4);
}

TEST_CASE("ghost pass does not drive the prefetch worker across the file",
          "[gcode][ghost][streaming][cost]") {
    StreamingFixture fx;

    const int layer_count = static_cast<int>(fx.controller.get_layer_count());
    REQUIRE(layer_count > ghost_sample_budget(CANVAS_H) * 2);

    GCodeLayerRenderer renderer;
    renderer.set_canvas_size(CANVAS_W, CANVAS_H);
    renderer.set_streaming_controller(&fx.controller);
    renderer.set_view_mode(ViewMode::FRONT);

    fx.controller.wait_for_prefetch_idle();
    fx.counter->reset();

    GCodeLayerRendererTestAccess::run_ghost_pass(renderer);

    // Give any prefetch the pass scheduled a chance to run before counting, so
    // this fails loudly if the pass goes back to warming neighbours rather than
    // racing the assertion.
    fx.controller.wait_for_prefetch_idle();

    const size_t reads = fx.counter->reads();
    const auto plan = plan_ghost_sampling(layer_count, /*streaming=*/true, CANVAS_H);
    INFO("layers=" << layer_count << " reads=" << reads << " planned=" << plan.count);

    // Each get_layer_segments() used to schedule a prefetch of radius 3 around
    // the layer it loaded — up to 7 layers touched per layer visited. A strided
    // pass never reads those, so they are pure cost; this pins that they are no
    // longer paid.
    CHECK(reads <= static_cast<size_t>(plan.count) * 4);
}

TEST_CASE("a streamed ghost still spans the whole model", "[gcode][ghost][streaming][cost]") {
    // Bounding the work must not quietly bound the *height*: a ghost that stops
    // partway up claims the print ends there. Checked against the real index's
    // layer count rather than the synthetic constant, since the parser decides
    // what counts as a layer.
    StreamingFixture fx;
    const int layer_count = static_cast<int>(fx.controller.get_layer_count());
    const auto plan = plan_ghost_sampling(layer_count, /*streaming=*/true, CANVAS_H);

    const int last_visited = plan.step * (plan.count - 1);
    INFO("layers=" << layer_count << " step=" << plan.step << " last=" << last_visited);
    CHECK(last_visited < layer_count);
    CHECK(last_visited >= layer_count - plan.step);
}
