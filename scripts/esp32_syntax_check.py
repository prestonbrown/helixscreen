#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Syntax-check the firmware sources a change touches, with the ESP32 compiler.

Code that compiles on the host can fail on the ESP32 image: int32_t is `long`
on Xtensa, so std::max(int, int32_t) deduces on the host and not there, and the
image builds -fno-exceptions with its own HELIX_HAS_* set. Local gates never
build the firmware, so those breaks surface 15 minutes into esp32-build CI.

This compiles, -fsyntax-only, every firmware translation unit whose source
changed between BASE and HEAD, plus every firmware unit that directly includes
a changed header, inside the espressif/idf image. Flags come from a
configure-only firmware build cached under ~/.cache, keyed on the firmware's
CMake inputs, so a push that does not touch them pays no configure.

Advisory, never blocking, when it cannot answer: no docker, no IDF image (it is
not pulled, ~9GB), or the configure fails. esp32-build CI stays the backstop.

usage: esp32_syntax_check.py --base SHA [--head SHA] [--tree DIR] [--jobs N]
       HELIX_ESP32_SYNTAX=0 skips it.
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


def rebase_path(tok, src_root, tree, fallback_root):
    """Point every path under the configured source root at the tree being checked.

    Rewrites the root wherever it sits in the token (-I, -isystem, -include,
    -D values such as LV_CONF_PATH="...", -fmacro-prefix-map=). A path that
    does not exist there (build/generated in a never-built checkout) falls
    back to the primary checkout's copy.
    """
    def sub(m):
        rel = m.group(1)
        if not rel:
            return tree
        cand = os.path.join(tree, rel)
        if not os.path.exists(cand) and os.path.exists(os.path.join(fallback_root, rel)):
            cand = os.path.join(fallback_root, rel)
        return cand

    # The root alone (-I<root>, a prefix map's <root>=) is a path too; a longer
    # sibling name (<root>x/) is not.
    return re.sub(re.escape(src_root) + r'(?:/+([^"\s\\=]*))?(?![^/"\s\\=])', sub, tok)


def select_units(units, changed, read_source):
    """Relative paths of the units to check.

    units: {relpath: entry}; changed: relpaths; read_source(rel) -> text.
    A changed source is checked if the firmware compiles it. A changed header
    is checked through the units that include it directly.
    Direct includers only; a transitive include graph when a break
    slips past through a header-of-a-header.
    """
    picked = {rel for rel in changed if rel in units}
    headers = [rel for rel in changed if rel.endswith(HEADER_EXTS)]
    if headers:
        for rel in units:
            text = read_source(rel)
            if text is None:
                continue
            for inc in INCLUDE_RE.findall(text):
                if any(h == inc or h.endswith("/" + inc) for h in headers):
                    picked.add(rel)
                    break
    return sorted(picked)


def docker_base(mounts):
    cmd = ["docker", "run", "--rm", "-u", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp"]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--tree")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--run-jobs", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.run_jobs:
        return run_jobs(args.run_jobs, args.jobs or 4)
    if os.environ.get("HELIX_ESP32_SYNTAX") == "0":
        return 0

    tree = os.path.realpath(args.tree or git(".", "rev-parse", "--show-toplevel").strip())
    primary = os.path.dirname(
        git(tree, "rev-parse", "--path-format=absolute", "--git-common-dir").strip())
    changed = [p for p in git(tree, "diff", "--name-only", "--diff-filter=d", args.base,
                              args.head).splitlines()
               if p.endswith(SOURCE_EXTS + HEADER_EXTS)]
    if not changed:
        return 0
    if not shutil.which("docker") or subprocess.run(
            ["docker", "image", "inspect", IDF_IMAGE], capture_output=True).returncode != 0:
        print(f"⚠️  esp32 syntax check skipped: needs docker and {IDF_IMAGE}")
        return 0

    mounts = [tree, primary]
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / \
        "helixscreen" / "esp32-syntax" / config_key(tree, args.head)
    if not (cache / "compile_commands.json").exists():
        # Each configure is ~33MB; keep the newest few.
        for old in sorted(cache.parent.glob("*"), key=lambda p: p.stat().st_mtime)[:-3]:
            shutil.rmtree(old, ignore_errors=True)
        print("🔧 esp32 syntax check: configuring firmware flags (once per CMake change)...")
        if not configure(tree, cache, mounts):
            return 0
    src_root = (cache / "source_root.txt").read_text().strip()
    units = {}
    for entry in json.loads((cache / "compile_commands.json").read_text()):
        f = entry["file"]
        if f.startswith(src_root + "/") and f.endswith(SOURCE_EXTS):
            units[f[len(src_root) + 1:]] = entry

    def read_source(rel):
        try:
            return Path(tree, rel).read_text(errors="replace")
        except OSError:
            return None

    picked = select_units(units, changed, read_source)
    if not picked:
        return 0
    specs = []
    for rel in picked:
        entry = units[rel]
        specs.append({"rel": rel, "cwd": entry["directory"],
                      "args": [rebase_path(t, src_root, tree, primary)
                               for t in compile_args(entry)]})
    jobs_file = cache / f"jobs-{os.getpid()}.json"
    jobs_file.write_text(json.dumps(specs))
    jobs = args.jobs or max(2, (os.cpu_count() or 4) // 4)
    print(f"🔧 esp32 syntax check: {len(specs)} firmware unit(s), -j{jobs}...")
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
        print(f"⚠️  esp32 syntax check could not run, skipping\n{res.stderr[-800:]}")
        return 0
    if not failed:
        print(f"✅ esp32 syntax check: {len(specs)} firmware unit(s) compile")
        return 0
    for f in failed:
        errors = [l for l in f["out"].splitlines() if "error" in l][:8]
        print(f"❌ {f['rel']} does not compile for the ESP32:")
        print("\n".join("   " + l for l in errors))
    print("   Reproduce: python3 scripts/esp32_syntax_check.py --base <sha> (HELIX_ESP32_SYNTAX=0 skips)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
