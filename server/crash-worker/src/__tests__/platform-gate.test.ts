// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the platform gate that keeps crash reports from binaries we did not
// build out of the issue tracker (see issue #1410).
//
// A HelixScreen build reports whatever UpdateChecker::get_platform_key() compiles
// in, and that function can only return one of ten keys. A report naming anything
// else came from a modified binary, whose addresses resolve against no symbol file
// we publish — so the issue it files is unactionable no matter how complete it looks.
//
// tests/shell/test_update_platform_coverage.bats enforces that the allowlist here
// and get_platform_key() stay identical; these tests cover the predicate itself.

import { describe, it, expect } from "vitest";
import { isKnownPlatform } from "../index";

/** Every key get_platform_key() can return, as of the gate landing. */
const RELEASE_PLATFORMS = [
  "ad5m",
  "ad5x",
  "cc1",
  "esp32",
  "k1",
  "k2",
  "pi",
  "pi32",
  "snapmaker-u1",
  "x86",
];

describe("isKnownPlatform", () => {
  it("accepts every platform a real build can report", () => {
    for (const p of RELEASE_PLATFORMS) {
      expect(isKnownPlatform(p), `${p} should be accepted`).toBe(true);
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
    // A build target name is not a platform key: the k1 target is "mips".
    expect(isKnownPlatform("mips")).toBe(false);
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

  it("allows exactly the release set and nothing more", () => {
    // A stale extra key is as much a defect as a missing one: it is a platform
    // no build emits, so it can only ever admit a report we cannot symbolicate.
    const accepted = RELEASE_PLATFORMS.filter(isKnownPlatform);
    expect(accepted).toEqual(RELEASE_PLATFORMS);
    expect(accepted).toHaveLength(10);
  });
});
