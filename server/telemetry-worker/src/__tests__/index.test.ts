// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for telemetry worker endpoints.

import { describe, it, expect, vi, beforeEach } from "vitest";
import worker from "../index";
import { mapEventToDataPoints } from "../analytics";
import { parseRange } from "../queries";

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
    expect(data).toEqual({ status: "healthy" });
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
    expect(dp.blobs).toHaveLength(12);
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
    expect(dp.blobs).toHaveLength(12);
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
      expect(data.print_success_rate).toBe(0);
      expect(data.events_over_time).toEqual([]);
    });
  });

  // -- GET /v1/dashboard/adoption --

  describe("GET /v1/dashboard/adoption", () => {
    it("returns adoption data with correct shape", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({ data: [{ name: "linux-arm", count: 100 }] })
        .mockResolvedValueOnce({ data: [{ name: "0.9.19", count: 80 }] })
        .mockResolvedValueOnce({ data: [{ name: "Voron 2.4", count: 50 }] })
        .mockResolvedValueOnce({ data: [{ name: "corexy", count: 90 }] });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/adoption?range=7d"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.platforms).toEqual([{ name: "linux-arm", count: 100 }]);
      expect(data.versions).toEqual([{ name: "0.9.19", count: 80 }]);
      expect(data.printer_models).toEqual([{ name: "Voron 2.4", count: 50 }]);
      expect(data.kinematics).toEqual([{ name: "corexy", count: 90 }]);
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

      // Default limit (no param)
      await worker.fetch(dashboardRequest("/v1/dashboard/crash-list"), env);
      const firstCall = mockExecuteQuery.mock.calls[0][1] as string;
      expect(firstCall).toContain("LIMIT 50");

      mockExecuteQuery.mockClear();

      // Over 200 gets clamped
      await worker.fetch(dashboardRequest("/v1/dashboard/crash-list?limit=500"), env);
      const secondCall = mockExecuteQuery.mock.calls[0][1] as string;
      expect(secondCall).toContain("LIMIT 200");
    });
  });

  // -- GET /v1/dashboard/releases --

  describe("GET /v1/dashboard/releases", () => {
    it("returns release comparison data", async () => {
      mockExecuteQuery
        .mockResolvedValueOnce({
          data: [{ ver: "0.9.19", total_sessions: 200, total_crashes: 4 }],
        })
        .mockResolvedValueOnce({
          data: [{ ver: "0.9.19", active_devices: 50 }],
        })
        .mockResolvedValueOnce({
          data: [{ ver: "0.9.19", print_successes: 88, print_total: 100 }],
        });

      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/releases?versions=v0.9.19"),
        env,
      );
      expect(res.status).toBe(200);
      const data = await res.json();
      expect(data.versions).toHaveLength(1);
      expect(data.versions[0].version).toBe("0.9.19");
      expect(data.versions[0].active_devices).toBe(50);
      expect(data.versions[0].crash_rate).toBeCloseTo(0.02);
      expect(data.versions[0].print_success_rate).toBeCloseTo(0.88);
      expect(data.versions[0].total_sessions).toBe(200);
      expect(data.versions[0].total_crashes).toBe(4);
    });

    it("requires versions parameter", async () => {
      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/releases"),
        env,
      );
      expect(res.status).toBe(400);
      const data = await res.json();
      expect(data.error).toContain("versions parameter required");
    });

    it("rejects empty versions parameter", async () => {
      const res = await worker.fetch(
        dashboardRequest("/v1/dashboard/releases?versions="),
        env,
      );
      expect(res.status).toBe(400);
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

// ---------- Backfill endpoint tests ----------

describe("POST /v1/admin/backfill", () => {
  let env: ReturnType<typeof createEnv>;

  function backfillRequest(body: unknown, apiKey = "test-admin-key") {
    return makeRequest("/v1/admin/backfill", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-API-Key": apiKey,
      },
      body: JSON.stringify(body),
    });
  }

  beforeEach(() => {
    env = createEnv();
  });

  // -- Auth --

  it("rejects unauthenticated requests", async () => {
    const req = makeRequest("/v1/admin/backfill", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ events: [] }),
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(401);
  });

  it("rejects INGEST key", async () => {
    const res = await worker.fetch(
      backfillRequest({ events: [validEvent()] }, "test-ingest-key"),
      env,
    );
    expect(res.status).toBe(401);
  });

  // -- Validation --

  it("rejects non-JSON content type", async () => {
    const req = makeRequest("/v1/admin/backfill", {
      method: "POST",
      headers: {
        "Content-Type": "text/plain",
        "X-API-Key": "test-admin-key",
      },
      body: "not json",
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("application/json");
  });

  it("rejects invalid JSON", async () => {
    const req = makeRequest("/v1/admin/backfill", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-API-Key": "test-admin-key",
      },
      body: "not json{{{",
    });
    const res = await worker.fetch(req, env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("Invalid JSON");
  });

  it("rejects array body (must be object)", async () => {
    const res = await worker.fetch(backfillRequest([validEvent()]), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("object");
  });

  it("rejects body without events array", async () => {
    const res = await worker.fetch(backfillRequest({ events: "not array" }), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("events must be an array");
  });

  it("rejects empty events array", async () => {
    const res = await worker.fetch(backfillRequest({ events: [] }), env);
    expect(res.status).toBe(400);
    const data = await res.json();
    expect(data.error).toContain("empty");
  });

  it("returns 503 when Analytics Engine is not configured", async () => {
    const noAnalyticsEnv = createEnv();
    (noAnalyticsEnv as Record<string, unknown>).TELEMETRY_ANALYTICS = undefined;
    const res = await worker.fetch(
      backfillRequest({ events: [validEvent()] }),
      noAnalyticsEnv,
    );
    expect(res.status).toBe(503);
    const data = await res.json();
    expect(data.error).toContain("not configured");
  });

  // -- Happy path --

  it("writes events to Analytics Engine and returns count", async () => {
    const events = [
      { event: "session", device_id: "dev-1", version: "0.9.19" },
      { event: "crash", device_id: "dev-2", signal_name: "SIGSEGV" },
      { event: "print_outcome", device_id: "dev-3", outcome: "success" },
    ];
    const res = await worker.fetch(backfillRequest({ events }), env);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.status).toBe("ok");
    expect(data.written).toBe(3);
    expect(env.TELEMETRY_ANALYTICS.writeDataPoint).toHaveBeenCalledTimes(3);
  });

  it("continues backfilling when individual events fail", async () => {
    const analytics = createMockAnalyticsEngine();
    let callCount = 0;
    analytics.writeDataPoint = vi.fn(() => {
      callCount++;
      if (callCount === 2) throw new Error("bad event");
    });
    const failEnv = createEnv({ TELEMETRY_ANALYTICS: analytics });

    const events = [
      { event: "session", device_id: "dev-1" },
      { event: "bad", device_id: "dev-2" },
      { event: "session", device_id: "dev-3" },
    ];
    const res = await worker.fetch(backfillRequest({ events }), failEnv);
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.written).toBe(2); // 1st and 3rd succeed
    expect(analytics.writeDataPoint).toHaveBeenCalledTimes(3);
  });

  it("does not write to R2 (backfill is Analytics Engine only)", async () => {
    const putSpy = vi.spyOn(env.TELEMETRY_BUCKET, "put");
    const events = [{ event: "session", device_id: "dev-1" }];
    await worker.fetch(backfillRequest({ events }), env);
    expect(putSpy).not.toHaveBeenCalled();
  });
});
