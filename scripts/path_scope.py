#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decide whether a gate skips a file because of the directory it sits in.

Gates skip derived or foreign trees (build/, android/, .worktrees/) by
directory name. The name test must look only at the path BELOW the repository
root: every git worktree of this repo lives under .worktrees/, so a test of the
absolute path skips every file of a worktree and the gate passes vacuously
there, in its commit hook and in its land-gate alike.

    from path_scope import in_skipped_dir

    if in_skipped_dir(path, repo_root, SKIP_PARTS):
        continue
"""

from __future__ import annotations

from pathlib import Path


def in_skipped_dir(path, root, skip_parts) -> bool:
    """True when a component of `path`, taken relative to `root`, is in `skip_parts`.

    A relative `path` is already relative to `root`. An absolute one outside
    `root` has no location in the tree to skip, so it is never skipped.
    """
    p = Path(path)
    if p.is_absolute():
        for base in (Path(root).absolute(), Path(root).resolve()):
            try:
                p = p.relative_to(base)
                break
            except ValueError:
                continue
        else:
            return False
    return any(part in skip_parts for part in p.parts)
