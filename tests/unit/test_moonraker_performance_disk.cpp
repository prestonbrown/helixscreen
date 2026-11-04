// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_performance_disk.cpp
 * @brief The Moonraker performance source reads the gcodes disk only once a consumer asks
 * for it, and publishes the figure it gets back.
 *
 * Run with: ./build/bin/helix-tests "[performance][moonraker_disk]"
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "mock_disk_usage.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_performance_source.h"

#include <optional>

#include "../catch_amalgamated.hpp"

using helix::perf::MoonrakerPerformanceSource;
using helix::perf::PerfSample;

namespace {

struct DiskSourceFixture : public LVGLUITestFixture {
    DiskSourceFixture()
        : mock(MoonrakerClientMock::PrinterType::VORON_24), mock_api(mock, state()),
          source(&mock_api) {
        source.set_callback([this](const PerfSample& s) {
            last = s;
            ++samples;
        });
    }
    ~DiskSourceFixture() override {
        source.stop();
        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    MoonrakerClientMock mock;
    MoonrakerAPI mock_api;
    MoonrakerPerformanceSource source;
    std::optional<PerfSample> last;
    int samples = 0;
};

} // namespace

TEST_CASE_METHOD(DiskSourceFixture, "disk usage is read once a consumer wants it",
                 "[performance][moonraker_disk]") {
    source.start();
    drain();
    CHECK_FALSE(last.has_value());

    source.set_disk_wanted(true);
    drain();

    REQUIRE(last.has_value());
    REQUIRE(last->host_disk_total_mb.has_value());
    REQUIRE(last->host_disk_free_mb.has_value());
    const helix::mock::DiskMb disk = helix::mock::gcodes_disk_mb();
    CHECK(*last->host_disk_total_mb == disk.total);
    CHECK(*last->host_disk_free_mb == disk.free);
}

TEST_CASE_METHOD(DiskSourceFixture, "disk usage is not read while nobody wants it",
                 "[performance][moonraker_disk]") {
    source.start();
    source.set_disk_wanted(false);
    drain();
    CHECK(samples == 0);
}
