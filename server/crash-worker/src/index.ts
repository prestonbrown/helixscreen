// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cloudflare Worker: helix-crash-worker
// Receives crash reports from HelixScreen devices and creates GitHub issues.
//
// Endpoints:
//   GET  /                      - Health check
//   POST /v1/report             - Submit a crash report (requires X-API-Key)
//   POST /v1/debug-bundle       - Upload debug bundle (requires X-API-Key)
//   GET  /v1/debug-bundle/:code - Retrieve debug bundle (requires X-Admin-Key)
//   GET  /v1/debug-bundle       - List debug bundles (requires X-Admin-Key)
//       ?limit=N                  Max results (1-100, default 20)
//       &since=YYYY-MM-DD         Uploaded on or after this date
//       &until=YYYY-MM-DD         Uploaded on or before this date
//       &match=STRING             Substring match on version/model/platform/code
//       &cursor=TOKEN             R2 pagination cursor from previous response
//
// Secrets (configure via `wrangler secret put`):
//   INGEST_API_KEY          - API key baked into HelixScreen binaries
//   GITHUB_APP_PRIVATE_KEY  - GitHub App private key (PEM)
//   ADMIN_API_KEY           - Admin API key for retrieving debug bundles
//   RESEND_API_KEY          - Resend API key for email notifications

import {
  getInstallationToken,
  crashFingerprint,
  findExistingIssue,
  addDuplicateComment,
  isKnownRelease,
} from "./github-app";
import { resolveBacktrace } from "./symbol-resolver";
import type { CrashReport, ResolvedBacktrace } from "./symbol-resolver";

/** Worker environment bindings. */
interface Env {
  // R2 buckets
  DEBUG_BUNDLES: R2Bucket;
  RELEASES_BUCKET: R2Bucket;

  // Rate limiters
  CRASH_LIMITER: RateLimit;
  DEBUG_BUNDLE_LIMITER: RateLimit;

  // Vars (wrangler.toml [vars])
  GITHUB_REPO: string;
  GITHUB_APP_ID: string;
  NOTIFICATION_EMAIL: string;
  EMAIL_FROM: string;

  // Secrets (wrangler secret put)
  INGEST_API_KEY: string;
  GITHUB_APP_PRIVATE_KEY: string;
  ADMIN_API_KEY: string;
  RESEND_API_KEY: string;
}

/** GitHub issue creation result. */
interface IssueResult {
  number: number;
  html_url: string;
  is_duplicate: boolean;
  /** Set when the report was accepted but deliberately not filed. */
  ignored?: boolean;
}

/** Metadata extracted from a debug bundle's gzipped JSON body. */
interface BundleMetadata {
  version: string;
  printer_model: string;
  klipper_version: string;
  platform: string;
  timestamp: string;
  user_note: string;
}

/** Entry in the bundle listing response. */
interface BundleListEntry {
  share_code: string;
  size: number;
  uploaded: string;
  metadata: {
    version: string;
    printer_model: string;
    platform: string;
    klipper_version: string;
    user_note: string;
  };
}

/** Escape HTML special characters to prevent XSS in email templates. */
function escapeHtml(str: string): string {
  return str
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    // Handle CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, {
        status: 204,
        headers: corsHeaders(),
      });
    }

    const url = new URL(request.url);

    // Health check
    if (url.pathname === "/" && request.method === "GET") {
      return jsonResponse(200, {
        service: "helix-crash-worker",
        status: "ok",
        timestamp: new Date().toISOString(),
      });
    }

    // Crash report ingestion
    if (url.pathname === "/v1/report" && request.method === "POST") {
      return handleCrashReport(request, env);
    }

    // Debug bundle upload
    if (url.pathname === "/v1/debug-bundle" && request.method === "POST") {
      return handleDebugBundleUpload(request, env);
    }

    // Debug bundle listing (must check before retrieval since /v1/debug-bundle matches both)
    if (url.pathname === "/v1/debug-bundle" && request.method === "GET") {
      return handleDebugBundleList(request, env, url);
    }

    // Debug bundle retrieval
    if (url.pathname.startsWith("/v1/debug-bundle/") && request.method === "GET") {
      return handleDebugBundleRetrieve(request, env, url);
    }

    return jsonResponse(404, { error: "Not found" });
  },
} satisfies ExportedHandler<Env>;

/**
 * Handle an incoming crash report.
 * Validates the API key and payload, then creates a GitHub issue.
 */
async function handleCrashReport(request: Request, env: Env): Promise<Response> {
  // --- Authentication ---
  const apiKey = request.headers.get("X-API-Key");
  if (!apiKey || apiKey !== env.INGEST_API_KEY) {
    return jsonResponse(401, { error: "Unauthorized: invalid or missing API key" });
  }

  // --- Rate limiting (per client IP) ---
  const clientIP = request.headers.get("CF-Connecting-IP") || "unknown";
  const { success } = await env.CRASH_LIMITER.limit({ key: clientIP });
  if (!success) {
    return jsonResponse(429, { error: "Rate limit exceeded — try again later" });
  }

  // --- Parse and validate body ---
  let body: CrashReport;
  try {
    body = await request.json();
  } catch {
    return jsonResponse(400, { error: "Invalid JSON body" });
  }

  const missing = validateRequiredFields(body, ["signal", "signal_name", "app_version"]);
  if (missing.length > 0) {
    return jsonResponse(400, {
      error: `Missing required fields: ${missing.join(", ")}`,
    });
  }

  // The version is the R2 symbol-file lookup key and goes straight into the
  // issue title, so anything that isn't semver-shaped is refused outright.
  if (!isVersionShapeValid(String(body.app_version))) {
    return jsonResponse(400, { error: "Invalid app_version" });
  }

  // --- Create GitHub issue (or add comment to existing) ---
  try {
    const issue = await createGitHubIssue(env, body);
    if (issue.ignored) {
      return jsonResponse(202, {
        status: "ignored",
        reason: "app_version does not match a published release",
      });
    }
    return jsonResponse(issue.is_duplicate ? 200 : 201, {
      status: issue.is_duplicate ? "duplicate" : "created",
      issue_number: issue.number,
      issue_url: issue.html_url,
      is_duplicate: issue.is_duplicate || false,
    });
  } catch (err) {
    console.error("Failed to create GitHub issue:", (err as Error).message);
    return jsonResponse(500, { error: "Failed to create GitHub issue" });
  }
}

/**
 * Check that all required fields are present and non-empty.
 * Returns an array of missing field names.
 */
function validateRequiredFields(body: Record<string, unknown>, fields: string[]): string[] {
  return fields.filter(
    (f) => body[f] === undefined || body[f] === null || body[f] === ""
  );
}

/**
 * Semver shape a HelixScreen build can actually stamp (HELIX_VERSION comes from
 * VERSION.txt at compile time). The bounded digit counts also keep a hostile
 * app_version from turning into a 4 kB issue title or an R2 key with newlines.
 */
const VERSION_SHAPE = /^\d{1,4}\.\d{1,4}\.\d{1,5}([-+][0-9A-Za-z.-]{1,32})?$/;

export function isVersionShapeValid(version: string): boolean {
  return VERSION_SHAPE.test(version);
}

/** Trim and escape a client-supplied string destined for the issue title. */
function titleField(value: string): string {
  return mdEscape(value).slice(0, 48);
}

/**
 * Build the issue title. Everything interpolated here comes from the device, so
 * each field is escaped and length-capped; app_version has already passed
 * {@link isVersionShapeValid}.
 */
export function crashIssueTitle(report: CrashReport): string {
  const signal = titleField(report.signal_name || `SIG${report.signal}`);
  const version = titleField(report.app_version || "unknown");

  if (report.fault_code_name && report.fault_addr) {
    const code = titleField(report.fault_code_name);
    const addr = titleField(report.fault_addr);
    return `Crash: ${signal} (${code} at ${addr}) in v${version}`;
  }
  return `Crash: ${signal} in v${version}`;
}

/**
 * Build a markdown issue body from the crash report and create it via GitHub API.
 * Uses GitHub App authentication (issues appear as "HelixScreen Crash Reporter [bot]").
 * Deduplicates by fingerprint — adds a comment to existing issues instead of creating new ones.
 */
async function createGitHubIssue(env: Env, report: CrashReport): Promise<IssueResult> {
  const [owner, repo] = env.GITHUB_REPO.split("/");

  // Get installation token from GitHub App
  const token = await getInstallationToken(
    env.GITHUB_APP_ID,
    env.GITHUB_APP_PRIVATE_KEY,
    owner,
    repo
  );

  // Reports from builds we never published can't be symbolicated — no symbol
  // file for them exists in R2 or on a release, and none ever will (#1240 came
  // from a v0.1.4 binary; VERSION.txt has never held that value). Accept the
  // POST so the device stops retrying, but don't file an issue.
  if (!(await isKnownRelease(token, owner, repo, report.app_version ?? ""))) {
    console.warn(
      `Ignoring crash from unpublished build: v${report.app_version} ` +
        `(${report.signal_name}, platform ${report.platform || "unknown"})`
    );
    return { number: 0, html_url: "", is_duplicate: false, ignored: true };
  }

  const fingerprint = crashFingerprint(report);

  // Check for existing open issue with same fingerprint
  const existing = await findExistingIssue(token, owner, repo, fingerprint);
  if (existing) {
    await addDuplicateComment(token, owner, repo, existing.number, report, fingerprint);
    return { number: existing.number, html_url: existing.html_url, is_duplicate: true };
  }

  // Resolve backtrace symbols from R2 (best-effort, never throws)
  let resolved: ResolvedBacktrace | null = null;
  if (env.RELEASES_BUCKET) {
    try {
      resolved = await resolveBacktrace(env.RELEASES_BUCKET, report);
    } catch (err) {
      console.error("Symbol resolution failed:", (err as Error).message);
    }
  }

  // Includes the fault type when available (e.g., "SEGV_MAPERR at 0x00000000")
  const title = crashIssueTitle(report);

  const body = formatIssueBody(report, fingerprint, resolved);

  const response = await fetch(
    `https://api.github.com/repos/${env.GITHUB_REPO}/issues`,
    {
      method: "POST",
      headers: {
        Authorization: `Bearer ${token}`,
        Accept: "application/vnd.github+json",
        "User-Agent": "HelixScreen-Crash-Reporter",
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        title,
        body,
        labels: ["crash", "auto-reported"],
      }),
    }
  );

  if (!response.ok) {
    const text = await response.text();
    throw new Error(`GitHub API ${response.status}: ${text}`);
  }

  const issue = (await response.json()) as { number: number; html_url: string };
  return { number: issue.number, html_url: issue.html_url, is_duplicate: false };
}

/**
 * Escape characters that break Markdown table cells ({@code |}, newlines).
 */
export function mdEscape(s: string): string {
  return s.replace(/\|/g, "\\|").replace(/[\r\n]+/g, " ");
}

/**
 * Map lv_event_code_t values to their symbolic names so crash issues show
 * "code=42 (DELETE)" instead of a bare integer. Unknown codes fall through to "".
 *
 * The table is GENERATED from lib/lvgl/src/misc/lv_event.h by
 * scripts/gen_lvgl_event_codes.py -- do not edit it by hand. It was
 * hand-maintained until LVGL 9.5 inserted four codes mid-enum; 58 of 63 entries
 * went stale and a DELETE crash was filed as SCREEN_UNLOAD_START, which pointed
 * triage away from the teardown bug the label would have named. quality-checks.sh
 * now fails on drift; `make regen-lvgl-event-codes` repairs it.
 */
export function lvglEventCodeName(code: number | undefined): string {
  if (code == null) return "";
  const names: Record<number, string> = {
  /* BEGIN GENERATED lv_event_code_t (scripts/gen_lvgl_event_codes.py) */
     0: "ALL",
     1: "PRESSED",
     2: "PRESSING",
     3: "PRESS_LOST",
     4: "SHORT_CLICKED",
     5: "SINGLE_CLICKED",
     6: "DOUBLE_CLICKED",
     7: "TRIPLE_CLICKED",
     8: "LONG_PRESSED",
     9: "LONG_PRESSED_REPEAT",
    10: "CLICKED",
    11: "RELEASED",
    12: "SCROLL_BEGIN",
    13: "SCROLL_THROW_BEGIN",
    14: "SCROLL_END",
    15: "SCROLL",
    16: "GESTURE",
    17: "KEY",
    18: "ROTARY",
    19: "FOCUSED",
    20: "DEFOCUSED",
    21: "LEAVE",
    22: "HIT_TEST",
    23: "INDEV_RESET",
    24: "HOVER_OVER",
    25: "HOVER_LEAVE",
    26: "COVER_CHECK",
    27: "REFR_EXT_DRAW_SIZE",
    28: "DRAW_MAIN_BEGIN",
    29: "DRAW_MAIN",
    30: "DRAW_MAIN_END",
    31: "DRAW_POST_BEGIN",
    32: "DRAW_POST",
    33: "DRAW_POST_END",
    34: "DRAW_TASK_ADDED",
    35: "VALUE_CHANGED",
    36: "INSERT",
    37: "REFRESH",
    38: "READY",
    39: "CANCEL",
    40: "STATE_CHANGED",
    41: "CREATE",
    42: "DELETE",
    43: "CHILD_CHANGED",
    44: "CHILD_CREATED",
    45: "CHILD_DELETED",
    46: "SCREEN_UNLOAD_START",
    47: "SCREEN_LOAD_START",
    48: "SCREEN_LOADED",
    49: "SCREEN_UNLOADED",
    50: "SIZE_CHANGED",
    51: "STYLE_CHANGED",
    52: "LAYOUT_CHANGED",
    53: "GET_SELF_SIZE",
    54: "INVALIDATE_AREA",
    55: "RESOLUTION_CHANGED",
    56: "COLOR_FORMAT_CHANGED",
    57: "REFR_REQUEST",
    58: "REFR_START",
    59: "REFR_READY",
    60: "RENDER_START",
    61: "RENDER_READY",
    62: "FLUSH_START",
    63: "FLUSH_FINISH",
    64: "FLUSH_WAIT_START",
    65: "FLUSH_WAIT_FINISH",
    66: "SYNC_START",
    67: "SYNC_FINISH",
    68: "SYNC_WAIT_START",
    69: "SYNC_WAIT_FINISH",
    70: "UPDATE_LAYOUT_COMPLETED",
    71: "VSYNC",
    72: "VSYNC_REQUEST",
    73: "TRANSLATION_LANGUAGE_CHANGED",
  /* END GENERATED lv_event_code_t */
  };
  // The device records e->code raw, and a preprocess dispatch carries
  // LV_EVENT_PREPROCESS (0x8000) in it. Mask it exactly the way
  // lv_event_get_code() does (lib/lvgl/src/misc/lv_event.c:386) so those
  // resolve instead of falling through to "".
  return names[code & ~0x8000] || "";
}

/**
 * Format the crash report into a structured markdown issue body.
 */
export function formatIssueBody(r: CrashReport, fingerprint: string, resolved: ResolvedBacktrace | null): string {
  const timestamp = r.timestamp || new Date().toISOString();
  const uptime = r.uptime_seconds != null ? `${r.uptime_seconds}s` : "unknown";

  let md = `## Crash Summary

| Field | Value |
|-------|-------|
| **Signal** | ${r.signal} (${r.signal_name}) |
| **Version** | ${r.app_version} |
| **Uptime** | ${uptime} |
| **Timestamp** | ${timestamp} |
`;

  // Fault info (Phase 2)
  if (r.fault_code_name && r.fault_addr) {
    md += `| **Fault** | ${mdEscape(r.fault_code_name)} at ${mdEscape(r.fault_addr)} |\n`;
  }

  if (r.exception) {
    md += `| **Exception** | ${mdEscape(r.exception)} |\n`;
  }

  if (r.queue_callback) {
    md += `| **Queue Callback** | \`${mdEscape(r.queue_callback)}\` |\n`;
  }

  // LVGL event under dispatch at crash time (set by event_send_core hook).
  if (r.event_target) {
    const codeName = lvglEventCodeName(r.event_code);
    const codePart = r.event_code != null ? ` code=${r.event_code}${codeName ? ` (${codeName})` : ""}` : "";
    const origPart = r.event_original_target && r.event_original_target !== r.event_target
      ? ` original=\`${mdEscape(r.event_original_target)}\``
      : "";
    md += `| **LVGL Event** | target=\`${mdEscape(r.event_target)}\`${origPart}${codePart} |\n`;
  }

  if (r.debug_bundle_share_code) {
    const code = mdEscape(r.debug_bundle_share_code);
    md += `| **Debug Bundle** | \`${code}\` (use \`./scripts/debug-bundle.sh ${code}\`) |\n`;
  }

  // Heap snapshot (cached on-device every ~10s; tells us memory pressure state at crash time).
  // The device writes glibc fields (arena/used/free) and LVGL fields (lv_*) as groups — if the
  // group's lead key is set, the rest are guaranteed present.
  if (r.heap) {
    const h = r.heap;
    const parts: string[] = [];
    if (h.rss_kb != null) parts.push(`RSS ${h.rss_kb}kB`);
    if (h.arena_kb != null) parts.push(`arena ${h.arena_kb}kB (${h.used_kb} used / ${h.free_kb} free)`);
    if (h.lv_total_kb != null) {
      parts.push(`LVGL ${h.lv_used_pct}% used, ${h.lv_frag_pct}% frag, biggest-free ${h.lv_free_biggest_kb}kB`);
    }
    if (h.age_ms != null) parts.push(`snapshot age ${h.age_ms}ms`);
    if (parts.length > 0) {
      md += `| **Heap** | ${parts.join(" · ")} |\n`;
    }
  }

  // Register state (Phase 2) — with resolved symbols when available
  if (r.registers) {
    md += `\n## Registers

| Register | Value |
|----------|-------|
`;
    const regs = resolved?.resolvedRegisters || {};
    for (const [reg, val] of Object.entries(r.registers)) {
      if (!val) continue;
      const label = reg.toUpperCase();
      const sym = regs[reg];
      if (sym) {
        md += `| **${label}** | \`${val}\` → \`${sym}\` |\n`;
      } else {
        md += `| **${label}** | \`${val}\` |\n`;
      }
    }
  }

  // Extra registers (ARM32: r0-r12, fp, ip)
  if (r.extra_registers && Object.keys(r.extra_registers).length > 0) {
    md += `\n### All Registers\n\n`;
    md += `| Register | Value |\n|----------|-------|\n`;
    for (const [reg, val] of Object.entries(r.extra_registers)) {
      md += `| **${reg}** | \`${val}\` |\n`;
    }
  }

  // System info section (all fields optional)
  if (r.platform || r.display_backend || r.ram_mb || r.cpu_cores || r.printer_model || r.klipper_version) {
    md += `\n## System Info

| Field | Value |
|-------|-------|
`;
    if (r.platform) md += `| **Platform** | ${mdEscape(r.platform)} |\n`;
    if (r.display_backend) md += `| **Display** | ${mdEscape(r.display_backend)} |\n`;
    if (r.ram_mb) md += `| **RAM** | ${r.ram_mb} MB |\n`;
    if (r.cpu_cores) md += `| **CPU** | ${r.cpu_cores} cores |\n`;
    if (r.printer_model) md += `| **Printer** | ${mdEscape(r.printer_model)} |\n`;
    if (r.klipper_version) md += `| **Klipper** | ${mdEscape(r.klipper_version)} |\n`;
  }

  // Backtrace section — with resolved symbols when available
  if (r.backtrace && r.backtrace.length > 0) {
    md += `\n## Backtrace\n\n`;

    if (resolved?.frames?.some((f) => f.symbol)) {
      // Resolved backtrace: show as table
      md += `| # | Address | Symbol |\n|---|---------|--------|\n`;
      for (let i = 0; i < resolved.frames.length; i++) {
        const f = resolved.frames[i];
        const sym = f.symbol ? `\`${f.symbol}\`` : "(unknown)";
        md += `| ${i} | \`${f.raw}\` | ${sym} |\n`;
      }

      // Add metadata about resolution
      const parts: string[] = [];
      if (resolved.loadBase) {
        parts.push(
          resolved.autoDetectedBase
            ? `load_base: ${resolved.loadBase} (auto-detected)`
            : `load_base: ${resolved.loadBase}`
        );
      }
      parts.push(`symbol file: v${r.app_version}/${r.platform || r.app_platform}.sym`);
      if (r.bt_source === "stack_scan") {
        parts.push("frames below crash_signal_handler are stack-scanned, not live (may be stale)");
      }
      md += `\n<sub>${parts.join(" · ")}</sub>\n`;
    } else {
      // Unresolved: raw addresses in code block (original format)
      md += `\`\`\`\n${r.backtrace.join("\n")}\n\`\`\`\n`;
      if (resolved?.symbolFileFound === false) {
        md += `\n<sub>No symbol file found for v${r.app_version}/${r.platform || r.app_platform || "unknown"}</sub>\n`;
      }
    }
  }

  // Stack scan results (ARM32: return addresses found in stack dump)
  if (resolved?.stackScan && resolved.stackScan.length > 0) {
    md += `\n## Stack Scan (likely call chain)\n\n`;
    md += `Addresses found on the stack within the binary's .text range:\n\n`;
    md += `| SP+offset | Address | Symbol |\n|-----------|---------|--------|\n`;
    for (const entry of resolved.stackScan) {
      md += `| SP+0x${entry.offset.toString(16)} | \`${entry.raw}\` | \`${entry.symbol}\` |\n`;
    }
  }

  // Activity breadcrumbs from the in-process ring buffer. The tail is the most
  // recent activity — whichever panel/modal/tick was active just before the crash.
  if (r.breadcrumbs && r.breadcrumbs.length > 0) {
    md += `\n## Breadcrumbs (recent activity)\n\n`;
    md += `\`\`\`\n${r.breadcrumbs.join("\n")}\n\`\`\`\n`;
  }

  // Memory map (collapsed — helps identify what's at crash addresses)
  if (r.memory_map && r.memory_map.length > 0) {
    md += `\n<details>\n<summary>Memory Map (${r.memory_map.length} regions)</summary>\n\n`;
    md += `\`\`\`\n${r.memory_map.join("\n")}\n\`\`\`\n\n</details>\n`;
  }

  // Log tail in a collapsed section
  if (r.log_tail && r.log_tail.length > 0) {
    md += `
<details>
<summary>Log Tail (last ${r.log_tail.length} lines)</summary>

\`\`\`
${r.log_tail.join("\n")}
\`\`\`

</details>
`;
  }

  md += `\n---\n*Auto-reported by HelixScreen Crash Reporter*\n`;
  if (fingerprint) {
    md += `<sub>Fingerprint: \`${fingerprint}\`</sub>\n`;
  }

  return md;
}

/**
 * Generate a random share code using an unambiguous character set.
 * Excludes I, O, 0, 1 to avoid confusion when reading codes aloud.
 */
function generateShareCode(length = 8): string {
  const charset = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  const values = crypto.getRandomValues(new Uint8Array(length));
  return Array.from(values, (v) => charset[v % charset.length]).join("");
}

/**
 * Handle an incoming debug bundle upload.
 * Validates the API key and payload, stores in R2, returns a share code.
 */
async function handleDebugBundleUpload(request: Request, env: Env): Promise<Response> {
  // --- Authentication ---
  const apiKey = request.headers.get("X-API-Key");
  if (!apiKey || apiKey !== env.INGEST_API_KEY) {
    return jsonResponse(401, { error: "Unauthorized: invalid or missing API key" });
  }

  // --- Rate limiting (per client IP) ---
  const clientIP = request.headers.get("CF-Connecting-IP") || "unknown";
  const { success } = await env.DEBUG_BUNDLE_LIMITER.limit({ key: clientIP });
  if (!success) {
    return jsonResponse(429, { error: "Rate limit exceeded — try again later" });
  }

  // --- Read and validate body ---
  const body = await request.arrayBuffer();
  if (!body || body.byteLength === 0) {
    return jsonResponse(400, { error: "Empty body" });
  }

  const maxSize = 500 * 1024; // 500KB
  if (body.byteLength > maxSize) {
    return jsonResponse(413, { error: "Payload too large (max 500KB)" });
  }

  // --- Extract metadata and store in R2 with a share code ---
  // Metadata is decompressed from the gzipped body and stored as R2 custom
  // metadata so the list endpoint can filter/display without decompressing
  // every bundle. Bundles uploaded before this change lack custom metadata
  // and show empty fields in listings.
  const shareCode = generateShareCode();
  const metadata = await extractBundleMetadata(body);

  await env.DEBUG_BUNDLES.put(shareCode, body, {
    httpMetadata: {
      contentType: "application/json",
      contentEncoding: "gzip",
    },
    customMetadata: {
      version: metadata.version,
      printer_model: metadata.printer_model,
      platform: metadata.platform,
      klipper_version: metadata.klipper_version,
      user_note: metadata.user_note.slice(0, 100),
      uploaded: new Date().toISOString(),
    },
  });

  // --- Send email notification (best-effort, don't fail the upload) ---
  try {
    await sendBundleNotification(env, shareCode, clientIP, metadata);
  } catch (err) {
    console.error("Failed to send bundle notification:", (err as Error).message);
  }

  return jsonResponse(201, { share_code: shareCode });
}

/**
 * Retrieve a debug bundle by share code.
 * Requires admin API key for access.
 */
async function handleDebugBundleRetrieve(request: Request, env: Env, url: URL): Promise<Response> {
  // --- Authentication (admin key) ---
  const adminKey = request.headers.get("X-Admin-Key");
  if (!adminKey || adminKey !== env.ADMIN_API_KEY) {
    return jsonResponse(401, { error: "Unauthorized: invalid or missing admin key" });
  }

  // --- Extract share code from URL ---
  const code = url.pathname.split("/").pop();
  if (!code) {
    return jsonResponse(400, { error: "Missing share code" });
  }

  // --- Retrieve from R2 ---
  const object = await env.DEBUG_BUNDLES.get(code);
  if (!object) {
    return jsonResponse(404, { error: "Debug bundle not found" });
  }

  return new Response(object.body, {
    status: 200,
    headers: {
      "Content-Type": "application/json",
      "Content-Encoding": "gzip",
      ...corsHeaders(),
    },
  });
}

/**
 * List debug bundles with optional filtering.
 * Requires admin API key (X-Admin-Key header).
 *
 * Query params:
 *   limit  - Max results, 1-100 (default 20)
 *   since  - ISO date (YYYY-MM-DD); only bundles uploaded on or after
 *   until  - ISO date (YYYY-MM-DD); only bundles uploaded on or before (inclusive)
 *   match  - Case-insensitive substring matched against version, printer_model,
 *            platform, klipper_version, and share_code
 *   cursor - R2 pagination token from a previous truncated response
 *
 * Returns { bundles: BundleListEntry[], truncated: boolean, cursor?: string }.
 * Bundles uploaded before custom metadata was added will have empty metadata fields.
 */
async function handleDebugBundleList(request: Request, env: Env, url: URL): Promise<Response> {
  // --- Authentication (admin key) ---
  const adminKey = request.headers.get("X-Admin-Key");
  if (!adminKey || adminKey !== env.ADMIN_API_KEY) {
    return jsonResponse(401, { error: "Unauthorized: invalid or missing admin key" });
  }

  // --- Parse query parameters ---
  const limitParam = parseInt(url.searchParams.get("limit") || "20", 10);
  const limit = Math.min(Math.max(1, limitParam), 100);
  const since = url.searchParams.get("since") || null;
  const until = url.searchParams.get("until") || null;
  const match = url.searchParams.get("match")?.toLowerCase() || null;
  const cursor = url.searchParams.get("cursor") || undefined;

  // --- List objects from R2 ---
  // R2 list returns objects sorted by key. We fetch more than needed to allow
  // for filtering, then trim to limit. Use a larger page size to reduce the
  // chance of needing multiple R2 list calls.
  const pageSize = match || since || until ? 500 : limit;
  const listed = await env.DEBUG_BUNDLES.list({
    limit: pageSize,
    cursor,
    include: ["httpMetadata", "customMetadata"],
  });

  // --- Build result array with filtering ---
  const bundles: BundleListEntry[] = [];

  for (const obj of listed.objects) {
    const custom = obj.customMetadata || {};
    const uploaded = custom.uploaded || obj.uploaded?.toISOString() || "";

    // Date range filtering
    if (since && uploaded < since) continue;
    if (until) {
      // "until" is inclusive of the full day
      const untilEnd = until.length === 10 ? until + "T23:59:59Z" : until;
      if (uploaded > untilEnd) continue;
    }

    // Pattern matching against metadata values
    if (match) {
      const searchable = [
        custom.version || "",
        custom.printer_model || "",
        custom.platform || "",
        custom.klipper_version || "",
        custom.user_note || "",
        obj.key,
      ]
        .join(" ")
        .toLowerCase();
      if (!searchable.includes(match)) continue;
    }

    bundles.push({
      share_code: obj.key,
      size: obj.size,
      uploaded,
      metadata: {
        version: custom.version || "",
        printer_model: custom.printer_model || "",
        platform: custom.platform || "",
        klipper_version: custom.klipper_version || "",
        user_note: custom.user_note || "",
      },
    });

    if (bundles.length >= limit) break;
  }

  // Sort newest-first by uploaded timestamp
  bundles.sort((a, b) => b.uploaded.localeCompare(a.uploaded));

  return jsonResponse(200, {
    bundles,
    truncated: listed.truncated || bundles.length >= limit,
    cursor: listed.truncated ? listed.cursor : undefined,
  });
}

/**
 * Decompress a gzipped ArrayBuffer and extract metadata from the JSON bundle.
 * Returns an object with version, printer_model, timestamp, etc.
 */
async function extractBundleMetadata(gzippedBody: ArrayBuffer): Promise<BundleMetadata> {
  try {
    const ds = new DecompressionStream("gzip");
    const writer = ds.writable.getWriter();
    writer.write(new Uint8Array(gzippedBody));
    writer.close();

    const reader = ds.readable.getReader();
    const chunks: Uint8Array[] = [];
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
    }

    const text = new TextDecoder().decode(
      chunks.reduce((acc, chunk) => {
        const merged = new Uint8Array(acc.length + chunk.length);
        merged.set(acc);
        merged.set(chunk, acc.length);
        return merged;
      }, new Uint8Array())
    );

    const json = JSON.parse(text) as Record<string, unknown>;
    const printerInfo = (json.printer ?? json.printer_info) as Record<string, string> | undefined;
    const systemInfo = (json.system ?? json.system_info) as Record<string, string> | undefined;
    const rawNote = typeof json.user_note === "string" ? json.user_note : "";
    return {
      version: (json.version as string) || "unknown",
      printer_model: printerInfo?.model || "unknown",
      klipper_version: printerInfo?.klipper_version || "unknown",
      platform: systemInfo?.platform || "unknown",
      timestamp: (json.timestamp as string) || new Date().toISOString(),
      user_note: rawNote.slice(0, 500),
    };
  } catch {
    return {
      version: "unknown",
      printer_model: "unknown",
      klipper_version: "unknown",
      platform: "unknown",
      timestamp: new Date().toISOString(),
      user_note: "",
    };
  }
}

/**
 * Send a notification email via Resend when a debug bundle is uploaded.
 */
async function sendBundleNotification(
  env: Env,
  shareCode: string,
  clientIP: string,
  metadata: BundleMetadata
): Promise<void> {
  if (!env.RESEND_API_KEY) {
    console.warn("RESEND_API_KEY not set, skipping notification");
    return;
  }

  const from = env.EMAIL_FROM || "HelixScreen <noreply@helixscreen.org>";
  const to = env.NOTIFICATION_EMAIL;
  if (!to) {
    console.warn("NOTIFICATION_EMAIL not set, skipping notification");
    return;
  }

  const subject = `Debug Bundle: ${shareCode} — ${metadata.printer_model} v${metadata.version}`;

  const html = `
    <h2>New Debug Bundle Uploaded</h2>
    <table style="border-collapse: collapse; font-family: monospace;">
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">Share Code</td><td>${shareCode}</td></tr>
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">Version</td><td>${metadata.version}</td></tr>
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">Printer</td><td>${metadata.printer_model}</td></tr>
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">Klipper</td><td>${metadata.klipper_version}</td></tr>
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">Platform</td><td>${metadata.platform}</td></tr>
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">Client IP</td><td>${clientIP}</td></tr>
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">Time</td><td>${metadata.timestamp}</td></tr>${metadata.user_note ? `
      <tr><td style="padding: 4px 12px 4px 0; font-weight: bold;">User Note</td><td>${escapeHtml(metadata.user_note)}</td></tr>` : ""}
    </table>
    <p style="margin-top: 16px;">Retrieve with:</p>
    <pre style="background: #f4f4f4; padding: 8px; border-radius: 4px;">curl --compressed -H "X-Admin-Key: \$HELIX_ADMIN_KEY" https://crash.helixscreen.org/v1/debug-bundle/${shareCode}</pre>
  `;

  const text = `New Debug Bundle: ${shareCode}
Version: ${metadata.version}
Printer: ${metadata.printer_model}
Klipper: ${metadata.klipper_version}
Platform: ${metadata.platform}
IP: ${clientIP}
Time: ${metadata.timestamp}${metadata.user_note ? `\nUser Note: ${metadata.user_note}` : ""}

Retrieve: curl --compressed -H "X-Admin-Key: $HELIX_ADMIN_KEY" https://crash.helixscreen.org/v1/debug-bundle/${shareCode}`;

  const response = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.RESEND_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ from, to: [to], subject, html, text }),
  });

  if (!response.ok) {
    const err = await response.text();
    throw new Error(`Resend API ${response.status}: ${err}`);
  }
}

/**
 * Build a JSON response with CORS headers.
 */
function jsonResponse(status: number, data: Record<string, unknown>): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...corsHeaders(),
    },
  });
}

/**
 * Standard CORS headers.
 * Not strictly needed for device-to-worker traffic, but included for
 * completeness if a web dashboard ever hits this endpoint.
 */
function corsHeaders(): Record<string, string> {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, X-API-Key, X-Admin-Key",
  };
}
