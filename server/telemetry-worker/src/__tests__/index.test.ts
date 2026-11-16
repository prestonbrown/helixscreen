// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for telemetry worker endpoints.

import { describe, it, expect, vi, beforeEach } from "vitest";
import worker, { CDN_CRON, LOOP_CRON, configuredFeature, deriveFeatureAdoption, toYesNo } from "../index";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { col } from "../schema";
import { mapEventToDataPoints } from "../analytics";
import * as queries from "../queries";
import { parseRange } from "../queries";
import { buildReconcileTable, countR2Points, parseEnvValue } from "../../scripts/reconcile";
import { compareVersions } from "../versions";
import { compareVersions as dashboardCompareVersions } from "../../../analytics-dashboard/src/utils/chart";

// Mock the executeQuery function so dashboard tests don't hit real Cloudflare API
vi.mock("../queries", async (importOriginal) => {
  const actual = await importOriginal() as Record<string, unknown>;
  return {
    ...actual,
    executeQuery: vi.fn(),
  };
});

import { executeQuery } from "../queries";
const mockExecuteQuery = vi.mocked(executeQuery);

// ---------- Mock R2 helpers ----------

interface StoredObject {
  key: string;
  body: string;
  size: number;
  uploaded: Date;
  httpMetadata?: Record<string, string>;
}

function createMockBucket() {
  const storage = new Map<string, StoredObject>();

  return {
    _storage: storage,

    async put(key: string, value: string, opts?: { httpMetadata?: Record<string, string> }) {
      storage.set(key, {
        key,
        body: value,
        size: value.length,
        uploaded: new Date(),
        httpMetadata: opts?.httpMetadata,
      });
    },

    async get(key: string) {
      const obj = storage.get(key);
      if (!obj) return null;
      return {
        key: obj.key,
        size: obj.size,
        uploaded: obj.uploaded,
        text: async () => obj.body,
        body: new ReadableStream({
          start(controller) {
            controller.enqueue(new TextEncoder().encode(obj.body));
            controller.close();
          },
        }),
      };
    },

    async list(opts?: { prefix?: string; cursor?: string; limit?: number }) {
      const prefix = opts?.prefix ?? "";
      const limit = opts?.limit ?? 1000;
      const objects = Array.from(storage.values())
        .filter((o) => o.key.startsWith(prefix))
        .slice(0, limit)
        .map((o) => ({ key: o.key, size: o.size, uploaded: o.uploaded }));
      return { objects, truncated: false, cursor: undefined };
    },
  };
}

// ---------- Mock Analytics Engine ----------

function createMockAnalyticsEngine() {
  const points: Array<{ blobs?: string[]; doubles?: number[]; indexes?: string[] }> = [];
  return {
    _points: points,
    writeDataPoint: vi.fn((point: { blobs?: string[]; doubles?: number[]; indexes?: string[] }) => {
      points.push(point);
    }),
  };
}

// ---------- Test env factory ----------

function createMockRateLimiter(shouldLimit = false) {
  return {
    limit: async (_opts: { key: string }) => ({ success: !shouldLimit }),
  };
}

function createEnv(overrides?: Partial<{
  INGEST_API_KEY: string;
  ADMIN_API_KEY: string;
  TELEMETRY_BUCKET: ReturnType<typeof createMockBucket>;
  INGEST_LIMITER: ReturnType<typeof createMockRateLimiter>;
  TELEMETRY_ANALYTICS: ReturnType<typeof createMockAnalyticsEngine>;
  CLOUDFLARE_ACCOUNT_ID: string;
  HELIX_ANALYTICS_READ_TOKEN: string;
}>) {
  return {
    INGEST_API_KEY: "test-ingest-key",
    ADMIN_API_KEY: "test-admin-key",
    TELEMETRY_BUCKET: createMockBucket(),
    INGEST_LIMITER: createMockRateLimiter(),
    TELEMETRY_ANALYTICS: createMockAnalyticsEngine(),
    CLOUDFLARE_ACCOUNT_ID: "test-account-id",
    HELIX_ANALYTICS_READ_TOKEN: "test-api-token",
    ...overrides,
  };
}

// ---------- Request helpers ----------

function makeRequest(url: string, init?: RequestInit): Request {
  return new Request(`https://telemetry.helixscreen.org${url}`, init);
}

function validEvent(overrides?: Record<string, unknown>) {
  return {
    schema_version: 1,
    event: "app.started",
    device_id: "device-abc-123",
    timestamp: "2026-01-15T10:30:00Z",
    ...overrides,
  };
}

function ingestRequest(body: unknown, apiKey = "test-ingest-key") {
  return makeRequest("/v1/events", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-API-Key": apiKey,
    },
    body: JSON.stringify(body),
  });
}

// ---------- Tests ----------

describe("GET /health", () => {
  it("returns healthy status", async () => {
    const res = await worker.fetch(makeRequest("/health"), createEnv());
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data).toEqual({ status: "healthy", git_sha: "unknown" });
  });

  it("reports the deployed git sha", async () => {
    const res = await worker.fetch(makeRequest("/health"), { ...createEnv(), GIT_SHA: "abc1234" });
    expect(await res.json()).toEqual({ status: "healthy", git_sha: "abc1234" });
  });

  it("includes CORS headers", async () => {
    const res = await worker.fetch(makeRequest("/health"), createEnv());
    expect(res.headers.get("Access-Control-Allow-Origin")).toBe("*");
  });
});

describe("OPTIONS (CORS preflight)", () => {
  it("returns 204 with CORS headers", async () => {
    const res = await worker.fetch(
      makeRequest("/v1/events", { method: "OPTIONS" }),
      createEnv(),
    );
    expect(res.status).toBe(204);
    expect(res.headers.get("Access-Control-Allow-Methods")).toContain("POST");
    expect(res.headers.get("Access-Control-Allow-Headers")).toContain("X-API-Key");
  });

  it("works on any path", async () => {
    const res = await worker.fetch(
      makeRequest("/any/random/path", { method: "OPTIONS" }),
      createEnv(),
    );
    expect(res.status).toBe(204);
  });
});

describe("404 for unknown routes", () => {
  it("returns 404 for unknown GET path", async () => {
    const res = await worker.fetch(makeRequest("/nonexistent"), createEnv());
    expect(res.status).toBe(404);
    const data = await res.json();
    expect(data.error).toBe("Not found");
  });
});

describe("POST /v1/events (ingestion)", () => {
  let env: ReturnType<typeof createEnv>;

  beforeEach(() => {
    env = createEnv();
  });

  // -- Auth --

  it("rejects request with no API key", async () => {
    const req = makeRequest("/v1/events", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify([validEvent()]),
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(401);
  });

  it("rejects request with wrong API key", async () => {
    const res = await worker.fetch(ingestRequest([validEvent()], "wrong-key"), env);
    expect(res.status).toBe(401);
  });

  it("rejects ADMIN_API_KEY on ingest endpoint", async () => {
    const res = await worker.fetch(ingestRequest([validEvent()], "test-admin-key"), env);
    expect(res.status).toBe(401);
  });

  // -- Rate limiting --

  it("returns 429 when rate limited", async () => {
    const limitedEnv = createEnv({ INGEST_LIMITER: createMockRateLimiter(true) });
    const res = await worker.fetch(ingestRequest([validEvent()]), limitedEnv);
    expect(res.status).toBe(429);
    const body = await res.json() as { error: string };
    expect(body.error).toContain("Rate limit");
  });

  // -- Method --

  it("rejects GET method", async () => {
    const req = makeRequest("/v1/events", {
      method: "GET",
      headers: { "X-API-Key": "test-ingest-key" },
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(405);
  });

  // -- Content-Type --

  it("rejects non-JSON content type", async () => {
    const req = makeRequest("/v1/events", {
      method: "POST",
      headers: {
        "Content-Type": "text/plain",
        "X-API-Key": "test-ingest-key",
      },
      body: JSON.stringify([validEvent()]),
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("application/json");
  });

  // -- Body validation --

  it("rejects invalid JSON", async () => {
    const req = makeRequest("/v1/events", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-API-Key": "test-ingest-key",
      },
      body: "not json{{{",
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("Invalid JSON");
  });

  it("rejects non-array body", async () => {
    const res = await worker.fetch(ingestRequest({ not: "an array" }), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("array");
  });

  it("rejects empty array", async () => {
    const res = await worker.fetch(ingestRequest([]), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("1-20");
  });

  it("rejects batch larger than 20", async () => {
    const events = Array.from({ length: 21 }, () => validEvent());
    const res = await worker.fetch(ingestRequest(events), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("1-20");
  });

  // -- Schema validation --

  it("rejects event missing schema_version", async () => {
    const res = await worker.fetch(
      ingestRequest([validEvent({ schema_version: undefined })]),
      env,
    );
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("schema_version");
  });

  it("rejects event with non-number schema_version", async () => {
    const res = await worker.fetch(
      ingestRequest([validEvent({ schema_version: "1" })]),
      env,
    );
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("schema_version");
  });

  it("rejects event missing event name", async () => {
    const res = await worker.fetch(
      ingestRequest([validEvent({ event: "" })]),
      env,
    );
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("event");
  });

  it("rejects event missing device_id", async () => {
    const res = await worker.fetch(
      ingestRequest([validEvent({ device_id: "" })]),
      env,
    );
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("device_id");
  });

  it("rejects event missing timestamp", async () => {
    const res = await worker.fetch(
      ingestRequest([validEvent({ timestamp: "" })]),
      env,
    );
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("timestamp");
  });

  it("rejects non-object event (string)", async () => {
    const res = await worker.fetch(ingestRequest(["not an object"]), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("must be an object");
  });

  it("rejects null event in array", async () => {
    const res = await worker.fetch(ingestRequest([null]), env);
    expect(res.status).toBe(400);
  });

  it("reports correct index for bad event in batch", async () => {
    const events = [validEvent(), validEvent({ event: "" })];
    const res = await worker.fetch(ingestRequest(events), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("event[1]");
  });

  // -- Happy path --

  it("stores a single valid event and returns ok", async () => {
    const res = await worker.fetch(ingestRequest([validEvent()]), env);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.status).toBe("ok");
    expect(data.stored).toBe(1);
    // Verify something was stored in the mock bucket
    expect(env.TELEMETRY_BUCKET._storage.size).toBe(1);
  });

  it("stores batch of 20 events", async () => {
    const events = Array.from({ length: 20 }, (_, i) =>
      validEvent({ event: `event.${i}` }),
    );
    const res = await worker.fetch(ingestRequest(events), env);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.stored).toBe(20);
  });

  it("stores events under events/ prefix with .json extension", async () => {
    await worker.fetch(ingestRequest([validEvent()]), env);
    const keys = Array.from(env.TELEMETRY_BUCKET._storage.keys());
    expect(keys).toHaveLength(1);
    expect(keys[0]).toMatch(/^events\/\d{4}\/\d{2}\/\d{2}\/\d+-[0-9a-f]+\.json$/);
  });

  it("stores the event payload as JSON in R2", async () => {
    const event = validEvent({ event: "print.started" });
    await worker.fetch(ingestRequest([event]), env);
    const stored = Array.from(env.TELEMETRY_BUCKET._storage.values())[0];
    const parsed = JSON.parse(stored.body);
    expect(parsed).toEqual([event]);
  });

  it("returns 500 when R2 put fails", async () => {
    const failBucket = createMockBucket();
    failBucket.put = vi.fn().mockRejectedValue(new Error("R2 down"));
    const failEnv = createEnv({ TELEMETRY_BUCKET: failBucket });
    const res = await worker.fetch(ingestRequest([validEvent()]), failEnv);
    expect(res.status).toBe(500);
    const data = await res.json();
    expect(data.error).toContain("Failed to store");
  });
});

describe("GET /v1/events/list (admin listing)", () => {
  let env: ReturnType<typeof createEnv>;

  beforeEach(() => {
    env = createEnv();
  });

  function listRequest(params = "", apiKey = "test-admin-key") {
    return makeRequest(`/v1/events/list${params ? "?" + params : ""}`, {
      headers: { "X-API-Key": apiKey },
    });
  }

  // -- Auth --

  it("rejects request with no API key", async () => {
    const req = makeRequest("/v1/events/list");
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(401);
  });

  it("rejects wrong API key", async () => {
    const res = await worker.fetch(listRequest("", "wrong-key"), env);
    expect(res.status).toBe(401);
  });

  it("rejects INGEST_API_KEY on list endpoint", async () => {
    const res = await worker.fetch(listRequest("", "test-ingest-key"), env);
    expect(res.status).toBe(401);
  });

  // -- Prefix validation --

  it("rejects prefix not starting with events/", async () => {
    const res = await worker.fetch(listRequest("prefix=secrets/"), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("events/");
  });

  it("rejects directory traversal prefix", async () => {
    const res = await worker.fetch(listRequest("prefix=../etc/"), env);
    expect(res.status).toBe(400);
  });

  // -- Happy path --

  it("returns empty list when no objects exist", async () => {
    const res = await worker.fetch(listRequest(), env);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.keys).toEqual([]);
    expect(data.truncated).toBe(false);
  });

  it("lists stored events with key, size, uploaded fields", async () => {
    // Store an event first
    await worker.fetch(ingestRequest([validEvent()]), env);

    const res = await worker.fetch(listRequest("prefix=events/"), env);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.keys).toHaveLength(1);
    expect(data.keys[0]).toHaveProperty("key");
    expect(data.keys[0]).toHaveProperty("size");
    expect(data.keys[0]).toHaveProperty("uploaded");
    expect(data.keys[0].key).toMatch(/^events\//);
  });

  it("defaults prefix to events/ when not provided", async () => {
    await worker.fetch(ingestRequest([validEvent()]), env);
    const res = await worker.fetch(listRequest(), env);
    const data = await res.json();
    expect(data.keys).toHaveLength(1);
  });

  it("passes limit parameter to R2 list", async () => {
    const listSpy = vi.spyOn(env.TELEMETRY_BUCKET, "list");
    await worker.fetch(listRequest("limit=5"), env);
    expect(listSpy).toHaveBeenCalledWith(
      expect.objectContaining({ limit: 5 }),
    );
  });

  it("clamps limit to 1-1000 range", async () => {
    const listSpy = vi.spyOn(env.TELEMETRY_BUCKET, "list");

    // Over 1000 gets clamped to 1000
    await worker.fetch(listRequest("limit=5000"), env);
    expect(listSpy).toHaveBeenCalledWith(
      expect.objectContaining({ limit: 1000 }),
    );

    listSpy.mockClear();

    // 0 is falsy so falls through to default 1000 via || operator
    await worker.fetch(listRequest("limit=0"), env);
    expect(listSpy).toHaveBeenCalledWith(
      expect.objectContaining({ limit: 1000 }),
    );

    listSpy.mockClear();

    // Negative values get clamped to 1 via Math.max
    await worker.fetch(listRequest("limit=-5"), env);
    expect(listSpy).toHaveBeenCalledWith(
      expect.objectContaining({ limit: 1 }),
    );
  });
});

describe("GET /v1/events/get (admin download)", () => {
  let env: ReturnType<typeof createEnv>;

  beforeEach(() => {
    env = createEnv();
  });

  function getRequest(params = "", apiKey = "test-admin-key") {
    return makeRequest(`/v1/events/get${params ? "?" + params : ""}`, {
      headers: { "X-API-Key": apiKey },
    });
  }

  // -- Auth --

  it("rejects request with no API key", async () => {
    const req = makeRequest("/v1/events/get?key=events/2026/01/15/test.json");
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(401);
  });

  it("rejects wrong API key", async () => {
    const res = await worker.fetch(
      getRequest("key=events/2026/01/15/test.json", "wrong-key"),
      env,
    );
    expect(res.status).toBe(401);
  });

  it("rejects INGEST_API_KEY on get endpoint", async () => {
    const res = await worker.fetch(
      getRequest("key=events/2026/01/15/test.json", "test-ingest-key"),
      env,
    );
    expect(res.status).toBe(401);
  });

  // -- Key validation --

  it("rejects missing key parameter", async () => {
    const res = await worker.fetch(getRequest(), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("Invalid key");
  });

  it("rejects key not starting with events/", async () => {
    const res = await worker.fetch(getRequest("key=secrets/data.json"), env);
    expect(res.status).toBe(400);
  });

  it("rejects key not ending with .json", async () => {
    const res = await worker.fetch(getRequest("key=events/2026/01/data.txt"), env);
    expect(res.status).toBe(400);
  });

  it("rejects key that starts with events/ but does not end with .json", async () => {
    const res = await worker.fetch(getRequest("key=events/2026/01/15/data"), env);
    expect(res.status).toBe(400);
  });

  // -- Happy path --

  it("returns stored event data", async () => {
    // Store an event first
    await worker.fetch(ingestRequest([validEvent()]), env);
    const storedKey = Array.from(env.TELEMETRY_BUCKET._storage.keys())[0];

    const res = await worker.fetch(getRequest(`key=${storedKey}`), env);
    expect(res.status).toBe(200);
    expect(res.headers.get("Content-Type")).toBe("application/json");

    const body = await res.text();
    const parsed = JSON.parse(body);
    expect(parsed).toEqual([validEvent()]);
  });

  it("returns 404 for non-existent key", async () => {
    const res = await worker.fetch(
      getRequest("key=events/2026/01/15/nonexistent.json"),
      env,
    );
    expect(res.status).toBe(404);
    const data = await res.json();
    expect(data.error).toBe("Not found");
  });

  it("includes CORS headers on response", async () => {
    await worker.fetch(ingestRequest([validEvent()]), env);
    const storedKey = Array.from(env.TELEMETRY_BUCKET._storage.keys())[0];
    const res = await worker.fetch(getRequest(`key=${storedKey}`), env);
    expect(res.headers.get("Access-Control-Allow-Origin")).toBe("*");
  });
});

describe("GET /v1/symbols/:version", () => {
  let env: ReturnType<typeof createEnv>;

  beforeEach(() => {
    env = createEnv();
  });

  it("returns platforms for a valid version", async () => {
    // Pre-populate symbol files
    await env.TELEMETRY_BUCKET.put("symbols/v1.2.3/linux-arm.sym", "data");
    await env.TELEMETRY_BUCKET.put("symbols/v1.2.3/linux-x86_64.sym", "data");

    const res = await worker.fetch(makeRequest("/v1/symbols/1.2.3"), env);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.version).toBe("1.2.3");
    expect(data.platforms).toContain("linux-arm");
    expect(data.platforms).toContain("linux-x86_64");
  });

  it("returns empty platforms when no symbols exist", async () => {
    const res = await worker.fetch(makeRequest("/v1/symbols/9.9.9"), env);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.platforms).toEqual([]);
  });

  it("rejects invalid version format", async () => {
    // Use a version string that fails the regex but stays in the path
    const res = await worker.fetch(makeRequest("/v1/symbols/bad version!@#"), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("Invalid version");
  });

  it("rejects non-GET method", async () => {
    const res = await worker.fetch(
      makeRequest("/v1/symbols/1.0.0", { method: "POST" }),
      env,
    );
    expect(res.status).toBe(405);
  });
});

describe("Cross-key isolation", () => {
  const env = createEnv();

  it("ADMIN key cannot ingest events", async () => {
    const res = await worker.fetch(ingestRequest([validEvent()], "test-admin-key"), env);
    expect(res.status).toBe(401);
  });

  it("INGEST key cannot list events", async () => {
    const req = makeRequest("/v1/events/list", {
      headers: { "X-API-Key": "test-ingest-key" },
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(401);
  });

  it("INGEST key cannot download events", async () => {
    const req = makeRequest("/v1/events/get?key=events/2026/01/15/test.json", {
      headers: { "X-API-Key": "test-ingest-key" },
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(401);
  });
});

// ---------- Analytics Engine dual-write ----------

describe("Analytics Engine dual-write", () => {
  let env: ReturnType<typeof createEnv>;

  beforeEach(() => {
    env = createEnv();
  });

  it("writes each event to Analytics Engine after R2 store", async () => {
    const events = [validEvent({ event: "session" }), validEvent({ event: "crash" })];
    const res = await worker.fetch(ingestRequest(events), env);
    expect(res.status).toBe(200);
    expect(env.TELEMETRY_ANALYTICS.writeDataPoint).toHaveBeenCalledTimes(2);
  });

  it("does not write to Analytics Engine when R2 fails", async () => {
    const failBucket = createMockBucket();
    failBucket.put = vi.fn().mockRejectedValue(new Error("R2 down"));
    const failEnv = createEnv({ TELEMETRY_BUCKET: failBucket });
    const res = await worker.fetch(ingestRequest([validEvent()]), failEnv);
    expect(res.status).toBe(500);
    expect(failEnv.TELEMETRY_ANALYTICS.writeDataPoint).not.toHaveBeenCalled();
  });

  it("still returns ok when Analytics Engine throws", async () => {
    const failAnalytics = createMockAnalyticsEngine();
    failAnalytics.writeDataPoint = vi.fn(() => {
      throw new Error("Analytics Engine down");
    });
    const failEnv = createEnv({ TELEMETRY_ANALYTICS: failAnalytics });
    const res = await worker.fetch(ingestRequest([validEvent()]), failEnv);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.status).toBe("ok");
  });

  it("works when TELEMETRY_ANALYTICS binding is missing", async () => {
    const noAnalyticsEnv = createEnv();
    // Remove the analytics binding to simulate it not being configured
    (noAnalyticsEnv as Record<string, unknown>).TELEMETRY_ANALYTICS = undefined;
    const res = await worker.fetch(ingestRequest([validEvent()]), noAnalyticsEnv);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.status).toBe("ok");
  });
});

// ---------- mapEventToDataPoints unit tests ----------

describe("mapEventToDataPoints", () => {
  it("maps session event correctly", () => {
    const event = {
      event: "session",
      device_id: "dev-123",
      version: "0.9.19",
      platform: "linux-arm",
      printer_model: "Voron 2.4",
      kinematics: "corexy",
      display: "HDMI-800x480",
      locale: "en_US",
      theme: "dark",
      arch: "armv7l",
      ram_total_mb: 1024,
      cpu_cores: 4,
      extruder_count: 1,
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["session"]);
    expect(dp.blobs![0]).toBe("dev-123");
    expect(dp.blobs![1]).toBe("0.9.19");
    expect(dp.blobs![2]).toBe("linux-arm");
    expect(dp.blobs![3]).toBe("Voron 2.4");
    expect(dp.blobs![4]).toBe("corexy");
    expect(dp.blobs).toHaveLength(14);
    expect(dp.doubles![0]).toBe(1024);
    expect(dp.doubles![1]).toBe(4);
    expect(dp.doubles![2]).toBe(1);
    expect(dp.doubles).toHaveLength(8);
  });

  it("maps print_outcome event correctly", () => {
    const event = {
      event: "print_outcome",
      device_id: "dev-456",
      outcome: "success",
      filament_type: "PLA",
      version: "0.9.19",
      duration_sec: 3600,
      nozzle_temp: 210,
      bed_temp: 60,
      filament_used_mm: 15000,
      phases_completed: 8,
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["print_outcome"]);
    expect(dp.blobs![0]).toBe("dev-456");
    expect(dp.blobs![1]).toBe("success");
    expect(dp.blobs![2]).toBe("PLA");
    expect(dp.blobs![3]).toBe("0.9.19");
    expect(dp.blobs).toHaveLength(14);
    expect(dp.doubles![0]).toBe(3600);
    expect(dp.doubles![1]).toBe(210);
    expect(dp.doubles![2]).toBe(60);
    expect(dp.doubles![3]).toBe(15000);
    expect(dp.doubles![4]).toBe(8);
    expect(dp.doubles).toHaveLength(8);
  });

  it("maps crash event correctly", () => {
    const event = {
      event: "crash",
      device_id: "dev-789",
      version: "0.9.18",
      signal_name: "SIGSEGV",
      platform: "linux-arm",
      uptime_sec: 7200,
      signal_number: 11,
      backtrace_depth: 15,
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["crash"]);
    expect(dp.blobs![0]).toBe("dev-789");
    expect(dp.blobs![1]).toBe("0.9.18");
    expect(dp.blobs![2]).toBe("SIGSEGV");
    expect(dp.blobs![3]).toBe("linux-arm");
    expect(dp.blobs).toHaveLength(12);
    expect(dp.doubles![0]).toBe(7200);
    expect(dp.doubles![1]).toBe(11);
    expect(dp.doubles![2]).toBe(15);
    expect(dp.doubles).toHaveLength(8);
  });

  it("maps crash app_board into blob8 and keeps the other crash slots", () => {
    const event = {
      event: "crash",
      device_id: "dev-crash",
      app_version: "1.1.0",
      signal_name: "SIGSEGV",
      app_platform: "mips",
      app_board: "creator5-pro",
      fault_addr: "0x10",
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![7]).toBe("creator5-pro");
    expect(dp.blobs![2]).toBe("SIGSEGV");
    expect(dp.blobs![3]).toBe("mips");
    expect(dp.blobs![6]).toBe("0x10");
    expect(dp.blobs).toHaveLength(12);
    const old = mapEventToDataPoints({ event: "crash", device_id: "d" })[0];
    expect(old.blobs![7]).toBe("");
  });

  it("maps session zip_tool into blob10", () => {
    const event = {
      event: "session",
      device_id: "dev-zip",
      app: { version: "0.99.102", platform: "k2", zip_tool: "python" },
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![9]).toBe("python");
    expect(dp.blobs).toHaveLength(14);
  });

  it("maps session app.board into blob11", () => {
    const event = {
      event: "session",
      device_id: "dev-board",
      app: { version: "1.1.0", platform: "mips", board: "creator5-pro" },
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![10]).toBe("creator5-pro");
    expect(dp.blobs![2]).toBe("mips");
    expect(dp.blobs).toHaveLength(14);
  });

  it("leaves session board empty for clients that do not send it", () => {
    const event = { event: "session", device_id: "dev-old", app: { version: "1.0.5" } };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![10]).toBe("");
  });

  it("leaves session zip_tool empty for clients that do not send it", () => {
    const event = { event: "session", device_id: "dev-old", app: { version: "0.99.90" } };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![9]).toBe("");
  });

  it("keeps the established hardware_profile slots in place", () => {
    // Guards the additions below from shifting anything the dashboard reads.
    const event = {
      event: "hardware_profile",
      device_id: "dev-hw",
      app: { version: "0.99.110", platform: "k1" },
      printer: { detected_model: "Voron 2.4", kinematics: "corexy" },
      mcus: { primary: "stm32f446" },
      probe: { type: "beacon", has_probe: true },
      ams: { type: "afc" },
      macros: { total_count: 42 },
      extruders: { count: 1 },
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["hardware_profile"]);
    expect(dp.blobs![0]).toBe("dev-hw");
    expect(dp.blobs![1]).toBe("0.99.110");
    expect(dp.blobs![2]).toBe("k1");
    expect(dp.blobs![3]).toBe("Voron 2.4");
    expect(dp.blobs![4]).toBe("corexy");
    expect(dp.blobs![5]).toBe("stm32f446");
    expect(dp.blobs![6]).toBe("beacon");
    expect(dp.blobs![7]).toBe("afc");
    expect(dp.blobs).toHaveLength(13);
    expect(dp.doubles![3]).toBe(42);
    expect(dp.doubles).toHaveLength(8);
  });

  it("maps hardware_profile has_helix_macros into blob9", () => {
    const event = {
      event: "hardware_profile",
      device_id: "dev-hm",
      macros: { total_count: 30, has_helix_macros: true },
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![8]).toBe("1");
  });

  it("distinguishes has_helix_macros false from a client that never reports it", () => {
    // A bitmask bit could not express this: absent and false would both read 0,
    // so every pre-existing event would look like a printer without the macros.
    const withoutMacros = mapEventToDataPoints({
      event: "hardware_profile",
      device_id: "dev-hm-false",
      macros: { total_count: 30, has_helix_macros: false },
    })[0];
    expect(withoutMacros.blobs![8]).toBe("0");

    const notReported = mapEventToDataPoints({
      event: "hardware_profile",
      device_id: "dev-hm-absent",
      macros: { total_count: 30 },
    })[0];
    expect(notReported.blobs![8]).toBe("");

    const noMacrosSection = mapEventToDataPoints({
      event: "hardware_profile",
      device_id: "dev-hm-none",
    })[0];
    expect(noMacrosSection.blobs![8]).toBe("");
  });

  it("maps moonraker_is_local into blob10", () => {
    const local = mapEventToDataPoints({
      event: "hardware_profile",
      device_id: "dev-local",
      moonraker_is_local: true,
    })[0];
    expect(local.blobs![9]).toBe("1");

    const remote = mapEventToDataPoints({
      event: "hardware_profile",
      device_id: "dev-remote",
      moonraker_is_local: false,
    })[0];
    expect(remote.blobs![9]).toBe("0");
  });

  it("leaves moonraker_is_local empty for clients that predate the field", () => {
    // Every release before this one. Without the empty case they would all be
    // counted as remote, which is the wrong answer in the safe direction.
    const dp = mapEventToDataPoints({
      event: "hardware_profile",
      device_id: "dev-old-hw",
      app: { version: "0.99.110" },
    })[0];
    expect(dp.blobs![9]).toBe("");
  });

  it("maps update_failed event to real slots, not the unknown fallback", () => {
    const event = {
      event: "update_failed",
      device_id: "dev-uf",
      reason: "corrupt_download",
      version: "0.99.103",
      from_version: "0.99.100",
      platform: "ad5m",
      http_code: 200,
      file_size: 1048576,
      exit_code: 127,
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["update_failed"]);
    expect(dp.blobs![0]).toBe("dev-uf");
    // blob2 is the RUNNING version, not the target. The default dashboard
    // filter maps version=blob2, and the target is always the newest release —
    // keying on it would collapse every failure into one bucket. What we need
    // is which deployed versions are failing (helixscreen#993).
    expect(dp.blobs![1]).toBe("0.99.100");
    expect(dp.blobs![2]).toBe("ad5m");
    expect(dp.blobs![3]).toBe("corrupt_download");
    expect(dp.blobs![4]).toBe("0.99.103"); // target
    expect(dp.blobs).toHaveLength(12);
    expect(dp.doubles![0]).toBe(200);
    expect(dp.doubles![1]).toBe(1048576);
    expect(dp.doubles![2]).toBe(127);
    expect(dp.doubles).toHaveLength(8);
    // The unknown-event fallback would have put the event name in blob2.
    expect(dp.blobs![1]).not.toBe("update_failed");
  });

  it("maps update_success event to real slots, not the unknown fallback", () => {
    const event = {
      event: "update_success",
      device_id: "dev-us",
      version: "0.99.103",
      from_version: "0.99.100",
      platform: "pi",
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["update_success"]);
    expect(dp.blobs![0]).toBe("dev-us");
    expect(dp.blobs![1]).toBe("0.99.103");
    expect(dp.blobs![2]).toBe("pi");
    expect(dp.blobs![3]).toBe("0.99.100");
    expect(dp.blobs).toHaveLength(12);
    expect(dp.doubles).toHaveLength(8);
    expect(dp.blobs![1]).not.toBe("update_success");
  });

  it("maps unknown event type with basic info", () => {
    const event = {
      event: "custom_thing",
      device_id: "dev-abc",
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["custom_thing"]);
    expect(dp.blobs![0]).toBe("dev-abc");
    expect(dp.blobs![1]).toBe("custom_thing");
    expect(dp.blobs).toHaveLength(12);
    expect(dp.doubles).toHaveLength(8);
  });

  it("maps crash event with flat app_version field (schema v2)", () => {
    const event = {
      event: "crash",
      device_id: "dev-crash",
      app_version: "0.9.9",
      signal_name: "SIGSEGV",
      app_platform: "pi",
      uptime_sec: 325,
      signal_number: 11,
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["crash"]);
    expect(dp.blobs![1]).toBe("0.9.9");
    expect(dp.blobs![2]).toBe("SIGSEGV");
    expect(dp.blobs![3]).toBe("pi");
    expect(dp.doubles![0]).toBe(325);
  });

  it("maps session event with nested app object (schema v2)", () => {
    const event = {
      event: "session",
      device_id: "dev-nested",
      app: { version: "0.9.10", platform: "pi", display: "800x480" },
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![1]).toBe("0.9.10");
    expect(dp.blobs![2]).toBe("pi");
  });

  it("prefers nested app.version over flat app_version", () => {
    const event = {
      event: "crash",
      device_id: "dev-both",
      app: { version: "nested" },
      app_version: "flat",
      signal_name: "SIGABRT",
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![1]).toBe("nested");
  });

  it("maps print_outcome with flat app_version", () => {
    const event = {
      event: "print_outcome",
      device_id: "dev-print",
      outcome: "success",
      app_version: "0.10.0",
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.blobs![3]).toBe("0.10.0");
  });

  it("handles missing optional fields with defaults", () => {
    const event = {
      event: "session",
      device_id: "dev-min",
    };
    const dp = mapEventToDataPoints(event)[0];
    expect(dp.indexes).toEqual(["session"]);
    expect(dp.blobs![0]).toBe("dev-min");
    expect(dp.blobs![1]).toBe(""); // version
    expect(dp.doubles![0]).toBe(0); // ram_total_mb
  });
});

// ---------- parseRange unit tests ----------

describe("parseRange", () => {
  it("parses valid range strings", () => {
    expect(parseRange("7d")).toBe(7);
    expect(parseRange("30d")).toBe(30);
    expect(parseRange("90d")).toBe(90);
  });

  it("defaults to 30 for null or invalid", () => {
    expect(parseRange(null)).toBe(30);
    expect(parseRange("")).toBe(30);
    expect(parseRange("abc")).toBe(30);
    expect(parseRange("7h")).toBe(30);
  });

  it("clamps to 1-90 range", () => {
    expect(parseRange("0d")).toBe(1);
    expect(parseRange("200d")).toBe(90);
  });
});

// ---------- Dashboard endpoint tests ----------

describe("Dashboard endpoints", () => {
  let env: ReturnType<typeof createEnv>;

  function dashboardRequest(path: string, apiKey = "test-admin-key") {
    return makeRequest(path, {
      headers: { "X-API-Key": apiKey },
    });
  }

  beforeEach(() => {
    env = createEnv();
    mockExecuteQuery.mockReset();
    // Default every query to an empty result set. Each test still chains
    // mockResolvedValueOnce for the queries its assertions care about; this
    // only backstops the rest.
    //
    // Without it, a handler that issues more queries than the test mocks gets
    // `undefined` back, and reading `.data` off it throws — surfacing as an
    // opaque 502 rather than a useful assertion failure. That is exactly how
    // these tests drifted: overviewQueries grew to 7 while the tests still
    // mocked 5, and the whole Dashboard block had been red long enough that
    // nobody noticed (the worker suite is not wired into CI).
    mockExecuteQuery.mockResolvedValue({ data: [] } as never);
  });

  // -- Shared auth/config tests --

  it("rejects unauthenticated dashboard requests", async () => {
    const res = await worker.fetch(
      makeRequest("/v1/dashboard/overview"),
      env,
    );
    expect(res.status).toBe(401);
  });

  it("rejects wrong API key on dashboard", async () => {
    const res = await worker.fetch(
      dashboardRequest("/v1/dashboard/overview", "wrong-key"),
      env,
    );
    expect(res.status).toBe(401);
  });

  it("rejects INGEST key on dashboard", async () => {
    const res = await worker.fetch(
      dashboardRequest("/v1/dashboard/overview", "test-ingest-key"),
      env,
    );
    expect(res.status).toBe(401);
  });

  it("rejects non-GET methods on dashboard", async () => {
    const res = await worker.fetch(
      makeRequest("/v1/dashboard/overview", {
        method: "POST",
        headers: { "X-API-Key": "test-admin-key" },
      }),
      env,
    );
    expect(res.status).toBe(405);
  });

  it("returns 503 when Cloudflare credentials are missing", async () => {
    const noCredsEnv = createEnv({
      CLOUDFLARE_ACCOUNT_ID: undefined as unknown as string,
      HELIX_ANALYTICS_READ_TOKEN: undefined as unknown as string,
    });
    const res = await worker.fetch(
      dashboardRequest("/v1/dashboard/overview"),
      noCredsEnv,
    );
    expect(res.status).toBe(503);
    const data = await res.json();
    expect(data.error).toContain("not configured");
  });

  it("returns 502 when Analytics Engine query fails", async () => {
    mockExecuteQuery.mockRejectedValue(new Error("SQL API down"));
    const res = await worker.fetch(
      dashboardRequest("/v1/dashboard/overview"),
      env,
    );
    expect(res.status).toBe(502);
    const data = await res.json();
    expect(data.error).toContain("Analytics query failed");
  });

  // -- GET /v1/dashboard/overview --

  describe("GET /v1/dashboard/overview", () => {
    it("returns overview data with correct shape", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [{ active_devices: 150 }] })
        .mockResolvedValueOnce({ data: [{ total_events: 5000 }] })
        .mockResolvedValueOnce({ data: [{ crash_count: 10, session_count: 500 }] })
        .mockResolvedValueOnce({ data: [{ successes: 85, total: 100 }] })
        .mockResolvedValueOnce({ data: [{ date: "2026-01-15", count: 120 }] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/overview?range=30d"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.active_devices).toBe(150);
      expect(data.total_events).toBe(5000);
      expect(data.crash_rate).toBeCloseTo(0.02);
      expect(data.print_success_rate).toBeCloseTo(0.85);
      expect(data.events_over_time).toHaveLength(1);
      expect(data.events_over_time[0]).toEqual({ date: "2026-01-15", count: 120 });
    });

    it("handles empty data gracefully", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [] })
        .mockResolvedValueOnce({ data: [] })
        .mockResolvedValueOnce({ data: [] })
        .mockResolvedValueOnce({ data: [] })
        .mockResolvedValueOnce({ data: [] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/overview"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.active_devices).toBe(0);
      expect(data.total_events).toBe(0);
      expect(data.crash_rate).toBe(0);
      expect(data.print_success_rate).toBeNull();
      expect(data.events_over_time).toEqual([]);
    });

    it("reports no print success rate under a filter that excludes print_outcome", async () => {
      mockExecuteQuery.mockResolvedValue({
        data: [{ active_devices: 1, total_events: 1, crash_count: 0, session_count: 1, successes: 5, total: 10 }],
      });
      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/overview?model=Voron"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.print_success_rate).toBeNull();
    });
  });

  // -- GET /v1/dashboard/adoption --

  describe("GET /v1/dashboard/adoption", () => {
    it("returns adoption data with correct shape", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [{ name: "linux-arm", count: 100 }] })
        .mockResolvedValueOnce({ data: [{ name: "0.9.19", count: 80 }] })
        .mockResolvedValueOnce({ data: [{ name: "Voron 2.4", count: 50 }] })
        .mockResolvedValueOnce({ data: [{ name: "corexy", count: 90 }] })
        .mockResolvedValueOnce({ data: [{ name: "btt-octopus", count: 12 }] })
        .mockResolvedValueOnce({ data: [{ name: "kiauh", count: 7 }] })
        .mockResolvedValueOnce({ data: [{ name: "forge_x", count: 5 }] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/adoption?range=7d"),
        env,
      );
      expect(res.status).toBe(200);
      const data = (await res.json()) as Record<string, unknown>;
      expect(data.platforms).toEqual([{ name: "linux-arm", count: 100 }]);
      expect(data.versions).toEqual([{ name: "0.9.19", count: 80 }]);
      expect(data.printer_models).toEqual([{ name: "Voron 2.4", count: 50 }]);
      expect(data.kinematics).toEqual([{ name: "corexy", count: 90 }]);
      expect(data.boards).toEqual([{ name: "btt-octopus", count: 12 }]);
      expect(data.install_sources).toEqual([{ name: "kiauh", count: 7 }]);
      expect(data.host_mods).toEqual([{ name: "forge_x", count: 5 }]);
    });
  });

  // -- GET /v1/dashboard/hardware --

  describe("GET /v1/dashboard/hardware", () => {
    // The hardware endpoint runs 10 queries; only the last two matter here, so
    // the rest are fed empty result sets in order.
    const emptyThrough = (n: number) => {
      for (let i = 0; i < n; i++) mockExecuteQuery.mockResolvedValueOnce({ data: [] });
    };

    it("splits helix_macros and moonraker_locality into labelled counts", async () => {
      emptyThrough(8);
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [{ name: "1", count: 30 }, { name: "0", count: 70 }] })
        .mockResolvedValueOnce({ data: [{ name: "1", count: 85 }, { name: "0", count: 15 }] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/hardware?range=30d"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.helix_macros).toEqual({ installed: 30, not_installed: 70, reported: 100 });
      expect(data.moonraker_locality).toEqual({ local: 85, remote: 15, reported: 100 });
    });

    it("reports zero rather than failing when nothing has answered yet", async () => {
      // The state on the day this ships: no client sends moonraker_is_local, so
      // the split must read as "no data" and not as 100% remote.
      emptyThrough(10);

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/hardware?range=30d"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.moonraker_locality).toEqual({ local: 0, remote: 0, reported: 0 });
      expect(data.helix_macros).toEqual({ installed: 0, not_installed: 0, reported: 0 });
    });
  });

  // -- GET /v1/dashboard/prints --

  describe("GET /v1/dashboard/prints", () => {
    it("returns print data with correct shape", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [{ date: "2026-01-15", rate: 0.85, total: 20 }] })
        .mockResolvedValueOnce({ data: [{ type: "PLA", success_rate: 0.9, count: 100 }] })
        .mockResolvedValueOnce({ data: [{ avg_duration_sec: 3600 }] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/prints?range=30d"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.success_rate_over_time).toEqual([{ date: "2026-01-15", rate: 0.85, total: 20 }]);
      expect(data.by_filament).toEqual([{ type: "PLA", success_rate: 0.9, count: 100 }]);
      expect(data.avg_duration_sec).toBe(3600);
    });
  });

  // -- GET /v1/dashboard/crashes --

  describe("GET /v1/dashboard/crashes", () => {
    it("returns crash data with rate calculation", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [{ ver: "0.9.19", crash_count: 5 }] })
        .mockResolvedValueOnce({ data: [{ ver: "0.9.19", session_count: 100 }] })
        .mockResolvedValueOnce({ data: [{ signal: "SIGSEGV", count: 10 }] })
        .mockResolvedValueOnce({ data: [{ avg_uptime_sec: 7200 }] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/crashes?range=30d"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.by_version).toEqual([
        { version: "0.9.19", crash_count: 5, session_count: 100, rate: 0.05 },
      ]);
      expect(data.by_signal).toEqual([{ signal: "SIGSEGV", count: 10 }]);
      expect(data.avg_uptime_sec).toBe(7200);
    });

    it("handles version with crashes but no sessions", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [{ ver: "0.9.18", crash_count: 3 }] })
        .mockResolvedValueOnce({ data: [] }) // no session data for this version
        .mockResolvedValueOnce({ data: [] })
        .mockResolvedValueOnce({ data: [{ avg_uptime_sec: 0 }] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/crashes"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.by_version[0].rate).toBe(0);
      expect(data.by_version[0].session_count).toBe(0);
    });
  });

  /** Answers each query by the first rule whose text the SQL contains; anything else is empty. */
  function mockBySql(rules: Array<[string, Array<Record<string, unknown>>]>) {
    mockExecuteQuery.mockImplementation(async (_config, sql) => {
      for (const [needle, data] of rules) if (sql.includes(needle)) return { data };
      return { data: [] };
    });
  }

  // `any`: each test reads its own endpoint's response shape.
  async function dashboard(path: string): Promise<any> {
    const res = await worker.fetch(dashboardRequest(path), env);
    expect(res.status).toBe(200);
    return res.json();
  }

  // -- Delta-era endpoints --

  describe("delta-era values", () => {
    it("engagement is null with a device count until a schema-3 client reports", async () => {
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 0 }]],
        ["as avg_session_sec", [{ avg_session_sec: 120 }]],
      ]);
      const data = await dashboard("/v1/dashboard/engagement");
      expect(data.avg_session_sec).toBeNull();
      expect(data.delta_era_devices).toBe(0);
    });

    it("engagement reports the boot-average session once devices report", async () => {
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 3 }]],
        ["toDate(written_at)", [{ date: "2026-10-14", avg_session_sec: 90 }]],
        ["as avg_session_sec", [{ avg_session_sec: 120 }]],
      ]);
      const data = await dashboard("/v1/dashboard/engagement");
      expect(data.avg_session_sec).toBe(120);
      expect(data.delta_era_devices).toBe(3);
      expect(data.session_duration_trend).toEqual([{ date: "2026-10-14", avg_session_sec: 90 }]);
    });

    it("a resent snapshot reaches the panel totals once", async () => {
      // AE stores the resend as a second identical row; the dedup subquery
      // returns the panel once, and the endpoint passes that through.
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 1 }]],
        ["as total_time_sec", [{ panel: "controls", total_time_sec: 60 }]],
      ]);
      const data = await dashboard("/v1/dashboard/engagement");
      expect(data.panel_time).toEqual([{ panel: "controls", total_time_sec: 60 }]);
      const panelSql = mockExecuteQuery.mock.calls
        .map((c) => c[1] as string)
        .find((sql) => sql.includes("as total_time_sec"))!;
      expect(panelSql).toMatch(/FROM \(\s*SELECT .* GROUP BY \S+, \S+, \S+, \S+\s*\)/s);
    });

    it("ux insights is null until a schema-3 client reports", async () => {
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 0 }]],
        ["as avg_session_sec", [{ avg_session_sec: 120 }]],
      ]);
      const data = await dashboard("/v1/dashboard/ux");
      expect(data.avg_session_sec).toBeNull();
      expect(data.delta_era_devices).toBe(0);
    });

    it("reliability's longest outage is null until a schema-3 client reports", async () => {
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 0 }]],
        ["as max_disconnect_sec", [{ max_disconnect_sec: 30 }]],
      ]);
      const data = await dashboard("/v1/dashboard/reliability");
      expect(data.max_disconnect_sec).toBeNull();
      expect(data.delta_era_devices).toBe(0);
    });

    it("reliability reports the longest outage once devices report", async () => {
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 2 }]],
        ["as max_disconnect_sec", [{ max_disconnect_sec: 30 }]],
      ]);
      const data = await dashboard("/v1/dashboard/reliability");
      expect(data.max_disconnect_sec).toBe(30);
    });

    it("stability carries the device count beside the klippy trend", async () => {
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 4 }]],
        ["as shutdowns", [{ date: "2026-10-14", errors: 2, shutdowns: 1 }]],
      ]);
      const data = await dashboard("/v1/dashboard/stability");
      expect(data.delta_era_devices).toBe(4);
      expect(data.klippy_trend).toEqual([{ date: "2026-10-14", errors: 2, shutdowns: 1 }]);
    });

    it("stability lists crash clusters over the range, largest first", async () => {
      mockBySql([
        [
          "as fault",
          [
            { sig: "SIGSEGV", fault: "NULL_DEREF", ver: "1.0.5", board: "k1", count: 2, devices: 2 },
            { sig: "SIGABRT", fault: "", ver: "1.0.4", board: "pi", count: 7, devices: 3 },
            { sig: "SIGBUS", fault: "", ver: "1.0.4", board: "pi", count: 0, devices: 0 },
          ],
        ],
      ]);
      const data = await dashboard("/v1/dashboard/stability?range=7d&board=k1,pi");
      expect(data.crash_clusters).toEqual([
        { signal: "SIGABRT", fault_code: "", version: "1.0.4", board: "pi", count: 7, devices: 3 },
        { signal: "SIGSEGV", fault_code: "NULL_DEREF", version: "1.0.5", board: "k1", count: 2, devices: 2 },
      ]);
      const sql = mockExecuteQuery.mock.calls.map(([, q]) => q as string).find((q) => q.includes("as fault"))!;
      expect(sql).toContain("INTERVAL '7' DAY");
      expect(sql).toContain(`${col("crash", "app_board")} IN ('k1', 'pi')`);
      expect(sql).toContain("GROUP BY sig, fault, ver, board");
    });

    it("stability lists error codes per version", async () => {
      mockBySql([
        [
          "GROUP BY category, code, version",
          [
            { category: "ui", code: "E1", version: "1.0.4", count: 1, devices: 1 },
            { category: "net", code: "E2", version: "1.0.5", count: 5, devices: 2 },
          ],
        ],
      ]);
      const data = await dashboard("/v1/dashboard/stability?platform=pi");
      expect(data.errors_by_version).toEqual([
        { category: "net", code: "E2", version: "1.0.5", count: 5, devices: 2 },
        { category: "ui", code: "E1", version: "1.0.4", count: 1, devices: 1 },
      ]);
      const sql = mockExecuteQuery.mock.calls
        .map(([, q]) => q as string)
        .find((q) => q.includes("GROUP BY category, code, version"))!;
      expect(sql).toContain(`${col("error_encountered", "app_version")} as version`);
      expect(sql).toContain(`${col("error_encountered", "app_platform")} IN ('pi')`);
    });

    it("features are adoption derived from overlay and widget deltas", async () => {
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 4 }]],
        ["as overlay_name", [{ id: "fan_control_overlay", devices: 2 }]],
        ["as widget_id", [{ id: "temperature", devices: 1 }]],
      ]);
      const data = await dashboard("/v1/dashboard/features");
      expect(data.delta_era_devices).toBe(4);
      expect(data.features).toEqual([
        { id: "fan_control_overlay", devices: 2, adoption_rate: 0.5 },
        { id: "temperature", devices: 1, adoption_rate: 0.25 },
      ]);
      for (const [, sql] of mockExecuteQuery.mock.calls) expect(sql).not.toContain("feature_adoption");
    });

    it("features report the configured flags beside derived adoption", async () => {
      const flag = (f: string) => `${col("settings_snapshot", f)} as name`;
      mockBySql([
        ["as delta_era_devices", [{ delta_era_devices: 4 }]],
        [flag("spoolman_configured"), [{ name: "1", count: 1 }, { name: "0", count: 3 }]],
        [flag("multi_printer"), [{ name: 0, count: 2 }]],
      ]);
      const data = await dashboard("/v1/dashboard/features");
      expect(data.configured).toEqual([
        { id: "spoolman_configured", devices: 1, reported: 4, adoption_rate: 0.25 },
        { id: "theme_changed", devices: 0, reported: 0, adoption_rate: null },
        { id: "multi_printer", devices: 0, reported: 2, adoption_rate: 0 },
      ]);
    });
  });

  describe("a filter the delta kinds do not carry", () => {
    const ENDPOINTS = ["engagement", "ux", "features", "reliability", "stability"];

    it("is named on every delta-era endpoint under model=", async () => {
      for (const name of ENDPOINTS) {
        mockBySql([]);
        const data = await dashboard(`/v1/dashboard/${name}?model=Voron`);
        expect(data.filter_not_applicable, name).toBe("model");
      }
    });

    it("is null with no filter", async () => {
      for (const name of ENDPOINTS) {
        mockBySql([]);
        const data = await dashboard(`/v1/dashboard/${name}`);
        expect(data.filter_not_applicable, name).toBeNull();
      }
    });

    it("is null under board= because every delta kind carries it", async () => {
      for (const name of ENDPOINTS) {
        mockBySql([]);
        expect((await dashboard(`/v1/dashboard/${name}?board=k1`)).filter_not_applicable, name).toBeNull();
      }
    });
  });

  describe("crash rate under a filter that excludes crashes", () => {
    const crashRows: Array<[string, Array<Record<string, unknown>>]> = [
      ["as crash_count", [{ crash_count: 0, session_count: 10 }]],
      ["as session_count", [{ date: "2026-10-14", ver: "1.0.0", platform: "pi", session_count: 10, device_count: 2 }]],
    ];

    it("overview reads n/a, not 0%", async () => {
      mockBySql(crashRows);
      expect((await dashboard("/v1/dashboard/overview?model=Voron")).crash_rate).toBeNull();
      mockBySql(crashRows);
      expect((await dashboard("/v1/dashboard/overview?platform=pi")).crash_rate).toBe(0);
    });

    it("crashes reads n/a on every rate", async () => {
      mockBySql(crashRows);
      const data = await dashboard("/v1/dashboard/crashes?model=Voron");
      expect(data.trailing_14d.rate).toBeNull();
      expect(data.by_platform.length).toBeGreaterThan(0);
      for (const p of data.by_platform) expect(p.rate).toBeNull();
    });

    it("stability's crash rate trend reads n/a", async () => {
      mockBySql(crashRows);
      const data = await dashboard("/v1/dashboard/stability?model=Voron");
      expect(data.crash_rate_trend.length).toBeGreaterThan(0);
      for (const d of data.crash_rate_trend) expect(d.rate).toBeNull();
    });
  });

  describe("a model filter, which crash, error and print rows do not carry", () => {
    it("health reports n/a crash rates and names the unreported blocks", async () => {
      mockBySql([
        ["LIMIT 8", [{ ver: "1.0.4", devices: 10 }]],
        ["as update_successes", [{ ver: "1.0.4", sessions: 20, crashes: 0, update_failures: 0, update_successes: 0 }]],
        ["index1 as event", [{ ver: "1.0.4", event: "session", devices: 10 }]],
      ]);
      const data = await dashboard("/v1/dashboard/health?model=Voron");
      expect(data.releases[0].cur.devices).toBe(10);
      expect(data.releases[0].cur.crash_rate).toBeNull();
      expect(data.releases[0].cur.crash_free).toBeNull();
      expect(data.not_applicable).toEqual({ crashes: "model", errors: "model", prints: "model", updates: "model" });
    });

    it("health keeps real crash rates under a filter crashes carry", async () => {
      mockBySql([
        ["LIMIT 8", [{ ver: "1.0.4", devices: 10 }]],
        ["as update_successes", [{ ver: "1.0.4", sessions: 20, crashes: 0, update_failures: 0, update_successes: 0 }]],
        ["index1 as event", [{ ver: "1.0.4", event: "session", devices: 10 }]],
      ]);
      const data = await dashboard("/v1/dashboard/health?platform=pi");
      expect(data.releases[0].cur.crash_rate).toBe(0);
      expect(data.releases[0].cur.crash_free).toBe(1);
      expect(data.not_applicable.crashes).toBeNull();
    });

    it("releases reports n/a crash rates and names the unreported blocks", async () => {
      mockBySql([
        ["as update_successes", [{ ver: "1.0.4", sessions: 20, crashes: 0 }]],
        ["as active_devices", [{ ver: "1.0.4", active_devices: 10 }]],
      ]);
      const data = await dashboard("/v1/dashboard/releases?versions=1.0.4&model=Voron");
      const v = data.versions[0];
      expect(v.active_devices).toBe(10);
      expect(v.crash_rate).toBeNull();
      expect(v.crash_free_devices).toBeNull();
      expect(data.not_applicable).toEqual({ crashes: "model", prints: "model", updates: "model", performance: "model" });
    });

    it("stability names crashes and errors as unreported", async () => {
      mockBySql([]);
      expect((await dashboard("/v1/dashboard/stability?model=Voron")).not_applicable).toEqual({ crashes: "model", errors: "model" });
      mockBySql([]);
      expect((await dashboard("/v1/dashboard/stability?board=k1")).not_applicable).toEqual({ crashes: null, errors: null });
    });

    it("prints names prints as unreported", async () => {
      mockBySql([]);
      expect((await dashboard("/v1/dashboard/prints?model=Voron")).not_applicable).toEqual({ prints: "model" });
      mockBySql([]);
      expect((await dashboard("/v1/dashboard/prints")).not_applicable).toEqual({ prints: null });
    });

    it("devices names the crash and print columns as unreported", async () => {
      mockBySql([]);
      expect((await dashboard("/v1/dashboard/devices?model=Voron")).not_applicable).toEqual({ crashes: "model", prints: "model" });
      mockBySql([]);
      expect((await dashboard("/v1/dashboard/devices?platform=pi")).not_applicable).toEqual({ crashes: null, prints: null });
    });
  });

  // -- GET /v1/dashboard/devices and /device/<id> --

  describe("device endpoints", () => {
    const ID = "ab".repeat(32);

    it("device page 400s on a bad id before any query", async () => {
      for (const bad of ["abc", ID.toUpperCase(), ID + "a", "x'%20OR%201=1"]) {
        const res = await worker.fetch(dashboardRequest(`/v1/dashboard/device/${bad}`), env as never);
        expect(res.status).toBe(400);
        expect(await res.json()).toEqual({ error: "invalid device id" });
      }
      expect(mockExecuteQuery).not.toHaveBeenCalled();
    });

    it("device page requires the admin key", async () => {
      const res = await worker.fetch(dashboardRequest(`/v1/dashboard/device/${ID}`, "nope"), env as never);
      expect(res.status).toBe(401);
    });

    it("device page 404s when the device has no rows", async () => {
      mockExecuteQuery.mockResolvedValue({ data: [] });
      const res = await worker.fetch(dashboardRequest(`/v1/dashboard/device/${ID}`), env as never);
      expect(res.status).toBe(404);
    });

    it("device page returns the page for canned rows", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({
          data: [{ timestamp: "2026-10-03 00:00:00", index1: "crash", blob1: ID, blob2: "1.0.0", blob3: "SIGSEGV", double1: 9 }],
        })
        .mockResolvedValueOnce({ data: [{ version: "1.0.0", first_seen: "2026-10-01 00:00:00", last_seen: "2026-10-03 00:00:00" }] });
      const res = await worker.fetch(dashboardRequest(`/v1/dashboard/device/${ID}`), env as never);
      expect(res.status).toBe(200);
      const page = (await res.json()) as any;
      expect(page.device_id).toBe(ID);
      expect(page.versions).toHaveLength(1);
      expect(page.timeline[0]).toEqual({
        timestamp: "2026-10-03 00:00:00",
        kind: "crash",
        fields: { app_version: "1.0.0", signal_name: "SIGSEGV", uptime_sec: 9 },
      });
      expect(mockExecuteQuery.mock.calls[0][1]).toContain(`blob1 = '${ID}'`);
    });

    it("device list joins counts and carries the filter into the session queries", async () => {
      mockExecuteQuery.mockImplementation(async (_c, sql) => {
        if (sql.includes("argMax")) return { data: [{ device_id: "d1", last_seen: "t", version: "1", platform: "pi", board: "", model: "m", sessions: 2 }] };
        if (sql.includes("count(DISTINCT")) return { data: [{ total: 12 }] };
        if (sql.includes("'crash'")) return { data: [{ device_id: "d1", crashes: 4 }] };
        return { data: [] };
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/devices?platform=pi"), env as never);
      expect(res.status).toBe(200);
      const body = (await res.json()) as any;
      expect(body.total).toBe(12);
      expect(body.devices).toEqual([
        { device_id: "d1", last_seen: "t", version: "1", platform: "pi", board: "", model: "m", sessions: 2, crashes: 4, prints: 0 },
      ]);
      const sessionSql = mockExecuteQuery.mock.calls[0][1] as string;
      expect(sessionSql).toContain(queries.filterFor("session", { platform: "pi" }));
    });
  });

  // -- GET /v1/dashboard/health --

  describe("GET /v1/dashboard/health", () => {
    it("runs top_versions first, then the rest with those versions, and returns all five blocks", async () => {
      mockExecuteQuery.mockImplementation(async (_c, sql) => {
        if (sql.includes("LIMIT 8")) return { data: [{ ver: "1.0.4", devices: 10 }] };
        if (sql.includes("'cdn_fleet_daily'") && sql.includes("LIMIT 1")) return { data: [{ date: "2026-10-14", sources: 7 }] };
        if (sql.includes("index1 IN ('session', 'crash', 'update_failed'")) {
          return { data: [{ ver: "1.0.4", sessions: 20, crashes: 1, update_failures: 0, update_successes: 0 }] };
        }
        return { data: [] };
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/health?version=1.0.4"), env as never);
      expect(res.status).toBe(200);
      const body = (await res.json()) as any;
      expect(Object.keys(body).sort()).toEqual(["crash_clusters", "data_health", "errors", "fleet", "not_applicable", "releases"]);
      expect(body.not_applicable).toEqual({ crashes: null, errors: null, prints: null, updates: null });
      expect(body.releases[0].version).toBe("1.0.4");
      expect(body.releases[0].cur.crash_rate).toBeCloseTo(0.05);
      expect(body.fleet.cdn_sources).toBe(7);
      const sqls = mockExecuteQuery.mock.calls.map((c) => c[1] as string);
      expect(sqls[0]).toContain("LIMIT 8");
      expect(sqls.slice(1).some((s) => s.includes("'1.0.4'"))).toBe(true);
    });
  });

  // -- GET /v1/dashboard/crash-list --

  describe("GET /v1/dashboard/crash-list", () => {
    it("returns crash list with correct shape", async () => {
      mockExecuteQuery.mockResolvedValueOnce({
        data: [
          {
            timestamp: "2026-02-10T17:17:03Z",
            device_id: "dev-abc123",
            ver: "0.9.9",
            sig: "SIGSEGV",
            platform: "pi",
            uptime_sec: 325,
          },
          {
            timestamp: "2026-02-10T15:00:00Z",
            device_id: "dev-def456",
            ver: "0.9.8",
            sig: "SIGABRT",
            platform: "pi",
            uptime_sec: 7200,
          },
        ],
      });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/crash-list?range=30d&limit=10"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.crashes).toHaveLength(2);
      expect(data.crashes[0]).toEqual({
        timestamp: "2026-02-10T17:17:03Z",
        device_id: "dev-abc123",
        version: "0.9.9",
        signal: "SIGSEGV",
        platform: "pi",
        uptime_sec: 325,
        // deduplicateCrashes collapses repeats and reports how many it merged.
        occurrences: 1,
      });
    });

    it("handles empty crash list", async () => {
      mockExecuteQuery.mockResolvedValueOnce({ data: [] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/crash-list"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.crashes).toEqual([]);
    });

    it("clamps limit to 1-200 range", async () => {
      mockExecuteQuery.mockResolvedValue({ data: [] });

      // The handler over-fetches 5x the requested limit so dedup still has
      // enough rows to work with, then slices back down. So the SQL LIMIT is
      // always limit*5, not limit.

      // Default limit (no param) -> 50, SQL asks for 250
      await worker.fetch(dashboardRequest("/v1/dashboard/crash-list"), env);
      const firstCall = mockExecuteQuery.mock.calls[0][1] as string;
      expect(firstCall).toContain("LIMIT 250");

      mockExecuteQuery.mockClear();

      // Over 200 gets clamped to 200, SQL asks for 1000
      await worker.fetch(dashboardRequest("/v1/dashboard/crash-list?limit=500"), env);
      const secondCall = mockExecuteQuery.mock.calls[0][1] as string;
      expect(secondCall).toContain("LIMIT 1000");

      mockExecuteQuery.mockClear();

      // A negative is what the Math.max(1, ...) floor actually guards against:
      // -5 clamps to 1, so the SQL asks for 5.
      await worker.fetch(dashboardRequest("/v1/dashboard/crash-list?limit=-5"), env);
      expect(mockExecuteQuery.mock.calls[0][1] as string).toContain("LIMIT 5");

      mockExecuteQuery.mockClear();

      // limit=0 and a non-numeric limit both fall back to the default 50
      // (-> 250) rather than reaching the floor: the handler uses `|| 50`, and
      // 0 is falsy. Asserted so the fallback is deliberate rather than assumed.
      await worker.fetch(dashboardRequest("/v1/dashboard/crash-list?limit=0"), env);
      expect(mockExecuteQuery.mock.calls[0][1] as string).toContain("LIMIT 250");

      mockExecuteQuery.mockClear();

      await worker.fetch(dashboardRequest("/v1/dashboard/crash-list?limit=abc"), env);
      expect(mockExecuteQuery.mock.calls[0][1] as string).toContain("LIMIT 250");
    });
  });

  // -- GET /v1/dashboard/releases --

  describe("GET /v1/dashboard/releases", () => {
    // Answers each scorecard query by its shape, so tests do not depend on call order.
    function scorecardMock(rows: {
      stats?: unknown[];
      actives?: unknown[];
      prints?: unknown[];
      crashers?: unknown[];
      frame?: unknown[];
      rss?: unknown[];
      adoption?: unknown[];
      top?: unknown[];
      sessions?: unknown[];
    }) {
      mockExecuteQuery.mockImplementation(async (_c, sql: string) => {
        if (sql.includes("LIMIT 8")) return { data: rows.top ?? [] };
        if (sql.includes("argMax") && sql.includes("as model")) return { data: rows.sessions ?? [] };
        if (sql.includes("as update_successes")) return { data: rows.stats ?? [] };
        if (sql.includes("as active_devices")) return { data: rows.actives ?? [] };
        if (sql.includes("as print_successes")) return { data: rows.prints ?? [] };
        if (sql.includes("as crashing_devices")) return { data: rows.crashers ?? [] };
        if (sql.includes("as p95_frame_ms")) return { data: rows.frame ?? [] };
        if (sql.includes("as p95_rss_kb")) return { data: rows.rss ?? [] };
        if (sql.includes("as devices") && sql.includes("toDate(timestamp) as date")) return { data: rows.adoption ?? [] };
        return { data: [] };
      });
    }

    it("returns release comparison data", async () => {
      scorecardMock({
        stats: [{ ver: "0.9.19", sessions: 200, crashes: 4 }],
        actives: [{ ver: "0.9.19", active_devices: 50 }],
        prints: [{ ver: "0.9.19", print_successes: 88, print_total: 100 }],
        crashers: [{ ver: "0.9.19", crashing_devices: 5 }],
        frame: [{ ver: "0.9.19", p95_frame_ms: 33.4 }],
        rss: [{ ver: "0.9.19", p95_rss_kb: 90000 }],
        adoption: [{ date: "2026-10-01", ver: "0.9.19", devices: 12 }],
      });

      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases?versions=v0.9.19"), env as never);
      expect(res.status).toBe(200);
      const data = (await res.json()) as any;
      expect(data.versions).toHaveLength(1);
      const v = data.versions[0];
      expect(v.version).toBe("0.9.19");
      expect(v.active_devices).toBe(50);
      expect(v.crash_rate).toBeCloseTo(0.02);
      expect(v.crash_free_devices).toBeCloseTo(0.9);
      expect(v.p95_frame_ms).toBeCloseTo(33.4);
      expect(v.p95_rss_kb).toBe(90000);
      expect(v.print_success_rate).toBeCloseTo(0.88);
      expect(v.total_sessions).toBe(200);
      expect(v.total_crashes).toBe(4);
      expect(v.update_failures).toBe(0);
      expect(v.update_successes).toBe(0);
      expect(v.update_failure_rate).toBeNull();
      expect(data.adoption).toEqual([{ date: "2026-10-01", version: "0.9.19", devices: 12 }]);
    });

    it("attributes prints without a version to each device's session version", async () => {
      scorecardMock({
        stats: [{ ver: "0.9.19", sessions: 10 }, { ver: "0.9.18", sessions: 10 }],
        prints: [
          { device_id: "a", ver: "", print_successes: 3, print_total: 4 },
          { device_id: "b", ver: "0.9.18", print_successes: 1, print_total: 2 },
          { device_id: "z", ver: "", print_successes: 0, print_total: 9 },
        ],
        sessions: [{ device_id: "a", model: "K1", ver: "0.9.19" }, { device_id: "b", model: "K1", ver: "0.9.19" }],
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases?versions=0.9.19,0.9.18"), env as never);
      const rate = Object.fromEntries(((await res.json()) as any).versions.map((v: any) => [v.version, v.print_success_rate]));
      expect(rate["0.9.19"]).toBeCloseTo(0.75);
      expect(rate["0.9.18"]).toBeCloseTo(0.5);
    });

    it("picks the versions itself when the param is absent", async () => {
      scorecardMock({
        top: [{ ver: "1.0.4", devices: 9 }],
        stats: [{ ver: "1.0.4", sessions: 10, crashes: 1 }],
        actives: [{ ver: "1.0.4", active_devices: 9 }],
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases"), env as never);
      expect(res.status).toBe(200);
      const data = (await res.json()) as any;
      expect(data.versions[0].version).toBe("1.0.4");
      const sqls = mockExecuteQuery.mock.calls.map((c) => c[1] as string);
      expect(sqls[0]).toContain("LIMIT 8");
      expect(sqls.slice(1).some((s) => s.includes("'1.0.4'"))).toBe(true);
    });

    it("reports a null crash rate and crash-free share with no sessions or devices", async () => {
      scorecardMock({
        stats: [{ ver: "0.9.18", sessions: 0, crashes: 0 }],
        actives: [],
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases?versions=v0.9.18"), env as never);
      expect(res.status).toBe(200);
      const v = ((await res.json()) as any).versions[0];
      expect(v.crash_rate).toBeNull();
      expect(v.crash_free_devices).toBeNull();
      expect(v.p95_frame_ms).toBeNull();
      expect(v.p95_rss_kb).toBeNull();
    });

    it("clamps crash-free devices at 0 when more devices crashed than were active", async () => {
      scorecardMock({
        stats: [{ ver: "1.0.0", sessions: 10, crashes: 9 }],
        actives: [{ ver: "1.0.0", active_devices: 2 }],
        crashers: [{ ver: "1.0.0", crashing_devices: 5 }],
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases?versions=1.0.0"), env as never);
      expect(((await res.json()) as any).versions[0].crash_free_devices).toBe(0);
    });

    it("returns empty lists when no version has sessions", async () => {
      scorecardMock({ top: [] });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases"), env as never);
      expect(await res.json()).toEqual({
        versions: [],
        adoption: [],
        not_applicable: { crashes: null, prints: null, updates: null, performance: null },
      });
      expect(mockExecuteQuery).toHaveBeenCalledTimes(1);
    });

    it("reports self-update health per running version", async () => {
      scorecardMock({
        stats: [{ ver: "0.99.97", sessions: 100, crashes: 1, update_failures: 9, update_successes: 1 }],
        actives: [{ ver: "0.99.97", active_devices: 40 }],
        prints: [{ ver: "0.99.97", print_successes: 10, print_total: 10 }],
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases?versions=v0.99.97"), env as never);
      expect(res.status).toBe(200);
      const data = (await res.json()) as any;
      expect(data.versions[0].update_failures).toBe(9);
      expect(data.versions[0].update_successes).toBe(1);
      expect(data.versions[0].update_failure_rate).toBeCloseTo(0.9);
    });

    it("reports a null print success rate for a version with no prints", async () => {
      scorecardMock({
        stats: [{ ver: "0.9.18", sessions: 10, crashes: 0 }],
        actives: [{ ver: "0.9.18", active_devices: 5 }],
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases?versions=v0.9.18"), env as never);
      expect(((await res.json()) as any).versions[0].print_success_rate).toBeNull();
    });

    it("rejects empty versions parameter", async () => {
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/releases?versions="), env as never);
      expect(res.status).toBe(400);
    });

    it("rejects more than 20 versions", async () => {
      const many = Array.from({ length: 21 }, (_, i) => `1.0.${i}`).join(",");
      const res = await worker.fetch(dashboardRequest(`/v1/dashboard/releases?versions=${many}`), env as never);
      expect(res.status).toBe(400);
    });
  });

  describe("GET /v1/dashboard/fleet/retention", () => {
    it("returns cohorts from per-device first and last days", async () => {
      const today = new Date().toISOString().slice(0, 10);
      mockExecuteQuery.mockResolvedValue({ data: [{ first_day: today, last_day: today }] });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/fleet/retention"), env as never);
      expect(res.status).toBe(200);
      const body = (await res.json()) as any;
      expect(body.cohorts).toHaveLength(13);
      expect(body.cohorts[0].label).toBe("before window");
      expect(body.cohorts[12].size).toBe(1);
      expect(mockExecuteQuery.mock.calls[0][1]).toContain("min(toDate(timestamp))");
    });
  });

  describe("prints and performance additions", () => {
    it("prints sums per-device outcomes into models and versions through the session join", async () => {
      mockExecuteQuery.mockImplementation(async (_c, sql: string) => {
        if (sql.includes("argMax") && sql.includes("as model")) {
          return { data: [{ device_id: "a", model: "Voron 2.4", ver: "1.0.0" }, { device_id: "b", model: "Voron 2.4", ver: "1.0.1" }] };
        }
        if (sql.includes("as device_id") && sql.includes("as print_successes")) {
          return { data: [
            { device_id: "a", ver: "", print_successes: 8, print_total: 10 },
            { device_id: "b", ver: "1.0.0", print_successes: 2, print_total: 10 },
            { device_id: "z", ver: "", print_successes: 1, print_total: 4 },
          ] };
        }
        return { data: [] };
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/prints"), env as never);
      expect(res.status).toBe(200);
      const body = (await res.json()) as any;
      const byModel = Object.fromEntries(body.by_model.map((m: any) => [m.model, m]));
      expect(byModel["Voron 2.4"]).toEqual({ model: "Voron 2.4", successes: 10, total: 20, rate: 0.5 });
      expect(byModel["Unknown"]).toEqual({ model: "Unknown", successes: 1, total: 4, rate: 0.25 });
      expect(body.by_version).toEqual([{ version: "1.0.0", successes: 10, total: 20, rate: 0.5 }]);
    });

    it("performance returns per-platform version and panel breakdowns", async () => {
      mockExecuteQuery.mockImplementation(async (_c, sql: string) => {
        if (sql.includes("memory_snapshot") || sql.includes("as avg_rss_kb")) {
          return { data: [{ platform: "pi", version: "1.0.0", avg_rss_kb: 70000.4 }] };
        }
        if (sql.includes("GROUP BY platform, version")) {
          return { data: [{ platform: "pi", version: "1.0.0", p95_frame_ms: 31.2, devices: 6 }] };
        }
        if (sql.includes("GROUP BY platform, panel")) {
          return { data: [{ platform: "pi", panel: "home", times_worst: 3, avg_p95_ms: 40.6 }] };
        }
        return { data: [] };
      });
      const res = await worker.fetch(dashboardRequest("/v1/dashboard/performance"), env as never);
      expect(res.status).toBe(200);
      const body = (await res.json()) as any;
      expect(body.by_platform_version).toEqual([
        { platform: "pi", version: "1.0.0", p95_frame_ms: 31.2, avg_rss_kb: 70000.4, devices: 6 },
      ]);
      expect(body.jankiest_by_platform).toEqual([{ platform: "pi", panel: "home", times_worst: 3, avg_p95_ms: 41 }]);
    });
  });

  // -- Unknown dashboard path --

  it("returns 404 for unknown dashboard path", async () => {
    mockExecuteQuery.mockResolvedValue({ data: [] });
    const res = await worker.fetch(
      dashboardRequest("/v1/dashboard/nonexistent"),
      env,
    );
    expect(res.status).toBe(404);
  });
});

// ---------- Admin routes that do not exist ----------

describe("POST /v1/admin/backfill", () => {
  it("returns 404 even for an authenticated request", async () => {
    const res = await worker.fetch(
      makeRequest("/v1/admin/backfill", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-API-Key": "test-admin-key" },
        body: JSON.stringify({ events: [validEvent()] }),
      }),
      createEnv(),
    );
    expect(res.status).toBe(404);
  });
});

// ---------- AE per-kind counts ----------

describe("aeCountsQuery", () => {
  it("groups by index1 and filters on the given date", () => {
    const sql = queries.aeCountsQuery("2026-10-12");
    expect(sql).toContain("toDate(timestamp) = '2026-10-12'");
    expect(sql).toContain("GROUP BY kind");
    expect(sql).toContain("count() AS row_count");
    expect(sql).toContain("sum(_sample_interval) AS weighted");
    expect(sql).toContain("count(DISTINCT blob1) AS devices");
    expect(sql).not.toContain("uniqExact");
  });

  it.each(["2026-10-12'; --", "2026-10-1", "20261012", "", "2026-13-45", "2026-02-30"])(
    "rejects %j before building any SQL",
    (bad) => {
      expect(() => queries.aeCountsQuery(bad)).toThrow();
    },
  );
});

describe("GET /v1/admin/ae-counts", () => {
  function aeCountsRequest(search: string, apiKey?: string) {
    return makeRequest(`/v1/admin/ae-counts${search}`, {
      headers: apiKey ? { "X-API-Key": apiKey } : {},
    });
  }

  beforeEach(() => {
    mockExecuteQuery.mockReset();
  });

  it("rejects a request without the admin key", async () => {
    const res = await worker.fetch(aeCountsRequest("?date=2026-10-12"), createEnv());
    expect(res.status).toBe(401);
    expect(mockExecuteQuery).not.toHaveBeenCalled();
  });

  it("rejects the ingest key", async () => {
    const res = await worker.fetch(
      aeCountsRequest("?date=2026-10-12", "test-ingest-key"),
      createEnv(),
    );
    expect(res.status).toBe(401);
  });

  it.each(["", "?date=nope", "?date=2026-10-12'; --"])(
    "returns 400 for a bad date (%j)",
    async (search) => {
      const res = await worker.fetch(aeCountsRequest(search, "test-admin-key"), createEnv());
      expect(res.status).toBe(400);
      expect(mockExecuteQuery).not.toHaveBeenCalled();
    },
  );

  it("returns the per-kind rows for the date", async () => {
    mockExecuteQuery.mockResolvedValue({
      data: [
        { kind: "session", row_count: 10, weighted: 12, devices: 7 },
        { kind: "panel_usage", row_count: 40, weighted: 40, devices: 5 },
      ],
    } as never);
    const res = await worker.fetch(
      aeCountsRequest("?date=2026-10-12", "test-admin-key"),
      createEnv(),
    );
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({
      date: "2026-10-12",
      kinds: [
        { kind: "session", rows: 10, weighted: 12, devices: 7 },
        { kind: "panel_usage", rows: 40, weighted: 40, devices: 5 },
      ],
    });
    expect(mockExecuteQuery.mock.calls[0][1]).toBe(queries.aeCountsQuery("2026-10-12"));
  });

  it("returns 502 when the SQL API fails", async () => {
    mockExecuteQuery.mockRejectedValue(new Error("SQL API down"));
    const res = await worker.fetch(
      aeCountsRequest("?date=2026-10-12", "test-admin-key"),
      createEnv(),
    );
    expect(res.status).toBe(502);
  });
});

// ---------- reconcile script ----------

describe("buildReconcileTable", () => {
  it("joins R2 point counts with AE rows and computes kept%", () => {
    const table = buildReconcileTable(
      { session: 10, panel_usage: 4, crash: 2 },
      [
        { kind: "session", rows: 9, weighted: 10, devices: 5 },
        { kind: "panel_usage", rows: 1, weighted: 2, devices: 1 },
        { kind: "ae_only", rows: 3, weighted: 3, devices: 1 },
      ],
    );
    expect(table).toEqual([
      { kind: "crash", r2_points: 2, ae_rows: 0, ae_weighted: 0, "kept%": "0.0" },
      { kind: "panel_usage", r2_points: 4, ae_rows: 1, ae_weighted: 2, "kept%": "50.0" },
      { kind: "session", r2_points: 10, ae_rows: 9, ae_weighted: 10, "kept%": "100.0" },
      { kind: "ae_only", r2_points: 0, ae_rows: 3, ae_weighted: 3, "kept%": "n/a" },
    ]);
  });

  it("sorts kinds present in R2 alphabetically, then AE-only kinds", () => {
    const table = buildReconcileTable({ b: 1, a: 1 }, [{ kind: "z", rows: 1, weighted: 1, devices: 1 }]);
    expect(table.map((r) => r.kind)).toEqual(["a", "b", "z"]);
  });
});

describe("parseEnvValue", () => {
  const text = [
    "# comment",
    "OTHER=nope",
    "HELIX_TELEMETRY_ADMIN_KEY=abc123",
    "",
  ].join("\n");

  it("reads a plain KEY=VALUE line, as .env.telemetry is written", () => {
    expect(parseEnvValue(text, "HELIX_TELEMETRY_ADMIN_KEY")).toBe("abc123");
  });

  it("accepts export prefix, quotes and CRLF, and the last assignment wins", () => {
    expect(parseEnvValue('A=1\r\nexport A="two"\r\n', "A")).toBe("two");
  });

  it("returns undefined for a missing name or a commented-out line", () => {
    expect(parseEnvValue("# A=1\nB=2", "A")).toBeUndefined();
  });
});

describe("countR2Points", () => {
  it("counts mapped data points per event kind across batches", () => {
    const counts = countR2Points([
      [
        { event: "session", device_id: "d1" },
        { event: "crash", device_id: "d1", signal_name: "SIGSEGV" },
      ],
      [{ event: "session", device_id: "d2" }],
    ]);
    expect(counts).toEqual({ session: 2, crash: 1 });
  });

  it("counts by the AE kind a point lands in, not by the event name", () => {
    const counts = countR2Points([
      [
        { event: "settings_snapshot", device_id: "d1", home_widgets: ["temp", "temp", "fan"] },
        {
          event: "panel_usage",
          device_id: "d1",
          panel_time_sec: { home: 5, motion: 3 },
          panel_visits: { home: 1, motion: 1, settings: 2 },
        },
        {
          event: "settings_changes",
          device_id: "d1",
          changes: [
            { setting: "a", old_value: "1", new_value: "2" },
            { setting: "b", old_value: "x", new_value: "y" },
          ],
        },
      ],
    ]);
    expect(counts).toEqual({
      settings_snapshot: 1,
      widget_placement: 2,
      panel_usage: 3,
      settings_change: 2,
    });
  });

  it("tolerates a batch shaped as {events: [...]} and skips non-objects", () => {
    const counts = countR2Points([{ events: [{ event: "session", device_id: "d1" }, null, 7] }, "junk"]);
    expect(counts).toEqual({ session: 1 });
  });
});

describe("client field names reach the right Analytics Engine slot", () => {
  it("connection_stability reads total_connected_sec and total_disconnected_sec", () => {
    const dp = mapEventToDataPoints({
      event: "connection_stability",
      device_id: "dev-1",
      total_connected_sec: 3600,
      total_disconnected_sec: 120,
    })[0];
    expect(dp.doubles![3]).toBe(3600);
    expect(dp.doubles![4]).toBe(120);
  });

  it("settings_snapshot reads auto_update_channel and screen_blank_timeout_sec", () => {
    const dp = mapEventToDataPoints({
      event: "settings_snapshot",
      device_id: "dev-1",
      auto_update_channel: "beta",
      screen_blank_timeout_sec: 300,
    })[0];
    expect(dp.blobs![5]).toBe("beta");
    expect(dp.doubles![2]).toBe(300);
  });

  it("print_start_context reads estimated_duration_bucket", () => {
    const dp = mapEventToDataPoints({
      event: "print_start_context",
      device_id: "dev-1",
      estimated_duration_bucket: "1-4h",
    })[0];
    expect(dp.blobs![5]).toBe("1-4h");
  });
});

describe("SQL builders", () => {
  it("never emit uniqExact", () => {
    const builders = Object.entries(queries).filter(([name]) => /Quer(y|ies)$/.test(name) && name !== "executeQuery" && name !== "aeCountsQuery");
    expect(builders.length).toBeGreaterThan(10);
    for (const [name, fn] of builders) {
      const out =
        name === "crashListQuery"
          ? (fn as (d: number, l: number) => string)(30, 10)
          : name === "updateFailureGroupsQuery"
            ? (fn as unknown as (w: readonly [number, number]) => string)([0, 7])
          : name === "releaseScorecardQueries"
            ? (fn as (v: string[], d: number) => string[])(["1.0.0"], 30)
            : name === "retentionQuery"
              ? (fn as () => string)()
            : name === "deviceQueries"
              ? (fn as unknown as (id: string) => string[])("0".repeat(64))
              : (fn as (d: number) => string | string[])(30);
      for (const sql of Array.isArray(out) ? out : [out]) {
        expect(sql, name).not.toContain("uniqExact");
      }
    }
  });
});

describe("toYesNo", () => {
  it("counts numeric 1 and 0 names as yes and no", () => {
    const res = { data: [{ name: 1, count: 3 }, { name: 0, count: 1 }] };
    expect(toYesNo(res, "installed", "not_installed")).toEqual({ installed: 3, not_installed: 1, reported: 4 });
  });

  it("counts string names the same way", () => {
    const res = { data: [{ name: "1", count: 2 }, { name: "0", count: 5 }] };
    expect(toYesNo(res, "local", "remote")).toEqual({ local: 2, remote: 5, reported: 7 });
  });
});

describe("compareVersions", () => {
  // The worker and the dashboard each carry their own comparator; every case
  // runs against both so the two cannot drift apart.
  const copies: Array<[string, (a: string, b: string) => number]> = [
    ["worker", compareVersions],
    ["dashboard", dashboardCompareVersions],
  ];
  // [a, b, sign of compare(a, b)]
  const cases: Array<[string, string, -1 | 0 | 1]> = [
    ["1.2.3", "1.2.3", 0],
    ["1.2", "1.2.0", 0],
    ["1.10.0", "1.9.0", 1],
    ["1.0.5", "1.1.0", -1],
    ["1.1.0-beta.9", "1.1.0-beta.10", -1],
    ["1.1.0-beta", "1.1.0-beta.1", -1],
    ["1.1.0-rc", "1.1.0-beta", 1],
    ["1.1.0-rc.1", "1.1.0-beta.9", 1],
    ["1.1.0-1", "1.1.0-alpha", -1],
    ["1.1.0-beta.1", "1.1.0-beta.alpha", -1],
    ["1.1.0-beta.2", "1.1.0", -1],
    ["1.0.9", "1.1.0-beta.1", -1],
    ["v1.2.0", "1.2.0", 0],
    ["v1.2.0-beta.1", "1.2.0-beta.1", 0],
    ["1.2.0+build5", "1.2.0", 0],
    ["1.2.0-beta.1+abc", "1.2.0-beta.1", 0],
    ["1.2.0-beta.1+abc", "1.2.0-beta.2+aaa", -1],
  ];

  for (const [name, cmp] of copies) {
    describe(name, () => {
      for (const [a, b, want] of cases) {
        it(`${a} vs ${b} is ${want}`, () => {
          expect(Math.sign(cmp(a, b))).toBe(want);
          expect(Math.sign(cmp(b, a))).toBe(-want || 0);
        });
      }

      it("sorts a mixed list", () => {
        const input = ["1.1.0", "1.1.0-beta.10", "1.0.5", "1.1.0-beta.9", "0.99.118"];
        expect([...input].sort(cmp)).toEqual(["0.99.118", "1.0.5", "1.1.0-beta.9", "1.1.0-beta.10", "1.1.0"]);
      });
    });
  }
});

describe("configuredFeature", () => {
  it("is the share of reporting devices with the flag on", () => {
    expect(configuredFeature("x", { data: [{ name: "1", count: 3 }, { name: "0", count: 1 }] })).toEqual({
      id: "x",
      devices: 3,
      reported: 4,
      adoption_rate: 0.75,
    });
  });

  it("has no rate before any device reports", () => {
    expect(configuredFeature("x", { data: [] }).adoption_rate).toBeNull();
  });
});

describe("deriveFeatureAdoption", () => {
  it("rates each id against the delta-era devices, highest first", () => {
    expect(
      deriveFeatureAdoption([{ id: "a_overlay", devices: 1 }], [{ id: "b", devices: 3 }], 4),
    ).toEqual([
      { id: "b", devices: 3, adoption_rate: 0.75 },
      { id: "a_overlay", devices: 1, adoption_rate: 0.25 },
    ]);
  });

  it("is empty before any schema-3 device reports", () => {
    expect(deriveFeatureAdoption([{ id: "a", devices: 1 }], [], 0)).toEqual([]);
  });

  it("an id reported as both overlay and widget appears once", () => {
    expect(deriveFeatureAdoption([{ id: "x", devices: 2 }], [{ id: "x", devices: 3 }], 4)).toEqual([
      { id: "x", devices: 3, adoption_rate: 0.75 },
    ]);
  });

  it("drops empty ids", () => {
    expect(deriveFeatureAdoption([{ id: "", devices: 2 }], [], 4)).toEqual([]);
  });
});

// ---------- Feedback loop: cron dispatch, admin run, report endpoints ----------

describe("feedback loop wiring", () => {
  const TUESDAY = new Date("2026-10-20T13:00:00Z");
  const MONDAY = new Date("2026-10-19T13:00:00Z");

  /** Runs scheduled() and settles every promise it handed to waitUntil. */
  async function runScheduled(cron: string, at: Date, env: unknown) {
    const waits: Promise<unknown>[] = [];
    const ctx = { waitUntil: (p: Promise<unknown>) => void waits.push(p), passThroughOnException() {} };
    await worker.scheduled({ cron, scheduledTime: at.getTime(), noRetry() {} } as never, env as never, ctx as never);
    return Promise.allSettled(waits);
  }

  const reportKeys = (env: ReturnType<typeof createEnv>) =>
    [...env.TELEMETRY_BUCKET._storage.keys()].filter((k) => k.startsWith("reports/"));

  beforeEach(() => {
    vi.restoreAllMocks();
    mockExecuteQuery.mockReset();
    mockExecuteQuery.mockResolvedValue({ data: [] });
  });

  describe("scheduled", () => {
    it("is scheduled by wrangler.toml for both crons, and nothing else", () => {
      const toml = readFileSync(resolve(__dirname, "../../wrangler.toml"), "utf8");
      const line = toml.match(/^\s*crons\s*=\s*\[(.*)\]/m);
      expect(line).not.toBeNull();
      const crons = [...line![1].matchAll(/"([^"]+)"/g)].map((m) => m[1]);
      expect(crons.sort()).toEqual([CDN_CRON, LOOP_CRON].sort());
    });

    it("runs the daily loop only, on a weekday 13:00 cron", async () => {
      const env = createEnv();
      const fetchSpy = vi.spyOn(globalThis, "fetch");
      const settled = await runScheduled(LOOP_CRON, TUESDAY, env);
      expect(settled.every((s) => s.status === "fulfilled")).toBe(true);
      expect(mockExecuteQuery).toHaveBeenCalled();
      expect(reportKeys(env)).toEqual([]);
      expect(fetchSpy).not.toHaveBeenCalled();
    });

    it("runs the weekly loop as well on a Monday", async () => {
      const env = createEnv();
      await runScheduled(LOOP_CRON, MONDAY, env);
      expect(reportKeys(env)).toEqual(["reports/2026-W42.json"]);
    });

    it("still runs the weekly loop when the daily one fails", async () => {
      const env = createEnv();
      const err = vi.spyOn(console, "error").mockImplementation(() => {});
      mockExecuteQuery.mockRejectedValueOnce(new Error("daily boom"));
      await runScheduled(LOOP_CRON, MONDAY, env);
      expect(err.mock.calls.map((c) => c.join(" ")).filter((l) => l.includes("daily run failed"))).toHaveLength(1);
      expect(reportKeys(env)).toEqual(["reports/2026-W42.json"]);
    });

    it("logs the run's alertsSent and skipped, as an error when something was skipped", async () => {
      const log = vi.spyOn(console, "log").mockImplementation(() => {});
      const err = vi.spyOn(console, "error").mockImplementation(() => {});
      await runScheduled(LOOP_CRON, TUESDAY, createEnv());
      expect(log.mock.calls.map((c) => c.join(" ")).join("\n")).toContain("alertsSent=[]");
      expect(err).not.toHaveBeenCalled();
      // Monday's weekly run has no mailer configured, so it skips the email.
      await runScheduled(LOOP_CRON, MONDAY, createEnv());
      expect(err.mock.calls.map((c) => c.join(" ")).join("\n")).toContain("email: not configured");
    });

    it("skips issue sync once, with one line, when the GitHub App is not configured", async () => {
      const err = vi.spyOn(console, "error").mockImplementation(() => {});
      await runScheduled(LOOP_CRON, MONDAY, createEnv());
      const weekly = err.mock.calls.map((c) => c.join(" ")).filter((l) => l.includes("weekly run"));
      expect(weekly).toHaveLength(1);
      expect(weekly[0].match(/github: not configured/g)).toHaveLength(1);
    });

    it("logs a loop failure with console.error and does not throw", async () => {
      const env = createEnv();
      const err = vi.spyOn(console, "error").mockImplementation(() => {});
      mockExecuteQuery.mockRejectedValue(new Error("analytics down"));
      const settled = await runScheduled(LOOP_CRON, TUESDAY, env);
      expect(settled.every((s) => s.status === "fulfilled")).toBe(true);
      expect(err).toHaveBeenCalled();
      expect(String(err.mock.calls[0].join(" "))).toContain("analytics down");
    });

    it("runs the CDN snapshot, not the loop, on the 02:10 cron", async () => {
      const env = createEnv();
      const settled = await runScheduled(CDN_CRON, new Date("2026-10-20T02:10:00Z"), env);
      expect(mockExecuteQuery).not.toHaveBeenCalled();
      expect(settled).toHaveLength(1);
      // The snapshot's rejection is left visible to Cloudflare, not swallowed.
      expect(settled[0]).toMatchObject({ status: "rejected" });
      expect(String((settled[0] as PromiseRejectedResult).reason)).toContain("CDN fleet snapshot not configured");
    });
  });

  describe("POST /v1/admin/loop/run", () => {
    const post = (qs: string, key: string | null = "test-admin-key") =>
      makeRequest(`/v1/admin/loop/run${qs}`, { method: "POST", headers: key ? { "X-API-Key": key } : {} });

    it("rejects a missing or wrong key", async () => {
      expect((await worker.fetch(post("?mode=daily", null), createEnv() as never)).status).toBe(401);
      expect((await worker.fetch(post("?mode=daily", "nope"), createEnv() as never)).status).toBe(401);
      expect(mockExecuteQuery).not.toHaveBeenCalled();
    });

    it("rejects a mode that is not daily or weekly", async () => {
      const res = await worker.fetch(post("?mode=hourly"), createEnv() as never);
      expect(res.status).toBe(400);
    });

    it("treats an absent dry as a dry run and writes nothing", async () => {
      const env = createEnv();
      const res = await worker.fetch(post("?mode=weekly"), env as never);
      expect(res.status).toBe(200);
      const body = (await res.json()) as any;
      expect(body.mode).toBe("weekly");
      expect(body.week).toMatch(/^\d{4}-W\d{2}$/);
      expect(env.TELEMETRY_BUCKET._storage.size).toBe(0);
    });

    it("writes only when dry=0 is explicit", async () => {
      const env = createEnv();
      expect((await worker.fetch(post("?mode=weekly&dry=1"), env as never)).status).toBe(200);
      expect(env.TELEMETRY_BUCKET._storage.size).toBe(0);
      expect((await worker.fetch(post("?mode=weekly&dry=0"), env as never)).status).toBe(200);
      expect(reportKeys(env)).toHaveLength(1);
    });

    it("returns 502 with the reason when a read fails", async () => {
      mockExecuteQuery.mockRejectedValue(new Error("analytics down"));
      const res = await worker.fetch(post("?mode=daily"), createEnv() as never);
      expect(res.status).toBe(502);
      expect(((await res.json()) as any).error).toContain("analytics down");
    });

    it("is 503 when Analytics Engine is not configured", async () => {
      const env = createEnv({ HELIX_ANALYTICS_READ_TOKEN: "" });
      expect((await worker.fetch(post("?mode=daily"), env as never)).status).toBe(503);
    });
  });

  describe("GET /v1/dashboard/reports", () => {
    const get = (path: string, key = "test-admin-key") => makeRequest(path, { headers: { "X-API-Key": key } });
    const seed = (env: ReturnType<typeof createEnv>, week: string, extra = {}) =>
      env.TELEMETRY_BUCKET.put(`reports/${week}.json`, JSON.stringify({ week, ...extra }));

    it("needs the admin key", async () => {
      expect((await worker.fetch(get("/v1/dashboard/reports", "nope"), createEnv() as never)).status).toBe(401);
    });

    it("lists the stored weeks newest first, ignoring other keys", async () => {
      const env = createEnv();
      await seed(env, "2026-W41");
      await seed(env, "2026-W43");
      await seed(env, "2025-W52");
      await env.TELEMETRY_BUCKET.put("reports/notes.txt", "x");
      await env.TELEMETRY_BUCKET.put("loop/state.json", "{}");
      const res = await worker.fetch(get("/v1/dashboard/reports"), env as never);
      expect(await res.json()).toEqual({ weeks: ["2026-W43", "2026-W41", "2025-W52"] });
    });

    it("returns an empty list when nothing is stored", async () => {
      const res = await worker.fetch(get("/v1/dashboard/reports"), createEnv() as never);
      expect(await res.json()).toEqual({ weeks: [] });
    });

    it("returns one stored report", async () => {
      const env = createEnv();
      await seed(env, "2026-W42", { findings: [] });
      const res = await worker.fetch(get("/v1/dashboard/reports/2026-W42"), env as never);
      expect(res.status).toBe(200);
      expect(await res.json()).toEqual({ week: "2026-W42", findings: [] });
    });

    it.each(["2026-W4", "latest", "2026-W42.json", "..%2Floop", "2026-w42"])("rejects the week %s with 400", async (week) => {
      const res = await worker.fetch(get(`/v1/dashboard/reports/${week}`), createEnv() as never);
      expect(res.status).toBe(400);
    });

    it("is 404 for a well-formed week that is not stored", async () => {
      const res = await worker.fetch(get("/v1/dashboard/reports/2026-W42"), createEnv() as never);
      expect(res.status).toBe(404);
    });
  });
});
