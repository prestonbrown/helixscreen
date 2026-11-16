#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Syntax-check the firmware sources a change touches, with the ESP32 compiler.

Code that compiles on the host can fail on the ESP32 image: int32_t is `long`
on Xtensa, so std::max(int, int32_t) deduces on the host and not there, and the
image builds -fno-exceptions with its own HELIX_HAS_* set. Local gates never
build the firmware, so those breaks surface 15 minutes into esp32-build CI.

This compiles, -fsyntax-only, every firmware translation unit whose source
changed between BASE and HEAD, plus every firmware unit that reaches a changed
header through any chain of includes (a firmware header holding a repo type by
value is such a chain), inside the espressif/idf image. The include graph is
parsed once per file, so a wide header costs the compiles it selects, not the
walk. Flags come from a
configure-only firmware build cached under ~/.cache, keyed on the firmware's
CMake inputs, so a push that does not touch them pays no configure. A source
that two components compile (helixnet and helixapp share a few) is checked
once per component, with that component's include path: helixnet has no LVGL.

Advisory, never blocking, when it cannot answer: no docker, no IDF image (it is
not pulled, ~9GB), or the configure fails. esp32-build CI stays the backstop.
--require turns every such answer into exit 3, for a caller whose verdict must
mean the firmware compiled (make land-gate).

usage: esp32_syntax_check.py --base SHA [--head SHA] [--tree DIR] [--jobs N] [--require]
       esp32_syntax_check.py --staged
       HELIX_ESP32_SYNTAX=0 skips it.

--staged is the commit hook's form: the staged sources the firmware compiles,
and each staged header through ONE unit that includes it (a staged source that
does, when there is one), so a commit touching a widely included header pays
for one compile, not hundreds; pre-push still checks every includer. It never
configures: with no cached flags it says so and leaves that to pre-push.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

IDF_IMAGE = "espressif/idf:v5.5.5"
FW_DIR = "firmware/helixscreen-esp32"
SOURCE_EXTS = (".c", ".cpp")
HEADER_EXTS = (".h", ".hpp")
# Force-included on every firmware C++ command line (helixapp's CMakeLists), so
# every unit depends on it and no #include names it.
FORCED_INCLUDES = frozenset({"include/lvgl_pch.h"})
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)
CONFIG_INPUT_RE = re.compile(r"(CMakeLists\.txt|\.txt|\.yml|\.csv|sdkconfig[^/]*|Kconfig[^/]*)$")


def git(tree, *args):
    return subprocess.run(["git", "-C", str(tree), *args], check=True, capture_output=True,
                          text=True).stdout


def config_key(tree, head):
    """Hash of the firmware's CMake inputs at HEAD: blob ids, so no file reads."""
    listing = git(tree, "ls-tree", "-r", head, "--", FW_DIR)
    inputs = [line for line in listing.splitlines() if CONFIG_INPUT_RE.search(line)]
    return hashlib.sha1("\n".join(inputs).encode()).hexdigest()[:16]


def compile_args(entry):
    """The entry's compiler command as -fsyntax-only: no object, no depfile."""
    out, skip = [], False
    for tok in shlex.split(entry["command"]):
        if skip:
            skip = False
        elif tok in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif tok.startswith("-fdiagnostics-color"):
            out.append("-fdiagnostics-color=never")
        elif tok not in ("-MD", "-MMD"):
            out.append(tok)
    return out + ["-fsyntax-only"]


def firmware_units(entries, src_root):
    """{relpath: [entry, ...]} for the sources under src_root, one entry per component."""
    units = {}
    for entry in entries:
        f = entry["file"]
        if f.startswith(src_root + "/") and f.endswith(SOURCE_EXTS):
            units.setdefault(f[len(src_root) + 1:], []).append(entry)
    return units


COMPONENT_RE = re.compile(r"(?:^|[/\s])esp-idf/([^/]+)/")


def component_of(entry):
    """The ESP-IDF component an entry compiles for, from its object path."""
    m = COMPONENT_RE.search(entry.get("output") or entry["command"])
    return m.group(1) if m else "?"


def rebase_path(tok, src_root, tree, *fallback_roots):
    """Point every path under the configured source root at the tree being checked.

    Rewrites the root wherever it sits in the token (-I, -isystem, -include,
    -D values such as LV_CONF_PATH="...", -fmacro-prefix-map=). A path that
    does not exist there (build/generated in a never-built checkout, lib/ in an
    index overlay) resolves through the fallback roots in order: the checkout,
    then the primary checkout's copy.
    """
    def sub(m):
        rel = m.group(1)
        if not rel:
            return tree
        for root in (tree, *fallback_roots):
            cand = os.path.join(root, rel)
            if os.path.exists(cand):
                return cand
        return os.path.join(tree, rel)

    # The root alone (-I<root>, a prefix map's <root>=) is a path too; a longer
    # sibling name (<root>x/) is not.
    return re.sub(re.escape(src_root) + r'(?:/+([^"\s\\=]*))?(?![^/"\s\\=])', sub, tok)


# Where a quoted or angled include resolves when it is not relative to the
# includer: the roots the firmware's compile commands pass as -I under the repo.
# check_units reads the real set from those commands; this is the shape.
DEFAULT_ROOTS = (".", "include", "src", "src/generated", f"{FW_DIR}/main",
                 f"{FW_DIR}/components/helixapp", f"{FW_DIR}/components/helixcore",
                 f"{FW_DIR}/components/helixnet")


def include_roots(entries, src_root):
    """The -I and -isystem directories under src_root, relative, from the firmware's
    compile commands; lib/ and build/ are left out, since a header there is a leaf:
    nothing the repo changes reaches it, and parsing LVGL's tree would cost more
    than every compile it could select."""
    roots = set()
    for entry in entries:
        toks = shlex.split(entry["command"])
        for i, tok in enumerate(toks):
            path = None
            if tok in ("-I", "-isystem") and i + 1 < len(toks):
                path = toks[i + 1]
            elif tok.startswith("-I"):
                path = tok[2:]
            elif tok.startswith("-isystem"):
                path = tok[8:]
            if path and path.rstrip("/") == src_root:
                # helixapp passes -I${REPO_ROOT}: `#include "src/ui/x.h"` resolves here.
                roots.add(".")
            elif path and path.startswith(src_root + "/"):
                rel = path[len(src_root) + 1:].rstrip("/")
                if rel and not rel.startswith(("lib/", "build/")):
                    roots.add(rel)
    return tuple(sorted(roots)) or DEFAULT_ROOTS


class IncludeGraph:
    """Which repo files a unit reaches through #include, parsed once per file.

    An include resolves relative to its includer first, then against `roots`,
    and only to a file `read_source` can read: a system or lib/ header resolves
    to nothing and is a leaf. An include that resolves to nothing but names one
    of `watched` by path suffix (`<stb_image.h>` for lib/stb/stb_image.h, whose
    -isystem root is not walked) is an edge to that file. The closure walks
    cycles once.
    """

    def __init__(self, read_source, roots, watched=()):
        self.read_source = read_source
        self.roots = roots
        self.watched = tuple(watched)
        self.text = {}
        self.edges = {}

    def exists(self, rel):
        if rel not in self.text:
            self.text[rel] = self.read_source(rel)
        return self.text[rel] is not None

    def includes(self, rel):
        if rel in self.edges:
            return self.edges[rel]
        out = []
        text = self.text.get(rel) if rel in self.text else self.read_source(rel)
        self.text[rel] = text
        for inc in INCLUDE_RE.findall(text or ""):
            here = os.path.normpath(os.path.join(os.path.dirname(rel), inc))
            for cand in (here, *(os.path.normpath(os.path.join(r, inc)) for r in self.roots)):
                if cand != rel and self.exists(cand):
                    out.append(cand)
                    break
            else:
                out.extend(w for w in self.watched
                           if w != rel and (w == inc or w.endswith("/" + inc)))
        self.edges[rel] = out
        return out

    def closure(self, rel):
        seen, stack = set(), [rel]
        while stack:
            for nxt in self.includes(stack.pop()):
                if nxt not in seen:
                    seen.add(nxt)
                    stack.append(nxt)
        return seen


def header_includers(units, headers, read_source, roots=DEFAULT_ROOTS):
    """{header: [unit, ...]}: the units reaching each changed header, directly or
    through any chain of headers (a firmware header holding a repo type by
    value is such a chain). A header no unit reaches maps to [].

    A forced include is on every command line and in no #include, so it and
    everything it includes (helix_features.h, where every HELIX_HAS_* default
    lives) are reached by every unit.
    """
    includers = {h: [] for h in headers}
    if not headers:
        return includers
    graph = IncludeGraph(read_source, roots, watched=headers)
    forced = set()
    for f in FORCED_INCLUDES:
        forced |= {f} | graph.closure(f)
    for rel in sorted(units):
        reached = graph.closure(rel) | forced
        for h in headers:
            if h in reached:
                includers[h].append(rel)
    return includers


def select_units(units, changed, read_source, one_per_header=False, roots=DEFAULT_ROOTS):
    """Relative paths of the units to check.

    units: {relpath: [entry, ...]}; changed: relpaths; read_source(rel) -> text.
    A changed source is checked if the firmware compiles it. A changed header
    is checked through every unit that reaches it (header_includers), or with
    one_per_header through a single one of them, preferring a unit already
    picked.
    """
    headers = [rel for rel in changed if rel.endswith(HEADER_EXTS)]
    return pick(units, changed, header_includers(units, headers, read_source, roots),
                one_per_header)


def pick(units, changed, includers, one_per_header=False):
    picked = {rel for rel in changed if rel in units}
    for units_of_h in includers.values():
        if not one_per_header:
            picked.update(units_of_h)
        elif units_of_h and not picked.intersection(units_of_h):
            picked.add(units_of_h[0])
    return sorted(picked)


def unreached_note(includers):
    """One line naming the changed headers no firmware unit reaches, or None.

    A desktop-only header is a normal case, so this is a note, not a failure:
    under --require the run still answers for every unit it did compile. What it
    rules out is the silent form, a header edit that selects nothing and prints
    nothing, which reads like a pass.
    """
    missed = sorted(h for h, us in includers.items() if not us)
    if not missed:
        return None
    return (f"ℹ️  esp32 syntax check: {len(missed)} changed header(s) reach no firmware "
            f"unit, so nothing checks them: {', '.join(missed)}")


INDEX_DIRS = ("src", "include", FW_DIR)


def index_overlay(tree, dest):
    """Write the staged copy of every tracked file under INDEX_DIRS into dest.

    The commit hook compiles what the commit will contain. The working tree can
    hold a fix the index lacks, or a peer's unstaged header, and either would
    decide the verdict instead. ~0.25s for the ~2200 files.
    """
    dest.mkdir(parents=True, exist_ok=True)
    listing = subprocess.run(["git", "-C", str(tree), "ls-files", "-z", "--", *INDEX_DIRS],
                             check=True, capture_output=True).stdout
    subprocess.run(["git", "-C", str(tree), "checkout-index", "-z", "--stdin", "-f",
                    f"--prefix={dest}/"], input=listing, check=True, capture_output=True)
    return dest


# The IDF container's memory limit, with no swap. Its compiles are syntax-only
# and light, but they run --jobs wide on whatever host runs this, often beside
# a build: a runaway is killed inside the container, not on the host.
DOCKER_MEMORY = "16g"


def docker_base(mounts):
    cmd = ["docker", "run", "--rm", "-u", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp",
           "--memory", DOCKER_MEMORY, "--memory-swap", DOCKER_MEMORY]
    for m in sorted(set(mounts)):
        cmd += ["-v", f"{m}:{m}"]
    return cmd + [IDF_IMAGE]


def configure(tree, cache, mounts):
    """Configure-only firmware build into `cache`; True when it produced flags."""
    cache.mkdir(parents=True, exist_ok=True)
    # main/CMakeLists.txt refuses to configure without the packed asset image,
    # which only the link consumes.
    (cache / "storage_frogfs.bin").touch()
    project = os.path.join(tree, FW_DIR)
    script = (f". /opt/esp/idf/export.sh >/dev/null 2>&1; "
              f"idf.py -C {shlex.quote(project)} -B {shlex.quote(str(cache))} reconfigure")
    res = subprocess.run(docker_base(mounts + [str(cache)]) + ["bash", "-c", script],
                         capture_output=True, text=True)
    if res.returncode != 0 or not (cache / "compile_commands.json").exists():
        tail = "\n".join((res.stdout + res.stderr).splitlines()[-15:])
        print(f"⚠️  esp32 syntax check: firmware configure failed, skipping\n{tail}")
        shutil.rmtree(cache, ignore_errors=True)
        return False
    (cache / "source_root.txt").write_text(str(tree))
    return True


def run_jobs(jobs_file, jobs):
    """Container side: run each job, report failures as JSON on stdout."""
    specs = json.loads(Path(jobs_file).read_text())

    def one(spec):
        res = subprocess.run(spec["args"], cwd=spec["cwd"], capture_output=True, text=True)
        return spec["rel"], res.returncode, res.stdout + res.stderr

    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for rel, rc, out in pool.map(one, specs):
            if rc != 0:
                failed.append({"rel": rel, "out": out})
    print(json.dumps(failed))
    return 0


# Exit status under --require when nothing was compiled because nothing could be.
kCannotAnswer = 3


def cannot_answer(args, message):
    """Report a check that could not run: advisory, or exit 3 under --require."""
    print(message)
    return kCannotAnswer if args.require else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--tree")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--staged", action="store_true")
    ap.add_argument("--require", action="store_true")
    ap.add_argument("--run-jobs", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.run_jobs:
        return run_jobs(args.run_jobs, args.jobs or 4)
    if os.environ.get("HELIX_ESP32_SYNTAX") == "0":
        # An explicit opt-out is silent; only a check that must answer says it did not.
        if not args.require:
            return 0
        return cannot_answer(args, "⚠️  esp32 syntax check skipped: HELIX_ESP32_SYNTAX=0")

    tree = os.path.realpath(args.tree or git(".", "rev-parse", "--show-toplevel").strip())
    primary = os.path.dirname(
        git(tree, "rev-parse", "--path-format=absolute", "--git-common-dir").strip())
    if args.staged:
        diff = git(tree, "diff", "--cached", "--name-only", "--diff-filter=d")
    else:
        diff = git(tree, "diff", "--name-only", "--diff-filter=d", args.base, args.head)
    changed = [p for p in diff.splitlines() if p.endswith(SOURCE_EXTS + HEADER_EXTS)]
    if not changed:
        return 0
    if not shutil.which("docker") or subprocess.run(
            ["docker", "image", "inspect", IDF_IMAGE], capture_output=True).returncode != 0:
        return cannot_answer(args, f"⚠️  esp32 syntax check skipped: needs docker and {IDF_IMAGE}")

    mounts = [tree, primary]
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / \
        "helixscreen" / "esp32-syntax" / config_key(tree, args.head)
    if not (cache / "compile_commands.json").exists():
        if args.staged:
            return cannot_answer(args, "ℹ️  esp32 syntax check: no cached firmware flags for "
                                 "this CMake state; pre-push configures them (minutes, once)")
        # Each configure is ~33MB; keep the newest few.
        for old in sorted(cache.parent.glob("*"), key=lambda p: p.stat().st_mtime)[:-3]:
            shutil.rmtree(old, ignore_errors=True)
        print("🔧 esp32 syntax check: configuring firmware flags (once per CMake change)...")
        if not configure(tree, cache, mounts):
            return cannot_answer(args, "⚠️  esp32 syntax check: no firmware flags, nothing compiled")
    src_root = (cache / "source_root.txt").read_text().strip()
    units = firmware_units(json.loads((cache / "compile_commands.json").read_text()), src_root)

    overlay = None
    source_tree, fallbacks = tree, (primary,)
    if args.staged:
        overlay = index_overlay(tree, cache / f"index-{os.getpid()}")
        source_tree, fallbacks = str(overlay), (tree, primary)
    try:
        return check_units(args, units, changed, src_root, source_tree, fallbacks, cache, mounts)
    finally:
        if overlay is not None:
            shutil.rmtree(overlay, ignore_errors=True)


def check_units(args, units, changed, src_root, source_tree, fallbacks, cache, mounts):
    def read_source(rel):
        try:
            return Path(source_tree, rel).read_text(errors="replace")
        except OSError:
            return None

    roots = include_roots([e for es in units.values() for e in es], src_root)
    headers = [rel for rel in changed if rel.endswith(HEADER_EXTS)]
    includers = header_includers(units, headers, read_source, roots)
    note = unreached_note(includers)
    if note:
        print(note)
    picked = pick(units, changed, includers, one_per_header=args.staged)
    if not picked:
        return 0
    specs = []
    for rel in picked:
        entries = units[rel]
        for entry in entries:
            label = rel if len(entries) == 1 else f"{rel} [{component_of(entry)}]"
            specs.append({"rel": label, "cwd": entry["directory"],
                          "args": [rebase_path(t, src_root, source_tree, *fallbacks)
                                   for t in compile_args(entry)]})
    jobs_file = cache / f"jobs-{os.getpid()}.json"
    jobs_file.write_text(json.dumps(specs))
    jobs = args.jobs or max(2, (os.cpu_count() or 4) // 4)
    print(f"🔧 esp32 syntax check: {len(specs)} firmware compile(s) of {len(picked)} "
          f"unit(s), -j{jobs}...")
    try:
        res = subprocess.run(docker_base(mounts + [str(cache)]) + [
            "bash", "-c", ". /opt/esp/idf/export.sh >/dev/null 2>&1; python3 " +
            shlex.quote(os.path.abspath(__file__)) + f" --run-jobs {shlex.quote(str(jobs_file))}"
            f" --jobs {jobs}"], capture_output=True, text=True)
    finally:
        jobs_file.unlink(missing_ok=True)
    try:
        failed = json.loads(res.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError):
        return cannot_answer(
            args, f"⚠️  esp32 syntax check could not run, skipping\n{res.stderr[-800:]}")
    if not failed:
        print(f"✅ esp32 syntax check: {len(picked)} firmware unit(s) compile")
        return 0
    for f in failed:
        errors = [l for l in f["out"].splitlines() if "error" in l][:8]
        print(f"❌ {f['rel']} does not compile for the ESP32:")
        print("\n".join("   " + l for l in errors))
    form = "--staged" if args.staged else "--base <sha>"
    print(f"   Reproduce: python3 scripts/esp32_syntax_check.py {form} (HELIX_ESP32_SYNTAX=0 skips)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
