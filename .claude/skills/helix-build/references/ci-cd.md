# CI/CD Guide

## Workflows

### Build Workflow (`.github/workflows/build.yml`)
Multi-platform parallel builds on Ubuntu 22.04 and macOS 14.

**Triggers:** push to `main` or `claude/**`, PRs to `main`

**Steps:**
1. Checkout with recursive submodules
2. Install platform-specific dependencies
3. Install Node.js deps (`npm install`)
4. Generate fonts (platform-conditional)
5. Build libhv (`./configure --with-http-client && make`)
6. Build wpa_supplicant (Linux only)
7. Build HelixScreen (`make -j$(nproc)`)
8. Upload binary artifacts (7-day retention)

**Artifacts:** `helix-screen-ubuntu-22.04`, `helix-screen-macos-14`

### Quality Workflow (`.github/workflows/quality.yml`)
Runs `scripts/quality-checks.sh` — same checks locally and in CI.

**Checks:**
- Required directories exist
- Copyright headers (GPL v3 SPDX) on source files
- XML file encoding (UTF-8/ASCII)
- TODO/FIXME comments without context
- Code formatting (clang-format)
- Merge conflict markers
- Trailing whitespace

**Exclusions from copyright check:**
- `test_*.cpp` files
- `*_data.h`, `*_icon_data.h` (generated)
- Third-party code

## Dependency Build Order (CI)

```bash
# 1. libhv
cd libhv && ./configure --with-http-client && make -j$(nproc)

# 2. wpa_supplicant (Linux only)
make -C wpa_supplicant/wpa_supplicant -j$(nproc) libwpa_client.a

# 3. HelixScreen
npm install && npm run convert-fonts-ci  # or ./scripts/regen_mdi_fonts.sh
make -j$(nproc)
```

## R2 Release Pipeline

### Bucket Layout
```
s3://helixscreen-releases/
  stable/   ← Stable releases (no prerelease suffix)
  beta/     ← Beta/RC releases (v1.0.0-beta, v1.0.0-rc.1)
  dev/      ← All releases (bleeding edge)
```

### Channel Routing

| Tag Format | Channels |
|------------|----------|
| `v1.0.0` (no `-`) | stable + dev |
| `v1.0.0-beta.1` (has `-`) | beta + dev |

### GitHub Configuration Required

| Type | Name | Description |
|------|------|-------------|
| Variable | `R2_PUBLIC_URL` | CDN URL (e.g., `https://releases.helixscreen.org`) |
| Variable | `R2_BUCKET_NAME` | R2 bucket name |
| Secret | `R2_ACCOUNT_ID` | Cloudflare account ID |
| Secret | `R2_ACCESS_KEY_ID` | R2 API access key |
| Secret | `R2_SECRET_ACCESS_KEY` | R2 API secret key |

R2 upload uses `continue-on-error: true` — GitHub releases are always created even if R2 fails.

### Manual Recovery
```bash
gh release download v0.9.5 -D release-files/
scripts/generate-manifest.sh --version "0.9.5" --tag "v0.9.5" \
  --dir release-files --base-url "https://releases.helixscreen.org/stable" \
  --output release-files/manifest-stable.json
aws s3 cp release-files/ s3://helixscreen-releases/stable/ --recursive
```

## Testing Locally

### Simulate Ubuntu Build
```bash
make clean
cd libhv && ./configure --with-http-client && make -j$(nproc) && cd ..
make -C wpa_supplicant/wpa_supplicant -j$(nproc) libwpa_client.a
npm install && npm run convert-fonts-ci
make -j$(nproc)
```

### Docker-Based CI Testing
```bash
docker run -it --rm -v "$PWD:/work" -w /work ubuntu:22.04 bash
# Inside: apt install deps, npm install, make -j$(nproc)
```

### Run Quality Checks
```bash
./scripts/quality-checks.sh    # Same as CI
```

## Artifact Retention

| Type | Retention |
|------|-----------|
| Development builds | 7 days |
| Release candidates | Consider 30 days |
| Tagged releases | Consider 90 days |
