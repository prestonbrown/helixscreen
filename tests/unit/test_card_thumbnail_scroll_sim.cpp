// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The card thumbnail planner and slot pool driven the way PrintSelectPanel
// drives them on the ESP32, through a long scroll with recycled card widgets:
// every slot a decode needs must be free when it asks, however the window,
// the lane and the completions interleave.

#include "ui_virtual_list.h"

#include "card_thumbnail_plan.h"
#include "thumbnail_slot_pool.h"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <memory>
#include <random>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

constexpr size_t SLOT = 80688; // 164x164 RGB565A8
constexpr size_t BUDGET = 960 * 1024;
constexpr size_t LANE_DEPTH = 8;

/// A decoded image holding one pool slot, as EspPsramThumbnail does.
struct Thumb {
    Thumb(std::shared_ptr<helix::ThumbnailSlotPool> p, uint8_t* s) : pool(std::move(p)), slot(s) {}
    Thumb(const Thumb&) = delete;
    Thumb& operator=(const Thumb&) = delete;
    std::shared_ptr<helix::ThumbnailSlotPool> pool;
    uint8_t* slot;
    ~Thumb() {
        pool->release(slot);
    }
};
using ThumbPtr = std::shared_ptr<Thumb>;

struct File {
    ThumbPtr thumb;
    bool tried = false;
    uint32_t shown = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct Job {
    size_t index;
    std::shared_ptr<helix::ThumbnailSlotPool> pool;
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct Done {
    size_t index;
    ThumbPtr thumb;
};

/// PrintSelectPanel's card-thumbnail bookkeeping, with the card view's widget
/// pool and the HTTP lane reduced to what holds a slot.
struct Panel {
    std::vector<File> files;
    std::shared_ptr<helix::ThumbnailSlotPool> pool;
    int in_flight = 0;
    bool refused = false;
    uint32_t tick = 0;
    size_t first = 0, end = 0;
    std::deque<Job> lane;  ///< fetched, not yet decoded
    std::deque<Done> done; ///< decoded, deferred to the main thread
    std::vector<ssize_t> widget_items;
    std::vector<ThumbPtr> widget_thumbs;
    int acquire_failures = 0;
    int decodes = 0;
    int wasted = 0; ///< decodes whose card had left by the time they completed
    bool cancel_dropped = true;

    explicit Panel(size_t n, size_t widgets)
        : files(n), widget_items(widgets, -1), widget_thumbs(widgets) {}

    void bind_widget(size_t w, ssize_t item) {
        widget_thumbs[w] = item >= 0 ? files[static_cast<size_t>(item)].thumb : nullptr;
    }

    /// PrintSelectCardView::update_visible: show_window, then the metadata/sync callback.
    void show(size_t f, size_t e) {
        const std::vector<ssize_t> before = widget_items;
        const auto order =
            helix::ui::assign_pool_slots(widget_items, static_cast<int>(f), static_cast<int>(e));
        for (size_t s : order) {
            if (before[s] != widget_items[s]) {
                bind_widget(s, widget_items[s]);
            }
        }
        for (size_t s = 0; s < widget_items.size(); ++s) {
            if (widget_items[s] < 0) {
                widget_thumbs[s].reset();
            }
        }
        sync(f, e);
    }

    void sync(size_t f, size_t e) {
        first = f;
        end = e;
        ++tick;
        std::vector<helix::CardThumbnailState> states(files.size());
        for (size_t i = 0; i < files.size(); ++i) {
            if (i >= f && i < e) {
                files[i].shown = tick;
            }
            states[i].fetchable = true;
            states[i].tried = files[i].tried;
            states[i].held = files[i].thumb ? SLOT : 0;
            states[i].last_shown = files[i].shown;
        }
        const auto plan = helix::plan_card_thumbnails(
            states, f, e, static_cast<size_t>(std::max(in_flight, 0)), SLOT, BUDGET, refused);
        if (!plan.fetch.empty() && !pool) {
            pool = std::make_shared<helix::ThumbnailSlotPool>(
                SLOT, BUDGET / SLOT, [](size_t n) { return std::malloc(n); },
                [](void* p) { std::free(p); });
        }
        for (size_t i : plan.drop) {
            if (cancel_dropped && files[i].cancel) {
                files[i].cancel->store(true);
            }
            files[i].cancel.reset();
            files[i].thumb.reset();
            files[i].tried = false;
        }
        for (size_t i : plan.fetch) {
            files[i].tried = true;
            if (lane.size() >= LANE_DEPTH) {
                files[i].tried = false;
                refused = true;
                break;
            }
            files[i].cancel = std::make_shared<std::atomic<bool>>(false);
            lane.push_back({i, pool, files[i].cancel});
            ++in_flight;
        }
    }

    /// The lane worker decodes the oldest job into a slot of the pool it was given.
    void decode_one() {
        if (lane.empty()) {
            return;
        }
        Job job = lane.front();
        lane.pop_front();
        if (job.cancel->load()) {
            done.push_back({job.index, nullptr}); // dropped unsent: an error reply
            return;
        }
        ++decodes;
        uint8_t* slot = job.pool->acquire();
        if (!slot) {
            ++acquire_failures;
            done.push_back({job.index, nullptr});
            return;
        }
        done.push_back({job.index, std::make_shared<Thumb>(job.pool, slot)});
    }

    /// The deferred completion on the main thread.
    void complete_one() {
        if (done.empty()) {
            return;
        }
        Done d = std::move(done.front());
        done.pop_front();
        --in_flight;
        refused = false;
        if (d.thumb && !files[d.index].tried) {
            ++wasted;
        }
        if (d.thumb && files[d.index].tried) {
            files[d.index].thumb = std::move(d.thumb);
            for (size_t s = 0; s < widget_items.size(); ++s) {
                if (widget_items[s] == static_cast<ssize_t>(d.index)) {
                    bind_widget(s, widget_items[s]);
                }
            }
        }
        sync(first, end);
    }
};

constexpr int COLS = 4, ROW = 215, VIEW = 425, FILES = 50;
constexpr int ROWS = (FILES + COLS - 1) / COLS;
constexpr int BOTTOM = ROWS * ROW - VIEW;

/// Down to the bottom and back, twice, in uneven steps, with decodes and
/// completions landing between steps at random; then the lane drains.
void scroll(Panel& panel, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<int> path;
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y <= BOTTOM; y += 20 + static_cast<int>(rng() % 90)) {
            path.push_back(y);
        }
        path.push_back(BOTTOM);
        for (int y = BOTTOM; y >= 0; y -= 20 + static_cast<int>(rng() % 90)) {
            path.push_back(y);
        }
        path.push_back(0);
    }
    for (int y : path) {
        const auto win = helix::ui::compute_window(y, VIEW, ROW, ROWS, 0);
        panel.show(static_cast<size_t>(win.first * COLS),
                   static_cast<size_t>(std::min(FILES, win.last * COLS)));
        // The lane decodes far slower than the main thread scrolls and runs
        // completions: a decode is ~300ms, a scroll frame tens of ms.
        if (rng() % 3 == 0) {
            panel.decode_one();
        }
        if (rng() % 2) {
            panel.complete_one();
        }
        // Stopping at either end lets the lane catch up.
        if (y == 0 || y == BOTTOM) {
            for (int k = 0; k < 40; ++k) {
                panel.decode_one();
                panel.complete_one();
            }
        }
    }
    while (!panel.lane.empty() || !panel.done.empty()) {
        panel.decode_one();
        panel.complete_one();
    }
}

} // namespace

TEST_CASE("a long card scroll never asks the slot pool for a slot it does not have",
          "[card_thumbnail_plan][slots]") {
    for (unsigned seed = 1; seed <= 20; ++seed) {
        Panel panel(FILES, 12);
        scroll(panel, seed);
        INFO("seed " << seed);
        CHECK(panel.acquire_failures == 0);
        // Every window card ends up with its picture.
        for (size_t i = panel.first; i < panel.end; ++i) {
            CHECK(panel.files[i].thumb);
        }
    }
}

TEST_CASE("cards that scroll away cancel their queued fetches, so a drag decodes only what stays",
          "[card_thumbnail_plan][slots]") {
    int decodes = 0, wasted = 0, decodes_uncancelled = 0, wasted_uncancelled = 0;
    for (unsigned seed = 1; seed <= 20; ++seed) {
        Panel panel(FILES, 12);
        scroll(panel, seed);
        decodes += panel.decodes;
        wasted += panel.wasted;

        Panel uncancelled(FILES, 12);
        uncancelled.cancel_dropped = false;
        scroll(uncancelled, seed);
        decodes_uncancelled += uncancelled.decodes;
        wasted_uncancelled += uncancelled.wasted;
    }
    INFO("decodes " << decodes << " (wasted " << wasted << "), without cancelling "
                    << decodes_uncancelled << " (wasted " << wasted_uncancelled << ")");
    // Without cancelling, most of a drag's decodes are for cards already gone.
    REQUIRE(wasted_uncancelled > decodes_uncancelled / 2);
    // Cancelled, only a card dropped between its decode and its completion is
    // (here 80 of 1012; uncancelled, 1257 of 2189).
    CHECK(wasted * 10 <= decodes);
    CHECK(decodes < decodes_uncancelled);
}
