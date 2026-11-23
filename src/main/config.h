// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace Gow3 {
enum class DisplayMode { Windowed, Borderless, Fullscreen };
enum class VSync { Fifo, Mailbox, Off }; // swapchain present mode
enum class AntiAliasing { None, Cmaa2, Taa };
enum class ScalingFilter { Bilinear, Fsr1 };
enum class Speakers { Auto, Stereo, Surround51, Surround71 }; // the layout the game's 7.1 mix is sent to the sound device in
enum class ShaderRecompilation { Sync, Async };
enum class ShaderMechanism { Jit, Aot };
// Output resolutions of the resolution patches (God_of_War_III_Remastered.xml, kvicken + cuesta4).
// Base video arena in the stock eboot is 0x50000000 (1280 MiB), high byte 0x50.
// Every option enlarges the Video Arena (`arena` is the high byte of its size, where 1 unit = 16 MiB).
// Extra DMEM is aligned strictly 1:1 with the additional video memory requested over base.
inline constexpr unsigned BaseVideoArenaHighByte = 0x50;
inline constexpr unsigned BaseVideoArenaMb = BaseVideoArenaHighByte * 16; // 1280 MiB

inline constexpr unsigned CalculateExtraDmemMb(unsigned arena_high_byte) {
    return arena_high_byte > BaseVideoArenaHighByte ? (arena_high_byte - BaseVideoArenaHighByte) * 16 : 0;
}

struct Resolution {
    std::string_view name;
    int width, height;
    unsigned arena;
    unsigned dmem_mb;

    constexpr Resolution(std::string_view n, int w, int h, unsigned a)
        : name(n), width(w), height(h), arena(a), dmem_mb(CalculateExtraDmemMb(a)) {}

    constexpr Resolution(std::string_view n, int w, int h, unsigned a, unsigned d)
        : name(n), width(w), height(h), arena(a), dmem_mb(d) {}
};
inline constexpr std::array<Resolution, 6> Resolutions{{
    {"480p", 854, 480, 0x70},
    {"720p", 1280, 720, 0x70},
    {"1080p", 1920, 1080, 0x70},
    {"1440p", 2560, 1440, 0x78},
    {"1800p", 3200, 1800, 0x82},
    {"2160p", 3840, 2160, 0x8e},
}};
inline constexpr unsigned NativeResolution = 2;
inline constexpr std::array<unsigned, 3> MsaaLevels{1, 2, 4};
inline constexpr std::array<unsigned, 4> AnisotropyLevels{0, 4, 8, 16}; // 0 keeps the game's samplers
// TAA tuning (menu and config.cfg): the value names are what both show; the numbers go to the resolve shader.
inline constexpr std::array<std::string_view, 6> TaaBlendNames{"0.05", "0.08", "0.12", "0.18", "0.25", "0.35"};
inline constexpr std::array<float, 6> TaaBlendValues{0.05f, 0.08f, 0.12f, 0.18f, 0.25f, 0.35f};          // share of the new frame per frame
inline constexpr std::array<std::string_view, 6> TaaSharpNames{"0", "0.15", "0.3", "0.45", "0.6", "0.8"};
inline constexpr std::array<float, 6> TaaSharpValues{0.f, 0.15f, 0.3f, 0.45f, 0.6f, 0.8f};
inline constexpr std::array<std::string_view, 6> TaaClampNames{"0.75", "1", "1.25", "1.6", "2", "3"};
inline constexpr std::array<float, 6> TaaClampValues{0.75f, 1.f, 1.25f, 1.6f, 2.f, 3.f};                 // history box width, in deviations
struct FrameRate {
    int nominal;
    std::string_view name;
};
inline constexpr std::array<FrameRate, 6> FrameRates{{
    {30, "29.97"}, {40, "39.96"}, {60, "59.94"}, {120, "119.88"}, {240, "239.76"}, {480, "479.52"},
}};
// Match the eboot's original 0x426fc28f float, including its rounding.
inline constexpr float GameFrameRate(int nominal) { return 59.94f * (static_cast<float>(nominal) / 60.f); }
inline constexpr std::int64_t FramePeriodNs(int nominal) {
    return std::llround(1'000'000'000.0 / GameFrameRate(nominal));
}
struct Config {
    unsigned resolution = NativeResolution; // index into Resolutions
    int fps = 60;                           // nominal FrameRates selection; actual rate is GameFrameRate(fps)
    unsigned anisotropy = 0;
    unsigned msaa = 2;
    bool skip_intro = false;
    bool skip_videos = true;                // cinematics can be skipped with X
    bool cinematics_4k = false;
    DisplayMode display_mode = DisplayMode::Borderless;
    bool vrr = false;                       // full screen: the display at its highest refresh rate
    VSync vsync = VSync::Fifo;
    bool nvidia_reflex = false;
    AntiAliasing anti_aliasing = AntiAliasing::None;
    ScalingFilter scaling_filter = ScalingFilter::Bilinear;
    unsigned taa_blend = 2, taa_sharpness = 2, taa_clamp = 2;  // indices into the Taa* tables above
    bool motion_blur = true;                // false: the game's motion blur pass gets no velocity (gcn_spirv.cpp)
    Speakers speakers = Speakers::Auto;     // Auto follows the sound device's channel count
    ShaderRecompilation shader_recompilation = ShaderRecompilation::Async;
    ShaderMechanism shader_mechanism = ShaderMechanism::Aot;
    bool lazy_aot = true;                   // AOT builds a fixed quarter of all WAD variants per eligible launch
};
Config LoadConfig(const std::filesystem::path& path);
void SaveMenuConfig(const std::filesystem::path& path, const Config& config);
}
