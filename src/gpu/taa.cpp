// SPDX-License-Identifier: GPL-2.0-or-later
// Temporal anti-aliasing (opt-in, anti_aliasing=taa). The scene (MSAA targets) is drawn with a sub-pixel viewport jitter; before
// the bloom reads the game's resolved scene colour, this pass blends it with the previous frames' result (history clamped to
// the 3x3 neighbourhood of the current frame, so camera and object motion cannot leave a trail) and sharpens it slightly.
#include "gpu/renderer_impl.h"
#include "main/host.h"

namespace Gpu {
namespace {
constexpr const char* kTaaSource = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, FMT) uniform readonly image2D cur;
layout(binding = 1, FMT) uniform readonly image2D hist;
layout(binding = 2, FMT) uniform writeonly image2D outi;
layout(push_constant) uniform P { ivec2 size; uint first; float alpha; vec2 jit; float sharp; float clampw; } p;
vec3 Tm(vec3 c) { return c / (1.0 + max(max(c.r, c.g), c.b)); }
vec3 Un(vec3 c) { return c / max(1.0 - max(max(c.r, c.g), c.b), 1e-3); }
vec4 L0(ivec2 c) { return imageLoad(cur, clamp(c, ivec2(0), p.size - 1)); }
// The scene was drawn shifted by the jitter: sample it back where the unshifted image would have it (bilinear).
vec4 Ld(ivec2 c) {
    const vec2 q = vec2(c) + p.jit, f = floor(q), w = q - f;
    const ivec2 i = ivec2(f);
    return mix(mix(L0(i), L0(i + ivec2(1, 0)), w.x), mix(L0(i + ivec2(0, 1)), L0(i + ivec2(1, 1)), w.x), w.y);
}
void main() {
    const ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= p.size.x || c.y >= p.size.y) return;
    const vec4 s = Ld(c);
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), lo = vec3(1e9), hi = vec3(-1e9), sum4 = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const vec3 n = Tm(Ld(c + ivec2(x, y)).rgb);
            m1 += n, m2 += n * n, lo = min(lo, n), hi = max(hi, n);
            if ((x == 0) != (y == 0)) sum4 += n;
        }
    const vec3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0));
    const vec3 cur_t = Tm(s.rgb);
    vec3 res = cur_t;
    if (p.first == 0u) {
        const vec3 h = Tm(imageLoad(hist, c).rgb);
        const vec3 bl = mu - p.clampw * sigma, bh = mu + p.clampw * sigma;
        const vec3 hc = clamp(h, min(bl, bh), max(bl, bh));
        res = mix(hc, cur_t, p.alpha);
    }
    res = max(res + p.sharp * (res - sum4 * 0.25), vec3(0.0));
    imageStore(outi, c, vec4(Un(min(res, vec3(0.999))), s.a));
}
)";
}  // namespace

void Renderer::Impl::TaaJitter(float& x, float& y) const {
    auto halton = [](u32 i, u32 b) {
        float f = 1.0f, r = 0.0f;
        for (; i; i /= b) f /= float(b), r += f * float(i % b);
        return r;
    };
    if (u32 ran = taa.ran_frame.load(); ran == ~0u || frame - ran > 8) return;  // no resolve ran: a jitter nothing averages away
    const u32 i = taa.count.load() % 8 + 1;  // one offset per game frame (it spans several submits), not per submit
    x += halton(i, 2) - 0.5f, y += halton(i, 3) - 0.5f;
}

void Renderer::Impl::RunTaa() {
    if (taa.failed) return;
    Target* t = nullptr;
    {
        std::scoped_lock lock(targets_lock);
        for (const u64 base : {kTaaResolved, kTaaResolved2})  // the game alternates between two resolved buffers
            TargetsAt(base, [&](Target& c) {
                if (c.depth || c.samples != 1 || std::atomic_ref(c.rendered).load(std::memory_order_relaxed) != frame) return;
                if (!t || c.w * c.h > t->w * t->h) t = &c;
            });
    }
    if (!t) return;  // a scene without it (menu, video): the jitter stops by itself
    auto fail = [&](const std::string& why) {
        taa.failed = true;
        Gow3::Log("TAA: disabled: " + why);
    };
    if (t->fmt != VK_FORMAT_R16G16B16A16_SFLOAT && t->fmt != VK_FORMAT_R8G8B8A8_UNORM)
        return fail(std::format("resolved scene format {} is not RGBA8 or RGBA16F", (int)t->fmt));
    if (taa.fmt != VK_FORMAT_UNDEFINED && taa.fmt != t->fmt) return fail("resolved scene format changed");
    if (!taa.pipe) {
        const VkDescriptorSetLayoutBinding b[]{{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
        const VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr,
                                                  VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 3, b};
        const VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        const VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &taa.set, 1, &pc};
        std::string err;
        const auto spirv = compiler.Spirv(kTaaSource, shaderc_compute_shader, err, {{"FMT", t->fmt == VK_FORMAT_R8G8B8A8_UNORM ? "rgba8" : "rgba16f"}});
        if (spirv.empty()) return fail("shader: " + err.substr(0, 300));
        if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &taa.set) != VK_SUCCESS) return fail("descriptor layout");
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &taa.layout) != VK_SUCCESS) return fail("pipeline layout");
        VkShaderModule mod;
        const VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spirv.size() * 4, spirv.data()};
        vkCreateShaderModule(dev, &mi, nullptr, &mod);
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
        pci.layout = taa.layout;
        const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &taa.pipe);
        vkDestroyShaderModule(dev, mod, nullptr);
        if (r != VK_SUCCESS) return fail("pipeline");
        taa.fmt = t->fmt;
    }
    bool fresh_history = false;
    if (taa.w != t->w || taa.h != t->h || !taa.cur.img) {  // (old images, if any, stay: a size change is rare)
        for (Tex* x : {&taa.cur, &taa.hist, &taa.out}) {
            *x = MakeImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, t->fmt, t->w, t->h, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            if (!x->img) return fail("images");
            fresh.push_back({x->img, VK_IMAGE_ASPECT_COLOR_BIT});
        }
        taa.w = t->w, taa.h = t->h, fresh_history = true;
    }
    const u32 ran = taa.ran_frame.load();
    if (ran == ~0u || frame - ran > 8) fresh_history = true;

    EndRendering(false);
    Add(hz.r, t->t.img);
    if (!hz.fenced) Add(hz.in_r, t->t.img);
    Barrier(cmd, true);
    const VkImageCopy cp{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {t->w, t->h, 1}};
    vkCmdCopyImage(cmd, t->t.img, VK_IMAGE_LAYOUT_GENERAL, taa.cur.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    StageBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taa.pipe);
    const VkDescriptorImageInfo in[]{{VK_NULL_HANDLE, taa.cur.view, VK_IMAGE_LAYOUT_GENERAL},
                                     {VK_NULL_HANDLE, taa.hist.view, VK_IMAGE_LAYOUT_GENERAL},
                                     {VK_NULL_HANDLE, taa.out.view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkWriteDescriptorSet w[]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &in[0]},
                                   {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &in[1]},
                                   {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &in[2]}};
    PushDescriptors(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taa.layout, 0, 3, w);
    float jx = 0, jy = 0;
    if (!fresh_history) TaaJitter(jx, jy);  // what this frame's scene was drawn with (the offset has not advanced yet)
    const struct { int w, h; u32 first; float alpha; float jx, jy; float sharp, clampw; } pc{int(t->w), int(t->h), fresh_history ? 1u : 0u, taa_params[0], jx, jy, taa_params[1], taa_params[2]};
    vkCmdPushConstants(cmd, taa.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc), dyn.pushed = false;
    vkCmdDispatch(cmd, (t->w + 7) / 8, (t->h + 7) / 8, 1);
    StageBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImage(cmd, taa.out.img, VK_IMAGE_LAYOUT_GENERAL, t->t.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    vkCmdCopyImage(cmd, taa.out.img, VK_IMAGE_LAYOUT_GENERAL, taa.hist.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    StageBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    Add(hz.w, t->t.img);
    if (ran != ~0u) taa.gap_sum += frame - ran;
    taa.ran_frame.store(frame);
    taa.advance.store(true);
    const u32 n = ++taa.resolves;
    if (n % 600 == 0) Gow3::Log(std::format("TAA: {} resolves, {:.2f} submits per resolve", n, double(taa.gap_sum) / n));
    if (!taa.logged) taa.logged = true, Gow3::Log(std::format("TAA: first resolve {}x{} at {:x}", t->w, t->h, t->base));
}
} // namespace Gpu
