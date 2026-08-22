# AD5M Klipper Mod Native Variant

HelixScreen can be built by the AD5M Klipper Mod firmware (xblax/flashforge_ad5m_klipper_mod)
as a first-class variant alongside GuppyScreen and KlipperScreen. This doc
explains the integration contract we provide upstream.

## Two build modes for AD5M

| Mode | Target | When to use |
|---|---|---|
| `ad5m` | ARM GCC 10.3 (bundled via Docker) + fully static | Our release flow. Sideload install onto ForgeX/zmod/kmod; produces a standalone tarball. |
| `ad5m-br` | External cross-toolchain (buildroot / yocto / caller-provided) + dynamic linking | Consumed by an outer build system (kmod's buildroot package) that manages the toolchain and system libraries. |

Both produce an armv7-a (Cortex-A7 hard-float) binary with identical functional
behavior. `ad5m-br` trades binary size (static → dynamic) for integration
simplicity with upstream distros.

## What `ad5m-br` expects from the caller

- `CROSS_COMPILE` prefix pointing at an armv7-a hard-float glibc toolchain
  (buildroot's default: `arm-buildroot-linux-gnueabihf-`).
- `CC`, `CXX`, `AR`, `RANLIB`, `STRIP` set to the full toolchain binaries (or
  prefixed names resolvable via `PATH`).
- A sysroot with `libssl`, `libcrypto`, `libstdc++`, `libpthread`, `libm`,
  `libdl`, `libz`.
- `libevdev` available at link time (for touch input — buildroot: `BR2_PACKAGE_LIBEVDEV=y`).

What we build in-tree (not from sysroot): `libhv`, `spdlog`, `libnl-3`,
`libnl-genl-3`, `libwpa_client`. These are pinned submodules with project-
specific patches; using system versions would force incompatible version
churn.

## Invoking the build from an external tree

```make
# buildroot example (kmod's helixscreen.mk):
define HELIXSCREEN_BUILD_CMDS
    $(MAKE) $(TARGET_CONFIGURE_OPTS) PLATFORM_TARGET=ad5m-br -C $(@D)
endef

define HELIXSCREEN_INSTALL_TARGET_CMDS
    $(MAKE) $(TARGET_CONFIGURE_OPTS) PLATFORM_TARGET=ad5m-br \
        install DESTDIR=$(TARGET_DIR) -C $(@D)
endef
```

`$(TARGET_CONFIGURE_OPTS)` is buildroot-provided; it expands to something like:
`CC=arm-buildroot-linux-gnueabihf-gcc CXX=... AR=... RANLIB=... STRIP=... CFLAGS=...`

## Install layout

`make install DESTDIR=$(TARGET_DIR)` writes:

```
$(DESTDIR)/opt/helixscreen/
├── bin/
│   ├── helix-screen           # main binary (armv7-a)
│   ├── helix-splash           # boot splash (optional)
│   └── helix-watchdog         # crash recovery (optional)
├── ui_xml/                    # runtime XML layouts
├── assets/
│   ├── fonts/                 # medium + large tier only for AD5M
│   ├── images/
│   ├── sounds/                # PWM-tone only on AD5M
│   └── config/
│       ├── printer_database.json
│       ├── presets/{ad5m,ad5m_pro,ad5m_zmod,ad5m_pro_zmod}.json
│       ├── print_start_profiles/ad5m.json
│       └── platform/hooks-ad5m-kmod.sh
└── certs/ca-certificates.crt  # only if built with `make ad5m-docker`
```

Runtime writable state (config file, log, cache) is **not** installed — the
init script creates it under `/data/helixscreen/` on first boot.

## How `data_root_resolver` finds everything

`src/application/data_root_resolver.cpp` strips known suffixes (`/build/bin`,
`/bin`) from the binary's argv[0] to discover the asset root. Installing to
`/opt/helixscreen/bin/helix-screen` means the resolver strips `/bin` and lands
at `/opt/helixscreen/`, where it finds `ui_xml/` — no env vars needed.

## Testing locally

```bash
# Build + install into a staging dir using our Docker toolchain image
# (close enough to buildroot's toolchain for verification)
make docker-toolchain-ad5m

STAGE=$(pwd)/build/ad5m-br-staging
docker run --rm --user $(id -u):$(id -g) \
  -v "$PWD":/src -w /src \
  -e PATH=/opt/arm-toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  -v "$HOME/.cache/helixscreen-ccache/ad5m-br":/ccache \
  -e CCACHE_DIR=/ccache \
  helixscreen/toolchain-ad5m \
  sh -c 'make PLATFORM_TARGET=ad5m-br \
         CROSS_COMPILE=arm-none-linux-gnueabihf- \
         CC=arm-none-linux-gnueabihf-gcc CXX=arm-none-linux-gnueabihf-g++ \
         AR=arm-none-linux-gnueabihf-ar RANLIB=arm-none-linux-gnueabihf-ranlib \
         STRIP=arm-none-linux-gnueabihf-strip SKIP_OPTIONAL_DEPS=1 -j$(nproc) && \
         make install PLATFORM_TARGET=ad5m-br DESTDIR=/src/build/ad5m-br-staging'

# If running from a worktree, ALSO bind-mount the main repo path so symlinks resolve:
#   -v /absolute/main/repo/path:/absolute/main/repo/path

# Inspect
find build/ad5m-br-staging/opt/helixscreen -maxdepth 2 -type d | sort
```
