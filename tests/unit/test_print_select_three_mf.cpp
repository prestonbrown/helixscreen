// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_preparation_manager.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/stored_zip.h"
#include "three_mf_reader.h"
#include "thumbnail_cache.h"
#include "thumbnail_load_context.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string fixture_bytes(const char* name) {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/three_mf/"
                                   : "tests/fixtures/three_mf/";
    std::ifstream in(dir + name, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(!bytes.empty());
    return bytes;
}

/// The panel over MoonrakerAPIMock: its transfers read the planted archives
/// byte range by byte range, and its client lists them with no metadata.
struct ArchivePanel : PrintSelectGlobalStateReset, PrintSelectPanelFixture {
    ArchivePanel()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Unregistered,
                                  PrintSelectVisit::Immediate, PrintSelectApi::Mock) {}

    MoonrakerAPIMock& mock() {
        return static_cast<MoonrakerAPIMock&>(*api_);
    }

    // One panel accessor each, so the tests below read as the panel's own words.
    void select_file(const std::string& name) {
        PrintSelectPanelTestAccess::select_file(*panel_, name);
    }
    bool prepare_archive_start() {
        return PrintSelectPanelTestAccess::prepare_archive_start(*panel_);
    }
    std::string plate_text() {
        return PrintSelectPanelTestAccess::plate_text(*panel_);
    }
    void step_plate(int step) {
        PrintSelectPanelTestAccess::step_plate(*panel_, step);
    }
    std::string plate_position() {
        return PrintSelectPanelTestAccess::plate_position(*panel_);
    }
    int plate_count() {
        return PrintSelectPanelTestAccess::plate_count(*panel_);
    }
    std::optional<std::string> queueable_path() {
        return PrintSelectPanelTestAccess::queueable_path(*panel_);
    }
    ui::PrintPreparationManager* prep_manager() {
        return PrintSelectPanelTestAccess::prep_manager(*panel_);
    }
    const PrintFileData* find_file(const std::string& name) {
        return PrintSelectPanelTestAccess::find_file(*panel_, name);
    }
    std::vector<std::string> selected_colors() {
        return PrintSelectPanelTestAccess::selected_colors(*panel_);
    }
    std::string selected_print_time() {
        return PrintSelectPanelTestAccess::selected_print_time(*panel_);
    }
    void apply_metadata(const std::string& name, const FileMetadata& meta) {
        PrintSelectPanelTestAccess::apply_metadata(*panel_, name, meta);
    }

    /// List the planted files, then read @p name the way its card would.
    const PrintFileData& load(const std::string& name) {
        panel_->refresh_files(true);
        drain();
        REQUIRE(find_file(name) != nullptr);
        PrintSelectPanelTestAccess::fetch_metadata(*panel_, name);
        drain();
        const PrintFileData* row = find_file(name);
        REQUIRE(row != nullptr);
        return *row;
    }
};

} // namespace

TEST_CASE_METHOD(ArchivePanel, "a .3mf without printer metadata is described by its archive",
                 "[print_select][three_mf]") {
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());

    REQUIRE(row.archive);
    CHECK(row.archive_extract);
    REQUIRE(row.archive->plates.size() == 1);
    CHECK(row.print_time_minutes == 5238 / 60);
    CHECK(row.filament_grams == Catch::Approx(23.73f));
    CHECK(row.filament_types == std::vector<std::string>{"ABS"});
    CHECK(row.filament_colors == std::vector<std::string>{"#000000"});
    // Its render is the card's thumbnail, prescaled like any other.
    CHECK(row.thumbnail_path.find(".bin") != std::string::npos);
    // The tail (central directory and slice_info) and the render: no metascan,
    // no whole-file read.
    CHECK(mock().transfers_mock().range_reads() == 2);
    CHECK(mock().transfers_mock().download_destinations().empty());
}

TEST_CASE_METHOD(ArchivePanel, "a .3mf the printer has metadata for keeps it",
                 "[print_select][three_mf]") {
    ScopedEnv native("HELIX_MOCK_3MF_METADATA", "1");
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());

    // The mock reports no thumbnail for it, so only the render comes from the
    // archive; the plate is the printer's to choose.
    CHECK_FALSE(row.archive_extract);
    CHECK(row.thumbnail_path.find(".bin") != std::string::npos);

    select_file(file.name());
    CHECK(plate_count() == 0);
    REQUIRE(prepare_archive_start());
    auto* prep = prep_manager();
    REQUIRE(prep);
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 0);
}

TEST_CASE_METHOD(ArchivePanel,
                 "the plate stepper walks the sliced plates and Print takes the shown one",
                 "[print_select][three_mf]") {
    PlantedGcode file("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    load(file.name());
    select_file(file.name());
    drain();

    CHECK(plate_count() == 2);
    CHECK(plate_text() == "Plate 2");
    CHECK(plate_position() == "1/2");
    const std::string first_time = selected_print_time();

    auto* prep = prep_manager();
    REQUIRE(prep);
    REQUIRE(prepare_archive_start());
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 2);

    // The card is about the whole file: both plates added up.
    const PrintFileData* row = find_file(file.name());
    REQUIRE(row);
    CHECK(row->print_time_minutes == (5238 + 1800) / 60);
    CHECK(row->filament_grams == Catch::Approx(23.73f + 12.5f));

    step_plate(1);
    drain();
    CHECK(plate_text() == "Plate 5");
    CHECK(plate_position() == "2/2");
    row = find_file(file.name());
    REQUIRE(row);
    CHECK(row->print_time_minutes == (5238 + 1800) / 60);
    REQUIRE(row->filament_types.size() == 2);
    CHECK(row->filament_types[1] == "PLA");
    CHECK(selected_print_time() != first_time);
    // The plate's own filaments reach the mapping card and the start.
    CHECK(selected_colors() == std::vector<std::string>{"#000000", "#FF0000"});
    REQUIRE(prepare_archive_start());
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 5);

    // Both ways round, wrapping.
    step_plate(1);
    CHECK(plate_text() == "Plate 2");
    CHECK(plate_position() == "1/2");
    step_plate(-1);
    CHECK(plate_text() == "Plate 5");
    CHECK(plate_position() == "2/2");
}

TEST_CASE_METHOD(ArchivePanel, "a reprint of a plate opens the archive already read on that plate",
                 "[print_select][three_mf][reprint]") {
    PlantedGcode file("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    load(file.name());
    select_file(file.name());
    drain();
    REQUIRE(plate_text() == "Plate 2");
    REQUIRE(plate_position() == "1/2");
    REQUIRE(selected_colors() != std::vector<std::string>{"#000000", "#FF0000"});

    REQUIRE(panel_->select_file_by_name(file.name(), 5));
    drain();
    CHECK(plate_text() == "Plate 5");
    CHECK(plate_position() == "2/2");
    CHECK(selected_colors() == std::vector<std::string>{"#000000", "#FF0000"});
    const PrintFileData* row = find_file(file.name());
    REQUIRE(row);
    REQUIRE(row->filament_types.size() == 2);
    CHECK(row->filament_types[1] == "PLA");
    auto* prep = prep_manager();
    REQUIRE(prep);
    REQUIRE(prepare_archive_start());
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 5);
}

TEST_CASE_METHOD(ArchivePanel, "the plate stepper shows a plate's name from model_settings",
                 "[print_select][three_mf][model_settings]") {
    const std::string settings = R"(<config>
  <plate><metadata key="plater_id" value="1"/><metadata key="plater_name" value="Hull"/></plate>
  <plate><metadata key="plater_id" value="2"/><metadata key="plater_name" value=""/></plate>
</config>)";
    PlantedGcode file("Named.gcode.3mf", "",
                      helix::test::stored_zip({{"Metadata/plate_1.gcode", "G28\n"},
                                               {"Metadata/plate_2.gcode", "G28\n"},
                                               {"Metadata/model_settings.config", settings}}));
    load(file.name());
    select_file(file.name());
    drain();
    CHECK(plate_text() == "Hull");
    CHECK(plate_position() == "1/2");
    // A plate with an empty name keeps its number.
    step_plate(1);
    CHECK(plate_text() == "Plate 2");
    CHECK(plate_position() == "2/2");
}

TEST_CASE_METHOD(ArchivePanel, "one sliced plate shows its number and no arrows",
                 "[print_select][three_mf]") {
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    load(file.name());
    select_file(file.name());
    CHECK(plate_count() == 1);
    CHECK(plate_text() == "Plate 9");
    CHECK(plate_position().empty());
    REQUIRE(prepare_archive_start());
    CHECK(prep_manager()->archive_route(file.name(), print_3mf::Rewrite::None).plate == 9);
}

TEST_CASE_METHOD(ArchivePanel,
                 "a .3mf with nothing sliced, or on a board with no local copies, "
                 "does not start",
                 "[print_select][three_mf]") {
    SECTION("a project") {
        PlantedGcode file("Project.3mf", "", fixture_bytes("project.3mf"));
        load(file.name());
        select_file(file.name());
        CHECK_FALSE(prepare_archive_start());
    }
    SECTION("no local copies") {
        PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
        load(file.name());
        mock().transfers_mock().mock_no_local_copies();
        select_file(file.name());
        CHECK_FALSE(prepare_archive_start());
    }
}

TEST_CASE_METHOD(ArchivePanel, "a .3mf the printer converted hides behind its G-code",
                 "[print_select][three_mf][conversions]") {
    PlantedGcode archive("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    PlantedGcode output("Door.gcode", "", "; converted\nG28\n");
    struct stat st {};
    REQUIRE(::stat(archive.path().c_str(), &st) == 0);

    const std::string json_path = std::filesystem::temp_directory_path().string() +
                                  "/helix_3mf_conversions_" + std::to_string(::getpid()) + ".json";
    auto write_conversions = [&](const char* state, uint64_t size) {
        std::ofstream out(json_path);
        out << R"({"conversions":[{"source":"Door.gcode.3mf","source_mtime":)"
            << std::to_string(static_cast<double>(st.st_mtime)) << R"(,"source_size":)" << size
            << R"(,"plate":9,"output":"Door.gcode","state":")" << state << R"("}]})";
    };
    ScopedEnv conversions("HELIX_MOCK_3MF_CONVERSIONS", json_path.c_str());

    auto listed = [&]() {
        panel_->refresh_files(true);
        drain();
        return PrintSelectPanelTestAccess::list_contains(*panel_, archive.name());
    };

    SECTION("done, unchanged") {
        write_conversions("done", static_cast<uint64_t>(st.st_size));
        CHECK_FALSE(listed());
        CHECK(PrintSelectPanelTestAccess::list_contains(*panel_, output.name()));
    }
    SECTION("pending") {
        write_conversions("pending", static_cast<uint64_t>(st.st_size));
        CHECK(listed());
    }
    SECTION("the archive changed since") {
        write_conversions("done", static_cast<uint64_t>(st.st_size) + 1);
        CHECK(listed());
    }
    SECTION("the output is gone") {
        write_conversions("done", static_cast<uint64_t>(st.st_size));
        output.remove_from_disk();
        CHECK(listed());
    }
    SECTION("no converter on the printer") {
        ScopedEnv none("HELIX_MOCK_3MF_CONVERSIONS", nullptr);
        CHECK(listed());
    }
    std::remove(json_path.c_str());
}

TEST_CASE_METHOD(ArchivePanel, "a second size of a .3mf render keeps the card's prescaled image",
                 "[print_select][three_mf][thumbnail]") {
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());
    REQUIRE(row.archive);
    const std::string card = row.thumbnail_path;
    REQUIRE(card.rfind("A:", 0) == 0);
    REQUIRE(std::filesystem::exists(card.substr(2)));
    const int reads = mock().transfers_mock().range_reads();

    // Print status asks for the same render at its own, larger size.
    std::string detail;
    const auto* plate = three_mf::find_plate(*row.archive, 0);
    REQUIRE(plate);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<uint32_t> gen{0};
    helix::fetch_thumbnail_from_archive(
        file.name(), *three_mf::plate_thumbnail(*plate), row.modified_timestamp, api_.get(),
        helix::ThumbnailProcessor::get_target_for_display(helix::ThumbnailSize::Detail),
        ThumbnailLoadContext::capture(alive, &gen),
        [&detail](const std::string& path, bool) { detail = path; }, nullptr);
    drain();

    CHECK_FALSE(detail.empty());
    CHECK(detail != card);
    CHECK(std::filesystem::exists(card.substr(2)));
    // Prescaled from the cached PNG, not read again.
    CHECK(mock().transfers_mock().range_reads() == reads);
}

TEST_CASE_METHOD(ArchivePanel, "a plate the printer already converted prints from its output",
                 "[print_select][three_mf][conversions]") {
    PlantedGcode archive("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    PlantedGcode output("Set_plate2.gcode", "", "; converted\nG28\n");
    struct stat st {};
    REQUIRE(::stat(archive.path().c_str(), &st) == 0);
    const std::string json_path = std::filesystem::temp_directory_path().string() +
                                  "/helix_3mf_outputs_" + std::to_string(::getpid()) + ".json";
    {
        const std::string mtime = std::to_string(static_cast<double>(st.st_mtime));
        const std::string size = std::to_string(st.st_size);
        std::ofstream out(json_path);
        out << R"({"conversions":[{"source":"Set.gcode.3mf","source_mtime":)" << mtime
            << R"(,"source_size":)" << size
            << R"(,"plate":2,"output":"Set_plate2.gcode","state":"done"},)"
            << R"({"source":"Set.gcode.3mf","source_mtime":)" << mtime << R"(,"source_size":)"
            << size << R"(,"plate":5,"output":"","state":"pending"}]})";
    }
    ScopedEnv conversions("HELIX_MOCK_3MF_CONVERSIONS", json_path.c_str());

    // Plate 5 is not converted yet, so the archive stays listed.
    load(archive.name());
    select_file(archive.name());
    auto* prep = prep_manager();
    REQUIRE(prep);

    SECTION("the converted plate needs no local copy") {
        mock().transfers_mock().mock_no_local_copies();
        REQUIRE(prepare_archive_start());
        CHECK(prep->printable_path(archive.name()) == "Set_plate2.gcode");
        CHECK(prep->archive_route(archive.name(), print_3mf::Rewrite::None).plate == 0);
    }
    SECTION("the plate still pending is extracted") {
        step_plate(1);
        REQUIRE(prepare_archive_start());
        CHECK(prep->printable_path(archive.name()) == archive.name());
        CHECK(prep->archive_route(archive.name(), print_3mf::Rewrite::None).plate == 5);
    }
    SECTION("the queue takes the converted plate's output, and refuses the pending one") {
        CHECK(queueable_path() == std::optional<std::string>("Set_plate2.gcode"));
        step_plate(1);
        CHECK_FALSE(queueable_path());
    }
    std::remove(json_path.c_str());
}

TEST_CASE_METHOD(ArchivePanel, "the detail view shows the plate while the card adds up the file",
                 "[print_select][three_mf]") {
    PlantedGcode file("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    load(file.name());
    select_file(file.name());
    const std::string plate2 = selected_print_time();
    step_plate(1);
    const std::string plate5 = selected_print_time();
    const PrintFileData* row = find_file(file.name());
    REQUIRE(row);
    // Three different numbers: plate 2, plate 5, and the card's total.
    CHECK(plate2 != plate5);
    CHECK(plate2 != row->print_time_str);
    CHECK(plate5 != row->print_time_str);
}

TEST_CASE_METHOD(ArchivePanel,
                 "a .3mf the printer describes keeps its metadata and gets only "
                 "what it left out",
                 "[print_select][three_mf][native]") {
    // A printer that answers metadata for its .3mf files.
    ScopedEnv native("HELIX_MOCK_3MF_METADATA", "1");
    PlantedGcode file("Q2.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    panel_->refresh_files(true);
    drain();
    REQUIRE(find_file(file.name()));

    FileMetadata meta;
    meta.filename = file.name();
    SECTION("time present, thumbnails empty") {
        meta.estimated_time = 600;
        meta.filament_weight_total = 5.0;
        meta.filament_type = "PETG";
        apply_metadata(file.name(), meta);
        drain();
        const PrintFileData* row = find_file(file.name());
        REQUIRE(row);
        CHECK(row->thumbnail_path.find(".bin") != std::string::npos);
        CHECK(row->print_time_minutes == 10);
        CHECK(row->filament_grams == Catch::Approx(5.0f));
        CHECK(row->filament_type == "PETG");
        CHECK_FALSE(row->archive_extract);
    }
    SECTION("no time either") {
        apply_metadata(file.name(), meta);
        drain();
        const PrintFileData* row = find_file(file.name());
        REQUIRE(row);
        CHECK(row->thumbnail_path.find(".bin") != std::string::npos);
        CHECK(row->print_time_minutes == 5238 / 60);
        CHECK(row->filament_grams == Catch::Approx(23.73f));
        CHECK(row->filament_type == "ABS");
        CHECK_FALSE(row->archive_extract);
    }
    // Either way it prints as the .3mf itself.
    select_file(file.name());
    REQUIRE(prepare_archive_start());
    auto* prep = prep_manager();
    REQUIRE(prep);
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 0);
    CHECK(prep->printable_path(file.name()) == file.name());
}

TEST_CASE_METHOD(ArchivePanel, "a .3mf with no conversion is not queued; any other file is",
                 "[print_select][three_mf][queue]") {
    PlantedGcode archive("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    load(archive.name());
    select_file(archive.name());
    CHECK_FALSE(queueable_path());

    PlantedGcode plain("plain.gcode");
    panel_->refresh_files(true);
    drain();
    select_file(plain.name());
    CHECK(queueable_path() == std::optional<std::string>(plain.name()));
}

TEST_CASE_METHOD(ArchivePanel, "a .3mf the printer reads itself queues as itself",
                 "[print_select][three_mf][queue]") {
    ScopedEnv native("HELIX_MOCK_3MF_METADATA", "1");
    PlantedGcode archive("Q2.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    load(archive.name());
    select_file(archive.name());
    CHECK(queueable_path() == std::optional<std::string>(archive.name()));
}

TEST_CASE_METHOD(ArchivePanel, "a refresh stops the archive reads under way and asks again later",
                 "[print_select][three_mf][cancel]") {
    PlantedGcode file("Door.gcode.3mf", "", fixture_bytes("single_plate.gcode.3mf"));
    auto& transfers = mock().transfers_mock();
    transfers.mock_hold_range_reads();
    panel_->refresh_files(true);
    drain();
    PrintSelectPanelTestAccess::fetch_metadata(*panel_, file.name());
    drain();
    const size_t before = transfers.held_range_reads();
    REQUIRE(before > 0);

    panel_->refresh_files(true);
    drain();
    transfers.release_held_range_reads(before);
    drain();
    const PrintFileData* row = find_file(file.name());
    REQUIRE(row);
    CHECK_FALSE(row->archive);

    // Asked for again, it reads.
    transfers.mock_hold_range_reads(false);
    transfers.release_held_range_reads();
    PrintSelectPanelTestAccess::fetch_metadata(*panel_, file.name());
    drain();
    row = find_file(file.name());
    REQUIRE(row);
    CHECK(row->archive);
}

TEST_CASE_METHOD(ArchivePanel,
                 "a .3mf whose printer lists its plates steps them and Print names the shown one",
                 "[print_select][three_mf][native_3mf]") {
    ScopedEnv plates("HELIX_MOCK_3MF_PLATES", "1");
    PlantedGcode file("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());
    CHECK(row.archive_native);
    CHECK_FALSE(row.archive_extract);
    // The plate the printer's metadata describes (its first sliced one) shows first.
    CHECK(row.archive_native_plate == 2);

    select_file(file.name());
    drain();
    CHECK(plate_count() == 2);
    CHECK(plate_text() == "Plate 2");

    auto* prep = prep_manager();
    REQUIRE(prep);
    REQUIRE(prepare_archive_start());
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 2);
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).kind ==
          print_3mf::RouteKind::NativePlate);
    // The default plate is what a queued start, which names none, prints.
    CHECK(queueable_path() == std::optional<std::string>(file.name()));

    step_plate(1);
    drain();
    CHECK(plate_text() == "Plate 5");
    CHECK(selected_colors() == std::vector<std::string>{"#000000", "#FF0000"});
    REQUIRE(prepare_archive_start());
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 5);
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).kind ==
          print_3mf::RouteKind::NativePlate);
    CHECK_FALSE(queueable_path());
}

TEST_CASE_METHOD(ArchivePanel,
                 "a .3mf whose printer metadata lists no plates starts the archive naming no plate",
                 "[print_select][three_mf][native_3mf]") {
    ScopedEnv native("HELIX_MOCK_3MF_METADATA", "1");
    PlantedGcode file("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    const PrintFileData& row = load(file.name());
    CHECK_FALSE(row.archive_native);
    select_file(file.name());
    drain();
    CHECK(plate_count() == 0);
    REQUIRE(prepare_archive_start());
    auto* prep = prep_manager();
    REQUIRE(prep);
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).plate == 0);
    CHECK(prep->archive_route(file.name(), print_3mf::Rewrite::None).kind ==
          print_3mf::RouteKind::AsFile);
}

TEST_CASE_METHOD(ArchivePanel, "the plate stepper offers only the plates the printer lists",
                 "[print_select][three_mf][native_3mf]") {
    ScopedEnv native("HELIX_MOCK_3MF_METADATA", "1");
    PlantedGcode file("Set.gcode.3mf", "", fixture_bytes("multi_plate.gcode.3mf"));
    panel_->refresh_files(true);
    drain();
    FileMetadata meta;
    meta.filename = file.name();
    meta.estimated_time = 600;
    meta.sliced_plates = std::vector<int>{5};
    meta.plate_index = 5;
    apply_metadata(file.name(), meta);
    drain();
    select_file(file.name());
    drain();

    // The archive holds plates 2 and 5; the printer starts only 5, its default.
    CHECK(plate_count() == 1);
    CHECK(plate_text() == "Plate 5");
    step_plate(1);
    drain();
    CHECK(plate_text() == "Plate 5");
    REQUIRE(prepare_archive_start());
    auto* prep = prep_manager();
    REQUIRE(prep);
    const print_3mf::Route route = prep->archive_route(file.name(), print_3mf::Rewrite::None);
    CHECK(route.kind == print_3mf::RouteKind::NativePlate);
    CHECK(route.plate == 5);
}
