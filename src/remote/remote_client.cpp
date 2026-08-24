// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file remote_client.cpp
 * @brief The helixctl client, folded into helix-screen as a subcommand.
 *
 * Sends JSON-RPC 2.0 commands over a Unix domain socket to HelixScreen's
 * RemoteControlServer. Reached via `helix-screen ctl <cmd>` / `helix-screen
 * repl` (main.cpp dispatches to remote_client_main before the app starts).
 * No separate binary. Dev/test-only — compiled out of release builds.
 *
 * Usage:
 *   helix-screen ctl ping
 *   helix-screen ctl navigate controls
 *   helix-screen ctl screenshot
 *   helix-screen ctl status
 *   helix-screen repl                  # Interactive REPL with line editing
 */

#include "remote_client.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "hv/json.hpp"

// POSIX headers
#include "unix_socket_transport.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// Line editing for REPL
#include "linenoise.h"

static const char* PROGRAM_NAME = "helix-screen ctl";

// ---------------------------------------------------------------------------
// Help text
// ---------------------------------------------------------------------------
//
// ONE table drives both `ctl help` and the REPL's `help`. They used to be two
// hand-maintained printf blocks and had already drifted: `geom` and `get_const`
// were absent from the CLI listing, and the REPL listing was missing those plus
// cd/pwd/back/demo/focus/scroll/text/wake/press/move/release. A command you
// cannot discover is a command nobody uses.
//
// `detail` is the extra context the CLI prints under the summary; the REPL
// prints the summary alone to keep the in-session listing scannable.

struct HelpEntry {
    const char* section; ///< nullptr continues the previous section
    const char* usage;   ///< invocation, as typed
    const char* summary; ///< one line, shown everywhere
    const char* detail;  ///< extra CLI-only lines ("\n"-separated), or nullptr
};

static const HelpEntry HELP[] = {
    {"Navigation (fs metaphor)", "help, ?", "Show this help", nullptr},
    {nullptr, "ping", "Health check", nullptr},
    {nullptr, "navigate <panel>", "Go to a base panel", nullptr},
    {nullptr, "cd <container>", "Move the working directory (REPL only, no UI side effect)",
     "Targets resolve relative to it, so `cd` to a row and then type\n"
     "short names. `cd ..` goes up one container, or pops the overlay\n"
     "at its root; `cd` alone returns to whatever is frontmost.\n"
     "To make something HAPPEN, use click — cd never fires an event.\n"
     "One-shot: scope a single command with -C <path>."},
    {nullptr, "go_back, back", "Pop the current overlay/level", nullptr},
    {nullptr, "current, pwd", "Show the panel, overlay stack, and working directory", nullptr},
    {nullptr, "resolve <target>", "Print the absolute locator a target resolves to", nullptr},
    {nullptr, "list_panels", "List available panels", nullptr},
    {nullptr, "list_components", "List every registered XML component (live registry)", nullptr},
    {nullptr, "list_callbacks", "List every registered event-callback name", nullptr},
    {nullptr, "screenshot [path] [--target W] [--stable]", "Capture the screen",
     "a .png path encodes PNG; default is a timestamped .bmp in the\n"
     "runtime dir. --target crops to a widget's bounds; --stable polls\n"
     "until pixels stop changing (see `freeze`)"},
    {nullptr, "status", "Show panel, connection state, printer status", nullptr},
    {nullptr, "wake", "Reset idle timer / dismiss the screensaver", nullptr},
    {nullptr, "demo <name>", "Show a sample-data overlay unreachable in mock mode",
     "preflight-check, runout-modal, lock-screen, print-status,\n"
     "print-tune, ams, camera"},

    {"Subjects", "get <subject>", "Read current value of named subject", nullptr},
    {nullptr, "set <subject> <value>", "Set subject value", nullptr},
    {nullptr, "list_subjects", "List all registered subjects", nullptr},
    {nullptr, "wait_for <subject> <value> [--timeout N]", "Block until subject matches value",
     "default 30s"},

    {"Widgets (targets: a name, 'name[k]' for the k-th match, a 'glob*' pattern,\n"
     "  a path relative to the cwd, or an absolute @path from `ls`)",
     "ls, describe_screen [target]", "List on-screen widgets: name, path, type, actions",
     "No target lists the working directory. With a target, only that\n"
     "widget's subtree; with a pattern ('row_*'), every match. Quote it."},
    {nullptr, "geom <target> [depth]", "Why a widget is the size it is",
     "Absolute x/y/w/h, content box, DECLARED vs computed size, flex\n"
     "grow, hidden/scrollable state. [depth] recurses into children.\n"
     "Use this instead of measuring pixels off a screenshot."},
    {nullptr, "get_const [scope] [name]", "Read XML design-token constants",
     "#space_lg, #nav_width, ... No name lists the whole scope;\n"
     "scope defaults to 'globals'."},
    {nullptr, "text <target>", "Read a widget's text (label/textarea/dropdown)",
     "Descends into a composite (e.g. a button) to find it."},
    {nullptr, "state <target>", "Read a widget's LVGL states and flags",
     "checked/disabled/focused/pressed as booleans + an active-states\n"
     "array, plus hidden/clickable/scrollable flags. Descends a composite\n"
     "row to its control, matching what click/set_value act on. A hidden\n"
     "widget still resolves — assert bind_flag_if contracts here."},
    {nullptr, "click <target>", "Click (toggles switches/checkboxes)",
     "On a composite row, descends to the control inside it."},
    {nullptr, "set_value <target> <v>", "Set value (slider, switch, dropdown, textarea)", nullptr},
    {nullptr, "scroll <target> [dx dy]", "Scroll into view, or by a delta", nullptr},
    {nullptr, "focus <target>", "Focus a widget through its input group",
     "Raises the on-screen keyboard for a textarea (click does not)."},

    {"Synthetic pointer (drives LVGL's real input pipeline — gestures, long-press,\n"
     "scroll-vs-tap — unlike `click`, which sends a bare widget event)",
     "press <x> <y>", "Put the pointer down at x,y", nullptr},
    {nullptr, "move <x> <y>", "Move it (a drag while pressed, a hover while not)", nullptr},
    {nullptr, "release [x y]", "Lift it, at x,y if given, else where it is",
     "e.g. long-press: press 100 300; sleep 0.6; release"},

    {"Diagnostics & lifecycle", "wait_idle [--timeout N]",
     "Block until UpdateQueue and HttpExecutor are both quiet",
     "default 10s. Best-effort — see docs/devel/HELIXCTL.md for what\n"
     "it cannot see"},
    {nullptr, "freeze", "Stop animations + pause periodic timers for a reproducible capture",
     "skips the update-queue and display-refresh timers so the channel\n"
     "stays alive"},
    {nullptr, "unfreeze", "Reverse freeze: resume paused timers, re-enable animations", nullptr},
    {nullptr, "log [-n N]", "Tail the app's in-memory log ring", "default 50 lines"},
    {nullptr, "shutdown, quit", "Ask the running app to exit", nullptr},
    {nullptr, "reset", "Return to home with no overlays/modals",
     "cheaper than a reboot — see docs/devel/HELIXCTL.md"},

    {"Scenarios", "scenario <name>", "Apply named mock scenario", nullptr},
    {nullptr, "list_scenarios", "List available mock scenarios", nullptr},

    {"Interactive", "repl", "Interactive REPL with line editing and history", nullptr},
};

static constexpr int HELP_USAGE_WIDTH = 26;

/// Render the shared table. `verbose` adds each entry's `detail` lines, which
/// the REPL omits so its in-session listing stays scannable.
static void print_help_table(bool verbose) {
    for (const HelpEntry& e : HELP) {
        if (e.section) {
            printf("\n%s:\n", e.section);
        }
        int pad = HELP_USAGE_WIDTH - 2 - static_cast<int>(strlen(e.usage));
        if (pad > 0) {
            printf("  %s%*s%s\n", e.usage, pad, "", e.summary);
        } else {
            // Usage too long to share a line — summary goes underneath.
            printf("  %s\n%*s%s\n", e.usage, HELP_USAGE_WIDTH, "", e.summary);
        }
        if (verbose && e.detail) {
            for (const char* p = e.detail; p;) {
                const char* nl = strchr(p, '\n');
                int len = nl ? static_cast<int>(nl - p) : static_cast<int>(strlen(p));
                printf("%*s%.*s\n", HELP_USAGE_WIDTH, "", len, p);
                p = nl ? nl + 1 : nullptr;
            }
        }
    }
}

static void print_usage() {
    printf("Usage: %s [options] <command> [args...]\n", PROGRAM_NAME);
    print_help_table(/*verbose=*/true);
    printf("\nOptions:\n");
    printf("  -s, --socket <path>     Socket path (default: auto-detect)\n");
    printf("  -C, --cwd <path>        Resolve this command's target inside <path>\n");
    printf("                          (the one-shot equivalent of the REPL's cd)\n");
    printf("  --json                  Emit the raw JSON-RPC result/error (one-shot only)\n");
    printf("  -h, --help              Show this help\n");
    printf("\nSocket path resolution:\n");
    printf("  1. --socket <path>  (explicit)\n");
    printf("  2. $XDG_RUNTIME_DIR/helixscreen-control.sock\n");
    printf("  3. /tmp/helixscreen-control.sock\n");
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

static std::string resolve_socket_path(const std::string& override_path) {
    if (!override_path.empty()) {
        return override_path;
    }

    const char* xdg_runtime = getenv("XDG_RUNTIME_DIR");
    const std::string dir =
        (xdg_runtime && xdg_runtime[0] != '\0') ? std::string(xdg_runtime) : std::string("/tmp");
    const std::string well_known = dir + "/helixscreen-control.sock";

    // Liveness, not mere existence: a crashed instance leaves the file behind, and
    // connecting to it fails with a confusing error instead of finding the app that
    // is actually running on a pid-suffixed path.
    if (helix::UnixSocketTransport::path_is_live(well_known)) {
        return well_known;
    }

    std::vector<std::string> instances = helix::UnixSocketTransport::discover_instances(dir);
    if (instances.size() == 1) {
        return instances[0];
    }
    if (instances.size() > 1) {
        // Guessing here would silently drive the wrong app — exactly the failure
        // this whole change exists to prevent. Make the user choose.
        fprintf(stderr, "Error: several HelixScreen instances are running. Pick one with -s:\n");
        for (const std::string& path : instances) {
            fprintf(stderr, "  --socket %s\n", path.c_str());
        }
        exit(1);
    }

    return well_known; // Nothing running; report against the expected path.
}

static int connect_to_server(const std::string& socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;

    if (socket_path.length() >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Error: Socket path too long\n");
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            fprintf(stderr, "Error: No HelixScreen instance found at %s\n", socket_path.c_str());
            fprintf(stderr, "Is HelixScreen running with --remote or --test?\n");
        } else {
            fprintf(stderr, "Error: Failed to connect: %s\n", strerror(errno));
        }
        close(fd);
        return -1;
    }

    return fd;
}

static bool write_all(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool send_request(int fd, const nlohmann::json& request) {
    std::string data = request.dump() + "\n";
    if (!write_all(fd, data.c_str(), data.length())) {
        fprintf(stderr, "Error: Failed to send request: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static std::string read_response(int fd, int timeout_sec = 30) {
    std::string buffer;
    char chunk[4096];

    // Set a read timeout
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (true) {
        ssize_t n = read(fd, chunk, sizeof(chunk) - 1);
        if (n <= 0) {
            break;
        }
        chunk[n] = '\0';
        buffer.append(chunk, static_cast<size_t>(n));

        // Check for newline (end of response)
        if (buffer.find('\n') != std::string::npos) {
            break;
        }
    }

    // Trim trailing newline
    while (!buffer.empty() && buffer.back() == '\n') {
        buffer.pop_back();
    }

    return buffer;
}

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

static int g_request_id = 1;

/// When set, the one-shot client emits the raw JSON-RPC `result` instead of the
/// human-formatted rendering. Errors from the server go to stderr as the raw
/// `error` object. Client-side usage errors stay human-readable — a caller that
/// mistyped a command name needs prose, not a protocol object.
static bool g_json_output = false;

/// The working directory every target resolves against: an absolute locator, or
/// empty for "wherever the app currently is" (the server's reported root_path —
/// the frontmost panel, overlay or modal).
///
/// The REPL builds this up with `cd`. A one-shot `ctl` invocation has nowhere to
/// persist one between processes, so it takes `-C <path>` instead.
static std::string g_cwd;

/// The root_path last reported by the server. When a navigate/click swaps the
/// frontmost thing on screen, any cwd pointing into the old subtree is stale —
/// watching this is how the cwd follows navigation without the client having to
/// know which commands navigate.
static std::string g_last_root;

static nlohmann::json build_request(const std::string& method,
                                    const nlohmann::json& params = nlohmann::json::object()) {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"id", g_request_id++}};
}

/// Handle a JSON-RPC response. Returns 0 on success, 1 on error.
/// If out_result is provided, stores the result JSON there instead of printing.
static int handle_response(const std::string& raw_response, nlohmann::json* out_result = nullptr) {
    if (raw_response.empty()) {
        fprintf(stderr, "Error: Empty response from server\n");
        return 1;
    }

    try {
        auto response = nlohmann::json::parse(raw_response);

        if (response.contains("error")) {
            const auto& error = response["error"];
            if (g_json_output) {
                // dump() is safe for any JSON type, including the non-object
                // errors the human path guards against below.
                fprintf(stderr, "%s\n", error.dump().c_str());
            } else {
                // A server is free to send a non-object error ("error": null, or a bare
                // string). value() throws type_error.306 on those, so probe the type
                // first. The message is held in a named string because c_str() on the
                // value() temporary only survives to the end of the full expression.
                std::string message = "Unknown error";
                int code = -1;
                if (error.is_object()) {
                    const auto it = error.find("message");
                    if (it != error.end() && it->is_string())
                        message = it->get<std::string>();
                    const auto ic = error.find("code");
                    if (ic != error.end() && ic->is_number_integer())
                        code = ic->get<int>();
                } else if (error.is_string()) {
                    message = error.get<std::string>();
                }
                fprintf(stderr, "Error: %s (code %d)\n", message.c_str(), code);
            }
            return 1;
        }

        if (response.contains("result")) {
            if (out_result) {
                *out_result = response["result"];
            } else if (g_json_output) {
                // Raw, single-line. Callers pipe this to jq or json.loads.
                printf("%s\n", response["result"].dump().c_str());
            } else {
                auto& result = response["result"];
                if (result.is_string()) {
                    printf("%s\n", result.get<std::string>().c_str());
                } else if (result.is_object() && result.contains("lines") &&
                           result["lines"].is_array()) {
                    // Log tail: print the lines as lines. JSON-escaped log output
                    // is unreadable and defeats piping it to grep.
                    for (const auto& line : result["lines"]) {
                        if (line.is_string()) {
                            printf("%s\n", line.get<std::string>().c_str());
                        }
                    }
                } else {
                    printf("%s\n", result.dump(2).c_str());
                }
            }
            return 0;
        }

        fprintf(stderr, "Error: Unexpected response format\n");
        return 1;

    } catch (const nlohmann::json::parse_error& e) {
        fprintf(stderr, "Error: Failed to parse response: %s\n", e.what());
        return 1;
    } catch (const nlohmann::json::exception& e) {
        // Backstop for type_error/out_of_range from a well-formed but
        // unexpectedly shaped response. This runs on a main() path, so an
        // escaping exception is std::terminate rather than a usable message.
        fprintf(stderr, "Error: Unexpected response content: %s\n", e.what());
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Command building from tokens (shared by CLI and REPL)
// ---------------------------------------------------------------------------

/// Try to parse a string as int, return JSON int on success, JSON string otherwise.
/// Only treats the token as an int when the ENTIRE string is numeric — otherwise
/// std::stoi("3dbenchy") would silently send 3, and a digit-leading string value
/// (e.g. a filename) could never be set on a string subject/widget.
static nlohmann::json parse_value(const std::string& val_str) {
    try {
        size_t consumed = 0;
        int v = std::stoi(val_str, &consumed);
        if (consumed == val_str.size()) {
            return v;
        }
    } catch (...) {
        // fall through to string
    }
    return val_str;
}

/// Build a target param object for click/set/scroll. "@s/3/1" (or a bare
/// "s/3/1") addresses a widget by its describe_screen path, unique even for
/// duplicate names; anything else is treated as a widget name.
///
/// The current working directory rides along as "scope": a plain name is looked
/// up inside it rather than across the whole screen, which is what makes
/// `cd`-ing to a row and then typing short names work.
static nlohmann::json target_param(const std::string& t) {
    nlohmann::json params;
    if (!t.empty() && t[0] == '@') {
        params = {{"path", t.substr(1)}};
    } else if (helix::is_bare_path(t)) {
        // Accept a bare absolute path too, so a locator pasted straight out of
        // `ls` works without remembering the '@'. Widget names never contain
        // '/', so the "s/1/2" / "t/0" shape is unambiguous.
        params = {{"path", t}};
    } else if (t.find('/') != std::string::npos) {
        // Same reasoning, one level down: a name cannot contain '/', so
        // "row_theme/toggle" is a locator relative to the cwd.
        params = {{"path", t}};
    } else {
        params = {{"name", t}};
    }
    if (!g_cwd.empty()) {
        params["scope"] = g_cwd;
    }
    return params;
}

/// Build a JSON-RPC request from a command + args vector.
/// Returns empty json on parse error (with error printed to stderr).
static nlohmann::json build_request_from_tokens(const std::vector<std::string>& tokens) {
    if (tokens.empty())
        return {};

    const auto& cmd = tokens[0];

    if (cmd == "ping") {
        return build_request("ping");
    } else if (cmd == "cd") {
        // `cd` moves the working directory, which only a REPL session can hold
        // between commands. The REPL intercepts it before this point; reaching
        // here means the one-shot client, where there is nowhere to put it.
        fprintf(stderr, "Error: cd only means something in the REPL, which keeps a working "
                        "directory between commands.\n"
                        "       One-shot: scope a single command with -C <path>, e.g.\n"
                        "         helix-screen ctl -C s/settings_list click 'toggle[1]'\n"
                        "       To change what is on screen, use navigate or click.\n");
        return {};
    } else if (cmd == "navigate") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: navigate requires a panel name\n");
            return {};
        }
        return build_request("navigate", {{"panel", tokens[1]}});
    } else if (cmd == "resolve") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: resolve requires a widget name or @path\n");
            return {};
        }
        return build_request("resolve", target_param(tokens[1]));
    } else if (cmd == "go_back" || cmd == "back") {
        return build_request("go_back");
    } else if (cmd == "list_panels") {
        return build_request("list_panels");
    } else if (cmd == "list_components") {
        return build_request("list_components");
    } else if (cmd == "list_callbacks") {
        return build_request("list_callbacks");
    } else if (cmd == "current" || cmd == "pwd") {
        return build_request("get_current");
    } else if (cmd == "screenshot") {
        // Optional destination; a .png suffix asks the app to encode PNG.
        // Optional --target <widget> crops to its bounds; --stable polls
        // until the pixels stop changing before capturing (see `freeze`).
        nlohmann::json params = nlohmann::json::object();
        size_t i = 1;
        if (i < tokens.size() && tokens[i].compare(0, 2, "--") != 0) {
            params["path"] = tokens[i];
            i++;
        }
        for (; i < tokens.size(); i++) {
            if (tokens[i] == "--stable") {
                params["stable"] = true;
            } else if (tokens[i] == "--target" && i + 1 < tokens.size()) {
                params["target"] = tokens[++i];
            }
        }
        // --target names a widget, so it resolves inside the cwd like any other
        // target. (params["path"] here is the output FILE, not a locator.)
        if (params.contains("target") && !g_cwd.empty()) {
            params["scope"] = g_cwd;
        }
        return build_request("screenshot", params);
    } else if (cmd == "shutdown" || cmd == "quit" || cmd == "exit") {
        // In the REPL, quit/exit are intercepted before dispatch (they leave the
        // REPL); reaching here means the one-shot client, where stopping the app
        // is the only sensible reading.
        return build_request("shutdown");
    } else if (cmd == "log") {
        nlohmann::json params = nlohmann::json::object();
        for (size_t i = 1; i < tokens.size(); i++) {
            if ((tokens[i] == "-n" || tokens[i] == "--lines") && i + 1 < tokens.size()) {
                params["lines"] = std::atoi(tokens[i + 1].c_str());
                i++;
            } else {
                params["lines"] = std::atoi(tokens[i].c_str());
            }
        }
        return build_request("log", params);
    } else if (cmd == "status") {
        return build_request("status");
    } else if (cmd == "demo") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: demo requires a name (preflight-check, runout-modal, ams, "
                            "lock-screen, print-status, print-tune, ams, camera)\n");
            return {};
        }
        return build_request("demo", {{"name", tokens[1]}});
    } else if (cmd == "get") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: get requires a subject name\n");
            return {};
        }
        return build_request("get", {{"name", tokens[1]}});
    } else if (cmd == "set") {
        if (tokens.size() < 3) {
            fprintf(stderr, "Error: set requires a subject name and value\n");
            return {};
        }
        return build_request("set", {{"name", tokens[1]}, {"value", parse_value(tokens[2])}});
    } else if (cmd == "list_subjects") {
        return build_request("list_subjects");
    } else if (cmd == "wait_for") {
        if (tokens.size() < 3) {
            fprintf(stderr, "Error: wait_for requires a subject name and value\n");
            return {};
        }
        nlohmann::json params = {{"name", tokens[1]}, {"value", tokens[2]}};
        for (size_t i = 3; i < tokens.size(); i++) {
            if ((tokens[i] == "--timeout" || tokens[i] == "-t") && i + 1 < tokens.size()) {
                params["timeout"] = std::atoi(tokens[i + 1].c_str());
                i++;
            }
        }
        return build_request("wait_for", params);
    } else if (cmd == "wait_idle") {
        nlohmann::json params = nlohmann::json::object();
        for (size_t i = 1; i < tokens.size(); i++) {
            if ((tokens[i] == "--timeout" || tokens[i] == "-t") && i + 1 < tokens.size()) {
                params["timeout"] = std::atof(tokens[i + 1].c_str());
                i++;
            }
        }
        return build_request("wait_idle", params);
    } else if (cmd == "freeze") {
        return build_request("freeze");
    } else if (cmd == "unfreeze") {
        return build_request("unfreeze");
    } else if (cmd == "click") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: click requires a widget name or @path\n");
            return {};
        }
        return build_request("click", target_param(tokens[1]));
    } else if (cmd == "focus") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: focus requires a widget name or @path\n");
            return {};
        }
        return build_request("focus", target_param(tokens[1]));
    } else if (cmd == "press" || cmd == "move") {
        if (tokens.size() < 3) {
            fprintf(stderr, "Error: %s requires x and y coordinates\n", cmd.c_str());
            return {};
        }
        nlohmann::json params;
        params["x"] = std::atoi(tokens[1].c_str());
        params["y"] = std::atoi(tokens[2].c_str());
        return build_request(cmd == "press" ? "pointer_press" : "pointer_move", params);
    } else if (cmd == "release") {
        nlohmann::json params;
        // Optional coordinates; without them the pointer lifts where it already is.
        if (tokens.size() >= 3) {
            params["x"] = std::atoi(tokens[1].c_str());
            params["y"] = std::atoi(tokens[2].c_str());
        }
        return build_request("pointer_release", params);
    } else if (cmd == "long_press") {
        if (tokens.size() < 3) {
            fprintf(stderr, "Error: long_press requires x and y coordinates\n");
            return {};
        }
        nlohmann::json params;
        params["x"] = std::atoi(tokens[1].c_str());
        params["y"] = std::atoi(tokens[2].c_str());
        // Optional hold in ms; the server defaults it from the live long-press
        // threshold rather than a number duplicated here.
        if (tokens.size() >= 4) {
            params["hold_ms"] = std::atoi(tokens[3].c_str());
        }
        return build_request("pointer_long_press", params);
    } else if (cmd == "set_value") {
        if (tokens.size() < 3) {
            fprintf(stderr, "Error: set_value requires a widget name/@path and value\n");
            return {};
        }
        nlohmann::json p = target_param(tokens[1]);
        p["value"] = parse_value(tokens[2]);
        return build_request("set_widget_value", p);
    } else if (cmd == "scenario") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: scenario requires a name\n");
            return {};
        }
        return build_request("scenario", {{"name", tokens[1]}});
    } else if (cmd == "list_scenarios") {
        return build_request("list_scenarios");
    } else if (cmd == "wake" || cmd == "screensaver") {
        // `wake` or `screensaver off` — reset the idle timer / dismiss the saver.
        return build_request("wake");
    } else if (cmd == "describe_screen" || cmd == "ls") {
        // `ls` lists the working directory (the whole screen when there is
        // none); `ls <name|@path>` scopes to a subtree.
        if (tokens.size() >= 2) {
            return build_request("describe_screen", target_param(tokens[1]));
        }
        nlohmann::json params = nlohmann::json::object();
        if (!g_cwd.empty()) {
            params["scope"] = g_cwd;
        }
        return build_request("describe_screen", params);
    } else if (cmd == "scroll") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: scroll requires a widget name/@path [dx dy]\n");
            return {};
        }
        nlohmann::json params = target_param(tokens[1]);
        if (tokens.size() >= 4) {
            params["dx"] = std::atoi(tokens[2].c_str());
            params["dy"] = std::atoi(tokens[3].c_str());
        }
        return build_request("scroll", params);
    } else if (cmd == "text") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: text requires a widget name/@path\n");
            return {};
        }
        return build_request("text", target_param(tokens[1]));
    } else if (cmd == "state") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: state requires a widget name/@path\n");
            return {};
        }
        return build_request("state", target_param(tokens[1]));
    } else if (cmd == "set_text") {
        if (tokens.size() < 3) {
            fprintf(stderr, "Error: set_text requires a widget name/@path and text\n");
            return {};
        }
        nlohmann::json p = target_param(tokens[1]);
        // Join the rest so an unquoted multi-word string still arrives intact
        // from the REPL, where the line is split on spaces.
        std::string text = tokens[2];
        for (size_t i = 3; i < tokens.size(); ++i) {
            text += " " + tokens[i];
        }
        p["text"] = text;
        return build_request("set_text", p);
    } else if (cmd == "geom") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: geom requires a widget name/@path [depth]\n");
            return {};
        }
        nlohmann::json params = target_param(tokens[1]);
        if (tokens.size() >= 3) {
            params["depth"] = std::atoi(tokens[2].c_str());
        }
        return build_request("geom", params);
    } else if (cmd == "get_const") {
        if (tokens.size() < 2) {
            fprintf(stderr, "Error: get_const requires <name>, <scope> <name>, or <scope> \n");
            return {};
        }
        nlohmann::json params;
        if (tokens.size() >= 3) {
            params["scope"] = tokens[1];
            params["name"] = tokens[2];
        } else if (tokens[1].rfind("@", 0) == 0) {
            // @scope with no name dumps every const in that scope.
            params["scope"] = tokens[1].substr(1);
        } else {
            params["name"] = tokens[1];
        }
        return build_request("get_const", params);
    } else if (cmd == "reset") {
        return build_request("reset");
    }

    fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
    return {};
}

// ---------------------------------------------------------------------------
// REPL
// ---------------------------------------------------------------------------

// All known commands for tab completion
static const char* REPL_COMMANDS[] = {"ping",
                                      "navigate",
                                      "cd",
                                      "go_back",
                                      "back",
                                      "list_panels",
                                      "list_components",
                                      "list_callbacks",
                                      "current",
                                      "pwd",
                                      "resolve",
                                      "screenshot",
                                      "status",
                                      "wake",
                                      "demo",
                                      "get",
                                      "set",
                                      "list_subjects",
                                      "wait_for",
                                      "wait_idle",
                                      "freeze",
                                      "unfreeze",
                                      "ls",
                                      "describe_screen",
                                      "click",
                                      "set_value",
                                      "focus",
                                      "press",
                                      "move",
                                      "release",
                                      "long_press",
                                      "scroll",
                                      "scenario",
                                      "list_scenarios",
                                      "help",
                                      "refresh",
                                      "log",
                                      "shutdown",
                                      "reset",
                                      "geom",
                                      "text",
                                      "state",
                                      "set_text",
                                      "get_const",
                                      "quit",
                                      "exit",
                                      nullptr};

// Cached subject names for tab completion (populated lazily)
static std::vector<std::string> g_cached_subjects;
static std::vector<std::string> g_cached_scenarios;
static std::vector<std::string> g_cached_panels;

/// Socket the REPL session is attached to, so tab completion can ask the app
/// what is currently on screen. Empty outside a REPL.
static std::string g_repl_socket;

// Defined below with the other transport helpers; completion needs it here.
static bool query_result(const std::string& socket_path, const nlohmann::json& request,
                         nlohmann::json& out);

// Distinct widget names in the working directory, for completing a target.
// Duplicates are collapsed: offering the same word three times is noise, and
// the index suffix that tells them apart comes from `ls`, not from here.
static std::vector<std::string> widget_names_here() {
    std::vector<std::string> names;
    if (g_repl_socket.empty()) {
        return names;
    }
    nlohmann::json params = nlohmann::json::object();
    if (!g_cwd.empty()) {
        params["scope"] = g_cwd;
    }
    nlohmann::json r;
    if (!query_result(g_repl_socket, build_request("describe_screen", params), r)) {
        return names;
    }
    if (!r.contains("widgets") || !r["widgets"].is_array()) {
        return names;
    }
    std::set<std::string> seen;
    for (const auto& w : r["widgets"]) {
        const std::string name = w.value("name", std::string());
        if (!name.empty() && seen.insert(name).second) {
            names.push_back(name);
        }
    }
    return names;
}

static void repl_completion(const char* buf, linenoiseCompletions* lc) {
    std::string input(buf);

    // Find the first space to determine if we're completing a command or an argument
    size_t space_pos = input.find(' ');

    if (space_pos == std::string::npos) {
        // Completing a command name
        for (int i = 0; REPL_COMMANDS[i]; i++) {
            if (strncmp(buf, REPL_COMMANDS[i], input.length()) == 0) {
                linenoiseAddCompletion(lc, REPL_COMMANDS[i]);
            }
        }
    } else {
        // Completing an argument
        std::string cmd = input.substr(0, space_pos);
        std::string partial = input.substr(space_pos + 1);

        // Strip leading spaces from partial
        while (!partial.empty() && partial[0] == ' ')
            partial.erase(0, 1);

        const std::vector<std::string>* candidates = nullptr;
        std::vector<std::string> live; // widget names, which no cache can hold

        if (cmd == "get" || cmd == "set" || cmd == "wait_for") {
            candidates = &g_cached_subjects;
        } else if (cmd == "scenario") {
            candidates = &g_cached_scenarios;
        } else if (cmd == "navigate") {
            candidates = &g_cached_panels;
        } else if (cmd == "cd" || cmd == "ls" || cmd == "click" || cmd == "focus" ||
                   cmd == "text" || cmd == "set_text" || cmd == "state" || cmd == "geom" ||
                   cmd == "set_value" || cmd == "scroll" || cmd == "resolve") {
            // Widget names are re-read every time rather than cached: the tree
            // changes under the REPL constantly (navigation, hot reload, the
            // printer's own state), so a cache would offer completions for
            // widgets that are no longer on screen.
            live = widget_names_here();
            candidates = &live;
        }

        if (candidates) {
            for (const auto& c : *candidates) {
                if (c.compare(0, partial.length(), partial) == 0) {
                    std::string completion = cmd + " " + c;
                    linenoiseAddCompletion(lc, completion.c_str());
                }
            }
        }
    }
}

static char* repl_hints(const char* buf, int* color, int* bold) {
    std::string input(buf);
    *color = 90; // dark gray
    *bold = 0;

    if (input == "navigate")
        return strdup(" <panel>");
    if (input == "get")
        return strdup(" <subject>");
    if (input == "set")
        return strdup(" <subject> <value>");
    if (input == "click")
        return strdup(" <widget>");
    if (input == "text")
        return strdup(" <widget>");
    if (input == "state")
        return strdup(" <widget>");
    if (input == "set_text")
        return strdup(" <widget> <text>");
    if (input == "set_value")
        return strdup(" <widget> <value>");
    if (input == "scenario")
        return strdup(" <name>");
    if (input == "wait_for")
        return strdup(" <subject> <value> [--timeout N]");
    if (input == "wait_idle")
        return strdup(" [--timeout N]");
    if (input == "screenshot")
        return strdup(" [path] [--target W] [--stable]");

    return nullptr;
}

static void repl_free_hints(void* ptr) {
    free(ptr);
}

/// Split a string into tokens by whitespace
static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string token;
    bool in_quotes = false;
    char quote_char = 0;

    for (char c : line) {
        if (in_quotes) {
            if (c == quote_char) {
                in_quotes = false;
            } else {
                token += c;
            }
        } else if (c == '"' || c == '\'') {
            in_quotes = true;
            quote_char = c;
        } else if (c == ' ' || c == '\t') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) {
        tokens.push_back(token);
    }
    return tokens;
}

/// Send a request and get the parsed result. Returns true on success.
static bool repl_rpc(int fd, const std::string& method, const nlohmann::json& params,
                     nlohmann::json& result) {
    auto request = build_request(method, params);
    if (!send_request(fd, request))
        return false;
    auto raw = read_response(fd, 30);
    return handle_response(raw, &result) == 0;
}

/// Populate tab-completion caches by querying the server
static void repl_populate_caches(int fd) {
    nlohmann::json result;

    // Cache subject names
    if (repl_rpc(fd, "list_subjects", {}, result) && result.is_array()) {
        g_cached_subjects.clear();
        for (const auto& entry : result) {
            if (entry.contains("name")) {
                g_cached_subjects.push_back(entry["name"].get<std::string>());
            }
        }
    }

    // Cache scenario names
    if (repl_rpc(fd, "list_scenarios", {}, result) && result.is_array()) {
        g_cached_scenarios.clear();
        for (const auto& entry : result) {
            if (entry.contains("name")) {
                g_cached_scenarios.push_back(entry["name"].get<std::string>());
            } else if (entry.is_string()) {
                g_cached_scenarios.push_back(entry.get<std::string>());
            }
        }
    }

    // Cache panel names
    if (repl_rpc(fd, "list_panels", {}, result) && result.is_array()) {
        g_cached_panels.clear();
        for (const auto& entry : result) {
            if (entry.is_string()) {
                g_cached_panels.push_back(entry.get<std::string>());
            }
        }
    }
}

static void repl_print_help() {
    printf("Commands:");
    print_help_table(/*verbose=*/false);
    printf("\n  quit, exit, Ctrl-D      Exit REPL\n");
    printf("  refresh                 Reload tab-completion caches\n");
    printf("\nTab completion works for commands, subjects, panels, and scenarios.\n");
    printf("Emacs keybindings: Ctrl-A/E, Ctrl-B/F, Ctrl-K/U, Ctrl-W, Ctrl-D, etc.\n");
}

/// Resolve history file path: $XDG_DATA_HOME/helixscreen/helixctl_history
/// or ~/.local/share/helixscreen/helixctl_history
static std::string resolve_history_path() {
    std::string dir;
    const char* xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0') {
        dir = std::string(xdg_data) + "/helixscreen";
    } else {
        const char* home = getenv("HOME");
        if (home) {
            dir = std::string(home) + "/.local/share/helixscreen";
        } else {
            return ""; // Can't determine home
        }
    }
    return dir + "/helixctl_history";
}

// Fetch the JSON-RPC result for a request; returns false on any failure.
static bool query_result(const std::string& socket_path, const nlohmann::json& request,
                         nlohmann::json& out) {
    int fd = connect_to_server(socket_path);
    if (fd < 0) {
        return false;
    }
    bool ok = false;
    if (send_request(fd, request)) {
        auto raw = read_response(fd, 10);
        ok = (handle_response(raw, &out) == 0);
    }
    close(fd);
    return ok;
}

// The cwd with the active root trimmed off, which is how the prompt shows it:
// the overlay is already named in the breadcrumb, so repeating its whole
// locator is noise. Returns the cwd unchanged when it sits outside the root.
static std::string cwd_below_root(const std::string& root) {
    if (g_cwd.empty() || root.empty()) {
        return g_cwd;
    }
    if (g_cwd.rfind(root + "/", 0) == 0) {
        return g_cwd.substr(root.size() + 1);
    }
    return g_cwd;
}

// Filesystem-style breadcrumb prompt from the server's current location plus
// the working directory, e.g. "controls / motion_overlay / jog_grid > ".
static std::string build_prompt(const std::string& socket_path) {
    nlohmann::json r;
    if (!query_result(socket_path, build_request("get_current"), r)) {
        return "helixctl(offline)> ";
    }

    // A navigate/click that swapped the frontmost thing on screen leaves any
    // cwd pointing into a subtree that no longer exists. Dropping it here is
    // what makes the cwd follow navigation without the client needing to know
    // which commands navigate — including a click that turned out to open an
    // overlay, which is not knowable in advance.
    const std::string root = r.value("root_path", std::string());
    if (root != g_last_root) {
        g_last_root = root;
        g_cwd.clear();
    }

    std::string crumb = r.value("panel", std::string("?"));
    if (r.contains("overlays") && r["overlays"].is_array()) {
        for (auto& o : r["overlays"]) {
            if (o.is_string()) {
                crumb += " / " + o.get<std::string>();
            }
        }
    }
    const std::string descent = cwd_below_root(root);
    if (!descent.empty()) {
        crumb += " / " + descent;
    }
    return crumb + " > ";
}

// `cd` is a pure move: it changes where later commands resolve from and never
// touches the UI. Reaching an overlay is `click` — a different act, so a
// different verb. (It used to be spelled `cd <widget>`, which meant a mistyped
// destination could dispatch a real event on a live printer.)
static void repl_cd(const std::string& socket_path, const std::vector<std::string>& tokens) {
    const std::string target = tokens.size() >= 2 ? tokens[1] : "/";

    // Bare `cd`, `cd /`: back to whatever is frontmost on screen.
    if (target == "/" || target == "~") {
        g_cwd.clear();
        return;
    }

    if (target == "..") {
        if (g_cwd.empty()) {
            // Already at the active root. Up from here means leaving it, which
            // is the navigation stack's business rather than the widget tree's
            // — the one place the two hierarchies meet.
            nlohmann::json r;
            if (query_result(socket_path, build_request("go_back"), r)) {
                g_cwd.clear();
            }
            return;
        }
        const size_t slash = g_cwd.rfind('/');
        std::string parent = (slash == std::string::npos) ? std::string() : g_cwd.substr(0, slash);
        // Arriving back at the active root is spelled "no cwd", so that a later
        // navigate still retargets it instead of pinning a stale locator.
        g_cwd = (parent == g_last_root) ? std::string() : parent;
        return;
    }

    nlohmann::json r;
    if (!query_result(socket_path, build_request("resolve", target_param(target)), r)) {
        return; // the server's error has already gone to stderr
    }
    const std::string path = r.value("path", std::string());
    if (path.empty()) {
        fprintf(stderr, "Error: could not resolve '%s'\n", target.c_str());
        return;
    }
    g_cwd = (path == g_last_root) ? std::string() : path;
}

// Pretty-print describe_screen output (`ls`) grouped by what you can do to each
// widget. Duplicate-named widgets get their @path appended so they stay
// addressable.
static void print_describe_grouped(const nlohmann::json& result) {
    if (!result.contains("widgets") || !result["widgets"].is_array()) {
        printf("%s\n", result.dump(2).c_str());
        return;
    }
    const auto& widgets = result["widgets"];
    std::map<std::string, int> name_count;
    for (const auto& w : widgets) {
        name_count[w.value("name", std::string())]++;
    }
    const char* verbs[] = {"click", "fill", "toggle", "set", "scroll"};
    for (const char* verb : verbs) {
        std::vector<std::string> items;
        for (const auto& w : widgets) {
            // find(), not w["actions"]: w is const, so operator[] on a widget
            // entry without an "actions" key hits JSON_ASSERT — an uncatchable
            // abort, not a throw. The dump comes from the server, which may be a
            // different build than this client.
            bool has = false;
            const auto actions_it = w.find("actions");
            if (actions_it != w.end() && actions_it->is_array()) {
                for (const auto& a : *actions_it) {
                    if (a == verb) {
                        has = true;
                        break;
                    }
                }
            }
            if (!has) {
                continue;
            }
            std::string name = w.value("name", std::string("?"));
            std::string label = name;
            if (name_count[name] > 1) {
                label += " @" + w.value("path", std::string());
            }
            if (w.contains("value")) {
                label += "=" + (w["value"].is_string() ? w["value"].get<std::string>()
                                                       : w["value"].dump());
            }
            items.push_back(label);
        }
        if (items.empty()) {
            continue;
        }
        printf("  %-7s", verb);
        for (const auto& it : items) {
            printf(" %s", it.c_str());
        }
        printf("\n");
    }

    // Everything else: labels, icons, containers. They carry no verb, so the
    // grouped listing above skips them — which reads as an empty result when
    // you have scoped down to a row that happens to contain only text.
    // Repeats are collapsed to "name x6": a settings list holds one row_icon
    // per row, and printing the word six times says nothing the count doesn't.
    std::vector<std::string> inert;
    std::map<std::string, int> inert_count;
    for (const auto& w : widgets) {
        const auto actions_it = w.find("actions");
        const bool has_any =
            actions_it != w.end() && actions_it->is_array() && !actions_it->empty();
        if (!has_any) {
            const std::string name = w.value("name", std::string("?"));
            if (inert_count[name]++ == 0) {
                inert.push_back(name);
            }
        }
    }
    if (!inert.empty()) {
        printf("  %-7s", "(inert)");
        for (const auto& n : inert) {
            const int n_seen = inert_count[n];
            if (n_seen > 1) {
                printf(" %s x%d", n.c_str(), n_seen);
            } else {
                printf(" %s", n.c_str());
            }
        }
        printf("\n");
    }

    if (result.contains("scope") && result["scope"].is_string()) {
        printf("  (%zu widgets under %s)\n", widgets.size(),
               result["scope"].get<std::string>().c_str());
    } else {
        printf("  (%zu widgets — `ls` shows all; @path targets any one)\n", widgets.size());
    }
}

static int run_repl(const std::string& socket_path) {
    // We reconnect for each command for simplicity — avoids handling connection
    // loss mid-session and keeps the REPL stateless. The server does support
    // multiple requests per connection if we want to optimize later.

    printf("helix-screen control REPL — type 'help' for commands, Tab for completion, "
           "Ctrl-D to quit\n");

    // Tab completion reaches the app on its own to list what is on screen.
    g_repl_socket = socket_path;

    // Set up linenoise
    linenoiseSetCompletionCallback(repl_completion);
    linenoiseSetHintsCallback(repl_hints);
    linenoiseSetFreeHintsCallback(repl_free_hints);
    linenoiseHistorySetMaxLen(500);

    // Load history
    std::string history_path = resolve_history_path();
    if (!history_path.empty()) {
        linenoiseHistoryLoad(history_path.c_str());
    }

    // Populate completion caches
    {
        int fd = connect_to_server(socket_path);
        if (fd < 0) {
            return 1;
        }
        repl_populate_caches(fd);
        close(fd);
    }

    int last_result = 0;
    char* line;

    while ((line = linenoise(build_prompt(socket_path).c_str())) != nullptr) {
        std::string input(line);
        linenoiseFree(line);

        // Skip empty lines
        auto tokens = tokenize(input);
        if (tokens.empty())
            continue;

        // Add to history
        linenoiseHistoryAdd(input.c_str());

        // Handle REPL-only commands
        if (tokens[0] == "quit" || tokens[0] == "exit") {
            break;
        }
        if (tokens[0] == "help") {
            repl_print_help();
            continue;
        }
        if (tokens[0] == "refresh") {
            int fd = connect_to_server(socket_path);
            if (fd >= 0) {
                repl_populate_caches(fd);
                close(fd);
                printf("Caches refreshed (%zu subjects, %zu scenarios, %zu panels)\n",
                       g_cached_subjects.size(), g_cached_scenarios.size(), g_cached_panels.size());
            }
            continue;
        }

        // `cd` moves the working directory and never reaches the wire as a
        // command of its own, so it is handled here rather than in
        // build_request_from_tokens.
        if (tokens[0] == "cd") {
            repl_cd(socket_path, tokens);
            continue;
        }

        // `pwd` answers both halves of "where am I": the navigation stack, and
        // the working directory targets resolve against.
        if (tokens[0] == "pwd" || tokens[0] == "current") {
            nlohmann::json r;
            if (query_result(socket_path, build_request("get_current"), r)) {
                printf("  panel:   %s\n", r.value("panel", std::string("?")).c_str());
                std::string overlays;
                if (r.contains("overlays") && r["overlays"].is_array()) {
                    for (const auto& o : r["overlays"]) {
                        if (o.is_string()) {
                            overlays += (overlays.empty() ? "" : ", ") + o.get<std::string>();
                        }
                    }
                }
                printf("  overlays: [%s]\n", overlays.c_str());
                const std::string root = r.value("root_path", std::string());
                printf("  cwd:     %s%s\n", g_cwd.empty() ? root.c_str() : g_cwd.c_str(),
                       g_cwd.empty() ? "  (active root)" : "");
            }
            continue;
        }

        // `ls` gets a grouped, human-readable rendering instead of raw JSON.
        // Its argument scopes the listing, and with no argument it lists the
        // working directory.
        if (tokens[0] == "ls" || tokens[0] == "describe_screen") {
            nlohmann::json params = nlohmann::json::object();
            if (tokens.size() >= 2) {
                params = target_param(tokens[1]);
            } else if (!g_cwd.empty()) {
                params["scope"] = g_cwd;
            }
            nlohmann::json r;
            if (query_result(socket_path, build_request("describe_screen", params), r)) {
                print_describe_grouped(r);
            }
            continue;
        }

        // Build and send request
        auto request = build_request_from_tokens(tokens);
        if (request.is_null())
            continue;

        int fd = connect_to_server(socket_path);
        if (fd < 0) {
            fprintf(stderr, "Connection lost. Is HelixScreen still running?\n");
            continue;
        }

        if (!send_request(fd, request)) {
            close(fd);
            continue;
        }

        // Use longer timeout for the blocking commands — the caller's own
        // --timeout can exceed the default socket read window otherwise.
        int timeout = (tokens[0] == "wait_for" || tokens[0] == "wait_idle") ? 120 : 30;
        auto raw = read_response(fd, timeout);
        close(fd);

        last_result = handle_response(raw);
    }

    // Save history
    if (!history_path.empty()) {
        // Ensure parent directories exist (simple recursive mkdir)
        std::string dir = history_path.substr(0, history_path.rfind('/'));
        for (size_t i = 1; i < dir.size(); i++) {
            if (dir[i] == '/') {
                dir[i] = '\0';
                mkdir(dir.c_str(), 0755);
                dir[i] = '/';
            }
        }
        mkdir(dir.c_str(), 0755);
        linenoiseHistorySave(history_path.c_str());
    }

    printf("\n");
    return last_result;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int helix::remote_client_main(int argc, char** argv) {
    std::string socket_path;
    int arg_start = 1;

    // Parse options
    while (arg_start < argc) {
        if (strcmp(argv[arg_start], "-h") == 0 || strcmp(argv[arg_start], "--help") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[arg_start], "--json") == 0) {
            g_json_output = true;
            arg_start++;
            continue;
        }
        if (strcmp(argv[arg_start], "-s") == 0 || strcmp(argv[arg_start], "--socket") == 0) {
            if (arg_start + 1 >= argc) {
                fprintf(stderr, "Error: --socket requires a path argument\n");
                return 1;
            }
            socket_path = argv[++arg_start];
            arg_start++;
        } else if (strcmp(argv[arg_start], "-C") == 0 || strcmp(argv[arg_start], "--cwd") == 0) {
            // A one-shot process cannot carry a working directory between
            // invocations the way the REPL does, so a script scopes each
            // command explicitly instead.
            if (arg_start + 1 >= argc) {
                fprintf(stderr, "Error: -C requires a path argument (e.g. -C s/settings_list)\n");
                return 1;
            }
            g_cwd = argv[++arg_start];
            arg_start++;
        } else {
            break; // Start of command
        }
    }

    // No command → drop into the interactive fs-style REPL.
    if (arg_start >= argc) {
        g_json_output = false; // --json is a one-shot flag; the REPL always formats
        return run_repl(resolve_socket_path(socket_path));
    }

    std::string command = argv[arg_start];

    // `ctl help` is the first thing anyone types; make it work as a command and
    // not just as the -h/--help option.
    if (command == "help" || command == "?") {
        print_usage();
        return 0;
    }

    // Explicit REPL mode
    if (command == "repl") {
        g_json_output = false; // --json is a one-shot flag; the REPL always formats
        std::string resolved_path = resolve_socket_path(socket_path);
        return run_repl(resolved_path);
    }

    // Single-command mode — build tokens from argv
    std::vector<std::string> tokens;
    for (int i = arg_start; i < argc; i++) {
        tokens.push_back(argv[i]);
    }

    auto request = build_request_from_tokens(tokens);
    if (request.is_null()) {
        return 1;
    }

    // Connect, send, receive
    std::string resolved_path = resolve_socket_path(socket_path);
    int fd = connect_to_server(resolved_path);
    if (fd < 0) {
        return 1;
    }

    if (!send_request(fd, request)) {
        close(fd);
        return 1;
    }

    // Use longer timeout for the blocking commands — the caller's own
    // --timeout can exceed the default socket read window otherwise.
    int timeout = (command == "wait_for" || command == "wait_idle") ? 120 : 30;
    std::string response = read_response(fd, timeout);
    close(fd);

    return handle_response(response);
}
