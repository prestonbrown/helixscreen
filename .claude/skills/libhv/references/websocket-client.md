# WebSocketClient & WebSocket Protocol

## Location

`http/client/WebSocketClient.h` (included via `hv/WebSocketClient.h`)

**Not** in the evpp/ directory — WebSocketClient is in the HTTP client layer.

---

## Class Hierarchy

```
TcpClientTmpl<WebSocketChannel>
  └── WebSocketClient
```

Inherits from `TcpClientTmpl`, which provides: reconnect logic, TLS support, thread-safe send, and event loop thread management.

---

## Lifecycle States

```
CONNECTING → CONNECTED → WS_UPGRADING → WS_OPENED → WS_CLOSED
```

- `CONNECTING` — TCP connection in progress
- `CONNECTED` — TCP established, WebSocket upgrade pending
- `WS_UPGRADING` — HTTP upgrade request sent
- `WS_OPENED` — WebSocket handshake complete, ready for messages
- `WS_CLOSED` — connection closed (clean close or error)

---

## Constructor

```cpp
WebSocketClient(EventLoopPtr loop = NULL);
```

- `loop = NULL` → creates its own `EventLoopThread` internally
- `loop = shared_ptr<EventLoop>` → shares an existing event loop (e.g., from `EventLoopThread::loop()`)

---

## Callback Members

| Callback | Signature | When Fired |
|----------|-----------|------------|
| `onopen` | `std::function<void()>` | WebSocket handshake complete, connection ready |
| `onclose` | `std::function<void()>` | Connection closed (clean or error) |
| `onmessage` | `std::function<void(const std::string& msg)>` | Text or binary message received |

Set before calling `open()`:

```cpp
ws.onopen = [&]() {
    // connection ready — send subscriptions
};
ws.onmessage = [&](const std::string& msg) {
    // process incoming message
};
ws.onclose = [&]() {
    // cleanup
};
```

---

## Key Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `open` | `int open(const std::string& url, const http_headers& headers = DefaultHeaders)` | Connect to `ws://` or `wss://` URL. Optional custom headers for upgrade request. Returns 0 on success. |
| `close` | `int close()` | Close WebSocket connection (sends close frame). |
| `send` (text) | `int send(const std::string& msg)` | Send text message. **Thread-safe** (inherited from TcpClient). |
| `send` (binary) | `int send(const void* buf, int len, int opcode = WS_OPCODE_BINARY)` | Send binary data with explicit opcode. **Thread-safe**. |
| `setPingInterval` | `void setPingInterval(int ms)` | Set keepalive ping interval. 0 disables. |
| `setHttpRequest` | `void setHttpRequest(HttpRequestPtr req)` | Customize the HTTP upgrade request. Call **before** `open()`. |
| `getHttpResponse` | `HttpResponsePtr getHttpResponse()` | Get server's upgrade response. Call in `onopen` callback. |
| `opcode` | `int opcode()` | Get last message opcode (see opcodes below). |
| `isConnected` | `bool isConnected()` | Check if TCP connection is active (inherited from TcpClient). |
| `setReconnect` | `void setReconnect(reconn_setting_t* setting)` | Configure auto-reconnect (inherited from TcpClient). |
| `setConnectTimeout` | `void setConnectTimeout(int ms)` | Connection timeout in ms (inherited). |
| `withTLS` | `void withTLS(ssl_ctx_opt_t opt = ssl_ctx_opt_t())` | Enable TLS with optional config (inherited). For `wss://` connections. |

---

## Reconnect Configuration (reconn_setting_t)

For auto-reconnect with exponential backoff:

```cpp
reconn_setting_t reconn;
reconn_setting_init(&reconn);
reconn.min_delay = 1000;    // minimum delay in ms
reconn.max_delay = 10000;   // maximum delay in ms
reconn.delay_policy = 2;    // exponential backoff multiplier
// Delay pattern: 1s, 2s, 4s, 8s, 10s, 10s, 10s...

ws.setReconnect(&reconn);
```

After a disconnect, WebSocketClient automatically retries using these settings. The `onopen` callback fires again on successful reconnect.

---

## WebSocket Opcodes

| Opcode | Value | Description |
|--------|-------|-------------|
| `WS_OPCODE_TEXT` | 0x1 | UTF-8 text message |
| `WS_OPCODE_BINARY` | 0x2 | Binary message |
| `WS_OPCODE_CLOSE` | 0x8 | Close frame |
| `WS_OPCODE_PING` | 0x9 | Keepalive ping |
| `WS_OPCODE_PONG` | 0xA | Keepalive pong response |

Use `ws.opcode()` in `onmessage` to distinguish text vs binary frames when needed.

---

## WebSocketChannel

Extends `SocketChannel` (from `evpp/Channel.h`). Handles:
- Frame encoding/decoding per RFC 6455
- Fragmentation for large messages
- Ping/pong for liveness detection
- Close handshake

---

## TcpClient Base Class (inherited by WebSocketClient)

Key inherited members:

| Member | Type | Description |
|--------|------|-------------|
| `createsocket` | `int createsocket(int port, const char* host = "127.0.0.1")` | Create socket. Returns connfd or negative error. |
| `closesocket` | `int closesocket()` | Close socket. Thread-safe. Disables reconnect. |
| `start` | `int start()` | Thread-safe. Start connect + event loop thread. |
| `stop` | `int stop()` | Thread-safe. Close socket + stop event loop. |
| `send` | `int send(const void* buf, int len)` | Thread-safe write to socket. |
| `channel` | `SocketChannelPtr` | The connection channel (valid after connect). |
| `onConnection` | `std::function<void(const SocketChannelPtr&)>` | Callback for connect/disconnect. |
| `onMessage` | `std::function<void(const SocketChannelPtr&, Buffer*)>` | Callback for raw received data. |
| `onWriteComplete` | `std::function<void(const SocketChannelPtr&, Buffer*)>` | Callback when write finishes. |
| `remote_host` | `std::string` | Connection target hostname. |
| `remote_port` | `int` | Connection target port. |
| `connect_timeout` | `int` | Connect timeout in ms. |
| `tls` | `bool` | Whether TLS is enabled. |
| `tls_setting` | `ssl_ctx_opt_t` | TLS configuration. |

---

## SocketChannel (evpp/Channel.h)

### Status Enum

```
OPENED → CONNECTING → CONNECTED → DISCONNECTED → CLOSED
```

### Key Methods

| Category | Methods |
|----------|---------|
| **SSL** | `enableSSL()`, `isSSL()`, `setSSL(ssl*)`, `setSslCtx(SSL_CTX*)`, `newSslCtx()`, `setHostname(name)` |
| **Timeouts** | `setConnectTimeout(ms)`, `setCloseTimeout(ms)`, `setReadTimeout(ms)`, `setWriteTimeout(ms)`, `setKeepaliveTimeout(ms)` |
| **Heartbeat** | `setHeartbeat(uint32_t interval_ms, HeartBeatFn fn)` |
| **Unpack** | `setUnpack(unpack_setting_t*)` |
| **Address** | `localaddr()` → string, `peeraddr()` → string |
| **Context** | `context()` / `setContext(any)`, `contextPtr<T>()` / `setContextPtr(T*)` — attach arbitrary user data |

---

## WebSocketServer (for reference — HelixScreen is client-only)

```cpp
// Server-side WebSocket
WebSocketService ws_service;
ws_service.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) { };
ws_service.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) { };
ws_service.onclose = [](const WebSocketChannelPtr& channel) { };
ws_service.setPingInterval(30000); // ms

HttpServer server;
server.registerWebSocketService(&ws_service);
```

---

## Shared EventLoopThread Pattern

Multiple WebSocket clients on one event loop thread:

```cpp
// Create one shared loop thread
auto loop_thread = std::make_shared<EventLoopThread>();
loop_thread->start();

// Both clients share the same background thread
auto ws1 = new WebSocketClient(loop_thread->loop());
auto ws2 = new WebSocketClient(loop_thread->loop());

// Connect both
ws1->open("ws://moonraker:7121/websocket");
ws2->open("ws://other-host:8080/ws");
```

Benefits: fewer threads, shared timer wheel, lower memory footprint.
