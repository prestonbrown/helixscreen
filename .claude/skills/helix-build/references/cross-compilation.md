# Cross-Compilation

## Docker Toolchain Architecture

Each target has a Dockerfile in `docker/` containing the cross-compiler, sysroot libraries, and build tools.

```
docker/
├── Dockerfile.pi              # Pi 64-bit (Debian Bullseye, GCC 10, dynamic)
├── Dockerfile.pi32            # Pi 32-bit (Debian Bullseye, GCC 10, dynamic)
├── Dockerfile.ad5m            # AD5M (Debian Buster, GCC 8, dynamic)
├── Dockerfile.cc1             # CC1 (Debian Bookworm, ARM GCC 10.3)
├── Dockerfile.k1              # K1 static (Bootlin mips32el-musl, GCC 12)
├── Dockerfile.k1-dynamic      # K1 dynamic (crosstool-NG, GCC 7.5, glibc 2.29)
├── Dockerfile.k2              # K2 (Bootlin armv7-eabihf-musl, GCC 12)
├── Dockerfile.ad5x            # AD5X (MIPS32r5)
├── Dockerfile.snapmaker-u1    # Snapmaker U1 (aarch64)
├── Dockerfile.x86             # x86_64 (Debian Bullseye, native)
└── build-toolchains.sh        # Build all images: ./docker/build-toolchains.sh [pi|ad5m|all]
```

### How Docker Builds Work

1. `make pi-docker` checks if `helixscreen/toolchain-pi` image exists
2. If missing, auto-builds the image (2-5 min first time)
3. Mounts source code into container: `-v $(PWD):/src`
4. Runs `make PLATFORM_TARGET=pi SKIP_OPTIONAL_DEPS=1 -j$(NPROC_DOCKER_RUN)`
5. Compiled binaries appear in local `build/` directory

### Docker CCache
Per-target ccache volumes mounted at `/ccache` inside container:
```bash
DOCKER_CCACHE_BASE=~/.cache/helixscreen-ccache  # Override default
```

### NPROC in Docker
Capped at 8 to prevent container resource exhaustion:
```bash
NPROC_DOCKER_RUN=8  # Override with env var
```

## Cross.mk Platform Definitions

`mk/cross.mk` defines each `PLATFORM_TARGET` with:
- `CROSS_COMPILE` — toolchain prefix (e.g., `aarch64-linux-gnu-`)
- `TARGET_ARCH` — architecture string
- `TARGET_TRIPLE` — for sysroot library paths
- `TARGET_CFLAGS` — arch-specific compiler flags
- `DISPLAY_BACKEND` — drm, fbdev, or sdl
- `ENABLE_*` — feature toggles (SDL, OpenGLES, GLES_3D, etc.)
- `BUILD_SUBDIR` — output subdirectory (e.g., `pi`, `ad5m`)
- `STRIP_BINARY` — whether to strip for release
- `FONT_TIERS` — which font sizes to include

### Key Configuration Patterns

**Pi (aarch64, DRM+GLES):**
```makefile
CROSS_COMPILE ?= aarch64-linux-gnu-
TARGET_CFLAGS := -march=armv8-a -funwind-tables ...
DISPLAY_BACKEND := drm
ENABLE_OPENGLES := yes
ENABLE_SSL := yes
```

**K1 static (MIPS, musl):**
```makefile
CROSS_COMPILE ?= mipsel-buildroot-linux-musl-
TARGET_CFLAGS := -march=mips32r2 -EL -msoft-float ...
DISPLAY_BACKEND := fbdev
# Fully static binary - no system library dependencies
LDFLAGS := $(LIBHV_LIBS) ... -latomic -ldl -lz -lm -lpthread
```

**K1 dynamic (MIPS, glibc):**
```makefile
# Mixed linking: project libs static, system libs dynamic
LDFLAGS := -Wl,-Bstatic $(LIBHV_LIBS) ... -Wl,-Bdynamic -lstdc++ -lz ...
```

## Deployment

### Pi
```bash
make pi-test           # Build + deploy + run
make deploy-pi         # Deploy binaries + assets
make deploy-pi-run     # Deploy and run
make deploy-pi PI_HOST=192.168.1.50 PI_USER=pi
```

### AD5M
```bash
make ad5m-test         # Remote build + deploy + run
make remote-ad5m       # Build on thelio.local, fetch binaries
make deploy-ad5m       # Deploy + restart in background
make deploy-adm-fg     # Deploy + run foreground (debug)
make deploy-adm-bin    # Deploy binaries only (fast iteration)
```

mDNS (`ad5m.local`) may not resolve — use IP:
```bash
AD5M_HOST=192.168.1.67 make deploy-ad5m
```

### Manual
```bash
scp build/pi/bin/helix-screen pi@mainsailos.local:~/
scp build/ad5m/bin/helix-screen root@192.168.1.x:/usr/data/
```

## Logging on Target

| Platform | Default Backend | View |
|----------|----------------|------|
| Linux + systemd | journal | `journalctl -t helix -f` |
| Linux (no systemd) | syslog | `tail -f /var/log/syslog \| grep helix` |
| File fallback | rotating file | `tail -f /var/log/helix-screen.log` |

Override: `./helix-screen --log-dest=journal|file --log-file=/tmp/debug.log`

## Dockerfile Pattern (Debian-based)

```dockerfile
FROM debian:bullseye-slim
RUN apt-get update && apt-get install -y \
    build-essential crossbuild-essential-arm64 \
    git pkg-config cmake python3 ccache ...
RUN dpkg --add-architecture arm64 && apt-get install -y \
    libc6-dev:arm64 libdrm-dev:arm64 libssl-dev:arm64 ...
ENV CROSS_COMPILE=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
CMD ["make", "PLATFORM_TARGET=pi", "SKIP_OPTIONAL_DEPS=1", "-j"]
```

Key notes:
- Use `gcc-ar`/`gcc-ranlib` (not plain `ar`) for LTO compatibility
- Build against oldest supported Debian (Bullseye) for forward-compatible glibc
- Statically link OpenSSL to avoid soname mismatches across Debian versions
- **Never** install `nlohmann-json3-dev` — use bundled `hv/json.hpp` from libhv

## Build Output Layout

```
build/
  {platform}/
    obj/           # All compiled objects
    lib/           # Static libraries (libhv, libwpa_client)
    bin/           # Final binaries
      helix-screen
      helix-splash    # (Pi/x86 DRM targets)
      helix-watchdog  # (Pi/x86 DRM targets)
  {platform}-fbdev/   # (dual-link targets only)
    bin/
      helix-screen
```
