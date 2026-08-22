# Print Status Camera Button + Unified Remote-Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Camera button to the print status panel (visible only when a webcam is configured AND the screen runs remotely from the printer), and sweep all existing "remote detection shaped behavior" onto one canonical live-endpoint truth: a new `moonraker_is_remote` subject backed by `helix::is_moonraker_on_same_host()`.

**Architecture:** A new int subject `moonraker_is_remote` lives in `PrinterNetworkState` (next to `printer_connection_state`), published by `MoonrakerManager` on each `CONNECTED` edge from the actual websocket endpoint. The XML button gates on `printer_has_webcam and moonraker_is_remote and platform_extras_available` via one inline `cond=`; its callback forwards to the existing `open_standalone_camera_fullscreen()`. Five UI sites drop private loopback-literal/Config-host checks in favor of the subject; two background/diagnostic sites consolidate on the canonical predicate, letting us delete two `is_local_host` twins.

**Tech Stack:** LVGL 9.5 XML engine (helix-xml fork), Catch2 tests, pure Makefile build, spdlog.

**Spec:** `docs/superpowers/specs/2026-08-18-print-status-camera-button-design.md` (committed at 3631ed83f)

## Global Constraints

- Work ONLY in the worktree `/home/pbrown/Code/Printing/helixscreen/.worktrees/print-status-camera` (branch `feature/print-status-camera`, based on `devel/1.1`). Bash cwd resets to the main repo between calls — always `make -C <worktree>` or pass `workdir`.
- Before any build: `pgrep -f 'make|c\+\+'` — concurrent compilations thrash the machine; wait or use `-j2` if another build is running.
- Build program only: `make -C <worktree> -j`; build tests only: `make -C <worktree> test`; build AND run tests: `make -C <worktree> test-run` (redirect to a file and grep — never pipe through `tail`, the filter masks the exit code).
- Run test binaries from the worktree root only (another cwd = mass fake failures).
- Declarative UI rules are absolute for new code: no `lv_obj_add_event_cb`, no imperative visibility, no C++ styling, compound conditions as inline `cond=` (word forms: `and`/`or`/`not`; `&&`/`<` need XML escaping).
- spdlog only; SPDX header `// SPDX-License-Identifier: GPL-3.0-or-later` on new files; no RTTI.
- Threading: never touch LVGL/subjects from background threads — route through the existing `async_lifetime_.defer()` façade pattern shown below.
- Pre-commit gates ratchet (counts may fall, never rise): imperative-UI 380, hardcoded pixels 162, design tokens 34. Committing runs the full hook (~8 min; includes an incremental build).
- Commit style: subject + ~4-line paragraph. `git add` explicit paths only — NEVER `git add -A`/`git add .`.
- Driving the app for verification: pin socket AND config dir:
  `HELIX_SOCK=/tmp/helix-print-status-camera.sock HELIX_CONFIG_DIR=/tmp/helix-config-print-status-camera` (mkdir the config dir first), run `--test -vv --remote-socket "$HELIX_SOCK"`, then `./build/bin/helix-screen ctl -s "$HELIX_SOCK" ...`. Kill the instance when done.
- `docs/superpowers/plans/` files need `git add -f` (path is gitignored by default; tracked files exist).

---

### Task 1: Consolidate host parsing into `host_identity` (Tier B)

**Files:**
- Modify: `include/host_identity.h`
- Modify: `src/system/host_identity.cpp`
- Modify: `include/helix_plugin_installer.h` (delete `is_local_host` + `extract_host_from_websocket_url` decls)
- Modify: `src/system/helix_plugin_installer.cpp` (delete both impls; convert `is_local_moonraker()`)
- Modify: `src/system/telemetry_manager.cpp` (convert `classify_moonraker_locality()`)
- Modify: `include/timelapse_thumbnailer.h` — NOT in this task (Task 5 deletes it)
- Test: `tests/unit/test_host_identity.cpp`, `tests/unit/test_helix_plugin_installer.cpp`

**Interfaces:**
- Produces (later tasks rely on these exact signatures):
  - `helix::extract_host_from_websocket_url(const std::string& url) -> std::string` declared in `include/host_identity.h`
  - `helix::is_moonraker_on_same_host(std::string_view host) -> bool` (unchanged, existing)
- `helix::is_local_host` (installer) and `helix::timelapse::is_local_host` (thumbnailer) are being deleted — Task 5 handles the thumbnailer's.

- [ ] **Step 1: Write failing tests for the relocated parser**

In `tests/unit/test_host_identity.cpp`, inside the existing `namespace helix` / TEST_CASE structure (follow the file's current style), add:

```cpp
TEST_CASE("extract_host_from_websocket_url parses scheme URLs", "[host_identity]") {
    REQUIRE(extract_host_from_websocket_url("ws://192.168.1.100:7125/websocket") == "192.168.1.100");
    REQUIRE(extract_host_from_websocket_url("wss://printer.local:7125/websocket") == "printer.local");
    REQUIRE(extract_host_from_websocket_url("ws://[::1]:7125/websocket") == "::1");
    REQUIRE(extract_host_from_websocket_url("ws://myhost/websocket") == "myhost");
    REQUIRE(extract_host_from_websocket_url("ws://10.0.0.5:7125") == "10.0.0.5");
    REQUIRE(extract_host_from_websocket_url("").empty());
    REQUIRE(extract_host_from_websocket_url("http://192.168.1.100:8080/") == ""); // unknown scheme
}
```

- [ ] **Step 2: Verify it fails**

Run: `make -C <worktree> test && <worktree>/build/bin/helix-tests "[host_identity]"` (run from the worktree root)
Expected: FAIL — `extract_host_from_websocket_url` not declared in `host_identity.h`.

- [ ] **Step 3: Relocate the function**

In `include/host_identity.h`, after the `is_moonraker_on_same_host` declaration, add:

```cpp
/// Extract the host portion of a websocket URL ("ws://host:port/..." or
/// "wss://..."; bracketed IPv6 handled). Returns "" for empty input or an
/// unknown scheme. Canonical home for "which host are we talking to" —
/// keep URL parsing and host-identity checks together.
[[nodiscard]] std::string extract_host_from_websocket_url(const std::string& url);
```

(Add `#include <string>` alongside the existing `<string_view>`.)

Move the implementation verbatim from `src/system/helix_plugin_installer.cpp` (the ~40-line function starting `std::string extract_host_from_websocket_url(const std::string& url) {`) into `src/system/host_identity.cpp` inside `namespace helix`.

- [ ] **Step 4: Delete the installer's loopback twin and convert its caller**

In `src/system/helix_plugin_installer.cpp`:

Replace the body of `is_local_moonraker()` (currently `std::string host = extract_host_from_websocket_url(websocket_url_); return is_local_host(host);`) with:

```cpp
bool HelixPluginInstaller::is_local_moonraker() const {
    if (websocket_url_.empty()) {
        return false;
    }

    // Canonical predicate (own hostname + interface IPs, not just loopback
    // literals) — installing into a printer whose address resolves to THIS
    // machine is exactly the local case.
    return is_moonraker_on_same_host(extract_host_from_websocket_url(websocket_url_));
}
```

Delete the file-local `is_local_host()` definition (lines ~32-35: `return host == "localhost" || host == "127.0.0.1" || host == "::1";`). In `include/helix_plugin_installer.h`, delete the `[[nodiscard]] bool is_local_host(const std::string& host);` declaration and the `extract_host_from_websocket_url` declaration (now in `host_identity.h`). Add `#include "host_identity.h"` to `helix_plugin_installer.cpp` if not already present.

- [ ] **Step 5: Convert telemetry locality classification**

In `src/system/telemetry_manager.cpp`, `classify_moonraker_locality()` currently ends with `return helix::is_local_host(host);`. Replace with:

```cpp
    return helix::is_moonraker_on_same_host(host);
```

Update its `#include "helix_plugin_installer.h"` to `#include "host_identity.h"` (verify nothing else in the file uses the installer header first: `grep -n "helix_plugin_installer" src/system/telemetry_manager.cpp`).

- [ ] **Step 6: Update the installer tests**

In `tests/unit/test_helix_plugin_installer.cpp`, delete the `SECTION("is_local_host correctly identifies localhost URLs")` block (the `helix::is_local_host` REQUIREs at ~lines 32-41) — the twin is gone and `is_moonraker_on_same_host` has its own test file.

- [ ] **Step 7: Run tests**

Run: `make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "[host_identity]" && ./build/bin/helix-tests "[installer]" && ./build/bin/helix-tests "[telemetry]"` (adjust tags to whatever the files' TEST_CASEs actually use — check with `grep -n "TEST_CASE" tests/unit/test_helix_plugin_installer.cpp`)
Expected: all PASS, including the new parser tests.

- [ ] **Step 8: Commit**

```bash
git -C <worktree> add include/host_identity.h src/system/host_identity.cpp \
  include/helix_plugin_installer.h src/system/helix_plugin_installer.cpp \
  src/system/telemetry_manager.cpp tests/unit/test_host_identity.cpp \
  tests/unit/test_helix_plugin_installer.cpp
git -C <worktree> commit -m "refactor(host-identity): one home for host parsing + locality predicate

extract_host_from_websocket_url moves from helix_plugin_installer to
host_identity, next to is_moonraker_on_same_host. Installer and telemetry
locality checks drop their loopback-literal twin (is_local_host) for the
canonical predicate, so own-hostname/interface-IP hosts now read local."
```

---

### Task 2: `moonraker_is_remote` subject

**Files:**
- Modify: `include/printer_network_state.h`
- Modify: `src/printer/printer_network_state.cpp`
- Modify: `include/printer_state.h`
- Modify: `src/printer/printer_state.cpp`
- Test: create `tests/unit/test_moonraker_is_remote.cpp`

**Interfaces:**
- Produces (Tasks 3-6 rely on these exact names):
  - XML subject name: `moonraker_is_remote` (int; 1 = connected Moonraker is NOT this host; 0 = local/unknown; default 0)
  - `void PrinterState::set_moonraker_is_remote(bool remote)` — thread-safe façade (defers to main thread)
  - `void PrinterNetworkState::set_moonraker_is_remote_internal(bool remote)` — main-thread setter
  - `lv_subject_t* PrinterState::get_moonraker_is_remote_subject()`
  - `bool PrinterState::is_moonraker_remote()` — main-thread convenience read (`lv_subject_get_int(...) != 0`)

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_moonraker_is_remote.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_moonraker_is_remote.cpp
 * @brief moonraker_is_remote subject lifecycle
 *
 * The subject is the UI-facing mirror of helix::is_moonraker_on_same_host()
 * evaluated against the live websocket endpoint. Default is 0 (local/
 * unknown); it flips on every connected edge, so a mid-session printer
 * switch re-truths every gated affordance.
 */

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {
struct RemoteSubjectFixture : public LVGLUITestFixture {
    lv_subject_t* subject() {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "moonraker_is_remote");
        REQUIRE(s != nullptr);
        return s;
    }
};
} // namespace

TEST_CASE_METHOD(RemoteSubjectFixture, "moonraker_is_remote defaults to 0", "[subjects][network]") {
    REQUIRE(lv_subject_get_int(subject()) == 0);
}

TEST_CASE_METHOD(RemoteSubjectFixture, "moonraker_is_remote flips per connected edge",
                 "[subjects][network]") {
    get_printer_state().set_moonraker_is_remote(true);
    REQUIRE(lv_subject_get_int(subject()) == 1);
    REQUIRE(get_printer_state().is_moonraker_remote());

    // Printer switch: same session, opposite verdict
    get_printer_state().set_moonraker_is_remote(false);
    REQUIRE(lv_subject_get_int(subject()) == 0);
    REQUIRE_FALSE(get_printer_state().is_moonraker_remote());
}
```

Check how `tests/unit/test_print_status_header_action_button.cpp` gets `state()` / `get_printer_state()` and mirror that include set (`printer_state.h`, `app_globals.h` if needed). Register the file in the test build the way neighbors are (check `mk/tests.mk` or the test glob — if tests are globbed, nothing to do).

- [ ] **Step 2: Verify it fails**

Run: `make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "[network]"` — wait, first check the tag is not already crowded; better run by name filter. Use `./build/bin/helix-tests "moonraker_is_remote"`.
Expected: FAIL — no subject named `moonraker_is_remote`, `set_moonraker_is_remote` not declared.

- [ ] **Step 3: Implement in PrinterNetworkState**

`include/printer_network_state.h`:
- In the class doc comment change `Subjects (6 total):` to `Subjects (7 total):` and add the line ` * - moonraker_is_remote_ (int) - 1 when the connected Moonraker is NOT this host`.
- Public setters section, after `set_klippy_state_message`:

```cpp
    /**
     * @brief Set remote-screen verdict (synchronous, must be on UI thread)
     *
     * Published by MoonrakerManager on each CONNECTED edge from the live
     * websocket endpoint. 1 = the Moonraker we are talking to does not
     * resolve to this host (remote screen); 0 = local/unknown.
     */
    void set_moonraker_is_remote_internal(bool remote);
```

- Subject accessors, after `get_moonraker_connection_state_subject`:

```cpp
    /// Remote-screen verdict (1 = connected Moonraker is not this host; 0 = local/unknown)
    lv_subject_t* get_moonraker_is_remote_subject() {
        return &moonraker_is_remote_;
    }
```

- Private subjects, after `moonraker_connection_state_`:

```cpp
    lv_subject_t moonraker_is_remote_{}; // 1 when connected Moonraker is not this host
```

`src/printer/printer_network_state.cpp`:
- In `init_subjects()`, after the `moonraker_connection_state` init:

```cpp
    // Remote-screen verdict: 1 when the connected Moonraker endpoint does not
    // resolve to this host. Set on CONNECTED edges by MoonrakerManager; the
    // UI-facing mirror of helix::is_moonraker_on_same_host().
    INIT_SUBJECT_INT(moonraker_is_remote, 0, subjects_, register_xml);
```

- New setter (place after `set_printer_connection_state_internal`):

```cpp
void PrinterNetworkState::set_moonraker_is_remote_internal(bool remote) {
    const int value = remote ? 1 : 0;
    if (lv_subject_get_int(&moonraker_is_remote_) == value)
        return;
    spdlog::info("[PrinterNetworkState] moonraker_is_remote: {} ({} Moonraker)", value,
                 remote ? "remote" : "same-host");
    lv_subject_set_int(&moonraker_is_remote_, value);
}
```

- [ ] **Step 4: Add the PrinterState façade**

`src/printer/printer_state.cpp`, next to `set_printer_connection_state` (mirroring its defer pattern exactly):

```cpp
void PrinterState::set_moonraker_is_remote(bool remote) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    async_lifetime_.defer("PrinterState::set_moonraker_is_remote", [this, remote]() {
        network_state_.set_moonraker_is_remote_internal(remote);
    });
}

bool PrinterState::is_moonraker_remote() {
    // Main-thread convenience read; UI decision points only.
    return lv_subject_get_int(network_state_.get_moonraker_is_remote_subject()) != 0;
}
```

`include/printer_state.h`, in the public API near `set_printer_connection_state` (~line 1389):

```cpp
    /// Remote-screen verdict from the live websocket endpoint (thread-safe;
    /// defers the subject write to the main thread). Published by
    /// MoonrakerManager on CONNECTED edges.
    void set_moonraker_is_remote(bool remote);

    /// Main-thread read of moonraker_is_remote (true = connected Moonraker is
    /// not this host). For UI decision points; background code uses
    /// helix::is_moonraker_on_same_host() directly.
    bool is_moonraker_remote();
```

And next to the other subject accessors (~line 1237):

```cpp
    lv_subject_t* get_moonraker_is_remote_subject();
```

with an inline forwarding definition in the header next to `get_nav_buttons_enabled_subject`'s forwarding, or in printer_state.cpp beside the others at ~1207 — follow whichever style that block uses:

```cpp
lv_subject_t* PrinterState::get_moonraker_is_remote_subject() {
    return network_state_.get_moonraker_is_remote_subject();
}
```

- [ ] **Step 5: Run tests**

Run: `make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "moonraker_is_remote"`
Expected: PASS (3 test cases).

- [ ] **Step 6: Commit**

```bash
git -C <worktree> add include/printer_network_state.h src/printer/printer_network_state.cpp \
  include/printer_state.h src/printer/printer_state.cpp tests/unit/test_moonraker_is_remote.cpp
git -C <worktree> commit -m "feat(subjects): moonraker_is_remote — live-endpoint remote-screen verdict

New int subject in PrinterNetworkState, default 0, set per CONNECTED edge
from the actual websocket endpoint. UI-facing mirror of the canonical
is_moonraker_on_same_host predicate; background code keeps the predicate."
```

---

### Task 3: Publish from MoonrakerManager + `HELIX_MOCK_REMOTE_PRINTER`

**Files:**
- Modify: `include/runtime_config.h`
- Modify: `src/system/runtime_config.cpp`
- Modify: `src/application/moonraker_manager.cpp`
- Modify: `docs/devel/ENVIRONMENT_VARIABLES.md`

**Interfaces:**
- Consumes: `PrinterState::set_moonraker_is_remote(bool)` (Task 2), `helix::extract_host_from_websocket_url` + `helix::is_moonraker_on_same_host` (Task 1)
- Produces: env var `HELIX_MOCK_REMOTE_PRINTER=1` (test-mode only) forces the subject to 1.

- [ ] **Step 1: Add the RuntimeConfig accessor**

`include/runtime_config.h`, next to `should_mock_ams()` (~line 207):

```cpp
    /**
     * @brief Check if remote-printer mode should be forced in mock runs
     *
     * HELIX_MOCK_REMOTE_PRINTER=1 under --test forces moonraker_is_remote=1 so
     * remote-gated affordances (print-status camera button, remote playback
     * paths) are drivable via ctl. The mock connects over loopback, which
     * otherwise always reads as same-host.
     * @return true if test mode is on and the env var is set to a non-"0" value
     */
    bool should_mock_remote_printer() const {
        if (!test_mode)
            return false;
        const char* env = std::getenv("HELIX_MOCK_REMOTE_PRINTER");
        return env && env[0] && std::string(env) != "0";
    }
```

Add `#include <cstdlib>` and `#include <string>` to the header's include block if missing. (Inline accessor matches the `should_mock_*` style; env parse mirrors the HELIX_MOCK_AUTO_PRINT acceptance rules.)

- [ ] **Step 2: Wire the CONNECTED-edge update**

In `src/application/moonraker_manager.cpp`, `process_notifications()`, inside the existing `if (new_state == static_cast<int>(ConnectionState::CONNECTED) && m_api) {` block (right after the `helix::SensorState::instance().subscribe(*m_api);` line, ~line 263), add:

```cpp
                // Remote-screen verdict from the LIVE endpoint — Config's
                // moonraker_host can lag a mid-session printer switch. Published
                // on every CONNECTED edge so gated affordances re-truth after a
                // switch. HELIX_MOCK_REMOTE_PRINTER forces it for --test runs
                // (the mock's loopback endpoint always reads same-host).
                bool moonraker_remote = !helix::is_moonraker_on_same_host(
                    helix::extract_host_from_websocket_url(m_api->get_websocket_url()));
                if (get_runtime_config()->should_mock_remote_printer()) {
                    spdlog::info("[MoonrakerManager] HELIX_MOCK_REMOTE_PRINTER set — forcing "
                                 "remote-screen mode");
                    moonraker_remote = true;
                }
                get_printer_state().set_moonraker_is_remote(moonraker_remote);
```

Ensure includes: `#include "host_identity.h"` (add to the include block; Task 1 made `extract_host_from_websocket_url` live there) and `#include "runtime_config.h"` (verify with `grep -n "runtime_config" src/application/moonraker_manager.cpp` — `get_runtime_config()` is already used in this file, so it may already be transitively included; add it explicitly if the grep finds no direct include).

Note: `get_websocket_url()` is on `IMoonrakerAPI` (`include/i_moonraker_api.h:111`) — `m_api` is the interface unique_ptr, no concrete-type lint exposure.

- [ ] **Step 3: Extend the subject test with the mock override**

In `tests/unit/test_moonraker_is_remote.cpp`, add:

```cpp
TEST_CASE_METHOD(RemoteSubjectFixture, "HELIX_MOCK_REMOTE_PRINTER is test-mode gated",
                 "[subjects][network]") {
    // Accessor contract: production runs never force, regardless of env.
    // (Direct env manipulation inside the test process would leak across
    // parallel cases; assert the negative arm only.)
    REQUIRE_FALSE(get_runtime_config()->should_mock_remote_printer());
}
```

Include `runtime_config.h` / `app_globals.h` for `get_runtime_config()` (check the fixture's existing includes; `app_globals.h` provides it).

- [ ] **Step 4: Build + run**

Run: `make -C <worktree> -j && make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "moonraker_is_remote"`
Expected: PASS.

- [ ] **Step 5: Smoke-verify end to end with the mock**

```bash
export HELIX_SOCK=/tmp/helix-print-status-camera.sock
export HELIX_CONFIG_DIR=/tmp/helix-config-print-status-camera
mkdir -p "$HELIX_CONFIG_DIR"
cd <worktree> && HELIX_MOCK_REMOTE_PRINTER=1 ./build/bin/helix-screen --test -vv \
    --remote-socket "$HELIX_SOCK" > /tmp/helix-print-status-camera.log 2>&1 &
sleep 3; grep "moonraker_is_remote" /tmp/helix-print-status-camera.log
./build/bin/helix-screen ctl -s "$HELIX_SOCK" demo home
pkill -f "helix-print-status-camera.sock"   # kill ONLY by pinned socket string
```

Expected log line: `[PrinterNetworkState] moonraker_is_remote: 1 (remote Moonraker)` plus the `[MoonrakerManager] HELIX_MOCK_REMOTE_PRINTER set` info line.

- [ ] **Step 6: Document the env var**

In `docs/devel/ENVIRONMENT_VARIABLES.md`, Mock & Testing section (near `HELIX_MOCK_AUTO_PRINT`, ~line 883), add an entry following the neighbors' format:

```markdown
### `HELIX_MOCK_REMOTE_PRINTER`

**Remote-screen simulation:** Forces the `moonraker_is_remote` subject to 1 in
`--test` runs. The mock client connects over loopback, which always reads as
same-host — this flag makes remote-gated UI (print-status camera button, remote
video playback paths) appear and behave as if HelixScreen were a remote screen.

```bash
HELIX_MOCK_REMOTE_PRINTER=1 ./build/bin/helix-screen --test -vv
```
```

- [ ] **Step 7: Commit**

```bash
git -C <worktree> add include/runtime_config.h src/system/runtime_config.cpp \
  src/application/moonraker_manager.cpp docs/devel/ENVIRONMENT_VARIABLES.md \
  tests/unit/test_moonraker_is_remote.cpp
git -C <worktree> commit -m "feat(moonraker): publish moonraker_is_remote on CONNECTED edges

MoonrakerManager evaluates the canonical predicate against the live
websocket endpoint on each CONNECTED edge, so a mid-session printer switch
re-truths gated affordances. HELIX_MOCK_REMOTE_PRINTER=1 (--test only)
forces remote for ctl-driving remote-gated UI."
```

---

### Task 4: The Camera button

**Files:**
- Modify: `ui_xml/print_status_panel.xml` (landscape, ~line 427)
- Modify: `ui_xml/portrait/print_status_panel.xml` (~line 488)
- Modify: `include/ui_panel_print_status.h` (~line 636)
- Modify: `src/ui/ui_panel_print_status.cpp` (~line 1904 area + callback map ~line 682)
- Test: create `tests/unit/test_print_status_camera_button.cpp`

**Interfaces:**
- Consumes: subjects `printer_has_webcam` (existing), `moonraker_is_remote` (Task 2), `platform_extras_available` (existing); `helix::open_standalone_camera_fullscreen(lv_obj_t*)` (existing, camera_widget.cpp — forward-declared, NOT included: that directory is not on this file's include path).
- Produces: XML callback name `on_print_status_camera`.

- [ ] **Step 1: Write the failing gating test**

Create `tests/unit/test_print_status_camera_button.cpp`, modeled directly on `tests/unit/test_print_status_header_action_button.cpp` (same fixture shape — real `PrintStatusPanel` from production XML via `LVGLUITestFixture`):

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_status_camera_button.cpp
 * @brief btn_camera visibility = webcam AND remote AND platform camera support
 *
 * The camera button must stay hidden on the printer's own screen (the print is
 * right in front of it), with no webcam configured, and on builds whose camera
 * code is compiled out (platform_extras_available gates the ESP32 v1 cut).
 * All 8 combinations of the three gate subjects are driven through the REAL
 * panel XML — no C++ visibility pokes exist to drift from the binding.
 */

#include "ui_panel_print_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

struct CameraButtonFixture : public LVGLUITestFixture {
    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* root_ = nullptr;
    lv_subject_t* webcam_ = nullptr;
    lv_subject_t* remote_ = nullptr;
    lv_subject_t* extras_ = nullptr;

    CameraButtonFixture() {
        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = panel_->create(test_screen());
        REQUIRE(root_ != nullptr);
        process_lvgl(50);

        webcam_ = lv_xml_get_subject(nullptr, "printer_has_webcam");
        remote_ = lv_xml_get_subject(nullptr, "moonraker_is_remote");
        extras_ = lv_xml_get_subject(nullptr, "platform_extras_available");
        REQUIRE(webcam_ != nullptr);
        REQUIRE(remote_ != nullptr);
        REQUIRE(extras_ != nullptr);
    }

    lv_obj_t* button() {
        lv_obj_t* btn = lv_obj_find_by_name(root_, "btn_camera");
        REQUIRE(btn != nullptr);
        return btn;
    }

    void set(lv_subject_t* s, int v) {
        lv_subject_set_int(s, v);
        process_lvgl(20);
    }
};

} // namespace

TEST_CASE_METHOD(CameraButtonFixture, "btn_camera visible only for remote webcam builds",
                 "[print_status][camera][ui_integration]") {
    const bool visible[2][2][2] = {
        /* webcam=0 */ {/* remote=0 */ {/* extras=0 */ false, /* extras=1 */ false},
                        /* remote=1 */ {/* extras=0 */ false, /* extras=1 */ false}},
        /* webcam=1 */ {/* remote=0 */ {/* extras=0 */ false, /* extras=1 */ false},
                        /* remote=1 */ {/* extras=0 */ false, /* extras=1 */ true}}};

    for (int w = 0; w <= 1; ++w)
        for (int r = 0; r <= 1; ++r)
            for (int e = 0; e <= 1; ++e) {
                set(webcam_, w);
                set(remote_, r);
                set(extras_, e);
                INFO("webcam=" << w << " remote=" << r << " extras=" << e);
                const bool hidden = lv_obj_has_flag(button(), LV_OBJ_FLAG_HIDDEN);
                CHECK(hidden != visible[w][r][e]);
            }
}
```

(Match the neighbor fixture's exact construction call — check `test_print_status_header_action_button.cpp` lines ~55-62 for the `PrintStatusPanel` ctor args and `create()` call, and copy those verbatim. `process_lvgl` comes from the LVGL fixture.)

- [ ] **Step 2: Verify it fails**

Run: `make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "[camera][ui_integration]"`
Expected: FAIL — `lv_obj_find_by_name(root_, "btn_camera")` returns nullptr (REQUIRE fires).

- [ ] **Step 3: Add the button to the landscape XML**

In `ui_xml/print_status_panel.xml`, Row 2 of `button_grid`, between `btn_tune`'s closing `</ui_button>` (~line 431) and the `btn_cancel` comment (~line 432), insert:

```xml
            <!-- Camera button: live view of the print. Only when a webcam is
                 configured AND this screen runs remotely from the printer
                 (moonraker_is_remote — on the printer's own screen the print is
                 in front of you) AND the build has camera support compiled in
                 (platform_extras_available gates the ESP32 v1 cut). -->
            <ui_button name="btn_camera"
                       height="#button_height" flex_grow="1" icon="video" text="Camera" translation_tag="Camera">
              <bind_flag_if cond="printer_has_webcam and moonraker_is_remote and platform_extras_available"
                            flag="hidden" invert="true"/>
              <event_cb trigger="clicked" callback="on_print_status_camera"/>
            </ui_button>
```

Notes: `invert="true"` makes the cond read as the SHOW condition (guide § bind_flag_if). Bare subject names are truthy when non-zero. `video` icon exists (`include/ui_icon_codepoints.h:298`). `Camera` translation key already exists (`ui_xml/translations/en.xml:358`) — no translation-sync pass needed. Styling matches `btn_tune`/`btn_cancel` (labels visible; only Row 1 buttons carry `label_hidden_if_bp_eq`).

- [ ] **Step 4: Add the button to the portrait XML**

In `ui_xml/portrait/print_status_panel.xml`, same single button row, between `btn_tune`'s closing tag and `btn_cancel` (~line 488), insert the same block with the portrait row's button style (`icon_position="top"`, matching every sibling):

```xml
            <ui_button name="btn_camera"
                       height="#button_height" flex_grow="1" icon="video" text="Camera"
                       translation_tag="Camera" icon_position="top">
              <bind_flag_if cond="printer_has_webcam and moonraker_is_remote and platform_extras_available"
                            flag="hidden" invert="true"/>
              <event_cb trigger="clicked" callback="on_print_status_camera"/>
            </ui_button>
```

(The layout-variant parity gate requires identical wiring between variants — same name, same binding, same callback.)

- [ ] **Step 5: Register the callback**

`include/ui_panel_print_status.h`, next to `static void on_tune_clicked(lv_event_t* e);` (~line 635):

```cpp
    static void on_print_status_camera(lv_event_t* e);
```

`src/ui/ui_panel_print_status.cpp`:
1. Near the top includes, after the existing forward-declaration section (search the file for an existing `namespace helix {` forward block; if none, place directly after the last `#include`):

```cpp
#if HELIX_HAS_CAMERA
// Defined in src/ui/panel_widgets/camera_widget.cpp; that directory is not on
// this file's include path, so forward-declare rather than including the
// header (same pattern as ui_settings_hardware.cpp).
namespace helix {
void open_standalone_camera_fullscreen(lv_obj_t* parent_screen);
}
#endif
```

(If `ui_panel_print_status.cpp` is not itself inside `namespace helix`, drop the wrapper namespace accordingly — match how `ui_settings_hardware.cpp` does it relative to its own namespace.)

2. In the `register_xml_callbacks({...})` map (~line 681), add:

```cpp
        {"on_print_status_camera", on_print_status_camera},
```

3. Next to `PrintStatusPanel::on_tune_clicked` (~line 1904), add:

```cpp
void PrintStatusPanel::on_print_status_camera(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_print_status_camera");
    (void)e;
#if HELIX_HAS_CAMERA
    // No-ops when no webcam is configured or a fullscreen view is already
    // open; reuses an attached home CameraWidget's stream when one exists
    // (single-MJPEG-client safe).
    helix::open_standalone_camera_fullscreen(lv_display_get_screen_active(nullptr));
#else
    spdlog::debug("[PrintStatusPanel] Camera support disabled in this build");
#endif
    LVGL_SAFE_EVENT_CB_END();
}
```

The registration is unconditional and only the body is `#if`-guarded — that is the established cross-build pattern (`ui_settings_hardware.cpp:167-174`), so the XML never references an unregistered callback name. Visibility on camera-less builds is handled by `platform_extras_available`.

- [ ] **Step 6: Run the gating test**

Run: `make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "[camera][ui_integration]"`
Expected: PASS (all 8 combinations).

- [ ] **Step 7: Verify interactively with the mock**

```bash
export HELIX_SOCK=/tmp/helix-print-status-camera.sock HELIX_CONFIG_DIR=/tmp/helix-config-print-status-camera
mkdir -p "$HELIX_CONFIG_DIR"
cd <worktree> && HELIX_MOCK_AUTO_PRINT=1 HELIX_MOCK_REMOTE_PRINTER=1 \
  ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-print-status-camera.log 2>&1 &
sleep 8   # let the auto-print reach Printing (~15s at 1x; button is state-independent, Preparing is fine)
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate print_status
./build/bin/helix-screen ctl -s "$HELIX_SOCK" geom btn_camera          # non-zero size = visible
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click btn_camera
sleep 2
./build/bin/helix-screen ctl -s "$HELIX_SOCK" ls | grep -i camera_fullscreen   # overlay is up
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click fullscreen_close_btn
pkill -f "helix-print-status-camera.sock"
```

Expected: `geom btn_camera` reports a laid-out button; after click, camera_fullscreen nodes exist in the tree. (Camera feed itself stays at "Connecting Camera..." against the mock — there is no MJPEG source; that is expected.)

Also run the negative arm once (relaunch WITHOUT `HELIX_MOCK_REMOTE_PRINTER`): `ctl geom btn_camera` must report the button hidden (`geom` shows zero/hidden — check with `ctl ls` visibility output).

- [ ] **Step 8: Commit**

```bash
git -C <worktree> add ui_xml/print_status_panel.xml ui_xml/portrait/print_status_panel.xml \
  include/ui_panel_print_status.h src/ui/ui_panel_print_status.cpp \
  tests/unit/test_print_status_camera_button.cpp
git -C <worktree> commit -m "feat(print-status): camera button for remote screens

Row-2 ui_button gated by one inline cond on printer_has_webcam,
moonraker_is_remote and platform_extras_available (ESP32 cut), forwarding
to the existing standalone fullscreen camera overlay. Hidden on the
printer's own screen — the print is in front of you."
```

---

### Task 5: Sweep — timelapse videos overlay

**Files:**
- Modify: `src/ui/ui_overlay_timelapse_videos.cpp` (~lines 654-676)
- Modify: `include/timelapse_thumbnailer.h` (delete `is_local_host` decl, line 25)
- Modify: `src/print/timelapse_thumbnailer.cpp` (delete impl, lines 42-44)
- Test: `tests/unit/test_timelapse_videos.cpp` (delete the direct is_local_host cases)

**Interfaces:**
- Consumes: `PrinterState::is_moonraker_remote()` (Task 2)
- Deletes: `helix::timelapse::is_local_host` — verify no other consumer first.

- [ ] **Step 1: Confirm the twin is now single-consumer**

Run: `grep -rn "timelapse::is_local_host\|is_local_host" <worktree>/src <worktree>/include | grep -v test`
Expected: only `ui_overlay_timelapse_videos.cpp:672` (the call), plus the decl/impl pair being deleted, plus `helix_plugin_installer` references removed by Task 1. If any other consumer appeared, stop and re-scope.

- [ ] **Step 2: Convert the site**

In `src/ui/ui_overlay_timelapse_videos.cpp`, replace the whole block from `// Check if we're running on the same host as Moonraker (may change between connections)` through `is_local_moonraker_ = helix::timelapse::is_local_host(host);` and its closing `}` (~lines 653-677) with:

```cpp
    // Remote vs same-host playback path follows the live connection verdict
    // (moonraker_is_remote subject) — replaces the hand-rolled websocket-URL
    // parse + loopback-literal check this function used to carry.
    is_local_moonraker_ = !get_printer_state().is_moonraker_remote();
```

Add `#include "app_globals.h"` if not already present (check first — the file includes `i_moonraker_api.h`; `get_printer_state` lives in `app_globals.h`).

- [ ] **Step 3: Delete the twin**

- `include/timelapse_thumbnailer.h`: delete `bool is_local_host(const std::string& host);` (line 25) and its doc comment if one exists.
- `src/print/timelapse_thumbnailer.cpp`: delete the definition (lines 42-44).

- [ ] **Step 4: Update tests**

In `tests/unit/test_timelapse_videos.cpp`, delete `TEST_CASE("Playback: local vs remote detection", ...)` (lines ~10-20) — the loopback-literal expectations are exactly what the canonical predicate replaced. Keep the arg-construction/injection cases.

- [ ] **Step 5: Build + run**

Run: `make -C <worktree> -j && make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "[timelapse][videos]"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git -C <worktree> add src/ui/ui_overlay_timelapse_videos.cpp include/timelapse_thumbnailer.h \
  src/print/timelapse_thumbnailer.cpp tests/unit/test_timelapse_videos.cpp
git -C <worktree> commit -m "refactor(timelapse): playback locality follows moonraker_is_remote

Drops the hand-rolled websocket-URL parse + loopback-literal twin
(timelapse::is_local_host) in favor of the live-endpoint subject;
own-hostname/interface-IP hosts now correctly take the local path."
```

---

### Task 6: Sweep — shutdown, input shaper, spoolman, change-host

**Files:**
- Modify: `src/ui/panel_widgets/shutdown_widget.cpp` (~lines 386-391)
- Modify: `src/ui/ui_panel_input_shaper.cpp` (~lines 1003-1010)
- Modify: `src/ui/ui_spoolman_overlay.cpp` (~lines 1248-1257)
- Modify: `src/ui/ui_change_host_modal.cpp` (~lines 459-475)

**Interfaces:**
- Consumes: `PrinterState::is_moonraker_remote()` (Task 2)

All four sites are main-thread UI code (dialog open / toast / overlay open / modal show). Each swaps a Config-host read for the subject read. `app_globals.h` provides `get_printer_state()` — add the include where missing (grep each file first).

- [ ] **Step 1: Convert shutdown_widget**

In `src/ui/panel_widgets/shutdown_widget.cpp`, replace:

```cpp
    std::string host;
    if (Config* cfg = Config::get_instance()) {
        host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
    }

    if (helix::is_moonraker_on_same_host(host)) {
```

with:

```cpp
    // Same-host single-scope follows the LIVE connection verdict rather than
    // the Config host (stale across a mid-session printer switch).
    if (!get_printer_state().is_moonraker_remote()) {
```

(Keep the comment block above it about machine.shutdown semantics — update its first sentence only if it names the Config read.)

- [ ] **Step 2: Convert input shaper**

In `src/ui/ui_panel_input_shaper.cpp`, replace:

```cpp
        std::string host;
        if (Config* cfg = Config::get_instance()) {
            host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
        }
        const bool same_host = helix::is_moonraker_on_same_host(host);
```

with:

```cpp
        const bool same_host = !get_printer_state().is_moonraker_remote();
```

- [ ] **Step 3: Convert spoolman local-config fallback**

In `src/ui/ui_spoolman_overlay.cpp`, `try_local_config_fallback()`, replace:

```cpp
    std::string moonraker_host;
    if (Config* cfg = Config::get_instance())
        moonraker_host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");

    // Both gates must hold. Our /proc says nothing about a printer across the
    // network, and without a writable root there is nowhere to put the file the
    // include would name — an include with no matching file stops Moonraker dead.
    if (!helix::is_moonraker_on_same_host(moonraker_host) || config_root_abs_.empty()) {
```

with:

```cpp
    // Both gates must hold. Our /proc says nothing about a printer across the
    // network, and without a writable root there is nowhere to put the file the
    // include would name — an include with no matching file stops Moonraker dead.
    // Locality follows the live connection verdict (moonraker_is_remote), not
    // the Config host.
    if (get_printer_state().is_moonraker_remote() || config_root_abs_.empty()) {
```

- [ ] **Step 4: Convert change-host modal (keeping the empty-host special case)**

In `src/ui/ui_change_host_modal.cpp`, replace:

```cpp
        std::string host;
        if (Config* cfg = Config::get_instance()) {
            host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");
        }
        if (!host.empty() && helix::is_moonraker_on_same_host(host)) {
```

with:

```cpp
        // The emptiness check stays Config-based: an unconfigured host must keep
        // offering "Change Address", and the subject cannot distinguish "local"
        // from "never connected". The locality verdict itself follows the live
        // connection endpoint.
        std::string host;
        if (Config* cfg = Config::get_instance()) {
            host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");
        }
        if (!host.empty() && !get_printer_state().is_moonraker_remote()) {
```

(Preserve the existing comment above about why the default is "" — it stays accurate.)

- [ ] **Step 5: Check for orphaned includes**

Run: `grep -n "host_identity.h\|\"config.h\"" <worktree>/src/ui/panel_widgets/shutdown_widget.cpp <worktree>/src/ui/ui_panel_input_shaper.cpp <worktree>/src/ui/ui_spoolman_overlay.cpp <worktree>/src/ui/ui_change_host_modal.cpp`
Remove `host_identity.h` includes only if the file no longer references `is_moonraker_on_same_host`; leave `config.h` if other Config uses remain (change-host does; shutdown/input-shaper/spoolman may not — verify per file with `grep -n "Config::get_instance" <file>`).

- [ ] **Step 6: Build + run the neighbors' tests**

Run: `make -C <worktree> -j && make -C <worktree> test && cd <worktree> && ./build/bin/helix-tests "[shutdown][power]" && ./build/bin/helix-tests "[change_host]" && ./build/bin/helix-tests "[spoolman]" && ./build/bin/helix-tests "[input_shaper]"`
Expected: PASS (these suites test the surrounding policy; the locality source swap is compilation-verified here and behavior-verified in Task 7's manual pass).

- [ ] **Step 7: Commit**

```bash
git -C <worktree> add src/ui/panel_widgets/shutdown_widget.cpp src/ui/ui_panel_input_shaper.cpp \
  src/ui/ui_spoolman_overlay.cpp src/ui/ui_change_host_modal.cpp
git -C <worktree> commit -m "refactor(ui): locality decisions read moonraker_is_remote

Shutdown fallback scope, input-shaper chart toast, spoolman local-config
fallback, and change-host error wording all follow the live-endpoint
verdict instead of the Config host, so a mid-session printer switch no
longer leaves stale locality behavior. Change-host keeps its Config
emptiness check (unconfigured must offer Change Address)."
```

---

### Task 7: Full verification

**Files:** none (verification only; fix-forward + amend-commit if anything surfaces)

- [ ] **Step 1: Check build processes, then full build + full test suite**

```bash
pgrep -f 'make|c\+\+' || true
make -C <worktree> -j; echo "build exit: $?"
make -C <worktree> test-run > /tmp/helix-test-run.log 2>&1; echo "test exit: $?"; \
  grep -cE "^[0-9]+ (passed|test)" /tmp/helix-test-run.log; grep -iE "failed|sigterm|sigsegv" /tmp/helix-test-run.log | head
```

Expected: both exits 0; no failures (grep for SIGTERM too — a parallel session's pkill reads as failures, don't chase a phantom regression).

- [ ] **Step 2: Lint gates (the ones the pre-commit hook will run)**

```bash
cd <worktree>
python3 scripts/check_imperative_ui.py; python3 scripts/check_l081_anti_pattern.py; python3 scripts/check_timer_destructor_cancel.py
```

Expected: imperative-UI count == 380 baseline (must not rise), others clean.

- [ ] **Step 3: End-to-end manual pass (positive + negative)**

Positive (remote + webcam): the Task 4 Step 7 ctl sequence — button visible, click opens fullscreen, close returns.
Negative (same-host): relaunch without `HELIX_MOCK_REMOTE_PRINTER` — button hidden on print status.
Screenshot both states for the record: `./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot /tmp/camera-btn-<state>.png`.
Kill every instance you spawned: `pgrep -xl helix-screen` and kill only yours.

- [ ] **Step 4: Self-review the diff**

```bash
git -C <worktree> log --oneline devel/1.1..HEAD
git -C <worktree> diff devel/1.1..HEAD --stat
```

Check against the spec: subject default/update-point/threading, the one-cond gate, cross-build callback registration, all five Tier A sites + two Tier B conversions, twins deleted, env var documented, no leftover `is_local_host` references (`grep -rn "is_local_host" <worktree>/src <worktree>/include` → only `host_identity`'s internal loopback check may match by different name — it doesn't; expected result: zero hits).

---

## Self-Review Notes (already applied)

- Spec coverage: truth model (Tasks 1-3), button (Task 4), Tier A sweep (Tasks 5-6), Tier B (Task 1), mock env + docs (Task 3), testing (per-task + Task 7). The spec's "ams_backend_ad5x_ifs" site needs NO change — verified it already reads Config + the canonical `is_moonraker_on_same_host()` (runs pre-discovery; predicate-based by design). The spec's printer_image_widget site turned out to gate only a never-displayed subject (`printer_host_text` has zero XML/C++ consumers — verified); converting it to the subject would wire dead behavior, so it is intentionally left out — flag this in the PR description as a sweep discovery rather than silently dropping it.
- Type consistency: `set_moonraker_is_remote(bool)` / `is_moonraker_remote()` / `get_moonraker_is_remote_subject()` used identically in Tasks 2-6; XML subject name `moonraker_is_remote` identical in Task 3's doc, Task 4's cond, and Task 2's INIT_SUBJECT_INT.
- No placeholders: every step carries the actual code or exact command.
