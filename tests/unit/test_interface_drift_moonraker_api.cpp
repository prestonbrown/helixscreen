// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-only drift protection: if IMoonrakerAPI gains a pure-virtual method
// and neither MoonrakerAPI (the real implementation) nor MoonrakerAPIMock
// provides it, MoonrakerAPIMock becomes abstract and this fails to build.
//
// Also pins the ten Moonraker sub-API interfaces (i_moonraker_sub_apis.h):
// each concrete sub-API class must derive from its matching interface and
// implement every pure virtual (non-abstract), or this fails to build.

#include "i_moonraker_api.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_advanced_api.h"
#include "moonraker_file_api.h"
#include "moonraker_file_transfer_api.h"
#include "moonraker_history_api.h"
#include "moonraker_job_api.h"
#include "moonraker_motion_api.h"
#include "moonraker_queue_api.h"
#include "moonraker_rest_api.h"
#include "moonraker_spoolman_api.h"
#include "moonraker_timelapse_api.h"

#include <type_traits>

#include "../catch_amalgamated.hpp"

TEST_CASE("Moonraker sub-API classes satisfy their interfaces", "[compile][drift]") {
    static_assert(std::is_base_of_v<IMotionAPI, MoonrakerMotionAPI>,
                  "MoonrakerMotionAPI must derive from IMotionAPI");
    static_assert(!std::is_abstract_v<MoonrakerMotionAPI>,
                  "MoonrakerMotionAPI must implement every pure virtual from IMotionAPI");

    static_assert(std::is_base_of_v<IJobAPI, MoonrakerJobAPI>,
                  "MoonrakerJobAPI must derive from IJobAPI");
    static_assert(!std::is_abstract_v<MoonrakerJobAPI>,
                  "MoonrakerJobAPI must implement every pure virtual from IJobAPI");

    static_assert(std::is_base_of_v<IFilesAPI, MoonrakerFileAPI>,
                  "MoonrakerFileAPI must derive from IFilesAPI");
    static_assert(!std::is_abstract_v<MoonrakerFileAPI>,
                  "MoonrakerFileAPI must implement every pure virtual from IFilesAPI");

    static_assert(std::is_base_of_v<IQueueAPI, MoonrakerQueueAPI>,
                  "MoonrakerQueueAPI must derive from IQueueAPI");
    static_assert(!std::is_abstract_v<MoonrakerQueueAPI>,
                  "MoonrakerQueueAPI must implement every pure virtual from IQueueAPI");

    static_assert(std::is_base_of_v<IHistoryAPI, MoonrakerHistoryAPI>,
                  "MoonrakerHistoryAPI must derive from IHistoryAPI");
    static_assert(!std::is_abstract_v<MoonrakerHistoryAPI>,
                  "MoonrakerHistoryAPI must implement every pure virtual from IHistoryAPI");

    static_assert(std::is_base_of_v<IAdvancedAPI, MoonrakerAdvancedAPI>,
                  "MoonrakerAdvancedAPI must derive from IAdvancedAPI");
    static_assert(!std::is_abstract_v<MoonrakerAdvancedAPI>,
                  "MoonrakerAdvancedAPI must implement every pure virtual from IAdvancedAPI");

    static_assert(std::is_base_of_v<IRestAPI, MoonrakerRestAPI>,
                  "MoonrakerRestAPI must derive from IRestAPI");
    static_assert(!std::is_abstract_v<MoonrakerRestAPI>,
                  "MoonrakerRestAPI must implement every pure virtual from IRestAPI");

    static_assert(std::is_base_of_v<ITransfersAPI, MoonrakerFileTransferAPI>,
                  "MoonrakerFileTransferAPI must derive from ITransfersAPI");
    static_assert(!std::is_abstract_v<MoonrakerFileTransferAPI>,
                  "MoonrakerFileTransferAPI must implement every pure virtual from ITransfersAPI");

    static_assert(std::is_base_of_v<ISpoolmanAPI, MoonrakerSpoolmanAPI>,
                  "MoonrakerSpoolmanAPI must derive from ISpoolmanAPI");
    static_assert(!std::is_abstract_v<MoonrakerSpoolmanAPI>,
                  "MoonrakerSpoolmanAPI must implement every pure virtual from ISpoolmanAPI");

    static_assert(std::is_base_of_v<ITimelapseAPI, MoonrakerTimelapseAPI>,
                  "MoonrakerTimelapseAPI must derive from ITimelapseAPI");
    static_assert(!std::is_abstract_v<MoonrakerTimelapseAPI>,
                  "MoonrakerTimelapseAPI must implement every pure virtual from ITimelapseAPI");

    SUCCEED("All ten Moonraker sub-API interfaces ↔ concrete class pairs verified at compile time");
}

// Pin the sub-API accessor return types: IMoonrakerAPI must hand back the
// interface reference (not a concrete sub-API), so consumers depend only on the
// interface surface. The concrete MoonrakerAPI covariantly overrides these to
// return the concrete sub-APIs — that is verified by the mock-parity case below.
TEST_CASE("IMoonrakerAPI sub-API accessors return interface references", "[compile][drift]") {
    static_assert(
        std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().advanced()), IAdvancedAPI&>,
        "IMoonrakerAPI::advanced() must return IAdvancedAPI&");
    static_assert(
        std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().transfers()), ITransfersAPI&>,
        "IMoonrakerAPI::transfers() must return ITransfersAPI&");
    static_assert(std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().history()), IHistoryAPI&>,
                  "IMoonrakerAPI::history() must return IHistoryAPI&");
    static_assert(std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().job()), IJobAPI&>,
                  "IMoonrakerAPI::job() must return IJobAPI&");
    static_assert(
        std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().timelapse()), ITimelapseAPI&>,
        "IMoonrakerAPI::timelapse() must return ITimelapseAPI&");
    static_assert(std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().motion()), IMotionAPI&>,
                  "IMoonrakerAPI::motion() must return IMotionAPI&");
    static_assert(std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().rest()), IRestAPI&>,
                  "IMoonrakerAPI::rest() must return IRestAPI&");
    static_assert(
        std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().spoolman()), ISpoolmanAPI&>,
        "IMoonrakerAPI::spoolman() must return ISpoolmanAPI&");
    static_assert(std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().files()), IFilesAPI&>,
                  "IMoonrakerAPI::files() must return IFilesAPI&");
    static_assert(std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().queue()), IQueueAPI&>,
                  "IMoonrakerAPI::queue() must return IQueueAPI&");
    static_assert(std::is_same_v<decltype(std::declval<IMoonrakerAPI&>().get_client()),
                                 helix::IMoonrakerClient&>,
                  "IMoonrakerAPI::get_client() must return helix::IMoonrakerClient&");
    SUCCEED("IMoonrakerAPI accessor return types pinned to interface references");
}

TEST_CASE("IFilesAPI exposes the file manager's root list", "[compile][drift]") {
    // server.files.roots is the only call that names the writable config directory
    // in absolute terms, which is what lets an absolute [include] be judged reachable
    // (stock Creality K2). It must stay on the interface, not just the concrete class,
    // or every consumer would have to name MoonrakerFileAPI to reach it.
    static_assert(
        std::is_same_v<decltype(std::declval<IFilesAPI&>().get_file_roots(
                           std::declval<IFilesAPI::FileRootsCallback>(),
                           std::declval<IFilesAPI::ErrorCallback>())),
                       void>,
        "IFilesAPI::get_file_roots(FileRootsCallback, ErrorCallback) must exist and return void");
    static_assert(
        std::is_same_v<IFilesAPI::FileRootsCallback,
                       std::function<void(const std::vector<FileRoot>&)>>,
        "IFilesAPI::FileRootsCallback must deliver parsed FileRoot entries, not raw JSON");
    SUCCEED("IFilesAPI::get_file_roots pinned");
}

#ifdef HELIX_ENABLE_MOCKS
#include "moonraker_api_mock.h"

TEST_CASE("MoonrakerAPIMock satisfies IMoonrakerAPI interface", "[compile][drift]") {
    static_assert(std::is_base_of_v<IMoonrakerAPI, MoonrakerAPIMock>,
                  "MoonrakerAPIMock must derive from IMoonrakerAPI");
    static_assert(!std::is_abstract_v<MoonrakerAPIMock>,
                  "MoonrakerAPIMock must implement every pure virtual from IMoonrakerAPI");
    SUCCEED("IMoonrakerAPI ↔ MoonrakerAPIMock parity verified at compile time");
}
#endif
