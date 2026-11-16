// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

namespace helix {

/// A map over a vector of key/value pairs kept in key order, for UI state that
/// holds a handful of keys. std::map and std::unordered_map instantiate their
/// tree or hash-table code per key/value pair, which the firmware image carries
/// once per map type; this is a vector and a linear scan. Iteration is in key
/// order, as std::map iterates. Lookup is linear: keep it to tens of keys.
///
/// Elements are std::pair<K, V> with a mutable key; never assign through
/// `it->first`. Inserting or erasing invalidates iterators and references.
template <typename K, typename V, typename Less = std::less<>> class FlatMap {
  public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    iterator begin() {
        return items_.begin();
    }
    iterator end() {
        return items_.end();
    }
    const_iterator begin() const {
        return items_.begin();
    }
    const_iterator end() const {
        return items_.end();
    }
    size_t size() const {
        return items_.size();
    }
    bool empty() const {
        return items_.empty();
    }
    void clear() {
        items_.clear();
    }
    void reserve(size_t n) {
        items_.reserve(n);
    }

    template <typename Key> iterator find(const Key& key) {
        iterator it = lower_bound(key);
        return (it != items_.end() && !less_(key, it->first)) ? it : items_.end();
    }
    template <typename Key> const_iterator find(const Key& key) const {
        return const_cast<FlatMap*>(this)->find(key);
    }
    template <typename Key> size_t count(const Key& key) const {
        return find(key) != end() ? 1 : 0;
    }
    template <typename Key> bool contains(const Key& key) const {
        return find(key) != end();
    }

    /// Inserts a default-constructed value for a missing key.
    V& operator[](const K& key) {
        return try_emplace(key).first->second;
    }

    /// Inserts {key, V(args...)} when the key is absent; an existing entry is
    /// left untouched, as std::map::try_emplace does.
    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const K& key, Args&&... args) {
        iterator it = lower_bound(key);
        if (it != items_.end() && !less_(key, it->first)) {
            return {it, false};
        }
        it = items_.emplace(it, std::piecewise_construct, std::forward_as_tuple(key),
                            std::forward_as_tuple(std::forward<Args>(args)...));
        return {it, true};
    }

    std::pair<iterator, bool> insert(value_type kv) {
        iterator it = lower_bound(kv.first);
        if (it != items_.end() && !less_(kv.first, it->first)) {
            return {it, false};
        }
        return {items_.insert(it, std::move(kv)), true};
    }

    template <typename M> std::pair<iterator, bool> insert_or_assign(const K& key, M&& value) {
        auto r = try_emplace(key);
        r.first->second = std::forward<M>(value);
        return r;
    }

    /// Erases the entry for `key`; returns how many were erased (0 or 1).
    size_t erase(const K& key) {
        iterator it = find(key);
        if (it == items_.end()) {
            return 0;
        }
        items_.erase(it);
        return 1;
    }
    iterator erase(const_iterator pos) {
        return items_.erase(pos);
    }

  private:
    template <typename Key> iterator lower_bound(const Key& key) {
        iterator it = items_.begin();
        while (it != items_.end() && less_(it->first, key)) {
            ++it;
        }
        return it;
    }

    std::vector<value_type> items_;
    Less less_;
};

} // namespace helix
