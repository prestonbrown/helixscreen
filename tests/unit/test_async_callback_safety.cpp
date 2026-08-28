// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: the queue_update / queue_widget_update / async_call cases call the
//                 shipped helix::ui:: entry points from include/ui_update_queue.h, pulled
//                 in transitively by ../lvgl_test_fixture.h and
//                 ../test_helpers/update_queue_test_access.h.
/**
 * @file test_async_callback_safety.cpp
 * @brief Unit tests for thread-safety and async callback patterns
 *
 * Tests the following patterns used for safe async callbacks:
 *
 * 1. File Path Construction:
 *    - Root directory paths: "gcodes/file.gcode"
 *    - Subdirectory paths: "gcodes/subdir/file.gcode"
 *    - Nested paths: "gcodes/a/b/c/file.gcode"
 *
 * 2. Destruction Flag Pattern (shared_ptr<atomic<bool>>):
 *    - Flag starts as true when object is alive
 *    - Setting to false propagates to all shared_ptr copies
 *    - Thread-safe access from multiple threads
 *    - Proper memory cleanup when all references released
 *
 * 3. Callback Safety Pattern:
 *    - Callbacks properly check alive flag before accessing object
 *    - Safe when object is destroyed before callback fires
 *    - Safe when callback fires after object destruction
 *
 * Run with sanitizers to detect memory/thread issues:
 *   ThreadSanitizer: CXXFLAGS="-fsanitize=thread" make test
 *   AddressSanitizer: CXXFLAGS="-fsanitize=address" make test
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Path Construction Helper (extracted for testability)
// ============================================================================

namespace helix {
namespace path {

/**
 * @brief Construct full Moonraker file path from current directory and filename
 *
 * Moonraker's file API requires paths relative to the root, prefixed with
 * the storage root (e.g., "gcodes/"). This function handles the path
 * construction for both root-level and subdirectory files.
 *
 * @param current_path Current directory path (empty string = root gcodes dir)
 * @param filename File name without path
 * @param root_prefix Storage root prefix (default: "gcodes")
 * @return Full path suitable for Moonraker API calls
 *
 * @example
 *   // Root directory
 *   construct_moonraker_path("", "file.gcode") -> "gcodes/file.gcode"
 *
 *   // Subdirectory
 *   construct_moonraker_path("subdir", "file.gcode") -> "gcodes/subdir/file.gcode"
 *
 *   // Nested subdirectory
 *   construct_moonraker_path("a/b/c", "file.gcode") -> "gcodes/a/b/c/file.gcode"
 */
inline std::string construct_moonraker_path(const std::string& current_path,
                                            const std::string& filename,
                                            const std::string& root_prefix = "gcodes") {
    if (current_path.empty()) {
        return root_prefix + "/" + filename;
    }
    return root_prefix + "/" + current_path + "/" + filename;
}

} // namespace path
} // namespace helix

// ============================================================================
// Destruction Flag Pattern Test Helpers
// ============================================================================

/**
 * @brief Example class demonstrating the destruction flag pattern
 *
 * This pattern is used throughout HelixScreen for safe async callbacks:
 * - Panel classes (BedMeshPanel, InputShaperPanel, etc.)
 * - MoonrakerManager
 * - AMS backends
 *
 * The key insight is that capturing a shared_ptr<atomic<bool>> by value
 * in a lambda keeps the atomic bool alive even after the object is destroyed,
 * allowing the callback to safely check if the object still exists.
 */
class ObjectWithDestructionFlag {
  public:
    ObjectWithDestructionFlag() : alive_(std::make_shared<std::atomic<bool>>(true)) {}

    ~ObjectWithDestructionFlag() {
        // Mark as dead BEFORE any member destruction
        alive_->store(false);
    }

    /**
     * @brief Get a copy of the alive flag for use in callbacks
     *
     * The callback should capture this by value (not reference) to ensure
     * the shared_ptr keeps the atomic bool alive.
     */
    std::shared_ptr<std::atomic<bool>> get_alive_flag() const {
        return alive_;
    }

    /**
     * @brief Check if this object is still alive
     */
    bool is_alive() const {
        return alive_->load();
    }

    /**
     * @brief Simulate an async operation that may outlive this object
     *
     * @param callback Function to call, receives alive flag and object pointer
     */
    void start_async_operation(
        std::function<void(std::shared_ptr<std::atomic<bool>>, ObjectWithDestructionFlag*)>
            callback) {
        // Capture alive_ by value - the shared_ptr copy keeps the bool alive
        auto alive = alive_;
        auto* self = this;

        // In real code, this would be ui_async_call or thread pool dispatch
        // For testing, we just store the callback
        pending_callback_ = [alive, self, callback]() { callback(alive, self); };
    }

    /**
     * @brief Execute any pending callback (for testing)
     */
    void execute_pending_callback() {
        if (pending_callback_) {
            pending_callback_();
            pending_callback_ = nullptr;
        }
    }

    /**
     * @brief Simulate work being done
     */
    void do_work() {
        work_counter_++;
    }

    int get_work_counter() const {
        return work_counter_;
    }

  private:
    std::shared_ptr<std::atomic<bool>> alive_;
    std::function<void()> pending_callback_;
    int work_counter_ = 0;
};

// ============================================================================
// Path Construction Tests
// ============================================================================

TEST_CASE("Path construction: root directory", "[path][async]") {
    SECTION("Empty current_path produces gcodes/filename") {
        std::string result = helix::path::construct_moonraker_path("", "file.gcode");
        REQUIRE(result == "gcodes/file.gcode");
    }

    SECTION("Simple filename without extension") {
        std::string result = helix::path::construct_moonraker_path("", "testfile");
        REQUIRE(result == "gcodes/testfile");
    }

    SECTION("Filename with spaces") {
        std::string result = helix::path::construct_moonraker_path("", "my file.gcode");
        REQUIRE(result == "gcodes/my file.gcode");
    }
}

TEST_CASE("Path construction: single subdirectory", "[path][async]") {
    SECTION("Simple subdirectory") {
        std::string result = helix::path::construct_moonraker_path("subdir", "file.gcode");
        REQUIRE(result == "gcodes/subdir/file.gcode");
    }

    SECTION("Subdirectory with spaces") {
        std::string result = helix::path::construct_moonraker_path("my folder", "file.gcode");
        REQUIRE(result == "gcodes/my folder/file.gcode");
    }

    SECTION("Subdirectory with special characters") {
        std::string result = helix::path::construct_moonraker_path("folder-1_test", "file.gcode");
        REQUIRE(result == "gcodes/folder-1_test/file.gcode");
    }
}

TEST_CASE("Path construction: nested subdirectories", "[path][async]") {
    SECTION("Two levels deep") {
        std::string result = helix::path::construct_moonraker_path("a/b", "file.gcode");
        REQUIRE(result == "gcodes/a/b/file.gcode");
    }

    SECTION("Three levels deep") {
        std::string result = helix::path::construct_moonraker_path("a/b/c", "file.gcode");
        REQUIRE(result == "gcodes/a/b/c/file.gcode");
    }

    SECTION("Deep nesting with complex names") {
        std::string result =
            helix::path::construct_moonraker_path("projects/2024/benchy-tests", "benchy_v2.gcode");
        REQUIRE(result == "gcodes/projects/2024/benchy-tests/benchy_v2.gcode");
    }
}

TEST_CASE("Path construction: custom root prefix", "[path][async]") {
    SECTION("USB root") {
        std::string result = helix::path::construct_moonraker_path("", "file.gcode", "usb");
        REQUIRE(result == "usb/file.gcode");
    }

    SECTION("USB with subdirectory") {
        std::string result = helix::path::construct_moonraker_path("folder", "file.gcode", "usb");
        REQUIRE(result == "usb/folder/file.gcode");
    }

    SECTION("Config root") {
        std::string result =
            helix::path::construct_moonraker_path("macros", "my_macro.cfg", "config");
        REQUIRE(result == "config/macros/my_macro.cfg");
    }
}

TEST_CASE("Path construction: edge cases", "[path][async][edge]") {
    SECTION("Empty filename") {
        std::string result = helix::path::construct_moonraker_path("", "");
        REQUIRE(result == "gcodes/");
    }

    SECTION("Empty path and empty filename") {
        std::string result = helix::path::construct_moonraker_path("", "");
        REQUIRE(result == "gcodes/");
    }

    SECTION("Path with trailing slash (should not add double slash)") {
        // Note: The current implementation doesn't handle trailing slashes specially
        // This documents the current behavior
        std::string result = helix::path::construct_moonraker_path("subdir/", "file.gcode");
        REQUIRE(result == "gcodes/subdir//file.gcode");
    }

    SECTION("Filename with leading slash (documents current behavior)") {
        // Note: This is technically malformed input
        std::string result = helix::path::construct_moonraker_path("", "/file.gcode");
        REQUIRE(result == "gcodes//file.gcode");
    }
}

// ============================================================================
// Destruction Flag Pattern Tests
// ============================================================================

TEST_CASE("Destruction flag: initial state", "[destruction_flag][async]") {
    ObjectWithDestructionFlag obj;

    SECTION("Flag starts as true") {
        REQUIRE(obj.is_alive() == true);
        REQUIRE(obj.get_alive_flag()->load() == true);
    }

    SECTION("Multiple calls to get_alive_flag return shared state") {
        auto flag1 = obj.get_alive_flag();
        auto flag2 = obj.get_alive_flag();

        // Both point to the same atomic bool
        REQUIRE(flag1.get() == flag2.get());
        REQUIRE(flag1->load() == flag2->load());
    }
}

TEST_CASE("Destruction flag: destructor sets flag to false", "[destruction_flag][async]") {
    auto flag = std::make_shared<std::atomic<bool>>(false);

    {
        ObjectWithDestructionFlag obj;
        flag = obj.get_alive_flag();
        REQUIRE(flag->load() == true);

        // Object goes out of scope here
    }

    // Flag should now be false
    REQUIRE(flag->load() == false);
}

TEST_CASE("Destruction flag: flag survives object destruction", "[destruction_flag][async]") {
    std::shared_ptr<std::atomic<bool>> flag;

    {
        ObjectWithDestructionFlag obj;
        flag = obj.get_alive_flag();

        // Verify we have a reference
        REQUIRE(flag.use_count() >= 2); // obj has one, we have one
    }

    // After destruction, our copy still exists
    REQUIRE(flag != nullptr);
    REQUIRE(flag.use_count() == 1); // Only our copy remains
    REQUIRE(flag->load() == false);
}

TEST_CASE("Destruction flag: thread-safe access", "[destruction_flag][async][thread][slow]") {
    // This test verifies that atomic operations on the flag are thread-safe.
    // We create an object, capture its flag, and verify we can read it
    // from multiple threads without issues.

    constexpr int NUM_THREADS = 4;
    constexpr int READS_PER_THREAD = 100;

    std::atomic<int> total_reads{0};
    std::atomic<int> true_reads{0};
    std::vector<std::thread> threads;

    // Create object and capture flag
    auto obj = std::make_unique<ObjectWithDestructionFlag>();
    auto flag = obj->get_alive_flag(); // Capture as shared_ptr

    // Start reader threads - capture flag BY VALUE for thread safety
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back([flag, &total_reads, &true_reads]() {
            for (int j = 0; j < READS_PER_THREAD; j++) {
                bool val = flag->load();
                total_reads.fetch_add(1);
                if (val) {
                    true_reads.fetch_add(1);
                }
            }
        });
    }

    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }

    // Verify results - all reads should have succeeded and seen true
    // (object not destroyed yet)
    REQUIRE(total_reads.load() == NUM_THREADS * READS_PER_THREAD);
    REQUIRE(true_reads.load() == NUM_THREADS * READS_PER_THREAD);
    REQUIRE(flag->load() == true);

    // Now destroy the object and verify flag is false
    obj.reset();
    REQUIRE(flag->load() == false);
}

TEST_CASE("Destruction flag: concurrent readers during destruction",
          "[destruction_flag][async][thread][slow]") {
    constexpr int NUM_READERS = 4;
    constexpr int READS_PER_THREAD = 100;

    std::shared_ptr<std::atomic<bool>> flag;
    std::vector<std::thread> readers;
    std::atomic<int> true_count{0};
    std::atomic<int> false_count{0};
    std::atomic<bool> start_reading{false};

    // Create object and capture flag
    auto obj = std::make_unique<ObjectWithDestructionFlag>();
    flag = obj->get_alive_flag();

    // Start reader threads that will read the flag repeatedly
    for (int i = 0; i < NUM_READERS; i++) {
        readers.emplace_back([&flag, &true_count, &false_count, &start_reading]() {
            // Wait for signal to start
            while (!start_reading.load()) {
                std::this_thread::yield();
            }

            for (int j = 0; j < READS_PER_THREAD; j++) {
                if (flag->load()) {
                    true_count++;
                } else {
                    false_count++;
                }
                std::this_thread::yield();
            }
        });
    }

    // Signal readers to start
    start_reading.store(true);

    // Give readers a chance to start
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Destroy the object while readers are running
    obj.reset();

    // Wait for all readers to finish
    for (auto& t : readers) {
        t.join();
    }

    // Verify results
    int total_reads = true_count.load() + false_count.load();
    REQUIRE(total_reads == NUM_READERS * READS_PER_THREAD);

    // Some reads should have been true (before destruction)
    // and some should be false (after destruction)
    // We can't predict exact counts, but both should be non-zero
    // (unless timing is very unlucky)
    INFO("True reads: " << true_count.load() << ", False reads: " << false_count.load());
    REQUIRE(flag->load() == false); // Final state must be false
}

// ============================================================================
// Callback Safety Pattern Tests
// ============================================================================

TEST_CASE("Callback safety: callback executes while object alive", "[callback][async]") {
    ObjectWithDestructionFlag obj;
    bool callback_executed = false;
    bool was_alive = false;

    obj.start_async_operation([&callback_executed, &was_alive](auto alive, auto* self) {
        callback_executed = true;
        was_alive = alive->load();
        if (was_alive) {
            self->do_work();
        }
    });

    // Execute while object is still alive
    obj.execute_pending_callback();

    REQUIRE(callback_executed == true);
    REQUIRE(was_alive == true);
    REQUIRE(obj.get_work_counter() == 1);
}

TEST_CASE("Callback safety: callback executes after object destroyed", "[callback][async]") {
    std::function<void()> stored_callback;
    bool callback_executed = false;
    bool was_alive = false;
    int work_done_count = 0;

    {
        ObjectWithDestructionFlag obj;

        obj.start_async_operation(
            [&callback_executed, &was_alive, &work_done_count](auto alive, auto* self) {
                callback_executed = true;
                was_alive = alive->load();
                if (was_alive) {
                    // This should NOT execute because alive is false
                    self->do_work();
                    work_done_count = self->get_work_counter();
                }
            });

        // Extract the callback before object is destroyed
        // In real code, this would be in a queue or timer
        auto alive = obj.get_alive_flag();
        auto* self_ptr = &obj;
        stored_callback = [alive, self_ptr, &callback_executed, &was_alive, &work_done_count]() {
            callback_executed = true;
            was_alive = alive->load();
            if (was_alive) {
                self_ptr->do_work();
                work_done_count = self_ptr->get_work_counter();
            }
        };
    }

    // Object is now destroyed

    // Execute the stored callback (simulates delayed async callback)
    stored_callback();

    REQUIRE(callback_executed == true);
    REQUIRE(was_alive == false);   // Callback saw the object as dead
    REQUIRE(work_done_count == 0); // No work was done (guard prevented it)
}

TEST_CASE("Callback safety: multiple pending callbacks", "[callback][async]") {
    std::vector<std::function<void()>> callbacks;
    std::atomic<int> alive_callbacks{0};
    std::atomic<int> dead_callbacks{0};

    {
        ObjectWithDestructionFlag obj;
        auto alive = obj.get_alive_flag();

        // Queue up multiple callbacks
        for (int i = 0; i < 5; i++) {
            callbacks.emplace_back([alive, &alive_callbacks, &dead_callbacks]() {
                if (alive->load()) {
                    alive_callbacks++;
                } else {
                    dead_callbacks++;
                }
            });
        }

        // Execute some while alive
        callbacks[0]();
        callbacks[1]();
    }

    // Object destroyed - execute remaining callbacks
    callbacks[2]();
    callbacks[3]();
    callbacks[4]();

    REQUIRE(alive_callbacks.load() == 2);
    REQUIRE(dead_callbacks.load() == 3);
}

// The property under test is memory visibility across a thread boundary: a
// reader running on a thread other than the one that ran the destructor must
// observe the destructor's store to the flag. Ordering is established by a
// condition variable, not by sleeping — a sleep-based version only asserts
// that the destroying thread happened to be rescheduled inside the sleep
// window, which is a statement about machine load rather than about the code.
TEST_CASE("Callback safety: callback in separate thread", "[callback][async][thread][slow]") {
    std::mutex mtx;
    std::condition_variable cv;
    bool object_destroyed = false;

    std::atomic<bool> callback_started{false};
    std::atomic<bool> callback_finished{false};
    std::atomic<bool> was_alive_in_callback{false};
    std::atomic<bool> wait_timed_out{false};

    std::thread callback_thread;

    {
        auto obj = std::make_unique<ObjectWithDestructionFlag>();
        auto alive = obj->get_alive_flag();

        // Start a thread that reads the alive flag only after destruction
        callback_thread = std::thread([&, alive]() {
            callback_started.store(true);

            // Block until the owner is definitively gone. The bound is
            // generous so a genuine regression surfaces as a failed assertion
            // instead of hanging the suite.
            {
                std::unique_lock<std::mutex> lock(mtx);
                if (!cv.wait_for(lock, std::chrono::seconds(5),
                                 [&object_destroyed]() { return object_destroyed; })) {
                    wait_timed_out.store(true);
                }
            }

            was_alive_in_callback.store(alive->load());
            callback_finished.store(true);
        });

        // Wait for thread to start
        while (!callback_started.load()) {
            std::this_thread::yield();
        }

        // Destroy object while the reader thread is blocked on the CV
        obj.reset();
    }

    // Release the reader now that destruction has definitely completed
    {
        std::lock_guard<std::mutex> lock(mtx);
        object_destroyed = true;
    }
    cv.notify_all();

    // Wait for callback thread to finish
    callback_thread.join();

    REQUIRE(wait_timed_out.load() == false);
    REQUIRE(callback_finished.load() == true);
    REQUIRE(was_alive_in_callback.load() == false);
}

// ============================================================================
// Real-World Pattern Simulation Tests
// ============================================================================

/**
 * @brief Simulates the async callback pattern used in PrintSelectPanel::delete_file
 *
 * This test simulates the exact pattern:
 * 1. Panel captures alive flag
 * 2. Calls API method with callbacks
 * 3. Callbacks use ui_async_call to defer to main thread
 * 4. Deferred callback checks alive flag before accessing panel
 */
TEST_CASE("Real-world: delete file callback pattern", "[callback][async][real_world]") {
    struct SimulatedPanel {
        std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
        std::string last_deleted_file;
        bool delete_confirmed = false;

        ~SimulatedPanel() {
            alive_->store(false);
        }

        void hide_delete_confirmation() {
            delete_confirmed = false;
        }

        void hide_detail_view() {
            last_deleted_file.clear();
        }

        void refresh_files() {
            // Would refresh file list
        }
    };

    // Simulated async call queue (like LVGL's async call)
    std::vector<std::function<void()>> deferred_calls;

    auto simulate_ui_async_call = [&deferred_calls](std::function<void()> fn) {
        deferred_calls.push_back(std::move(fn));
    };

    // Test case 1: Panel still alive when callbacks fire
    {
        deferred_calls.clear();
        auto panel = std::make_unique<SimulatedPanel>();
        auto alive = panel->alive_;
        auto* self = panel.get();

        // Simulate API success callback (runs on background thread)
        // This captures alive and self, then defers to main thread
        auto on_success = [alive, self, &simulate_ui_async_call]() {
            simulate_ui_async_call([alive, self]() {
                if (!alive->load()) {
                    return; // Panel was destroyed
                }
                self->hide_delete_confirmation();
                self->hide_detail_view();
                self->refresh_files();
            });
        };

        on_success();

        // Execute deferred call while panel is still alive
        REQUIRE(deferred_calls.size() == 1);
        deferred_calls[0]();

        // Panel should have processed the callback
        REQUIRE(panel->delete_confirmed == false);
    }

    // Test case 2: Panel destroyed before callbacks fire
    {
        deferred_calls.clear();
        std::shared_ptr<std::atomic<bool>> captured_alive;

        {
            auto panel = std::make_unique<SimulatedPanel>();
            captured_alive = panel->alive_;
            auto alive = panel->alive_;
            auto* self = panel.get();

            // Simulate API success callback
            auto on_success = [alive, self, &simulate_ui_async_call]() {
                simulate_ui_async_call([alive, self]() {
                    if (!alive->load()) {
                        return; // Panel was destroyed - this path should be taken
                    }
                    // This should NOT execute
                    self->hide_delete_confirmation();
                });
            };

            on_success();

            // Panel is destroyed here
        }

        // Execute deferred call AFTER panel is destroyed
        REQUIRE(deferred_calls.size() == 1);
        REQUIRE(captured_alive->load() == false);

        // This should not crash (would crash without alive check)
        REQUIRE_NOTHROW(deferred_calls[0]());
    }
}

// ============================================================================
// Memory Cleanup Tests
// ============================================================================

TEST_CASE("Memory: shared_ptr cleanup when all references released", "[memory][async]") {
    std::weak_ptr<std::atomic<bool>> weak_flag;

    {
        ObjectWithDestructionFlag obj;
        weak_flag = obj.get_alive_flag();

        REQUIRE(weak_flag.use_count() == 1);
        REQUIRE(weak_flag.lock() != nullptr);
    }

    // After object destruction, the flag should be released
    // (assuming no other references were kept)
    REQUIRE(weak_flag.expired() == true);
    REQUIRE(weak_flag.lock() == nullptr);
}

TEST_CASE("Memory: callback keeps flag alive", "[memory][async]") {
    std::weak_ptr<std::atomic<bool>> weak_flag;
    std::function<void()> stored_callback;

    {
        ObjectWithDestructionFlag obj;
        weak_flag = obj.get_alive_flag();
        auto alive = obj.get_alive_flag();

        stored_callback = [alive]() {
            // Callback captures shared_ptr by value
            return alive->load();
        };
    }

    // Object destroyed, but callback still holds reference
    REQUIRE(weak_flag.expired() == false);
    REQUIRE(weak_flag.use_count() == 1); // Only callback's copy

    // Clear callback, releasing last reference
    stored_callback = nullptr;
    REQUIRE(weak_flag.expired() == true);
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_CASE("Stress: rapid create/destroy with pending callbacks", "[stress][async][thread][.slow]") {
    constexpr int ITERATIONS = 100;

    for (int i = 0; i < ITERATIONS; i++) {
        std::function<void()> pending_callback;
        std::shared_ptr<std::atomic<bool>> flag;

        {
            ObjectWithDestructionFlag obj;
            flag = obj.get_alive_flag();
            auto alive = obj.get_alive_flag();

            pending_callback = [alive]() { return alive->load(); };
        }

        // Execute after destruction
        REQUIRE(flag->load() == false);
        REQUIRE_NOTHROW(pending_callback());
    }
}

TEST_CASE("Stress: concurrent object creation and destruction", "[stress][async][thread][.slow]") {
    constexpr int NUM_THREADS = 8;
    constexpr int OBJECTS_PER_THREAD = 50;

    std::atomic<int> completed{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&completed]() {
            for (int i = 0; i < OBJECTS_PER_THREAD; i++) {
                std::function<void()> callback;

                {
                    ObjectWithDestructionFlag obj;
                    auto alive = obj.get_alive_flag();
                    callback = [alive]() {
                        // Just read the flag
                        volatile bool val = alive->load();
                        (void)val;
                    };
                }

                // Execute callback after object is destroyed
                callback();
            }
            completed++;
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(completed.load() == NUM_THREADS);
}

// ============================================================================
// Widget-Safe API Tests (require LVGL)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe queue_update: valid widget executes callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;
    auto data = std::make_unique<int>(42);

    helix::ui::queue_update<int>(widget, std::move(data), [&callback_fired](lv_obj_t* w, int* d) {
        REQUIRE(lv_obj_is_valid(w));
        REQUIRE(*d == 42);
        callback_fired = true;
    });

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe queue_update: deleted widget skips callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;
    auto data = std::make_unique<int>(99);

    helix::ui::queue_update<int>(widget, std::move(data),
                                 [&callback_fired](lv_obj_t*, int*) { callback_fired = true; });

    // Destroy widget before draining queue
    lv_obj_delete(widget);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe queue_update: data freed even when skipped",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    struct TrackDestruction {
        bool* destroyed;
        ~TrackDestruction() {
            *destroyed = true;
        }
    };

    bool data_destroyed = false;
    auto data = std::make_unique<TrackDestruction>();
    data->destroyed = &data_destroyed;

    helix::ui::queue_update<TrackDestruction>(widget, std::move(data),
                                              [](lv_obj_t*, TrackDestruction*) {});

    // Destroy widget — callback should be skipped but data still freed
    lv_obj_delete(widget);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(data_destroyed);
}

TEST_CASE_METHOD(LVGLTestFixture, "queue_widget_update: valid widget executes callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;

    helix::ui::queue_widget_update(widget, [&callback_fired](lv_obj_t* w) {
        REQUIRE(lv_obj_is_valid(w));
        callback_fired = true;
    });

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "queue_widget_update: deleted widget skips callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;

    helix::ui::queue_widget_update(widget, [&callback_fired](lv_obj_t*) { callback_fired = true; });

    lv_obj_delete(widget);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe async_call: valid widget executes callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;

    helix::ui::async_call(
        widget,
        [](void* ud) {
            auto* fired = static_cast<bool*>(ud);
            *fired = true;
        },
        &callback_fired);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(callback_fired);
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-safe async_call: deleted widget skips callback",
                 "[widget_safe][async]") {
    lv_obj_t* widget = lv_obj_create(test_screen());
    REQUIRE(widget != nullptr);

    bool callback_fired = false;

    helix::ui::async_call(
        widget,
        [](void* ud) {
            auto* fired = static_cast<bool*>(ud);
            *fired = true;
        },
        &callback_fired);

    lv_obj_delete(widget);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(callback_fired);
}
