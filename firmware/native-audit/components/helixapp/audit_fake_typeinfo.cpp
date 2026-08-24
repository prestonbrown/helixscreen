// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audit-grade fake typeinfo objects, isolated in a TU with NO app includes:
// GCC rejects an extern "C" variable named _ZTI<class> in any TU where the
// real class declaration is visible (conflicting implicit declaration).
//
// Rationale: audit_moonraker_stub.cpp defines out-of-line methods of these
// classes, so their vtables get emitted here, and a vtable's second slot is a
// reference to the class typeinfo SYMBOL. Emitting the real one would drag each
// class's full vtable - 100+ virtuals on classes the slice never instantiates.
// The struct mimics the {vptr, name} ABI layout that libstdc++'s non-virtual
// type_info ops (hash_code/operator==/name) read.
//
// The runtime downcasts that used to want these same symbols (moonraker_manager.cpp,
// ams_backend.cpp) are gone; only the vtable slot references remain, and they
// are emitted regardless of whether anything queries the type at runtime.

namespace {
struct AuditFakeTypeinfo {
    const void* vptr;
    const char* name;
};
} // namespace

extern "C" AuditFakeTypeinfo _ZTI12MoonrakerAPI = {nullptr, "12MoonrakerAPI"};
extern "C" AuditFakeTypeinfo _ZTI19MoonrakerClientMock = {nullptr, "19MoonrakerClientMock"};
extern "C" AuditFakeTypeinfo _ZTIN5helix15MoonrakerClientE = {nullptr, "N5helix15MoonrakerClientE"};
