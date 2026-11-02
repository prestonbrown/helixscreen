// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"
#include "plugin_table.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace helix::plugin {

namespace {

RegistryKey g_table_state_key{"table_state"};
const char kTableMeta[] = "helix.table";
constexpr size_t kMaxTitleBytes = 256;

struct TableEntry {
    std::string full;
    int activate_ref = LUA_NOREF;
    size_t charged = 0; ///< the entry, its name and its model
};

struct TableState {
    LuaRuntime* rt = nullptr;
    LifetimeToken token;
    // Never erased: handle userdata index into it.
    std::vector<std::unique_ptr<TableEntry>> tables;
};

TableState* table_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &g_table_state_key);
    auto* s = static_cast<TableState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

TableEntry& check_table(lua_State* L) {
    const uint32_t index = *static_cast<uint32_t*>(luaL_checkudata(L, 1, kTableMeta));
    return *table_state(L)->tables[index];
}

/// Moves the entry's charge to `bytes`, or raises with the old charge kept.
void recharge(lua_State* L, TableEntry& e, size_t bytes, const char* call) {
    if (bytes > e.charged)
        charge_plugin_memory(L, bytes - e.charged, call);
    else
        context(L).rt.release_external(e.charged - bytes);
    e.charged = bytes;
}

size_t entry_bytes(const TableEntry& e, const TableModel* m) {
    return sizeof(TableEntry) + e.full.size() + (m ? m->bytes() : 0);
}

std::vector<TableColumn> read_columns(lua_State* L, int idx, const char* fn) {
    lua_getfield(L, idx, "columns");
    if (!lua_istable(L, -1))
        luaL_error(L, "%s: columns must be a list of column tables", fn);
    const lua_Integer n = luaL_len(L, -1);
    if (n < 1 || static_cast<size_t>(n) > kMaxTableColumns)
        luaL_error(L, "%s: 1 to %d columns", fn, static_cast<int>(kMaxTableColumns));
    std::vector<TableColumn> cols;
    int total = 0;
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1))
            luaL_error(L, "%s: column %d must be a table", fn, static_cast<int>(i));
        TableColumn c;
        lua_getfield(L, -1, "title");
        size_t len = 0;
        const char* title = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : nullptr;
        if (!title || len > kMaxTitleBytes)
            luaL_error(L, "%s: column %d title must be a string of at most %d bytes", fn,
                       static_cast<int>(i), static_cast<int>(kMaxTitleBytes));
        c.title.assign(title, len);
        lua_pop(L, 1);
        lua_getfield(L, -1, "width");
        if (!lua_isnil(L, -1)) {
            int ok = 0;
            const lua_Integer w = lua_tointegerx(L, -1, &ok);
            if (!ok || w < 1 || w > 100)
                luaL_error(L, "%s: column %d width must be a whole percent from 1 to 100", fn,
                           static_cast<int>(i));
            c.width_pct = static_cast<int>(w);
            total += c.width_pct;
        }
        lua_pop(L, 2);
        cols.push_back(std::move(c));
    }
    lua_pop(L, 1);
    if (total > 100)
        luaL_error(L, "%s: column widths add up to more than 100 percent", fn);
    return cols;
}

std::string read_cell(lua_State* L, int idx, size_t row, size_t col, const char* fn) {
    const int t = lua_type(L, idx);
    if (t != LUA_TSTRING && t != LUA_TNUMBER)
        luaL_error(L, "%s: row %d cell %d must be a string or a number", fn, static_cast<int>(row),
                   static_cast<int>(col));
    size_t len = 0;
    const char* s = luaL_tolstring(L, idx, &len);
    if (len > kMaxTableCellBytes)
        luaL_error(L, "%s: row %d cell %d is over %d bytes", fn, static_cast<int>(row),
                   static_cast<int>(col), static_cast<int>(kMaxTableCellBytes));
    std::string out(s, len);
    lua_pop(L, 1);
    return out;
}

std::vector<TableRow> read_rows(lua_State* L, int idx, size_t columns, const char* fn) {
    luaL_checktype(L, idx, LUA_TTABLE);
    const lua_Integer n = luaL_len(L, idx);
    if (static_cast<size_t>(n) > kMaxTableRows)
        luaL_error(L, "%s: at most %d rows", fn, static_cast<int>(kMaxTableRows));
    std::vector<TableRow> rows;
    rows.reserve(static_cast<size_t>(n));
    std::set<std::string> keys;
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        const int row = lua_gettop(L);
        if (!lua_istable(L, row))
            luaL_error(L, "%s: row %d must be a table", fn, static_cast<int>(i));
        TableRow r;
        lua_getfield(L, row, "key");
        size_t len = 0;
        const char* key = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : nullptr;
        if (!key || len == 0 || len > kMaxTableKeyBytes)
            luaL_error(L, "%s: row %d key must be a string of 1 to %d bytes", fn,
                       static_cast<int>(i), static_cast<int>(kMaxTableKeyBytes));
        r.key.assign(key, len);
        lua_pop(L, 1);
        if (!keys.insert(r.key).second)
            luaL_error(L, "%s: row key '%s' appears twice", fn, r.key.c_str());
        const lua_Integer cells = luaL_len(L, row);
        if (static_cast<size_t>(cells) > columns)
            luaL_error(L, "%s: row %d has %d cells for %d columns", fn, static_cast<int>(i),
                       static_cast<int>(cells), static_cast<int>(columns));
        for (lua_Integer c = 1; c <= cells; ++c) {
            lua_rawgeti(L, row, c);
            r.cells.push_back(read_cell(L, -1, static_cast<size_t>(i), static_cast<size_t>(c), fn));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        rows.push_back(std::move(r));
    }
    return rows;
}

int m_set_rows(lua_State* L) {
    forbid_in_check(L, "table:set_rows");
    TableEntry& e = check_table(L);
    const TableModel* m = table_model(e.full);
    const size_t columns = m ? m->columns.size() : 0;
    std::vector<TableRow> rows = read_rows(L, 2, columns, "table:set_rows");
    TableModel next;
    next.columns = m ? m->columns : std::vector<TableColumn>{};
    next.rows = rows;
    next.selected = m ? m->selected : std::string();
    recharge(L, e, entry_bytes(e, &next), "table:set_rows");
    table_set_rows(e.full, std::move(rows));
    return 0;
}

int m_selected(lua_State* L) {
    TableEntry& e = check_table(L);
    const TableModel* m = table_model(e.full);
    if (m && !m->selected.empty())
        lua_pushlstring(L, m->selected.data(), m->selected.size());
    else
        lua_pushnil(L);
    return 1;
}

int m_select(lua_State* L) {
    forbid_in_check(L, "table:select");
    TableEntry& e = check_table(L);
    const char* key = luaL_optstring(L, 2, "");
    lua_pushboolean(L, table_select(e.full, key));
    return 1;
}

int table_new(lua_State* L) {
    const char* fn = "helix.ui.table";
    forbid_in_check(L, fn);
    PluginContext& ctx = context(L);
    const char* local = luaL_checkstring(L, 1);
    if (!is_valid_local_name(local))
        return luaL_error(L, "%s: name '%s' must be 1-48 of [a-z0-9_-]", fn, local);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<TableColumn> columns = read_columns(L, 2, fn);
    lua_getfield(L, 2, "on_activate");
    const bool bad_fn = !lua_isnil(L, -1) && !lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (bad_fn)
        return luaL_error(L, "%s: on_activate must be a function", fn);

    const std::string full = plugin_owned_name(ctx.manifest.id, local);
    TableState* s = table_state(L);
    size_t index = s->tables.size();
    for (size_t i = 0; i < s->tables.size(); ++i) {
        if (s->tables[i]->full == full)
            index = i; // the same table for repeated calls
    }
    TableModel next;
    next.columns = columns;
    if (index == s->tables.size()) {
        auto e = std::make_unique<TableEntry>();
        e->full = full;
        recharge(L, *e, entry_bytes(*e, &next), fn);
        s->tables.push_back(std::move(e));
    } else {
        recharge(L, *s->tables[index], entry_bytes(*s->tables[index], &next), fn);
    }
    TableEntry& e = *s->tables[index];

    // Refs only once every error path is past.
    if (e.activate_ref != LUA_NOREF)
        s->rt->unref(e.activate_ref);
    e.activate_ref = LUA_NOREF;
    lua_getfield(L, 2, "on_activate");
    if (lua_isfunction(L, -1))
        e.activate_ref = s->rt->ref_value(L, -1);
    lua_pop(L, 1);

    table_set_columns(full, std::move(columns));
    if (e.activate_ref != LUA_NOREF) {
        table_set_on_activate(
            full, [rt = s->rt, token = s->token, ref = e.activate_ref](const std::string& key) {
                if (token.expired())
                    return;
                rt->invoke(ref, [key](lua_State* co) {
                    lua_pushlstring(co, key.data(), key.size());
                    return 1;
                });
            });
    } else {
        table_set_on_activate(full, nullptr);
    }

    *static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0)) =
        static_cast<uint32_t>(index);
    luaL_setmetatable(L, kTableMeta);
    return 1;
}

} // namespace

void install_table_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new TableState{&ctx.rt, ctx.rt.token(), {}};
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &g_table_state_key);
    ctx.rt.on_close([state] {
        // An unloaded plugin's tables go empty and stop answering taps.
        for (const auto& e : state->tables) {
            table_drop(e->full);
            state->rt->release_external(e->charged);
        }
        delete state;
    });

    new_locked_metatable(L, kTableMeta);
    lua_newtable(L);
    static const luaL_Reg methods[] = {{"set_rows", &m_set_rows},
                                       {"selected", &m_selected},
                                       {"select", &m_select},
                                       {nullptr, nullptr}};
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_getglobal(L, "helix");
    lua_getfield(L, -1, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "ui");
    }
    lua_pushcfunction(L, &table_new);
    lua_setfield(L, -2, "table");
    lua_pop(L, 2);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
