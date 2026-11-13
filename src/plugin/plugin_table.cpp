// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_table.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/src/indev/lv_indev_private.h" // scroll_limit: no public getter
#include "theme_manager.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace helix::plugin {

size_t TableModel::bytes() const {
    size_t n = sizeof(TableModel) + selected.size();
    for (const auto& c : columns)
        n += sizeof(TableColumn) + c.title.size();
    for (const auto& r : rows) {
        n += sizeof(TableRow) + r.key.size();
        for (const auto& cell : r.cells)
            n += sizeof(std::string) + cell.size();
    }
    return n;
}

namespace {

struct Slot {
    TableModel model;
    bool has_model = false;
    TableActivate on_activate;
    std::vector<lv_obj_t*> instances;
};

/// One live widget's state, freed with it.
struct Instance {
    std::string name;
    lv_point_t press_point{0, 0};
    bool dragged = false; ///< the pointer left the press point by a scroll limit or more
};

std::map<std::string, Slot>& slots() {
    static std::map<std::string, Slot> registry;
    return registry;
}

bool slot_is_empty(const Slot& s) {
    return s.instances.empty() && !s.has_model && !s.on_activate;
}

int selected_index(const TableModel& m) {
    if (m.selected.empty())
        return -1;
    for (size_t i = 0; i < m.rows.size(); ++i) {
        if (m.rows[i].key == m.selected)
            return static_cast<int>(i);
    }
    return -1;
}

void apply_widths(lv_obj_t* obj, const TableModel& m) {
    const auto widths = table_column_widths(m.columns, lv_obj_get_content_width(obj));
    for (size_t c = 0; c < widths.size(); ++c)
        lv_table_set_column_width(obj, static_cast<uint32_t>(c), widths[c]);
}

void render(lv_obj_t* obj, const Slot* s) {
    const TableModel* m = s && s->has_model ? &s->model : nullptr;
    const size_t cols = m ? m->columns.size() : 0;
    if (cols == 0) {
        lv_table_set_column_count(obj, 1);
        lv_table_set_row_count(obj, 1);
        lv_table_set_cell_value(obj, 0, 0, "");
        return;
    }
    lv_table_set_column_count(obj, static_cast<uint32_t>(cols));
    lv_table_set_row_count(obj, static_cast<uint32_t>(m->rows.size() + 1));
    for (size_t c = 0; c < cols; ++c)
        lv_table_set_cell_value(obj, 0, static_cast<uint32_t>(c), m->columns[c].title.c_str());
    for (size_t r = 0; r < m->rows.size(); ++r) {
        const auto& cells = m->rows[r].cells;
        for (size_t c = 0; c < cols; ++c)
            lv_table_set_cell_value(obj, static_cast<uint32_t>(r + 1), static_cast<uint32_t>(c),
                                    c < cells.size() ? cells[c].c_str() : "");
    }
    apply_widths(obj, *m);
}

void render_all(const std::string& name) {
    auto it = slots().find(name);
    if (it == slots().end())
        return;
    for (lv_obj_t* obj : it->second.instances)
        render(obj, &it->second);
}

void invalidate_all(const std::string& name) {
    auto it = slots().find(name);
    if (it == slots().end())
        return;
    for (lv_obj_t* obj : it->second.instances)
        lv_obj_invalidate(obj);
}

Instance* instance_of(lv_event_t* e) {
    return static_cast<Instance*>(lv_event_get_user_data(e));
}

void table_on_size(lv_event_t* e) {
    auto it = slots().find(instance_of(e)->name);
    if (it != slots().end() && it->second.has_model)
        apply_widths(lv_event_get_current_target_obj(e), it->second.model);
}

// lv_table suppresses its release activation only when the drag scrolled it, so a table
// whose rows all fit would activate the last row a drag crossed.
void table_on_press(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
        return;
    Instance* inst = instance_of(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        inst->press_point = p;
        inst->dragged = false;
        return;
    }
    const int32_t limit = indev->scroll_limit;
    if (LV_ABS(p.x - inst->press_point.x) >= limit || LV_ABS(p.y - inst->press_point.y) >= limit)
        inst->dragged = true;
}

bool pointer_dragged(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && instance_of(e)->dragged;
}

void table_on_value_changed(lv_event_t* e) {
    if (pointer_dragged(e))
        return;
    const std::string& name = instance_of(e)->name;
    auto it = slots().find(name);
    if (it == slots().end() || !it->second.has_model)
        return;
    uint32_t row = 0;
    uint32_t col = 0;
    lv_table_get_selected_cell(lv_event_get_current_target_obj(e), &row, &col);
    Slot& s = it->second;
    const int data_row = table_data_row(row, col, s.model.rows.size());
    if (data_row < 0)
        return;
    const std::string key = s.model.rows[static_cast<size_t>(data_row)].key;
    s.model.selected = key;
    invalidate_all(name);
    // Copied first: the handler may replace the model, the handler or drop the table.
    TableActivate fn = s.on_activate;
    if (fn)
        fn(key);
}

// Colors resolve here, on every draw, so a theme switch is the next repaint.
void table_on_draw_task(lv_event_t* e) {
    lv_draw_task_t* task = lv_event_get_draw_task(e);
    auto* base = static_cast<lv_draw_dsc_base_t*>(lv_draw_task_get_draw_dsc(task));
    if (!base || base->part != LV_PART_ITEMS)
        return;
    const uint32_t row = base->id1;
    auto it = slots().find(instance_of(e)->name);
    const int selected = it != slots().end() ? selected_index(it->second.model) : -1;
    switch (lv_draw_task_get_type(task)) {
    case LV_DRAW_TASK_TYPE_LABEL: {
        auto* d = lv_draw_task_get_label_dsc(task);
        d->color = theme_manager_get_color(row == 0 ? "text_muted" : "text");
        break;
    }
    case LV_DRAW_TASK_TYPE_FILL: {
        auto* d = lv_draw_task_get_fill_dsc(task);
        const bool is_selected = row > 0 && static_cast<int>(row) - 1 == selected;
        d->color = theme_manager_get_color("primary");
        d->opa = is_selected ? LV_OPA_30 : LV_OPA_TRANSP;
        break;
    }
    case LV_DRAW_TASK_TYPE_BORDER: {
        auto* d = lv_draw_task_get_border_dsc(task);
        d->color = theme_manager_get_color("border");
        break;
    }
    default:
        break;
    }
}

void table_on_delete(lv_event_t* e) {
    Instance* self = instance_of(e);
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    auto it = slots().find(self->name);
    if (it != slots().end()) {
        auto& inst = it->second.instances;
        inst.erase(std::remove(inst.begin(), inst.end(), obj), inst.end());
        if (slot_is_empty(it->second))
            slots().erase(it);
    }
    delete self;
}

void* table_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    const char* name_attr = nullptr;
    for (int i = 0; attrs && attrs[i]; i += 2) {
        if (strcmp(attrs[i], "name") == 0)
            name_attr = attrs[i + 1];
    }

    // Geometry and the cell face only: every color comes from the draw-task hook. The face
    // is a style because lv_table measures each row in it, header included.
    lv_obj_t* obj = lv_table_create(parent);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    const int32_t pad = theme_manager_get_spacing("space_sm");
    lv_obj_set_style_pad_all(obj, pad, LV_PART_ITEMS);
    lv_obj_set_style_text_font(obj, theme_manager_get_font("font_body"), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(obj, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_send_draw_task_events(obj, true);
    lv_obj_set_scroll_elastic(obj, false);

    // An unnamed table never joins a slot and stays empty.
    auto* inst = new Instance{name_attr ? name_attr : ""};
    Slot* slot = nullptr;
    if (!inst->name.empty()) {
        slot = &slots()[inst->name];
        slot->instances.push_back(obj);
    }
    render(obj, slot);
    lv_obj_add_event_cb(obj, table_on_size, LV_EVENT_SIZE_CHANGED, inst);
    lv_obj_add_event_cb(obj, table_on_press, LV_EVENT_PRESSED, inst);
    lv_obj_add_event_cb(obj, table_on_press, LV_EVENT_PRESSING, inst);
    lv_obj_add_event_cb(obj, table_on_value_changed, LV_EVENT_VALUE_CHANGED, inst);
    lv_obj_add_event_cb(obj, table_on_draw_task, LV_EVENT_DRAW_TASK_ADDED, inst);
    lv_obj_add_event_cb(obj, table_on_delete, LV_EVENT_DELETE, inst);
    return obj;
}

} // namespace

int table_data_row(uint32_t row, uint32_t col, size_t data_rows) {
    if (row == LV_TABLE_CELL_NONE || col == LV_TABLE_CELL_NONE || row == 0 || row > data_rows)
        return -1;
    return static_cast<int>(row) - 1;
}

std::vector<int32_t> table_column_widths(const std::vector<TableColumn>& columns,
                                         int32_t content_w) {
    std::vector<int32_t> out(columns.size(), 0);
    if (columns.empty())
        return out;
    const int32_t w = std::max<int32_t>(0, content_w);
    int32_t used = 0;
    size_t flexible = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].width_pct > 0) {
            out[i] = w * columns[i].width_pct / 100;
            used += out[i];
        } else {
            ++flexible;
        }
    }
    if (flexible > 0) {
        const int32_t rest = std::max<int32_t>(0, w - used);
        for (size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].width_pct <= 0)
                out[i] = rest / static_cast<int32_t>(flexible);
        }
    }
    // A column never collapses to nothing: lv_table draws a zero-width column as a sliver.
    for (int32_t& v : out)
        v = std::max<int32_t>(v, 1);
    return out;
}

void register_plugin_table_widget() {
    lv_xml_register_widget("plugin_table", table_xml_create, lv_xml_obj_apply);
}

void table_set_columns(const std::string& name, std::vector<TableColumn> columns) {
    Slot& s = slots()[name];
    s.model.columns = std::move(columns);
    s.model.rows.clear();
    s.model.selected.clear();
    s.has_model = true;
    render_all(name);
}

void table_set_rows(const std::string& name, std::vector<TableRow> rows) {
    Slot& s = slots()[name];
    s.model.rows = std::move(rows);
    s.has_model = true;
    if (selected_index(s.model) < 0)
        s.model.selected.clear();
    render_all(name);
}

void table_set_on_activate(const std::string& name, TableActivate fn) {
    Slot& s = slots()[name];
    s.on_activate = std::move(fn);
    if (slot_is_empty(s))
        slots().erase(name);
}

bool table_select(const std::string& name, const std::string& key) {
    auto it = slots().find(name);
    if (it == slots().end())
        return key.empty();
    TableModel& m = it->second.model;
    if (key.empty()) {
        m.selected.clear();
    } else {
        const std::string previous = m.selected;
        m.selected = key;
        if (selected_index(m) < 0) {
            m.selected = previous;
            return false;
        }
    }
    invalidate_all(name);
    return true;
}

const TableModel* table_model(const std::string& name) {
    auto it = slots().find(name);
    return it != slots().end() && it->second.has_model ? &it->second.model : nullptr;
}

void table_drop(const std::string& name) {
    auto it = slots().find(name);
    if (it == slots().end())
        return;
    it->second.model = TableModel{};
    it->second.has_model = false;
    it->second.on_activate = nullptr;
    render_all(name);
    if (slot_is_empty(it->second))
        slots().erase(it);
}

size_t table_instance_count(const std::string& name) {
    auto it = slots().find(name);
    return it == slots().end() ? 0 : it->second.instances.size();
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
