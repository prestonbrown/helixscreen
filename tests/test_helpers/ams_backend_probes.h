// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ams_backend_probes.h
 * @brief One minimal, connection-free instance of each AMS backend.
 *
 * Every backend takes an api and a client pointer it only dereferences when it
 * talks to Moonraker, so passing nullptr for both gives a real backend object
 * that answers its own declarations - capabilities, strategy, readiness, the
 * static parts of its status model - without a printer on the other end.
 *
 * That one line was hand-copied into eight test files, which is a copy-paste
 * pattern rather than a shared decision, and the copies had already drifted
 * apart in name (AfcProbe, AfcCapabilityProbe, AfcCharHelperForClassify) while
 * being identical in body.
 *
 * NOT for a probe that needs more: a fixture flipping `running_`, seeding
 * status, or injecting a store is a different helper that happens to share a
 * base class, and belongs in the file that needs it.
 */

#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"

class AfcProbe : public helix::AmsBackendAfc {
  public:
    AfcProbe() : helix::AmsBackendAfc(nullptr, nullptr) {}
};

class HappyHareProbe : public helix::AmsBackendHappyHare {
  public:
    HappyHareProbe() : helix::AmsBackendHappyHare(nullptr, nullptr) {}
};

class CfsProbe : public helix::printer::AmsBackendCfs {
  public:
    CfsProbe() : helix::printer::AmsBackendCfs(nullptr, nullptr) {}
};

class Ad5xIfsProbe : public helix::AmsBackendAd5xIfs {
  public:
    using helix::AmsBackendAd5xIfs::AmsBackendAd5xIfs;
    Ad5xIfsProbe() : helix::AmsBackendAd5xIfs(nullptr, nullptr) {}
};

class ToolChangerProbe : public helix::AmsBackendToolChanger {
  public:
    ToolChangerProbe() : helix::AmsBackendToolChanger(nullptr, nullptr) {}
};

class SnapmakerProbe : public helix::AmsBackendSnapmaker {
  public:
    SnapmakerProbe() : helix::AmsBackendSnapmaker(nullptr, nullptr) {}
};

class AceProbe : public helix::AmsBackendAce {
  public:
    AceProbe() : helix::AmsBackendAce(nullptr, nullptr) {}
};

class QidiProbe : public helix::AmsBackendQidi {
  public:
    QidiProbe() : helix::AmsBackendQidi(nullptr, nullptr) {}
};
