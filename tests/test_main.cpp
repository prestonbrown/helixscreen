// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// Catch2 test runner main file
// This file compiles the Catch2 implementation once
#define CATCH_CONFIG_MAIN
#include "helix_test_fixture.h"
#include "test_helpers/stale_binary_check.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

#include "catch_amalgamated.hpp"

/// "<base> <content-hash>" for the sources this binary was built from, and the
/// file listing them, written by the link rule in mk/tests.mk
/// (scripts/tree_stamp.sh, scripts/test_link_sources.py).
extern "C" const char helix_build_tree_stamp[];
extern "C" const char helix_build_tree_stamp_paths[];

namespace {

/// Per-run cache sandbox, as a POD so it is zero-initialized before ANY dynamic
/// initialization runs — the pinning below happens too early to rely on a
/// std::string having been constructed.
char g_cache_sandbox[256];

} // namespace

/// Pin every cache rung a test can reach into a per-run sandbox.
///
/// The real cache cascade (src/system/helix_cache_dir.cpp) is linked, so an
/// unpinned run resolves to $HOME/.cache/helix and writes there.
///
/// WHY A CONSTRUCTOR AND NOT A CATCH2 LISTENER: the cache directory is latched
/// by a Meyer's singleton, and tests/unit/test_isolation_listener.cpp reaches it
/// from testRunStarting() — it calls ThumbnailProcessor::instance() to pre-warm
/// the pool, and that constructor resolves and creates its cache dir on the
/// spot. Catch2 runs listeners in registration order, which across translation
/// units is static-init order and therefore unspecified, so a listener that
/// pins the environment is in a race it can silently lose. It did: the run
/// still wrote to $HOME/.cache/helix/helix_thumbs. Priority 101 is the earliest
/// user constructor priority, so this lands before all normal-priority dynamic
/// initialization and before any listener exists to be ordered against.
///
/// Two variables, because one does not cover it. HELIX_CACHE_DIR is rung 1, and
/// pinning it is enough for tests that leave it alone. The cases in
/// test_cache_dir.cpp that deliberately clear or blank it to exercise
/// fall-through would sail straight past into $HOME/.cache — XDG_CACHE_HOME
/// catches those at rung 4, which is consulted before home() at rung 5.
///
/// Both are set only when the caller has not already chosen a value, so a
/// developer or CI job pointing the suite somewhere specific still wins.
///
/// POSIX only (mkdir/snprintf/setenv): this runs before the C++ runtime is
/// fully warmed up, so it deliberately avoids anything that needs it.
__attribute__((constructor(101))) static void helix_pin_test_cache_sandbox() {
    char root[sizeof(g_cache_sandbox)];
    if (snprintf(root, sizeof(root), "/tmp/helix_test_sandbox_%ld", (long)getpid()) <= 0)
        return;

    char sub[sizeof(g_cache_sandbox) + 16];
    if (mkdir(root, 0700) != 0 && access(root, W_OK) != 0)
        return;
    snprintf(sub, sizeof(sub), "%s/cache", root);
    mkdir(sub, 0700);
    const char* existing = getenv("HELIX_CACHE_DIR");
    if (!existing || existing[0] == '\0')
        setenv("HELIX_CACHE_DIR", sub, 1);

    snprintf(sub, sizeof(sub), "%s/xdg", root);
    mkdir(sub, 0700);
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (!xdg || xdg[0] == '\0')
        setenv("XDG_CACHE_HOME", sub, 1);

    snprintf(g_cache_sandbox, sizeof(g_cache_sandbox), "%s", root);
}

/// Pin both diagnostic ingest endpoints to a refused loopback port, so no run
/// of this binary can reach the production ingest under any environment.
///
/// Unconditional by design, unlike the cache pins above: for these two
/// variables an inherited value is itself the hazard, and the suite must fail
/// fast against a refused port rather than succeed against the real worker.
///
/// The side effect is load-bearing: with this pin in place, every test's
/// EnvVarGuard saves the loopback as its restore target, so a guard that dies
/// before a deferred upload executes can only ever retarget to a refused
/// port, never to production.
__attribute__((constructor(101))) static void helix_pin_test_ingest_loopback() {
    setenv("HELIX_BUNDLE_WORKER_URL", "http://127.0.0.1:9/", 1);
    setenv("HELIX_CRASH_WORKER_URL", "http://127.0.0.1:9/", 1);
}

/// Pin the in-app USB automounter off for the whole binary.
///
/// Same unconditional shape as the ingest pins, and for the same reason: an
/// armed value is itself the hazard. UsbBackendLinux::start() constructs the
/// fallback mounter, CI containers run as root, and a host with a USB stick
/// attached must never take real mount(2) calls from a test run. The
/// automounter's create() refuses to arm while HELIX_USB_AUTOMOUNT=0, so this
/// constructor disarms every current and future test that touches the
/// backend - with or without a fixture, at any euid, even if the caller
/// exported a different value.
__attribute__((constructor(101))) static void helix_disarm_test_usb_automount() {
    setenv("HELIX_USB_AUTOMOUNT", "0", 1);
}

/// Keep the Bluetooth plugin unloaded for the whole binary.
///
/// The plugin is built beside helix-tests, so on a host with an adapter the loader
/// would open the system bus and register a BlueZ pairing agent on the developer's
/// machine the first time a test reaches a plugin context. Tests that exercise
/// plugin consumers swap fakes into BluetoothLoader's function pointers instead.
__attribute__((constructor(101))) static void helix_disable_test_bluetooth_plugin() {
    setenv("HELIX_BLUETOOTH", "0", 1);
}

namespace {

/// Remove the sandbox once the run is over. Only the teardown lives in a
/// listener — by testRunEnded the ordering hazard above is long past.
///
/// A run also starts by warning when the binary predates the tree's sources:
/// `./build/bin/helix-tests` after an edit without `make test` reports the
/// previous build's results.
///
/// Every case also starts from HelixTestFixture's baseline, not only the ones
/// that derive from it: CI shards run in random order, so a plain-fixture or
/// fixture-less case would otherwise inherit whatever the case before it left in
/// the process-wide singletons and the shared config sandbox.
class CacheSandboxListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        helix::test::warn_if_binary_stale("helix-tests", helix_build_tree_stamp,
                                          helix_build_tree_stamp_paths);
    }

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        HelixTestFixture::reset_all();
    }

    void testRunEnded(Catch::TestRunStats const&) override {
        HelixTestFixture::keep_logger_for_exit();

        // Best-effort: a leftover sandbox is litter, never a test failure.
        if (g_cache_sandbox[0] == '\0')
            return;
        std::error_code ec;
        std::filesystem::remove_all(g_cache_sandbox, ec);
    }
};

} // namespace

CATCH_REGISTER_LISTENER(CacheSandboxListener)
