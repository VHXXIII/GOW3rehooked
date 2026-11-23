// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <functional>
#include <span>
#include "orbis/common/types.h"

namespace Gpu {
struct Surface;
}

// The GPU thread: each submit is drawn by the Vulkan renderer (gpu/vk_gpu.cpp) and waited for, then its PM4 is
// walked in order for what the guest observes in memory (fences, labels, interrupts, memory writes and copies), so labels
// follow the GPU's work. GOW3_NULL_GPU=1 skips the drawing.
namespace Gow3::GpuThread {
// ---- Lifecycle and submission ----
void Configure(unsigned anisotropy, bool motion_blur, bool taa = false, float taa_blend = 0.12f, float taa_sharpness = 0.3f, float taa_clamp = 1.25f);
void Start();
// Command buffers as (address, dword count); processed asynchronously in submission order.
void Submit(std::span<const u32* const> buffers, std::span<const u32> dwords);
// The frame loop's start (hooks.cpp), before 011F0F00 resets arenas (hooks.cpp FlipArenas): waits until the renderer has
// copied the memory of every submit the reset may reuse: all of them, or all but the last while spare arenas rotate. The game
// submits a frame during the next one and checks the GPU's progress only mid-frame (label F−2), after its first updates
// already rewrote that arena; the real GPU has read it by then.
void AwaitCopies();
// The arena descriptors FlipArenas holds aside this pass (the half, the 1 MB one), from the game's main thread: regions too.
void SetSpareArenas(u64 half, u64 small);

// ---- Display ----
// The image the renderer drew at a display buffer's guest address; callable from any thread.
bool DisplaySurface(u64 address, Gpu::Surface& out);

// ---- Video frame state ----
void TraceDecodedVideo(u64 address, u32 width, u32 height, u64 tag);
u64 VideoFrameVersion(u64 address);
void ResetVideoFrameVersions();
bool IsVideoOutput(u64 address, u64 size);
bool CinematicActive();

// ---- Frame timing recorder & video/audio trace (debug/frame_recorder.cpp, debug/video_trace.cpp) ----
// Frame recorder (debug runs only; no-ops otherwise): each flip (vblank thread) and present (presentation thread); the file,
// when the window closes. A flip: the vblank thread's wake-up after its deadline, of which `stale_us` was already overdue
// when it planned the sleep (it was busy before), and the microseconds the previous pass spent completing the flip and
// on the vblank events.
void RecordFlip(s64 late_ns, u32 pending, u32 stale_us, u32 flip_us, u32 event_us);
void RecordPresent(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point acquired);
void WriteFrames();
void TraceVideoTexture(u64 address, u64 frame, u32 flags);
void TraceVideoCopy(u64 address, u64 frame, bool end);
bool VideoTraceEnabled();
u64 VideoTraceClock();
void TraceAudioQueue(u64 stream, u64 start, u64 schedule, u32 queued, u32 threshold, u32 buffer_size, u32 rate, u32 channels);
void TraceAudioFlow(u32 kind, u64 port, s64 value = 0);
void TraceVideoReadback(u64 address, u64 size, u64 frame, bool skipped);
} // namespace Gow3::GpuThread
