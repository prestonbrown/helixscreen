// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_echo.h"

#include <tuple>
#include <type_traits>
#include <utility>

namespace helix::ams {

namespace {

/// One Observation field and the answer to the single question this guard
/// asks of it.
template <bool Suppressible, typename Member> struct FieldRow {
    static constexpr bool suppressible = Suppressible;
    Member member;
};

template <bool Suppressible, typename M> constexpr auto field(M member) {
    return FieldRow<Suppressible, M>{member};
}

/// Every Observation field, once, answering one question: can firmware only
/// hold this value because somebody told it? Those are cached declarations,
/// and firmware restating one is not a new fact. The rest are readings in
/// their own right and pass through untouched.
constexpr auto FIELD_ROSTER = std::make_tuple(
    // Presence is sensed. No backend writes it and no write can echo it.
    field<false>(&Observation::present), field<true>(&Observation::color_rgb),
    field<true>(&Observation::color_name), field<true>(&Observation::material),
    field<true>(&Observation::brand), field<true>(&Observation::spool_name),
    // No AMS firmware carries a branded catalog product, so nothing echoes
    // it: a non-empty value can only have come from a user pick, and the
    // record path is the only thing that files one.
    field<false>(&Observation::catalog_id),
    // A read path may spell a written spool_name as the product line, so a
    // prune can relocate a declaration onto this field.
    field<true>(&Observation::product_name),
    // A spool-id echo is a true statement that firmware is now bound to that
    // spool, and resolve() ranks Spoolman above VendorCache regardless. The
    // in-flight race on a re-bind belongs to own_write_expectation.
    field<false>(&Observation::spoolman_id), field<false>(&Observation::spoolman_vendor_id),
    // A weight write reseeds a real meter, and what the meter reports
    // afterwards is its own state decrementing as filament is consumed.
    // Withholding it would blind the lane the moment the meter legitimately
    // reads back the seeded number.
    field<false>(&Observation::remaining_weight_g), field<false>(&Observation::total_weight_g),
    field<false>(&Observation::echo_token));

static_assert(std::tuple_size_v<decltype(FIELD_ROSTER)> ==
                  std::tuple_size_v<decltype(std::declval<Observation&>().fields())>,
              "every Observation field needs a row above: a field with no row is one this guard "
              "lets through unexamined, so a backend that writes it back launders it into a "
              "firmware reading with nothing red");

/// Call @p fn with a pointer-to-member for each field a write can echo.
template <typename Fn> void for_each_suppressible(Fn&& fn) {
    auto visit = [&fn](const auto& row) {
        if constexpr (std::decay_t<decltype(row)>::suppressible)
            fn(row.member);
    };
    std::apply([&visit](const auto&... rows) { (visit(rows), ...); }, FIELD_ROSTER);
}

/// True when @p obs still declares a field this guard could withhold. A
/// declaration of nothing but out-of-scope fields is not a declaration here.
bool declares_anything(const Observation& obs) {
    bool any = false;
    for_each_suppressible([&](auto member) { any = any || (obs.*member).has_value(); });
    return any;
}

} // namespace

void OwnWriteEchoes::stage(int slot_index, Observation declared) {
    Entry& entry = entries_[slot_index];
    entry.declared = std::move(declared);
    entry.boundary.clear();
    entry.armed = false;
}

Observation* OwnWriteEchoes::staged(int slot_index) {
    auto it = entries_.find(slot_index);
    return it == entries_.end() ? nullptr : &it->second.declared;
}

void OwnWriteEchoes::arm(int slot_index, std::string boundary) {
    auto it = entries_.find(slot_index);
    if (it == entries_.end())
        return;
    if (!declares_anything(it->second.declared)) {
        entries_.erase(it);
        return;
    }
    it->second.boundary = std::move(boundary);
    it->second.armed = true;
}

void OwnWriteEchoes::abandon(int slot_index) {
    entries_.erase(slot_index);
}

int OwnWriteEchoes::withhold(int slot_index, const std::string& boundary,
                             Observation& producer_record) {
    auto it = entries_.find(slot_index);
    if (it == entries_.end() || !it->second.armed)
        return 0;

    if (!boundary.empty() && boundary != it->second.boundary) {
        entries_.erase(it);
        return 0;
    }

    Observation& declared = it->second.declared;
    int withheld = 0;
    for_each_suppressible([&](auto member) {
        auto& mine = declared.*member;
        auto& theirs = producer_record.*member;
        // A field the producer says nothing about this frame leaves the
        // declaration standing: silence is not a differing value.
        if (!mine.has_value() || !theirs.has_value())
            return;
        if (*mine == *theirs) {
            theirs.reset();
            ++withheld;
        } else {
            mine.reset();
        }
    });

    if (!declares_anything(declared))
        entries_.erase(it);
    return withheld;
}

} // namespace helix::ams
