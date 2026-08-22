// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_updates_external.cpp
 * @brief Tests for the in-app-update gates (firmware flag + physical writability).
 *
 * Covers helix_parse_truthy_env() (the pure parse that feeds the cached
 * updates_externally_managed() helper) and confirms the cached predicate is
 * consistent with the process environment. The cache is deliberately NOT
 * exercised for both true/false in one process — the parse function is the
 * testable unit; the cache is a thin static wrapper around it.
 *
 * Also covers compute_self_update_supported() — the pure predicate behind the
 * "can a self-update PHYSICALLY be applied?" check — using real temp dirs. It has
 * three terms, one per route install.sh can take: a writable PARENT (atomic swap),
 * a writable install ROOT (in-place replacement), or root obtainable (sudo). Also
 * the combined update_install_suppressed() gate.
 */

#include "app_globals.h"
#include "platform_info.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

TEST_CASE("helix_parse_truthy_env recognizes truthy values", "[update][external]") {
    // Truthy — case-insensitive
    CHECK(helix_parse_truthy_env("1"));
    CHECK(helix_parse_truthy_env("true"));
    CHECK(helix_parse_truthy_env("TRUE"));
    CHECK(helix_parse_truthy_env("True"));
    CHECK(helix_parse_truthy_env("yes"));
    CHECK(helix_parse_truthy_env("YES"));
    CHECK(helix_parse_truthy_env("on"));
    CHECK(helix_parse_truthy_env("ON"));
    // Surrounding whitespace tolerated (helixscreen.env may carry a stray space)
    CHECK(helix_parse_truthy_env("  1  "));
    CHECK(helix_parse_truthy_env("\ttrue\n"));
}

TEST_CASE("helix_parse_truthy_env rejects falsy and empty values", "[update][external]") {
    CHECK_FALSE(helix_parse_truthy_env(nullptr));
    CHECK_FALSE(helix_parse_truthy_env(""));
    CHECK_FALSE(helix_parse_truthy_env("0"));
    CHECK_FALSE(helix_parse_truthy_env("false"));
    CHECK_FALSE(helix_parse_truthy_env("no"));
    CHECK_FALSE(helix_parse_truthy_env("off"));
    CHECK_FALSE(helix_parse_truthy_env("2"));
    CHECK_FALSE(helix_parse_truthy_env("enabled"));
    CHECK_FALSE(helix_parse_truthy_env("   "));
}

TEST_CASE("compute_updates_externally_managed: explicit flag beats the platform default",
          "[update][external]") {
    // Args: (disable_auto_updates, platform_default)

    // Truthy flag suppresses, whatever the platform would have said.
    CHECK(compute_updates_externally_managed("1", false));
    CHECK(compute_updates_externally_managed("true", false));
    CHECK(compute_updates_externally_managed("yes", false));
    CHECK(compute_updates_externally_managed("on", false));

    // Falsy flag FORCE-ENABLES self-update even where the platform defaults to
    // managed. This is the dev-box escape hatch on the Snapmaker U1 — without it,
    // a platform default would be unoverridable without a rebuild.
    CHECK_FALSE(compute_updates_externally_managed("0", true));
    CHECK_FALSE(compute_updates_externally_managed("no", true));
    CHECK_FALSE(compute_updates_externally_managed("false", true));

    // Unset (null or empty) defers to the platform, in both directions. An empty
    // string must read as unset, not as falsy — helixscreen.env can export an
    // empty value, and treating that as an explicit "no" would silently re-enable
    // self-update on a firmware-managed box.
    CHECK(compute_updates_externally_managed(nullptr, true));
    CHECK(compute_updates_externally_managed("", true));
    CHECK_FALSE(compute_updates_externally_managed(nullptr, false));
    CHECK_FALSE(compute_updates_externally_managed("", false));

    // Blank is ABSENT, not falsy. helixscreen.env values routinely carry a stray
    // space, and reading an all-whitespace value as an explicit "no" would flip a
    // firmware-managed install back to self-updating without anyone asking.
    CHECK(compute_updates_externally_managed(" ", true));
    CHECK(compute_updates_externally_managed("   ", true));
    CHECK(compute_updates_externally_managed("\t", true));
    CHECK_FALSE(compute_updates_externally_managed(" ", false));

    // A real value still parses with its surrounding whitespace trimmed.
    CHECK(compute_updates_externally_managed(" 1 ", false));
    CHECK_FALSE(compute_updates_externally_managed(" 0 ", true));
}

TEST_CASE("platform_defaults_to_external_updates drives the unset case",
          "[update][external][platform]") {
    // The platform answer is what a real U1 build supplies. Exercise both arms
    // through the override so this holds on every build host.
    helix::set_external_updates_default_override(1);
    CHECK(helix::platform_defaults_to_external_updates());
    CHECK(compute_updates_externally_managed(nullptr,
                                             helix::platform_defaults_to_external_updates()));

    helix::set_external_updates_default_override(0);
    CHECK_FALSE(helix::platform_defaults_to_external_updates());
    CHECK_FALSE(compute_updates_externally_managed(nullptr,
                                                   helix::platform_defaults_to_external_updates()));

    helix::set_external_updates_default_override(-1);
}

TEST_CASE("updates_externally_managed reflects the environment (cached)", "[update][external]") {
    // The value is cached process-wide, so we assert it agrees with the pure
    // predicate over the current env rather than trying to flip it mid-process.
    const bool expected = compute_updates_externally_managed(
        std::getenv("HELIX_DISABLE_AUTO_UPDATES"), helix::platform_defaults_to_external_updates());
    CHECK(updates_externally_managed() == expected);
    // Stable across calls (proves the cache doesn't re-read differently).
    CHECK(updates_externally_managed() == updates_externally_managed());
}

TEST_CASE("compute_self_update_supported treats an empty install root as supported",
          "[update][external]") {
    // Empty install root = unresolvable layout (bind-mounted binary). Conservative:
    // return TRUE so the installer's own fallbacks + the explicit flag remain the
    // deciding factors rather than a false negative from an unknown parent.
    // Neither escalation value may turn that into a block.
    CHECK(compute_self_update_supported("", /*can_escalate=*/false));
    CHECK(compute_self_update_supported("", /*can_escalate=*/true));
}

TEST_CASE("compute_self_update_supported is TRUE when the install-root parent is writable",
          "[update][external]") {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec) /
                                       ("helix_selfupdate_ok_" + std::to_string(::getpid()));
    std::filesystem::create_directories(base, ec);
    REQUIRE_FALSE(ec);

    // The install root itself need not exist — the rename swap acts on the PARENT
    // (base), which is writable here. Escalation is irrelevant to a writable parent.
    const std::string install_root = (base / "helixscreen").string();
    CHECK(compute_self_update_supported(install_root, /*can_escalate=*/false));
    CHECK(compute_self_update_supported(install_root, /*can_escalate=*/true));

    std::filesystem::remove_all(base, ec);
}

TEST_CASE("compute_self_update_supported is TRUE on a non-writable parent when root is reachable",
          "[update][external]") {
    if (::geteuid() == 0) {
        SKIP("running as root: access(W_OK) ignores permission bits");
    }

    // Escalation as the sole deciding term: neither the parent nor the root is
    // writable (the root does not exist here), so only can_escalate can answer.
    // That is the root-run embedded platforms, where geteuid()==0 short-circuits
    // the probe. It is NOT the /opt + unprivileged-service layout, which the unit's
    // NoNewPrivileges=true puts out of sudo's reach entirely — that one is covered
    // by the in-place case above, via write access to the root itself.
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec) /
                                       ("helix_selfupdate_sudo_" + std::to_string(::getpid()));
    std::filesystem::create_directories(base, ec);
    REQUIRE_FALSE(ec);

    std::filesystem::permissions(
        base, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    const std::string install_root = (base / "helixscreen").string();
    // Same non-writable parent, opposite answers — escalation is the deciding term.
    CHECK(compute_self_update_supported(install_root, /*can_escalate=*/true));
    CHECK_FALSE(compute_self_update_supported(install_root, /*can_escalate=*/false));

    std::filesystem::permissions(base, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("compute_self_update_supported is TRUE on a read-only parent when the install root "
          "itself is writable",
          "[update][external]") {
    if (::geteuid() == 0) {
        SKIP("running as root: access(W_OK) ignores permission bits");
    }

    // The standalone-display layout, and the one that was hidden in the field:
    // no local Klipper, so install.sh falls through to /opt/helixscreen. /opt is
    // root-owned (no rename, no atomic swap) but the root itself is chowned to
    // the service user by the unit's ExecStartPre. install.sh detects exactly
    // this and replaces the root's CONTENTS in place, so the install updates
    // fine and the updater must not hide itself.
    //
    // Escalation is deliberately false here: the shipped unit sets
    // NoNewPrivileges=true, so sudo cannot rescue this case even where sudoers
    // would allow it. If this term is dropped the user is locked out for good,
    // because the fix can only reach them through an update.
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec) /
                                       ("helix_selfupdate_inplace_" + std::to_string(::getpid()));
    const std::filesystem::path root = base / "helixscreen";
    std::filesystem::create_directories(root, ec);
    REQUIRE_FALSE(ec);

    // Root writable (0700), parent not (0500).
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::permissions(
        base, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    CHECK(compute_self_update_supported(root.string(), /*can_escalate=*/false));

    std::filesystem::permissions(base, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("compute_self_update_supported is FALSE when neither the root nor its parent is "
          "writable and root is not reachable",
          "[update][external]") {
    if (::geteuid() == 0) {
        // root bypasses W_OK permission bits on a normal fs (access(W_OK) still
        // returns 0), so the read-only assertion is meaningless under root CI.
        SKIP("running as root: access(W_OK) ignores permission bits");
    }

    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec) /
                                       ("helix_selfupdate_ro_" + std::to_string(::getpid()));
    const std::filesystem::path root = base / "helixscreen";
    std::filesystem::create_directories(root, ec);
    REQUIRE_FALSE(ec);

    // Drop write on BOTH (0500: owner read + exec only). rename() into the parent
    // fails, and so does rewriting the root's contents → nothing can apply.
    // Innermost first: an unwritable parent still permits chmod on its children.
    std::filesystem::permissions(
        root, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::permissions(
        base, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    CHECK_FALSE(compute_self_update_supported(root.string(), /*can_escalate=*/false));
    // Root still rescues it — this is a permissions problem, not a read-only mount.
    CHECK(compute_self_update_supported(root.string(), /*can_escalate=*/true));

    // Restore write so remove_all can clean up.
    std::filesystem::permissions(base, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("self_update_supported / update_install_suppressed are cached and consistent",
          "[update][external]") {
    // self_update_supported() is cached process-wide off the real install root.
    // Assert stability and that the combined gate is the OR of the two reasons.
    CHECK(self_update_supported() == self_update_supported());
    CHECK(update_install_suppressed() ==
          (updates_externally_managed() || !self_update_supported()));
}

TEST_CASE("checking is gated more weakly than installing", "[update][external]") {
    // The whole point of the split. Checking is a manifest fetch that touches no
    // files, so an install tree we cannot write must never silence it: a user on a
    // non-updatable install still needs to be told a new version exists, and that
    // notice is the only route back out — the fix for whatever made the install
    // non-updatable can only reach them through an update they cannot apply.
    //
    // Concretely: update_checks_suppressed() must not depend on
    // self_update_supported() at all.
    CHECK(update_checks_suppressed() == updates_externally_managed());

    // Checking is therefore implied by installing, never the reverse. If the two
    // ever become equal for a reason OTHER than the firmware flag, the trap is
    // back.
    if (update_checks_suppressed()) {
        CHECK(update_install_suppressed());
    }
    if (!self_update_supported() && !updates_externally_managed()) {
        CHECK(update_install_suppressed());
        CHECK_FALSE(update_checks_suppressed());
    }
}

TEST_CASE("root_escalation_available is cached and true under root", "[update][external]") {
    // The value depends on the host's sudoers, so the only universally true
    // assertions are stability and the euid-0 shortcut. (The test build stubs the
    // sudo probe out — see the mirror in tests/ui_test_utils.cpp — so this checks
    // the contract, not the probe.)
    CHECK(root_escalation_available() == root_escalation_available());
    if (::geteuid() == 0) {
        CHECK(root_escalation_available());
    }
}
