#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when a workflow job runs a submodule-dependent target without checking out submodules.

The shell suite reads files out of the submodules. Most of those tests skip
themselves when a submodule is absent, but `tests/shell/test_lvgl_event_code_gate.bats`
copies `lib/lvgl/src/misc/lv_event.h` in `setup()`, so a bare checkout kills the
whole file instead of skipping it.

That is not hypothetical: `build.yml` was fixed for it in 83b0b9d51 and
`release.yml` was left behind, so the v0.99.117 tag failed `validate-shell` and
skipped every build, publish and deploy job behind it. Nothing was released and
the tag had to be moved.

The enum has to be PATCHED as well, not merely present: `lvgl_display_sync_cb.patch`
inserts four values and renumbers everything after them, so the committed worker
table only matches a patched checkout. Both steps are therefore required.

Deliberately not solved by making the bats file skip on a missing header: that
would turn "the committed table is up to date" green-by-skip, which is the
failure this gate exists to prevent.
"""

import pathlib
import re
import sys

import yaml

WORKFLOW_DIR = pathlib.Path(".github/workflows")
INIT_ACTION = "./.github/actions/init-submodules"

# Ways a job can invoke the bats suite. release.yml goes through the make
# target; build.yml and nightly.yml call bats on the directory directly. Both
# read the submodules, so both have to be recognised - keying on only one form
# is how this gate would quietly cover a third of the jobs it is meant to.
INVOCATIONS = (
    (re.compile(r"\bmake\b[^\n;|&]*\btest-shell\b"), "make test-shell"),
    (re.compile(r"\bbats\b[^\n;|&]*\btests/shell\b"), "bats tests/shell/"),
)

APPLY_PATCHES_RE = re.compile(r"\bmake\b[^\n;|&]*\bapply-patches\b")


def steps_of(job):
    steps = job.get("steps")
    return steps if isinstance(steps, list) else []


def job_runs_shell_suite(steps):
    """Return how this job invokes the bats suite, or None if it does not."""
    for step in steps:
        run = step.get("run") or ""
        for pattern, label in INVOCATIONS:
            if pattern.search(run):
                return label
    return None


def job_has_init(steps):
    return any((step.get("uses") or "").strip() == INIT_ACTION for step in steps)


def job_has_apply_patches(steps):
    return any(APPLY_PATCHES_RE.search(step.get("run") or "") for step in steps)


def main():
    if not WORKFLOW_DIR.is_dir():
        print(f"❌ {WORKFLOW_DIR} not found", file=sys.stderr)
        return 1

    # removeprefix, not lstrip: lstrip("./") strips a *character set* and would
    # eat the dot in ".github" as well.
    if not (pathlib.Path(INIT_ACTION.removeprefix("./")) / "action.yml").is_file():
        print(f"❌ the composite action {INIT_ACTION} is missing", file=sys.stderr)
        return 1

    errors = []
    checked = 0

    for wf in sorted(WORKFLOW_DIR.glob("*.yml")):
        try:
            doc = yaml.safe_load(wf.read_text())
        except yaml.YAMLError as exc:
            errors.append(f"{wf}: cannot parse: {exc}")
            continue
        if not isinstance(doc, dict):
            continue

        for job_id, job in (doc.get("jobs") or {}).items():
            if not isinstance(job, dict):
                continue
            steps = steps_of(job)
            invocation = job_runs_shell_suite(steps)
            if not invocation:
                continue

            checked += 1
            missing = []
            if not job_has_init(steps):
                missing.append(f"`uses: {INIT_ACTION}`")
            if not job_has_apply_patches(steps):
                missing.append("`run: make apply-patches`")
            if missing:
                errors.append(
                    f"{wf.name}: job '{job_id}' runs `{invocation}` but is missing "
                    + " and ".join(missing)
                    + "\n    A bare checkout has no lib/lvgl, so test_lvgl_event_code_gate.bats"
                    "\n    dies in setup() and takes every job behind it with it."
                )

    if errors:
        print("❌ workflow submodule gate:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1

    print(
        f"✅ workflow submodule gate: {checked} job(s) running the shell suite "
        "init and patch their submodules"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
