// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three runtimes decide whether a symbol map is absolute-linked: this one,
// scripts/resolve-backtrace.sh and scripts/telemetry-crashes.py. They cannot
// share code, so they share tests/fixtures/load_base_rule.json and each is
// checked against it (the other two in tests/shell/test_load_base_rule_conformance.bats).
// The failure mode this guards is a backtrace that names a plausible wrong
// function rather than erroring, so drift here is silent.

import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { symbolsAreAbsolute } from "../symbol-resolver.js";

type Case = {
  name: string;
  symbols: string[];
  load_base: string;
  absolute: boolean;
};

const fixture = JSON.parse(
  readFileSync(
    join(__dirname, "..", "..", "..", "..", "tests", "fixtures", "load_base_rule.json"),
    "utf8",
  ),
) as { cases: Case[] };

describe("shared load-base rule", () => {
  it("has cases to check", () => {
    expect(fixture.cases.length).toBeGreaterThanOrEqual(5);
  });

  for (const c of fixture.cases) {
    it(c.name, () => {
      const symbols = c.symbols.map((a, i) => ({
        address: Number.parseInt(a, 16),
        name: `sym${i}`,
      }));
      expect(symbolsAreAbsolute(symbols, Number.parseInt(c.load_base, 16))).toBe(
        c.absolute,
      );
    });
  }
});
