#pragma once
#include <source_port/render_assets.h>
#include <source_port/graphics_os.h>

namespace SourcePort {
enum class TextureCacheFile : uint8 { None, Retail, Venice };
enum class TextureSource : uint32 { Dictionary, Image, Font };
constexpr uint32 TextureCacheVersion = 0x3143544e; // NTC1
constexpr unsigned TextureCacheLimit = 128;
constexpr unsigned MaxTextureSources = 8192;
constexpr unsigned MaxTextureGroupBytes = 8 * 1024 * 1024;

struct TextureCacheEntry {
    uint32 key = 0;
    unsigned width = 0, height = 0, levels = 0;
    unsigned sample_width = 0, sample_height = 0;
    SourceTextureFormat format = SOURCE_TEXTURE_RGBA8;
    std::size_t offset = 0, bytes = 0;
    uint32 distant_color = 0; // RGB from the smallest opaque ETC mip, high byte marks validity
};

std::size_t NativeTextureLevelBytes(SourceTextureFormat format, unsigned width, unsigned height);
bool ReadTextureCacheGroup(const ResourceBytes&, std::vector<TextureCacheEntry>&);
std::shared_ptr<const ResourceBytes> FindTextureCacheGroup(const ResourceBytes&, TextureSource);
std::shared_ptr<const ResourceBytes> FindTextureCacheGroup(uint32 checksum, uint32 size, TextureSource);
TextureCacheFile FindDiskTextureCacheGroup(uint32 checksum, uint32 size, TextureSource, std::vector<TextureCacheEntry>&);
bool ReadDiskTexturePayload(TextureCacheFile, const TextureCacheEntry&, ResourceBytes&);
void ResetTextureCacheIndex();
}
