#include "gpu/present.hpp"

#include "gpu/shaders.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace gpu {

// ---- Device ----

namespace {

// Metal's older Macs too (MTLGPUFamilyMac1: Haswell and Broadwell's Iris and
// HD, e.g. the 2014 Mac mini), which SDL leaves out unless asked: it allows
// them for programs that never write to sRGB textures, and nothing here does
// (the swapchain is SDR, every texture UNORM).
SDL_GPUDevice* create_device() {
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN, true);
    SDL_GPUDevice* device = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    return device;
}

} // namespace

bool Device::open(SDL_Window* window, bool fifo) {
    device_ = create_device();
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
    // Mailbox where there is one: a picture waits for the display's next
    // refresh (no tearing) but doesn't hold the window up. With plain vsync
    // (FIFO) Vulkan on Windows can hand out a picture only every second or
    // third refresh (seen: 33 a second on a 99 Hz display), showing the game's
    // 57.5 frames a second at half that. The window presents each game frame
    // once, as it finishes, either way.
    if (!fifo && SDL_WindowSupportsGPUPresentMode(device_, window, SDL_GPU_PRESENTMODE_MAILBOX))
        mailbox_ = SDL_SetGPUSwapchainParameters(device_, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                                 SDL_GPU_PRESENTMODE_MAILBOX);
    return true;
}

bool Device::open_headless() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { // SDL's GPU wants it, window or not
        error_ = SDL_GetError();
        return false;
    }
    video_ = true;
    device_ = create_device();
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
    SDL_GPUShader* fs = device.shader(fragment, Device::Stage::Fragment, {.samplers = samplers, .uniform_buffers = 1});
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if (vs && fs) {
        SDL_GPUColorTargetDescription target{};
        target.format = device.window() ? SDL_GetGPUSwapchainTextureFormat(d, device.window())
                                        : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // headless: Presenter::set_target's
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

// shaders/look.glsl's uniforms.
struct LookUniform {
    float matrix[3][4];
    float area[4];
    float filter[4];
    float wide[4]; // widescreen (screen.frag): the 3D's extension each side, the 2D's width; smooth motion: the lower layers' shift
};

// The CRT colours' monitors (Supermodel's Src/Graphics/SuperAA.cpp, from Dan
// Mons' colour_matrix_adaptations): gamma (0: sRGB's curve) and the matrix to
// sRGB, by rows.
struct CrtColours {
    float gamma, m[9];
};
constexpr CrtColours kCrtColours[] = {
    {0, {1, 0, 0, 0, 1, 0, 0, 0, 1}},
    {2.5f, // ARI (D93) -> sRGB: a Japanese arcade monitor
     {1.3272128334714093f, -0.3802108879412366f, -0.1003607696463202f, -0.0241848600268417f, 0.9544506228550125f,
      0.0807358585208939f, -0.0239585810555497f, -0.0409736005706461f, 1.4076563728858242f}},
    {2.25f, // Sony PVM-20M2U (D93) -> sRGB
     {0.9241392201737613f, -0.0690701363506526f, -0.0084279079392561f, 0.0231962420140721f, 0.9729221778325590f,
      0.0148832015024337f, -0.0054875731998806f, 0.0098220960740544f, 1.3383896683854550f}},
    {0, // BT.601 525 lines, D93 (Japan) -> sRGB
     {0.7822490684754086f, 0.0506168576073398f, 0.0137752498011040f, 0.0147968947158740f, 0.9741745313046067f,
      0.0220301953285839f, -0.0013501205469208f, -0.0044076726925543f, 1.3484819844991036f}},
    {0, // BT.601 525 lines (US) -> sRGB
     {0.9395420637732393f, 0.0501813568598678f, 0.0102765793668928f, 0.0177722231435608f, 0.9657928624969044f,
      0.0164349143595347f, -0.0016215999431855f, -0.0043697496597356f, 1.0059913496029214f}},
    {0, // BT.601 625 lines (Europe, Australia) -> sRGB
     {1.0440432087628346f, -0.0440432087628348f, 0.0000000000000001f, 0.0f, 1.0000000000000002f, 0.0f, 0.0f,
      0.0117933782840052f, 0.9882066217159947f}},
};

// The screen filter's glass at a window pixel (as look.glsl), its three colours.
void glass_at(int filter, int x, int y, float w, float h, float lines, float out[3]) {
    const bool crt = filter == 2;
    const float per_line = h / lines;
    const float depth = (crt ? 0.45f : 0.6f) * std::clamp((per_line - 1.5f) / 1.5f, 0.0f, 1.0f);
    const int period = std::max(1, static_cast<int>(std::lround(per_line)));
    const float f = period == 2 ? static_cast<float>(y % 2) * 0.5f
                                : (static_cast<float>(y % period) + 0.5f) / static_cast<float>(period);
    const float row = 1 - depth * (1 - std::sin(f * 3.14159265f));
    out[0] = out[1] = out[2] = row;
    if (!crt) return;
    for (int c = 0; c < 3; ++c)
        if (c != x % 3) out[c] *= 0.78f;
    const float dx = (static_cast<float>(x) + 0.5f) / w * 2 - 1, dy = (static_cast<float>(y) + 0.5f) / h * 2 - 1;
    const float d = std::min(1.0f, (dx * dx + dy * dy) / 2);
    for (int c = 0; c < 3; ++c) out[c] *= 1 - 0.22f * d * d;
}

} // namespace

void Presenter::set_look(int crt_colours, int filter) {
    crt_colours_ = std::clamp(crt_colours, 0, static_cast<int>(std::size(kCrtColours)) - 1);
    filter_ = std::clamp(filter, 0, 2);
}

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
                     std::initializer_list<SDL_GPUTexture*> inputs, int width, int height, uint32_t smooth_input,
                     int extend) {
    SDL_GPUTexture* swapchain = target_;
    Uint32 sw = target_width_, sh = target_height_;
    if (!target_ && (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, device_->window(), &swapchain, &sw, &sh) || !swapchain)) {
        SDL_SubmitGPUCommandBuffer(cmd); // minimized: nothing to draw to
        return;
    }
    pass_into(cmd, pipeline, inputs, width, height, smooth_input, extend, swapchain, sw, sh);
    if (overlay) overlay(cmd, swapchain, sw, sh);
    if (capture_width_ > 0) return capture(cmd, pipeline, inputs, width, height, smooth_input, extend); // (it submits)
    SDL_SubmitGPUCommandBuffer(cmd);
}

void Presenter::pass_into(SDL_GPUCommandBuffer* cmd, SDL_GPUGraphicsPipeline* pipeline, std::initializer_list<SDL_GPUTexture*> inputs,
                          int width, int height, uint32_t smooth_input, int extend, SDL_GPUTexture* swapchain, Uint32 sw, Uint32 sh) {
    SDL_GPUColorTargetInfo colour{};
    colour.texture = swapchain;
    colour.clear_color = {0, 0, 0, 1};
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &colour, 1, nullptr);
    // Letterboxed: the largest rectangle of the frame's aspect (widescreen:
    // the frame `extend` pixels wider each side).
    const int full = width + 2 * extend;
    const float scale = std::min(static_cast<float>(sw) / static_cast<float>(full), static_cast<float>(sh) / static_cast<float>(height));
    SDL_GPUViewport vp{};
    vp.w = static_cast<float>(full) * scale;
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
    // The looks. The filter's gain (its glass averaged over the picture, as
    // vf3-recomp's) keeps the picture about as bright: worked out again when
    // the picture's size changes.
    const int pw = static_cast<int>(vp.w), ph = static_cast<int>(vp.h);
    if (filter_ && (pw != gain_width_ || ph != gain_height_ || height != gain_lines_ || filter_ != gain_filter_)) {
        double sum = 0;
        int count = 0;
        for (int y = 0; y < ph; y += 3)
            for (int x = 0; x < pw; x += 7) {
                float g[3];
                glass_at(filter_, x, y, vp.w, vp.h, static_cast<float>(height), g);
                sum += g[0] + g[1] + g[2];
                count += 3;
            }
        gain_ = sum > 0 ? std::min(1.4f, static_cast<float>(count / sum)) : 1.0f;
        gain_width_ = pw;
        gain_height_ = ph;
        gain_lines_ = height;
        gain_filter_ = filter_;
    }
    LookUniform look{};
    const CrtColours& crt = kCrtColours[crt_colours_];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) look.matrix[r][c] = crt.m[r * 3 + c];
    look.matrix[0][3] = crt.gamma;
    look.area[0] = vp.x;
    look.area[1] = vp.y;
    look.area[2] = vp.w;
    look.area[3] = vp.h;
    look.filter[0] = static_cast<float>(filter_);
    look.filter[1] = static_cast<float>(height);
    look.filter[2] = filter_ ? gain_ : 1.0f;
    look.filter[3] = crt_colours_ ? 1.0f : 0.0f;
    look.wide[0] = static_cast<float>(extend);
    look.wide[1] = static_cast<float>(width);
    look.wide[2] = shift_[0];
    look.wide[3] = shift_[1];
    SDL_PushGPUFragmentUniformData(cmd, 0, &look, sizeof look);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

// The picture drawn again, the same way, into a texture of the size asked for (no menu over it), read back, kept for
// take_capture. Submits `cmd` and waits for it (a screenshot: once).
void Presenter::capture(SDL_GPUCommandBuffer* cmd, SDL_GPUGraphicsPipeline* pipeline, std::initializer_list<SDL_GPUTexture*> inputs,
                        int width, int height, uint32_t smooth_input, int extend) {
    SDL_GPUDevice* d = device_->get();
    const auto w = static_cast<Uint32>(capture_width_), h = static_cast<Uint32>(capture_height_);
    capture_width_ = capture_height_ = 0;
    const SDL_GPUTextureFormat format = device_->window() ? SDL_GetGPUSwapchainTextureFormat(d, device_->window())
                                                          : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // (the pipelines')
    const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    SDL_GPUTextureCreateInfo tex{};
    tex.type = SDL_GPU_TEXTURETYPE_2D;
    tex.format = format;
    tex.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tex.width = w, tex.height = h, tex.layer_count_or_depth = 1, tex.num_levels = 1;
    SDL_GPUTexture* target = (bgra || rgba) ? SDL_CreateGPUTexture(d, &tex) : nullptr;
    SDL_GPUTransferBufferCreateInfo tb{};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb.size = w * h * 4;
    SDL_GPUTransferBuffer* down = target ? SDL_CreateGPUTransferBuffer(d, &tb) : nullptr;
    if (!target || !down) {
        if (target) SDL_ReleaseGPUTexture(d, target);
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    // (the filter's gain is worked out for this size, then again for the window's next picture)
    pass_into(cmd, pipeline, inputs, width, height, smooth_input, extend, target, w, h);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion from{};
    from.texture = target, from.w = w, from.h = h, from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = down, to.pixels_per_row = w, to.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd)) {
        SDL_WaitForGPUFences(d, true, &fence, 1);
        SDL_ReleaseGPUFence(d, fence);
        if (const auto* p = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(d, down, false))) {
            captured_.assign(p, p + static_cast<std::size_t>(w) * h);
            SDL_UnmapGPUTransferBuffer(d, down);
            if (rgba) // (to 0xAARRGGBB, as B8G8R8A8's bytes read)
                for (uint32_t& c : captured_) c = (c & 0xff00ff00u) | ((c & 0xffu) << 16) | ((c >> 16) & 0xffu);
            for (uint32_t& c : captured_) c |= 0xff000000u;
            captured_width_ = static_cast<int>(w), captured_height_ = static_cast<int>(h);
        }
    }
    SDL_ReleaseGPUTransferBuffer(d, down);
    SDL_ReleaseGPUTexture(d, target);
}

bool Presenter::take_capture(std::vector<uint32_t>& pixels, int& width, int& height) {
    if (captured_.empty()) return false;
    pixels = std::move(captured_), width = captured_width_, height = captured_height_;
    captured_.clear();
    return true;
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
                               int width, int height, int extend, float shift_x, float shift_y) {
    if (!screen_pipeline_ || !three_d) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_->get());
    if (!cmd) return;
    if (!upload(cmd, {bottom, top}, width + 2 * std::max(extend, 0), height)) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    shift_[0] = shift_x;
    shift_[1] = shift_y;
    draw(cmd, screen_pipeline_, {textures_[0], three_d, textures_[1]}, width, height, 1, extend);
    shift_[0] = shift_[1] = 0;
}

} // namespace gpu
