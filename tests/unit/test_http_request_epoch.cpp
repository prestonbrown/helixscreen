// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_http_request_epoch.cpp
 * @brief A REST reply from the previous printer never lands as the current printer's data.
 */

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "http_executor.h"
#include "http_request_epoch.h"
#include "mock_http_file_server.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "moonraker_file_transfer_api.h"
#include "printer_state.h"

#include <chrono>
#include <future>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

struct Outcome {
    int successes = 0;
    int lost = 0;
};

/// Downloads mock.png while the single slow worker is held, runs `while_held`, then lets it go.
template <typename F> Outcome download_with_worker_held(F while_held) {
    helix::MockHttpFileServer server;
    REQUIRE(server.start());
    helix::test::FakeMoonrakerClient client;
    const std::string base = server.base_url();
    MoonrakerFileTransferAPI transfers(client, base);

    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    helix::http::HttpExecutor::slow().submit([released] { released.wait(); });

    Outcome outcome;
    std::promise<void> done;
    transfers.download_file(
        "gcodes", "mock.png",
        [&](const std::string&) {
            ++outcome.successes;
            done.set_value();
        },
        [&](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::CONNECTION_LOST) {
                ++outcome.lost;
            }
            done.set_value();
        });

    while_held();
    release.set_value();
    REQUIRE(done.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return outcome;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "HTTP epoch: a reply under the same printer lands",
                 "[http_epoch][multi-printer]") {
    const Outcome outcome = download_with_worker_held([] {});
    CHECK(outcome.successes == 1);
    CHECK(outcome.lost == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "HTTP epoch: a reply that finishes after a switch is lost",
                 "[http_epoch][multi-printer]") {
    const Outcome outcome = download_with_worker_held([] { helix::http_epoch::advance(); });
    CHECK(outcome.successes == 0);
    CHECK(outcome.lost == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "HTTP epoch: only a new base URL starts a new epoch",
                 "[http_epoch][multi-printer]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.set_http_base_url("http://10.0.0.1:7125");
    const uint64_t first = helix::http_epoch::current();

    api.set_http_base_url("http://10.0.0.1:7125");
    CHECK(helix::http_epoch::current() == first);

    api.set_http_base_url("http://10.0.0.2:7125");
    CHECK(helix::http_epoch::current() != first);
}
