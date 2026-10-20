#include "gpu/real3d_gpu.hpp"

#include "gpu/present.hpp"
#include "gpu/shaders.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace gpu {

namespace {

// What the shaders read (scene_common.glsl), std430 with 16-byte fields.
struct GpuVertex {
    float x, y, z, u, v, shade, nx, ny, nz;
    uint32_t polygon;
};
static_assert(sizeof(GpuVertex) == 40);

struct GpuPolygon {
    float colour[4];
    float values[4];
    uint32_t tex[4];
    uint32_t tex2[4];
    uint32_t flags[4];
};
static_assert(sizeof(GpuPolygon) == 80);

struct GpuViewport {
    float rect[4], frustum[4], sun[4], light[4], fog_colour[4], fog_colour_set[4], spot[4], spot2[4], spot_colour[4],
        spot_fog_colour[4];
};
static_assert(sizeof(GpuViewport) == 160);

enum : uint32_t {
    kFlagLit = 1,
    kFlagFixedShading = 2,
    kFlagSpecular = 4,
    kFlagSmooth = 8,
    kFlagTextured = 16,
    kFlagContour = 32,
    kFlagTextureAlpha = 64,
    kFlagPolyAlpha = 128,
    kFlagLayered = 256,
    kFlagMicro = 512,
    kFlagInverted = 1024,
};

struct FrameUniform {
    float scale, pad[3];
};
struct FogUniform {
    float base[4], spot[4], ellipse[4];
    float scale, pad[3];
};

constexpr SDL_GPUTextureFormat kColourFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // 0xAARRGGBB in a word

GpuPolygon pack(const m3::Real3dScene::Polygon& p) {
    GpuPolygon g{};
    g.colour[0] = p.r;
    g.colour[1] = p.g;
    g.colour[2] = p.b;
    g.colour[3] = p.alpha;
    g.values[0] = p.fog;
    g.values[1] = p.specular_value;
    g.values[2] = p.lod_base;
    g.tex[0] = p.tx;
    g.tex[1] = p.ty;
    g.tex[2] = p.tw;
    g.tex[3] = p.th;
    g.tex2[0] = p.page_y;
    g.tex2[1] = static_cast<uint32_t>(p.max_level);
    g.tex2[2] = p.wrap_u | uint32_t{p.wrap_v} << 8;
    g.tex2[3] = static_cast<uint32_t>(p.format);
    uint32_t f = 0;
    if (p.lit) f |= kFlagLit;
    if (p.fixed_shading) f |= kFlagFixedShading;
    if (p.specular) f |= kFlagSpecular;
    if (p.smooth) f |= kFlagSmooth;
    if (p.textured) f |= kFlagTextured;
    if (p.contour) f |= kFlagContour;
    if (p.texture_alpha) f |= kFlagTextureAlpha;
    if (p.poly_alpha) f |= kFlagPolyAlpha;
    if (p.layered) f |= kFlagLayered;
    if (p.micro) f |= kFlagMicro;
    if (p.inverted) f |= kFlagInverted;
    g.flags[0] = f;
    g.flags[1] = static_cast<uint32_t>(p.micro_id & 7) | static_cast<uint32_t>(p.micro_min_lod & 3) << 8 |
                 static_cast<uint32_t>(p.shininess & 3) << 16;
    g.flags[2] = p.viewport;
    return g;
}

GpuViewport pack(const m3::Real3dScene::Viewport& v) {
    GpuViewport g{};
    auto set = [](float* to, float a, float b, float c, float d) {
        to[0] = a;
        to[1] = b;
        to[2] = c;
        to[3] = d;
    };
    set(g.rect, v.x, v.y, v.width, v.height);
    set(g.frustum, v.left, v.right, v.bottom, v.top);
    set(g.sun, v.sun[0], v.sun[1], v.sun[2], v.sun_intensity);
    set(g.light, v.ambient, v.fog_density, v.fog_start, v.fog_attenuation);
    set(g.fog_colour, v.fog_colour[0], v.fog_colour[1], v.fog_colour[2], 0);
    set(g.fog_colour_set, v.fog_colour_set[0], v.fog_colour_set[1], v.fog_colour_set[2], 0);
    set(g.spot, v.spot_x, v.spot_y, v.spot_rx, v.spot_ry);
    set(g.spot2, v.spot_start, v.spot_extent, v.spot ? 1.0f : 0.0f, 0);
    set(g.spot_colour, v.spot_colour[0], v.spot_colour[1], v.spot_colour[2], 0);
    set(g.spot_fog_colour, v.spot_fog_colour[0], v.spot_fog_colour[1], v.spot_fog_colour[2], 0);
    return g;
}

} // namespace

// ---- Set-up ----

bool Real3dGpu::open(Device& device, int scale) {
    device_ = &device;
    scale_ = std::max(1, scale);
    SDL_GPUDevice* d = device.get();
    const auto w = static_cast<Uint32>(width()), h = static_cast<Uint32>(height());

    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    if (!SDL_GPUTextureSupportsFormat(d, depth_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;

    auto texture = [&](SDL_GPUTextureFormat format, Uint32 tw, Uint32 th, SDL_GPUTextureUsageFlags usage) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.usage = usage;
        info.width = tw;
        info.height = th;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        return SDL_CreateGPUTexture(d, &info);
    };
    const SDL_GPUTextureUsageFlags target = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    colour_ = texture(kColourFormat, w, h, target);
    layers_[0] = texture(kColourFormat, w, h, target);
    layers_[1] = texture(kColourFormat, w, h, target);
    output_ = texture(kColourFormat, w, h, target);
    depth_copy_ = texture(SDL_GPU_TEXTUREFORMAT_R32_FLOAT, w, h, target);
    depth_ = texture(depth_format, w, h, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    trans_depth_ = texture(depth_format, w, h, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    texture_ram_ = texture(SDL_GPU_TEXTUREFORMAT_R16_UINT, m3::Real3d::kTextureWidth, m3::Real3d::kTextureHeight,
                           SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!colour_ || !layers_[0] || !layers_[1] || !output_ || !depth_copy_ || !depth_ || !trans_depth_ || !texture_ram_) {
        error_ = SDL_GetError();
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = SDL_CreateGPUSampler(d, &sampler);

    SDL_GPUTransferBufferCreateInfo up{};
    up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    up.size = m3::Real3d::kTextureWidth * m3::Real3d::kTextureHeight * 2;
    texture_upload_ = SDL_CreateGPUTransferBuffer(d, &up);
    SDL_GPUTransferBufferCreateInfo down{};
    down.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    down.size = w * h * 4;
    download_ = SDL_CreateGPUTransferBuffer(d, &down);

    // Shaders.
    using R = Device::Resources;
    SDL_GPUShader* fullscreen = device.shader(shaders::present_vert, Device::Stage::Vertex, {});
    SDL_GPUShader* scene_vs = device.shader(shaders::scene_vert, Device::Stage::Vertex, R{.storage_buffers = 2});
    SDL_GPUShader* opaque_fs =
        device.shader(shaders::scene_opaque_frag, Device::Stage::Fragment, R{.samplers = 1, .uniform_buffers = 1, .storage_buffers = 2});
    SDL_GPUShader* trans_fs =
        device.shader(shaders::scene_frag, Device::Stage::Fragment, R{.samplers = 1, .uniform_buffers = 1, .storage_buffers = 2});
    SDL_GPUShader* fog_fs = device.shader(shaders::background_fog_frag, Device::Stage::Fragment, R{.uniform_buffers = 1});
    SDL_GPUShader* copy_fs = device.shader(shaders::depth_copy_frag, Device::Stage::Fragment, R{.samplers = 1});
    SDL_GPUShader* composite_fs = device.shader(shaders::composite_frag, Device::Stage::Fragment, R{.samplers = 3});

    // Pipelines.
    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(GpuVertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    const SDL_GPUVertexAttribute attributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, u)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(GpuVertex, shade)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, nx)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(GpuVertex, polygon)},
    };
    auto pipeline = [&](SDL_GPUShader* vs, SDL_GPUShader* fs, std::initializer_list<SDL_GPUTextureFormat> targets,
                        bool scene, bool depth, SDL_GPUCompareOp compare, bool decal, bool write_colour = true) {
        SDL_GPUColorTargetDescription descriptions[4]{};
        Uint32 n = 0;
        for (SDL_GPUTextureFormat f : targets) {
            descriptions[n].format = f;
            if (!write_colour) {
                descriptions[n].blend_state.enable_color_write_mask = true;
                descriptions[n].blend_state.color_write_mask = 0;
            }
            ++n;
        }
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vs;
        info.fragment_shader = fs;
        if (scene) {
            info.vertex_input_state.vertex_buffer_descriptions = &vb;
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = 5;
        }
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        // Clipped at the near plane (depth clip; SDL's default clamps
        // depth instead, which lets polygons at or behind the eye through).
        info.rasterizer_state.enable_depth_clip = true;
        if (depth) {
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = true;
            info.depth_stencil_state.compare_op = compare;
            if (decal) { // a decal only where no decal is yet (the stencil counts them)
                info.depth_stencil_state.enable_stencil_test = true;
                SDL_GPUStencilOpState s{};
                s.fail_op = SDL_GPU_STENCILOP_KEEP;
                s.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
                s.pass_op = SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
                s.compare_op = SDL_GPU_COMPAREOP_EQUAL;
                info.depth_stencil_state.front_stencil_state = s;
                info.depth_stencil_state.back_stencil_state = s;
                info.depth_stencil_state.compare_mask = 0xFF;
                info.depth_stencil_state.write_mask = 0xFF;
            }
            info.target_info.depth_stencil_format = depth_format;
            info.target_info.has_depth_stencil_target = true;
        }
        info.target_info.color_target_descriptions = descriptions;
        info.target_info.num_color_targets = n;
        return SDL_CreateGPUGraphicsPipeline(d, &info);
    };
    if (fullscreen && scene_vs && opaque_fs && trans_fs && fog_fs && copy_fs && composite_fs) {
        const auto ge = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, gt = SDL_GPU_COMPAREOP_GREATER;
        const SDL_GPUTextureFormat r32 = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        fog_ = pipeline(fullscreen, fog_fs, {kColourFormat}, false, false, ge, false);
        // Opaque: as near as what is there wins (the software renderer's test);
        // translucent: only nearer.
        opaque_[0] = pipeline(scene_vs, opaque_fs, {kColourFormat, kColourFormat, kColourFormat, r32}, true, true, ge, false);
        opaque_[1] = pipeline(scene_vs, opaque_fs, {kColourFormat, kColourFormat, kColourFormat, r32}, true, true, ge, true);
        trans_[0] = pipeline(scene_vs, trans_fs, {kColourFormat}, true, true, gt, false);
        trans_[1] = pipeline(scene_vs, trans_fs, {kColourFormat}, true, true, gt, true);
        copy_ = pipeline(fullscreen, copy_fs, {kColourFormat}, false, true, SDL_GPU_COMPAREOP_ALWAYS, false, false);
        composite_ = pipeline(fullscreen, composite_fs, {kColourFormat}, false, false, ge, false);
    }
    for (SDL_GPUShader* s : {fullscreen, scene_vs, opaque_fs, trans_fs, fog_fs, copy_fs, composite_fs})
        if (s) SDL_ReleaseGPUShader(d, s);
    if (!fog_ || !opaque_[0] || !opaque_[1] || !trans_[0] || !trans_[1] || !copy_ || !composite_ || !nearest_ ||
        !texture_upload_ || !download_) {
        error_ = device.error().empty() ? SDL_GetError() : device.error();
        return false;
    }
    return true;
}

Real3dGpu::~Real3dGpu() {
    if (!device_ || !device_->get()) return;
    SDL_GPUDevice* d = device_->get();
    SDL_WaitForGPUIdle(d);
    for (SDL_GPUGraphicsPipeline* p : {fog_, opaque_[0], opaque_[1], trans_[0], trans_[1], copy_, composite_})
        if (p) SDL_ReleaseGPUGraphicsPipeline(d, p);
    if (nearest_) SDL_ReleaseGPUSampler(d, nearest_);
    for (SDL_GPUTexture* t : {colour_, layers_[0], layers_[1], depth_copy_, depth_, trans_depth_, output_, texture_ram_})
        if (t) SDL_ReleaseGPUTexture(d, t);
    for (SDL_GPUBuffer* b : {vertex_buffer_, polygon_buffer_, viewport_buffer_})
        if (b) SDL_ReleaseGPUBuffer(d, b);
    for (SDL_GPUTransferBuffer* t : {vertex_upload_, polygon_upload_, viewport_upload_, texture_upload_, download_})
        if (t) SDL_ReleaseGPUTransferBuffer(d, t);
}

// ---- A frame ----

// The draw lists, in the software renderer's order (its raster()): per
// priority, the ordinary polygons and then the high-priority ones; in each,
// the opaque pass and the two translucency layers, each viewport by
// viewport with its decals after the rest of it.
void Real3dGpu::build(const m3::Real3dScene& scene) {
    const auto& polys = scene.polygons;
    vertices_.clear();
    groups_.clear();
    polygons_.resize(polys.size() * sizeof(GpuPolygon));
    for (size_t i = 0; i < polys.size(); ++i) {
        const GpuPolygon g = pack(polys[i]);
        std::memcpy(polygons_.data() + i * sizeof(GpuPolygon), &g, sizeof g);
    }
    viewports_.resize(scene.viewports.size() * sizeof(GpuViewport));
    for (size_t i = 0; i < scene.viewports.size(); ++i) {
        const GpuViewport g = pack(scene.viewports[i]);
        std::memcpy(viewports_.data() + i * sizeof(GpuViewport), &g, sizeof g);
    }

    auto emit = [&](std::vector<Draw>& draws, size_t index, bool decal) {
        const m3::Real3dScene::Polygon& p = polys[index];
        const m3::Real3dScene::Vertex* e = scene.vertices.data() + p.first_vertex;
        auto put = [&](const m3::Real3dScene::Vertex& v) {
            const GpuVertex g{v.x, v.y, v.z, v.u, v.v, v.shade, v.nx, v.ny, v.nz, static_cast<uint32_t>(index)};
            const size_t at = vertices_.size();
            vertices_.resize(at + sizeof g);
            std::memcpy(vertices_.data() + at, &g, sizeof g);
        };
        const auto first = static_cast<uint32_t>(vertices_.size() / sizeof(GpuVertex));
        put(e[0]);
        put(e[1]);
        put(e[2]);
        if (p.vertex_count == 4) {
            put(e[0]);
            put(e[2]);
            put(e[3]);
        }
        const uint32_t count = p.vertex_count == 4 ? 6 : 3;
        if (!draws.empty() && draws.back().viewport == p.viewport && draws.back().decal == decal &&
            draws.back().first + draws.back().count == first)
            draws.back().count += count;
        else
            draws.push_back({first, count, p.viewport, decal});
    };

    size_t begin = 0;
    for (int priority = 0; priority < 4; ++priority) {
        size_t end = begin;
        while (end < polys.size() && polys[end].priority == priority) ++end;
        if (begin == end) continue;
        const bool any_high = std::any_of(polys.begin() + static_cast<std::ptrdiff_t>(begin),
                                          polys.begin() + static_cast<std::ptrdiff_t>(end),
                                          [](const m3::Real3dScene::Polygon& p) { return p.high_priority; });
        for (int high = 0; high < (any_high ? 2 : 1); ++high) {
            Group g;
            for (int pass = 0; pass < 3; ++pass)
                for (size_t a = begin; a < end;) {
                    size_t b = a;
                    while (b < end && polys[b].viewport == polys[a].viewport) ++b;
                    for (int decals = 0; decals < 2; ++decals)
                        for (size_t i = a; i < b; ++i) {
                            const m3::Real3dScene::Polygon& p = polys[i];
                            if (p.high_priority != (high == 1) || p.layered != (decals == 1)) continue;
                            const bool wanted = pass == 0 ? !p.poly_alpha
                                                          : (p.texture_alpha || p.poly_alpha) && p.trans_layer == pass - 1;
                            if (wanted) emit(g.passes[pass], i, decals == 1);
                        }
                    a = b;
                }
            groups_.push_back(std::move(g));
        }
        begin = end;
    }
}

// Into `buffer` (grown as needed) through `transfer`.
bool Real3dGpu::upload(SDL_GPUBuffer*& buffer, SDL_GPUTransferBuffer*& transfer, uint32_t& capacity, const void* data,
                       uint32_t size, uint32_t usage, SDL_GPUCopyPass* copy) {
    SDL_GPUDevice* d = device_->get();
    if (size == 0) size = 16; // something to bind
    if (size > capacity) {
        if (buffer) SDL_ReleaseGPUBuffer(d, buffer);
        if (transfer) SDL_ReleaseGPUTransferBuffer(d, transfer);
        capacity = std::max(size, capacity * 2);
        SDL_GPUBufferCreateInfo info{};
        info.usage = usage;
        info.size = capacity;
        buffer = SDL_CreateGPUBuffer(d, &info);
        SDL_GPUTransferBufferCreateInfo t{};
        t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        t.size = capacity;
        transfer = SDL_CreateGPUTransferBuffer(d, &t);
        if (!buffer || !transfer) {
            capacity = 0;
            return false;
        }
    }
    void* p = SDL_MapGPUTransferBuffer(d, transfer, true);
    if (!p) return false;
    if (data) std::memcpy(p, data, size);
    SDL_UnmapGPUTransferBuffer(d, transfer);
    SDL_GPUTransferBufferLocation from{transfer, 0};
    SDL_GPUBufferRegion to{buffer, 0, size};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    return true;
}

void Real3dGpu::render(const m3::Real3dScene& scene, std::span<const uint16_t> texture_ram, uint64_t texture_version,
                       std::vector<uint32_t>* pixels) {
    if (!composite_) return;
    SDL_GPUDevice* d = device_->get();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(d);
    if (!cmd) return;
    const auto w = static_cast<float>(width()), h = static_cast<float>(height()), s = static_cast<float>(scale_);

    auto colour_target = [](SDL_GPUTexture* t, bool clear, SDL_FColor c = {0, 0, 0, 0}) {
        SDL_GPUColorTargetInfo info{};
        info.texture = t;
        info.clear_color = c;
        info.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        info.store_op = SDL_GPU_STOREOP_STORE;
        return info;
    };
    auto set_full_view = [&](SDL_GPURenderPass* pass) {
        const SDL_GPUViewport vp{0, 0, w, h, 0, 1};
        SDL_SetGPUViewport(pass, &vp);
        const SDL_Rect all{0, 0, width(), height()};
        SDL_SetGPUScissor(pass, &all);
    };

    if (scene.block_culled) { // no 3D: white, over the layers below (as the software renderer)
        SDL_GPUColorTargetInfo t = colour_target(output_, true, {1, 1, 1, 1});
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &t, 1, nullptr));
    } else {
        build(scene);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        upload(vertex_buffer_, vertex_upload_, vertex_capacity_, vertices_.data(), static_cast<uint32_t>(vertices_.size()),
               SDL_GPU_BUFFERUSAGE_VERTEX, copy);
        upload(polygon_buffer_, polygon_upload_, polygon_capacity_, polygons_.data(), static_cast<uint32_t>(polygons_.size()),
               SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, copy);
        upload(viewport_buffer_, viewport_upload_, viewport_capacity_, viewports_.data(),
               static_cast<uint32_t>(viewports_.size()), SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, copy);
        if (texture_version_ != texture_version && texture_ram.size() == m3::Real3d::kTextureWidth * m3::Real3d::kTextureHeight) { // texture RAM, whole
            if (void* p = SDL_MapGPUTransferBuffer(d, texture_upload_, true)) {
                const auto ram = texture_ram;
                std::memcpy(p, ram.data(), ram.size_bytes());
                SDL_UnmapGPUTransferBuffer(d, texture_upload_);
                SDL_GPUTextureTransferInfo from{};
                from.transfer_buffer = texture_upload_;
                SDL_GPUTextureRegion to{};
                to.texture = texture_ram_;
                to.w = m3::Real3d::kTextureWidth;
                to.h = m3::Real3d::kTextureHeight;
                to.d = 1;
                SDL_UploadToGPUTexture(copy, &from, &to, true);
                texture_version_ = texture_version;
            }
        }
        SDL_EndGPUCopyPass(copy);

        // The translucency layers start empty.
        {
            SDL_GPUColorTargetInfo t[2] = {colour_target(layers_[0], true), colour_target(layers_[1], true)};
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, t, 2, nullptr));
        }
        // The background fog, in an empty picture.
        {
            SDL_GPUColorTargetInfo t = colour_target(colour_, true);
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, fog_);
            const m3::Real3dScene::BackgroundFog fog = scene.background_fog();
            auto fill = [&](const m3::Real3dScene::Viewport& rect, const FogUniform& u) {
                const float x0 = std::max(0.0f, rect.x), y0 = std::max(0.0f, rect.y);
                const float x1 = std::min(static_cast<float>(kWidth), rect.x + rect.width);
                const float y1 = std::min(static_cast<float>(kHeight), rect.y + rect.height);
                if (x0 >= x1 || y0 >= y1) return;
                const SDL_GPUViewport vp{x0 * s, y0 * s, (x1 - x0) * s, (y1 - y0) * s, 0, 1};
                SDL_SetGPUViewport(pass, &vp);
                const SDL_Rect r{static_cast<int>(x0 * s), static_cast<int>(y0 * s), static_cast<int>((x1 - x0) * s),
                                 static_cast<int>((y1 - y0) * s)};
                SDL_SetGPUScissor(pass, &r);
                SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
                SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            };
            if (fog.ambient >= 0) {
                FogUniform u{{0, 0, 0, fog.ambient_alpha}, {0, 0, 0, 0}, {0, 0, 1, 1}, s, {}};
                fill(scene.viewports[static_cast<size_t>(fog.ambient)], u);
            }
            if (fog.scroll >= 0) {
                const auto& scroll = scene.viewports[static_cast<size_t>(fog.scroll)];
                const auto& v = scene.viewports[static_cast<size_t>(fog.light)];
                FogUniform u{};
                for (size_t i = 0; i < 3; ++i) {
                    u.base[i] = scroll.fog_colour_set[i] * v.fog_ambient;
                    u.spot[i] = v.spot_fog_colour[i] * scroll.fog_colour_set[i] * fog.attenuation;
                }
                u.base[3] = scroll.scroll_fog;
                u.spot[3] = fog.spot ? 1.0f : 0.0f;
                u.ellipse[0] = v.spot_x;
                u.ellipse[1] = v.spot_y;
                u.ellipse[2] = v.spot_rx;
                u.ellipse[3] = v.spot_ry;
                u.scale = s;
                fill(scroll, u);
            }
            SDL_EndGPURenderPass(pass);
        }

        const FrameUniform frame{s, {}};
        SDL_PushGPUFragmentUniformData(cmd, 0, &frame, sizeof frame);
        SDL_GPUBuffer* storage[2] = {polygon_buffer_, viewport_buffer_};
        const SDL_GPUBufferBinding vertices{vertex_buffer_, 0};
        const SDL_GPUTextureSamplerBinding texels{texture_ram_, nearest_};
        auto draw = [&](SDL_GPURenderPass* pass, SDL_GPUGraphicsPipeline* const pipelines[2], const std::vector<Draw>& draws) {
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            SDL_BindGPUVertexStorageBuffers(pass, 0, storage, 2);
            SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
            SDL_BindGPUFragmentSamplers(pass, 0, &texels, 1);
            int bound = -1;
            for (const Draw& dr : draws) {
                if (bound != (dr.decal ? 1 : 0)) {
                    bound = dr.decal ? 1 : 0;
                    SDL_BindGPUGraphicsPipeline(pass, pipelines[bound]);
                    SDL_SetGPUStencilReference(pass, 0);
                }
                const auto& v = scene.viewports[dr.viewport];
                const SDL_GPUViewport vp{v.x * s, v.y * s, v.width * s, v.height * s, 0, 1};
                SDL_SetGPUViewport(pass, &vp);
                const int x0 = std::clamp(static_cast<int>(v.x * s), 0, width()),
                          y0 = std::clamp(static_cast<int>(v.y * s), 0, height());
                const int x1 = std::clamp(static_cast<int>((v.x + v.width) * s), 0, width()),
                          y1 = std::clamp(static_cast<int>((v.y + v.height) * s), 0, height());
                if (x0 >= x1 || y0 >= y1) continue;
                const SDL_Rect r{x0, y0, x1 - x0, y1 - y0};
                SDL_SetGPUScissor(pass, &r);
                SDL_DrawGPUPrimitives(pass, dr.count, 1, dr.first, 0);
            }
        };

        for (const Group& g : groups_) {
            // The opaque pass, from a fresh depth buffer; it clears the
            // layers where it draws, and keeps its depth for them.
            {
                SDL_GPUColorTargetInfo t[4] = {colour_target(colour_, false), colour_target(layers_[0], false),
                                               colour_target(layers_[1], false), colour_target(depth_copy_, true)};
                SDL_GPUDepthStencilTargetInfo ds{};
                ds.texture = depth_;
                ds.clear_depth = 0;
                ds.load_op = SDL_GPU_LOADOP_CLEAR;
                ds.store_op = SDL_GPU_STOREOP_DONT_CARE;
                ds.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
                ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                ds.clear_stencil = 0;
                SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, t, 4, &ds);
                draw(pass, opaque_, g.passes[0]);
                SDL_EndGPURenderPass(pass);
            }
            // Each translucency layer, against the opaque depth.
            for (int layer = 0; layer < 2; ++layer) {
                if (g.passes[1 + layer].empty()) continue;
                SDL_GPUColorTargetInfo t = colour_target(layers_[layer], false);
                SDL_GPUDepthStencilTargetInfo ds{};
                ds.texture = trans_depth_;
                ds.clear_depth = 0;
                ds.load_op = SDL_GPU_LOADOP_CLEAR;
                ds.store_op = SDL_GPU_STOREOP_DONT_CARE;
                ds.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
                ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, &ds);
                SDL_BindGPUGraphicsPipeline(pass, copy_);
                set_full_view(pass);
                const SDL_GPUTextureSamplerBinding depth{depth_copy_, nearest_};
                SDL_BindGPUFragmentSamplers(pass, 0, &depth, 1);
                SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                draw(pass, trans_, g.passes[1 + layer]);
                SDL_EndGPURenderPass(pass);
            }
        }

        // The layers over the opaque picture.
        SDL_GPUColorTargetInfo t = colour_target(output_, true);
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, composite_);
        set_full_view(pass);
        const SDL_GPUTextureSamplerBinding inputs[3] = {{colour_, nearest_}, {layers_[0], nearest_}, {layers_[1], nearest_}};
        SDL_BindGPUFragmentSamplers(pass, 0, inputs, 3);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }

    if (!pixels) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion from{};
    from.texture = output_;
    from.w = static_cast<Uint32>(width());
    from.h = static_cast<Uint32>(height());
    from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = download_;
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return;
    SDL_WaitForGPUFences(d, true, &fence, 1);
    SDL_ReleaseGPUFence(d, fence);
    pixels->resize(static_cast<size_t>(width()) * static_cast<size_t>(height()));
    if (const void* p = SDL_MapGPUTransferBuffer(d, download_, false)) {
        std::memcpy(pixels->data(), p, pixels->size() * 4);
        SDL_UnmapGPUTransferBuffer(d, download_);
    }
}

} // namespace gpu
