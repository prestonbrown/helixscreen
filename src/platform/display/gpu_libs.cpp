// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_libs.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <mutex>

namespace helix::gpu_libs {

namespace {

constexpr size_t kLibCount = 5;

struct Slot {
    std::once_flag once;
    void* handle = nullptr;
};

std::array<Slot, kLibCount>& slots() {
    static std::array<Slot, kLibCount> s;
    return s;
}

size_t index(Lib lib) {
    return static_cast<size_t>(lib);
}

} // namespace

void* handle(Lib lib) {
    Slot& slot = slots()[index(lib)];
    // RTLD_GLOBAL so the trampolines' own dlopen of the same soname, and any
    // library that needs this one, resolve to the copy already loaded.
    std::call_once(slot.once, [&] {
        slot.handle = dlopen(soname(lib), RTLD_LAZY | RTLD_GLOBAL);
        if (slot.handle == nullptr) {
            const char* err = dlerror();
            spdlog::info("[GpuLibs] {} does not load: {}", soname(lib), err ? err : "unknown");
        }
    });
    return slot.handle;
}

bool available(Lib lib) {
#ifdef HELIX_DLOPEN_GPU_LIBS
    return handle(lib) != nullptr;
#else
    // Linked directly: the binary would not have started without it.
    (void)lib;
    return true;
#endif
}

bool available_all(std::initializer_list<Lib> libs) {
    for (Lib lib : libs) {
        if (!available(lib)) {
            return false;
        }
    }
    return true;
}

bool drm_libs_present(bool egl_presentation, LoadCheck loads) {
    if (!loads(Lib::Drm)) {
        return false;
    }
    if (!egl_presentation) {
        return true;
    }
    for (Lib lib : {Lib::Gbm, Lib::Egl, Lib::Gles2}) {
        if (!loads(lib)) {
            return false;
        }
    }
    return true;
}

InputDiscovery input_discovery(bool libinput_built, LoadCheck loads) {
    return libinput_built && loads(Lib::Input) ? InputDiscovery::Libinput
                                               : InputDiscovery::SysfsEvdev;
}

void* gles2_proc(const char* name) {
    void* lib = handle(Lib::Gles2);
    return lib != nullptr ? dlsym(lib, name) : nullptr;
}

} // namespace helix::gpu_libs

namespace {

// Stand-ins for the functions of a library that does not load, one per kind of
// failure. Extra arguments are ignored under every ABI we build for (caller-
// cleaned, arguments in registers), and a long long fills both result
// registers on 32-bit ARM, so it reads as 0 or -1 whatever width was declared.
extern "C" long long helix_gpu_lib_return_zero() {
    return 0;
}

extern "C" long long helix_gpu_lib_return_negative() {
    errno = ENOSYS;
    return -1;
}

extern "C" double helix_gpu_lib_return_zero_double() {
    return 0.0;
}

// A new reference to the handle helix_gpu_lib_open() gives for a library that
// does not load. Implib.so dlclose()s whatever handle it got when the process
// exits, so it must be a real one: libdl, which every glibc binary here already
// has loaded (RTLD_NOLOAD returns the same handle each time).
// helix_gpu_lib_symbol() recognises it and never searches it, since a search
// would also reach libdl's dependencies and find libc's functions.
void* reference_sentinel() {
    void* h = dlopen("libdl.so.2", RTLD_LAZY | RTLD_NOLOAD);
    return h != nullptr ? h : dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
}

void* missing_library_handle() {
    static void* const sentinel = reference_sentinel();
    return sentinel;
}

} // namespace

// The build's table of what each trampolined function returns
// (mk/implib.mk); absent in a build without trampolines. Only ELF resolves an
// undefined weak data reference to null; Mach-O's linker rejects it, and a
// macOS build never has trampolines, so there the table is simply empty.
#if defined(__APPLE__)
namespace {
constexpr const helix::gpu_libs::ReturnKind* helix_gpu_lib_return_kinds = nullptr;
constexpr size_t kReturnKindCount = 0;
} // namespace
#else
// NAMESPACE_OK: generated C table, linked by its C name
extern "C" const helix::gpu_libs::ReturnKind helix_gpu_lib_return_kinds[] __attribute__((weak));
// NAMESPACE_OK: generated C table, linked by its C name
extern "C" const size_t helix_gpu_lib_return_kind_count __attribute__((weak));
#endif

namespace helix::gpu_libs {

Failure failure_for(const char* name, const ReturnKind* table, size_t count) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = std::strcmp(table[mid].name, name);
        if (cmp == 0) {
            return static_cast<Failure>(table[mid].kind);
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return Failure::Zero;
}

void* stand_in(Failure failure) {
    switch (failure) {
    case Failure::Negative:
        return reinterpret_cast<void*>(&helix_gpu_lib_return_negative);
    case Failure::ZeroDouble:
        return reinterpret_cast<void*>(&helix_gpu_lib_return_zero_double);
    case Failure::Zero:
        break;
    }
    return reinterpret_cast<void*>(&helix_gpu_lib_return_zero);
}

} // namespace helix::gpu_libs

// Implib.so's loader and resolver for the trampolines (mk/implib.mk passes
// these as --dlopen-callback and --dlsym-callback). Upstream aborts when a
// library or symbol is missing; these answer instead, so a call that reached a
// missing library past include/gpu_libs.h logs it and gets a failure back.

extern "C" void*
helix_gpu_lib_open(const char* soname) { // NAMESPACE_OK: Implib.so calls it by its C name
    // A fresh reference each time: the trampoline module dlclose()s its handle
    // at exit.
    if (void* lib = dlopen(soname, RTLD_LAZY | RTLD_GLOBAL)) {
        return lib;
    }
    spdlog::error("[GpuLibs] Called into {} which does not load; its functions fail", soname);
    missing_library_handle();
    return reference_sentinel();
}

extern "C" void*
helix_gpu_lib_symbol(void* lib,
                     const char* name) { // NAMESPACE_OK: Implib.so calls it by its C name
    if (lib != nullptr && lib != missing_library_handle()) {
        if (void* fn = dlsym(lib, name)) {
            return fn;
        }
    }
#if defined(__APPLE__)
    const size_t count = kReturnKindCount;
#else
    const size_t count =
        &helix_gpu_lib_return_kind_count != nullptr ? helix_gpu_lib_return_kind_count : 0;
#endif
    const auto failure = helix::gpu_libs::failure_for(name, helix_gpu_lib_return_kinds, count);
    spdlog::error("[GpuLibs] {} is unavailable; the call fails", name);
    return helix::gpu_libs::stand_in(failure);
}
