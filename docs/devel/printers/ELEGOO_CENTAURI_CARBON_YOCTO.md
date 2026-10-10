# Elegoo Centauri Carbon: Yocto Build (OpenCentauri COSMOS)

The Centauri Carbon 1 (Allwinner R528, Cortex-A7 hard-float NEON VFPv4, 480x272 fbdev, 112 MB
RAM) has two build targets:

| Target | What it is |
|--------|------------|
| `cc1` | The release build: fully static, built in Docker (`make cc1-docker`, `make package-cc1`), installed onto COSMOS by the regular installer into `/user-resource/helixscreen/`. Deploy with `make deploy-cc1 CC1_HOST=<ip>` (`-fg`, `-bin` variants as for other printers). User steps: [`../../user/guide/install-cc1.md`](../../user/guide/install-cc1.md) |
| `yocto` | Built by bitbake as a recipe inside [OpenCentauri/cosmos](https://github.com/OpenCentauri/cosmos) (`meta-opencentauri/recipes-apps/helixscreen/`). Bitbake supplies `CC`/`CXX`/`CFLAGS`/`LDFLAGS` and every library through the recipe's `DEPENDS`, so `YOCTO_BUILD := yes` skips all in-tree submodule builds |

The rest of this page is the local dev loop for the `yocto` target, which lets you iterate on
the recipe and the Makefile without pushing commits to GitHub each time. The sunxi BSP lives in
cosmos's `meta-sunxi` + `meta-opencentauri`.

## One-time setup

Requires Docker, git, and ~40GB free disk.

```bash
# 1. Clone cosmos (~1.5GB including submodules)
git clone --recurse-submodules --jobs=8 \
    https://github.com/OpenCentauri/cosmos.git ~/Code/Printing/yocto-cosmos

# 2. Pull the official poky dev container
docker pull crops/poky:ubuntu-22.04

# 3. Drop a dev-only auto.conf into cosmos's build/conf/ (see below)
cat > ~/Code/Printing/yocto-cosmos/build/conf/auto.conf <<'EOF'
# Public Yocto sstate mirror: pulls prebuilt gcc/glibc/pkgconfig-native/etc.
# for the scarthgap release, saving hours on the first build.
SSTATE_MIRRORS ?= "file://.* http://sstate.yoctoproject.org/all/PATH;downloadfilename=PATH"

# Build helixscreen from our live worktree mounted at /workdir/helixscreen
# (no push-to-github per iteration).
INHERIT += "externalsrc"
EXTERNALSRC:pn-helixscreen = "/workdir/helixscreen"
# B = S so the Makefile actually runs. Our Makefile builds into ./build/
# which is gitignored.
EXTERNALSRC_BUILD:pn-helixscreen = "/workdir/helixscreen"

BB_NUMBER_THREADS = "8"
PARALLEL_MAKE = "-j 8"
EOF
```

The `auto.conf` stays *outside* this repo: it is a user-local dev override
for the cosmos tree.

## Iteration loop

From anywhere inside the helixscreen repo (including a worktree):

```bash
./scripts/yocto-docker.sh                           # full build
./scripts/yocto-docker.sh helixscreen -c compile -f # force-recompile only (fastest)
./scripts/yocto-docker.sh -e helixscreen | less     # dump recipe env
./scripts/yocto-docker.sh bash                      # interactive poky shell
```

The script mounts whichever tree it was invoked from (via `$HELIX_SRC`, auto-
detected) at `/workdir/helixscreen`. Edit Makefile / code locally → rerun →
see errors in seconds-to-minutes.

First build will also pull sstate from yoctoproject.org's public mirror for
gcc-cross, glibc, pkgconfig-native, etc. Expect 10-30 minutes. Subsequent
incremental builds take seconds.

## Recipe location

```
~/Code/Printing/yocto-cosmos/meta-opencentauri/recipes-apps/helixscreen/helixscreen_0.1.bb
```

When iterating on recipe changes, edit that file directly. When it's right,
we open a PR against `OpenCentauri/cosmos` with the updated recipe.

## Notes

- `scripts/yocto-docker.sh` reads the cosmos checkout from `$YOCTO_COSMOS` (default
  `~/Code/Printing/yocto-cosmos`) and the helixscreen tree from `$HELIX_SRC` (default: the
  script's own repo). From a worktree it also mounts the main tree so `lib/` symlinks resolve.
- `-Wno-psabi` is on for every build (top of `Makefile`), so ARM psABI-transition notes do not
  trip `-Werror`.
