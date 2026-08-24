// SPDX-License-Identifier: GPL-3.0-or-later
// HelixScreen telemetry ingestion worker — stores batched events in R2.

import { mapEventToDataPoints } from "./analytics";
import {
  executeQuery,
  parseRange,
  overviewQueries,
  adoptionQueries,
  printsQueries,
  crashesQueries,
  crashListQuery,
  releasesQueries,
  memoryQueries,
  memoryWarningQueries,
  hardwareQueries,
  engagementQueries,
  reliabilityQueries,
  stabilityQueries,
  printStartQueries,
  performanceQueries,
  featuresQueries,
  uxInsightsQueries,
  type QueryConfig,
  type FilterParams,
} from "./queries";
import {
  snapshotDay,
  snapshotToDataPoints,
  snapshotR2Key,
  previousUtcDay,
  utcDay,
  type FleetDaySnapshot,
} from "./cdn_fleet";

// Rate limiting binding type (added in @cloudflare/workers-types after our pinned version)
interface RateLimiter {
  limit(options: { key: string }): Promise<{ success: boolean }>;
}

// Analytics Engine dataset binding type
interface AnalyticsEngineDataset {
  writeDataPoint(point: {
    blobs?: string[];
    doubles?: number[];
    indexes?: string[];
  }): void;
}

interface Env {
  TELEMETRY_BUCKET: R2Bucket;
  INGEST_API_KEY: string; // Cloudflare secret: wrangler secret put INGEST_API_KEY
  ADMIN_API_KEY: string; // Cloudflare secret: wrangler secret put ADMIN_API_KEY (for analytics)
  INGEST_LIMITER: RateLimiter; // Rate limiting binding (see wrangler.toml)
  TELEMETRY_ANALYTICS?: AnalyticsEngineDataset; // Analytics Engine (see wrangler.toml)
  CLOUDFLARE_ACCOUNT_ID: string; // Set in wrangler.toml [vars]
  HELIX_ANALYTICS_READ_TOKEN?: string; // Cloudflare secret: wrangler secret put HELIX_ANALYTICS_READ_TOKEN
  CDN_ZONE_ID?: string; // Set in wrangler.toml [vars]
  CF_ZONE_ANALYTICS_TOKEN?: string; // Cloudflare secret: needs Zone Analytics:Read on helixscreen.org
}

const CORS_HEADERS: Record<string, string> = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, X-API-Key",
};

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", ...CORS_HEADERS },
  });
}

interface RawCrashRow {
  timestamp: string;
  device_id: string;
  ver: string;
  sig: string;
  platform: string;
  uptime_sec: number;
}

interface DedupedCrashRow extends RawCrashRow {
  occurrences: number;
}

/**
 * De-duplicate raw crash rows in TypeScript. Devices re-report the same crash
 * on every reboot, so we group by (device_id, signal, uptime bucket rounded
 * to 60s) and keep the most recent timestamp + occurrence count.
 */
function deduplicateCrashes(rows: RawCrashRow[]): DedupedCrashRow[] {
  const groups = new Map<string, { row: RawCrashRow; count: number }>();
  for (const row of rows) {
    const uptimeBucket = Math.floor(row.uptime_sec / 60);
    const key = `${row.device_id}:${row.sig}:${uptimeBucket}`;
    const existing = groups.get(key);
    if (existing) {
      existing.count++;
      if (row.timestamp > existing.row.timestamp) {
        existing.row = row;
      }
    } else {
      groups.set(key, { row, count: 1 });
    }
  }
  return [...groups.values()]
    .map(({ row, count }) => ({ ...row, occurrences: count }))
    .sort((a, b) => b.timestamp.localeCompare(a.timestamp));
}

function randomHex(bytes: number): string {
  const buf = new Uint8Array(bytes);
  crypto.getRandomValues(buf);
  return Array.from(buf, (b) => b.toString(16).padStart(2, "0")).join("");
}

function validateEvent(evt: unknown, index: number): string | null {
  if (typeof evt !== "object" || evt === null || Array.isArray(evt)) {
    return `event[${index}]: must be an object`;
  }
  const e = evt as Record<string, unknown>;
  if (typeof e.schema_version !== "number") {
    return `event[${index}]: schema_version must be a number`;
  }
  if (typeof e.event !== "string" || e.event.length === 0) {
    return `event[${index}]: event must be a non-empty string`;
  }
  if (typeof e.device_id !== "string" || e.device_id.length === 0) {
    return `event[${index}]: device_id must be a non-empty string`;
  }
  if (typeof e.timestamp !== "string" || e.timestamp.length === 0) {
    return `event[${index}]: timestamp must be a non-empty string`;
  }
  return null;
}

function parseFilters(searchParams: URLSearchParams): FilterParams {
  const filters: FilterParams = {};
  const platform = searchParams.get("platform");
  const version = searchParams.get("version");
  const model = searchParams.get("model");
  if (platform) filters.platform = platform;
  if (version) filters.version = version;
  if (model) filters.model = model;
  return filters;
}

/**
 * Snapshot one UTC day of CDN manifest polls and persist it both ways: R2 for
 * permanence, Analytics Engine so the dashboard can read it alongside the
 * telemetry metrics.
 *
 * R2 is written first and is the durable copy - an Analytics Engine failure
 * must not lose the day, because zone analytics will not still have it
 * tomorrow.
 */
async function captureCdnFleetDay(env: Env, date: string): Promise<FleetDaySnapshot> {
  // Zone analytics needs Zone Analytics:Read, which is a different permission
  // from the Account Analytics:Read that the dashboard's SQL token carries. If
  // one token happens to hold both, the fallback means no new secret is needed;
  // otherwise set CF_ZONE_ANALYTICS_TOKEN and it takes precedence.
  const token = env.CF_ZONE_ANALYTICS_TOKEN || env.HELIX_ANALYTICS_READ_TOKEN;
  if (!env.CDN_ZONE_ID || !token) {
    throw new Error("CDN fleet snapshot not configured (CDN_ZONE_ID / CF_ZONE_ANALYTICS_TOKEN)");
  }

  const snap = await snapshotDay(token, env.CDN_ZONE_ID, date);

  await env.TELEMETRY_BUCKET.put(snapshotR2Key(date), JSON.stringify(snap), {
    httpMetadata: { contentType: "application/json" },
  });

  if (env.TELEMETRY_ANALYTICS) {
    try {
      for (const point of snapshotToDataPoints(snap)) {
        env.TELEMETRY_ANALYTICS.writeDataPoint(point);
      }
    } catch {
      // R2 already holds the day; a failed AE write is recoverable by backfill.
    }
  }

  return snap;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    // CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }

    // Health check
    if (url.pathname === "/health" && request.method === "GET") {
      return json({ status: "healthy" });
    }

    // Event ingestion
    if (url.pathname === "/v1/events") {
      if (request.method !== "POST") {
        return json({ error: "Method not allowed" }, 405);
      }

      // Verify API key
      const apiKey = request.headers.get("x-api-key") ?? "";
      if (!env.INGEST_API_KEY || apiKey !== env.INGEST_API_KEY) {
        return json({ error: "Unauthorized" }, 401);
      }

      // Rate limiting (per client IP)
      const clientIP = request.headers.get("CF-Connecting-IP") || "unknown";
      const { success } = await env.INGEST_LIMITER.limit({ key: clientIP });
      if (!success) {
        return json({ error: "Rate limit exceeded" }, 429);
      }

      const contentType = request.headers.get("content-type") ?? "";
      if (!contentType.includes("application/json")) {
        return json({ error: "Content-Type must be application/json" }, 400);
      }

      let body: unknown;
      try {
        body = await request.json();
      } catch {
        return json({ error: "Invalid JSON body" }, 400);
      }

      if (!Array.isArray(body)) {
        return json({ error: "Body must be a JSON array of events" }, 400);
      }
      if (body.length === 0 || body.length > 20) {
        return json({ error: "Batch must contain 1-20 events" }, 400);
      }

      for (let i = 0; i < body.length; i++) {
        const err = validateEvent(body[i], i);
        if (err) return json({ error: err }, 400);
      }

      // Build R2 key: events/YYYY/MM/DD/{epochMs}-{random6hex}.json
      const now = new Date();
      const yyyy = now.getUTCFullYear();
      const mm = String(now.getUTCMonth() + 1).padStart(2, "0");
      const dd = String(now.getUTCDate()).padStart(2, "0");
      const key = `events/${yyyy}/${mm}/${dd}/${now.getTime()}-${randomHex(3)}.json`;

      try {
        await env.TELEMETRY_BUCKET.put(key, JSON.stringify(body), {
          httpMetadata: { contentType: "application/json" },
        });
      } catch {
        return json({ error: "Failed to store events" }, 500);
      }

      // Dual-write to Analytics Engine (fire-and-forget, never blocks response)
      if (env.TELEMETRY_ANALYTICS) {
        for (const evt of body) {
          try {
            const points = mapEventToDataPoints(evt as Record<string, unknown>);
            for (const point of points) {
              env.TELEMETRY_ANALYTICS.writeDataPoint(point);
            }
          } catch {
            // Analytics Engine failure must not affect ingestion
          }
        }
      }

      return json({ status: "ok", stored: body.length });
    }

    // Event listing — returns keys for a given date prefix (for analytics pull)
    // GET /v1/events/list?prefix=events/2026/01/15/&cursor=...
    // Requires ADMIN_API_KEY (NOT the ingest key baked into client binaries)
    if (url.pathname === "/v1/events/list" && request.method === "GET") {
      const apiKey = request.headers.get("x-api-key") ?? "";
      if (!env.ADMIN_API_KEY || apiKey !== env.ADMIN_API_KEY) {
        return json({ error: "Unauthorized" }, 401);
      }

      const prefix = url.searchParams.get("prefix") ?? "events/";
      if (!prefix.startsWith("events/")) {
        return json({ error: "Prefix must start with events/" }, 400);
      }
      const cursor = url.searchParams.get("cursor") ?? undefined;
      const limit = Math.max(1, Math.min(
        parseInt(url.searchParams.get("limit") ?? "1000", 10) || 1000,
        1000,
      ));

      const listed = await env.TELEMETRY_BUCKET.list({ prefix, cursor, limit });
      return json({
        keys: listed.objects.map((obj) => ({
          key: obj.key,
          size: obj.size,
          uploaded: obj.uploaded.toISOString(),
        })),
        truncated: listed.truncated,
        cursor: listed.truncated ? listed.cursor : undefined,
      });
    }

    // Event download — stream a specific event file
    // GET /v1/events/get?key=events/2026/01/15/1234567890-abc123.json
    // Requires ADMIN_API_KEY (NOT the ingest key baked into client binaries)
    if (url.pathname === "/v1/events/get" && request.method === "GET") {
      const apiKey = request.headers.get("x-api-key") ?? "";
      if (!env.ADMIN_API_KEY || apiKey !== env.ADMIN_API_KEY) {
        return json({ error: "Unauthorized" }, 401);
      }

      const key = url.searchParams.get("key");
      if (!key || !key.startsWith("events/") || !key.endsWith(".json")) {
        return json({ error: "Invalid key" }, 400);
      }

      const obj = await env.TELEMETRY_BUCKET.get(key);
      if (!obj) {
        return json({ error: "Not found" }, 404);
      }

      return new Response(obj.body, {
        headers: {
          "Content-Type": "application/json",
          ...CORS_HEADERS,
        },
      });
    }

    // ---------- Dashboard endpoints (all require ADMIN_API_KEY) ----------

    if (url.pathname.startsWith("/v1/dashboard/")) {
      if (request.method !== "GET") {
        return json({ error: "Method not allowed" }, 405);
      }

      const apiKey = request.headers.get("x-api-key") ?? "";
      if (!env.ADMIN_API_KEY || apiKey !== env.ADMIN_API_KEY) {
        return json({ error: "Unauthorized" }, 401);
      }

      if (!env.CLOUDFLARE_ACCOUNT_ID || !env.HELIX_ANALYTICS_READ_TOKEN) {
        return json({ error: "Analytics Engine not configured" }, 503);
      }

      const queryConfig: QueryConfig = {
        accountId: env.CLOUDFLARE_ACCOUNT_ID,
        apiToken: env.HELIX_ANALYTICS_READ_TOKEN,
      };

      const range = url.searchParams.get("range");
      const days = parseRange(range);
      const filters = parseFilters(url.searchParams);

      try {
        // GET /v1/dashboard/overview
        if (url.pathname === "/v1/dashboard/overview") {
          const queries = overviewQueries(days, filters);
          const [
            devicesRes, totalRes, rateRes, printRes, timeRes, dailyActiveRes, firstSeenRes,
            cdnFleetRes,
          ] = await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const devicesData = devicesRes as { data: Array<{ active_devices: number }> };
          const totalData = totalRes as { data: Array<{ total_events: number }> };
          const rateData = rateRes as { data: Array<{ crash_count: number; session_count: number }> };
          const printData = printRes as { data: Array<{ successes: number; total: number }> };
          const timeData = timeRes as { data: Array<{ date: string; count: number }> };
          const dailyActiveData = dailyActiveRes as { data: Array<{ date: string; devices: number }> };
          const firstSeenData = firstSeenRes as { data: Array<{ first_seen: string; new_devices: number }> };
          const cdnFleetData = cdnFleetRes as {
            data: Array<{ date: string; sources: number; polls: number; errors: number }>;
          };

          const crashRow = rateData.data?.[0] ?? { crash_count: 0, session_count: 0 };
          const printRow = printData.data?.[0] ?? { successes: 0, total: 0 };

          // Build cumulative growth: count new devices per first-seen date, then accumulate
          // Each row = one device (GROUP BY blob1), so increment by 1 per row
          const newPerDay = new Map<string, number>();
          for (const row of firstSeenData.data ?? []) {
            const d = row.first_seen;
            newPerDay.set(d, (newPerDay.get(d) ?? 0) + 1);
          }
          const sortedDates = [...newPerDay.keys()].sort();
          let cumulative = 0;
          const cumulativeGrowth = sortedDates.map((date) => {
            cumulative += newPerDay.get(date)!;
            return { date, total: cumulative };
          });

          return json({
            active_devices: devicesData.data?.[0]?.active_devices ?? 0,
            total_events: totalData.data?.[0]?.total_events ?? 0,
            crash_rate: crashRow.session_count > 0 ? crashRow.crash_count / crashRow.session_count : 0,
            print_success_rate: printRow.total > 0 ? printRow.successes / printRow.total : 0,
            events_over_time: (timeData.data ?? []).map((r) => ({
              date: r.date,
              count: r.count,
            })),
            daily_active_devices: (dailyActiveData.data ?? []).map((r) => ({
              date: r.date,
              devices: r.devices,
            })),
            cumulative_devices: cumulativeGrowth,
            // Fleet estimate from CDN update-manifest polls. Independent of the
            // opt-in telemetry above, so it sees installs `active_devices`
            // never will. Latest closed day is the headline; the series backs
            // the trend line.
            cdn_fleet: (cdnFleetData.data ?? []).map((r) => ({
              date: r.date,
              sources: r.sources,
              polls: r.polls,
              errors: r.errors,
            })),
          });
        }

        // GET /v1/dashboard/adoption
        if (url.pathname === "/v1/dashboard/adoption") {
          const queries = adoptionQueries(days, filters);
          const [platformsRes, versionsRes, modelsRes, kinematicsRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const toList = (res: unknown) => {
            const d = res as { data: Array<{ name: string; count: number }> };
            return (d.data ?? []).map((r) => ({ name: r.name, count: r.count }));
          };

          return json({
            platforms: toList(platformsRes),
            versions: toList(versionsRes),
            printer_models: toList(modelsRes),
            kinematics: toList(kinematicsRes),
          });
        }

        // GET /v1/dashboard/prints
        if (url.pathname === "/v1/dashboard/prints") {
          const queries = printsQueries(days, filters);
          const startQueries = printStartQueries(days, filters);
          const allResults = await Promise.all(
            [...queries, ...startQueries].map((q) => executeQuery(queryConfig, q)),
          );

          const [rateTimeRes, filamentRes, avgDurRes, slicerRes, fileSizeRes, thumbRes, amsRes, sourceRes] = allResults;

          const rateTimeData = rateTimeRes as { data: Array<{ date: string; rate: number; total: number }> };
          const filamentData = filamentRes as { data: Array<{ type: string; success_rate: number; count: number }> };
          const avgDurData = avgDurRes as { data: Array<{ avg_duration_sec: number }> };
          const slicerData = slicerRes as { data: Array<{ name: string; count: number }> };
          const fileSizeData = fileSizeRes as { data: Array<{ name: string; count: number }> };
          const thumbData = thumbRes as { data: Array<{ thumbnail_rate: number }> };
          const amsData = amsRes as { data: Array<{ ams_rate: number }> };
          const sourceData = sourceRes as { data: Array<{ name: string; count: number }> };

          return json({
            success_rate_over_time: (rateTimeData.data ?? []).map((r) => ({
              date: r.date,
              rate: r.rate,
              total: r.total,
            })),
            by_filament: (() => {
              // Multi-filament prints store types as JSON arrays like '["PLA","ABS","PLA"]'.
              // Parse these, extract unique types, and aggregate counts + weighted success rates.
              const typeMap = new Map<string, { successes: number; total: number }>();
              for (const r of filamentData.data ?? []) {
                const rawType = String(r.type).trim();
                const count = Number(r.count);
                const successRate = Number(r.success_rate);
                const successes = Math.round(successRate * count);
                // Parse JSON array strings into individual types
                let types: string[];
                if (rawType.startsWith("[")) {
                  try {
                    const parsed = JSON.parse(rawType) as string[];
                    types = [...new Set(parsed.filter(Boolean))];
                  } catch {
                    types = [rawType];
                  }
                } else {
                  types = [rawType];
                }
                for (const t of types) {
                  const existing = typeMap.get(t) ?? { successes: 0, total: 0 };
                  existing.successes += successes;
                  existing.total += count;
                  typeMap.set(t, existing);
                }
              }
              return [...typeMap.entries()]
                .map(([type, { successes, total }]) => ({
                  type,
                  success_rate: total > 0 ? successes / total : 0,
                  count: total,
                }))
                .sort((a, b) => b.count - a.count);
            })(),
            avg_duration_sec: avgDurData.data?.[0]?.avg_duration_sec ?? 0,
            start_context: {
              slicers: (slicerData.data ?? []).map((r) => ({ name: r.name, count: r.count })),
              file_size_buckets: (fileSizeData.data ?? []).map((r) => ({ name: r.name, count: r.count })),
              thumbnail_rate: thumbData.data?.[0]?.thumbnail_rate ?? 0,
              ams_rate: amsData.data?.[0]?.ams_rate ?? 0,
              sources: (sourceData.data ?? []).map((r) => ({ name: r.name, count: r.count })),
            },
          });
        }

        // GET /v1/dashboard/crashes
        if (url.pathname === "/v1/dashboard/crashes") {
          const queries = crashesQueries(days, filters);
          const [
            crashByVerRes,
            sessionByVerRes,
            signalRes,
            uptimeRes,
            crashByPlatRes,
            sessionByPlatRes,
            trailing14CrashRes,
            trailing14SessionRes,
            trailing14CrashByPlatRes,
            trailing14SessionByPlatRes,
          ] = await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const crashByVer = crashByVerRes as { data: Array<{ ver: string; crash_count: number }> };
          const sessionByVer = sessionByVerRes as { data: Array<{ ver: string; session_count: number }> };
          const signalData = signalRes as { data: Array<{ signal: string; count: number }> };
          const uptimeData = uptimeRes as { data: Array<{ avg_uptime_sec: number }> };
          const crashByPlat = crashByPlatRes as {
            data: Array<{ platform: string; crash_count: number; device_count: number }>;
          };
          const sessionByPlat = sessionByPlatRes as {
            data: Array<{ platform: string; session_count: number; device_count: number }>;
          };
          const trailing14Crash = trailing14CrashRes as { data: Array<{ crash_count: number }> };
          const trailing14Session = trailing14SessionRes as { data: Array<{ session_count: number }> };
          const trailing14CrashByPlat = trailing14CrashByPlatRes as {
            data: Array<{ platform: string; crash_count: number }>;
          };
          const trailing14SessionByPlat = trailing14SessionByPlatRes as {
            data: Array<{ platform: string; session_count: number }>;
          };

          // Build session count lookup
          const sessionMap = new Map<string, number>();
          for (const row of sessionByVer.data ?? []) {
            sessionMap.set(row.ver, row.session_count);
          }

          // Merge crash and session counts per platform for the headline cards.
          // Platforms that had sessions but no crashes still appear (rate=0).
          const platMap = new Map<
            string,
            {
              platform: string;
              crash_count: number;
              session_count: number;
              crashing_devices: number;
              session_devices: number;
            }
          >();
          for (const row of sessionByPlat.data ?? []) {
            platMap.set(row.platform, {
              platform: row.platform,
              crash_count: 0,
              session_count: row.session_count,
              crashing_devices: 0,
              session_devices: row.device_count,
            });
          }
          for (const row of crashByPlat.data ?? []) {
            const entry = platMap.get(row.platform) ?? {
              platform: row.platform,
              crash_count: 0,
              session_count: 0,
              crashing_devices: 0,
              session_devices: 0,
            };
            entry.crash_count = row.crash_count;
            entry.crashing_devices = row.device_count;
            platMap.set(row.platform, entry);
          }

          // Same merge for the trailing-14d window. Separate from the
          // range-scoped breakdown above so the UI can show both.
          const platMap14 = new Map<string, { crash_count: number; session_count: number }>();
          for (const row of trailing14SessionByPlat.data ?? []) {
            platMap14.set(row.platform, { crash_count: 0, session_count: row.session_count });
          }
          for (const row of trailing14CrashByPlat.data ?? []) {
            const entry = platMap14.get(row.platform) ?? { crash_count: 0, session_count: 0 };
            entry.crash_count = row.crash_count;
            platMap14.set(row.platform, entry);
          }

          const overall14Crash = trailing14Crash.data?.[0]?.crash_count ?? 0;
          const overall14Session = trailing14Session.data?.[0]?.session_count ?? 0;

          return json({
            by_version: (crashByVer.data ?? []).map((r) => {
              const sessionCount = sessionMap.get(r.ver) ?? 0;
              return {
                version: r.ver,
                crash_count: r.crash_count,
                session_count: sessionCount,
                rate: sessionCount > 0 ? r.crash_count / sessionCount : 0,
              };
            }),
            by_signal: (signalData.data ?? []).map((r) => ({
              signal: r.signal,
              count: r.count,
            })),
            avg_uptime_sec: uptimeData.data?.[0]?.avg_uptime_sec ?? 0,
            by_platform: Array.from(platMap.values())
              .map((p) => ({
                ...p,
                rate: p.session_count > 0 ? p.crash_count / p.session_count : 0,
              }))
              .sort((a, b) => b.session_count - a.session_count),
            trailing_14d: {
              crash_count: overall14Crash,
              session_count: overall14Session,
              rate: overall14Session > 0 ? overall14Crash / overall14Session : 0,
              by_platform: Array.from(platMap14.entries())
                .map(([platform, v]) => ({
                  platform,
                  crash_count: v.crash_count,
                  session_count: v.session_count,
                  rate: v.session_count > 0 ? v.crash_count / v.session_count : 0,
                }))
                .sort((a, b) => b.session_count - a.session_count),
            },
          });
        }

        // GET /v1/dashboard/crash-list?range=30d&limit=50
        if (url.pathname === "/v1/dashboard/crash-list") {
          const limit = Math.max(1, Math.min(
            parseInt(url.searchParams.get("limit") ?? "50", 10) || 50,
            200,
          ));
          // Fetch more raw rows than needed so dedup still yields enough results
          const sql = crashListQuery(days, limit * 5, filters);
          const result = await executeQuery(queryConfig, sql);
          const data = result as {
            data: Array<{
              timestamp: string;
              device_id: string;
              ver: string;
              sig: string;
              platform: string;
              uptime_sec: number;
            }>;
          };

          const deduped = deduplicateCrashes(data.data ?? []);

          return json({
            crashes: deduped.slice(0, limit).map((r) => ({
              timestamp: r.timestamp,
              device_id: r.device_id,
              version: r.ver,
              signal: r.sig,
              platform: r.platform,
              uptime_sec: r.uptime_sec,
              occurrences: r.occurrences,
            })),
          });
        }

        // GET /v1/dashboard/memory
        if (url.pathname === "/v1/dashboard/memory") {
          const queries = memoryQueries(days, filters);
          const [rssTimeRes, rssPlatformRes, vmPeakRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const rssTimeData = rssTimeRes as { data: Array<{ date: string; avg_rss_kb: number; p95_rss_kb: number; max_rss_kb: number }> };
          const rssPlatformData = rssPlatformRes as { data: Array<{ platform: string; avg_rss_kb: number }> };
          const vmPeakData = vmPeakRes as { data: Array<{ date: string; avg_vm_peak_kb: number }> };

          return json({
            rss_over_time: (rssTimeData.data ?? []).map((r) => ({
              date: r.date,
              avg_rss_kb: r.avg_rss_kb,
              p95_rss_kb: r.p95_rss_kb,
              max_rss_kb: r.max_rss_kb,
            })),
            rss_by_platform: (rssPlatformData.data ?? []).map((r) => ({
              platform: r.platform,
              avg_rss_kb: r.avg_rss_kb,
            })),
            vm_peak_trend: (vmPeakData.data ?? []).map((r) => ({
              date: r.date,
              avg_vm_peak_kb: r.avg_vm_peak_kb,
            })),
          });
        }

        // GET /v1/dashboard/memory-warnings
        if (url.pathname === "/v1/dashboard/memory-warnings") {
          const queries = memoryWarningQueries(days, filters);
          const [byLevelRes, overTimeRes, rssAtWarningRes, byPlatformRes, affectedRes, recentRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const byLevelData = byLevelRes as { data: Array<{ level: string; count: number }> };
          const overTimeData = overTimeRes as { data: Array<{ date: string; level: string; count: number }> };
          const rssAtWarningData = rssAtWarningRes as { data: Array<{ date: string; avg_rss_kb: number; max_rss_kb: number }> };
          const byPlatformData = byPlatformRes as { data: Array<{ platform: string; count: number; avg_rss_kb: number }> };
          const affectedData = affectedRes as { data: Array<{ affected_devices: number }> };
          const recentData = recentRes as {
            data: Array<{
              timestamp: string;
              device_id: string;
              version: string;
              platform: string;
              level: string;
              reason: string;
              uptime_sec: number;
              rss_kb: number;
              system_available_mb: number;
              growth_5min_kb: number;
              private_dirty_kb: number;
              pss_kb: number;
            }>;
          };

          const totalWarnings = (byLevelData.data ?? []).reduce((sum, r) => sum + r.count, 0);

          return json({
            total_warnings: totalWarnings,
            affected_devices: affectedData.data?.[0]?.affected_devices ?? 0,
            by_level: (byLevelData.data ?? []).map((r) => ({
              level: r.level,
              count: r.count,
            })),
            over_time: (overTimeData.data ?? []).map((r) => ({
              date: r.date,
              level: r.level,
              count: r.count,
            })),
            rss_at_warning: (rssAtWarningData.data ?? []).map((r) => ({
              date: r.date,
              avg_rss_kb: r.avg_rss_kb,
              max_rss_kb: r.max_rss_kb,
            })),
            by_platform: (byPlatformData.data ?? []).map((r) => ({
              platform: r.platform,
              count: r.count,
              avg_rss_kb: r.avg_rss_kb,
            })),
            recent_warnings: (recentData.data ?? []).map((r) => ({
              timestamp: r.timestamp,
              device_id: r.device_id,
              version: r.version,
              platform: r.platform,
              level: r.level,
              reason: r.reason,
              uptime_sec: r.uptime_sec,
              rss_kb: r.rss_kb,
              system_available_mb: r.system_available_mb,
              growth_5min_kb: r.growth_5min_kb,
              private_dirty_kb: r.private_dirty_kb,
              pss_kb: r.pss_kb,
            })),
          });
        }

        // GET /v1/dashboard/hardware
        if (url.pathname === "/v1/dashboard/hardware") {
          const queries = hardwareQueries(days, filters);
          const [modelsRes, kinematicsRes, mcuRes, capsRes, volRes, countsRes, ramRes, amsRes, helixMacrosRes, moonrakerLocalityRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const toList = (res: unknown) => {
            const d = res as { data: Array<{ name: string; count: number }> };
            return (d.data ?? []).map((r) => ({ name: r.name, count: r.count }));
          };

          // Collapse a "1"/"0" tri-state column into a labelled yes/no split.
          // Rows with an empty name never reach here (the query filters them),
          // so `reported` counts only devices that actually answered.
          const toYesNo = (res: unknown, yesKey: string, noKey: string) => {
            const rows = toList(res);
            const yes = rows.find((r) => r.name === "1")?.count ?? 0;
            const no = rows.find((r) => r.name === "0")?.count ?? 0;
            return { [yesKey]: yes, [noKey]: no, reported: yes + no };
          };

          const capsData = capsRes as { data: Array<Record<string, number>> };
          const capsRow = capsData.data?.[0] ?? {};
          const volData = volRes as { data: Array<{ avg_vol_x: number; avg_vol_y: number; avg_vol_z: number }> };
          const volRow = volData.data?.[0] ?? { avg_vol_x: 0, avg_vol_y: 0, avg_vol_z: 0 };
          const countsData = countsRes as { data: Array<{ avg_fan_count: number; avg_sensor_count: number; avg_macro_count: number }> };
          const countsRow = countsData.data?.[0] ?? { avg_fan_count: 0, avg_sensor_count: 0, avg_macro_count: 0 };

          // Bucket RAM values into human-readable ranges
          const ramRaw = ramRes as { data: Array<{ ram_mb: number; count: number }> };
          const ramBuckets = new Map<string, number>();
          for (const r of ramRaw.data ?? []) {
            const mb = r.ram_mb;
            let bucket: string;
            if (mb <= 384) bucket = "256 MB";
            else if (mb <= 768) bucket = "512 MB";
            else if (mb <= 1536) bucket = "1 GB";
            else if (mb <= 3072) bucket = "2 GB";
            else if (mb <= 6144) bucket = "4 GB";
            else if (mb <= 12288) bucket = "8 GB";
            else bucket = "16+ GB";
            ramBuckets.set(bucket, (ramBuckets.get(bucket) ?? 0) + r.count);
          }
          const ramOrder = ["256 MB", "512 MB", "1 GB", "2 GB", "4 GB", "8 GB", "16+ GB"];
          const ram_distribution = ramOrder
            .filter((b) => ramBuckets.has(b))
            .map((name) => ({ name, count: ramBuckets.get(name)! }));

          return json({
            printer_models: toList(modelsRes),
            kinematics: toList(kinematicsRes),
            mcu_chips: toList(mcuRes),
            capabilities: {
              total: capsRow.total ?? 0,
              bits: [
                capsRow.cap_0 ?? 0,
                capsRow.cap_1 ?? 0,
                capsRow.cap_2 ?? 0,
                capsRow.cap_3 ?? 0,
                capsRow.cap_4 ?? 0,
                capsRow.cap_5 ?? 0,
                capsRow.cap_6 ?? 0,
                capsRow.cap_7 ?? 0,
                capsRow.cap_8 ?? 0,
                capsRow.cap_9 ?? 0,
                capsRow.cap_10 ?? 0,
                capsRow.cap_11 ?? 0,
                capsRow.cap_12 ?? 0,
              ],
            },
            avg_build_volume: {
              x: volRow.avg_vol_x,
              y: volRow.avg_vol_y,
              z: volRow.avg_vol_z,
            },
            avg_counts: {
              fans: countsRow.avg_fan_count,
              sensors: countsRow.avg_sensor_count,
              macros: countsRow.avg_macro_count,
            },
            ram_distribution,
            ams_backends: toList(amsRes).map((r) => {
              const AMS_NAMES: Record<string, string> = {
                afc: "AFC",
                happy_hare: "Happy Hare",
                valgace: "ValgACE",
                tool_changer: "Tool Changer",
                cfs: "CFS",
                ifs: "IFS",
              };
              return { name: AMS_NAMES[r.name] ?? r.name, count: r.count };
            }),
            // Both are yes/no splits over the devices that reported the field.
            // `reported` is the denominator: it excludes clients too old to
            // send it, so a small number here means "not enough data yet"
            // rather than "nobody has it".
            helix_macros: toYesNo(helixMacrosRes, "installed", "not_installed"),
            moonraker_locality: toYesNo(moonrakerLocalityRes, "local", "remote"),
          });
        }

        // GET /v1/dashboard/engagement
        if (url.pathname === "/v1/dashboard/engagement") {
          const queries = engagementQueries(days, filters);
          const [panelTimeRes, panelVisitsRes, sessionTrendRes, themeRes, localeRes, brightnessRes, darkLightRes, widgetPlacementRes, widgetInteractionRes, overlayVisitsRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const toList = (res: unknown) => {
            const d = res as { data: Array<{ name: string; count: number }> };
            return (d.data ?? []).map((r) => ({ name: r.name, count: r.count }));
          };

          const panelTimeData = panelTimeRes as { data: Array<{ panel: string; total_time_sec: number }> };
          const panelVisitsData = panelVisitsRes as { data: Array<{ panel: string; total_visits: number }> };
          const sessionTrendData = sessionTrendRes as { data: Array<{ date: string; avg_session_sec: number }> };
          const brightnessData = brightnessRes as { data: Array<{ p25: number; p50: number; p75: number }> };
          const brightnessRow = brightnessData.data?.[0] ?? { p25: 0, p50: 0, p75: 0 };

          // Aggregate themes by base name: strip " (Dark)"/" (Light)" suffix,
          // rename "Copy" themes to "(Custom)"
          const rawThemes = toList(themeRes);
          const themeMap = new Map<string, number>();
          for (const t of rawThemes) {
            let base = t.name
              .replace(/ \(Dark\)$/i, "")
              .replace(/ \(Light\)$/i, "")
              .replace(/ Copy$/i, " (Custom)");
            themeMap.set(base, (themeMap.get(base) ?? 0) + t.count);
          }
          const themes = [...themeMap.entries()]
            .map(([name, count]) => ({ name, count }))
            .sort((a, b) => b.count - a.count);

          const widgetPlacementData = widgetPlacementRes as { data: Array<{ widget: string; devices: number }> };
          const widgetInteractionData = widgetInteractionRes as { data: Array<{ widget: string; interactions: number }> };
          const overlayVisitsData = overlayVisitsRes as { data: Array<{ overlay: string; total_visits: number }> };

          return json({
            panel_time: (panelTimeData.data ?? []).map((r) => ({
              panel: r.panel,
              total_time_sec: r.total_time_sec,
            })),
            panel_visits: (panelVisitsData.data ?? []).map((r) => ({
              panel: r.panel,
              total_visits: r.total_visits,
            })),
            session_duration_trend: (sessionTrendData.data ?? []).map((r) => ({
              date: r.date,
              avg_session_sec: r.avg_session_sec,
            })),
            themes,
            dark_vs_light: toList(darkLightRes),
            locales: toList(localeRes),
            brightness: {
              p25: brightnessRow.p25,
              p50: brightnessRow.p50,
              p75: brightnessRow.p75,
            },
            widget_placement: (widgetPlacementData.data ?? []).map((r) => ({
              widget: r.widget,
              devices: r.devices,
            })),
            widget_interactions: (widgetInteractionData.data ?? []).map((r) => ({
              widget: r.widget,
              interactions: r.interactions,
            })),
            overlay_visits: (overlayVisitsData.data ?? []).map((r) => ({
              overlay: r.overlay,
              total_visits: r.total_visits,
            })),
          });
        }

        // GET /v1/dashboard/reliability
        if (url.pathname === "/v1/dashboard/reliability") {
          const queries = reliabilityQueries(days, filters);
          const [uptimeRes, disconnectRes, longestRes, errorCatsRes, errorCodesRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const uptimeData = uptimeRes as { data: Array<{ date: string; avg_uptime_pct: number }> };
          const disconnectData = disconnectRes as { data: Array<{ date: string; avg_disconnects: number }> };
          const longestData = longestRes as { data: Array<{ max_disconnect_sec: number }> };
          const errorCatsData = errorCatsRes as { data: Array<{ category: string; count: number }> };
          const errorCodesData = errorCodesRes as { data: Array<{ category: string; code: string; count: number }> };

          return json({
            uptime_trend: (uptimeData.data ?? []).map((r) => ({
              date: r.date,
              avg_uptime_pct: r.avg_uptime_pct,
            })),
            disconnect_trend: (disconnectData.data ?? []).map((r) => ({
              date: r.date,
              avg_disconnects: r.avg_disconnects,
            })),
            max_disconnect_sec: longestData.data?.[0]?.max_disconnect_sec ?? 0,
            error_categories: (errorCatsData.data ?? []).map((r) => ({
              category: r.category,
              count: r.count,
            })),
            error_codes: (errorCodesData.data ?? []).map((r) => ({
              category: r.category,
              code: r.code,
              count: r.count,
            })),
          });
        }

        // GET /v1/dashboard/stability
        if (url.pathname === "/v1/dashboard/stability") {
          const queries = stabilityQueries(days, filters);
          const displayLimit = Math.max(1, Math.min(
            parseInt(url.searchParams.get("limit") ?? "50", 10) || 50,
            200,
          ));
          // Fetch all unique crash incidents (deduped via GROUP BY in SQL)
          // with a high limit so we can compute accurate aggregates in TS.
          const crashListSql = crashListQuery(days, 10000, filters);

          const allResults = await Promise.all([
            // Skip queries 0,2,4,5 (crash aggregates) — we compute those from the deduped list
            executeQuery(queryConfig, queries[1]),  // session count over time
            executeQuery(queryConfig, queries[3]),  // session count by version
            executeQuery(queryConfig, queries[6]),  // klippy
            executeQuery(queryConfig, queries[7]),  // memory warnings
            executeQuery(queryConfig, queries[8]),  // error categories
            executeQuery(queryConfig, queries[9]),  // error codes
            executeQuery(queryConfig, crashListSql),
            executeQuery(queryConfig, queries[10]), // display anomaly trend
            executeQuery(queryConfig, queries[11]), // display anomaly codes
            executeQuery(queryConfig, queries[12]), // display anomaly by version
            executeQuery(queryConfig, queries[13]), // recent display anomalies
            executeQuery(queryConfig, queries[14]), // display affected devices
          ]);

          const sessionTimeRes = allResults[0] as { data: Array<{ date: string; session_count: number }> };
          const sessionByVerRes = allResults[1] as { data: Array<{ ver: string; session_count: number }> };
          const klippyRes = allResults[2] as { data: Array<{ date: string; errors: number; shutdowns: number }> };
          const memWarnRes = allResults[3] as { data: Array<{ date: string; count: number }> };
          const errorCatsRes = allResults[4] as { data: Array<{ category: string; count: number }> };
          const errorCodesRes = allResults[5] as { data: Array<{ category: string; code: string; count: number }> };
          const displayTrendRes = allResults[7] as { data: Array<{ date: string; count: number }> };
          const displayCodesRes = allResults[8] as { data: Array<{ code: string; count: number }> };
          const displayByVerRes = allResults[9] as { data: Array<{ version: string; count: number }> };
          const displayRecentRes = allResults[10] as {
            data: Array<{
              timestamp: string;
              device_id: string;
              version: string;
              platform: string;
              code: string;
              context: string;
              uptime_sec: number;
            }>;
          };
          const displayDevicesRes = allResults[11] as { data: Array<{ affected_devices: number }> };
          const crashListRes = allResults[6] as {
            data: Array<{
              timestamp: string;
              device_id: string;
              ver: string;
              sig: string;
              platform: string;
              uptime_sec: number;
            }>;
          };

          const dedupedCrashes = deduplicateCrashes(crashListRes.data ?? []);

          // Compute crash aggregates from the deduplicated crash list
          // Crash count by date (from deduped crash timestamps)
          const crashByDate = new Map<string, number>();
          for (const row of dedupedCrashes) {
            const date = row.timestamp.slice(0, 10); // "2026-04-03T..." → "2026-04-03"
            crashByDate.set(date, (crashByDate.get(date) ?? 0) + 1);
          }

          // Merge with session dates for crash rate trend
          const sessionByDate = new Map<string, number>();
          for (const row of sessionTimeRes.data ?? []) {
            sessionByDate.set(row.date, row.session_count);
          }
          const allDates = new Set([...crashByDate.keys(), ...sessionByDate.keys()]);
          const crash_rate_trend = [...allDates].sort().map((date) => {
            const crashes = crashByDate.get(date) ?? 0;
            const sessions = sessionByDate.get(date) ?? 0;
            return { date, crashes, sessions, rate: sessions > 0 ? crashes / sessions : 0 };
          });

          // Crash count by version (from deduped list)
          const crashByVer = new Map<string, number>();
          for (const row of dedupedCrashes) {
            if (row.ver) crashByVer.set(row.ver, (crashByVer.get(row.ver) ?? 0) + 1);
          }
          const sessionVerMap = new Map<string, number>();
          for (const row of sessionByVerRes.data ?? []) {
            sessionVerMap.set(row.ver, row.session_count);
          }
          const by_version = [...crashByVer.entries()]
            .sort((a, b) => b[1] - a[1])
            .map(([ver, crashes]) => {
              const sessionCount = sessionVerMap.get(ver) ?? 0;
              return {
                version: ver,
                crash_count: crashes,
                session_count: sessionCount,
                rate: sessionCount > 0 ? crashes / sessionCount : 0,
              };
            });

          // Crashes by signal (from deduped list)
          const crashBySignal = new Map<string, number>();
          for (const row of dedupedCrashes) {
            if (row.sig) crashBySignal.set(row.sig, (crashBySignal.get(row.sig) ?? 0) + 1);
          }
          const by_signal = [...crashBySignal.entries()]
            .sort((a, b) => b[1] - a[1])
            .map(([signal, count]) => ({ signal, count }));

          // Average uptime (from deduped list)
          const avg_uptime_sec = dedupedCrashes.length > 0
            ? dedupedCrashes.reduce((sum, r) => sum + r.uptime_sec, 0) / dedupedCrashes.length
            : 0;

          return json({
            crash_rate_trend,
            by_version,
            by_signal,
            avg_uptime_sec,
            klippy_trend: (klippyRes.data ?? []).map((r) => ({
              date: r.date,
              errors: r.errors,
              shutdowns: r.shutdowns,
            })),
            memory_warnings_trend: (memWarnRes.data ?? []).map((r) => ({
              date: r.date,
              count: r.count,
            })),
            error_categories: (errorCatsRes.data ?? []).map((r) => ({
              category: r.category,
              count: r.count,
            })),
            error_codes: (errorCodesRes.data ?? []).map((r) => ({
              category: r.category,
              code: r.code,
              count: r.count,
            })),
            recent_crashes: dedupedCrashes.slice(0, displayLimit).map((r) => ({
              timestamp: r.timestamp,
              device_id: r.device_id,
              version: r.ver,
              signal: r.sig,
              platform: r.platform,
              uptime_sec: r.uptime_sec,
              occurrences: r.occurrences,
            })),
            display_anomalies: {
              affected_devices: displayDevicesRes.data?.[0]?.affected_devices ?? 0,
              trend: (displayTrendRes.data ?? []).map((r) => ({
                date: r.date,
                count: r.count,
              })),
              by_code: (displayCodesRes.data ?? []).map((r) => ({
                code: r.code,
                count: r.count,
              })),
              by_version: (displayByVerRes.data ?? []).map((r) => ({
                version: r.version,
                count: r.count,
              })),
              recent: (displayRecentRes.data ?? []).map((r) => ({
                timestamp: r.timestamp,
                device_id: r.device_id,
                version: r.version,
                platform: r.platform,
                code: r.code,
                context: r.context,
                uptime_sec: r.uptime_sec,
              })),
            },
          });
        }

        // GET /v1/dashboard/releases?versions=v0.9.18,v0.9.19
        if (url.pathname === "/v1/dashboard/releases") {
          const versionsParam = url.searchParams.get("versions");
          if (!versionsParam) {
            return json({ error: "versions parameter required" }, 400);
          }
          const versions = versionsParam.split(",").map((v) => v.trim()).filter(Boolean);
          if (versions.length === 0 || versions.length > 20) {
            return json({ error: "Provide 1-20 comma-separated versions" }, 400);
          }

          const queries = releasesQueries(versions);
          const [statsRes, devicesRes, printStatsRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const statsData = statsRes as {
            data: Array<{
              ver: string;
              total_sessions: number;
              total_crashes: number;
              update_failures: number;
              update_successes: number;
            }>;
          };
          const devicesData = devicesRes as {
            data: Array<{ ver: string; active_devices: number }>;
          };
          const printStatsData = printStatsRes as {
            data: Array<{ ver: string; print_successes: number; print_total: number }>;
          };

          // Build device count lookup
          const deviceMap = new Map<string, number>();
          for (const row of devicesData.data ?? []) {
            deviceMap.set(row.ver, row.active_devices);
          }

          // Build print stats lookup
          const printMap = new Map<string, { successes: number; total: number }>();
          for (const row of printStatsData.data ?? []) {
            printMap.set(row.ver, { successes: row.print_successes, total: row.print_total });
          }

          return json({
            versions: (statsData.data ?? []).map((r) => {
              const prints = printMap.get(r.ver) ?? { successes: 0, total: 0 };
              return {
                version: r.ver,
                active_devices: deviceMap.get(r.ver) ?? 0,
                crash_rate: r.total_sessions > 0 ? r.total_crashes / r.total_sessions : 0,
                print_success_rate: prints.total > 0 ? prints.successes / prints.total : 0,
                total_sessions: r.total_sessions,
                total_crashes: r.total_crashes,
                // Self-update health for devices RUNNING this version. A version
                // whose update_success_rate collapses is one that cannot upgrade
                // itself — the helixscreen#993 signature.
                update_failures: r.update_failures ?? 0,
                update_successes: r.update_successes ?? 0,
                update_success_rate:
                  (r.update_successes ?? 0) + (r.update_failures ?? 0) > 0
                    ? (r.update_successes ?? 0) /
                      ((r.update_successes ?? 0) + (r.update_failures ?? 0))
                    : null,
              };
            }),
          });
        }

        // GET /v1/dashboard/performance
        if (url.pathname === "/v1/dashboard/performance") {
          const queries = performanceQueries(days, filters);
          const [timeRes, platRes, verRes, worstRes, fleetRes, highDropRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const timeData = timeRes as { data: Array<{ date: string; avg_p50: number; avg_p95: number; avg_p99: number }> };
          const platData = platRes as { data: Array<{ platform: string; dropped: number; total: number }> };
          const verData = verRes as { data: Array<{ date: string; version: string; dropped: number; total: number }> };
          const worstData = worstRes as { data: Array<{ panel: string; times_worst: number; avg_p95_ms: number }> };
          const fleetData = fleetRes as { data: Array<{ fleet_p50: number; total_dropped: number; total_frames: number; total_devices: number }> };
          const highDropData = highDropRes as { data: Array<{ high_drop_devices: number }> };

          const fleet = fleetData.data?.[0] ?? { fleet_p50: 0, total_dropped: 0, total_frames: 0, total_devices: 0 };

          return json({
            fleet_p50_ms: Math.round(fleet.fleet_p50),
            fleet_drop_rate: fleet.total_frames > 0 ? fleet.total_dropped / fleet.total_frames : 0,
            high_drop_devices: highDropData.data?.[0]?.high_drop_devices ?? 0,
            total_devices: fleet.total_devices,
            worst_panel: worstData.data?.[0]?.panel ?? "",
            frame_time_trend: (timeData.data ?? []).map(r => ({
              date: r.date, p50: Math.round(r.avg_p50), p95: Math.round(r.avg_p95), p99: Math.round(r.avg_p99),
            })),
            drop_rate_by_platform: (platData.data ?? []).map(r => ({
              platform: r.platform, rate: r.total > 0 ? r.dropped / r.total : 0, dropped: r.dropped, total: r.total,
            })),
            drop_rate_by_version: (verData.data ?? []).map(r => ({
              date: r.date, version: r.version, rate: r.total > 0 ? r.dropped / r.total : 0,
            })),
            jankiest_panels: (worstData.data ?? []).map(r => ({
              panel: r.panel, times_worst: r.times_worst, avg_p95_ms: Math.round(r.avg_p95_ms),
            })),
          });
        }

        // GET /v1/dashboard/features
        if (url.pathname === "/v1/dashboard/features") {
          const queries = featuresQueries(days, filters);
          const [adoptionRes, byVersionRes, totalRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const adoptionData = adoptionRes as { data: Array<Record<string, number>> };
          const byVersionData = byVersionRes as { data: Array<Record<string, unknown>> };
          const totalData = totalRes as { data: Array<{ total_devices: number }> };

          const row = adoptionData.data?.[0] ?? {};
          const totalDevices = totalData.data?.[0]?.total_devices ?? 0;

          const featureNames = ["macros", "camera", "bed_mesh", "console_gcode",
            "input_shaper", "filament_management", "manual_probe"];

          const features = featureNames.map(name => ({
            name,
            adoption_rate: Number(row[name] ?? 0),
          }));

          return json({
            total_devices: totalDevices,
            features: features.sort((a, b) => b.adoption_rate - a.adoption_rate),
            by_version: (byVersionData.data ?? []).map(r => ({
              version: String(r.version ?? ""),
              devices: Number(r.devices ?? 0),
              macros: Number(r.macros ?? 0),
              camera: Number(r.camera ?? 0),
              bed_mesh: Number(r.bed_mesh ?? 0),
            })),
          });
        }

        // GET /v1/dashboard/ux
        if (url.pathname === "/v1/dashboard/ux") {
          const queries = uxInsightsQueries(days, filters);
          const [panelTimeRes, panelVisitRes, settingsFreqRes, settingsDefaultsRes,
                 totalDevicesRes, avgSessionRes, changeRateRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const panelTime = panelTimeRes as { data: Array<{ panel: string; total_time_sec: number }> };
          const panelVisits = panelVisitRes as { data: Array<{ panel: string; total_visits: number; devices: number }> };
          const settingsFreq = settingsFreqRes as { data: Array<{ setting: string; change_count: number }> };
          const settingsDefaults = settingsDefaultsRes as { data: Array<{ setting: string; devices_changed: number }> };
          const totalDevices = totalDevicesRes as { data: Array<{ total_devices: number }> };
          const avgSession = avgSessionRes as { data: Array<{ avg_session_sec: number }> };
          const changeRate = changeRateRes as { data: Array<{ total_changes: number; devices: number }> };

          const total = totalDevices.data?.[0]?.total_devices ?? 0;
          const cr = changeRate.data?.[0] ?? { total_changes: 0, devices: 0 };
          const panels = panelTime.data ?? [];
          const mostVisited = panels[0]?.panel ?? "";
          const leastVisited = panels.length > 0 ? panels[panels.length - 1].panel : "";

          return json({
            avg_session_sec: avgSession.data?.[0]?.avg_session_sec ?? 0,
            most_visited_panel: mostVisited,
            least_visited_panel: leastVisited,
            settings_change_rate_per_device_per_week:
              cr.devices > 0 ? (cr.total_changes / cr.devices) * (7 / Math.max(days, 1)) : 0,
            panel_time: (panelTime.data ?? []).map(r => ({ panel: r.panel, total_time_sec: r.total_time_sec })),
            panel_visits: (panelVisits.data ?? []).map(r => ({
              panel: r.panel, total_visits: r.total_visits, visits_per_device: r.devices > 0 ? r.total_visits / r.devices : 0,
            })),
            settings_changes: (settingsFreq.data ?? []).map(r => ({ setting: r.setting, change_count: r.change_count })),
            settings_defaults: (settingsDefaults.data ?? []).map(r => ({
              setting: r.setting,
              pct_changed: total > 0 ? r.devices_changed / total : 0,
              devices_changed: r.devices_changed,
            })),
          });
        }

        return json({ error: "Not found" }, 404);
      } catch (err) {
        const message = err instanceof Error ? err.message : "Unknown error";
        return json({ error: `Analytics query failed: ${message}` }, 502);
      }
    }

    // ---------- Backfill endpoint (admin only) ----------
    // POST /v1/admin/backfill — writes historical events to Analytics Engine
    if (url.pathname === "/v1/admin/backfill" && request.method === "POST") {
      const apiKey = request.headers.get("x-api-key") ?? "";
      if (!env.ADMIN_API_KEY || apiKey !== env.ADMIN_API_KEY) {
        return json({ error: "Unauthorized" }, 401);
      }

      if (!env.TELEMETRY_ANALYTICS) {
        return json({ error: "Analytics Engine not configured" }, 503);
      }

      const contentType = request.headers.get("content-type") ?? "";
      if (!contentType.includes("application/json")) {
        return json({ error: "Content-Type must be application/json" }, 400);
      }

      let body: unknown;
      try {
        body = await request.json();
      } catch {
        return json({ error: "Invalid JSON body" }, 400);
      }

      if (typeof body !== "object" || body === null || Array.isArray(body)) {
        return json({ error: "Body must be an object with events array" }, 400);
      }

      const events = (body as Record<string, unknown>).events;
      if (!Array.isArray(events)) {
        return json({ error: "events must be an array" }, 400);
      }

      if (events.length === 0) {
        return json({ error: "events array must not be empty" }, 400);
      }

      let written = 0;
      for (const evt of events) {
        try {
          const points = mapEventToDataPoints(evt as Record<string, unknown>);
          for (const point of points) {
            env.TELEMETRY_ANALYTICS.writeDataPoint(point);
            written++;
          }
        } catch {
          // Skip individual failures, continue backfilling
        }
      }

      return json({ status: "ok", written });
    }

    // Symbol map listing — returns available platforms for a version
    if (url.pathname.startsWith("/v1/symbols/")) {
      if (request.method !== "GET") {
        return json({ error: "Method not allowed" }, 405);
      }

      const version = url.pathname.replace("/v1/symbols/", "").replace(/\/+$/, "");
      if (!version || !/^[\d.]+[-\w.]*$/.test(version)) {
        return json({ error: "Invalid version format" }, 400);
      }

      const prefix = `symbols/v${version}/`;
      const listed = await env.TELEMETRY_BUCKET.list({ prefix });
      const platforms = listed.objects
        .map((obj) => obj.key.replace(prefix, "").replace(/\.sym$/, ""))
        .filter((p) => p.length > 0);

      return json({ version, platforms });
    }

    // CDN fleet snapshot - manual run or backfill within the retention window.
    // POST /v1/admin/cdn-snapshot?date=YYYY-MM-DD   (one specific UTC day)
    // POST /v1/admin/cdn-snapshot?days=3            (the last N closed days, max 3)
    // Requires ADMIN_API_KEY. Re-running a day is safe: reads aggregate with
    // max(), so duplicate rows collapse rather than double-count.
    if (url.pathname === "/v1/admin/cdn-snapshot" && request.method === "POST") {
      const apiKey = request.headers.get("x-api-key") ?? "";
      if (!env.ADMIN_API_KEY || apiKey !== env.ADMIN_API_KEY) {
        return json({ error: "Unauthorized" }, 401);
      }

      const dateParam = url.searchParams.get("date");
      const dates: string[] = [];

      if (dateParam) {
        if (!/^\d{4}-\d{2}-\d{2}$/.test(dateParam)) {
          return json({ error: "date must be YYYY-MM-DD" }, 400);
        }
        dates.push(dateParam);
      } else {
        // Clamped to 3 days, not the ~8 the retention window allows.
        //
        // Analytics Engine silently drops writeDataPoint calls past a ceiling
        // within a single invocation. Measured 2026-08-15: a days=8 backfill
        // (8 days x 3 channels = 24 points) persisted only the first four days
        // and reported all eight as captured, because the R2 write and the
        // snapshot both succeeded - only the AE write was dropped, and that one
        // is deliberately fire-and-forget. Re-running the missing days one
        // request at a time persisted them fine.
        //
        // 3 days x 3 channels = 9 points stays well inside the ceiling. For a
        // longer backfill, call this repeatedly rather than raising the clamp.
        const n = Math.min(Math.max(parseInt(url.searchParams.get("days") ?? "1", 10) || 1, 1), 3);
        const now = new Date();
        for (let i = 1; i <= n; i++) {
          dates.push(utcDay(new Date(Date.UTC(
            now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - i))));
        }
      }

      const captured: FleetDaySnapshot[] = [];
      const failed: Array<{ date: string; error: string }> = [];
      for (const d of dates) {
        try {
          captured.push(await captureCdnFleetDay(env, d));
        } catch (e) {
          failed.push({ date: d, error: e instanceof Error ? e.message : String(e) });
        }
      }

      return json({ status: "ok", captured, failed }, failed.length && !captured.length ? 500 : 200);
    }

    // Everything else
    return json({ error: "Not found" }, 404);
  },

  /**
   * Daily CDN fleet snapshot (see wrangler.toml [triggers]).
   *
   * Snapshots the UTC day that just closed. Zone analytics retain roughly 8
   * days, so a missed run is recoverable via POST /v1/admin/cdn-snapshot for
   * a few days and unrecoverable after that.
   *
   * Failures are deliberately NOT caught. An expired or re-scoped token would
   * otherwise stop the metric silently and the tile would simply stop advancing
   * with nothing to explain why - which is the exact failure mode this whole
   * metric exists to avoid. Letting it reject marks the cron invocation as
   * errored in Cloudflare's observability, where it can be seen.
   */
  async scheduled(_event: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    const date = previousUtcDay(new Date());
    ctx.waitUntil(captureCdnFleetDay(env, date));
  },
} satisfies ExportedHandler<Env>;
