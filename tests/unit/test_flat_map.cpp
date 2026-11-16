// SPDX-License-Identifier: GPL-3.0-or-later

#include "flat_map.h"

#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::FlatMap;

namespace {

template <typename M> std::vector<std::string> keys_of(const M& m) {
    std::vector<std::string> out;
    for (const auto& kv : m) {
        out.push_back(kv.first);
    }
    return out;
}

} // namespace

TEST_CASE("FlatMap starts empty", "[flat_map]") {
    FlatMap<std::string, int> m;
    CHECK(m.empty());
    CHECK(m.size() == 0);
    CHECK(m.begin() == m.end());
    CHECK(m.find("x") == m.end());
    CHECK(m.count("x") == 0);
    CHECK(m.erase("x") == 0);
}

TEST_CASE("FlatMap iterates in key order whatever the insertion order", "[flat_map]") {
    FlatMap<std::string, int> m;
    m["pla"] = 1;
    m["abs"] = 2;
    m["tpu"] = 3;
    m["petg"] = 4;
    CHECK(keys_of(m) == std::vector<std::string>{"abs", "petg", "pla", "tpu"});
    CHECK(m.size() == 4);
}

TEST_CASE("FlatMap operator[] finds an existing key instead of duplicating it", "[flat_map]") {
    FlatMap<std::string, double> m;
    m["pla"] += 1.5;
    m["pla"] += 2.0;
    REQUIRE(m.size() == 1);
    CHECK(m.find("pla")->second == 3.5);
}

TEST_CASE("FlatMap insert and try_emplace keep the first value for a duplicate key", "[flat_map]") {
    FlatMap<int, std::string> m;
    CHECK(m.insert({2, "two"}).second);
    CHECK_FALSE(m.insert({2, "deux"}).second);
    CHECK(m.try_emplace(1, "one").second);
    auto r = m.try_emplace(1, "uno");
    CHECK_FALSE(r.second);
    CHECK(r.first->second == "one");
    CHECK(m.find(2)->second == "two");
    CHECK(m.size() == 2);
}

TEST_CASE("FlatMap insert_or_assign replaces an existing value", "[flat_map]") {
    FlatMap<int, std::string> m;
    CHECK(m.insert_or_assign(7, "a").second);
    CHECK_FALSE(m.insert_or_assign(7, "b").second);
    CHECK(m.find(7)->second == "b");
    CHECK(m.size() == 1);
}

TEST_CASE("FlatMap erase removes only the named key and keeps the order", "[flat_map]") {
    FlatMap<std::string, int> m;
    m["c"] = 3;
    m["a"] = 1;
    m["b"] = 2;
    CHECK(m.erase("b") == 1);
    CHECK(m.erase("b") == 0);
    CHECK(keys_of(m) == std::vector<std::string>{"a", "c"});
    CHECK_FALSE(m.contains("b"));
    CHECK(m.contains("a"));

    auto next = m.erase(m.find("a"));
    REQUIRE(next != m.end());
    CHECK(next->first == "c");
    CHECK(m.size() == 1);
}

TEST_CASE("FlatMap with one element finds it and nothing else", "[flat_map]") {
    FlatMap<const void*, int> m;
    int a = 0, b = 0;
    m[&a] = 5;
    CHECK(m.find(&a)->second == 5);
    CHECK(m.find(&b) == m.end());
    m.clear();
    CHECK(m.empty());
}
