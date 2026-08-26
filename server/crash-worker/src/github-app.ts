// SPDX-License-Identifier: GPL-3.0-or-later
//
// GitHub App authentication for Cloudflare Workers.
// Generates installation tokens using JWT + RS256 via Web Crypto API.

import type { CrashReport, ResolvedBacktrace } from "./symbol-resolver";

const GITHUB_API = "https://api.github.com";

/** Minimal shape of a GitHub issue from the search API. */
interface GitHubSearchIssue {
  number: number;
  html_url: string;
}

interface GitHubSearchResponse {
  total_count: number;
  items: GitHubSearchIssue[];
}

interface GitHubInstallationResponse {
  id: number;
}

interface GitHubTokenResponse {
  token: string;
}

/** Entry from the git/matching-refs API. */
interface GitHubRef {
  ref: string;
}

// =============================================================================
// PEM → CryptoKey
// =============================================================================

/**
 * Import a PEM-encoded RSA private key for RS256 signing.
 * Handles both PKCS#1 (BEGIN RSA PRIVATE KEY) and PKCS#8 (BEGIN PRIVATE KEY).
 */
export async function importPrivateKey(pem: string): Promise<CryptoKey> {
  const pemBody = pem
    .replace(/-----BEGIN RSA PRIVATE KEY-----/, "")
    .replace(/-----END RSA PRIVATE KEY-----/, "")
    .replace(/-----BEGIN PRIVATE KEY-----/, "")
    .replace(/-----END PRIVATE KEY-----/, "")
    .replace(/\s/g, "");

  const binaryDer = Uint8Array.from(atob(pemBody), (c) => c.charCodeAt(0));
  const isPkcs8 = pem.includes("BEGIN PRIVATE KEY");

  // GitHub App keys are PKCS#1 — need to wrap in PKCS#8 for Web Crypto
  const keyData = isPkcs8 ? binaryDer : wrapPkcs1InPkcs8(binaryDer);

  return crypto.subtle.importKey(
    "pkcs8",
    keyData,
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
    false,
    ["sign"]
  );
}

// =============================================================================
// ASN.1 helpers for PKCS#1 → PKCS#8 wrapping
// =============================================================================

function wrapPkcs1InPkcs8(pkcs1: Uint8Array): Uint8Array {
  // PKCS#8 envelope: SEQUENCE { INTEGER 0, SEQUENCE { OID rsaEncryption, NULL }, OCTET STRING { pkcs1 } }
  const rsaOid = new Uint8Array([
    0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
    0x01, 0x05, 0x00,
  ]);
  const version = new Uint8Array([0x02, 0x01, 0x00]);
  const octetString = wrapAsn1(0x04, pkcs1);
  return wrapAsn1(0x30, concatBytes(version, rsaOid, octetString));
}

function wrapAsn1(tag: number, content: Uint8Array): Uint8Array {
  const len = encodeAsn1Length(content.length);
  const result = new Uint8Array(1 + len.length + content.length);
  result[0] = tag;
  result.set(len, 1);
  result.set(content, 1 + len.length);
  return result;
}

function encodeAsn1Length(length: number): Uint8Array {
  if (length < 0x80) return new Uint8Array([length]);
  const bytes: number[] = [];
  let tmp = length;
  while (tmp > 0) {
    bytes.unshift(tmp & 0xff);
    tmp >>= 8;
  }
  return new Uint8Array([0x80 | bytes.length, ...bytes]);
}

function concatBytes(...arrays: Uint8Array[]): Uint8Array {
  const total = arrays.reduce((sum, a) => sum + a.length, 0);
  const result = new Uint8Array(total);
  let offset = 0;
  for (const a of arrays) {
    result.set(a, offset);
    offset += a.length;
  }
  return result;
}

// =============================================================================
// JWT
// =============================================================================

function base64url(data: string | Uint8Array): string {
  const bytes = typeof data === "string" ? new TextEncoder().encode(data) : data;
  return btoa(String.fromCharCode(...bytes))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");
}

async function createJWT(appId: string, privateKey: CryptoKey): Promise<string> {
  const now = Math.floor(Date.now() / 1000);
  const header = { alg: "RS256", typ: "JWT" };
  const payload = {
    iat: now - 60,
    exp: now + 600,
    iss: appId,
  };

  const headerB64 = base64url(JSON.stringify(header));
  const payloadB64 = base64url(JSON.stringify(payload));
  const signingInput = `${headerB64}.${payloadB64}`;

  const signature = await crypto.subtle.sign(
    "RSASSA-PKCS1-v1_5",
    privateKey,
    new TextEncoder().encode(signingInput)
  );

  return `${signingInput}.${base64url(new Uint8Array(signature))}`;
}

// =============================================================================
// GitHub API
// =============================================================================

async function githubFetch(path: string, token: string, options: RequestInit = {}): Promise<Response> {
  return fetch(`${GITHUB_API}${path}`, {
    ...options,
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": "2022-11-28",
      "User-Agent": "HelixScreen-Crash-Reporter",
      ...(options.headers || {}),
    },
  });
}

/**
 * Get a short-lived installation access token for the GitHub App.
 */
export async function getInstallationToken(
  appId: string,
  pemKey: string,
  owner: string,
  repo: string
): Promise<string> {
  const key = await importPrivateKey(pemKey);
  const jwt = await createJWT(appId, key);

  // Get installation ID for the repo
  const installRes = await githubFetch(
    `/repos/${owner}/${repo}/installation`,
    jwt
  );
  if (!installRes.ok) {
    const body = await installRes.text();
    throw new Error(`Failed to get installation: ${installRes.status} ${body}`);
  }
  const { id } = (await installRes.json()) as GitHubInstallationResponse;

  // Create installation access token
  const tokenRes = await githubFetch(
    `/app/installations/${id}/access_tokens`,
    jwt,
    { method: "POST" }
  );
  if (!tokenRes.ok) {
    const body = await tokenRes.text();
    throw new Error(`Failed to create token: ${tokenRes.status} ${body}`);
  }

  const { token } = (await tokenRes.json()) as GitHubTokenResponse;
  return token;
}

/**
 * Check whether `version` corresponds to a real release tag (`v<version>`).
 *
 * Uses git/matching-refs rather than a direct ref lookup because "no such tag"
 * comes back as 200 with an empty array, which is distinguishable from a
 * permission or transport failure. Every other outcome — non-2xx, a throw, a
 * body that isn't an array — reports `true`, so a GitHub outage or a missing
 * `contents: read` scope can never silently discard a genuine crash report.
 *
 * matching-refs is a prefix query, so v0.1.4 also matches v0.1.40; only an
 * exact `refs/tags/v<version>` entry counts.
 */
export async function isKnownRelease(
  token: string,
  owner: string,
  repo: string,
  version: string
): Promise<boolean> {
  const tag = `v${version}`;
  try {
    const res = await githubFetch(
      `/repos/${owner}/${repo}/git/matching-refs/tags/${encodeURIComponent(tag)}`,
      token
    );
    if (!res.ok) return true;

    const refs = await res.json();
    if (!Array.isArray(refs)) return true;

    return refs.some((r) => (r as GitHubRef)?.ref === `refs/tags/${tag}`);
  } catch {
    return true;
  }
}

// =============================================================================
// Crash fingerprint (for dedup)
// =============================================================================

/** Longest symbol accepted in a fingerprint; deep template names blow past this. */
const MAX_FINGERPRINT_SYMBOL = 120;

/** GCC suffixes appended straight onto a bare name: .lto_priv.0, .isra.1, .cold. */
const GCC_NAME_SUFFIX = /\.(?:lto_priv|isra|part|constprop|cold|localalias)(?:\.\d+)*$/;

/**
 * Drop the trailing parameter list from a demangled C++ name, keeping any
 * leading parenthesised group. Walks back from the closing paren so
 * `(anonymous namespace)::handler(int)` loses only the arguments, and
 * `Foo::operator()(int)` keeps its `operator()`.
 */
function stripArgList(name: string): string {
  if (!name.endsWith(")")) return name;
  let depth = 0;
  for (let i = name.length - 1; i >= 0; i--) {
    const ch = name[i];
    if (ch === ")") depth++;
    else if (ch === "(") {
      depth--;
      // A name that is nothing but a parenthesised group has no arguments to drop.
      if (depth === 0) return i === 0 ? name : name.slice(0, i);
    }
  }
  return name;
}

/**
 * Reduce a resolved frame to a build-independent function name.
 * Removes the +offset, GCC's clone/specialisation suffixes and the parameter
 * list - each of which moves between architectures and between builds of the
 * same source. Returns "" for frames that carry no real symbol.
 */
export function normalizeSymbol(symbol: string | undefined): string {
  if (!symbol) return "";
  let s = symbol.trim();
  // Placeholders the resolver emits for frames it could not attribute.
  if (!s || s.startsWith("<") || s === "(unknown)") return "";

  s = s.replace(/\+0x[0-9a-f]+$/i, "");
  s = s.replace(/\s*\[clone [^\]]*\]/g, "");
  s = stripArgList(s);
  while (GCC_NAME_SUFFIX.test(s)) s = s.replace(GCC_NAME_SUFFIX, "");
  s = s.trim();

  if (!s || s === "(unknown)") return "";
  return s.length > MAX_FINGERPRINT_SYMBOL ? s.slice(0, MAX_FINGERPRINT_SYMBOL) : s;
}

/**
 * Generate a short fingerprint for a crash to detect duplicates.
 *
 * Keyed on the resolved crash symbol plus the version. Signal and PC are
 * deliberately absent: both move with the architecture, so one defect files
 * once per platform. #1347, #1356, #1357 and #1361 were the same use-after-free
 * in `gcode_viewer_occluder_delete_cb` on one build - SEGV at 0xa5a5a6ad on ARM,
 * BUS at 0x0 on MIPS, a different PC on each - filed as four issues. The symbol
 * is the part that does not move.
 *
 * The version stays in the key so a defect that survives into the next release
 * files fresh rather than reviving a closed issue.
 *
 * Falls back to the old signal/version/PC shape when the top frame has no
 * symbol - no map published for that build, or a fault inside a shared library.
 */
export function crashFingerprint(
  report: CrashReport,
  resolved?: ResolvedBacktrace | null
): string {
  const ver = report.app_version || "unknown";

  const symbol = normalizeSymbol(resolved?.frames?.[0]?.symbol);
  if (symbol) return `${symbol}/${ver}`;

  const sig = report.signal_name || `SIG${report.signal}`;
  const frame = report.backtrace?.[0] || "no-bt";
  return `${sig}/${ver}/${frame}`;
}

/**
 * Search for an existing open issue with the same crash fingerprint.
 */
export async function findExistingIssue(
  token: string,
  owner: string,
  repo: string,
  fingerprint: string
): Promise<GitHubSearchIssue | null> {
  // Search the labelled body line rather than the bare fingerprint. GitHub
  // tokenises the query, and a symbol-keyed fingerprint also appears in the
  // backtrace table of every issue whose crash merely passed through that
  // function - so the bare form can attach a report to an unrelated issue. The
  // "Fingerprint:" prefix occurs only on the line the worker writes.
  const query = encodeURIComponent(
    `repo:${owner}/${repo} is:issue is:open label:crash "Fingerprint: ${fingerprint}" in:body`
  );
  const res = await githubFetch(`/search/issues?q=${query}&per_page=1`, token);
  if (!res.ok) return null;

  const data = (await res.json()) as GitHubSearchResponse;
  return data.total_count > 0 ? data.items[0] : null;
}

/**
 * Add a comment to an existing issue noting an additional occurrence.
 */
export async function addDuplicateComment(
  token: string,
  owner: string,
  repo: string,
  issueNumber: number,
  report: CrashReport,
  fingerprint: string
): Promise<void> {
  const comment = [
    `## Additional occurrence`,
    "",
    `Another device reported this crash:`,
    `- **Timestamp:** ${report.timestamp || new Date().toISOString()}`,
    `- **Platform:** ${report.platform || "unknown"}`,
    `- **Uptime:** ${report.uptime_seconds != null ? report.uptime_seconds + "s" : "unknown"}`,
    report.ram_mb ? `- **RAM:** ${report.ram_mb} MB` : "",
    report.printer_model ? `- **Printer:** ${report.printer_model}` : "",
    "",
    `<sub>Fingerprint: \`${fingerprint}\`</sub>`,
  ]
    .filter(Boolean)
    .join("\n");

  await githubFetch(`/repos/${owner}/${repo}/issues/${issueNumber}/comments`, token, {
    method: "POST",
    body: JSON.stringify({ body: comment }),
  });
}
