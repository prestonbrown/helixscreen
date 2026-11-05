// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ipv4_config.h
 * @brief Static or DHCP IPv4 configuration for a network interface
 *
 * The hardware-agnostic half of static IPv4 (prestonbrown/helixscreen#1900):
 * the config value, the form validation the UI runs before Apply, the
 * capability interface every Wi-Fi and Ethernet backend implements, and the
 * pure text transforms the platform stores use (NetworkManager argv and
 * output, dhcpcd.conf). Nothing here touches a socket, a process or LVGL;
 * the record of a change awaiting confirmation is the one piece that reads
 * and writes a file.
 *
 * @threading The pure functions are thread-safe. Ipv4Configurable's get/apply
 *            are BLOCKING and run off the LVGL thread.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::net {

/// An interface's IPv4 settings as its platform persists them.
struct Ipv4Config {
    bool dhcp = true;             ///< Address from DHCP; the fields below are then unused
    std::string address;          ///< Dotted quad, static only
    int prefix = 0;               ///< Subnet prefix length, static only
    std::string gateway;          ///< Dotted quad, static only
    std::vector<std::string> dns; ///< DNS servers; kept for DHCP too where the platform has them

    bool operator==(const Ipv4Config& other) const;
    bool operator!=(const Ipv4Config& other) const {
        return !(*this == other);
    }
};

/// "DHCP" or "192.168.1.50/24 via 192.168.1.1", for the log.
std::string ipv4_config_text(const Ipv4Config& config);

/// Which of the overlay's two status cards a config belongs to.
enum class Ipv4Link { Wifi, Ethernet };

/// "wifi" / "ethernet", as the pending-revert record stores it.
const char* ipv4_link_name(Ipv4Link link);
std::optional<Ipv4Link> ipv4_link_from_name(std::string_view name);

// ============================================================================
// Address arithmetic
// ============================================================================

/// Strict dotted quad: four decimal octets 0-255, no spaces inside, no leading
/// '+', at most three digits each. Surrounding whitespace is ignored.
/// @return Host byte order
std::optional<uint32_t> parse_ipv4(std::string_view text);

/// Host byte order to dotted quad.
std::string format_ipv4(uint32_t address);

/// A subnet mask as a prefix length: a contiguous dotted mask
/// ("255.255.255.0"), or CIDR with or without the slash ("/24", "24").
/// @return 0-32, or nullopt for a non-contiguous or malformed mask
std::optional<int> parse_netmask(std::string_view text);

/// Prefix length to dotted mask: 24 -> "255.255.255.0". Clamped to 0-32.
std::string prefix_to_netmask(int prefix);

// ============================================================================
// Form validation
// ============================================================================

/// The four text fields of the IPv4 screen, as typed.
struct Ipv4Form {
    std::string address;
    std::string netmask;
    std::string gateway;
    std::string dns;
};

enum class Ipv4Field { None, Address, Netmask, Gateway, Dns };

enum class Ipv4Problem {
    None,
    Empty,                ///< The field is blank
    NotAnAddress,         ///< Not a dotted quad
    NotUsable,            ///< 0.x, loopback, multicast, reserved or broadcast: never a host
    NotAMask,             ///< Malformed or non-contiguous mask, or /0
    NetworkAddress,       ///< The address is the subnet's network address
    BroadcastAddress,     ///< The address is the subnet's broadcast address
    GatewayOutsideSubnet, ///< The gateway is not reachable on the subnet
    GatewayIsAddress,     ///< The gateway is the device's own address
};

struct Ipv4Verdict {
    Ipv4Problem problem = Ipv4Problem::None;
    Ipv4Field field = Ipv4Field::None; ///< The field to point the user at
    Ipv4Config config;                 ///< The static config, filled when ok()

    bool ok() const {
        return problem == Ipv4Problem::None;
    }
};

/**
 * @brief Validate the static IPv4 form before anything is applied
 *
 * Checks the fields in screen order and reports the first problem: a usable
 * unicast address, a contiguous mask of /1 to /32, an address that is
 * neither the network nor the broadcast address (/31 and /32 have neither),
 * a gateway inside the subnet that is not the address itself, and one usable
 * DNS server, which may be anywhere (a loopback resolver included). A /32
 * has no gateway inside it, so the form never passes one.
 */
Ipv4Verdict validate_ipv4_form(const Ipv4Form& form);

/// The form a config pre-fills: the mask as a dotted quad, the first DNS server.
Ipv4Form ipv4_form_from_config(const Ipv4Config& config);

/// @p next with what the form cannot show carried over from @p current: the
/// form holds one DNS server, so an unedited first server keeps the rest.
Ipv4Config ipv4_keep_unshown(const Ipv4Config& current, Ipv4Config next);

// ============================================================================
// Backend capability
// ============================================================================

struct Ipv4Result {
    enum class Status { Ok, NotSupported, Failed };
    Status status = Status::Ok;
    std::string detail; ///< Technical detail for the log

    bool ok() const {
        return status == Status::Ok;
    }
    static Ipv4Result success() {
        return {};
    }
    static Ipv4Result not_supported(std::string why = "static IPv4 not supported") {
        return {Status::NotSupported, std::move(why)};
    }
    static Ipv4Result failed(std::string why) {
        return {Status::Failed, std::move(why)};
    }
};

/**
 * @brief What one change edits, and how to put it back exactly
 *
 * Captured before the change is applied and stored in the pending-revert
 * record, so a revert edits the same profile or interface the change did,
 * whatever is active by then, and writes back the original text rather than
 * a re-rendering of what was parsed from it.
 */
struct Ipv4Snapshot {
    std::string store;    ///< "networkmanager", "dhcpcd" or "mock"
    std::string iface;    ///< The interface the change targets
    std::string profile;  ///< NetworkManager connection UUID; empty for the others
    std::string original; ///< What a restore writes back (store-specific text)
    std::string written;  ///< dhcpcd: the file the change writes; restore refuses if it moved on
};

/**
 * @brief Reading and applying an interface's IPv4 configuration
 *
 * Every Wi-Fi and Ethernet backend implements this, each the way its platform
 * persists network settings. The defaults are the unsupported answer: a
 * backend that cannot make a static address survive a reboot leaves them, and
 * the UI hides the setting rather than offer one that is lost on restart.
 * Restoring a snapshot needs no backend: restore_ipv4_snapshot()
 * (ipv4_config_system.h) works from the snapshot alone.
 *
 * A virtual query rather than a cast to a concrete backend, because the
 * firmware builds -fno-rtti.
 */
class Ipv4Configurable {
  public:
    virtual ~Ipv4Configurable() = default;

    /// Whether apply_ipv4_config() can persist a static address across reboot.
    /// Cheap (no subprocess, no socket): the UI asks it on the LVGL thread.
    virtual bool supports_ipv4_config() const {
        return false;
    }

    /// BLOCKING. The interface's current IPv4 configuration as persisted.
    virtual Ipv4Result get_ipv4_config(Ipv4Config& out) {
        out = {};
        return Ipv4Result::not_supported();
    }

    /// BLOCKING. Resolve what applying @p next would edit and snapshot it.
    virtual Ipv4Result prepare_ipv4_change(const Ipv4Config& next, Ipv4Snapshot& out) {
        (void)next;
        out = {};
        return Ipv4Result::not_supported();
    }

    /// BLOCKING. Persist @p config into @p target and bring the interface onto it.
    virtual Ipv4Result apply_ipv4_config(const Ipv4Snapshot& target, const Ipv4Config& config) {
        (void)target;
        (void)config;
        return Ipv4Result::not_supported();
    }
};

/**
 * @brief An in-memory IPv4 configuration, for the mock backends
 *
 * Starts on DHCP. HELIX_MOCK_IPV4_APPLY_FAIL=1 makes every apply and restore
 * fail, so the screen's failure paths can be driven under --test.
 */
class MockIpv4Config {
  public:
    Ipv4Result get(Ipv4Config& out) const;
    Ipv4Result apply(const Ipv4Config& config);
    /// The static address, or "" on DHCP: what the mock reports as its IP.
    std::string static_address() const;

  private:
    mutable std::mutex mutex_;
    Ipv4Config config_;
};

/// The mock config for @p link, one per process: every manager builds its own
/// backend, and they all stand for the same port, which a restore reaches too.
MockIpv4Config& mock_ipv4_store(Ipv4Link link);

/// Snapshot for the mock store of @p link (restore_ipv4_snapshot replays it).
Ipv4Snapshot mock_ipv4_snapshot(Ipv4Link link);

/// Restore a "mock" snapshot.
Ipv4Result restore_mock_ipv4_snapshot(const Ipv4Snapshot& snapshot);

// ============================================================================
// NetworkManager (nmcli)
// ============================================================================

/// The UUID of the connection active on @p iface, from
/// `nmcli -t -f UUID,DEVICE connection show --active`. Empty when none is.
std::string nm_active_uuid_for_device(const std::string& output, const std::string& iface);

/// The profile a link uses while nothing is active on it: from
/// `nmcli -t -f UUID,TYPE,TIMESTAMP,DEVICE connection show`, the most recently
/// used inactive profile of @p link's type. Empty when there is none.
std::string nm_inactive_profile_for_link(const std::string& output, Ipv4Link link);

/// The four IPv4 values of a profile as nmcli prints them, lists intact.
struct NmIpv4Values {
    std::string method;
    std::string addresses;
    std::string gateway;
    std::string dns;
};
NmIpv4Values nm_parse_ipv4_values(const std::string& output);
std::string nm_encode_values(const NmIpv4Values& values);
NmIpv4Values nm_decode_values(const std::string& text);

/// Argument vector that writes @p values back into a profile exactly.
std::vector<std::string> nm_ipv4_restore_argv(const std::string& uuid, const NmIpv4Values& values);

/// Argument vector (program name excluded) that reads a profile's IPv4 settings.
std::vector<std::string> nm_ipv4_show_argv(const std::string& uuid);

/// Parse `nmcli -t -f ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns connection show`.
/// nullopt for a method other than auto/manual (disabled, link-local, shared)
/// or a manual profile without an address.
std::optional<Ipv4Config> nm_parse_ipv4(const std::string& output);

/// Argument vector (program name excluded) that writes @p config into a profile.
/// DHCP clears the static address and gateway and keeps @p config's DNS.
std::vector<std::string> nm_ipv4_modify_argv(const std::string& uuid, const Ipv4Config& config);

// ============================================================================
// dhcpcd (dhcpcd.conf)
// ============================================================================

/// @p conf with @p iface's static IPv4 replaced by @p config: any static
/// address, routers and DNS lines for the interface are removed, and a static
/// config is appended as one block HelixScreen marks as its own. Every other
/// line is kept as it was. nullopt when a HelixScreen block has no end marker:
/// where it stops cannot be known, so nothing is edited.
std::optional<std::string> dhcpcd_conf_with_ipv4(const std::string& conf, const std::string& iface,
                                                 const Ipv4Config& config);

/// @p iface's IPv4 config as @p conf states it: DHCP unless an
/// `interface <iface>` block carries `static ip_address=`; a DNS override is
/// kept either way. nullopt when the stanza says something the config cannot
/// hold (an address without a prefix, or several addresses).
std::optional<Ipv4Config> dhcpcd_conf_read_ipv4(const std::string& conf, const std::string& iface);

// ============================================================================
// Confirm-or-revert
// ============================================================================

/// Seconds the user has to keep a new config before it is reverted.
constexpr uint32_t kIpv4ConfirmSeconds = 60;

/**
 * @brief The keep-or-revert countdown, on whatever clock the caller passes
 *
 * Millisecond ticks are unsigned and may wrap; elapsed time is their
 * difference, so a wrap mid-countdown is harmless. The UI drives it from
 * lv_tick_get() and tests from plain numbers.
 */
class Ipv4RevertCountdown {
  public:
    void start(uint32_t now_ms, uint32_t seconds = kIpv4ConfirmSeconds);
    void stop() {
        running_ = false;
    }
    bool running() const {
        return running_;
    }
    /// Whole seconds left, rounded up; 0 once expired or when stopped.
    uint32_t seconds_left(uint32_t now_ms) const;
    /// Running and out of time.
    bool expired(uint32_t now_ms) const;

  private:
    bool running_ = false;
    uint32_t start_ms_ = 0;
    uint32_t total_ms_ = 0;
};

/**
 * @brief A change awaiting Keep, and the config to go back to
 *
 * Written before a new config is applied and removed when the user keeps it
 * or the revert lands. Finding it at startup means the app or the device
 * restarted mid-countdown, so the previous config is restored then.
 */
struct PendingIpv4Revert {
    Ipv4Link link = Ipv4Link::Wifi;
    Ipv4Config previous; ///< For the log and the screen; the restore uses `snapshot`
    Ipv4Snapshot snapshot;
    /// ipv4_process_owner() of the process running the countdown. A record
    /// this process wrote belongs to its live countdown, never to the startup
    /// revert.
    std::string owner;
};

/// A token unique to this process run, stamped into the records it writes.
const std::string& ipv4_process_owner();

/// Whether the record found at startup is a leftover to revert: there is one
/// for @p link and no countdown in this process owns it.
bool is_stale_pending_ipv4_revert(const std::optional<PendingIpv4Revert>& pending, Ipv4Link link);

std::string encode_pending_ipv4_revert(const PendingIpv4Revert& pending);
/// nullopt for anything that is not a record. A previous config is accepted
/// when every address in it parses: a system can hold configs the form never
/// would (no gateway, a /32), and those must still come back.
std::optional<PendingIpv4Revert> decode_pending_ipv4_revert(std::string_view text);

/// <user config dir>/ipv4_pending_revert.json: beside settings.json but not in
/// it, so a restored settings backup can never replay a stale revert.
std::string pending_ipv4_revert_path();
bool save_pending_ipv4_revert(const PendingIpv4Revert& pending);
std::optional<PendingIpv4Revert> load_pending_ipv4_revert();
/// True when no record remains (removed now, or there was none).
bool clear_pending_ipv4_revert();

/**
 * @brief When a startup revert tries again
 *
 * At start the network may not exist yet (NetworkManager still activating,
 * dhcpcd not started), so a failed restore is retried every
 * kIpv4RetryEveryMs until kIpv4RetryForMs have passed since the first try.
 */
constexpr uint32_t kIpv4RetryEveryMs = 5000;
constexpr uint32_t kIpv4RetryForMs = 120000;

class Ipv4RetrySchedule {
  public:
    void start(uint32_t now_ms) {
        first_ms_ = now_ms;
        last_ms_ = now_ms;
    }
    /// An attempt is starting now.
    void tried(uint32_t now_ms) {
        last_ms_ = now_ms;
    }
    bool due(uint32_t now_ms) const {
        return now_ms - last_ms_ >= kIpv4RetryEveryMs && !exhausted(now_ms);
    }
    bool exhausted(uint32_t now_ms) const {
        return now_ms - first_ms_ >= kIpv4RetryForMs;
    }

  private:
    uint32_t first_ms_ = 0;
    uint32_t last_ms_ = 0;
};

/// Held around every prepare, apply and restore, so a revert never runs
/// between a change's snapshot and its apply.
std::mutex& ipv4_change_mutex();

// ============================================================================
// The change, from Apply to Keep or revert
// ============================================================================

/**
 * @brief The IPv4 screen's change lifecycle, with no clock or widget of its own
 *
 * Applying -> Confirming on a good answer, then Keep makes it final and
 * anything else reverts: a failed apply, Revert now, a dismissal, the
 * countdown running out, or the screen going away. Each event returns what
 * the caller must do; the caller does it and reports back.
 */
class Ipv4ChangeFlow {
  public:
    enum class State { Idle, Applying, Confirming, Reverting };
    enum class Action {
        None,
        Apply,          ///< Apply next(); answer with applied()
        Close,          ///< Nothing changed: leave the screen
        ShowKeepPrompt, ///< Ask Keep or revert; the countdown has started
        Revert,         ///< Restore the snapshot; answer with reverted()
        Kept,           ///< Final: drop the record and leave the screen
        KeptForJob,     ///< Kept because a job took the machine: close the dialog, then as Kept
        Restored,       ///< The previous config is back
        RestoreFailed,  ///< Still unconfirmed; the record stays for the next start
    };

    State state() const {
        return state_;
    }
    const Ipv4Config& next() const {
        return next_;
    }
    /// Set when the screen went away while the apply was running: the apply
    /// restores the snapshot itself once it lands.
    std::shared_ptr<std::atomic<bool>> abandoned() const {
        return abandoned_;
    }
    const Ipv4RevertCountdown& countdown() const {
        return countdown_;
    }

    Action begin(const Ipv4Config& current, const Ipv4Config& next);
    Action applied(bool ok, uint32_t now_ms);
    Action tick(uint32_t now_ms);
    Action keep();
    /// Revert now, ESC, or the dialog closed without an answer.
    Action reject();
    Action reverted(bool ok);
    /// A job started holding the machine. While asked, the change is kept:
    /// reverting mid-print could drop what the print depends on.
    Action job_started();
    /// The screen is going away for good.
    Action teardown();

  private:
    State state_ = State::Idle;
    Ipv4Config next_;
    Ipv4RevertCountdown countdown_;
    std::shared_ptr<std::atomic<bool>> abandoned_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace helix::net
