#include <source_port/static_world_lod.h>
#include <source_port/application_os.h>
#include <sys/file/filesys.h>
#include <algorithm>
#include <string>

namespace SourcePort {
uint32 SceneLodChecksum(const ResourceBytes& bytes) {
    return Crc::GenerateCRCCaseSensitive(reinterpret_cast<const char*>(bytes.data()), int(bytes.size()));
}

bool CanSimplifyWorldMesh(const SectorAsset& sector, const MeshAsset& mesh, const MaterialAsset& material) {
    if (sector.bone >= 0 || (sector.flags & (16 | 0x800 | 0x00800000)) ||
        mesh.lods.size() != 1 || material.passes.empty() ||
        !material.color_wibble.empty() || material.grass || material.z_bias != 0)
        return false;
    // Additional environment passes are already excluded from the performance path.
    if (std::any_of(material.passes.begin() + 1, material.passes.end(),
        [](const auto& pass) { return !(pass.flags & 8); })) return false;
    const auto& pass = material.passes[0];
    const auto blend = pass.alpha & 0xffffff;
    return !(pass.flags & (1 | 2 | 8 | (1 << 11))) && (blend == 0 || blend == 5);
}

static bool read_scene_lods(const ResourceBytes& sidecar, SceneSignature scene, SceneAsset& asset) {
    if (sidecar.size() > StaticLodMaxBytes) return false;
    ResourceReader reader(sidecar.data(), sidecar.size(), nullptr);
    const auto version = reader.U32();
    const auto size = reader.U32();
    const auto checksum = reader.U32();
    const auto count = reader.U32();
    if (version != StaticLodVersion || size != scene.size ||
        !reader.Count(count, 22, 65535) || checksum != scene.checksum) return false;

    struct Entry {
        MeshAsset* mesh;
        float error;
        std::vector<uint16> triangles;
    };
    std::vector<Entry> entries;
    entries.reserve(count);
    std::pair<uint32, uint32> previous{};
    for (uint32 entry = 0; entry < count; ++entry) {
        const auto sector_index = reader.U32();
        const auto mesh_index = reader.U32();
        const auto indices = reader.U32();
        const auto error = reader.F32();
        const auto key = std::pair{sector_index, mesh_index};
        if ((entry && key <= previous) || sector_index >= asset.sectors.size()) return false;
        previous = key;
        auto& sector = asset.sectors[sector_index];
        if (mesh_index >= sector.meshes.size() || indices < 3 || indices % 3 ||
            error < 0 || error > StaticLodMaxError ||
            !reader.Count(indices, 2, 65535)) return false;
        auto& mesh = sector.meshes[mesh_index];
        auto material = std::find_if(asset.materials.begin(), asset.materials.end(),
            [&](const auto& item) { return item.checksum == mesh.material; });
        if (material == asset.materials.end() || !CanSimplifyWorldMesh(sector, mesh, *material)) return false;
        // Every generated vertex must occur in this particular original mesh.
        // This also prevents a malformed sidecar from exceeding its bounds.
        const auto& strip = mesh.lods[0];
        std::vector<bool> used(sector.VertexCount());
        std::size_t original_indices = 0;
        for (std::size_t i = 2; i < strip.size(); ++i) {
            const auto a = strip[i-2], b = strip[i-1], c = strip[i];
            if (a >= used.size() || b >= used.size() || c >= used.size()) return false;
            if (a == b || b == c || a == c) continue;
            used[a] = used[b] = used[c] = true;
            original_indices += 3;
        }
        if (indices >= original_indices) return false;
        std::vector<uint16> triangles;
        triangles.reserve(indices);
        for (uint32 i = 0; i < indices; i += 3) {
            const auto a = reader.U16(), b = reader.U16(), c = reader.U16();
            if (a >= used.size() || b >= used.size() || c >= used.size() ||
                a == b || b == c || a == c || !used[a] || !used[b] || !used[c]) return false;
            triangles.insert(triangles.end(), {a, b, c});
        }
        entries.push_back({&mesh, error, std::move(triangles)});
    }
    if (!reader.Ok() || reader.Remaining()) return false;
    for (auto& entry : entries) {
        entry.mesh->distant_error = entry.error;
        entry.mesh->distant_triangles = std::move(entry.triangles);
    }
    return true;
}

bool ReadSceneLods(const ResourceBytes& sidecar, const ResourceBytes& scene, SceneAsset& asset) {
    return read_scene_lods(sidecar, {scene.size(), SceneLodChecksum(scene)}, asset);
}

// FX01 binds names from the level QB to its matching retail scene.
static void load_scenery_effects(const std::string& stem, SceneSignature scene, SceneAsset& asset) {
    const std::string path = "lods/old/" + stem + ".3fx";
    void* file = File::Open(path.c_str(), "rb");
    if (!file) return;
    const long length = File::GetFileSize(file);
    ResourceBytes data;
    if (length >= 16 && length <= 16 + 5 * 4096) data.resize(length);
    const bool read = !data.empty() && File::Read(data.data(), 1, data.size(), file) == data.size();
    File::Close(file);
    if (!read || std::memcmp(data.data(), "FX01", 4)) return;
    ResourceReader reader(data.data() + 4, data.size() - 4, nullptr);
    if (reader.U32() != scene.size || reader.U32() != scene.checksum) return;
    const uint32 count = reader.U32();
    if (count > 4096 || reader.Remaining() != count * 5) return;
    std::vector<std::pair<uint32, unsigned>> names;
    names.reserve(count);
    for (uint32 i = 0; i < count; ++i) {
        const auto key = reader.U32();
        const unsigned kind = reader.U8();
        if ((kind < 1 || kind > 3) || (i && key <= names.back().first)) return;
        names.emplace_back(key, kind);
    }
    unsigned shadows = 0, reflections = 0, seats = 0;
    for (auto& sector : asset.sectors) {
        const auto match = std::lower_bound(names.begin(), names.end(), sector.checksum,
            [](const auto& entry, uint32 key) { return entry.first < key; });
        if (match == names.end() || match->first != sector.checksum || sector.meshes.empty()) continue;
        if (match->second == 3) {
            if (!strcasecmp(stem.c_str(), "sj")) {
                sector.scenery_effect = true;
                ++seats;
            }
            continue;
        }
        if (match->second == 1) {
            // Some sectors named "shadow" contain a solid ramp or floor.
            // Only remove shadow decals when every mesh uses blending.
            const bool all_decals = std::all_of(sector.meshes.begin(), sector.meshes.end(), [&](const auto& mesh) {
                const auto material = std::find_if(asset.materials.begin(), asset.materials.end(),
                    [&](const auto& item) { return item.checksum == mesh.material; });
                if (material == asset.materials.end() || material->passes.empty()) return false;
                const auto blend = material->passes.front().alpha & 0xffffff;
                return blend >= 1 && blend <= 6;
            });
            if (!all_decals) continue;
            ++shadows;
        } else {
            ++reflections;
        }
        sector.scenery_effect = true;
    }
    Log("OLD_SCENERY scene=%s shadows=%u reflections=%u seat_banks=%u", stem.c_str(), shadows, reflections, seats);
}

void LoadSceneLods(const char* scene_name, SceneSignature scene, SceneAsset& asset) {
    const std::string filename(scene_name);
    const auto separator = filename.find_last_of("/\\");
    const auto base = separator == std::string::npos ? filename : filename.substr(separator + 1);
    if (base.size() <= 8 || strcasecmp(base.c_str() + base.size() - 8, ".scn.xbx")) return;
    if (source_application_os_is_old_3ds()) load_scenery_effects(base.substr(0, base.size() - 8), scene, asset);
    const auto path = "lods/" + base.substr(0, base.size() - 8) + ".3dl";
    auto* file = source_application_os_is_old_3ds()?File::Open(("lods/old/"+base.substr(0,base.size()-8)+".3dl").c_str(),"rb"):nullptr;
    if(!file)file=File::Open(path.c_str(),"rb");
    if (!file) return;
    const auto size = File::GetFileSize(file);
    ResourceBytes sidecar;
    bool read = size > 0 && std::size_t(size) <= StaticLodMaxBytes;
    if (read) {
        sidecar.resize(size);
        read = File::Read(sidecar.data(), 1, sidecar.size(), file) == sidecar.size();
    }
    File::Close(file);
    if (!read || !read_scene_lods(sidecar, scene, asset)) {
        Log("WORLD_LOD rejected %s", scene_name);
        return;
    }
    unsigned meshes = 0;
    for (const auto& sector : asset.sectors)
        for (const auto& mesh : sector.meshes) meshes += !mesh.distant_triangles.empty();
    Log("WORLD_LOD loaded scene=%s meshes=%u bytes=%u", scene_name, meshes, unsigned(sidecar.size()));
}
}
