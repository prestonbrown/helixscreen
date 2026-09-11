// SPDX-License-Identifier: GPL-3.0-or-later
//
// Server-side symbol resolution for crash backtraces.
// Fetches zstd-compressed .sym maps (nm -nC output) from R2 and resolves raw
// hex addresses to function names + offsets.

import { decompress as zstdDecompress } from "fzstd";

/** A single symbol from nm -nC output. */
export interface Symbol {
  address: number;
  type: string;
  name: string;
}

/** A resolved backtrace frame. */
export interface ResolvedFrame {
  raw: string;
  fileAddr?: string;
  symbol?: string;
}

/** Result of resolving a full backtrace. */
export interface ResolvedBacktrace {
  frames: ResolvedFrame[];
  resolvedRegisters?: Record<string, string>;
  loadBase?: string;
  autoDetectedBase: boolean;
  symbolFileFound: boolean;
  stackScan?: { offset: number; raw: string; fileAddr: string; symbol: string }[];
}

/** Cached heap snapshot captured on the device just before the crash. */
export interface HeapSnapshot {
  age_ms?: number;
  rss_kb?: number;
  vsz_kb?: number;
  arena_kb?: number;
  used_kb?: number;
  free_kb?: number;
  mmap_kb?: number;
  lv_total_kb?: number;
  lv_used_pct?: number;
  lv_frag_pct?: number;
  lv_free_biggest_kb?: number;
}

/** Crash report fields used by the symbol resolver. */
export interface CrashReport {
  app_version?: string;
  platform?: string;
  app_platform?: string;
  load_base?: string;
  backtrace?: string[];
  registers?: Record<string, string>;
  signal?: number;
  signal_name?: string;
  fault_code_name?: string;
  fault_addr?: string;
  timestamp?: string;
  uptime_seconds?: number;
  ram_mb?: number;
  cpu_cores?: number;
  printer_model?: string;
  klipper_version?: string;
  display_backend?: string;
  log_tail?: string[];
  stack_base?: string;
  stack_dump?: string[];
  extra_registers?: Record<string, string>;
  memory_map?: string[];
  queue_callback?: string;

  // Richer context the client already captures but the worker historically ignored.
  breadcrumbs?: string[];
  heap?: HeapSnapshot;
  event_target?: string;
  event_original_target?: string;
  event_code?: number;
  exception?: string;
  bt_source?: string;
  text_start?: string;
  text_end?: string;

  // Share code of a debug bundle uploaded alongside this crash report.
  debug_bundle_share_code?: string;
}

/**
 * Parse `nm -nC` output into a sorted array of symbols.
 * Only includes text/code symbols (T/t/W/w types).
 */
export function parseSymbolTable(text: string): Symbol[] {
  // LTO compilation-unit section markers (e.g., "foo.c.35cb4f60", "bar.cpp.a1b2c3d4")
  // These appear in nm output from LTO builds but are NOT real functions.
  const LTO_PATTERN = /\.\w+\.[0-9a-f]{6,}$/;

  const symbols: Symbol[] = [];
  for (const line of text.split("\n")) {
    if (!line.trim()) continue;

    const match = line.match(/^([0-9a-fA-F]+)\s+([A-Za-z])\s+(.+)$/);
    if (!match) continue;

    const [, addrHex, type, name] = match;

    if (type !== "T" && type !== "t" && type !== "W" && type !== "w") continue;

    // Filter LTO compilation-unit section markers
    if (LTO_PATTERN.test(name.trim())) continue;

    symbols.push({
      address: parseInt(addrHex, 16),
      type,
      name: name.trim(),
    });
  }

  return symbols;
}

/**
 * Binary search for the largest symbol address <= target.
 */
export function lookupSymbol(symbols: Symbol[], address: number): Symbol | null {
  if (symbols.length === 0) return null;

  let lo = 0;
  let hi = symbols.length - 1;

  // Address is before first symbol
  if (address < symbols[0].address) return null;

  while (lo <= hi) {
    const mid = (lo + hi) >>> 1;
    if (symbols[mid].address <= address) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }

  // hi is now the index of the largest symbol address <= target
  return symbols[hi];
}

/**
 * Auto-detect ASLR load base by matching _start/main gap in backtrace.
 * Port of auto_detect_load_base() from scripts/resolve-backtrace.sh.
 */
export function autoDetectLoadBase(symbols: Symbol[], backtraceAddrs: string[]): number | null {
  // Find _start and main in symbol table
  const startSym = symbols.find((s) => s.name === "_start");
  const mainSym = symbols.find((s) => s.name === "main");

  if (!startSym || !mainSym) return null;

  const expectedGap = startSym.address - mainSym.address;

  // Parse backtrace addresses to numbers
  const addrs = backtraceAddrs.map((a) => parseInt(a.replace(/^0x/i, ""), 16));

  // Try each pair: look for the _start/main gap
  for (let i = 0; i < addrs.length; i++) {
    for (let j = i + 1; j < addrs.length; j++) {
      const gap = addrs[j] - addrs[i];

      // Check if this pair matches main→_start gap
      if (gap === expectedGap) {
        // addrs[i] = main, addrs[j] = _start
        const candidateBase = addrs[i] - mainSym.address;
        if (candidateBase > 0) return candidateBase;
      }

      // Check reverse: addrs[i] = _start, addrs[j] = main
      const negGap = addrs[i] - addrs[j];
      if (negGap === expectedGap) {
        const candidateBase = addrs[j] - mainSym.address;
        if (candidateBase > 0) return candidateBase;
      }
    }
  }

  // Fallback: try matching individual addresses to _start or main
  for (const addr of addrs) {
    // Try as _start
    const baseFromStart = addr - startSym.address;
    if (baseFromStart > 0) {
      const mainRuntime = baseFromStart + mainSym.address;
      if (addrs.includes(mainRuntime)) return baseFromStart;
    }

    // Try as main
    const baseFromMain = addr - mainSym.address;
    if (baseFromMain > 0) {
      const startRuntime = baseFromMain + startSym.address;
      if (addrs.includes(startRuntime)) return baseFromMain;
    }
  }

  return null;
}

/**
 * Parse a hex address string to a number.
 */
function parseHexAddr(addr: string): number {
  return parseInt(addr.replace(/^0x/i, ""), 16);
}

/**
 * Linker/runtime boundary symbols that are NOT real functions.
 * Resolving to these means the address wasn't in any real function
 * (e.g., it's in a shared library, not in the main binary).
 */
const GARBAGE_SYMBOLS = new Set([
  "data_start", "_edata", "_end", "__bss_start", "__bss_start__",
  "__bss_end__", "__data_start", "__dso_handle", "__libc_csu_init",
  "__libc_csu_fini", "_fini", "_init", "_fp_hw", "_IO_stdin_used",
  "__init_array_start", "__init_array_end", "__fini_array_start",
  "__fini_array_end", "__FRAME_END__", "__GNU_EH_FRAME_HDR",
  "__TMC_END__", "__ehdr_start", "__exidx_start", "__exidx_end",
  "_GLOBAL_OFFSET_TABLE_", "_DYNAMIC", "_PROCEDURE_LINKAGE_TABLE_",
  "completed.0",
]);

/** Maximum plausible offset from a symbol to still count as "within that function".
 *  Anything larger means the address is in a different region (e.g., shared library). */
const MAX_SYMBOL_OFFSET = 0x100000; // 1MB

/**
 * Check whether a file-relative address falls outside the main binary's
 * symbol range, indicating it belongs to a shared library (libc, etc.).
 * Returns true for negative offsets (address below load_base) or addresses
 * beyond the last known symbol.
 */
export function isSharedLibAddr(symbols: Symbol[], fileAddr: number): boolean {
  if (symbols.length === 0) return true;
  // Negative file offset means the runtime address was below load_base
  if (fileAddr < 0) return true;
  // Beyond the last symbol in the binary (with a generous margin)
  const lastSymAddr = symbols[symbols.length - 1].address;
  if (fileAddr > lastSymAddr + MAX_SYMBOL_OFFSET) return true;
  return false;
}

/**
 * Whether a symbol table already carries runtime addresses rather than
 * file-relative ones, so nothing may be subtracted from a crash address.
 *
 * A PIE image is linked at zero: its symbols sit near zero and a frame's
 * runtime address is symbol + load base. A static non-PIE image is linked at a
 * fixed base (0x400000 on the MIPS and ARM device builds) and its symbols are
 * runtime addresses already. The reported load base cannot separate the two on
 * its own, because dl_iterate_phdr returns the mapping address rather than a
 * bias for a static image, so a non-PIE build reports its own link address.
 * The table separates them: one whose lowest text symbol sits at or above the
 * load base is linked at that base.
 *
 * Takes the table in address order, which lookupSymbol's binary search
 * requires and the published `nm -n` maps satisfy.
 */
export function symbolsAreAbsolute(symbols: Symbol[], loadBase: number): boolean {
  if (loadBase <= 0 || symbols.length === 0) return false;
  return symbols[0].address >= loadBase;
}

/**
 * Resolve a single address against the symbol table.
 */
function resolveAddr(symbols: Symbol[], addr: number): string | null {
  const sym = lookupSymbol(symbols, addr);
  if (!sym) return null;
  if (GARBAGE_SYMBOLS.has(sym.name)) return null;
  const offset = addr - sym.address;
  // If the offset is unreasonably large, the address is likely in a shared
  // library, not in the matched function. Return null instead of a bogus match.
  if (offset > MAX_SYMBOL_OFFSET) return null;
  return `${sym.name}+0x${offset.toString(16)}`;
}

/**
 * Scan raw stack words for addresses that fall within the binary's .text range.
 * Returns resolved symbols for likely return addresses found on the stack.
 */
export function scanStackForReturnAddresses(
  symbols: Symbol[],
  stackDump: string[],
  stackBase: string,
  loadBase: number
): { offset: number; raw: string; fileAddr: string; symbol: string }[] {
  if (symbols.length === 0 || stackDump.length === 0) return [];

  const results: { offset: number; raw: string; fileAddr: string; symbol: string }[] = [];

  for (let i = 0; i < stackDump.length; i++) {
    const raw = stackDump[i];
    const runtimeAddr = parseHexAddr(raw);
    const fileAddr = loadBase > 0 ? runtimeAddr - loadBase : runtimeAddr;

    // Skip addresses outside the binary
    if (isSharedLibAddr(symbols, fileAddr)) continue;

    const resolved = resolveAddr(symbols, fileAddr);
    if (!resolved) continue;

    results.push({
      offset: i * 4, // ARM32: 4 bytes per word
      raw,
      fileAddr: `0x${fileAddr.toString(16)}`,
      symbol: resolved,
    });
  }

  return results;
}

/**
 * Resolve a crash backtrace using symbol files from R2.
 * Never throws — returns gracefully degraded results on any error.
 */
export async function resolveBacktrace(bucket: R2Bucket, report: CrashReport): Promise<ResolvedBacktrace> {
  const result: ResolvedBacktrace = {
    frames: [],
    resolvedRegisters: undefined,
    loadBase: undefined,
    autoDetectedBase: false,
    symbolFileFound: false,
  };

  try {
    // Determine version and platform
    const version = report.app_version;
    const platform = report.platform || report.app_platform;
    if (!version || !platform) return result;

    // Fetch symbol file from R2. Maps are published zstd-compressed (.sym.zst —
    // nm output compresses ~25:1). Workers have no zstd in DecompressionStream,
    // so decode it in-process with a pure-JS decompressor. Fall back to an
    // uncompressed .sym if one happens to exist (older/backfilled uploads).
    const symBase = `symbols/v${version}/${platform}`;
    let symText: string | null = null;

    const zstObj = await bucket.get(`${symBase}.sym.zst`);
    if (zstObj) {
      const compressed = new Uint8Array(await zstObj.arrayBuffer());
      symText = new TextDecoder().decode(zstdDecompress(compressed));
    } else {
      const rawObj = await bucket.get(`${symBase}.sym`);
      if (rawObj) symText = await rawObj.text();
    }
    if (symText === null) return result;

    result.symbolFileFound = true;

    const symbols = parseSymbolTable(symText);
    if (symbols.length === 0) return result;

    // Determine load base
    let loadBase = 0;
    let autoDetected = false;
    // Track whether load_base was explicitly provided (even as "0x0")
    const hasExplicitLoadBase = !!report.load_base;

    if (hasExplicitLoadBase) {
      loadBase = parseHexAddr(report.load_base!);
    }

    // Only try auto-detection if load_base was NOT provided at all.
    // When load_base is explicitly "0x0", that means the crash handler detected it
    // as 0 (non-PIE or static-PIE) — don't override with auto-detection.
    if (!hasExplicitLoadBase && report.backtrace && report.backtrace.length > 0) {
      const detected = autoDetectLoadBase(symbols, report.backtrace);
      if (detected !== null && detected > 0) {
        loadBase = detected;
        autoDetected = true;
      }
    }

    if (loadBase > 0) {
      result.loadBase = `0x${loadBase.toString(16)}`;
      result.autoDetectedBase = autoDetected;
    }

    // An absolute-linked table is indexed by runtime address, so subtracting
    // the reported base would shift every frame onto an unrelated function.
    const effectiveBase = symbolsAreAbsolute(symbols, loadBase) ? 0 : loadBase;

    // Resolve backtrace frames
    if (report.backtrace && report.backtrace.length > 0) {
      result.frames = report.backtrace.map((raw) => {
        const frame: ResolvedFrame = { raw };
        try {
          const runtimeAddr = parseHexAddr(raw);
          const fileAddr = effectiveBase > 0 ? runtimeAddr - effectiveBase : runtimeAddr;
          frame.fileAddr = `0x${fileAddr.toString(16)}`;
          // Detect shared library addresses before attempting resolution
          if (isSharedLibAddr(symbols, fileAddr)) {
            frame.symbol = "<shared library>";
          } else {
            const resolved = resolveAddr(symbols, fileAddr);
            if (resolved) frame.symbol = resolved;
          }
        } catch {
          // Skip unresolvable frames
        }
        return frame;
      });
    }

    // Resolve registers (PC and LR are most useful)
    if (report.registers) {
      const resolved: Record<string, string> = {};
      for (const [reg, val] of Object.entries(report.registers)) {
        if (!val) continue;
        try {
          const runtimeAddr = parseHexAddr(val);
          const fileAddr = effectiveBase > 0 ? runtimeAddr - effectiveBase : runtimeAddr;
          const sym = resolveAddr(symbols, fileAddr);
          if (sym) resolved[reg] = sym;
        } catch {
          // Skip unresolvable registers
        }
      }
      if (Object.keys(resolved).length > 0) {
        result.resolvedRegisters = resolved;
      }
    }
    // Scan stack dump for return addresses (ARM32/MIPS where backtrace() fails)
    if (report.stack_dump && report.stack_dump.length > 0 && report.stack_base) {
      result.stackScan = scanStackForReturnAddresses(
        symbols, report.stack_dump, report.stack_base, effectiveBase
      );
    }
  } catch {
    // Never throw — return whatever we have so far
  }

  return result;
}
