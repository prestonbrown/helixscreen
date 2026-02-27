# Telemetry Admin Guide

Administration guide for HelixScreen's telemetry pipeline and analytics dashboard.

## Architecture

```
[Devices] → POST /v1/events → [Worker] → R2 (raw archive, permanent)
                                        → Analytics Engine (queryable, 90-day)

[Vue Dashboard] → GET /v1/dashboard/* → [Worker] → Analytics Engine SQL API
      ↑
  Cloudflare Pages (analytics.helixscreen.org)
```

- **R2**: Permanent raw event archive in EU jurisdiction
- **Analytics Engine**: 90-day queryable store for dashboard queries
- **Worker**: `telemetry.helixscreen.org` — handles ingest + admin API + dashboard API
- **Dashboard**: `analytics.helixscreen.org` — Vue SPA on Cloudflare Pages

## Secrets & Configuration

### Worker Secrets (set via `wrangler secret put`)

| Secret | Purpose |
|--------|---------|
| `INGEST_API_KEY` | Baked into the HelixScreen binary. Write-only ingest access. |
| `ADMIN_API_KEY` | Server-side secret. Read access to events, dashboard queries, backfill. |
| `HELIX_ANALYTICS_READ_TOKEN` | Cloudflare API token for Analytics Engine SQL queries. |

### Worker Config (in `wrangler.toml`)

| Var | Purpose |
|-----|---------|
| `CLOUDFLARE_ACCOUNT_ID` | Your Cloudflare account ID (not secret). |

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
| `memory_snapshot` | Periodic memory check | trigger, uptime_sec, rss_kb, vm_size_kb, vm_peak_kb, vm_hwm_kb |
| `hardware_profile` | Hardware/feature inventory | printer.*, mcus.*, extruders.*, fans.*, capabilities.*, ams.*, tools.* |
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
| `panel_usage` | App shutdown | Once per session |
| `connection_stability` | App shutdown | Once per session |
| `print_outcome` | Print reaches terminal state | Per print |
| `print_start_context` | Print starts (metadata callback) | Per print |
| `error_encountered` | On non-fatal error | Rate-limited: 1/category/5min |
| `crash` | Next boot after crash | Once per crash |
| `update_failed` | Update failure | Per failure |
| `update_success` | Next boot after update | Once per update |

> **Note:** The dashboard views (Overview, Adoption, Prints, Crashes, Releases) currently query `session`, `print_outcome`, and `crash` events. Dashboard views for the newer event types (`hardware_profile`, `settings_snapshot`, `memory_snapshot`, `panel_usage`, `connection_stability`, `print_start_context`, `error_encountered`, `update_failed`, `update_success`) will be added in a future update.
