# AD5M Toolchain & glibc Compatibility Notes

Why the `ad5m` target links fully static, and the toolchains that were ruled out. Build and
deploy commands live in [`../printers/FLASHFORGE_AD5M_SUPPORT.md`](../printers/FLASHFORGE_AD5M_SUPPORT.md).

## Target Hardware Specs
- **CPU**: Cortex-A7 (armv7-a hard-float)
- **Display**: 800x480 framebuffer
- **RAM**: 110MB total
- **C Library**: glibc 2.25
- **Key Constraint**: Cannot use symbols newer than GLIBC_2.25

## Problem Summary

Modern ARM toolchains ship with sysroots containing glibc 2.28+ symbols. When we compile
with these toolchains, the resulting binary requires newer glibc symbols than AD5M provides.

**Symbols causing issues** (from `objdump -T` / `readelf --dyn-syms`):
- `stat@GLIBC_2.33` / `fstat@GLIBC_2.33` / `lstat@GLIBC_2.33` - stat family
- `pow@GLIBC_2.29` - math functions
- `fcntl@GLIBC_2.28` - file control

---

## Solution: Fully Static Build

**Approach**: Link everything statically with `-static` flag using ARM GCC 10.3

**Config** (in `mk/cross.mk`):
```makefile
TARGET_LDFLAGS := -Wl,--gc-sections -flto -static
```

**Results**:
- Binary size: 2.7MB helix-screen, 419KB helix-splash when the survey was run; the binary has grown with the app since
- glibc dependency: NONE (fully self-contained)
- Compatibility: ✅ Tested and working on AD5M with glibc 2.25
- Test command on AD5M: `/tmp/helix-screen-static --help` runs correctly

**Trade-offs**:
- ✅ Guaranteed compatibility - no glibc version issues
- ✅ Can use modern toolchain (GCC 10+) with C++17 features
- ✅ Self-contained binary, no external dependencies
- ⚠️ `getaddrinfo` warning during link (harmless for local Moonraker communication)
- ❌ Slightly larger binary (2.7MB vs ~1-2MB dynamic, but less than the 5-8MB we feared)

---

## Approaches Researched

### 1. ARM GCC 10.3-2021.07 (the Docker toolchain)
- **URL**: https://developer.arm.com/downloads/-/gnu-a
- **Sysroot glibc**: 2.33
- **Status**: ✅ WORKS with `-static` flag
- **Problem with dynamic**: Produces binaries requiring GLIBC_2.33 symbols
- **Solution**: Use `-static` flag for fully static build

### 2. Bootlin Toolchains
- **URL**: https://toolchains.bootlin.com/
- **Status**: ❌ NOT SUITABLE
- **Problem**: All current builds use glibc 2.39+ (way too new)
- **Note**: Bootlin rebuilds toolchains, doesn't keep old glibc versions

### 3. Linaro GCC 7.5-2019.12
- **URL**: https://releases.linaro.org/components/toolchain/binaries/7.5-2019.12/arm-linux-gnueabihf/
- **Sysroot glibc**: 2.25 (exact match for AD5M!)
- **Status**: ⚠️ NOT NEEDED - static build is simpler
- **Problem**: GCC 7.5 has `std::experimental::filesystem`, not `std::filesystem`

### 4. Musl-based Toolchain
- **Status**: ❌ NOT NEEDED - static glibc build works fine

---

## Build Issues Encountered & Fixes

### Issue 1: wpa_supplicant LTO Conflict
**Symptom**: Undefined references to `wpa_ctrl_*` functions despite libwpa_client.a being linked
```
arm-none-linux-gnueabihf-ar: plugin needed to handle lto object
undefined reference to `wpa_ctrl_request'
undefined reference to `wpa_ctrl_open'
```

**Status**: ✅ FIXED
**Root Cause**: wpa_supplicant was compiled with LTO flags inherited from our build.
LTO-compiled .a files contain GIMPLE IR (intermediate representation), not machine code.
When linking with our LTO-compiled objects, the linker couldn't process the mixed formats.

**Fix** (in `mk/deps.mk`):
```makefile
# Strip LTO flags for wpa_supplicant build
WPA_CFLAGS := $(filter-out -flto -ffunction-sections -fdata-sections,$(TARGET_CFLAGS))
# Use env -u to unset inherited CFLAGS
$(Q)env -u CFLAGS CC="$(CC)" EXTRA_CFLAGS="$(WPA_CFLAGS)" $(MAKE) -C ...
```

### Issue 2: helix-splash DisplayBackend::create_auto() Undefined
**Symptom**:
```
undefined reference to `DisplayBackend::create_auto()'
```

**Status**: ✅ FIXED
**Root Cause**: With LTO enabled, the linker aggressively strips symbols from static
libraries that it doesn't see as directly referenced. `create_auto()` is called
internally through `create()`, but the linker only sees the entry point in `main()`.

**Fix** (in `mk/splash.mk`):
```makefile
# Use --whole-archive to force all symbols from display library
$(Q)$(CXX) $(SPLASH_OBJ) -Wl,--whole-archive $(DISPLAY_LIB) -Wl,--no-whole-archive ...
```

### Issue 3: glibc Symbol Version Mismatch
**Symptom** (on AD5M device):
```
/lib/libc.so.6: version `GLIBC_2.33' not found (required by ./helix-screen)
/lib/libc.so.6: version `GLIBC_2.29' not found (required by ./helix-screen)
```

**Status**: ✅ FIXED via static linking
**Root Cause**: Toolchain sysroot has glibc 2.33, AD5M system has glibc 2.25
**Solution**: Static linking with `-static` flag eliminates all glibc dependencies

---

## Verification Commands

```bash
# Check if binary is static
file build/ad5m/bin/helix-screen    # Should show "statically linked"

# Verify no dynamic glibc dependencies
ldd build/ad5m/bin/helix-screen     # Should show "not a dynamic executable"

# Check binary size
ls -lh build/ad5m/bin/
```

---

## Lessons Learned

1. **Static linking is simpler** than hunting for perfectly matching toolchains
2. **Binary size is acceptable** for an embedded UI
3. **The getaddrinfo warning is harmless** - we only connect to local Moonraker IPs
4. **Permission issues** - always use `-u $(id -u):$(id -g)` with Docker to avoid root-owned files
5. **SKIP_OPTIONAL_DEPS=1** is required for cross-compilation (no npm/clang-format in Docker)
