// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the crash dedup fingerprint.
//
// The fingerprint used to be signal/version/PC, which splits one defect across
// several issues: the PC moves with the architecture (different load base,
// different faulting instruction) and so does the signal (a poisoned read is
// SEGV on ARM and BUS on MIPS). #1347, #1356, #1357 and #1361 were all the same
// use-after-free in gcode_viewer_occluder_delete_cb, filed four times.
// Keying on the resolved symbol collapses them.

import { describe, it, expect, vi, afterEach } from "vitest";
import { crashFingerprint, findExistingIssue, normalizeSymbol } from "../github-app";
import { resolveBacktrace } from "../symbol-resolver";
import type { CrashReport, ResolvedBacktrace } from "../symbol-resolver";

/** A resolved backtrace with just the top frame filled in. */
function topFrame(symbol: string): ResolvedBacktrace {
  return {
    frames: [{ raw: "0x0", symbol }],
    autoDetectedBase: false,
    symbolFileFound: true,
  };
}

describe("normalizeSymbol", () => {
  it("strips the +offset, which differs per architecture", () => {
    expect(normalizeSymbol("gcode_viewer_occluder_delete_cb+0xc")).toBe(
      "gcode_viewer_occluder_delete_cb"
    );
    expect(normalizeSymbol("gcode_viewer_occluder_delete_cb+0x18")).toBe(
      "gcode_viewer_occluder_delete_cb"
    );
  });

  it("strips the C++ parameter list", () => {
    expect(normalizeSymbol("gcode_viewer_occluder_delete_cb(_lv_event_t*)+0x18")).toBe(
      "gcode_viewer_occluder_delete_cb"
    );
    expect(normalizeSymbol("helix::timing::get_ticks()+0x44")).toBe(
      "helix::timing::get_ticks"
    );
  });

  it("keeps a leading (anonymous namespace) while dropping the arguments", () => {
    expect(
      normalizeSymbol("(anonymous namespace)::crash_signal_handler(int, siginfo_t*, void*)+0xfe0")
    ).toBe("(anonymous namespace)::crash_signal_handler");
  });

  it("keeps operator() distinguishable", () => {
    expect(normalizeSymbol("Foo::operator()(int)+0x4")).toBe("Foo::operator()");
  });

  it("strips GCC clone and specialisation suffixes", () => {
    // Both spellings occur in real nm output: bracketed after the signature,
    // and appended straight onto the bare name.
    expect(
      normalizeSymbol("gcode_viewer_occluder_delete_cb(_lv_event_t*) [clone .lto_priv.0]+0x18")
    ).toBe("gcode_viewer_occluder_delete_cb");
    expect(normalizeSymbol("lv_label_event.lto_priv.0+0x60")).toBe("lv_label_event");
    expect(normalizeSymbol("obj_valid_child.isra.0+0xa4")).toBe("obj_valid_child");
    expect(normalizeSymbol("do_thing.constprop.0.isra.1+0x10")).toBe("do_thing");
  });

  it("returns empty for frames that carry no real symbol", () => {
    expect(normalizeSymbol("<shared library>")).toBe("");
    expect(normalizeSymbol(undefined)).toBe("");
    expect(normalizeSymbol("")).toBe("");
    expect(normalizeSymbol("   ")).toBe("");
  });

  it("leaves a name that is only a parenthesised group alone", () => {
    expect(normalizeSymbol("(anonymous namespace)")).toBe("(anonymous namespace)");
  });
});

describe("crashFingerprint", () => {
  // The two halves of #1347: same defect, same build, two architectures.
  const ad5m: CrashReport = {
    signal: 11,
    signal_name: "SIGSEGV",
    app_version: "0.99.116",
    platform: "ad5m",
    backtrace: ["0x2a00c8"],
  };
  const ad5x: CrashReport = {
    signal: 10,
    signal_name: "SIGBUS",
    app_version: "0.99.116",
    platform: "ad5x",
    backtrace: ["0x55c279f0"],
  };

  it("collapses the same defect across architectures", () => {
    const a = crashFingerprint(ad5m, topFrame("gcode_viewer_occluder_delete_cb(_lv_event_t*)+0xc"));
    const b = crashFingerprint(
      ad5x,
      topFrame("gcode_viewer_occluder_delete_cb(_lv_event_t*) [clone .lto_priv.0]+0x18")
    );
    expect(a).toBe(b);
    expect(a).toBe("gcode_viewer_occluder_delete_cb/0.99.116");
  });

  it("keeps distinct crash sites apart", () => {
    expect(crashFingerprint(ad5m, topFrame("lv_label_event+0x60"))).not.toBe(
      crashFingerprint(ad5m, topFrame("obj_delete_core+0x74"))
    );
  });

  it("still separates versions, so a fixed defect that returns files fresh", () => {
    const next: CrashReport = { ...ad5m, app_version: "0.99.117" };
    expect(crashFingerprint(ad5m, topFrame("lv_label_event+0x60"))).not.toBe(
      crashFingerprint(next, topFrame("lv_label_event+0x60"))
    );
  });

  it("falls back to signal/version/PC when there is no symbol map", () => {
    expect(crashFingerprint(ad5x, null)).toBe("SIGBUS/0.99.116/0x55c279f0");
    expect(crashFingerprint(ad5x, undefined)).toBe("SIGBUS/0.99.116/0x55c279f0");
  });

  it("falls back when the top frame lands in a shared library", () => {
    expect(crashFingerprint(ad5x, topFrame("<shared library>"))).toBe(
      "SIGBUS/0.99.116/0x55c279f0"
    );
  });

  it("falls back when the map is present but the top frame did not resolve", () => {
    const unresolved: ResolvedBacktrace = {
      frames: [{ raw: "0x55c279f0" }],
      autoDetectedBase: false,
      symbolFileFound: true,
    };
    expect(crashFingerprint(ad5x, unresolved)).toBe("SIGBUS/0.99.116/0x55c279f0");
  });

  it("keeps the legacy shape usable when a report has no backtrace at all", () => {
    const bare: CrashReport = { signal: 6, signal_name: "SIGABRT", app_version: "0.99.62" };
    expect(crashFingerprint(bare, null)).toBe("SIGABRT/0.99.62/no-bt");
  });

  it("caps a pathologically long template name", () => {
    const long = "ns::" + "Very<Long<Template<".repeat(20) + "T>>>::run(int)+0x8";
    const fp = crashFingerprint(ad5m, topFrame(long));
    expect(fp.length).toBeLessThanOrEqual(120 + "/0.99.116".length);
    expect(fp.endsWith("/0.99.116")).toBe(true);
  });
});

// ---------------------------------------------------------------------------
// End to end: resolve against a real symbol map, then fingerprint.
// The dedup path in createGitHubIssue does exactly this pair of calls, so this
// is the assertion that "one defect files one issue" actually holds once the
// per-architecture load base and clone suffixes are in play.
// ---------------------------------------------------------------------------

function mockBucket(files: Record<string, string>): R2Bucket {
  return {
    get: async (key: string) =>
      files[key] === undefined
        ? null
        : ({ text: async () => files[key], arrayBuffer: async () => new ArrayBuffer(0) } as R2ObjectBody),
  } as unknown as R2Bucket;
}

describe("resolveBacktrace + crashFingerprint", () => {
  // Same function in two builds of v0.99.116. Non-PIE on ad5m, loaded at
  // 0x555b0000 on ad5x, and the MIPS build carries an LTO clone suffix.
  const AD5M_SYM = "002a00bc T gcode_viewer_occluder_delete_cb(_lv_event_t*)\n";
  const AD5X_SYM =
    "006779d8 T gcode_viewer_occluder_delete_cb(_lv_event_t*) [clone .lto_priv.0]\n";

  const bucket = mockBucket({
    "symbols/v0.99.116/ad5m.sym": AD5M_SYM,
    "symbols/v0.99.116/ad5x.sym": AD5X_SYM,
  });

  it("gives both architectures the same fingerprint", async () => {
    const ad5m: CrashReport = {
      signal: 11,
      signal_name: "SIGSEGV",
      app_version: "0.99.116",
      platform: "ad5m",
      load_base: "0x0",
      backtrace: ["0x2a00c8"],
    };
    const ad5x: CrashReport = {
      signal: 10,
      signal_name: "SIGBUS",
      app_version: "0.99.116",
      platform: "ad5x",
      load_base: "0x555b0000",
      backtrace: ["0x55c279f0"],
    };

    const a = crashFingerprint(ad5m, await resolveBacktrace(bucket, ad5m));
    const b = crashFingerprint(ad5x, await resolveBacktrace(bucket, ad5x));

    expect(a).toBe("gcode_viewer_occluder_delete_cb/0.99.116");
    expect(b).toBe(a);
  });

  it("falls back to signal/version/PC for a build with no published map", async () => {
    const unpublished: CrashReport = {
      signal: 11,
      signal_name: "SIGSEGV",
      app_version: "0.99.116",
      platform: "pi64",
      load_base: "0x0",
      backtrace: ["0x2a00c8"],
    };
    expect(crashFingerprint(unpublished, await resolveBacktrace(bucket, unpublished))).toBe(
      "SIGSEGV/0.99.116/0x2a00c8"
    );
  });
});

// ---------------------------------------------------------------------------
// The dedup search itself.
// ---------------------------------------------------------------------------

describe("findExistingIssue", () => {
  afterEach(() => vi.unstubAllGlobals());

  /** Capture the URL the worker asks GitHub for, and reply with `total_count` hits. */
  function stubSearch(totalCount: number, ok = true): { url: () => string } {
    let seen = "";
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string) => {
        seen = url;
        return {
          ok,
          json: async () => ({
            total_count: totalCount,
            items: [{ number: 1347, html_url: "https://example.invalid/1347" }],
          }),
        } as unknown as Response;
      })
    );
    return { url: () => seen };
  }

  it("searches the labelled fingerprint line, not the bare symbol", async () => {
    const search = stubSearch(1);
    await findExistingIssue("tok", "o", "r", "gcode_viewer_occluder_delete_cb/0.99.116");

    const q = decodeURIComponent(search.url());
    // Without the prefix this matches the backtrace table of any issue that
    // merely passed through the function.
    expect(q).toContain('"Fingerprint: gcode_viewer_occluder_delete_cb/0.99.116"');
    expect(q).toContain("is:open");
    expect(q).toContain("label:crash");
  });

  it("returns the matching issue, and null when nothing matches", async () => {
    stubSearch(1);
    expect(
      (await findExistingIssue("tok", "o", "r", "sym/0.99.116"))?.number
    ).toBe(1347);

    vi.unstubAllGlobals();
    stubSearch(0);
    expect(await findExistingIssue("tok", "o", "r", "sym/0.99.116")).toBeNull();
  });

  it("returns null rather than throwing when the search API errors", async () => {
    stubSearch(1, false);
    expect(await findExistingIssue("tok", "o", "r", "sym/0.99.116")).toBeNull();
  });
});
