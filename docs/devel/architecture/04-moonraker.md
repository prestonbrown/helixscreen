# 04 — Moonraker Integration

Everything HelixScreen knows about the printer arrives through one layered stack: a WebSocket client speaking JSON-RPC 2.0 to Moonraker, a domain-API façade split into ten sub-APIs on top of it, and a manager that owns both and hands them to the rest of the app as pure interfaces. Consumers — panels, controllers, state classes — never name a concrete class; two gates enforce that, which is why "which file do I edit to add an RPC method" always has the same shape of answer. This chapter is the integration view: the stack, the subscription that feeds status in, the send path and its error-ownership rules, connection lifecycle, and the mock stack that `--test` runs on.

Chapter 02 covered the main-thread half of the notification path — queue, dispatch ordering, subject writes — so this chapter picks up at the socket and stops where chapter 02's tour begins. The wire-level deep dive (client internals, HTTP execution, mock design) stays in [`../MOONRAKER_ARCHITECTURE.md`](../MOONRAKER_ARCHITECTURE.md); what follows is the map of which piece lives where and which gate fires when you cross a line.

```mermaid
flowchart TB
    subgraph EXT["Klipper host and off-screen services"]
        KL["Klipper MCU"]
        MR["Moonraker<br/>ws://host:7125/websocket<br/>+ http://host:7125"]
        SP["Spoolman"]
        KL -->|"status objects"| MR
        MR -.->|"server.spoolman.proxy"| SP
    end

    subgraph NET["Network layer - src/api/ - the only code naming concretes"]
        MC["MoonrakerClient<br/>hv::WebSocketClient + IMoonrakerClient<br/>JSON-RPC, discovery, auto-reconnect"]
        TRK["MoonrakerRequestTracker<br/>timeouts, error routing"]
        MA["MoonrakerAPI : IMoonrakerAPI<br/>+ ten sub-APIs (IMotionAPI, IFilesAPI,<br/>ITransfersAPI, ISpoolmanAPI, ...)"]
        HTEX["HttpExecutor<br/>fast() 4 workers / slow() 1 worker"]
        MC --> MA
        TRK --- MC
    end

    subgraph MOCK["--test only (HELIX_ENABLE_MOCKS)"]
        MCM["MoonrakerClientMock<br/>simulated printer, --sim-speed"]
        MAM["MoonrakerAPIMock<br/>local file transfers"]
        MHS["MockHttpFileServer<br/>loopback HTTP"]
    end

    subgraph MGR["MoonrakerManager - src/application/moonraker_manager.cpp"]
        OWN["unique_ptr owner<br/>create_client() / create_api()"]
        NQ["notification queue<br/>mutex-protected JSON"]
        OWN --> MC
        OWN --> MA
    end

    subgraph CONS["Consumers - interfaces only"]
        PANELS["Panels, overlays, managers<br/>get_moonraker_api() - 56 files in src/"]
        TCTRL["TemperatureController<br/>single authority for temp sends"]
        SPM["SpoolmanManager / AmsState"]
        PS["PrinterState and friends (ch. 02)"]
    end

    subgraph OFF["Off-socket system services (ch. 12)"]
        UC["UpdateChecker"]
        TLM["TelemetryManager"]
        CR["CrashReporter"]
    end

    MR <--|"WebSocket frames"| MC
    MA -->|"REST / file transfers<br/>via worker threads"| HTEX
    HTEX -->|"http://host:7125"| MR
    PANELS -->|"IMoonrakerAPI&, IXxxAPI&"| MA
    TCTRL --> MA
    SPM -->|"spoolman() - proxied by Moonraker"| MA
    MC -->|"notify_status_update<br/>on libhv thread"| NQ
    NQ -->|"process_notifications()<br/>on main thread"| PS
    UC -->|"releases.helixscreen.org (R2)<br/>api.github.com (fallback)"| EXT
    TLM -->|"telemetry.helixscreen.org/v1/events"| EXT
    CR -->|"crash.helixscreen.org CF Worker"| EXT
    OWN -.->|"test_mode"| MCM
    MCM --- MAM
    MCM --- MHS
```

## Key files

| File | Role |
|------|------|
| [`include/i_moonraker_client.h`](../../../include/i_moonraker_client.h) | Transport contract: connect, `send_jsonrpc` overloads, subscriptions, discovery, events |
| [`include/i_moonraker_api.h`](../../../include/i_moonraker_api.h) | API façade contract plus the ten sub-API accessor declarations |
| [`include/i_moonraker_sub_apis.h`](../../../include/i_moonraker_sub_apis.h) | The ten sub-API pure-virtual interfaces (`IMotionAPI` … `ITimelapseAPI`) |
| [`include/moonraker_manager.h`](../../../include/moonraker_manager.h) | The owner: `create_client()`/`create_api()`, notification queue, lifecycle |
| [`src/application/moonraker_manager.cpp`](../../../src/application/moonraker_manager.cpp) | Mock-vs-real construction, callback registration, main-thread dispatch |
| [`include/moonraker_client.h`](../../../include/moonraker_client.h) | Concrete transport (inherits libhv's `hv::WebSocketClient`); reconnect/health knobs |
| [`src/api/moonraker_client.cpp`](../../../src/api/moonraker_client.cpp) | Wire handling: JSON-RPC framing, install-once callbacks, reconnect state machine |
| [`src/api/moonraker_discovery_sequence.cpp`](../../../src/api/moonraker_discovery_sequence.cpp) | `printer.objects.list` → classification → narrowed `printer.objects.subscribe` |
| [`include/moonraker_api.h`](../../../include/moonraker_api.h) | Concrete façade; composes the ten sub-APIs as `unique_ptr` members |
| [`src/api/moonraker_motion_api.cpp`](../../../src/api/moonraker_motion_api.cpp) | Representative sub-API implementation (guards + jog coalescer integration) |
| [`src/api/moonraker_request_tracker.cpp`](../../../src/api/moonraker_request_tracker.cpp) | Pending-request map, timeouts, the RPC-error fallback decision |
| [`include/rpc_error_policy.h`](../../../include/rpc_error_policy.h) | `CallerIntent`/`Decision` types and `decide()` — the one error-ownership decision function |
| [`include/connection_state.h`](../../../include/connection_state.h) | The five-state `ConnectionState` enum, kept header-only so UI code avoids libhv |
| [`src/api/moonraker_spoolman_api.cpp`](../../../src/api/moonraker_spoolman_api.cpp) | Sub-API proving the proxy pattern: every call maps to `server.spoolman.*` |
| [`include/moonraker_events.h`](../../../include/moonraker_events.h) | `MoonrakerEvent` types and the transport→UI event callback |
| [`include/app_globals.h`](../../../include/app_globals.h) | `get_moonraker_api()` / `get_moonraker_client()` — how consumers reach the stack |
| [`include/runtime_config.h`](../../../include/runtime_config.h) | `should_mock_*()` predicates gating every mock arm |
| [`include/http_executor.h`](../../../include/http_executor.h) | The two bounded HTTP worker pools the sub-APIs submit to |
| [`include/mock_http_file_server.h`](../../../include/mock_http_file_server.h) | Loopback HTTP server standing in for Moonraker's file endpoints under `--test` |
| [`tests/shell/test_code_lint.bats`](../../../tests/shell/test_code_lint.bats) | Gate: naming a concrete Moonraker type outside the network layer fails CI |
| [`tests/unit/test_interface_drift_moonraker_api.cpp`](../../../tests/unit/test_interface_drift_moonraker_api.cpp) | Compile-time drift protection: mocks must implement every interface method |

## How it works

### The stack: three interfaces, one owner

The transport contract is `IMoonrakerClient` ([`include/i_moonraker_client.h:63`](../../../include/i_moonraker_client.h#L63)) — connection lifecycle, four `send_jsonrpc` overloads (fire-and-forget through success/error callbacks with explicit error-intent parameters), `gcode_script`, discovery hooks, notification/method-callback registration, and the event handler. The concrete `MoonrakerClient` ([`include/moonraker_client.h:78`](../../../include/moonraker_client.h#L78)) multiply inherits libhv's `hv::WebSocketClient` and this interface; the ESP32 firmware port swaps in its own implementation through the platform factory declared at the bottom of the same header (`create_platform_moonraker_client()`, [`include/i_moonraker_client.h:270`](../../../include/i_moonraker_client.h#L270)), which is the second reason consumers depend on the interface.

Above it sits `IMoonrakerAPI` ([`include/i_moonraker_api.h:45`](../../../include/i_moonraker_api.h#L45)) — the façade for heaters, fans, LEDs, system control, power devices, the Moonraker database, and the Helix plugin — plus ten sub-API interfaces in [`include/i_moonraker_sub_apis.h`](../../../include/i_moonraker_sub_apis.h), counted from the tree: `IMotionAPI`, `IJobAPI`, `IFilesAPI`, `IQueueAPI`, `IHistoryAPI`, `IAdvancedAPI`, `IRestAPI`, `ITransfersAPI`, `ISpoolmanAPI`, `ITimelapseAPI`. The façade exposes them through ten accessors ([`include/i_moonraker_api.h:352`](../../../include/i_moonraker_api.h#L352)); the concrete `MoonrakerAPI` overrides covariantly, so a caller holding `IMoonrakerAPI&` and calling `api->files().list_files(...)` sees only interface types end to end. The concrete façade composes its sub-APIs as `unique_ptr` members declared *after* the data they reference so they are destroyed first ([`include/moonraker_api.h:846`](../../../include/moonraker_api.h#L846)).

`MoonrakerManager` is the single owner and the only place the concrete names appear outside `src/api/`. `create_client()` ([`src/application/moonraker_manager.cpp:337`](../../../src/application/moonraker_manager.cpp#L337)) picks the ESP32 platform client, the mock (when built with `HELIX_ENABLE_MOCKS` and `runtime_config.should_mock_moonraker()`), or the real transport; `create_api()` ([`src/application/moonraker_manager.cpp:625`](../../../src/application/moonraker_manager.cpp#L625)) does the same for the façade. One nuance the older docs state imprecisely: the client is owned as `std::unique_ptr<helix::IMoonrakerClient>` but the API as the concrete `std::unique_ptr<MoonrakerAPI>` ([`include/moonraker_manager.h:391`](../../../include/moonraker_manager.h#L391)-392) — `MoonrakerAPIMock` still inherits the concrete `MoonrakerAPI`, so both arms fit one holder, and `create_api()` needs a concrete `MoonrakerClient&` for the mock. To avoid a `dynamic_cast` down to it (firmware builds `-fno-rtti`), `create_client()` records `m_concrete_client` while it still knows the static type. After construction the manager publishes both through `app_globals` as interfaces — `get_moonraker_api()` returns `IMoonrakerAPI*` ([`include/app_globals.h:67`](../../../include/app_globals.h#L67)) — and that accessor, plus interface-typed constructor parameters, is how consumers reach the stack; 56 files under `src/` call `get_moonraker_api()` directly.

Two gates keep the boundary real:

- The bats lint [`tests/shell/test_code_lint.bats:176`](../../../tests/shell/test_code_lint.bats#L176) fails CI if `helix::MoonrakerClient` or `MoonrakerAPI` appears in `src/` or `include/` outside the network-layer allowlist (the implementation files and `*_mock.*`). Its comment block (`:136`) documents the deliberate compile-time-only exceptions — static timeout constants and the `MPCResult` alias — that the grep intentionally ignores.
- The compile-only drift tests — [`tests/unit/test_interface_drift_moonraker_api.cpp:23`](../../../tests/unit/test_interface_drift_moonraker_api.cpp#L23) and six siblings, tagged `[compile][drift]` — `static_assert` that every mock and concrete still derives from and fully implements each interface, so adding a pure virtual breaks the build rather than silently leaving the mock behind.

Besides the panels, a few subsystems hold the client or API directly from startup: `PrintHistoryManager` receives both at construction ([`src/application/application.cpp:2112`](../../../src/application/application.cpp#L2112)), `PrintStartCollector` takes the client (`MoonrakerManager::init_print_start_collector()`), and `SoundManager` keeps a client pointer for M300 gcode audio ([`src/application/moonraker_manager.cpp:467`](../../../src/application/moonraker_manager.cpp#L467)). All of them receive interface types.

### Status in: discovery, narrowed subscription, deltas

Connecting is two steps, and the second is mandatory: `MoonrakerManager::connect()` ([`src/application/moonraker_manager.cpp:165`](../../../src/application/moonraker_manager.cpp#L165)) opens the WebSocket, and its `on_connected` callback immediately calls `client->discover_printer()`. The manager's own comment states the stakes (`:208`): without discovery, `printer.objects.subscribe` is never called and `notify_status_update` frames never arrive. The URL itself is built by the application from the per-printer config — `moonraker_host` (default `localhost`) and `moonraker_port` (default 7125) become `ws://host:port/websocket` plus the paired HTTP base ([`src/application/application.cpp:3664`](../../../src/application/application.cpp#L3664)); a `--moonraker <url>` CLI argument overrides both. Under `--test` the mock ignores the host entirely, but the HTTP base is redirected to the loopback server (see below).

`MoonrakerDiscoverySequence` ([`src/api/moonraker_discovery_sequence.cpp`](../../../src/api/moonraker_discovery_sequence.cpp)) runs the handshake: query `printer.objects.list`, classify what exists (heaters, sensors, fans, LEDs, MCUs, and the vendor-specific AMS objects chapter 07 covers), then `build_subscription_objects()` (`:1092`) assembles the subscribe payload. Field lists are deliberately narrowed — `toolhead`, `gcode_move`, and `motion_report` change on every motion step (~100 Hz during a print), so subscribing whole objects would stream every internal field per step. Core objects like `print_stats` and `webhooks` get fixed field lists; discovered hardware gets per-type shapes (`{temperature, target}` for heaters, `{speed}` for fans). The `printer.objects.subscribe` call at `:1536` returns the current values in `result.status` — the initial snapshot that seeds every subject — and completion fans out through the `on_discovery_complete` callback. From the moment the client dispatches a status frame, chapter 02 owns the story: raw JSON into the manager's mutex-protected queue, out on the main thread via `process_notifications()`, into `PrinterState::update_from_status()`.

One property worth internalizing: **subscriptions live per connection.** Moonraker replaces the subscription on every `printer.objects.subscribe` call, and each successful (re)open re-fires the stored `on_connected` callback, restarting discovery — so a reconnect always re-runs classification and re-subscribes against whatever objects the printer now reports. That is also how a printer switch heals: the soft restart tears the manager down and rebuilds it.

There is one deliberate replay in the other direction: at the end of *initial* discovery, the application re-dispatches the subscription response's status snapshot through `dispatch_status_update(..., from_cached_snapshot = true)` ([`src/application/application.cpp:3076`](../../../src/application/application.cpp#L3076)), stamped with the `CACHED_SNAPSHOT_MARKER` so liveness-sensitive consumers (the klippy-state freshness guard) refuse to regress on it. Chapter 02 covers why that marker exists; from this chapter's view it is the rule that status snapshots captured off-thread are always marked before they join live traffic.

### Commands out: sub-APIs, gcode sends, error ownership

A click that needs the printer goes: C++ handler → `get_moonraker_api()` → the matching sub-API accessor → `send_jsonrpc` on the client. The ten accessors and what lives behind them (call-site counts measured across `src/` at audit time):

| Accessor | Domain | Sites |
|----------|--------|-------|
| `transfers()` | HTTP file download/upload, thumbnails | 43 |
| `spoolman()` | Spoolman spools/filaments/vendors (proxied) | 42 |
| `advanced()` | Bed mesh, input shaper, PID/MPC, macros, belt | 34 |
| `files()` | File listing/metadata/delete/move via RPC | 24 |
| `rest()` | Generic REST passthrough, WLED, server config | 14 |
| `timelapse()` | Timelapse settings, webcam list | 11 |
| `job()` | Start/pause/resume/cancel, modified print | 11 |
| `motion()` | Homing, jogging (feeds the jog coalescer) | 8 |
| `history()` | Print history list/totals/delete | 7 |
| `queue()` | Moonraker job queue | 5 |

The placement rule is mechanical: a new Moonraker RPC belongs in the sub-API whose domain it serves, declared on the matching `IXxxAPI` so the drift test forces mock parity; the façade keeps what is genuinely cross-cutting (temperatures, fans, LEDs, power, system, database). Two exceptions are lint-enforced rather than advisory: all heater target sends must go through `TemperatureController`, never a raw `set_temperature` call, and HTTP-shaped work (file downloads/uploads, REST, timelapse) leaves through the two `HttpExecutor` pools ([`include/http_executor.h:87`](../../../include/http_executor.h#L87)) instead of the WebSocket — `fast()` (4 workers) for small requests and thumbnails, `slow()` (1 worker) for streaming transfers, so a large upload cannot head-of-line-block status traffic.

Gcode goes through `execute_gcode`, whose signature is a contract about failure: `on_queued` is a third disposition fired when a discretionary command was accepted behind a blocking operation and its reply dropped; `caller_surfaces_errors` declares whether your `on_error` actually shows the user something (a log line is not a report). Who reports a failed call is not the caller's to improvise: three surfaces could speak — the caller's `on_error`, the request tracker's generic fallback toast, and `GcodeErrorRouter` reporting Klipper's `!!` broadcast — and exactly one must. The decision lives in one function, `helix::rpc_error_policy::decide()`, invoked from `MoonrakerRequestTracker::route_response()` ([`src/api/moonraker_request_tracker.cpp:144`](../../../src/api/moonraker_request_tracker.cpp#L144)); for `printer.gcode.script` the `!!` broadcast outranks the generic fallback, which stands down entirely. Read [`../RPC_ERROR_OWNERSHIP.md`](../RPC_ERROR_OWNERSHIP.md) before adding an `on_error` to any send — it is the full contract and the reasoning behind it.

Spoolman deserves one correction to the old system-overview diagram: HelixScreen never talks to the Spoolman service directly. `ISpoolmanAPI` calls map to Moonraker's `server.spoolman.proxy` JSON-RPC method ([`src/api/moonraker_spoolman_api.cpp:176`](../../../src/api/moonraker_spoolman_api.cpp#L176)) — Moonraker fronts Spoolman — so its availability is really "is the proxy configured on the printer," queried via `server.spoolman.status` (`:143`).

The services that never touch the Moonraker socket at all round out the picture, and the old diagram got their wiring wrong in one place. `UpdateChecker` polls its own release manifest on the R2 CDN at `releases.helixscreen.org` ([`include/system/update_checker.h:82`](../../../include/system/update_checker.h#L82)) with the GitHub releases API as fallback ([`src/system/update_checker.cpp:73`](../../../src/system/update_checker.cpp#L73)) — never Moonraker. `TelemetryManager` POSTs events to `telemetry.helixscreen.org/v1/events` ([`include/system/telemetry_manager.h:763`](../../../include/system/telemetry_manager.h#L763)). `CrashReporter` and the debug-bundle collector post to a Cloudflare Worker at `crash.helixscreen.org` ([`include/system/crash_reporter.h:259`](../../../include/system/crash_reporter.h#L259)), which files a GitHub issue — a different host from telemetry, correcting the old diagram's "crash POST → telemetry.helixscreen.org" edge. All three are chapter 12 subjects; they appear here because the Comms picture is incomplete without them.

### Connection lifecycle: reconnect, events, failure UX

The transport owns reconnection. `connect()` ([`src/api/moonraker_client.cpp:397`](../../../src/api/moonraker_client.cpp#L397)) installs libhv callbacks exactly once — reassigning `onopen`/`onmessage`/`onclose` per connect raced the event-loop thread into a use-after-free, so install-once trampolines forward to per-connect state (`:429`). `apply_reconnect_settings()` (`:456`) arms libhv auto-reconnect with exponential backoff between 200 ms and 2 s. The knobs all come from config with those defaults, applied by `configure_timeouts()` ([`src/application/moonraker_manager.cpp:491`](../../../src/application/moonraker_manager.cpp#L491)):

- `moonraker_connection_timeout_ms` — 10 s to establish the socket
- `moonraker_request_timeout_ms` — per-RPC deadline, defaulting to the tracker's 60 s ([`include/moonraker_request_tracker.h:145`](../../../include/moonraker_request_tracker.h#L145))
- `moonraker_keepalive_interval_ms` — WebSocket ping, 10 s
- `moonraker_reconnect_min_delay_ms` / `moonraker_reconnect_max_delay_ms` — 200 ms / 2000 ms backoff bounds

A 5 s health timer ([`include/moonraker_client.h:812`](../../../include/moonraker_client.h#L812)) detects stalled reconnection. Each attempt increments a counter in `set_connection_state()` ([`src/api/moonraker_client.cpp:215`](../../../src/api/moonraker_client.cpp#L215)); exceeding the maximum emits `CONNECTION_FAILED` exactly once and latches `ConnectionState::FAILED` — the five-state enum in [`include/connection_state.h:14`](../../../include/connection_state.h#L14) the rest of the app observes as a subject. `force_reconnect()` (`:356`) is the deliberate full reset, used by display wake and printer switch; `set_auto_reconnect(false)` exists for host-probe flows (setup wizard, change-host modal) that must not race a background retry loop — and is transient by design, since the next `connect()` reinstalls its own settings.

Failures the user must see travel as `MoonrakerEvent`s, not return codes. The manager registers exactly one handler and marshals its whole body to the main thread in a single hop — `lifetime_.bg_cb()` around `present_event()` ([`src/application/moonraker_manager.cpp:515`](../../../src/application/moonraker_manager.cpp#L515)) — because everything downstream (translation lookups, toasts, modals) is LVGL-facing while the event fires on the libhv loop (#1219). Routing is a pure function, `decide_moonraker_event()`: a wizard on screen ignores the failure, a modal already open degrades to a toast, otherwise the connection-failed prompt appears — Reconnect primary, with Change Address offered only as the secondary action, and dropped entirely when the configured host is this same machine (`is_moonraker_on_same_host`), where the address is not the fault and retrying the service is the meaningful action; Klippy shutdown always gets its recovery overlay. KLIPPY_READY routes to Ignore unconditionally: klippy becoming ready is an internal lifecycle event, not a notification, and the user-facing completion signal is the klippy_state READY observer - it dismisses the EmergencyStopOverlay recovery dialog with a "Printer ready" success toast ([`src/ui/ui_emergency_stop.cpp:334`](../../../src/ui/ui_emergency_stop.cpp#L334)), or fires that toast directly when the READY ends an expected restart whose dialog was suppressed (SAVE_CONFIG, power/host flows). The interaction trap is that connection-state *changes* ride the same notification queue as status deltas (as `_connection_state` marker objects, chapter 02), so modal side effects — like auto-closing the Connection Failed prompt on reconnect — run from `process_notifications()`, never from the callback thread.

The mock arm of all this is what `--test` runs. `RuntimeConfig` ([`include/runtime_config.h:26`](../../../include/runtime_config.h#L26)) exposes eight mock predicates — seven `should_mock_*()` plus `should_use_test_files()` — each `test_mode && !use_real_*`; when `HELIX_ENABLE_MOCKS` is not defined — every production and firmware build — they are `constexpr` functions returning `false`, so production never falls back to a mock even if construction fails. Under `--test`, `create_client()` builds `MoonrakerClientMock` with a printer personality from `HELIX_MOCK_PRINTER` and a clock speedup from `--sim-speed`; `create_api()` builds `MoonrakerAPIMock` over the same concrete client; and the manager stands up `MockHttpFileServer`, a loopback HTTP server, because thumbnails and gcode downloads issue *real* HTTP GETs even in test mode — `connect()` redirects the HTTP base URL to it ([`src/application/moonraker_manager.cpp:199`](../../../src/application/moonraker_manager.cpp#L199)). Two more mock-only wirings happen inside `create_client()`: the client is published under its concrete mock type via `get_moonraker_client_mock()` ([`include/app_globals.h:52`](../../../include/app_globals.h#L52), null on real runs) for consumers that need the simulator's extra surface, and an `AmsBackendMock` created earlier in boot subscribes to the simulator's active-gcode-tool notifications so AMS state follows the fake print ([`src/application/moonraker_manager.cpp:481`](../../../src/application/moonraker_manager.cpp#L481)). The mock is interface-compatible by construction (it inherits the concretes), and the drift tests pin that compatibility at compile time.

## Patterns & gotchas

- **Name interfaces, never concretes.** `helix::IMoonrakerClient`, `IMoonrakerAPI`, and the ten `IXxxAPI` types are the consumer vocabulary; the bats gate at [`tests/shell/test_code_lint.bats:176`](../../../tests/shell/test_code_lint.bats#L176) fails CI on `MoonrakerClient`/`MoonrakerAPI` outside the network-layer allowlist. Compile-time-only exceptions (static timeout constants, the `MPCResult` alias) are documented in that file's comment block.
- **Adding a pure virtual to an interface breaks the build twice** — concrete first, then the mock — by design. That cascade is the drift protection working; implement both, don't weaken the interface.
- **New RPC method: sub-API, not façade.** Declare on the `IXxxAPI`, implement in the `moonraker_*_api.cpp` pair, and the mock inherits parity pressure. The façade is for cross-cutting sends only.
- **Temperature sends go through `TemperatureController`.** Calling `set_temperature` directly is lint-enforced against — see chapter 02 and chapter 05 (the TemperatureController section).
- **No raw `std::thread` for HTTP.** `HttpExecutor::fast()`/`slow()` exist because per-request spawning hit `EAGAIN` thread exhaustion on shared-user hosts; the two-lane rationale is in [`../MOONRAKER_ARCHITECTURE.md`](../MOONRAKER_ARCHITECTURE.md) § HTTP Work Execution.
- **Read [`../RPC_ERROR_OWNERSHIP.md`](../RPC_ERROR_OWNERSHIP.md) before adding `on_error`.** Passing a logging callback but claiming `caller_surfaces_errors = true` silences Klipper's `!!` broadcast — the surface that would have explained the failure.
- **Client callbacks run on the libhv event loop.** Touching LVGL from one is the chapter 03 crash family; the manager's queue and `bg_cb()` marshalling are the sanctioned crossings.
- **Reconnect re-runs discovery and subscription.** Anything cached from a previous connection (capability lookups, per-printer caches via `PrinterCacheRegistry`) must be re-derived or invalidated on reconnect/switch, not assumed.
- **Teardown order is load-bearing:** `shutdown()` drops the client *first* — its destructor waits for in-flight libhv callbacks that capture raw pointers to the API ([`src/application/moonraker_manager.cpp:163`](../../../src/application/moonraker_manager.cpp#L163), #628) — then macro analysis, then the API.
- **Intentional disconnects suppress the modal.** `disconnect()` arms a 2 s `suppress_disconnect_modal` window so the close it causes does not surface as "connection lost" ([`src/api/moonraker_client.cpp:297`](../../../src/api/moonraker_client.cpp#L297)); connection-test flows additionally disable auto-reconnect. If you add a deliberate disconnect path, use both.
- **The mock API still takes a concrete `MoonrakerClient&`** — `MoonrakerAPIMock` predates the interface split. That is why `create_client()` records `m_concrete_client` for `create_api()` instead of downcasting the interface pointer.
- **Mock knobs are env-gated and additive**: `HELIX_MOCK_PRINTER` (personality), `HELIX_MOCK_AUTO_PRINT`, `HELIX_MOCK_AMS=none`, `HELIX_MOCK_SPOOLMAN=0`, `--sim-speed`. Default `--test` behavior is a Voron 2.4 sitting in Preparing; details in [`../MOCK_ENVIRONMENT_VARIABLES.md`](../MOCK_ENVIRONMENT_VARIABLES.md).

## Going deeper

- [`../MOONRAKER_ARCHITECTURE.md`](../MOONRAKER_ARCHITECTURE.md) — the wire-level deep dive this chapter summarizes: client/API responsibility split, `HttpExecutor` lanes and why they are bounded, the event-routing decision table, mock internals, and Moonraker config-file resolution.
- [`../RPC_ERROR_OWNERSHIP.md`](../RPC_ERROR_OWNERSHIP.md) — the full three-surface error contract, the correlation window, and the rule that makes the decision computable at the call site.
- [`../UPDATE_SYSTEM.md`](../UPDATE_SYSTEM.md) — release channels, the R2 CDN, and how `UpdateChecker` picks between `releases.helixscreen.org` and the GitHub API.
- [`../TELEMETRY_ADMIN.md`](../TELEMETRY_ADMIN.md) and [`../CRASH_REPORTER.md`](../CRASH_REPORTER.md) — the pipelines behind the two `*.helixscreen.org` endpoints.
- [`../MOCK_ENVIRONMENT_VARIABLES.md`](../MOCK_ENVIRONMENT_VARIABLES.md) — the full `HELIX_MOCK_*` matrix and every knob the mock stack reads.
- [`../TESTING.md`](../TESTING.md) — the fixture-isolation design the mocks feed into (`HelixTestFixture` / `XMLTestFixture`) and the `[compile][drift]` tests that pin mock parity.
- [`02-subjects-dataflow.md`](02-subjects-dataflow.md) — the main-thread half of this chapter's notification path: queue, dispatch ordering, subject writes.
- [`03-threading-lifetime.md`](03-threading-lifetime.md) — the guards (`AsyncLifetimeGuard`, `bg_cb`) the event marshalling here relies on.

## Guided code tour

Read in this order; about 30 minutes total.

1. [`include/i_moonraker_client.h:63`](../../../include/i_moonraker_client.h#L63) — the transport contract in one screen: sections for connection, JSON-RPC, discovery, subscriptions, events. Note the error-intent parameter on the callback `send_jsonrpc` at `:109`.
2. [`include/moonraker_client.h:78`](../../../include/moonraker_client.h#L78) — the concrete: `class MoonrakerClient : public hv::WebSocketClient, public IMoonrakerClient`, and at `:812` the 5 s health-check interval constant.
3. [`include/i_moonraker_api.h:352`](../../../include/i_moonraker_api.h#L352) — the ten sub-API accessors back-to-back; then the façade's `execute_gcode` contract at `:211` with `on_queued` and `caller_surfaces_errors`.
4. [`include/i_moonraker_sub_apis.h:65`](../../../include/i_moonraker_sub_apis.h#L65) — `IMotionAPI` through `ITimelapseAPI`; notice each documents the concrete header it mirrors, and shared payload types (`JobQueueStatus`, `IAdvancedAPI::MPCResult`) live here by necessity.
5. [`include/moonraker_manager.h:311`](../../../include/moonraker_manager.h#L311) — the private section: `create_client()`/`create_api()` declarations, the two `unique_ptr` members at `:327` (interface vs concrete holder), and `m_concrete_client` with its no-RTTI rationale.
6. [`include/app_globals.h:33`](../../../include/app_globals.h#L33) — the accessors every consumer actually uses: `get_moonraker_client()`, `get_moonraker_api()`, and the mock-only `get_moonraker_client_mock()` with its null-on-real-runs contract.
7. [`src/application/moonraker_manager.cpp:337`](../../../src/application/moonraker_manager.cpp#L337) — `create_client()`: ESP32 arm, mock arm (`HELIX_MOCK_PRINTER` parsing, `--sim-speed`, `MockHttpFileServer`), real arm, and the mock-to-mock AMS wiring. Then `create_api()` at `:600`.
8. [`src/api/moonraker_discovery_sequence.cpp:1182`](../../../src/api/moonraker_discovery_sequence.cpp#L1182) — `build_subscription_objects()`: the narrowed field lists and the comments explaining why (100 Hz motion objects, fork-only fields). The subscribe call and initial-snapshot handling at `:1586`.
9. [`src/api/moonraker_motion_api.cpp:181`](../../../src/api/moonraker_motion_api.cpp#L181) — `move_relative()`: a sub-API method in full — input guards with `on_error`, the zero-delta short circuit, G0 composition for the jog coalescer. Then skim [`src/api/moonraker_spoolman_api.cpp:138`](../../../src/api/moonraker_spoolman_api.cpp#L138) for the proxy pattern: every call maps to `server.spoolman.*` (`:176`), which is what makes "Spoolman" a Moonraker-fronted service.
10. [`src/api/moonraker_request_tracker.cpp:144`](../../../src/api/moonraker_request_tracker.cpp#L144) — `route_response()`: where a failed reply becomes either your `on_error`, a generic `RPC_ERROR` event, or nothing (because the `!!` broadcast owns it); `decide()` at `:220`.
11. [`src/api/moonraker_client.cpp:397`](../../../src/api/moonraker_client.cpp#L397) — `connect()`: install-once trampolines and the reconnect settings; then `set_connection_state()` at `:215` for the attempt counter and the single `CONNECTION_FAILED` emission.
12. [`src/application/moonraker_manager.cpp:571`](../../../src/application/moonraker_manager.cpp#L571) — `register_callbacks()`: the one-hop `bg_cb()` event marshalling and the `_connection_state` markers joining the notification queue; cross-reference `process_notifications()` at `:234`.
13. [`include/runtime_config.h:191`](../../../include/runtime_config.h#L191) — `should_mock_moonraker()` and its siblings; scroll to `:241` for the `constexpr` false arms that make production builds mock-proof.
14. [`tests/unit/test_interface_drift_moonraker_api.cpp:23`](../../../tests/unit/test_interface_drift_moonraker_api.cpp#L23) — the drift test: `static_assert(is_base_of_v<...> && !is_abstract_v<...>)` for the façade and all ten sub-APIs.
15. [`tests/shell/test_code_lint.bats:136`](../../../tests/shell/test_code_lint.bats#L136) — the comment block documenting the concrete-type gate, its allowlist, and the deliberate compile-time-only exceptions; the test itself at `:176`.
