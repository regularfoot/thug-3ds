#include <source_port/files.h>
#include <source_port/mesh_assets.h>
#include <source_port/static_world_lod.h>
#include <sys/file/filesys.h>
#include <sys/file/pre.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>

namespace {
using namespace SourcePort;
namespace fs = std::filesystem;
struct Vertex {
    std::array<float, 3> position;
    std::array<float, 6> attributes; // original UV plus RGBA
};

bool MostlyUpright(const std::vector<Vertex>& vertices, const std::vector<unsigned>& indices) {
    float total = 0;
    float upright = 0;
    auto point = [&](unsigned i) {
        const auto& p = vertices[i].position;
        return Mth::Vector(p[0], p[1], p[2], 0);
    };
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        const auto a = point(indices[i]);
        const auto normal = Mth::CrossProduct(point(indices[i+1])-a, point(indices[i+2])-a);
        const auto area = normal.Length();
        total += area;
        if (std::abs(normal[Y]) < area * .45f) upright += area;
    }
    return total > 0 && upright >= total * .60f;
}

void LockBoundaryCorners(const std::vector<Vertex>& vertices, const std::vector<unsigned>& indices,
                         std::vector<unsigned char>& locks) {
    std::map<std::pair<unsigned, unsigned>, unsigned> edges;
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        for (unsigned side = 0; side < 3; ++side) {
            auto [a, b] = std::minmax(indices[i + side], indices[i + (side + 1) % 3]);
            ++edges[{a, b}];
        }
    }
    struct Neighbors { std::array<unsigned, 2> ends{}; unsigned count=0; };
    std::vector<Neighbors> border(vertices.size());
    auto add = [&](unsigned a, unsigned b) {
        auto& point = border[a];
        if (point.count < point.ends.size()) point.ends[point.count] = b;
        ++point.count;
    };
    for (const auto& [edge, count] : edges) {
        if (count == 1) {
            add(edge.first, edge.second);
            add(edge.second, edge.first);
        } else if (count > 2) {
            locks[edge.first] = locks[edge.second] = meshopt_SimplifyVertex_Lock;
        }
    }
    for (unsigned i = 0; i < border.size(); ++i) {
        const auto& neighbors = border[i];
        if (!neighbors.count || locks[i]) continue;
        bool straight = false;
        if (neighbors.count == 2) {
            const auto& a = vertices[neighbors.ends[0]];
            const auto& b = vertices[neighbors.ends[1]];
            const auto& p = vertices[i];
            Mth::Vector edge(b.position[0]-a.position[0], b.position[1]-a.position[1], b.position[2]-a.position[2], 0);
            Mth::Vector delta(p.position[0]-a.position[0], p.position[1]-a.position[1], p.position[2]-a.position[2], 0);
            const float length = edge.LengthSqr();
            if (length > 0) {
                const float t = Mth::DotProduct(delta, edge) / length;
                straight = t > 0 && t < 1 && (delta-edge*t).LengthSqr() <= .000004f;
                for (unsigned k = 0; k < p.attributes.size(); ++k)
                    straight &= std::abs(p.attributes[k] - (a.attributes[k]+t*(b.attributes[k]-a.attributes[k]))) <= .0002f;
            }
        }
        // The only movable border vertices are redundant subdivisions of
        // a straight, attribute-linear edge. Preserve corners and holes.
        if (!straight) locks[i] = meshopt_SimplifyVertex_Lock;
    }
}

// Cutoff 1 is also used on solid scenery. Evaluate the source texture once
// per dictionary entry, including mips that the handheld may select at a distance.
class TextureOpacity {
    std::vector<TextureAsset> textures;
    std::map<uint32, const TextureAsset*> by_id;
    std::map<uint32, bool> cache;
public:
    void Load(File::PreFile& pre, const std::string& level) {
        const auto name = "levels/" + level + "/" + level + ".tex.xbx";
        if (!pre.GetContainedFile(name.c_str())) return;
        pre.Reset();
        auto bytes = std::make_shared<ResourceBytes>(pre.GetFileSize());
        const bool read = pre.Read(bytes->data(), bytes->size()) == int(bytes->size());
        pre.Close(false);
        if (read && ReadTextureDictionary(bytes, textures))
            for (const auto& texture : textures) by_id.emplace(texture.checksum, &texture);
    }

    bool IsOpaque(uint32 id) {
        if (!id) return true; // An untextured material uses vertex alpha only.
        if (auto it = cache.find(id); it != cache.end()) return it->second;
        const auto it = by_id.find(id);
        bool opaque = it != by_id.end() && !it->second->levels.empty();
        ResourceBytes rgba;
        for (unsigned level = 0; opaque && level < it->second->levels.size(); ++level) {
            opaque = DecodeTextureLevel(*it->second, level, rgba) && !rgba.empty();
            for (std::size_t i = 3; opaque && i < rgba.size(); i += 4)
                opaque = rgba[i] == 255;
        }
        cache.emplace(id, opaque);
        return opaque;
    }
};

std::vector<uint16> Simplify(const SectorAsset& sector, const MaterialAsset& material,
                             const RenderTopology& topology, bool opaque_surfaces, bool old_surfaces, TextureOpacity& opacity, float& error) {
    if (topology.triangles.size() < 18 || topology.triangles.size() > 60000) return {};
    // Preserve boundaries and seams, but put less weight on interior UV
    // and color variation for distant opaque meshes on the smaller target.
    const auto& pass = material.passes.front();
    const bool solid_pass = (pass.alpha & 0xffffff) == 0;
    const bool solid_cutoff = old_surfaces && solid_pass && material.alpha_cutoff == 1 &&
        !material.sorted && opacity.IsOpaque(pass.texture) &&
        ((pass.flags & (1u << 12)) || std::all_of(topology.vertices.begin(), topology.vertices.end(),
            [&](uint16 index) { return (sector.Color(index) >> 24) >= 128; }));
    const bool opaque = solid_pass && (material.alpha_cutoff == 0 || solid_cutoff);
    const bool flat = old_surfaces && opaque && !material.sorted && !(pass.flags & 8);
    const bool has_uv = material.passes[0].texture && sector.uv_sets;
    std::vector<Vertex> vertices;
    std::vector<uint16> source_indices;
    std::vector<unsigned> remap;
    std::map<std::pair<std::array<float, 3>, std::array<float, 6>>, unsigned> equivalent;
    vertices.reserve(topology.vertices.size());
    for (auto index : topology.vertices) {
        const auto& source = sector.vertices[index];
        const auto uv = has_uv ? sector.uv[index * sector.uv_sets] : std::array<float, 2>{};
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return {};
        const auto c = source.color;
        const Vertex vertex{source.position, {uv[0], uv[1], float((c >> 16) & 255) / 255,
            float((c >> 8) & 255) / 255, float(c & 255) / 255, float(c >> 24) / 255}};
        const auto [entry, inserted] = equivalent.try_emplace(
            std::pair{vertex.position, vertex.attributes}, vertices.size());
        remap.push_back(entry->second);
        if (inserted) {
            vertices.push_back(vertex);
            source_indices.push_back(index);
        }
    }
    std::vector<unsigned> indices;
    indices.reserve(topology.triangles.size());
    for (std::size_t i = 0; i < topology.triangles.size(); i += 3) {
        const auto a = remap[topology.triangles[i]], b = remap[topology.triangles[i+1]], c = remap[topology.triangles[i+2]];
        if (a != b && b != c && a != c) indices.insert(indices.end(), {a, b, c});
    }
    if (indices.size() < 6) return {};
    if ((!opaque_surfaces || !opaque) && !MostlyUpright(vertices, indices)) return {};

    // Preserve all UV and color seams, including a seam represented by
    // coincident vertices with different UV tiles in the original exporter.
    std::map<std::array<float, 3>, unsigned> first;
    std::vector<unsigned char> locks(vertices.size());
    for (unsigned i = 0; i < vertices.size(); ++i) {
        const auto [at, inserted] = first.try_emplace(vertices[i].position, i);
        if (!inserted && vertices[at->second].attributes != vertices[i].attributes)
            locks[at->second] = meshopt_SimplifyVertex_Lock;
    }
    for (unsigned i = 0; i < vertices.size(); ++i)
        locks[i] = locks[first.at(vertices[i].position)];
    LockBoundaryCorners(vertices, indices, locks);

    constexpr float weights[]{1.5f, 1.5f, .25f, .25f, .25f, 1.f};
    std::vector<unsigned> simplified(indices.size());
    constexpr float distant_weights[]{.08f,.08f,.05f,.05f,.05f,1.f};
    const auto target = std::max<std::size_t>(2, indices.size() / (flat ? 15 : opaque_surfaces && opaque ? 9 : 6)) * 3;
    auto run = [&](const float* importance, std::size_t target_count) {
        return meshopt_simplifyWithAttributes(simplified.data(), indices.data(),
            indices.size(), vertices[0].position.data(), vertices.size(), sizeof(Vertex),
            vertices[0].attributes.data(), sizeof(Vertex), importance, std::size(weights),
            locks.data(), target_count, StaticLodMaxError, meshopt_SimplifyErrorAbsolute, &error);
    };
    auto acceptable = [&](std::size_t count) {
        return count >= 3 && count <= topology.triangles.size() * 4 / 5 &&
               std::isfinite(error) && error <= StaticLodMaxError;
    };
    auto count = run(flat ? distant_weights : weights, target);
    // A more aggressive target can lose a previously valid upright LOD.
    if (!acceptable(count) && solid_cutoff && MostlyUpright(vertices, indices))
        count = run(weights, std::max<std::size_t>(2, indices.size() / 6) * 3);
    if (!acceptable(count)) return {};
    std::vector<uint16> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) result.push_back(source_indices[simplified[i]]);
    return result;
}

void AppendWord(ResourceBytes& bytes, uint32 value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(uint8(value >> shift));
}

bool Bake(File::PreFile& pre, const std::string& level, const fs::path& output, bool opaque_surfaces, bool old_surfaces) {
    const auto name = "levels/" + level + "/" + level + ".scn.xbx";
    if (!pre.GetContainedFile(name.c_str())) return true;
    pre.Reset();
    auto bytes = std::make_shared<ResourceBytes>(pre.GetFileSize());
    const bool loaded = pre.Read(bytes->data(), bytes->size()) == int(bytes->size());
    pre.Close(false);
    SceneAsset asset;
    if (!loaded || !ReadScene(bytes, asset)) return false;
    std::map<uint32, const MaterialAsset*> materials;
    for (const auto& material : asset.materials) materials.emplace(material.checksum, &material);

    TextureOpacity opacity;
    if (old_surfaces) opacity.Load(pre, level);
    ResourceBytes entries;
    unsigned count = 0, blended = 0;
    std::size_t before = 0, after = 0;
    for (unsigned si = 0; si < asset.sectors.size(); ++si) {
        const auto& sector = asset.sectors[si];
        for (unsigned mi = 0; mi < sector.meshes.size(); ++mi) {
            const auto& mesh = sector.meshes[mi];
            const auto& material = *materials.at(mesh.material);
            if (!CanSimplifyWorldMesh(sector, mesh, material)) continue;
            RenderTopology topology;
            if (!BuildRenderTopology(asset, si, mi, 0, nullptr, 0, topology)) return false;
            float error = 0;
            auto reduced = Simplify(sector, material, topology, opaque_surfaces, old_surfaces, opacity, error);
            if (reduced.empty()) continue;
            ++count;
            blended += (material.passes[0].alpha & 0xffffff) != 0;
            before += topology.triangles.size()/3;
            after += reduced.size()/3;
            AppendWord(entries, si);
            AppendWord(entries, mi);
            AppendWord(entries, reduced.size());
            uint32 error_bits;
            std::memcpy(&error_bits, &error, sizeof(error_bits));
            AppendWord(entries, error_bits);
            for (auto index : reduced) {
                entries.push_back(uint8(index));
                entries.push_back(uint8(index >> 8));
            }
        }
    }
    if (!count) return true;
    ResourceBytes wire;
    AppendWord(wire, StaticLodVersion);
    AppendWord(wire, bytes->size());
    AppendWord(wire, SceneLodChecksum(*bytes));
    AppendWord(wire, count);
    wire.insert(wire.end(), entries.begin(), entries.end());
    if (!ReadSceneLods(wire, *bytes, asset)) return false;
    std::ofstream file(output / (level + ".3dl"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(wire.data()), wire.size());
    file.close();
    std::printf("%s meshes=%u blended=%u triangles=%zu->%zu bytes=%zu\n",
                level.c_str(), count, blended, before, after, wire.size());
    return bool(file);
}
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: source_bake_world_lods RETAIL_DATA NEW_LOD_DIRECTORY [--opaque-surfaces|--old-surfaces] [level ...]\n");
        return 2;
    }
    Mem::Manager::sSetUp();
    Mem::Manager::sHandle().InitOtherHeaps();
    bool ok = true;
    {
        Dbg_Assert(SourcePort::ConfigureFileRoots(argv[1]));
        SourcePort::FileSystemSession files;
        const fs::path output(argv[2]);
        fs::create_directories(output);
        std::vector<std::string> levels;
        for (const auto& entry : fs::directory_iterator(fs::path(argv[1]) / "pre")) {
            auto name = entry.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            if (entry.is_regular_file() && name.size() > 7 && name.compare(name.size()-7, 7, "scn.pre") == 0)
                levels.push_back(name.substr(0, name.size()-7));
        }
        std::sort(levels.begin(), levels.end());
        const bool old_surfaces=argc>3 && std::string_view(argv[3])=="--old-surfaces";
        const bool opaque_surfaces = old_surfaces || (argc > 3 && std::string_view(argv[3]) == "--opaque-surfaces");
        const int first_level = opaque_surfaces ? 4 : 3;
        if (argc > first_level) levels.assign(argv+first_level, argv+argc);
        for (const auto& level : levels) {
            auto* bytes = static_cast<uint8*>(File::LoadAllocNoPre(("pre/" + level + "scn.pre").c_str()));
            if (!bytes) { ok = false; break; }
            File::PreFile pre(bytes);
            if (!Bake(pre, level, output, opaque_surfaces, old_surfaces)) {
                std::fprintf(stderr, "Cannot bake scene %s\n", level.c_str());
                ok = false;
                break;
            }
        }
    }
    Mem::Manager::sCloseDown();
    return ok ? 0 : 1;
}
