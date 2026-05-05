# EventLoop & EventLoopThread (evpp C++ layer)

## Class Hierarchy

- `EventLoop` wraps the C `hloop_t` — provides the core event loop abstraction
- `EventLoopThread` owns an `EventLoop` + `std::thread` — manages lifecycle of a loop running in its own thread

Both are in `evpp/EventLoop.h` and `evpp/EventLoopThread.h` (accessible via `hv/EventLoop.h`).

---

## EventLoop

Key methods and thread-safety:

| Method | Signature | Purpose | Thread-Safe |
|--------|-----------|---------|-------------|
| `run()` | `void run()` | Block and run loop forever | No — single thread |
| `stop()` | `void stop()` | Stop the event loop | **Yes** |
| `pause()` | `void pause()` | Pause event processing | No |
| `resume()` | `void resume()` | Resume event processing | No |
| `setTimer` | `TimerID setTimer(uint32_t timeout_ms, TimerCallback cb, uint32_t repeat = INFINITE)` | Add timer (repeat=0 for one-shot, INFINITE for repeating) | No — loop thread only |
| `setTimerInLoop` | `TimerID setTimerInLoop(uint32_t timeout_ms, TimerCallback cb, uint32_t repeat = INFINITE)` | Thread-safe timer add (queues to loop if called from other thread) | **Yes** |
| `setTimeout` | `TimerID setTimeout(uint32_t timeout_ms, TimerCallback cb)` | JS-style one-shot timer | **Yes** |
| `setInterval` | `TimerID setInterval(uint32_t interval_ms, TimerCallback cb)` | JS-style repeating timer | **Yes** |
| `killTimer` | `void killTimer(TimerID timerID)` | Cancel a timer | **Yes** |
| `resetTimer` | `void resetTimer(TimerID timerID, uint32_t timeout_ms = 0)` | Reset timer (restart countdown) | **Yes** |
| `runInLoop` | `void runInLoop(Fn fn)` | Run fn in loop thread. Runs immediately if already in loop thread, otherwise queues | **Yes** |
| `queueInLoop` | `void queueInLoop(Fn fn)` | Queue fn for loop thread — always queues, even if already in loop thread | **Yes** |
| `postEvent` | `void postEvent(EventCallback cb)` | Post custom event to loop | **Yes** |
| `isInLoopThread` | `bool isInLoopThread()` | Check if current thread is the loop thread | — |
| `tid` | `std::thread::id tid()` | Get loop thread ID | — |
| `loop` | `hloop_t* loop()` | Access underlying C loop handle | — |
| `isRunning` | `bool isRunning()` | Check if loop is active | — |
| `context` | `any& context()` | User-defined context data | — |

### TimerID

- Type: `uint64_t`
- Generated from `(thread_id << 32) | incrementing_counter`
- Invalid/nil timer: `INVALID_TIMER_ID` (0)

### Thread-Local Access

```cpp
// Current thread's event loop (NULL if none)
EventLoop* loop = EventLoop::currentThreadEventLoop;
// or
EventLoop* loop = tlsEventLoop();
```

### Connection Counting

- `connectionNum` — atomic counter used internally for load balancing across multiple event loop threads

---

## EventLoopThread

Owns an EventLoop and a `std::thread`. Provides start/stop/join lifecycle.

### Constructor

```cpp
// Creates a new EventLoop internally
EventLoopThread();

// Wraps an existing EventLoop (does NOT create a thread — you manage the loop)
EventLoopThread(EventLoopPtr loop);
```

### Lifecycle Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `start` | `void start(bool wait_thread_started = true, Functor pre = Functor(), Functor post = Functor())` | Start loop thread. `pre` runs in loop thread before `loop()->run()` — return non-zero to abort. `post` runs after loop exits. |
| `stop` | `void stop(bool wait_thread_stopped = false)` | Stop the loop thread. Thread-safe. |
| `join` | `void join()` | Wait for thread to finish |
| `loop` | `EventLoopPtr loop()` | Get the owned EventLoop (shared_ptr) |
| `isRunning` | `bool isRunning()` | Check if thread is running |
| `tid` | `std::thread::id tid()` | Get thread ID |

### Start Parameters

- `wait_thread_started` — block until the loop thread is running (default: true)
- `pre` — functor called **in the loop thread** before `loop()->run()`. If it returns non-zero, start aborts.
- `post` — functor called **in the loop thread** after `loop()->run()` returns

---

## Event Types & Priorities

| Type | Priority | Description |
|------|----------|-------------|
| `HEVENT_TYPE_IO` | 0 | File descriptor I/O (sockets, pipes) |
| `HEVENT_TYPE_TIMEOUT` | 5 | One-shot timers |
| `HEVENT_TYPE_PERIOD` | 3 | Periodic/repeating timers |
| `HEVENT_TYPE_IDLE` | -5 | Background tasks (run when no other events pending) |
| `HEVENT_TYPE_SIGNAL` | 5 | OS signal handling |
| `HEVENT_TYPE_CUSTOM` | configurable | User-defined events |

Higher priority value = processed first. Idle events only fire when nothing else is pending.

---

## Platform I/O Multiplexing

| Platform | Mechanism |
|----------|-----------|
| Linux | `epoll` |
| macOS / BSD | `kqueue` |
| Windows | `IOCP` |
| Fallback | `select` / `poll` |

Selected automatically at compile time via `HAVE_*` macros. No runtime configuration needed.

---

## Shared EventLoopThread Pattern

Multiple clients (WebSocket, HTTP, timers) can share a single event loop thread:

```cpp
// Create one loop thread for all async operations
auto loop_thread = std::make_shared<EventLoopThread>();
loop_thread->start();

// Share the loop across multiple clients
auto ws = new WebSocketClient(loop_thread->loop());
auto http_client = new HttpClient(loop_thread->loop());

// All I/O runs on the same background thread
// Timer-safe operations work across all clients
```

When `WebSocketClient(EventLoopPtr loop)` receives a non-null loop, it uses that loop instead of creating its own `EventLoopThread`.
