#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Print the absolute git dir of DIR when DIR is the top of its own git
# checkout; print nothing and exit 1 otherwise.
#
# A submodule unpacked from a source tarball is a plain directory with no
# .git. Asked about it, git answers from the nearest enclosing repository
# instead (a distribution's packaging tree, or a superproject that was itself
# `git init`ed): `git apply` there skips every path outside the directory and
# exits 0, `ls-files` calls every upstream file untracked, and a stamp lands
# in someone else's git dir. Every question about a vendored submodule's
# checkout goes through this answer first.
#
# The superproject's hook variables (GIT_DIR and friends) are scrubbed, or
# they would answer for the superproject no matter what DIR is.
#
# Usage: own_git_dir.sh DIR

set -u

dir=${1:?usage: own_git_dir.sh DIR}
real=$(cd "$dir" 2>/dev/null && pwd -P) || exit 1
out=$(env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
  git -C "$real" rev-parse --show-toplevel --absolute-git-dir 2>/dev/null) || exit 1
top=${out%%$'\n'*}
git_dir=${out#*$'\n'}
[ "$top" = "$real" ] && [ -n "$git_dir" ] && [ "$git_dir" != "$out" ] || exit 1
printf '%s\n' "$git_dir"
