---
name: libhv
description: >
  libhv C networking library knowledge for HelixScreen development.
  Use when working with WebSocket connections, HTTP clients, event loops,
  JSON parsing (hv/json.hpp), or any networking code in HelixScreen.
  Includes HelixScreen-specific context: 3 local patches, WebSocketClient
  integration, UpdateQueue threading pattern, DNS resolver fallback.
  Trigger on hv:: WebSocketClient, EventLoop, json.hpp, or networking code.
---

# libhv for HelixScreen

HelixScreen uses libhv primarily for **WebSocket communication with Moonraker** (Klipper's API server). The `hv::WebSocketClient` class provides the persistent connection, while `hv/json.hpp` (nlohmann/json bundled with libhv) handles JSON-RPC messages.

## Build Integration

- **Submodule**: `lib/libhv/`
- **3 local patches**: DNS resolver fallback, streaming upload, OpenSSL static link
- **Headers used**: `hv/WebSocketClient.h`, `hv/EventLoop.h`, `hv/json.hpp`, `hv/HttpContent.h`

## HelixScreen WebSocket Architecture

```
Moonraker ←→ hv::WebSocketClient (BG thread) ←→ UpdateQueue ←→ LVGL main thread
```

**CRITICAL RULE**: WebSocket callbacks run on background threads. **NEVER call `lv_subject_set_*()` directly** from callbacks. Use `ui_queue_update()` to marshal updates to the main LVGL thread.

```cpp
// WRONG - crashes on ARM
void on_message(const string& msg) {
    lv_subject_set_int(&my_subject, value);  // UAF/race
}

// CORRECT - thread-safe
void on_message(const string& msg) {
    ui_queue_update([value]() {
        lv_subject_set_int(&my_subject, value);
    });
}
```

## Key Classes

### hv::WebSocketClient
Persistent WebSocket connection to Moonraker:
- Auto-reconnect with configurable retry delay
- Message callbacks: `onopen`, `onclose`, `onmessage`
- Thread-safe send: `send(json_str)` safe from any thread
- Used via `IMoonrakerClient` interface in HelixScreen

### hv::EventLoop / EventLoopThread
Event loop for async operations:
- WiFi management uses `hv::EventLoopThread` for WPA supplicant communication
- Moonraker client uses WebSocket's built-in event loop
- Tests using EventLoop must be tagged `[eventloop][slow]`

### hv/json.hpp (nlohmann/json)
Bundled JSON library — use for all JSON-RPC communication:
```cpp
#include "hv/json.hpp"
using json = nlohmann::json;

json rpc = {
    {"jsonrpc", "2.0"},
    {"method", "printer.objects.subscribe"},
    {"params", {{"objects", {{"print_stats", nullptr}}}}},
    {"id", request_id}
};
ws.send(rpc.dump());
```

## Threading Patterns

### UpdateQueue (ui_queue_update)
Thread-safe queue from BG threads to LVGL main thread:
- `ui_queue_update(lambda)` — post work to LVGL thread
- `UpdateQueue::scoped_freeze` — MANDATORY around drain+destroy sequences
- `observe_int_sync/async` — observer factories that use UpdateQueue internally

### AsyncLifetimeGuard
Prevents UAF when BG threads update UI after object destruction:
- `tok.defer()` (NOT `lifetime_.defer()`) from background threads
- `Modal` and `OverlayBase` provide `lifetime_` automatically

## Local Patches

| Patch | Purpose |
|-------|---------|
| `libhv-dns-resolver-fallback.patch` | Direct UDP DNS resolution fallback for statically-linked glibc builds where `getaddrinfo()` fails |
| `libhv-streaming-upload.patch` | Streaming upload support for large file transfers |
| `libhv-openssl-static-link.patch` | OpenSSL/static build hook for cross-compilation |

## Reference Files

| Topic | File |
|-------|------|
| EventLoop & EventLoopThread API | `references/event-loop.md` |
| WebSocketClient & WebSocket protocol | `references/websocket-client.md` |
| HTTP content, JSON, requests | `references/http-content.md` |
