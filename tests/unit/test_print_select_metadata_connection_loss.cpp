// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_metadata_connection_loss.cpp
 * @brief A metadata request lost with the connection is fetched again, never
 *        answered by downloading the gcode head
 *
 * When a file's metadata request fails, the panel falls back to a metascan and
 * then to extracting a thumbnail from the first bytes of the gcode. A request
 * failed because the connection dropped says nothing about the file, and on a
 * flapping link every card on screen would otherwise start a download into the
 * moment the link is weakest.
 */

#include "ui_panel_print_select.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "moonraker_error.h"
#include "moonraker_file_api.h"
#include "moonraker_file_transfer_api.h"

#include <atomic>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Answers metadata and metascan requests the way each case configures.
class FailingMetadataFileAPI : public MoonrakerFileAPI {
  public:
    enum class Answer { ConnectionLost, NotFound, Unscanned };

    explicit FailingMetadataFileAPI(helix::IMoonrakerClient& client) : MoonrakerFileAPI(client) {}

    void get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                           ErrorCallback on_error, bool) override {
        ++metadata_calls;
        answer(metadata_answer, filename, on_success, on_error);
    }

    void metascan_file(const std::string& filename, FileMetadataCallback on_success,
                       ErrorCallback on_error, bool) override {
        ++metascan_calls;
        answer(metascan_answer, filename, on_success, on_error);
    }

    Answer metadata_answer = Answer::ConnectionLost;
    Answer metascan_answer = Answer::ConnectionLost;
    std::atomic<int> metadata_calls{0};
    std::atomic<int> metascan_calls{0};

  private:
    static void answer(Answer a, const std::string& filename,
                       const FileMetadataCallback& on_success, const ErrorCallback& on_error) {
        switch (a) {
        case Answer::ConnectionLost:
            on_error(MoonrakerError::connection_lost("server.files.metadata"));
            break;
        case Answer::NotFound:
            on_error(MoonrakerError::file_not_found("server.files.metadata", filename));
            break;
        case Answer::Unscanned:
            // No thumbnails and no estimate: Moonraker has not scanned the file yet.
            on_success(FileMetadata{});
            break;
        }
    }
};

/// Records partial downloads, the request gcode thumbnail extraction makes.
class CountingTransferAPI : public MoonrakerFileTransferAPI {
  public:
    explicit CountingTransferAPI(helix::IMoonrakerClient& client)
        : MoonrakerFileTransferAPI(client, "") {}

    void download_file_partial(const std::string&, const std::string&, size_t, StringCallback,
                               ErrorCallback on_error, CancelFlag = nullptr) override {
        ++partial_calls;
        on_error(MoonrakerError::connection_lost("download_file_partial"));
    }

    std::atomic<int> partial_calls{0};
};

class FailingMetadataAPI : public MoonrakerAPI {
  public:
    FailingMetadataAPI(helix::IMoonrakerClient& client, PrinterState& state)
        : MoonrakerAPI(client, state) {
        file_api_ = std::make_unique<FailingMetadataFileAPI>(client);
        file_transfer_api_ = std::make_unique<CountingTransferAPI>(client);
    }

    FailingMetadataFileAPI& failing_files() {
        return static_cast<FailingMetadataFileAPI&>(*file_api_);
    }
    CountingTransferAPI& counting_transfers() {
        return static_cast<CountingTransferAPI&>(*file_transfer_api_);
    }
};

class MetadataLossFixture : public PrintSelectPanelFixture {
  public:
    MetadataLossFixture()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Unregistered,
                                  PrintSelectVisit::Immediate) {
        auto failing = std::make_unique<FailingMetadataAPI>(mock_client_, get_printer_state());
        failing_ = failing.get();
        panel_->set_api(failing_);
        api_ = std::move(failing);
        drain();
    }

    /// Lists @p file, then asks for its metadata once more from a clean state.
    void fetch(const PlantedGcode& file) {
        panel_->refresh_files(true);
        drain();
        REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
        PrintSelectPanelTestAccess::forget_metadata(*panel_, file.name());
        failing_->failing_files().metadata_calls = 0;
        failing_->failing_files().metascan_calls = 0;
        failing_->counting_transfers().partial_calls = 0;
        PrintSelectPanelTestAccess::fetch_metadata(*panel_, file.name());
        drain();
        REQUIRE(failing_->failing_files().metadata_calls == 1);
    }

    FailingMetadataAPI* failing_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(MetadataLossFixture,
                 "Print select: metadata lost with the connection is fetched again, not extracted",
                 "[print_select][metadata][connection_loss]") {
    PlantedGcode file("metadata_connection_loss.gcode");
    fetch(file);

    CHECK(failing_->failing_files().metascan_calls == 0);
    CHECK(failing_->counting_transfers().partial_calls == 0);
    // Unfetched, so the refresh after reconnecting asks again.
    const PrintFileData* entry = PrintSelectPanelTestAccess::find_file(*panel_, file.name());
    REQUIRE(entry != nullptr);
    CHECK_FALSE(entry->metadata_fetched);
}

TEST_CASE_METHOD(MetadataLossFixture,
                 "Print select: a metascan lost with the connection is not followed by extraction",
                 "[print_select][metadata][connection_loss]") {
    using Answer = FailingMetadataFileAPI::Answer;
    PlantedGcode file("metascan_connection_loss.gcode");
    SECTION("after a metadata error") {
        failing_->failing_files().metadata_answer = Answer::NotFound;
    }
    SECTION("after metadata for an unscanned file") {
        failing_->failing_files().metadata_answer = Answer::Unscanned;
    }
    failing_->failing_files().metascan_answer = Answer::ConnectionLost;
    fetch(file);

    REQUIRE(failing_->failing_files().metascan_calls == 1);
    CHECK(failing_->counting_transfers().partial_calls == 0);
    const PrintFileData* entry = PrintSelectPanelTestAccess::find_file(*panel_, file.name());
    REQUIRE(entry != nullptr);
    CHECK_FALSE(entry->metadata_fetched);
}

TEST_CASE_METHOD(MetadataLossFixture,
                 "Print select: metadata missing for the file still falls back to extraction",
                 "[print_select][metadata][connection_loss]") {
    using Answer = FailingMetadataFileAPI::Answer;
    PlantedGcode file("metadata_not_indexed.gcode");
    failing_->failing_files().metadata_answer = Answer::NotFound;
    failing_->failing_files().metascan_answer = Answer::NotFound;
    fetch(file);

    CHECK(failing_->failing_files().metascan_calls == 1);
    CHECK(failing_->counting_transfers().partial_calls == 1);
}
