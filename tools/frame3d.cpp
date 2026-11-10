// frame3d: draws a 3D frame dump (docs/frame-dump.md) with our renderers, as tools/supermodel's patched Supermodel
// draws the same dump with its own, to compare the two (tools/compare-supermodel.ps1).
//
//   frame3d ROMS DUMP [--renderer gpu|software] [--scale N] [--png OUT.png] [--models OUT.txt]
//
// ROMS: the ROM set (a folder or spikeofe.zip), for the models and textures in VROM. --png: the 3D alone, over black
// (default DUMP with .png). --models: each model drawn, in drawing order (its address, viewport, polygons, where its
// matrix puts it, its path's hash), for a list to compare with Supermodel's. Only the 3D: the 2D layers aren't in a dump.
#include "games/games.hpp"
#include "png_write.hpp"
#include "runtime/real3d.hpp"
#include "runtime/real3d_render.hpp"
#include "runtime/real3d_scene.hpp"
#include "runtime/rom.hpp"
#ifdef SPIKEOUT_GPU
#include "gpu/present.hpp"
#include "gpu/real3d_gpu.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool write_models(const std::string& path, const m3::Real3dScene& scene) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# model address, viewport, polygons, x y z (eye space), path hash\n");
    for (const auto& m : scene.models)
        std::fprintf(f, "%06x %u %u %.2f %.2f %.2f %08x\n", m.address, static_cast<unsigned>(m.viewport), m.polygon_count,
                     m.matrix.m[12], m.matrix.m[13], m.matrix.m[14], m.path);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: frame3d ROMS DUMP [--renderer gpu|software] [--scale N] [--png OUT.png] [--models OUT.txt]\n");
        return 2;
    }
    const std::string roms_path = argv[1], dump_path = argv[2];
    std::string renderer = "gpu", png, models;
    int scale = 1;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--renderer" && i + 1 < argc) renderer = argv[++i];
        else if (a == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
        else if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--models" && i + 1 < argc) models = argv[++i];
        else {
            std::fprintf(stderr, "frame3d: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (png.empty()) png = dump_path.substr(0, dump_path.find_last_of('.')) + ".png";

    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    m3::RomSet roms;
    try {
        roms = m3::load_rom_set(game, roms_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "frame3d: the ROM set: %s\n", e.what());
        return 1;
    }
    if (!roms.complete()) {
        std::fprintf(stderr, "frame3d: the ROM set is incomplete (romcheck says what is missing)\n");
        return 1;
    }
    m3::Real3d mem(roms.region(m3::RomRegionId::Vrom));
    std::string error;
    if (!mem.load_frame_dump(read_file(dump_path), game.security_key, error)) {
        std::fprintf(stderr, "frame3d: %s: %s\n", dump_path.c_str(), error.c_str());
        return 1;
    }

    m3::Real3dScene scene;
    std::vector<uint32_t> pixels;
    int width = m3::Real3dRenderer::kWidth, height = m3::Real3dRenderer::kHeight;
    if (renderer == "software") {
        m3::Real3dRenderer soft;
        soft.render(mem);
        scene = soft.scene();
        pixels.assign(soft.image().begin(), soft.image().end());
    } else if (renderer == "gpu") {
#ifdef SPIKEOUT_GPU
        gpu::Device device;
        if (!device.open_headless()) {
            std::fprintf(stderr, "frame3d: no GPU (%s)\n", device.error().c_str());
            return 1;
        }
        {
            gpu::Real3dGpu gpu3d;
            if (!gpu3d.open(device, scale)) {
                std::fprintf(stderr, "frame3d: the GPU renderer didn't open\n");
                return 1;
            }
            m3::Real3dSceneBuilder builder;
            builder.build(mem, scene);
            gpu3d.render(mem, scene, &pixels);
            width = gpu3d.width(), height = gpu3d.height();
        } // (the renderer gone before the device)
#else
        std::fprintf(stderr, "frame3d: built without the GPU renderer: --renderer software\n");
        return 1;
#endif
    } else {
        std::fprintf(stderr, "frame3d: --renderer gpu or software\n");
        return 2;
    }
    for (uint32_t& p : pixels) p |= 0xFF000000u;
    if (!tools::write_png(png, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
        std::fprintf(stderr, "frame3d: can't write %s\n", png.c_str());
        return 1;
    }
    std::printf("frame3d: %s drawn (%s, %d x %d): %zu models, %zu polygons, %zu viewports -> %s\n", dump_path.c_str(),
                renderer.c_str(), width, height, scene.models.size(), scene.polygons.size(), scene.viewports.size(), png.c_str());
    if (!models.empty()) {
        if (!write_models(models, scene)) {
            std::fprintf(stderr, "frame3d: can't write %s\n", models.c_str());
            return 1;
        }
        std::printf("frame3d: the models drawn -> %s\n", models.c_str());
    }
    return 0;
}
