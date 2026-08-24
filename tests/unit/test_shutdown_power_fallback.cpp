// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/ui/panel_widgets/shutdown_widget.h"
#include "../helix_test_fixture.h"

#include "../catch_amalgamated.hpp"

/**
 * Local-fallback policy for a failed Moonraker machine.reboot / machine.shutdown.
 *
 * Non-systemd printer hosts run Moonraker with `provider: none`, whose
 * BaseProvider implements reboot/shutdown as `sudo systemctl ...`. Hosts like
 * the Creality K2 (OpenWrt/procd) ship neither `sudo` nor `systemctl`, so those
 * RPCs can only ever fail — the user gets "Reboot failed" and no way to power
 * the machine down from the UI, even though busybox /sbin/reboot is right there
 * and SystemPower already knows how to call it.
 *
 * When helixscreen runs ON the printer host, "reboot the printer" and "reboot
 * this device" are the same act, so a failed RPC may be retried locally. When
 * the printer is a *different* host, it must not be: rebooting locally would
 * take down the screen while the printer stays up.
 */
class ShutdownFallbackFixture : public HelixTestFixture {};

TEST_CASE_METHOD(ShutdownFallbackFixture, "Same-host machine action failure falls back to local",
                 "[shutdown][power]") {
    SECTION("reboot failure runs the local action") {
        int calls = 0;
        const bool handled = helix::handle_machine_power_failure(
            "sudo: not found", /*is_reboot=*/true, /*allow_local_fallback=*/true, [&calls]() {
                ++calls;
                return true;
            });

        CHECK(calls == 1);
        CHECK(handled);
    }

    SECTION("shutdown failure runs the local action") {
        int calls = 0;
        const bool handled = helix::handle_machine_power_failure(
            "sudo: not found", /*is_reboot=*/false, /*allow_local_fallback=*/true, [&calls]() {
                ++calls;
                return true;
            });

        CHECK(calls == 1);
        CHECK(handled);
    }

    SECTION("a local action that itself fails reports failure") {
        int calls = 0;
        const bool handled = helix::handle_machine_power_failure(
            "sudo: not found", /*is_reboot=*/true, /*allow_local_fallback=*/true, [&calls]() {
                ++calls;
                return false;
            });

        CHECK(calls == 1);
        CHECK_FALSE(handled);
    }
}

TEST_CASE_METHOD(ShutdownFallbackFixture, "Remote-printer machine action failure stays remote",
                 "[shutdown][power]") {
    // Dual-scope "Printer" target: the failing host is NOT this one. Powering
    // this device down instead would kill the screen and leave the printer up.
    int calls = 0;
    const bool handled = helix::handle_machine_power_failure(
        "Klippy Request Timed Out", /*is_reboot=*/true, /*allow_local_fallback=*/false, [&calls]() {
            ++calls;
            return true;
        });

    CHECK(calls == 0);
    CHECK_FALSE(handled);
}

TEST_CASE_METHOD(ShutdownFallbackFixture, "Missing local action is not invoked",
                 "[shutdown][power]") {
    // Defensive: an empty std::function must not be called even when the
    // fallback is permitted.
    CHECK_FALSE(helix::handle_machine_power_failure("boom", /*is_reboot=*/true,
                                                    /*allow_local_fallback=*/true, {}));
}
