// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_connection_refresh.cpp
 * @brief The connection observer re-lists on a reconnect, never on its own
 *        registration
 *
 * On the K-Touch the panel is set up on its first visit, already connected, and
 * activated before the observer's queued registration fire drains. A listing
 * from that fire supersedes the first visit's, so the first fill waits for a
 * second round trip.
 */

#include "ui_panel_print_select.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "connection_state.h"
#include "moonraker_file_api.h"

#include <atomic>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void set_connection(ConnectionState s) {
    lv_subject_set_int(get_printer_state().network_state().get_printer_connection_state_subject(),
                       static_cast<int>(s));
}

/// Counts directory listings and answers them through the mock client.
class CountingFileAPI : public MoonrakerFileAPI {
  public:
    explicit CountingFileAPI(helix::IMoonrakerClient& client) : MoonrakerFileAPI(client) {}

    void get_directory(const std::string& root, const std::string& path,
                       FileListCallback on_success, ErrorCallback on_error) override {
        ++listings;
        MoonrakerFileAPI::get_directory(root, path, std::move(on_success), std::move(on_error));
    }

    std::atomic<int> listings{0};
};

class CountingAPI : public MoonrakerAPI {
  public:
    CountingAPI(helix::IMoonrakerClient& client, PrinterState& state)
        : MoonrakerAPI(client, state) {
        file_api_ = std::make_unique<CountingFileAPI>(client);
    }
    CountingFileAPI& counting_files() {
        return static_cast<CountingFileAPI&>(*file_api_);
    }
};

class ConnectionRefreshFixture : private PrintSelectGlobalStateReset,
                                 public PrintSelectPanelFixture {
  public:
    ConnectionRefreshFixture()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Unregistered,
                                  PrintSelectVisit::Deferred, PrintSelectApi::Real,
                                  PrintSelectConnectionAtSetup::Connected) {
        auto counting = std::make_unique<CountingAPI>(mock_client_, get_printer_state());
        counting_ = counting.get();
        panel_->set_api(counting_);
        api_ = std::move(counting);
    }

    int listings() const {
        return counting_->counting_files().listings.load();
    }

    CountingAPI* counting_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(ConnectionRefreshFixture,
                 "Print select: the first visit lists once; the observer's registration adds none",
                 "[print_select][refresh][connection]") {
    // The observer's registration fire is still queued: the first visit
    // activates ahead of it, as the deferred build on the K-Touch does.
    NavigationManager::instance().set_active(PanelId::PrintSelect);
    REQUIRE(listings() == 1);

    drain();
    REQUIRE(listings() == 1);
}

TEST_CASE_METHOD(ConnectionRefreshFixture, "Print select: a reconnect lists again",
                 "[print_select][refresh][connection]") {
    NavigationManager::instance().set_active(PanelId::PrintSelect);
    drain();
    REQUIRE(listings() == 1);

    set_connection(ConnectionState::RECONNECTING);
    drain();
    set_connection(ConnectionState::CONNECTED);
    drain();
    REQUIRE(listings() == 2);
}
