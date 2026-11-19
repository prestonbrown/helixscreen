# CI/CD Guide for HelixScreen

This guide documents the GitHub Actions CI/CD setup for HelixScreen, including platform-specific builds and quality checks.

## Table of Contents

- [Overview](#overview)
- [Workflows](#workflows)
- [Platform-Specific Dependencies](#platform-specific-dependencies)
- [Dependency Build Order](#dependency-build-order)
- [Quality Checks](#quality-checks)
- [CI Watch](#ci-watch)
- [Testing Locally](#testing-locally)
- [R2 Release Pipeline](#r2-release-pipeline)
- [Troubleshooting](#troubleshooting)

## Overview

HelixScreen uses GitHub Actions for continuous integration across multiple platforms. `.github/workflows/` holds:

| Workflow | Runs on | Does |
|----------|---------|------|
| `build.yml` | push to `main`, `claude/**` (not `claude/report-*`), `release/**`; PRs to `main`, `release/**` | Multi-platform builds (Ubuntu 22.04, macOS 15, Android), sharded unit tests, shell tests, plugin and telemetry-worker tests |
| `quality.yml` | pushes and PRs | Code quality checks via the shared script |
| `server-tests.yml` | `server/**` changes | Cloudflare Worker suites and the analytics dashboard build |
| `lint-xml.yml` | changes under `ui_xml/`, `src/ui/` | XML lint |
| `esp32-build.yml` | PRs, `main`, `release/**` (not `release/1.0`); `workflow_call` from `release.yml` | K-Touch firmware build and image-size check |
| `release.yml` | `v*` tags; `workflow_dispatch` (always a dry run) | Release builds, R2 publish |
| `nightly.yml` | nightly schedule, manual | Full test suite |
| `ccache-warm.yml`, `cache-prune.yml` | schedule, manual | Keep release ccaches warm; delete superseded ones |
| `cleanup-r2.yml`, `recover-r2.yml` | manual | Prune old R2 releases; republish a tag's R2 upload |
| `test-quality-deep.yml` | manual | `cov-diff` or `mutate-diff` on a base ref |
| `auto-milestone.yml`, `telemetry-printers.yml` | issue opened; weekly | Issue triage automation |
| `ci-watch.yml` | a watched workflow finishing on `main`; hourly; manual | Reruns infrastructure failures once; opens and closes the `ci-red` issue |

A push to a `claude/**` branch or a PR cancels the older run of the same ref; `main` never cancels an in-flight run, and `release/**` keys on the SHA so every commit keeps its own run.

## Workflows

### Build Workflow

**File:** `.github/workflows/build.yml`

The build workflow runs parallel compile/test/build jobs on Ubuntu, macOS, and Android, then aggregates results. Its jobs:

| Job | Name |
|-----|------|
| `compile-ubuntu` | Compile (Ubuntu 22.04) |
| `test-ubuntu` | Test (Ubuntu 22.04), a matrix of legs |
| `build-macos` | Build (macOS 15) |
| `build-android` | Build (Android) |
| `link-aux-binaries` | Link splash + watchdog (non-LTO) |
| `test-shell` | Shell Tests (BATS) |
| `test-plugin-py38` | Moonraker Plugin Tests (Python 3.8, the plugin's floor) |
| `test-telemetry-worker` | Telemetry Worker Tests (vitest) |
| `prune-ccache` | Prune superseded ccaches |
| `build-status` | Aggregates the others; fails if any failed. `devel/**` branches are reported, not blocked |

**Key Steps:**
1. Checkout with recursive submodules
2. Install platform-specific dependencies
3. Verify submodule initialization
4. Install Node.js dependencies (font converters)
5. Generate fonts (platform-conditional)
6. Build dependencies (libhv, wpa_supplicant)
7. Build HelixScreen
8. Upload binary artifacts (7-day retention)

**Artifacts:**
- `helix-screen-ubuntu-22.04` - Ubuntu binary
- `helix-screen-macos-15` - macOS binary

### Server Tests Workflow

**File:** `.github/workflows/server-tests.yml`

Pure Node; needs neither submodules nor the C++ toolchain, so it runs apart from
Build and finishes in well under a minute.

| Job | Covers |
|-----|--------|
| `crash-worker` / `dl-worker` / `telemetry-worker` (matrix) | `npm ci && npm test` (vitest) |
| `analytics-dashboard` | `npm run build`, which is `vue-tsc --noEmit && vite build` |

`fail-fast` is off so one red worker does not hide the state of the other two.
The dashboard job type-checks the SPA, which is what catches a view reading a
field the worker does not actually return.

**Lockfiles are committed for `server/*` only.** `.gitignore` ignores
`package-lock.json` repo-wide with a `!server/*/package-lock.json` exception,
because `npm ci` requires one and floating transitive dependencies would
otherwise turn CI red with no code change. Regenerate with
`npm install --package-lock-only` and commit the result alongside any
`package.json` change.

### Quality Workflow

**File:** `.github/workflows/quality.yml`

Simple workflow that runs the shared quality checks script:

```yaml
name: Code Quality

on:
  push:
    branches: [ main, "claude/**", "!claude/report-*", "release/**" ]
  pull_request:
    branches: [ main, "release/**" ]

jobs:
  quality-checks:
    name: Code Quality Checks
    runs-on: ubuntu-22.04
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
    - name: Install pinned tooling (clang-format) into .venv
      run: |
        python3 -m venv .venv
        .venv/bin/pip install -r requirements.txt
    - name: Run quality checks
      run: |
        chmod +x scripts/quality-checks.sh
        ./scripts/quality-checks.sh
```

**Checks performed by `scripts/quality-checks.sh`:** every gate is one file, `scripts/qc/<gate>.sh`, with a `QC_TRIGGER_qc_<gate>` path regex that decides whether `--staged-only` runs it; a full sweep (CI) runs them all in parallel. `scripts/qc/copyright.sh` (copyright headers, test and generated files excluded) runs first, then the gates (lint, XML, translations, doc anchors, ESP32 checks, and so on). `ls scripts/qc/` is the list; `scripts/qc_timing.py` shows where a run's time went.

## CI Watch

**Files:** `.github/workflows/ci-watch.yml`, `scripts/ci/ci_watch.py` (tests: `tests/python/test_ci_watch.py`)

A red `main` gets handled without anyone noticing it first. When Build, esp32-build, Code Quality, XML Lint or Nightly Full Test Suite finishes on a `main` push (or Nightly's schedule), ci-watch judges the run:

| Run | What ci-watch does |
|-----|--------------------|
| Failed, every failing job an infrastructure signature, attempt 1 | Reruns the failed jobs once (`rerun-failed-jobs`). No issue |
| Failed otherwise: a real failure, or infrastructure again on attempt 2+ | Opens the one open `ci-red` issue, or comments on it. The report names the workflow, the commit, the run, the failing jobs, each failure it can parse (Catch2 `FAILED:` with the case name and file:line, bats `not ok`, pytest `FAILED`, a sanitizer report's kind and first frame in `tests/`, `src/` or `include/`, a compiler error's location and message), and per job an excerpt of the lines that explain it, about 40 lines in all |
| Succeeded | Closes the open `ci-red` issue, naming the green SHA, once the latest completed `main` run of every watched workflow is green. Nightly counts only when the issue reports a Nightly failure, because its latest run judges a commit hours old |

A job is infrastructure when its log or its annotations (a lost runner often leaves no log) match a signature and no failure was parsed in it; a run is infrastructure when every failing job is. The aggregator jobs (`Build Status`, `Nightly Status`) only repeat the others and decide nothing. Cancelled jobs decide only when nothing else failed, since a job-level timeout reports as cancelled.

**Signatures** (`SIGNATURES` in `scripts/ci/ci_watch.py`; patterns match per line after the timestamp and colors are stripped):

| Name | Matches |
|------|---------|
| `apt-mirror` | apt `E:` lines: `Failed to fetch`, `Some index files failed to download`, `File has unexpected size`, `Hash Sum mismatch`, `Temporary failure resolving` |
| `ccache-missing` | `ccache: command not found`; `Failed to install ccache via package manager` |
| `retry-eperm` | nick-fields/retry's `code: 'EPERM'` together with `syscall: 'kill'` |
| `runner-lost` | `The runner has received a shutdown signal`, `lost communication with the server`, `The job was not acquired by Runner`, `The hosted runner encountered an error` |
| `timeout` | `The job has exceeded the maximum execution time`, `The action '<step>' has timed out after N minutes` |
| `timeout-cancel` | `The operation was canceled` together with a timed-out message |
| `disk-full` | `No space left on device` |
| `artifact-service` | `Failed to CreateArtifact: Unable to make request` |

A timeout counts as infrastructure because the cost of being wrong is one rerun: a test that really hangs times out again on attempt 2 and is reported then.

**The excerpt** keeps the lines that explain each failure and marks skipped stretches with `...`: Catch2 `FAILED:` with its expression, bats `not ok` with its `#` lines, the first three lines of each pytest `E` block and its `FAILED` summary line, a sanitizer's `==N==ERROR` line with its first three frames in this repo's code and its `SUMMARY`, a compiler `error:` with its source and caret lines. A job with none of those shows the lines before the step's final error. The ASAN and TSAN steps print only the report headers; the stacks are in the run's `asan-report` and `tsan-report` artifacts. Parsing lives in `failures()` and `_key_line_spans()` in `scripts/ci/ci_watch.py`.

**Adding a signature:** add a `Signature` to `SIGNATURES` with `any_of` (one pattern on one line suffices) or `all_of` (every pattern somewhere in the job), cut a few lines of the real log into `tests/python/fixtures/ci_watch/`, and add a case to `tests/python/test_ci_watch.py`. Anchor on the error form of a message: apt prints the same text as a `W:` warning when it carries on. `scripts/ci/ci_watch.py classify LOG...` prints the verdict for downloaded job logs (`gh api repos/OWNER/REPO/actions/jobs/JOB_ID/logs`).

**The `ci-red` label** marks the one issue; ci-watch creates the label when it is missing. Never more than one is open: the workflow runs one at a time (`concurrency: ci-watch`), and each report carries a hidden `<!-- ci-watch run=... attempt=... -->` marker, so a run already reported, even in an issue since closed by hand, is not reported again.

**The hourly sweep** judges the latest completed `main` run of each watched workflow the same way. A rerun started with `GITHUB_TOKEN` may not raise its own `workflow_run` event, and the concurrency group keeps only the newest queued event, so the sweep catches what an event missed.

**Testing it:** Actions > CI Watch > Run workflow, with `run_id` set to a known failed run and `dry_run` left on, prints the verdict and the issue it would write without rerunning or writing anything; `run_id` empty sweeps. Locally: `GH_TOKEN=... scripts/ci/ci_watch.py run --repo prestonbrown/helixscreen --run-id ID --dry-run --any-branch`.

## Platform-Specific Dependencies

### Font Generation

MDI icon fonts are generated by `scripts/regen_mdi_fonts.sh` (platform-independent). The script adds `node_modules/.bin` to PATH to access `lv_font_conv`.

**Font regeneration in CI:**
```yaml
- name: Generate MDI icon fonts
  run: ./scripts/regen_mdi_fonts.sh
```

MDI icon fonts and Noto Sans text fonts are committed to the repository and only regenerated when `scripts/regen_mdi_fonts.sh` changes.

### Platform Differences Reference

| Aspect | macOS | Linux |
|--------|-------|-------|
| System fonts | `/System/Library/Fonts/` | `/usr/share/fonts/` (varies) |
| CPU cores | `sysctl -n hw.ncpu` | `nproc` |
| Package manager | `brew` | `apt` / `dnf` / `pacman` |
| Library paths | `/usr/local/lib` | `/usr/lib` |
| Compiler | Apple Clang | GNU GCC / LLVM Clang |

## Dependency Build Order

HelixScreen depends on several libraries that must be built in order:

### Build Order

1. **libhv** - HTTP/WebSocket client library
   ```bash
   cd libhv
   ./configure --with-http-client
   make -j$(nproc)
   ```

2. **wpa_supplicant** (Linux only) - WiFi management library
   ```bash
   make -C wpa_supplicant/wpa_supplicant -j$(nproc) libwpa_client.a
   ```

3. **HelixScreen** - Main application
   ```bash
   make -j$(nproc)
   ```

### CI Workflow Pattern

```yaml
- name: Build libhv
  run: |
    cd libhv
    ./configure --with-http-client
    make -j$(nproc) libhv
    echo "✓ libhv built successfully"

- name: Build wpa_supplicant client library
  run: |
    make -C wpa_supplicant/wpa_supplicant -j$(nproc) libwpa_client.a
    echo "✓ wpa_supplicant client library built successfully"

- name: Build HelixScreen
  run: |
    make -j$(nproc)
    echo "✓ HelixScreen built successfully"
```

## Quality Checks

The quality workflow uses a shared script (`scripts/quality-checks.sh`) that runs the same checks locally and in CI.

### Copyright Header Checks

Every `.cpp`, `.c`, `.h` and `.mm` file outside `lib/`, the generated and vendored font and `lv_conf.h` files, and vendored `firmware/` components must carry an SPDX identifier. The gate is `scripts/qc/copyright.sh`; the header format is in [COPYRIGHT_HEADERS.md](COPYRIGHT_HEADERS.md).

### XML and work-marker checks

XML files are validated by the `lint-xml` workflow and the XML gates under `scripts/qc/` (`xml_attr.sh`, `decl_ui.sh`, `ui_lint.sh`). A `TODO`/`FIXME` comment must cite an owning issue as `// MARKER(#NNNN): <the constraint>` (`scripts/qc/todo_markers.sh`, `scripts/check_todo_markers.py --list`).

## Testing Locally

Test CI-like builds locally before pushing to catch issues early.

### Simulate Ubuntu Build

```bash
# Clean state
make clean

# Build dependencies
cd libhv
./configure --with-http-client
make -j$(nproc)
cd ..

make -C wpa_supplicant/wpa_supplicant -j$(nproc) libwpa_client.a

# Build project
npm install
./scripts/regen_mdi_fonts.sh
make -j$(nproc)

# Verify binary
./build/bin/helix-screen --help
```

### Simulate macOS Build

```bash
# Clean state
make clean

# Build dependencies
cd libhv
./configure --with-http-client
make -j$(sysctl -n hw.ncpu)
cd ..

# Build project
npm install
./scripts/regen_mdi_fonts.sh  # Generate MDI icon fonts
make -j$(sysctl -n hw.ncpu)

# Verify binary
./build/bin/helix-screen --help
```

### Run Quality Checks Locally

```bash
# Use the same script as CI
./scripts/quality-checks.sh
```

### Docker-Based CI Testing

To exactly match the CI environment:

```bash
# Ubuntu 22.04 (matches build workflow)
docker run -it --rm -v "$PWD:/work" -w /work ubuntu:22.04 bash

# Inside container
apt update
apt install -y \
  libsdl2-dev clang make python3 npm git \
  libssl-dev pkg-config libfmt-dev \
  libcairo2-dev libpango1.0-dev \
  libpng-dev libjpeg-dev

# Run build steps
npm install
./scripts/regen_mdi_fonts.sh
make -j$(nproc)
```

## R2 Release Pipeline

The release workflow (`.github/workflows/release.yml`) uploads build artifacts to Cloudflare R2 for CDN-based update distribution. This enables faster downloads and reduces dependency on GitHub's API.

### R2 Bucket Layout

```
s3://helixscreen-releases/
  stable/manifest.json        ← Stable releases (no prerelease suffix)
  stable/helixscreen-*.zip
  stable/helixscreen-*.tar.gz  ← Legacy (bridge release only)
  beta/manifest.json           ← Beta/RC releases (v1.0.0-beta, v1.0.0-rc.1)
  beta/helixscreen-*.zip
  beta/helixscreen-*.tar.gz    ← Legacy (bridge release only)
  dev/manifest.json            ← All releases (bleeding edge)
  dev/helixscreen-*.zip
  dev/helixscreen-*.tar.gz     ← Legacy (bridge release only)
```

> **Bridge release:** The manifest now advertises `zip_url`/`zip_sha256` as the **preferred** asset by default, so the `.zip` artifacts are manifest-referenced. The upload step puts both the `.tar.gz` and `.zip` artifacts up **before** the manifest goes live (clients following `zip_url` — or the legacy `url` — never 404 mid-download); `install.sh` and symbol maps upload *after* the manifest. The `.tar.gz` still ships alongside `.zip` during this bridge release — its `url`/`sha256` remain the fallback for pre-v0.99.31 clients, which never read `zip_url` — and will be removed in a following release. Pass `generate-manifest.sh --no-include-zip` to suppress the zip fields.

### Channel Upload Routing

| Tag Format | Channels | Example |
|------------|----------|---------|
| `v1.0.0` (no `-`) | `stable` + `dev` | Stable release |
| `v1.0.0-beta.1` (has `-`) | `beta` + `dev` | Prerelease |

Dev always receives every release, making it the bleeding-edge channel.

### Required GitHub Configuration

| Type | Name | Description |
|------|------|-------------|
| Variable | `R2_PUBLIC_URL` | Public CDN URL (e.g., `https://releases.helixscreen.org`) |
| Variable | `R2_BUCKET_NAME` | R2 bucket name (default: `helixscreen-releases`) |
| Secret | `R2_ACCOUNT_ID` | Cloudflare account ID |
| Secret | `R2_ACCESS_KEY_ID` | R2 API access key |
| Secret | `R2_SECRET_ACCESS_KEY` | R2 API secret key |

### Non-Fatal Upload

R2 upload uses `continue-on-error: true`, so GitHub releases are always created even if R2 upload fails. The HelixScreen client falls back to GitHub API automatically when R2 is unavailable.

### Manual Recovery

If a release's R2 upload fails, republish it with `recover-r2.yml`, dispatched from the tag itself
and approved when it waits on the `release-signing` environment:

```bash
gh workflow run recover-r2.yml --ref v1.2.0 -f tag=v1.2.0
```

It pulls the tag's artifacts from the GitHub release, regenerates and **signs** each channel's
manifest with the release key, and uploads `manifest.json.minisig` before `manifest.json`. Never
upload a hand-generated manifest: clients refuse one whose signature does not verify
(`docs/devel/RELEASE_SIGNING.md`). The workflow that runs is the one at the tag, so a tag cut
before recover-r2 learned to sign cannot be recovered this way on a signed channel.

## Troubleshooting

### Build Fails: "file not found"

**Symptom:**
```
fatal error: 'hv/json.hpp' file not found
```

**Solution:** Ensure libhv is built before HelixScreen:
```bash
cd libhv
./configure --with-http-client
make -j$(nproc)
cd ..
make -j$(nproc)
```

### Submodule Issues

**Symptom:**
```
fatal: not a git repository: lvgl/.git
```

**Solution:** Initialize submodules recursively:
```bash
git submodule update --init --recursive
```

### Workflow Not Triggering

**Symptom:** Push to branch doesn't trigger workflow.

**Check:** Verify your branch name matches the trigger patterns:
- `main` - Always triggers
- `claude/**` - Any branch starting with `claude/` triggers
- Pull requests to `main` - Always trigger

**Debug:** Check the "Actions" tab in GitHub - workflows may be disabled or skipped.

### Artifacts Not Found

**Symptom:** Artifact upload succeeds but download gives "artifact not found".

**Check workflow file:**
```yaml
- name: Upload binary
  uses: actions/upload-artifact@v4
  with:
    name: helix-screen-ubuntu-22.04
    path: build/bin/helix-screen  # Verify this path is correct
    retention-days: 7
```

**Verify:** Check that the binary actually exists at `build/bin/helix-screen` after the build step.

## Workflow Best Practices

✅ **Parallel platform builds** - Ubuntu and macOS build simultaneously
✅ **Platform-conditional scripts** - Different font generation per platform
✅ **Shared quality script** - Same checks locally and in CI
✅ **Explicit dependency order** - Build libhv → wpa_supplicant → HelixScreen
✅ **Clear error messages** - Include helpful context in build output
✅ **Test locally first** - Simulate CI builds before pushing
✅ **Artifact retention** - 7 days for development builds

## Artifact Retention

Current retention policy:

- **7 days** - Development builds (current default)
- **30 days** - Consider for release candidates
- **90 days** - Consider for tagged releases

To modify retention:
```yaml
- name: Upload binary
  uses: actions/upload-artifact@v4
  with:
    retention-days: 30  # Adjust as needed
```

## See Also

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [DEVELOPMENT.md](DEVELOPMENT.md) - Build system and daily workflow
- [BUILD_SYSTEM.md](BUILD_SYSTEM.md) - Makefile details and submodule patches
- [COPYRIGHT_HEADERS.md](COPYRIGHT_HEADERS.md) - GPL v3 header templates
