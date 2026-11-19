# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/ci/ci_watch.py: the infrastructure classifier, the run verdict, the report.

The fixtures under fixtures/ci_watch/ are cut from real failed main runs: the apt
mirror and the retry/ccache pair from run 44477048636, the Catch2 case from
35551452922, the bats case from the resource-advisor suite.
"""
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "ci"))
import ci_watch as cw  # noqa: E402

FIX = Path(__file__).resolve().parent / "fixtures" / "ci_watch"


def fixture(name):
    return (FIX / name).read_text(encoding="utf-8")


def job(name, text, conclusion="failure", annotations=(), job_id=1):
    return cw.analyze_job(
        {"id": job_id, "name": name, "conclusion": conclusion}, text, list(annotations)
    )


RUN = {
    "id": 44477048636,
    "name": "Build",
    "run_attempt": 1,
    "head_sha": "87ffd829471a37ba12cc327ea35a9afdd3d9fc91",
    "html_url": "https://github.com/o/r/actions/runs/44477048636",
}


# --- signatures ---------------------------------------------------------------


@pytest.mark.parametrize(
    "fixture_name, signature",
    [
        ("apt_mirror.txt", "apt-mirror"),
        ("retry_eperm_ccache.txt", "ccache-missing"),
    ],
)
def test_real_infrastructure_logs_match_their_signature(fixture_name, signature):
    rep = job("Compile", fixture(fixture_name))
    assert rep.signature is not None and rep.signature.name == signature
    assert rep.infra


def test_eperm_needs_both_halves_of_the_node_error():
    eperm = "\n".join(
        l for l in fixture("retry_eperm_ccache.txt").splitlines() if "ccache" not in l
    )
    assert cw.match_signature(cw.clean(eperm)).name == "retry-eperm"
    only_code = "\n".join(l for l in eperm.splitlines() if "syscall" not in l)
    assert cw.match_signature(cw.clean(only_code)) is None


@pytest.mark.parametrize(
    "annotation, signature",
    [
        ("The runner has received a shutdown signal. This can happen when the runner "
         "service is stopped, or a manually started runner is canceled.", "runner-lost"),
        ("The self-hosted runner: x lost communication with the server.", "runner-lost"),
        ("The job was not acquired by Runner of type hosted even after multiple attempts",
         "runner-lost"),
        ("The job has exceeded the maximum execution time of 45m0s", "timeout"),
        ("The action 'Build tests' has timed out after 90 minutes.", "timeout"),
        ("Failed to CreateArtifact: Unable to make request: ETIMEDOUT", "artifact-service"),
    ],
)
def test_annotations_carry_runner_and_timeout_signatures(annotation, signature):
    # A lost runner often leaves no log at all; the annotation is all there is.
    rep = job("Compile", "", annotations=[annotation])
    assert rep.signature is not None and rep.signature.name == signature


def test_apt_warning_alone_is_not_a_mirror_failure():
    # apt prints the same text as W: when it carries on with an older index, and the
    # job then fails for its own reason.
    text = (
        "W: Failed to fetch http://archive.ubuntu.com/ubuntu/dists/jammy/InRelease  "
        "Temporary failure resolving 'archive.ubuntu.com'\n"
        "W: Some index files failed to download. They have been ignored, or old ones used instead.\n"
        "src/foo.cpp:12:3: error: 'bar' was not declared in this scope\n"
        "##[error]Process completed with exit code 2.\n"
    )
    assert cw.match_signature(cw.clean(text)) is None


def test_a_test_named_after_a_signature_is_still_a_test_failure():
    text = "ok 1 something\nnot ok 2 reports ccache: command not found to the user\n"
    rep = job("Shell Tests (BATS)", text)
    assert rep.tests == ["bats: reports ccache: command not found to the user"]
    assert not rep.infra


def test_a_passing_test_named_after_a_signature_is_not_infrastructure():
    text = "ok 7 flags ccache: command not found\nmake: *** [all] Error 2\n##[error]Process completed\n"
    assert cw.match_signature(cw.clean(text)) is None


def test_test_failures_outweigh_a_signature_in_the_same_job():
    text = fixture("catch2_failure.txt") + "\n" + fixture("apt_mirror.txt")
    rep = job("Test (Ubuntu 22.04)", text)
    assert rep.tests and rep.signature is None and not rep.infra


# --- failing tests and excerpt ------------------------------------------------


def test_catch2_failure_names_the_case_and_location():
    tests = cw.failing_tests(cw.clean(fixture("catch2_failure.txt")))
    assert tests == [
        "ams context menu: subjects outlive every menu instance "
        "(tests/unit/test_ui_ams_context_menu_dispatch.cpp:139)"
    ]


def test_sharded_catch2_lines_lose_their_shard_prefix():
    text = (
        "[shard 6] -------------------------------------------------------------------------------\n"
        "[shard 6] thermistor target holds\n"
        "[shard 6] -------------------------------------------------------------------------------\n"
        "[shard 6] tests/unit/test_thermistor_target.cpp:80\n"
        "[shard 6] ...............................................................................\n"
        "[shard 6] \n"
        "[shard 6] tests/unit/test_thermistor_target.cpp:90: FAILED:\n"
    )
    assert cw.failing_tests(cw.clean(text)) == [
        "thermistor target holds (tests/unit/test_thermistor_target.cpp:90)"
    ]


def test_bats_failure_is_named_and_excerpted():
    text = cw.clean(fixture("bats_failure.txt"))
    assert cw.failing_tests(text) == ["bats: every spelling of an oversized -j is flagged"]
    ex = cw.excerpt(text)
    assert "not ok 2973" in ex and "test_resource_advisor.bats, line 353" in ex
    assert not re.search(r"^\d{4}-\d\d-\d\dT", ex, re.MULTILINE)


def test_pytest_failure_is_named():
    text = "FAILED tests/ui/test_state.py::test_hidden_widget - AssertionError\n"
    assert cw.failing_tests(text) == ["pytest: tests/ui/test_state.py::test_hidden_widget"]


def test_excerpt_starts_just_before_the_first_failure():
    lines = [f"setup {i}" for i in range(100)] + ["not ok 1 the thing"] + [f"# detail {i}" for i in range(5)]
    ex = cw.excerpt("\n".join(lines)).splitlines()
    assert ex[0] == "setup 92" and "not ok 1 the thing" in ex


def test_excerpt_is_capped_and_redacts_tokens():
    lines = [f"line {i}" for i in range(200)]
    lines[150] = "Authorization: token ghp_abcdefghijklmnopqrstuvwxyz0123"
    lines.append("##[error]Process completed with exit code 1.")
    ex = cw.excerpt("\n".join(lines))
    assert len(ex.splitlines()) == cw.EXCERPT_LINES
    assert ex.splitlines()[-1].startswith("##[error]")
    assert "ghp_" not in ex


# --- run verdict --------------------------------------------------------------


def run_44477048636_attempt(attempt):
    """The jobs that failed in run 44477048636: Android on the apt mirror, two compile
    jobs on the retry action and ccache, and the Build Status aggregator."""
    return [
        job("Build (Android)", fixture("apt_mirror.txt")),
        job("Compile (Ubuntu 22.04)", fixture("retry_eperm_ccache.txt")),
        job("Link splash + watchdog (non-LTO)", fixture("retry_eperm_ccache.txt").split("Node.js")[0]),
        job("Build Status", fixture("build_status_aggregator.txt")),
    ]


def test_infrastructure_on_attempt_one_is_rerun():
    jobs = run_44477048636_attempt(1)
    assert cw.run_is_infra(jobs)
    assert cw.decide("failure", 1, jobs) == "rerun"


def test_infrastructure_again_on_attempt_two_is_reported():
    assert cw.decide("failure", 2, run_44477048636_attempt(2)) == "report"


def test_one_real_job_makes_the_run_real():
    jobs = run_44477048636_attempt(1) + [job("Test (Ubuntu 22.04)", fixture("catch2_failure.txt"))]
    assert cw.decide("failure", 1, jobs) == "report"


def test_a_job_with_no_signature_is_real():
    jobs = [job("build", "esp32 image: 2100000 bytes / budget 2000000 (105.0%)\n##[error]x")]
    assert cw.decide("failure", 1, jobs) == "report"


def test_the_aggregator_alone_decides_nothing():
    jobs = [job("Build Status", fixture("build_status_aggregator.txt"))]
    assert not cw.run_is_infra(jobs)
    assert cw.decide("failure", 1, jobs) == "report"


def test_cancelled_jobs_decide_only_when_nothing_failed():
    timed_out = job(
        "Shell Tests (BATS)", "ok 1 x\n##[error]The operation was canceled.\n", conclusion="cancelled",
        annotations=["The job has exceeded the maximum execution time of 30m0s"],
    )
    assert cw.decide("failure", 1, [timed_out]) == "rerun"
    real = job("Compile (Ubuntu 22.04)", "error: no member named 'x'\n##[error]Process completed")
    sibling = job("Test (Ubuntu 22.04)", "##[error]The operation was canceled.", conclusion="cancelled")
    assert cw.deciding_jobs([real, sibling]) == [real]


@pytest.mark.parametrize("conclusion, expected", [("success", "resolve"), ("cancelled", "none"),
                                                  ("timed_out", "rerun")])
def test_conclusions(conclusion, expected):
    jobs = run_44477048636_attempt(1)
    assert cw.decide(conclusion, 1, jobs) == expected


# --- report and resolution ----------------------------------------------------


def test_report_body_names_commit_run_jobs_and_tests():
    jobs = [
        job("Test (Ubuntu 22.04)", fixture("catch2_failure.txt")),
        job("Build Status", fixture("build_status_aggregator.txt")),
    ]
    body = cw.report_body(RUN, jobs, "o/r")
    assert body.startswith(cw.marker(RUN["id"], 1, "Build"))
    assert "https://github.com/o/r/commit/87ffd829471a37ba12cc327ea35a9afdd3d9fc91" in body
    assert RUN["html_url"] in body
    assert "Failing jobs: Test (Ubuntu 22.04)\n" in body
    assert "test_ui_ams_context_menu_dispatch.cpp:139" in body
    assert "SIGSEGV" in body
    assert cw.issue_title("Build", RUN["head_sha"]) == "main CI red: Build at 87ffd82947"


def test_report_on_persistent_infrastructure_says_so():
    body = cw.report_body(dict(RUN, run_attempt=2), run_44477048636_attempt(2), "o/r")
    assert "Infrastructure (apt-mirror, ccache-missing, retry-eperm) still failing on attempt 2" in body


def test_markers_round_trip():
    texts = ["intro\n" + cw.marker(5, 2, "Nightly Full Test Suite"), cw.marker(7, 1, "Build")]
    assert cw.markers_in(texts) == [(5, 2, "Nightly Full Test Suite"), (7, 1, "Build")]


def green(sha="a" * 40):
    return {"conclusion": "success", "head_sha": sha}


def test_all_green_ignores_nightly_unless_the_issue_names_it():
    latest = {wf: green() for wf in cw.WATCHED}
    latest[cw.NIGHTLY] = {"conclusion": "failure", "head_sha": "b" * 40}
    assert cw.all_green(latest, include_nightly=False) == (True, [])
    assert cw.all_green(latest, include_nightly=True) == (False, [cw.NIGHTLY])


def test_all_green_holds_on_any_red_workflow():
    latest = {wf: green() for wf in cw.WATCHED}
    latest["XML Lint"] = None  # never ran on main: nothing red
    latest["Code Quality"] = {"conclusion": "failure", "head_sha": "c" * 40}
    assert cw.all_green(latest, include_nightly=False) == (False, ["Code Quality"])


# --- the workflow file --------------------------------------------------------


def test_watched_list_matches_the_workflow_and_the_workflow_names():
    wf = (ROOT / ".github" / "workflows" / "ci-watch.yml").read_text(encoding="utf-8")
    m = re.search(r"workflow_run:\n(?:\s*#.*\n)*\s*workflows:\n((?:\s*- .+\n)+)", wf)
    assert m, "ci-watch.yml has no workflow_run.workflows list"
    listed = [re.sub(r"^\s*- ", "", l).strip().strip('"') for l in m[1].splitlines() if l.strip()]
    assert listed == cw.WATCHED
    names = set()
    for path in (ROOT / ".github" / "workflows").glob("*.yml"):
        first = re.search(r"^name:\s*(.+)$", path.read_text(encoding="utf-8"), re.MULTILINE)
        if first:
            names.add(first[1].strip().strip('"'))
    assert set(cw.WATCHED) <= names
