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
    // Presence and docking are sensed. No backend writes either and no write
    // can echo them.
    field<false>(&Observation::present), field<false>(&Observation::tool_docked),
    field<true>(&Observation::color_rgb), field<true>(&Observation::color_name),
    field<true>(&Observation::material), field<true>(&Observation::brand),
    field<true>(&Observation::spool_name),
    // No AMS firmware carries a branded catalog product, so nothing echoes
    // it: a non-empty value can only have come from a user pick, and the
    // record path is the only thing that files one.
    field<false>(&Observation::catalog_id),
    // A read path may spell a written spool_name as the product line, so a
    // prune can relocate a declaration onto this field.
    field<true>(&Observation::product_name),
    // A spool-id echo is a true statement that firmware is now bound to that
    // spool, and resolve() ranks Spoolman above VendorCache regardless. The
    // in-flight race on a re-bind belongs to own_write_expectation. The
    // filament definition id rides the same statement as the spool id.
    field<false>(&Observation::spoolman_id), field<false>(&Observation::spoolman_filament_id),
    field<false>(&Observation::spoolman_vendor_id),
    // A weight write reseeds a real meter, and what the meter reports
    // afterwards is its own state decrementing as filament is consumed.
    // Withholding it would blind the lane the moment the meter legitimately
    // reads back the seeded number.
    field<false>(&Observation::remaining_weight_g), field<false>(&Observation::total_weight_g));

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

std::uint64_t OwnWriteEchoes::stage(int slot_index, Observation declared) {
    Entry& entry = entries_[slot_index];
    // Stash an armed predecessor for arm() to carry forward: the next edit
    // declares only what moved, while its write re-sends every identity
    // field, so the un-restated declarations still explain the echoes.
    entry.carry_armed = entry.armed;
    if (entry.armed) {
        entry.carry_declared = std::move(entry.declared);
        entry.carry_boundary = std::move(entry.boundary);
        entry.carry_sequence = entry.sequence;
    } else {
        entry.carry_declared = Observation{ObservationSource::LocalUser};
        entry.carry_boundary.clear();
        entry.carry_sequence = 0;
    }
    entry.declared = std::move(declared);
    entry.boundary.clear();
    entry.armed = false;
    entry.sequence = ++next_sequence_;
    return entry.sequence;
}

std::uint64_t OwnWriteEchoes::staged_sequence(int slot_index) const {
    const auto it = entries_.find(slot_index);
    return it == entries_.end() ? 0 : it->second.sequence;
}

Observation* OwnWriteEchoes::staged(int slot_index) {
    auto it = entries_.find(slot_index);
    return it == entries_.end() ? nullptr : &it->second.declared;
}

void OwnWriteEchoes::arm(int slot_index, std::string boundary) {
    auto it = entries_.find(slot_index);
    if (it == entries_.end())
        return;
    Entry& entry = it->second;
    if (entry.carry_armed) {
        if (entry.carry_boundary == boundary) {
            // Fill the fields this declaration does not restate; a restated
            // field keeps its new value by not being empty here. The carry
            // itself stays: a failed dispatch restores the predecessor
            // through the matched abandon().
            for_each_suppressible([&entry](auto member) {
                auto& mine = entry.declared.*member;
                if (!mine.has_value())
                    mine = entry.carry_declared.*member;
            });
        } else {
            // The write went to a different spool than the predecessor named,
            // so the predecessor's echo explains nothing read now.
            entry.carry_armed = false;
            entry.carry_declared = Observation{ObservationSource::LocalUser};
            entry.carry_boundary.clear();
            entry.carry_sequence = 0;
        }
    }
    if (!declares_anything(entry.declared)) {
        entries_.erase(it);
        return;
    }
    entry.boundary = std::move(boundary);
    entry.armed = true;
}

void OwnWriteEchoes::abandon(int slot_index) {
    entries_.erase(slot_index);
}

void OwnWriteEchoes::abandon(int slot_index, std::uint64_t staged_sequence) {
    auto it = entries_.find(slot_index);
    if (it == entries_.end() || it->second.sequence != staged_sequence)
        return;
    if (it->second.carry_armed) {
        // The failed write's echo is not coming; the predecessor's went out
        // and firmware is still repeating it, so its suppression stands
        // again under its own stamp, which a duplicated failure answer can
        // no longer reach.
        it->second.declared = std::move(it->second.carry_declared);
        it->second.boundary = std::move(it->second.carry_boundary);
        it->second.armed = true;
        it->second.sequence = it->second.carry_sequence;
        it->second.carry_armed = false;
        return;
    }
    entries_.erase(it);
}

void OwnWriteEchoes::abandon_fields(int slot_index, std::uint64_t staged_sequence,
                                    const Observation& fields) {
    auto it = entries_.find(slot_index);
    if (it == entries_.end() || it->second.sequence != staged_sequence)
        return;
    Entry& entry = it->second;
    for_each_suppressible([&](auto member) {
        if (!(fields.*member).has_value())
            return;
        if (entry.carry_armed) {
            // The refused command's echo is not coming, but firmware still
            // holds whatever the predecessor's write left at this field and
            // keeps repeating it, so the predecessor's declaration stands.
            (entry.declared.*member) = entry.carry_declared.*member;
        } else {
            (entry.declared.*member).reset();
        }
    });
    if (!declares_anything(entry.declared))
        abandon(slot_index, staged_sequence);
}

int OwnWriteEchoes::withhold(int slot_index, const std::string& boundary,
                             Observation& producer_record, const Observation& cleared) {
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
        // The frame carried the key and it read as a clear: the producer
        // stating "no value" is a statement about the field, not silence, so
        // the declaration releases even though there is no value to differ
        // from.
        if ((cleared.*member).has_value() && mine.has_value()) {
            mine.reset();
            return;
        }
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

int OwnWriteEchoes::strip_standing(int slot_index, Observation& producer_record) const {
    const auto it = entries_.find(slot_index);
    if (it == entries_.end() || !it->second.armed)
        return 0;
    const Observation& declared = it->second.declared;
    int stripped = 0;
    for_each_suppressible([&](auto member) {
        const auto& mine = declared.*member;
        auto& theirs = producer_record.*member;
        if (mine.has_value() && theirs.has_value() && *mine == *theirs) {
            theirs.reset();
            ++stripped;
        }
    });
    return stripped;
}

bool OwnWriteEchoes::standing(int slot_index) const {
    const auto it = entries_.find(slot_index);
    return it != entries_.end() && it->second.armed;
}

} // namespace helix::ams
