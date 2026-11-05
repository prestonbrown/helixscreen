// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipv4_config_system.h"

#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)

#include "core/text_io.h"
#include "system/child_process.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <vector>

extern char** environ; // the libc environment, handed to posix_spawnp

namespace helix::net {

namespace {

struct RunResult {
    int exit_code = -1; ///< -1: not started, signalled or timed out
    std::string out;    ///< stdout and stderr, interleaved
};

// One subprocess, no shell (every argument reaches the program verbatim), its
// output captured, killed if it outlives @p timeout. helix::run_capture() goes
// through popen and a shell and waits forever, which suits neither an argument
// that holds an address nor a daemon that may hang.
RunResult run_capture(const std::string& program, const std::vector<std::string>& args,
                      std::chrono::seconds timeout) {
    RunResult result;
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        result.out = std::string("pipe: ") + std::strerror(errno);
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fd[0]);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fd[1]);
    if (rc != 0) {
        close(pipe_fd[0]);
        result.out = program + ": " + std::strerror(rc);
        return result;
    }

    // Nonblocking and polled: a child that hangs with the pipe open must not
    // hold the worker past the deadline.
    fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK);
    const auto drain = [&] {
        char buf[512];
        for (;;) {
            const ssize_t n = read(pipe_fd[0], buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            result.out.append(buf, static_cast<size_t>(n));
        }
        return true;
    };
    const helix::ChildWaitResult wait = helix::terminate_and_reap(
        pid, std::chrono::steady_clock::now() + timeout, std::chrono::milliseconds(500),
        std::chrono::milliseconds(20), drain);
    drain();
    close(pipe_fd[0]);
    if (wait.outcome == helix::ChildWaitOutcome::Exited) {
        result.exit_code = wait.exit_code();
    } else if (wait.outcome == helix::ChildWaitOutcome::TimedOut) {
        result.out += "\n(timed out after " + std::to_string(timeout.count()) + " s)";
    }
    return result;
}

bool executable(const char* path) {
    return access(path, X_OK) == 0;
}

bool exists(const char* path) {
    return access(path, F_OK) == 0;
}

std::string first_output_line(const std::string& text) {
    return std::string(helix::text_io::trim(text.substr(0, text.find('\n'))));
}

// ============================================================================
// NetworkManager
// ============================================================================

// An apply runs up to three nmcli calls inside WiFiManager's 45 s user-op
// deadline, so each gets a third of it.
constexpr auto kNmcliTimeout = std::chrono::seconds(15);

class NetworkManagerIpv4Store : public Ipv4SystemStore {
  public:
    Ipv4Result read(const std::string& iface, Ipv4Config& out) override {
        std::string uuid;
        if (Ipv4Result r = active_uuid(iface, uuid); !r.ok()) {
            return r;
        }
        const RunResult show = run_capture("nmcli", nm_ipv4_show_argv(uuid), kNmcliTimeout);
        if (show.exit_code != 0) {
            return Ipv4Result::failed("nmcli connection show: " + first_output_line(show.out));
        }
        const auto parsed = nm_parse_ipv4(show.out);
        if (!parsed) {
            return Ipv4Result::failed("profile " + uuid +
                                      " uses an IPv4 method other than "
                                      "auto or manual");
        }
        out = *parsed;
        return Ipv4Result::success();
    }

    Ipv4Result apply(const std::string& iface, const Ipv4Config& config) override {
        std::string uuid;
        if (Ipv4Result r = active_uuid(iface, uuid); !r.ok()) {
            return r;
        }
        const RunResult modify =
            run_capture("nmcli", nm_ipv4_modify_argv(uuid, config), kNmcliTimeout);
        if (modify.exit_code != 0) {
            return Ipv4Result::failed("nmcli connection modify: " + first_output_line(modify.out));
        }
        // reapply moves the device onto the edited profile without dropping the
        // association; a device that refuses it gets the profile brought up again.
        const RunResult reapply = run_capture("nmcli", {"device", "reapply", iface}, kNmcliTimeout);
        if (reapply.exit_code == 0) {
            return Ipv4Result::success();
        }
        spdlog::info("[Ipv4] nmcli device reapply {} refused ({}); bringing the profile up", iface,
                     first_output_line(reapply.out));
        const RunResult up =
            run_capture("nmcli", {"connection", "up", "uuid", uuid}, kNmcliTimeout);
        if (up.exit_code != 0) {
            return Ipv4Result::failed("nmcli connection up: " + first_output_line(up.out));
        }
        return Ipv4Result::success();
    }

  private:
    static Ipv4Result active_uuid(const std::string& iface, std::string& uuid) {
        if (iface.empty()) {
            return Ipv4Result::failed("no interface");
        }
        const RunResult active = run_capture(
            "nmcli", {"-t", "-f", "UUID,DEVICE", "connection", "show", "--active"}, kNmcliTimeout);
        if (active.exit_code != 0) {
            return Ipv4Result::failed("nmcli connection show --active: " +
                                      first_output_line(active.out));
        }
        uuid = nm_active_uuid_for_device(active.out, iface);
        if (uuid.empty()) {
            return Ipv4Result::failed("no active NetworkManager connection on " + iface);
        }
        return Ipv4Result::success();
    }
};

// ============================================================================
// dhcpcd
// ============================================================================

constexpr const char* kDhcpcdConf = "/etc/dhcpcd.conf";
constexpr auto kDhcpcdTimeout = std::chrono::seconds(15);

const char* dhcpcd_binary() {
    for (const char* p : {"/usr/sbin/dhcpcd", "/sbin/dhcpcd", "/usr/bin/dhcpcd"}) {
        if (executable(p)) {
            return p;
        }
    }
    return nullptr;
}

bool dhcpcd_running() {
    // dhcpcd 9 keeps its pid and control sockets under /run/dhcpcd/; older
    // releases write /run/dhcpcd.pid and /run/dhcpcd.sock.
    for (const char* p : {"/run/dhcpcd/pid", "/run/dhcpcd.pid", "/var/run/dhcpcd.pid",
                          "/run/dhcpcd/sock", "/run/dhcpcd.sock", "/var/run/dhcpcd.sock"}) {
        if (exists(p)) {
            return true;
        }
    }
    return false;
}

class DhcpcdIpv4Store : public Ipv4SystemStore {
  public:
    explicit DhcpcdIpv4Store(std::string binary) : binary_(std::move(binary)) {}

    Ipv4Result read(const std::string& iface, Ipv4Config& out) override {
        const auto conf = helix::text_io::read_file(kDhcpcdConf, 1024 * 1024);
        if (!conf) {
            return Ipv4Result::failed(std::string("cannot read ") + kDhcpcdConf);
        }
        out = dhcpcd_conf_read_ipv4(*conf, iface);
        return Ipv4Result::success();
    }

    Ipv4Result apply(const std::string& iface, const Ipv4Config& config) override {
        if (iface.empty()) {
            return Ipv4Result::failed("no interface");
        }
        const auto conf = helix::text_io::read_file(kDhcpcdConf, 1024 * 1024);
        if (!conf) {
            return Ipv4Result::failed(std::string("cannot read ") + kDhcpcdConf);
        }
        const std::string updated = dhcpcd_conf_with_ipv4(*conf, iface, config);
        if (updated != *conf && !helix::text_io::write_file_atomic(
                                    kDhcpcdConf, updated, helix::text_io::Durability::Fsync)) {
            return Ipv4Result::failed(std::string("cannot write ") + kDhcpcdConf + ": " +
                                      std::strerror(errno));
        }
        // -n: reload the configuration and rebind the interface.
        const RunResult rebind = run_capture(binary_, {"-n", iface}, kDhcpcdTimeout);
        if (rebind.exit_code != 0) {
            return Ipv4Result::failed("dhcpcd -n " + iface + ": " + first_output_line(rebind.out));
        }
        return Ipv4Result::success();
    }

  private:
    std::string binary_;
};

} // namespace

bool network_manager_active() {
    const bool has_nmcli = executable("/usr/bin/nmcli") || executable("/bin/nmcli") ||
                           executable("/usr/local/bin/nmcli");
    const bool daemon_active = exists("/run/NetworkManager") || exists("/var/run/NetworkManager");
    return has_nmcli && daemon_active;
}

std::unique_ptr<Ipv4SystemStore> make_network_manager_ipv4_store() {
    if (!network_manager_active()) {
        return nullptr;
    }
    return std::make_unique<NetworkManagerIpv4Store>();
}

std::unique_ptr<Ipv4SystemStore> make_dhcpcd_ipv4_store() {
    const char* binary = dhcpcd_binary();
    if (!binary || !dhcpcd_running() || access(kDhcpcdConf, W_OK) != 0 ||
        access("/etc", W_OK) != 0) {
        return nullptr;
    }
    return std::make_unique<DhcpcdIpv4Store>(binary);
}

std::unique_ptr<Ipv4SystemStore> make_system_ipv4_store() {
    if (auto nm = make_network_manager_ipv4_store()) {
        return nm;
    }
    return make_dhcpcd_ipv4_store();
}

} // namespace helix::net

#else // no Linux network configuration owners on this platform

namespace helix::net {

bool network_manager_active() {
    return false;
}
std::unique_ptr<Ipv4SystemStore> make_network_manager_ipv4_store() {
    return nullptr;
}
std::unique_ptr<Ipv4SystemStore> make_dhcpcd_ipv4_store() {
    return nullptr;
}
std::unique_ptr<Ipv4SystemStore> make_system_ipv4_store() {
    return nullptr;
}

} // namespace helix::net

#endif
