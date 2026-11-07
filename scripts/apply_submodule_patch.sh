#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Apply one patch to one vendored submodule with a verdict that cannot lie.
#
# A bare `git apply --check` cannot distinguish "already applied" from "this
# patch no longer matches the submodule": both exit non-zero, so an else-branch
# that prints "already applied" turns a dead patch into a green line. A reverse
# check does not settle it either — several patches share one file (seven touch
# src/misc/lv_event.c), and once a sibling patch has moved the context, a
# correctly applied patch neither forward- nor reverse-applies.
#
# So the verdict is three-way, and the hard failure belongs to the only run
# that can judge: a from-clean apply (HELIX_PATCHES_FROM_CLEAN=1, set by `make
# reapply-patches`). From clean nothing is applied yet, so every patch must
# take the apply branch; anything else means the patch does not match the
# submodule. Incremental runs warn instead, because there a sibling patch
# earlier in the same recipe may legitimately have moved the context.
#
# The flag is a claim about the tree, and the claim is verified, not assumed:
# `make clean` deletes the stamp but leaves the submodules patched, and on a
# patched tree shared-file shadowing makes healthy patches read "neither", so
# an unverified flag would fail a perfectly good checkout. A mk/patches.mk run
# settles the claim once at recipe start into HELIX_FROM_CLEAN_SENTINEL; a
# standalone invocation judges the submodule directly.
#
# Usage: apply_submodule_patch.sh <submodule-dir> <patch-file> <label> [note]
#
# The optional note names the runtime consequence of building without the
# patch. It is appended to the verdicts that mean "this patch may be
# missing" - the marker-absent warn and the from-clean fatal - never to the
# success lines.
#
# The neither-branch is triaged by the marker table: one line this patch
# adds (or removes) that upstream never contained, grepped by
# check_patch_markers.py --only. A present marker means the patch's effect
# is in the checkout and a sibling merely moved the context git compares,
# which is the routine case on shared files (seven patches touch
# src/misc/lv_event.c); an absent marker means the effect is genuinely
# missing; no row means no information and the verdict stays hedged.
#
# A submodule that is not its own git checkout (unpacked from a source
# tarball) gets the same three-way verdict from the same engine: `git apply`
# run inside the directory with repository discovery stopped at it, so an
# enclosing repository cannot claim the paths and skip them. With no git
# installed, GNU `patch -p1 --fuzz=0` stands in; it allows the same line
# offsets git does and no more. Such a tree can never back the from-clean
# claim, because nothing records what its pristine bytes were.

set -u

submodule=$1
patch_file=$2
label=$3
note=${4:-}

# A pre-commit hook exports GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE for the
# superproject, and anything spawning git under it inherits them: `git -C
# lib/lvgl ...` would then answer with the superproject's repository instead
# of the submodule's. Scrub them so every question below is answered by the
# submodule the -C path names (mirrors GIT_NOENV in mk/patches.mk).
git_submodule() {
  env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
    git -C "$submodule" "$@"
}

here=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$here/own_git_dir.sh" ]; then
  echo "✗ ${label}: $here/own_git_dir.sh is missing" >&2
  exit 1
fi

# git: the submodule's own checkout. git-dir: a plain directory, judged by git
# outside any repository. patch: a plain directory and no git on PATH.
if bash "$here/own_git_dir.sh" "$submodule" >/dev/null 2>&1; then
  backend="git"
elif command -v git >/dev/null 2>&1; then
  backend="git-dir"
elif command -v patch >/dev/null 2>&1; then
  backend="patch"
else
  echo "✗ ${label}: $submodule is not a git checkout and neither git nor patch is installed" >&2
  exit 1
fi

case $patch_file in
  /*) abs_patch=$patch_file ;;
  *) abs_patch=$PWD/$patch_file ;;
esac

# apply_patch [--check] [--reverse]: one apply question, asked of the backend.
apply_patch() {
  case $backend in
    git)
      git_submodule apply "$@" "$patch_file"
      ;;
    git-dir)
      ( cd "$submodule" &&
        GIT_CEILING_DIRECTORIES=$(dirname "$(pwd -P)") \
          env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
          git apply "$@" "$abs_patch" )
      ;;
    patch)
      local args=() a
      for a in "$@"; do
        case $a in
          --check) args+=(--dry-run) ;;
          --reverse) args+=(-R) ;;
        esac
      done
      patch -p1 -f -s --fuzz=0 --no-backup-if-mismatch -r - ${args[@]+"${args[@]}"} \
        -d "$submodule" -i "$abs_patch"
      ;;
  esac
}

if [ -n "${NO_COLOR:-}" ] ||
   ! { [ -t 0 ] && [ -t 2 ] && [ -n "${TERM:-}" ] && [ "${TERM:-}" != "dumb" ]; }; then
  green='' yellow='' red='' reset=''
else
  green=$'\033[32m' yellow=$'\033[33m' red=$'\033[31m' reset=$'\033[0m'
fi

# Did this run actually start from a pristine checkout? The fatal verdict is
# licensed by the answer, and the answer cannot come from the flag alone: on a
# patched tree a healthy shared-file patch reads "neither" too.
# The files this patch touches, from its own diff headers.
patch_files() {
  sed -n -e 's|^diff --git a/[^ ]* b/||p' -e 's|^+++ b/||p' "$patch_file" 2>/dev/null |
    sort -u | grep -v '^/dev/null$' ||:
}

from_clean_verified() {
  local sentinel="${HELIX_FROM_CLEAN_SENTINEL:-}"
  if [ -n "$sentinel" ] && [ -f "$sentinel" ]; then
    [ "$(cat "$sentinel" 2>/dev/null)" = "1" ]
    return
  fi
  # Standalone invocation with no recipe guard: judge the submodule directly,
  # scoped to the files this patch touches - the recipe guard checks its
  # explicit file list the same way, and unrelated dirt (an untracked file
  # anywhere in the tree) is not evidence about this patch. One status run
  # answers both whether git can read the tree and what changed in it; a
  # pathspec the checkout has no file for simply matches nothing, which is
  # the pristine answer for a file the patch itself creates.
  [ "$backend" = git ] || return 1
  local files out
  files=$(patch_files)
  [ -n "$files" ] || files=.
  out=$(git_submodule status --porcelain -- $files 2>/dev/null) || return 1
  [ -z "$out" ]
}

# Marker-table verdict for this one patch: 0 present, 1 absent, 2 unknown
# (no row, no python, or no answer). Anything but a clean answer reads as
# unknown to the caller.
marker_state() {
  python3 "$here/check_patch_markers.py" \
    --tsv "${HELIX_PATCH_MARKERS_TSV:-$here/../mk/patch-markers.tsv}" \
    --lvgl "${HELIX_MARKER_LVGL_DIR:-lib/lvgl}" \
    --libhv "${HELIX_MARKER_LIBHV_DIR:-lib/libhv}" \
    --lua "${HELIX_MARKER_LUA_DIR:-lib/lua}" \
    --only "$(basename "$patch_file")" >/dev/null 2>&1
}

if apply_patch --check >/dev/null 2>&1; then
  echo "${yellow}→ Applying ${label}...${reset}"
  if ! apply_patch; then
    echo "${red}✗ ${label}: ${backend%-dir} apply failed${reset}"
    exit 1
  fi
  echo "${green}✓ ${label} applied${reset}"
elif apply_patch --check --reverse >/dev/null 2>&1; then
  echo "${green}✓ ${label} already applied${reset}"
elif [ "${HELIX_PATCHES_FROM_CLEAN:-0}" = "1" ] && from_clean_verified; then
  echo "${red}✗ ${label} does not apply to a clean checkout — the patch and the submodule disagree.${note:+ $note}" >&2
  echo "${red}  Regenerate it: patches/README.md § \"Regenerating a patch whose file is shared\"${reset}" >&2
  echo "${red}  If a later patch now owns every file this one touches, it is superseded - delete it rather than regenerate it (patches/README.md § \"A patch superseded by a later patch\").${reset}" >&2
  exit 1
else
  marker_rc=0; marker_state || marker_rc=$?
  if [ "$marker_rc" -eq 0 ]; then
    echo "${green}✓ ${label} already applied${reset} (marker present; sibling patches moved the context git compares)"
  elif [ "$marker_rc" -eq 1 ]; then
    echo "${yellow}⚠ ${label}: its marker is absent from the checkout - the patch's effect is missing.${note:+ $note} Run 'make reapply-patches'${reset}"
  else
    echo "${yellow}⚠ ${label} is not verifiable in place: neither applies nor reverses (later patches may share its files).${note:+ $note} Run 'make reapply-patches' to judge it from a clean checkout${reset}"
  fi
fi
