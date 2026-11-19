// SPDX-License-Identifier: GPL-2.0-or-later
#include "main/api.h"
#include "main/cpu.h"
#include "main/host.h"
#include "main/config.h"
#include "orbis/common/elf_info.h"
#include "orbis/common/path_util.h"
#include "orbis/common/ntapi.h"
#include "orbis/common/util.h"
#include "orbis/core/cpu_patches.h"
#include "orbis/core/aerolib/stubs.h"
#include "orbis/core/emulator_settings.h"
#include "orbis/core/file_sys/fs.h"
#include "orbis/internals/libs.h"
#include "orbis/internals/audio/audioout_backend.h"
#include "orbis/core/linker.h"
#include "patches/hooks.h"
#include "patches/memory_patcher.h"
#include "present/window.h"
#include "gpu/gpu_thread.h"
#include "gpu/shaders/shader_aot.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_recipes.h"
#include "gpu/shaders/shader_jit.h"
#include <format>
#include <stdexcept>
#include <array>
#include <bit>

namespace {
// The error mode below hides Windows' crash dialog, so without this a crash closes the game silently.
LONG WINAPI ReportUnhandledCrash(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    const auto address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    std::string where = std::format("{:#x}", address);
    HMODULE module{};
    char path[MAX_PATH]{};  // narrow and non-throwing: only the module's file name is shown
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(record->ExceptionAddress), &module) &&
        GetModuleFileNameA(module, path, MAX_PATH)) {
        const char* name = path;
        for (const char* c = path; *c; ++c)
            if (*c == '\\' || *c == '/') name = c + 1;
        where = std::format("{}+{:#x}", name, address - reinterpret_cast<uintptr_t>(module));
    }
    std::string text = std::format("Crash: exception {:#010x} at {}", static_cast<u32>(record->ExceptionCode), where);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        text += std::format(" ({} {:#x})", record->ExceptionInformation[0] ? "writing" : "reading",
                            record->ExceptionInformation[1]);
    text += "\n\nIf it happens while the game starts, make sure the Windows pagefile is enabled (system managed).";
    ShowFatalError(text.c_str());
    return EXCEPTION_EXECUTE_HANDLER; // ends the process
}

thread_local std::wstring last_error;
std::filesystem::path validation_log;
void ObserveRuntimeImport(const char* name, const char* nid, void* caller) {
    const std::string_view function(name);
    if (function.starts_with("sceGnm") || function.starts_with("sceVideoOut")) {
        Gow3::Log(std::format("RUNTIME_GRAPHICS_BOUNDARY {} [{}] caller={:#x}", name, nid,
                             reinterpret_cast<uintptr_t>(caller)));
        Gow3::FlushLog(validation_log);
        ExitProcess(2);
    }
}
void StoreError(const char* error) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, error, -1, nullptr, 0);
    if (!size) { last_error = L"Runtime initialization failed"; return; }
    last_error.resize(static_cast<size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, error, -1, last_error.data(), size);
    last_error.resize(static_cast<size_t>(size - 1));
}
}

const wchar_t* Gow3LastError() { return last_error.c_str(); }

int Gow3Run(const wchar_t* executable_directory, const wchar_t* game_directory, uint32_t flags) {
    using namespace Gow3;
    std::filesystem::path log_path;
    last_error.clear();
    try {
        if (!executable_directory || !game_directory ||
            (flags != 0 && flags != Gow3Inspect && flags != Gow3ValidateRuntime))
            throw std::runtime_error("Invalid runtime arguments");
        const auto game = std::filesystem::absolute(game_directory);
        if (flags == 0 && !std::filesystem::is_regular_file(game / "eboot.bin")) {
            // A packaged game is only looked for when there is no eboot.bin.
            std::error_code error;
            const auto packaged = std::ranges::any_of(std::filesystem::directory_iterator(game, error), [](const auto& entry) {
                return _wcsicmp(entry.path().extension().c_str(), L".fpkg") == 0;
            });
            Window::ReportStartupError(packaged ? Window::StartupError::Packaged : Window::StartupError::NoGame);
        }
        const auto app = PreparePortDirectory(game);
        log_path = app / "gow3.log";
        if (DebugEnabled()) FlushLog(log_path); // debug lines are written through from here on
        if (DebugEnabled()) Log("GOW3 Orbis runtime initialization; debug mode");
        Common::NtApi::Initialize();
        Log(Cpu::Describe());
        SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
        SetUnhandledExceptionFilter(ReportUnhandledCrash);
        const auto config = LoadConfig(app / PortDir::Settings / "config.cfg");
        GpuThread::Configure(config.anisotropy, config.motion_blur);
        Libraries::AudioOut::SetSpeakerMode(int(config.speakers));
        if (DebugEnabled()) Log(std::format("Configuration: {} at {:.2f} fps, skip intro {}, skip videos {}, mode {}",
                        Resolutions[config.resolution].name, GameFrameRate(config.fps), config.skip_intro, config.skip_videos,
                        static_cast<int>(config.display_mode)));
        // By Common::FS::PathType; data this game does not use stays under the guest home, which nothing creates (its
        // sys_modules and custom_modules are only looked up for user-provided modules).
        const auto home = std::filesystem::path(PortDir::Home);
        const std::array<std::filesystem::path, size_t(Common::FS::PathType::Count)> paths{PortDir::Settings, PortDir::Dumps,
            PortDir::Screenshots, PortDir::ShaderCache, home / "temp", home / "data", home / "sys_modules", home / "download",
            PortDir::Dumps, home / "cheats", home / "patches", home / "game_data", home / "custom_trophy",
            std::filesystem::path(PortDir::Settings) / "custom_configs", home / "cache", home / "fonts", home / "trophy",
            home, home / "custom_modules", home / "licenses", PortDir::Saves};
        for (size_t i = 0; i < paths.size(); ++i) Common::FS::SetUserPath(static_cast<Common::FS::PathType>(i), app / paths[i]);
        EmulatorSettings.SetSysModulesDir(app / home / "sys_modules");
        EmulatorSettings.SetHomeDir(app / home);
        EmulatorSettings.SetExtraDmemInMBytes(Resolutions[config.resolution].dmem_mb); // Video Arena enlarged by the patches (1:1 over base)
        if (DebugEnabled()) Log(std::format("Video Arena 1:1 DMEM alignment: extra {} MiB allocated (Base 1280 MiB -> Total Arena {} MiB)",
                        Resolutions[config.resolution].dmem_mb, Resolutions[config.resolution].arena * 16));
        EmulatorSettings.SetVblankFrequency(config.fps); // nominal selection; VideoOut uses FramePeriodNs
        Common::ElfInfo::Instance().Initialize(game);
        if (flags == 0) {
            if (!IsSupportedEboot(game / "eboot.bin")) Window::ReportStartupError(Window::StartupError::WrongVersion);
            if (BeginLaunchSession(app)) Window::AskAfterCrash(game);
        }
        auto* mounts = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        mounts->Mount(game, "/app0", true);
        mounts->Mount(game, "/hostapp", true);
        Common::Singleton<Core::FileSys::HandleTable>::Instance()->CreateStdHandles();
        auto* linker = Common::Singleton<Core::Linker>::Instance();
        Libraries::InitHLELibs(&linker->GetHLESymbols());
        if (flags == 0) {
            Gpu::Jit::mode = static_cast<Gpu::Jit::Mode>(config.shader_recompilation);
            // A restart after skipping the AOT build runs JIT once; the saved setting (and the menu) stay AOT.
            const bool aot = config.shader_mechanism == ShaderMechanism::Aot && !Window::TakeJitOnce();
            if (config.shader_mechanism == ShaderMechanism::Jit) ResetAotCooldown();
            Gpu::ShaderCache::Instance().Configure(app / PortDir::ShaderCache, aot ? Gpu::CacheKind::Aot : Gpu::CacheKind::Jit);
            Gpu::Recipes::Instance().Configure(app / PortDir::ShaderCache);
            Gpu::Aot::game_directory = game;
            Gpu::Aot::lazy = config.lazy_aot;
            Gpu::Aot::sample_log2 = std::countr_zero(config.msaa);
            Window::Open(config);
        }
        Core::WindowsGuestRedZoneProtection::SetActiveMode(WindowsGuestRedZoneProtectionMode::StaticPatching);
        if (linker->LoadModule(game / "eboot.bin") != 0) throw std::runtime_error("Cannot load EBOOT");
        if (flags == 0) {
            InstallHooks(*linker->GetModule(0), config);
            ApplyGamePatches(*linker->GetModule(0), config);
        }
        for (const auto* name : {"libc.prx", "libSceFios2.prx"})
            if (linker->LoadModule(game / "sce_module" / name) < 0) throw std::runtime_error("Cannot load game PRX");
        // No system module ships with the Port: Linker::Execute maps libSceLibcInternal only from a user-provided
        // sys_modules directory and otherwise registers the HLE implementation.
        for (s32 i = 0; const auto* module = linker->GetModule(i); ++i)
            if (DebugEnabled()) Log(std::format("Mapped {} base={:#x} entry={:#x}", module->name,
                            module->GetBaseAddress(), module->GetEntryAddress()));
        linker->RelocateAllImports();
        linker->PrepareStaticTls();
        const auto graphics_import = [](const auto& symbol) {
            return symbol.library == "libSceGnmDriver" || symbol.library == "libSceVideoOut";
        };
        const auto pending_graphics = std::ranges::count_if(linker->UnresolvedImports(), graphics_import);
        if (DebugEnabled()) Log(std::format("Registered Orbis symbols: {}; pending graphics imports: {}; upstream runtime fallbacks: {}; Windows TCB slot: {}",
                        linker->GetHLESymbols().GetSize(), pending_graphics,
                        linker->UnresolvedImports().size() - pending_graphics, Core::GetTcbKey()));
        for (const auto& symbol : linker->UnresolvedImports())
            if (DebugEnabled()) Log(std::format("{} {} [{}] library={} module={}",
                            graphics_import(symbol) ? "PENDING_GRAPHICS" : "UPSTREAM_FALLBACK", symbol.nidName,
                            symbol.name, symbol.library, symbol.module));
        if (flags & Gow3Inspect) {
            FlushLog(log_path);
            return 0;
        }
        FlushLog(log_path);
        if (flags & Gow3ValidateRuntime) {
            validation_log = log_path;
            Core::AeroLib::SetStubObserver(ObserveRuntimeImport);
        }
        static const std::vector<std::string> args{"/app0/eboot.bin"}; // the guest main thread keeps a reference
        linker->Execute(args);
        if (flags & Gow3ValidateRuntime) Sleep(INFINITE);
        Window::RunEventLoop(log_path, game);
    } catch (const std::exception& error) {
        EndLaunch(); // reported to the user: not a crash
        StoreError(error.what());
        Log(std::string("STARTUP: ") + error.what());
        if (!log_path.empty()) {
            try { FlushLog(log_path); } catch (...) { last_error += L"\nCould not write gow3.log."; }
        }
        return 1;
    } catch (...) {
        EndLaunch();
        last_error = L"Unexpected runtime initialization error";
        return 1;
    }
}
