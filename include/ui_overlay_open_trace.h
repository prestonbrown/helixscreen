// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

typedef struct _lv_obj_t lv_obj_t;

namespace helix::nav {

/// What opening one overlay has cost, by its OverlayBase name.
struct OverlayOpenRecord {
    std::string name;
    /// Opens measured to their first drawn frame. An open whose push was cancelled, or
    /// that a newer open of the same root replaced, is not counted.
    uint32_t opens = 0;
    /// show() calls that built the widget tree (create() ran): the first open, and
    /// every open of a destroy_on_close() overlay.
    uint32_t builds = 0;
    double last_ms = 0;
    double max_ms = 0;
    double total_ms = 0;
};

/// Times an overlay open from its show() request to the end of the first frame drawn
/// after its queued push ran. The push is queued, so the frame that ends show()'s pass
/// can still be the old screen; the clock stops only once the overlay is in the stack.
/// Pure: every time is passed in, in milliseconds on one monotonic clock.
class OverlayOpenTracker {
  public:
    static constexpr std::size_t kMaxPending = 4;
    static constexpr std::size_t kMaxOverlays = 64;
    /// An open with no frame this long after its request is dropped, not recorded: its
    /// push was cancelled, or the display slept and the wait measures the sleep.
    static constexpr double kStaleMs = 10000.0;

    void requested(const void* root, const char* name, bool built, double t0_ms);
    void pushed(const void* root);
    /// A frame finished drawing: ends every open whose push has run.
    void frame_drawn(double now_ms);
    bool awaiting_frame() const {
        return pending_count_ > 0;
    }
    const std::vector<OverlayOpenRecord>& records() const {
        return records_;
    }

  private:
    struct Pending {
        const void* root = nullptr;
        std::size_t record = 0;
        double t0_ms = 0;
        bool pushed = false;
    };
    std::size_t record_for(const char* name);
    void drop_pending(std::size_t index);

    std::vector<OverlayOpenRecord> records_;
    std::array<Pending, kMaxPending> pending_{};
    std::size_t pending_count_ = 0;
};

/// The process-wide tracker, fed by OverlayBase::show(), the queued overlay push and the
/// default display's RENDER_READY event. Main thread only.
void overlay_open_requested(lv_obj_t* root, const char* name, bool built,
                            std::chrono::steady_clock::time_point t0);
void overlay_open_pushed(lv_obj_t* root);
std::vector<OverlayOpenRecord> overlay_open_records();
/// Forget every record and pending open, so a test starts with an empty trace and
/// the overlays earlier tests opened do not fill its kMaxOverlays slots.
void reset_overlay_open_trace_for_test();

} // namespace helix::nav
