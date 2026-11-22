// SPDX-License-Identifier: GPL-2.0-or-later
// Vulkan back end of the PS4 GPU: executes a frame's PM4 chain. Guest memory the GPU reads is mirrored 1:1 in a sparse
// buffer (device address = mirror base + guest address), programs go through the GCN translator, state is fully dynamic
// (shader objects), and render targets are images keyed by their guest base.
#pragma once
#include "main/host.h"
#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace Gpu {
using Gow3::DebugAuto, Gow3::DebugDeep, Gow3::DebugEnabled, Gow3::DebugFlag, Gow3::DebugLevel, Gow3::DebugOption, Gow3::DiagNow;
// Debug: images made (MakeImage) and guest texture checks (Check) by every recording thread, time and count; outside a debug
// run the timer reads no clock and counts nothing.
inline std::atomic<std::uint64_t> temp_make_ns{0}, temp_makes{0}, temp_check_ns{0}, temp_checks{0};
struct TempTimer {
    std::atomic<std::uint64_t>& ns;
    std::atomic<std::uint64_t>& n;
    std::chrono::steady_clock::time_point t0 = Gow3::DiagNow();
    ~TempTimer() {
        GOW3_DIAG ns.fetch_add(std::uint64_t((std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed), n.fetch_add(1, std::memory_order_relaxed);
    }
};
// A device created elsewhere (the game window's, through CreateDevice in device.h); `queue_lock` guards the shared queue.
struct External {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    std::uint32_t family;
    std::mutex* queue_lock;
};

// A render target's image (layout GENERAL), for presentation.
struct Surface {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t w = 0, h = 0;
};

// CPU view of guest memory: pointer to `size` readable bytes at `addr`, or null. It refers to the caller's callable, which
// outlives the Submit it is passed to (no copy, no allocation, one indirect call).
class CpuRead {
    const void* obj;
    const void* (*call)(const void*, std::uint64_t, std::uint64_t);

public:
    template <class F>
    CpuRead(const F& f) : obj(&f), call([](const void* o, std::uint64_t a, std::uint64_t n) -> const void* { return (*static_cast<const F*>(o))(a, n); }) {}
    const void* operator()(std::uint64_t addr, std::uint64_t size) const { return call(obj, addr, size); }
};

struct Segment {
    const std::uint32_t* dw;
    std::size_t count;
};

struct Stats {
    std::uint64_t draws = 0, dispatches = 0, skipped = 0, shaders = 0, shader_fail = 0, targets = 0, textures = 0, uploaded = 0;
    std::uint64_t validation = 0;
    double record_ms = 0, gpu_ms = 0;  // last Submit: CPU until the queue submit, then the wait for the GPU
    // Debug: last Submit's phases (setup, walk, blocks, tail, xfer submit, fence, readback, submit), the frame before's GPU
    // time (xfer, copy, render, readback copy), MB staged and textures uploaded.
    double temp_ms[14] = {};
    std::uint64_t temp_barriers[2] = {};  // debug: last Submit's full barriers, attachment barriers
    // Last Submit's stalls (frame recorder): programs sent to compile, recording waits for one, sparse binds (each waits
    // for the queue), texture evictions and regrows (a regrow or a full bindless array after a wait for the frame in
    // flight, not counted here).
    std::uint32_t compiles = 0, compile_waits = 0, binds = 0, evicts = 0;
    float compile_wait_ms = 0, bind_ms = 0, evict_ms = 0;
    std::uint32_t reaped = 0;  // evicted textures' objects destroyed on their thread meanwhile, and the longest of them
    float reap_max_ms = 0;
    // Debug: device memory by owner (MB) when GetStats was last called: the guest memory mirror, image blocks, their free
    // part, dedicated images, device-local buffers; and the guest textures held.
    float vram_mb[5] = {};
    std::uint32_t vram_textures = 0;
    // Programs compiled since the last Submit (frame recorder) and what made each a new variant, against its program's
    // last one. changed: 1 first variant, 2 code, 4 fetch shader, 8 RSRC1, 16 RSRC2, 32 rect list, 64 slot count, 128 the
    // spec of slot `slot` (`was` -> `now`: dim | class << 4 | unnormalized << 8 | degamma << 9 | srgb << 10 | vfmt << 11 |
    // dfmt << 12 | nfmt << 16 | dst_sel << 20, then the part-of-image flag), 256 PS state, 512 compute threads,
    // 1 << 31 prefetched (Discover), not a draw's miss; us: translation and compile time.
    struct Compile {
        std::uint64_t prog, key;
        std::uint32_t stage, changed, slot, us, was[2], now[2];
    };
    std::vector<Compile> compiled;
    std::vector<std::string> notes;  // first occurrence of each unsupported case
    std::vector<std::string> report;  // TEMP diagnostics (debug runs): lines for the log; the caller clears them
    bool trace = false;              // one line per draw in `trace_lines` (offline replay diagnostics)
    std::uint64_t stop = ~0ull;      // draws after this index of each frame are not executed (diagnostics)
    std::vector<std::string> trace_lines;
    std::vector<std::pair<std::string, std::string>> trace_spirv;  // name, SPIR-V bytes of each translated program (trace only)
    // Debug runs with GOW3_BOOM=1: draws of the retired frame with exploded vertices, keyed by vertex shader, pixel shader and
    // vertices drawn (`key`), and their first one's loads {pc, address / 4, value, writers, this frame's upload there, the next frame's}.
    struct Boom {
        std::uint64_t key;
        std::string head;
        std::vector<std::uint32_t> loads;
    };
    std::vector<Boom> boom;
    // Debug runs, the last Submit (`frame`): the guest ranges its draws read (vertex, constant and index buffers, by the draw's
    // vertex shader), the ranges its mirror upload skipped because the frame in flight writes them, and the pieces that changed
    // while they were copied to the staging (torn snapshots: bytes that differ between the copy and the memory right after it,
    // the first's and last's offsets in the piece; prog, lo, hi: the first read over them, `hits` reads in all, set by FrameDump).
    std::uint64_t frame = 0;
    struct Read {
        std::uint64_t lo, hi, prog;
    };
    std::vector<Read> reads;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> gpu_reads;  // TEMP: every range given to Track (FrameDump::Boom's coverage)
    std::vector<std::pair<std::uint64_t, std::uint64_t>> skips;
    struct Tear {
        std::uint64_t addr, prog, lo, hi;
        std::uint32_t n, first, last, bytes, hits;
    };
    std::vector<Tear> tears;
};

struct Image {
    std::uint64_t base = 0;
    std::uint32_t w = 0, h = 0, format = 0;  // VkFormat
    bool depth = false;
    std::vector<std::uint8_t> rgba;  // 8-bit RGBA, depth as gray
    std::uint32_t level = 0;         // ReadTextures: mip
};

class Renderer {
public:
    // ---- Lifecycle and configuration ----
    Renderer();
    ~Renderer();
    bool Init(bool validation, std::string& error);  // own instance and device; defined only by tools/framecap
    bool Init(const External& device, std::string& error);
    void SetAnisotropy(unsigned level);
    void SetMotionBlurEnabled(bool enabled);
    void SetMotionBlurFrameTime(float seconds);

    // ---- Guest memory, occlusion queries and display surfaces ----
    // Guest memory the GPU may read; staged and copied to the mirror at the next Submit. True when any of it changed.
    bool Write(std::uint64_t addr, const void* data, std::uint64_t size);
    // Many at once, hashed on the recording threads (outside recording); `changed` as Write's result.
    struct WriteRange {
        std::uint64_t addr;
        const void* data;
        std::uint64_t size;
        bool changed = false;
        bool capture = false;  // hash a worker-local snapshot, then stage only changed pieces without rereading the guest
        const std::uint64_t* blocks = nullptr;  // capture, page-aligned: only the 512-byte blocks set here (a bit each from addr)
    };
    void Write(std::span<WriteRange> ranges);
    // Live: called with every guest range [lo, hi) a frame's GPU work reads (textures, index and vertex/constant buffers,
    // descriptor tables), from every recording thread at once; it Writes what the mirror lacks (serialized) and returns
    // true when it staged new bytes.
    void SetTracker(std::function<bool(std::uint64_t lo, std::uint64_t hi)> track);
    // Live linear readback: after each Submit, the bytes of every guest range the GPU wrote (storage images, linear
    // color targets), for the caller to store in guest memory.
    void SetReadback(std::function<std::uint64_t(std::uint64_t addr, const void* data, std::uint64_t size)> readback);
    // Live: the 128-byte occlusion query result blocks in [lo, hi) the CPU reads (8 {begin, end} pairs of sample counts).
    // Their queries run on the GPU; each Submit stores (readback) the counts of the last frame retired into its blocks.
    void SetQueries(std::uint64_t lo, std::uint64_t hi);
    // The single-sampled color target at `base`; callable from any thread.
    bool FindTarget(std::uint64_t base, Surface& out);

    // ---- Frame submission and retirement ----
    // Runs one frame: segments in chain order, registers carried over from the previous frame.
    // Live: `deferred` guest spans (17 MB in all, out to whole pages) are staged after recording, only in the 512-byte blocks the
    // frame's work gave Track; `staged` runs once they are read (the caller may let them change from then on).
    void Submit(std::span<const Segment> chain, const CpuRead& cpu, std::span<const std::pair<std::uint64_t, std::uint64_t>> deferred = {},
                const std::function<void()>& staged = {});
    // The last Submit's packets that act outside the GPU (WRITE_DATA, WAIT_REG_MEM, EVENT_WRITE_EOP/EOS, RELEASE_MEM,
    // DMA_DATA), by header in chain order, so the caller executes them without walking the chain again; null when the
    // chain held an INDIRECT_BUFFER (its packets are not walked).
    const std::vector<const std::uint32_t*>* Effects() const;
    // The frame in flight, once the GPU is done with it, retired now (its writes reach guest memory) rather than at the next
    // Submit; true when no frame is in flight. The game reads some of them back within a frame: its exposure loop
    // (FUN_011711d0) reads the scene's luminance, and a frame of extra delay makes the lighting pulse.
    bool Retired();

    // ---- Inspection and diagnostics ----
    std::vector<Image> ReadTargets();  // every render target, after the last Submit
    // Diagnostics: mips 0 and 2 (layer 0) of every 2D guest texture, raw channels. `traced`: only the textures the last traced
    // frame bound, mip 0 at most 2048 texels wide or high, alpha kept; one that cannot be read comes without pixels.
    std::vector<Image> ReadTextures(bool traced = false);
    Stats& GetStats();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace Gpu
