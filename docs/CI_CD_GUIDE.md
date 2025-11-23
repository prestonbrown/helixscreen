# CI/CD Guide for HelixScreen

This guide documents the GitHub Actions CI/CD setup for HelixScreen, including platform-specific builds and quality checks.

## Table of Contents

- [Overview](#overview)
- [Workflows](#workflows)
- [Platform-Specific Dependencies](#platform-specific-dependencies)
- [Dependency Build Order](#dependency-build-order)
- [Quality Checks](#quality-checks)
- [Testing Locally](#testing-locally)
- [Troubleshooting](#troubleshooting)

## Overview

HelixScreen uses GitHub Actions for continuous integration across multiple platforms:

- **Build Workflow** (`.github/workflows/build.yml`) - Multi-platform builds (Ubuntu 22.04, macOS 14)
- **Quality Workflow** (`.github/workflows/quality.yml`) - Code quality checks via shared script

Both workflows trigger on:
- Pushes to `main` branch
- Pushes to branches matching `claude/**` pattern
- Pull requests to `main` branch

## Workflows

### Build Workflow

**File:** `.github/workflows/build.yml`

The build workflow runs parallel builds on Ubuntu and macOS, then aggregates results:

```yaml
name: Build

on:
  push:
    branches: [ main, "claude/**" ]
  pull_request:
    branches: [ main ]

jobs:
  build-ubuntu:
    name: Build (Ubuntu 22.04)
    runs-on: ubuntu-22.04
    # ... build steps ...

  build-macos:
    name: Build (macOS 14)
    runs-on: macos-14
    # ... build steps ...

  build-status:
    name: Build Status
    runs-on: ubuntu-latest
    needs: [build-ubuntu, build-macos]
    # ... aggregates results ...
```

**Key Steps:**
1. Checkout with recursive submodules
2. Install platform-specific dependencies
3. Verify submodule initialization
4. Install Node.js dependencies (font converters)
5. Generate fonts (platform-conditional)
6. Build dependencies (libhv, wpa_supplicant, TinyGL)
7. Build HelixScreen
8. Upload binary artifacts (7-day retention)

**Artifacts:**
- `helix-ui-proto-ubuntu-22.04` - Ubuntu binary
- `helix-ui-proto-macos-14` - macOS binary

### Quality Workflow

**File:** `.github/workflows/quality.yml`

Simple workflow that runs the shared quality checks script:

```yaml
name: Code Quality

on:
  push:
    branches: [ main, "claude/**" ]
  pull_request:
    branches: [ main ]

jobs:
  quality-checks:
    name: Code Quality Checks
    runs-on: ubuntu-latest
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
    - name: Run quality checks
      run: |
        chmod +x scripts/quality-checks.sh
        ./scripts/quality-checks.sh
```

**Checks performed by `scripts/quality-checks.sh`:**
- Verify required directories exist
- Check copyright headers on source files (excludes test files and generated files)
- Validate XML file encoding (UTF-8/ASCII)
- Check for TODO/FIXME comments without context

## Platform-Specific Dependencies

### Problem: macOS System Fonts Don't Exist on Linux

Our font generation uses macOS-specific fonts for arrow icons:
```
/System/Library/Fonts/Supplemental/Arial Unicode.ttf
```

### Solution: Commit Pre-Generated Platform-Specific Files

Arrow fonts are **pre-generated and committed** to the repository:
```bash
assets/fonts/arrows_32.c  # ✅ In git
assets/fonts/arrows_48.c  # ✅ In git
assets/fonts/arrows_64.c  # ✅ In git
```

### Platform-Conditional Font Generation

**Ubuntu build:**
```yaml
- name: Generate fonts (FontAwesome only, skip platform-specific arrows)
  run: npm run convert-fonts-ci
```

**macOS build:**
```yaml
- name: Generate fonts (all fonts including arrows)
  run: npm run convert-all-fonts
```

**package.json pattern:**
```json
{
  "scripts": {
    "convert-all-fonts": "npm run convert-font-64 && ... && npm run convert-arrows-64",
    "convert-fonts-ci": "npm run convert-font-64 && ... (no arrows)"
  }
}
```

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

3. **TinyGL** - Software 3D rasterizer
   ```bash
   cd tinygl
   make -j$(nproc)
   ```

4. **HelixScreen** - Main application
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

- name: Build TinyGL
  run: |
    cd tinygl
    make -j$(nproc)
    echo "✓ TinyGL built successfully"

- name: Build HelixScreen
  run: |
    make -j$(nproc)
    echo "✓ HelixScreen built successfully"
```

## Quality Checks

The quality workflow uses a shared script (`scripts/quality-checks.sh`) that runs the same checks locally and in CI.

### Copyright Header Checks

**Pattern:** All source files must have GPL v3 copyright headers

**Exclusions:**
- Test files: `test_*.cpp`
- Generated files: `*_data.h`, `*_icon_data.h`
- Third-party code

```bash
for file in src/*.cpp include/*.h; do
  basename=$(basename "$file")
  if [[ "$basename" == test_*.cpp ]] || [[ "$basename" == *_data.h ]]; then
    continue  # Skip
  fi
  if ! head -n 5 "$file" | grep -q "Copyright"; then
    echo "⚠️  Missing copyright header: $file"
  fi
done
```

### XML Encoding Validation

**Pattern:** All XML files must be UTF-8 or ASCII encoded

```bash
for xml in ui_xml/*.xml; do
  if ! file "$xml" | grep -qE "UTF-8|ASCII"; then
    echo "❌ $xml is not UTF-8/ASCII encoded"
    exit 1
  fi
done
```

**Why ASCII is accepted:** ASCII is a valid subset of UTF-8, so ASCII files are UTF-8 compatible.

### TODO/FIXME Comments

**Pattern:** Check for uncommented TODOs that lack context

```bash
if grep -r "TODO\|FIXME" src/ include/ --exclude-dir=build | grep -v "//.*TODO"; then
  echo "⚠️  Found uncommented TODO/FIXME"
fi
```

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

cd tinygl
make -j$(nproc)
cd ..

# Build project
npm install
npm run convert-fonts-ci
make -j$(nproc)

# Verify binary
./build/bin/helix-ui-proto --help
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

cd tinygl
make -j$(sysctl -n hw.ncpu)
cd ..

# Build project
npm install
npm run convert-all-fonts
make -j$(sysctl -n hw.ncpu)

# Verify binary
./build/bin/helix-ui-proto --help
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
npm run convert-fonts-ci
# ... continue with build ...
```

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

### Font Generation Fails on Linux

**Symptom:**
```
Cannot read file "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
```

**Solution:** Use `npm run convert-fonts-ci` instead of `npm run convert-all-fonts` on Linux. The CI-safe version skips platform-specific arrow fonts (which are pre-generated and committed).

### Submodule Issues

**Symptom:**
```
fatal: not a git repository: lvgl/.git
```

**Solution:** Initialize submodules recursively:
```bash
git submodule update --init --recursive
```

### Quality Check Fails on Test Files

**Symptom:**
```
⚠️  Missing copyright header: src/test_dynamic_cards.cpp
```

**Solution:** The quality checks script should automatically exclude `test_*.cpp` files. If not, update `scripts/quality-checks.sh` to skip test files.

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
    name: helix-ui-proto-ubuntu-22.04
    path: build/bin/helix-ui-proto  # Verify this path is correct
    retention-days: 7
```

**Verify:** Check that the binary actually exists at `build/bin/helix-ui-proto` after the build step.

## Workflow Best Practices

✅ **Parallel platform builds** - Ubuntu and macOS build simultaneously
✅ **Platform-conditional scripts** - Different font generation per platform
✅ **Shared quality script** - Same checks locally and in CI
✅ **Explicit dependency order** - Build libhv → wpa_supplicant → TinyGL → HelixScreen
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
- [DEVELOPMENT.md](../DEVELOPMENT.md) - Build system and daily workflow
- [BUILD_SYSTEM.md](BUILD_SYSTEM.md) - Makefile details and submodule patches
- [COPYRIGHT_HEADERS.md](COPYRIGHT_HEADERS.md) - GPL v3 header templates
