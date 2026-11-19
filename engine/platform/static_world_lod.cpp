#include <source_port/static_world_lod.h>
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

void LoadSceneLods(const char* scene_name, SceneSignature scene, SceneAsset& asset) {
    const std::string filename(scene_name);
    const auto separator = filename.find_last_of("/\\");
    const auto base = separator == std::string::npos ? filename : filename.substr(separator + 1);
    if (base.size() <= 8 || strcasecmp(base.c_str() + base.size() - 8, ".scn.xbx")) return;
    const auto path = "lods/" + base.substr(0, base.size() - 8) + ".3dl";
    auto* file = File::Open(path.c_str(), "rb");
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
