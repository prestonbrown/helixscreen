// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_next_media.cpp
 * @brief JobQueueState publishes the first queued job's estimated time and
 * thumbnail, fetched once per change of first job.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "../test_helpers/thumbnail_test_png.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_print_state.h"
#include "printer_state.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"

#include <functional>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

const char* const kFileA = "2022-big-ben-by-miniworld3d_ABS_36m21s.gcode";
const char* const kFileB = "ECC_0.4_stand_PLA0.2_2h42m.gcode";
constexpr const char* kMetadataMethod = "server.files.metadata";

struct NextMediaFixture : public LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::GENERIC_COREXY};
    MoonrakerAPI mock_api{client, state()};
    JobQueueState jqs{&mock_api, &client};
    ScopedJobQueueSubjects subject_guard;

    NextMediaFixture() {
        jqs.init_subjects();
        // Connected, with the connection watcher's first answer already in: a
        // drop is what a test says, never what an earlier test left behind.
        lv_subject_set_int(state().network_state().get_printer_connection_state_subject(),
                           static_cast<int>(ConnectionState::CONNECTED));
        drain();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty(8);
    }

    void deliver(const std::vector<std::string>& files) {
        JobQueueStateTestAccess::deliver_status(jqs, status_of(files));
        drain();
    }

    /// What the mock serves for a file, read through the same call the state makes.
    int mock_estimate(const char* file) {
        int seconds = -1;
        mock_api.files().get_file_metadata(
            file, [&](const FileMetadata& md) { seconds = static_cast<int>(md.estimated_time); },
            [](const MoonrakerError&) {});
        return seconds;
    }

    /// The cache key the state will fetch for a file's thumbnail.
    std::string thumbnail_key(const char* file) {
        std::string key;
        mock_api.files().get_file_metadata(
            file,
            [&](const FileMetadata& md) {
                key = select_and_resolve_thumbnail(md.thumbnails, gcode_dir_of(file), 0, 0);
            },
            [](const MoonrakerError&) {});
        return key;
    }

    /// Join the processor pool and drain until `done`, or give up.
    void settle(const std::function<bool()>& done) {
        for (int i = 0; i < 40 && !done(); ++i) {
            ThumbnailProcessor::instance().wait_for_completion();
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    int estimate_subject() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "job_queue_next_estimate"));
    }
    std::string thumbnail_subject() {
        return lv_subject_get_string(lv_xml_get_subject(nullptr, "job_queue_next_thumbnail"));
    }
};

} // namespace

TEST_CASE_METHOD(NextMediaFixture, "first queued job's estimate comes from its file metadata",
                 "[job_queue][next_media]") {
    const int expected = mock_estimate(kFileA);
    REQUIRE(expected > 0);

    deliver({kFileA});

    CHECK(estimate_subject() == expected);
    CHECK(jqs.next_estimated_seconds() == expected);
}

TEST_CASE_METHOD(NextMediaFixture, "a metadata answer for a job no longer first is dropped",
                 "[job_queue][next_media]") {
    const int estimate_b = mock_estimate(kFileB);
    REQUIRE(estimate_b > 0);
    REQUIRE(mock_estimate(kFileA) != estimate_b);

    SECTION("through the real request path") {
        client.defer_next(kMetadataMethod);
        deliver({kFileA});
        deliver({kFileB});
        REQUIRE(estimate_subject() == estimate_b);

        client.fire_deferred(kMetadataMethod);
        drain();

        CHECK(estimate_subject() == estimate_b);
        CHECK(jqs.next_estimated_seconds() == estimate_b);
    }

    SECTION("through a direct delivery") {
        deliver({kFileB});
        REQUIRE(estimate_subject() == estimate_b);

        FileMetadata late;
        late.estimated_time = 999.0;
        JobQueueStateTestAccess::deliver_next_metadata(jqs, kFileA, late);

        CHECK(estimate_subject() == estimate_b);
    }
}

TEST_CASE_METHOD(NextMediaFixture, "an empty queue clears the next job's media",
                 "[job_queue][next_media]") {
    deliver({kFileA});
    REQUIRE(estimate_subject() > 0);

    deliver({});

    CHECK(estimate_subject() == 0);
    CHECK(thumbnail_subject() == PrinterPrintState::no_thumbnail_placeholder());
}

TEST_CASE_METHOD(NextMediaFixture, "the thumbnail subject is never empty",
                 "[job_queue][next_media]") {
    CHECK(thumbnail_subject() == PrinterPrintState::no_thumbnail_placeholder());

    FileMetadata no_thumbs;
    no_thumbs.estimated_time = 120.0;
    deliver({kFileA});
    JobQueueStateTestAccess::deliver_next_metadata(jqs, kFileA, no_thumbs);

    CHECK(estimate_subject() == 120);
    CHECK(thumbnail_subject() == PrinterPrintState::no_thumbnail_placeholder());
}

TEST_CASE_METHOD(NextMediaFixture, "one fetch per change of first job", "[job_queue][next_media]") {
    const int before = client.call_count(kMetadataMethod);

    deliver({kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 1);

    deliver({kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 1);

    deliver({kFileB, kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 2);
}

TEST_CASE_METHOD(NextMediaFixture, "a failed metadata read leaves the job unknown",
                 "[job_queue][next_media]") {
    client.fail_next(kMetadataMethod);

    deliver({kFileA});

    CHECK(estimate_subject() == 0);
    CHECK(thumbnail_subject() == PrinterPrintState::no_thumbnail_placeholder());
}

TEST_CASE_METHOD(NextMediaFixture, "a failed read is asked again on the next fetch",
                 "[job_queue][next_media]") {
    const int expected = mock_estimate(kFileA);
    const int before = client.call_count(kMetadataMethod);
    client.fail_next(kMetadataMethod);
    deliver({kFileA});
    REQUIRE(estimate_subject() == 0);

    deliver({kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 2);
    CHECK(estimate_subject() == expected);
}

TEST_CASE_METHOD(NextMediaFixture, "a reconnect asks again for the same first job",
                 "[job_queue][next_media]") {
    const int expected = mock_estimate(kFileA);
    const int before = client.call_count(kMetadataMethod);
    deliver({kFileA});
    REQUIRE(estimate_subject() == expected);

    jqs.invalidate(); // the socket dropped
    deliver({kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 2);
    // What it showed stays until the new answer, which is the same here.
    CHECK(estimate_subject() == expected);

    deliver({kFileA});
    CHECK(client.call_count(kMetadataMethod) == before + 2);
}

TEST_CASE_METHOD(NextMediaFixture, "an emptied queue refetches when the same job returns",
                 "[job_queue][next_media]") {
    const int expected = mock_estimate(kFileA);
    const int before = client.call_count(kMetadataMethod);

    deliver({kFileA});
    deliver({});
    deliver({kFileA});

    CHECK(client.call_count(kMetadataMethod) == before + 2);
    CHECK(estimate_subject() == expected);
}

TEST_CASE_METHOD(NextMediaFixture, "the next job's thumbnail replaces the placeholder",
                 "[job_queue][next_media][thumbnail]") {
    auto& cache = get_thumbnail_cache();
    const std::string key = thumbnail_key(kFileA);
    REQUIRE_FALSE(key.empty());
    cache.invalidate(key);
    plant_cached_png(cache, key);
    const std::string placeholder = PrinterPrintState::no_thumbnail_placeholder();

    deliver({kFileA});
    settle([&] { return thumbnail_subject() != placeholder; });

    const std::string shown = thumbnail_subject();
    CHECK(shown != placeholder);
    CHECK(shown.rfind("A:", 0) == 0);
    CHECK(shown.find(".bin") != std::string::npos);

    cache.invalidate(key);
}

TEST_CASE_METHOD(NextMediaFixture, "a thumbnail still loading for a superseded job is dropped",
                 "[job_queue][next_media][thumbnail]") {
    auto& cache = get_thumbnail_cache();
    const std::string key_a = thumbnail_key(kFileA);
    const std::string key_b = thumbnail_key(kFileB);
    REQUIRE_FALSE(key_a.empty());
    REQUIRE_FALSE(key_b.empty());
    cache.invalidate(key_a);
    cache.invalidate(key_b);
    plant_cached_png(cache, key_a);
    plant_cached_png(cache, key_b);
    const std::string placeholder = PrinterPrintState::no_thumbnail_placeholder();

    // A's pre-scale finishes and parks its result in the UpdateQueue; what
    // happens to the queue next decides whether that result may land.
    JobQueueStateTestAccess::deliver_status(jqs, status_of({kFileA}));
    helix::ui::UpdateQueue::instance().drain();
    ThumbnailProcessor::instance().wait_for_completion();
    REQUIRE(thumbnail_subject() == placeholder);

    ThumbnailRequest req_a;
    req_a.key = key_a;
    req_a.target = ThumbnailProcessor::get_target_for_display(ThumbnailSize::Card);
    const std::string path_a = cache.get_if_cached(req_a);
    REQUIRE_FALSE(path_a.empty());

    SECTION("the queue empties while it loads") {
        JobQueueStateTestAccess::deliver_status(jqs, status_of({}));
        settle([] { return false; });

        CHECK(thumbnail_subject() == placeholder);
    }

    SECTION("a different job becomes first while it loads") {
        JobQueueStateTestAccess::deliver_status(jqs, status_of({kFileB}));
        settle([&] { return thumbnail_subject() != placeholder; });

        CHECK(thumbnail_subject() != path_a);
    }

    cache.invalidate(key_a);
    cache.invalidate(key_b);
}
