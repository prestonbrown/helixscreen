// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_probe_overlay_lifecycle.cpp
 * @brief ProbeOverlay's lifecycle hooks retire the async guard they defer on
 *
 * ProbeOverlay reads its probe configuration off Klipper and hands the result
 * back through lifetime_.defer() / lifetime_.token(), so the overlay can be
 * torn down with a reply still in flight. OverlayBase::on_deactivate() and
 * OverlayBase::cleanup() are what invalidate that guard — expiring outstanding
 * tokens so a late reply is dropped instead of writing into an overlay that is
 * no longer on screen. An override that does not reach the base leaves the
 * guard live and the deferred work runnable.
 *
 * The cleanup_called_ flag the base maintains rides the same call, so it pins
 * the contract from the outside as well. visible_ is deliberately not asserted:
 * it is already false on a freshly built overlay, so it holds whether or not
 * the base call happens and would discriminate nothing.
 */

#include "ui_probe_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "async_lifetime_guard.h"

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// lifetime_ is protected on OverlayBase, so a subclass reaches it without any
/// production-side test hook. The overlay under test is the real ProbeOverlay:
/// only the reads are added here, no hook is overridden.
class ProbeOverlayAccess : public ProbeOverlay {
  public:
    helix::LifetimeToken token() const {
        return lifetime_.token();
    }

    /// Queue work the way load_probe_config() does, so the assertion is about
    /// the guard the production path actually defers on.
    void defer_marker(bool& ran) {
        lifetime_.defer("ProbeOverlayAccess::marker", [&ran]() { ran = true; });
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ProbeOverlay::on_deactivate expires outstanding async tokens",
                 "[ui][probe_overlay][lifecycle][async]") {
    ProbeOverlayAccess overlay;

    helix::LifetimeToken tok = overlay.token();
    REQUIRE_FALSE(tok.expired());

    overlay.on_deactivate();

    // A reply arriving after the overlay left the screen must find the token
    // dead; this is the check every deferred continuation makes before running.
    CHECK(tok.expired());
}

TEST_CASE_METHOD(LVGLTestFixture, "ProbeOverlay::cleanup expires outstanding async tokens",
                 "[ui][probe_overlay][lifecycle][async]") {
    ProbeOverlayAccess overlay;

    helix::LifetimeToken tok = overlay.token();
    REQUIRE_FALSE(tok.expired());

    overlay.cleanup();

    CHECK(tok.expired());
    CHECK(overlay.cleanup_called());
}

TEST_CASE_METHOD(LVGLTestFixture, "work deferred before deactivation does not run afterwards",
                 "[ui][probe_overlay][lifecycle][async]") {
    ProbeOverlayAccess overlay;

    bool ran = false;
    overlay.defer_marker(ran);
    overlay.on_deactivate();

    UpdateQueue::instance().drain();

    // The queued lambda captures the overlay; running it here is the
    // use-after-deactivate the guard exists to prevent.
    CHECK_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "work deferred while the overlay is live still runs",
                 "[ui][probe_overlay][lifecycle][async]") {
    ProbeOverlayAccess overlay;

    bool ran = false;
    overlay.defer_marker(ran);

    UpdateQueue::instance().drain();

    // Without this the skip above could come from the queue never draining,
    // and every assertion here would hold for the wrong reason.
    CHECK(ran);
}
