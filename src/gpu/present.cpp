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

bool Device::open_headless() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { // SDL's GPU wants it, window or not
        error_ = SDL_GetError();
        return false;
    }
    video_ = true;
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!device_) error_ = SDL_GetError();
    return device_ != nullptr;
}

Device::~Device() {
    if (device_) {
        if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
        SDL_DestroyGPUDevice(device_);
    }
    if (video_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
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

namespace {

SDL_GPUGraphicsPipeline* fullscreen_pipeline(Device& device, const ShaderCode& fragment, uint32_t samplers) {
    SDL_GPUDevice* d = device.get();
    SDL_GPUShader* vs = device.shader(shaders::present_vert, Device::Stage::Vertex, {});
    SDL_GPUShader* fs = device.shader(fragment, Device::Stage::Fragment, {.samplers = samplers});
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if (vs && fs) {
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GetGPUSwapchainTextureFormat(d, device.window());
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vs;
        info.fragment_shader = fs;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(d, &info);
    }
    if (vs) SDL_ReleaseGPUShader(d, vs);
    if (fs) SDL_ReleaseGPUShader(d, fs);
    return pipeline;
}

SDL_GPUTexture* frame_texture(SDL_GPUDevice* d, int width, int height) {
    SDL_GPUTextureCreateInfo tex{};
    tex.type = SDL_GPU_TEXTURETYPE_2D;
    tex.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // 0xAARRGGBB in a little-endian word
    tex.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex.width = static_cast<Uint32>(width);
    tex.height = static_cast<Uint32>(height);
    tex.layer_count_or_depth = 1;
    tex.num_levels = 1;
    return SDL_CreateGPUTexture(d, &tex);
}

} // namespace

bool Presenter::open(Device& device) {
    device_ = &device;
    pipeline_ = fullscreen_pipeline(device, shaders::present_frag, 1);
    screen_pipeline_ = fullscreen_pipeline(device, shaders::screen_frag, 3);
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device.get(), &sampler);
    // The 3D: filtered, and averaged down through its mipmaps when the
    // window is smaller than it was drawn.
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    smooth_ = SDL_CreateGPUSampler(device.get(), &sampler);
    return pipeline_ && screen_pipeline_ && sampler_ && smooth_;
}

Presenter::~Presenter() {
    if (!device_ || !device_->get()) return;
    SDL_GPUDevice* d = device_->get();
    SDL_WaitForGPUIdle(d);
    for (SDL_GPUGraphicsPipeline* p : {pipeline_, screen_pipeline_})
        if (p) SDL_ReleaseGPUGraphicsPipeline(d, p);
    for (SDL_GPUSampler* s : {sampler_, smooth_})
        if (s) SDL_ReleaseGPUSampler(d, s);
    for (SDL_GPUTexture* t : textures_)
        if (t) SDL_ReleaseGPUTexture(d, t);
    if (upload_) SDL_ReleaseGPUTransferBuffer(d, upload_);
}

// Room for two frames of this size: textures_ 0 and 1, and their upload.
void Presenter::resize(int width, int height) {
    SDL_GPUDevice* d = device_->get();
    for (SDL_GPUTexture*& t : textures_) {
        if (t) SDL_ReleaseGPUTexture(d, t);
        t = frame_texture(d, width, height);
    }
    if (upload_) SDL_ReleaseGPUTransferBuffer(d, upload_);
    SDL_GPUTransferBufferCreateInfo buf{};
    buf.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    buf.size = static_cast<Uint32>(width * height * 4 * 2);
    upload_ = SDL_CreateGPUTransferBuffer(d, &buf);
    width_ = width;
    height_ = height;
}

bool Presenter::upload(SDL_GPUCommandBuffer* cmd, std::initializer_list<std::span<const uint32_t>> frames, int width, int height) {
    if (width != width_ || height != height_) resize(width, height);
    if (!textures_[0] || !textures_[1] || !upload_) return false;
    SDL_GPUDevice* d = device_->get();
    auto* p = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(d, upload_, true));
    if (!p) return false;
    const auto size = static_cast<std::size_t>(width * height) * 4;
    std::size_t i = 0;
    for (std::span<const uint32_t> f : frames) std::memcpy(p + size * i++, f.data(), std::min(f.size_bytes(), size));
    SDL_UnmapGPUTransferBuffer(d, upload_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (std::size_t k = 0; k < frames.size(); ++k) {
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = upload_;
        from.offset = static_cast<Uint32>(size * k);
        SDL_GPUTextureRegion to{};
        to.texture = textures_[k];
        to.w = static_cast<Uint32>(width);
        to.h = static_cast<Uint32>(height);
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, true);
    }
    SDL_EndGPUCopyPass(copy);
    return true;
}

void Presenter::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUGraphicsPipeline* pipeline,
                     std::initializer_list<SDL_GPUTexture*> inputs, int width, int height, uint32_t smooth_input) {
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
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_GPUTextureSamplerBinding bindings[3]{};
    Uint32 n = 0;
    for (SDL_GPUTexture* t : inputs) {
        bindings[n] = {t, n == smooth_input ? smooth_ : sampler_};
        ++n;
    }
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, n);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    if (overlay) overlay(cmd, swapchain, sw, sh);
    SDL_SubmitGPUCommandBuffer(cmd);
}

void Presenter::present(std::span<const uint32_t> pixels, int width, int height) {
    if (!pipeline_) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_->get());
    if (!cmd) return;
    if (!upload(cmd, {pixels}, width, height)) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    draw(cmd, pipeline_, {textures_[0]}, width, height);
}

void Presenter::present_layers(std::span<const uint32_t> bottom, std::span<const uint32_t> top, SDL_GPUTexture* three_d,
                               int width, int height) {
    if (!screen_pipeline_ || !three_d) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_->get());
    if (!cmd) return;
    if (!upload(cmd, {bottom, top}, width, height)) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    draw(cmd, screen_pipeline_, {textures_[0], three_d, textures_[1]}, width, height, 1);
}

} // namespace gpu
