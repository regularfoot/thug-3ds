#include <source_port/texture_cache.h>
#include <source_port/world_texture_bank.h>
#include <source_port/memory.h>
#include <sys/file/filesys.h>
#include <algorithm>
#include <tuple>

namespace SourcePort {
namespace {
struct IndexEntry {
    uint32 checksum, size, kind, offset, bytes, payload_crc;
    auto Key() const { return std::tie(checksum, size, kind); }
};

constexpr unsigned IndexHeaderSize = 16;
constexpr unsigned IndexRecordSize = 24;
std::array<IndexEntry, MaxTextureSources> index;
std::array<IndexEntry, 256> venice_index;
unsigned entries = 0, venice_entries = 0;
bool indexed = false, venice_indexed = false;
void* retail_payload = nullptr;
void* venice_payload = nullptr;

const char* CachePath(TextureCacheFile cache) {
    return cache == TextureCacheFile::Venice ? "venice.3tc" : "textures.3tc";
}
void*& CacheHandle(TextureCacheFile cache) {
    return cache == TextureCacheFile::Venice ? venice_payload : retail_payload;
}

void* OpenCache(TextureCacheFile cache) {
    auto& handle = CacheHandle(cache);
    if (!handle) handle = File::Open(CachePath(cache), "rb");
    return handle;
}

void DropCacheHandle(TextureCacheFile cache) {
    auto& handle = CacheHandle(cache);
    if (handle) File::Close(handle);
    handle = nullptr;
}

bool SeekCache(void* handle, unsigned offset) {
    return File::GetFilePosition(handle) == long(offset) || File::Seek(handle,offset,SEEK_SET) == 0;
}

bool IsTextureDimension(unsigned n) { return n >= 8 && n <= 1024 && !(n & (n - 1)); }

template<class Index>
bool LoadIndex(void* file, Index& index, unsigned& entries) {
    const auto file_size = File::GetFileSize(file);
    std::array<uint8, IndexHeaderSize> header{};
    if (file_size < long(header.size()) ||
        File::Read(header.data(), 1, header.size(), file) != header.size()) return false;
    ResourceReader reader(header.data(), header.size(), nullptr);
    if (reader.U32() != TextureCacheVersion || reader.U32() != TextureCacheLimit) return false;
    const auto count = reader.U32();
    const auto crc = reader.U32();
    if (!count || count > index.size() || IndexHeaderSize + count * IndexRecordSize > unsigned(file_size)) return false;
    ResourceBytes wire(count * IndexRecordSize);
    if (File::Read(wire.data(), 1, wire.size(), file) != wire.size() ||
        WorldTextureChecksum(wire.data(), wire.size()) != crc) return false;
    ResourceReader records(wire.data(), wire.size(), nullptr);
    const auto first_data = IndexHeaderSize + wire.size();
    for (unsigned i = 0; i < count; ++i) {
        auto& item = index[i];
        item = {records.U32(), records.U32(), records.U32(), records.U32(), records.U32(), records.U32()};
        if (!item.size || item.kind > uint32(TextureSource::Font) ||
            !item.bytes || item.bytes > MaxTextureGroupBytes || item.offset < first_data ||
            item.offset > unsigned(file_size) || item.bytes > unsigned(file_size) - item.offset ||
            (i && !(index[i-1].Key() < item.Key()))) return false;
    }
    entries = count;
    return true;
}

const IndexEntry* FindIndexEntry(uint32 checksum, uint32 size, TextureSource kind, TextureCacheFile cache) {
    const bool venice = cache == TextureCacheFile::Venice;
    auto& loaded = venice ? venice_indexed : indexed;
    auto& count = venice ? venice_entries : entries;
    if (!loaded) {
        loaded = true;
        if (auto* file = OpenCache(cache)) {
            if (!SeekCache(file,0) ||
                !(venice ? LoadIndex(file,venice_index,count) : LoadIndex(file,index,count))) DropCacheHandle(cache);
        }
    }
    if (!count) return nullptr;
    const auto key = std::make_tuple(checksum, size, uint32(kind));
    const auto begin = venice ? venice_index.data() : index.data();
    const auto end = begin + count;
    const auto item = std::lower_bound(begin, end, key, [](const auto& record, const auto& value) {
        return record.Key() < value;
    });
    return item == end || item->Key() != key ? nullptr : item;
}

}

std::size_t NativeTextureLevelBytes(SourceTextureFormat format, unsigned width, unsigned height) {
    switch (format) {
        case SOURCE_TEXTURE_ETC1: return std::size_t(width) * height / 2;
        case SOURCE_TEXTURE_ETC1A4: return std::size_t(width) * height;
        case SOURCE_TEXTURE_RGB565:
        case SOURCE_TEXTURE_RGBA5551:
        case SOURCE_TEXTURE_RGBA4:
        case SOURCE_TEXTURE_LA8: return std::size_t(width) * height * 2;
        case SOURCE_TEXTURE_RGBA8: return std::size_t(width) * height * 4;
    }
    return 0;
}

namespace {
bool ReadGroup(ResourceReader& reader, std::vector<TextureCacheEntry>& out, std::size_t base) {
    const auto count = reader.U32();
    if (!count || !reader.Count(count, 20, 16384)) return false;
    std::vector<TextureCacheEntry> parsed;
    parsed.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        TextureCacheEntry entry;
        entry.key = reader.U32();
        entry.width = reader.U16();
        entry.height = reader.U16();
        entry.sample_width = reader.U16();
        entry.sample_height = reader.U16();
        entry.levels = reader.U16();
        const auto format = reader.U16();
        entry.bytes = reader.U32();
        entry.offset = base + reader.Position();
        if (!reader.Ok() || format > SOURCE_TEXTURE_ETC1A4 ||
            !IsTextureDimension(entry.width) || !IsTextureDimension(entry.height) ||
            !entry.sample_width || entry.sample_width > entry.width ||
            !entry.sample_height || entry.sample_height > entry.height ||
            !entry.levels || entry.levels > 8 ||
            (i && entry.key <= parsed.back().key)) return false;
        entry.format = SourceTextureFormat(format);
        auto width = entry.width;
        auto height = entry.height;
        std::size_t bytes = 0;
        for (unsigned level = 0; level < entry.levels; ++level, width /= 2, height /= 2) {
            if (width < 8 || height < 8) return false;
            bytes += NativeTextureLevelBytes(entry.format, width, height);
        }
        if (bytes != entry.bytes) return false;
        while (bytes) {
            const auto chunk = std::min<std::size_t>(bytes, 4096);
            if (!reader.Skip(chunk)) return false;
            bytes -= chunk;
        }
        parsed.push_back(entry);
    }
    if (!reader.Ok() || reader.Remaining()) return false;
    out.swap(parsed);
    return true;
}

}
bool ReadTextureCacheGroup(const ResourceBytes& data, std::vector<TextureCacheEntry>& out) {
    ResourceReader reader(data.data(), data.size(), nullptr);
    return ReadGroup(reader, out, 0);
}

TextureCacheFile FindDiskTextureCacheGroup(uint32 checksum, uint32 size, TextureSource kind,
    std::vector<TextureCacheEntry>& entries) {
    if (!size) return TextureCacheFile::None;
    RootHeapScope storage;
    for (const auto cache : {TextureCacheFile::Venice, TextureCacheFile::Retail}) {
        const auto* record = FindIndexEntry(checksum, size, kind, cache);
        if (!record) continue;
        auto* file = OpenCache(cache);
        if (!file) continue;
        struct Input { void* file; uint32 crc = 0xffffffff; } input{file};
        const auto fill = [](void* context, uint8* bytes, std::size_t count) {
            auto& in = *static_cast<Input*>(context);
            if (File::Read(bytes, 1, count, in.file) != count) return false;
            in.crc = Crc::UpdateCRC(reinterpret_cast<const char*>(bytes), int(count), in.crc);
            return true;
        };
        std::array<uint8, 4096> buffer;
        ResourceReader reader(record->bytes, nullptr, fill, &input, buffer.data(), buffer.size());
        const bool valid = SeekCache(file, record->offset) &&
            ReadGroup(reader, entries, record->offset) && input.crc == record->payload_crc;
        if (valid) return cache;
        DropCacheHandle(cache);
        entries.clear();
    }
    return TextureCacheFile::None;
}

bool ReadDiskTexturePayload(TextureCacheFile cache, const TextureCacheEntry& entry, ResourceBytes& bytes) {
    if (cache == TextureCacheFile::None || entry.offset > INT_MAX || !entry.bytes) return false;
    auto* file = OpenCache(cache);
    if (!file) return false;
    try {
        NativeHeapScope staging;
        bytes.resize(entry.bytes);
    } catch (const std::bad_alloc&) {
        auto* region=Mem::Manager::sHandle().BottomUpHeap()->ParentRegion();
        const auto available=region->MemAvailable();
        if (available<0 || std::size_t(available)<std::size_t(entry.bytes)+65536u) return false;
        RootHeapScope fallback(RootHeapDirection::TopDown);
        bytes.resize(entry.bytes);
    }
    const bool read = SeekCache(file, entry.offset) &&
        File::Read(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (!read) DropCacheHandle(cache);
    return read;
}

std::shared_ptr<const ResourceBytes> FindTextureCacheGroup(const ResourceBytes& source, TextureSource kind) {
    if (source.empty() || source.size() > INT_MAX) return {};
    return FindTextureCacheGroup(WorldTextureChecksum(source.data(), source.size()), source.size(), kind);
}

std::shared_ptr<const ResourceBytes> FindTextureCacheGroup(uint32 checksum, uint32 size, TextureSource kind) {
    if (!size) return {};
    RootHeapScope storage;
    const auto* item = FindIndexEntry(checksum, size, kind, TextureCacheFile::Retail);
    if (!item) return {};

    // Keep enough contiguous room for original scene/UI objects and scratch.
    constexpr unsigned reserve = 2 * 1024 * 1024;
    const auto available = Mem::Manager::sHandle().BottomUpHeap()->ParentRegion()->MemAvailable();
    if (available < long(item->bytes + reserve)) return {};
    auto* file = OpenCache(TextureCacheFile::Retail);
    if (!file) return {};
    std::shared_ptr<ResourceBytes> group;
    if (SeekCache(file, item->offset)) {
        group = std::make_shared<ResourceBytes>(item->bytes);
        if (File::Read(group->data(), 1, group->size(), file) != group->size() ||
            WorldTextureChecksum(group->data(), group->size()) != item->payload_crc) group.reset();
    }
    if (!group) DropCacheHandle(TextureCacheFile::Retail);
    return group;
}

void ResetTextureCacheIndex() {
    DropCacheHandle(TextureCacheFile::Retail);
    DropCacheHandle(TextureCacheFile::Venice);
    indexed = venice_indexed = false;
    entries = venice_entries = 0;
}
}
