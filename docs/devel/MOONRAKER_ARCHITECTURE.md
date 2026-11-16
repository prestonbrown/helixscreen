# Moonraker Layer Architecture

This document describes the architecture of the Moonraker integration layer.

## Overview

The Moonraker integration is split into three distinct layers with clean separation of concerns:

```
┌─────────────────────────────────────────────────────────────────┐
│                         UI Panels                                │
│  (BedMeshPanel, Wizards, etc.)                                  │
└─────────────────────────────┬───────────────────────────────────┘
                              │ uses
┌─────────────────────────────▼───────────────────────────────────┐
│                      MoonrakerAPI                                │
│  (Domain Logic Layer)                                           │
│  ├─ PrinterDiscovery (SINGLE SOURCE OF TRUTH)           │
│  │   ├─ heaters, fans, sensors, leds                            │
│  │   ├─ macros, hostname, printer_info                          │
│  │   └─ capability queries (has_qgl, has_probe, etc.)           │
│  ├─ Bed mesh data (owned by the advanced() sub-API)            │
│  ├─ Hardware guessing (via PrinterHardware, printer_hardware.h) │
│  ├─ G-code execution (execute_gcode)                            │
│  ├─ Temperature control (set_temperature)                       │
│  └─ Object exclusion (get_excluded_objects, get_available_objects)│
└─────────────────────────────┬───────────────────────────────────┘
                              │ uses (JSON-RPC calls)
                              │ receives (discovery callbacks)
┌─────────────────────────────▼───────────────────────────────────┐
│                    MoonrakerClient                               │
│  (Transport Layer)                                               │
│  ├─ WebSocket connection/reconnection                           │
│  ├─ JSON-RPC 2.0 request/response handling                      │
│  ├─ Event emission (CONNECTION_FAILED, KLIPPY_DISCONNECTED...)  │
│  ├─ Printer state subscriptions                                 │
│  └─ Dispatches discovery data via callbacks (NO storage)        │
└─────────────────────────────────────────────────────────────────┘
```

**Key Architectural Principle:** MoonrakerClient is pure transport. It does NOT store hardware data. All discovered hardware information flows via callbacks to MoonrakerAPI, which owns the `PrinterDiscovery` instance as the single source of truth.

**Abstraction boundary:** UI code talks only to the API layer, never the transport layer directly. The API provides proxy methods for connection state (`get_connection_state()`, `get_websocket_url()`), subscriptions (`subscribe_notifications()`, `register_method_callback()`), database operations (`database_get_item()`, `database_post_item()`), `suppress_disconnect_modal()` and plugin RPCs. Shared data types (`BedMeshProfile`, `GcodeStoreEntry`) live in `moonraker_types.h`.

**Interfaces are the consumer contract:** `IMoonrakerAPI` (`include/i_moonraker_api.h`), `helix::IMoonrakerClient` (`include/i_moonraker_client.h`), and the ten sub-API interfaces in `include/i_moonraker_sub_apis.h` are what every consumer depends on, not the concrete classes, so the network layer can be swapped out (the ESP32 port's non-libhv client) behind the same interfaces. The concretes (`MoonrakerAPI`, `helix::MoonrakerClient`, the ten `Moonraker*API` sub-classes) live behind `MoonrakerManager` (`include/moonraker_manager.h`), which owns them via `std::unique_ptr<MoonrakerAPI>` (the concrete façade - the mock inherits it) / `std::unique_ptr<helix::IMoonrakerClient>` and constructs them in `create_api()` / `create_client()`. Mocks still inherit the concretes. Drift protection: `tests/unit/test_interface_drift_moonraker_*.cpp`. Lint-enforced: `scripts/code_lint.sh` fails CI if a concrete type is named outside the network layer.

## Layer Responsibilities

### MoonrakerClient (Transport Layer)

**Location:** `include/moonraker_client.h`, `src/api/moonraker_client.cpp` (+ mock split files)

**Responsibilities:**
- WebSocket connection management (connect, disconnect, reconnect)
- JSON-RPC 2.0 protocol handling
- Request timeout management
- Event emission for transport events
- Printer state subscriptions (`register_notify_update()`)
- **Dispatches discovery data via callbacks** (heaters, fans, sensors, LEDs, macros, hostname, printer info, bed mesh)

**Does NOT do:**
- UI notifications (replaced with event emission)
- Business logic decisions
- Hardware "guessing" logic
- **Store hardware discovery data** (moved to MoonrakerAPI)

**Broadcast notifications must be filtered by the consumer, at the edge.** The client fans a method callback out to every registered handler; it does not know which of them cares. `notify_filelist_changed` is the one that bites: Moonraker fires it for *every* registered root, and printers write to `config` constantly - an AFC unit rewrites `AFC/AFC.var.unit` on every `SET_*` command, a `SAVE_VARIABLE` `delayed_gcode` rewrites `saved_variables.cfg`. On bundle L53W5PKG that was one notification per ~10 s for a whole print. Two consumers, two different filters, both required:

- `PrintSelectPanel` lists the `gcodes` root only (`PrintSelectFileProvider` hardcodes it), so it filters on the roots an operation touches - `include/json_utils.h#filelist_change_affects_gcodes` takes both `item.root` and `source_item.root` and counts *either* end as `gcodes`. `item` describes the destination and the origin rides in `source_item`, so a file moved out of `gcodes` reports a foreign item root; both arguments are required, because looking at the item root alone is exactly the case that leaves the listing naming a file that is gone. Without the filter every config write cost a full `server.files.get_directory` round trip plus a list rebuild, 113 of them in that one session, while the user was on the print-status panel.
- `PrintHistoryManager` cares about files disappearing, so it filters on *action first, then root* - `filelist_change_affects_history()` admits only `delete_file`/`delete_dir`/`move_file`/`move_dir`, and of those only the ones touching `gcodes`. The root half matters as much as the action half: Moonraker's timelapse component moves frame and render artifacts throughout a print, and an unfiltered `move_file` costs a full 500-job refetch, ~1 s of round-trip on a Pi 4. A move counts if *either* end is in `gcodes`; a payload with no `item.root` at all invalidates, because going stale is worse than one extra round-trip.

A new consumer of this notification needs to state which axis it filters on before it registers. Filtering the log line alone is not the fix; the work behind it is the cost.

### MoonrakerAPI (Domain Logic Layer)

**Location:** `include/moonraker_api.h`, `src/api/moonraker_api.cpp`

**Responsibilities:**
- **Owns `PrinterDiscovery`** - single source of truth for all hardware info
- **Owns bed mesh data** - in the `advanced()` sub-API (`MoonrakerAdvancedAPI`: `get_active_bed_mesh()`, `get_bed_mesh_profiles()`, `has_bed_mesh()`)
- Hardware guessing via `PrinterHardware` (`include/printer_hardware.h`) - `guess_bed_heater()`, `guess_hotend_sensor()`, etc. are methods of a guesser constructed from the discovery lists
- G-code command execution
- Temperature control
- Object exclusion state queries
- Print control (pause, resume, cancel)

**`execute_gcode()`'s three dispositions:** `on_success`, `on_error`, and `on_queued` (see `include/moonraker_api.h`). `on_queued` is not decoration - when a script is discretionary (see `include/gcode_classify.h`) and an external blocking op (`G28`, `BED_MESH_CALIBRATE`, a manual probe) holds Klipper's gcode lock, `execute_gcode()` queues the command fire-and-forget and DROPS its RPC response: neither `on_success` nor `on_error` will ever fire. `on_queued` means "accepted for later execution", never "the printer did it" - a caller that needs to know the command actually ran must not treat it as completion, only as a signal to release a caller-side in-flight counter.

**`on_error` is not a claim to report.** Supplying an error callback says the caller wants to *know*; it does not say the user will be *told*. The trailing `caller_surfaces_errors` parameter (default `true`) is the claim, and it must be captured from the caller's own `on_error` **before** any internal wrapper - `execute_gcode()` wraps unconditionally to settle its activity counter, so intent derived afterwards reads our own bookkeeping as a caller promise. Pass `false` when the callback only logs or only resets state: a false claim makes the request tracker record the rejection for cross-channel dedup, `GcodeErrorRouter` then suppresses its report of Klipper's `!!` broadcast, and the failure reaches nobody. The `silent` parameter is a separate axis - it means "no automatic toast from us", never "the user has been told", so it neither records for dedup nor stops the `!!` router. `scripts/check_gcode_error_ownership.py` gates the log-only case at zero. Full contract and the decision matrix: `RPC_ERROR_OWNERSHIP.md`.

**What counts as "an external blocking op" is debounced, not the raw flag.** `idle_timeout.state` reads `"Printing"` while *any* gcode executes, so a printer with housekeeping `delayed_gcode` loops reports it in short bursts forever while sitting idle - bundle L53W5PKG (a Voron Trident running `bedfanloop`, `_AIR_FILTER_TIMER` and AFC `PREP`) logged 632 transitions, the flag high for ~0.7 s out of every 10 s. Each burst made the gate refuse jogs with "Printer is busy - try again in a moment" on an idle printer, roughly 7% of that machine's idle life. `IdleTimeoutBusy` (`include/idle_timeout_busy.h`) requires the flag to hold for `kSettle` (1 s) before `is_blocking_operation_active()` reports it; the real ops the gate exists for hold it for many seconds. Three consequences worth knowing:

- **Only the rising edge is debounced.** Clearing takes effect immediately - there is no false negative to protect against on that edge, and making the user wait a second after a homing completes would be its own bug.
- **A repeated `"Printing"` report does not re-arm the window.** Restarting the settle on every status batch would let a printer that re-sends faster than `kSettle` never reach the blocking state at all, turning the debounce into a hole in the gate rather than a filter.
- **It must be cleared wherever the subject is.** `idle_timeout` is a delta-only field, so `PrinterCalibrationState::reset_klippy_volatile()` resets it on a Klippy transition (#1129). The debounce caches the same field but is not a subject, so it is cleared there explicitly - miss that and the #1129 wedge returns one layer down, invisible to the subject-level tests.

The raw `get_idle_timeout_printing_subject()` remains the literal Klipper state for display; gates read `idle_timeout_busy()`.

**The busy toast speaks only for an episode the app did not begin.** When a discretionary command goes out during a blocking episode, `execute_gcode()` shows "Printer is busy - your ... will run when it's ready." at most once per episode (`PrinterCalibrationState::claim_busy_queue_toast()`). It stays quiet when the episode is the app's own, because any gcode reads `"Printing"` while it runs: light taps 0.2 s apart each hold the flag for most of a second, enough to keep a settled episode up on an idle printer for as long as the burst lasts. Ownership is decided by `BusyEpisodeOwner` (`include/busy_episode_owner.h`) through `PrinterState::is_busy_episode_app_initiated()`:

- **Every non-silent `execute_gcode()` send is stamped**, before it goes out, into `PrinterState::app_command_episode()`. Silent sends are the app's own housekeeping (`LedAutoState` reacting to a state change) and can follow an external op's start closely, so they never stamp.
- **An episode is the app's when a stamped send falls in the `kGraceWindow` (2 s) before its start.** The start is when the main thread saw `idle_timeout` go high (`IdleTimeoutBusy::printing_since()`), so a send after it, even by a fraction of a second, is joining someone else's operation and never adopts it. The last `kHistory` (16) send times are kept, because the first ask comes about `kSettle` into the episode after more sends have landed.
- **Ownership is decided once per episode**, on the first ask that can settle it, and lapses `kMaxSelfBusy` (30 s) after the latest activity (an ack or an untracked send), which bounds how long an op that starts mid-burst can pass as the app's. Sends tracked with `note_sent()`/`note_done()` are a second witness: one stuck without an ack for `kGraceWindow` disowns the episode, a prompt ack adopts it. The header comment in `include/busy_episode_owner.h` has the full rule.

This is toast attribution only. The blocking-op predicates and the motion refusal never read `app_command_episode()`, so a light tap cannot let a jog through during an external bed mesh or filament op (#1108). Motion has its own owner under the same rule: `AppMotionActivity` keeps a `BusyEpisodeOwner` fed from motion sends alone, and `PrinterState::is_external_blocking_operation_active()` treats an episode those sends began as self-inflicted, so one jog does not refuse the next. It also passes a jog while the previous jog is in flight or acked within the last 2s (`AppMotionActivity::recently_active`). A manual probe session blocks regardless.

Before adding a command to `detail::categorize_gcode_token()` in `gcode_classify.h`, verify every caller that emits it either holds no in-flight counter or passes `on_queued`. Skipping this check is exactly how #1129 happened - a cached `idle_timeout_printing` made the app treat an idle printer as busy, routed a `SET_LED` down the fire-and-forget path, and left the LED in-flight counter pinned for the session. `SET_GCODE_OFFSET` is deliberately excluded from the discretionary set: it CONTROLS a blocking op (z-offset calibration sends it mid-probe), so queuing it behind that op would break calibration.

### PrinterDiscovery (Hardware Data)

**Location:** `include/printer_discovery.h`, `src/printer/printer_discovery.cpp`

**Responsibilities:**
- Store discovered hardware: heaters, fans, sensors, LEDs
- Store macros and capability flags (has_qgl, has_probe, etc.)
- Store printer metadata: hostname, klipper_version, mcu_version
- Provide unified query interface for all hardware capabilities

**Accessed via:** `MoonrakerAPI::hardware()`

**Key Pattern:** All async methods use callback pattern:
```cpp
void method_name(
    std::function<void(ResultType)> on_success,
    ErrorCallback on_error
);
```

### HTTP Work Execution (HttpExecutor)

**Location:** `include/core/http_executor.h`, `src/core/http_executor.cpp`

HTTP requests from the Moonraker REST sub-APIs run on one of two process-wide `HttpExecutor` singletons, not on per-request threads.

**Lanes:**

| Lane | Workers | Used by |
|------|---------|---------|
| `HttpExecutor::fast()` | 4 | REST (`MoonrakerRestAPI`), extras (`MoonrakerAPI` extras endpoints), timelapse (`MoonrakerTimelapseAPI`), thumbnail downloads, memory-buffer uploads (config edits, PRINT_START shim, macro writes) |
| `HttpExecutor::slow()` | 1 | Streaming gcode downloads, large file-from-path uploads |

**Why two lanes:** A long upload on the slow lane cannot head-of-line block thumbnail fetches or status polls on the fast lane. The fast lane's 4 workers preserve burst parallelism when the file browser scrolls (~20 concurrent thumbnail requests) without reintroducing unbounded thread spawning.

**Why bounded:** Per-request `std::thread` spawning crashed under thread exhaustion (EAGAIN from `pthread_create`) on systems where multiple services share the user's `RLIMIT_NPROC` - e.g. RatOS 2.1 where klipper+moonraker+nginx+helixscreen all run as `pi`. The total HTTP thread cap is 5 across the whole process.

**Submitting work:**

```cpp
helix::http::HttpExecutor::fast().submit([url, on_success, on_error]() {
    auto resp = requests::get(url.c_str());
    if (resp) on_success(resp->body);
    else on_error(...);
});
```

The submitted lambda runs on a worker thread - callbacks invoked from it are on a background thread and must use `ui_queue_update()` or `tok.defer()` to touch UI. 

**Lifecycle:** `HttpExecutor::start_all()` is called just before `init_moonraker()` in `Application::run()`. `HttpExecutor::stop_all()` is called in `Application::shutdown()` after `PrinterSession::teardown_printer_scope()`, so no submitter outlives the workers. A soft restart (printer switch) destroys and recreates `MoonrakerManager` but leaves the executors running.

**When to use which lane (rule of thumb):**
- **Fast:** small request (≤ ~1 MB response/body), frequent, short-latency expected, or called in bursts. Safe default.
- **Slow:** response/body measured in tens of MB, transfer duration measured in seconds-to-minutes, or the call path explicitly streams to/from disk. Anything that would noticeably stall the user if head-of-line blocked behind it.

**Do NOT:** spawn a raw `std::thread` for HTTP work. If `HttpExecutor::fast()/slow()` don't fit the shape you need, discuss - adding a new lane is preferable to reintroducing unbounded spawning.

### Moonraker API Key

A Moonraker with `force_logins` refuses every request without a credential, trusted address or not, and accepts the printer's API key in an `X-Api-Key` header. Each saved printer keeps its own key in its `moonraker_api_key` setting, and a connection uses the active printer's. Change Host edits the active printer's key; Add Printer stores the key entered with the address on the added printer (`src/application/printer_switch_flow.cpp#add_printer`).

| Piece | Role |
|-------|------|
| `include/moonraker_auth.h` | The pure rules: `set_credential()` records the active printer's credential, `key_for()` / `key_for_url()` answer what key a host:port gets, `header_action()` decides Set / Leave / Remove for a request's header. Built for the desktop and the ESP32 firmware alike |
| `src/application/printer_retarget.cpp#connect_printer` | Records `active_printer_credential()`, built from the same host and port the connection URL uses, before every connect (startup, printer switch, Change Host) |
| `src/api/moonraker_auth_http.cpp` | Installs `apply_to_request()` as libhv's request hook (`patches/libhv-http-request-hook.patch`), so every libhv HTTP request, sync, async or upload, built from any URL, gets the key when it is bound for that printer |
| `MoonrakerClient::connect()` | Adds the header to the WebSocket upgrade, which libhv sends without the hook |
| `EspMoonrakerClient::create_transport()` | The firmware's WebSocket upgrade, through `esp_websocket_client_config_t::headers` |
| `EspHttpLane::run_one()` | The firmware's HTTP file reads, through `esp_http_client_set_header()` |

**The key goes only to the host and port saved with it.** Moonraker refuses a wrong key even from a trusted address, so a key sent to a different printer (the connection wizard testing a typed address, a CLI override) would break a connection that works without one. Change Host moves a printer entry to a new address and keeps its key, since it is still that printer. A synchronous redirect re-runs the hook, so the key comes off at another host; libhv's async client follows redirects without the hook, and nothing in the app sends HTTP through it. An `X-Api-Key` another caller set for its own service (telemetry) is left alone. The WebSocket upgrade request is created once per `MoonrakerClient` and reused by every connect, so `connect()` erases the previous key before it sets the current one.

**Entering the key.** The Change Host dialog (`src/ui/printers/ui_change_host_modal.cpp`) carries a masked key field beside the host and port, reached from Settings > Connection's Host and API Key rows; Save writes the key with `set_active_printer_api_key()` and reconnects through `connect_printer`. The connection wizard shows the same field when its test answers 401, and records the typed key for the address under test only (`WizardConnectionStep::connect_confirmed`). Test Connection sends the key in its `/server/info` probe, so it tells "needs a key" from "rejected this key" (`MoonrakerConnectionTest::Result::login_required`, `key_sent`). A key that is not 32 hex digits is refused before any request (`moonraker_auth::is_plausible_api_key`).

**A refused login.** When `server.connection.identify` fails with Unauthorized, `MoonrakerDiscoverySequence::stop_for_refused_login` stops discovery, marks `moonraker_auth::unauthorized()` and emits one `LOGIN_REFUSED` event per credential (`set_credential()` clears the mark). The manager shows it as a Login Required prompt whose button opens the key field; the request tracker's generic toast and the file-list and PRINT_START-analysis toasts stay quiet while the mark is set, so that message is not buried.

### Event System

**Location:** `include/moonraker_events.h`

The event system decouples the transport layer from the UI:

```cpp
enum class MoonrakerEventType {
    CONNECTION_FAILED,    // Max reconnect attempts exceeded
    CONNECTION_LOST,      // WebSocket closed unexpectedly
    RECONNECTING,         // Attempting to reconnect
    RECONNECTED,          // Successfully reconnected
    MESSAGE_OVERSIZED,    // Received message exceeds size limit
    RPC_ERROR,            // JSON-RPC request failed and no other surface will report it
    KLIPPY_DISCONNECTED,  // Klipper firmware disconnected
    KLIPPY_SHUTDOWN,      // Klipper firmware entered shutdown state (M112, thermal, error)
    KLIPPY_READY,         // Klipper firmware ready
    DISCOVERY_FAILED,     // Printer discovery failed (non-retryable)
    DISCOVERY_DEFERRED,   // Discovery waiting on Klippy (retries on state change)
    REQUEST_TIMEOUT,      // JSON-RPC request timed out
    LOGIN_REFUSED         // Moonraker refused this client's login (once per credential)
};

struct MoonrakerEvent {
    MoonrakerEventType type;
    std::string message;   // English
    std::string details;
    bool is_error;
    const char* message_tag = nullptr;      // untranslated template, or nullptr
    std::vector<std::string> message_args;  // fills the template's {} slots
};
```

**Emitting an event is not deciding how to show it.** `helix::decide_moonraker_event()` (`moonraker_event_routing.cpp`) owns that, as a pure function so the caller can apply `lv_tr()` on the main thread (#1219). The event's own text follows the same split: an emitter builds it with `MoonrakerEvent::translatable()`, which keeps the untranslated template in `message_tag` and its arguments in `message_args` beside the English `message` the logs use, and `MoonrakerManager::present_event()` renders `lv_tr(message_tag)` on the main thread. Text from Klipper or Moonraker stays untagged and is shown as it arrived. `CONNECTION_FAILED` is the one with real routing rules, because it is latched and fires ~60 s after startup - reliably landing on whatever the user is doing about it:

| Context | Route | Why |
|---|---|---|
| Setup wizard on screen | `Ignore` | The wizard's Connection step *is* the host-entry UI and reports its own result inline. On a fresh install the address that "failed" is only the `127.0.0.1` default the wizard exists to replace, and on a standalone display the message even claims Klipper runs on this machine. Bundle L53W5PKG: pushed over the Language step, left sitting there 16.5 minutes. |
| A modal already open | `ErrorToast` | Bundle 865DXBQ7: pushed at stack depth 2 over the WiFi password keyboard, password retyped from scratch. Degrade, never drop - the user is doing something else and still needs to know. |
| Otherwise | `ConnectionFailedModal` | The connection-failed prompt. **Reconnect is the primary action** - a dropped Moonraker is usually transient, so retrying is the first button. "Change Address" is secondary and only offered when the configured host is remote: when the printer is known to run HelixScreen itself (`is_moonraker_on_same_host`), the address is not the fault and the prompt is a plain Reconnect alert, since "Change Address" there walks the user into editing a correct `127.0.0.1` while the real problem is a Moonraker service that did not start (`src/ui/printers/ui_change_host_modal.cpp`). |

Recovery events (`KLIPPY_DISCONNECTED`, `KLIPPY_SHUTDOWN`) return before all of this and are suppressible by nothing - a shut-down Klippy needs its dialog whatever is on screen. `KLIPPY_READY` routes to `Ignore` unconditionally: klippy becoming ready is an internal lifecycle event, not a notification; the user-facing completion signal is the `klippy_state` READY observer (the "Printer ready" toast that dismisses the emergency-stop recovery dialog), not this event.

**Moonraker announces a Klippy that started, never one that failed to.** `notify_klippy_ready`
arrives on success; a startup error (a `printer.cfg` mistake, an MCU that will not answer, K2's
`key298`) produces no notification at all, and without help the app would read `SHUTDOWN` forever.
So after `notify_klippy_disconnected` the client polls `printer.info` from its health tick until
Klippy answers `ready`, `shutdown` or `error`, `notify_klippy_ready` arrives, the socket closes, or
two minutes pass (`include/moonraker_client.h#KlippyStatePoll`). `startup` and Moonraker's own
`disconnected` keep it polling. The answer goes through the same webhooks mapping as a status frame
(`PrinterState::apply_klippy_status()`), so `KlippyState::ERROR` and Klipper's message reach the
recovery dialog like any other state change, and an answer from before the socket last closed is
dropped by the Klippy epoch.

`PrinterRecoveryService` (`include/printer_recovery_service.h`) uses the same
`printer.info` answer to verify a restart instead of trusting that Moonraker accepted it:
`firmware_restart`, then a poll for `ready`, escalating once to the platform's
`<install_dir>/bin/helix-recover.sh` (written by `scripts/lib/installer/recovery.sh`) on an `error` or no `ready`, then one more poll after the script. The class
comment has the whole chain; its bounds are `RecoveryTiming` (one 120 s deadline from `recover()`,
the script killed at 90 s), and `AbortManager`'s restart backstop is held above that worst case by
a `static_assert` in `src/abort/abort_manager.cpp`. One chain runs at a time, and a chain whose
client is destroyed (printer switch, soft restart) ends without calling back.

**`RPC_ERROR` is a fallback, not a report of every failure.** `MoonrakerRequestTracker::route_response()` emits it only when `helix::rpc_error_policy::decide()` finds that nothing else will speak - and for `printer.gcode.script` that is never, because Klipper mirrors the rejection as a `!!` line that `GcodeErrorRouter` reports instead. See `RPC_ERROR_OWNERSHIP.md`.

**Usage:** the one handler is registered by `MoonrakerManager` (`src/application/moonraker_manager.cpp`) through `IMoonrakerClient::register_event_handler()`, and hands each event to `present_event()` on the main thread.

## Mock Architecture

For testing, parallel mock implementations exist:

```
┌─────────────────────────────────────────────────────────────────┐
│                      MockPrinterState                            │
│  (Shared State - tests/mocks/mock_printer_state.h)              │
│  ├─ Temperatures (atomic<double>)                               │
│  ├─ Print state                                                 │
│  ├─ Excluded objects (mutex-protected set)                      │
│  └─ Available objects                                           │
└─────────────────────┬─────────────────┬─────────────────────────┘
                      │                 │
┌─────────────────────▼───────┐  ┌─────▼─────────────────────────┐
│   MoonrakerClientMock       │  │    MoonrakerAPIMock           │
│   (Transport Mock)          │  │    (Domain Mock)              │
│   ├─ Simulated temps        │  │    ├─ Local file downloads    │
│   ├─ EXCLUDE_OBJECT parsing │  │    ├─ Mock uploads            │
│   └─ Synthetic bed mesh     │  │    └─ Shared state access     │
└─────────────────────────────┘  └─────────────────────────────────┘
```

### MockPrinterState

Thread-safe shared state between mock implementations:

```cpp
auto state = std::make_shared<MockPrinterState>();

MoonrakerClientMock client(PrinterType::VORON_24);
client.set_mock_state(state);

MoonrakerAPIMock api(client, printer_state);
api.set_mock_state(state);

// Now excluded objects sync between mocks
client.gcode_script("EXCLUDE_OBJECT NAME=Part_1", ...);
// api.get_excluded_objects_from_mock() returns {"Part_1"}
```

## Key Differences: API vs Client

| Aspect | MoonrakerAPI | MoonrakerClient |
|--------|--------------|-----------------|
| **Purpose** | Domain logic + data ownership | Transport only |
| **Hardware data** | Owns `PrinterDiscovery` | Dispatches via callbacks |
| **Bed mesh** | Owns `active_bed_mesh_`, `bed_mesh_profiles_` (in the `advanced()` sub-API) | Dispatches via callbacks |
| Return types | Pointers (nullable) | N/A for hardware data |
| G-code | `execute_gcode()` (async) | `gcode_script()` (sync-ish) |
| Connection state | `is_connected()`, `get_connection_state()` | Internal state |
| Subscriptions | `subscribe_notifications()`, `register_method_callback()` | Direct registration |
| Database | `database_get_item()`, `database_post_item()` | Raw `send_jsonrpc()` |
| Thread safety | Delegates to client | Internal mutexes |
| UI coupling | None | None (events only) |

## Testing

### Running Moonraker Tests

```bash
# All moonraker-related tests
./build/bin/helix-tests "[moonraker]"

# Just event tests
./build/bin/helix-tests "[events]"

# Integration tests
./build/bin/helix-tests "[integration]"

# Domain method parity tests
./build/bin/helix-tests "[domain]"

# Shared state tests
./build/bin/helix-tests "[shared_state]"
```

### Test Coverage

| Test File | Focus |
|-----------|-------|
| `test_moonraker_events.cpp` | Event emission, handler registration |
| `test_moonraker_api_domain.cpp` | Domain method parity (API vs Client) |
| `test_moonraker_mock_behavior.cpp` | Mock client behavior |
| `test_mock_shared_state.cpp` | MockPrinterState thread safety |
| `test_moonraker_full_stack.cpp` | Integration across all layers |

## File Reference

### Headers

| File | Purpose |
|------|---------|
| `include/moonraker_client.h` | Transport layer (WebSocket, JSON-RPC) |
| `include/moonraker_api.h` | Domain logic layer |
| `include/moonraker_events.h` | Event types and callbacks |
| `include/moonraker_types.h` | Shared data types (BedMeshProfile, GcodeStoreEntry, etc.) |
| `include/moonraker_client_mock.h` | Transport layer mock |
| `include/moonraker_api_mock.h` | Domain layer mock |
| `include/i_moonraker_client.h` | `helix::IMoonrakerClient` - transport-layer consumer contract |
| `include/i_moonraker_api.h` | `IMoonrakerAPI` - domain-layer consumer contract |
| `include/i_moonraker_sub_apis.h` | The ten sub-API interfaces (`IAdvancedAPI`, `IJobAPI`, etc.) |
| `include/moonraker_manager.h` | Owns the concrete client/API instances behind the interfaces |

### Sources

| File | Purpose |
|------|---------|
| `src/api/moonraker_client.cpp` | Transport implementation |
| `src/api/moonraker_api.cpp` | Domain logic (+ `moonraker_api_*.cpp` splits) |
| `src/mock/moonraker_client_mock.cpp` | Mock transport (+ `moonraker_client_mock_*.cpp` splits) |
| `src/mock/moonraker_api_mock.cpp` | Mock domain (local file access) |
| `src/application/moonraker_manager.cpp` | Lifecycle wiring, profile loading, observer setup |

### Test Files

| File | Purpose |
|------|---------|
| `tests/mocks/mock_printer_state.h` | Shared mock state |
| `tests/unit/test_moonraker_*.cpp` | Unit tests |
| `tests/unit/test_mock_shared_state.cpp` | Shared state tests |
| `tests/unit/test_moonraker_full_stack.cpp` | Integration tests |

## Capability-Driven Subscriptions

`printer.objects.subscribe` **replaces** the whole per-connection subscription, so
every capability that needs an object must contribute it to the single union map
built by `MoonrakerDiscoverySequence::build_subscription_objects()`.

Firmware-persisted z-offset is one such capability: on printers whose firmware keeps
the offset outside `gcode_move`, the storing object is added by
`helix::zoffset::required_status_objects(hw)` (`include/z_offset_persistence.h`) -
`save_variables` on ZMOD. The builder asks the capability question and never names a
firmware; see the vendor-abstraction rule in the root `CLAUDE.md`.

## Multi-Tool & Multi-Extruder Subscriptions

When `PrinterDiscovery` detects multiple extruders or a tool changer, additional Moonraker subscription keys are registered:

### Extruder Subscriptions

For each discovered extruder (`"extruder"`, `"extruder1"`, etc.), the status subscription includes:

```
extruder: temperature, target
extruder1: temperature, target
...
```

`PrinterTemperatureState::update_from_status()` iterates its `extruders_` map and updates per-extruder subjects for any matching keys in the status JSON.

### Toolchanger Subscriptions

When a tool changer is detected, the subscription includes:

```
toolchanger: status, tool_number, tool_numbers
tool T0: active, mounted, detect_state, gcode_x_offset, gcode_y_offset, gcode_z_offset, extruder, fan
tool T1: active, mounted, detect_state, ...
```

`ToolState::update_from_status()` parses these keys and bumps the `tools_version` subject on changes.

### Multi-Backend AMS

Each AMS backend manages its own Moonraker subscriptions independently. When `AmsState::init_backends_from_hardware()` creates multiple backends, each backend registers its own WebSocket subscriptions for its object types (e.g., `mmu` for Happy Hare, `AFC` for AFC, `tool T*` for tool changers). Backend state changes are synced to per-backend slot subjects via `AmsState::sync_backend()`.

**Full docs:** [TOOL_ABSTRACTION.md](TOOL_ABSTRACTION.md), [MULTI_EXTRUDER_TEMPERATURE.md](MULTI_EXTRUDER_TEMPERATURE.md), [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md)

---

## Locating Moonraker's Config File

Anything that edits Moonraker's configuration (today: Spoolman setup, in
`ui_spoolman_overlay.cpp`) has to answer one question first - **which file on disk did
Moonraker actually load, and can the file API reach it?** The reported name is not an
answer, and the difference has produced silent-failure bugs on three firmwares.

### Why the reported name is not a path

`server.config` names each loaded file **relative to the root config's own parent
directory**, falling back to the full absolute path for anything outside it.
`server.files.*` addresses files **relative to the file manager's `config` root**. Those
are the same directory on a stock install and different ones on real firmware:

| Firmware | file manager `config` root | `server.config` reports | Same? |
|---|---|---|---|
| Raspberry Pi, CB1 | `~/printer_data/config` | `moonraker.conf` | yes |
| Creality K1 | `/usr/data/printer_data/config` | `moonraker.conf` | yes |
| Snapmaker U1 | `/oem/printer_data/config` | a relative subpath | yes |
| Creality K2 | `/mnt/UDISK/printer_data/config` | `moonraker.conf` | **no** - the real config is `/usr/share/moonraker/moonraker.conf`, launched via `-c`, and 404s over HTTP |
| Flashforge AD5M | `/opt/config` | absolute paths under `/root/printer_data/config` | **yes, but only by luck** - that path is a symlink to `/opt/config`, so the same files *are* served under the root by their tail |

### The resolution chain

`MoonrakerConfigManager::candidate_config_paths()` turns a reported name into a **ranked
list**, and `SpoolmanOverlay::verify_config_reachable()` proves the winner by downloading
it and grading its sections. Nothing is ever written on the strength of a path alone.

1. **Already relative** - the file API takes it verbatim.
2. **Absolute and under the `config` root** (from `server.files.roots`, the only call that
   reports an absolute writable path) - strip the prefix, on a component boundary so
   `.../config` cannot swallow `.../config_backup`.
3. **Absolute and outside it** - *speculate* from the tail after the last `config/`
   component, then the bare basename.

Case 3 is a guess, and `candidates_are_speculative()` says so. A guessed candidate must
match Moonraker's section list **exactly**; only a derived one may lean on drift
tolerance. The two compound otherwise: a stray `moonraker.conf` under the writable root
shares the whole stock section set with the vendor config, so a guessed path plus a
fractional match rule writes confidently to a file Moonraker never reads.

### Section drift vs. a different file

Moonraker serves the section list it parsed **when it last started**, so a file edited
since then legitimately disagrees with it - uninstalling HelixScreen strips
`[update_manager helixscreen]` while a long-running Moonraker keeps reporting it.
`classify_section_match()` grades by **how many sections went missing**, not what fraction
survived: tolerance is one always, a quarter of the list once that is more. A
fraction-of-total rule scores a decoy well above chance, because two unrelated
`moonraker.conf` files agree on everything stock and differ only in the extras.

### The local-write fallback (K2 only, in practice)

When the config is genuinely outside anything the file API serves *and* Moonraker runs on
this host, `moonraker_local_probe.cpp` reads its `-c`/`-d` from `/proc` and edits the file
directly. Three things make that safe enough to ship:

- **Order.** `helixscreen.conf` is uploaded through the file API **first**; the absolute
  `[include]` naming it is appended **second**. An include with no matching file makes
  Moonraker raise `ConfigError` and refuse to start, taking the printer's web stack down.
- **The include is absolute.** Moonraker resolves a relative include against the
  *including* file's directory - a bare `helixscreen.conf` in a vendor config under
  `/usr/share` names a file that does not exist.
- **Durability.** Read whole, append, write to a temp file in the same directory, `fsync`,
  `rename`, `fsync` the directory. The rename alone orders only the directory entry, not
  the data, which is exactly how a power cut yields a zero-length `moonraker.conf`.
  Symlinked configs are resolved first, or `rename` would replace the link itself.

Two gates keep it narrow. `SpoolmanConfigTarget::proved_out_of_reach` distinguishes "the
file API answered and the config is not under a root it serves" from "we could not tell" -
a dropped socket must never license editing vendor firmware. And `plan_local_include()`
refuses when the config already resolves inside the writable root, compared through
`fs::weakly_canonical()` rather than string prefixes, because on the AD5M the two are one
directory named two ways.

**Code:** `include/moonraker_config_manager.h`, `include/system/moonraker_local_probe.h`,
`src/ui/filament/ui_spoolman_overlay.cpp`.
**Tests:** `tests/unit/test_config_path_candidates.cpp`,
`test_moonraker_local_include_plan.cpp`, `test_local_config_append.cpp`,
`test_moonraker_file_roots.cpp`.

---

## See Also

- `docs/devel/TESTING.md` - General testing guide
- `include/moonraker_api.h` - Full API documentation (Doxygen)
