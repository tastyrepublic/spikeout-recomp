# RmlUi's SDL_GPU renderer

`RmlUi_Renderer_SDL_GPU.cpp`, `RmlUi_Renderer_SDL_GPU.h` and
`ShadersCompiledSPV.h` (its shaders, compiled for Vulkan, Metal and Direct3D 12)
from RmlUi 6.3's `Backends/` folder (https://github.com/mikke89/RmlUi, MIT,
`LICENSE.txt`). Changed here: images load as BMP through SDL itself instead of
through SDL_image (the menu loads none), and the shaders' include path.
