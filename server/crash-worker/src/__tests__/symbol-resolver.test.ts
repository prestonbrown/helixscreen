// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for server-side symbol resolution.

import { describe, it, expect } from "vitest";
import {
  parseSymbolTable,
  lookupSymbol,
  autoDetectLoadBase,
  resolveBacktrace,
  isSharedLibAddr,
  scanStackForReturnAddresses,
  symbolsAreAbsolute,
} from "../symbol-resolver";

// ---------- Sample nm -nC output ----------

const SAMPLE_NM_OUTPUT = `00010000 T _start
00010100 T _init
00020000 T main
00020200 T Application::run()
00020500 t Application::handle_signal(int)
00030000 W std::vector<int>::push_back(int const&)
00040000 D global_data
00050000 B bss_section
00060000 T PrinterState::update()
00060200 T PrinterState::connect()
`;

// A static non-PIE device build (k1, ad5m, ad5x, cc1, k2, snapmaker-u1) is
// linked at a fixed base, so its nm addresses are already runtime addresses.
// The device still reports a load_base: dl_iterate_phdr hands back the mapping
// address rather than a bias for a static image, so the report carries the
// link address itself. Addresses here are the real v1.0.0 k1 layout.
const NONPIE_NM_OUTPUT = `00410228 T _init
00410260 T _ftext
008c9038 T helix::SettingsManager::init_subjects()
009bc134 t std::_Function_handler<void (), FanControlOverlay::on_activate()>::_M_invoke(std::_Any_data const&)
009bdcb4 T helix::ui::ExcludeObjectSideList::populate_rows()
00cc9470 T helix::ui::WizardWifiStep::update_wifi_ip(char const*)
00dbc0d8 T lv_obj_get_child_count
00dbdc40 T lv_obj_find_by_name
0127ee4c T _fini
`;

// Values may be a string (served via .text(), for uncompressed .sym keys) or a
// Uint8Array (served via .arrayBuffer(), for compressed .sym.zst keys).
function createMockBucket(files: Record<string, string | Uint8Array> = {}): R2Bucket {
  return {
    async get(key: string) {
      const content = files[key];
      if (content == null) return null;
      if (typeof content === "string") {
        return { text: async () => content };
      }
      return {
        arrayBuffer: async () =>
          content.buffer.slice(content.byteOffset, content.byteOffset + content.byteLength),
      };
    },
  } as unknown as R2Bucket;
}

// ---------- parseSymbolTable ----------

describe("parseSymbolTable", () => {
  it("parses standard nm output into sorted symbol array", () => {
    const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);
    expect(symbols.length).toBe(8); // T, t, W only (not D, B)
    expect(symbols[0]).toEqual({ address: 0x10000, type: "T", name: "_start" });
    expect(symbols[1]).toEqual({ address: 0x10100, type: "T", name: "_init" });
    expect(symbols[2]).toEqual({ address: 0x20000, type: "T", name: "main" });
  });

  it("filters out non-text symbol types (D, B, etc.)", () => {
    const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);
    const types = symbols.map((s) => s.type);
    expect(types).not.toContain("D");
    expect(types).not.toContain("B");
    expect(types).toContain("T");
    expect(types).toContain("t");
    expect(types).toContain("W");
  });

  it("handles demangled names with spaces", () => {
    const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);
    const pushBack = symbols.find((s) => s.name.includes("push_back"));
    expect(pushBack).toBeDefined();
    expect(pushBack!.name).toBe("std::vector<int>::push_back(int const&)");
  });

  it("skips malformed lines", () => {
    const input = `not a valid line
00010000 T _start
another bad line

00020000 T main`;
    const symbols = parseSymbolTable(input);
    expect(symbols.length).toBe(2);
    expect(symbols[0].name).toBe("_start");
    expect(symbols[1].name).toBe("main");
  });

  it("handles empty input", () => {
    expect(parseSymbolTable("")).toEqual([]);
    expect(parseSymbolTable("\n\n\n")).toEqual([]);
  });

  it("filters LTO compilation-unit section markers", () => {
    const input = `00010000 T _start
00020000 T main
1bdf2f2c t lv_i18n_translations.c.35cb4f60
00030000 T real_function
004dd6c0 t tvgSwRle.cpp.a1b2c3d4`;
    const symbols = parseSymbolTable(input);
    const names = symbols.map((s) => s.name);
    expect(names).not.toContain("lv_i18n_translations.c.35cb4f60");
    expect(names).not.toContain("tvgSwRle.cpp.a1b2c3d4");
    expect(names).toContain("_start");
    expect(names).toContain("main");
    expect(names).toContain("real_function");
  });
});

// ---------- lookupSymbol ----------

describe("lookupSymbol", () => {
  const symbols = [
    { address: 0x1000, type: "T", name: "func_a" },
    { address: 0x2000, type: "T", name: "func_b" },
    { address: 0x3000, type: "T", name: "func_c" },
    { address: 0x4000, type: "T", name: "func_d" },
  ];

  it("finds exact match", () => {
    const result = lookupSymbol(symbols, 0x2000);
    expect(result!.name).toBe("func_b");
  });

  it("finds symbol when address is between two symbols", () => {
    const result = lookupSymbol(symbols, 0x2500);
    expect(result!.name).toBe("func_b");
  });

  it("returns null when address is before first symbol", () => {
    const result = lookupSymbol(symbols, 0x500);
    expect(result).toBeNull();
  });

  it("finds last symbol when address is after all symbols", () => {
    const result = lookupSymbol(symbols, 0x5000);
    expect(result!.name).toBe("func_d");
  });

  it("returns null for empty array", () => {
    expect(lookupSymbol([], 0x1000)).toBeNull();
  });

  it("handles single-element array", () => {
    const single = [{ address: 0x1000, type: "T", name: "only" }];
    expect(lookupSymbol(single, 0x1000)!.name).toBe("only");
    expect(lookupSymbol(single, 0x1500)!.name).toBe("only");
    expect(lookupSymbol(single, 0x500)).toBeNull();
  });
});

// ---------- autoDetectLoadBase ----------

describe("autoDetectLoadBase", () => {
  const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);

  it("detects load base from _start/main gap in backtrace", () => {
    // _start = 0x10000, main = 0x20000 in file
    // Simulate ASLR: load_base = 0xAAAA0000
    const loadBase = 0xaaaa0000;
    const backtraceAddrs = [
      `0x${(loadBase + 0x20200).toString(16)}`, // Application::run
      `0x${(loadBase + 0x20000).toString(16)}`, // main
      `0x${(loadBase + 0x10000).toString(16)}`, // _start
    ];

    const detected = autoDetectLoadBase(symbols, backtraceAddrs);
    expect(detected).toBe(loadBase);
  });

  it("returns null when _start is not in symbol table", () => {
    const noStart = symbols.filter((s) => s.name !== "_start");
    const result = autoDetectLoadBase(noStart, ["0x100000", "0x200000"]);
    expect(result).toBeNull();
  });

  it("returns null when main is not in symbol table", () => {
    const noMain = symbols.filter((s) => s.name !== "main");
    const result = autoDetectLoadBase(noMain, ["0x100000", "0x200000"]);
    expect(result).toBeNull();
  });

  it("returns null when backtrace has no matching gap", () => {
    const result = autoDetectLoadBase(symbols, [
      "0xDEAD0001",
      "0xDEAD0002",
      "0xDEAD0003",
    ]);
    expect(result).toBeNull();
  });

  it("returns null for empty backtrace", () => {
    expect(autoDetectLoadBase(symbols, [])).toBeNull();
  });
});

// ---------- isSharedLibAddr ----------

describe("isSharedLibAddr", () => {
  const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);

  it("returns true for negative file offsets", () => {
    // Negative offset means address was below load_base (shared lib region)
    expect(isSharedLibAddr(symbols, -0x1000)).toBe(true);
  });

  it("returns true for addresses far beyond the last symbol", () => {
    // 0x7fff80012345 is way beyond any symbol in our binary
    expect(isSharedLibAddr(symbols, 0x7fff80012345)).toBe(true);
  });

  it("returns false for addresses within the binary range", () => {
    expect(isSharedLibAddr(symbols, 0x20200)).toBe(false); // Application::run
    expect(isSharedLibAddr(symbols, 0x10000)).toBe(false); // _start
  });

  it("returns false for addresses slightly past last symbol", () => {
    // Last symbol is PrinterState::connect() at 0x60200
    // A small offset past it should still be considered in-binary
    expect(isSharedLibAddr(symbols, 0x60300)).toBe(false);
  });

  it("returns true for empty symbol table", () => {
    expect(isSharedLibAddr([], 0x1000)).toBe(true);
  });
});

// ---------- resolveBacktrace ----------

describe("resolveBacktrace", () => {
  const symFileContent = SAMPLE_NM_OUTPUT;

  // zstd -19 of SAMPLE_NM_OUTPUT (base64) — the release pipeline publishes maps
  // only as .sym.zst, so this is the format the worker actually fetches. The
  // zstd test below decodes this and asserts the expected symbols resolve; if
  // SAMPLE_NM_OUTPUT's addresses change, regenerate with:
  //   zstd -19 -c sample.txt | base64 -w0
  const SAMPLE_NM_ZSTD_B64 =
    "KLUv/WQiAH0FAEIKIBhg1wOLeRgdJGRS9KbUB/CfP5gvn4aBYQ7AILyc7lUWgzCfqUyDcD4/ZOd72yfx2i/njfdOrkH4mYqUp0or5xzBX6/SXZKnaF0tg3B5b8XWl3ODkPrrJOO9Bcpe9Y2TUs7WI4PgmoXQaz/LplnGFN6MFe1NAG/5b3SRe+894r0XFCDAQtwNklBsdM+82tussr4w6+1emXO32yVs+QRMMBZXjEFGeUmb8aqWhgETRGlc";
  const symFileZstd = Uint8Array.from(atob(SAMPLE_NM_ZSTD_B64), (c) => c.charCodeAt(0));

  it("resolves from a zstd-compressed .sym.zst (the published format)", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym.zst": symFileZstd,
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020200", "0x00020000", "0x00010000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
    expect(result.frames[1].symbol).toBe("main+0x0");
    expect(result.frames[2].symbol).toBe("_start+0x0");
  });

  it("prefers .sym.zst over an uncompressed .sym when both exist", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym.zst": symFileZstd,
      "symbols/v0.9.9/pi.sym": "00099999 T should_not_be_used\n",
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.frames[0].symbol).toBe("main+0x0");
  });

  it("resolves backtrace with explicit load_base", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020200", "0x00020000", "0x00010000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames).toHaveLength(3);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
    expect(result.frames[1].symbol).toBe("main+0x0");
    expect(result.frames[2].symbol).toBe("_start+0x0");
  });

  it("resolves backtrace with non-zero load_base", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const loadBase = 0xaaaa0000;
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: `0x${loadBase.toString(16)}`,
      backtrace: [`0x${(loadBase + 0x20200).toString(16)}`],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.loadBase).toBe(`0x${loadBase.toString(16)}`);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
  });

  it("auto-detects load_base when not provided", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const loadBase = 0xbbbb0000;
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      backtrace: [
        `0x${(loadBase + 0x20200).toString(16)}`, // Application::run
        `0x${(loadBase + 0x20000).toString(16)}`, // main
        `0x${(loadBase + 0x10000).toString(16)}`, // _start
      ],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.autoDetectedBase).toBe(true);
    expect(result.loadBase).toBe(`0x${loadBase.toString(16)}`);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
  });

  it("returns symbolFileFound: false when sym file missing", async () => {
    const bucket = createMockBucket({}); // empty bucket

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      backtrace: ["0x00020200"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
    expect(result.frames).toEqual([]);
  });

  it("resolves registers", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020200"],
      registers: {
        pc: "0x00060000",
        lr: "0x00020200",
        sp: "0x7ffff000",
      },
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.resolvedRegisters).toBeDefined();
    expect(result.resolvedRegisters!.pc).toBe("PrinterState::update()+0x0");
    expect(result.resolvedRegisters!.lr).toBe("Application::run()+0x0");
    // SP (0x7ffff000) is way past all code symbols — the resolver correctly
    // filters it out as an implausibly large offset (shared lib / stack space)
    expect(result.resolvedRegisters!.sp).toBeUndefined();
  });

  it("handles missing version gracefully", async () => {
    const bucket = createMockBucket({});
    const report = { platform: "pi", backtrace: ["0x1234"] };
    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
  });

  it("handles missing platform gracefully", async () => {
    const bucket = createMockBucket({});
    const report = { app_version: "0.9.9", backtrace: ["0x1234"] };
    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
  });

  it("uses app_platform fallback field", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      app_platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames[0].symbol).toBe("main+0x0");
  });

  it("handles address with offset correctly", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020123"], // main+0x123
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.frames[0].symbol).toBe("main+0x123");
  });

  it("never throws even with bad bucket", async () => {
    const bucket = {
      get: async () => {
        throw new Error("R2 exploded");
      },
    } as unknown as R2Bucket;

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      backtrace: ["0x1234"],
    };

    // Should not throw
    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
  });

  it("labels shared library addresses instead of producing garbage symbols", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    // Simulate a real crash: some frames in our binary, some in libc.
    // load_base = 0xaaaab000, libc address = 0x7fff80012345 (way outside binary).
    const loadBase = 0xaaaab000;
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: `0x${loadBase.toString(16)}`,
      backtrace: [
        `0x${(loadBase + 0x20200).toString(16)}`, // Application::run (in binary)
        "0x7fff80012345", // libc address (shared library)
        `0x${(loadBase + 0x20000).toString(16)}`, // main (in binary)
        "0x7fff80098765", // another libc address
      ],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames).toHaveLength(4);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
    expect(result.frames[1].symbol).toBe("<shared library>");
    expect(result.frames[2].symbol).toBe("main+0x0");
    expect(result.frames[3].symbol).toBe("<shared library>");
  });

  it("treats load_base '0x0' as file-relative addresses without auto-detect override", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    // Simulate static-PIE ARM32: backtrace() returns file-relative addresses,
    // load_base explicitly reported as 0x0
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020200", "0x00020000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    // Should NOT try auto-detection — load_base was explicitly provided
    expect(result.autoDetectedBase).toBe(false);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
    expect(result.frames[1].symbol).toBe("main+0x0");
  });
});

// ---------- Absolute-linked (non-PIE) symbol tables ----------

describe("absolute-linked symbol tables", () => {
  it("treats a table whose lowest symbol is at or above the load base as absolute", () => {
    const symbols = parseSymbolTable(NONPIE_NM_OUTPUT);
    expect(symbolsAreAbsolute(symbols, 0x400000)).toBe(true);
  });

  it("treats a table linked at zero as file-relative", () => {
    const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);
    expect(symbolsAreAbsolute(symbols, 0xaaaa0000)).toBe(false);
  });

  it("reports file-relative when there is no load base to compare against", () => {
    const symbols = parseSymbolTable(NONPIE_NM_OUTPUT);
    expect(symbolsAreAbsolute(symbols, 0)).toBe(false);
  });

  it("resolves a non-PIE backtrace without shifting it by the load base", async () => {
    const bucket = createMockBucket({
      "symbols/v1.0.0/k1.sym": NONPIE_NM_OUTPUT,
    });

    const report = {
      app_version: "1.0.0",
      platform: "k1",
      load_base: "0x400000",
      text_start: "0x400000",
      text_end: "0x127ee68",
      backtrace: ["0xdbc168", "0xdbdce0", "0xcc97d8"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames[0].symbol).toBe("lv_obj_get_child_count+0x90");
    expect(result.frames[1].symbol).toBe("lv_obj_find_by_name+0xa0");
    expect(result.frames[2].symbol).toBe(
      "helix::ui::WizardWifiStep::update_wifi_ip(char const*)+0x368"
    );
  });

  it("resolves non-PIE registers against the same unshifted base", async () => {
    const bucket = createMockBucket({
      "symbols/v1.0.0/k1.sym": NONPIE_NM_OUTPUT,
    });

    const report = {
      app_version: "1.0.0",
      platform: "k1",
      load_base: "0x400000",
      backtrace: ["0xdbc168"],
      registers: { pc: "0xdbc168", ra: "0xdbdce0" },
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.resolvedRegisters?.pc).toBe("lv_obj_get_child_count+0x90");
    expect(result.resolvedRegisters?.ra).toBe("lv_obj_find_by_name+0xa0");
  });

  it("resolves a non-PIE stack scan against the same unshifted base", async () => {
    const bucket = createMockBucket({
      "symbols/v1.0.0/k1.sym": NONPIE_NM_OUTPUT,
    });

    const report = {
      app_version: "1.0.0",
      platform: "k1",
      load_base: "0x400000",
      backtrace: ["0xdbc168"],
      stack_base: "0x7f845ec0",
      stack_dump: ["0x00000001", "0xcc97d8"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.stackScan).toHaveLength(1);
    expect(result.stackScan![0].symbol).toBe(
      "helix::ui::WizardWifiStep::update_wifi_ip(char const*)+0x368"
    );
  });
});

// ---------- scanStackForReturnAddresses ----------

describe("scanStackForReturnAddresses", () => {
  it("finds return addresses in stack dump within text range", () => {
    const symText = [
      "00010000 T _start",
      "00020000 T main",
      "00030000 T some_function",
      "00040000 T another_function",
      "00050000 T _fini",
    ].join("\n");

    const symbols = parseSymbolTable(symText);
    const stackDump = [
      "0x00025678", // in main
      "0xb1c6b0a0", // NOT in binary
      "0x00035abc", // in some_function
      "0x00000000", // null
      "0x00045def", // in another_function
    ];

    const results = scanStackForReturnAddresses(symbols, stackDump, "0xb1c6b070", 0);

    expect(results.length).toBe(3);
    expect(results[0].symbol).toContain("main+");
    expect(results[0].offset).toBe(0);
    expect(results[1].symbol).toContain("some_function+");
    expect(results[1].offset).toBe(8);
    expect(results[2].symbol).toContain("another_function+");
  });

  it("returns empty array when no stack dump provided", () => {
    const symbols = parseSymbolTable("00010000 T _start\n00050000 T _fini");
    const results = scanStackForReturnAddresses(symbols, [], "0x0", 0);
    expect(results).toEqual([]);
  });

  it("applies load_base offset to stack addresses", () => {
    const symText = "00010000 T _start\n00020000 T main\n00050000 T _fini";
    const symbols = parseSymbolTable(symText);
    const loadBase = 0xb0a00000;
    const stackDump = [`0x${(loadBase + 0x20123).toString(16)}`];

    const results = scanStackForReturnAddresses(symbols, stackDump, "0xb1c6b070", loadBase);

    expect(results.length).toBe(1);
    expect(results[0].symbol).toContain("main+0x123");
  });
});
