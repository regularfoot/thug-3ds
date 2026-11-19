#pragma once
#include <source_port/render_assets.h>
#include <set>

namespace SourcePort {
struct WorldTextureEntry {
    uint32 checksum = 0;
    unsigned width = 0, height = 0, levels = 0;
    std::size_t offset = 0, bytes = 0;
};

std::set<uint32> OpaqueWorldTextures(const std::vector<MaterialAsset>& materials, bool allow_alpha_test = false);
uint32 WorldTextureChecksum(const uint8* data, std::size_t size);
bool ReadWorldTextureBank(const ResourceBytes& bank, const ResourceBytes& dictionary, unsigned limit,
    std::vector<WorldTextureEntry>& entries);
std::shared_ptr<const ResourceBytes> LoadWorldTextureBank(const char* scene_name);
}
