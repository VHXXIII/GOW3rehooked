// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/gpu_thread.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "orbis/common/thread.h"
#include "orbis/common/util.h"
#include "main/config.h"
#include "main/cpu.h"
#include "orbis/core/linker.h"
#include "orbis/core/platform.h"
#include "present/presenter.h"
#include "gpu/buffers.h"
#include "gpu/piece_hash.h"
#include "gpu/vk_gpu.h"
#include "main/host.h"
#include "orbis/common/futex.h"
#include "gpu/device.h"
#include "debug/cpu_probe.h"
#include "debug/frame_dump.h"
#include "debug/frame_recorder.h"
#include "debug/render_probe.h"
#include "debug/s4_trace.h"
#include "debug/video_trace.h"

namespace Libraries::VideoOut {
extern std::atomic<u64> temp_vblank_loops, temp_vblank_flips;  // debug: read by the per-second line
}
namespace Gow3::GuestMemory {
extern std::atomic<u64> temp_queries;  // debug
}

namespace Gow3::GpuThread {
namespace {
// ---- Guest globals and worker state ----
constexpr u64 kRingGlobals = 0x024B92E8, kArenaPtrs = 0x02A78060, kGodRays = 0x0146A8E8, kGhidraBase = 0x01000000;

u64 Clock() { return u64(std::chrono::steady_clock::now().time_since_epoch().count()); }

// GPU thread: command buffers run in submission order. The engine's flip buffer waits on a label the CPU writes after a
// flip (WAIT_REG_MEM); flips are queued by the guest threads themselves (VideoOut), so the wait never needs this thread.
std::mutex queue_mutex;
std::condition_variable_any queue_cv;
std::deque<std::function<void()>> submits;
std::jthread gpu_thread;
std::atomic<u64> submitted{0};
std::atomic<u64> copied{0};  // the last submit whose memory the renderer has staged (BeginFrame)
u64 resting[2];              // SetSpareArenas (game main thread, like Submit and AwaitCopies)
std::atomic<bool> cinematic_active{false};

// The renderer, created on the GPU thread at the first submit after the window's device exists. GOW3_NULL_GPU=1 keeps
// the GPU null (fences only). Never destroyed: process exit tears the device down.
std::atomic<Gpu::Renderer*> live{nullptr};
unsigned configured_anisotropy{};
bool configured_motion_blur = true, configured_taa = false;
bool render_off = std::getenv("GOW3_NULL_GPU") != nullptr;
size_t notes_logged = 0;

// Debug: frame timing and counters logged once per second in debug builds.
std::atomic<u64> temp_late{0};                    // debug: frames copied after the next submit (the game already rewriting their arena)
std::atomic<u64> temp_held{0}, temp_held_us{0};   // debug: frame loop starts that waited for a copy (AwaitCopies)
double temp_render_ms[16]{};                      // debug: BeginFrame, chain, then the renderer's Stats::temp_ms (summed per second)
struct TempTimes {
    u64 frames = 0;
    double idle = 0, render = 0, record = 0, retire = 0, process = 0;
    std::chrono::steady_clock::time_point since = DiagNow();
} temp_t;

// ---- Video frame tracking ----
// The video frames the decoder produced, by guest address: each new frame gets a serial, so the texture and copy caches see a
// frame's content change even though its buffer address stays.
namespace VideoVersions {
struct Slot {
    std::atomic<u64> address{}, chroma{}, serial{};  // chroma: the NV12 UV plane's address
};
Slot slots[32];
std::atomic<u64> next_serial{};
u64 Serial(u64 address, bool* chroma = nullptr) {
    for (auto& slot : slots) {
        const u64 base = slot.address.load(std::memory_order_acquire);
        if (!base || (address != base && address != slot.chroma.load(std::memory_order_relaxed))) continue;
        if (chroma) *chroma = address != base;
        return slot.serial.load(std::memory_order_acquire);
    }
    return 0;
}
}  // namespace VideoVersions

// ---- PM4 command packet execution ----
enum Op : u32 {
    WriteData = 0x37, WaitRegMem = 0x3C, IndirectBuffer = 0x3F, EventWriteEop = 0x47, EventWriteEos = 0x48,
    ReleaseMem = 0x49, DmaData = 0x50,
};

template <class T> void Store(u64 address, T value) {
    std::atomic_ref(*reinterpret_cast<T*>(address)).store(value, std::memory_order_release);
}
u32 Load(u64 address) { return std::atomic_ref(*reinterpret_cast<u32*>(address)).load(std::memory_order_acquire); }

// Debug: WAIT_REG_MEM packets that had to wait, since the last per-second line.
struct {
    u64 n = 0, addr = 0;
    u32 ref = 0, fn = 0;
    double ms = 0;
} temp_waits;

// EVENT_WRITE_EOP / RELEASE_MEM data_sel and int_sel.
void Fence(u64 address, u32 data_sel, u32 int_sel, u64 data) {
    switch (data_sel) {
    case 1: Store<u32>(address, u32(data)); break;
    case 2: Store<u64>(address, data); break;
    case 3: case 4: Store<u64>(address, Clock()); break;
    default: break;
    }
    if (int_sel) Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
    Common::NotifyGpuWrite();
}

bool Test(const u32* p) {
    const u32 value = Load(u64(p[1] & ~3u) | u64(p[2]) << 32) & p[4], ref = p[3];
    switch (p[0] & 7) {
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

void Process(const u32* cmd, u64 dwords);
// One type-3 packet's effect outside the GPU; true: the rest of its buffer is not executed (a chained INDIRECT_BUFFER).
bool Execute(u32 header, const u32* p, u32 payload) {
    switch (header >> 8 & 0xff) {
    case IndirectBuffer:
        Process(reinterpret_cast<const u32*>(u64(p[0]) | u64(p[1] & 0xffff) << 32), p[2] & 0xfffff);
        return p[2] >> 20 & 1;
    case EventWriteEop:
        Fence(u64(p[1]) | u64(p[2] & 0xffff) << 32, p[2] >> 29, p[2] >> 24 & 3, u64(p[3]) | u64(p[4]) << 32);
        break;
    case ReleaseMem:
        Fence(u64(p[2]) | u64(p[3]) << 32, p[1] >> 29, p[1] >> 24 & 7, u64(p[4]) | u64(p[5]) << 32);
        break;
    case EventWriteEos:
        if ((p[2] >> 29) == 2) {
            Store<u32>(u64(p[1]) | u64(p[2] & 0xffff) << 32, p[3]);
            Common::NotifyGpuWrite();
        }
        break;
    case WriteData:
        if (const u32 dst = p[0] >> 8 & 0xf; (dst == 2 || dst == 5) && !(p[0] >> 16 & 1)) {
            std::memcpy(reinterpret_cast<void*>(u64(p[1]) | u64(p[2]) << 32), p + 3, (payload - 3) * 4);
            Common::NotifyGpuWrite();
        }
        break;
    case WaitRegMem:
        if (p[0] >> 4 & 1) {
            const auto t_wait = DiagNow();
            bool waited = false;
            // Labels written by guest threads (flip handlers) land within a vblank: a short yield spin catches the quick
            // ones, then high-resolution timer sleeps (no 1 ms OS granularity) instead of burning a core until the vblank.
            for (auto start = std::chrono::steady_clock::now(); !Test(p);) {
                waited = true;
                if (auto* r = live.load(std::memory_order_acquire)) r->Retired();
                std::chrono::steady_clock::now() - start < std::chrono::microseconds(200)
                    ? std::this_thread::yield()
                    : void(Common::AccurateSleep(std::chrono::microseconds(100), nullptr, false, false));
            }
            if (waited) GOW3_DIAG {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wait).count();
                temp_waits.n++, temp_waits.ms += ms;
                temp_waits.addr = u64(p[1] & ~3u) | u64(p[2]) << 32, temp_waits.ref = p[3], temp_waits.fn = p[0] & 7;
            }
        }
        break;
    case DmaData: {
        const u32 dst = p[0] >> 20 & 3, src = p[0] >> 29 & 3, bytes = p[5] & 0x1fffff;
        if ((dst != 0 && dst != 3) || (p[5] >> 26 & 3)) break;  // GDS or register address spaces
        auto* to = reinterpret_cast<u8*>(u64(p[3]) | u64(p[4]) << 32);
        if (src == 2)
            for (u32 i = 0; i + 4 <= bytes; i += 4) std::memcpy(to + i, p + 1, 4);
        else if (src != 1) std::memmove(to, reinterpret_cast<const void*>(u64(p[1]) | u64(p[2]) << 32), bytes);
        break;
    }
    default: break;
    }
    return false;
}

void Process(const u32* cmd, u64 dwords) {
    for (const u32* end = cmd + dwords; cmd < end;) {
        const u32 header = *cmd, type = header >> 30, payload = (header >> 16 & 0x3fff) + 1;
        if (type == 2) {
            ++cmd;
            continue;
        }
        const u32* p = cmd + 1;
        cmd += payload + 1;
        if (type != 3) continue;  // type 0 register writes
        if (S4::enabled) S4::Packet(header, p, payload);
        if (Execute(header, p, payload)) return;
    }
}

// ---- Renderer lifecycle, frame memory capture, and render pass ----
Gpu::Renderer* Renderer() {
    if (auto* r = live.load(std::memory_order_acquire); r || render_off) return r;
    const auto& vk = Vulkan::Get();
    if (!vk.device) return nullptr;
    auto r = std::make_unique<Gpu::Renderer>();
    std::string error;
    if (!r->Init(Gpu::External{vk.instance, vk.physical, vk.device, vk.queue, vk.family, &Vulkan::QueueMutex()}, error)) {
        Log("Renderer: " + error + "; the GPU stays null");
        render_off = true;
        return nullptr;
    }
    if (DebugEnabled()) Log(Gpu::PieceHash::UsesAes() ? "Renderer piece hash: AES-NI" : "Renderer piece hash: XXH3");
    r->SetAnisotropy(configured_anisotropy);
    r->SetMotionBlurEnabled(configured_motion_blur);
    r->SetTaaEnabled(configured_taa);
#ifdef _WIN32
    GuestMemory::Install();
#endif
    r->SetTracker([p = r.get()](u64 lo, u64 hi) { return GuestMemory::Track(*p, lo, hi); });
    r->SetReadback(GuestMemory::Store);
    live.store(r.get(), std::memory_order_release);
    return r.release();
}

// Guest memory a frame's GPU work reads that the CPU rewrites every frame, read at submit (Ghidra VAs, live = VA - 0x01000000
// + EBOOT base): the data ring slice up to its cursor (+64 KB), offered before recording, and the prepared CPU arenas,
// deferred: staged after recording, only where the frame's work reads them (a third of the half, none of the 1 MB one).
struct FrameMemory {
    std::vector<GuestMemory::Range> offer, regions, deferred;
    u64 rays = 0;  // fxGodRays' occlusion query blocks (2 frame sets of 32 lights, 128 B each), read two frames later
    u64 video_base{}, video_serial{};
    float motion_blur_dt{};
};

FrameMemory CaptureFrameMemory() {
    FrameMemory f;
    const auto* eboot = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
    if (!eboot) return f;
    const u64 delta = eboot->GetBaseAddress() - kGhidraBase;
    f.motion_blur_dt = *reinterpret_cast<const float*>(delta + 0x013cdca0);
    if (VideoTrace::enabled) {
        const auto* t = reinterpret_cast<const u32*>(delta + 0x02a794f8);
        f.video_base = (u64(t[0]) | (u64(t[1] & 0x3f) << 32)) << 8;
        f.video_serial = VideoVersions::Serial(f.video_base);
        if (f.video_serial) VideoTrace::Add(VideoTrace::Selected, {Clock(), f.video_base, f.video_serial, 0, 1,
            *reinterpret_cast<const u32*>(delta + 0x02a794f0), *reinterpret_cast<const u32*>(delta + 0x02a794f4),
            *reinterpret_cast<const u32*>(delta + 0x02a79250)});
    }
    f.rays = *reinterpret_cast<const u64*>(delta + kGodRays);
    const u64* ring = reinterpret_cast<const u64*>(delta + kRingGlobals);
    for (u32 i = 0; i < 4; ++i)
        if (ring[3 + i * 2] && ring[4 + i * 2] > ring[3 + i * 2]) f.regions.push_back({ring[3 + i * 2], ring[4 + i * 2]});
    if (const u32 s = ring[11] & 3; ring[3 + s * 2]) {
        const u64 lo = ring[3 + s * 2], hi = ring[4 + s * 2], cursor = ring[13];
        f.offer.push_back({lo, cursor >= lo && cursor < hi ? std::min(hi, cursor + 0x10000) : hi});
    }
    // Only the prepared half and the 1 MB arena, deferred (offered when their pages do not fit Submit's 17 MB). The other
    // half and spares retain their snapshots: no draw of the next submit reads what this one did not.
    // All of them stay regions (never write-protected). Descriptor {u32 front, u32 back offset, u64 base, u64 end}.
    const u64* arenas = reinterpret_cast<const u64*>(delta + kArenaPtrs);
    const u32 arena_index = *reinterpret_cast<const u32*>(delta + kArenaPtrs - 4) & 1;
    const u64 descs[]{arenas[0], arenas[1], arenas[2], resting[0], resting[1]};
    for (u64 i = 0, deferred = 0; i < 5; ++i) {
        const auto* desc = reinterpret_cast<const u8*>(descs[i]);
        if (!desc || !GuestMemory::Readable(descs[i], 24)) continue;
        const u32 used = *reinterpret_cast<const u32*>(desc), cap = *reinterpret_cast<const u32*>(desc + 4);
        const u64 base = *reinterpret_cast<const u64*>(desc + 8), end = *reinterpret_cast<const u64*>(desc + 16);
        if (!base || end <= base || end - base > (64u << 20)) continue;
        f.regions.push_back({base, end});
        if (i != arena_index && i != 2) continue;
        if (const u64 lo = base & ~0xFFFull, hi = (end + 0xFFF) & ~0xFFFull; deferred + (hi - lo) <= (17u << 20) && GuestMemory::Readable(lo, hi - lo)) {
            f.deferred.push_back({base, end}), deferred += hi - lo;
        } else {
            f.offer.push_back({base, base + std::min(used, cap)});
            if (base + cap < end) f.offer.push_back({base + cap, end});
        }
    }
    std::sort(f.regions.begin(), f.regions.end());  // the arenas change slots every pass
    return f;
}

// Draws the frame and waits for it, so the fences and labels Process writes next follow the GPU's work.
void Render(Gpu::Renderer& r, const std::vector<std::pair<const u32*, u32>>& buffers, const FrameMemory& mem, Recorder::Frame& rec, u64 n) {
    if (mem.video_serial) VideoTrace::Add(VideoTrace::Rendered, {Clock(), mem.video_base, VideoVersions::Serial(mem.video_base), mem.video_serial, 2, 0, 0, 0});
    static std::vector<GuestMemory::Range> regions;
    const auto temp_t0 = DiagNow();
    RenderProbe::Begin(n, mem.regions);
    if (mem.regions != regions) GuestMemory::SetRegions(regions = mem.regions);
    const u32 faults = GuestMemory::BeginFrame(r, mem.offer);
    const auto temp_t1 = DiagNow();
    // The chain is the submitted command buffers: 011A2AA0 submits the segments in chain order, and the game's commands
    // hold no INDIRECT_BUFFER (the renderer notes one; Process still follows it).
    std::vector<Gpu::Segment> chain(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) chain[i] = {buffers[i].first, buffers[i].second};
    GOW3_DIAG {
        rec.faults = faults;
        rec.ms[0] = float(DiagMs(temp_t0, temp_t1));
        rec.ms[1] = float(DiagMs(temp_t1, std::chrono::steady_clock::now()));
        temp_render_ms[0] += rec.ms[0], temp_render_ms[1] += rec.ms[1];
    }
    auto& stats = r.GetStats();
    FrameDump::Begin(stats);  // F9 also in a normal run: the next 3 frames are traced into the session archive (gow3-*.zip)
    r.SetQueries(mem.rays, mem.rays ? mem.rays + 0x2000 : 0);
    r.SetMotionBlurFrameTime(mem.motion_blur_dt);
    // Once Submit has staged the deferred arenas, the game may reset them (AwaitCopies).
    r.Submit(chain, [](u64 a, u64 n) -> const void* { return GuestMemory::Readable(a, n) ? reinterpret_cast<const void*>(a) : nullptr; }, mem.deferred,
             [n] {
                 copied.store(n, std::memory_order_release);
#ifdef _WIN32
                 WakeByAddressAll(&copied);
#else
                 Common::FutexWakeAll(&copied);
#endif
             });
    RenderProbe::End();
    FrameDump::End(r, stats);  // returns at once unless F9 was pressed
    GOW3_DIAG {
        for (const auto& notes = stats.notes; notes_logged < notes.size(); ++notes_logged) Log("Renderer: " + notes[notes_logged]);
        for (const auto& line : stats.report) Log("Renderer: " + line);  // TEMP diagnostics
        stats.report.clear();
        auto staged = mem.offer;
        staged.insert(staged.end(), mem.deferred.begin(), mem.deferred.end());
        FrameDump::Boom(stats, mem.regions, staged);
    }
}

// ---- Worker thread ----
void LogTempStats() {
    if (!DebugEnabled()) return;
    if (const auto now = std::chrono::steady_clock::now(); DiagMs(temp_t.since, now) < 1000 || !temp_t.frames) return;
    const double n = double(temp_t.frames);
    Log(std::format("TEMP gpu thread: {} frames/s; per frame idle {:.2f} render {:.2f} (record {:.2f} retire {:.2f}) process {:.2f} ms; "
                    "waits {:.2f}/frame {:.2f} ms/frame, last {:x} fn {} ref {:x} now {:x}; frame loop held for the copy {} times {:.2f} ms; "
                    "vblank loops {} flips {}; late copies {}",
                    temp_t.frames, temp_t.idle / n, temp_t.render / n, temp_t.record / n, temp_t.retire / n, temp_t.process / n,
                    temp_waits.n / n, temp_waits.ms / n, temp_waits.addr, temp_waits.fn, temp_waits.ref,
                    temp_waits.addr ? Load(temp_waits.addr) : 0, temp_held.exchange(0), temp_held_us.exchange(0) / 1000.0,
                    Libraries::VideoOut::temp_vblank_loops.exchange(0),
                    Libraries::VideoOut::temp_vblank_flips.exchange(0), temp_late.exchange(0)));
    const double* m = temp_render_ms;
    Log(std::format("TEMP render phases ms/frame: beginframe {:.2f} chain {:.2f} | setup {:.2f} walk {:.2f} blocks {:.2f} tail {:.2f} "
                    "xfer-submit {:.2f} fence {:.2f} readback {:.2f} submit {:.2f}; guest memory queries {}/s; GPU xfer {:.2f} copy {:.2f} "
                    "render {:.2f} readback-copy {:.2f} ms, staged {:.2f} MB, texture uploads {:.1f}",
                    m[0] / n, m[1] / n, m[2] / n, m[3] / n, m[4] / n, m[5] / n, m[6] / n, m[7] / n, m[8] / n, m[9] / n,
                    GuestMemory::temp_queries.exchange(0), m[10] / n, m[11] / n, m[12] / n, m[13] / n, m[14] / n, m[15] / n));
    if (auto* r = Renderer()) {
        const auto& s = r->GetStats();
        Log(std::format("TEMP vram MB: mirror {:.0f}, image blocks {:.0f} (free {:.0f}), dedicated images {:.0f}, device buffers {:.0f}; {} guest textures held",
                        s.vram_mb[0], s.vram_mb[1], s.vram_mb[2], s.vram_mb[3], s.vram_mb[4], s.vram_textures));
    }
    std::fill(std::begin(temp_render_ms), std::end(temp_render_ms), 0.0);
    temp_t = {}, temp_waits = {};
}

void Run(const std::stop_token& token) {
    Common::SetCurrentThreadName("GOW3 GPU");
    Cpu::Place(Cpu::Role::GpuThread);
    CpuProbe::Register(1);
    bool in_flight = false;  // the GPU may still run the last frame submitted
    for (;;) {
        std::function<void()> submit;
        const auto t_wait = DiagNow();
        {
            std::unique_lock lock(queue_mutex);
            const auto ready = [] { return !submits.empty(); };
            // Between submits, the frame in flight retires as soon as the GPU is done with it (looked at every millisecond):
            // the game reads its luminance back for the exposure, which pulses when it arrives a frame late.
            while (in_flight && !queue_cv.wait_for(lock, token, std::chrono::milliseconds(1), ready) && !token.stop_requested()) {
                lock.unlock();
                in_flight = !live.load(std::memory_order_acquire)->Retired();
                lock.lock();
            }
            if (!queue_cv.wait(lock, token, ready)) return;
            submit = std::move(submits.front());
            submits.pop_front();
        }
        GOW3_DIAG temp_t.idle += DiagMs(t_wait, std::chrono::steady_clock::now());
        submit();
        in_flight = live.load(std::memory_order_acquire) != nullptr;
        LogTempStats();
    }
}
} // namespace

// ---- Lifecycle and submission ----
void Configure(unsigned anisotropy, bool motion_blur, bool taa) { configured_anisotropy = anisotropy, configured_motion_blur = motion_blur, configured_taa = taa; }

void Start() {
    CpuProbe::Start();
    gpu_thread = std::jthread(Run);
    if (S4::enabled) S4::Start();
}

void SetSpareArenas(u64 half, u64 small) { resting[0] = half, resting[1] = small; }

void Submit(std::span<const u32* const> buffers, std::span<const u32> dwords) {
    CpuProbe::Register(0);
    // The game submits one frame per iteration. Pace the producer as well as the presenter so it does not render
    // hundreds of copies of the same movie frame. Its movie clock uses this rate as the frame-time divisor.
    static auto last_movie_frame = std::chrono::steady_clock::time_point{};
    static float previous_rate = 0;
    const auto* eboot = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
    if (eboot) {
        const u64 delta = eboot->GetBaseAddress() - kGhidraBase;
        const bool playing = *reinterpret_cast<const u64*>(delta + 0x02a79278) != 0;
        auto& rate = *reinterpret_cast<float*>(delta + 0x013cdc80);
        if (playing) {
            if (!previous_rate) previous_rate = rate, rate = GameFrameRate(60);
            const auto now = std::chrono::steady_clock::now();
            const auto deadline = last_movie_frame + std::chrono::nanoseconds(FramePeriodNs(60));
            if (last_movie_frame != std::chrono::steady_clock::time_point{} && deadline > now)
                Common::AccurateSleep(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now), nullptr, false);
            last_movie_frame = std::chrono::steady_clock::now();
        } else if (previous_rate) {
            rate = previous_rate, previous_rate = 0, last_movie_frame = {};
        }
        cinematic_active.store(playing, std::memory_order_release);
    }
    std::vector<std::pair<const u32*, u32>> copy(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) copy[i] = {buffers[i], dwords[i]};
    const u64 reflex_frame = Presenter::EndSimulation();
    const u64 n = submitted.fetch_add(1, std::memory_order_relaxed) + 1, t_submit = DebugEnabled() ? Clock() : 0;
    const Recorder::GameThread game = DebugEnabled() ? Recorder::SampleGameThread() : Recorder::GameThread{};
    FrameMemory mem = render_off ? FrameMemory{} : CaptureFrameMemory();
    const u64 t_captured = DebugEnabled() ? Clock() : 0;
    { std::scoped_lock lock(queue_mutex);
      submits.emplace_back([copy = std::move(copy), mem = std::move(mem), n, t_submit, t_captured, game, reflex_frame] {
          const auto t0 = DiagNow();
          static Recorder::Frame unrecorded;  // the frame recorder's record, outside a debug run: written only in one
          Recorder::Frame& rec = DebugEnabled() ? Recorder::frames.Next() : unrecorded;
          GOW3_DIAG {
              rec.n = n, rec.submit = t_submit, rec.captured = t_captured, rec.game = game, rec.start = Clock();
              if (submitted.load(std::memory_order_relaxed) > n) temp_late.fetch_add(1, std::memory_order_relaxed);
          }
          Presenter::BeginRender(reflex_frame);
          const std::vector<const u32*>* effects = nullptr;  // the renderer's walk collected the packets Process executes
          if (auto* r = Renderer()) {
              Render(*r, copy, mem, rec, n);
              effects = S4::enabled ? nullptr : r->Effects();
              GOW3_DIAG {
                  const auto& s = r->GetStats();
                  temp_t.record += s.record_ms, temp_t.retire += s.gpu_ms;
                  CpuProbe::Frame(n);
                  for (int k = 0; k < 14; ++k) temp_render_ms[2 + k] += s.temp_ms[k], rec.ms[2 + k] = float(s.temp_ms[k]);
                  static u64 draws_seen = 0, queries_seen = 0;
                  const u64 q = GuestMemory::temp_queries.load(std::memory_order_relaxed);  // zeroed each second by the per-second line
                  rec.compiles = s.compiles, rec.compile_waits = s.compile_waits, rec.binds = s.binds, rec.evicts = s.evicts;
                  rec.compile_wait_ms = s.compile_wait_ms, rec.bind_ms = s.bind_ms, rec.evict_ms = s.evict_ms, rec.reaped = s.reaped, rec.reap_max_ms = s.reap_max_ms;
                  rec.draws = u32(s.draws - draws_seen), rec.queries = u32(q >= queries_seen ? q - queries_seen : q);
                  draws_seen = s.draws, queries_seen = q;
                  for (const auto& c : s.compiled) {
                      auto& x = Recorder::compiles.Next();
                      x = {n, c.prog, c.key, c.stage, c.changed, c.slot, c.us};
                      std::copy_n(c.was, 2, x.was), std::copy_n(c.now, 2, x.now);
                      Recorder::compiles.Commit();
                  }
              }
          } else {
              copied.store(n, std::memory_order_release);
#ifdef _WIN32
              WakeByAddressAll(&copied);
#else
              Common::FutexWakeAll(&copied);
#endif
          }
          Presenter::EndRender(reflex_frame);
          const auto t1 = DiagNow();
          GOW3_DIAG rec.rendered = Clock();
          const auto waits = temp_waits;
          if (effects)
              for (const u32* h : *effects) Execute(*h, h + 1, (*h >> 16 & 0x3fff) + 1);
          else
              for (auto [dcb, size] : copy) Process(dcb, size);
          GOW3_DIAG {
              rec.label_waits = u32(temp_waits.n - waits.n), rec.label_wait_ms = float(temp_waits.ms - waits.ms), rec.processed = Clock();
              Recorder::frames.Commit();
              temp_t.render += DiagMs(t0, t1), temp_t.process += DiagMs(t1, std::chrono::steady_clock::now()), ++temp_t.frames;
          }
          if (S4::enabled) S4::Arm();
      }); }
    queue_cv.notify_one();
}

void AwaitCopies() {
    // Without spares the reset takes the last submit's arenas; with them, those of the submit before it.
    const u64 s = submitted.load(std::memory_order_acquire), lag = resting[0] != 0;
    if (s <= lag) return;
    const u64 n = s - lag;
    if (copied.load(std::memory_order_acquire) >= n) return;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(10);
    for (u64 done; (done = copied.load(std::memory_order_acquire)) < n;) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {  // the copy needs nothing from the game thread
            Log(std::format("GPU thread: submit {} not copied after 10 s (copied {}); the frame loop goes on", n, copied.load()));
            break;
        }
#ifdef _WIN32
        WaitOnAddress(&copied, &done, sizeof(done), DWORD(std::chrono::ceil<std::chrono::milliseconds>(remaining).count()));
#else
        Common::FutexWait(copied, done, remaining);
#endif
    }
    GOW3_DIAG temp_held.fetch_add(1, std::memory_order_relaxed),
        temp_held_us.fetch_add(u64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
}

// ---- Display ----
bool DisplaySurface(u64 address, Gpu::Surface& out) {
    auto* r = live.load(std::memory_order_acquire);
    return r && address && r->FindTarget(address, out);
}

// ---- Video frame state ----
void TraceDecodedVideo(u64 address, u32 width, u32 height, u64 tag) {
    for (auto& slot : VideoVersions::slots) {
        u64 current = slot.address.load(std::memory_order_acquire);
        if (current != address && (current != 0 || !slot.address.compare_exchange_strong(current, address))) continue;
        slot.chroma.store(address + u64(Common::AlignUp(width, 64u)) * Common::AlignUp(height, 16u), std::memory_order_relaxed);
        const u64 serial = VideoVersions::next_serial.fetch_add(1, std::memory_order_relaxed) + 1;
        slot.serial.store(serial, std::memory_order_release);
        if (VideoTrace::enabled) VideoTrace::Add(VideoTrace::Decoded, {Clock(), address, serial, tag, 0, width, height, 0});
        break;
    }
}

void TraceVideoTexture(u64 address, u64 frame, u32 flags) {
    if (!VideoTrace::enabled) return;
    bool chroma = false;
    if (const u64 serial = VideoVersions::Serial(address, &chroma))
        VideoTrace::Add(VideoTrace::Textures, {Clock(), address, serial, frame, chroma ? 4u : 3u, 0, 0, flags});
}

u64 VideoFrameVersion(u64 address) { return VideoVersions::Serial(address); }

void ResetVideoFrameVersions() {
    for (auto& slot : VideoVersions::slots) {
        slot.serial.store(0, std::memory_order_release);
        slot.address.store(0, std::memory_order_release);
    }
}

// The game's Video target (DAT_02a79280, DAT_02a794f0 x DAT_02a794f4 RGBA) while it converts decoded frames: rendered and
// sampled on the GPU only.
bool IsVideoOutput(u64 address, u64 size) {
    const auto* eboot = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
    if (!eboot) return false;
    const u64 delta = eboot->GetBaseAddress() - kGhidraBase;
    const auto* dim = reinterpret_cast<const u32*>(delta + 0x02a794f0);
    const u64 base = (u64(dim[2]) | (u64(dim[3] & 0x3f) << 32)) << 8;
    return size == u64(dim[0]) * dim[1] * 4 && *reinterpret_cast<const u64*>(delta + 0x02a79280) == address &&
           VideoVersions::Serial(base);
}

bool CinematicActive() { return cinematic_active.load(std::memory_order_acquire); }

} // namespace Gow3::GpuThread
