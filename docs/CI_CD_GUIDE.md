# CI/CD Guide for HelixScreen

This guide documents the GitHub Actions CI/CD setup for the HelixScreen project, including patterns for platform-specific builds, quality checks, and submodule management.

## Table of Contents

- [Overview](#overview)
- [Platform-Specific Dependencies](#platform-specific-dependencies)
- [Dependency Build Order](#dependency-build-order)
- [Quality Checks](#quality-checks)
- [Workflow Structure](#workflow-structure)
- [Testing Locally](#testing-locally)
- [Troubleshooting](#troubleshooting)

## Overview

The HelixScreen project uses GitHub Actions for continuous integration across multiple platforms:

- **Build Workflow** (`.github/workflows/build.yml`) - Multi-platform builds (Ubuntu 22.04, macOS 14)
- **Quality Workflow** (`.github/workflows/quality.yml`) - Code quality, structure, and dependency validation

**Key Features:**
- Parallel builds on Ubuntu and macOS
- Automatic submodule dependency building
- Platform-specific handling (macOS fonts, Linux wpa_supplicant)
- Fast quality checks before expensive builds

## Workflow Triggers

The workflows are triggered on:
- Push to `main` branch
- Push to any `claude/**` branches (AI assistant development branches)
- Pull requests targeting `main`

```yaml
on:
  push:
    branches: [ main, "claude/**" ]
  pull_request:
    branches: [ main ]
```

**Why `claude/**` branches?**
- AI assistants (Claude Code) create feature branches with this prefix
- Allows CI validation before creating pull requests
- Ensures all AI-generated code passes quality checks

## Platform-Specific Dependencies

### Problem: macOS System Fonts Don't Exist on Linux

Our initial CI failure:
```
lv_font_conv: error: Cannot read file "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
```

**Root cause:** Font generation scripts used macOS-specific paths:
```json
{
  "convert-arrows-64": "lv_font_conv --font '/System/Library/Fonts/Supplemental/Arial Unicode.ttf' ..."
}
```

### Solution 1: Commit Pre-Generated Platform-Specific Files

Arrow fonts are **already generated and committed** to the repository:
```bash
assets/fonts/arrows_32.c  # ✅ In git
assets/fonts/arrows_48.c  # ✅ In git
assets/fonts/arrows_64.c  # ✅ In git
```

Linux builds skip regeneration and use committed files.

### Solution 2: Platform-Conditional Generation

**Makefile pattern:**
```makefile
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PLATFORM := macOS
else
    PLATFORM := Linux
endif

.fonts.stamp: package.json
	@if [ "$(PLATFORM)" = "macOS" ]; then \
		npm run convert-all-fonts; \
	else \
		npm run convert-fonts-ci; \  # Skips platform-specific fonts
	fi
```

**package.json pattern:**
```json
{
  "scripts": {
    "convert-all-fonts": "npm run convert-font-64 && ... && npm run convert-arrows-64 && ...",
    "convert-fonts-ci": "npm run convert-font-64 && ... (no arrows)"
  }
}
```

### Lessons Learned

⚠️ **Never assume platform-specific paths exist** on CI runners
✅ **Commit generated files** for platform-specific dependencies
✅ **Create CI-safe variants** of scripts that skip optional platform-specific steps
✅ **Use platform detection** in Makefiles to choose appropriate commands

### Other Platform Differences to Watch

| Aspect | macOS | Linux |
|--------|-------|-------|
| System fonts | `/System/Library/Fonts/` | `/usr/share/fonts/` (varies) |
| CPU cores | `sysctl -n hw.ncpu` | `nproc` |
| Package manager | `brew` | `apt` / `dnf` / `pacman` |
| Library paths | `/usr/local/lib` | `/usr/lib` |
| Compiler | Apple Clang | GNU GCC / LLVM Clang |

## Dependency Build Order

### Problem: Missing Header Files

When dependencies aren't built before the main project:
```
include/config.h:7:10: fatal error: 'hv/json.hpp' file not found
```

**Root cause:** Main project tried to compile before libhv was built.

### Solution: Build Submodule Dependencies First

```yaml
- name: Checkout repository
  uses: actions/checkout@v4
  with:
    submodules: recursive  # Initialize all git submodules

- name: Build libhv
  working-directory: libhv
  run: |
    ./configure --with-openssl=no
    make -j$(nproc)

- name: Build wpa_supplicant (Linux only)
  working-directory: wpa_supplicant/wpa_supplicant
  run: |
    # Minimal config for client library
    cat > .config << 'EOF'
    CONFIG_CTRL_IFACE=y
    CONFIG_CTRL_IFACE_UNIX=y
    EOF
    make -j$(nproc) libwpa_client.a

- name: Build HelixScreen
  run: make -j$(nproc)
```

### Submodule Dependencies

HelixScreen uses the following git submodules:
```
helixscreen/
├── lvgl/              # LVGL 9.4 graphics library
├── libhv/             # HTTP/WebSocket client for Moonraker
├── spdlog/            # Logging library (header-only)
└── wpa_supplicant/    # WiFi control (Linux only)
```

**Build requirements:**
1. **libhv** - Must be built before main project (provides headers)
2. **wpa_supplicant** - Linux only, build `libwpa_client.a` before main project
3. **spdlog** - Header-only, no build needed
4. **lvgl** - Built as part of main project

### Lessons Learned

⚠️ **Submodules must be initialized** with `submodules: recursive`
✅ **Build dependencies explicitly** before main project
✅ **Use working-directory** to build submodules in their directories
✅ **Platform-specific deps** should be conditional (wpa_supplicant on Linux only)

## Quality Checks

### Problem: False Positives in Copyright Header Checks

Initial CI failure:
```
⚠️  Missing copyright header: src/test_dynamic_cards.cpp
⚠️  Missing copyright header: include/helix_icon_data.h
```

**Root cause:** Test files and auto-generated files don't need copyright headers.

### Solution: Exclude Auto-Generated and Test Files

```yaml
- name: Check C++ copyright headers
  run: |
    for file in src/*.cpp include/*.h; do
      basename=$(basename "$file")

      # Skip test files and generated files
      if [[ "$basename" == test_*.cpp ]] || [[ "$basename" == *_data.h ]]; then
        echo "⏭️  Skipping: $file"
        continue
      fi

      if ! head -n 5 "$file" | grep -q "Copyright"; then
        echo "⚠️  Missing copyright header: $file"
        MISSING=$((MISSING + 1))
      fi
    done
```

### Problem: ASCII Files Reported as Not UTF-8

Initial CI failure:
```
❌ ui_xml/advanced_panel.xml is not UTF-8 encoded
```

**Root cause:** The `file` command reports ASCII files as "ASCII text", not "UTF-8 text", even though ASCII is a valid subset of UTF-8.

### Solution: Accept Both UTF-8 and ASCII

```yaml
- name: Check XML files
  run: |
    for xml in ui_xml/*.xml; do
      if ! file "$xml" | grep -qE "UTF-8|ASCII"; then
        echo "❌ $xml is not UTF-8/ASCII encoded"
        exit 1
      fi
    done
```

### Quality Check Patterns

| Check Type | Pattern to Exclude | Example |
|------------|-------------------|---------|
| Copyright headers | `test_*.cpp` | Test files |
| Copyright headers | `*_data.h` | Auto-generated files |
| Copyright headers | `*.min.js` | Minified files |
| Linting | `vendor/*` | Third-party code |
| Encoding | Accept "ASCII" as valid | ASCII ⊂ UTF-8 |

### Lessons Learned

⚠️ **Not all source files need copyright headers** (tests, generated files)
✅ **Use pattern matching** to exclude special files (`test_*.cpp`, `*_data.h`)
✅ **ASCII is valid UTF-8** - accept both in encoding checks
✅ **Document exclusion patterns** in workflow for maintainability

## Workflow Structure

### Recommended Structure: Separate Build and Quality Workflows

**Why separate?**
- **Faster feedback** - Quality checks (1-2 min) complete before builds (5-10 min)
- **Clear failure reasons** - Know immediately if it's a quality issue or build failure
- **Parallelization** - Both run simultaneously

### Build Workflow Structure

```yaml
jobs:
  build-ubuntu:     # Ubuntu build job
  build-macos:      # macOS build job
  build-status:     # Summary job (runs after both)
```

**Key points:**
- Multi-platform matrix builds run in parallel
- Each platform uploads its binary as artifact
- Summary job aggregates results and fails if any build fails

### Quality Workflow Structure

```yaml
jobs:
  dependency-check:    # Verify submodules, Makefile, scripts
  project-structure:   # Verify required files exist
  copyright-headers:   # Validate GPL v3 headers
```

**Key points:**
- Independent checks run in parallel
- Fast validation (no compilation)
- Clear, actionable error messages

### Artifact Retention

```yaml
- name: Upload binary
  uses: actions/upload-artifact@v4
  with:
    retention-days: 7  # Good default for dev builds
```

**Recommendations:**
- **7 days** - Development branches
- **30 days** - Release candidates
- **90 days** - Tagged releases

## Testing Locally

Before pushing, test CI-like builds locally to catch issues early.

### Simulate Ubuntu CI Build

```bash
# Clean state
make clean
rm -rf libhv/lib
rm -rf wpa_supplicant/wpa_supplicant/libwpa_client.a

# Install Node.js dependencies
npm install

# Generate fonts (FontAwesome only, skip platform-specific arrows)
npm run convert-fonts-ci

# Build libhv
cd libhv
./configure --with-openssl=no
make -j$(nproc)
cd ..

# Build wpa_supplicant client library
cd wpa_supplicant/wpa_supplicant
cat > .config << 'EOF'
CONFIG_CTRL_IFACE=y
CONFIG_CTRL_IFACE_UNIX=y
EOF
make -j$(nproc) libwpa_client.a
cd ../..

# Build project
make -j$(nproc)

# Verify binary
./build/bin/helix-ui-proto --help || true
```

### Simulate macOS CI Build

```bash
# Clean state
make clean
rm -rf libhv/lib

# Install Node.js dependencies
npm install

# Generate all fonts (including arrows)
npm run convert-all-fonts

# Build libhv
cd libhv
./configure --with-openssl=no
make -j$(sysctl -n hw.ncpu)
cd ..

# Build project
make -j$(sysctl -n hw.ncpu)

# Verify binary
./build/bin/helix-ui-proto --help || true
```

### Run Quality Checks Locally

```bash
# Validate XML encoding
for xml in ui_xml/*.xml; do
  file "$xml" | grep -qE "UTF-8|ASCII" || echo "❌ $xml"
done

# Check copyright headers (excluding test files)
for file in src/*.cpp include/*.h; do
  basename=$(basename "$file")
  if [[ "$basename" != test_*.cpp ]] && [[ "$basename" != *_data.h ]]; then
    head -n 5 "$file" | grep -q "Copyright" || echo "⚠️  $file"
  fi
done

# Verify Makefile syntax
make -n help > /dev/null 2>&1 || echo "❌ Makefile has syntax errors"

# Check submodules are initialized
[ -f "lvgl/lvgl.h" ] || echo "❌ LVGL submodule not initialized"
[ -f "libhv/include/hv/HttpServer.h" ] || echo "❌ libhv submodule not initialized"
[ -f "spdlog/include/spdlog/spdlog.h" ] || echo "❌ spdlog submodule not initialized"
```

### Docker-Based CI Testing (Advanced)

To exactly match CI environment:

```bash
# Ubuntu 22.04 (matches ubuntu-22.04 runner)
docker run -it --rm -v "$PWD:/work" -w /work ubuntu:22.04 bash

# Inside container
apt update
apt install -y libsdl2-dev clang make python3 nodejs npm git libssl-dev pkg-config

# Clone with submodules (or just work with mounted volume if already cloned)
git submodule update --init --recursive

# ... follow Ubuntu build steps from above ...
```

## Troubleshooting

### Workflow Not Triggering

**Symptom:** Push to branch doesn't trigger workflow.

**Check:**
```yaml
on:
  push:
    branches: [ main, "claude/**" ]  # Is your branch listed or matching pattern?
  pull_request:
    branches: [ main ]
```

**Debug:**
1. Check "Actions" tab → "All workflows" (not just "Active workflows")
2. Verify branch name matches pattern (e.g., `claude/fix-build` matches `claude/**`)
3. Check workflow YAML syntax is valid

### Build Fails: "file not found"

**Symptom:**
```
fatal error: 'hv/json.hpp' file not found
```

**Solution:** Add libhv build step **before** main build:
```yaml
- name: Build libhv
  working-directory: libhv
  run: |
    ./configure --with-openssl=no
    make -j$(nproc)
```

### Font Generation Fails on Linux

**Symptom:**
```
Cannot read file "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
```

**Solution:** Use platform-conditional font generation (see [Platform-Specific Dependencies](#platform-specific-dependencies)).

### Artifacts Not Found

**Symptom:** Artifact upload succeeds but download gives "artifact not found".

**Solution:** Verify artifact path is correct relative to repository root:
```yaml
# ✅ Correct
- name: Upload binary
  uses: actions/upload-artifact@v4
  with:
    name: helix-ui-proto-ubuntu-22.04
    path: build/bin/helix-ui-proto  # Relative to repo root
```

### Copyright Check Fails on Test Files

**Symptom:**
```
⚠️  Missing copyright header: src/test_dynamic_cards.cpp
```

**Solution:** Exclude test files and generated files (see [Quality Checks](#quality-checks)). The workflow already skips:
- Test files: `test_*.cpp`
- Generated files: `*_data.h`

### Submodules Not Initialized

**Symptom:**
```
fatal error: 'lvgl/lvgl.h' file not found
```

**Solution:** Ensure checkout action initializes submodules:
```yaml
- name: Checkout repository
  uses: actions/checkout@v4
  with:
    submodules: recursive  # Required!
```

## Summary: CI/CD Best Practices

✅ **Submodule initialization** - Always use `submodules: recursive` when checking out
✅ **Platform detection** - Handle platform-specific dependencies gracefully (macOS fonts, Linux wpa_supplicant)
✅ **Build order** - Build dependencies (libhv, wpa_supplicant) before main project
✅ **Exclude special files** - Skip test files (`test_*.cpp`) and generated files (`*_data.h`) in quality checks
✅ **Clear failure messages** - Include helpful context in error output
✅ **Test locally** - Simulate CI builds before pushing (see Testing Locally section)
✅ **Separate workflows** - Build and quality checks run in parallel for faster feedback
✅ **Document exceptions** - Explain why files are excluded from checks
✅ **Artifact naming** - Use descriptive names with platform and version (e.g., `helix-ui-proto-ubuntu-22.04`)

## See Also

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [CLAUDE.md](../CLAUDE.md) - Project context for AI assistants
- [QUICK_REFERENCE.md](QUICK_REFERENCE.md) - Common development patterns
- [COPYRIGHT_HEADERS.md](COPYRIGHT_HEADERS.md) - GPL v3 header templates
