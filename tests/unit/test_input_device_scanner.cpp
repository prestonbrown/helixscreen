// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../mock_input_tree.h"
#include "input_device_scanner.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>

#include "../catch_amalgamated.hpp"

using helix::input::check_capability_bit;
using helix::input::check_capability_bit_at_width;
using helix::input::sysfs_bitmap_word_bits;
using helix::input::sysfs_bitmap_word_bits_for_machine;

namespace fs = std::filesystem;

using helix::test::caps_string;
using helix::test::MockInputTree;
using helix::test::mouse_key_caps;

TEST_CASE("check_capability_bit parses sysfs hex bitmasks", "[input]") {
    SECTION("empty string returns false") {
        REQUIRE_FALSE(check_capability_bit("", 0));
        REQUIRE_FALSE(check_capability_bit("", 30));
    }

    SECTION("bit 0 in single word") {
        REQUIRE(check_capability_bit("1", 0));
        REQUIRE_FALSE(check_capability_bit("0", 0));
    }

    SECTION("bit 1 in single word") {
        REQUIRE(check_capability_bit("3", 1));
        REQUIRE(check_capability_bit("2", 1));
        REQUIRE_FALSE(check_capability_bit("1", 1));
    }

    SECTION("KEY_A (bit 30) in last word") {
        REQUIRE(check_capability_bit("40000000", 30));
        REQUIRE_FALSE(check_capability_bit("20000000", 30));
    }

    SECTION("KEY_A in multi-word bitmask (32-bit words)") {
        REQUIRE(check_capability_bit_at_width("10000 40000000", 30, 32));
    }

    SECTION("KEY_A in multi-word bitmask (64-bit words)") {
        // A real 64-bit kernel prints this bitmap (bits 30 and 48) as a
        // single word; the two-word form exercises the index math when word 0
        // is the rightmost of several words.
        REQUIRE(check_capability_bit_at_width("10000 40000000", 30, 64));
        REQUIRE(check_capability_bit_at_width("1000040000000", 30, 64));
        REQUIRE(check_capability_bit_at_width("1000040000000", 48, 64));
    }

    SECTION("BTN_LEFT (bit 272) in 32-bit word format") {
        REQUIRE(check_capability_bit_at_width("1f0000 0 0 0 0 0 0 0 0", 272, 32));
    }

    SECTION("BTN_LEFT (bit 272) in 64-bit word format") {
        // The same mouse on a 64-bit kernel: word 4 from the right is the
        // highest nonzero word, so only five words print. The kernel uses %lx
        // without zero padding and strips leading zero words, so no word here
        // has 16 hex digits.
        REQUIRE(check_capability_bit_at_width("1f0000 0 0 0 0", 272, 64));
    }

    SECTION("BTN_LEFT not set") {
        REQUIRE_FALSE(check_capability_bit("0 0 0 0 0 0 0 0 0", 272));
    }

    SECTION("KEY_POWER (bit 116) - should not match KEY_A check") {
        // 32-bit word format: the set word is word 3 from the right.
        REQUIRE(check_capability_bit_at_width("0 0 0 0 0 100000 0 0 0", 116, 32));
        REQUIRE_FALSE(check_capability_bit_at_width("0 0 0 0 0 100000 0 0 0", 30, 32));
    }

    SECTION("real-world keyboard capability string from Pi 5 (aarch64)") {
        // 64-bit kernel words: this combo keyboard+trackpad has KEY_A in word
        // 0 and BTN_LEFT in word 4 from the right, which prints as "403ffff".
        const char* real_kb = "3 0 0 0 0 0 403ffff 73ffff206efffd f3cfffff ffffffff fffffffe";
        REQUIRE(check_capability_bit_at_width(real_kb, 30, 64));  // KEY_A
        REQUIRE(check_capability_bit_at_width(real_kb, 272, 64)); // BTN_LEFT (combo device)
    }

    SECTION("real-world mouse capability string") {
        // 32-bit kernel form: nine words, BTN_LEFT..BTN_EXTRA in word 8.
        const char* real_mouse_32 = "1f0000 0 0 0 0 0 0 0 0";
        REQUIRE(check_capability_bit_at_width(real_mouse_32, 272, 32));
        REQUIRE(check_capability_bit_at_width(real_mouse_32, 273, 32));
        REQUIRE_FALSE(check_capability_bit_at_width(real_mouse_32, 30, 32));

        // The same mouse on a 64-bit kernel (field report, Flytech KPC6
        // x86_64): five words, unpadded.
        const char* real_mouse_64 = "1f0000 0 0 0 0";
        REQUIRE(check_capability_bit_at_width(real_mouse_64, 272, 64));
        REQUIRE(check_capability_bit_at_width(real_mouse_64, 273, 64));
        REQUIRE_FALSE(check_capability_bit_at_width(real_mouse_64, 30, 64));
    }

    SECTION("wrapper parses at the running kernel's word width") {
        // Built the way this machine's own kernel would print a mouse's key
        // caps; the wrapper must resolve bits at the kernel's word width.
        const std::string mouse_key = mouse_key_caps();
        REQUIRE(check_capability_bit(mouse_key, 272));
        REQUIRE(check_capability_bit(mouse_key, 273));
        REQUIRE_FALSE(check_capability_bit(mouse_key, 30));
    }

    SECTION("negative bit number returns false") {
        REQUIRE_FALSE(check_capability_bit("ffffffff", -1));
    }
}

TEST_CASE("check_capability_bit_at_width parses real sysfs field strings", "[input]") {
    SECTION("64-bit mouse key caps from an x86_64 field report") {
        // Real /sys/class/input/eventN/device/capabilities/key of a USB mouse
        // on a 64-bit kernel: BTN_LEFT..BTN_EXTRA (272-276) land in word 4
        // from the right. Every word is short, which is why digit-count
        // inference misreads this as 32-bit words.
        const char* mouse_key = "1f0000 0 0 0 0";
        REQUIRE(check_capability_bit_at_width(mouse_key, 272, 64));       // BTN_LEFT
        REQUIRE(check_capability_bit_at_width(mouse_key, 273, 64));       // BTN_RIGHT
        REQUIRE(check_capability_bit_at_width(mouse_key, 276, 64));       // BTN_EXTRA
        REQUIRE_FALSE(check_capability_bit_at_width(mouse_key, 330, 64)); // BTN_TOUCH
        REQUIRE_FALSE(check_capability_bit_at_width(mouse_key, 30, 64));  // KEY_A
    }

    SECTION("64-bit keyboard key caps from the same machine") {
        const char* kb_key = "1000000000007 ff800000000007ff febeffdff3cfffff fffffffffffffffe";
        REQUIRE(check_capability_bit_at_width(kb_key, 30, 64));        // KEY_A
        REQUIRE_FALSE(check_capability_bit_at_width(kb_key, 272, 64)); // no BTN_LEFT
        REQUIRE_FALSE(check_capability_bit_at_width(kb_key, 330, 64)); // no BTN_TOUCH
    }

    SECTION("32-bit mouse key caps (shipped ARM targets)") {
        const char* mouse_key = "1f0000 0 0 0 0 0 0 0 0";
        REQUIRE(check_capability_bit_at_width(mouse_key, 272, 32));
        REQUIRE_FALSE(check_capability_bit_at_width(mouse_key, 30, 32));
    }

    SECTION("32-bit KEY_POWER word") {
        const char* power_key = "0 0 0 0 0 100000 0 0 0";
        REQUIRE(check_capability_bit_at_width(power_key, 116, 32));
        REQUIRE_FALSE(check_capability_bit_at_width(power_key, 30, 32));
    }

    SECTION("32-bit multitouch abs caps") {
        const char* mt_abs = "600000 0"; // ABS_MT_POSITION_X(53) + Y(54)
        REQUIRE(check_capability_bit_at_width(mt_abs, 53, 32));
        REQUIRE(check_capability_bit_at_width(mt_abs, 54, 32));
    }

    SECTION("negative bit and empty string return false at both widths") {
        REQUIRE_FALSE(check_capability_bit_at_width("ffffffff", -1, 32));
        REQUIRE_FALSE(check_capability_bit_at_width("ffffffff", -1, 64));
        REQUIRE_FALSE(check_capability_bit_at_width("", 30, 32));
        REQUIRE_FALSE(check_capability_bit_at_width("", 30, 64));
    }
}

TEST_CASE("sysfs bitmap word width maps kernel arch to word bits", "[input]") {
    SECTION("64-bit kernels") {
        REQUIRE(sysfs_bitmap_word_bits_for_machine("x86_64") == 64);
        REQUIRE(sysfs_bitmap_word_bits_for_machine("aarch64") == 64);
        REQUIRE(sysfs_bitmap_word_bits_for_machine("mips64") == 64);
        REQUIRE(sysfs_bitmap_word_bits_for_machine("riscv64") == 64);
        REQUIRE(sysfs_bitmap_word_bits_for_machine("s390x") == 64);
    }

    SECTION("armv8l is 32-bit userland on a 64-bit kernel") {
        // A 32-bit process on an aarch64 kernel sees COMPAT_UTS_MACHINE
        // "armv8l"; the kernel that printed the bitmap is still 64-bit.
        REQUIRE(sysfs_bitmap_word_bits_for_machine("armv8l") == 64);
    }

    SECTION("32-bit kernels") {
        REQUIRE(sysfs_bitmap_word_bits_for_machine("armv7l") == 32);
        REQUIRE(sysfs_bitmap_word_bits_for_machine("armv6l") == 32);
        REQUIRE(sysfs_bitmap_word_bits_for_machine("i686") == 32);
    }

    SECTION("running machine resolves to a real kernel word width") {
        const int bits = sysfs_bitmap_word_bits();
        REQUIRE((bits == 32 || bits == 64));
    }
}

TEST_CASE("find_mouse_device detects USB HID mice via sysfs", "[input]") {
    using helix::input::find_mouse_device;

    SECTION("detects mouse with REL_X + REL_Y + BTN_LEFT") {
        MockInputTree tree("mouse_basic");
        tree.add_device(3, "Logitech USB Mouse",
                        {{"rel", "3"}, {"key", mouse_key_caps()}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Logitech USB Mouse");
        REQUIRE(result->event_num == 3);
    }

    SECTION("skips touchscreen with ABS_X + ABS_Y") {
        MockInputTree tree("mouse_skip_touch");
        tree.add_device(0, "Goodix Touchscreen", {{"abs", "3"}, {"rel", "0"}, {"key", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device without BTN_LEFT") {
        MockInputTree tree("mouse_no_btn");
        tree.add_device(1, "Some Sensor", {{"rel", "3"}, {"key", "0"}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("empty directory returns nullopt") {
        MockInputTree tree("mouse_empty");
        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("picks mouse when touchscreen also present") {
        MockInputTree tree("mouse_with_touch");
        tree.add_device(0, "Goodix Touchscreen", {{"abs", "3"}, {"rel", "0"}, {"key", "0"}});
        tree.add_device(2, "USB Mouse", {{"rel", "3"}, {"key", mouse_key_caps()}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Mouse");
        REQUIRE(result->event_num == 2);
    }

    SECTION("skips device with both ABS and REL (touchscreen with mouse emulation)") {
        MockInputTree tree("mouse_abs_rel");
        tree.add_device(0, "TouchMouseCombo",
                        {{"abs", "3"}, {"rel", "3"}, {"key", mouse_key_caps()}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips MT-only touchscreen (ABS_MT_POSITION_X/Y without legacy ABS_X/Y)") {
        MockInputTree tree("mouse_mt_only");
        // Goodix GT911 MT-only: ABS_MT_POSITION_X=53, ABS_MT_POSITION_Y=54
        // bits 53+54 set = 0x60000000000000 in a 64-bit word, or
        // in 32-bit: bit 53 = word 1 bit 21, bit 54 = word 1 bit 22
        // = 0x600000 in word 1 from right
        tree.add_device(
            0, "Goodix Capacitive TouchScreen",
            {
                {"abs", caps_string({53, 54})}, // ABS_MT_POSITION_X(53) + ABS_MT_POSITION_Y(54)
                {"rel", "0"},
                {"key", caps_string({330})} // BTN_TOUCH(330)
            });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device with BTN_TOUCH even without ABS axes") {
        MockInputTree tree("mouse_btn_touch");
        // Hypothetical device with REL_X/REL_Y + BTN_LEFT but also BTN_TOUCH
        tree.add_device(0, "WeirdTouchDevice",
                        {
                            {"abs", "0"},
                            {"rel", "3"},
                            {"key", caps_string({330, 272, 273, 274, 275,
                                                 276})} // BTN_TOUCH(330) + BTN_LEFT(272)
                        });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("still detects real mouse when MT-only touchscreen present") {
        MockInputTree tree("mouse_with_mt_touch");
        tree.add_device(
            0, "Goodix Capacitive TouchScreen",
            {{"abs", caps_string({53, 54})}, {"rel", "0"}, {"key", caps_string({330})}});
        tree.add_device(2, "USB Mouse", {{"rel", "3"}, {"key", mouse_key_caps()}, {"abs", "0"}});

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Mouse");
    }

    SECTION("skips MCE IR receiver with mouse capabilities (non-USB bus)") {
        MockInputTree tree("mouse_mce_ir");
        // Real-world: Allwinner sunxi-ir-tx MCE device has REL_X+Y, BTN_LEFT,
        // full keyboard keys, but is on the platform bus (0x0019), not USB.
        tree.add_device(
            3, "MCE IR Keyboard/Mouse (sunxi-ir-tx)",
            {{"abs", "0"},
             {"rel", "3"},
             {"key", "30000 0 7 ff87207a c14057ff febeffdf ffefffff ffffffff fffffffe"}},
            "0019"); // BUS_HOST

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device with no bustype file") {
        MockInputTree tree("mouse_no_bus");
        tree.add_device(0, "Virtual Mouse", {{"rel", "3"}, {"key", mouse_key_caps()}, {"abs", "0"}},
                        ""); // No bustype file

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("detects Bluetooth mouse") {
        MockInputTree tree("mouse_bt");
        tree.add_device(0, "BT Mouse", {{"rel", "3"}, {"key", mouse_key_caps()}, {"abs", "0"}},
                        "0005"); // BUS_BLUETOOTH

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "BT Mouse");
    }
}

TEST_CASE("find_touch_device picks the touchscreen via sysfs", "[input]") {
    using helix::input::find_touch_device;

    const std::string mt_only_abs = caps_string({53, 54});
    auto set_attr = [](MockInputTree& tree, int event_num, const std::string& attr,
                       const std::string& value) {
        std::ofstream(tree.sysfs_dir + "/event" + std::to_string(event_num) + "/device/" + attr)
            << value;
    };

    SECTION("finds an MT-only touchscreen that the mouse scan skips") {
        MockInputTree tree("touch_mt_only");
        tree.add_device(0, "tlsc6x_touch", {{"abs", mt_only_abs}, {"key", caps_string({330})}},
                        "0018");

        auto result = find_touch_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(result->path == tree.dev_dir + "/event0");
        CHECK(result->event_num == 0);
        CHECK_FALSE(helix::input::find_mouse_device(tree.dev_dir, tree.sysfs_dir).has_value());
    }

    SECTION("ignores devices without touch axes") {
        MockInputTree tree("touch_none");
        tree.add_device(0, "gpio-keys", {{"abs", "0"}, {"key", caps_string({116})}}, "0019");
        tree.add_device(1, "USB Mouse", {{"rel", "3"}, {"key", mouse_key_caps()}, {"abs", "0"}});

        CHECK_FALSE(find_touch_device(tree.dev_dir, tree.sysfs_dir).has_value());
    }

    SECTION("a non-touch device on a lower event number is not taken") {
        MockInputTree tree("touch_after_keys");
        tree.add_device(0, "gpio-keys", {{"abs", "0"}, {"key", caps_string({116})}}, "0019");
        tree.add_device(1, "Generic ABS Panel", {{"abs", "3"}}, "0019");

        auto result = find_touch_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(result->event_num == 1);
    }

    SECTION("a known touchscreen name outranks an unknown one") {
        MockInputTree tree("touch_known");
        tree.add_device(0, "Generic ABS Panel", {{"abs", "3"}}, "0019");
        tree.add_device(3, "Goodix Capacitive TouchScreen", {{"abs", mt_only_abs}}, "0019");

        auto result = find_touch_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(result->event_num == 3);
    }

    SECTION("INPUT_PROP_DIRECT outranks a USB phys") {
        MockInputTree tree("touch_direct");
        tree.add_device(0, "USB Digitizer", {{"abs", "3"}});
        set_attr(tree, 0, "phys", "usb-0000:01:00.0-1.3/input0");
        tree.add_device(2, "Panel ABS", {{"abs", "3"}}, "0019");
        set_attr(tree, 2, "properties", "2");

        auto result = find_touch_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(result->event_num == 2);
    }

    SECTION("a USB phys breaks an otherwise equal score") {
        MockInputTree tree("touch_usb");
        tree.add_device(0, "Panel A", {{"abs", "3"}}, "0019");
        tree.add_device(4, "Panel B", {{"abs", "3"}});
        set_attr(tree, 4, "phys", "usb-0000:01:00.0-1.3/input0");

        auto result = find_touch_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(result->event_num == 4);
    }

    SECTION("equal scores go to the lowest event number") {
        MockInputTree tree("touch_tie");
        tree.add_device(5, "Panel A", {{"abs", "3"}}, "0019");
        tree.add_device(2, "Panel B", {{"abs", "3"}}, "0019");
        tree.add_device(7, "Panel C", {{"abs", "3"}}, "0019");

        auto result = find_touch_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(result->event_num == 2);
        CHECK(result->name == "Panel B");
    }
}

TEST_CASE("a touch panel that dropped off the bus is reopened only on a real touch device",
          "[input][display]") {
    using helix::input::find_touch_reopen_path;

    SECTION("an override is reopened once its node is back") {
        MockInputTree tree("reopen_override_back");
        tree.add_device(4, "Generic ABS Panel", {{"abs", "3"}}, "0019");

        auto result =
            find_touch_reopen_path(tree.dev_dir + "/event4", tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(*result == tree.dev_dir + "/event4");
    }

    SECTION("an override whose node is still missing waits, even beside another touch panel") {
        MockInputTree tree("reopen_override_missing");
        tree.add_device(1, "Generic ABS Panel", {{"abs", "3"}}, "0019");

        CHECK_FALSE(find_touch_reopen_path(tree.dev_dir + "/event4", tree.dev_dir, tree.sysfs_dir)
                        .has_value());
    }

    SECTION("without an override the panel is found on the node it re-enumerated as") {
        MockInputTree tree("reopen_renumbered");
        tree.add_device(0, "gpio-keys", {{"abs", "0"}, {"key", caps_string({116})}}, "0019");
        tree.add_device(5, "WaveShare WS170120", {{"abs", "3"}});

        auto result = find_touch_reopen_path("", tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        CHECK(*result == tree.dev_dir + "/event5");
    }

    SECTION("without an override a node with no touch axes is never taken") {
        // Startup on fbdev settles on the first readable event node when no
        // touchscreen exists; a reopen that did the same would end the retry
        // on a keyboard while the panel is still away.
        MockInputTree tree("reopen_no_touch");
        tree.add_device(0, "gpio-keys", {{"abs", "0"}, {"key", caps_string({116})}}, "0019");
        tree.add_device(1, "USB Mouse", {{"rel", "3"}, {"key", mouse_key_caps()}, {"abs", "0"}});

        CHECK_FALSE(find_touch_reopen_path("", tree.dev_dir, tree.sysfs_dir).has_value());
    }
}

TEST_CASE("event_number_for_device reads the number only from an event node", "[input][display]") {
    using helix::input::event_number_for_device;

    SECTION("an event node gives its number") {
        CHECK(event_number_for_device("/dev/input/event7") == 7);
    }

    SECTION("a path with no event node gives none") {
        CHECK(event_number_for_device("/dev/input/mouse0") == -1);
        CHECK(event_number_for_device("/dev/input/by-id/usb-Panel-touch") == -1);
        CHECK(event_number_for_device("event") == -1);
        CHECK(event_number_for_device("") == -1);
    }

    SECTION("an event name in a directory is not the node") {
        CHECK(event_number_for_device("/tmp/event3/touch") == -1);
    }

    SECTION("a link to an event node gives the node's number") {
        MockInputTree tree("event_number_link");
        tree.add_device(5, "Panel", {{"abs", "3"}});
        const std::string link = tree.dev_dir + "/by-id-panel";
        fs::create_symlink(tree.dev_dir + "/event5", link);
        CHECK(event_number_for_device(link) == 5);
    }
}

TEST_CASE("pointer devices are classified by how they position themselves", "[input][rotation]") {
    using helix::input::classify_pointer_capabilities;
    using helix::input::PointerKind;

    // The Pi 3B's ft5x06 and Logitech M705, as its InputScanner logs them.
    const std::string ft5x06_abs = caps_string({0, 1, 47, 53, 54, 57});
    const std::string btn_touch = caps_string({330});

    SECTION("a USB mouse is relative") {
        CHECK(classify_pointer_capabilities("0", "1943", mouse_key_caps()) ==
              PointerKind::Relative);
    }

    SECTION("a touch panel is panel-absolute") {
        CHECK(classify_pointer_capabilities(ft5x06_abs, "0", btn_touch) ==
              PointerKind::PanelAbsolute);
        CHECK(classify_pointer_capabilities("3", "0", "0") == PointerKind::PanelAbsolute);
        CHECK(classify_pointer_capabilities(caps_string({53, 54}), "0", "0") ==
              PointerKind::PanelAbsolute);
    }

    SECTION("touchscreen capabilities win over relative axes") {
        CHECK(classify_pointer_capabilities("3", "3", mouse_key_caps()) ==
              PointerKind::PanelAbsolute);
        CHECK(classify_pointer_capabilities("0", "3", btn_touch) == PointerKind::PanelAbsolute);
    }

    SECTION("relative motion needs both REL_X and REL_Y") {
        CHECK(classify_pointer_capabilities("0", "1", mouse_key_caps()) ==
              PointerKind::PanelAbsolute);
        CHECK(classify_pointer_capabilities("", "", "") == PointerKind::PanelAbsolute);
    }
}

TEST_CASE("pointer_kind_for_device reads the capabilities of the device it names",
          "[input][rotation]") {
    using helix::input::pointer_kind_for_device;
    using helix::input::PointerKind;

    MockInputTree tree("pointer_kind");
    tree.add_device(
        2, "generic ft5x06 (79)",
        {{"abs", caps_string({0, 1, 47, 53, 54, 57})}, {"rel", "0"}, {"key", caps_string({330})}},
        "0018");
    tree.add_device(5, "Logitech M705", {{"abs", "0"}, {"rel", "1943"}, {"key", mouse_key_caps()}});

    SECTION("an event node") {
        CHECK(pointer_kind_for_device(tree.dev_dir + "/event2", tree.sysfs_dir) ==
              PointerKind::PanelAbsolute);
        CHECK(pointer_kind_for_device(tree.dev_dir + "/event5", tree.sysfs_dir) ==
              PointerKind::Relative);
    }

    SECTION("a link to an event node classifies as its target") {
        // Configured devices are often named through /dev/input/by-id, whose names
        // do not start with "event".
        const std::string by_id = tree.base + "/dev/input/by-id";
        fs::create_directories(by_id);
        fs::create_symlink(tree.dev_dir + "/event5", by_id + "/usb-Logitech_M705-event-mouse");
        CHECK(pointer_kind_for_device(by_id + "/usb-Logitech_M705-event-mouse", tree.sysfs_dir) ==
              PointerKind::Relative);
    }

    SECTION("a path whose capabilities cannot be read is panel-absolute") {
        CHECK(pointer_kind_for_device("", tree.sysfs_dir) == PointerKind::PanelAbsolute);
        CHECK(pointer_kind_for_device(tree.dev_dir + "/event9", tree.sysfs_dir) ==
              PointerKind::PanelAbsolute);
    }
}

TEST_CASE("find_keyboard_device detects USB HID keyboards via sysfs", "[input]") {
    using helix::input::find_keyboard_device;

    SECTION("detects keyboard with KEY_A") {
        MockInputTree tree("kb_basic");
        tree.add_device(1, "USB Keyboard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("skips power button (KEY_POWER=116 but no KEY_A)") {
        MockInputTree tree("kb_power");
        tree.add_device(0, "Power Button",
                        {{"key", caps_string({116})}, {"rel", "0"}, {"abs", "0"}}, "0019");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("finds keyboard among other devices") {
        MockInputTree tree("kb_mixed");
        tree.add_device(0, "Goodix Touchscreen", {{"key", "0"}, {"abs", "3"}, {"rel", "0"}},
                        "0018");
        tree.add_device(1, "Power Button", {{"key", "100000"}, {"abs", "0"}, {"rel", "0"}}, "0019");
        tree.add_device(2, "USB Keyboard", {{"key", "10000 40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("empty directory returns nullopt") {
        MockInputTree tree("kb_empty");
        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("combo keyboard+mouse device detected as keyboard") {
        MockInputTree tree("kb_combo");
        tree.add_device(
            0, "Logitech K400",
            {{"key", caps_string({30, 272, 273, 274, 275, 276})}, {"rel", "3"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Logitech K400");
    }

    SECTION("skips MCE IR device with KEY_A (non-USB bus)") {
        MockInputTree tree("kb_mce_ir");
        tree.add_device(
            3, "MCE IR Keyboard/Mouse (sunxi-ir-tx)",
            {{"abs", "0"},
             {"rel", "3"},
             {"key", "30000 0 7 ff87207a c14057ff febeffdf ffefffff ffffffff fffffffe"}},
            "0019");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("find_keyboard_device excludes barcode scanners", "[input]") {
    using helix::input::find_keyboard_device;

    SECTION("skips device with 'barcode' in name") {
        MockInputTree tree("kb_excl_barcode");
        tree.add_device(1, "Tera Barcode Scanner",
                        {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device with 'scanner' in name (case-insensitive)") {
        MockInputTree tree("kb_excl_scanner");
        tree.add_device(1, "QR SCANNER Pro", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device matching exclude_vendor_product") {
        MockInputTree tree("kb_excl_vid");
        tree.add_device_with_ids(1, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("returns real keyboard when scanner is excluded by vendor:product") {
        MockInputTree tree("kb_excl_with_real");
        tree.add_device_with_ids(1, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "1a2c",
                                 "4c5e");
        tree.add_device(2, "USB Keyboard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("returns real keyboard when scanner is excluded by name") {
        MockInputTree tree("kb_excl_name_real");
        tree.add_device(1, "Tera Barcode Scanner",
                        {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});
        tree.add_device(2, "USB Keyboard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("no exclude param still returns generic HID keyboard") {
        MockInputTree tree("kb_excl_none");
        tree.add_device(1, "TMS HIDKeyBoard", {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}});

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "TMS HIDKeyBoard");
    }
}

TEST_CASE("find_hid_keyboard_devices detects USB HID keyboards for scanner use", "[input]") {
    using helix::input::find_hid_keyboard_devices;

    SECTION("detects generic USB HID keyboard (e.g. QR scanner reporting as USBKey)") {
        MockInputTree tree("hid_generic");
        tree.add_device(0, "goodix-ts", {{"abs", "3"}, {"key", "0"}, {"rel", "0"}}, "0018");
        tree.add_device(2, "USBKey Chip USBKey Module",
                        {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "USBKey Chip USBKey Module");
    }

    SECTION("named barcode scanner is returned before generic keyboard") {
        MockInputTree tree("hid_priority");
        tree.add_device(1, "USBKey Chip USBKey Module",
                        {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});
        tree.add_device(2, "Tera Barcode Scanner",
                        {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 2);
        // Named scanner should come first
        REQUIRE(result[0].name == "Tera Barcode Scanner");
        REQUIRE(result[1].name == "USBKey Chip USBKey Module");
    }

    SECTION("skips touchscreen devices") {
        MockInputTree tree("hid_skip_touch");
        tree.add_device(0, "goodix-ts", {{"abs", "3"}, {"key", "40000000"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("skips non-USB devices") {
        MockInputTree tree("hid_skip_platform");
        tree.add_device(0, "MCE IR Keyboard", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}},
                        "0019");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("skips devices without KEY_A") {
        MockInputTree tree("hid_skip_no_keya");
        tree.add_device(0, "Power Button", {{"key", "100000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("returns multiple USB HID devices") {
        MockInputTree tree("hid_multi");
        tree.add_device(1, "USB Keyboard", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});
        tree.add_device(3, "QR Scanner", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}});

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 2);
    }

    SECTION("empty directory returns empty vector") {
        MockInputTree tree("hid_empty");
        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("configured vendor:product device wins over named scanner") {
        MockInputTree tree("hid_configured");
        tree.add_device_with_ids(1, "Tera Barcode Scanner",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "aaaa",
                                 "bbbb");
        tree.add_device_with_ids(2, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "TMS HIDKeyBoard");
    }

    SECTION("configured device not found falls back to normal priority") {
        MockInputTree tree("hid_configured_missing");
        tree.add_device_with_ids(1, "Tera Barcode Scanner",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "aaaa",
                                 "bbbb");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "Tera Barcode Scanner");
    }

    SECTION("empty configured string uses normal priority") {
        MockInputTree tree("hid_configured_empty");
        tree.add_device_with_ids(1, "USBKey Module",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1234",
                                 "5678");
        tree.add_device_with_ids(2, "Tera Barcode Scanner",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "aaaa",
                                 "bbbb");

        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, "");
        REQUIRE(result.size() == 2);
        REQUIRE(result[0].name == "Tera Barcode Scanner");
    }
}

TEST_CASE("is_vid_pid_blacklisted matches normalized vid:pid entries", "[input]") {
    using helix::input::is_vid_pid_blacklisted;

    SECTION("empty blacklist returns false") {
        REQUIRE_FALSE(is_vid_pid_blacklisted("1a2c", "0003", {}));
    }

    SECTION("exact lowercase match") {
        REQUIRE(is_vid_pid_blacklisted("1a2c", "0003", {"1a2c:0003"}));
    }

    SECTION("case-insensitive match (uppercase entry vs lowercase sysfs)") {
        REQUIRE(is_vid_pid_blacklisted("1a2c", "0003", {"1A2C:0003"}));
    }

    SECTION("case-insensitive match (uppercase sysfs vs lowercase entry)") {
        REQUIRE(is_vid_pid_blacklisted("1A2C", "0003", {"1a2c:0003"}));
    }

    SECTION("whitespace around entry is ignored") {
        REQUIRE(is_vid_pid_blacklisted("1a2c", "0003", {"  1a2c : 0003  "}));
    }

    SECTION("leading 0x prefix is stripped on both sides") {
        REQUIRE(is_vid_pid_blacklisted("0x1a2c", "0x0003", {"1a2c:0003"}));
        REQUIRE(is_vid_pid_blacklisted("1a2c", "0003", {"0x1a2c:0x0003"}));
    }

    SECTION("non-matching entry returns false") {
        REQUIRE_FALSE(is_vid_pid_blacklisted("1a2c", "0003", {"aaaa:bbbb"}));
    }

    SECTION("vendor matches but product differs returns false") {
        REQUIRE_FALSE(is_vid_pid_blacklisted("1a2c", "0003", {"1a2c:9999"}));
    }

    SECTION("matches when present among several entries") {
        REQUIRE(is_vid_pid_blacklisted("002c", "261a", {"aaaa:bbbb", "002c:261a", "1111:2222"}));
    }

    SECTION("malformed entry without colon is skipped gracefully") {
        REQUIRE_FALSE(is_vid_pid_blacklisted("1a2c", "0003", {"1a2c0003"}));
        // A malformed entry alongside a valid one must not break matching.
        REQUIRE(is_vid_pid_blacklisted("1a2c", "0003", {"garbage", "1a2c:0003"}));
    }

    SECTION("empty vendor/product never matches") {
        REQUIRE_FALSE(is_vid_pid_blacklisted("", "", {"1a2c:0003"}));
    }
}

TEST_CASE("find_keyboard_device honors the device blacklist", "[input]") {
    using helix::input::find_keyboard_device;

    SECTION("blacklisted VID:PID keyboard is skipped") {
        MockInputTree tree("kb_blacklist_skip");
        tree.add_device_with_ids(1, "Generic HID Keyboard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "002c",
                                 "261a");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "", {"002c:261a"});
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("non-blacklisted keyboard is still returned") {
        MockInputTree tree("kb_blacklist_keep");
        tree.add_device_with_ids(1, "Generic HID Keyboard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "04d9",
                                 "a070");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "", {"002c:261a"});
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Generic HID Keyboard");
    }

    SECTION("real keyboard returned when a blacklisted scanner is also present") {
        MockInputTree tree("kb_blacklist_mixed");
        tree.add_device_with_ids(1, "Generic HID Keyboard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "002c",
                                 "261a");
        tree.add_device_with_ids(2, "Real Keyboard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "04d9",
                                 "a070");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "", {"002c:261a"});
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Real Keyboard");
    }

    SECTION("empty blacklist behaves exactly as before") {
        MockInputTree tree("kb_blacklist_empty");
        tree.add_device_with_ids(1, "Generic HID Keyboard",
                                 {{"key", "40000000"}, {"rel", "0"}, {"abs", "0"}}, "0003", "002c",
                                 "261a");

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir, "", {});
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Generic HID Keyboard");
    }
}

TEST_CASE("find_hid_keyboard_devices honors the device blacklist", "[input]") {
    using helix::input::find_hid_keyboard_devices;

    SECTION("blacklisted device is omitted, others present") {
        MockInputTree tree("hid_blacklist_skip");
        tree.add_device_with_ids(1, "Generic HID Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "002c",
                                 "261a");
        tree.add_device_with_ids(2, "Real Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "04d9",
                                 "a070");

        std::vector<std::string> blacklist{"002c:261a"};
        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, blacklist);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "Real Keyboard");
    }

    SECTION("empty blacklist behaves exactly as before") {
        MockInputTree tree("hid_blacklist_empty");
        tree.add_device_with_ids(1, "Generic HID Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "002c",
                                 "261a");
        tree.add_device_with_ids(2, "Real Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "04d9",
                                 "a070");

        std::vector<std::string> blacklist;
        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, blacklist);
        REQUIRE(result.size() == 2);
    }

    SECTION("configured overload also filters the blacklist") {
        MockInputTree tree("hid_blacklist_configured");
        tree.add_device_with_ids(1, "Generic HID Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "002c",
                                 "261a");
        tree.add_device_with_ids(2, "Real Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "04d9",
                                 "a070");

        // No configured VID:PID, but the blacklist must still be applied.
        auto result = find_hid_keyboard_devices(tree.dev_dir, tree.sysfs_dir, std::string(""),
                                                std::vector<std::string>{"002c:261a"});
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "Real Keyboard");
    }
}

TEST_CASE("find_configured_hid_device reports only the configured device", "[input]") {
    using helix::input::find_configured_hid_device;
    const std::map<std::string, std::string> kbd_caps{
        {"key", "40000000"}, {"abs", "0"}, {"rel", "0"}};

    SECTION("configured device present is returned") {
        MockInputTree tree("cfg_hid_present");
        tree.add_device_with_ids(1, "Tera Barcode Scanner", kbd_caps, "0003", "aaaa", "bbbb");
        tree.add_device_with_ids(2, "TMS HIDKeyBoard", kbd_caps, "0003", "1a2c", "4c5e");

        auto result = find_configured_hid_device(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e");
        REQUIRE(result.has_value());
        REQUIRE(result->name == "TMS HIDKeyBoard");
    }

    SECTION("configured device absent does not fall back to another keyboard") {
        MockInputTree tree("cfg_hid_absent");
        tree.add_device_with_ids(1, "Tera Barcode Scanner", kbd_caps, "0003", "aaaa", "bbbb");

        REQUIRE_FALSE(find_configured_hid_device(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e"));
    }

    SECTION("empty or malformed configured id finds nothing") {
        MockInputTree tree("cfg_hid_empty");
        tree.add_device_with_ids(1, "Tera Barcode Scanner", kbd_caps, "0003", "aaaa", "bbbb");

        REQUIRE_FALSE(find_configured_hid_device(tree.dev_dir, tree.sysfs_dir, ""));
        REQUIRE_FALSE(find_configured_hid_device(tree.dev_dir, tree.sysfs_dir, "aaaabbbb"));
    }

    SECTION("blacklisted configured device is not present") {
        MockInputTree tree("cfg_hid_blacklisted");
        tree.add_device_with_ids(1, "TMS HIDKeyBoard", kbd_caps, "0003", "1a2c", "4c5e");

        REQUIRE_FALSE(find_configured_hid_device(tree.dev_dir, tree.sysfs_dir, "1a2c:4c5e",
                                                 std::vector<std::string>{"1a2c:4c5e"}));
    }
}

TEST_CASE("enumerate_usb_hid_devices returns devices with vendor/product IDs", "[input]") {
    using helix::input::enumerate_usb_hid_devices;

    SECTION("returns device with correct vendor/product IDs") {
        MockInputTree tree("enum_basic");
        tree.add_device_with_ids(2, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 1);
        REQUIRE(result[0].name == "TMS HIDKeyBoard");
        REQUIRE(result[0].vendor_id == "1a2c");
        REQUIRE(result[0].product_id == "4c5e");
        REQUIRE(result[0].event_path.find("event2") != std::string::npos);
    }

    SECTION("returns multiple devices") {
        MockInputTree tree("enum_multi");
        tree.add_device_with_ids(1, "USB Keyboard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "04d9",
                                 "a070");
        tree.add_device_with_ids(3, "TMS HIDKeyBoard",
                                 {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}}, "0003", "1a2c",
                                 "4c5e");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.size() == 2);
    }

    SECTION("skips non-USB devices") {
        MockInputTree tree("enum_skip_platform");
        tree.add_device(0, "MCE IR Keyboard", {{"key", "40000000"}, {"abs", "0"}, {"rel", "0"}},
                        "0019");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("skips touchscreens") {
        MockInputTree tree("enum_skip_touch");
        tree.add_device_with_ids(0, "Goodix TS", {{"abs", "3"}, {"key", "40000000"}, {"rel", "0"}},
                                 "0003", "1234", "5678");

        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }

    SECTION("empty directory returns empty vector") {
        MockInputTree tree("enum_empty");
        auto result = enumerate_usb_hid_devices(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.empty());
    }
}
