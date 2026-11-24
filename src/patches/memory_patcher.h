// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "main/config.h"
#include "orbis/core/module.h"

namespace Gow3 {
struct MemoryPatch {
    u64 rva{};
    std::vector<u8> expected;
    std::vector<u8> replacement;
};
class MemoryPatcher {
public:
    explicit MemoryPatcher(Core::Module& module) : module(module) {}
    void Apply(std::span<const MemoryPatch> patches);
private:
    Core::Module& module;
};
// Startup EBOOT patches for native save capacity and configured compatibility settings.
void ApplyGamePatches(Core::Module& eboot, const Config& config);
// Cheats chosen in the menu: code patches found by their bytes, so they follow the game code wherever the loader put it.
void ApplyCheats(Core::Module& eboot, const Config& config);
// The file holds version 1.02's code (European and American) where the patches go: checked before the window opens.
bool IsSupportedEboot(const std::filesystem::path& path);
}
