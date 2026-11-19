// SPDX-License-Identifier: GPL-2.0-or-later
// Shader runtime: identifies a draw's programs, builds the translation key and gets the shader object, translating on a miss.
#include "gpu/shaders/shader_runtime.h"
#include <bit>
#include "gpu/shaders/shader_jit.h"
#include "gpu/shaders/shader_recipes.h"
#include "gpu/textures.h"
#include "gpu/renderer_impl.h"

namespace Gpu {
namespace {
constexpr u32 kClearVs[]{0xBEEB03FF, 7, 0x36020081, 0x34020281, 0x360000C2, 0x4A0202C1, 0x4A0000C1, 0x7E020B01,
                         0x7E000B00, 0x7E040280, 0x7E0602F2, 0xF80008CF, 0x03020001, 0xF800020F, 0x03030303, 0xBF810000};
}

bool Renderer::Impl::ClearVs(std::span<const u32> code) {
    return code.size() >= std::size(kClearVs) && std::equal(std::begin(kClearVs), std::end(kClearVs), code.begin());
}

bool Renderer::Impl::SetupClearRect(std::string& error) {
    clear_rect_spirv = compiler.Spirv(R"(#version 460
layout(location = 0) out vec4 param0;
void main() {
    gl_Position = vec4(2.0 * float(gl_VertexIndex & 1) - 1.0,
                       2.0 * float((gl_VertexIndex >> 1) & 1) - 1.0, 0.0, 1.0);
    param0 = vec4(1.0);
}
)", shaderc_vertex_shader, error);
    if (clear_rect_spirv.empty()) return false;
    clear_rect_vs = Create(clear_rect_spirv, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
    if (!clear_rect_vs) return error = "SDK clear vertex shader creation failed", false;
    return true;
}

bool Renderer::Impl::RectVs(u32 op, const u32* b, const CpuRead& cpu) const {
    if (uc[0x242] != 0x11 || op != 0x2D || b[0] != 3) return false;
    if (!clear_rect_vs) return true;
    const u64 addr = (u64)(sh[0x48] | (u64)sh[0x49] << 32) << 8;
    const auto* code = static_cast<const u32*>(cpu(addr, sizeof(kClearVs)));
    return code && ClearVs({code, std::size(kClearVs)});
}

VkShaderEXT Renderer::Impl::Create(const std::vector<u32>& spirv, VkShaderStageFlagBits stage, VkShaderStageFlags next) {
    return CreateFrom(VK_SHADER_CODE_TYPE_SPIRV_EXT, spirv.data(), spirv.size() * 4, stage, next);
}

// A shader object from SPIR-V or from a driver binary (VK_INCOMPATIBLE_SHADER_BINARY_EXT, a binary of another driver: null).
VkShaderEXT Renderer::Impl::CreateFrom(VkShaderCodeTypeEXT type, const void* code, size_t bytes, VkShaderStageFlagBits stage, VkShaderStageFlags next) {
    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, sizeof(Push)};
    const VkDescriptorSetLayout layouts[2]{set_layout, table_layout};
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.stage = stage, ci.nextStage = next, ci.codeType = type, ci.codeSize = bytes, ci.pCode = code;
    ci.pName = "main", ci.setLayoutCount = 2, ci.pSetLayouts = layouts, ci.pushConstantRangeCount = 1, ci.pPushConstantRanges = &pcr;
    VkShaderEXT s = VK_NULL_HANDLE;
    if (const VkResult e = CreateShaders(dev, 1, &ci, nullptr, &s); e != VK_SUCCESS) {
        if (e != VK_INCOMPATIBLE_SHADER_BINARY_EXT) Report(std::format("vkCreateShadersEXT {}: stage {:x} next {:x} code type {} of {} bytes", (int)e, (u32)stage, (u32)next, (int)type, bytes));
        s = VK_NULL_HANDLE;
    }
    return s;
}

std::vector<u8> Renderer::Impl::Binary(VkShaderEXT s) {
    size_t size = 0;
    if (!GetShaderBinary || GetShaderBinary(dev, s, &size, nullptr) != VK_SUCCESS || !size) return {};
    std::vector<u8> data(size);
    if (GetShaderBinary(dev, s, &size, data.data()) != VK_SUCCESS) return {};
    data.resize(size);
    return data;
}

int FetchSgpr(std::span<const u32> code) {
    for (size_t i = 0; i < code.size() && code[i] != 0xBF810000;) {
        const u32 x = code[i];
        if (x >> 23 == 0x17D && (x >> 8 & 0xFF) == 33) return int(x & 0xFF);
        const u32 e = x >> 26;
        i += e == 0x34 || e == 0x36 || e == 0x38 || e == 0x3A || e == 0x3C || e == 0x3E ? 2
             : x >> 27 == 0x18                                                         ? 1 + ((x & 0x1FF) == 0xFF)
             : x >> 30 == 2                                                            ? 1 + ((x & 0xFF) == 0xFF || (x >> 8 & 0xFF) == 0xFF)
                                                                                       : 1 + ((x & 0x1FF) == 0xFF);
    }
    return -1;
}

// Program bytes up to its OrbShdr footer (the code size the engine stored after s_endpgm).
static std::span<const u32> Program(u64 addr, const CpuRead& cpu) {
    const u32* h = (const u32*)cpu(addr, 8);
    if (!h || h[0] != 0xBEEB03FF) return {};
    const u64 bytes = ((u64)h[1] + 1) * 8;
    const u32* code = (const u32*)cpu(addr, bytes);
    return code ? std::span<const u32>(code, bytes / 4) : std::span<const u32>{};
}
// Where a descriptor or pointer of `n` dwords lies: user data, then 64-bit pointers loaded from memory (null: unreadable).
static const u32* Locate(const Gcn::Path& path, u32 n, const u32* user, const CpuRead& cpu) {
    const u32* w = path.reg + (path.depth ? 2 : n) <= 16 ? user + path.reg : nullptr;
    for (u32 d = 0; d < path.depth && w; ++d)
        w = (const u32*)cpu(((u64)(w[1] & 0xFFFF) << 32 | w[0]) + path.offset[d], d + 1 < path.depth ? 8 : n * 4);
    return w;
}
static std::span<const u32> FetchProgram(u64 addr, const CpuRead& cpu) {
    for (u32 i = 0; i < 0x100; ++i) {
        const u32* w = (const u32*)cpu(addr + i * 4ull, 4);
        if (!w) return {};
        if ((w[0] & 0xFF80FF00) == 0xBE802000) return {(const u32*)cpu(addr, (i + 1) * 4ull), i + 1};
    }
    return {};
}

// Translated program for a draw stage; fills `table` with user data and the slots' bindless indices.
VkShaderEXT Renderer::Impl::Shader(Gcn::Stage stage, u64 prog, const u32* user, const CpuRead& cpu, u32 rsrc1, u32 rsrc2, u64& out_mask,
                                   bool rect, std::vector<u32>& table, u32 blend) {
    RenderProbe::Scope shader_time(RenderProbe::Shader);
    Prepared& p = prep;
    if (!Identify(stage, prog, user, cpu, rsrc1, rsrc2, rect, p)) return VK_NULL_HANDLE;
    p.in.blend_interlock = blend;
    if (track)  // live: the memory of every V# the program loads through, and of the pointers leading to it
        for (const Gcn::Path& bp : *p.paths) {
            const u32* at = bp.reg + (bp.depth ? 2 : 4) <= 16 ? user + bp.reg : nullptr;
            for (u32 d = 0; d < bp.depth && at; ++d) {
                const u64 p = ((u64)(at[1] & 0xFFFF) << 32 | at[0]) + bp.offset[d];
                const u32 n = d + 1 < bp.depth ? 8 : 16;
                Track(p, p + n);
                at = (const u32*)cpu(p, n);
            }
            if (!at) continue;
            const u64 base = (u64)(at[1] & 0xFFFF) << 32 | at[0];
            const u32 stride = at[1] >> 16 & 0x3FFF;
            const u64 size = std::min<u64>(stride ? (u64)stride * at[2] : at[2], 64ull << 20);
            Track(base, base + size), NoteRead(base, base + size);
        }
    // Trace: every V# the program loads through, with what the GPU will find there.
    for (const Gcn::Path& bp : *p.paths) {
        if (!stats.trace || stats.trace_lines.empty()) break;
        const u32* at = bp.reg + (bp.depth ? 2 : 4) <= 16 ? user + bp.reg : nullptr;
        for (u32 d = 0; d < bp.depth && at; ++d)
            at = (const u32*)cpu(((u64)(at[1] & 0xFFFF) << 32 | at[0]) + bp.offset[d], d + 1 < bp.depth ? 8 : 16);
        if (!at) {
            stats.trace_lines.back() += std::format(" | V{} s{} unreadable", "vpc"[(int)stage], bp.reg);
            continue;
        }
        const u64 base = (u64)(at[1] & 0xFFFF) << 32 | at[0];
        const u32 stride = at[1] >> 16 & 0x3FFF, probe = std::min<u32>(stride ? stride * at[2] : at[2], 4096);
        const u8* m = (const u8*)cpu(base, probe);
        const bool mirrored = (base >> 12 < kPieces && written[base >> 12]) ||
                              std::any_of(pending.begin(), pending.end(), [&](const Pending& p) { return base >= p.addr && base < p.addr + p.size; });
        // TEMP: float elements that cannot be geometry (NaN, Inf, |x| >= 2^34): count, first byte offset and bits.
        std::string bad;
        if (const u32 df = at[3] >> 15 & 0xF, comps = df == 4 ? 1 : df == 11 ? 2 : df == 13 ? 3 : df == 14 ? 4 : 0; base && comps && (at[3] >> 12 & 7) == 7) {
            const u32 es = stride ? stride : comps * 4, n = std::min<u32>(stride ? at[2] : at[2] / es, (16u << 20) / es);
            if (const u8* all = n ? (const u8*)cpu(base, (u64)es * (n - 1) + comps * 4) : nullptr) {
                u32 count = 0, first = 0, bits = 0;
                for (u32 i = 0; i < n; ++i)
                    for (u32 c = 0; c < comps; ++c) {
                        u32 v;
                        std::memcpy(&v, all + (u64)i * es + c * 4, 4);
                        if ((v >> 23 & 0xFF) >= 127 + 34 && !count++) first = i * es + c * 4, bits = v;
                    }
                if (count) bad = std::format(" BAD {} of {} @{:x} {:08x}", count, n * comps, first, bits);
            }
        }
        stats.trace_lines.back() += std::format(" | V{} s{}{} {:x} {}x{} f{}/{} {}{}", "vpc"[(int)stage], bp.reg, bp.depth ? "*" : "", base, stride, at[2],
                                                at[3] >> 15 & 0xF, at[3] >> 12 & 7,
                                                !m ? "unreadable" : !mirrored ? "not-mirrored" : std::all_of(m, m + probe, [](u8 x) { return !x; }) ? "zero" : "ok", bad);
    }
    // Draw table (the user data goes as push constants): the scalar loads' constants as the GPU would read them now (past
    // the V#'s range: 0); the bindless element of each slot (0: dummy image / default sampler).
    table.clear();
    for (const Gcn::Const& c : *p.consts) {
        const size_t at = table.size();
        u64 n = (c.hi - c.lo) / 4;
        table.resize(at + n);
        const u32* w = Locate(c.path, c.buffer ? 4 : 2, user, cpu);
        if (!w) continue;
        const u64 base = ((u64)(w[1] & 0xFFFF) << 32 | w[0]) + c.lo;
        if (const u64 stride = w[1] >> 16 & 0x3FFF, lim = stride ? stride * w[2] : w[2]; c.buffer) n = c.lo >= lim ? 0 : std::min(n, (lim - c.lo + 3) / 4);
        if (const u32* s = n ? (const u32*)cpu(base, n * 4) : nullptr) std::copy_n(s, n, table.data() + at);
        if (stats.trace && n)  // TEMP: constants the GPU wrote (the CPU's copy would be stale)
            for (auto* list : {&gpu_writes, &last_writes})
                for (auto& g : *list)
                    if (g.lo < base + n * 4 && g.hi > base) Note(std::format("constants {:x} over a GPU write", base));
    }
    table_ps = stage == Gcn::Stage::Pixel;
    // Everything the variant and the slot elements come from besides the program key (Specify, Texture, Sampler): a
    // repeat of one of the program's last inputs reuses them and redoes only the slots' per-draw checks.
    const u64 ep = Memos().epoch;
    u32 n = 0;  // past kInputs: not kept
    auto put = [&](const u32* w, u32 k) {
        if (n + k <= kInputs) std::memcpy(inputs + n, w, k * 4);
        n += k;
    };
    const u32 head[7]{u32(p.pkey), u32(p.pkey >> 32), rsrc1, u32(rect), u32(ep), u32(ep >> 32), blend};
    put(head, 7);
    if (stage == Gcn::Stage::Pixel) {  // input enables, COL_FORMAT, the 8 CB_COLOR*_INFO (15 registers apart), input controls
        const u32 ps[11]{ctx[0x1B3], ctx[0x1B4], ctx[0x1C5], ctx[0x31C], ctx[0x32B], ctx[0x33A], ctx[0x349], ctx[0x358], ctx[0x367], ctx[0x376], ctx[0x385]};
        put(ps, 11), put(&ctx[0x191], 32);
    }
    if (stage == Gcn::Stage::Compute) put(&sh[0x207], 3);
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const u32 present = p.desc[k] != nullptr;
        put(&present, 1);
        if (present) put(p.desc[k], (*p.slots)[k].kind == Gcn::SlotKind::Sampler ? 4 : 8);
    }
    for (const u32* v : p.fdesc) {
        const u32 w[2]{v != nullptr, v ? v[3] : 0};  // all BufferSpec reads
        put(w, v ? 2 : 1);
    }
    if (!p.stencil_resolve && !stats.trace && n <= kInputs)
        for (u32 i = 0; i < 4; ++i)
            if (const Recent& r = p.use->recent[p.use->order[i]]; r.in.size() == n && !std::memcmp(r.in.data(), inputs, n * 4)) {
                if (RenderProbe::current) ++RenderProbe::current->recent_hits;
                std::rotate(p.use->order, p.use->order + i, p.use->order + i + 1);
                for (size_t k = 0; k < p.slots->size(); ++k)
                    table.push_back(!p.desc[k] || (*p.slots)[k].kind == Gcn::SlotKind::Sampler
                                        ? r.slots[k].e
                                        : Reuse(r.slots[k], p.desc[k], (*p.slots)[k].kind == Gcn::SlotKind::Storage));
                if (p.motion_blur) table.push_back(std::bit_cast<u32>(motion_blur_rate));
                return out_mask = r.out_mask, r.s;
            }
    Specify(stage, p);
    bool subs = false, keep = !stats.trace && n <= kInputs;
    slot_used.clear();
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const auto& slot = (*p.slots)[k];
        const bool tex = p.desc[k] && slot.kind != Gcn::SlotKind::Sampler;
        tex_sub = 0;
        const u32 e = !p.desc[k] ? 0 : tex ? Texture(p.desc[k], p.spec[k], slot.kind == Gcn::SlotKind::Storage, p.stencil_resolve) : Sampler(p.desc[k]);
        table.push_back(e), slot_used.push_back(tex ? tex_used : MemoTex{nullptr, e, VK_NULL_HANDLE, 0}), keep &= !tex || tex_keep;
        if (tex_sub)  // the slot samples part of its image: one variant of the program (the extents come from the T# and the image)
            p.spec[k].part = true, subs = true;
    }
    if (p.motion_blur) table.push_back(std::bit_cast<u32>(motion_blur_rate));
    if (subs) p.key = TranslationKey(p.pkey, p.in, p.spec);
    const VkShaderEXT s = Translated(stage, prog, cpu, p, out_mask);
    if (ProgUse* u = keep && s ? Memos().progs.Find(p.pkey) : nullptr) {  // not a shader still compiling  // found again: compiling may have added program keys
        Recent& r = u->recent[u->order[3]];  // the least recent gives way
        std::rotate(u->order, u->order + 3, u->order + 4);
        r.in.assign(inputs, inputs + n), r.out_mask = out_mask, r.s = s, r.slots = slot_used;
    }
    return s;
}

VkShaderEXT Renderer::Impl::Translated(Gcn::Stage stage, u64 prog, const CpuRead& cpu, Prepared& p, u64& out_mask) {
    const u64 key = p.key;
    const bool clear_rect = clear_rect_vs && p.in.rect && ClearVs(p.in.code);
    auto found = [&] {
        auto it = shaders.find(key);
        if (it == shaders.end()) return false;
        if (auto o = vs_outputs.find(key); o != vs_outputs.end()) out_mask = o->second;
        return true;
    };
    // Traced frames keep the SPIR-V of every program they run, compiled before or not (written with the dump).
    if (stats.trace) {
        std::string name = std::format("{}-{:x}-{:x}", "vpc"[(int)stage], prog, key & 0xFFFF), err;
        if (std::none_of(stats.trace_spirv.begin(), stats.trace_spirv.end(), [&](auto& e) { return e.first == name; })) {
            std::vector<u32> words;
            u64 mask = 0;
            if (clear_rect) words = clear_rect_spirv;
            else Gcn::EmitSpirv(Gcn::Analyze(p.in), p.in, p.spec, words, mask, err);
            stats.trace_spirv.push_back({name, std::string((const char*)words.data(), words.size() * 4)});
        }
    }
    if (clear_rect) return out_mask = 1, clear_rect_vs;
    auto& memo = Memos().shaders;  // compiled shaders stay
    if (const auto* m = memo.Find(key)) return out_mask = m->second, m->first;
    {
        std::shared_lock lock(prog_lock);
        if (found()) return memo[key] = {shaders.find(key)->second, out_mask}, shaders.find(key)->second;
    }
    // First miss of the frame: the rest of the frame's programs go to the compile threads (traced frames compile here,
    // in order). A parallel block's first miss does the same for the rest of the block, so the variants a camera turn
    // reveals together load or translate at once on those threads, not one after another here; the block waits for each.
    if (!stats.trace && !in_worker && !discovered) discovered = true, Discover(cpu, {walk_chain.size(), 0}, false);
    else if (!stats.trace && in_worker && block_now && !block_discovered) block_discovered = true, Discover(cpu, block_now->to, true);
    // First use of this variant in the run (translated now or found in the cache): kept as a recipe for the AOT.
    if (!stats.trace && !p.in.probe && !p.in.boom) Recipes::Instance().Note(key, p.in, p.spec, ShaderCache::Instance().Contains(key));
    Compiled c;
    if (stats.trace) {
        c = Compile(stage, prog, key, p.in,std::vector<u32>(p.in.code.begin(), p.in.code.end()), std::vector<u32>(p.in.fetch.begin(), p.in.fetch.end()),
                    p.spec);
    } else {
        // Sync waits for the compile. Async skips the draw until the workers finish it. Compute always waits.
        const Jit::Mode mode = Jit::mode;
        const bool compute = stage == Gcn::Stage::Compute;
        // Built by an earlier run or the AOT: the driver's binary only loads (no compile), here, so the draw is never skipped.
        const u64 content = mode == Jit::Mode::Async ? ShaderCache::Instance().ContentOf(key) : 0;
        const bool built = content && ShaderCache::Instance().ContainsBinary(BinaryKey(content));
        std::promise<Compiled> here;
        std::shared_future<Compiled> f;
        bool own = false;
        {
            std::unique_lock lock(prog_lock);
            if (found()) return shaders.find(key)->second;
            auto [it, fresh] = compiling.try_emplace(key);
            if (fresh) it->second = built ? here.get_future().share() : Async(stage, prog, p, true), own = built;
            f = it->second;
        }
        if (own) here.set_value(Compile(stage, prog, key, p.in, std::vector<u32>(p.in.code.begin(), p.in.code.end()),
                                        std::vector<u32>(p.in.fetch.begin(), p.in.fetch.end()), p.spec));
        if (mode == Jit::Mode::Async && !compute && !built && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return waiting = true, VK_NULL_HANDLE;
        Stalled(stall_waits, stall_wait_ns, [&] { c = f.get(); });
    }
    std::unique_lock lock(prog_lock);
    if (found()) {  // another block or the WAD's warm-up (Prepare) finished it first: the first to end this compile drops its copy
        if (const auto it = compiling.find(key); it != compiling.end()) {
            compiling.erase(it);
            if (c.s && c.s != shaders.find(key)->second) DestroyShader(dev, c.s, nullptr);
        }
        return shaders.find(key)->second;
    }
    compiling.erase(key);
    if (stage == Gcn::Stage::Vertex) vs_outputs[key] = out_mask = c.out_mask;
    Count(stats.shaders);
    if (!c.s) Count(stats.shader_fail), Note(std::format("shader {:x} stage {}: {}", prog, (int)stage, c.err.substr(0, 300)));
    return shaders[key] = c.s;
}

bool Renderer::Impl::Identify(Gcn::Stage stage, u64 prog, const u32* user, const CpuRead& cpu, u32 rsrc1, u32 rsrc2, bool rect, Prepared& p) {
    RenderProbe::Scope identify_time(RenderProbe::Identify);
    // Code, content hash and fetch SGPR, checked once per program per frame: read again only when a page under the code
    // changed (Version), every frame when that is unknown.
    auto stale = [&](ProgMemo& m, u64 a) {
        if (m.frame == frame) return false;
        m.frame = frame;
        return !m.ver || m.ver != Version(a, m.code.size_bytes());
    };
    ProgMemo& pm = prog_memo[prog | (u64)stage];
    if (stale(pm, prog)) {
        pm.code = Program(prog, cpu);
        pm.hash = ::Gpu::PieceHash::ShaderCode(pm.code.data(), pm.code.size_bytes());
        pm.fetch_sgpr = stage == Gcn::Stage::Vertex ? FetchSgpr(pm.code) : -1;
        pm.ver = Version(prog, pm.code.size_bytes());
    }
    const auto code = pm.code;
    if (code.empty()) return Note(std::format("program {:x} unreadable", prog)), false;
    std::span<const u32> fetch;
    u64 fetch_hash = 0;
    if (const int s = pm.fetch_sgpr; s >= 0 && s < 15)
        if (const u64 fa = (u64)(user[s + 1] & 0xFFFF) << 32 | user[s]) {
            ProgMemo& fm = fetch_memo[fa];
            if (stale(fm, fa)) fm.code = FetchProgram(fa, cpu), fm.hash = ::Gpu::PieceHash::ShaderCode(fm.code.data(), fm.code.size_bytes()), fm.ver = Version(fa, fm.code.size_bytes());
            fetch = fm.code, fetch_hash = fm.hash;
        }
    p.spec.clear(), p.desc.clear(), p.fdesc.clear(), p.in = {}, p.rect = rect;
    p.motion_blur = stage == Gcn::Stage::Pixel && pm.hash == Gcn::MotionBlurCodeHash();
    p.stencil_resolve = stage == Gcn::Stage::Pixel && pm.hash == Gcn::MotionVectorResolveCodeHash();
    Gcn::Input& in = p.in;
    in.stage = stage, in.code = code, in.fetch = fetch, in.rsrc1 = rsrc1, in.rsrc2 = rsrc2, in.probe = stats.trace && stage == Gcn::Stage::Vertex;
    in.boom = boom_on && !in.probe && stage == Gcn::Stage::Vertex;
    in.rect = rect && stage == Gcn::Stage::Vertex;
    static const char* probe_pc = DebugOption("GOW3_PROBE_PC");  // e.g. 100017: after the fetch
    if (in.probe && probe_pc) in.probe_pc = (u32)std::strtoul(probe_pc, nullptr, 16);
    p.pkey = ProgramKey(stage, pm.hash, fetch_hash, rsrc2, in.probe, in.boom);
    // Elements of an unordered_map stay put: the entry outlives the lock. The thread's own entries start over past 4096
    // (programs come and go with the levels; each keeps up to 4 inputs).
    auto& progs = Memos().progs;
    if (progs.used >= 4096 && !progs.Find(p.pkey)) progs.clear();
    ProgUse& u = progs[p.pkey];
    if (!u.ps) {
        std::shared_lock lock(prog_lock);
        if (auto it = slots.find(p.pkey); it != slots.end()) u.ps = &it->second;
    }
    if (!u.ps) {
        Gcn::Analysis an = Gcn::Analyze(in);
        std::unique_lock lock(prog_lock);
        u.ps = &slots.emplace(p.pkey, ProgSlots{std::move(an.slots), std::move(an.buffers), std::move(an.formats), std::move(an.consts), an.attrs, an.stored, an.depth_resolve_slot}).first->second;
    }
    p.use = &u, p.slots = &u.ps->slots, p.paths = &u.ps->buffers, p.consts = &u.ps->consts, p.stored = u.ps->stored;
    p.depth_resolve_slot = u.ps->depth_resolve_slot;
    for (auto& s : *p.slots) p.desc.push_back(Locate(s.path, s.kind == Gcn::SlotKind::Sampler ? 4 : 8, user, cpu));
    for (auto& f : u.ps->formats) p.fdesc.push_back(Locate(f, 4, user, cpu));
    return true;
}

void Renderer::Impl::Specify(Gcn::Stage stage, Prepared& p) {
    Gcn::Input& in = p.in;
    if (stage == Gcn::Stage::Compute) std::copy_n(&sh[0x207], 3, in.threads);  // COMPUTE_NUM_THREAD_X/Y/Z
    if (stage == Gcn::Stage::Pixel) {
        in.ps_input_ena = ctx[0x1B3], in.ps_input_addr = ctx[0x1B4], in.col_format = ctx[0x1C5];
        std::memcpy(in.ps_input_cntl, &ctx[0x191], sizeof(in.ps_input_cntl));
        for (u32 k = 0; k < 32; ++k) in.ps_input_cntl[k] &= -u32(p.use->ps->attrs >> k & 1);  // only the interpolants the code reads count
        for (u32 k = 0; k < 8; ++k) {
            const u32 info = ctx[0x31C + k * 15], nt = info >> 8 & 7;
            if (!(in.col_format >> (4 * k) & 0xF)) continue;
            in.mrt_class[k] = nt == 4 ? 1 : nt == 5 ? 2 : 0;
            const auto swz = AmdGpu::MrtSwizzle(info >> 11 & 3, AmdGpu::DataFormat(info >> 2 & 0x1F));
            in.mrt_map[k] = u8(swz.Map(0) | swz.Map(1) << 2 | swz.Map(2) << 4 | swz.Map(3) << 6);
        }
    }
    SpecifySlots(p);
    if (!p.stencil_resolve) return;
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const u32* w = p.desc[k];
        auto& spec = p.spec[k];
        if (!w || (*p.slots)[k].kind != Gcn::SlotKind::Texture || spec.dim != Gcn::Dim::D2Ms || spec.cls != Gcn::Class::Float ||
            (w[1] >> 20 & 0x3F) != 1 || (w[1] >> 26 & 0xF)) continue;
        const u64 base = ((u64)w[0] | (u64)(w[1] & 0x3F) << 32) << 8;
        if (auto it = stencil_at.find(base); it != stencil_at.end() && it->second.second == frame && it->second.first &&
            it->second.first->samples == (1u << (w[3] >> 16 & 0xF)) && (w[2] & 0x3FFF) + 1 == it->second.first->w &&
            (w[2] >> 14 & 0x3FFF) + 1 == it->second.first->h) {
            const auto sel = DstSel(w);
            spec.stencil = true;
            spec.dst_sel = u16(u32(sel.r) | u32(sel.g) << 3 | u32(sel.b) << 6 | u32(sel.a) << 9);
            p.key = TranslationKey(p.pkey, p.in, p.spec);
        }
    }
}

// The specs of the slots from their descriptors (p.desc, p.fdesc) and the translation key. The AOT calls it with descriptors it made.
void Renderer::Impl::SpecifySlots(Prepared& p) {
    Gcn::Input& in = p.in;
    p.spec.clear();
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const auto& s = (*p.slots)[k];
        const u32* w = p.desc[k];
        p.spec.push_back(!w ? Gcn::SlotSpec{} : s.kind == Gcn::SlotKind::Sampler ? Gcn::SamplerSpec(w) : Gcn::TextureSpec(w));
        if (k != size_t(p.depth_resolve_slot) || p.spec.back().dim != Gcn::Dim::D2Ms || p.spec.back().cls != Gcn::Class::Float)
            p.spec.back().msaa4 = false;
        // Storage views cannot swizzle: the shader applies DST_SEL to its loads and the inverse to its stores (0: identity).
        if (const AmdGpu::CompMapping sel = w && s.kind == Gcn::SlotKind::Storage ? DstSel(w) : AmdGpu::IdentityMapping; sel != AmdGpu::IdentityMapping)
            p.spec.back().dst_sel = u16(u32(sel.r) | u32(sel.g) << 3 | u32(sel.b) << 6 | u32(sel.a) << 9);
    }
    // The engine pairs the fog composite with the fullscreen VS's normalized UV (SPI_PS_INPUT_CNTL_0 = 1: its param 1) yet binds
    // the scene with its unnormalized WhiteBuffer sampler: every pixel would read the corner texel (dark pit). The depth
    // sample at the same UV is normalized: the scene sample takes it as normalized too.
    if (in.stage == Gcn::Stage::Pixel && (in.ps_input_cntl[0] & 0x1F) == 1 && in.code.size() == 128 &&
        ::Gpu::PieceHash::ShaderCode(in.code.data(), in.code.size_bytes()) == Gcn::FogCompositeCodeHash())
        for (auto& s : p.spec) s.unnormalized = false;
    for (const u32* w : p.fdesc) p.spec.push_back(w ? Gcn::BufferSpec(w) : Gcn::SlotSpec{});
    p.key = TranslationKey(p.pkey, in, p.spec);
}

u64 ProgramKey(Gcn::Stage stage, u64 code_hash, u64 fetch_hash, u32 rsrc2, bool probe, bool boom) {
    if (stage == Gcn::Stage::Pixel && code_hash == Gcn::MotionBlurCodeHash()) code_hash ^= 0x6d6f74696f6e3032ull;
    const u64 hashes[2]{code_hash, fetch_hash};
    // Probe and exploded-vertex variants apart: each writes its own record, which only its frames push.
    return ::Gpu::PieceHash::ShaderKey(hashes, sizeof(hashes), (u64)stage << 32 | rsrc2 | (u64)probe << 62 | (boom ? (u64)(0x100 | Gcn::BoomClip()) << 40 | (u64)Gcn::BoomParamLimit() << 49 | 1ull << 63 | 1ull << 62 : 0));  // bit 62: the record carries the constant bank (never with a probe)
}

u64 TranslationKey(u64 program_key, const Gcn::Input& in, std::span<const Gcn::SlotSpec> spec) {
    u64 packed[64], *w = packed;  // the specs field by field
    std::vector<u64> more;
    if (spec.size() > std::size(packed)) more.resize(spec.size()), w = more.data();
    for (size_t k = 0; k < spec.size(); ++k) w[k] = Gcn::Pack(spec[k]);
    u64 key = ::Gpu::PieceHash::ShaderSpecKey(w, spec.size() * 8, program_key ^ in.rsrc1 ^ (in.rect ? 1ull << 63 : 0));
    if (in.stage == Gcn::Stage::Compute) key = ::Gpu::PieceHash::ShaderKey(in.threads, sizeof(in.threads), key);
    if (in.stage == Gcn::Stage::Pixel)
        key = ::Gpu::PieceHash::ShaderKey(in.ps_input_cntl, sizeof(in.ps_input_cntl),
                                   ::Gpu::PieceHash::ShaderKey(&in.mrt_class, 16, key ^ in.ps_input_ena ^ (u64)in.ps_input_addr << 32 ^ in.col_format));
    if (in.blend_interlock) key = ::Gpu::PieceHash::ShaderKey(&in.blend_interlock, sizeof(in.blend_interlock), key);
    return key;
}

// Rect list: three corners of an axis-aligned rectangle; the corner with the right angle is opposite the missing one.
VkShaderEXT Renderer::Impl::RectGs(u64 mask) {
    std::unique_lock lock(prog_lock);
    if (auto it = rect_gs.find(mask); it != rect_gs.end()) return it->second;
    std::string s = "#version 460\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
    for (u64 m = mask; m; m &= m - 1) s += std::format("layout(location = {0}) in vec4 i{0}[];\nlayout(location = {0}) out vec4 o{0};\n", std::countr_zero(m));
    s += R"(void Emit(int k, int a, int b, int c, bool far) {
    gl_Position = far ? gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position : gl_in[k].gl_Position;
)";
    for (u64 m = mask; m; m &= m - 1) s += std::format("    o{0} = far ? i{0}[a] + i{0}[b] - i{0}[c] : i{0}[k];\n", std::countr_zero(m));
    s += R"(    EmitVertex();
}
void main() {
    vec2 p[3];
    for (int k = 0; k < 3; ++k) p[k] = gl_in[k].gl_Position.xy / gl_in[k].gl_Position.w;
    int c = 0;
    float best = 3.4e38;
    for (int k = 0; k < 3; ++k) {
        float d = abs(dot(p[(k + 1) % 3] - p[k], p[(k + 2) % 3] - p[k]));
        if (d < best) best = d, c = k;
    }
    int a = (c + 1) % 3, b = (c + 2) % 3;
    Emit(c, a, b, c, false);
    Emit(a, a, b, c, false);
    Emit(b, a, b, c, false);
    Emit(0, a, b, c, true);
}
)";
    std::string err;
    auto spirv = compiler.Spirv(s, shaderc_geometry_shader, err);
    VkShaderEXT gs = spirv.empty() ? VK_NULL_HANDLE : Create(spirv, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!gs) Note("rect list geometry shader: " + err.substr(0, 300));
    return rect_gs[mask] = gs;
}
} // namespace Gpu
