// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The Host Terminal (#1573): the libvterm screen, the pty session and its output
// buffer, the pure input/layout helpers the overlay is built on, and the platform gate.

#include "ui_panel_host_terminal.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "platform_info.h"
#include "system/pty_session.h"
#include "system/terminal_screen.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using helix::terminal::Key;
using helix::terminal::OutputBuffer;
using helix::terminal::PtySession;
using helix::terminal::TerminalScreen;

// ---------------------------------------------------------------------------
// TerminalScreen
// ---------------------------------------------------------------------------

TEST_CASE("TerminalScreen draws text and moves the cursor", "[host_terminal]") {
    TerminalScreen screen(4, 20, 10);
    screen.feed("hello\r\nworld");
    CHECK(screen.row_text(0) == "hello");
    CHECK(screen.row_text(1) == "world");
    CHECK(screen.screen_text() == "hello\nworld");
    CHECK(screen.cursor().row == 1);
    CHECK(screen.cursor().col == 5);
}

TEST_CASE("TerminalScreen interprets escape sequences rather than printing them",
          "[host_terminal]") {
    TerminalScreen screen(4, 20, 10);
    screen.feed("\x1b[31mred\x1b[0m");
    CHECK(screen.row_text(0) == "red");

    screen.feed("\x1b[2J\x1b[3;5Hx");
    CHECK(screen.row_text(0).empty());
    CHECK(screen.row_text(2) == "    x");

    screen.feed("\x1b[?1049h");
    CHECK(screen.alt_screen());
    screen.feed("\x1b[?1049l");
    CHECK_FALSE(screen.alt_screen());
}

TEST_CASE("TerminalScreen keeps a capped scrollback, oldest dropped first", "[host_terminal]") {
    TerminalScreen screen(3, 10, 2);
    screen.feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
    CHECK(screen.screen_text() == "4\n5\n6");
    CHECK(screen.scrollback_pushed() == 3);
    REQUIRE(screen.scrollback().size() == 2);
    CHECK(screen.scrollback().front() == "2");
    CHECK(screen.scrollback().back() == "3");
}

TEST_CASE("TerminalScreen draws a double-width character once", "[host_terminal]") {
    TerminalScreen screen(2, 10, 0);
    screen.feed("\xe6\x97\xa5\xe6\x9c\xac!"); // 日本!
    CHECK(screen.row_text(0) == "\xe6\x97\xa5\xe6\x9c\xac!");
    CHECK(screen.cursor().col == 5);
}

TEST_CASE("TerminalScreen encodes keys the way an xterm sends them", "[host_terminal]") {
    TerminalScreen screen(4, 20, 0);

    screen.type_text("ls -l");
    CHECK(screen.take_input() == "ls -l");
    CHECK(screen.take_input().empty());

    screen.type_text("c", true);
    CHECK(screen.take_input() == "\x03");
    screen.type_text("D", true);
    CHECK(screen.take_input() == "\x04");

    screen.type_text("\n");
    CHECK(screen.take_input() == "\r");
    screen.press_key(Key::Backspace);
    CHECK(screen.take_input() == "\x7f");
    screen.press_key(Key::Tab);
    CHECK(screen.take_input() == "\t");
    screen.press_key(Key::Escape);
    CHECK(screen.take_input() == "\x1b");
    screen.press_key(Key::Up);
    CHECK(screen.take_input() == "\x1b[A");
    screen.press_key(Key::Left);
    CHECK(screen.take_input() == "\x1b[D");
}

TEST_CASE("TerminalScreen answers a child's terminal queries", "[host_terminal]") {
    TerminalScreen screen(4, 20, 0);
    screen.feed("ab\x1b[6n"); // where is the cursor?
    CHECK(screen.take_input() == "\x1b[1;3R");
}

TEST_CASE("TerminalScreen resize keeps the cursor on the grid", "[host_terminal]") {
    TerminalScreen screen(10, 40, 0);
    screen.feed("\x1b[9;30H");
    screen.resize(4, 20);
    CHECK(screen.rows() == 4);
    CHECK(screen.cols() == 20);
    CHECK(screen.cursor().row < 4);
    CHECK(screen.cursor().col < 20);
    screen.resize(0, -3);
    CHECK(screen.rows() == 1);
    CHECK(screen.cols() == 1);
}

// ---------------------------------------------------------------------------
// OutputBuffer
// ---------------------------------------------------------------------------

TEST_CASE("OutputBuffer holds the reader back while full", "[host_terminal]") {
    OutputBuffer buffer(4);
    bool was_empty = false;
    REQUIRE(buffer.append("abcd", was_empty));
    CHECK(was_empty);

    std::atomic<bool> appended{false};
    std::thread reader([&] {
        bool empty_before = true;
        buffer.append("e", empty_before);
        appended = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(appended.load());
    CHECK(buffer.take() == "abcd");
    reader.join();
    CHECK(appended.load());
    CHECK(buffer.take() == "e");
}

TEST_CASE("OutputBuffer close releases a blocked reader and refuses more", "[host_terminal]") {
    OutputBuffer buffer(2);
    bool was_empty = false;
    REQUIRE(buffer.append("xy", was_empty));
    std::atomic<int> result{-1};
    std::thread reader([&] {
        bool empty_before = false;
        result = buffer.append("z", empty_before) ? 1 : 0;
    });
    buffer.close();
    reader.join();
    CHECK(result.load() == 0);
    CHECK_FALSE(buffer.append("q", was_empty));
}

// ---------------------------------------------------------------------------
// PtySession
// ---------------------------------------------------------------------------

namespace {

/// Collects a session's output on the test thread, woken by the session.
struct OutputCollector {
    std::mutex mutex;
    std::condition_variable cv;
    int wakes = 0;

    PtySession::WakeFn wake_fn() {
        return [this] {
            std::lock_guard<std::mutex> lock(mutex);
            ++wakes;
            cv.notify_all();
        };
    }

    /// Drain @p session until @p done says so or two seconds pass.
    template <typename Pred> bool wait_for(PtySession& session, std::string& out, Pred done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            out += session.take_output();
            if (done()) {
                return true;
            }
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(20));
        }
        out += session.take_output();
        return done();
    }
};

} // namespace

TEST_CASE("PtySession runs a shell on a terminal and reaps its exit code", "[host_terminal]") {
    OutputCollector collector;
    PtySession session;
    std::string error;
    REQUIRE(
        session.start({"/bin/sh"}, {"TERM=xterm", "PS1=$ "}, 24, 80, collector.wake_fn(), error));
    CHECK(session.running());

    REQUIRE(session.write("[ -t 0 ] && echo tty_$((40+2)); stty size\n"));
    std::string out;
    CHECK(collector.wait_for(session, out, [&] {
        return out.find("tty_42") != std::string::npos && out.find("24 80") != std::string::npos;
    }));

    // A new size reaches the child as its terminal's size (TIOCSWINSZ).
    session.resize(10, 40);
    REQUIRE(session.write("stty size\n"));
    CHECK(collector.wait_for(session, out, [&] { return out.find("10 40") != std::string::npos; }));

    REQUIRE(session.write("exit 3\n"));
    CHECK(collector.wait_for(session, out, [&] { return session.hung_up(); }));
    CHECK(session.stop() == 3);
    CHECK_FALSE(session.running());
    CHECK(session.stop() == -1);
}

TEST_CASE("PtySession stop ends a busy child and every process on its terminal",
          "[host_terminal]") {
    OutputCollector collector;
    PtySession session;
    std::string error;
    REQUIRE(session.start({"/bin/sh"}, {"PS1=$ "}, 24, 80, collector.wake_fn(), error));
    REQUIRE(session.write("trap '' HUP; sleep 300 & echo bg=$!; sleep 300\n"));
    // The terminal echoes the command line too, so the pid is the "bg=" that a
    // digit follows, not the first one.
    std::string out;
    std::size_t at = std::string::npos;
    REQUIRE(collector.wait_for(session, out, [&] {
        for (auto p = out.find("bg="); p != std::string::npos; p = out.find("bg=", p + 1)) {
            if (p + 3 < out.size() && std::isdigit(static_cast<unsigned char>(out[p + 3])) &&
                out.find('\n', p) != std::string::npos) {
                at = p + 3;
                return true;
            }
        }
        return false;
    }));
    const pid_t background = static_cast<pid_t>(std::strtol(out.c_str() + at, nullptr, 10));
    REQUIRE(background > 1);
    const pid_t shell = session.pid();

    const auto started = std::chrono::steady_clock::now();
    session.stop();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK_FALSE(session.running());
    errno = 0;
    CHECK((::kill(shell, 0) == -1 && errno == ESRCH));
    // The background job is no child of ours: once killed it stays a zombie until
    // whichever ancestor adopted it reaps it. It must not still be running.
    bool gone = false;
    for (int i = 0; i < 100 && !gone; ++i) {
        std::ifstream stat("/proc/" + std::to_string(background) + "/stat");
        std::string line;
        // Absent, or a zombie waiting for whichever ancestor adopted it.
        gone = !std::getline(stat, line) || line.find(") Z ") != std::string::npos;
        if (!gone) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    CHECK(gone);
}

TEST_CASE("PtySession refuses a second start and reports a failed exec", "[host_terminal]") {
    OutputCollector collector;
    PtySession session;
    std::string error;
    CHECK_FALSE(session.start({}, {}, 24, 80, collector.wake_fn(), error));
    CHECK_FALSE(error.empty());

    REQUIRE(session.start({"/nonexistent/shell"}, {}, 24, 80, collector.wake_fn(), error));
    std::string out;
    CHECK(collector.wait_for(session, out, [&] { return session.hung_up(); }));
    CHECK(session.stop() == 127);
}

TEST_CASE("session_from_proc_stat reads the session past a hostile command name",
          "[host_terminal]") {
    using helix::terminal::session_from_proc_stat;
    CHECK(session_from_proc_stat("1234 (sh) S 1 1234 1234 34816 1234 4194560") == 1234);
    CHECK(session_from_proc_stat("77 (a) b) (c) R 5 70 71 0 -1") == 71);
    CHECK(session_from_proc_stat("77 (sh) S 5 70") == -1);
    CHECK(session_from_proc_stat("77 sh S 5 70 71") == -1);
    CHECK(session_from_proc_stat("77 (sh) S 5 70 7x1 0") == -1);
}

TEST_CASE("choose_shell prefers an executable $SHELL, else /bin/sh", "[host_terminal]") {
    const auto yes = [](const std::string&) { return true; };
    const auto no = [](const std::string&) { return false; };
    CHECK(helix::terminal::choose_shell("/bin/bash", yes) == "/bin/bash");
    CHECK(helix::terminal::choose_shell("/bin/bash", no) == "/bin/sh");
    CHECK(helix::terminal::choose_shell("bash", yes) == "/bin/sh");
    CHECK(helix::terminal::choose_shell(nullptr, yes) == "/bin/sh");
    CHECK(helix::terminal::choose_shell("", yes) == "/bin/sh");
}

// ---------------------------------------------------------------------------
// Overlay helpers and the platform gate
// ---------------------------------------------------------------------------

TEST_CASE("Host Terminal classifies what the capture field was about to insert",
          "[host_terminal]") {
    using helix::ui::classify_terminal_insert;
    using helix::ui::TerminalInputKind;
    CHECK(classify_terminal_insert("\x7f") == TerminalInputKind::Backspace);
    CHECK(classify_terminal_insert("\n") == TerminalInputKind::Enter);
    CHECK(classify_terminal_insert("\r") == TerminalInputKind::Enter);
    CHECK(classify_terminal_insert("a") == TerminalInputKind::Text);
    CHECK(classify_terminal_insert("\x7f\x7f") == TerminalInputKind::Text);
    CHECK(classify_terminal_insert("ls\n") == TerminalInputKind::Text);
}

TEST_CASE("Host Terminal grid fits the view and stays in range", "[host_terminal]") {
    using helix::ui::HostTerminalPanel;
    using helix::ui::terminal_grid_for;
    auto grid = terminal_grid_for(800, 300, 8, 15);
    CHECK(grid.cols == 100);
    CHECK(grid.rows == 20);

    grid = terminal_grid_for(100, 30, 8, 15);
    CHECK(grid.cols == HostTerminalPanel::kMinCols);
    CHECK(grid.rows == HostTerminalPanel::kMinRows);

    grid = terminal_grid_for(4000, 4000, 8, 15);
    CHECK(grid.cols == HostTerminalPanel::kMaxCols);
    CHECK(grid.rows == HostTerminalPanel::kMaxRows);

    grid = terminal_grid_for(800, 300, 0, 15);
    CHECK(grid.cols == HostTerminalPanel::kMinCols);
}

TEST_CASE("Host Terminal scrollback text keeps the newest lines that fit", "[host_terminal]") {
    const std::deque<std::string> lines{"one", "two", "three"};
    CHECK(helix::ui::join_tail_lines(lines, 100) == "one\ntwo\nthree");
    CHECK(helix::ui::join_tail_lines(lines, 9) == "two\nthree");
    CHECK(helix::ui::join_tail_lines(lines, 5) == "three");
    CHECK(helix::ui::join_tail_lines(lines, 4).empty());
    CHECK(helix::ui::join_tail_lines({}, 10).empty());
}

TEST_CASE("Host Terminal is never offered on Android", "[host_terminal][android][platform]") {
    helix::set_platform_override(0);
    CHECK(helix::platform_host_terminal_supported());
    helix::set_platform_override(1);
    CHECK_FALSE(helix::platform_host_terminal_supported());
    helix::set_platform_override(-1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Host Terminal shows its key row only with the on-screen keyboard up",
                 "[host_terminal][ui]") {
    auto& keyboard = helix::DisplaySettingsManager::instance();
    auto& panel = helix::ui::get_global_host_terminal_panel();
    panel.ensure_subjects();
    panel.register_callbacks();
    lv_obj_t* root = panel.create(lv_screen_active());
    REQUIRE(root != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(root, "key_row");
    REQUIRE(row != nullptr);
    lv_subject_t* ctrl = lv_xml_get_subject(nullptr, "host_terminal_ctrl");
    REQUIRE(ctrl != nullptr);

    keyboard.set_soft_keyboard_visible(false);
    panel.on_activate();
    const auto settle = [&] {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    };
    settle();
    CHECK(lv_obj_is_hidden(row));
    const int rows_without_row = panel.grid().rows;
    REQUIRE(rows_without_row > 0);

    keyboard.set_soft_keyboard_visible(true);
    settle();
    CHECK_FALSE(lv_obj_is_hidden(row));
    // The row takes its height from the terminal, and the grid gives it up.
    CHECK(panel.grid().rows < rows_without_row);

    lv_subject_set_int(ctrl, 1);
    keyboard.set_soft_keyboard_visible(false);
    settle();
    CHECK(lv_obj_is_hidden(row));
    CHECK(panel.grid().rows == rows_without_row);
    CHECK(lv_subject_get_int(ctrl) == 0);

    panel.on_deactivate(DeactivateReason::NavigateAway);
    StaticPanelRegistry::instance().destroy_all();
}
