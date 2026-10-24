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
    // shaders we have (Vulkan or Metal). Presents in mailbox mode when the
    // window can (fifo: always plain vsync).
    bool open(SDL_Window* window, bool fifo = false);
    // Without a window: drawing only off screen (gamerun).
    bool open_headless();
    ~Device();

    SDL_GPUDevice* get() const { return device_; }
    SDL_Window* window() const { return window_; }
    const char* driver() const;
    bool mailbox() const { return mailbox_; } // else FIFO: presenting waits for the display
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
    bool mailbox_ = false;
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
    // the GPU (any size; gpu::Real3dGpu::image), letterboxed. `extend`:
    // widescreen, the 3D that many screen pixels wider each side than the
    // 2D (gpu::Real3dGpu::open's), and the layers as wide (width + 2 *
    // extend, their sides filled in by m3::WideLayer; shaders/screen.frag).
    void present_layers(std::span<const uint32_t> bottom, std::span<const uint32_t> top, SDL_GPUTexture* three_d,
                        int width, int height, int extend = 0);

    // The looks over the picture (shaders/look.glsl): CRT colours (0 off,
    // then kCrtColourNames' monitors, Supermodel's) and a screen filter
    // (0 off, 1 scanlines, 2 CRT).
    static constexpr const char* kCrtColourNames[] = {"Off", "Sega arcade (9300 K)", "Sony PVM (9300 K)",
                                                      "NTSC Japan (9300 K)", "NTSC US (6500 K)", "PAL (6500 K)"};
    static constexpr const char* kFilterNames[] = {"Off", "Scanlines", "CRT"};
    void set_look(int crt_colours, int filter);

    // Draws into this texture (width x height, B8G8R8A8, a colour target)
    // instead of the window: gamerun, timing the screen's pass. Null: the
    // window.
    void set_target(SDL_GPUTexture* texture, uint32_t width, uint32_t height) {
        target_ = texture;
        target_width_ = width;
        target_height_ = height;
    }

    // Drawn over each picture, in its command buffer, onto the window's
    // texture (width x height pixels): the menu.
    std::function<void(SDL_GPUCommandBuffer*, SDL_GPUTexture*, uint32_t, uint32_t)> overlay;

private:
    void resize(int width, int height);
    bool upload(SDL_GPUCommandBuffer* cmd, std::initializer_list<std::span<const uint32_t>> frames, int width, int height);
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUGraphicsPipeline* pipeline, std::initializer_list<SDL_GPUTexture*> inputs,
              int width, int height, uint32_t smooth_input = UINT32_MAX, // that input filtered
              int extend = 0);

    Device* device_ = nullptr;
    SDL_GPUGraphicsPipeline *pipeline_ = nullptr, *screen_pipeline_ = nullptr;
    SDL_GPUSampler *sampler_ = nullptr, *smooth_ = nullptr; // nearest; linear with mipmaps (the 3D)
    SDL_GPUTexture* textures_[2] = {}; // the frame, or the layers below and above the 3D
    SDL_GPUTransferBuffer* upload_ = nullptr;
    int width_ = 0, height_ = 0;
    SDL_GPUTexture* target_ = nullptr;
    uint32_t target_width_ = 0, target_height_ = 0;
    int crt_colours_ = 0, filter_ = 0;
    // The filter's gain, for the picture's size and lines it was worked out for.
    float gain_ = 1;
    int gain_height_ = 0, gain_width_ = 0, gain_lines_ = 0, gain_filter_ = 0;
};

} // namespace gpu
