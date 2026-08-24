# ESP32 Port — Plan 2: Main-Tree Seams Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the four audit-proven abstractions in the main tree — asset root, build-time theme-token table, config storage backend, WiFi platform-backend hook — each desktop-neutral or desktop-improving, so Plans 3–4 can consume them from the firmware tree without forking app code.

**Architecture:** Seams-first hybrid (approved spec `docs/devel/plans/2026-07-13-esp32-native-port-design.md`). This plan is **main-tree only** — zero changes under `firmware/`. Research findings that shaped it (2026-07-13 exploration):
- The implicit asset root is the process CWD (`Application::ensure_project_root_cwd()` chdir); the ESP32 audit's `overrides/theme_manager.cpp` proves theme_manager's `"ui_xml"` literals are the boot-critical chokepoint. All `"A:..."` LVGL paths re-home for free once the strings route through one accessor (LVGL POSIX driver has an empty base prefix).
- Theme token discovery runs **~28 full-directory scans** at boot (per element-type × per breakpoint-suffix), all funneling through exactly two aggregation functions in `theme_manager.cpp` — that's the table seam, NOT the audit's per-file byte cache (which still re-parses ~25×).
- The settings seam belongs under `helix::Config` (the ONLY class that touches disk — every `*SettingsManager` delegates), at document level. The audit already compiled `config.cpp` on Xtensa; the legacy `fs::` migration blocks no-op harmlessly when their desktop paths don't exist, so only document load/store/probe needs abstracting.
- The WiFi seam **already exists** (`WifiBackend` pure-virtual + `WifiBackend::create()` factory with wpa_supplicant / NetworkManager / macOS / mock backends). Only guard hygiene and a platform hook are needed.

**Tech Stack:** C++17 main tree, Catch2 tests (`make test-run`), Python 3 generator script, GNU Make.

## Program sequence (this is Plan 2 of 5)

| Plan | Deliverable | Status |
|---|---|---|
| 1. Foundation | Touch-responsive LVGL on K-Touch, OTA A/B partitions, CI + size gate | ✅ done (merged `d92f74073`) |
| **2. Main-tree seams** (this plan) | Asset root, token table, config storage backend, WiFi hook | — |
| 3. Network | `helixnet`: esp_websocket/http impls of `IMoonrakerClient`/`IMoonrakerAPI` | not written |
| 4. App integration | Full shell + Core+AMS panels + provisioning + NVS settings + mock mode on device | not written |
| 5. Product hardening | OTA end-to-end, crash reporting, CJK fonts, image diet, HIL suite | not written |

## Global Constraints

- **Zero behavior change on default desktop builds.** Every seam defaults to the current behavior; new paths activate only via explicit setter/env/define.
- **The XML-no-rebuild workflow is sacred** (edit `ui_xml/*.xml` → relaunch, no `make`). The token table is therefore OFF in dev builds by default and gated ON only where ui_xml is immutable (release/cross/firmware).
- Image/boot budgets are firmware-side (Plan 5); this plan's measurable win is desktop/release boot time from the token table.
- No new third-party dependencies. No changes under `firmware/` (the `esp32-build` CI job must stay green — it compiles only LVGL/helix-xml + firmware `main/`, unaffected by `src/` changes).
- Commit prefixes: `feat(paths)`, `feat(theme)`, `feat(config)`, `feat(wifi)` — this is main-tree work, not `feat(esp32-fw)`.
- Run `make test-run` (or targeted tags) before every commit; check for running builds first (`pgrep -fc cc1plus`).
- Deferred consumers (route in Plan 4 when the device build exercises them, one-liners each): `filament_catalog.cpp` builtin paths, splash/prerendered images, sound `find_readable`, `ams_state.cpp` / `ui_screensaver.cpp` `"A:assets/..."` literals.

## File Structure (end state)

```
include/
├── data_root_resolver.h        # + asset_root()/set_asset_root()/asset_path()   (Task 1)
├── theme_token_table.h         # NEW: TokenEntry, table externs, enabled()/for_element()/for_suffix() (Task 3/4)
├── config_storage.h            # NEW: ConfigStorage interface + file factory    (Task 5)
├── config.h                    # + set_storage(), storage_ member              (Task 5)
└── wifi_backend.h              # + create_platform_wifi_backend() declaration  (Task 6)
src/
├── application/data_root_resolver.cpp   # asset root impl                      (Task 1)
├── layout_manager.cpp                   # asset_path routing                   (Task 2)
├── system/translation_loader.cpp        # asset_path routing                   (Task 2)
├── system/config.cpp                    # storage_ routing                     (Task 5)
├── system/config_storage_file.cpp       # NEW: FileConfigStorage               (Task 5)
├── ui/theme_manager.cpp                 # tm_ui_xml_dir() + table seam         (Task 2/4)
├── ui/theme_token_table_runtime.cpp     # NEW: enabled()/for_element()/for_suffix() (Task 4)
├── generated/theme_token_table.cpp      # GENERATED, committed                 (Task 3)
└── api/{wifi_backend.cpp, wifi_manager.cpp}  # ESP_PLATFORM guards             (Task 6)
scripts/gen_theme_tokens.py              # NEW generator                        (Task 3)
mk/tools.mk                              # + regen-tokens target                (Task 3)
tests/unit/{test_asset_root.cpp, test_theme_token_table.cpp, test_config_storage.cpp}
tests/test_helpers/mock_config_storage.h
```

---

### Task 1: `helix::asset_root()` accessor

**Files:**
- Modify: `include/data_root_resolver.h` (after `get_data_dir()`, line 59)
- Modify: `src/application/data_root_resolver.cpp`
- Test: `tests/unit/test_asset_root.cpp` (new)

**Interfaces:**
- Produces: `const std::string& helix::asset_root();`, `void helix::set_asset_root(const std::string&);`, `std::string helix::asset_path(const std::string& relpath);`. Task 2 routes consumers through `asset_path`; the ESP32 firmware (Plan 4) calls `set_asset_root("/littlefs")` before UI init.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_asset_root.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_root_resolver.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("asset_path is identity under the default root", "[paths][asset_root]") {
    helix::set_asset_root("");  // reset to default
    REQUIRE(helix::asset_root() == ".");
    REQUIRE(helix::asset_path("ui_xml") == "ui_xml");
    REQUIRE(helix::asset_path("assets/filaments.json") == "assets/filaments.json");
}

TEST_CASE("asset_path joins under an explicit root", "[paths][asset_root]") {
    helix::set_asset_root("/littlefs/");  // trailing slash must be stripped
    REQUIRE(helix::asset_root() == "/littlefs");
    REQUIRE(helix::asset_path("ui_xml") == "/littlefs/ui_xml");
    helix::set_asset_root("");  // restore for other tests
    REQUIRE(helix::asset_path("ui_xml") == "ui_xml");
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
make test -j && ./build/bin/helix-tests "[asset_root]"
```
Expected: compile FAILURE (`set_asset_root` not declared). If the test file isn't picked up, check the glob in `mk/tests.mk` (unit tests are auto-discovered from `tests/unit/*.cpp`).

- [ ] **Step 3: Implement**

`include/data_root_resolver.h`, insert after the `get_data_dir()` declaration (line 59):
```cpp
/**
 * @brief Root directory containing ui_xml/ and assets/
 *
 * Defaults to "." — byte-compatible with the historical CWD assumption
 * (Application chdir()s into the data root at startup, so relative paths
 * already resolve). Embedded targets with no working directory (ESP-IDF
 * VFS has none — every relative path fails there) call
 * set_asset_root("/littlefs") before any UI/theme init.
 *
 * Same thread-safety caveats as get_user_config_dir(): set once during
 * single-threaded startup, read-only afterwards.
 */
const std::string& asset_root();

/** @brief Override the asset root (empty string resets to "."). */
void set_asset_root(const std::string& root);

/**
 * @brief Join a relative asset path onto asset_root()
 *
 * Identity when the root is "." (desktop: asset_path("ui_xml") == "ui_xml",
 * keeping every existing path byte-identical); prefix-joined otherwise
 * ("/littlefs/ui_xml").
 */
std::string asset_path(const std::string& relpath);
```

`src/application/data_root_resolver.cpp`, add inside `namespace helix` (after `get_data_dir()`):
```cpp
namespace {
std::string& asset_root_storage() {
    static std::string root = ".";
    return root;
}
} // namespace

const std::string& asset_root() {
    return asset_root_storage();
}

void set_asset_root(const std::string& root) {
    asset_root_storage() =
        root.empty() ? "." : helix::paths::strip_trailing_slash(root);
}

std::string asset_path(const std::string& relpath) {
    const std::string& root = asset_root_storage();
    if (root == ".") {
        return relpath;
    }
    return root + "/" + relpath;
}
```
(Note: the file already has an anonymous namespace at the top with `path_exists` — add `asset_root_storage` to that existing anonymous namespace instead of opening a second one.)

- [ ] **Step 4: Run the test to verify it passes**

```bash
make test -j && ./build/bin/helix-tests "[asset_root]"
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/data_root_resolver.h src/application/data_root_resolver.cpp tests/unit/test_asset_root.cpp
git commit -m "feat(paths): asset_root()/asset_path() seam for CWD-less targets"
```

---

### Task 2: Route boot-critical ui_xml consumers through `asset_path`

**Files:**
- Modify: `src/ui/theme_manager.cpp` (27 `"ui_xml"` literals + new helper)
- Modify: `src/layout_manager.cpp:55-74`
- Modify: `src/system/translation_loader.cpp:38`

**Interfaces:**
- Consumes: `helix::asset_path()` (Task 1).
- Produces: `tm_ui_xml_dir()` (file-static in theme_manager.cpp) — Task 4's table seam compares against it to detect the canonical directory.

- [ ] **Step 1: Add the cached helper to theme_manager.cpp**

Near the top of `src/ui/theme_manager.cpp` (after the existing includes; also add `#include "data_root_resolver.h"`):
```cpp
// Canonical ui_xml directory for token discovery, resolved once through the
// asset-root seam. On desktop this is byte-identical to the old "ui_xml"
// literal; on CWD-less targets (ESP-IDF VFS) it becomes an absolute path
// under the mount the firmware configured via helix::set_asset_root().
static const char* tm_ui_xml_dir() {
    static const std::string dir = helix::asset_path("ui_xml");
    return dir.c_str();
}
```

- [ ] **Step 2: Replace the 27 literals**

Replace every `"ui_xml"` argument to the two aggregation functions with `tm_ui_xml_dir()`. Exact lines (verified 2026-07-13): 785, 786, 827, 836, 843, 953, 964, 965, 966, 967, 968, 969, 970, 1074, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1195, 1196, 1197, 1198, 1199, 1200, 1201. Example (line 785):
```cpp
// before
auto light_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "color", "_light");
// after
auto light_tokens = theme_manager_parse_all_xml_for_suffix(tm_ui_xml_dir(), "color", "_light");
```
Then verify no stragglers:
```bash
grep -n '"ui_xml"' src/ui/theme_manager.cpp
```
Expected: no matches on `parse_all_xml` calls (comments/log strings are fine).

- [ ] **Step 3: Route layout_manager**

`src/layout_manager.cpp` — replace `resolve_xml_path` and `has_override` bodies (lines 55-74); add `#include "data_root_resolver.h"`:
```cpp
std::string LayoutManager::resolve_xml_path(const std::string& filename) const {
    if (is_standard()) {
        return helix::asset_path("ui_xml/" + filename);
    }

    std::string variant_path = helix::asset_path("ui_xml/" + name_ + "/" + filename);
    if (access(variant_path.c_str(), F_OK) == 0) {
        return variant_path;
    }

    return helix::asset_path("ui_xml/" + filename);
}

bool LayoutManager::has_override(const std::string& filename) const {
    if (is_standard()) {
        return false;
    }
    std::string variant_path = helix::asset_path("ui_xml/" + name_ + "/" + filename);
    return access(variant_path.c_str(), F_OK) == 0;
}
```
Note: `xml_registration.cpp:255` builds `"A:" + lm.resolve_xml_path(...)` — with an absolute asset root this yields `"A:/littlefs/ui_xml/..."`, which LVGL's POSIX driver (empty `LV_FS_POSIX_PATH`) opens as `/littlefs/ui_xml/...`. Desktop stays `"A:ui_xml/..."`. No change needed there.

- [ ] **Step 4: Route translation_loader**

`src/system/translation_loader.cpp:38`; add `#include "data_root_resolver.h"`:
```cpp
// before
std::string path = "A:ui_xml/translations/" + lang + ".xml";
// after
std::string path = "A:" + helix::asset_path("ui_xml/translations/" + lang + ".xml");
```

- [ ] **Step 5: Build, run the full suite, sanity-boot**

```bash
make -j 2>&1 | tail -5; make test-run 2>&1 | tail -15
```
Expected: build clean, all tests pass (theme tests pass temp dirs to the scan APIs, so they exercise the unchanged fallback path; nothing observes the literal change on desktop).

Sanity-boot the app (XML + theme + translations all load through the new routing):
```bash
./build/bin/helix-screen --test -v 2>&1 | grep -E "Registered.*static|error|ERROR" | head -10
```
Expected: the usual `[Theme] Registered N static colors...` line, no errors.

- [ ] **Step 6: Commit**

```bash
git add src/ui/theme_manager.cpp src/layout_manager.cpp src/system/translation_loader.cpp
git commit -m "feat(paths): route theme/layout/translation ui_xml access through asset_path"
```

---

### Task 3: Token-table generator + committed artifact

**Files:**
- Create: `scripts/gen_theme_tokens.py`
- Create: `include/theme_token_table.h`
- Create: `src/generated/theme_token_table.cpp` (generated, committed)
- Modify: `mk/tools.mk` (add `regen-tokens` next to `regen-xml-schema`, line ~242)

**Interfaces:**
- Produces: `helix::theme_tokens::TokenEntry`, `k_token_table[]`, `k_token_table_count` — consumed by Task 4's runtime. `make regen-tokens` regenerates; the Task 4 parity test is the staleness gate (same pattern as `regen-xml-schema`'s committed `schema.json`).

- [ ] **Step 1: Write the header**

`include/theme_token_table.h`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace helix::theme_tokens {

/// One <color>/<px>/<string> design token from ui_xml/*.xml, resolved
/// last-wins across alphabetically-sorted files (same semantics as the
/// runtime scanner in theme_manager.cpp).
struct TokenEntry {
    const char* type;   // "color" | "px" | "string"
    const char* name;   // full token name (suffix included)
    const char* value;
};

// Defined in src/generated/theme_token_table.cpp (make regen-tokens).
extern const TokenEntry k_token_table[];
extern const size_t k_token_table_count;

/// True when the aggregation functions should consult the table instead of
/// scanning ui_xml/. Default: ON for HELIX_RELEASE_BUILD (immutable ui_xml),
/// OFF for dev builds (preserves the edit-XML-and-relaunch workflow).
/// Runtime override either way: HELIX_TOKEN_TABLE=1 / =0.
bool enabled();

/// Table-backed equivalents of theme_manager_parse_all_xml_for_element /
/// _for_suffix. Always available (independent of enabled()) so the parity
/// test can compare them against the live scanner.
std::unordered_map<std::string, std::string> for_element(const char* element_type);
std::unordered_map<std::string, std::string> for_suffix(const char* element_type,
                                                        const char* suffix);

} // namespace helix::theme_tokens
```

- [ ] **Step 2: Write the generator**

`scripts/gen_theme_tokens.py`:
```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate src/generated/theme_token_table.cpp from ui_xml/*.xml.

Mirrors theme_manager.cpp's runtime scan exactly: top-level *.xml only
(no recursion), files sorted alphabetically, collecting the name=/value=
attributes of every <color>, <px> and <string> element at any depth,
last-wins on duplicate (type, name) across and within files.

Usage: gen_theme_tokens.py [--check]
  --check: regenerate to a string and exit 1 if the committed file differs.
"""
import os
import sys
import xml.etree.ElementTree as ET

UI_XML_DIR = "ui_xml"
OUT_PATH = "src/generated/theme_token_table.cpp"
TYPES = ("color", "px", "string")


def cstr(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def collect() -> dict:
    tokens = {}  # (type, name) -> value, insertion order preserved
    files = sorted(
        f for f in os.listdir(UI_XML_DIR)
        if f.endswith(".xml") and os.path.isfile(os.path.join(UI_XML_DIR, f))
    )
    for fname in files:
        path = os.path.join(UI_XML_DIR, fname)
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as e:
            # Runtime scanner keeps partial results on parse errors; a
            # lint-clean tree should never hit this. Fail loudly instead.
            print(f"error: {path}: {e}", file=sys.stderr)
            sys.exit(1)
        for t in TYPES:
            for el in root.iter(t):
                name = el.get("name")
                value = el.get("value")
                if name is not None and value is not None:
                    tokens[(t, name)] = value
    return tokens


def render(tokens: dict) -> str:
    lines = [
        "// GENERATED FILE — DO NOT EDIT. Regenerate with: make regen-tokens",
        "// Source of truth: ui_xml/*.xml (top level, sorted, last-wins).",
        "// Parity-gated against the runtime scanner by test_theme_token_table.cpp.",
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        '#include "theme_token_table.h"',
        "",
        "namespace helix::theme_tokens {",
        "",
        "const TokenEntry k_token_table[] = {",
    ]
    for (t, name), value in tokens.items():
        lines.append(f"    {{{cstr(t)}, {cstr(name)}, {cstr(value)}}},")
    lines += [
        "};",
        "",
        "const size_t k_token_table_count = sizeof(k_token_table) / sizeof(k_token_table[0]);",
        "",
        "} // namespace helix::theme_tokens",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    if not os.path.isdir(UI_XML_DIR):
        print("error: run from the repo root (ui_xml/ not found)", file=sys.stderr)
        return 1
    content = render(collect())
    if "--check" in sys.argv:
        try:
            with open(OUT_PATH) as f:
                if f.read() == content:
                    print("token table is up to date")
                    return 0
        except FileNotFoundError:
            pass
        print(f"STALE: {OUT_PATH} — run 'make regen-tokens'", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w") as f:
        f.write(content)
    print(f"wrote {OUT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Add the make target**

`mk/tools.mk`, next to `regen-xml-schema` (~line 242):
```make
.PHONY: regen-tokens
regen-tokens: ## Regenerate src/generated/theme_token_table.cpp from ui_xml/
	python3 scripts/gen_theme_tokens.py
```
(Match the local `##` help-comment style used by neighboring targets in that file; if targets there aren't self-documenting, drop the `##` part and add the target to the help block near `Makefile:902`.)

- [ ] **Step 4: Generate and eyeball**

```bash
chmod +x scripts/gen_theme_tokens.py && make regen-tokens
head -20 src/generated/theme_token_table.cpp
grep -c "TokenEntry\|{\"" src/generated/theme_token_table.cpp
python3 scripts/gen_theme_tokens.py --check; echo "check=$?"
```
Expected: file exists with several hundred entries (globals.xml alone defines hundreds of responsive tokens); `--check` prints "up to date", exit 0. Spot-check one known token:
```bash
grep '"space_md_medium"' src/generated/theme_token_table.cpp
```
Expected: one `{"px", "space_md_medium", "..."}` entry matching `ui_xml/globals.xml`.

- [ ] **Step 5: Verify the new .cpp compiles into the build**

```bash
make -j 2>&1 | tail -3
```
The Makefile's source discovery must pick up `src/generated/theme_token_table.cpp` — it will fail to *link* only when something references the symbols (Task 4), so for this task just confirm the object file is built:
```bash
find build -name "theme_token_table*.o" | head -2
```
If empty, check the Makefile's `SRCS` glob for `src/` recursion and add `src/generated` if it's excluded (the retired lv_i18n artifacts used to live there — the glob may have been narrowed when they were removed).

- [ ] **Step 6: Commit**

```bash
git add scripts/gen_theme_tokens.py include/theme_token_table.h src/generated/theme_token_table.cpp mk/tools.mk
git commit -m "feat(theme): build-time token table generator + committed artifact"
```

---

### Task 4: theme_manager consumes the table (parity-gated)

**Files:**
- Create: `src/ui/theme_token_table_runtime.cpp`
- Modify: `src/ui/theme_manager.cpp` (aggregation functions, lines 2753-2778; init timing log)
- Test: `tests/unit/test_theme_token_table.cpp` (new — this is also the CI staleness gate)

**Interfaces:**
- Consumes: `k_token_table` (Task 3), `tm_ui_xml_dir()` (Task 2).
- Produces: table-fast path inside `theme_manager_parse_all_xml_for_element/_for_suffix`. Firmware (Plan 4) turns it on by defining `HELIX_TOKEN_TABLE_DEFAULT_ON` (or setting the env var); release/cross builds get it automatically via `HELIX_RELEASE_BUILD`.

- [ ] **Step 1: Write the failing parity test**

`tests/unit/test_theme_token_table.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parity gate: the generated table must be indistinguishable from the live
// runtime scan over the real ui_xml/. A failure here means ui_xml tokens
// changed without regenerating — run: make regen-tokens
#include "theme_manager.h"
#include "theme_token_table.h"

#include <catch2/catch_test_macros.hpp>

static const char* kTypes[] = {"color", "px", "string"};
static const char* kSuffixes[] = {"_light", "_dark",  "_micro",  "_tiny",   "_small",
                                  "_medium", "_large", "_xlarge", "_xxlarge"};

TEST_CASE("token table matches runtime scan (full element maps)", "[theme][tokens]") {
    for (const char* type : kTypes) {
        INFO("type=" << type << " — if this fails, run: make regen-tokens");
        auto scanned = theme_manager_parse_all_xml_for_element("ui_xml", type);
        auto table = helix::theme_tokens::for_element(type);
        REQUIRE(table == scanned);
    }
}

TEST_CASE("token table matches runtime scan (suffix maps)", "[theme][tokens]") {
    for (const char* type : kTypes) {
        for (const char* suffix : kSuffixes) {
            INFO("type=" << type << " suffix=" << suffix
                         << " — if this fails, run: make regen-tokens");
            auto scanned = theme_manager_parse_all_xml_for_suffix("ui_xml", type, suffix);
            auto table = helix::theme_tokens::for_suffix(type, suffix);
            REQUIRE(table == scanned);
        }
    }
}
```
Note: tests run with CWD = repo root (that's how existing XML fixtures find `ui_xml/`), and they pass the literal `"ui_xml"` — which after this task's seam only hits the table when `enabled()` is true, so the test calls the scanner directly and compares against the table functions explicitly. `HELIX_TOKEN_TABLE` must NOT be set in the test environment (the scanner side must scan).

- [ ] **Step 2: Run to verify it fails**

```bash
make test -j && ./build/bin/helix-tests "[tokens]"
```
Expected: LINK FAILURE (`for_element` undefined) — the runtime .cpp doesn't exist yet.

- [ ] **Step 3: Implement the runtime**

`src/ui/theme_token_table_runtime.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme_token_table.h"

#include <cstdlib>
#include <cstring>

namespace helix::theme_tokens {

bool enabled() {
    static const bool on = [] {
        if (const char* env = std::getenv("HELIX_TOKEN_TABLE")) {
            return env[0] == '1';
        }
#if defined(HELIX_RELEASE_BUILD) || defined(HELIX_TOKEN_TABLE_DEFAULT_ON)
        return true;   // immutable ui_xml: table is safe and fast
#else
        return false;  // dev build: preserve edit-XML-and-relaunch
#endif
    }();
    return on;
}

std::unordered_map<std::string, std::string> for_element(const char* element_type) {
    std::unordered_map<std::string, std::string> out;
    for (size_t i = 0; i < k_token_table_count; ++i) {
        const TokenEntry& e = k_token_table[i];
        if (std::strcmp(e.type, element_type) == 0) {
            out[e.name] = e.value;
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> for_suffix(const char* element_type,
                                                        const char* suffix) {
    std::unordered_map<std::string, std::string> out;
    const size_t slen = std::strlen(suffix);
    for (size_t i = 0; i < k_token_table_count; ++i) {
        const TokenEntry& e = k_token_table[i];
        if (std::strcmp(e.type, element_type) != 0) {
            continue;
        }
        const size_t nlen = std::strlen(e.name);
        // Same predicate as the runtime scanner: name must be strictly
        // longer than the suffix and end with it; key is the stripped base.
        if (nlen > slen && std::strcmp(e.name + nlen - slen, suffix) == 0) {
            out[std::string(e.name, nlen - slen)] = e.value;
        }
    }
    return out;
}

} // namespace helix::theme_tokens
```

- [ ] **Step 4: Insert the seam in theme_manager.cpp**

At the top of both aggregation functions (lines 2753 and 2763; add `#include "theme_token_table.h"`):
```cpp
std::unordered_map<std::string, std::string>
theme_manager_parse_all_xml_for_element(const char* directory, const char* element_type) {
    // Build-time token table: skip the ~28-scan boot storm when the table is
    // enabled and the caller wants the canonical ui_xml dir (tests and
    // alternate dirs always scan live).
    if (helix::theme_tokens::enabled() && directory &&
        std::strcmp(directory, tm_ui_xml_dir()) == 0) {
        return helix::theme_tokens::for_element(element_type);
    }
    std::unordered_map<std::string, std::string> token_values;
    std::vector<std::string> files = theme_manager_find_xml_files(directory);
    for (const auto& filepath : files) {
        theme_manager_parse_xml_file_for_all(filepath.c_str(), element_type, token_values);
    }
    return token_values;
}
```
Apply the same guard to `theme_manager_parse_all_xml_for_suffix` returning `for_suffix(element_type, suffix)`.

Confirm these two functions are the only scan entry points on the boot path:
```bash
grep -rn "parse_all_xml_for" src/ include/ | grep -v tests | grep -v theme_manager
```
Expected: no other production callers (test files call them with temp dirs — fine).

- [ ] **Step 5: Add an init timing log**

In `theme_manager_init()` (`src/ui/theme_manager.cpp:1548`), wrap the body:
```cpp
// at the top of theme_manager_init():
auto tm_init_start = std::chrono::steady_clock::now();
```
and before its return:
```cpp
spdlog::debug("[Theme] theme_manager_init took {} ms (token table {})",
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - tm_init_start)
                  .count(),
              helix::theme_tokens::enabled() ? "on" : "off");
```
(add `#include <chrono>` if missing.)

- [ ] **Step 6: Run tests, then measure both modes**

```bash
make test-run 2>&1 | tail -10
./build/bin/helix-screen --test -vv 2>&1 | grep "theme_manager_init took" &
HELIX_TOKEN_TABLE=1 ./build/bin/helix-screen --test -vv 2>&1 | grep "theme_manager_init took"
```
(Run each briefly and Ctrl-C / let the harness kill it after the line appears; or grep a short log capture.) Expected: all tests pass; the `HELIX_TOKEN_TABLE=1` run reports a smaller number with "token table on". Record both numbers in the commit message.

- [ ] **Step 7: Commit**

```bash
git add src/ui/theme_token_table_runtime.cpp src/ui/theme_manager.cpp tests/unit/test_theme_token_table.cpp
git commit -m "feat(theme): token-table fast path for boot-time constant registration (X ms -> Y ms)"
```

---

### Task 5: ConfigStorage document seam under `helix::Config`

**Files:**
- Create: `include/config_storage.h`
- Create: `src/system/config_storage_file.cpp`
- Modify: `include/config.h` (member + setter)
- Modify: `src/system/config.cpp` (init parse block ~1062-1124, RO probe 1340-1359, save-back 1362-1366, `save()` 1515-1618)
- Test: `tests/unit/test_config_storage.cpp`, `tests/test_helpers/mock_config_storage.h` (new)

**Interfaces:**
- Produces: `helix::ConfigStorage` (pure virtual: `load/store/preserve_corrupt/read_only/describe`), `helix::make_file_config_storage(path)`, `Config::set_storage(std::unique_ptr<ConfigStorage>)` (call before `init()`). Plan 4's firmware constructs Config with a LittleFS/NVS-backed implementation.
- **Stays in Config (do NOT move):** the `json data` model, all `migrate_*` + `run_versioned_migrations`, defaults, multi-printer routing, preset logic, the `friend ConfigTestAccess` in-memory injection, and the file-level legacy migrations in `init()` (lines 962-1058: helixconfig→settings renames, symlink handling, backup restore — desktop-only paths that no-op harmlessly elsewhere; the audit already compiles this file on Xtensa).

- [ ] **Step 1: Write the interface**

`include/config_storage.h`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <optional>
#include <string>

namespace helix {

/**
 * Document-level persistence backend for Config. Desktop = atomic-rename
 * JSON file (fsync file + parent dir, rolling backup); embedded targets
 * substitute NVS or LittleFS. Config keeps the JSON model, migrations,
 * defaults and multi-printer routing — the backend only moves bytes durably.
 */
class ConfigStorage {
  public:
    virtual ~ConfigStorage() = default;

    /// Whole-document read. nullopt = document does not exist (first boot).
    virtual std::optional<std::string> load() = 0;

    /// Atomic, durable whole-document write. False on failure (caller logs).
    virtual bool store(const std::string& bytes) = 0;

    /// Set the current (corrupt) document aside so load() stops returning
    /// it, preserving it for diagnosis where the backend can (.corrupt file).
    virtual void preserve_corrupt() = 0;

    /// True when the backing store cannot accept writes (RO filesystem).
    virtual bool read_only() = 0;

    /// Human-readable location for logs ("config/settings.json", "nvs://…").
    virtual std::string describe() const = 0;
};

/// Atomic-rename file implementation; behavior extracted verbatim from the
/// pre-seam Config::save() / Config::init().
std::unique_ptr<ConfigStorage> make_file_config_storage(const std::string& path);

} // namespace helix
```

- [ ] **Step 2: Write the mock + failing tests**

`tests/test_helpers/mock_config_storage.h`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config_storage.h"

#include <optional>
#include <string>

namespace helix::test {

class MockConfigStorage : public helix::ConfigStorage {
  public:
    std::optional<std::string> doc;
    std::string corrupt_stash;
    bool ro = false;
    int store_calls = 0;

    explicit MockConfigStorage(std::optional<std::string> initial = std::nullopt)
        : doc(std::move(initial)) {}

    std::optional<std::string> load() override { return doc; }
    bool store(const std::string& bytes) override {
        if (ro) return false;
        doc = bytes;
        store_calls++;
        return true;
    }
    void preserve_corrupt() override {
        if (doc) corrupt_stash = *doc;
        doc.reset();
    }
    bool read_only() override { return ro; }
    std::string describe() const override { return "mock://config"; }
};

} // namespace helix::test
```

`tests/unit/test_config_storage.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "config_storage.h"
#include "../test_helpers/mock_config_storage.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST_CASE("file storage round-trips a document atomically", "[config][storage]") {
    fs::path dir = fs::temp_directory_path() / "helix-storage-test";
    fs::create_directories(dir);
    std::string path = (dir / "settings.json").string();
    auto storage = helix::make_file_config_storage(path);

    REQUIRE_FALSE(storage->load().has_value());  // missing = nullopt
    REQUIRE(storage->store("{\"config_version\": 19}\n"));
    auto doc = storage->load();
    REQUIRE(doc.has_value());
    REQUIRE(doc->find("config_version") != std::string::npos);
    REQUIRE_FALSE(fs::exists(path + ".tmp"));  // no temp litter after store

    storage->preserve_corrupt();
    REQUIRE_FALSE(storage->load().has_value());
    REQUIRE(fs::exists(path + ".corrupt"));

    fs::remove_all(dir);
}

TEST_CASE("Config routes load and save through an injected backend",
          "[config][storage]") {
    auto mock = std::make_unique<helix::test::MockConfigStorage>(
        std::string(R"({"config_version": 19, "wizard_completed": true})"));
    auto* mock_raw = mock.get();

    helix::Config cfg;
    cfg.set_storage(std::move(mock));
    cfg.init("config/settings-test.json");

    REQUIRE(cfg.get<bool>("/wizard_completed", false) == true);

    cfg.set<int>("/test_marker", 42);
    REQUIRE(cfg.save());
    REQUIRE(mock_raw->store_calls >= 1);
    REQUIRE(mock_raw->doc.has_value());
    REQUIRE(mock_raw->doc->find("test_marker") != std::string::npos);
}
```
Note: `init()` runs its legacy fs:: migration blocks against `config/settings-test.json`-relative paths before consulting the backend — in a test checkout those files don't exist, so the blocks no-op. If `HELIX_CONFIG_DIR` is set in the test env, init() redirects `path` but the injected backend still owns the actual document I/O, which is the property under test.

- [ ] **Step 3: Run to verify failure**

```bash
make test -j 2>&1 | tail -5
```
Expected: compile FAILURE (`set_storage` / `make_file_config_storage` undeclared).

- [ ] **Step 4: Implement FileConfigStorage**

`src/system/config_storage_file.cpp` — move the I/O verbatim from `config.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_storage.h"

#include "config_backup.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace helix {

namespace {

std::string errno_reason(int err) {
    switch (err) {
    case ENOSPC:
        return "disk full";
    case EROFS:
        return "read-only filesystem";
    case EACCES:
        return "permission denied";
    default:
        return strerror(err);
    }
}

class FileConfigStorage : public ConfigStorage {
  public:
    explicit FileConfigStorage(std::string path) : path_(std::move(path)) {}

    std::optional<std::string> load() override {
        struct stat st;
        if (stat(path_.c_str(), &st) != 0) {
            return std::nullopt;
        }
        std::ifstream in(path_);
        if (!in.is_open()) {
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool store(const std::string& bytes) override {
        // Atomic save: symlink-resolve, write .tmp, fsync, rename, fsync
        // parent dir. Moved verbatim from Config::save() (see #943: without
        // the fsyncs a power cycle can leave settings.json empty on
        // flash-backed filesystems).
        try {
            std::string target_path = path_;
            {
                std::error_code ec;
                if (fs::is_symlink(path_, ec)) {
                    auto real = fs::canonical(path_, ec);
                    if (!ec) {
                        spdlog::debug("[ConfigStorage] Resolved symlink {} -> {}", path_,
                                      real.string());
                        target_path = real.string();
                    }
                }
            }

            std::string tmp_path = target_path + ".tmp";
            {
                std::ofstream o(tmp_path);
                if (!o.is_open()) {
                    spdlog::error("[ConfigStorage] open failed: {} ({})", tmp_path,
                                  errno_reason(errno));
                    return false;
                }
                o << bytes;
                o.flush();
                if (!o.good()) {
                    spdlog::error("[ConfigStorage] write failed: {} ({})", tmp_path,
                                  errno_reason(errno));
                    std::remove(tmp_path.c_str());
                    return false;
                }
            }

            {
                int fd = ::open(tmp_path.c_str(), O_RDONLY);
                if (fd >= 0) {
                    (void)::fsync(fd);
                    ::close(fd);
                }
            }

            if (std::rename(tmp_path.c_str(), target_path.c_str()) != 0) {
                spdlog::error("[ConfigStorage] rename '{}' -> '{}' failed: {}", tmp_path,
                              target_path, strerror(errno));
                std::remove(tmp_path.c_str());
                return false;
            }

            {
                std::string dir = fs::path(target_path).parent_path().string();
                if (!dir.empty()) {
                    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
                    if (dfd >= 0) {
                        (void)::fsync(dfd);
                        ::close(dfd);
                    }
                }
            }

            // Rolling backup outside install dir (survives Moonraker wipes).
            write_rolling_backup(path_, CONFIG_BACKUP_PRIMARY, config_backup_fallback());
            return true;
        } catch (const std::exception& e) {
            spdlog::error("[ConfigStorage] exception saving {}: {}", path_, e.what());
            return false;
        }
    }

    void preserve_corrupt() override {
        std::string corrupt_path = path_ + ".corrupt";
        std::rename(path_.c_str(), corrupt_path.c_str());
        spdlog::info("[ConfigStorage] Corrupt config saved to {}", corrupt_path);
    }

    bool read_only() override {
        // Write-probe, moved verbatim from Config::init() (lines 1340-1359).
        fs::path config_dir = fs::path(path_).parent_path();
        std::string probe_path = (config_dir / ".helix-write-probe").string();
        std::ofstream probe(probe_path);
        if (!probe.is_open()) {
            int err = errno;
            if (err == EROFS || err == EACCES) {
                spdlog::warn("[ConfigStorage] Read-only filesystem detected ({})",
                             strerror(err));
                return true;
            }
            return false;
        }
        probe.close();
        std::remove(probe_path.c_str());
        return false;
    }

    std::string describe() const override { return path_; }

  private:
    std::string path_;
};

} // namespace

std::unique_ptr<ConfigStorage> make_file_config_storage(const std::string& path) {
    return std::make_unique<FileConfigStorage>(path);
}

} // namespace helix
```
Check `config_backup.h` for the exact declarations of `write_rolling_backup` / `CONFIG_BACKUP_PRIMARY` / `config_backup_fallback` (they're what `config.cpp` already calls at line 1607) and match the include/namespace.

- [ ] **Step 5: Route Config through the backend**

`include/config.h`:
```cpp
// with the other includes:
#include "config_storage.h"
// private members (next to read_only_mode_):
std::unique_ptr<ConfigStorage> storage_;
// public, next to init():
/**
 * @brief Inject a persistence backend (call BEFORE init()).
 *
 * Default when unset: make_file_config_storage(resolved path). Embedded
 * targets substitute NVS/LittleFS; tests substitute an in-memory mock.
 */
void set_storage(std::unique_ptr<ConfigStorage> storage) { storage_ = std::move(storage); }
```
Also add `storage_.reset();` inside `clear_path()` (line 107-110) so test fixtures that re-init with a new temp dir get a fresh default backend.

`src/system/config.cpp` — five edits:

1. After path resolution settles (immediately after the symlink-redirect block ends, line ~995, and before the `stat`-driven legacy block continues to use `path`), create the default backend **after** all path rewrites — i.e. right before the load at line 1060:
```cpp
    if (!storage_) {
        storage_ = make_file_config_storage(path);
    }
    bool config_modified = false;

    auto loaded_doc = storage_->load();
    if (loaded_doc) {
```
(replacing `bool config_modified = false;` + `if (stat(path.c_str(), &buffer) == 0) {` at lines 1060-1062).

2. Replace the parse at line 1066:
```cpp
            data = json::parse(*loaded_doc);
```

3. Replace the corrupt-file rename at lines 1097-1100 with:
```cpp
            // Preserve the corrupt document for diagnosis
            storage_->preserve_corrupt();
```

4. Replace the read-only probe block (lines 1340-1359) with:
```cpp
    // Probe for read-only storage before attempting any writes.
    read_only_mode_ = storage_->read_only();
    if (read_only_mode_) {
        spdlog::warn("[Config] Read-only storage ({}): config changes will not be persisted",
                     storage_->describe());
    }
```

5. Replace the non-atomic save-back (lines 1362-1366) with a real save (bonus: this path was previously NOT atomic):
```cpp
    if (config_modified && !read_only_mode_) {
        save();
    }
```
and replace the whole body of `Config::save()` after the two early-return guards (keep lines 1516-1524 exactly) with:
```cpp
    spdlog::trace("[Config] Saving config to {}", storage_ ? storage_->describe() : path);

    if (!storage_) {
        storage_ = make_file_config_storage(path);
    }

    std::ostringstream oss;
    oss << std::setw(2) << data << std::endl;
    if (!storage_->store(oss.str())) {
        NOTIFY_ERROR("Failed to save configuration file");
        CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                            fmt::format("store failed: {}", storage_->describe()));
        return false;
    }
    spdlog::trace("[Config] saved successfully to {}", storage_->describe());
    return true;
```
The tarball-detection backup parse (lines 1074-1090), `restore_from_backup` calls, env backup, and the startup `write_rolling_backup` (1372-1380) stay as-is — they are desktop backup infrastructure reading sibling files by path, not the primary document.

- [ ] **Step 6: Run the new tests + the full config suites**

```bash
make test -j && ./build/bin/helix-tests "[storage]"
./build/bin/helix-tests "[config]" 2>&1 | tail -5
make test-run 2>&1 | tail -10
```
Expected: new tests PASS; `test_config.cpp`, `test_config_migration_v18.cpp`, `test_config_preset.cpp`, `test_config_backup.cpp` all still green (they exercise the file backend through the default path); full suite green.

- [ ] **Step 7: Commit**

```bash
git add include/config_storage.h include/config.h src/system/config_storage_file.cpp src/system/config.cpp tests/unit/test_config_storage.cpp tests/test_helpers/mock_config_storage.h
git commit -m "feat(config): document-level ConfigStorage seam with atomic file backend"
```

---

### Task 6: WiFi platform-backend hook + ESP guard fences

**Files:**
- Modify: `include/wifi_backend.h` (declaration near the factory)
- Modify: `src/api/wifi_backend.cpp` (includes lines 14-19; factory lines 37-49)
- Modify: `src/api/wifi_manager.cpp:103` (failover fence)

**Interfaces:**
- Produces: `helix::create_platform_wifi_backend(bool silent)` — declared in the main tree, **defined only by embedded platform trees** (the ESP32 firmware provides the esp_wifi implementation in Plan 4). Desktop builds never reference the symbol.

- [ ] **Step 1: Declare the platform hook**

`include/wifi_backend.h`, immediately after the `WifiBackend` class (near the `create()` factory docs):
```cpp
namespace helix {
/**
 * Platform-provided backend factory for embedded targets. NOT defined in
 * the desktop build — the ESP32 firmware tree implements it against
 * esp_wifi (WifiBackend::create() calls it when ESP_PLATFORM is defined).
 */
std::unique_ptr<WifiBackend> create_platform_wifi_backend(bool silent);
} // namespace helix
```

- [ ] **Step 2: Wire the factory**

`src/api/wifi_backend.cpp`, includes (lines 14-19):
```cpp
#ifdef __APPLE__
#include "wifi_backend_macos.h"
#elif defined(ESP_PLATFORM)
// esp_wifi backend lives in the firmware tree; only the declaration in
// wifi_backend.h is needed here.
#elif !defined(__ANDROID__)
#include "wifi_backend_networkmanager.h"
#include "wifi_backend_wpa_supplicant.h"
#endif
```
Factory (insert between the `__APPLE__` arm ending at line 44 and the `__ANDROID__` arm at line 45):
```cpp
#elif defined(ESP_PLATFORM)
    // Embedded: the platform tree owns the backend (esp_wifi).
    return helix::create_platform_wifi_backend(silent);
```

- [ ] **Step 3: Fence the NM→wpa failover**

`src/api/wifi_manager.cpp:103`:
```cpp
// before
#if !defined(__APPLE__) && !defined(__ANDROID__)
// after
#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)
```

- [ ] **Step 4: Verify desktop is untouched**

```bash
make -j 2>&1 | tail -3
./build/bin/helix-tests "[wifi]" 2>&1 | tail -5
```
Expected: clean build (no `ESP_PLATFORM` on desktop, so the new arm compiles away; the undefined `create_platform_wifi_backend` symbol is never referenced), wifi test suites green.

- [ ] **Step 5: Commit**

```bash
git add include/wifi_backend.h src/api/wifi_backend.cpp src/api/wifi_manager.cpp
git commit -m "feat(wifi): platform-backend hook for embedded targets behind ESP_PLATFORM"
```

---

## Definition of done (Plan 2)

- `helix::asset_path()` exists; theme_manager (27 sites), layout_manager, and translation_loader route through it; desktop paths are byte-identical under the default root (full test suite + sanity boot).
- `make regen-tokens` regenerates a committed `src/generated/theme_token_table.cpp`; the parity test (`[tokens]`) fails CI whenever the table is stale.
- `HELIX_TOKEN_TABLE=1` boot registers identical constants measurably faster (numbers recorded in the Task 4 commit); dev builds still runtime-scan by default (XML-no-rebuild workflow preserved).
- `Config` loads/saves through `ConfigStorage`; all existing config/migration/preset/backup tests green; a mock backend proves injection.
- `WifiBackend::create()` has an `ESP_PLATFORM` arm delegating to `helix::create_platform_wifi_backend()`; desktop wifi tests green.
- Zero changes under `firmware/`; `esp32-build` CI job still green.
