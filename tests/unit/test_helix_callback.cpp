// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// helix::Callback<Sig> replaces std::function on the UpdateQueue and observer
// APIs. These tests pin the storage path each kind of callable takes and the
// ownership rule every path must keep: a stored callable is destroyed exactly
// once, whether it is invoked, moved, overwritten or dropped unrun.

#include "helix_callback.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::Callback;
using helix::callback_storage;
using helix::CallbackStorage;

namespace {

/// Counts destructor runs of every live or moved-from instance.
struct DtorCounter {
    int* count;
    explicit DtorCounter(int* c) : count(c) {}
    DtorCounter(DtorCounter&& o) noexcept : count(o.count) {
        o.count = nullptr;
    }
    DtorCounter(const DtorCounter&) = delete;
    ~DtorCounter() {
        if (count != nullptr) {
            ++*count;
        }
    }
};

/// Reports the address the callable runs at, which tells inline storage
/// (moves with the Callback) from heap storage (stays put).
struct AddressReporter {
    const void** seen;
    void operator()() const {
        *seen = this;
    }
};

/// A small callable with a user move constructor, so relocating it must go
/// through the Inline manager rather than a buffer copy.
struct MovingAddressReporter {
    const void** seen;
    bool* moved;
    std::string payload = "payload";
    MovingAddressReporter(const void** s, bool* m) : seen(s), moved(m) {}
    MovingAddressReporter(MovingAddressReporter&& o) noexcept
        : seen(o.seen), moved(o.moved), payload(std::move(o.payload)) {
        *moved = true;
    }
    void operator()() const {
        *seen = this;
    }
};

/// Over-aligned past the inline buffer, so it must take the heap path.
struct alignas(32) OverAligned {
    DtorCounter counter;
    int* runs;
    void operator()() const {
        ++*runs;
    }
};

struct BigAddressReporter {
    const void** seen;
    std::array<char, helix::callback_detail::kInlineSize + 1> pad{};
    void operator()() const {
        *seen = this;
    }
};

int add_one(int v) {
    return v + 1;
}

} // namespace

TEST_CASE("Callback: invokes with arguments and returns the result", "[helix_callback]") {
    Callback<int(int, int)> cb = [](int a, int b) { return a * 10 + b; };
    REQUIRE(cb);
    REQUIRE(cb(4, 2) == 42);

    Callback<int(int)> fp = &add_one;
    REQUIRE(fp(1) == 2);
}

TEST_CASE("Callback: storage path follows the callable's shape", "[helix_callback]") {
    int x = 0;
    auto trivial = [&x, y = 3]() { x += y; };
    auto with_string = [s = std::string("payload")]() { (void)s; };
    auto with_token = [t = std::make_shared<int>(1)]() { (void)t; };
    auto oversized = [pad = std::array<char, helix::callback_detail::kInlineSize + 1>{}]() {
        (void)pad;
    };

    CHECK(callback_storage<decltype(trivial)>() == CallbackStorage::Trivial);
    CHECK(callback_storage<decltype(with_string)>() == CallbackStorage::Inline);
    CHECK(callback_storage<decltype(with_token)>() == CallbackStorage::Inline);
    CHECK(callback_storage<decltype(oversized)>() == CallbackStorage::Heap);
    CHECK(callback_storage<OverAligned>() == CallbackStorage::Heap);
    CHECK(callback_storage<MovingAddressReporter>() == CallbackStorage::Inline);
}

TEST_CASE("Callback: LifetimeToken::defer's wrapper is stored inline", "[helix_callback]") {
    // The tagged defer wrapper: generation counter, uint64 snapshot, tag, and
    // a body capturing `this`. Every deferred UI update has this shape.
    struct Owner {
        int value = 0;
    } owner;
    auto gen = std::make_shared<std::atomic<uint64_t>>(0);
    uint64_t snapshot = 0;
    const char* tag = "tag";
    auto body = [self = &owner]() { ++self->value; };
    auto wrapper = [gen, snapshot, tag, f = body]() mutable {
        if (gen->load(std::memory_order_acquire) != snapshot || tag == nullptr) {
            return;
        }
        f();
    };
    STATIC_REQUIRE(callback_storage<decltype(wrapper)>() != CallbackStorage::Heap);

    Callback<void()> cb = std::move(wrapper);
    cb();
    REQUIRE(owner.value == 1);
}

TEST_CASE("Callback: an over-aligned callable lives on the heap", "[helix_callback]") {
    int dtors = 0;
    int runs = 0;
    {
        Callback<void()> a = OverAligned{DtorCounter(&dtors), &runs};
        Callback<void()> b = std::move(a);
        REQUIRE_FALSE(a);
        b();
        REQUIRE(runs == 1);
        REQUIRE(dtors == 0);
    }
    REQUIRE(dtors == 1);
}

TEST_CASE("Callback: a trivially copyable callable moves with its state", "[helix_callback]") {
    int total = 0;
    Callback<void()> a = [&total, step = 5]() { total += step; };
    a();
    Callback<void()> b = std::move(a);
    REQUIRE_FALSE(a);
    b();
    REQUIRE(total == 10);
}

TEST_CASE("Callback: inline storage relocates, heap storage stays put", "[helix_callback]") {
    const void* seen = nullptr;

    SECTION("trivial") {
        Callback<void()> a = AddressReporter{&seen};
        a();
        const void* before = seen;
        Callback<void()> b = std::move(a);
        b();
        REQUIRE(seen != before);
    }

    SECTION("inline, through the manager") {
        bool moved = false;
        Callback<void()> a = MovingAddressReporter(&seen, &moved);
        a();
        const void* before = seen;
        moved = false;
        Callback<void()> b = std::move(a);
        REQUIRE(moved);
        REQUIRE_FALSE(a);
        b();
        REQUIRE(seen != before);
    }

    SECTION("heap") {
        Callback<void()> a = BigAddressReporter{&seen};
        a();
        const void* before = seen;
        Callback<void()> b = std::move(a);
        REQUIRE_FALSE(a);
        b();
        REQUIRE(seen == before);
    }
}

TEST_CASE("Callback: a non-trivial callable is destroyed exactly once", "[helix_callback]") {
    int dtors = 0;

    SECTION("invoked then destroyed") {
        {
            Callback<void()> cb = [c = DtorCounter(&dtors)]() {};
            cb();
            REQUIRE(dtors == 0);
        }
        REQUIRE(dtors == 1);
    }

    SECTION("moved twice") {
        {
            Callback<void()> a = [c = DtorCounter(&dtors)]() {};
            Callback<void()> b = std::move(a);
            Callback<void()> c;
            c = std::move(b);
            REQUIRE(dtors == 0);
        }
        REQUIRE(dtors == 1);
    }

    SECTION("overwritten by assignment") {
        Callback<void()> cb = [c = DtorCounter(&dtors)]() {};
        cb = [] {};
        REQUIRE(dtors == 1);
        cb = nullptr;
        REQUIRE_FALSE(cb);
        REQUIRE(dtors == 1);
    }

    SECTION("move-assigned over") {
        int newer = 0;
        Callback<void()> a = [c = DtorCounter(&dtors)]() {};
        Callback<void()> b = [c = DtorCounter(&newer)]() {};
        a = std::move(b);
        REQUIRE(dtors == 1);
        REQUIRE(newer == 0);
        a = nullptr;
        REQUIRE(dtors == 1);
        REQUIRE(newer == 1);
    }

    SECTION("oversized, on the heap") {
        {
            Callback<void()> a = [c = DtorCounter(&dtors),
                                  pad = std::array<char, helix::callback_detail::kInlineSize>{}]() {
                (void)pad;
            };
            Callback<void()> b = std::move(a);
            REQUIRE(dtors == 0);
        }
        REQUIRE(dtors == 1);
    }
}

TEST_CASE("Callback: move-assign is safe when the held callable owns the source",
          "[helix_callback]") {
    int dtors = 0;
    int runs = 0;

    SECTION("source owned by the held callable") {
        auto inner =
            std::make_unique<Callback<void()>>([c = DtorCounter(&dtors), &runs]() { ++runs; });
        Callback<void()>* source = inner.get();
        Callback<void()> holder = [owned = std::move(inner)]() {};
        holder = std::move(*source);
        REQUIRE(dtors == 0);
        REQUIRE(holder);
        holder();
        REQUIRE(runs == 1);
        holder = nullptr;
        REQUIRE(dtors == 1);
    }

    SECTION("self-move") {
        Callback<void()> cb = [c = DtorCounter(&dtors), &runs]() { ++runs; };
        Callback<void()>& alias = cb;
        cb = std::move(alias);
        REQUIRE(cb);
        REQUIRE(dtors == 0);
        cb();
        REQUIRE(runs == 1);
        cb = nullptr;
        REQUIRE(dtors == 1);
    }
}

TEST_CASE("Callback: holds a move-only capture", "[helix_callback]") {
    auto owned = std::make_unique<int>(7);
    int seen = 0;
    Callback<void()> cb = [p = std::move(owned), &seen]() { seen = *p; };
    Callback<void()> moved = std::move(cb);
    moved();
    REQUIRE(seen == 7);
}

TEST_CASE("Callback: a const call keeps a mutable callable's state", "[helix_callback]") {
    const Callback<int()> counter = [n = 0]() mutable { return ++n; };
    REQUIRE(counter() == 1);
    REQUIRE(counter() == 2);
}

TEST_CASE("Callback: wraps a non-empty std::function", "[helix_callback]") {
    int runs = 0;
    Callback<void()> wrapped = std::function<void()>([&runs]() { ++runs; });
    REQUIRE(wrapped);
    wrapped();
    REQUIRE(runs == 1);
}

TEST_CASE("Callback: empty states", "[helix_callback]") {
    REQUIRE_FALSE(Callback<void()>{});
    REQUIRE_FALSE(Callback<void()>{nullptr});

    void (*null_fp)() = nullptr;
    REQUIRE_FALSE(Callback<void()>{null_fp});
    REQUIRE_FALSE(Callback<void()>{std::function<void()>{}});

    // Desktop builds throw like std::function; the firmware aborts.
    Callback<void()> empty;
    REQUIRE_THROWS_AS(empty(), std::bad_function_call);
}
