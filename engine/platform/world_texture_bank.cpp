#include <source_port/world_texture_bank.h>
#include <sys/file/filesys.h>
#include <algorithm>
#include <string>

namespace SourcePort {
namespace {
constexpr uint32 BankVersion = 0x31435445; // ETC1
constexpr std::size_t HeaderBytes = 24;
constexpr std::size_t MaxBankBytes = 2 * 1024 * 1024;

bool ValidDimension(unsigned value, unsigned limit) {
    return value >= 8 && value <= limit && !(value & (value - 1));
}
}

std::set<uint32> OpaqueWorldTextures(const std::vector<MaterialAsset>& materials, bool allow_alpha_test) {
    std::set<uint32> eligible, excluded;
    for (const auto& material : materials) {
        for (const auto& pass : material.passes) {
            const bool opaque_base = &pass == &material.passes.front() &&
                (allow_alpha_test || !material.alpha_cutoff) &&
                !(pass.flags & 8) && (pass.alpha & 0xffffff) == 0;
            if (pass.texture) (opaque_base ? eligible : excluded).insert(pass.texture);
            // Animated replacements can be shared with a blended or cutout pass.
            for (const auto& key : pass.animation) excluded.insert(key.checksum);
        }
    }
    for (auto key : excluded) eligible.erase(key);
    return eligible;
}

uint32 WorldTextureChecksum(const uint8* data, std::size_t size) {
    return Crc::GenerateCRCCaseSensitive(reinterpret_cast<const char*>(data), int(size));
}

bool ReadWorldTextureBank(const ResourceBytes& bank, const ResourceBytes& dictionary, unsigned limit,
    std::vector<WorldTextureEntry>& entries) {
    if (bank.size() < HeaderBytes || bank.size() > MaxBankBytes || limit < 8 || limit > 1024) return false;
    ResourceReader reader(bank.data(), bank.size(), nullptr);
    if (reader.U32() != BankVersion || reader.U32() != dictionary.size() || reader.U32() != limit) return false;
    if (reader.U32() != WorldTextureChecksum(dictionary.data(), dictionary.size())) return false;
    const auto count = reader.U32();
    const auto crc = reader.U32();
    if (!count || !reader.Count(count, 16, 4096) ||
        crc != WorldTextureChecksum(bank.data() + HeaderBytes, bank.size() - HeaderBytes)) return false;

    std::vector<WorldTextureEntry> parsed;
    parsed.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        WorldTextureEntry entry;
        entry.checksum = reader.U32();
        entry.width = reader.U16();
        entry.height = reader.U16();
        entry.levels = reader.U32();
        entry.bytes = reader.U32();
        entry.offset = reader.Position();
        if ((!parsed.empty() && entry.checksum <= parsed.back().checksum) ||
            !ValidDimension(entry.width, limit) || !ValidDimension(entry.height, limit) ||
            entry.levels < 2 || entry.levels > 8) return false;
        std::size_t expected = 0;
        auto w = entry.width;
        auto h = entry.height;
        for (unsigned level = 0; level < entry.levels; ++level, w /= 2, h /= 2) {
            if (w < 8 || h < 8) return false;
            expected += w * h / 2;
        }
        if (entry.bytes != expected || !reader.Skip(expected)) return false;
        parsed.push_back(entry);
    }
    if (!reader.Ok() || reader.Remaining()) return false;
    entries.swap(parsed);
    return true;
}

std::shared_ptr<const ResourceBytes> LoadWorldTextureBank(const char* scene_name) {
    const std::string filename(scene_name);
    const auto separator = filename.find_last_of("/\\");
    const auto base = separator == std::string::npos ? filename : filename.substr(separator + 1);
    if (base.size() <= 8 || strcasecmp(base.c_str() + base.size() - 8, ".scn.xbx")) return {};
    const auto path = "worldtex/" + base.substr(0, base.size() - 8) + ".etc";
    auto* file = File::Open(path.c_str(), "rb");
    if (!file) return {};
    const auto size = File::GetFileSize(file);
    std::shared_ptr<ResourceBytes> data;
    // This cache is optional. Leave space for the original scene's objects.
    constexpr long scene_reserve = 2 * 1024 * 1024;
    auto& memory = Mem::Manager::sHandle();
    const long root_free = memory.BottomUpHeap()->ParentRegion()->MemAvailable();
    if (size >= long(HeaderBytes) && std::size_t(size) <= MaxBankBytes &&
        size + scene_reserve <= root_free) {
        data = std::make_shared<ResourceBytes>(size);
        if (File::Read(data->data(), 1, data->size(), file) != data->size()) data.reset();
    }
    File::Close(file);
    return data;
}
}
