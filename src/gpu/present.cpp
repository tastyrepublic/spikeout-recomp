#include "gpu/present.hpp"

#include "gpu/shaders.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace gpu {

// ---- Device ----

bool Device::open(SDL_Window* window) {
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!device_) {
        error_ = SDL_GetError();
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
        error_ = SDL_GetError();
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return false;
    }
    window_ = window;
    return true;
}

Device::~Device() {
    if (!device_) return;
    if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
}

const char* Device::driver() const { return device_ ? SDL_GetGPUDeviceDriver(device_) : "none"; }

SDL_GPUShader* Device::shader(const ShaderCode& code, Stage stage, const Resources& resources) {
    SDL_GPUShaderCreateInfo info{};
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.code = code.spirv;
        info.code_size = code.spirv_size;
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        info.code = reinterpret_cast<const Uint8*>(code.msl);
        info.code_size = std::strlen(code.msl);
        info.entrypoint = "main0"; // SPIRV-Cross's name for main
        info.format = SDL_GPU_SHADERFORMAT_MSL;
    } else {
        error_ = "no shader format this device takes";
        return nullptr;
    }
    info.stage = stage == Stage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = resources.samplers;
    info.num_uniform_buffers = resources.uniform_buffers;
    info.num_storage_buffers = resources.storage_buffers;
    SDL_GPUShader* s = SDL_CreateGPUShader(device_, &info);
    if (!s) error_ = SDL_GetError();
    return s;
}

// ---- Presenter ----

bool Presenter::open(Device& device) {
    device_ = &device;
    SDL_GPUDevice* d = device.get();
    SDL_GPUShader* vs = device.shader(shaders::present_vert, Device::Stage::Vertex, {});
    SDL_GPUShader* fs = device.shader(shaders::present_frag, Device::Stage::Fragment, {.samplers = 1});
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(d, vs);
        if (fs) SDL_ReleaseGPUShader(d, fs);
        return false;
    }
    SDL_GPUColorTargetDescription target{};
    target.format = SDL_GetGPUSwapchainTextureFormat(d, device.window());
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(d, &info);
    SDL_ReleaseGPUShader(d, vs);
    SDL_ReleaseGPUShader(d, fs);
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(d, &sampler);
    return pipeline_ && sampler_;
}

Presenter::~Presenter() {
    if (!device_ || !device_->get()) return;
    SDL_GPUDevice* d = device_->get();
    SDL_WaitForGPUIdle(d);
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(d, pipeline_);
    if (sampler_) SDL_ReleaseGPUSampler(d, sampler_);
    if (texture_) SDL_ReleaseGPUTexture(d, texture_);
    if (upload_) SDL_ReleaseGPUTransferBuffer(d, upload_);
}

void Presenter::resize(int width, int height) {
    SDL_GPUDevice* d = device_->get();
    if (texture_) SDL_ReleaseGPUTexture(d, texture_);
    if (upload_) SDL_ReleaseGPUTransferBuffer(d, upload_);
    SDL_GPUTextureCreateInfo tex{};
    tex.type = SDL_GPU_TEXTURETYPE_2D;
    tex.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // 0xAARRGGBB in a little-endian word
    tex.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex.width = static_cast<Uint32>(width);
    tex.height = static_cast<Uint32>(height);
    tex.layer_count_or_depth = 1;
    tex.num_levels = 1;
    texture_ = SDL_CreateGPUTexture(d, &tex);
    SDL_GPUTransferBufferCreateInfo buf{};
    buf.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    buf.size = static_cast<Uint32>(width * height * 4);
    upload_ = SDL_CreateGPUTransferBuffer(d, &buf);
    width_ = width;
    height_ = height;
}

void Presenter::present(std::span<const uint32_t> pixels, int width, int height) {
    if (!pipeline_) return;
    if (width != width_ || height != height_) resize(width, height);
    if (!texture_ || !upload_) return;
    SDL_GPUDevice* d = device_->get();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(d);
    if (!cmd) return;

    if (void* p = SDL_MapGPUTransferBuffer(d, upload_, true)) {
        std::memcpy(p, pixels.data(), std::min(pixels.size_bytes(), static_cast<std::size_t>(width * height) * 4));
        SDL_UnmapGPUTransferBuffer(d, upload_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = upload_;
        SDL_GPUTextureRegion to{};
        to.texture = texture_;
        to.w = static_cast<Uint32>(width);
        to.h = static_cast<Uint32>(height);
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, true);
        SDL_EndGPUCopyPass(copy);
    }

    SDL_GPUTexture* swapchain = nullptr;
    Uint32 sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, device_->window(), &swapchain, &sw, &sh) || !swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd); // minimized: nothing to draw to
        return;
    }
    SDL_GPUColorTargetInfo colour{};
    colour.texture = swapchain;
    colour.clear_color = {0, 0, 0, 1};
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &colour, 1, nullptr);
    // Letterboxed: the largest rectangle of the frame's aspect.
    const float scale = std::min(static_cast<float>(sw) / static_cast<float>(width), static_cast<float>(sh) / static_cast<float>(height));
    SDL_GPUViewport vp{};
    vp.w = static_cast<float>(width) * scale;
    vp.h = static_cast<float>(height) * scale;
    vp.x = (static_cast<float>(sw) - vp.w) / 2;
    vp.y = (static_cast<float>(sh) - vp.h) / 2;
    vp.max_depth = 1;
    SDL_SetGPUViewport(pass, &vp);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUTextureSamplerBinding binding{texture_, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
}

} // namespace gpu
