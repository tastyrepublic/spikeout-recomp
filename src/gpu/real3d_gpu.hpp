// Draws the Real3D's scene (m3::Real3dScene) on the GPU, as the software
// renderer (m3::Real3dRenderer) draws it: the same passes and order
// (background fog; per priority, and again for its high-priority polygons,
// the opaque pass with decals, then each translucency layer against a copy
// of the opaque depth; the layers mixed over the opaque picture), and
// shaders that sample, light and fog as it does (src/gpu/shaders/scene.*).
// Texture RAM is uploaded whole when the game has uploaded textures.
//
// The picture is `scale` times the Real3D's 496x384, 0xAARRGGBB with alpha
// 0 where nothing was drawn, as the software renderer's.
//
// With set_quads, quads that aren't flat and linearly mapped are drawn whole
// (the software renderer's --quads). For the window the picture is also kept
// premultiplied with mipmaps (display_image), so it can be shown smaller than
// drawn with every drawn pixel counted: --render-scale above the window's
// size is supersampling antialiasing.
//
// Not yet: the probe,
// and a translucent decal seeing decals drawn in the opaque pass (the stencil
// starts afresh in each translucency layer).
#pragma once

#include "runtime/real3d.hpp"
#include "runtime/real3d_scene.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct SDL_GPUBuffer;
struct SDL_GPUCopyPass;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUSampler;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;

namespace gpu {

class Device;

class Real3dGpu {
public:
    static constexpr int kWidth = 496, kHeight = 384;

    // `extend`: widescreen, the picture that many screen pixels wider each
    // side (m3::Real3dSceneBuilder::set_extend), the 496 x 384 screen in
    // its middle.
    bool open(Device& device, int scale = 1, int extend = 0);
    ~Real3dGpu();
    const std::string& error() const { return error_; }

    // Draws the scene, with texture RAM from `mem`. With `pixels`, waits
    // for the GPU and reads the picture back into it.
    void render(const m3::Real3d& mem, const m3::Real3dScene& scene, std::vector<uint32_t>* pixels = nullptr) {
        render(scene, mem.texture_ram(), mem.textures_uploaded(), pixels);
    }
    // The same from a copy of texture RAM (2048 x 2048), uploaded when
    // `texture_version` differs from the last frame's.
    void render(const m3::Real3dScene& scene, std::span<const uint16_t> texture_ram, uint64_t texture_version,
                std::vector<uint32_t>* pixels = nullptr);

    // Quads drawn whole (default: as two triangles).
    void set_quads(bool on) { quads_on_ = on; }

    // The picture as read back (straight alpha), and for showing:
    // premultiplied alpha, with mipmaps.
    SDL_GPUTexture* image() const { return output_; }
    SDL_GPUTexture* display_image() const { return display_; }
    int width() const { return (kWidth + 2 * extend_) * scale_; }
    int extend() const { return extend_; }
    int height() const { return kHeight * scale_; }

private:
    // What a draw is: triangles, quads drawn whole, or quads drawn whole
    // with a depth bound (all corners beyond the near plane).
    enum Kind : uint8_t { kTriangles, kQuads, kBoundedQuads, kKinds };
    struct Draw {
        uint32_t first, count; // vertices
        uint16_t viewport;
        bool decal;
        Kind kind;
    };
    // One priority (or its high-priority half): the opaque pass and the two
    // translucency layers.
    struct Group {
        std::vector<Draw> passes[3];
    };

    void build(const m3::Real3dScene& scene);
    bool upload(SDL_GPUBuffer*& buffer, SDL_GPUTransferBuffer*& transfer, uint32_t& capacity, const void* data,
                uint32_t size, uint32_t usage, SDL_GPUCopyPass* copy);

    Device* device_ = nullptr;
    int scale_ = 1;
    int extend_ = 0;
    std::string error_;

    // opaque_ and trans_ by [kind * 2 + decal].
    SDL_GPUGraphicsPipeline *fog_ = nullptr, *opaque_[kKinds * 2] = {}, *trans_[kKinds * 2] = {}, *composite_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUTexture *colour_ = nullptr, *layers_[2] = {}, *depth_copy_ = nullptr, *depth_ = nullptr, *trans_depth_ = nullptr,
                   *output_ = nullptr, *display_ = nullptr, *texture_ram_ = nullptr;
    SDL_GPUBuffer *vertex_buffer_ = nullptr, *polygon_buffer_ = nullptr, *viewport_buffer_ = nullptr, *quad_buffer_ = nullptr;
    SDL_GPUTransferBuffer *vertex_upload_ = nullptr, *polygon_upload_ = nullptr, *viewport_upload_ = nullptr,
                          *quad_upload_ = nullptr, *texture_upload_ = nullptr, *download_ = nullptr;
    uint32_t vertex_capacity_ = 0, polygon_capacity_ = 0, viewport_capacity_ = 0, quad_capacity_ = 0;
    uint32_t display_levels_ = 1;
    bool quads_on_ = false;
    // Which texture RAM the GPU has (render's texture_version).
    uint64_t texture_version_ = UINT64_MAX;

    std::vector<uint8_t> vertices_, polygons_, viewports_, quads_; // this frame's, as uploaded
    std::vector<Group> groups_;
};

} // namespace gpu
