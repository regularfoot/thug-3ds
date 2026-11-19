#include <source_port/texture_cache.h>
#include <source_port/world_texture_bank.h>
#include <source_port/files.h>
#include <source_port/graphics.h>
#include <sys/file/filesys.h>
#include <sys/file/pre.h>
#include "rg_etc1.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <pthread.h>
#include <tuple>

namespace {
namespace fs = std::filesystem;
using namespace SourcePort;

struct Pixels {
    unsigned width, height;
    ResourceBytes rgba;
};

unsigned PowerOfTwo(unsigned n) {
    unsigned size = 8;
    while (size < n) size *= 2;
    return size;
}

std::vector<Pixels> SourceLevels(const TextureAsset& asset, unsigned limit) {
    std::vector<Pixels> levels;
    unsigned first = 0;
    if (!asset.image && !asset.font_palette) {
        while (first < asset.levels.size() &&
            std::max(asset.levels[first].width, asset.levels[first].height) > limit) ++first;
    }
    const bool authored = first < asset.levels.size() &&
        PowerOfTwo(asset.levels[first].width) == asset.levels[first].width &&
        PowerOfTwo(asset.levels[first].height) == asset.levels[first].height;
    if (authored && !asset.image && !asset.font_palette) {
        for (unsigned i = first; i < asset.levels.size(); ++i) {
            const auto& src = asset.levels[i];
            if (src.width < 8 || src.height < 8) break;
            Pixels pixels{src.width, src.height, {}};
            if (!DecodeTextureLevel(asset, i, pixels.rgba)) return {};
            levels.push_back(std::move(pixels));
        }
        return levels;
    }

    Pixels pixels{asset.width, asset.height, {}};
    if (!DecodeTextureLevel(asset, 0, pixels.rgba)) return {};
    if (!asset.image && !asset.font_palette && std::max(asset.width, asset.height) > limit) {
        ResourceBytes scaled;
        if (!ReduceTextureRGBA(pixels.rgba, asset.width, asset.height, limit,
                scaled, pixels.width, pixels.height)) return {};
        pixels.rgba.swap(scaled);
    }
    if (pixels.width > 1024 || pixels.height > 1024) return {};
    levels.push_back(std::move(pixels));
    return levels;
}

struct EtcJob {
    const Pixels& pixels;
    uint8* output;
    unsigned width, height, first, step;
    bool alpha;
};

void* EncodeTiles(void* context) {
    const auto& job = *static_cast<EtcJob*>(context);
    const unsigned bytes_per_block = job.alpha ? 16 : 8;
    const unsigned tiles_x = job.width / 8;
    const unsigned tiles_y = job.height / 8;
    rg_etc1::etc1_pack_params params;
    params.clear();
    params.m_quality = rg_etc1::cMediumQuality;
    for (unsigned tile_y = job.first; tile_y < tiles_y; tile_y += job.step) {
        for (unsigned tile_x = 0; tile_x < tiles_x; ++tile_x) {
            auto* dest = job.output + (tile_y * tiles_x + tile_x) * 4 * bytes_per_block;
            for (unsigned block = 0; block < 4; ++block, dest += bytes_per_block) {
                unsigned texels[16];
                uint8 alpha[8]{};
                for (unsigned y = 0; y < 4; ++y) {
                    const unsigned v = std::min(tile_y * 8 + (block / 2) * 4 + y, job.pixels.height - 1);
                    for (unsigned x = 0; x < 4; ++x) {
                        const unsigned u = std::min(tile_x * 8 + (block % 2) * 4 + x, job.pixels.width - 1);
                        const auto* pixel = job.pixels.rgba.data() + (v * job.pixels.width + u) * 4;
                        texels[y * 4 + x] = pixel[0] | (unsigned(pixel[1]) << 8) |
                            (unsigned(pixel[2]) << 16) | 0xff000000u;
                        alpha[x * 2 + y / 2] |= ((unsigned(pixel[3]) * 15 + 127) / 255) << ((y % 2) * 4);
                    }
                }
                uint8 color[8];
                rg_etc1::pack_etc1_block(color, texels, params);
                if (job.alpha) std::copy_n(alpha, 8, dest);
                std::reverse_copy(color, color + 8, dest + (job.alpha ? 8 : 0));
            }
        }
    }
    return nullptr;
}

ResourceBytes EncodeEtc(const Pixels& pixels, unsigned width, unsigned height, bool alpha) {
    ResourceBytes output(std::size_t(width) * height / (alpha ? 1 : 2));
    if (width * height < 16384) {
        EtcJob job{pixels, output.data(), width, height, 0, 1, alpha};
        EncodeTiles(&job);
        return output;
    }
    EtcJob jobs[] = {{pixels, output.data(), width, height, 0, 4, alpha},
                     {pixels, output.data(), width, height, 1, 4, alpha},
                     {pixels, output.data(), width, height, 2, 4, alpha},
                     {pixels, output.data(), width, height, 3, 4, alpha}};
    pthread_t threads[3]{};
    bool started[3]{};
    for (unsigned i = 0; i < 3; ++i)
        started[i] = pthread_create(&threads[i], nullptr, EncodeTiles, &jobs[i + 1]) == 0;
    EncodeTiles(&jobs[0]);
    for (unsigned i = 0; i < 3; ++i) {
        if (started[i]) pthread_join(threads[i], nullptr);
        else EncodeTiles(&jobs[i + 1]);
    }
    return output;
}

void Put(ResourceBytes& out, uint32 value, unsigned bytes = 4) {
    for (unsigned shift = 0; shift < 8 * bytes; shift += 8) out.push_back(uint8(value >> shift));
}

bool BakeTexture(const TextureAsset& asset, ResourceBytes& group, std::array<std::size_t, 7>& counts,
                 bool small_opaque) {
    auto levels = SourceLevels(asset, TextureCacheLimit);
    if (levels.empty()) return false;
    bool opaque = true;
    bool grayscale = true;
    for (const auto& level : levels) {
        for (std::size_t i = 0; i < level.rgba.size(); i += 4) {
            opaque &= level.rgba[i+3] == 255;
            grayscale &= level.rgba[i] == level.rgba[i+1] && level.rgba[i] == level.rgba[i+2];
        }
    }
    if (small_opaque && opaque && std::max(levels.front().width, levels.front().height) > 64) {
        levels = SourceLevels(asset, 64);
        if (levels.empty()) return false;
    }
    const auto& base = levels.front();
    const unsigned width = PowerOfTwo(base.width);
    const unsigned height = PowerOfTwo(base.height);
    ResourceBytes chain;
    SourceTextureFormat format = opaque ? SOURCE_TEXTURE_ETC1 : SOURCE_TEXTURE_ETC1A4;
    unsigned count = levels.size();
    // Preserve full alpha precision for grayscale gradients and the original
    // packed pixels for bitmap fonts and their colored glyphs.
    if (asset.font_palette || (grayscale && !opaque)) {
        if (!TileCompactTexture(base.rgba, base.width, base.height, width, height, format, chain)) return false;
        count = 1;
    } else {
        for (const auto& level : levels) {
            auto encoded = EncodeEtc(level, PowerOfTwo(level.width), PowerOfTwo(level.height), !opaque);
            chain.insert(chain.end(), encoded.begin(), encoded.end());
        }
    }

    Put(group, asset.checksum);
    Put(group, width, 2);
    Put(group, height, 2);
    Put(group, base.width, 2);
    Put(group, base.height, 2);
    Put(group, count, 2);
    Put(group, format, 2);
    Put(group, chain.size());
    group.insert(group.end(), chain.begin(), chain.end());
    ++counts[format];
    return true;
}

using AssetKey = std::tuple<uint32, uint32, uint32>;
struct IndexValue { uint32 offset, bytes, crc; };

struct Baker {
    bool venice = false;
    std::ofstream file;
    std::map<AssetKey, IndexValue> entries;
    std::array<std::size_t, 7> formats{};
    std::size_t sources = 0, duplicate = 0, unsupported = 0, skipped = 0;
    std::size_t payload = 0;

    bool Add(const std::shared_ptr<const ResourceBytes>& source, TextureSource kind, const std::string& name) {
        if (!source || source->empty()) return false;
        ++sources;
        if (sources % 250 == 0)
            std::printf("Processed %zu source files, %zu unique cached so far\n", sources, entries.size());
        const auto key = std::make_tuple(WorldTextureChecksum(source->data(), source->size()),
            uint32(source->size()), uint32(kind));
        if (entries.count(key)) { ++duplicate; return true; }

        std::vector<TextureAsset> textures;
        if (kind == TextureSource::Dictionary) {
            if (!ReadTextureDictionary(source, textures)) {
                ++unsupported;
                std::printf("Unrecognized dictionary %s\n", name.c_str());
                return true;
            }
        } else if (kind == TextureSource::Font) {
            FontAsset font;
            if (!ReadFont(source, font)) {
                ++unsupported;
                std::printf("Unrecognized font %s\n", name.c_str());
                return true;
            }
            textures.push_back(std::move(font.texture));
        } else {
            TextureAsset texture;
            if (!ReadTextureImage(source, texture)) {
                ++unsupported;
                std::printf("Unrecognized image %s\n", name.c_str());
                return true;
            }
            textures.push_back(std::move(texture));
        }

        std::sort(textures.begin(), textures.end(), [](const auto& a, const auto& b) {
            return a.checksum < b.checksum;
        });
        ResourceBytes group(4);
        unsigned baked = 0;
        auto local_counts = formats;
        auto path = name;
        std::replace(path.begin(), path.end(), '\\', '/');
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
        const bool small_opaque = venice && (path == "levels/vn/vn.tex.xbx" || path == "levels/vn/vn.tex");
        if (small_opaque) std::printf("Baking Old 3DS Venice materials from %s\n", name.c_str());
        for (const auto& texture : textures) {
            if (BakeTexture(texture, group, local_counts, small_opaque)) ++baked;
            else { ++skipped; std::printf("Unconverted texture %s %08x\n", name.c_str(), texture.checksum); }
        }
        if (!baked) return true;
        for (unsigned i = 0; i < 4; ++i) group[i] = baked >> (8 * i);
        if (group.size() > MaxTextureGroupBytes || entries.size() >= MaxTextureSources) return false;
        const auto offset = file.tellp();
        if (offset < 0 || uint64(offset) + group.size() > INT_MAX) return false;
        entries.emplace(key, IndexValue{uint32(offset), uint32(group.size()),
            WorldTextureChecksum(group.data(), group.size())});
        file.write(reinterpret_cast<const char*>(group.data()), group.size());
        formats = local_counts;
        payload += group.size();
        return bool(file);
    }

    bool Finish() {
        ResourceBytes index;
        for (const auto& [key, record] : entries) {
            Put(index, std::get<0>(key));
            Put(index, std::get<1>(key));
            Put(index, std::get<2>(key));
            Put(index, record.offset);
            Put(index, record.bytes);
            Put(index, record.crc);
        }
        ResourceBytes header;
        Put(header, TextureCacheVersion);
        Put(header, TextureCacheLimit);
        Put(header, entries.size());
        Put(header, WorldTextureChecksum(index.data(), index.size()));
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(header.data()), header.size());
        file.write(reinterpret_cast<const char*>(index.data()), index.size());
        file.flush();
        std::printf("Sources=%zu unique=%zu duplicates=%zu unrecognized=%zu unconverted=%zu payload=%zu\n",
            sources, entries.size(), duplicate, unsupported, skipped, payload);
        for (unsigned i = 0; i < formats.size(); ++i)
            if (formats[i]) std::printf("Texture format %u: %zu\n", i, formats[i]);
        return bool(file) && !entries.empty();
    }
};

bool EndsWith(const std::string& path, std::string_view suffix) {
    return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool GetKind(std::string name, TextureSource& kind) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    if (EndsWith(name, ".xbx")) name.resize(name.size() - 4);
    if (EndsWith(name, ".tex") || EndsWith(name, ".txx")) kind = TextureSource::Dictionary;
    else if (EndsWith(name, ".img")) kind = TextureSource::Image;
    else if (EndsWith(name, ".fnt")) kind = TextureSource::Font;
    else return false;
    return true;
}

bool BakeArchive(Baker& baker, const std::string& name) {
    auto* raw = static_cast<uint8*>(File::LoadAllocNoPre(name.c_str()));
    if (!raw) return false;
    uint32 size;
    std::memcpy(&size, raw, sizeof(size));
    File::PreFile pre(raw);
    ResourceReader reader(raw, size, nullptr);
    reader.Skip(8);
    const auto count = reader.U32();
    for (unsigned i = 0; i < count; ++i) {
        const auto expanded = reader.U32();
        const auto packed = reader.U32();
        const auto name_size = reader.U16();
        reader.Skip(6);
        const auto* path = reinterpret_cast<const char*>(reader.Bytes(name_size));
        const auto entry_size = packed ? packed : expanded;
        if (!path || !reader.Skip((entry_size + 3u) & ~3u)) return false;
        TextureSource kind;
        if (!GetKind(path, kind)) continue;
        if (!pre.GetContainedFile(path)) return false;
        pre.Reset();
        auto bytes = std::make_shared<ResourceBytes>(pre.GetFileSize());
        const auto actual = pre.Read(bytes->data(), bytes->size());
        pre.Close(false);
        if (actual != int(bytes->size()) || !baker.Add(bytes, kind, path)) return false;
    }
    return reader.Ok() && !reader.Remaining();
}
}

int main(int argc, char** argv) {
    const bool venice = argc == 4 && std::string_view(argv[3]) == "--venice-low-memory";
    if (argc != 3 && !venice) {
        std::fprintf(stderr, "usage: source_bake_texture_cache RETAIL_DATA OUTPUT.3tc [--venice-low-memory]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    rg_etc1::pack_etc1_block_init();
    Mem::Manager::sSetUp();
    bool success = false;
    {
        Dbg_Assert(SourcePort::ConfigureFileRoots(argv[1]));
        SourcePort::FileSystemSession session;
        Baker baker;
        baker.venice = venice;
        baker.file.open(argv[2], std::ios::binary | std::ios::trunc);
        baker.file.seekp(16 + MaxTextureSources * 24);
        std::vector<std::string> files;
        if (venice) files = {"pre/vnscn.pre", "pre/vn.pre", "pre/vnped.pre"};
        else for (const auto& item : fs::recursive_directory_iterator(argv[1])) {
            if (item.is_regular_file()) files.push_back(item.path().lexically_relative(argv[1]).generic_string());
        }
        std::sort(files.begin(), files.end());
        success = bool(baker.file);
        for (const auto& path : files) {
            if (!success) break;
            TextureSource kind;
            if (GetKind(path, kind)) success = baker.Add(ReadRenderResource(path.c_str()), kind, path);
            else if (path.compare(0, 4, "pre/") == 0 && EndsWith(path, ".pre")) success = BakeArchive(baker, path);
            if (!success) std::fprintf(stderr, "Failed while converting %s\n", path.c_str());
        }
        success = success && baker.Finish();
    }
    Mem::Manager::sCloseDown();
    return success ? 0 : 1;
}
