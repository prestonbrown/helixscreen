# Moonraker Client Security Review

**Review Date:** 2025-11-03
**Reviewer:** Critical Security Analysis (Automated)
**Scope:** Complete Moonraker WebSocket client implementation

**Reviewed Files:**
- `include/moonraker_client.h` - Abstract base class and main client interface
- `src/moonraker_client.cpp` - Client implementation
- `include/moonraker_api.h` - API definitions
- `src/moonraker_api.cpp` - API implementation
- `include/moonraker_error.h` - Error type definitions
- `include/moonraker_request.h` - Request type definitions
- `include/moonraker_client_mock.h` - Mock client header
- `src/moonraker_client_mock.cpp` - Mock client implementation

---

## Executive Summary

**Overall Assessment:** The Moonraker client implementation demonstrates good architectural design with proper abstraction layers and separation of concerns. Critical command injection and path traversal vulnerabilities have been resolved with comprehensive input validation.

**Critical Issues:** 3 remaining (race condition, integer overflow, use-after-free)
**High Priority Issues:** 5 (deadlock, validation, callbacks, exceptions, buffers)
**Medium Priority Issues:** 6 (rate limiting, logging, timeouts, parsing, bounds checking)
**Low Priority Issues:** 4 (const correctness, code quality)

**Risk Level:** **MEDIUM-HIGH** - Command injection and path traversal fixed. Remaining issues require attention before production.

**Recommendation:** Phase 1 partially complete. Issues #2-4 must be resolved before production use.

---

## COMPLETED ISSUES

### ✅ Issue #1: G-code Command Injection - FIXED (2025-11-03)

**Status:** COMPLETE with comprehensive test coverage (26/26 tests passing)

**Implementation:**
- Added `is_safe_identifier()` validation for all heater/fan/component names
- Added `is_valid_axis()` validation for all axis parameters
- Rejects newlines, semicolons, null bytes, control characters
- All G-code generation methods now validate inputs before execution
- Error callbacks invoked with descriptive messages when validation fails

**Test Coverage:**
- 6 command injection tests (newlines, semicolons, special chars)
- 5 range validation tests (temperature, fan speed, distance, feedrate, position)
- 4 valid input acceptance tests
- 3 error message verification tests
- 8 edge case and G-code safety tests

**Files Modified:**
- `src/moonraker_api.cpp` - Added validation helpers and checks to all methods
- `include/moonraker_api.h` - Added SafetyLimits struct and configuration methods
- `config/helixconfig.json.template` - Added safety_limits configuration section
- `tests/unit/test_moonraker_api_security.cpp` - Comprehensive security tests

### ✅ Issue #5: Path Traversal in File Operations - FIXED (2025-11-03)

**Status:** COMPLETE with path validation

**Implementation:**
- Added `is_safe_path()` validation for all file operation paths
- Rejects parent directory references (..)
- Rejects absolute paths (/ and C:\)
- Rejects null bytes and dangerous characters
- Prevents directory traversal attacks

**Files Modified:**
- `src/moonraker_api.cpp` - Added path validation to list_files, get_file_metadata, delete_file, move_file, copy_file, create_directory, delete_directory

---

## CRITICAL ISSUES (Fix Immediately)

### Issue #1: G-code Command Injection Vulnerability

**Severity:** CRITICAL
**Type:** Security - Command Injection
**Location:** `src/moonraker_api.cpp:289-319`
**CVSS Score:** 9.8 (Critical)

**Description:**

Direct string concatenation of user-controlled input in G-code generation without sanitization creates command injection vulnerability. An attacker can inject arbitrary G-code commands by including newlines or semicolons in parameter values.

**Vulnerable Code:**

```cpp
// Lines 289-298: Heater temperature setting
void MoonrakerAPI::set_temperature(const std::string& heater,
                                   double temperature,
                                   SuccessCallback on_success,
                                   ErrorCallback on_error) {
    std::ostringstream gcode;
    gcode << "SET_HEATER_TEMPERATURE HEATER=" << heater << " TARGET=" << temperature;
    execute_gcode(gcode.str(), on_success, on_error);
}

// Lines 301-315: Fan speed setting
void MoonrakerAPI::set_fan_speed(const std::string& fan,
                                 double speed,
                                 SuccessCallback on_success,
                                 ErrorCallback on_error) {
    std::ostringstream gcode;
    if (fan == "fan") {
        gcode << "M106 S" << fan_value;
    } else {
        gcode << "SET_FAN_SPEED FAN=" << fan << " SPEED=" << (speed / 100.0);
    }
    execute_gcode(gcode.str(), on_success, on_error);
}
```

**Exploit Scenarios:**

```cpp
// Scenario 1: Newline injection
api.set_temperature("extruder\nM104 S999\n", 200, ...);
// Generated G-code:
// SET_HEATER_TEMPERATURE HEATER=extruder
// M104 S999           ← Injected command (set extruder to 999°C)
// TARGET=200

// Scenario 2: Semicolon injection (G-code comment character)
api.set_temperature("extruder ; M104 S999 ;", 200, ...);
// Generated: SET_HEATER_TEMPERATURE HEATER=extruder ; M104 S999 ; TARGET=200
//                                                     ^^^^^^^^^^^^ Injected

// Scenario 3: Multiple malicious commands
api.set_fan_speed("fan\nG1 X999 F9999\nM84\n", 100, ...);
// Moves X axis to destructive position, then disables motors

// Scenario 4: Execute dangerous macros
api.set_temperature("extruder\nRUN_MALICIOUS_MACRO\n", 200, ...);
```

**Impact:**

- **Physical Damage:** Set dangerous temperatures (fire hazard), move axes to destructive positions
- **Hardware Damage:** Damage print surface, hotend, stepper motors, or other components
- **Safety Risk:** Uncontrolled heating or movement could cause injury
- **Arbitrary Code:** Execute malicious printer macros with full printer privileges

**Affected Methods:**

All G-code generation methods are vulnerable:
- `set_temperature()` (line 289) - `heater` parameter
- `set_fan_speed()` (line 301) - `fan` parameter
- `home_axes()` (line 254) - `axes` parameter
- `move_axis()` (line 263) - `axis` parameter
- All motion control methods that generate G-code

**Fix Required:**

```cpp
// Add input validation helper
static bool is_safe_identifier(const std::string& str) {
    // Allow only alphanumeric, underscore, space (for "heater_generic chamber")
    if (str.empty()) {
        return false;
    }

    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isalnum(c) || c == '_' || c == ' ';
    });
}

// Add range validation helper
static bool is_safe_temperature(double temp) {
    return temp >= 0.0 && temp <= 400.0;  // Adjust max based on hardware
}

// Updated set_temperature with validation
void MoonrakerAPI::set_temperature(const std::string& heater,
                                   double temperature,
                                   SuccessCallback on_success,
                                   ErrorCallback on_error) {
    // Validate heater name
    if (!is_safe_identifier(heater)) {
        spdlog::error("Invalid heater name: {}", heater);
        if (on_error) {
            auto err = MoonrakerError();
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid heater name contains illegal characters";
            on_error(err);
        }
        return;
    }

    // Validate temperature range
    if (!is_safe_temperature(temperature)) {
        spdlog::error("Invalid temperature: {}", temperature);
        if (on_error) {
            auto err = MoonrakerError();
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Temperature out of safe range (0-400°C)";
            on_error(err);
        }
        return;
    }

    std::ostringstream gcode;
    gcode << "SET_HEATER_TEMPERATURE HEATER=" << heater << " TARGET=" << temperature;
    execute_gcode(gcode.str(), on_success, on_error);
}

// Updated set_fan_speed with validation
void MoonrakerAPI::set_fan_speed(const std::string& fan,
                                 double speed,
                                 SuccessCallback on_success,
                                 ErrorCallback on_error) {
    // Validate fan name
    if (!is_safe_identifier(fan)) {
        spdlog::error("Invalid fan name: {}", fan);
        if (on_error) {
            auto err = MoonrakerError();
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid fan name contains illegal characters";
            on_error(err);
        }
        return;
    }

    // Validate speed range
    if (speed < 0.0 || speed > 100.0) {
        spdlog::error("Invalid fan speed: {}", speed);
        if (on_error) {
            auto err = MoonrakerError();
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Fan speed must be 0-100%";
            on_error(err);
        }
        return;
    }

    std::ostringstream gcode;
    int fan_value = static_cast<int>(speed * 255.0 / 100.0);

    if (fan == "fan") {
        gcode << "M106 S" << fan_value;
    } else {
        gcode << "SET_FAN_SPEED FAN=" << fan << " SPEED=" << (speed / 100.0);
    }

    execute_gcode(gcode.str(), on_success, on_error);
}
```

**Testing:**

```cpp
// Test cases for validation
ASSERT_FALSE(is_safe_identifier("extruder\nM104"));
ASSERT_FALSE(is_safe_identifier("fan;M106"));
ASSERT_FALSE(is_safe_identifier("heater\r\nG1"));
ASSERT_FALSE(is_safe_identifier(""));
ASSERT_TRUE(is_safe_identifier("extruder"));
ASSERT_TRUE(is_safe_identifier("heater_generic chamber"));
ASSERT_TRUE(is_safe_identifier("fan_0"));

ASSERT_FALSE(is_safe_temperature(-1.0));
ASSERT_FALSE(is_safe_temperature(500.0));
ASSERT_TRUE(is_safe_temperature(0.0));
ASSERT_TRUE(is_safe_temperature(200.0));
ASSERT_TRUE(is_safe_temperature(400.0));
```

---

### Issue #2: Race Condition in Callback Invocation

**Severity:** CRITICAL
**Type:** Concurrency - Data Race
**Location:** `src/moonraker_client.cpp:96-134`

**Description:**

Callbacks are invoked with non-const reference (`json&`) to message parsed in libhv thread, creating data race if UI thread modifies the JSON while libhv thread still holds reference.

**Vulnerable Code:**

```cpp
// Lines 85-134: onmessage handler
onmessage = [this, on_connected, on_disconnected](const std::string& msg) {
    json j;  // ← Stack variable in libhv event loop thread
    try {
        j = json::parse(msg);
    } catch (const json::parse_error& e) {
        spdlog::error("JSON parse error: {}", e.what());
        return;
    }

    // ... later in same function ...
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        // ... find pending request ...
        success_cb = request.success_callback;
    }  // Lock released

    // Callback invoked with non-const reference to stack variable j
    if (success_cb) {
        success_cb(j);  // ← Non-const reference allows modification
    }
    // j still exists here, could be modified by callback while libhv thread
    // continues execution
};
```

**Exploit Scenario:**

```cpp
// Callback modifies JSON while it's still in scope
client.send_jsonrpc("printer.info", {}, [](json& response) {
    response["result"]["malicious"] = "injected data";

    // If another thread or notification handler still references response,
    // we have data corruption or use-after-free

    // Worse: store reference for later use
    global_cached_response = &response;  // ← Dangling reference after callback returns
});
```

**Impact:**

- Data race between libhv thread and LVGL main thread
- Potential use-after-free if callback stores reference
- Memory corruption if JSON is modified concurrently
- Undefined behavior per C++ standard

**Fix Required:**

```cpp
// Option 1: Change all callback signatures to use const json&
// moonraker_client.h
using SuccessCallback = std::function<void(const json&)>;  // Add const

// In onmessage handler, enforce const
if (success_cb) {
    const json& const_ref = j;  // Enforce const
    success_cb(const_ref);
}

// Option 2 (RECOMMENDED): Pass by value to avoid lifetime issues entirely
// moonraker_client.h
using SuccessCallback = std::function<void(json)>;  // Pass by value

// In onmessage handler
if (success_cb) {
    success_cb(j);  // Copy made, safe even if j goes out of scope
}

// moonraker_api.h - update all API callbacks too
using SuccessCallback = std::function<void(json)>;
```

**Apply to all callback types:**

- `MoonrakerClient::SuccessCallback` (line 293 of moonraker_client.h)
- `register_notify_update()` callbacks (line 143-145)
- `register_method_callback()` callbacks (line 167-169)
- All `MoonrakerAPI` success callbacks

**Migration Impact:**

This is a breaking API change. All existing callback registrations need updating:

```cpp
// OLD:
api.get_printer_info([](json& response) { ... });

// NEW:
api.get_printer_info([](json response) { ... });  // Remove &
// or
api.get_printer_info([](const json& response) { ... });  // Add const
```

---

### Issue #3: Integer Overflow in Request ID

**Severity:** CRITICAL
**Type:** Logic Error - Integer Overflow
**Location:** `include/moonraker_client.h:293`, `src/moonraker_client.cpp:97, 243, 259, 277`

**Description:**

Request ID counter uses `atomic_uint64_t` but casts to `uint32_t` when creating and looking up requests, causing wraparound and potential ID collision after 4 billion requests.

**Vulnerable Code:**

```cpp
// moonraker_client.h:293
std::atomic_uint64_t request_id_;  // 64-bit storage

// moonraker_client.cpp:243, 259, 277 - send_jsonrpc methods
rpc["id"] = request_id_++;  // Stores full uint64_t in JSON

// moonraker_client.cpp:97 (onmessage)
uint32_t id = j["id"].get<uint32_t>();  // ← Narrowing conversion!

// moonraker_client.cpp:277
uint32_t id = request_id_;  // ← Implicit narrowing, undefined behavior
std::lock_guard<std::mutex> lock(requests_mutex_);
pending_requests_.insert({id, PendingRequest(...)});
```

**Exploit Scenario:**

```cpp
// After 4,294,967,296 requests, IDs wrap around to 0
// Request with ID 1000 is still pending when counter wraps
// New request gets ID 1000 due to wraparound
// Old callback invoked with new response data

// Timeline:
// T0: Send request ID 1000, waits for response (slow network)
// T1-T2: 4 billion more requests
// T3: Counter wraps, new request gets ID 1000
// T4: New response arrives for ID 1000
// T5: OLD callback from T0 invoked with NEW response from T3
//     → Wrong data, wrong context, potential crash or corruption
```

**Impact:**

- Callback invoked with wrong response data
- Request timeout tracking broken
- Pending request map corruption
- Could lead to executing wrong command with wrong parameters
- Undefined behavior due to narrowing conversion

**Fix Required:**

```cpp
// Option 1: Use consistent uint64_t everywhere (RECOMMENDED)
// moonraker_client.h
std::atomic<uint64_t> request_id_;  // Explicit type

// moonraker_client.cpp
rpc["id"] = request_id_++;  // JSON can handle uint64

uint64_t id = j["id"].get<uint64_t>();  // Match type

std::map<uint64_t, PendingRequest> pending_requests_;  // uint64 keys

// Option 2: Add wraparound detection (if constrained to uint32_t)
uint32_t id = static_cast<uint32_t>(request_id_++ % UINT32_MAX);

std::lock_guard<std::mutex> lock(requests_mutex_);
if (pending_requests_.count(id) > 0) {
    spdlog::error("Request ID collision detected!");
    // Handle collision: either reject request or wait for ID to free up
    return -1;
}
pending_requests_.insert({id, PendingRequest(...)});
```

**Testing:**

```cpp
// Test wraparound behavior
TEST(MoonrakerClient, RequestIDNoCollision) {
    MoonrakerClient client;

    // Simulate wraparound
    client.request_id_ = UINT64_MAX - 10;

    std::set<uint64_t> used_ids;
    for (int i = 0; i < 20; i++) {
        uint64_t id = client.request_id_++;
        ASSERT_EQ(used_ids.count(id), 0) << "ID collision detected";
        used_ids.insert(id);
    }
}
```

---

### Issue #4: Callback Invoked After Object Destruction

**Severity:** CRITICAL
**Type:** Memory Safety - Use-After-Free
**Location:** `src/moonraker_client.cpp:38-39, 76-215`

**Description:**

WebSocket callbacks (`onopen`, `onmessage`, `onclose`) capture `this` pointer and user-provided callbacks, but can be invoked after `MoonrakerClient` destructor completes, leading to use-after-free.

**Vulnerable Code:**

```cpp
// moonraker_client.cpp:38-39
MoonrakerClient::~MoonrakerClient() {
    // NO cleanup of WebSocket callbacks!
    // No call to close() or stop event loop
    // Callbacks still registered in libhv event loop
}

// moonraker_client.cpp:76-82
onopen = [this, on_connected, url]() {  // ← Captures this pointer
    was_connected_ = true;              // ← Access member variable
    set_connection_state(ConnectionState::CONNECTED);  // ← Call method
    on_connected();  // ← May hold references to deleted objects
};

// moonraker_client.cpp:85-172
onmessage = [this, on_connected, on_disconnected](const std::string& msg) {
    // ... uses this->requests_mutex_, this->pending_requests_ ...
};

// moonraker_client.cpp:174-216
onclose = [this, on_disconnected]() {
    // ... uses this->was_connected_, this->cleanup_pending_requests() ...
};
```

**Exploit Scenario:**

```cpp
void connect_and_cancel() {
    auto client = std::make_unique<MoonrakerClient>();

    client->connect("ws://slow-printer:7125/websocket",
        []() { spdlog::info("Connected"); },
        []() { spdlog::info("Disconnected"); });

    // User cancels connection before it completes
    client.reset();  // ← Destructor runs, but libhv still has callbacks

    // Timeline:
    // T0: connect() called, WebSocket connection starts
    // T1: Destructor runs, client object deleted, memory freed
    // T2: WebSocket connection succeeds (slow network)
    // T3: libhv invokes onopen callback
    // T4: Callback accesses freed memory → CRASH or arbitrary code execution
}

// Happens when connection completes AFTER destruction
```

**Impact:**

- **Use-after-free crash** when callbacks invoked after destruction
- **Memory corruption** from accessing freed memory
- **Potential arbitrary code execution** if attacker controls timing
- **Information disclosure** if freed memory contains sensitive data

**Fix Required:**

```cpp
// Solution 1: Clean up callbacks in destructor (MINIMUM FIX)
MoonrakerClient::~MoonrakerClient() {
    spdlog::debug("MoonrakerClient destructor: cleaning up");

    // Clear all callbacks to prevent post-destruction invocation
    onopen = nullptr;
    onmessage = nullptr;
    onclose = nullptr;

    // Close WebSocket connection and wait for completion
    close();

    // Cancel all pending requests (invoke error callbacks)
    cleanup_pending_requests();

    // If using custom event loop, stop it and wait
    // if (custom_loop_) {
    //     custom_loop_->stop();
    //     loop_thread_.join();
    // }
}

// Solution 2: Use std::enable_shared_from_this and weak_ptr (BEST FIX)
// moonraker_client.h
class MoonrakerClient : public WebSocketClient,
                        public std::enable_shared_from_this<MoonrakerClient> {
    // ... rest of class ...
};

// moonraker_client.cpp - use weak_ptr in callbacks
int MoonrakerClient::connect(const char* url,
                             std::function<void()> on_connected,
                             std::function<void()> on_disconnected) {
    onopen = [weak_this = weak_from_this(), on_connected, url]() {
        auto self = weak_this.lock();
        if (!self) {
            spdlog::warn("Connection opened after MoonrakerClient destroyed");
            return;  // Safe: object is gone, just return
        }

        self->was_connected_ = true;
        self->set_connection_state(ConnectionState::CONNECTED);
        on_connected();
    };

    onmessage = [weak_this = weak_from_this(), on_connected, on_disconnected](const std::string& msg) {
        auto self = weak_this.lock();
        if (!self) {
            spdlog::warn("Message received after MoonrakerClient destroyed");
            return;
        }

        // ... rest of handler using self-> instead of this-> ...
    };

    onclose = [weak_this = weak_from_this(), on_disconnected]() {
        auto self = weak_this.lock();
        if (!self) {
            spdlog::warn("Close event after MoonrakerClient destroyed");
            return;
        }

        // ... rest of handler ...
    };

    // ... rest of connect() ...
}

// Note: Using shared_ptr requires changing MoonrakerClient ownership model
// in application code to use std::shared_ptr<MoonrakerClient> instead of
// std::unique_ptr<MoonrakerClient>
```

**API Migration for Solution 2:**

```cpp
// OLD:
auto client = std::make_unique<MoonrakerClient>();

// NEW:
auto client = std::make_shared<MoonrakerClient>();
```

**Testing:**

```cpp
TEST(MoonrakerClient, NoUseAfterFree) {
    std::atomic<bool> callback_invoked{false};

    {
        auto client = std::make_shared<MoonrakerClient>();
        client->connect("ws://localhost:7125/websocket",
            [&]() { callback_invoked = true; },
            []() {});

        // Destroy before connection completes
        client.reset();
    }

    // Wait for connection attempt
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Should not crash, callback should not be invoked
    ASSERT_FALSE(callback_invoked);
}
```

---

### Issue #5: Unvalidated File Path in File Operations

**Severity:** CRITICAL
**Type:** Security - Path Traversal
**Location:** `src/moonraker_api.cpp:93-187`

**Description:**

File operation methods accept unsanitized path strings without validation, allowing directory traversal attacks to read, write, or delete arbitrary files outside the intended directory.

**Vulnerable Code:**

```cpp
// Lines 93-109: delete_file
void MoonrakerAPI::delete_file(const std::string& filename,
                               SuccessCallback on_success,
                               ErrorCallback on_error) {
    json params = {
        {"path", filename}  // ← No validation!
    };
    client_.send_jsonrpc("server.files.delete_file", params,
                        /* ... callbacks ... */);
}

// Lines 111-129: move_file
void MoonrakerAPI::move_file(const std::string& source,
                             const std::string& dest,
                             SuccessCallback on_success,
                             ErrorCallback on_error) {
    json params = {
        {"source", source},  // ← No validation!
        {"dest", dest}       // ← No validation!
    };
    client_.send_jsonrpc("server.files.move_file", params, ...);
}
```

**Exploit Scenarios:**

```cpp
// Scenario 1: Delete system files
api.delete_file("../../etc/passwd", ...);
api.delete_file("../../../home/pi/.ssh/authorized_keys", ...);

// Scenario 2: Delete printer configuration
api.delete_file("../config/printer.cfg", ...);
api.delete_file("../config/moonraker.conf", ...);

// Scenario 3: Move sensitive files to accessible location
api.move_file("../../etc/shadow", "gcodes/shadow.txt", ...);
// Then download via web interface

// Scenario 4: Overwrite system files
api.move_file("gcodes/malicious.cfg", "../../etc/sudoers", ...);

// Scenario 5: Gain shell access
api.move_file("gcodes/attacker_key.pub",
              "../../home/pi/.ssh/authorized_keys", ...);
// Then SSH in with attacker's private key

// Scenario 6: Null byte injection (if backend vulnerable)
api.delete_file("../../etc/passwd\0.gcode", ...);
// Backend may truncate at null byte, deleting /etc/passwd
```

**Impact:**

- **Delete arbitrary files** on system (system config, user data)
- **Overwrite system configuration** (sudoers, SSH config, systemd units)
- **Escape printer sandbox** and access system files
- **Gain shell access** via SSH key injection
- **Exfiltrate sensitive data** by moving files to web-accessible directory
- **Denial of service** by deleting critical system files

**Affected Methods:**

All file operations are vulnerable (lines 35-187):
- `list_files()` (line 35) - `path` parameter
- `get_file_metadata()` (line 69) - `filename` parameter
- `delete_file()` (line 93) - `filename` parameter
- `move_file()` (line 111) - `source` and `dest` parameters
- `copy_file()` (line 131) - `source` and `dest` parameters
- `create_directory()` (line 151) - `path` parameter
- `delete_directory()` (line 169) - `path` parameter, `force` flag

**Fix Required:**

```cpp
// Add path validation helper
static bool is_safe_path(const std::string& path) {
    // Reject empty paths
    if (path.empty()) {
        return false;
    }

    // Reject paths with directory traversal
    if (path.find("..") != std::string::npos) {
        return false;
    }

    // Reject absolute paths
    if (path[0] == '/') {
        return false;
    }

    // Reject null bytes (path truncation attack)
    if (path.find('\0') != std::string::npos) {
        return false;
    }

    // Reject Windows-style absolute paths (if cross-platform)
    if (path.size() >= 2 && path[1] == ':') {
        return false;
    }

    // Additional: reject paths with suspicious characters
    // const std::string dangerous_chars = "<>|*?";
    // if (path.find_first_of(dangerous_chars) != std::string::npos) {
    //     return false;
    // }

    return true;
}

// Updated delete_file with validation
void MoonrakerAPI::delete_file(const std::string& filename,
                               SuccessCallback on_success,
                               ErrorCallback on_error) {
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

    json params = {{"path", filename}};
    client_.send_jsonrpc("server.files.delete_file", params,
        [on_success](json response) {
            if (on_success) on_success(response);
        },
        on_error
    );
}

// Updated move_file with validation
void MoonrakerAPI::move_file(const std::string& source,
                             const std::string& dest,
                             SuccessCallback on_success,
                             ErrorCallback on_error) {
    if (!is_safe_path(source)) {
        spdlog::error("Invalid source path: {}", source);
        if (on_error) {
            auto err = MoonrakerError();
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid source path";
            on_error(err);
        }
        return;
    }

    if (!is_safe_path(dest)) {
        spdlog::error("Invalid destination path: {}", dest);
        if (on_error) {
            auto err = MoonrakerError();
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid destination path";
            on_error(err);
        }
        return;
    }

    json params = {
        {"source", source},
        {"dest", dest}
    };
    client_.send_jsonrpc("server.files.move_file", params,
        [on_success](json response) {
            if (on_success) on_success(response);
        },
        on_error
    );
}
```

**Apply validation to ALL file operation methods.**

**Testing:**

```cpp
// Test path validation
ASSERT_FALSE(is_safe_path(""));
ASSERT_FALSE(is_safe_path("../../etc/passwd"));
ASSERT_FALSE(is_safe_path("/etc/passwd"));
ASSERT_FALSE(is_safe_path("../config/printer.cfg"));
ASSERT_FALSE(is_safe_path("file\0.gcode"));
ASSERT_FALSE(is_safe_path("C:/Windows/System32"));

ASSERT_TRUE(is_safe_path("gcodes/print.gcode"));
ASSERT_TRUE(is_safe_path("config/moonraker.conf"));
ASSERT_TRUE(is_safe_path("subfolder/file.txt"));

// Integration tests
TEST(MoonrakerAPI, RejectsPathTraversal) {
    MoonrakerAPI api(client);
    bool error_invoked = false;

    api.delete_file("../../etc/passwd",
        []() { FAIL() << "Should not succeed"; },
        [&](const MoonrakerError& err) {
            error_invoked = true;
            ASSERT_EQ(err.type, MoonrakerErrorType::VALIDATION_ERROR);
        });

    ASSERT_TRUE(error_invoked);
}
```

---

## HIGH PRIORITY ISSUES

### Issue #6: Mutex Held During Callback Invocation (Deadlock Risk)

**Severity:** HIGH
**Type:** Concurrency - Deadlock
**Location:** `src/moonraker_client.cpp:488-513`

**Description:**

`check_request_timeouts()` holds `requests_mutex_` while invoking error callbacks, creating deadlock risk if callback tries to send new request.

**Vulnerable Code:**

```cpp
// Lines 488-513
void MoonrakerClient::check_request_timeouts() {
    std::lock_guard<std::mutex> lock(requests_mutex_);  // ← Lock acquired

    std::vector<uint32_t> timed_out_ids;

    for (auto& [id, request] : pending_requests_) {
        if (request.is_timed_out()) {
            spdlog::warn("Request {} ({}) timed out after {}ms",
                       id, request.method, request.get_elapsed_ms());

            if (request.error_callback) {
                MoonrakerError error = MoonrakerError::timeout(
                    request.method, request.timeout_ms);
                request.error_callback(error);  // ← Callback invoked with lock held!
            }

            timed_out_ids.push_back(id);
        }
    }

    // Clean up after callbacks
    for (uint32_t id : timed_out_ids) {
        pending_requests_.erase(id);
    }
}  // Lock released
```

**Deadlock Scenario:**

```cpp
// Thread 1: Timeout checker
check_request_timeouts() {
    lock(requests_mutex_);          // ← Lock acquired
    callback(timeout_error);         // ← Invokes user callback
        send_jsonrpc(...) {          // ← Callback tries to send new request
            lock(requests_mutex_);   // ← Tries to acquire same lock
            // DEADLOCK!
        }
}

// Example user code that triggers deadlock:
client.send_jsonrpc("printer.info", {},
    [](json& response) { /* success */ },
    [&client](const MoonrakerError& err) {
        // Timeout handler tries to retry
        spdlog::warn("Request timed out, retrying...");
        client.send_jsonrpc("printer.info", {}, ...);  // ← DEADLOCK
    }
);
```

**Impact:**

- Application hangs indefinitely
- Watchdog timeouts (if enabled)
- User forced to kill application
- Data corruption if timeout during critical operation

**Fix Required:**

```cpp
void MoonrakerClient::check_request_timeouts() {
    // Vector of callbacks to invoke (outside lock)
    std::vector<std::function<void()>> timed_out_callbacks;

    // Phase 1: Find timed out requests and copy callbacks (under lock)
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        std::vector<uint32_t> timed_out_ids;

        for (auto& [id, request] : pending_requests_) {
            if (request.is_timed_out()) {
                spdlog::warn("Request {} ({}) timed out after {}ms",
                           id, request.method, request.get_elapsed_ms());

                if (request.error_callback) {
                    // Create error and copy callback
                    MoonrakerError error = MoonrakerError::timeout(
                        request.method, request.timeout_ms);

                    // Capture callback and error in lambda
                    timed_out_callbacks.push_back(
                        [cb = request.error_callback, error]() {
                            cb(error);
                        }
                    );
                }

                timed_out_ids.push_back(id);
            }
        }

        // Remove timed out requests while still holding lock
        for (uint32_t id : timed_out_ids) {
            pending_requests_.erase(id);
        }
    }  // Lock released here

    // Phase 2: Invoke callbacks outside lock (safe)
    for (auto& callback : timed_out_callbacks) {
        callback();  // Safe: no lock held, callbacks can call send_jsonrpc
    }
}
```

**Same pattern applies to:**

- `cleanup_pending_requests()` (line 515-532) - also holds lock during callback invocation
- Partially fixed in `onmessage` handler (line 85-172) but still risky with notify callbacks

---

### Issue #7: No Input Validation on JSON-RPC Parameters

**Severity:** HIGH
**Type:** Input Validation
**Location:** `src/moonraker_client.cpp:249-263`

**Description:**

`send_jsonrpc()` methods accept arbitrary JSON parameters without validation, allowing malformed requests and potential DoS attacks.

**Vulnerable Code:**

```cpp
// Lines 249-263
int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params) {
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;

    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;  // ← No validation of params structure
    }

    rpc["id"] = request_id_++;

    spdlog::debug("send_jsonrpc: {}", rpc.dump());  // ← Can throw if invalid UTF-8
    return send(rpc.dump());  // ← No size check, no exception handling
}
```

**Issues:**

1. **No params type validation:** JSON-RPC 2.0 requires params to be object or array
2. **No size limit:** Could send gigabyte-sized JSON (DoS attack)
3. **No exception handling:** `json::dump()` can throw if JSON contains invalid UTF-8
4. **No method name validation:** Empty or malformed method names allowed
5. **No logging size limit:** Huge params could overflow log files

**Attack Scenarios:**

```cpp
// DoS via huge payload
json huge_params;
for (int i = 0; i < 1000000; i++) {
    huge_params["key_" + std::to_string(i)] = std::string(1024, 'A');
}
client.send_jsonrpc("printer.info", huge_params);  // ← Gigabytes of data

// Invalid params type (violates JSON-RPC 2.0 spec)
client.send_jsonrpc("printer.info", 42);  // ← params is integer, not object/array

// Invalid UTF-8 causing crash
json params;
params["data"] = "\xFF\xFE\xFD";  // Invalid UTF-8
client.send_jsonrpc("method", params);  // ← dump() throws exception

// Empty method name
client.send_jsonrpc("", {});  // ← Malformed request
```

**Fix Required:**

```cpp
int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params) {
    // Validate method name
    if (method.empty()) {
        spdlog::error("Invalid method name: empty string");
        return -1;
    }

    if (method.size() > 256) {
        spdlog::error("Invalid method name: too long ({} bytes)", method.size());
        return -1;
    }

    // Validate params structure (must be object or array per JSON-RPC 2.0)
    if (!params.is_null() && !params.is_object() && !params.is_array()) {
        spdlog::error("Invalid params type (must be object or array)");
        return -1;
    }

    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;

    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;
    }

    rpc["id"] = request_id_++;

    try {
        std::string payload = rpc.dump();

        // Validate payload size (prevent DoS)
        constexpr size_t MAX_PAYLOAD_SIZE = 1024 * 1024;  // 1MB
        if (payload.size() > MAX_PAYLOAD_SIZE) {
            spdlog::error("Request payload too large: {} bytes (max {})",
                         payload.size(), MAX_PAYLOAD_SIZE);
            return -1;
        }

        // Log with size limit to prevent log overflow
        if (payload.size() <= 1024) {
            spdlog::debug("send_jsonrpc: {}", payload);
        } else {
            spdlog::debug("send_jsonrpc: {} ({} bytes, truncated)",
                         payload.substr(0, 1024), payload.size());
        }

        return send(payload);
    } catch (const std::exception& e) {
        spdlog::error("Failed to serialize JSON-RPC request: {}", e.what());
        return -1;
    }
}
```

**Testing:**

```cpp
TEST(MoonrakerClient, RejectsInvalidParams) {
    MoonrakerClient client;

    // Invalid params types
    ASSERT_EQ(client.send_jsonrpc("method", 42), -1);
    ASSERT_EQ(client.send_jsonrpc("method", "string"), -1);
    ASSERT_EQ(client.send_jsonrpc("method", true), -1);

    // Valid params types
    ASSERT_NE(client.send_jsonrpc("method", json::object()), -1);
    ASSERT_NE(client.send_jsonrpc("method", json::array()), -1);
    ASSERT_NE(client.send_jsonrpc("method", nullptr), -1);
}

TEST(MoonrakerClient, RejectsInvalidMethod) {
    MoonrakerClient client;

    ASSERT_EQ(client.send_jsonrpc("", {}), -1);
    ASSERT_EQ(client.send_jsonrpc(std::string(300, 'A'), {}), -1);
}

TEST(MoonrakerClient, RejectsOversizedPayload) {
    MoonrakerClient client;

    json huge;
    for (int i = 0; i < 100000; i++) {
        huge["key_" + std::to_string(i)] = std::string(100, 'A');
    }

    ASSERT_EQ(client.send_jsonrpc("method", huge), -1);
}
```

---

### Issue #8: Callbacks Not Cleared on Reconnection

**Severity:** HIGH
**Type:** Resource Leak
**Location:** `src/moonraker_client.cpp:218-237`

**Description:**

`register_notify_update()` and `register_method_callback()` accumulate callbacks across reconnections without cleanup, leading to memory leaks and duplicate invocations.

**Vulnerable Code:**

```cpp
// Lines 218-220
void MoonrakerClient::register_notify_update(std::function<void(json&)> cb) {
    notify_callbacks_.push_back(cb);  // ← Never cleared!
}

// Lines 222-237
void MoonrakerClient::register_method_callback(const std::string& method,
                                               const std::string& handler_name,
                                               std::function<void(json&)> cb) {
    auto it = method_callbacks_.find(method);
    if (it == method_callbacks_.end()) {
        std::map<std::string, std::function<void(json&)>> handlers;
        handlers.insert({handler_name, cb});
        method_callbacks_.insert({method, handlers});
    } else {
        // If handler_name already exists, this REPLACES it (good)
        // But if called multiple times with different names, accumulates (bad)
        it->second.insert({handler_name, cb});  // ← Accumulates handlers
    }
}

// onclose handler (lines 174-216) - does NOT clear callbacks
onclose = [this, on_disconnected]() {
    // ... cleanup pending requests ...
    // NO CLEANUP of notify_callbacks_ or method_callbacks_
};
```

**Issue Scenarios:**

```cpp
// Scenario 1: Memory leak across reconnections
for (int i = 0; i < 1000; i++) {
    client.connect(...);
    client.register_notify_update([](json& data) { /* ... */ });
    client.disconnect();
}
// notify_callbacks_ now has 1000 lambdas, even though only last one is relevant

// Scenario 2: Duplicate handler invocations
client.register_method_callback("notify_status_update", "temp_handler",
    [](json& data) { spdlog::info("Handler 1"); });

client.disconnect();
client.connect(...);

client.register_method_callback("notify_status_update", "temp_handler2",
    [](json& data) { spdlog::info("Handler 2"); });

// Next status update invokes BOTH handlers, even though we wanted to replace
```

**Impact:**

- Memory leak: callbacks accumulate indefinitely across reconnections
- Duplicate processing: multiple handlers process same event
- Performance degradation: linear slowdown with each reconnection
- Confusion: old handlers still active after reconnection

**Fix Required:**

```cpp
// Option 1: Add explicit cleanup methods
void MoonrakerClient::clear_callbacks() {
    notify_callbacks_.clear();
    method_callbacks_.clear();
}

// Call in disconnect or provide to user
void MoonrakerClient::disconnect() {
    close();
    // Optional: auto-clear callbacks on disconnect
    // clear_callbacks();
}

// Option 2: Add unregister methods
void MoonrakerClient::unregister_notify_update(size_t index) {
    if (index < notify_callbacks_.size()) {
        notify_callbacks_.erase(notify_callbacks_.begin() + index);
    }
}

void MoonrakerClient::unregister_method_callback(const std::string& method,
                                                  const std::string& handler_name) {
    auto method_it = method_callbacks_.find(method);
    if (method_it != method_callbacks_.end()) {
        method_it->second.erase(handler_name);

        // Clean up empty method entry
        if (method_it->second.empty()) {
            method_callbacks_.erase(method_it);
        }
    }
}

// Option 3 (BEST): Use callback handles for RAII cleanup
class CallbackHandle {
    MoonrakerClient* client_;
    std::string method_;
    std::string handler_;

public:
    CallbackHandle(MoonrakerClient* client, std::string method, std::string handler)
        : client_(client), method_(std::move(method)), handler_(std::move(handler)) {}

    ~CallbackHandle() {
        if (client_) {
            client_->unregister_method_callback(method_, handler_);
        }
    }

    // Non-copyable, movable
    CallbackHandle(const CallbackHandle&) = delete;
    CallbackHandle& operator=(const CallbackHandle&) = delete;
    CallbackHandle(CallbackHandle&&) = default;
    CallbackHandle& operator=(CallbackHandle&&) = default;
};

// Updated registration returns handle
CallbackHandle MoonrakerClient::register_method_callback(
    const std::string& method,
    const std::string& handler_name,
    std::function<void(json&)> cb) {

    auto it = method_callbacks_.find(method);
    if (it == method_callbacks_.end()) {
        std::map<std::string, std::function<void(json&)>> handlers;
        handlers.insert({handler_name, cb});
        method_callbacks_.insert({method, handlers});
    } else {
        it->second.insert_or_assign(handler_name, cb);  // Replace if exists
    }

    return CallbackHandle(this, method, handler_name);
}

// Usage with RAII:
{
    auto handle = client.register_method_callback("notify_status_update",
                                                   "temp_handler",
                                                   [](json& data) { /* ... */ });
    // ...
}  // handle destroyed, callback automatically unregistered
```

---

### Issue #9: State Change Callback Invoked Without Validation

**Severity:** HIGH
**Type:** Exception Safety
**Location:** `src/moonraker_client.cpp:62-65`

**Description:**

`state_change_callback_` invoked without exception handling or validation, potentially corrupting connection state if callback throws.

**Vulnerable Code:**

```cpp
// Lines 53-65
void MoonrakerClient::set_connection_state(ConnectionState new_state) {
    if (new_state == connection_state_) {
        return;
    }

    ConnectionState old_state = connection_state_;
    connection_state_ = new_state;

    // Invoke callback WITHOUT exception handling
    if (state_change_callback_) {
        state_change_callback_(old_state, new_state);  // ← Can throw
    }

    spdlog::debug("Connection state: {} -> {}",
                 static_cast<int>(old_state),
                 static_cast<int>(new_state));
}
```

**Issues:**

1. **No exception safety:** If callback throws, state is updated but callback failed
2. **No validation:** Callback could be destroyed/invalid
3. **Called during destruction:** If `set_connection_state()` called in destructor path

**Impact:**

- State corruption: `connection_state_` set but callback failed to process
- Exception propagation: crashes if callback throws and caller doesn't catch
- Use-after-free: callback could capture deleted objects

**Fix Required:**

```cpp
void MoonrakerClient::set_connection_state(ConnectionState new_state) {
    if (new_state == connection_state_) {
        return;
    }

    ConnectionState old_state = connection_state_;
    connection_state_ = new_state;

    spdlog::debug("Connection state: {} -> {}",
                 static_cast<int>(old_state),
                 static_cast<int>(new_state));

    // Invoke callback with exception safety
    if (state_change_callback_) {
        try {
            state_change_callback_(old_state, new_state);
        } catch (const std::exception& e) {
            spdlog::error("State change callback threw exception: {}", e.what());
            // State already changed, can't rollback
            // Log error and continue
        } catch (...) {
            spdlog::error("State change callback threw unknown exception");
        }
    }
}
```

---

### Issue #10: Buffer Overflow in PrinterState String Buffers

**Severity:** HIGH (if implementation uses unsafe string ops)
**Type:** Memory Safety - Buffer Overflow
**Location:** `include/printer_state.h:142-145`

**Description:**

Fixed-size character buffers in `PrinterState` could overflow if updated with unsafe string operations.

**Vulnerable Code:**

```cpp
// printer_state.h:142-145
char print_filename_buf_[256];
char print_state_buf_[32];
char homed_axes_buf_[8];
char connection_message_buf_[128];
```

**Potential Issues (requires verification in `printer_state.cpp`):**

```cpp
// UNSAFE (buffer overflow if filename > 255 chars):
strcpy(print_filename_buf_, filename.c_str());

// UNSAFE (no null termination if exactly 256 chars):
strncpy(print_filename_buf_, filename.c_str(), 256);

// SAFE:
strncpy(print_filename_buf_, filename.c_str(), sizeof(print_filename_buf_) - 1);
print_filename_buf_[sizeof(print_filename_buf_) - 1] = '\0';

// SAFER:
snprintf(print_filename_buf_, sizeof(print_filename_buf_), "%s", filename.c_str());
```

**Verification Required:**

Check `printer_state.cpp` implementation of `update_from_notification()` to ensure all buffer updates use bounded string operations.

**Recommended Fix:**

```cpp
// In printer_state.cpp - example for filename update
if (notification.contains("filename")) {
    std::string filename = notification["filename"].get<std::string>();

    // Truncate if too long (with ellipsis indicator)
    if (filename.size() >= sizeof(print_filename_buf_)) {
        filename = filename.substr(0, sizeof(print_filename_buf_) - 4) + "...";
    }

    // Safe copy with guaranteed null termination
    strncpy(print_filename_buf_, filename.c_str(), sizeof(print_filename_buf_) - 1);
    print_filename_buf_[sizeof(print_filename_buf_) - 1] = '\0';

    lv_subject_copy_string(&print_filename_, print_filename_buf_);
}

// Better: Use std::string instead of char buffers
// printer_state.h
std::string print_filename_;
std::string print_state_;
std::string homed_axes_;
std::string connection_message_;

// No buffer overflow risk with std::string
```

---

## MEDIUM PRIORITY ISSUES

### Issue #11: No Rate Limiting on Requests

**Severity:** MEDIUM
**Type:** Resource Exhaustion
**Location:** All `send_jsonrpc()` methods

**Description:**

No rate limiting or request queue depth limit. Malicious or buggy UI code could spam thousands of requests, exhausting memory and overwhelming Moonraker server.

**Impact:**

- Exhaust `pending_requests_` map memory
- DoS Moonraker server
- Overflow request ID counter faster
- Network congestion

**Fix:**

```cpp
// Add to MoonrakerClient class
static constexpr size_t MAX_PENDING_REQUESTS = 100;

int MoonrakerClient::send_jsonrpc(const std::string& method,
                                  const json& params,
                                  SuccessCallback on_success,
                                  ErrorCallback on_error,
                                  uint32_t timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);

        if (pending_requests_.size() >= MAX_PENDING_REQUESTS) {
            spdlog::warn("Too many pending requests ({}), rejecting new request: {}",
                        pending_requests_.size(), method);

            // Invoke error callback
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::REQUEST_FAILED;
                err.message = "Too many pending requests";
                on_error(err);
            }

            return -1;
        }
    }

    // Proceed with request
    // ...
}
```

---

### Issue #12: Logging Sensitive Data

**Severity:** MEDIUM
**Type:** Information Disclosure
**Location:** `src/moonraker_client.cpp:245, 261`

**Description:**

Full JSON payload logged at debug level could leak sensitive data if authentication tokens or API keys added in future.

**Vulnerable Code:**

```cpp
// Lines 245, 261
spdlog::debug("send_jsonrpc: {}", rpc.dump());
```

**Risk:**

If API key or authentication token added to future requests, it will be logged in plaintext to debug logs.

**Fix:**

```cpp
// Add redaction helper
static json redact_sensitive_fields(const json& j) {
    json redacted = j;

    // Redact known sensitive field names
    const std::vector<std::string> sensitive_keys = {
        "api_key", "apikey", "token", "password", "secret",
        "auth", "authorization", "credentials"
    };

    for (const auto& key : sensitive_keys) {
        if (redacted.contains(key)) {
            redacted[key] = "[REDACTED]";
        }

        // Also check in params
        if (redacted.contains("params") && redacted["params"].contains(key)) {
            redacted["params"][key] = "[REDACTED]";
        }
    }

    return redacted;
}

// Update logging
spdlog::debug("send_jsonrpc: {}", redact_sensitive_fields(rpc).dump());
```

---

### Issue #13: No Timeout for Connection Establishment

**Severity:** MEDIUM
**Type:** Reliability
**Location:** `src/moonraker_client.cpp:69-216`

**Description:**

`connection_timeout_ms_` configured but never actually used. WebSocket connection has no timeout and could hang indefinitely.

**Issue:**

```cpp
// constructor sets timeout (line 43)
connection_timeout_ms_ = timeout_ms;

// But connect() never uses it!
int MoonrakerClient::connect(const char* url, ...) {
    // ... no setTimeout() call ...
}
```

**Fix:**

```cpp
int MoonrakerClient::connect(const char* url,
                             std::function<void()> on_connected,
                             std::function<void()> on_disconnected) {
    // ... existing setup ...

    // Set connection timeout (libhv API)
    setTimeout(connection_timeout_ms_);

    // ... rest of connection setup ...

    return WebSocketClient::open(url);
}
```

---

### Issue #14: Exception Safety in JSON Parsing

**Severity:** MEDIUM
**Type:** Error Handling
**Location:** `src/moonraker_client.cpp:88-93`

**Description:**

JSON parsing catches exception but doesn't invoke error callbacks or handle repeated parse errors.

**Current Code:**

```cpp
// Lines 88-93
try {
    j = json::parse(msg);
} catch (const json::parse_error& e) {
    spdlog::error("JSON parse error: {}", e.what());
    return;  // ← Just returns, no error callback
}
```

**Issues:**

- No error callback for the failed request
- No tracking of repeated parse errors (could indicate attack)
- No automatic disconnection on persistent parse failures

**Improved Handling:**

```cpp
// Add member variable
std::atomic<size_t> parse_error_count_{0};

// In onmessage handler
try {
    j = json::parse(msg);
    parse_error_count_ = 0;  // Reset on successful parse
} catch (const json::parse_error& e) {
    spdlog::error("JSON parse error: {}", e.what());
    spdlog::debug("Invalid JSON: {}", msg);

    // Track repeated errors
    parse_error_count_++;

    // If this was a response to a request, try to extract ID and invoke error callback
    // (requires partial parsing or ID extraction from malformed JSON)

    // Trigger disconnection on repeated parse errors (possible attack)
    if (parse_error_count_ > 5) {
        spdlog::error("Too many consecutive parse errors ({}), closing connection",
                     parse_error_count_);
        close();
    }

    return;
}
```

---

### Issue #15: No Bounds Checking on Array Access

**Severity:** MEDIUM
**Type:** Robustness
**Location:** `src/moonraker_api.cpp:428-480`

**Description:**

`parse_file_list()` directly accesses JSON array elements without validating structure or types, could crash on malformed responses.

**Vulnerable Code:**

```cpp
// Lines 438-451: Parsing dirs array
if (result.contains("dirs")) {
    for (const auto& dir : result["dirs"]) {  // ← No check if "dirs" is array
        FileInfo info;
        if (dir.contains("dirname")) {
            info.filename = dir["dirname"].get<std::string>();  // ← No type check
            // ...
        }
        // ...
    }
}
```

**Issues:**

- No check if `result["dirs"]` is actually an array
- No check if `dir` is actually an object
- No type validation before `get<std::string>()`
- Could crash or return garbage on malformed response

**Better Implementation:**

```cpp
if (result.contains("dirs") && result["dirs"].is_array()) {
    for (const auto& dir : result["dirs"]) {
        // Validate dir is an object
        if (!dir.is_object()) {
            spdlog::warn("Skipping malformed dir entry (not an object)");
            continue;
        }

        FileInfo info;

        // Validate and extract dirname
        if (dir.contains("dirname") && dir["dirname"].is_string()) {
            info.filename = dir["dirname"].get<std::string>();
        } else {
            spdlog::warn("Dir entry missing 'dirname' field, skipping");
            continue;
        }

        // Validate and extract modified time
        if (dir.contains("modified") && dir["modified"].is_number()) {
            info.modified = dir["modified"].get<double>();
        } else {
            info.modified = 0.0;  // Default value
        }

        // Validate and extract size
        if (dir.contains("size") && dir["size"].is_number_unsigned()) {
            info.size = dir["size"].get<size_t>();
        } else {
            info.size = 0;  // Default value
        }

        info.is_directory = true;
        files.push_back(info);
    }
} else if (result.contains("dirs")) {
    spdlog::warn("'dirs' field is not an array, skipping");
}
```

**Apply same validation to:**
- `result["files"]` parsing (line 454-467)
- All JSON extraction in response handlers

---

### Issue #16: Mock Client Missing Security Checks

**Severity:** MEDIUM
**Type:** Security Policy Violation
**Location:** `src/moonraker_client_mock.cpp`

**Description:**

Mock implementation doesn't validate that it's actually running in test mode, could be accidentally used in production.

**Issue:**

Per project policy in CLAUDE.md Issue #4, mock implementations must NEVER be automatically used in production. Mock client should verify test mode on construction.

**Fix:**

```cpp
// moonraker_client_mock.cpp
#include "runtime_config.h"  // Add runtime config header

MoonrakerClientMock::MoonrakerClientMock(PrinterType type)
    : printer_type_(type) {

    // Verify we're in test mode (enforce policy)
    const auto& config = get_runtime_config();
    if (!config.is_test_mode()) {
        spdlog::critical("MoonrakerClientMock instantiated in production mode!");
        spdlog::critical("Mock clients require --test command-line flag");
        throw std::runtime_error("Mock client requires --test flag");
    }

    spdlog::info("[MoonrakerClientMock] Created with printer type: {}",
                static_cast<int>(type));
}
```

**Verification:**

```cpp
// Test that mock rejects production mode
TEST(MoonrakerClientMock, RejectsProductionMode) {
    // Temporarily clear test mode
    auto& config = get_runtime_config();
    config.set_test_mode(false);

    // Should throw
    ASSERT_THROW({
        MoonrakerClientMock mock(PrinterType::CARTESIAN);
    }, std::runtime_error);

    // Restore test mode
    config.set_test_mode(true);
}
```

---

## LOW PRIORITY / CODE QUALITY ISSUES

### Issue #17: Const Correctness

**Severity:** LOW
**Type:** Code Quality

**Description:**

Many methods that don't modify state are not marked `const`, reducing API clarity and preventing use with const objects.

**Affected Methods:**

```cpp
// moonraker_client.h
std::vector<std::string> get_heaters();  // Should be const (line 179)
std::vector<std::string> get_sensors();  // Should be const (line 184)
ConnectionState get_connection_state();  // Should be const (line 199)

// printer_state.h - all subject getters should be const
lv_subject_t* get_nozzle_temp_subject();  // Line 78
lv_subject_t* get_bed_temp_subject();     // Line 83
// ... etc ...
```

**Fix:**

```cpp
std::vector<std::string> get_heaters() const;
std::vector<std::string> get_sensors() const;
ConnectionState get_connection_state() const;

lv_subject_t* get_nozzle_temp_subject() const;
lv_subject_t* get_bed_temp_subject() const;
```

---

### Issue #18: Redundant State Tracking

**Severity:** LOW
**Type:** Code Duplication

**Description:**

Both `was_connected_` and `connection_state_` track connection status. Could simplify to just `connection_state_`.

**Current:**

```cpp
bool was_connected_ = false;
std::atomic<ConnectionState> connection_state_{ConnectionState::DISCONNECTED};
```

**Suggested:**

```cpp
// Remove was_connected_, use:
bool was_previously_connected() const {
    auto state = connection_state_.load();
    return state != ConnectionState::DISCONNECTED &&
           state != ConnectionState::CONNECTING;
}
```

---

### Issue #19: Magic Numbers

**Severity:** LOW
**Type:** Code Maintainability

**Description:**

Hardcoded magic numbers throughout code should be named constants.

**Examples:**

```cpp
// moonraker_error.h:121
-32601  // Should be: JSON_RPC_METHOD_NOT_FOUND

// moonraker_api.cpp:306
255  // Should be: GCODE_FAN_PWM_MAX

// Suggested in Issue #1
400  // Should be: MAX_SAFE_TEMPERATURE_CELSIUS
```

**Fix:**

```cpp
// moonraker_error.h or moonraker_client.h
namespace MoonrakerConstants {
    constexpr int JSON_RPC_METHOD_NOT_FOUND = -32601;
    constexpr int JSON_RPC_INVALID_REQUEST = -32600;
    constexpr int JSON_RPC_INTERNAL_ERROR = -32603;

    constexpr int GCODE_FAN_PWM_MAX = 255;
    constexpr double MAX_SAFE_TEMPERATURE_CELSIUS = 400.0;
    constexpr double MIN_SAFE_TEMPERATURE_CELSIUS = 0.0;
}
```

---

### Issue #20: Inconsistent Error Handling

**Severity:** LOW
**Type:** Code Consistency

**Description:**

Some methods check if `on_error` callback exists before invoking, others always invoke (will crash if nullptr).

**Examples:**

```cpp
// moonraker_api.cpp:104 - checks before invoke (SAFE)
if (on_error) {
    on_error(err);
}

// moonraker_api.cpp:207 - always invokes (UNSAFE if nullptr)
on_error(err);  // ← Crash if callback is nullptr
```

**Fix:**

Choose one pattern and apply consistently:

```cpp
// Option 1: Always check (defensive)
if (on_error) {
    on_error(err);
}

// Option 2: Document that callbacks are required (efficient)
// In header:
/**
 * @param on_error Error callback (must not be nullptr)
 */
void some_method(..., ErrorCallback on_error);

// In implementation, assert:
assert(on_error && "Error callback must not be nullptr");
on_error(err);
```

**Recommendation:** Use Option 1 (always check) for better robustness, or make callbacks optional parameters with default values.

---

## SUMMARY & RECOMMENDATIONS

### Critical Path to Production

**Phase 1: CRITICAL - Address Immediately (Before ANY Production Use)**

1. ✅ **Issue #1:** Add input validation to all G-code generation methods - **COMPLETED 2025-11-03**
2. **Issue #2:** Change all callbacks to `const json&` or pass-by-value
3. **Issue #3:** Use consistent `uint64_t` for request IDs
4. **Issue #4:** Fix destructor to clear callbacks and prevent use-after-free
5. ✅ **Issue #5:** Add path validation to all file operation methods - **COMPLETED 2025-11-03**

**Estimated Effort:** 1-2 days remaining (40% complete)
**Risk if not fixed:** Memory corruption, race conditions, crashes

---

**Phase 2: HIGH - Fix Before Production Release**

6. **Issue #6:** Refactor timeout checking to invoke callbacks outside mutex
7. **Issue #7:** Add JSON-RPC parameter validation and size limits
8. **Issue #8:** Implement callback cleanup or RAII handles
9. **Issue #9:** Add exception handling around state change callbacks
10. **Issue #10:** Verify buffer safety in `printer_state.cpp`, add bounds checking

**Estimated Effort:** 1-2 days
**Risk if not fixed:** Application hangs, memory leaks, crashes

---

**Phase 3: MEDIUM - Address During Next Development Cycle**

11-16. Rate limiting, logging redaction, timeout configuration, JSON validation, mock checks

**Estimated Effort:** 1 day
**Risk if not fixed:** DoS attacks, information disclosure, poor reliability

---

**Phase 4: LOW - Address During Maintenance**

17-20. Const correctness, code cleanup, magic number extraction, consistency

**Estimated Effort:** 0.5 days
**Risk if not fixed:** Reduced code quality, harder maintenance

---

### Testing Recommendations

1. **Security Testing:**
   - Fuzzing JSON-RPC responses with malformed data
   - Path traversal attack simulation
   - Command injection attempts in all G-code methods
   - Concurrent stress testing for race conditions

2. **Integration Testing:**
   - Reconnection stress test (100+ connect/disconnect cycles)
   - Request timeout verification
   - Callback lifecycle validation
   - Mock vs real backend behavior parity

3. **Unit Testing:**
   - Input validation for all public APIs
   - Buffer overflow prevention verification
   - Exception safety testing
   - Concurrent callback invocation

---

### Long-Term Architecture Recommendations

1. **Consider moving to std::shared_ptr ownership** for MoonrakerClient to enable safe weak_ptr callbacks

2. **Implement request builder pattern** to enforce validation at compile-time:
   ```cpp
   auto request = MoonrakerRequest()
       .method("printer.info")
       .param("objects", json::array({"extruder"}))
       .timeout(5000)
       .build();  // ← Validation happens here
   client.send(request);
   ```

3. **Add rate limiting and backpressure** at API level, not just client level

4. **Consider Protocol Buffers** instead of JSON for better type safety and performance

5. **Implement circuit breaker pattern** for automatic connection failure recovery

---

## Appendix: Related Files to Review

The following files were not included in this review but may contain related issues:

- `printer_state.cpp` - Verify buffer handling (Issue #10)
- `runtime_config.h` - Mock validation requirements (Issue #16)
- Any UI code that calls MoonrakerAPI methods - Verify they handle errors properly

---

**Review Completed:** 2025-11-03
**Last Updated:** 2025-11-03 - Issues #1 and #5 resolved
**Next Review:** After remaining Phase 1 fixes (#2, #3, #4) are implemented
