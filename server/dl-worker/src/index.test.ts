// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for dl-worker R2 proxy endpoints.

import { describe, it, expect, beforeEach } from "vitest";
import worker from "./index";

// ---------- Mock R2 helpers ----------

interface StoredObject {
  key: string;
  body: ArrayBuffer;
  size: number;
  httpEtag: string;
}

function createMockBucket() {
  const storage = new Map<string, StoredObject>();

  return {
    _storage: storage,

    /** Seed a file into the mock bucket for testing. */
    seed(key: string, content: string) {
      const encoded = new TextEncoder().encode(content);
      storage.set(key, {
        key,
        body: encoded.buffer,
        size: encoded.byteLength,
        httpEtag: `"${key}-etag"`,
      });
    },

    async get(key: string) {
      const obj = storage.get(key);
      if (!obj) return null;
      return {
        key: obj.key,
        size: obj.size,
        httpEtag: obj.httpEtag,
        body: new ReadableStream({
          start(controller) {
            controller.enqueue(new Uint8Array(obj.body));
            controller.close();
          },
        }),
      };
    },
  };
}

// ---------- Mock rate limiter ----------

function createMockRateLimiter(shouldLimit = false) {
  return {
    limit: async (_opts: { key: string }) => ({ success: !shouldLimit }),
  };
}

// ---------- Test env factory ----------

function createEnv(overrides?: Partial<{
  RELEASES_BUCKET: ReturnType<typeof createMockBucket>;
  DL_LIMITER: ReturnType<typeof createMockRateLimiter>;
}>) {
  return {
    RELEASES_BUCKET: createMockBucket(),
    DL_LIMITER: createMockRateLimiter(),
    ...overrides,
  };
}

// ---------- Request helpers ----------

function makeRequest(path: string, init?: RequestInit): Request {
  return new Request(`https://dl.helixscreen.org${path}`, init);
}

// ---------- Tests ----------

describe("Health check", () => {
  it("GET / returns 200 with service info", async () => {
    const res = await worker.fetch(makeRequest("/"), createEnv());
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.service).toBe("helix-dl-worker");
    expect(data.status).toBe("ok");
    expect(data.timestamp).toBeDefined();
  });

  it("GET /health returns 200 with service info", async () => {
    const res = await worker.fetch(makeRequest("/health"), createEnv());
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data.service).toBe("helix-dl-worker");
    expect(data.status).toBe("ok");
  });
});

describe("Method restrictions", () => {
  it("POST returns 405", async () => {
    const res = await worker.fetch(
      makeRequest("/install.sh", { method: "POST" }),
      createEnv(),
    );
    expect(res.status).toBe(405);
    const data = await res.json();
    expect(data.error).toContain("Method not allowed");
  });

  it("PUT returns 405", async () => {
    const res = await worker.fetch(
      makeRequest("/install.sh", { method: "PUT" }),
      createEnv(),
    );
    expect(res.status).toBe(405);
  });

  it("DELETE returns 405", async () => {
    const res = await worker.fetch(
      makeRequest("/install.sh", { method: "DELETE" }),
      createEnv(),
    );
    expect(res.status).toBe(405);
  });
});

describe("Rate limiting", () => {
  it("returns 429 when rate limited", async () => {
    const env = createEnv({ DL_LIMITER: createMockRateLimiter(true) });
    env.RELEASES_BUCKET.seed("install.sh", "#!/bin/bash");
    const res = await worker.fetch(makeRequest("/install.sh"), env);
    expect(res.status).toBe(429);
    const data = await res.json();
    expect(data.error).toContain("Rate limit");
  });

  it("does not rate-limit health checks", async () => {
    const env = createEnv({ DL_LIMITER: createMockRateLimiter(true) });
    const res = await worker.fetch(makeRequest("/"), env);
    expect(res.status).toBe(200);
  });
});

describe("GET /install.sh", () => {
  it("returns install script with correct content type", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("install.sh", "#!/bin/bash\necho hello");
    const res = await worker.fetch(makeRequest("/install.sh"), env);
    expect(res.status).toBe(200);
    expect(res.headers.get("Content-Type")).toBe("text/x-shellscript");
    expect(res.headers.get("Cache-Control")).toBe("public, max-age=300");
    const body = await res.text();
    expect(body).toBe("#!/bin/bash\necho hello");
  });

  it("returns 404 when install.sh is missing from R2", async () => {
    const res = await worker.fetch(makeRequest("/install.sh"), createEnv());
    expect(res.status).toBe(404);
  });
});

describe("GET /{channel}/manifest.json", () => {
  it("returns manifest for stable channel", async () => {
    const env = createEnv();
    const manifest = JSON.stringify({ version: "1.0.0" });
    env.RELEASES_BUCKET.seed("stable/manifest.json", manifest);
    const res = await worker.fetch(makeRequest("/stable/manifest.json"), env);
    expect(res.status).toBe(200);
    expect(res.headers.get("Content-Type")).toBe("application/json");
    expect(res.headers.get("Cache-Control")).toBe("public, max-age=300");
    const data = await res.json();
    expect(data.version).toBe("1.0.0");
  });

  it("returns manifest for beta channel", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("beta/manifest.json", "{}");
    const res = await worker.fetch(makeRequest("/beta/manifest.json"), env);
    expect(res.status).toBe(200);
  });

  it("returns manifest for dev channel", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("dev/manifest.json", "{}");
    const res = await worker.fetch(makeRequest("/dev/manifest.json"), env);
    expect(res.status).toBe(200);
  });

  it("returns 404 for invalid channel", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("nightly/manifest.json", "{}");
    const res = await worker.fetch(makeRequest("/nightly/manifest.json"), env);
    expect(res.status).toBe(404);
  });
});

describe("GET /{channel}/helixscreen-*.tar.gz", () => {
  it("returns tarball with correct content type and cache headers", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("stable/helixscreen-1.0.0-arm.tar.gz", "binary-data");
    const res = await worker.fetch(
      makeRequest("/stable/helixscreen-1.0.0-arm.tar.gz"),
      env,
    );
    expect(res.status).toBe(200);
    expect(res.headers.get("Content-Type")).toBe("application/gzip");
    expect(res.headers.get("Cache-Control")).toBe("public, max-age=86400, immutable");
  });

  it("returns 404 when tarball does not exist in R2", async () => {
    const res = await worker.fetch(
      makeRequest("/stable/helixscreen-2.0.0-arm.tar.gz"),
      createEnv(),
    );
    expect(res.status).toBe(404);
  });

  it("returns 404 for tarball with invalid channel", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("alpha/helixscreen-1.0.0.tar.gz", "data");
    const res = await worker.fetch(
      makeRequest("/alpha/helixscreen-1.0.0.tar.gz"),
      env,
    );
    expect(res.status).toBe(404);
  });

  it("serves retired k1/ad5x tarball names from the unified mips object", async () => {
    // One binary serves the K1 series and the AD5X; the board keys live on
    // only as URL aliases so deployed binaries keep finding an update.
    const env = createEnv();
    env.RELEASES_BUCKET.seed("stable/helixscreen-mips-v1.1.0.tar.gz", "mips-bytes");
    for (const retired of ["k1", "ad5x"]) {
      const res = await worker.fetch(
        makeRequest(`/stable/helixscreen-${retired}-v1.1.0.tar.gz`),
        env,
      );
      expect(res.status, retired).toBe(200);
      expect(await res.text(), retired).toBe("mips-bytes");
    }
    // Only the mips key exists in the bucket; the aliases are rewrites.
    expect(env.RELEASES_BUCKET._storage.has("stable/helixscreen-k1-v1.1.0.tar.gz")).toBe(false);
  });

  it("does not rewrite k1-dynamic, a different artifact that shares the k1 prefix", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("stable/helixscreen-mips-v0.99.31.tar.gz", "mips-bytes");
    const res = await worker.fetch(
      makeRequest("/stable/helixscreen-k1-dynamic-v0.99.31.tar.gz"),
      env,
    );
    expect(res.status).toBe(404);
  });
});

describe("GET /releases/{version}/helixscreen-*.tar.gz", () => {
  it("returns tarball with correct content type and cache headers", async () => {
    // k1 here exercises the retired-alias rewrite: the request names the
    // board key, the bucket holds only the unified mips object.
    const env = createEnv();
    env.RELEASES_BUCKET.seed(
      "releases/v0.99.25/helixscreen-mips-v0.99.25.tar.gz",
      "binary-data",
    );
    const res = await worker.fetch(
      makeRequest("/releases/v0.99.25/helixscreen-k1-v0.99.25.tar.gz"),
      env,
    );
    expect(res.status).toBe(200);
    expect(res.headers.get("Content-Type")).toBe("application/gzip");
    expect(res.headers.get("Cache-Control")).toBe(
      "public, max-age=86400, immutable",
    );
  });

  it("returns 404 when tarball does not exist in R2", async () => {
    const res = await worker.fetch(
      makeRequest("/releases/v1.0.0/helixscreen-k1-v1.0.0.tar.gz"),
      createEnv(),
    );
    expect(res.status).toBe(404);
  });

  it("returns 404 for invalid version format", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed(
      "releases/latest/helixscreen-k1-v1.0.0.tar.gz",
      "data",
    );
    const res = await worker.fetch(
      makeRequest("/releases/latest/helixscreen-k1-v1.0.0.tar.gz"),
      env,
    );
    expect(res.status).toBe(404);
  });

  it("returns 404 for non-tarball files under releases path", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("releases/v1.0.0/secrets.txt", "secret");
    const res = await worker.fetch(
      makeRequest("/releases/v1.0.0/secrets.txt"),
      env,
    );
    expect(res.status).toBe(404);
  });
});

describe("GET /releases/{version}/helixscreen-*.zip", () => {
  it("returns the zip with its content type and cache headers", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("releases/v1.1.0/helixscreen-ad5m.zip", "zip-bytes");
    const res = await worker.fetch(
      makeRequest("/releases/v1.1.0/helixscreen-ad5m.zip"),
      env,
    );
    expect(res.status).toBe(200);
    expect(await res.text()).toBe("zip-bytes");
    expect(res.headers.get("Content-Type")).toBe("application/zip");
    expect(res.headers.get("Cache-Control")).toBe(
      "public, max-age=86400, immutable",
    );
  });

  it("returns 404 when the zip does not exist in R2", async () => {
    const res = await worker.fetch(
      makeRequest("/releases/v1.1.0/helixscreen-pi.zip"),
      createEnv(),
    );
    expect(res.status).toBe(404);
  });

  it("does not serve a zip by channel", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("stable/helixscreen-ad5m.zip", "zip-bytes");
    const res = await worker.fetch(
      makeRequest("/stable/helixscreen-ad5m.zip"),
      env,
    );
    expect(res.status).toBe(404);
  });

  it("does not serve a non-helixscreen zip under releases", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("releases/v1.1.0/other.zip", "zip-bytes");
    const res = await worker.fetch(
      makeRequest("/releases/v1.1.0/other.zip"),
      env,
    );
    expect(res.status).toBe(404);
  });
});

describe("Disallowed paths", () => {
  it("returns 404 for arbitrary paths", async () => {
    const res = await worker.fetch(makeRequest("/some/random/path"), createEnv());
    expect(res.status).toBe(404);
  });

  it("returns 404 for path traversal attempts", async () => {
    const res = await worker.fetch(makeRequest("/../etc/passwd"), createEnv());
    expect(res.status).toBe(404);
  });

  it("returns 404 for non-tarball files in channel dir", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("stable/secrets.txt", "secret");
    const res = await worker.fetch(makeRequest("/stable/secrets.txt"), env);
    expect(res.status).toBe(404);
  });

  it("returns 404 for deeply nested paths", async () => {
    const res = await worker.fetch(
      makeRequest("/stable/sub/helixscreen-1.0.0.tar.gz"),
      createEnv(),
    );
    expect(res.status).toBe(404);
  });
});

describe("HEAD requests", () => {
  it("HEAD returns metadata without body for install.sh", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("install.sh", "#!/bin/bash\necho hello");
    const res = await worker.fetch(
      makeRequest("/install.sh", { method: "HEAD" }),
      env,
    );
    expect(res.status).toBe(200);
    expect(res.headers.get("Content-Type")).toBe("text/x-shellscript");
    expect(res.headers.get("Content-Length")).toBeDefined();
    expect(res.headers.get("ETag")).toBeDefined();
    // HEAD should have null body
    expect(res.body).toBeNull();
  });

  it("HEAD returns 404 for missing file", async () => {
    const res = await worker.fetch(
      makeRequest("/install.sh", { method: "HEAD" }),
      createEnv(),
    );
    expect(res.status).toBe(404);
  });

  it("HEAD returns metadata for tarball", async () => {
    const env = createEnv();
    env.RELEASES_BUCKET.seed("beta/helixscreen-0.9.0-arm.tar.gz", "tarball-data");
    const res = await worker.fetch(
      makeRequest("/beta/helixscreen-0.9.0-arm.tar.gz", { method: "HEAD" }),
      env,
    );
    expect(res.status).toBe(200);
    expect(res.headers.get("Content-Type")).toBe("application/gzip");
    expect(res.headers.get("Cache-Control")).toBe("public, max-age=86400, immutable");
  });
});
