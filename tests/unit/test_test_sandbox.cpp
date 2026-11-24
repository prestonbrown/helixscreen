// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// helix-tests keeps every file a run writes under one per-run sandbox and
// removes it on exit; whatever a killed run leaves, the next run's startup sweep
// removes. Nothing a run writes may pile up in /tmp, where every scan of /tmp
// pays for each leftover entry.

#include "../test_helpers/test_sandbox.h"
#include "helix_fs.h"
#include "mock_planted_gcodes.h"
#include "moonraker_client_mock.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

bool under(const std::string& path, const std::string& dir) {
    return !dir.empty() && path.rfind(dir + "/", 0) == 0;
}

/// A pid that no process holds: a child that already exited and was reaped.
pid_t reaped_pid() {
    const pid_t pid = fork();
    if (pid == 0) {
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return pid;
}

} // namespace

TEST_CASE("the sandbox sweep removes only the sandboxes of runs that are gone", "[test_sandbox]") {
    char tmpl[] = "/tmp/helix_sweep_test_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const std::string parent = tmpl;
    const std::string dead = parent + "/" + std::to_string(reaped_pid());
    const std::string live = parent + "/" + std::to_string(getpid());
    const std::string other = parent + "/not-a-run";
    for (const auto& dir : {dead, dead + "/cache", live, other}) {
        REQUIRE(mkdir(dir.c_str(), 0700) == 0);
    }
    std::FILE* f = std::fopen((dead + "/cache/thumb").c_str(), "w");
    REQUIRE(f != nullptr);
    std::fclose(f);

    CHECK(helix::test::sweep_dead_sandboxes(parent.c_str()) == 1);

    CHECK_FALSE(helix::fs::exists(dead));
    CHECK(helix::fs::exists(live));
    CHECK(helix::fs::exists(other));
    helix::test::remove_sandbox_tree(parent.c_str());
}

TEST_CASE("a run of helix-tests removes its sandbox when it exits", "[test_sandbox]") {
    if (!helix::fs::exists("/proc/self/exe")) {
        SKIP("needs /proc/self/exe to start this binary again");
    }
    // A listing runs no test case: its sandbox goes only if the exit path takes it.
    const pid_t pid = fork();
    if (pid == 0) {
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execl("/proc/self/exe", "helix-tests", "--list-reporters", static_cast<char*>(nullptr));
        _exit(127);
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE((WIFEXITED(status) && WEXITSTATUS(status) == 0));

    CHECK_FALSE(helix::fs::exists(std::string(helix::test::kTestSandboxParent) + "/" +
                                  std::to_string(pid)));
}

TEST_CASE("the mock's host-side files live in this run's sandbox", "[test_sandbox][mock]") {
    const std::string sandbox =
        std::string(helix::test::kTestSandboxParent) + "/" + std::to_string(getpid());
    REQUIRE(helix::fs::exists(sandbox));
    CHECK(under(helix::mock::scratch_dir(), sandbox));
    CHECK(under(MoonrakerClientMock::shaper_csv_path('x'), helix::mock::scratch_dir()));
    CHECK(under(MoonrakerClientMock::belt_csv_path("axis=1.000,-1.000,0.000", "helix_belt_a"),
                helix::mock::scratch_dir()));
    CHECK(under(helix::mock::uploaded_gcode_dir(), helix::mock::scratch_dir()));
}
