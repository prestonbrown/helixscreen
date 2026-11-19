// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_queued_job_options.cpp
 * @brief Pure encode/decode/prune rules for the per-job queue option store,
 * plus the automatic_transition parse off Moonraker's server.config.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][options]"
 *
 * No fixture: everything here is a free function over json. The wiring that
 * pushes these values through the Moonraker database and the queue refresh
 * is covered separately in test_queued_job_options_store.cpp.
 */

#include "job_queue_state.h"
#include "json_fwd.h"
#include "queued_job_options.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::queue::QueuedJobOptions;
using helix::queue::QueuedJobOptionsMap;

namespace {

QueuedJobOptions sample_entry() {
    return QueuedJobOptions{"benchy_v2.gcode", {{"skip_beam", true}, {"soak", false}}};
}

QueuedJobOptionsMap sample_map() {
    QueuedJobOptionsMap m;
    m["0001"] = sample_entry();
    m["0002"] = QueuedJobOptions{"cube.gcode", {{"skip_beam", false}}};
    return m;
}

} // namespace

TEST_CASE("queued_job_option_key prefixes the job id with the store key", "[job_queue][options]") {
    CHECK(helix::queue::queued_job_option_key("0001") == "queued_job_options.0001");
    CHECK(helix::queue::queued_job_option_key("").empty() == false);
}

TEST_CASE("queued job entry encode/decode round trip", "[job_queue][options]") {
    const auto original = sample_entry();

    const auto decoded =
        helix::queue::decode_queued_job_entry(helix::queue::encode_queued_job_entry(original));

    REQUIRE(decoded.filename == "benchy_v2.gcode");
    REQUIRE(decoded.options.size() == 2);
    CHECK(decoded.options.at("skip_beam") == true);
    CHECK(decoded.options.at("soak") == false);
}

TEST_CASE("queued job entry decode treats malformed shapes as defaults", "[job_queue][options]") {
    using helix::queue::decode_queued_job_entry;

    // Not an object at all
    CHECK(decode_queued_job_entry(json::array({"x"})).filename.empty());
    CHECK(decode_queued_job_entry(json("garbage")).options.empty());
    CHECK(decode_queued_job_entry(json(nullptr)).filename.empty());
    CHECK(decode_queued_job_entry(json::object()).filename.empty());

    // Object shape but wrong member types: the whole entry reads as default,
    // never half-parsed
    CHECK(decode_queued_job_entry(json{{"filename", 7}, {"options", json::object()}})
              .filename.empty());
    CHECK(decode_queued_job_entry(json{{"filename", "b.gcode"}, {"options", 7}}).options.empty());
    CHECK(decode_queued_job_entry(json{{"filename", "c.gcode"}, {"options", {{"y", "yes"}}}})
              .options.empty());

    // A missing options member is valid: filename kept, no choices
    const auto bare = decode_queued_job_entry(json{{"filename", "a.gcode"}});
    CHECK(bare.filename == "a.gcode");
    CHECK(bare.options.empty());
}

TEST_CASE("queued job options map decode keeps only well-formed rows", "[job_queue][options]") {
    // The parent key's value: one child per stored job.
    json mixed;
    mixed["good"] = {{"filename", "a.gcode"}, {"options", {{"x", true}}}};
    mixed["not_an_object"] = "oops";
    mixed["no_filename_key"] = {{"options", json::object()}};
    mixed["options_not_object"] = {{"filename", "b.gcode"}, {"options", 7}};
    mixed["option_not_bool"] = {{"filename", "c.gcode"}, {"options", {{"y", "yes"}}}};

    const auto decoded = helix::queue::decode_queued_job_options(mixed);
    REQUIRE(decoded.size() == 1);
    CHECK(decoded.count("good") == 1);
    CHECK(decoded.at("good").filename == "a.gcode");
    CHECK(decoded.at("good").options.at("x") == true);

    // Non-object parents decode to an empty map.
    CHECK(helix::queue::decode_queued_job_options(json::array({"0001"})).empty());
    CHECK(helix::queue::decode_queued_job_options(json::object()).empty());
}

TEST_CASE("stale_queued_job_option_ids names stored ids absent from the queue",
          "[job_queue][options]") {
    using helix::queue::stale_queued_job_option_ids;
    const auto stored = sample_map();

    SECTION("nothing stale") {
        CHECK(stale_queued_job_option_ids(stored, {"0002", "0001"}).empty());
    }

    SECTION("empty store is never stale") {
        CHECK(stale_queued_job_option_ids({}, {}).empty());
        CHECK(stale_queued_job_option_ids({}, {"0001"}).empty());
    }

    SECTION("one job left the queue") {
        const auto stale = stale_queued_job_option_ids(stored, {"0002"});
        REQUIRE(stale.size() == 1);
        CHECK(stale[0] == "0001");
    }

    SECTION("empty queue makes every stored id stale") {
        const auto stale = stale_queued_job_option_ids(stored, {});
        REQUIRE(stale.size() == 2);
        // Key order: the store is a std::map
        CHECK(stale[0] == "0001");
        CHECK(stale[1] == "0002");
    }
}

namespace {

helix::JobQueueEntry entry(const char* job_id, const char* filename) {
    return {job_id, filename, 0.0, 0.0};
}

} // namespace

TEST_CASE("find_new_job_id recovers the id added between two queue snapshots",
          "[job_queue][options]") {
    using helix::queue::find_new_job_id;

    const std::vector<std::string> before = {"0001", "0002"};
    const std::vector<helix::JobQueueEntry> after = {
        entry("0001", "a.gcode"), entry("0002", "b.gcode"), entry("0003", "benchy.gcode")};

    SECTION("one new id") {
        const auto found = find_new_job_id(before, after);
        REQUIRE(found.has_value());
        CHECK(*found == "0003");
    }

    SECTION("unchanged queue: nothing new") {
        CHECK_FALSE(
            find_new_job_id({"0001", "0002"}, {entry("0001", "a.gcode"), entry("0002", "b.gcode")})
                .has_value());
    }

    SECTION("empty before list: the whole queue is new, but one entry is one id") {
        const auto found = find_new_job_id({}, {entry("0007", "only.gcode")});
        REQUIRE(found.has_value());
        CHECK(*found == "0007");
    }

    SECTION("two new ids at once: ambiguous, refuse to guess") {
        // Another client queued concurrently; either id could be ours, so the
        // saved options must not attach to the wrong job.
        CHECK_FALSE(find_new_job_id(before, {entry("0001", "a.gcode"), entry("0002", "b.gcode"),
                                             entry("0003", "x.gcode"), entry("0004", "y.gcode")})
                        .has_value());
    }

    SECTION("a job removed while ours was added still yields one new id") {
        const auto found =
            find_new_job_id(before, {entry("0002", "b.gcode"), entry("0003", "x.gcode")});
        REQUIRE(found.has_value());
        CHECK(*found == "0003");
    }

    SECTION("duplicate filename already queued: the id is still the new one") {
        const auto found = find_new_job_id(
            before, {entry("0001", "a.gcode"), entry("0002", "b.gcode"), entry("0009", "b.gcode")});
        REQUIRE(found.has_value());
        CHECK(*found == "0009");
    }

    SECTION("empty everything") {
        CHECK_FALSE(find_new_job_id({}, {}).has_value());
    }
}

TEST_CASE("automatic_transition parse off server.config", "[job_queue][options]") {
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = 42;

    SECTION("true") {
        response["result"]["config"]["job_queue"]["automatic_transition"] = true;
        CHECK(helix::parse_automatic_transition(response) == true);
    }

    SECTION("false") {
        response["result"]["config"]["job_queue"]["automatic_transition"] = false;
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("missing job_queue section") {
        response["result"]["config"]["file_manager"] = json::object();
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("missing automatic_transition key") {
        response["result"]["config"]["job_queue"]["job_transition_delay"] = 0.5;
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("missing config section entirely") {
        response["result"]["debug"] = json::object();
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("non-boolean value reads as false, not a crash") {
        response["result"]["config"]["job_queue"]["automatic_transition"] = "yes";
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("unreadable response (no result) reads as false") {
        CHECK(helix::parse_automatic_transition(json::object()) == false);
    }
}
