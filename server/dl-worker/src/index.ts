// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cloudflare Worker: helix-dl-worker
// Proxies the helixscreen-releases R2 bucket over plain HTTP at dl.helixscreen.org.
//
// Endpoints:
//   GET  /            - Health check
//   GET  /health      - Health check
//   GET  /install.sh  - Install script
//   GET  /{channel}/manifest.json                    - Release manifest
//   GET  /{channel}/helixscreen-*.tar.gz             - Release tarball (by channel)
//   GET  /releases/{version}/helixscreen-*.tar.gz    - Release tarball (by version)
//   GET  /releases/{version}/helixscreen-*.zip       - Release zip (by version)
//   HEAD (same paths) - Metadata without body

/** Valid release channels. */
const VALID_CHANNELS = new Set(["stable", "beta", "dev"]);

/** Pattern for allowed tarball filenames. */
const TARBALL_PATTERN = /^helixscreen-[a-zA-Z0-9._-]+\.tar\.gz$/;

/**
 * Pattern for allowed zip filenames. Releases ship only zips, so an
 * HTTPS-incapable installer (BusyBox wget on the AD5M) has nothing else to
 * fetch here. The k1/ad5x zips are real objects in R2, so they need no alias.
 */
const ZIP_PATTERN = /^helixscreen-[a-zA-Z0-9._-]+\.zip$/;

/**
 * Retired board keys whose tarballs became the unified MIPS package. A
 * deployed binary keeps requesting helixscreen-{k1,ad5x}-v*.tar.gz until the
 * fleet turns over, so those URLs rewrite to the single mips object instead
 * of R2 carrying byte-identical copies under three keys. The version's 'v'
 * anchors the split: a longer key that starts with k1- or ad5x- names a
 * different artifact and must NOT be rewritten.
 */
function rewriteRetiredMipsAlias(filename: string): string {
  const match = /^helixscreen-(k1|ad5x)-(v[0-9][a-zA-Z0-9.+-]*\.tar\.gz)$/.exec(
    filename,
  );
  return match ? `helixscreen-mips-${match[2]}` : filename;
}

/** Worker environment bindings. */
interface Env {
  RELEASES_BUCKET: R2Bucket;
  DL_LIMITER: RateLimit;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    // Only allow GET and HEAD
    if (request.method !== "GET" && request.method !== "HEAD") {
      return jsonResponse(405, { error: "Method not allowed" });
    }

    const url = new URL(request.url);
    const path = url.pathname;

    // Health check
    if (path === "/" || path === "/health") {
      return jsonResponse(200, {
        service: "helix-dl-worker",
        status: "ok",
        timestamp: new Date().toISOString(),
      });
    }

    // Rate limiting (per client IP)
    const clientIP = request.headers.get("CF-Connecting-IP") || "unknown";
    const { success } = await env.DL_LIMITER.limit({ key: clientIP });
    if (!success) {
      return jsonResponse(429, { error: "Rate limit exceeded — try again later" });
    }

    // Resolve the R2 key and cache policy from the request path
    const resolved = resolvePath(path);
    if (!resolved) {
      return jsonResponse(404, { error: "Not found" });
    }

    const { r2Key, contentType, cacheControl } = resolved;

    // Fetch from R2
    const object = await env.RELEASES_BUCKET.get(r2Key);
    if (!object) {
      return jsonResponse(404, { error: "Not found" });
    }

    // HEAD: return metadata without body
    if (request.method === "HEAD") {
      return new Response(null, {
        status: 200,
        headers: {
          "Content-Type": contentType,
          "Content-Length": object.size.toString(),
          "Cache-Control": cacheControl,
          "ETag": object.httpEtag,
        },
      });
    }

    // GET: return full response
    return new Response(object.body, {
      status: 200,
      headers: {
        "Content-Type": contentType,
        "Content-Length": object.size.toString(),
        "Cache-Control": cacheControl,
        "ETag": object.httpEtag,
      },
    });
  },
} satisfies ExportedHandler<Env>;

/** Result of resolving a URL path to an R2 key. */
interface ResolvedPath {
  r2Key: string;
  contentType: string;
  cacheControl: string;
}

/**
 * Resolve a URL path to an R2 key, content type, and cache policy.
 * Returns null if the path is not in the allowlist.
 */
function resolvePath(path: string): ResolvedPath | null {
  // Strip leading slash for R2 key
  const trimmed = path.replace(/^\//, "");

  // install.sh
  if (trimmed === "install.sh") {
    return {
      r2Key: "install.sh",
      contentType: "text/x-shellscript",
      cacheControl: "public, max-age=300",
    };
  }

  const parts = trimmed.split("/");

  // {channel}/{filename} — manifest or tarball by channel
  if (parts.length === 2) {
    const [channel, filename] = parts;

    if (!VALID_CHANNELS.has(channel)) {
      return null;
    }

    // {channel}/manifest.json
    if (filename === "manifest.json") {
      return {
        r2Key: trimmed,
        contentType: "application/json",
        cacheControl: "public, max-age=300",
      };
    }

    // {channel}/helixscreen-*.tar.gz
    if (TARBALL_PATTERN.test(filename)) {
      return {
        r2Key: `${channel}/${rewriteRetiredMipsAlias(filename)}`,
        contentType: "application/gzip",
        cacheControl: "public, max-age=86400, immutable",
      };
    }

    return null;
  }

  // releases/{version}/{filename} — release archive by version
  // The HTTPS CDN (releases.helixscreen.org) uses this path structure.
  // Mirror it here so BusyBox wget (K1, AD5M) can download via plain HTTP.
  if (parts.length === 3 && parts[0] === "releases") {
    const [, version, filename] = parts;

    // Validate version format (e.g., "v0.99.25")
    if (!/^v\d+\.\d+\.\d+/.test(version)) {
      return null;
    }

    if (TARBALL_PATTERN.test(filename)) {
      return {
        r2Key: `releases/${version}/${rewriteRetiredMipsAlias(filename)}`,
        contentType: "application/gzip",
        cacheControl: "public, max-age=86400, immutable",
      };
    }

    if (ZIP_PATTERN.test(filename)) {
      return {
        r2Key: trimmed,
        contentType: "application/zip",
        cacheControl: "public, max-age=86400, immutable",
      };
    }

    return null;
  }

  return null;
}

/**
 * Build a JSON response.
 */
function jsonResponse(status: number, data: Record<string, unknown>): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json",
    },
  });
}
