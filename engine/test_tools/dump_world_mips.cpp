#include <source_port/files.h>
#include <source_port/graphics.h>
#include <source_port/world_texture_bank.h>
#include <sys/file/filesys.h>
#include <sys/file/pre.h>
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using namespace SourcePort;

std::shared_ptr<const ResourceBytes> ReadEntry(File::PreFile& pre, const std::string& name) {
    if (!pre.GetContainedFile(name.c_str())) return {};
    pre.Reset();
    auto bytes = std::make_shared<ResourceBytes>(pre.GetFileSize());
    const bool complete = pre.Read(bytes->data(), bytes->size()) == int(bytes->size());
    pre.Close(false);
    return complete ? bytes : nullptr;
}

bool Dump(const std::string& level, const fs::path& output, unsigned limit) {
    auto* archive = static_cast<uint8*>(File::LoadAllocNoPre(("pre/" + level + "scn.pre").c_str()));
    if (!archive) return false;
    File::PreFile pre(archive);
    const auto name = "levels/" + level + "/" + level;
    SceneAsset scene;
    std::vector<TextureAsset> textures;
    const auto dictionary = ReadEntry(pre, name + ".tex.xbx");
    if (!ReadScene(ReadEntry(pre, name + ".scn.xbx"), scene) || !ReadTextureDictionary(dictionary, textures))
        return false;
    const auto eligible = OpaqueWorldTextures(scene.materials, true);
    std::ofstream list(output / (level + ".txt"));
    list << dictionary->size() << ' ' << limit << ' '
         << WorldTextureChecksum(dictionary->data(), dictionary->size()) << '\n';
    ResourceBytes tiled, rgba;
    for (const auto& texture : textures) {
        if (!eligible.count(texture.checksum)) continue;
        unsigned width = 0, height = 0, count = 0;
        if (!BuildOpaqueWorldMipChain(texture, limit, width, height, count, tiled)) continue;
        // Alpha-test materials can use an ETC1 source when the complete
        // original texture is opaque. Check every level, including skipped ones.
        bool opaque = true;
        for (unsigned level = 0; opaque && level < texture.levels.size(); ++level) {
            if (!DecodeTextureLevel(texture, level, rgba)) return false;
            for (std::size_t a = 3; a < rgba.size(); a += 4) {
                if (rgba[a] != 255) { opaque = false; break; }
            }
        }
        if (!opaque) continue;
        list << texture.checksum << ' ' << width << ' ' << height << ' ' << count << '\n';
        unsigned first = 0;
        while (texture.levels[first].width != width || texture.levels[first].height != height) ++first;
        for (unsigned i = 0; i < count; ++i) {
            if (!DecodeTextureLevel(texture, first + i, rgba)) return false;
            const auto filename = level + "-" + std::to_string(texture.checksum) + "-" + std::to_string(i) + ".rgba";
            std::ofstream pixels(output / filename, std::ios::binary);
            pixels.write(reinterpret_cast<const char*>(rgba.data()), rgba.size());
            if (!pixels) return false;
        }
    }
    return bool(list);
}
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: source_dump_world_mips RETAIL_DATA OUTPUT TEXTURE_LIMIT LEVEL...\n");
        return 2;
    }
    const fs::path output(argv[2]);
    fs::create_directories(output);
    const auto limit = std::stoul(argv[3]);
    Mem::Manager::sSetUp();
    bool ok = false;
    {
        ok = SourcePort::ConfigureFileRoots(argv[1]);
        SourcePort::FileSystemSession files;
        for (int i = 4; ok && i < argc; ++i) {
            ok = Dump(argv[i], output, limit);
            if (!ok) std::fprintf(stderr, "Could not export original textures for %s\n", argv[i]);
        }
    }
    Mem::Manager::sCloseDown();
    return ok ? 0 : 1;
}
