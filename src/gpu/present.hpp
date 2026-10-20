// Shows finished frames in a window through SDL's GPU API (Vulkan, Metal):
// each frame is uploaded as a texture and drawn letterboxed to the window,
// scaled without filtering.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUSampler;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;
struct SDL_GPUCommandBuffer;
struct SDL_GPUShader;

namespace gpu {

struct ShaderCode;

// The device a window draws with, and loading the shaders for it.
class Device {
public:
    // false (and error() says why) when there is no GPU SDL can use with
    // shaders we have (Vulkan or Metal).
    bool open(SDL_Window* window);
    // Without a window: drawing only off screen (gamerun).
    bool open_headless();
    ~Device();

    SDL_GPUDevice* get() const { return device_; }
    SDL_Window* window() const { return window_; }
    const char* driver() const;
    const std::string& error() const { return error_; }

    enum class Stage { Vertex, Fragment };
    struct Resources {
        uint32_t samplers = 0, uniform_buffers = 0, storage_buffers = 0;
    };
    SDL_GPUShader* shader(const ShaderCode& code, Stage stage, const Resources& resources);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    bool video_ = false; // open_headless started SDL's video
    std::string error_;
};

class Presenter {
public:
    bool open(Device& device);
    ~Presenter();

    // Draws a frame of 0xAARRGGBB pixels (alpha ignored) to the window,
    // letterboxed to its aspect.
    void present(std::span<const uint32_t> pixels, int width, int height);
    // Draws the screen from the 2D layers below and above the 3D (width x
    // height, 0xAARRGGBB, 0 where nothing was drawn) and the 3D picture on
    // the GPU (any size; gpu::Real3dGpu::image), letterboxed.
    void present_layers(std::span<const uint32_t> bottom, std::span<const uint32_t> top, SDL_GPUTexture* three_d,
                        int width, int height);

    // Drawn over each picture, in its command buffer, onto the window's
    // texture (width x height pixels): the menu.
    std::function<void(SDL_GPUCommandBuffer*, SDL_GPUTexture*, uint32_t, uint32_t)> overlay;

private:
    void resize(int width, int height);
    bool upload(SDL_GPUCommandBuffer* cmd, std::initializer_list<std::span<const uint32_t>> frames, int width, int height);
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUGraphicsPipeline* pipeline, std::initializer_list<SDL_GPUTexture*> inputs,
              int width, int height, uint32_t smooth_input = UINT32_MAX); // that input filtered

    Device* device_ = nullptr;
    SDL_GPUGraphicsPipeline *pipeline_ = nullptr, *screen_pipeline_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr, *smooth_ = nullptr; // nearest; linear with mipmaps (the 3D)
    SDL_GPUTexture* textures_[2] = {}; // the frame, or the layers below and above the 3D
    SDL_GPUTransferBuffer* upload_ = nullptr;
    int width_ = 0, height_ = 0;
};

} // namespace gpu
