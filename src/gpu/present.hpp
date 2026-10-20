// Shows finished frames in a window through SDL's GPU API (Vulkan, Metal):
// each frame is uploaded as a texture and drawn letterboxed to the window,
// scaled without filtering.
#pragma once

#include <cstdint>
#include <span>
#include <string>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUSampler;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;
struct SDL_GPUShader;

namespace gpu {

struct ShaderCode;

// The device a window draws with, and loading the shaders for it.
class Device {
public:
    // false (and error() says why) when there is no GPU SDL can use with
    // shaders we have (Vulkan or Metal).
    bool open(SDL_Window* window);
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
    std::string error_;
};

class Presenter {
public:
    bool open(Device& device);
    ~Presenter();

    // Draws a frame of 0xAARRGGBB pixels (alpha ignored) to the window,
    // letterboxed to its aspect.
    void present(std::span<const uint32_t> pixels, int width, int height);

private:
    void resize(int width, int height);

    Device* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    int width_ = 0, height_ = 0;
};

} // namespace gpu
