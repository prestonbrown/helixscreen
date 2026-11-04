// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_queued_job_options_store.cpp
 * @brief Wiring of the queue option store and automatic_transition through
 * the mock Moonraker: the database round trip on refresh, and add_job
 * returning the queued_jobs list.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][options_store]"
 *
 * Pure rules live in test_queued_job_options.cpp; this file proves the store
 * is actually consulted on every queue refresh and written back only through
 * the Moonraker database, and that a connect-time fetch exposes Moonraker's
 * job_queue.automatic_transition on JobQueueState.
 *
 * JobQueueState subjects are deliberately never initialized (same reasoning
 * as test_job_queue_staleness.cpp): a local instance registering into
 * helix-xml's process-global scope would dangle after the test.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"
#include "job_queue_state.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "queued_job_options.h"

#include <cstdlib>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

class QueueOptionsStoreFixture : public LVGLTestFixture {
  public:
    QueueOptionsStoreFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        printer_state_.init_subjects(false);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(client_, printer_state_);
        state_ = std::make_unique<JobQueueState>(api_.get(), &client_);
    }

    ~QueueOptionsStoreFixture() override {
        state_.reset();
        api_.reset();
        client_.disconnect();
        UpdateQueue::instance().drain();
    }

    /// The mock dispatches handlers inline on the calling thread, so each
    /// queued callback can queue one more round (get -> prune -> post); drain
    /// repeatedly the way JobQueueStalenessFixture does.
    void pump(int rounds = 10) {
        UpdateQueue::instance().drain_until_empty(rounds);
    }

    /// Reads the stored option map straight from the (mock) database. The
    /// mock answers synchronously, so a plain capturing lambda is safe here.
    queue::QueuedJobOptionsMap read_store() {
        queue::QueuedJobOptionsMap out;
        bool answered = false;
        api_->database_get_item(
            queue::kOptionsDbNamespace, queue::kOptionsDbKey,
            [&out, &answered](const json& value) {
                out = queue::decode_queued_job_options(value);
                answered = true;
            },
            [&answered](const MoonrakerError&) { answered = true; });
        REQUIRE(answered);
        return out;
    }

    /// Seeds the store by posting the parent object whole — the shape a
    /// database dump or an earlier run leaves behind.
    void seed_store(const queue::QueuedJobOptionsMap& entries) {
        json value = json::object();
        for (const auto& [id, entry] : entries) {
            value[id] = queue::encode_queued_job_entry(entry);
        }
        api_->database_post_item(
            queue::kOptionsDbNamespace, queue::kOptionsDbKey, value, []() {},
            [](const MoonrakerError&) { FAIL("seed post failed"); });
    }

    MoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<JobQueueState> state_;
    /// Stands in for the panel's object_lifetime_ in load_queued_job_options calls
    AsyncLifetimeGuard guard_;
};

/// Sets an env var for the scope of a test, restoring (or clearing) it after.
struct ScopedEnv {
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name_);
        had_old_ = old != nullptr;
        if (had_old_) {
            old_ = old;
        }
        ::setenv(name_, value, 1);
    }
    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(name_, old_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    const char* name_;
    std::string old_;
    bool had_old_ = false;
};

} // namespace

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "queue refresh prunes stored options against the live queue",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    // Seed the database: 0001 is still in the mock's default queue, dead-job
    // is not. Assert the setup reached the store before refreshing.
    QueuedJobOptionsMap seed;
    seed["0001"] = QueuedJobOptions{"benchy_v2.gcode", {{"skip_beam", true}}};
    seed["dead-job"] = QueuedJobOptions{"gone.gcode", {{"soak", false}}};
    seed_store(seed);
    REQUIRE(read_store().size() == 2);

    state_->fetch();
    pump();

    const auto after = read_store();
    REQUIRE(after.size() == 1);
    CHECK(after.count("0001") == 1);
    CHECK(after.at("0001").filename == "benchy_v2.gcode");
    CHECK(after.at("0001").options.at("skip_beam") == true);
    CHECK(after.count("dead-job") == 0);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "the prune decides against a queue read taken after the store read",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    // 0004 has no entry in the mock's queue yet, so the triggering fetch's
    // snapshot cannot name it; dead-job is in neither read.
    QueuedJobOptionsMap seed;
    seed["0004"] = QueuedJobOptions{"wedge.gcode", {{"soak", true}}};
    seed["dead-job"] = QueuedJobOptions{"gone.gcode", {{"soak", false}}};
    seed_store(seed);

    // The fetch answers before the add lands: its snapshot is 0001-0003.
    state_->fetch();

    // The add lands between the triggering fetch and the prune's own queue
    // read; the mock allocates 0004 for the first job added.
    bool added = false;
    api_->queue().add_job(
        "wedge.gcode", [&added](const JobQueueStatus&) { added = true; },
        [](const MoonrakerError& err) { FAIL(err.message); });
    REQUIRE(added);

    pump();

    const auto after = read_store();
    CHECK(after.count("0004") == 1);
    CHECK(after.count("dead-job") == 0);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "a refresh with nothing to prune leaves the store untouched",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    QueuedJobOptionsMap seed;
    seed["0002"] = QueuedJobOptions{"calibration_cube.gcode", {{"soak", true}}};
    seed_store(seed);

    state_->fetch();
    pump();

    const auto after = read_store();
    REQUIRE(after.size() == 1);
    CHECK(after.count("0002") == 1);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture, "a refresh after the first keeps freshly saved options",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    // The first fetch of the connection takes the prune (latches it).
    state_->fetch();
    pump();

    // A job queued and saved after that fetch: the mock's queue is still
    // 0001-0003, so "0004" is new from the queue's point of view.
    save_queued_job_options(api_.get(), "0004", QueuedJobOptions{"wedge.gcode", {{"soak", true}}});
    pump();
    REQUIRE(read_store().count("0004") == 1);

    // The next refresh's queue snapshot cannot name 0004; the saved options
    // must survive it.
    state_->fetch();
    pump();

    CHECK(read_store().count("0004") == 1);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "the first refresh after a reconnect prunes stale options again",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    const QueuedJobOptionsMap seed = {
        {"dead-job", QueuedJobOptions{"gone.gcode", {{"soak", false}}}}};

    seed_store(seed);
    state_->fetch();
    pump();
    REQUIRE(read_store().empty());

    // A reconnect re-arms the prune: a stale entry written while the socket
    // was down must still be collected on the next connect's first fetch.
    seed_store(seed);
    state_->invalidate();
    state_->fetch();
    pump();

    CHECK(read_store().empty());
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "automatic_transition reads Moonraker's job_queue config at fetch",
                 "[job_queue][options_store]") {
    SECTION("mock default is false") {
        CHECK_FALSE(state_->automatic_transition());
        state_->fetch();
        pump();
        CHECK_FALSE(state_->automatic_transition());
    }

    SECTION("overridden to true via HELIX_MOCK_JOB_QUEUE_AUTOMATIC_TRANSITION") {
        ScopedEnv env("HELIX_MOCK_JOB_QUEUE_AUTOMATIC_TRANSITION", "1");
        state_->fetch();
        pump();
        CHECK(state_->automatic_transition());
    }
}

TEST_CASE_METHOD(QueueOptionsStoreFixture, "save_queued_job_options writes its own job's key",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    // Seed one unrelated job's entry; the save below must keep it.
    QueuedJobOptionsMap seed;
    seed["0001"] = QueuedJobOptions{"benchy_v2.gcode", {{"skip_beam", true}}};
    seed_store(seed);
    REQUIRE(read_store().size() == 1);

    save_queued_job_options(api_.get(), "0042", QueuedJobOptions{"wedge.gcode", {{"soak", false}}});
    pump();

    const auto after = read_store();
    REQUIRE(after.size() == 2);
    CHECK(after.count("0001") == 1);
    CHECK(after.at("0001").options.at("skip_beam") == true);
    REQUIRE(after.count("0042") == 1);
    CHECK(after.at("0042").filename == "wedge.gcode");
    CHECK(after.at("0042").options.at("soak") == false);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "save_queued_job_options stores a dotted child key readable on its own",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    save_queued_job_options(api_.get(), "0042", QueuedJobOptions{"wedge.gcode", {{"soak", true}}});
    pump();

    bool answered = false;
    QueuedJobOptions direct;
    api_->database_get_item(
        kOptionsDbNamespace, queued_job_option_key("0042"),
        [&answered, &direct](const json& value) {
            answered = true;
            direct = decode_queued_job_entry(value);
        },
        [](const MoonrakerError&) {});
    REQUIRE(answered);
    CHECK(direct.filename == "wedge.gcode");
    CHECK(direct.options.at("soak") == true);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "save_queued_job_options starts from empty on a missing key",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    // Nothing seeded: the child key simply does not exist yet, which is the
    // first-ever-save case, not an error.
    save_queued_job_options(api_.get(), "0042", QueuedJobOptions{"wedge.gcode", {{"soak", true}}});
    pump();

    const auto after = read_store();
    REQUIRE(after.size() == 1);
    REQUIRE(after.count("0042") == 1);
    CHECK(after.at("0042").filename == "wedge.gcode");
    CHECK(after.at("0042").options.at("soak") == true);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture, "add_job reports the resulting queue",
                 "[job_queue][options_store]") {
    bool answered = false;
    api_->queue().add_job(
        "wedge.gcode",
        [&answered](const JobQueueStatus& status) {
            answered = true;
            CHECK(status.queue_state == "ready");
            // The new job must be identifiable as the id absent before the
            // call: the mock seeds 0001-0003, so the fresh id is something
            // else and carries the queued filename.
            REQUIRE(status.queued_jobs.size() == 4);
            const JobQueueEntry* added = nullptr;
            for (const auto& job : status.queued_jobs) {
                if (job.filename == "wedge.gcode") {
                    added = &job;
                }
            }
            REQUIRE(added != nullptr);
            CHECK(added->job_id != "0001");
            CHECK(added->job_id != "0002");
            CHECK(added->job_id != "0003");
            CHECK_FALSE(added->job_id.empty());
        },
        [](const MoonrakerError& err) { FAIL(err.message); });
    CHECK(answered);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture, "load_queued_job_options answers a stored entry",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    QueuedJobOptionsMap seed;
    seed["0001"] = QueuedJobOptions{"benchy_v2.gcode", {{"skip_beam", true}, {"soak", false}}};
    seed["0099"] = QueuedJobOptions{"other.gcode", {{"unrelated", true}}};
    seed_store(seed);

    bool answered = false;
    QueuedJobOptions loaded;
    load_queued_job_options(guard_, api_.get(), "0001", [&](QueuedJobOptions entry) {
        answered = true;
        loaded = std::move(entry);
    });
    pump();

    REQUIRE(answered);
    CHECK(loaded.filename == "benchy_v2.gcode");
    REQUIRE(loaded.options.size() == 2);
    CHECK(loaded.options.at("skip_beam") == true);
    CHECK(loaded.options.at("soak") == false);
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "load_queued_job_options answers empty on a missing key or entry",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    SECTION("nothing stored at all") {
        // The mock answers the child-key read with the same JSON-RPC 404 the
        // real server does for a never-written key: defaults, not an error.
        bool answered = false;
        QueuedJobOptions loaded{"sentinel", {{"sentinel", true}}};
        load_queued_job_options(guard_, api_.get(), "0042", [&](QueuedJobOptions entry) {
            answered = true;
            loaded = std::move(entry);
        });
        pump();

        REQUIRE(answered);
        CHECK(loaded.filename.empty());
        CHECK(loaded.options.empty());
    }

    SECTION("store present, job's key absent") {
        QueuedJobOptionsMap seed;
        seed["0001"] = QueuedJobOptions{"benchy_v2.gcode", {{"skip_beam", true}}};
        seed_store(seed);

        bool answered = false;
        load_queued_job_options(guard_, api_.get(), "0042", [&](QueuedJobOptions entry) {
            answered = true;
            CHECK(entry.filename.empty());
            CHECK(entry.options.empty());
        });
        pump();
        CHECK(answered);
    }
}

TEST_CASE_METHOD(QueueOptionsStoreFixture,
                 "delete_queued_job_options removes only that job's entry",
                 "[job_queue][options_store]") {
    using namespace helix::queue;

    QueuedJobOptionsMap seed;
    seed["0001"] = QueuedJobOptions{"benchy_v2.gcode", {{"skip_beam", true}}};
    seed["0042"] = QueuedJobOptions{"wedge.gcode", {{"soak", false}}};
    seed_store(seed);
    REQUIRE(read_store().size() == 2);

    delete_queued_job_options(api_.get(), "0042");
    pump();

    auto after = read_store();
    REQUIRE(after.size() == 1);
    CHECK(after.count("0001") == 1);
    CHECK(after.count("0042") == 0);

    // Deleting an id that is not stored must not disturb what is: the 404 is
    // normalized to success before it reaches here.
    delete_queued_job_options(api_.get(), "9999");
    pump();

    after = read_store();
    REQUIRE(after.size() == 1);
    CHECK(after.count("0001") == 1);
}
