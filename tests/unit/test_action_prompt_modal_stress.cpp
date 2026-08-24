// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// ActionPromptModal show/hide stress harness — designed to surface the L081
// crash family (#875 SIGBUS, #877 SEGV at lv_event_mark_deleted+0x3c, both
// from AD5X bundle EB93VR9T) on faster hardware where the race window is
// short.
//
// Reproducing the AD5X bug on x86_64 desktop is hard because:
//   - the desktop drains LVGL's async-delete list inside one lv_timer_handler
//     iteration, so deletes don't pile up the way they do on MIPS;
//   - the breadcrumbs show modals stacked on top of print_completion_modal,
//     so the parent-child interaction matters;
//   - the original crash backtrace passes through lv_obj_destructor →
//     button-event teardown, so click-driven hides exercise different paths
//     than callback-driven ones.
//
// Each variant below targets one of those gaps:
//
//   "raw show/hide"       — sanity baseline (kept from earlier iteration)
//   "burst pattern"       — mirrors the EB93VR9T breadcrumb cadence
//   "alternating shapes"  — stress button_callback_data_ vector reallocs
//   "stacked under base"  — opens action_prompt at depth 2 with a parent
//                           modal at depth 1 (matches the AD5X crumb shape)
//   "click-driven hide"   — fires LV_EVENT_CLICKED on a real button so the
//                           gcode-callback → handle_button_click → hide()
//                           teardown path runs (not just a direct hide())
//   "single-tick drain"   — N show/hide pairs queued before a SINGLE
//                           lv_timer_handler_safe() fires; mimics the AD5X
//                           where a slow tick lets many async-deletes
//                           accumulate before drain. This is the closest
//                           x86 approximation of MIPS tick spacing.
//   "queue-update racer"  — interleaves queue_update() lambdas (the path
//                           websocket-driven prompts take in production)
//                           with show/hide cycles on the main thread, so
//                           the next tick's process_pending() runs while
//                           the previous tick's async-deletes are still in
//                           flight inside obj_delete_core
//
// Run under AddressSanitizer for actionable UAF reports:
//   make test SANITIZE=address
//   LD_PRELOAD=/lib/x86_64-linux-gnu/libasan.so.8 \
//   ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:fast_unwind_on_malloc=0 \
//       ./build/bin/helix-tests "[stress][action_prompt]"
//
// Override iteration counts for soak runs:
//   ACTION_PROMPT_STRESS_ITERATIONS=500 ./build/bin/helix-tests \
//       "[stress][action_prompt]"
//
// Tagged [.ui_integration] (hidden by default) — needs the XML component
// tree on disk. Mirrors test_wizard_step_stress.cpp's tagging pattern.

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "display_settings_manager.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Heap accounting for the leak check below. <cstdlib> is what defines
// __GLIBC__, so this block has to follow it.
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include "../catch_amalgamated.hpp"

namespace {

int env_iterations(int default_count) {
    if (const char* v = std::getenv("ACTION_PROMPT_STRESS_ITERATIONS")) {
        try {
            int n = std::stoi(v);
            if (n > 0)
                return n;
        } catch (...) {
        }
    }
    return default_count;
}

helix::PromptData make_prompt(const std::string& title, int n_buttons) {
    helix::PromptData data;
    data.title = title;
    data.text_lines.push_back("Stress prompt " + title);
    for (int i = 0; i < n_buttons; ++i) {
        helix::PromptButton btn;
        btn.label = "Btn" + std::to_string(i);
        btn.gcode = "ECHO_" + std::to_string(i);
        btn.color = (i % 2 == 0) ? "primary" : "secondary";
        data.buttons.push_back(std::move(btn));
    }
    return data;
}

// ---------------------------------------------------------------------------
// Observable end-state helpers
//
// The defect class this file exists for — safe_delete_deferred_raw corrupting
// LVGL's async-delete list — is invisible in a plain build unless something
// actually inspects the wreckage once the burst is over. Every stress case
// below now ends on the same three observables.
//
// 1. WIDGET CENSUS back to its pre-loop baseline.
//
//    This is the sharp instrument, and getting it right means knowing where a
//    stranded dialog actually ends up. safe_delete_deferred_raw()
//    (include/ui_utils.h) does NOT delete in place: it hides the object,
//    defocuses it, REPARENTS it to lv_layer_top(), and only then calls
//    lv_obj_delete_async(). So a dropped or corrupted async delete leaves the
//    whole dialog tree alive on the TOP LAYER, not on the screen — an
//    assertion that only counted lv_screen_active()'s children would be green
//    through exactly the bug this file hunts. The census therefore walks the
//    screen, lv_layer_top() and lv_layer_sys() recursively. It is exact, has no
//    noise floor at all, and covers both failure shapes: the early-return paths
//    in safe_delete_deferred_raw (object stays on the screen) and the dropped
//    async delete (object stays on the top layer).
//
// 2. The modal reports itself hidden with a null dialog. Modal::hide() nulls
//    both synchronously, so this goes red if hide() took an early-return branch
//    (already-exiting, backdrop untracked) it should not have.
//
// 3. HEAP GROWTH under a ceiling — the backstop for a leak that is not an
//    lv_obj at all (PromptData copies, ButtonCallbackData, style arrays held by
//    something the census cannot see).
//
//    It deliberately does NOT use lv_mem_monitor(): lv_conf.h sets
//    LV_USE_STDLIB_MALLOC = LV_STDLIB_CLIB, and lv_mem_monitor_core() for CLIB
//    is a literal no-op ("/*Not supported*/", lib/lvgl/src/stdlib/clib/
//    lv_mem_core_clib.c) that leaves lv_mem_monitor_t zeroed — free_size would
//    read 0 on both samples and the check would pass against any leak
//    whatsoever. The number comes from the C allocator instead. Treat it as a
//    coarse backstop: see HEAP_GROWTH_LIMIT_BYTES for the measured noise.
size_t heap_bytes() {
#if defined(__APPLE__)
    return static_cast<size_t>(mstats().bytes_used);
#elif defined(__GLIBC__)
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)
    return static_cast<size_t>(mallinfo2().uordblks);
#else
    // Pre-2.33 mallinfo() reports an int that wraps past 2 GiB. These bursts
    // allocate far less than that, and the value is only ever differenced.
    return static_cast<size_t>(static_cast<unsigned int>(mallinfo().uordblks));
#endif
#else
    return 0; // unsupported libc — the heap check self-disables
#endif
}

// Ceiling on heap growth between the mid-loop sample and the post-burst sample.
//
// This number is empirical, and it is deliberately loose, because the measured
// noise floor of the underlying counter is large. On macOS heap_bytes() reads
// mstats().bytes_used, which aggregates every malloc zone and is subject to the
// magazine allocator's caching; running one case in isolation at 1x / 2x / 4x
// its default iteration count produced deltas of -1.1 MB, +0.39 MB, +0.00 MB
// (burst) and +1.08 MB, +0.72 MB, +2.55 MB (alternating shapes) — non-monotonic
// in both directions and unrelated to cycle count. Across full-suite runs the
// largest clean-tree delta observed was +3.0 MB (pstat racer), and the smallest
// was -0.39 MB (stacked). A tight byte budget here would be a flake generator,
// not a test.
//
// 8 MB sits at ~2.5x the worst clean-tree reading — enough headroom that a
// green run stays green — while still catching a gross runaway: a full dialog
// tree is a dozen-plus lv_obj_t with their style arrays, so a per-cycle leak
// over the 100-600 post-baseline cycles these loops run lands in the tens of MB.
//
// The exact detector is the widget census below; this is only the backstop for
// a leak that is not an lv_obj. If you are hunting a byte-level regression, run
// the file under ASan (see the header comment) rather than tightening this.
constexpr long HEAP_GROWTH_LIMIT_BYTES = 8L * 1024L * 1024L;

void require_no_heap_growth(size_t before, size_t after, const char* what) {
    if (before == 0 || after == 0) {
        WARN("heap accounting unavailable on this libc — leak check skipped for " << what);
        return;
    }
    const long delta = static_cast<long>(after) - static_cast<long>(before);
    spdlog::info("[action_prompt-stress/{}] heap delta {} bytes (limit {})", what, delta,
                 HEAP_GROWTH_LIMIT_BYTES);
    INFO("heap grew " << delta << " bytes across the " << what << " burst (limit "
                      << HEAP_GROWTH_LIMIT_BYTES << ")");
    REQUIRE(delta < HEAP_GROWTH_LIMIT_BYTES);
}

uint32_t count_descendants(lv_obj_t* obj) {
    if (!obj)
        return 0;
    const uint32_t n = lv_obj_get_child_count(obj);
    uint32_t total = n;
    for (uint32_t i = 0; i < n; ++i) {
        total += count_descendants(lv_obj_get_child(obj, i));
    }
    return total;
}

// Snapshot of every place a stranded ActionPromptModal widget can survive.
struct TreeBaseline {
    uint32_t screen_children = 0; // direct children of the active screen
    uint32_t census = 0;          // recursive: screen + top layer + sys layer
};

TreeBaseline tree_baseline(lv_obj_t* screen) {
    TreeBaseline b;
    b.screen_children = lv_obj_get_child_count(screen);
    b.census = count_descendants(screen) + count_descendants(lv_layer_top()) +
               count_descendants(lv_layer_sys());
    return b;
}

// Everything the modal built is gone. Call only after a drain (process_lvgl).
void require_tree_settled(const helix::ui::ActionPromptModal& modal, lv_obj_t* screen,
                          const TreeBaseline& base) {
    CHECK_FALSE(modal.is_visible());
    CHECK(modal.dialog() == nullptr);

    const TreeBaseline now = tree_baseline(screen);
    INFO("screen children " << now.screen_children << " (baseline " << base.screen_children
                            << "), widget census " << now.census << " (baseline " << base.census
                            << ")");
    CHECK(now.screen_children == base.screen_children);
    REQUIRE(now.census == base.census);
}

class ActionPromptStressFixture : public LVGLUITestFixture {
  public:
    ActionPromptStressFixture() {
        // Mirror the AD5X preset where #875/#877 reproduced. With
        // animations off, Modal::hide() takes the synchronous remove() +
        // safe_delete_deferred_raw() branch — that's the path whose
        // breadcrumbs show stack depth toggling 1<->2 in sub-millisecond
        // intervals. With animations on, the exit fade serializes deletes
        // through the animation timer and hides the race entirely.
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);

        modal_.set_gcode_callback([](const std::string&) { /* no-op */ });

        // Settle the tree BEFORE any test takes its baseline. Every check here
        // is a delta, so the starting sample has to come from a quiet tree.
        // safe_delete_deferred_raw() reparents to lv_layer_top() and only then
        // calls lv_obj_delete_async(), so an earlier test in the same process
        // that returned with deletes still queued leaves that debris sitting on
        // the top layer — inside the census. Our own process_lvgl() then drains
        // it mid-run, and the post-run census lands BELOW the baseline, failing
        // as if this modal had over-deleted. Catch2 shuffles test order, so
        // which run trips it depends on the seed.
        //
        // Loop because a drained callback can queue more work.
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(20);
        }
    }

    ~ActionPromptStressFixture() override {
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    helix::ui::ActionPromptModal modal_;
    bool prev_animations_ = true;
};

} // namespace

// ============================================================================
// Baseline — kept so regressions in show_prompt() itself surface here first
// ============================================================================

TEST_CASE_METHOD(ActionPromptStressFixture, "ActionPromptModal stress: rapid show/hide",
                 "[action_prompt][stress][.ui_integration]") {
    const int iterations = env_iterations(200);
    spdlog::info("[action_prompt-stress] rapid show/hide × {}", iterations);

    const TreeBaseline baseline = tree_baseline(test_screen());
    // The heap baseline is taken at the first drain AT OR PAST the loop's
    // halfway point, and every other case below follows the same rule. Two
    // reasons, both measured rather than assumed:
    //
    //   - it must land on a drain, never mid-burst, or it counts dialog trees
    //     that are legitimately still sitting on the async-delete list;
    //   - it must land late. Sampling at cycle 25 charges the check for LVGL's
    //     one-time caches (style, glyph, XML component) still filling: measured
    //     growth from cycle 25 is 1.4 MB by cycle 75 but only 2.3 MB by cycle
    //     775, i.e. strongly saturating, which is warm-up and not a leak. From
    //     the midpoint on, both samples are steady state and a per-cycle leak
    //     is the only thing that can separate them.
    //
    // Stays 0 — check self-disables — if the iteration count is overridden
    // below the drain interval.
    size_t heap_warm = 0;

    auto data = make_prompt("Rapid", 3);
    for (int i = 0; i < iterations; ++i) {
        REQUIRE(modal_.show_prompt(test_screen(), data));
        modal_.hide();
        if ((i + 1) % 25 == 0) {
            process_lvgl(40);
            if (heap_warm == 0 && (i + 1) * 2 >= iterations)
                heap_warm = heap_bytes();
            spdlog::info("[action_prompt-stress] iter {}/{}", i + 1, iterations);
        }
    }
    process_lvgl(120);
    require_tree_settled(modal_, test_screen(), baseline);
    require_no_heap_growth(heap_warm, heap_bytes(), "rapid");
}

TEST_CASE_METHOD(ActionPromptStressFixture, "ActionPromptModal stress: burst pattern from #877",
                 "[action_prompt][stress][.ui_integration]") {
    const int bursts = env_iterations(40);
    const int pairs_per_burst = 6;
    spdlog::info("[action_prompt-stress] {} bursts × {} pairs", bursts, pairs_per_burst);

    const TreeBaseline baseline = tree_baseline(test_screen());
    size_t heap_warm = 0;

    auto data = make_prompt("Burst", 2);
    for (int b = 0; b < bursts; ++b) {
        for (int p = 0; p < pairs_per_burst; ++p) {
            REQUIRE(modal_.show_prompt(test_screen(), data));
            modal_.hide();
        }
        process_lvgl(50);
        if (heap_warm == 0 && (b + 1) * 2 >= bursts)
            heap_warm = heap_bytes();
        if ((b + 1) % 5 == 0) {
            spdlog::info("[action_prompt-stress] burst {}/{}", b + 1, bursts);
        }
    }
    process_lvgl(120);
    require_tree_settled(modal_, test_screen(), baseline);
    require_no_heap_growth(heap_warm, heap_bytes(), "burst");
}

TEST_CASE_METHOD(ActionPromptStressFixture, "ActionPromptModal stress: alternating prompt shapes",
                 "[action_prompt][stress][.ui_integration]") {
    const int iterations = env_iterations(120);
    spdlog::info("[action_prompt-stress] alternating shapes × {}", iterations);

    const TreeBaseline baseline = tree_baseline(test_screen());
    size_t heap_warm = 0;

    for (int i = 0; i < iterations; ++i) {
        auto data = make_prompt("Shape" + std::to_string(i), 1 + (i % 5));
        REQUIRE(modal_.show_prompt(test_screen(), data));
        modal_.hide();
        if ((i + 1) % 30 == 0) {
            process_lvgl(40);
            if (heap_warm == 0 && (i + 1) * 2 >= iterations)
                heap_warm = heap_bytes();
        }
    }
    process_lvgl(120);
    require_tree_settled(modal_, test_screen(), baseline);
    require_no_heap_growth(heap_warm, heap_bytes(), "alternating-shapes");
}

// ============================================================================
// Harsher variants — designed to actually trip ASan
// ============================================================================

// Open a parent modal first so action_prompt sits at depth 2, matching the
// EB93VR9T breadcrumb shape exactly:
//   modal+ print_completion_modal 1
//   modal+ action_prompt_modal 2
//   modal- action_prompt_modal 1
//   modal+ action_prompt_modal 2
//   ...
// The deletes from the action_prompt cycles share LVGL's async list with the
// parent's lifetime, so children/siblings of both can be in-flight at once.
TEST_CASE_METHOD(ActionPromptStressFixture,
                 "ActionPromptModal stress: stacked under print_completion-style base",
                 "[action_prompt][stress][.ui_integration]") {
    // Use any registered XML component as the base modal — print_completion_modal
    // is the production case but its content needs print state. Substitute a
    // simpler always-available component that goes through the same Modal
    // machinery: ums_action_modal or any registered backdrop is fine. Fall
    // back to any registered component if the chosen one isn't available.
    // Baseline is taken BEFORE the base modal exists, so the final check also
    // proves the depth-1 modal tore down and not just the depth-2 ones.
    const TreeBaseline baseline = tree_baseline(test_screen());

    lv_obj_t* base = Modal::show("print_completion_modal");
    if (!base) {
        SKIP("Base modal component not available in test fixture");
    }
    process_lvgl(20);

    const int bursts = env_iterations(40);
    const int pairs_per_burst = 8;
    spdlog::info("[action_prompt-stress/stacked] {} bursts × {} pairs (depth-2 cycles)", bursts,
                 pairs_per_burst);

    size_t heap_warm = 0;
    auto data = make_prompt("Stacked", 3);
    for (int b = 0; b < bursts; ++b) {
        for (int p = 0; p < pairs_per_burst; ++p) {
            REQUIRE(modal_.show_prompt(test_screen(), data));
            modal_.hide();
        }
        process_lvgl(50);
        if (heap_warm == 0 && (b + 1) * 2 >= bursts)
            heap_warm = heap_bytes();
    }

    Modal::hide(base);
    process_lvgl(120);
    require_tree_settled(modal_, test_screen(), baseline);
    require_no_heap_growth(heap_warm, heap_bytes(), "stacked");
}

// Trigger hide() through a real button click instead of calling hide()
// directly. The click path runs through ButtonCallbackData → gcode_callback
// → handle_button_click → hide(), which is the actual production path and
// exercises the ButtonCallbackData lifetime token wiring (#437) plus
// lv_event_send → lv_obj_destructor sequence visible in the #877 backtrace.
TEST_CASE_METHOD(ActionPromptStressFixture,
                 "ActionPromptModal stress: click-driven hide via real button events",
                 "[action_prompt][stress][.ui_integration]") {
    const int iterations = env_iterations(150);
    spdlog::info("[action_prompt-stress/click] click-driven × {}", iterations);

    int clicks_observed = 0;
    modal_.set_gcode_callback([&clicks_observed](const std::string&) { ++clicks_observed; });

    // Walk the dialog tree depth-first for any button. Buttons created by
    // ActionPromptModal::create_button are lv_button_t instances added to a
    // dynamic content container — they're nameless, so we find them by type.
    std::function<lv_obj_t*(lv_obj_t*)> find_button = [&](lv_obj_t* node) -> lv_obj_t* {
        if (!node)
            return nullptr;
        if (lv_obj_check_type(node, &lv_button_class))
            return node;
        int n = lv_obj_get_child_count(node);
        for (int j = 0; j < n; ++j) {
            if (auto* hit = find_button(lv_obj_get_child(node, j)))
                return hit;
        }
        return nullptr;
    };

    const TreeBaseline baseline = tree_baseline(test_screen());
    size_t heap_warm = 0;

    auto data = make_prompt("Click", 4);
    int click_attempts = 0;
    for (int i = 0; i < iterations; ++i) {
        REQUIRE(modal_.show_prompt(test_screen(), data));

        // Use modal_.dialog() directly — Modal exposes the dialog object so we
        // don't need a name-based lookup. Synthesize a click on any button in
        // the tree. The click triggers handle_button_click → hide(), exactly
        // the production teardown path that shows up in the #877 backtrace.
        lv_obj_t* dialog = modal_.dialog();
        lv_obj_t* btn = dialog ? find_button(dialog) : nullptr;
        if (btn) {
            ++click_attempts;
            lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
            // The click handler closes the modal, but be defensive: if the
            // handler somehow didn't, hide() explicitly to keep iteration
            // counts consistent.
            if (modal_.is_visible())
                modal_.hide();
        } else {
            modal_.hide();
        }

        if ((i + 1) % 25 == 0) {
            process_lvgl(40);
            if (heap_warm == 0 && (i + 1) * 2 >= iterations)
                heap_warm = heap_bytes();
        }
    }
    spdlog::info("[action_prompt-stress/click] click attempts: {}/{}", click_attempts, iterations);
    process_lvgl(120);
    spdlog::info("[action_prompt-stress/click] {} gcode_callbacks invoked of {} iterations",
                 clicks_observed, iterations);
    REQUIRE(clicks_observed > iterations / 2); // most iterations should fire a click
    require_tree_settled(modal_, test_screen(), baseline);
    require_no_heap_growth(heap_warm, heap_bytes(), "click-driven");
}

// The closest x86 approximation of the AD5X tick cadence: queue many
// show/hide pairs WITHOUT calling lv_timer_handler in between, then drain
// them all in a single timer iteration. On AD5X a "tick" is ~30 ms during
// which the websocket-driven prompts can fire 5-8 times; on x86 our test
// loop runs in microseconds, so accumulating without flushing is the only
// way to recreate the queue depth.
//
// We also vary the prompt content per cycle so each show() builds a
// differently-shaped tree, defeating any address-reuse hint that would
// help an x86 allocator hand the same memory back consistently.
TEST_CASE_METHOD(ActionPromptStressFixture,
                 "ActionPromptModal stress: single-tick drain of large pending queue",
                 "[action_prompt][stress][.ui_integration]") {
    const int bursts = env_iterations(20);
    const int pairs_per_burst = 30; // much larger than the natural 5-8 from AD5X
    spdlog::info("[action_prompt-stress/single-tick] {} bursts × {} pairs (single drain each)",
                 bursts, pairs_per_burst);

    const TreeBaseline baseline = tree_baseline(test_screen());
    size_t heap_warm = 0;

    for (int b = 0; b < bursts; ++b) {
        for (int p = 0; p < pairs_per_burst; ++p) {
            auto data =
                make_prompt("ST" + std::to_string(b) + "_" + std::to_string(p), 1 + (p % 4));
            REQUIRE(modal_.show_prompt(test_screen(), data));
            modal_.hide();
        }
        // ONE timer iteration to drain everything — the largest async-delete
        // batch we can produce. lv_timer_handler_safe() pauses repeating
        // timers, fires ready one-shots, then resumes — same as production
        // ticks but without our 5ms slicing.
        lv_tick_inc(30); // simulate a single MIPS-class tick
        lv_timer_handler_safe();
        if (heap_warm == 0 && (b + 1) * 2 >= bursts)
            heap_warm = heap_bytes();
    }
    process_lvgl(120);
    require_tree_settled(modal_, test_screen(), baseline);
    // Nothing here goes through UpdateQueue, but a corrupted drain that left
    // work stranded would show up here too, and it costs nothing to say so.
    REQUIRE(helix::ui::UpdateQueue::instance().pending_count() == 0);
    require_no_heap_growth(heap_warm, heap_bytes(), "single-tick-drain");
}

// Interleave queue_update() lambdas (the path websocket-driven prompts take
// in production: notify_gcode_response handler → ActionPromptManager →
// show callback → queue_update → modal->show_prompt) with direct hide()
// calls. The next tick's process_pending() then runs while the previous
// tick's async-deletes are still in flight inside obj_delete_core.
TEST_CASE_METHOD(ActionPromptStressFixture,
                 "ActionPromptModal stress: queue_update racer with direct hide",
                 "[action_prompt][stress][.ui_integration]") {
    const int bursts = env_iterations(30);
    const int pairs_per_burst = 12;
    spdlog::info("[action_prompt-stress/racer] {} bursts × {} pairs (queued show + direct hide)",
                 bursts, pairs_per_burst);

    const TreeBaseline baseline = tree_baseline(test_screen());
    size_t heap_warm = 0;
    auto data = make_prompt("Race", 2);

    for (int b = 0; b < bursts; ++b) {
        for (int p = 0; p < pairs_per_burst; ++p) {
            // Queued show (mimics the websocket → queue_update path)
            helix::ui::queue_update("test::action_prompt_show",
                                    [this, data]() { modal_.show_prompt(test_screen(), data); });
            // Direct hide on the next loop iteration's tick boundary
            helix::ui::queue_update("test::action_prompt_hide", [this]() { modal_.hide(); });
        }
        // Drain UpdateQueue + LVGL async list together
        lv_tick_inc(30);
        lv_timer_handler_safe();
        if (heap_warm == 0 && (b + 1) * 2 >= bursts)
            heap_warm = heap_bytes();
    }
    // Final flush
    process_lvgl(120);
    require_tree_settled(modal_, test_screen(), baseline);
    // Every queued show/hide must have run. A non-zero count here means the
    // drain stopped early — the exact shape a corrupted async-delete walk
    // produces when it aborts mid-batch.
    REQUIRE(helix::ui::UpdateQueue::instance().pending_count() == 0);
    require_no_heap_growth(heap_warm, heap_bytes(), "queue-update-racer");
}

// Reproducer for cluster:pstat-async-delete (#906 canonical, dups #913/#915/
// #918/#919/#921/#923 plus issueless bundles BRBLKTMY/TXVE53X6/24HY6NTC/
// 3R4QK9XS/8N2BPWMV/RDQLVFFJ between 2026-04-29 and 2026-05-04).
//
// Production breadcrumbs share the identical shape:
//   modal+ action_prompt_modal 1
//   modal- action_prompt_modal 0
//   overlay Print Status
//   pstat_t on_activate           ← from src/ui/ui_panel_print_status.cpp:739
//   <fault in lv_event_mark_deleted / get_prop_core / lv_ll_remove>
//
// The fault is reached via lv_timer_handler → lv_async_call →
// lv_obj_delete_async_cb → recursive obj_delete_core. Hypothesis: the modal
// hide enqueues safe_delete_deferred_raw on the dialog tree at the same time
// that on_activate's lv_image_set_src + child re-shaping schedule async
// deletes on a sibling overlay subtree. The next tick processes both batches
// in one obj_delete_core walk; deleting one widget invalidates the global
// event-list iterator the other branch is mid-traversal.
//
// We approximate the production interaction with a host subtree shaped like
// PrintStatusPanel's reactivation surface (image + transient child container)
// without dragging in PrintStatusPanel's full plumbing. The point is to land
// the modal hide and the host subtree mutation in the SAME single-tick drain
// so they fight for the async-delete list.
TEST_CASE_METHOD(ActionPromptStressFixture,
                 "ActionPromptModal stress: print-status reactivation racer (#906 cluster)",
                 "[action_prompt][stress][pstat][.ui_integration]") {
    const int bursts = env_iterations(40);
    const int pairs_per_burst = 8;
    const int transient_children = 6;
    spdlog::info("[action_prompt-stress/pstat] {} bursts × {} pairs (modal hide + sibling mutate)",
                 bursts, pairs_per_burst);

    // Baseline is taken BEFORE the host subtree exists, so the final check
    // covers the host's own async teardown as well as the modal's.
    const TreeBaseline baseline = tree_baseline(test_screen());
    size_t heap_warm = 0;

    // Sibling subtree representing the Print Status overlay's reactivation
    // surface. Stays alive across iterations; its children churn each cycle.
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_set_size(host, 400, 240);
    lv_obj_t* image = lv_image_create(host);
    lv_obj_set_size(image, 200, 200);

    // Two image source paths to alternate so lv_image_set_src actually
    // takes the cache-eviction branch each call. LVGL accepts symbolic
    // names for its built-in images via the asset manager but we keep this
    // independent of asset registration by using inline LV_SYMBOL strings,
    // which set_src treats as text-symbol sources (still exercises the
    // src-change path that would free a previous descriptor).
    const char* src_a = LV_SYMBOL_FILE;
    const char* src_b = LV_SYMBOL_IMAGE;

    auto rebuild_transients = [&]() {
        // Mirror "destroy children async; create fresh ones synchronously"
        // — Print Status doesn't recreate buttons but other overlays in the
        // same activation chain do (gcode_viewer, color swatches). We use
        // lv_obj_delete_async, which is the supposed-safe escape route, to
        // verify that even the sanctioned async delete corrupts the event
        // list when batched with Modal::hide()'s safe_delete_deferred_raw.
        int n = lv_obj_get_child_count(host);
        for (int i = n - 1; i >= 1; --i) { // skip child 0 = image
            lv_obj_delete_async(lv_obj_get_child(host, i));
        }
        for (int i = 0; i < transient_children; ++i) {
            lv_obj_t* btn = lv_button_create(host);
            lv_obj_set_size(btn, 60, 30);
        }
    };

    auto data = make_prompt("Pstat", 3);
    for (int b = 0; b < bursts; ++b) {
        for (int p = 0; p < pairs_per_burst; ++p) {
            REQUIRE(modal_.show_prompt(test_screen(), data));
            modal_.hide(); // schedules safe_delete_deferred_raw on dialog
            // Sibling mutation in the same tick window — this is the
            // "pstat_t on_activate" moment in the production breadcrumbs.
            lv_image_set_src(image, (p & 1) ? src_b : src_a);
            rebuild_transients();
        }
        // Single-tick drain: every modal hide AND every host rebuild from
        // this burst lands in one lv_async_call_cancel + obj_delete_core
        // pass. AD5X/MIPS produces the same accumulation naturally because
        // its tick spacing (~30ms) accommodates several websocket prompts
        // between drains.
        lv_tick_inc(30);
        lv_timer_handler_safe();
        if (heap_warm == 0 && (b + 1) * 2 >= bursts)
            heap_warm = heap_bytes();
        if ((b + 1) % 5 == 0) {
            spdlog::info("[action_prompt-stress/pstat] burst {}/{}", b + 1, bursts);
        }
    }
    process_lvgl(120);
    // Tear the host down through the same async path so the destructor walk
    // gets one more shot at landing on a corrupted list.
    lv_obj_delete_async(host);
    process_lvgl(120);
    // Both the modal dialogs and the whole host subtree must be gone: an
    // async-delete list that lost entries mid-walk leaves either one parented.
    require_tree_settled(modal_, test_screen(), baseline);
    REQUIRE(helix::ui::UpdateQueue::instance().pending_count() == 0);
    require_no_heap_growth(heap_warm, heap_bytes(), "pstat-racer");
}
