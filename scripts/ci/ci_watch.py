#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Watch main's CI: rerun infrastructure failures once, track real ones in a ci-red issue.

Run by .github/workflows/ci-watch.yml. For one finished run of a watched workflow:

  failure, every failing job an infrastructure signature, attempt 1
      -> rerun the failed jobs once; no issue.
  failure otherwise (a real failure, or infrastructure again on attempt 2+)
      -> open the one open `ci-red` issue, or comment on it if one is open.
  success
      -> close the open `ci-red` issue once the latest completed main run of every
         watched workflow is green (Nightly only when the issue names it).

`--sweep` does the same for the latest completed main run of each watched workflow.
The workflow runs it on a schedule because a rerun started with GITHUB_TOKEN may
not raise its own workflow_run event, and a queued event can be dropped by the
concurrency group. Every action is idempotent: a rerun is requested only while the
run sits completed at attempt 1, and each issue comment carries a marker naming its
run and attempt, so a second pass over the same run posts nothing.

A job is infrastructure when its log or annotations match a SIGNATURE and it has no
parsed test failure. A run is infrastructure when every failing job is. Jobs that
only collect other jobs' results (AGGREGATOR_JOBS) say nothing on their own.

  scripts/ci/ci_watch.py classify LOG...          # print each log's verdict
  scripts/ci/ci_watch.py run --run-id ID [--dry-run]
  scripts/ci/ci_watch.py sweep [--dry-run]

Talks to GitHub through `gh api`, so GH_TOKEN must be set; --repo is owner/name.
Standard library only: the workflow runs it on a bare runner.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field

# The workflows ci-watch.yml listens to, by their `name:`. tests/python/test_ci_watch.py
# checks this against the workflow_run list and against each workflow file.
WATCHED = ["Build", "esp32-build", "Code Quality", "XML Lint", "Nightly Full Test Suite"]
# Runs once a day, so its latest run judges a commit hours old: it holds an issue open
# only when the issue is about it.
NIGHTLY = "Nightly Full Test Suite"
LABEL = "ci-red"

# Jobs whose only work is failing when a job they need failed. Their log names the
# other jobs and nothing else.
AGGREGATOR_JOBS = {"Build Status", "Nightly Status"}


@dataclass(frozen=True)
class Signature:
    """A known infrastructure failure. `any_of`: one pattern matching one line is
    enough. `all_of`: every pattern must match some line of the same text."""

    name: str
    description: str
    any_of: tuple[str, ...] = ()
    all_of: tuple[str, ...] = ()


# Patterns are matched per line, after the runner's timestamp and ANSI colors are
# stripped, with re.MULTILINE. apt's patterns anchor on `E:` because apt prints the
# same text as a `W:` warning when it carries on with an older index.
SIGNATURES = (
    Signature(
        "apt-mirror",
        "an Ubuntu apt mirror mid-sync or unreachable",
        any_of=(
            r"^E: Failed to fetch \S+",
            r"^E: Some index files failed to download",
            r"^E: .*File has unexpected size \(\d+ != \d+\)",
            r"^E: .*Hash Sum mismatch",
            r"^E: .*Temporary failure resolving",
        ),
    ),
    Signature(
        "ccache-missing",
        "the ccache action failed to install ccache, so the build's ccache call is not found",
        any_of=(
            r"ccache: command not found",
            r"Failed to install ccache via package manager",
        ),
    ),
    Signature(
        "retry-eperm",
        "nick-fields/retry failing to kill its child process",
        all_of=(r"code: 'EPERM'", r"syscall: 'kill'"),
    ),
    Signature(
        "runner-lost",
        "the hosted runner went away or never picked the job up",
        any_of=(
            r"The runner has received a shutdown signal",
            r"lost communication with the server",
            r"The job was not acquired by Runner",
            r"The hosted runner encountered an error",
        ),
    ),
    Signature(
        "timeout",
        "a step or job ran past its timeout",
        any_of=(
            r"The job has exceeded the maximum execution time",
            r"The action '[^']+' has timed out after \d+ minutes",
        ),
    ),
    # A job-level timeout ends the log at the bare cancel line; the reason is in
    # the job's annotation, which analyze_job reads with the log.
    Signature(
        "timeout-cancel",
        "the runner cancelled a job that hit its timeout",
        all_of=(r"The operation was canceled", r"(timed out|exceeded the maximum execution time)"),
    ),
    Signature(
        "disk-full",
        "the runner's disk filled",
        any_of=(r"No space left on device",),
    ),
    Signature(
        "artifact-service",
        "the Actions artifact service timed out",
        any_of=(r"Failed to CreateArtifact: Unable to make request",),
    ),
)

_TIMESTAMP = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z ?", re.MULTILINE)
_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# A TAP result line quotes a test's name, and a name may quote a signature.
_TAP_LINE = re.compile(r"^(?:not )?ok \d+ .*$", re.MULTILINE)
_SHARD_PREFIX = re.compile(r"^\[shard \d+\] ")
_CATCH2_FAILED = re.compile(r"^(?P<loc>\S+?\.(?:cpp|cc|h|hpp|mm|c):\d+): FAILED:")
_BATS_NOT_OK = re.compile(r"^not ok \d+ (?P<name>.+?)\s*$")
_PYTEST_FAILED = re.compile(r"^FAILED (?P<name>\S+::\S+)")
_DASHES = re.compile(r"^-{20,}$")
_SECRETS = re.compile(
    r"(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|"
    r"(?i:bearer|token|authorization)[:= ]+\S{12,})"
)

EXCERPT_LINES = 40
EXCERPT_WIDTH = 240


def clean(text: str) -> str:
    """The log as a reader sees it: no runner timestamps, no ANSI colors."""
    text = _ANSI.sub("", text)
    text = _TIMESTAMP.sub("", text)
    return "\n".join(_SHARD_PREFIX.sub("", line) for line in text.splitlines())


def match_signature(text: str) -> Signature | None:
    """The first infrastructure signature in `text` (already cleaned), or None."""
    body = _TAP_LINE.sub("", text)
    for sig in SIGNATURES:
        if sig.any_of and any(re.search(p, body, re.MULTILINE) for p in sig.any_of):
            return sig
        if sig.all_of and all(re.search(p, body, re.MULTILINE) for p in sig.all_of):
            return sig
    return None


def failing_tests(text: str) -> list[str]:
    """Test failures named in `text` (already cleaned): Catch2 as `name (file:line)`,
    bats as its `not ok` name, pytest as its node id. In log order, without repeats."""
    lines = text.splitlines()
    found: list[str] = []
    for i, line in enumerate(lines):
        m = _CATCH2_FAILED.match(line)
        if m:
            name = _catch2_case_name(lines, i)
            found.append(f"{name} ({m['loc']})" if name else m["loc"])
            continue
        m = _BATS_NOT_OK.match(line)
        if m:
            found.append(f"bats: {m['name']}")
            continue
        m = _PYTEST_FAILED.match(line)
        if m:
            found.append(f"pytest: {m['name']}")
    return list(dict.fromkeys(found))


def _catch2_case_name(lines: list[str], failed_at: int) -> str | None:
    """Catch2's console reporter heads a failure with the case name between two
    dashed rules; walk back to the nearest such block."""
    rules = []
    for j in range(failed_at - 1, max(failed_at - 60, -1), -1):
        if _DASHES.match(lines[j].strip()):
            rules.append(j)
            if len(rules) == 2:
                name = lines[rules[1] + 1].strip()
                return name or None
    return None


def excerpt(text: str, limit: int = EXCERPT_LINES) -> str:
    """Up to `limit` lines around the first failure marker in `text` (cleaned), or
    the lines before the step's final error; long lines cut, secrets-shaped strings
    redacted."""
    lines = text.splitlines()
    anchor = None
    for i, line in enumerate(lines):
        if _CATCH2_FAILED.match(line) or _BATS_NOT_OK.match(line) or _PYTEST_FAILED.match(line):
            anchor = i
            break
    if anchor is not None:
        start = max(anchor - 8, 0)
        chunk = lines[start : start + limit]
    else:
        end = len(lines)
        for i in range(len(lines) - 1, -1, -1):
            if lines[i].startswith("##[error]"):
                end = i + 1
                break
        chunk = lines[max(end - limit, 0) : end]
    chunk = [l if len(l) <= EXCERPT_WIDTH else l[:EXCERPT_WIDTH] + " ..." for l in chunk]
    return _SECRETS.sub("[redacted]", "\n".join(chunk)).strip("\n")


@dataclass
class JobReport:
    id: int
    name: str
    conclusion: str
    text: str = ""
    signature: Signature | None = None
    tests: list[str] = field(default_factory=list)

    @property
    def infra(self) -> bool:
        return self.signature is not None and not self.tests


def analyze_job(job: dict, raw_log: str, annotations: list[str]) -> JobReport:
    text = clean(raw_log + "\n" + "\n".join(annotations))
    tests = failing_tests(text)
    return JobReport(
        id=job["id"],
        name=job["name"],
        conclusion=job["conclusion"],
        text=text,
        signature=None if tests else match_signature(text),
        tests=tests,
    )


def deciding_jobs(jobs: list[JobReport]) -> list[JobReport]:
    """The jobs whose failure decides the run: failed or timed-out jobs other than
    aggregators. A run where nothing failed but something was cancelled (a job-level
    timeout reports as cancelled) is decided by its cancelled jobs."""
    real = [j for j in jobs if j.name not in AGGREGATOR_JOBS]
    failed = [j for j in real if j.conclusion in ("failure", "timed_out")]
    return failed or [j for j in real if j.conclusion == "cancelled"]


def run_is_infra(jobs: list[JobReport]) -> bool:
    deciding = deciding_jobs(jobs)
    return bool(deciding) and all(j.infra for j in deciding)


def decide(conclusion: str, attempt: int, jobs: list[JobReport]) -> str:
    """What to do with one completed run: 'rerun', 'report', 'resolve' or 'none'."""
    if conclusion == "success":
        return "resolve"
    if conclusion not in ("failure", "timed_out"):
        return "none"
    if run_is_infra(jobs) and attempt == 1:
        return "rerun"
    return "report"


def marker(run_id: int, attempt: int, workflow: str) -> str:
    return f"<!-- ci-watch run={run_id} attempt={attempt} workflow={workflow} -->"


_MARKER = re.compile(r"<!-- ci-watch run=(\d+) attempt=(\d+) workflow=(.+?) -->")


def markers_in(texts: list[str]) -> list[tuple[int, int, str]]:
    return [(int(m[1]), int(m[2]), m[3]) for t in texts for m in _MARKER.finditer(t or "")]


def issue_title(workflow: str, sha: str) -> str:
    return f"main CI red: {workflow} at {sha[:10]}"


def report_body(run: dict, jobs: list[JobReport], repo: str) -> str:
    """The issue body or comment for one red run."""
    sha = run["head_sha"]
    deciding = deciding_jobs(jobs)
    infra = run_is_infra(jobs)
    lines = [
        marker(run["id"], run["run_attempt"], run["name"]),
        f"**{run['name']}** is red on `main`.",
        "",
        f"- Commit: [`{sha[:10]}`](https://github.com/{repo}/commit/{sha})",
        f"- Run: {run['html_url']} (attempt {run['run_attempt']})",
        f"- Failing jobs: {', '.join(j.name for j in deciding) or '(none parsed)'}",
    ]
    if infra:
        sigs = sorted({j.signature.name for j in deciding if j.signature})
        lines.append(
            f"- Infrastructure ({', '.join(sigs)}) still failing on attempt "
            f"{run['run_attempt']}, so it was not rerun again."
        )
    tests = [t for j in deciding for t in j.tests]
    if tests:
        lines += ["", "Failing tests:", ""] + [f"- `{t}`" for t in tests[:30]]
        if len(tests) > 30:
            lines.append(f"- ...and {len(tests) - 30} more")
    first = next((j for j in deciding if j.tests), None) or next(
        (j for j in deciding if j.text.strip()), None
    )
    if first:
        lines += [
            "",
            f"<details><summary>Log excerpt: {first.name}</summary>",
            "",
            "```",
            excerpt(first.text).replace("```", "'''"),
            "```",
            "",
            "</details>",
        ]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# GitHub side
# ---------------------------------------------------------------------------


class GitHub:
    def __init__(self, repo: str, dry_run: bool):
        self.repo = repo
        self.dry_run = dry_run

    def _gh(self, args: list[str], data: dict | None = None) -> str:
        cmd = ["gh", "api", *args]
        inp = None
        if data is not None:
            cmd += ["--input", "-"]
            inp = json.dumps(data)
        r = subprocess.run(cmd, input=inp, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"gh api {args[0]} failed: {r.stderr.strip()}")
        return r.stdout

    def get(self, path: str):
        return json.loads(self._gh([f"repos/{self.repo}/{path}"]) or "null")

    def get_text(self, path: str) -> str:
        try:
            return self._gh([f"repos/{self.repo}/{path}"])
        except RuntimeError as e:
            log(f"{path}: {e}")
            return ""

    def write(self, method: str, path: str, data: dict | None = None):
        if self.dry_run:
            log(f"[dry run] {method} {path}")
            for key, value in (data or {}).items():
                log(f"  {key}: {value}")
            return {}
        out = self._gh(["-X", method, f"repos/{self.repo}/{path}"], data)
        return json.loads(out) if out.strip() else {}

    def open_issue(self):
        issues = self.get(f"issues?labels={LABEL}&state=open&per_page=10")
        issues = [i for i in issues if "pull_request" not in i]
        return min(issues, key=lambda i: i["number"]) if issues else None

    def recent_issues(self) -> list[dict]:
        """The ci-red issues most recently touched, open or closed."""
        issues = self.get(f"issues?labels={LABEL}&state=all&sort=updated&per_page=10")
        return [i for i in issues if "pull_request" not in i]

    def issue_texts(self, issue: dict) -> list[str]:
        comments = self.get(f"issues/{issue['number']}/comments?per_page=100")
        return [issue.get("body") or ""] + [c.get("body") or "" for c in comments]

    def latest_main_run(self, workflow: str) -> dict | None:
        """The newest main run of `workflow` from a push or the schedule that finished
        with a verdict; a cancelled run judged nothing."""
        wf = self._workflow_ids().get(workflow)
        if wf is None:
            return None
        runs = self.get(f"actions/workflows/{wf}/runs?branch=main&per_page=30")["workflow_runs"]
        for run in runs:
            if run["event"] not in ("push", "schedule"):
                continue
            if run["status"] != "completed":
                continue
            if run["conclusion"] in ("cancelled", "skipped", "neutral", "stale"):
                continue
            return run
        return None

    _wf_cache: dict | None = None

    def _workflow_ids(self) -> dict:
        if self._wf_cache is None:
            data = self.get("actions/workflows?per_page=100")
            self._wf_cache = {w["name"]: w["id"] for w in data["workflows"]}
        return self._wf_cache


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def collect_jobs(gh: GitHub, run: dict) -> list[JobReport]:
    jobs = gh.get(f"actions/runs/{run['id']}/attempts/{run['run_attempt']}/jobs?per_page=100")["jobs"]
    reports = []
    for job in jobs:
        if job["conclusion"] not in ("failure", "timed_out", "cancelled"):
            continue
        raw = gh.get_text(f"actions/jobs/{job['id']}/logs")
        anns = gh.get(f"check-runs/{job['id']}/annotations") or []
        rep = analyze_job(job, raw, [a.get("message") or "" for a in anns])
        log(
            f"job {job['name']!r} {job['conclusion']}: "
            f"{'tests ' + str(len(rep.tests)) if rep.tests else (rep.signature.name if rep.signature else 'no signature')}"
        )
        reports.append(rep)
    return reports


def handle_failure(gh: GitHub, run: dict, jobs: list[JobReport]) -> None:
    # A run reported once is never reported again, even in an issue someone has
    # since closed: the sweep revisits the latest run every hour.
    key = (run["id"], run["run_attempt"])
    for prior in gh.recent_issues():
        if key in {(r, a) for r, a, _ in markers_in(gh.issue_texts(prior))}:
            log(f"issue #{prior['number']} already reports run {key[0]} attempt {key[1]}")
            return
    body = report_body(run, jobs, gh.repo)
    issue = gh.open_issue()
    if issue is None:
        title = issue_title(run["name"], run["head_sha"])
        log(f"opening issue: {title}")
        gh.write("POST", "issues", {"title": title, "body": body, "labels": [LABEL]})
        return
    log(f"commenting on issue #{issue['number']}")
    gh.write("POST", f"issues/{issue['number']}/comments", {"body": body})


def all_green(latest: dict[str, dict | None], include_nightly: bool) -> tuple[bool, list[str]]:
    """Whether the latest run of every watched workflow is green; the workflows that
    are not. A workflow with no main run at all is not holding anything red."""
    red = []
    for wf in WATCHED:
        if wf == NIGHTLY and not include_nightly:
            continue
        run = latest.get(wf)
        if run is not None and run["conclusion"] != "success":
            red.append(wf)
    return not red, red


def handle_success(gh: GitHub, green_sha: str) -> None:
    issue = gh.open_issue()
    if issue is None:
        log("no open ci-red issue")
        return
    named = {wf for _, _, wf in markers_in(gh.issue_texts(issue))}
    latest = {wf: gh.latest_main_run(wf) for wf in WATCHED}
    ok, red = all_green(latest, include_nightly=NIGHTLY in named)
    if not ok:
        log(f"issue #{issue['number']} stays open: {', '.join(red)} still red")
        return
    shas = [r for r in latest.values() if r is not None]
    newest = max(shas, key=lambda r: r["created_at"])["head_sha"] if shas else green_sha
    msg = (
        f"Every watched workflow's latest main run is green as of "
        f"[`{newest[:10]}`](https://github.com/{gh.repo}/commit/{newest}). Closing."
    )
    log(f"closing issue #{issue['number']}")
    gh.write("POST", f"issues/{issue['number']}/comments", {"body": msg})
    gh.write("PATCH", f"issues/{issue['number']}", {"state": "closed", "state_reason": "completed"})


def process_run(gh: GitHub, run_id: int, allow_any_branch: bool = False) -> str:
    run = gh.get(f"actions/runs/{run_id}")
    log(
        f"run {run_id}: {run['name']} {run['event']} {run['head_branch']} "
        f"{run['status']}/{run['conclusion']} attempt {run['run_attempt']}"
    )
    if run["name"] not in WATCHED:
        log("not a watched workflow")
        return "none"
    if not allow_any_branch and (run["head_branch"] != "main" or run["event"] not in ("push", "schedule")):
        log("not a main push or schedule run")
        return "none"
    if run["status"] != "completed":
        log("still running")
        return "none"
    jobs = collect_jobs(gh, run) if run["conclusion"] in ("failure", "timed_out") else []
    action = decide(run["conclusion"], run["run_attempt"], jobs)
    log(f"decision: {action}")
    if action == "rerun":
        # Re-read right before acting: another pass may have started the rerun.
        fresh = gh.get(f"actions/runs/{run_id}")
        if fresh["status"] == "completed" and fresh["run_attempt"] == 1:
            gh.write("POST", f"actions/runs/{run_id}/rerun-failed-jobs")
        else:
            log("already rerun")
    elif action == "report":
        handle_failure(gh, run, jobs)
    elif action == "resolve":
        handle_success(gh, run["head_sha"])
    return action


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("classify", help="print the verdict for saved job logs")
    c.add_argument("logs", nargs="+")
    r = sub.add_parser("run", help="handle one completed run")
    r.add_argument("--run-id", type=int, required=True)
    r.add_argument("--repo", required=True)
    r.add_argument("--dry-run", action="store_true")
    r.add_argument("--any-branch", action="store_true", help="judge a run off main too (testing)")
    s = sub.add_parser("sweep", help="handle the latest main run of every watched workflow")
    s.add_argument("--repo", required=True)
    s.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    if args.cmd == "classify":
        for path in args.logs:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = clean(f.read())
            tests = failing_tests(text)
            sig = None if tests else match_signature(text)
            verdict = "real" if tests else (f"infra:{sig.name}" if sig else "real")
            print(f"{verdict}\t{path}" + (f"\t{len(tests)} tests" if tests else ""))
        return 0

    gh = GitHub(args.repo, args.dry_run)
    if args.cmd == "run":
        process_run(gh, args.run_id, args.any_branch)
        return 0
    for wf in WATCHED:
        run = gh.latest_main_run(wf)
        if run is not None:
            process_run(gh, run["id"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
