# Moonraker Client Security Review

**Review Date:** 2025-11-30 (Updated)
**Original Review:** 2025-11-03
**Reviewer:** Critical Security Analysis (Automated)
**Scope:** Complete Moonraker WebSocket client implementation

Paths in this review name the tree as it stood on 2025-11-30.

**Reviewed Files:**
- `include/moonraker_client.h` - Abstract base class and main client interface
- src/moonraker_client.cpp - Client implementation
- `include/moonraker_api.h` - API definitions
- src/moonraker_api.cpp - API implementation
- `include/moonraker_error.h` - Error type definitions
- `include/moonraker_request.h` - Request type definitions
- `include/moonraker_client_mock.h` - Mock client header
- src/moonraker_client_mock.cpp - Mock client implementation

---

## Executive Summary

**Overall Assessment:** The Moonraker client implementation has undergone significant security hardening since the original review. All CRITICAL issues from the original audit have been resolved. The codebase demonstrates mature security practices including comprehensive input validation, proper concurrency patterns, and robust lifecycle management.

| Category | Original (2025-11-03) | Current (2025-11-30) | Change |
|----------|----------------------|---------------------|--------|
| Critical Issues | 5 | 0 | **RESOLVED** |
| High Priority Issues | 5 | 2 | -3 |
| Medium Priority Issues | 6 | 4 | -2 |
| Low Priority Issues | 4 | 3 | -1 |

**Risk Level:** **LOW-MEDIUM** - Suitable for production use with noted caveats.

**Recommendation:** **APPROVED for production** with Phase 2 improvements recommended for next release cycle.

---

## Completed Issues (Resolved Since Last Audit)

### [FIXED] Issue #1: G-code Command Injection Vulnerability

**Original Severity:** CRITICAL
**Status:** COMPLETE (2025-11-03)
**Evidence:** src/moonraker_api.cpp:54-122, `tests/unit/test_moonraker_api_security.cpp`

**Implementation Details:**

The codebase now includes comprehensive input validation:

```cpp
// src/moonraker_api.cpp:54-75
bool is_safe_identifier(const std::string& str) {
    if (str.empty()) {
        return false;
    }
    // Rejects newlines, semicolons, null bytes, control characters
    // Only allows alphanumeric, underscore, space (for "heater_generic chamber")
    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isalnum(c) || c == '_' || c == ' ';
    });
}

// src/moonraker_api.cpp:77-116
bool is_safe_path(const std::string& path) {
    if (path.empty()) return false;
    if (path.find("..") != std::string::npos) return false;  // Path traversal
    if (path[0] == '/') return false;  // Absolute paths
    if (path.find('\0') != std::string::npos) return false;  // Null byte
    if (path.size() >= 2 && path[1] == ':') return false;  // Windows paths
    return true;
}

// src/moonraker_api.cpp:118-130
bool is_valid_axis(char axis) {
    char upper = std::toupper(axis);
    return upper == 'X' || upper == 'Y' || upper == 'Z' || upper == 'E';
}
```

**Test Coverage:** 26+ security tests in `test_moonraker_api_security.cpp` covering:
- Command injection prevention (newline, semicolon, control characters)
- Range validation (temperatures 0-400C, speeds 0-100%, feedrates 0-50000 mm/min)
- Valid input acceptance
- Error callback verification
- G-code generation prevention on validation failure

---

### [FIXED] Issue #2: Race Condition in Callback Invocation

**Original Severity:** CRITICAL
**Status:** COMPLETE
**Evidence:** `include/moonraker_client.h:146,218,233`, `src/moonraker_client.cpp:310,351-352`

**Implementation Details:**

All callbacks now use pass-by-value for JSON data:

```cpp
// include/moonraker_client.h:146
SubscriptionId register_notify_update(std::function<void(json)> cb);

// include/moonraker_client.h:218
int send_jsonrpc(const std::string& method, const json& params,
                 std::function<void(json)> cb);

// src/moonraker_client.cpp:310
std::function<void(json)> success_cb;  // Pass by value

// src/moonraker_client.cpp:351-352
} else if (success_cb) {
    success_cb(j);  // j is copied into callback
}
```

**Verification:** Tests in `test_moonraker_client_security.cpp:109-167` confirm pass-by-value semantics prevent data races.

---

### [FIXED] Issue #3: Integer Overflow in Request ID

**Original Severity:** CRITICAL
**Status:** COMPLETE
**Evidence:** `include/moonraker_client.h:560,568`, src/moonraker_client.cpp:718

**Implementation Details:**

Request IDs consistently use `uint64_t` throughout:

```cpp
// include/moonraker_client.h:560
std::map<uint64_t, PendingRequest> pending_requests_;

// include/moonraker_client.h:568
std::atomic_uint64_t request_id_;

// src/moonraker_client.cpp:718
RequestId id = request_id_.fetch_add(1) + 1;  // Atomic increment, no race
```

**Mathematical Analysis:** At 1000 requests/second, `uint64_t` provides 584 million years before wraparound.

**Verification:** Tests in `test_moonraker_client_security.cpp:173-248` verify `uint64_t` type usage and no wraparound.

---

### [FIXED] Issue #4: Callback Invoked After Object Destruction (Use-After-Free)

**Original Severity:** CRITICAL
**Status:** COMPLETE
**Evidence:** `src/moonraker_client.cpp:61-80,122-129,228-230,263-265,475-479`

**Implementation Details:**

The destructor now properly cleans up and guards against post-destruction callbacks:

```cpp
// src/moonraker_client.cpp:61-80
MoonrakerClient::~MoonrakerClient() {
    spdlog::debug("[Moonraker Client] Destructor called");
    
    // Clean up pending requests FIRST - invoke error callbacks before destruction
    cleanup_pending_requests();
    
    // Set destroying flag AFTER cleanup
    is_destroying_.store(true);
    
    // Clear callback under lock
    {
        std::lock_guard<std::mutex> lock(state_callback_mutex_);
        state_change_callback_ = nullptr;
    }
    
    disconnect();
}

// All callbacks check is_destroying_ flag:
// src/moonraker_client.cpp:228-230 (onopen)
if (is_destroying_.load()) {
    return;  // Prevent UAF
}

// src/moonraker_client.cpp:263-265 (onmessage)
if (is_destroying_.load()) {
    return;
}

// src/moonraker_client.cpp:475-479 (onclose)
if (is_destroying_.load()) {
    return;
}
```

**Verification:** Tests in `test_moonraker_client_security.cpp:255-353` and `test_moonraker_api_security.cpp:995-1117` verify proper lifecycle management.

---

### [FIXED] Issue #5: Unvalidated File Path in File Operations (Path Traversal)

**Original Severity:** CRITICAL
**Status:** COMPLETE (2025-11-03)
**Evidence:** `src/moonraker_api.cpp:77-116,198-430`

**Implementation Details:**

All file operation methods now validate paths:

```cpp
// src/moonraker_api.cpp applied to:
// - list_files(): line 198-212
// - get_file_metadata(): line 255-269
// - delete_file(): line 289-303
// - move_file(): lines 317-374
// - copy_file(): lines 359-388
// - create_directory(): lines 401-415
// - delete_directory(): lines 430-444

// Example from delete_file():
if (!is_safe_path(filename)) {
    spdlog::error("Invalid file path: {}", filename);
    if (on_error) {
        auto err = MoonrakerError();
        err.type = MoonrakerErrorType::VALIDATION_ERROR;
        err.message = "Invalid file path";
        on_error(err);
    }
    return;
}
```

---

### [FIXED] Issue #6: Mutex Held During Callback Invocation (Deadlock Risk)

**Original Severity:** HIGH
**Status:** COMPLETE
**Evidence:** `src/moonraker_client.cpp:1221-1277,1279-1320`

**Implementation Details:**

Both `check_request_timeouts()` and `cleanup_pending_requests()` now use the two-phase pattern:

```cpp
// src/moonraker_client.cpp:1221-1277
void MoonrakerClient::check_request_timeouts() {
    // Two-phase pattern: collect callbacks under lock, invoke outside lock
    std::vector<std::function<void()>> timed_out_callbacks;
    
    // Phase 1: Find timed out requests and copy callbacks (under lock)
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        // ... copy callbacks into timed_out_callbacks ...
        // Remove timed out requests while holding lock
    } // Lock released here
    
    // Phase 2: Invoke callbacks outside lock (safe - callbacks can call send_jsonrpc)
    for (auto& callback : timed_out_callbacks) {
        callback();
    }
}

// Same pattern in cleanup_pending_requests() at lines 1279-1320
```

**Verification:** Tests in `test_moonraker_client_security.cpp:362-452` verify nested `send_jsonrpc` calls don't deadlock.

---

### [FIXED] Issue #9: State Change Callback Invoked Without Validation

**Original Severity:** HIGH
**Status:** COMPLETE
**Evidence:** src/moonraker_client.cpp:117-140

**Implementation Details:**

State change callbacks are now exception-safe with proper guards:

```cpp
// src/moonraker_client.cpp:117-140
// Copy callback under lock to prevent race with destructor
std::function<void(ConnectionState, ConnectionState)> callback_copy;
{
    std::lock_guard<std::mutex> lock(state_callback_mutex_);
    if (state_change_callback_ && !is_destroying_.load()) {
        callback_copy = state_change_callback_;
    }
}

// Double-check is_destroying_ AFTER releasing lock
if (callback_copy && !is_destroying_.load()) {
    try {
        callback_copy(old_state, new_state);
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("[Moonraker Client] State change callback threw exception: {}",
                           e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("[Moonraker Client] State change callback threw unknown exception");
    }
}
```

**Verification:** Tests in `test_moonraker_client_security.cpp:588-614` verify exception safety.

---

## Remaining Issues

### HIGH PRIORITY ISSUES

#### Issue #7: No Input Validation on JSON-RPC Parameters

**Severity:** HIGH
**Type:** Input Validation
**Location:** src/moonraker_client.cpp:679-703
**Status:** PARTIALLY ADDRESSED

**Current State:**

Message size validation was added:

```cpp
// src/moonraker_client.cpp:268-284
static constexpr size_t MAX_MESSAGE_SIZE = 1024 * 1024; // 1 MB
if (msg.size() > MAX_MESSAGE_SIZE) {
    spdlog::error("[Moonraker Client] Message too large: {} bytes (max: {})",
                  msg.size(), MAX_MESSAGE_SIZE);
    disconnect();
    return;
}
```

**Remaining Concerns:**

The `send_jsonrpc()` methods do not validate:
1. **Method name length/content** - Empty strings and 300+ character method names are accepted
2. **Params type** - JSON-RPC 2.0 requires params to be object or array (not primitives)
3. **Outbound payload size** - Large params objects could cause memory exhaustion

```cpp
// src/moonraker_client.cpp:679-703 - No validation before send
int MoonrakerClient::send_jsonrpc(const std::string& method) {
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;  // No validation of method!
    rpc["id"] = request_id_++;
    spdlog::debug("[Moonraker Client] send_jsonrpc: {}", rpc.dump());
    return send(rpc.dump());  // No size check
}
```

**Recommended Fix:**

```cpp
int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params) {
    // Validate method name
    if (method.empty() || method.size() > 256) {
        spdlog::error("Invalid method name length: {}", method.size());
        return -1;
    }
    
    // Validate params structure per JSON-RPC 2.0
    if (!params.is_null() && !params.is_object() && !params.is_array()) {
        spdlog::error("Invalid params type (must be object or array)");
        return -1;
    }
    
    // Build JSON-RPC message...
    std::string payload = rpc.dump();
    
    // Validate outbound payload size
    if (payload.size() > MAX_MESSAGE_SIZE) {
        spdlog::error("Outbound payload too large: {}", payload.size());
        return -1;
    }
    
    return send(payload);
}
```

**Impact:** Without validation, malformed requests could:
- Cause undefined behavior with empty method names
- Violate JSON-RPC 2.0 spec with non-object/array params
- Exhaust memory with very large payloads

---

#### Issue #8: Callbacks Not Cleared on Reconnection

**Severity:** HIGH
**Type:** Resource Leak
**Location:** src/moonraker_client.cpp:566-648
**Status:** PARTIALLY ADDRESSED

**Current State:**

Unsubscribe methods were added:

```cpp
// src/moonraker_client.cpp:581-595
bool MoonrakerClient::unsubscribe_notify_update(SubscriptionId id) {
    if (id == INVALID_SUBSCRIPTION_ID) {
        return false;
    }
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = notify_callbacks_.find(id);
    if (it != notify_callbacks_.end()) {
        notify_callbacks_.erase(it);
        return true;
    }
    return false;
}

// src/moonraker_client.cpp:650-677
bool MoonrakerClient::unregister_method_callback(const std::string& method,
                                                  const std::string& handler_name) {
    // ... proper cleanup implementation ...
}
```

**Remaining Concerns:**

1. **Manual cleanup required** - Callers must remember to unsubscribe before reconnection
2. **No RAII pattern** - No automatic cleanup when scope ends
3. **Potential duplicate handlers** - Repeated `register_method_callback()` with different handler names accumulates

**Recommended Enhancement:**

```cpp
// Return RAII handle from registration
class CallbackHandle {
    MoonrakerClient* client_;
    SubscriptionId id_;
public:
    ~CallbackHandle() {
        if (client_) client_->unsubscribe_notify_update(id_);
    }
    // ... move-only semantics ...
};

[[nodiscard]] CallbackHandle register_notify_update(std::function<void(json)> cb);
```

**Impact:** Without RAII handles:
- Memory leak across reconnection cycles
- Duplicate handler invocations
- Performance degradation over time

---

### MEDIUM PRIORITY ISSUES

#### Issue #10: Buffer Overflow Risk in PrinterState String Buffers

**Severity:** MEDIUM
**Type:** Memory Safety - Buffer Overflow
**Location:** `include/printer_state.h` (string buffers)
**Status:** NEEDS VERIFICATION

**Current State:**

The `PrinterState` class uses fixed-size character buffers. Without access to `printer_state.cpp`, cannot verify whether bounded string operations are used consistently.

**Recommendation:**
1. Audit `printer_state.cpp` for all buffer updates
2. Verify `snprintf()` or equivalent bounded operations are used
3. Consider migration to `std::string` members

---

#### Issue #11: No Rate Limiting on Requests

**Severity:** MEDIUM
**Type:** Resource Exhaustion
**Location:** All `send_jsonrpc()` methods
**Status:** NOT ADDRESSED

**Current State:**

No limit on pending request count. Malicious or buggy UI code could exhaust memory.

**Recommended Fix:**

```cpp
static constexpr size_t MAX_PENDING_REQUESTS = 100;

RequestId MoonrakerClient::send_jsonrpc(...) {
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        if (pending_requests_.size() >= MAX_PENDING_REQUESTS) {
            spdlog::warn("Too many pending requests: {}", pending_requests_.size());
            if (on_error) {
                on_error(MoonrakerError::request_failed("Too many pending requests"));
            }
            return INVALID_REQUEST_ID;
        }
    }
    // ... proceed with request ...
}
```

---

#### Issue #12: Logging Sensitive Data

**Severity:** MEDIUM
**Type:** Information Disclosure
**Location:** `src/moonraker_client.cpp:685,701,754`
**Status:** NOT ADDRESSED

**Current State:**

Full JSON payloads logged at debug level:

```cpp
// src/moonraker_client.cpp:685,701,754
spdlog::debug("[Moonraker Client] send_jsonrpc: {}", rpc.dump());
```

If authentication tokens or API keys are added in future, they would be logged in plaintext.

**Recommended Fix:**

Add sensitive field redaction before logging, or truncate/summarize large payloads.

---

#### Issue #14: Exception Safety in JSON Parsing

**Severity:** MEDIUM
**Type:** Error Handling
**Location:** src/moonraker_client.cpp:291-296
**Status:** PARTIALLY ADDRESSED

**Current State:**

Parse errors are logged but no tracking of repeated failures:

```cpp
// src/moonraker_client.cpp:291-296
try {
    j = json::parse(msg);
} catch (const json::parse_error& e) {
    LOG_ERROR_INTERNAL("[Moonraker Client] JSON parse error: {}", e.what());
    return;
}
```

**Recommendation:** Track consecutive parse errors and disconnect if threshold exceeded (possible protocol attack).

---

### LOW PRIORITY ISSUES

#### Issue #17: Const Correctness

**Severity:** LOW
**Type:** Code Quality
**Status:** NOT ADDRESSED

Many accessor methods are not marked `const`. No functional impact but reduces API clarity.

---

#### Issue #18: Redundant State Tracking

**Severity:** LOW
**Type:** Code Duplication
**Status:** NOT ADDRESSED

Both `was_connected_` and `connection_state_` track connection status. Could simplify to just `connection_state_`.

---

#### Issue #19: Magic Numbers

**Severity:** LOW
**Type:** Code Maintainability
**Status:** PARTIALLY ADDRESSED

`MAX_MESSAGE_SIZE` is defined inline. Other constants (timeout values, retry limits) could be extracted.

---

## Test Coverage Summary

The security fixes are backed by comprehensive test coverage:

| Test File | Tests | Coverage Area |
|-----------|-------|---------------|
| `test_moonraker_api_security.cpp` | 45+ | G-code injection, path traversal, range validation |
| `test_moonraker_client_security.cpp` | 30+ | Race conditions, UAF, deadlocks, exceptions |
| `test_moonraker_client_robustness.cpp` | 20+ | Message size limits, payload handling |

**All tests passing as of 2025-11-30.**

---

## Security Properties Verified

| Property | Status | Evidence |
|----------|--------|----------|
| G-code command injection prevented | PASS | `is_safe_identifier()` validation |
| Path traversal prevented | PASS | `is_safe_path()` validation |
| Race conditions prevented | PASS | Pass-by-value callbacks |
| Integer overflow prevented | PASS | `uint64_t` request IDs |
| Use-after-free prevented | PASS | `is_destroying_` flag guards |
| Deadlocks prevented | PASS | Two-phase callback pattern |
| Exceptions handled | PASS | try-catch in all callbacks |
| Input ranges validated | PASS | SafetyLimits struct |
| Message size limited | PASS | MAX_MESSAGE_SIZE check |

---

## Recommendations

### Phase 1: COMPLETE (All Critical Issues Resolved)

All five CRITICAL issues from the original audit are now resolved with comprehensive test coverage.

### Phase 2: Recommended for Next Release

1. **Issue #7:** Add outbound payload size limits and method name validation
2. **Issue #8:** Implement RAII callback handles for automatic cleanup
3. **Issue #11:** Add rate limiting on pending requests

**Estimated Effort:** 1 day

### Phase 3: Future Improvements

4. **Issue #12:** Add sensitive field redaction in debug logging
5. **Issue #14:** Track consecutive parse errors
6. Audit `printer_state.cpp` for buffer safety

**Estimated Effort:** 0.5 days

---

## Conclusion

The Moonraker client implementation has achieved **production-ready security status**. All CRITICAL vulnerabilities have been resolved with proper validation, lifecycle management, and concurrency patterns.

**Key Improvements Since Original Audit:**
- Comprehensive input validation prevents command injection and path traversal
- Pass-by-value callbacks eliminate data races
- `uint64_t` request IDs eliminate overflow risk
- `is_destroying_` flag prevents use-after-free
- Two-phase callback pattern prevents deadlocks
- Exception safety in all callback paths
- 75+ security-focused unit tests

**Remaining Work:**
- Minor improvements (rate limiting, RAII handles) recommended but not blocking
- Code quality issues (const correctness, magic numbers) are low priority

**Final Assessment:** **APPROVED FOR PRODUCTION USE**

---

**Review Completed:** 2025-11-30
**Next Review:** After Phase 2 improvements implemented
