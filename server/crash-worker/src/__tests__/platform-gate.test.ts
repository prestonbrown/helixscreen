// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the platform gate that keeps crash reports from binaries we did not
// build out of the issue tracker (see issue #1410).
//
// A HelixScreen build reports whatever UpdateChecker::get_platform_key() compiles
// in, plus platform keys retired by the MIPS unification that deployed binaries
// keep sending until the fleet turns over. A report naming anything else came
// from a modified binary, whose addresses resolve against no symbol file we
// publish — so the issue it files is unactionable no matter how complete it looks.
//
// tests/shell/test_update_platform_coverage.bats enforces that the allowlist here
// covers every get_platform_key() value and that worker-only keys are declared
// retired with a reason; these tests cover the predicate itself.

import { describe, it, expect } from "vitest";
import { isKnownPlatform } from "../index";

/** Every key get_platform_key() can return today. */
const RELEASE_PLATFORMS = [
  "ad5m",
  "android",
  "cc1",
  "esp32",
  "k2",
  "mips",
  "pi",
  "pi32",
  "snapmaker-u1",
  "x86",
  "x86-desktop",
];

/** Keys no current build returns but deployed binaries still report. */
const RETIRED_PLATFORMS = [
  "k1", // pre-unification K1 series builds; now "mips"
  "ad5x", // pre-unification AD5X builds; now "mips"
];

describe("isKnownPlatform", () => {
  it("accepts every platform a real build can report", () => {
    for (const p of RELEASE_PLATFORMS) {
      expect(isKnownPlatform(p), `${p} should be accepted`).toBe(true);
    }
  });

  it("accepts retired keys while pre-unification binaries remain deployed", () => {
    // Refusing these would silently drop every crash report from the
    // pre-unification k1/ad5x fleet.
    for (const p of RETIRED_PLATFORMS) {
      expect(isKnownPlatform(p), `${p} should still be accepted`).toBe(true);
    }
  });

  it("rejects the platform from the fork bundle that motivated this", () => {
    // AAHQWVA6 reported platform "creator5" on v0.99.115. No release has ever
    // published a creator5 asset and get_platform_key() cannot return that
    // string, so the binary was not ours.
    expect(isKnownPlatform("creator5")).toBe(false);
  });

  it("rejects junk, empty, and near-miss spellings", () => {
    expect(isKnownPlatform("")).toBe(false);
    expect(isKnownPlatform("linux")).toBe(false);
    expect(isKnownPlatform("raspberry-pi")).toBe(false);
    // A build target name is not a platform key.
    expect(isKnownPlatform("mips-docker")).toBe(false);
    expect(isKnownPlatform("k1c")).toBe(false);
  });

  it("is exact, not fuzzy — no case folding, trimming, or prefix matching", () => {
    // The value goes on to select an R2 symbol path, so a loose match would
    // send us looking up symbols under a key no build ever wrote.
    expect(isKnownPlatform("PI")).toBe(false);
    expect(isKnownPlatform("Pi")).toBe(false);
    expect(isKnownPlatform(" pi")).toBe(false);
    expect(isKnownPlatform("pi ")).toBe(false);
    expect(isKnownPlatform("pi3")).toBe(false);
    expect(isKnownPlatform("pi32x")).toBe(false);
  });

  it("does not treat Set internals as members", () => {
    // Guards the "new Set([...])" spelling against a regression to a plain
    // object literal, where "constructor" and friends would test true.
    expect(isKnownPlatform("constructor")).toBe(false);
    expect(isKnownPlatform("toString")).toBe(false);
    expect(isKnownPlatform("has")).toBe(false);
  });

  it("allows the release set plus the declared retired keys, and nothing more", () => {
    // A stale extra key is as much a defect as a missing one: it is a platform
    // no build emits, so it can only ever admit a report we cannot symbolicate.
    const accepted = RELEASE_PLATFORMS.filter(isKnownPlatform);
    expect(accepted).toEqual(RELEASE_PLATFORMS);
    expect(accepted).toHaveLength(RELEASE_PLATFORMS.length);
    const retiredAccepted = RETIRED_PLATFORMS.filter(isKnownPlatform);
    expect(retiredAccepted).toEqual(RETIRED_PLATFORMS);
  });
});
