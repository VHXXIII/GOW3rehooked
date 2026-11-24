// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <thread>
#include <chrono>
#include "patches/memory_patcher.h"
#include "main/host.h"
#include "orbis/core/loader/elf.h"
#include "orbis/core/memory.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace Gow3 {
void MemoryPatcher::Apply(std::span<const MemoryPatch> patches) {
    for (size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        if (p.expected.empty() || p.expected.size() != p.replacement.size() ||
            !module.Contains(p.rva, p.expected.size()))
            throw std::runtime_error("Invalid patch range");
        if (std::memcmp(reinterpret_cast<const void*>(module.GetBaseAddress() + p.rva),
                        p.expected.data(), p.expected.size()))
            throw std::runtime_error("Patch original bytes do not match");
        for (size_t j = 0; j < i; ++j) {
            const auto& other = patches[j];
            if (p.rva < other.rva + other.expected.size() && other.rva < p.rva + p.expected.size())
                throw std::runtime_error("Overlapping patches");
        }
    }
    // Startup-only: write the loader's backing allocation without changing guest permissions.
    try {
        for (const auto& p : patches)
            if (!module.memory->TryWriteBacking(reinterpret_cast<void*>(module.GetBaseAddress() + p.rva),
                                                 p.replacement.data(), p.replacement.size()))
                throw std::runtime_error("Patch range has no writable loader backing");
#ifdef _WIN32
        if (!FlushInstructionCache(GetCurrentProcess(), nullptr, 0))
            throw std::runtime_error(HostError("Publish memory patches"));
#endif
    } catch (...) {
        for (const auto& p : patches)
            module.memory->TryWriteBacking(reinterpret_cast<void*>(module.GetBaseAddress() + p.rva),
                                            p.expected.data(), p.expected.size());
#ifdef _WIN32
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
#endif
        throw;
    }
}

bool IsSupportedEboot(const std::filesystem::path& path) {
    struct Site {
        u64 rva;
        std::array<u8, 5> bytes;
        u32 size;
    };
    // Three of ApplyGamePatches' originals: the heap size, the save count and the save table size.
    static constexpr Site sites[]{{0x000db, {0x00, 0x00, 0x00, 0xb0}, 4}, {0x210a76, {0x83, 0x7f, 0x20, 0x1f}, 4},
                                  {0x21e3cd, {0xbf, 0x40, 0x04, 0x00, 0x00}, 5}};
    Core::Loader::Elf elf;
    elf.Open(path);
    if (!elf.IsElfFile()) return false;
    const auto headers = elf.GetProgramHeader();
    return std::ranges::all_of(sites, [&](const Site& s) {
        const auto h = std::ranges::find_if(headers, [&](const auto& h) {
            return h.p_type == PT_LOAD && s.rva >= h.p_vaddr && s.rva + s.size <= h.p_vaddr + h.p_filesz;
        });
        std::array<u8, 5> got{};
        if (h == headers.end()) return false;
        elf.LoadSegment(reinterpret_cast<u64>(got.data()), h->p_offset + (s.rva - h->p_vaddr), s.size);
        return std::memcmp(got.data(), s.bytes.data(), s.size) == 0;
    });
}


namespace {
// Cheats from Celogamez's CUSA01623 v01.02 GoldHEN cheats (the same code sites SpartanPort hooks). Each site is found by its bytes
// ("??" any), `at` being the distance from the match to the instruction the stub replaces.
struct CheatSite {
    const char* pattern;
    u32 at;
    std::vector<u8> original, replacement;  // a replacement with the original's size goes in place, a longer one into a stub
    bool bare = false;                      // the replacement touches neither the stack nor the flags: the stub needs no wrapper
    bool gated = false;                     // the stub starts disabled; a byte in it is set after a delay (see ApplyCheats)
};
struct Cheat {
    const char* name;
    bool Config::* enabled;
    std::vector<CheatSite> sites;
};
const std::vector<Cheat>& Cheats() {
    static const std::vector<Cheat> cheats{
        {"infinite health", &Config::cheat_health,
         {{"c4 c1 7a 11 85 cc 01 00 00 49 8b 45 00 4c 89 ef", 0, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0xcc, 0x01, 0, 0},
           {0x9c, 0x41, 0x80, 0xbd, 0x98, 0x02, 0, 0, 0, 0x75, 9, 0xc4, 0xc1, 0x7a, 0x10, 0x85, 0xd0, 0x01, 0, 0, 0x9d,
            0xc4, 0xc1, 0x7a, 0x11, 0x85, 0xcc, 0x01, 0, 0}, false, true}}},
        {"infinite magic and item meter", &Config::cheat_magic,
         {{"c5 fa 5f c2 c5 fa 5d c1 c5 fa 11 83 d0 01 00 00", 8, {0xc5, 0xfa, 0x11, 0x83, 0xd0, 0x01, 0, 0},
           {0xc5, 0xfa, 0x10, 0x83, 0xd0, 0x01, 0, 0}},  // the store becomes a load: nothing is written, xmm0 holds the old value
          {"c5 fa 5f c2 c5 fa 5d c1 c5 fa 11 87 d4 01 00 00 c3", 8, {0xc5, 0xfa, 0x11, 0x87, 0xd4, 0x01, 0, 0},
           {0xc5, 0xfa, 0x10, 0x87, 0xd4, 0x01, 0, 0}}}},
        {"max red orbs", &Config::cheat_orbs,
         {{"c5 fa 11 87 e8 01 00 00 48 8b 87 b0 01 00 00 a9 00 00 04 00", 0, {0xc5, 0xfa, 0x11, 0x87, 0xe8, 0x01, 0, 0},
           {0xc7, 0x87, 0xe8, 0x01, 0, 0, 0xf0, 0x23, 0x74, 0x49}, true}}},
        {"all difficulties", &Config::cheat_difficulties,
         {{"48 8b 08 b8 03 00 00 00 f6 c1 08 75 1b b8 02 00 00 00", 3, {0xb8, 3, 0, 0, 0, 0xf6, 0xc1, 8},
           {0xb8, 3, 0, 0, 0, 0x85, 0xc0, 0x90}}}},
    };
    return cheats;
}

// First match of a pattern in the module's executable segments, as a host address; 0 when there is none.
u64 FindPattern(Core::Module& module, const char* pattern) {
    std::vector<int> bytes;  // -1: any
    for (const char* c = pattern; *c;) {
        if (*c == ' ') { ++c; continue; }
        if (*c == '?') { bytes.push_back(-1); c += 2; continue; }
        bytes.push_back(int(std::strtoul(std::string(c, 2).c_str(), nullptr, 16)));
        c += 2;
    }
    const auto info = module.GetModuleInfo();
    for (u32 i = 0; i < info.num_segments && i < info.segments.size(); ++i) {
        const auto& seg = info.segments[i];
        if (!(seg.prot & 4) || seg.size < bytes.size()) continue;
        const auto* mem = reinterpret_cast<const u8*>(seg.address);
        for (u64 at = 0; at + bytes.size() <= seg.size; ++at) {
            size_t k = 0;
            while (k < bytes.size() && (bytes[k] < 0 || mem[at + k] == bytes[k])) ++k;
            if (k == bytes.size()) return seg.address + at;
        }
    }
    return 0;
}

// Room for a stub that a 32-bit jump reaches: a run of 0xCC padding between functions, else the zero fill at the end of the
// executable segment. `fill` is the byte that is there now.
u64 FindCave(Core::Module& module, u32 size, const std::vector<u64>& claimed, u8& fill) {
    const auto info = module.GetModuleInfo();
    const auto free = [&](u64 start) {
        return module.Contains(start - module.GetBaseAddress(), size) &&  // inside the mapped image, not just the segment's page rounding
               std::ranges::none_of(claimed, [&](u64 c) { return c < start + size && start < c + 128; });
    };
    for (u32 i = 0; i < info.num_segments && i < info.segments.size(); ++i) {
        const auto& seg = info.segments[i];
        if (!(seg.prot & 4)) continue;
        const auto* mem = reinterpret_cast<const u8*>(seg.address);
        for (u64 at = 0; at + size + 16 <= seg.size;) {
            if (mem[at] != 0xcc) { ++at; continue; }
            u64 end = at;
            while (end < seg.size && mem[end] == 0xcc) ++end;
            for (u64 start = (seg.address + at + 15) & ~15ull; start + size <= seg.address + end; start += 16)  // 16-byte aligned, inside the run
                if (free(start)) return fill = 0xcc, start;
            at = end;
        }
        u64 tail = seg.size;  // zero fill after the last code
        while (tail && mem[tail - 1] == 0) --tail;
        for (u64 start = (seg.address + tail + 15) & ~15ull; start + size <= seg.address + seg.size; start += 16)
            if (free(start)) return fill = 0, start;
    }
    return 0;
}
}  // namespace

void ApplyCheats(Core::Module& eboot, const Config& config) {
    std::vector<MemoryPatch> patches;
    std::string report;
    const u64 base = eboot.GetBaseAddress();
    std::vector<u64> claimed;
    u64 gate = 0;  // the gate byte of the health stub
    {
        const auto info = eboot.GetModuleInfo();
        std::string seg_report;
        for (u32 i = 0; i < info.num_segments && i < info.segments.size(); ++i)
            seg_report += std::format(" [{:x}+{:x} prot {}]", info.segments[i].address - base, info.segments[i].size, info.segments[i].prot);
        Log("Cheats: module segments" + seg_report);
    }
    for (const auto& cheat : Cheats()) {
        if (!(config.*cheat.enabled)) continue;
        std::vector<MemoryPatch> mine;
        bool ok = true;
        std::string reason = "bytes not found in the game code";
        for (const auto& site : cheat.sites) {
            const u64 match = FindPattern(eboot, site.pattern);
            const u64 address = match + site.at;
            if (!match || !eboot.Contains(address - base, site.original.size()) ||
                std::memcmp(reinterpret_cast<const void*>(address), site.original.data(), site.original.size())) {
                ok = false;
                break;
            }
            if (site.replacement.size() == site.original.size()) {
                mine.push_back({address - base, site.original, site.replacement});
                continue;
            }
            // The stub: the replacement code, then the jump back after the patched instruction (a wrapper keeps the red zone and flags).
            u8 fill = 0;
            const u64 stub = FindCave(eboot, site.bare ? 16 : 64, claimed, fill);
            if (!stub) { reason = "no free code space"; ok = false; break; }
            claimed.push_back(stub);
            Log(std::format("Cheats[gated-v2]: {} stub at {:x} (fill {:02x}), site at {:x}", cheat.name, stub - base, fill, address - base));
            const u64 resume = address + site.original.size();
            std::vector<u8> code;
            if (!site.bare) code = {0x48, 0x8d, 0x64, 0x24, 0x80};  // lea rsp, [rsp-0x80]
            if (site.gated) {
                // pushfq; cmp byte [rip+gate], 0; je <popfq>; then the replacement without its own pushfq (its popfq is the target)
                const u32 gate_at = u32(code.size()) + 1 + 7 + 2 + (u32(site.replacement.size()) - 1) + 8 + 5;
                const u32 cmp_end = u32(code.size()) + 1 + 7;
                const std::int32_t gate_rel = std::int32_t(gate_at - cmp_end);
                code.push_back(0x9c);
                code.insert(code.end(), {0x80, 0x3d});
                code.insert(code.end(), reinterpret_cast<const u8*>(&gate_rel), reinterpret_cast<const u8*>(&gate_rel) + 4);
                code.push_back(0);
                code.insert(code.end(), {0x74, u8(site.replacement.size() - 2 - site.original.size())});
                code.insert(code.end(), site.replacement.begin() + 1, site.replacement.end());
                gate = stub + gate_at;
            } else
            code.insert(code.end(), site.replacement.begin(), site.replacement.end());
            if (!site.bare) code.insert(code.end(), {0x48, 0x8d, 0xa4, 0x24, 0x80, 0, 0, 0});  // lea rsp, [rsp+0x80]
            const std::int64_t back = std::int64_t(resume) - std::int64_t(stub + code.size() + 5);
            if (back < INT32_MIN || back > INT32_MAX) { reason = "stub out of reach"; ok = false; break; }
            code.push_back(0xe9);
            const std::int32_t back32 = std::int32_t(back);
            code.insert(code.end(), reinterpret_cast<const u8*>(&back32), reinterpret_cast<const u8*>(&back32) + 4);
            const std::int64_t delta = std::int64_t(stub) - std::int64_t(address + 5);
            if (site.gated) code.push_back(0);  // the gate byte, after the jump back
            std::vector<u8> jump{0xe9};
            const std::int32_t rel = std::int32_t(delta);
            jump.insert(jump.end(), reinterpret_cast<const u8*>(&rel), reinterpret_cast<const u8*>(&rel) + 4);
            jump.resize(site.original.size(), 0x90);
            mine.push_back({stub - base, std::vector<u8>(code.size(), fill), std::move(code)});
            mine.push_back({address - base, site.original, std::move(jump)});
        }
        report += std::format("{}{}: {}", report.empty() ? "" : "; ", cheat.name, ok ? "on" : reason);
        if (ok) patches.insert(patches.end(), mine.begin(), mine.end());
    }
    if (patches.empty()) {
        if (!report.empty()) Log("Cheats: " + report);
        return;
    }
    MemoryPatcher(eboot).Apply(patches);
    Log("Cheats: " + report);
    if (gate) {
        // The health stub stays off while the engine initialises; it is opened later from a thread of its own.
        std::thread([&eboot, gate] {
            std::this_thread::sleep_for(std::chrono::seconds(45));
            const u8 one = 1;
            const bool ok = eboot.memory->TryWriteBacking(reinterpret_cast<void*>(gate), &one, 1);
            Log(ok ? "Cheats: health gate opened" : "Cheats: health gate could not be opened");
        }).detach();
    }
}

void ApplyGamePatches(Core::Module& eboot, const Config& config) {
    std::vector<MemoryPatch> patches;
    const auto value = [&](u64 rva, u32 original, u32 replacement, u32 size = 4) {
        const auto bytes = [size](u32 v) { return std::vector<u8>(reinterpret_cast<const u8*>(&v), reinterpret_cast<const u8*>(&v) + size); };
        patches.push_back({rva, bytes(original), bytes(replacement)});
    };
    // 64 manual saves plus AUTOSAVE/USERDATA; each directory name remains 32 bytes.
    patches.push_back({0x210a76, {0x83, 0x7f, 0x20, 0x1f}, {0x83, 0x7f, 0x20, 0x3f}});
    for (const u64 rva : {0x21e3cd, 0x21e42f})
        patches.push_back({rva, {0xbf, 0x40, 0x04, 0x00, 0x00}, {0xbf, 0x40, 0x08, 0x00, 0x00}});
    for (const u64 rva : {0x21e3e8, 0x21e446})
        patches.push_back({rva, {0xb9, 0x22, 0x00, 0x00, 0x00}, {0xb9, 0x42, 0x00, 0x00, 0x00}});
    // User Malloc (General Heap) reduction from 2816 MiB (0xB0000000) to 1888 MiB (0x76000000).
    // Verified in Ghidra at FUN_010000a0 and FUN_010002f0 (allocate, map, mspace create).
    for (const u64 rva : {0x000db, 0x00115, 0x0013c, 0x0031a, 0x0034f, 0x0037a}) {
        value(rva, 0xb0000000, 0x76000000);
    }
    // Resolution patches (kvicken + cuesta4): output size, the fixed 1920x1080 pool render target and its users, and
    // the half-resolution luminance buffers; all of them below 1080p too, since effects sample them with the scene's
    // texel size. Every option includes the Texture Corruption Fix: Video Arena 1.25 GiB -> `arena`.
    const auto& resolution = Resolutions[config.resolution];
    if (config.resolution != NativeResolution) {
        struct Site { u64 rva; u32 size; bool height, half; };
        static constexpr Site sites[]{
            {0x1f0c5b, 4, false}, {0x1f0c63, 4, true}, {0x1f0c6c, 2, false}, {0x1f0c73, 2, true},
            {0x1643e3, 4, true}, {0x1643f0, 4, false}, {0x1ff889, 4, false}, {0x1ff88f, 4, true},
            {0x3c46b4, 4, false}, {0x3c46b8, 4, true}, {0x16bb72, 4, true}, {0x20965d, 4, true},
            {0x3f2a3c, 4, false, true}, {0x3f2a40, 4, true, true},
        };
        for (const auto& site : sites) {
            const u32 divisor = site.half ? 2 : 1;
            value(site.rva, (site.height ? 1080 : 1920) / divisor, u32(site.height ? resolution.height : resolution.width) / divisor, site.size);
        }
    }
    value(0x3cd918, 0x50000000, resolution.arena << 24);
    if (config.msaa != 2) value(0x3cdcd0, 1, std::countr_zero(config.msaa));
    if (config.fps != 60) { // Keep swing normalization at the original 59.94 Hz for every other rate.
        patches.push_back({0x08ff0a, {0x48, 0x8d, 0x05, 0x6f, 0xdd, 0x33, 0x00}, {0x48, 0x8d, 0x05, 0xf3, 0xdd, 0x2c, 0x00}});
        // The timing helper alone reads these constants. Preserve its original maximum delta, 4/59.94 s.
        value(0x35dca8, 0x43c80000, std::bit_cast<u32>(400.f * (static_cast<float>(config.fps) / 60.f)));
        value(0x35dcac, 0x40800000, std::bit_cast<u32>(4.f * (static_cast<float>(config.fps) / 60.f)));
    }
    if (config.skip_intro) { // Skip Intro (cuesta4): skips the initial logos.
        patches.push_back({0x184940, {0x75, 0x34}, {0xeb, 0x34}});
        patches.push_back({0x0c8430, {0x48, 0x8d, 0x05, 0xa5, 0x5b, 0x38, 0x00, 0x8b, 0x00, 0xc3, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
                                     {0xc7, 0x05, 0x1e, 0x0e, 0x9b, 0x01, 0x01, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3}});
        patches.push_back({0x01b36a, {0xe8, 0x11, 0x99, 0x09, 0x00}, {0xe8, 0x21, 0x6f, 0x1f, 0x00}});
        patches.push_back({0x212290, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                                     {0xc7, 0x05, 0xbe, 0x6f, 0x86, 0x01, 0x01, 0x00, 0x00, 0x00, 0xe9, 0xe1, 0x29, 0xea, 0xff}});
    }
    if (config.skip_videos) { // Skip Any Video With X Button (cuesta4).
        patches.push_back({0x01b2a3, {0x75, 0x4a}, {0x75, 0x42}});
        patches.push_back({0x01b2e7, {0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc8}, {0xb9, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90}});
    }
    if (config.cinematics_4k) {
        // Frame queue capacity, decoder limits, and the Y/UV render height clamp.
        value(0x1f2d69, 1920, 3840);
        value(0x1f2d6e, 1080, 2160);
        value(0x2e1c53, 42, 51);
        value(0x2e1c67, 1920, 3840);
        value(0x2e1c71, 1088, 2160);
        value(0x1f24bb, 1080, 2160);
        value(0x1f24c0, 1080, 2160);
        // Retain two 512 KiB blocks when the compressed stream wraps; 4K access units can exceed one block.
        struct Wrap { u64 rva; u32 displacement; };
        for (const auto [rva, displacement] : {Wrap{0x1f2095, 0x0188b1e1}, Wrap{0x1f21f1, 0x0188b085}, Wrap{0x1f2cca, 0x0188a5ac}}) {
            std::vector<u8> before{0xc1, 0xe1, 0x13, 0x48, 0x8b, 0x1d};
            const auto* disp = reinterpret_cast<const u8*>(&displacement);
            before.insert(before.end(), disp, disp + 4);
            before.insert(before.end(), {0x48, 0x63, 0xf1, 0x48, 0x01, 0xde, 0xba, 0x00, 0x00, 0x08, 0x00});
            const u32 moved_displacement = displacement + 1;
            const auto* moved = reinterpret_cast<const u8*>(&moved_displacement);
            std::vector<u8> after{0x90, 0x90, 0x48, 0x8b, 0x1d};
            after.insert(after.end(), moved, moved + 4);
            after.insert(after.end(), {0x48, 0x8d, 0xb3, 0x00, 0x00, 0x90, 0x00, 0xba, 0x00, 0x00, 0x10, 0x00});
            patches.push_back({rva, std::move(before), std::move(after)});
            value(rva + 0x23, 1, 2);
            value(rva + 0x28, 0x80000, 0x100000);
        }
        value(0x1f2c07, 0x980000, 0x900000);
    }
    MemoryPatcher(eboot).Apply(patches);
    ApplyCheats(eboot, config);
    if (DebugEnabled()) Log(std::format("Applied {} game patch sites: {} at {:.2f} fps; 64 manual saves", patches.size(), resolution.name, GameFrameRate(config.fps)));
}
}
