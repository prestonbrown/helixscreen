---
paths:
  - "lib/**/*"
  - "patches/**/*"
  - "mk/patches.mk"
  - "mk/deps.mk"
---
# Submodules and Patches

**`lib/helix-xml/` is our own repo** ([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)):
edit it directly, commit and push *in the submodule*, then commit the bumped pointer
here. Never write a patch for it, never `git restore` or `git clean` inside it. Read
`docs/devel/HELIX_XML_FORK.md` first: fork origin, the MIT position, why there is no
upstream, and the clean-room rule for anything LVGL Pro also has.

**Every other submodule (`lib/lvgl/`, `lib/libhv/`, …) is third-party and read-only.**
Changes live in `patches/*.patch`, applied by `mk/patches.mk` (`LVGL_PATCHED_FILES`,
`LIBHV_PATCHED_FILES`, …). `scripts/check_patch_drift.py` fails the build when what is
applied is not what `patches/` says. A direct edit is wiped on the next
`git submodule update`.

Workflow: edit under `lib/<sub>/`, then

```bash
cd lib/<sub> && git diff -- <files you touched> > ../../patches/<name>.patch && git restore -- <those files>
```

- **Scope the diff.** A bare `git diff` captures every patch currently applied.
- **A file two patches share folds the others into even a scoped diff** (a dozen-plus
  files are shared; `src/misc/lv_event.c` has seven). Use the pristine-file method in
  `patches/README.md` § "Regenerating a patch whose file is shared".
- **Amend the existing patch** when the change is in the same area
  (`lvgl_sdl_window.patch` already owns `lv_sdl_window.c`). Check `patches/` and
  `mk/patches.mk` before creating a new one.
- **Creating the patch is half the job.** Wire it so CI applies it, then reset the
  submodule and rebuild to prove the build passes through the patch, not the dirty tree.
- **Never `git clean` in a submodule.** Applying patches creates untracked files it would
  delete.

A worktree from `scripts/setup-worktree.sh` gets a PRIVATE checkout of `lib/lvgl`,
`lib/libhv` and `lib/helix-xml` (patches are per-branch, so one shared checkout cannot
serve two branches). Everything else in `lib/` is a symlink shared with the main tree:
never clean it from a worktree.
