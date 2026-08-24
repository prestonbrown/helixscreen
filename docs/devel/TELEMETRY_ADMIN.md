# Telemetry Admin Guide

Administration guide for HelixScreen's telemetry pipeline and analytics dashboard.

## Architecture

```
[Devices] → POST /v1/events → [Worker] → R2 (raw archive, permanent)
                                        → Analytics Engine (queryable, 90-day)

[Cron 02:10 UTC] → [Worker.scheduled] → Cloudflare zone analytics (GraphQL)
                                      → R2 cdn/ (permanent)
                                      → Analytics Engine (cdn_fleet_daily)

[Vue Dashboard] → GET /v1/dashboard/* → [Worker] → Analytics Engine SQL API
      ↑
  Cloudflare Pages (analytics.helixscreen.org)
```

- **R2**: Permanent raw event archive in EU jurisdiction
- **Analytics Engine**: 90-day queryable store for dashboard queries
- **Worker**: `telemetry.helixscreen.org` — handles ingest + admin API + dashboard API
- **Dashboard**: `analytics.helixscreen.org` — Vue SPA on Cloudflare Pages

## Fleet size: two numbers that do not agree

The dashboard Overview shows two population figures, and they measure different
things. Quoting the wrong one understates the project by roughly half.

| Tile | Source | What it counts |
|------|--------|----------------|
| **Active Devices** | `session` events in Analytics Engine | Installs whose owner **opted into telemetry** and that launched in the window |
| **Fleet (CDN)** | `cdn_fleet_daily`, from zone HTTP analytics | Distinct addresses that polled the update manifest, **regardless of telemetry opt-in** |

Telemetry is opt-in and defaults OFF, so `Active Devices` is a self-selected
floor. Every install polls `{channel}/manifest.json` 15s after startup and every
24h thereafter (`UpdateChecker`), whether or not telemetry is on - so the CDN
figure sees installs the telemetry pipeline never will. When first measured
(2026-08-14) they read 549 and ~470 respectively, but over incomparable windows:
549 was 30 days of opt-in devices, ~470 was **a single day** of distinct
addresses. The 8-day distinct union was 1,092.

**Read `Fleet (CDN)` as an estimate with error bars in both directions.** NAT
puts several printers behind one address and undercounts; dynamic residential
IPs split one printer across several days and overcount. It is a better
population estimate than `Active Devices`, not a headcount.

### Why a scheduled snapshot rather than a counter

`dl-worker` cannot count these polls. 99.9% of them are served by
`releases.helixscreen.org`, an R2 custom domain that never executes Worker code;
only ~0.1% reach `dl.helixscreen.org`. The numbers therefore come from
Cloudflare's zone HTTP analytics, which **retain only about 8 days** on the
current plan. The daily cron exists because anything not snapshotted inside that
window is gone permanently - there is no backfill beyond ~8 days, ever.

### Privacy

The snapshot computes the cardinality of client IPs in memory and discards the
addresses. Every persisted field is an integer or a date. `cdn_fleet_daily`
carries no `device_id` and no per-install identifier of any kind, which makes it
strictly less identifying than the opt-in telemetry it sits beside.

### Operating it

```bash
source .env.telemetry
BASE=https://telemetry.helixscreen.org/v1/admin/cdn-snapshot

# One specific UTC day.
curl -X POST -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" "$BASE?date=2026-08-14"

# Backfill the whole retention window - ONE DAY PER REQUEST, on purpose.
for i in $(seq 1 8); do
  D=$(date -u -d "$i days ago" +%F)
  curl -sS -X POST -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" "$BASE?date=$D"
  echo
done
```

Safe to repeat: reads aggregate with `max()`, so duplicate rows collapse rather
than double-count.

**`days` is clamped to 3, and a longer backfill must be looped one day per
request.** Analytics Engine silently drops `writeDataPoint` calls past a ceiling
within a single invocation. Measured 2026-08-15: a `days=8` run wrote 24 points
(8 days x 3 channels), persisted only the **first four days**, and still
reported all eight as `captured` - because the snapshot and the R2 write both
succeeded and only the fire-and-forget AE write was dropped. Re-running the
missing days individually persisted them correctly.

So `captured` in the response means "queried and archived to R2", **not**
"queryable in the dashboard". Confirm against the dashboard before believing a
large backfill:

```bash
curl -sS -H "X-API-Key: $HELIX_TELEMETRY_ADMIN_KEY" \
  "https://telemetry.helixscreen.org/v1/dashboard/overview?range=30d" \
  | python3 -c "import json,sys; print(len(json.load(sys.stdin)['cdn_fleet']), 'days queryable')"
```

Allow a minute or two for AE ingestion before concluding a day is missing.

**A non-zero `errors` count on a channel means installs on it are failing their
update check.** This is not cosmetic: it was how we found that
/beta/manifest.json was returning 404 to ~19 devices a day before the 1.0
branch cut created it.

**Known gap:** the Overview query filters `blob2 = 'stable'`, so the tile's
error count only reflects the stable channel. Beta and dev rows *are* written
and stored - they are just not surfaced yet. To see them, query the snapshot
endpoint output directly or the R2 archive under `cdn/`. Adding a per-channel
breakdown to the dashboard is the obvious next step if beta traffic ever
matters more than it does today.

## Secrets & Configuration

### Worker Secrets (set via `wrangler secret put`)

| Secret | Purpose |
|--------|---------|
| `INGEST_API_KEY` | Baked into the HelixScreen binary. Write-only ingest access. |
| `ADMIN_API_KEY` | Server-side secret. Read access to events, dashboard queries, backfill. |
| `HELIX_ANALYTICS_READ_TOKEN` | Cloudflare API token for Analytics Engine SQL queries. |
| `CF_ZONE_ANALYTICS_TOKEN` | Optional. Cloudflare API token with **Zone Analytics:Read** on `helixscreen.org`, for the daily CDN fleet snapshot. Falls back to `HELIX_ANALYTICS_READ_TOKEN` if unset - which works only if that token also carries zone analytics scope (Account Analytics:Read alone is not enough). |

### Worker Config (in `wrangler.toml`)

| Var | Purpose |
|-----|---------|
| `CLOUDFLARE_ACCOUNT_ID` | Your Cloudflare account ID (not secret). |
| `CDN_ZONE_ID` | Zone whose HTTP analytics carry the manifest polls (`helixscreen.org`). Not secret. |

### Local Environment

Create `.env.telemetry` in the project root (gitignored):
```bash
HELIX_TELEMETRY_ADMIN_KEY=your-admin-api-key-here
```

This is auto-loaded by `telemetry-pull.sh` and `telemetry-backfill.sh`.

## Dashboard

### URL

`https://analytics.helixscreen.org`

### Login

Enter the `ADMIN_API_KEY` value. Stored in browser sessionStorage (clears when tab closes).

### Views

| View | Shows |
|------|-------|
| **Overview** | Active devices, total events, crash rate, print success rate, events-over-time chart |
| **Adoption** | Platform, version, printer model, kinematics distributions |
| **Prints** | Print success rate over time, by filament type, average duration |
| **Crashes** | Crash rate by version, by signal type, average uptime before crash |
| **Releases** | Side-by-side version comparison (select versions to compare) |

All views support 7d / 30d / 90d time range selection.

## Worker API Endpoints

### Ingest (client binary)

```bash
POST /v1/events
X-API-Key: <INGEST_API_KEY>
Content-Type: application/json

[{ "schema_version": 2, "event": "session", "device_id": "...", "timestamp": "...", ... }]
```

- Batch of 1-20 events per request
- Rate limited: 10 requests/min per IP
- Dual-writes to R2 (permanent) + Analytics Engine (90-day)

### Admin Endpoints (all require `X-API-Key: <ADMIN_API_KEY>`)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/v1/events/list?prefix=events/2026/01/` | GET | List R2 event files |
| `/v1/events/get?key=events/2026/01/15/...json` | GET | Download a single event file |
| `/v1/admin/backfill` | POST | Write events to Analytics Engine (for backfill) |
| `/v1/dashboard/overview?range=30d` | GET | Dashboard overview metrics |
| `/v1/dashboard/adoption?range=7d` | GET | Adoption/distribution metrics |
| `/v1/dashboard/prints?range=30d` | GET | Print reliability metrics |
| `/v1/dashboard/crashes?range=30d` | GET | Crash analytics |
| `/v1/dashboard/releases?versions=v0.9.18,v0.9.19` | GET | Per-version comparison |

### Other

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check |
| `/v1/symbols/:version` | GET | List symbol files for crash resolution |

## Scripts

All scripts live in `scripts/` and auto-load `.env.telemetry`.

### telemetry-pull.sh

Pull raw event data from R2 to local disk for offline analysis.

```bash
# Pull last 30 days (default)
./scripts/telemetry-pull.sh

# Pull specific date range
./scripts/telemetry-pull.sh --since 2026-01-01 --until 2026-02-01

# See what would be downloaded
./scripts/telemetry-pull.sh --dry-run
```

Downloaded to `.telemetry-data/events/` (gitignored).

### telemetry-backfill.sh

Backfill Analytics Engine from existing R2 data. Use after first enabling Analytics Engine, or if data gets out of sync.

```bash
# Backfill last 90 days (default, max Analytics Engine retention)
./scripts/telemetry-backfill.sh

# Backfill specific range
./scripts/telemetry-backfill.sh --since 2025-12-01 --until 2026-02-13

# Preview without writing
./scripts/telemetry-backfill.sh --dry-run
```

### telemetry-analyze.py / telemetry-crashes.py

Offline Python analysis of pulled event data. Requires `.venv/`:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r scripts/telemetry-requirements.txt

# General analytics
python3 scripts/telemetry-analyze.py .telemetry-data/events/

# Crash analysis
python3 scripts/telemetry-crashes.py .telemetry-data/events/
```

## Deployment

### Worker

```bash
cd server/telemetry-worker
wrangler deploy
```

### Dashboard

```bash
cd server/analytics-dashboard
npm run build
wrangler pages deploy dist --project-name=helixscreen-analytics
```

### Creating the Analytics Engine API Token

If `HELIX_ANALYTICS_READ_TOKEN` needs to be recreated:

1. Go to [Cloudflare API Tokens](https://dash.cloudflare.com/profile/api-tokens)
2. **Create Token** → **Create Custom Token**
3. Name: `helixscreen-analytics-read`
4. Permissions: Account → **Analytics** → **Read**
5. Account Resources: Include → your account
6. Create, then: `cd server/telemetry-worker && wrangler secret put HELIX_ANALYTICS_READ_TOKEN`

## Data Retention

- **R2**: Permanent. Raw event JSON files partitioned by date.
- **Analytics Engine**: 90-day rolling window. Dashboard queries only see the last 90 days.
- **Offline**: Run `telemetry-pull.sh` periodically to archive data locally beyond 90 days.

## Event Types

| Event | Description | Key Fields |
|-------|-------------|------------|
| `session` | App startup | platform, version, printer_model, kinematics, display, ram, cpu_cores |
| `print_outcome` | Print completed/failed | outcome, filament_type, duration, temps, filament_used |
| `crash` | App crash | signal_name, version, platform, uptime, backtrace_depth |
| `update_failed` | Update attempt failed | reason, version, from_version, platform, http_code |
| `update_success` | Update completed | version, from_version, platform |
| `memory_snapshot` | Periodic memory check | trigger, uptime_sec, rss_kb, vm_size_kb, vm_peak_kb, vm_hwm_kb, private_dirty_kb, private_clean_kb, shared_clean_kb, pss_kb, system_total_mb, system_available_mb |
| `memory_warning` | Memory threshold breach | level, reason, rss_kb, vm_size_kb, vm_hwm_kb, vm_swap_kb, system_total_mb, system_available_mb, growth_5min_kb, private_dirty_kb, pss_kb |
| `hardware_profile` | Hardware/feature inventory | printer.*, mcus.*, extruders.*, fans.*, steppers.*, leds.*, sensors.*, probe.*, capabilities.*, ams.*, tools.*, macros.*, printer_objects[] |
| `settings_snapshot` | User configuration | theme, brightness_pct, locale, animations_enabled, time_format |
| `panel_usage` | Session navigation summary | session_duration_sec, panel_time_sec.*, panel_visits.*, overlay_open_count |
| `connection_stability` | Connection lifecycle | connect_count, disconnect_count, total_connected_sec, klippy_error_count |
| `print_start_context` | Print metadata at start | source, has_thumbnail, file_size_bucket, slicer, ams_active |
| `error_encountered` | Rate-limited error log | category, code, context, uptime_sec |

### Event Trigger Summary

| Event | When | Frequency |
|-------|------|-----------|
| `session` | After printer discovery | Once per launch |
| `hardware_profile` | After printer discovery | Once per launch |
| `settings_snapshot` | After printer discovery | Once per launch |
| `memory_snapshot` | Session start + hourly timer | ~1/hour |
| `memory_warning` | Memory threshold breach | Rate-limited: 1/level/5min |
| `panel_usage` | App shutdown | Once per session |
| `connection_stability` | App shutdown | Once per session |
| `print_outcome` | Print reaches terminal state | Per print |
| `print_start_context` | Print starts (metadata callback) | Per print |
| `error_encountered` | On non-fatal error | Rate-limited: 1/category/5min |
| `crash` | Next boot after crash | Once per crash |
| `update_failed` | Update failure | Per failure |
| `update_success` | Next boot after update | Once per update |

> **Note:** The dashboard views (Overview, Adoption, Prints, Crashes, Releases) currently query `session`, `print_outcome`, and `crash` events. Dashboard views for the newer event types (`hardware_profile`, `settings_snapshot`, `memory_snapshot`, `memory_warning`, `panel_usage`, `connection_stability`, `print_start_context`, `error_encountered`, `update_failed`, `update_success`) will be added in a future update.

### hardware_profile Event Details

Recorded once per session after printer discovery. Contains a full hardware inventory including name-level data for printer detection analysis.

**Name arrays** (for detection heuristic development):

| Field | Source | Cap | Description |
|-------|--------|-----|-------------|
| `fans.names[]` | `PrinterDiscovery::fans()` | 200 | Fan object names (e.g., `"heater_fan hotend_fan"`) |
| `sensors.temperature_names[]` | `PrinterDiscovery::sensors()` | 200 | Temperature sensor names (e.g., `"chamber"`, `"tvocValue"`) |
| `sensors.filament_names[]` | `PrinterDiscovery::filament_sensor_names()` | 200 | Filament sensor names |
| `leds.names[]` | `PrinterDiscovery::leds()` | 200 | LED/neopixel object names |
| `steppers.names[]` | `PrinterDiscovery::steppers()` | 200 | Stepper motor names |
| `macros.names[]` | `PrinterDiscovery::macros()` | 200 | G-code macro names |
| `printer_objects[]` | `PrinterDiscovery::printer_objects()` | 500 | Full Klipper object list |

These names are firmware/config-defined identifiers (no PII). Hostnames are explicitly excluded.

**Analysis:** Use `scripts/telemetry-printer-profiles.py` to analyze hardware profiles — finds discriminating names per detected model, clusters undetected printers, and generates candidate heuristics for `config/printer_database.json`.

### memory_warning Event Details

Fired when `MemoryMonitor` detects a threshold breach. Rate-limited to at most one event per pressure level per 5 minutes.

**Pressure levels:**

| Level | Meaning | Log Level |
|-------|---------|-----------|
| `elevated` | RSS growth exceeding 5-min threshold | INFO |
| `warning` | RSS or available memory past warning threshold | WARN |
| `critical` | RSS or available memory past critical threshold | ERROR |

**Thresholds by device tier:**

| Tier | Total RAM | Warn RSS | Critical RSS | Warn Available | Critical Available | Growth/5min |
|------|-----------|----------|--------------|----------------|--------------------|-------------|
| Constrained | <256 MB | 15 MB | 20 MB | 15 MB | 8 MB | 1 MB |
| Normal | 256-512 MB | 120 MB | 180 MB | 32 MB | 16 MB | 3 MB |
| Good | >512 MB | 180 MB | 230 MB | 48 MB | 24 MB | 5 MB |

**Event fields:**

```json
{
  "schema_version": 2,
  "event": "memory_warning",
  "device_id": "<hashed>",
  "timestamp": "<ISO8601>",
  "level": "warning",
  "reason": "RSS 185MB exceeds warning threshold 180MB",
  "uptime_sec": 3600,
  "rss_kb": 189440,
  "vm_size_kb": 524288,
  "vm_hwm_kb": 195000,
  "vm_swap_kb": 0,
  "system_total_mb": 1024,
  "system_available_mb": 512,
  "growth_5min_kb": 2048,
  "private_dirty_kb": 150000,
  "private_clean_kb": 20000,
  "shared_clean_kb": 15000,
  "shared_dirty_kb": 1000,
  "pss_kb": 170000,
  "swap_pss_kb": 0
}
```

**Implementation:** `MemoryMonitor` (background thread, 5s sampling) → `WarningCallback` → `TelemetryManager::record_memory_warning()`. Growth tracking uses a circular buffer of 10 RSS samples at 30s intervals (5-minute window).
