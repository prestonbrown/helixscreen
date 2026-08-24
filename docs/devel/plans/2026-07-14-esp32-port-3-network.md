# ESP32 Port Plan 3 — Network: Interface-First Re-architecture + ESP Client

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `IMoonrakerClient`/`IMoonrakerAPI` the complete, lint-enforced consumer contract for all Moonraker access (fixing every concrete-type bypass), then implement `IMoonrakerClient` on ESP-IDF over `esp_websocket_client` and prove it live against a real Moonraker from the K-Touch.

**Architecture:** Preston's decision (2026-07-14): **widen the interfaces — no half measures.** Consumers depend only on interfaces; concrete `MoonrakerClient` (libhv) and `MoonrakerAPI` become implementations owned by `MoonrakerManager`. `MoonrakerAPI` is decoupled from the concrete client (consumes `IMoonrakerClient&`) so its ~800 lines of portable JSON-RPC wrappers are reused verbatim on ESP32; only the WS transport is reimplemented. A lint gate prevents new concrete-type bypasses.

**Tech Stack:** C++17 main tree (libhv stays desktop transport), ESP-IDF v5.5 + managed component `espressif/esp_websocket_client ^1.6.1` (firmware), vendored nlohmann::json both sides.

## Global Constraints

- **Zero behavior change on desktop.** Every main-tree task is a pure refactor: same wire traffic, same callbacks, same threading. Full `make test-run` green before every commit.
- **Interfaces are the ONLY consumer surface** (Preston, 2026-07-14: "re-architect this RIGHT... if there are places that bypass the interfaces, we should fix them"). After Task 7, no file outside the allowlist (`moonraker_manager.*`, `moonraker_client.*`, `moonraker_api*.{h,cpp}`, sub-API impl files, mocks, drift tests) may name the concrete types — enforced by `tests/shell/test_code_lint.bats`.
- **No new third-party dependencies** in the main tree. Firmware adds exactly one managed component: `espressif/esp_websocket_client` pinned `^1.6.1`.
- **No changes under `firmware/native-audit/`** (frozen reference). New firmware code goes in `firmware/helixscreen-esp32/components/helixnet/` + `main/`.
- **`esp32-build` CI job must stay green** including the 5.8MB size gate (`scripts/check_esp32_size.py`).
- Threading rules are non-negotiable: ESP WS events fire on the websocket task → app callbacks marshal exactly like libhv's event-loop thread does today (consumers already `ui_queue_update()`; the ESP client must preserve "callbacks on a background task" semantics, never call back on the LVGL/UI thread directly).
- Commit prefixes: `refactor(net)` for interface/migration tasks, `feat(net)` for new seams, `feat(esp32-fw)` for firmware tasks.
- Check `pgrep -xc cc1plus` before builds; bound with `-j4` if another session is building.
- HIL evidence: serial capture via `firmware/native-audit/capture_serial.py` (IDF venv python, `sg dialout -c`, logs contain NULs → always `grep -a`). Flash: `sg dialout -c "python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 ..."`.

## Design decisions locked by research (do not re-litigate in-task)

1. **`ConnectionState` moves to its own header** — it is the one symbol that makes `observer_factory.h` pull `moonraker_client.h` → `hv/WebSocketClient.h` into ~149 TUs (consumers report §2).
2. **`MoonrakerAPI` consumes `IMoonrakerClient&`, not `helix::MoonrakerClient&`.** Its JSON-RPC surface is transport-agnostic; only `rest()`/`transfers()`/`timelapse()` touch HTTP and those are v1-gated on ESP32 (api report §5). ESP32 reuses concrete `MoonrakerAPI`.
3. **Sub-APIs get thin pure-virtual interfaces** (`IMotionAPI`, `IJobAPI`, `IFilesAPI`, …) and `IMoonrakerAPI` accessors return interface references. Concrete sub-API classes implement them; nothing else changes inside them.
4. **Mocks keep subclassing concretes** (they reuse concrete plumbing and the concretes implement the interfaces, so `MoonrakerClientMock` IS-an `IMoonrakerClient` transitively). Drift tests keep pinning mock-vs-interface compatibility.
5. **ESP client implements `IMoonrakerClient` from scratch** (Preston's pick): own bounded request tracker, `esp_websocket_client` transport, fragment reassembly with a hard cap. It does NOT port `MoonrakerDiscoverySequence` in this plan — discovery reuse/subsetting is a Plan 4 decision when PrinterState wiring happens (`discover_printer()` is a stub in Plan 3's HIL).
6. **TLS is out of scope for Plan 3.** Moonraker LAN is plain `ws://`/`http://` throughout (api report §7). The OTA-manifest pinned-CA decision belongs to the OTA plan (Plan 5).
7. **ESP WS buffer_size = 32768** (>16384 ⇒ auto-PSRAM under `SPIRAM_USE_MALLOC`, espidf report), reassembly cap 256KB (largest expected narrowed payload is `printer.objects.list` / `server.temperature_store`; desktop logs >50KB as "large", 5MB is the desktop guard). Oversize ⇒ drop message + error log, never OOM.

## File Structure (end state)

```
include/
├── connection_state.h            # NEW: ConnectionState enum (Task 1)
├── i_moonraker_client.h          # WIDENED: full consumer surface (Task 2)
├── i_moonraker_api.h             # WIDENED: core methods + sub-API accessors (Task 5)
├── i_moonraker_sub_apis.h        # NEW: IMotionAPI/IJobAPI/IFilesAPI/... (Task 4)
├── moonraker_client.h            # concrete; drops ConnectionState defn (Task 1); overrides (Task 2)
├── moonraker_api.h               # concrete; ctor takes IMoonrakerClient& (Task 6)
├── app_globals.h                 # getters return interface pointers (Task 3/5)
└── ui_subscription_guard.h       # takes IMoonrakerClient* (Task 3)
src/api/*.cpp                     # sub-APIs implement interfaces (Task 4/6)
src/application/moonraker_manager.cpp  # owns concretes, ESP factory hook (Task 8)
tests/shell/test_code_lint.bats   # concrete-type bypass lint (Task 7)
tests/unit/test_interface_drift_* # updated pins (Tasks 2/4/5)
firmware/helixscreen-esp32/
├── main/idf_component.yml        # + esp_websocket_client ^1.6.1 (Task 9)
├── components/helixnet/          # NEW: EspMoonrakerClient + request tracker (Task 9)
└── main/net_hil.c(pp)            # HIL: WiFi up + live Moonraker session (Task 10)
```

## Execution order & dependencies

Tasks 1→2→3 are strictly sequential (each builds on the previous header state). Task 4 depends on 1 only; Task 5 depends on 2+4; Task 6 depends on 5; Task 7 depends on 3+6 (lint goes in only after migrations are done); Task 8 depends on 6; Tasks 9-10 (firmware) depend on 2 (the widened `IMoonrakerClient` header) and are parallelizable with 4-8 in a separate worktree ONLY if the Task-2 header is final first.

---

### Task 1: Extract ConnectionState — break the libhv header cascade

**Files:**
- Create: `include/connection_state.h`
- Modify: `include/moonraker_client.h` (remove enum defn, include new header)
- Modify: `include/observer_factory.h:27` (swap include)
- Test: existing suite (pure move); compile-time proof step below

**Interfaces:**
- Produces: `helix::ConnectionState` in `connection_state.h` — consumed by Tasks 2/5 and by `observer_factory.h` without dragging libhv.

- [ ] **Step 1: Create the header**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {

/**
 * @brief Connection state for the Moonraker transport
 *
 * Lives in its own header so UI/observer code can bind to connection state
 * without including the concrete client (which drags libhv on desktop).
 */
enum class ConnectionState {
    DISCONNECTED, // Not connected
    CONNECTING,   // Connection in progress
    CONNECTED,    // Connected and ready
    RECONNECTING, // Automatic reconnection in progress
    FAILED        // Connection failed (max retries exceeded)
};

} // namespace helix
```

- [ ] **Step 2: Point moonraker_client.h at it**

In `include/moonraker_client.h`: delete the `enum class ConnectionState { ... };` block (the ~lines 82-92 definition inside `namespace helix`) and add `#include "connection_state.h"` to the include block at the top. No other change.

- [ ] **Step 3: Fix observer_factory.h**

```cpp
// before (observer_factory.h:27)
#include "moonraker_client.h" // ConnectionState
// after
#include "connection_state.h"
```

If `observer_factory.h` compiles cleanly it used nothing else from the client header. If it did use more, STOP and report — do not re-add the include.

- [ ] **Step 4: Sweep other headers including moonraker_client.h only for the enum**

```bash
grep -rn '#include "moonraker_client.h"' include/ src/ | grep -i "connectionstate\|// ConnectionState"
```
Swap any hit that only needs the enum to `connection_state.h`.

- [ ] **Step 5: Prove the cascade is cut**

```bash
echo '#include "observer_factory.h"
int main(){return 0;}' > /tmp/cascade_probe.cpp
g++ -std=c++17 -Iinclude -Ilib -Ilib/libhv/include -Isrc -E /tmp/cascade_probe.cpp | grep -c "WebSocketClient.h" || true
```
Expected: `0` (no libhv WebSocketClient header reached from observer_factory.h). Record the number in the commit message. (Use the project's real include flags from `make -n` output if the ad-hoc flags miss something.)

- [ ] **Step 6: Full test suite + commit**

```bash
make test-run
git add include/connection_state.h include/moonraker_client.h include/observer_factory.h
git commit -m "refactor(net): extract ConnectionState; observer_factory no longer drags libhv into ~149 TUs"
```

---

### Task 2: Widen IMoonrakerClient to the full consumer surface

**Files:**
- Modify: `include/i_moonraker_client.h`
- Modify: `include/moonraker_client.h` (mark lifted methods `override` / `virtual`)
- Modify: `tests/unit/test_interface_drift_moonraker_client.cpp`
- Test: `./build/bin/helix-tests "[compile][drift]"` + full suite

**Interfaces:**
- Produces the complete `helix::IMoonrakerClient` that Tasks 3, 8, 9 build on. Exact additions (signatures copied from the concrete class — keep parameter names/defaults identical):

```cpp
// --- Subscriptions & method callbacks (moonraker_client.h:183-219) ---
virtual SubscriptionId register_notify_update(std::function<void(const json&)> cb) = 0;
virtual bool unsubscribe_notify_update(SubscriptionId id) = 0;
virtual void register_method_callback(const std::string& method, const std::string& handler_name,
                                      std::function<void(const json&)> cb) = 0;
virtual bool unregister_method_callback(const std::string& method,
                                        const std::string& handler_name) = 0;

// --- Connection state & observers (415, 506-512, 170) ---
virtual ConnectionState get_connection_state() const = 0;
virtual void add_connected_observer(const std::string& handler_name, std::function<void()> cb) = 0;
virtual bool remove_connected_observer(const std::string& handler_name) = 0;
virtual void force_reconnect() = 0;

// --- Events & modal suppression (register_event_handler, suppress_disconnect_modal) ---
virtual void register_event_handler(MoonrakerEventCallback cb) = 0;
virtual void suppress_disconnect_modal(uint32_t duration_ms = 10000) = 0;
virtual bool is_disconnect_modal_suppressed() const = 0;

// --- Discovery data & cache (386, 148, 366) ---
virtual PrinterDiscovery hardware() const = 0;
virtual void parse_objects(const json& objects) = 0;
virtual void clear_discovery_cache() = 0;

// --- Request management (288) ---
virtual bool cancel_request(RequestId id) = 0;

// --- Owner wiring (444, 551, 575, 591, set_default_request_timeout) ---
virtual void set_state_change_callback(std::function<void(ConnectionState, ConnectionState)> cb) = 0;
virtual void set_connection_timeout(uint32_t timeout_ms) = 0;
virtual void set_default_request_timeout(uint32_t timeout_ms) = 0;
virtual void configure_timeouts(uint32_t connection_timeout_ms, uint32_t request_timeout_ms,
                                uint32_t keepalive_interval_ms, uint32_t reconnect_min_delay_ms,
                                uint32_t reconnect_max_delay_ms) = 0;
virtual void process_timeouts() = 0;

// --- Lifetime guard for SubscriptionGuard (624) ---
virtual std::weak_ptr<bool> lifetime_weak() const = 0;
```

New includes for `i_moonraker_client.h`: `connection_state.h`, `moonraker_events.h` (for `MoonrakerEventCallback`), `printer_discovery.h` (for `helix::PrinterDiscovery` — check the actual header name via `grep -rn "struct PrinterDiscovery\|class PrinterDiscovery" include/`), `<memory>`. Also move `SubscriptionId`/`INVALID_SUBSCRIPTION_ID` (currently defined in `moonraker_client.h:70-74`) into `i_moonraker_client.h` so the interface is self-contained, and delete them from `moonraker_client.h` (it gets them via inheritance include).

- [ ] **Step 1: Write the failing drift check** — extend `tests/unit/test_interface_drift_moonraker_client.cpp` with compile-time pins for each new method, e.g.:

```cpp
// Pin: interface exposes the full consumer surface (Plan 3 Task 2).
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::register_notify_update),
                             helix::SubscriptionId (helix::IMoonrakerClient::*)(
                                 std::function<void(const json&)>)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::hardware),
                             helix::PrinterDiscovery (helix::IMoonrakerClient::*)() const>);
// ...one static_assert per lifted method (same pattern; overloaded names use
// static_cast disambiguation as the existing send_jsonrpc pins in this file do)
```

- [ ] **Step 2: Run to verify failure** — `make test 2>&1 | tail -20` → compile errors: no such member on the interface.

- [ ] **Step 3: Widen the interface** — add the pure-virtual declarations above to `include/i_moonraker_client.h`, replace the header's "intentionally narrow — mirrors only the 10 methods" doc comment with one stating the interface IS the complete consumer contract (cite this plan). In `include/moonraker_client.h`, add `override` to every lifted method. Methods currently defined inline in the header (`set_connection_timeout`, `configure_timeouts`, `process_timeouts`, `cancel_request`, `clear_discovery_cache`, `parse_objects`, `hardware`, `get_connection_state`, `set_state_change_callback`, `set_default_request_timeout`, `lifetime_weak`, `toggle_filament_runout_simulation` [already override]) keep their inline bodies — just add `override`.

- [ ] **Step 4: Tests pass** — `make test-run` (drift + full suite; `MoonrakerClientMock` inherits the concrete so it needs no changes).

- [ ] **Step 5: Commit** — `git commit -m "refactor(net): widen IMoonrakerClient to the full consumer surface"`

---

### Task 3: Migrate every client consumer to IMoonrakerClient*

**Files:**
- Modify: `include/app_globals.h` + its impl (getter/setter types)
- Modify: `include/ui_subscription_guard.h` (ctor takes `IMoonrakerClient*`)
- Modify: every consumer file (inventory step below; research lists 25+: sensor_state, power_device_state, ams_backend_ad5x_ifs, application.cpp, gcode_error_router, gcode_narration_router, job_queue_state, ui_panel_console, ui_panel_print_select, ui_probe_overlay, moonraker_performance_source, print_history_manager, active_print_media_manager, plugin_api, ams_subscription_backend, print_start_collector, ui_wizard*, ui_change_host_modal, detection_manager, telemetry_manager, printer_hardware.h, ui_print_preparation_manager, helix_plugin_installer, all four `ams_backend_*` ctors' client param, moonraker_manager wiring…)
- Test: full suite

**Interfaces:**
- Consumes: Task 2's widened interface.
- Produces: `helix::MoonrakerClient` named ONLY in the allowlist files (Task 7 enforces).

- [ ] **Step 1: Generate the live inventory** (do not trust the list above blindly):

```bash
grep -rln "helix::MoonrakerClient\|MoonrakerClient\b" src/ include/ \
  | grep -v -E "moonraker_client|moonraker_manager|_mock|test_" | sort > /tmp/task3_inventory.txt
wc -l /tmp/task3_inventory.txt && cat /tmp/task3_inventory.txt
```

- [ ] **Step 2: Flip the global seam**

```cpp
// app_globals.h — before
helix::MoonrakerClient* get_moonraker_client();
void set_moonraker_client(helix::MoonrakerClient* client);
// after
helix::IMoonrakerClient* get_moonraker_client();
void set_moonraker_client(helix::IMoonrakerClient* client);
```
Forward-declare `helix::IMoonrakerClient` instead of the concrete. `MoonrakerManager` still holds `std::unique_ptr<helix::MoonrakerClient>` internally and passes `m_client.get()` — implicit upcast, no cast needed.

- [ ] **Step 3: SubscriptionGuard**

`include/ui_subscription_guard.h`: change the stored pointer + ctor param from `helix::MoonrakerClient*` to `helix::IMoonrakerClient*`; it only calls `lifetime_weak()` and `unsubscribe_notify_update()` — both on the interface after Task 2. Include `i_moonraker_client.h` instead of `moonraker_client.h`.

- [ ] **Step 4: Mechanical sweep**

For each file in the inventory: replace `helix::MoonrakerClient*` / `MoonrakerClient*` types (member vars, ctor params, locals) with `helix::IMoonrakerClient*`, and swap `#include "moonraker_client.h"` for `#include "i_moonraker_client.h"` (or `connection_state.h` where only the enum is used). AMS backend ctors (`ams_backend_{ace,qidi,cfs,ad5x_ifs}.cpp/.h`) change their `client` param type; their call bodies (`send_jsonrpc`, `register_method_callback`) are already interface-covered. `MoonrakerManager` keeps the concrete type for construction/ownership only — its `register_callbacks()`/wiring calls now go through the interface methods it already uses.

- [ ] **Step 5: Verify zero stragglers + tests**

```bash
grep -rln "helix::MoonrakerClient" src/ include/ \
  | grep -v -E "moonraker_client|moonraker_manager|_mock|test_"   # expect: empty
make test-run
```

- [ ] **Step 6: Commit** — `git commit -m "refactor(net): all client consumers depend on IMoonrakerClient"`


---

### Task 4: Sub-API interfaces

**Files:**
- Create: `include/i_moonraker_sub_apis.h`
- Modify: `include/moonraker_{advanced,file_transfer,history,job,timelapse,motion,rest,spoolman,file,queue}_api.h` (each class additionally inherits its interface; public methods gain `override`)
- Modify: `tests/unit/test_interface_drift_moonraker_api.cpp` (pins)
- Test: `[compile][drift]` + full suite

**Interfaces:**
- Produces: `IMotionAPI`, `IJobAPI`, `IFilesAPI`, `IQueueAPI`, `IHistoryAPI`, `IAdvancedAPI`, `IRestAPI`, `ITransfersAPI`, `ISpoolmanAPI`, `ITimelapseAPI` — one pure-virtual interface per existing sub-API class, mirroring that class's full public method list verbatim (same names, params, defaults).
- Consumed by Task 5's `IMoonrakerAPI` accessors and by every migrated call site.

- [ ] **Step 1: Generate each interface from the concrete header.** For each sub-API class: copy every public method declaration, make it pure virtual, keep doc comments' @brief line. Example shape (repeat per class — the implementer must transcribe from the REAL headers, not invent):

```cpp
// i_moonraker_sub_apis.h (one header, ten interfaces — they are small)
class IMotionAPI {
  public:
    virtual ~IMotionAPI() = default;
    // every public method of MoonrakerMotionAPI (include/moonraker_motion_api.h),
    // verbatim signatures, = 0
};
// ... IJobAPI (moonraker_job_api.h), IFilesAPI (moonraker_file_api.h), etc.
```
Rules: methods that return internal references/concrete helpers stay OUT only if no consumer outside the api layer calls them (verify with grep; if a consumer calls it, it goes on the interface). Static helpers stay on the concrete class.

- [ ] **Step 2: Failing drift pins** — add to `test_interface_drift_moonraker_api.cpp`: `static_assert(std::is_base_of_v<IMotionAPI, MoonrakerMotionAPI>);` (one per pair) plus non-abstractness checks mirroring the existing file's pattern. Run: compile fails (interfaces don't exist yet / classes don't inherit).

- [ ] **Step 3: Implement** — write the interfaces; each concrete sub-API class inherits (`class MoonrakerMotionAPI : public IMotionAPI`), add `override`. Zero body changes.

- [ ] **Step 4: Tests pass** — `make test-run`.

- [ ] **Step 5: Commit** — `git commit -m "refactor(api): pure-virtual interfaces for all ten Moonraker sub-APIs"`

---

### Task 5: Widen IMoonrakerAPI + migrate API consumers

**Files:**
- Modify: `include/i_moonraker_api.h` (core methods + sub-API accessors)
- Modify: `include/moonraker_api.h` (`override` on lifted methods)
- Modify: `include/app_globals.h` (`get_moonraker_api()` returns `IMoonrakerAPI*`)
- Modify: every API consumer (inventory step; research names: TemperatureController, ControlsPanel, ui_panel_console, MacroModificationManager, macro_executor, standard_macros, PrintSelectPanel, JobQueueState, PrintHistoryManager, EmergencyStopOverlay, AbortManager, all `ams_backend_*` api params, SubjectInitializer::init_panels, SpoolmanManager, plus everything the Step-1 inventory finds)
- Test: full suite

**Interfaces:**
- Consumes: Task 2 (`IMoonrakerClient`), Task 4 (sub-API interfaces).
- Produces the complete `IMoonrakerAPI`. Additions (signatures verbatim from `include/moonraker_api.h`; line refs from research):

```cpp
// Core printer control (moonraker_api.h:121-349)
virtual void set_temperature(...) = 0;              // :121
virtual void set_fan_speed(...) = 0;                // :132
virtual void set_led(...) = 0; set_led_on/off       // :149-172
virtual void execute_gcode(...) = 0;                // :229
virtual void exclude_object(...) = 0;               // :258
virtual void emergency_stop(...) = 0;               // :267
virtual void restart_firmware/klipper/service/moonraker(...) = 0;  // :275-315
virtual void machine_shutdown/machine_reboot(...) = 0;             // :322-329
virtual bool is_printer_ready() const = 0;          // :341
virtual std::string get_print_state() const = 0;    // :349
// Safety limits + config (363-407)
virtual ... set_safety_limits/get_safety_limits/update_safety_limits_from_printer = 0;
virtual void query_configfile(...) = 0;
// HTTP base URL accessors (421-428) — needed by thumbnail/webcam consumers
virtual void set_http_base_url(...) = 0;  get_http_base_url / ensure_http_base_url / resolve_webcam_url
virtual IMoonrakerClient* get_client() = 0;         // returns interface, NOT concrete
// Sub-API accessors — interface references:
virtual IAdvancedAPI& advanced() = 0;    virtual ITransfersAPI& transfers() = 0;
virtual IHistoryAPI& history() = 0;      virtual IJobAPI& job() = 0;
virtual ITimelapseAPI& timelapse() = 0;  virtual IMotionAPI& motion() = 0;
virtual IRestAPI& rest() = 0;            virtual ISpoolmanAPI& spoolman() = 0;
virtual IFilesAPI& files() = 0;          virtual IQueueAPI& queue() = 0;
```
The implementer transcribes EXACT signatures (params, callbacks, defaults) from `moonraker_api.h` — the sketch above shows scope, not final code. Concrete accessors (`moonraker_api.h:652-774`) already return concrete refs; changing their declared return type to the interface ref on `IMoonrakerAPI` while the concrete override keeps returning the concrete type is valid C++ ONLY via covariant returns on references — which C++ does not support. Therefore: the concrete `MoonrakerAPI::motion()` changes its return type to `IMotionAPI&` (body unchanged — implicit upcast). Callers use interface methods only (guaranteed by Task 4's verbatim mirroring).

- [ ] **Step 1: Inventory** — `grep -rln "MoonrakerAPI\b" src/ include/ | grep -v -E "moonraker_api|moonraker_manager|_mock|test_" | sort > /tmp/task5_inventory.txt && wc -l /tmp/task5_inventory.txt`
- [ ] **Step 2: Failing drift pins** for the lifted methods + accessor return types (same static_assert pattern as Task 2).
- [ ] **Step 3: Widen interface, add overrides, flip accessor return types.**
- [ ] **Step 4: Migrate consumers** — `MoonrakerAPI*` → `IMoonrakerAPI*` (members, ctor params, `app_globals`), `#include "moonraker_api.h"` → `#include "i_moonraker_api.h"`. AMS backend ctors take `(IMoonrakerAPI* api, helix::IMoonrakerClient* client)`.
- [ ] **Step 5: Verify + tests** — straggler grep (expect empty outside allowlist), `make test-run`.
- [ ] **Step 6: Commit** — `git commit -m "refactor(api): IMoonrakerAPI is the complete consumer contract; all consumers migrated"`

---

### Task 6: Decouple MoonrakerAPI (and sub-APIs) from the concrete client

**Files:**
- Modify: `include/moonraker_api.h:106` ctor: `MoonrakerAPI(helix::IMoonrakerClient& client, helix::PrinterState& state)`; member `client_` type flips to `IMoonrakerClient&`
- Modify: each sub-API header/impl whose ctor takes `helix::MoonrakerClient&` (e.g. `moonraker_rest_api.h:47`) → `IMoonrakerClient&`
- Modify: `src/application/moonraker_manager.cpp:522` (passes `*m_client` — implicit upcast, no change needed; verify)
- Modify: `tests/` fixtures that construct `MoonrakerAPI` with a concrete client (XMLTestFixture et al. — compile will find them)
- Test: full suite

**Steps:** (1) flip the ctor + member types; (2) chase compile errors — every error is a sub-API or fixture still demanding the concrete; flip each; any call into a client method NOT on `IMoonrakerClient` is a Task-2 omission: add it to the interface (with drift pin) rather than keeping a concrete reference; (3) `make test-run`; (4) commit `refactor(api): MoonrakerAPI consumes IMoonrakerClient — transport-agnostic`.

---

### Task 7: Lint gate + docs

**Files:**
- Modify: `tests/shell/test_code_lint.bats` (new test)
- Modify: `CLAUDE.md` ("Mock-facing interfaces" paragraph — now stale) + `docs/devel/MOONRAKER_ARCHITECTURE.md`
- Test: `bats tests/shell/test_code_lint.bats`

- [ ] **Step 1: The lint** (mirror the existing TemperatureController lint's structure in the same file):

```bash
@test "no concrete Moonraker types outside the network layer (Plan 3: interfaces are the consumer contract)" {
  local allowlist='moonraker_client|moonraker_manager|moonraker_api|moonraker_rest_api|moonraker_file_api|moonraker_file_transfer_api|moonraker_advanced_api|moonraker_history_api|moonraker_job_api|moonraker_motion_api|moonraker_queue_api|moonraker_spoolman_api|moonraker_timelapse_api|moonraker_request_tracker|moonraker_discovery_sequence|_mock|test_'
  run bash -c "grep -rln 'helix::MoonrakerClient\|\bMoonrakerAPI\b' src/ include/ | grep -v -E \"$allowlist\""
  [ -z \"$output\" ]
}
```
(Adjust the grep to the file's established helpers; the intent is: naming the concrete types outside the allowlist fails CI.)

- [ ] **Step 2: Update CLAUDE.md** — replace the "Callers continue to use the concrete types — interfaces exist to enforce mock-parity, not to drive call-site migration" sentence with the new contract: consumers depend on `IMoonrakerClient`/`IMoonrakerAPI`/sub-API interfaces ONLY; concretes live behind `MoonrakerManager`; lint-enforced. Update `MOONRAKER_ARCHITECTURE.md`'s class diagram section the same way.

- [ ] **Step 3: Run lint + full suite; commit** — `git commit -m "refactor(net): lint-enforce interface-only Moonraker access + doc updates"`

---

### Task 8: Platform factory hook for the client

**Files:**
- Modify: `include/i_moonraker_client.h` (factory declaration at bottom, wifi_backend pattern)
- Modify: `src/application/moonraker_manager.cpp` `create_client()`
- Test: full suite (desktop path unchanged)

- [ ] **Step 1:** Declaration:

```cpp
namespace helix {
/**
 * Platform-provided client factory for embedded targets. NOT defined in the
 * desktop build — the ESP32 firmware tree implements it over
 * esp_websocket_client (MoonrakerManager calls it when ESP_PLATFORM is defined).
 */
std::unique_ptr<IMoonrakerClient> create_platform_moonraker_client();
} // namespace helix
```

- [ ] **Step 2:** In `MoonrakerManager::create_client()` (moonraker_manager.cpp:~356), around the existing construction:

```cpp
#if defined(ESP_PLATFORM)
    auto owned = helix::create_platform_moonraker_client();
    // m_client's type must widen to std::unique_ptr<helix::IMoonrakerClient> for this arm;
    // desktop keeps constructing the concrete into the same unique_ptr<IMoonrakerClient>.
#else
    ... existing construction ...
#endif
```
Flip `MoonrakerManager::m_client` to `std::unique_ptr<helix::IMoonrakerClient>` (moonraker_manager.h:304). Desktop assigns `std::make_unique<MoonrakerClient>()` (upcast on assignment). Any manager call on a concrete-only method is, again, a Task-2 omission → lift it. Mock arm unchanged (mock IS-an interface).

- [ ] **Step 3:** `make test-run`; commit `feat(net): platform factory hook for IMoonrakerClient behind ESP_PLATFORM`.

---

### Task 9: helixnet — EspMoonrakerClient over esp_websocket_client

**Files:**
- Create: `firmware/helixscreen-esp32/components/helixnet/CMakeLists.txt`
- Create: `firmware/helixscreen-esp32/components/helixnet/idf_component.yml` (`espressif/esp_websocket_client: "^1.6.1"`)
- Create: `firmware/helixscreen-esp32/components/helixnet/esp_moonraker_client.h` / `.cpp`
- Test: `idf.py build` (cross-compile) — behavior verified on-device in Task 10

**Interfaces:**
- Consumes: the widened `helix::IMoonrakerClient` (Task 2 header, via the helixcore component's include path into the repo tree).
- Produces: `class helix::EspMoonrakerClient final : public IMoonrakerClient` + `create_platform_moonraker_client()` definition.

**Implementation contract (complete design; implementer writes the code to this spec):**

- **Transport:** one `esp_websocket_client_handle_t`; config: `buffer_size=32768`, `task_stack=8192` (4096 default is too small once nlohmann is on the callback path), `network_timeout_ms=10000`, `ping_interval_sec=10`, plain `ws://` only (reject `wss://` with an error log in v1). Enable `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` in sdkconfig.defaults — without it a send can block 10s+ behind an in-progress receive (esp-idf#14918), and our traffic is bidirectional (RPCs out while status streams in).
- **Reconnect backoff (manual):** the component's auto-reconnect is FIXED-interval (10s) with no exponential backoff. Mirror desktop (200ms→2000ms, ×2): on each `WEBSOCKET_EVENT_DISCONNECTED` call `esp_websocket_client_set_reconnect_timeout(next_delay)` (documented safe from the handler) and double `next_delay` up to 2000ms; reset to 200ms on `WEBSOCKET_EVENT_CONNECTED`. Desktop's app-level `max_reconnect_attempts_` is dead code (never configured; libhv retries forever) — ESP intentionally retries forever too, with the 60s RECONNECTING→FAILED state transition purely informational (state callback fires; reconnect keeps going).
- **Fragment reassembly:** on `WEBSOCKET_EVENT_DATA`, if `payload_offset==0` reserve `min(payload_len, 262144)`; append chunks; if `payload_len>262144` set a skip-flag, log `E helixnet: dropping %zu-byte message (cap 256KB)`, ignore until the final fragment. Complete message → dispatch.
- **Dispatch (on the websocket task — this IS the "background thread" contract consumers expect):** parse with `json::parse(buf, nullptr, false)`; `is_discarded()` → log + drop. If `id` present → request tracker; else if `method` present → method/notify callback maps.
- **Request tracker (own, bounded):** `std::map<uint64_t, Pending>` capped at 64 (not desktop's 500); monotonic id from `std::atomic<uint64_t>`; on cap: invoke error_cb synchronously with a `MoonrakerErrorType`-appropriate error (mirror desktop semantics from `moonraker_request_tracker.cpp`). Timeouts: `process_timeouts()` walks the map (called from a 5s `esp_timer` the client owns); default timeout 60s. Two-phase locking exactly like desktop: copy callback out under mutex, invoke outside.
- **Callback maps:** `notify_callbacks_` (SubscriptionId→cb) + `method_callbacks_` (method→handler→cb), plain `std::mutex`, same copy-then-invoke discipline.
- **Interface coverage:** every Task-2 pure-virtual gets a real or documented-stub implementation: `hardware()` returns a default-constructed `PrinterDiscovery` (Plan 4 fills this), `parse_objects`/`clear_discovery_cache` no-op with a `spdlog::debug` (esp_log via shim), `discover_printer()` invokes `on_complete` immediately after a `server.info` round-trip (real discovery = Plan 4), `register_event_handler`/`suppress_disconnect_modal` fully functional (trivial state), `lifetime_weak()` backed by a `shared_ptr<bool>` member exactly like desktop, `force_reconnect()` = `esp_websocket_client_stop()` + `start()`.
- **Connection state:** mirror desktop transitions (CONNECTING on connect(), CONNECTED on `WEBSOCKET_EVENT_CONNECTED` + fire on_connected + connected-observers, RECONNECTING on `WEBSOCKET_EVENT_DISCONNECTED` while auto-reconnect armed, FAILED after 60s of RECONNECTING via the 5s timer). `set_state_change_callback` fired on every transition.
- **Memory:** no allocation in the hot notify path beyond the json parse itself; reassembly buffer reused across messages (grow-only up to cap, `shrink_to_fit` on disconnect). Note: Moonraker does NOT chunk at the protocol level — a large response is ONE WS text message, so a >256KB response is dropped whole and its RPC will time out; v1 must simply never issue unnarrowed queries that big (subscriptions are narrowed; `objects.list` on complex printers is the case to watch — log the observed max in HIL).
- **Destruction order (safety-critical, desktop precedent):** dtor flips an `alive` atomic, calls `esp_websocket_client_stop()` (blocks until the WS task exits, draining in-flight events), THEN frees callback maps/tracker. Never free maps while the task can still dispatch.

- [ ] Steps: (1) component skeleton + idf_component.yml, `idf.py build` green with an empty class; (2) implement per contract; (3) `idf.py build` green + `scripts/check_esp32_size.py` under budget; (4) commit `feat(esp32-fw): helixnet EspMoonrakerClient over esp_websocket_client`.

---

### Task 10: HIL — live Moonraker session from the K-Touch

**Files:**
- Create: `firmware/helixscreen-esp32/main/net_hil.cpp` (+ hook into app_main behind `CONFIG_HELIX_NET_HIL`)
- Create: `firmware/helixscreen-esp32/main/Kconfig.projbuild` additions: `HELIX_NET_HIL` (bool, default n), `HELIX_HIL_WIFI_SSID` (default ""), `HELIX_HIL_WIFI_PASS` (default ""), `HELIX_HIL_MOONRAKER_URL` (string, default `ws://192.168.1.112:7125/websocket` — Preston's Voron V2, chosen 2026-07-14). **Credentials are NEVER committed** (public repo): SSID/pass defaults stay empty; the build injects real values via a git-ignored `sdkconfig.local` (add to .gitignore) or interactive menuconfig. The controller holds the real values and supplies them at flash time.
- Test: on-device, serial evidence

**HIL scenario (single pthread, 32KB stack, started after display init so the hello card stays up):**
1. Bring up WiFi station with the Kconfig credentials (raw `esp_wifi` init in this test file — the real `WifiBackend` impl is Plan 4's; keep this self-contained and clearly marked test-only).
2. Construct the client via `helix::create_platform_moonraker_client()`.
3. `connect(CONFIG_HELIX_HIL_MOONRAKER_URL, on_connected, on_disconnected)`.
4. On connected: `send_jsonrpc("server.info", {}, cb)` → log klippy_state + moonraker version.
5. `send_jsonrpc("printer.objects.subscribe", {"objects": {"extruder": ["temperature"], "heater_bed": ["temperature"]}}, cb)` then `register_notify_update` logging each temp update: `I net_hil: extruder=%.1f bed=%.1f`.
6. Every 15s: `send_jsonrpc("printer.info")` round-trip latency log.
7. TX-during-RX probe: at t=30s fire 5 back-to-back `printer.info` RPCs while the temp subscription is streaming; log each round-trip latency. Any latency >2s indicates the TX-lock contention class (esp-idf#14918) — fail the scenario.
8. After 60s: log `I net_hil: PASS msgs=%u drops=%u max_msg=%u heap_free=%u psram_free=%u` + heap-flat check (free-size counters ONLY — no heap walks, display is live).

**Pass criteria (Preston confirms from serial):** connects to the desk Moonraker, server.info logged, ≥30 temp notifications in 60s, zero drops, heap flat (±8KB), UI still rendering (no flicker — the audit's ISR-starvation trap).

- [ ] Steps: (1) write scenario + Kconfig; (2) build + flash (`sg dialout`, 460800, background); (3) capture 90s serial @115200; (4) `grep -a` evidence + hand Preston the exact lines to expect; (5) commit `feat(esp32-fw): network HIL scenario — live Moonraker session` after his confirmation.

---

## Definition of done (Plan 3)

- `observer_factory.h` compiles without reaching any libhv header (Task 1 probe = 0).
- `IMoonrakerClient`/`IMoonrakerAPI` + ten sub-API interfaces cover every consumer call; drift tests pin all of it.
- No file outside the allowlist names a concrete Moonraker type — lint enforced in CI.
- Desktop behavior byte-identical: full suite green after every task; no wire-protocol or threading changes.
- `MoonrakerAPI` constructible from any `IMoonrakerClient` (proven by the ESP arm compiling and by existing mock tests).
- `esp32-build` CI green with helixnet + esp_websocket_client; image under the 5.8MB gate.
- K-Touch HIL: live Moonraker session with temp streaming, 60s heap-flat, user-confirmed.

## Deferred to Plan 4 (recorded so nothing silently drops)

- Real ESP discovery (`MoonrakerDiscoverySequence` reuse-vs-subset decision) + `hardware()` population.
- `MoonrakerAPI` instantiation on ESP32 (needs PrinterState port) + HTTP sub-APIs over `esp_http_client` (thumbnails via in-memory `download_file_partial` path; `download_file_to_path` call sites redirect).
- Real `WifiBackend` ESP implementation (Plan 2's `create_platform_wifi_backend`).
- AMS vendor-backend HTTP polling decision, now precisely scoped (read-verified transport split): AFC/HappyHare/Toolchanger/CFS/Snapmaker = JSON-RPC only (zero HTTP); QIDI = one-shot config download at startup; ACE = 500ms REST-polling fallback mode; AD5X-IFS = 5s whole-file HTTP poll. v1 AMS-on-ESP32 can ship the JSON-RPC-only five cheaply; ACE-REST/AD5X need explicit budgeting or gating.
- Capped print-select file list is NEW design work (no existing cap constant; listing uses `files().get_directory`, viewport-lazy `fetch_metadata_range` is the paging prior art).
- ESP `HttpExecutor` lane-equivalent (dedicated task + queue).
