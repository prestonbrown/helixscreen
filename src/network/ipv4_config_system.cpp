// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipv4_config_system.h"

#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)

#include "core/text_io.h"
#include "system/child_process.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern char** environ; // NAMESPACE_OK: libc global, handed to posix_spawnp

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

// A change runs up to five nmcli calls (two lookups and a read to snapshot, a
// modify, then reapply or up) inside WiFiManager's 58 s IPv4 deadline.
constexpr auto kNmcliQuick = std::chrono::seconds(5);
constexpr auto kNmcliModify = std::chrono::seconds(10);
constexpr auto kNmcliReapply = std::chrono::seconds(10);
constexpr auto kNmcliUp = std::chrono::seconds(20);

class NetworkManagerIpv4Store : public Ipv4SystemStore {
  public:
    Ipv4Result read(const std::string& iface, Ipv4Link link, Ipv4Config& out) override {
        std::string uuid;
        if (Ipv4Result r = profile_for(iface, link, uuid); !r.ok()) {
            return r;
        }
        NmIpv4Values values;
        if (Ipv4Result r = show(uuid, values); !r.ok()) {
            return r;
        }
        const auto parsed = nm_parse_ipv4(nm_encode_values(values));
        if (!parsed) {
            return Ipv4Result::failed("profile " + uuid +
                                      " uses an IPv4 method other than auto or manual");
        }
        out = *parsed;
        return Ipv4Result::success();
    }

    Ipv4Result prepare(const std::string& iface, Ipv4Link link, const Ipv4Config& next,
                       Ipv4Snapshot& out) override {
        (void)next;
        std::string uuid;
        if (Ipv4Result r = profile_for(iface, link, uuid); !r.ok()) {
            return r;
        }
        NmIpv4Values values;
        if (Ipv4Result r = show(uuid, values); !r.ok()) {
            return r;
        }
        out = {};
        out.store = "networkmanager";
        out.iface = iface;
        out.profile = uuid;
        out.original = nm_encode_values(values);
        return Ipv4Result::success();
    }

    Ipv4Result apply(const Ipv4Snapshot& target, const Ipv4Config& config) override {
        const RunResult modify =
            run_capture("nmcli", nm_ipv4_modify_argv(target.profile, config), kNmcliModify);
        if (modify.exit_code != 0) {
            return Ipv4Result::failed("nmcli connection modify: " + first_output_line(modify.out));
        }
        return activate(target, /*required=*/true);
    }

  private:
    // The profile in use on @p iface, or with none active, the profile the
    // link would bring up: a screen must open on a link that is down.
    static Ipv4Result profile_for(const std::string& iface, Ipv4Link link, std::string& uuid) {
        if (iface.empty()) {
            return Ipv4Result::failed("no interface");
        }
        const RunResult active = run_capture(
            "nmcli", {"-t", "-f", "UUID,DEVICE", "connection", "show", "--active"}, kNmcliQuick);
        if (active.exit_code != 0) {
            return Ipv4Result::failed("nmcli connection show --active: " +
                                      first_output_line(active.out));
        }
        uuid = nm_active_uuid_for_device(active.out, iface);
        if (!uuid.empty()) {
            return Ipv4Result::success();
        }
        const RunResult all = run_capture(
            "nmcli", {"-t", "-f", "UUID,TYPE,TIMESTAMP,DEVICE", "connection", "show"}, kNmcliQuick);
        uuid = all.exit_code == 0 ? nm_inactive_profile_for_link(all.out, link) : std::string();
        if (uuid.empty()) {
            return Ipv4Result::failed("no NetworkManager profile for " + iface);
        }
        return Ipv4Result::success();
    }

    static Ipv4Result show(const std::string& uuid, NmIpv4Values& out) {
        const RunResult shown = run_capture("nmcli", nm_ipv4_show_argv(uuid), kNmcliQuick);
        if (shown.exit_code != 0) {
            return Ipv4Result::failed("nmcli connection show: " + first_output_line(shown.out));
        }
        out = nm_parse_ipv4_values(shown.out);
        return Ipv4Result::success();
    }

  public:
    // Moves the interface onto the edited profile. reapply keeps the
    // association; a device that refuses it, or a profile not active, is
    // brought up on the recorded interface. When @p required is false a
    // failure is only logged: the profile holds the config, and autoconnect
    // brings it up when the link can.
    static Ipv4Result activate(const Ipv4Snapshot& target, bool required) {
        const RunResult reapply =
            run_capture("nmcli", {"device", "reapply", target.iface}, kNmcliReapply);
        if (reapply.exit_code == 0) {
            return Ipv4Result::success();
        }
        const RunResult up = run_capture(
            "nmcli", {"connection", "up", "uuid", target.profile, "ifname", target.iface},
            kNmcliUp);
        if (up.exit_code == 0) {
            return Ipv4Result::success();
        }
        if (!required) {
            spdlog::warn("[Ipv4] Profile {} restored but not brought up on {}: {}", target.profile,
                         target.iface, first_output_line(up.out));
            return Ipv4Result::success();
        }
        return Ipv4Result::failed("nmcli connection up: " + first_output_line(up.out));
    }
};

Ipv4Result restore_network_manager(const Ipv4Snapshot& snapshot) {
    if (!network_manager_active()) {
        return Ipv4Result::failed("NetworkManager is not running");
    }
    const RunResult modify = run_capture(
        "nmcli", nm_ipv4_restore_argv(snapshot.profile, nm_decode_values(snapshot.original)),
        kNmcliModify);
    if (modify.exit_code != 0) {
        return Ipv4Result::failed("nmcli connection modify: " + first_output_line(modify.out));
    }
    return NetworkManagerIpv4Store::activate(snapshot, /*required=*/false);
}

// ============================================================================
// dhcpcd
// ============================================================================

constexpr const char* kDhcpcdConf = "/etc/dhcpcd.conf";
constexpr auto kDhcpcdTimeout = std::chrono::seconds(15);
// A dhcpcd.conf this size or larger may have been cut short by the read.
constexpr std::size_t kDhcpcdConfMax = 1024 * 1024;

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

Ipv4Result read_dhcpcd_conf(std::string& out) {
    const auto conf = helix::text_io::read_file(kDhcpcdConf, kDhcpcdConfMax);
    if (!conf) {
        return Ipv4Result::failed(std::string("cannot read ") + kDhcpcdConf);
    }
    if (conf->size() >= kDhcpcdConfMax) {
        return Ipv4Result::failed(std::string(kDhcpcdConf) + " is too large to edit safely");
    }
    out = *conf;
    return Ipv4Result::success();
}

// Writes through a symlink to its target and keeps the file's mode and owner.
Ipv4Result write_dhcpcd_conf(const std::string& text) {
    char real[PATH_MAX];
    if (!realpath(kDhcpcdConf, real)) {
        return Ipv4Result::failed(std::string("cannot resolve ") + kDhcpcdConf);
    }
    struct stat st {};
    const bool have_stat = stat(real, &st) == 0;
    if (!helix::text_io::write_file_atomic(real, text, helix::text_io::Durability::Fsync)) {
        return Ipv4Result::failed(std::string("cannot write ") + real + ": " +
                                  std::strerror(errno));
    }
    if (have_stat) {
        if (chown(real, st.st_uid, st.st_gid) != 0 || chmod(real, st.st_mode & 07777) != 0) {
            spdlog::warn("[Ipv4] Could not keep the owner and mode of {}: {}", real,
                         std::strerror(errno));
        }
    }
    return Ipv4Result::success();
}

Ipv4Result rebind_dhcpcd(const std::string& iface) {
    const char* binary = dhcpcd_binary();
    if (!binary) {
        return Ipv4Result::failed("dhcpcd not found");
    }
    // -n: reload the configuration and rebind the interface.
    const RunResult rebind = run_capture(binary, {"-n", iface}, kDhcpcdTimeout);
    if (rebind.exit_code != 0) {
        return Ipv4Result::failed("dhcpcd -n " + iface + ": " + first_output_line(rebind.out));
    }
    return Ipv4Result::success();
}

class DhcpcdIpv4Store : public Ipv4SystemStore {
  public:
    Ipv4Result read(const std::string& iface, Ipv4Link, Ipv4Config& out) override {
        std::string conf;
        if (Ipv4Result r = read_dhcpcd_conf(conf); !r.ok()) {
            return r;
        }
        const auto parsed = dhcpcd_conf_read_ipv4(conf, iface);
        if (!parsed) {
            return Ipv4Result::failed("the " + iface + " stanza in " + kDhcpcdConf +
                                      " holds an address the screen cannot show");
        }
        out = *parsed;
        return Ipv4Result::success();
    }

    Ipv4Result prepare(const std::string& iface, Ipv4Link, const Ipv4Config& next,
                       Ipv4Snapshot& out) override {
        if (iface.empty()) {
            return Ipv4Result::failed("no interface");
        }
        std::string conf;
        if (Ipv4Result r = read_dhcpcd_conf(conf); !r.ok()) {
            return r;
        }
        const auto updated = dhcpcd_conf_with_ipv4(conf, iface, next);
        if (!updated) {
            return Ipv4Result::failed(std::string(kDhcpcdConf) +
                                      " has a HelixScreen block with no end marker");
        }
        out = {};
        out.store = "dhcpcd";
        out.iface = iface;
        out.original = conf;
        out.written = *updated;
        return Ipv4Result::success();
    }

    Ipv4Result apply(const Ipv4Snapshot& target, const Ipv4Config&) override {
        std::string conf;
        if (Ipv4Result r = read_dhcpcd_conf(conf); !r.ok()) {
            return r;
        }
        if (conf != target.original) {
            return Ipv4Result::failed(std::string(kDhcpcdConf) + " changed since it was read");
        }
        if (Ipv4Result r = write_dhcpcd_conf(target.written); !r.ok()) {
            return r;
        }
        return rebind_dhcpcd(target.iface);
    }
};

Ipv4Result restore_dhcpcd(const Ipv4Snapshot& snapshot) {
    std::string conf;
    if (Ipv4Result r = read_dhcpcd_conf(conf); !r.ok()) {
        return r;
    }
    if (conf != snapshot.original) {
        // Anything other than what the change wrote is someone else's edit.
        if (conf != snapshot.written) {
            return Ipv4Result::failed(std::string(kDhcpcdConf) +
                                      " was edited since the change; not restoring over it");
        }
        if (Ipv4Result r = write_dhcpcd_conf(snapshot.original); !r.ok()) {
            return r;
        }
    }
    if (!dhcpcd_running()) {
        // The file is right; dhcpcd reads it when it starts.
        return Ipv4Result::success();
    }
    if (Ipv4Result r = rebind_dhcpcd(snapshot.iface); !r.ok()) {
        spdlog::warn("[Ipv4] {} restored, but {}", kDhcpcdConf, r.detail);
    }
    return Ipv4Result::success();
}

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
    return std::make_unique<DhcpcdIpv4Store>();
}

std::unique_ptr<Ipv4SystemStore> make_system_ipv4_store() {
    if (auto nm = make_network_manager_ipv4_store()) {
        return nm;
    }
    return make_dhcpcd_ipv4_store();
}

Ipv4Result restore_ipv4_snapshot(const Ipv4Snapshot& snapshot) {
    if (snapshot.store == "networkmanager") {
        return restore_network_manager(snapshot);
    }
    if (snapshot.store == "dhcpcd") {
        return restore_dhcpcd(snapshot);
    }
    if (snapshot.store == "mock") {
        return restore_mock_ipv4_snapshot(snapshot);
    }
    return Ipv4Result::failed("unknown store '" + snapshot.store + "'");
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
Ipv4Result restore_ipv4_snapshot(const Ipv4Snapshot& snapshot) {
    if (snapshot.store == "mock") {
        return restore_mock_ipv4_snapshot(snapshot);
    }
    return Ipv4Result::not_supported();
}

} // namespace helix::net

#endif

namespace helix::net {

Ipv4Result store_read(Ipv4SystemStore* store, const std::string& iface, Ipv4Link link,
                      Ipv4Config& out) {
    if (!store) {
        return Ipv4Result::not_supported();
    }
    if (iface.empty()) {
        return Ipv4Result::failed("no interface");
    }
    return store->read(iface, link, out);
}

Ipv4Result store_prepare(Ipv4SystemStore* store, const std::string& iface, Ipv4Link link,
                         const Ipv4Config& next, Ipv4Snapshot& out) {
    if (!store) {
        return Ipv4Result::not_supported();
    }
    if (iface.empty()) {
        return Ipv4Result::failed("no interface");
    }
    return store->prepare(iface, link, next, out);
}

Ipv4Result store_apply(Ipv4SystemStore* store, const Ipv4Snapshot& target,
                       const Ipv4Config& config) {
    if (!store) {
        return Ipv4Result::not_supported();
    }
    return store->apply(target, config);
}

Ipv4Result apply_ipv4_change(Ipv4Configurable& backend, Ipv4Link link, const Ipv4Config& previous,
                             const Ipv4Config& next, const std::atomic<bool>* abandoned) {
    std::lock_guard<std::mutex> lock(ipv4_change_mutex());
    PendingIpv4Revert pending;
    pending.link = link;
    pending.previous = previous;
    pending.owner = ipv4_process_owner();
    if (Ipv4Result r = backend.prepare_ipv4_change(next, pending.snapshot); !r.ok()) {
        return r;
    }
    // Without the record a restart mid-countdown would keep a config nobody
    // confirmed, so nothing is applied unless it is on disk first.
    if (!save_pending_ipv4_revert(pending)) {
        return Ipv4Result::failed("could not save the revert record");
    }
    const Ipv4Result applied = backend.apply_ipv4_config(pending.snapshot, next);
    if (applied.ok() && abandoned && abandoned->load()) {
        spdlog::info("[Ipv4] The IPv4 screen went away mid-apply; restoring {}",
                     ipv4_config_text(previous));
        if (restore_ipv4_snapshot(pending.snapshot).ok()) {
            clear_pending_ipv4_revert();
        }
    }
    return applied;
}

Ipv4Result restore_pending_ipv4(Ipv4Link link, bool stale_only) {
    std::lock_guard<std::mutex> lock(ipv4_change_mutex());
    const auto pending = load_pending_ipv4_revert();
    if (!pending || pending->link != link ||
        (stale_only && !is_stale_pending_ipv4_revert(pending, link))) {
        return Ipv4Result::success();
    }
    spdlog::info("[Ipv4] Restoring {} on {} ({})", ipv4_config_text(pending->previous),
                 pending->snapshot.iface, pending->snapshot.store);
    const Ipv4Result r = restore_ipv4_snapshot(pending->snapshot);
    if (!r.ok()) {
        spdlog::warn("[Ipv4] Could not restore the previous IPv4 config: {}", r.detail);
        return r;
    }
    if (!clear_pending_ipv4_revert()) {
        return Ipv4Result::failed("restored, but the revert record could not be removed");
    }
    return r;
}

} // namespace helix::net
