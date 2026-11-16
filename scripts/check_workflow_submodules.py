#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when a workflow job runs a submodule-dependent target without checking out submodules.

The shell suite reads files out of the submodules. Most of those tests skip
themselves when a submodule is absent, but `tests/shell/test_lvgl_event_code_gate.bats`
copies `lib/lvgl/include/lvgl/core/lv_event.h` in `setup()`, so a bare checkout kills the
whole file instead of skipping it.

That is not hypothetical: `build.yml` was fixed for it in 83b0b9d51 and
`release.yml` was left behind, so the v0.99.117 tag failed `validate-shell` and
skipped every build, publish and deploy job behind it. Nothing was released and
the tag had to be moved.

The enum has to be the one the app builds from, patches applied: the committed
worker table mirrors that checkout, so a patch that adds an event code would leave
an unpatched one renumbered. Both steps are therefore required.

Every `make apply-patches` command, in any job, must run with
HELIX_PATCHES_FROM_CLEAN=1. A CI runner is always a fresh clone: nothing is
applied when the recipe starts, so every patch must take its apply branch, and a
patch that can neither apply nor reverse is dead, not shadowed. Without the flag
that case only warns and the job builds on with the patch silently missing.

The flag counts in either of two forms: on the make invocation itself, or as
`env: HELIX_PATCHES_FROM_CLEAN: "1"` at the workflow or job level. The env form
is the one a workflow should use, because the apply that matters usually runs
inside a plain `make -j` build step rather than a dedicated apply step - and a
flag pinned to one step never reaches that one.

A `docker run ... make ...` step is covered by neither form: workflow env stops
at the container boundary, so the docker command itself has to forward the flag
with `-e HELIX_PATCHES_FROM_CLEAN`. Forwarded without a value, `-e` takes it
from the runner's environment, so the workflow, job or step env must still set
it - otherwise the container receives an unset variable and the apply inside
runs in its warn mode.

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
FLAG_ENV = "HELIX_PATCHES_FROM_CLEAN"

# Ways a job can invoke the bats suite. release.yml goes through the make
# target; build.yml and nightly.yml call bats on the directory directly. Both
# read the submodules, so both have to be recognised - keying on only one form
# is how this gate would quietly cover a third of the jobs it is meant to.
INVOCATIONS = (
    (re.compile(r"\bmake\b[^\n;|&]*\btest-shell\b"), "make test-shell"),
    (re.compile(r"\bbats\b[^\n;|&]*\btests/shell\b"), "bats tests/shell/"),
)

# A run string is a list of commands, not one command: `make apply-patches; make
# apply-patches HELIX_PATCHES_FROM_CLEAN=1` flags only the second apply, and a
# flag anywhere in the string must not bless the rest of it. Split on every
# separator shell recognizes, then judge each command on its own.
# Backslash-newline is a continuation, not a separator, so it is joined before
# the split - otherwise a docker command written across lines has its `docker
# run` and its `make` judged as different fragments.
COMMAND_SPLIT_RE = re.compile(r"[\n;|&]+")
CONTINUATION_RE = re.compile(r"\\\n")
MAKE_RE = re.compile(r"\bmake\b")
# `reapply-patches` is a different target that must not be excused by this rule;
# `force-apply-patches` is the apply target and must be. A plain \b cannot tell
# them apart, so anchor on the character before the target name: 'y' precedes
# the one ('reapply'), '-' the other ('force-').
APPLY_TARGET_RE = re.compile(r"(?<![a-zA-Z0-9])apply-patches\b")
FLAG_RE = re.compile(r"HELIX_PATCHES_FROM_CLEAN=1")
# A `docker run` make invocation: workflow env does not cross the container
# boundary, so the flag must be forwarded on the docker command itself. The
# value group distinguishes `-e HELIX_PATCHES_FROM_CLEAN` (value comes from the
# runner's environment) from `-e HELIX_PATCHES_FROM_CLEAN=1` (self-contained).
DOCKER_RUN_RE = re.compile(r"\bdocker\b.*\brun\b")
DOCKER_FLAG_RE = re.compile(
    r"(?:-e|--env)[ \t=]*HELIX_PATCHES_FROM_CLEAN(?P<value>=\S+)?\b"
)


def commands_of(run):
    """Return `run` as a list of single commands, continuations joined."""
    return COMMAND_SPLIT_RE.split(CONTINUATION_RE.sub(" ", run))


def steps_of(job):
    steps = job.get("steps")
    return steps if isinstance(steps, list) else []


def apply_commands(run):
    """Return the commands in `run` that invoke `make apply-patches`."""
    return [
        seg.strip()
        for seg in commands_of(run)
        if MAKE_RE.search(seg) and APPLY_TARGET_RE.search(seg)
    ]


def flag_covered_by_env(doc, job):
    """Does a workflow- or job-level `env:` set the from-clean flag?"""
    merged = {}
    for env in (doc.get("env"), job.get("env")):
        if isinstance(env, dict):
            merged.update(env)
    return str(merged.get(FLAG_ENV)) == "1"


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
    return any(apply_commands(step.get("run") or "") for step in steps)


def bare_apply_patches(steps, env_covered):
    """Return unflagged `make apply-patches` commands as (step name, command)."""
    bare = []
    for step in steps:
        name = step.get("name") or "unnamed step"
        for command in apply_commands(step.get("run") or ""):
            if not env_covered and not FLAG_RE.search(command):
                bare.append((name, command))
    return bare


def step_env_sets_flag(step):
    env = step.get("env")
    return isinstance(env, dict) and str(env.get(FLAG_ENV)) == "1"


def docker_make_without_flag(steps, env_covered):
    """Return `docker run` make commands the flag cannot reach.

    Tuples of (step name, command, reason): "missing" when no `-e`/`--env`
    names the flag on the docker command, "no-value" when it is forwarded
    without a value - that form reads the runner's environment, so a workflow,
    job or step env must set it or the container receives an unset variable.
    """
    findings = []
    for step in steps:
        name = step.get("name") or "unnamed step"
        for seg in commands_of(step.get("run") or ""):
            if not (DOCKER_RUN_RE.search(seg) and MAKE_RE.search(seg)):
                continue
            command = " ".join(seg.split())
            m = DOCKER_FLAG_RE.search(command)
            if m is None:
                findings.append((name, command, "missing"))
            elif m.group("value") is None and not (
                env_covered or step_env_sets_flag(step)
            ):
                findings.append((name, command, "no-value"))
    return findings


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
            env_covered = flag_covered_by_env(doc, job)

            for step_name, command in bare_apply_patches(steps, env_covered):
                errors.append(
                    f"{wf.name}: job '{job_id}' step '{step_name}' runs `{command}` "
                    "without HELIX_PATCHES_FROM_CLEAN=1"
                    "\n    A CI runner is a fresh clone, so every patch must take its"
                    " apply branch; without the flag a drifted patch only warns and"
                    " the job builds on with the patch silently missing."
                    "\n    Pass HELIX_PATCHES_FROM_CLEAN=1 on the make invocation, or"
                    f" set `env: {FLAG_ENV}: \"1\"` at the workflow or job level - the"
                    " env form also covers applies that run inside a plain `make -j`."
                )

            for step_name, command, reason in docker_make_without_flag(
                steps, env_covered
            ):
                if reason == "missing":
                    detail = (
                        "without forwarding HELIX_PATCHES_FROM_CLEAN into the"
                        " container"
                        "\n    Workflow env stops at the container boundary: the make"
                        " inside runs the patch recipe, and without the flag a drifted"
                        " patch only warns and the build proceeds with the patch"
                        " silently missing. Add `-e HELIX_PATCHES_FROM_CLEAN`."
                    )
                else:
                    detail = (
                        "forwards HELIX_PATCHES_FROM_CLEAN without a value"
                        "\n    The value comes from the runner's environment, so the"
                        " workflow, job or step env must set it - otherwise the"
                        " container receives an unset variable and the apply inside"
                        " runs in its warn mode. Set"
                        f" `env: {FLAG_ENV}: \"1\"` or pass `-e {FLAG_ENV}=1`."
                    )
                errors.append(
                    f"{wf.name}: job '{job_id}' step '{step_name}' runs `{command}`"
                    f" - a make inside `docker run` {detail}"
                )

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
