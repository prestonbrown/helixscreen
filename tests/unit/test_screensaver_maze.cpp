// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "app_globals.h"
#include "print_history_data.h"
#include "printer_state.h"
#include "screensaver_fast_math.h"
#include "screensaver_maze.h"
#include "screensaver_maze_sim.h"
#include "screensaver_pixel_writer.h"
#include "thumbnail_cache.h"
#include "thumbnail_rules.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::kMazeLevels;
using helix::ui::MazeArt;
using helix::ui::MazeItem;
using helix::ui::MazeItemKind;
using helix::ui::MazeMap;
using helix::ui::MazePose;
using helix::ui::MazeRenderer;
using helix::ui::MazeScreensaver;
using helix::ui::MazeSim;
using helix::ui::MazeTile;
using helix::ui::MazeWalker;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;
using helix::ui::WalkEvent;

namespace {

using helix::test::frame_fnv1a;
using helix::test::TestFrame;

/// Cells reachable from cell (0, 0) through open passages.
int reachable_cells(const MazeMap& map) {
    const int n = map.cells();
    std::vector<bool> seen(static_cast<size_t>(n * n), false);
    std::deque<int> todo{0};
    seen[0] = true;
    int count = 0;
    while (!todo.empty()) {
        const int c = todo.front();
        todo.pop_front();
        count++;
        for (int d = 0; d < 4; d++) {
            if (!map.passage(c % n, c / n, d)) {
                continue;
            }
            const int nx = c % n + helix::ui::kHeadingDx[d];
            const int ny = c / n + helix::ui::kHeadingDy[d];
            const int next = ny * n + nx;
            if (!seen[static_cast<size_t>(next)]) {
                seen[static_cast<size_t>(next)] = true;
                todo.push_back(next);
            }
        }
    }
    return count;
}

/// Open passages between cells, each counted once.
int open_edges(const MazeMap& map) {
    int edges = 0;
    for (int cy = 0; cy < map.cells(); cy++) {
        for (int cx = 0; cx < map.cells(); cx++) {
            edges += map.passage(cx, cy, 0) ? 1 : 0;
            edges += map.passage(cx, cy, 1) ? 1 : 0;
        }
    }
    return edges;
}

/// Walks until the middle of every cell has been passed, or `limit` moves; returns the moves
/// taken, or -1 when some cell was never reached.
int walk_all(MazeWalker& walker, const MazeMap& map, uint32_t limit) {
    const int n = map.cells();
    std::vector<bool> seen(static_cast<size_t>(n * n), false);
    seen[static_cast<size_t>(walker.cell_y() * n + walker.cell_x())] = true;
    int unseen = n * n - 1;
    while (walker.moves() <= limit) {
        WalkEvent event = WalkEvent::NONE;
        walker.advance(map, 50, event);
        if (event != WalkEvent::ARRIVED) {
            continue;
        }
        const auto i = static_cast<size_t>(walker.cell_y() * n + walker.cell_x());
        if (!seen[i]) {
            seen[i] = true;
            if (--unseen == 0) {
                return static_cast<int>(walker.moves());
            }
        }
    }
    return -1;
}

/// Open passages out of cell (cx, cy).
int passages(const MazeMap& map, int cx, int cy) {
    int count = 0;
    for (int d = 0; d < 4; d++) {
        count += map.passage(cx, cy, d) ? 1 : 0;
    }
    return count;
}

/// A dead-end cell of `map` and the heading out of it.
std::pair<int, int> dead_end(const MazeMap& map, int& heading) {
    for (int cy = 0; cy < map.cells(); cy++) {
        for (int cx = 0; cx < map.cells(); cx++) {
            if (passages(map, cx, cy) != 1) {
                continue;
            }
            for (heading = 0; !map.passage(cx, cy, heading); heading++) {
            }
            return {cx, cy};
        }
    }
    heading = 0;
    return {0, 0};
}

/// A w x h image of one colour.
helix::ui::MazeImage solid(int w, int h, uint32_t argb) {
    helix::ui::MazeImage img;
    img.w = w;
    img.h = h;
    img.argb.assign(static_cast<size_t>(w * h), argb);
    return img;
}

/// room(15) with a 7 x 7 brick island in its middle (tiles 4..10): cells 2..4 square are
/// inside it, and the ring of cells round it circles it.
MazeMap island_room() {
    const int size = 15;
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            const bool island = x >= 4 && x <= 10 && y >= 4 && y <= 10;
            tiles[static_cast<size_t>(y * size + x)] = island ? MazeTile::BRICK : MazeTile::OPEN;
        }
    }
    MazeMap map;
    map.set_tiles(size, tiles);
    return map;
}

/// Advances in 50 ms steps until the walker stops or passes `limit` moves.
void walk_to_stop(MazeWalker& walker, const MazeMap& map, uint32_t limit) {
    while (!walker.stopped() && walker.moves() <= limit) {
        WalkEvent event = WalkEvent::NONE;
        walker.advance(map, 50, event);
    }
}

/// A started scene's map with its start and exit cells, as cy * cells + cx.
struct OpenedMaze {
    MazeSim sim;
    int start = -1;
    int exit = -1;

    explicit OpenedMaze(uint32_t seed) {
        std::minstd_rand rng(seed);
        TestFrame frame(8, 8, PixelFormat::XRGB8888);
        sim.init(frame.target, rng, 4, MazeArt{});
        const int n = sim.map().cells();
        start = sim.walker().cell_y() * n + sim.walker().cell_x();
        for (const MazeItem& item : sim.items()) {
            if (item.kind == MazeItemKind::EXIT) {
                exit = static_cast<int>(item.y) / 2 * n + static_cast<int>(item.x) / 2;
            }
        }
    }
};

/// Every tile of the outer ring is a wall.
void check_closed_outside(const MazeMap& map) {
    for (int i = 0; i < map.size(); i++) {
        CHECK(map.wall(i, 0));
        CHECK(map.wall(i, map.size() - 1));
        CHECK(map.wall(0, i));
        CHECK(map.wall(map.size() - 1, i));
    }
}

/// A corridor running east: open tiles (1..size-2, mid), brick elsewhere.
MazeMap corridor(int size) {
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    const int mid = size / 2;
    for (int x = 1; x < size - 1; x++) {
        tiles[static_cast<size_t>(mid * size + x)] = MazeTile::OPEN;
    }
    MazeMap map;
    map.set_tiles(size, tiles);
    return map;
}

/// An open room: every tile open but the outer ring.
MazeMap room(int size) {
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            tiles[static_cast<size_t>(y * size + x)] = MazeTile::OPEN;
        }
    }
    MazeMap map;
    map.set_tiles(size, tiles);
    return map;
}

} // namespace

TEST_CASE("a generated maze is perfect, closed outside, with no islands and 16 pictures on walls",
          "[screensaver][maze]") {
    const int cells = MazeSim::CELLS * MazeSim::CELLS;
    for (uint32_t seed = 1; seed <= 50; seed++) {
        INFO("seed " << seed);
        std::minstd_rand rng(seed);
        MazeMap map;
        map.generate(MazeSim::CELLS, rng);
        REQUIRE(map.size() == 2 * MazeSim::CELLS + 1);
        // Perfect: every cell reachable through exactly cells - 1 passages.
        CHECK(reachable_cells(map) == cells);
        CHECK(open_edges(map) == cells - 1);
        check_closed_outside(map);
        // Pictures hang only on walls, indices 0..15 once each; every wall
        // reaches the outside.
        std::vector<int> seen(MazeMap::kPictureWalls, 0);
        for (int y = 0; y < map.size(); y++) {
            for (int x = 0; x < map.size(); x++) {
                if (map.wall(x, y)) {
                    REQUIRE(map.boundary_wall(x, y));
                }
                if (map.tile(x, y) == MazeTile::PICTURE) {
                    CHECK(map.wall(x, y));
                    const int i = map.picture_index(x, y);
                    REQUIRE(i < MazeMap::kPictureWalls);
                    seen[static_cast<size_t>(i)]++;
                } else {
                    CHECK(map.picture_index(x, y) == 0);
                }
            }
        }
        for (int i = 0; i < MazeMap::kPictureWalls; i++) {
            CHECK(seen[static_cast<size_t>(i)] == 1);
        }
        CHECK(map.tile(-1, 3) == MazeTile::BRICK);
        CHECK(map.tile(map.size(), 3) == MazeTile::BRICK);
    }
}

TEST_CASE("an island is not on the boundary", "[screensaver][maze]") {
    const int size = 15;
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            const bool island = x >= 4 && x <= 10 && y >= 4 && y <= 10;
            tiles[static_cast<size_t>(y * size + x)] = island ? MazeTile::BRICK : MazeTile::OPEN;
        }
    }
    MazeMap map;
    map.set_tiles(size, tiles);
    CHECK_FALSE(map.boundary_wall(4, 4));
    CHECK_FALSE(map.boundary_wall(10, 10));
    CHECK(map.boundary_wall(0, 7));
    CHECK(map.boundary_wall(-1, 3));
    CHECK_FALSE(map.boundary_wall(7, 1));
}

TEST_CASE("set_tiles carries picture indices", "[screensaver][maze]") {
    const int size = 15;
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    std::vector<uint8_t> pictures(tiles.size(), 0);
    tiles[static_cast<size_t>(3 * size + 2)] = MazeTile::PICTURE;
    pictures[static_cast<size_t>(3 * size + 2)] = 3;
    tiles[static_cast<size_t>(6 * size + 4)] = MazeTile::PICTURE;
    pictures[static_cast<size_t>(6 * size + 4)] = 7;
    MazeMap map;
    map.set_tiles(size, tiles, pictures);
    CHECK(map.picture_index(2, 3) == 3);
    CHECK(map.picture_index(4, 6) == 7);
    CHECK(map.picture_index(-1, 6) == 0);
    MazeMap bare;
    bare.set_tiles(size, tiles);
    CHECK(bare.picture_index(2, 3) == 0);
    CHECK(bare.picture_index(4, 6) == 0);
}

TEST_CASE("an opened maze keeps every cell, has loops and rooms, walled dead ends and placed items",
          "[screensaver][maze]") {
    const int cells = MazeSim::CELLS * MazeSim::CELLS;
    int extra_total = 0;
    for (uint32_t seed = 1; seed <= 200; seed++) {
        INFO("seed " << seed);
        OpenedMaze m(seed);
        const MazeMap& map = m.sim.map();
        const int n = map.cells();
        CHECK(reachable_cells(map) == cells);
        check_closed_outside(map);
        const int extra = open_edges(map) - (cells - 1);
        CHECK(extra >= MazeMap::kExtraOpenings);
        extra_total += extra;

        // 2 or 3 rooms: a 2x2 room opens the one pillar inside it, a 2x3 room
        // two; nothing else opens one.
        int pillars = 0;
        for (int y = 2; y < map.size() - 1; y += 2) {
            for (int x = 2; x < map.size() - 1; x += 2) {
                pillars += map.wall(x, y) ? 0 : 1;
            }
        }
        CHECK(pillars >= 2);
        CHECK(pillars <= 6);
        int open_blocks = 0;
        for (int cy = 0; cy + 1 < n; cy++) {
            for (int cx = 0; cx + 1 < n; cx++) {
                open_blocks += map.passage(cx, cy, 0) && map.passage(cx, cy, 1) &&
                                       map.passage(cx + 1, cy + 1, 2) &&
                                       map.passage(cx + 1, cy + 1, 3)
                                   ? 1
                                   : 0;
            }
        }
        CHECK(open_blocks >= 2);

        // Start and exit stay dead ends walled by the boundary.
        REQUIRE(m.exit >= 0);
        REQUIRE(m.start != m.exit);
        for (int c : {m.start, m.exit}) {
            CHECK(passages(map, c % n, c / n) == 1);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int x = 2 * (c % n) + 1 + dx;
                    const int y = 2 * (c / n) + 1 + dy;
                    if (map.wall(x, y)) {
                        CHECK(map.boundary_wall(x, y));
                    }
                }
            }
        }

        // Items stand in open cells; logos and rocks each in a cell of their own.
        std::vector<int> used(static_cast<size_t>(n * n), 0);
        int counts[5] = {};
        for (const MazeItem& item : m.sim.items()) {
            counts[static_cast<int>(item.kind)]++;
            CHECK_FALSE(map.wall(static_cast<int>(item.x), static_cast<int>(item.y)));
            if (item.kind != MazeItemKind::LOGO && item.kind != MazeItemKind::ROCK) {
                continue;
            }
            const int c = static_cast<int>(item.y) / 2 * n + static_cast<int>(item.x) / 2;
            CHECK(c != m.start);
            CHECK(c != m.exit);
            CHECK(used[static_cast<size_t>(c)]++ == 0);
        }
        CHECK(counts[static_cast<int>(MazeItemKind::START)] == 1);
        CHECK(counts[static_cast<int>(MazeItemKind::EXIT)] == 1);
        CHECK(counts[static_cast<int>(MazeItemKind::LOGO)] == MazeSim::kLogoCount);
        CHECK(counts[static_cast<int>(MazeItemKind::ROCK)] == MazeSim::kRockCount);
        CHECK(counts[static_cast<int>(MazeItemKind::TOOLHEAD)] == 1);
    }
    CHECK(extra_total >= 10 * 200);
}

TEST_CASE("the wall follower passes every cell of a perfect maze within the step bound",
          "[screensaver][maze]") {
    const uint32_t bound = 4u * MazeSim::CELLS * MazeSim::CELLS;
    for (uint32_t seed = 1; seed <= 50; seed++) {
        std::minstd_rand rng(seed);
        MazeMap map;
        map.generate(MazeSim::CELLS, rng);
        for (bool left : {false, true}) {
            MazeWalker walker;
            walker.reset(0, 0, 0, left);
            INFO("seed " << seed << (left ? " left" : " right"));
            CHECK(walk_all(walker, map, bound) > 0);
        }
    }
}

TEST_CASE("a walker latched onto an island walks off it and reaches the exit",
          "[screensaver][maze]") {
    const MazeMap map = island_room();
    MazeWalker w;
    // Right hand on the island's west face, heading north.
    w.reset(1, 3, 3, false);
    w.set_stop_cell(0, 0);
    walk_to_stop(w, map, 12 * 49);
    CHECK(w.stopped());
    CHECK(w.recoveries() == 1);
}

TEST_CASE("a hand swap forgets the states seen", "[screensaver][maze]") {
    const MazeMap map = island_room();
    MazeWalker w;
    w.reset(1, 3, 3, false);
    w.set_stop_cell(0, 0);
    walk_to_stop(w, map, 5);
    REQUIRE(w.recoveries() == 0);
    w.set_left_handed(true);
    walk_to_stop(w, map, 12 * 49);
    CHECK(w.stopped());
    CHECK(w.cell_x() == 0);
    CHECK(w.cell_y() == 0);
    CHECK(w.recoveries() <= 1);
}

TEST_CASE("a walker never recovers on a perfect maze", "[screensaver][maze]") {
    for (uint32_t seed = 1; seed <= 50; seed++) {
        std::minstd_rand rng(seed);
        MazeMap map;
        map.generate(MazeSim::CELLS, rng);
        int heading = 0;
        const auto [sx, sy] = dead_end(map, heading);
        // The farthest cell from the start.
        const int n = map.cells();
        std::vector<int> dist(static_cast<size_t>(n * n), -1);
        std::deque<int> todo{sy * n + sx};
        dist[static_cast<size_t>(sy * n + sx)] = 0;
        int far = sy * n + sx;
        while (!todo.empty()) {
            const int c = todo.front();
            todo.pop_front();
            far = dist[static_cast<size_t>(c)] > dist[static_cast<size_t>(far)] ? c : far;
            for (int d = 0; d < 4; d++) {
                const int next =
                    (c / n + helix::ui::kHeadingDy[d]) * n + c % n + helix::ui::kHeadingDx[d];
                if (map.passage(c % n, c / n, d) && dist[static_cast<size_t>(next)] < 0) {
                    dist[static_cast<size_t>(next)] = dist[static_cast<size_t>(c)] + 1;
                    todo.push_back(next);
                }
            }
        }
        for (bool left : {false, true}) {
            INFO("seed " << seed << (left ? " left" : " right"));
            MazeWalker w;
            w.reset(sx, sy, heading, left);
            w.set_stop_cell(far % n, far / n);
            walk_to_stop(w, map, 4u * MazeSim::CELLS * MazeSim::CELLS);
            CHECK(w.stopped());
            CHECK(w.recoveries() == 0);
        }
    }
}

TEST_CASE("every opened maze reaches its exit, rocks and all", "[screensaver][maze]") {
    for (uint32_t seed = 1; seed <= 100; seed++) {
        INFO("seed " << seed);
        std::minstd_rand rng(seed);
        MazeSim sim;
        TestFrame frame(16, 12, PixelFormat::XRGB8888);
        sim.init(frame.target, rng, 4, MazeArt{});
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 60000 && sim.exits_reached() == 0 && sim.abandoned() == 0; i++) {
            sim.step(100, frame.target, rng, dirty);
        }
        CHECK(sim.exits_reached() == 1);
        CHECK(sim.abandoned() == 0);
        CHECK(sim.walker().moves() <= sim.step_bound());
        if (seed == 1) {
            // A new run counts from zero.
            sim.init(frame.target, rng, 4, MazeArt{});
            CHECK(sim.exits_reached() == 0);
        }
    }
    // Per hand (the first and one per rock): an island lap, the way off and a boundary lap,
    // each at most every (cell, heading) state once.
    MazeSim sim;
    CHECK(sim.step_bound() ==
          (MazeSim::kRockCount + 1) * 3u * 4u * MazeSim::CELLS * MazeSim::CELLS);
}

TEST_CASE("extra hand swaps still reach the exit", "[screensaver][maze]") {
    for (uint32_t seed = 1; seed <= 100; seed++) {
        INFO("seed " << seed);
        OpenedMaze m(seed);
        const MazeMap& map = m.sim.map();
        const int n = map.cells();
        int heading = 0;
        while (!map.passage(m.start % n, m.start / n, heading)) {
            heading++;
        }
        MazeWalker w;
        w.reset(m.start % n, m.start / n, heading, false);
        w.set_stop_cell(m.exit % n, m.exit / n);
        for (uint32_t swap : {17u, 34u, 51u}) {
            walk_to_stop(w, map, swap - 1);
            w.set_left_handed(!w.left_handed());
        }
        walk_to_stop(w, map, 4u * 12u * 144u);
        CHECK(w.stopped());
    }
}

TEST_CASE("a walker that swaps hands mid-walk still passes every cell", "[screensaver][maze]") {
    std::minstd_rand rng(11);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    MazeWalker walker;
    walker.reset(0, 0, 0, false);
    while (walker.moves() < 10) {
        WalkEvent event = WalkEvent::NONE;
        walker.advance(map, 50, event);
    }
    walker.set_left_handed(true);
    CHECK(walker.left_handed());
    CHECK(walk_all(walker, map, 8u * MazeSim::CELLS * MazeSim::CELLS) > 0);
}

TEST_CASE("the walk is a function of time: two short advances equal one long one",
          "[screensaver][maze]") {
    std::minstd_rand rng(3);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    MazeWalker a;
    MazeWalker b;
    a.reset(0, 0, 0, false);
    b.reset(0, 0, 0, false);
    for (int i = 0; i < 200; i++) {
        WalkEvent event = WalkEvent::NONE;
        uint32_t left = 32;
        while (left > 0) {
            left -= a.advance(map, left, event);
        }
        for (int half = 0; half < 2; half++) {
            uint32_t part = 16;
            while (part > 0) {
                part -= b.advance(map, part, event);
            }
        }
        REQUIRE(a.pose().x == Catch::Approx(b.pose().x).margin(1e-4));
        REQUIRE(a.pose().y == Catch::Approx(b.pose().y).margin(1e-4));
        REQUIRE(a.pose().angle == Catch::Approx(b.pose().angle).margin(1e-4));
    }
    CHECK(a.moves() > 0);
}

TEST_CASE("a walker stops at each doorway and each cell middle so the caller sees both",
          "[screensaver][maze]") {
    // Cell row 2 is tile row 5, the corridor's open row.
    MazeMap map = corridor(11);
    MazeWalker walker;
    walker.reset(0, 2, 0, false);
    WalkEvent event = WalkEvent::NONE;

    // From the middle of the first cell, speeding up to the doorway: one tile.
    uint32_t used = walker.advance(map, 100000, event);
    CHECK(event == WalkEvent::ENTERED);
    CHECK(used == MazeWalker::kDefaultMoveMs);
    CHECK(walker.cell_x() == 1);
    CHECK(walker.moves() == 0);
    CHECK(walker.pose().x == Catch::Approx(2.5f));

    // Then at walking pace, two tiles per move, to the middle of the next.
    used = walker.advance(map, 100000, event);
    CHECK(event == WalkEvent::ARRIVED);
    CHECK(used == MazeWalker::kDefaultMoveMs / 2);
    CHECK(walker.moves() == 1);
    CHECK(walker.pose().x == Catch::Approx(3.5f).margin(0.01f));
}

TEST_CASE("the walk keeps its distance from the wall ahead outside dead ends",
          "[screensaver][maze]") {
    // At walking pace through a cell, straight or round the inner corner, the nearest wall
    // straight ahead is never closer than about three quarters of a tile.
    for (uint32_t seed = 1; seed <= 20; seed++) {
        std::minstd_rand rng(seed);
        MazeMap map;
        map.generate(MazeSim::CELLS, rng);
        int heading = 0;
        const auto [sx, sy] = dead_end(map, heading);
        MazeWalker walker;
        walker.reset(sx, sy, heading, (seed & 1) != 0);
        MazeRenderer r;
        TestFrame frame(32, 20, PixelFormat::XRGB8888);
        r.init(32, 20, PixelFormat::XRGB8888, MazeArt{});
        size_t checked = 0;
        while (walker.moves() < 2u * MazeSim::CELLS * MazeSim::CELLS) {
            uint32_t left = 80;
            while (left > 0) {
                WalkEvent event = WalkEvent::NONE;
                left -= walker.advance(map, left, event);
            }
            if (passages(map, walker.cell_x(), walker.cell_y()) == 1) {
                continue;
            }
            r.render(map, {}, walker.pose(), 0.0f, 0, frame.target);
            INFO("seed " << seed << " cell " << walker.cell_x() << "," << walker.cell_y());
            REQUIRE(r.wall_distance(r.internal_w() / 2) >= 0.75f);
            checked++;
        }
        CHECK(checked > 100);
    }
}

TEST_CASE("a wall straight ahead has the slice height its distance gives, with no fisheye",
          "[screensaver][maze]") {
    // A room, facing the east wall from 3.5 tiles off: the wall face is at x = size - 1.
    const int size = 15;
    MazeMap map = room(size);
    MazeRenderer r;
    TestFrame frame(160, 100, PixelFormat::XRGB8888);
    r.init(frame.target.w, frame.target.h, PixelFormat::XRGB8888, MazeArt{});
    r.set_level(0);
    MazePose pose;
    pose.x = static_cast<float>(size - 1) - 3.5f;
    pose.y = size * 0.5f;
    pose.angle = 0.0f;
    r.render(map, {}, pose, 0.0f, 0, frame.target);

    const uint32_t mid = r.internal_w() / 2;
    CHECK(r.wall_distance(mid) == Catch::Approx(3.5f).margin(0.01f));
    const float focal = helix::ui::maze_focal(r.internal_w(), r.internal_h());
    const int expected = static_cast<int>(focal / 3.5f);
    REQUIRE(expected == 14);
    CHECK(std::abs((r.wall_bottom(mid) - r.wall_top(mid)) - expected) <= 1);
    // Its top is where the focal length puts it.
    CHECK(r.wall_top(mid) ==
          static_cast<int>(std::floor((static_cast<float>(r.internal_h()) - focal / 3.5f) * 0.5f)));
    // A flat wall square to the view stands the same height across every column that hits it.
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        INFO("column " << x);
        CHECK(r.wall_distance(x) == Catch::Approx(3.5f).margin(0.01f));
    }
}

TEST_CASE("the maze sees 90 degrees vertically, capped at 120 across", "[screensaver][maze]") {
    using helix::ui::maze_focal;
    CHECK(maze_focal(800, 480) == Catch::Approx(240.0f));
    CHECK(maze_focal(400, 240) == Catch::Approx(120.0f));
    // The horizontal cap binds on a wide frame.
    CHECK(maze_focal(480, 272) == Catch::Approx(138.564f).margin(0.01f));
    CHECK(maze_focal(1920, 440) == Catch::Approx(554.256f).margin(0.01f));
    CHECK(maze_focal(480, 800) == Catch::Approx(400.0f));
    CHECK(maze_focal(1, 1) == Catch::Approx(0.5f));
}

TEST_CASE("a sprite stands the height its distance gives", "[screensaver][maze]") {
    MazeMap map = corridor(11);
    MazeArt art;
    art.logo.w = 8;
    art.logo.h = 8;
    art.logo.argb.assign(64, 0xFFFFFFFFu);
    MazeRenderer r;
    TestFrame frame(160, 100, PixelFormat::XRGB8888);
    r.init(160, 100, PixelFormat::XRGB8888, art);
    const MazePose pose{1.5f, 5.5f, 0.0f};
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    const uint32_t mid = r.internal_w() / 2;
    std::vector<uint32_t> bare(r.internal_h());
    for (uint32_t y = 0; y < r.internal_h(); y++) {
        bare[y] = r.internal_pixel(mid, y);
    }
    std::vector<MazeItem> logo{{MazeItemKind::LOGO, 5.5f, 5.5f, true}};
    r.render(map, logo, pose, 0.0f, 0, frame.target);
    int first = -1;
    int last = -1;
    for (uint32_t y = 0; y < r.internal_h(); y++) {
        if (r.internal_pixel(mid, y) != bare[y]) {
            first = first < 0 ? static_cast<int>(y) : first;
            last = static_cast<int>(y);
        }
    }
    REQUIRE(first >= 0);
    const float focal = helix::ui::maze_focal(r.internal_w(), r.internal_h());
    // The logo fills 60 of its 64 texels, 0.55 tiles tall, 4 tiles away.
    CHECK(std::abs((last - first + 1) - 0.55f * focal / 4.0f) <= 1.0f);
}

namespace {

/// Mean column and row of the internal pixels a single item changes, 160x100, from (1.5, 5.5)
/// facing east in an open room.
std::pair<float, float> item_centre(const MazeItem& item) {
    MazeMap map = room(15);
    MazeArt art;
    art.logo.w = 8;
    art.logo.h = 8;
    art.logo.argb.assign(64, 0xFFFFFFFFu);
    MazeRenderer r;
    TestFrame frame(160, 100, PixelFormat::XRGB8888);
    r.init(160, 100, PixelFormat::XRGB8888, art);
    const MazePose pose{1.5f, 7.5f, 0.0f};
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    std::vector<uint32_t> bare(r.internal_w() * r.internal_h());
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        for (uint32_t y = 0; y < r.internal_h(); y++) {
            bare[x * r.internal_h() + y] = r.internal_pixel(x, y);
        }
    }
    r.render(map, {item}, pose, 0.0f, 0, frame.target);
    double sx = 0.0;
    double sy = 0.0;
    size_t n = 0;
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        for (uint32_t y = 0; y < r.internal_h(); y++) {
            if (r.internal_pixel(x, y) != bare[x * r.internal_h() + y]) {
                sx += x + 0.5;
                sy += y + 0.5;
                n++;
            }
        }
    }
    REQUIRE(n > 0);
    return {static_cast<float>(sx / n), static_cast<float>(sy / n)};
}

} // namespace

TEST_CASE("an item stands where the focal length puts it, beside or below the centre",
          "[screensaver][maze]") {
    const float focal = helix::ui::maze_focal(160, 100);
    // A logo 4 tiles ahead and 1 to the right: focal * 1 / 4 right of centre.
    const auto [lx, ly] = item_centre({MazeItemKind::LOGO, 5.5f, 8.5f, true});
    CHECK(lx == Catch::Approx(80.0f + focal / 4.0f).margin(1.0f));
    CHECK(ly == Catch::Approx(50.0f).margin(1.0f));
    // The toolhead's centre is 0.175 tiles up, 0.325 below the eye, 4 tiles ahead.
    const auto [tx, ty] = item_centre({MazeItemKind::TOOLHEAD, 5.5f, 7.5f, true});
    CHECK(tx == Catch::Approx(80.0f).margin(1.0f));
    CHECK(ty == Catch::Approx(50.0f + 0.325f * focal / 4.0f).margin(1.0f));
}

TEST_CASE("the fog curve and blend", "[screensaver][maze]") {
    using helix::ui::maze_fog;
    using helix::ui::maze_fog_k;
    using helix::ui::maze_fog_slot_depth;
    // Clearly darker with distance: a third gone by 3 tiles, about 60% by 6.
    CHECK(maze_fog_k(0.25f) == 32);
    CHECK(maze_fog_k(0.5f) == 32);
    CHECK(maze_fog_k(1.0f) == 29);
    CHECK(maze_fog_k(3.0f) == 21);
    CHECK(maze_fog_k(6.0f) == 13);
    CHECK(maze_fog_k(12.0f) == 6);
    CHECK(maze_fog_k(100.0f) == 3);
    for (float d = 0.0f; d < 40.0f; d += 0.25f) {
        INFO("depth " << d);
        CHECK(maze_fog_k(d + 0.25f) <= maze_fog_k(d));
    }
    CHECK(maze_fog({200, 100, 50}, 32) == Rgb{200, 100, 50});
    CHECK(maze_fog({255, 255, 255}, 15) == Rgb{123, 123, 125});
    CHECK(maze_fog({0, 0, 0}, 5) == Rgb{5, 5, 8});
    CHECK(maze_fog_slot_depth(3.5f) == Catch::Approx(3.625f));
    CHECK(maze_fog_slot_depth(1000.0f) == Catch::Approx(31.875f));
    CHECK(maze_fog_slot_depth(-2.0f) == Catch::Approx(0.125f));
}

TEST_CASE("a wall's fog follows its distance, darker on the y side", "[screensaver][maze]") {
    using helix::ui::maze_fog_k;
    using helix::ui::maze_fog_slot_depth;
    const int size = 15;
    MazeMap map = room(size);
    MazeRenderer r;
    TestFrame frame(160, 100, PixelFormat::XRGB8888);
    r.init(160, 100, PixelFormat::XRGB8888, MazeArt{});
    const uint32_t k = maze_fog_k(maze_fog_slot_depth(3.5f));
    REQUIRE(k < 32);
    const uint32_t mid = r.internal_w() / 2;
    r.render(map, {}, MazePose{static_cast<float>(size - 1) - 3.5f, size * 0.5f, 0.0f}, 0.0f, 0,
             frame.target);
    CHECK(r.wall_fog_k(mid) == k);
    // Facing north, 3.5 tiles from the north wall's face at y = 1.
    r.render(map, {}, MazePose{size * 0.5f, 4.5f, -helix::ui::kSaverPi * 0.5f}, 0.0f, 0,
             frame.target);
    REQUIRE(r.wall_distance(mid) == Catch::Approx(3.5f).margin(0.01f));
    CHECK(r.wall_fog_k(mid) == (k * helix::ui::kMazeSideK + 16) >> 5);
}

TEST_CASE("a picture wall is drawn at its fogged colour", "[screensaver][maze]") {
    using helix::ui::maze_fog;
    using helix::ui::maze_fog_k;
    using helix::ui::maze_fog_slot_depth;
    MazeArt art;
    art.pictures = {solid(4, 4, 0xFF00FF00u)};
    const int size = 15;
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            tiles[static_cast<size_t>(y * size + x)] = MazeTile::OPEN;
        }
    }
    tiles[static_cast<size_t>((size / 2) * size + size - 1)] = MazeTile::PICTURE;
    MazeMap map;
    map.set_tiles(size, tiles);
    MazeRenderer r;
    TestFrame frame(160, 100, PixelFormat::XRGB8888);
    r.init(160, 100, PixelFormat::XRGB8888, art, 0);
    r.render(map, {}, MazePose{static_cast<float>(size - 1) - 3.5f, size * 0.5f, 0.0f}, 0.0f, 0,
             frame.target);
    const Rgb expected = maze_fog({0, 255, 0}, maze_fog_k(maze_fog_slot_depth(3.5f)));
    REQUIRE(expected.g < 255);
    CHECK(r.internal_pixel(r.internal_w() / 2, r.internal_h() / 2) ==
          PixelWriter::pack(expected, PixelFormat::XRGB8888));
}

TEST_CASE("a sprite takes the fog of its distance", "[screensaver][maze]") {
    using helix::ui::maze_fog;
    using helix::ui::maze_fog_k;
    using helix::ui::maze_fog_slot_depth;
    MazeMap map = corridor(11);
    MazeArt art;
    art.logo.w = 8;
    art.logo.h = 8;
    art.logo.argb.assign(64, 0xFFFFFFFFu);
    MazeRenderer r;
    TestFrame frame(160, 100, PixelFormat::XRGB8888);
    r.init(160, 100, PixelFormat::XRGB8888, art);
    const MazePose pose{1.5f, 5.5f, 0.0f};
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    std::vector<uint32_t> bare(r.internal_w() * r.internal_h());
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        for (uint32_t y = 0; y < r.internal_h(); y++) {
            bare[x * r.internal_h() + y] = r.internal_pixel(x, y);
        }
    }
    std::vector<MazeItem> logo{{MazeItemKind::LOGO, 5.5f, 5.5f, true}};
    r.render(map, logo, pose, 0.0f, 0, frame.target);
    const uint32_t expected = PixelWriter::pack(
        maze_fog({255, 255, 255}, maze_fog_k(maze_fog_slot_depth(4.0f))), PixelFormat::XRGB8888);
    REQUIRE(expected != PixelWriter::pack({255, 255, 255}, PixelFormat::XRGB8888));
    size_t changed = 0;
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        for (uint32_t y = 0; y < r.internal_h(); y++) {
            const uint32_t px = r.internal_pixel(x, y);
            if (px != bare[x * r.internal_h() + y]) {
                changed++;
                REQUIRE(px == expected);
            }
        }
    }
    CHECK(changed >= 20);
}

TEST_CASE("flat levels bake the fog into each row", "[screensaver][maze]") {
    using helix::ui::kMazeFlatCeiling;
    using helix::ui::kMazeFlatFloor;
    using helix::ui::maze_fog;
    using helix::ui::maze_fog_k;
    using helix::ui::maze_fog_slot_depth;
    MazeMap map = room(15);
    MazeRenderer r;
    TestFrame frame(128, 96, PixelFormat::XRGB8888);
    r.init(128, 96, PixelFormat::XRGB8888, MazeArt{}, 2);
    REQUIRE(r.internal_w() == 64);
    REQUIRE(r.internal_h() == 48);
    r.render(map, {}, MazePose{7.5f, 7.5f, 0.0f}, 0.0f, 0, frame.target);
    // Focal 24: row 30 sees the floor 24 / 13 tiles away, row 17 the ceiling as far, row 47
    // the floor half a tile away.
    const uint32_t k = maze_fog_k(maze_fog_slot_depth(24.0f / 13.0f));
    const uint32_t near_k = maze_fog_k(maze_fog_slot_depth(24.0f / 47.0f));
    REQUIRE(k < near_k);
    const auto fmt = PixelFormat::XRGB8888;
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        INFO("column " << x);
        CHECK(r.internal_pixel(x, 30) == PixelWriter::pack(maze_fog(kMazeFlatFloor, k), fmt));
        CHECK(r.internal_pixel(x, 17) == PixelWriter::pack(maze_fog(kMazeFlatCeiling, k), fmt));
        CHECK(r.internal_pixel(x, 47) == PixelWriter::pack(maze_fog(kMazeFlatFloor, near_k), fmt));
    }
}

TEST_CASE("a textured floor darkens with distance", "[screensaver][maze]") {
    MazeMap map = room(31);
    MazeRenderer r;
    TestFrame frame(128, 96, PixelFormat::XRGB8888);
    r.init(128, 96, PixelFormat::XRGB8888, MazeArt{}, 1);
    r.render(map, {}, MazePose{1.5f, 15.5f, 0.0f}, 0.0f, 0, frame.target);
    auto luminance = [&](uint32_t y) {
        double sum = 0.0;
        for (uint32_t x = 0; x < r.internal_w(); x++) {
            const uint32_t p = r.internal_pixel(x, y);
            sum += 0.3 * (p >> 16 & 0xFF) + 0.59 * (p >> 8 & 0xFF) + 0.11 * (p & 0xFF);
        }
        return sum / r.internal_w();
    };
    // Row 25 sees the floor 8 tiles off, row 47 half a tile off.
    CHECK(luminance(25) < 0.6 * luminance(47));
}

TEST_CASE("a flat level gives the fog bands back", "[screensaver][maze]") {
    const size_t bands =
        2u * helix::ui::kMazeFogBands * MazeRenderer::kTex * MazeRenderer::kTex * 2u;
    MazeRenderer r;
    r.init(800, 480, PixelFormat::RGB565, MazeArt{}, 1);
    const size_t textured = r.heap_bytes();
    r.set_level(2);
    CHECK(textured - r.heap_bytes() >= bands);
    MazeRenderer flat;
    flat.init(800, 480, PixelFormat::RGB565, MazeArt{}, 2);
    CHECK(flat.heap_bytes() == r.heap_bytes());
}

TEST_CASE("a symmetric scene renders symmetric wall slices", "[screensaver][maze]") {
    MazeMap map = corridor(11);
    MazeRenderer r;
    TestFrame frame(120, 80, PixelFormat::XRGB8888);
    r.init(frame.target.w, frame.target.h, PixelFormat::XRGB8888, MazeArt{});
    r.set_level(0);
    MazePose pose{2.5f, 5.5f, 0.0f};
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    const uint32_t w = r.internal_w();
    for (uint32_t x = 0; x < w / 2; x++) {
        INFO("column " << x);
        CHECK(r.wall_distance(x) == Catch::Approx(r.wall_distance(w - 1 - x)).margin(0.02f));
    }
}

TEST_CASE("a sprite behind a wall draws nothing; in front of it, it draws", "[screensaver][maze]") {
    // Corridor east along row 5; the camera looks down it at a logo.
    MazeMap map = corridor(11);
    MazeRenderer r;
    TestFrame frame(120, 80, PixelFormat::XRGB8888);
    r.init(frame.target.w, frame.target.h, PixelFormat::XRGB8888, MazeArt{});
    r.set_level(0);
    const MazePose pose{1.5f, 5.5f, 0.0f};

    r.render(map, {}, pose, 0.0f, 0, frame.target);
    std::vector<uint32_t> bare(r.internal_w() * r.internal_h());
    for (uint32_t x = 0; x < r.internal_w(); x++) {
        for (uint32_t y = 0; y < r.internal_h(); y++) {
            bare[x * r.internal_h() + y] = r.internal_pixel(x, y);
        }
    }
    auto changed = [&]() {
        size_t n = 0;
        for (uint32_t x = 0; x < r.internal_w(); x++) {
            for (uint32_t y = 0; y < r.internal_h(); y++) {
                n += r.internal_pixel(x, y) != bare[x * r.internal_h() + y] ? 1 : 0;
            }
        }
        return n;
    };

    // Beyond the east wall (x = 10) the logo is outside the corridor and hidden.
    std::vector<MazeItem> hidden{{MazeItemKind::LOGO, 12.5f, 5.5f, true}};
    r.render(map, hidden, pose, 0.0f, 0, frame.target);
    CHECK(changed() == 0);

    std::vector<MazeItem> visible{{MazeItemKind::LOGO, 5.5f, 5.5f, true}};
    r.render(map, visible, pose, 0.0f, 0, frame.target);
    CHECK(changed() > 0);

    std::vector<MazeItem> inactive{{MazeItemKind::LOGO, 5.5f, 5.5f, false}};
    r.render(map, inactive, pose, 0.0f, 0, frame.target);
    CHECK(changed() == 0);
}

TEST_CASE("picture walls read the right way round from all four sides", "[screensaver][maze]") {
    // Left half red, right half blue.
    MazeArt art;
    art.pictures.push_back(solid(2, 1, 0xFFFF0000u));
    art.pictures[0].argb[1] = 0xFF0000FFu;
    const int size = 15;
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            tiles[static_cast<size_t>(y * size + x)] = MazeTile::OPEN;
        }
    }
    const int mid = size / 2;
    const int picture[4][2] = {{size - 1, mid}, {mid, size - 1}, {0, mid}, {mid, 0}};
    for (const auto& t : picture) {
        tiles[static_cast<size_t>(t[1] * size + t[0])] = MazeTile::PICTURE;
    }
    MazeMap map;
    map.set_tiles(size, tiles);
    MazeRenderer r;
    TestFrame frame(320, 200, PixelFormat::XRGB8888);
    r.init(320, 200, PixelFormat::XRGB8888, art);
    REQUIRE(r.picture_count() == 1);
    for (int heading = 0; heading < 4; heading++) {
        INFO("heading " << heading);
        // Three tiles back from the picture tile's centre, facing it.
        const MazePose pose{picture[heading][0] + 0.5f - 3.0f * helix::ui::kHeadingDx[heading],
                            picture[heading][1] + 0.5f - 3.0f * helix::ui::kHeadingDy[heading],
                            heading * helix::ui::kSaverPi * 0.5f};
        r.render(map, {}, pose, 0.0f, 0, frame.target);
        const uint32_t row = r.internal_h() / 2;
        const uint32_t left = r.internal_pixel(r.internal_w() / 2 - 10, row);
        const uint32_t right = r.internal_pixel(r.internal_w() / 2 + 10, row);
        // Fogged and, on the east-west faces, darkened: read which channel dominates.
        CHECK((left >> 16 & 0xFF) > 100);
        CHECK((left & 0xFF) < 60);
        CHECK((right & 0xFF) > 100);
        CHECK((right >> 16 & 0xFF) < 60);
    }
}

TEST_CASE("reduce_maze_image fits a box and keeps aspect", "[screensaver][maze]") {
    using helix::ui::MazeImage;
    using helix::ui::reduce_maze_image;
    const MazeImage big = reduce_maze_image(solid(128, 64, 0xFFFF0000u), 64);
    CHECK(big.w == 64);
    CHECK(big.h == 32);
    REQUIRE(big.argb.size() == 64u * 32u);
    for (uint32_t p : big.argb) {
        REQUIRE(p == 0xFFFF0000u);
    }
    const MazeImage small = reduce_maze_image(solid(10, 10, 0xFF123456u), 64);
    CHECK(small.w == 10);
    CHECK(small.h == 10);
    CHECK(small.argb == solid(10, 10, 0xFF123456u).argb);

    MazeImage pair = solid(2, 1, 0xFFFF0000u);
    pair.argb[1] = 0xFF0000FFu;
    const MazeImage one = reduce_maze_image(pair, 1);
    REQUIRE(one.w == 1);
    REQUIRE(one.h == 1);
    const uint32_t r = one.argb[0] >> 16 & 0xFF;
    const uint32_t b = one.argb[0] & 0xFF;
    CHECK((r == 127 || r == 128));
    CHECK((b == 127 || b == 128));

    // Left half clear white, right half opaque blue.
    MazeImage half = solid(4, 4, 0x00FFFFFFu);
    for (int y = 0; y < 4; y++) {
        half.argb[static_cast<size_t>(y * 4 + 2)] = 0xFF0000FFu;
        half.argb[static_cast<size_t>(y * 4 + 3)] = 0xFF0000FFu;
    }
    const MazeImage two = reduce_maze_image(half, 2);
    REQUIRE(two.w == 2);
    REQUIRE(two.h == 2);
    CHECK((two.argb[0] >> 24) == 0);
    CHECK(two.argb[1] == 0xFF0000FFu);
    CHECK(reduce_maze_image(MazeImage{}, 64).empty());
}

namespace {

/// A 15-tile room with picture walls at the east and north wall centres, given indices.
MazeMap two_picture_room(uint8_t east, uint8_t north) {
    const int size = 15;
    std::vector<MazeTile> tiles(static_cast<size_t>(size * size), MazeTile::BRICK);
    std::vector<uint8_t> pictures(tiles.size(), 0);
    for (int y = 1; y < size - 1; y++) {
        for (int x = 1; x < size - 1; x++) {
            tiles[static_cast<size_t>(y * size + x)] = MazeTile::OPEN;
        }
    }
    const size_t e = static_cast<size_t>((size / 2) * size + size - 1);
    const size_t n = static_cast<size_t>(size / 2);
    tiles[e] = MazeTile::PICTURE;
    pictures[e] = east;
    tiles[n] = MazeTile::PICTURE;
    pictures[n] = north;
    MazeMap map;
    map.set_tiles(size, tiles, pictures);
    return map;
}

/// The centre pixel facing east (heading 0) or north (heading 3) at the room's wall centres.
uint32_t centre_facing(MazeRenderer& r, const MazeMap& map, int heading) {
    TestFrame frame(160, 100, PixelFormat::XRGB8888);
    const MazePose pose = heading == 0 ? MazePose{14.0f - 3.0f, 7.5f, 0.0f}
                                       : MazePose{7.5f, 1.0f + 3.0f, -helix::ui::kSaverPi * 0.5f};
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    return r.internal_pixel(r.internal_w() / 2, r.internal_h() / 2);
}

bool red_dominant(uint32_t p) {
    return (p >> 16 & 0xFF) > 100 && (p & 0xFF) < 60;
}

bool blue_dominant(uint32_t p) {
    return (p & 0xFF) > 100 && (p >> 16 & 0xFF) < 60;
}

} // namespace

TEST_CASE("each picture wall shows its own picture", "[screensaver][maze]") {
    MazeArt art;
    art.pictures = {solid(4, 4, 0xFFFF0000u), solid(4, 4, 0xFF0000FFu)};
    MazeRenderer r;
    r.init(160, 100, PixelFormat::XRGB8888, art);
    const MazeMap map = two_picture_room(0, 1);
    CHECK(red_dominant(centre_facing(r, map, 0)));
    CHECK(blue_dominant(centre_facing(r, map, 3)));
}

TEST_CASE("picture indices wrap by the picture count", "[screensaver][maze]") {
    MazeArt art;
    art.pictures = {solid(4, 4, 0xFFFF0000u), solid(4, 4, 0xFF0000FFu)};
    MazeRenderer r;
    r.init(160, 100, PixelFormat::XRGB8888, art);
    const MazeMap map = two_picture_room(5, 4);
    CHECK(blue_dominant(centre_facing(r, map, 0)));
    CHECK(red_dominant(centre_facing(r, map, 3)));
}

TEST_CASE("pictures past twelve are ignored, empty ones skipped", "[screensaver][maze]") {
    MazeArt art;
    for (int i = 0; i < 14; i++) {
        art.pictures.push_back(solid(4, 4, 0xFF000000u | static_cast<uint32_t>(i * 16)));
    }
    MazeRenderer r;
    r.init(64, 48, PixelFormat::RGB565, art);
    CHECK(r.picture_count() == helix::ui::kMazeMaxPictures);

    MazeArt sparse;
    sparse.pictures = {helix::ui::MazeImage{}, solid(4, 4, 0xFFFF0000u), helix::ui::MazeImage{},
                       solid(4, 4, 0xFF0000FFu)};
    MazeRenderer s;
    s.init(160, 100, PixelFormat::XRGB8888, sparse);
    CHECK(s.picture_count() == 2);
    // Index 1 is the second picture that is there: blue.
    CHECK(blue_dominant(centre_facing(s, two_picture_room(1, 0), 0)));
}

TEST_CASE("frames of every size, level and depth stay inside their rows", "[screensaver][maze]") {
    const std::pair<uint32_t, uint32_t> sizes[] = {{801, 481}, {799, 479}, {480, 272},
                                                   {5, 2},     {3, 3},     {1, 1}};
    const uint32_t dts[] = {16, 700, 33, 16, 50, 1, 16, 400};
    for (PixelFormat fmt : {PixelFormat::RGB565, PixelFormat::XRGB8888}) {
        for (const auto& [w, h] : sizes) {
            for (size_t level = 0; level < helix::ui::kMazeLevelCount; level++) {
                INFO(w << "x" << h << " level " << level << " bpp " << helix::ui::pixel_bytes(fmt));
                TestFrame frame(w, h, fmt);
                FrameTarget& target = frame.target;
                std::minstd_rand rng(w * 31 + h + static_cast<uint32_t>(level));
                MazeSim sim;
                sim.init(target, rng, level, MazeArt{});
                std::vector<DirtyRect> dirty;
                for (int i = 0; i < 24; i++) {
                    if (i == 8) {
                        sim.request_level(helix::ui::kMazeLevelCount - 1);
                    } else if (i == 16) {
                        sim.request_level(0);
                    }
                    sim.step(dts[i % 8], target, rng, dirty);
                }
                REQUIRE(frame.padding_intact());
            }
        }
    }
}

TEST_CASE("an RGB565 maze holds its frame and art at two bytes a pixel", "[screensaver][maze]") {
    MazeRenderer wide;
    MazeRenderer narrow;
    wide.init(800, 480, PixelFormat::XRGB8888, MazeArt{});
    narrow.init(800, 480, PixelFormat::RGB565, MazeArt{});
    // Everything per pixel halves; only the per-row and per-column tables stay the same size.
    CHECK(narrow.heap_bytes() * 10 < wide.heap_bytes() * 6);
}

TEST_CASE("RGB565 and XRGB8888 renders of one scene agree within two 565 steps",
          "[screensaver][maze]") {
    std::minstd_rand rng(5);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    const MazePose pose{1.5f, 1.5f, 0.3f};
    TestFrame f32(96, 64, PixelFormat::XRGB8888);
    TestFrame f16(96, 64, PixelFormat::RGB565);
    MazeRenderer r32;
    MazeRenderer r16;
    r32.init(96, 64, PixelFormat::XRGB8888, MazeArt{});
    r16.init(96, 64, PixelFormat::RGB565, MazeArt{});
    r32.render(map, {}, pose, 0.0f, 0, f32.target);
    r16.render(map, {}, pose, 0.0f, 0, f16.target);
    PixelWriter p32(f32.target);
    PixelWriter p16(f16.target);
    size_t lit = 0;
    for (int32_t y = 0; y < 64; y++) {
        for (int32_t x = 0; x < 96; x++) {
            const Rgb a = p32.get(x, y);
            const Rgb b = p16.get(x, y);
            // Two 565 steps: the fog shades again in 565's own precision.
            REQUIRE(std::abs(a.r - b.r) <= 16);
            REQUIRE(std::abs(a.g - b.g) <= 8);
            REQUIRE(std::abs(a.b - b.b) <= 16);
            lit += (a.r | a.g | a.b) != 0 ? 1 : 0;
        }
    }
    CHECK(lit > 96 * 64 / 2);
}

TEST_CASE("upside down is the vertical mirror of upright", "[screensaver][maze]") {
    std::minstd_rand rng(13);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    const MazePose pose{1.5f, 1.5f, 0.0f};
    for (size_t level = 0; level < helix::ui::kMazeLevelCount; level++) {
        INFO("level " << level);
        TestFrame up(64, 48, PixelFormat::XRGB8888);
        TestFrame down(64, 48, PixelFormat::XRGB8888);
        MazeRenderer r;
        r.init(64, 48, PixelFormat::XRGB8888, MazeArt{});
        r.set_level(level);
        r.render(map, {}, pose, 0.0f, 0, up.target);
        r.render(map, {}, pose, 1.0f, 0, down.target);
        for (uint32_t y = 0; y < 48; y++) {
            REQUIRE(std::memcmp(up.bytes.data() + y * up.target.stride,
                                down.bytes.data() + (47 - y) * down.target.stride, 64 * 4) == 0);
        }
    }
}

TEST_CASE("mid-flip the view squashes to black at the horizon", "[screensaver][maze]") {
    std::minstd_rand rng(13);
    MazeMap map;
    map.generate(MazeSim::CELLS, rng);
    TestFrame frame(64, 48, PixelFormat::XRGB8888);
    MazeRenderer r;
    r.init(64, 48, PixelFormat::XRGB8888, MazeArt{});
    r.render(map, {}, MazePose{1.5f, 1.5f, 0.0f}, 0.5f, 0, frame.target);
    PixelWriter p(frame.target);
    for (int32_t x = 0; x < 64; x++) {
        CHECK(p.get(x, 0) == Rgb{0, 0, 0});
        CHECK(p.get(x, 47) == Rgb{0, 0, 0});
    }
}

TEST_CASE("the maze ladder runs 16, 33, 33, 50, 66 ms and shrinks the internal frame",
          "[screensaver][maze]") {
    REQUIRE(helix::ui::kMazeLevelCount == 5);
    CHECK(kMazeLevels[0].period_ms == 16);
    CHECK(kMazeLevels[1].period_ms == 33);
    CHECK(kMazeLevels[2].period_ms == 33);
    CHECK(kMazeLevels[3].period_ms == 50);
    CHECK(kMazeLevels[4].period_ms == 66);
    CHECK(kMazeLevels[4].scale == 4);
    CHECK_FALSE(kMazeLevels[4].textured_floor);
    CHECK(kMazeLevels[1].textured_floor);
    CHECK_FALSE(kMazeLevels[2].textured_floor);

    MazeRenderer r;
    r.init(800, 480, PixelFormat::RGB565, MazeArt{});
    r.set_level(0);
    CHECK(r.internal_w() == 800);
    CHECK(r.internal_h() == 480);
    r.set_level(3);
    CHECK(r.internal_w() == 200);
    CHECK(r.internal_h() == 120);
    r.set_level(4);
    CHECK(r.internal_w() == 200);
    CHECK(r.internal_h() == 120);
    r.set_level(99);
    CHECK(r.level() == 4);
    CHECK(r.level() == helix::ui::kMazeLevelCount - 1);

    // A run that starts low holds only the frame that level draws.
    MazeRenderer low;
    low.init(800, 480, PixelFormat::RGB565, MazeArt{}, 3);
    CHECK(low.internal_w() == 200);
    CHECK(r.heap_bytes() - low.heap_bytes() >= (800u * 480u - 200u * 120u) * 2u);
}

TEST_CASE("the flat-floor level draws the floor as one colour per row", "[screensaver][maze]") {
    MazeMap map = room(15);
    MazeRenderer r;
    TestFrame frame(128, 96, PixelFormat::XRGB8888);
    r.init(128, 96, PixelFormat::XRGB8888, MazeArt{});
    const MazePose pose{7.5f, 7.5f, 0.0f};
    auto row_is_flat = [&](uint32_t y) {
        for (uint32_t x = 1; x < r.internal_w(); x++) {
            if (r.internal_pixel(x, y) != r.internal_pixel(0, y)) {
                return false;
            }
        }
        return true;
    };
    r.set_level(2);
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    CHECK(row_is_flat(r.internal_h() - 1));
    r.set_level(1);
    r.render(map, {}, pose, 0.0f, 0, frame.target);
    CHECK_FALSE(row_is_flat(r.internal_h() - 1));
}

TEST_CASE("every maze frame dirties the whole canvas", "[screensaver][maze]") {
    std::minstd_rand rng(2);
    MazeSim sim;
    TestFrame frame(96, 64, PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 0, MazeArt{});
    std::vector<DirtyRect> dirty;
    sim.step(16, frame.target, rng, dirty);
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0] == DirtyRect{0, 0, 95, 63});
}

TEST_CASE("a cleared maze holds no render buffers", "[screensaver][maze]") {
    std::minstd_rand rng(4);
    MazeSim sim;
    TestFrame frame(160, 96, PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 0, MazeArt{});
    REQUIRE(sim.renderer().heap_bytes() > 160u * 96u * 4u);
    sim.clear();
    CHECK(sim.renderer().heap_bytes() == 0);
    CHECK(sim.items().capacity() == 0);
    CHECK(sim.map().size() == 0);
}

TEST_CASE("a scene places start, exit, logos, rocks and a toolhead", "[screensaver][maze]") {
    std::minstd_rand rng(21);
    MazeSim sim;
    TestFrame frame(64, 48, PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 3, MazeArt{});
    int counts[5] = {};
    for (const MazeItem& item : sim.items()) {
        counts[static_cast<int>(item.kind)]++;
        CHECK_FALSE(sim.map().wall(static_cast<int>(item.x), static_cast<int>(item.y)));
    }
    CHECK(counts[static_cast<int>(MazeItemKind::START)] == 1);
    CHECK(counts[static_cast<int>(MazeItemKind::EXIT)] == 1);
    CHECK(counts[static_cast<int>(MazeItemKind::LOGO)] == MazeSim::kLogoCount);
    CHECK(counts[static_cast<int>(MazeItemKind::ROCK)] == MazeSim::kRockCount);
    CHECK(counts[static_cast<int>(MazeItemKind::TOOLHEAD)] == 1);
}

TEST_CASE("the walk waits in the exit's doorway with the smiley in view", "[screensaver][maze]") {
    for (uint32_t seed : {5u, 6u, 7u}) {
        INFO("seed " << seed);
        std::minstd_rand rng(seed);
        MazeSim sim;
        TestFrame frame(160, 96, PixelFormat::XRGB8888);
        sim.init(frame.target, rng, 0, MazeArt{});
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 40000 && !sim.holding_exit(); i++) {
            sim.step(50, frame.target, rng, dirty);
        }
        REQUIRE(sim.holding_exit());
        sim.step(100, frame.target, rng, dirty);
        REQUIRE(sim.holding_exit());
        PixelWriter p(frame.target);
        size_t yellow = 0;
        for (int32_t y = 0; y < 96; y++) {
            for (int32_t x = 0; x < 160; x++) {
                const Rgb c = p.get(x, y);
                yellow += c.r > 200 && c.g > 170 && c.b < 90 ? 1 : 0;
            }
        }
        // The smiley stands 0.6 tiles tall a tile ahead: a sixth of its square is yellow.
        const float side = 0.6f * helix::ui::maze_focal(160, 96);
        CHECK(static_cast<float>(yellow) > side * side / 6.0f);
    }
}

TEST_CASE("touching a rock flips the view and swaps hands; reaching the exit starts a new maze",
          "[screensaver][maze]") {
    std::minstd_rand rng(17);
    MazeSim sim;
    TestFrame frame(48, 32, PixelFormat::XRGB8888);
    sim.init(frame.target, rng, 3, MazeArt{});
    std::vector<DirtyRect> dirty;
    bool saw_flip = false;
    bool saw_left = false;
    // Every cell of a perfect maze is on the wall walk, so both rocks and the exit come up.
    for (int i = 0; i < 40000 && sim.mazes() < 2; i++) {
        sim.step(50, frame.target, rng, dirty);
        const float flip = sim.flip();
        saw_flip = saw_flip || (flip > 0.0f && flip < 1.0f);
        saw_left = saw_left || sim.walker().left_handed();
        if (!sim.flipping()) {
            // Upside down exactly while following the left wall, once the flip settles.
            REQUIRE((flip == 1.0f) == sim.walker().left_handed());
        }
    }
    CHECK(saw_flip);
    CHECK(saw_left);
    CHECK(sim.mazes() >= 2);
}

TEST_CASE("a helix logo from outside replaces the drawn mark", "[screensaver][maze]") {
    std::minstd_rand rng(3);
    MazeMap map = corridor(11);
    const MazePose pose{1.5f, 5.5f, 0.0f};
    std::vector<MazeItem> logo{{MazeItemKind::LOGO, 4.5f, 5.5f, true}};

    auto render_with = [&](const MazeArt& art) {
        TestFrame frame(96, 64, PixelFormat::XRGB8888);
        MazeRenderer r;
        r.init(96, 64, PixelFormat::XRGB8888, art);
        r.render(map, logo, pose, 0.0f, 0, frame.target);
        return frame_fnv1a(frame.target);
    };
    MazeArt red;
    red.logo.w = 8;
    red.logo.h = 8;
    red.logo.argb.assign(64, 0xFFFF0000u);
    CHECK(render_with(red) != render_with(MazeArt{}));
}

TEST_CASE("picture walls show the picture only when there is one", "[screensaver][maze]") {
    MazeArt with;
    with.pictures = {solid(4, 4, 0xFF00FF00u)};
    MazeRenderer a;
    MazeRenderer b;
    a.init(32, 32, PixelFormat::XRGB8888, with);
    b.init(32, 32, PixelFormat::XRGB8888, MazeArt{});
    CHECK(a.picture_count() == 1);
    CHECK(b.picture_count() == 0);
}

TEST_CASE("PixelWriter::pack writes what put() writes", "[screensaver][maze]") {
    for (PixelFormat fmt : {PixelFormat::RGB565, PixelFormat::XRGB8888}) {
        TestFrame frame(2, 1, fmt);
        PixelWriter w(frame.target);
        const Rgb c{200, 100, 50};
        w.put(0, 0, c);
        const uint32_t packed = PixelWriter::pack(c, fmt);
        if (fmt == PixelFormat::RGB565) {
            uint16_t v;
            std::memcpy(&v, frame.bytes.data(), 2);
            CHECK(v == packed);
        } else {
            uint32_t v;
            std::memcpy(&v, frame.bytes.data(), 4);
            CHECK(v == packed);
        }
    }
}

namespace {

PrintHistoryJob history_job(const std::string& filename, std::vector<ThumbnailInfo> thumbnails) {
    PrintHistoryJob job;
    job.filename = filename;
    job.thumbnails = std::move(thumbnails);
    return job;
}

} // namespace

TEST_CASE("history art candidates: each file once, its thumbnails largest first",
          "[screensaver][maze]") {
    using helix::ui::history_art_candidates;
    const std::vector<PrintHistoryJob> jobs{
        history_job("sub/a.gcode", {{".thumbs/a-32.png", 32, 32}, {".thumbs/a-300.png", 300, 300}}),
        history_job("b.gcode", {}),
        history_job("c.gcode", {{".thumbs/c.png", 300, 300}}),
        history_job("sub/a.gcode", {{".thumbs/a-32.png", 32, 32}}),
    };
    const auto all = history_art_candidates(jobs, 5);
    REQUIRE(all.size() == 2);
    CHECK(all[0] == std::vector<std::string>{"sub/.thumbs/a-300.png", "sub/.thumbs/a-32.png"});
    CHECK(all[1] == std::vector<std::string>{".thumbs/c.png"});
    const auto first = history_art_candidates(jobs, 1);
    REQUIRE(first.size() == 1);
    CHECK(first[0] == all[0]);
    // A thumbnail too big to decode goes after the ones that fit.
    const auto big = history_art_candidates(
        {history_job("d.gcode", {{"d-600.png", 600, 600}, {"d-100.png", 100, 100}})}, 5);
    REQUIRE(big.size() == 1);
    CHECK(big[0] == std::vector<std::string>{"d-100.png", "d-600.png"});
}

TEST_CASE("cached art paths take one cached key per candidate within their probes",
          "[screensaver][maze]") {
    using helix::ui::cached_art_paths;
    const std::map<std::string, std::string> cache{{"k1", "A:/c/1.bin"}, {"k3", "A:/c/3.png"}};
    std::vector<std::string> asked;
    const helix::ui::MazeArtLookup lookup = [&](const std::string& key) {
        asked.push_back(key);
        const auto it = cache.find(key);
        return it == cache.end() ? std::string() : it->second;
    };
    const std::vector<helix::ui::MazeArtCandidate> candidates{{"k0", "k1"}, {"k2"}, {"k3"}};
    CHECK(cached_art_paths(candidates, lookup, 5, 10) ==
          std::vector<std::string>{"A:/c/1.bin", "A:/c/3.png"});
    asked.clear();
    CHECK(cached_art_paths(candidates, lookup, 1, 10) == std::vector<std::string>{"A:/c/1.bin"});
    CHECK(asked == std::vector<std::string>{"k0", "k1"});
    asked.clear();
    CHECK(cached_art_paths(candidates, lookup, 5, 2) == std::vector<std::string>{"A:/c/1.bin"});
    CHECK(asked == std::vector<std::string>{"k0", "k1"});
    CHECK(cached_art_paths(candidates, lookup, 5, 10, {"A:/c/1.bin"}) ==
          std::vector<std::string>{"A:/c/3.png"});
}

TEST_CASE("maze art mixes recent prints, other files and the printer", "[screensaver][maze]") {
    using helix::ui::MazeArtSources;
    using helix::ui::mix_maze_art;
    using V = std::vector<std::string>;
    const MazeArtSources full{{"r1", "r2", "r3"}, {"f1", "f2"}, "p"};
    CHECK(mix_maze_art(full, 12) == V{"r1", "f1", "p", "r2", "f2", "r3"});
    CHECK(mix_maze_art(full, 3) == V{"r1", "f1", "p"});
    CHECK(mix_maze_art(MazeArtSources{{"a"}, {"a", "b"}, ""}, 12) == V{"a", "b"});
    CHECK(mix_maze_art(MazeArtSources{{}, {}, "p"}, 12) == V{"p"});
    CHECK(mix_maze_art(MazeArtSources{}, 12).empty());
}

namespace {

/// Starts a maze saver whose art comes from `sources`; returns the pictures it hung.
/// Pictures hung from @p sources. The budget defaults to unbounded so the count
/// does not depend on how fast this machine decodes.
size_t pictures_hung(const helix::ui::MazeArtSources& sources,
                     uint32_t budget_ms = std::numeric_limits<uint32_t>::max()) {
    MazeScreensaver saver;
    saver.set_art_source([sources]() { return sources; });
    saver.set_art_decode_budget_ms(budget_ms);
    saver.start();
    REQUIRE(saver.is_active());
    const size_t count = saver.sim().renderer().picture_count();
    saver.stop();
    return count;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the maze saver hangs the art its source gives",
                 "[screensaver][maze]") {
    helix::ui::MazeArtSources sources;
    sources.recent = {"A:assets/images/helix-icon-64.png",
                      "A:assets/images/benchy_thumbnail_white.png"};
    sources.files = {"A:assets/images/folder.png"};
    sources.printer = "A:assets/images/printer_200.png";
    CHECK(pictures_hung(sources) == 4);
}

TEST_CASE_METHOD(LVGLTestFixture, "the maze saver starts no decode past its art budget",
                 "[screensaver][maze]") {
    helix::ui::MazeArtSources sources;
    sources.recent = {"A:assets/images/helix-icon-64.png"};
    sources.printer = "A:assets/images/printer_200.png";
    CHECK(pictures_hung(sources, 0) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "the maze saver skips missing and oversized thumbnails",
                 "[screensaver][maze]") {
    helix::ui::MazeArtSources sources;
    // 1929 x 1869: over the side cap for a thumbnail.
    sources.recent = {"A:/nonexistent.png", "A:assets/images/printers/snapmaker-u1.png",
                      "A:assets/images/folder.png"};
    CHECK(pictures_hung(sources) == 1);
    // The printer picture is held to the same cap.
    sources.recent.clear();
    sources.printer = "A:assets/images/printers/snapmaker-u1.png";
    CHECK(pictures_hung(sources) == 0);
    sources.printer = "A:assets/images/printer_200.png";
    CHECK(pictures_hung(sources) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "the maze saver keeps twelve pictures at most",
                 "[screensaver][maze]") {
    helix::ui::MazeArtSources sources;
    for (const char* lang : {"de", "en", "es", "fr", "it", "ja", "pt", "ru", "zh"}) {
        sources.files.push_back(std::string("A:assets/images/flags/flag_") + lang + ".png");
    }
    sources.files.push_back("A:assets/images/helix-icon-64.png");
    sources.files.push_back("A:assets/images/benchy_thumbnail_white.png");
    sources.files.push_back("A:assets/images/folder.png");
    sources.printer = "A:assets/images/printer_200.png";
    CHECK(pictures_hung(sources) == helix::ui::kMazeMaxPictures);
}

TEST_CASE_METHOD(LVGLTestFixture, "no wall art means brick", "[screensaver][maze]") {
    CHECK(pictures_hung(helix::ui::MazeArtSources{}) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "the maze's cache lookup reads, never writes",
                 "[screensaver][maze]") {
    namespace fs = std::filesystem;
    const std::string key = "maze-test/x.png";
    const std::string path = get_thumbnail_cache().get_cache_path(
        helix::thumbnail_cache_id(helix::ThumbnailSource::Moonraker, key));
    fs::create_directories(fs::path(path).parent_path());
    fs::copy_file("assets/images/helix-icon-64.png", path, fs::copy_options::overwrite_existing);
    // Long stale, so a freshness check would throw it away.
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 365 * 25);
    fs::last_write_time(path, old);

    CHECK(helix::ui::maze_cached_thumbnail(key) == "A:" + path);
    REQUIRE(fs::exists(path));
    // Parenthesised so Catch2 does not print the operands: libc++'s file clock
    // counts in __int128, which std::ostream cannot write.
    CHECK((fs::last_write_time(path) == old));
    CHECK(helix::ui::maze_cached_thumbnail("maze-test/not-cached.png").empty());
    CHECK(helix::ui::maze_cached_thumbnail("").empty());
    // A listing that names local files caches under that source; the lookup finds those too.
    const std::string local = get_thumbnail_cache().get_cache_path(
        helix::thumbnail_cache_id(helix::ThumbnailSource::LocalFile, "maze-test/local.png"));
    fs::copy_file("assets/images/helix-icon-64.png", local, fs::copy_options::overwrite_existing);
    CHECK(helix::ui::maze_cached_thumbnail("maze-test/local.png") == "A:" + local);
    fs::remove(local);
    fs::remove(path);
}

TEST_CASE_METHOD(LVGLTestFixture, "print select lists the printer files' thumbnail keys",
                 "[screensaver][maze]") {
    PrintSelectPanel panel(get_printer_state(), nullptr);
    std::vector<PrintFileData> files(4);
    files[0].filename = "a.gcode";
    files[0].original_thumbnail_url = "a.png";
    files[1].filename = "b.gcode";
    files[2].filename = "sub/c.gcode";
    files[2].original_thumbnail_url = "sub/b.png";
    files[3].filename = "dir";
    files[3].is_dir = true;
    files[3].original_thumbnail_url = "dir.png";
    PrintSelectPanelTestAccess::set_files(panel, std::move(files));
    CHECK(panel.printer_thumbnail_keys() == std::vector<std::string>{"a.png", "sub/b.png"});
}

#endif // HELIX_ENABLE_SCREENSAVER
