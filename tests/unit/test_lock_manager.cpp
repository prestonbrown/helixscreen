// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "lock_manager.h"

#include "../catch_amalgamated.hpp"

// [L065] Use friend class pattern instead of public test-only methods
class LockManagerTestAccess {
  public:
    static void reset(helix::LockManager& mgr) {
        mgr.pin_hash_.clear();
        mgr.locked_ = false;
        mgr.auto_lock_ = false;
    }
    static const std::string& pin_hash(const helix::LockManager& mgr) {
        return mgr.pin_hash_;
    }
};

TEST_CASE("LockManager: no PIN set by default", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);

    CHECK_FALSE(mgr.has_pin());
    CHECK_FALSE(mgr.is_locked());
}

TEST_CASE("LockManager: set and verify PIN", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);

    mgr.set_pin("1234");
    CHECK(mgr.has_pin());
    CHECK(mgr.verify_pin("1234"));
    CHECK_FALSE(mgr.verify_pin("0000"));
    CHECK_FALSE(mgr.verify_pin("12345"));
    CHECK_FALSE(mgr.verify_pin(""));
}

TEST_CASE("LockManager: the stored PIN hash is lowercase hex SHA-256", "[lock]") {
    // Hashes saved by existing installs must keep verifying.
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);

    mgr.set_pin("1234");
    CHECK(LockManagerTestAccess::pin_hash(mgr) ==
          "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4");
}

TEST_CASE("LockManager: lock and unlock", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);
    mgr.set_pin("5678");

    CHECK_FALSE(mgr.is_locked());

    mgr.lock();
    CHECK(mgr.is_locked());

    CHECK_FALSE(mgr.try_unlock("0000"));
    CHECK(mgr.is_locked());

    CHECK(mgr.try_unlock("5678"));
    CHECK_FALSE(mgr.is_locked());
}

TEST_CASE("LockManager: lock without PIN does nothing", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);

    mgr.lock();
    CHECK_FALSE(mgr.is_locked());
}

TEST_CASE("LockManager: remove PIN unlocks", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);
    mgr.set_pin("1234");
    mgr.lock();
    CHECK(mgr.is_locked());

    mgr.remove_pin();
    CHECK_FALSE(mgr.has_pin());
    CHECK_FALSE(mgr.is_locked());
}

TEST_CASE("LockManager: PIN validation rejects invalid lengths", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);

    CHECK_FALSE(mgr.set_pin("123"));     // too short
    CHECK_FALSE(mgr.set_pin("1234567")); // too long
    CHECK_FALSE(mgr.has_pin());

    CHECK_FALSE(mgr.set_pin("abcd")); // non-digit characters
    CHECK_FALSE(mgr.set_pin("12a4")); // mixed
    CHECK_FALSE(mgr.has_pin());

    CHECK(mgr.set_pin("1234")); // 4 digits OK
    mgr.remove_pin();
    CHECK(mgr.set_pin("12345")); // 5 digits OK
    mgr.remove_pin();
    CHECK(mgr.set_pin("123456")); // 6 digits OK
}

TEST_CASE("LockManager: auto-lock setting", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);

    CHECK_FALSE(mgr.auto_lock_enabled());

    mgr.set_auto_lock(true);
    CHECK(mgr.auto_lock_enabled());

    mgr.set_auto_lock(false);
    CHECK_FALSE(mgr.auto_lock_enabled());
}

TEST_CASE("LockManager: lock is idempotent", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);
    mgr.set_pin("1234");

    mgr.lock();
    mgr.lock(); // second call should be harmless
    CHECK(mgr.is_locked());

    CHECK(mgr.try_unlock("1234"));
    CHECK_FALSE(mgr.is_locked());
}

TEST_CASE("LockManager: set_pin while locked updates PIN", "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);
    mgr.set_pin("1234");
    mgr.lock();

    CHECK(mgr.set_pin("5678")); // change PIN while locked
    CHECK(mgr.is_locked());     // stays locked
    CHECK_FALSE(mgr.verify_pin("1234"));
    CHECK(mgr.verify_pin("5678"));
    CHECK(mgr.try_unlock("5678"));
}

TEST_CASE_METHOD(LVGLTestFixture, "LockManager: lock_pin_set follows setting and removing the PIN",
                 "[lock]") {
    auto& mgr = helix::LockManager::instance();
    LockManagerTestAccess::reset(mgr);
    mgr.init_subjects();
    lv_subject_t* pin_set = lv_xml_get_subject(nullptr, "lock_pin_set");
    REQUIRE(pin_set != nullptr);

    REQUIRE(mgr.set_pin("1234"));
    CHECK(lv_subject_get_int(pin_set) == 1);
    mgr.remove_pin();
    CHECK(lv_subject_get_int(pin_set) == 0);
}
