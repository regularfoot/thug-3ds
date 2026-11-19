#include <source_port/render_assets.h>
#include <source_port/diagnostics.h>
#include <source_port/application_os.h>
#include <sys/file/filesys.h>
#include <sys/file/pre.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <set>
namespace SourcePort {
namespace {
constexpr float UVScale = 2048.f;
constexpr int16_t WideUVMarker = std::numeric_limits<int16_t>::min();
constexpr int16_t UnusedUVMarker = WideUVMarker + 1;

int16_t pack_uv(float value, uint32 index, std::vector<WideUV>& wide) {
    uint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (bits == 0xffffffff) return UnusedUVMarker;
    constexpr auto maximum = (std::numeric_limits<int16_t>::max() - 1) / UVScale;
    if (value >= -maximum && value <= maximum)
        return static_cast<int16_t>(value * UVScale + (value < 0 ? -.5f : .5f));
    wide.push_back({index, value});
    return WideUVMarker;
}

void report_compact_scene(const char* name, const SceneAsset& asset) {
    std::size_t vertices = 0, original = 0, packed = 0;
    for (const auto& sector : asset.sectors) {
        if (sector.static_vertices.empty()) continue;
        vertices += sector.VertexCount();
        original += sector.VertexCount() * sizeof(AssetVertex) +
            sector.VertexCount() * sector.uv_sets * sizeof(std::array<float, 2>);
        packed += sector.static_vertices.capacity() * sizeof(StaticAssetVertex) +
            sector.static_normals.capacity() * sizeof(std::array<float, 3>) +
            sector.static_wibble.capacity() + sector.packed_uv.capacity() * sizeof(int16_t) +
            sector.wide_uv.capacity() * sizeof(WideUV) + sector.uv.capacity() * sizeof(std::array<float, 2>);
    }
    Log("STATIC_STREAMS scene=%s vertices=%zu old_bytes=%zu bytes=%zu", name, vertices, original, packed);
}

template<std::size_t N>void floats(ResourceReader& r,std::array<float,N>& a){for(auto& v:a)v=r.F32();}
template<std::size_t N>void source_bounds(ResourceReader& r,std::array<float,N>& a){for(auto& v:a)v=r.RawF32();}
bool materials(ResourceReader& r,std::vector<MaterialAsset>& out){
    auto count=r.U32();if(!r.Count(count,28,16384))return false;out.resize(count);std::set<uint32> keys;
    for(auto& m:out){
        m.checksum=r.U32();m.name=r.U32();auto passes=r.U32();m.alpha_cutoff=r.U32();
        if(!passes||passes>4||m.alpha_cutoff>255||!keys.insert(m.checksum).second)return r.Fail("invalid material header");
        m.sorted=r.U8()!=0;m.draw_order=r.F32();m.single_sided=r.U8()!=0;m.no_backface=r.U8()!=0;m.z_bias=r.I32();m.grass=r.U8()!=0;
        if(m.grass){m.grass_height=r.F32();m.grass_layers=r.I32();}
        m.specular[3]=r.F32();if(m.specular[3]>0)for(unsigned c=0;c<3;++c)m.specular[c]=r.F32();
        m.passes.resize(passes);
        for(unsigned i=0;i<passes&&r.Ok();++i){auto& p=m.passes[i];
            p.texture=r.U32();p.flags=r.U32();p.has_color=r.U8()!=0;floats(r,p.color);p.alpha=r.U64();
            p.u_address=r.U32();p.v_address=r.U32();floats(r,p.env_tiling);p.filter=r.U32();
            if(p.flags&1)floats(r,p.uv_wibble);
            if(!i&&(p.flags&2)){
                auto n=r.U32();if(!r.Count(n,8,4096))return false;m.color_wibble.resize(n);
                for(auto& seq:m.color_wibble){auto k=r.U32();seq.phase=r.I32();if(!r.Count(k,8,65536))return false;seq.keys.resize(k);
                    for(auto& key:seq.keys){key.time=r.I32();key.rgba=r.U32();}}
            }
            if(p.flags&(1<<11)){
                int n=r.I32();p.anim_period=r.I32();p.anim_iterations=r.I32();p.anim_phase=r.I32();
                if(n<=0||!r.Count(n,8,65536))return r.Fail("invalid animated texture keys");p.animation.resize(n);
                for(auto& key:p.animation){key.time=r.U32();key.checksum=r.U32();}
            }
            for(auto& v:p.mip)v=r.U32();
        }
        if(!r.Ok())return false;
    }return r.Ok();
}
bool parse_scene(ResourceReader& r, SceneAsset& out, bool compact_static){
    SceneAsset scene;
    for(auto& version:scene.versions){version=r.U32();if(!version||version>64)return r.Fail("unsupported scene versions");}
    r.Context("materials");if(!materials(r,scene.materials))return false;auto count=r.U32();if(!r.Count(count,64,65535))return false;
    scene.sectors.resize(count);std::set<uint32> sector_keys,material_keys;std::size_t total_vertices=0,total_indices=0;
    for(const auto& m:scene.materials)material_keys.insert(m.checksum);
    for(auto& sector:scene.sectors){
        r.Context("sector header");sector.checksum=r.U32();sector.bone=r.I32();sector.flags=r.U32();auto meshes=r.U32();
        if(!sector_keys.insert(sector.checksum).second||meshes>65535)return r.Fail("invalid sector header");
        // Some original empty/exporter sectors carry NaN bounding volumes.
        // Keep wire metadata; the native geometry derives valid culling bounds
        // from its actual positions rather than submitting these to the GPU.
        source_bounds(r,sector.box);source_bounds(r,sector.sphere);
        if(sector.flags&0x00800000){sector.billboard_type=r.U32();source_bounds(r,sector.billboard);}
        auto vertices=r.U32();sector.stride=r.U32();total_vertices+=vertices;
        if(total_vertices>1000000||!r.Count(vertices,12,65535))return r.Fail("invalid sector vertices");
        const bool compact = compact_static && vertices && sector.bone < 0 && !(sector.flags & (16 | 0x00800000));
        if (compact) sector.static_vertices.resize(vertices);
        else sector.vertices.resize(vertices);
        r.Context("positions");
        for (unsigned i=0;i<vertices;++i)
            floats(r, compact ? sector.static_vertices[i].position : sector.vertices[i].position);
        r.Context("normals");
        if (sector.flags & 4) {
            if (compact) sector.static_normals.resize(vertices);
            for (unsigned i=0;i<vertices;++i)
                floats(r, compact ? sector.static_normals[i] : sector.vertices[i].normal);
        }
        if(sector.flags&16){for(auto& v:sector.vertices)v.weights=r.U32();for(auto& v:sector.vertices)for(auto& bone:v.bones)bone=r.U16();}
        else if(sector.bone>=0){if(sector.bone>255)return r.Fail("invalid rigid sector bone");for(auto& v:sector.vertices)v.bones[0]=sector.bone;}
        if(sector.flags&1){
            sector.uv_sets=r.U32();const auto pairs=std::size_t(vertices)*sector.uv_sets;
            if(sector.uv_sets>16||!r.Count(pairs,8,16000000))return r.Fail("invalid UV stream");
            if (compact) {
                sector.packed_uv.resize(pairs * 2);
                for (uint32 i=0;i<pairs*2;++i) sector.packed_uv[i] = pack_uv(r.RawF32(), i, sector.wide_uv);
                // Heavily repeated UVs can use a large range. Keep a float
                // stream when the side table would cost as much as floats.
                if (sector.wide_uv.capacity() * sizeof(WideUV) >= pairs * 4 && pairs) {
                    sector.uv.reserve(pairs);
                    for (std::size_t i=0;i<pairs;++i) sector.uv.push_back(sector.UV(i));
                    std::vector<int16_t>().swap(sector.packed_uv);
                    std::vector<WideUV>().swap(sector.wide_uv);
                }
            } else {
                sector.uv.resize(pairs);
                for(auto& uv:sector.uv)for(auto& v:uv)v=r.RawF32();
            }
        }
        if (sector.flags & 2) for (unsigned i=0;i<vertices;++i)
            (compact ? sector.static_vertices[i].color : sector.vertices[i].color) = r.U32();
        if (sector.flags & 0x800) {
            if (compact) sector.static_wibble.resize(vertices);
            for (unsigned i=0;i<vertices;++i)
                (compact ? sector.static_wibble[i] : sector.vertices[i].color_wibble) = r.U8();
        }
        if(!r.Ok()||!r.Count(meshes,56,65535))return false;sector.meshes.resize(meshes);
        r.Context("mesh header");for(auto& mesh:sector.meshes){source_bounds(r,mesh.sphere);source_bounds(r,mesh.box);mesh.flags=r.U32();mesh.material=r.U32();auto lods=r.U32();
            if(!material_keys.count(mesh.material)||!lods||lods>8)return r.Fail("invalid mesh material/LOD");mesh.lods.resize(lods);
            for(auto& lod:mesh.lods){auto n=r.U32();total_indices+=n;if(total_indices>16000000||!r.Count(n,2,4000000))return r.Fail("invalid index stream");lod.resize(n);
                for(auto& index:lod){index=r.U16();if(index>=vertices)return r.Fail("index outside vertex stream");}}
        }
        if(!r.Ok())return false;
    }
    r.Context("hierarchy");auto n=r.U32();if(!r.Count(n,80,65535))return false;scene.hierarchy.resize(n);
    for(auto& object:scene.hierarchy){object.SetChecksum(r.U32());object.SetParentChecksum(r.U32());uint16 parent=r.U16();int16_t signed_parent;std::memcpy(&signed_parent,&parent,2);object.SetParentIndex(signed_parent);
        uint8 bone=r.U8();int8_t signed_bone;std::memcpy(&signed_bone,&bone,1);object.SetBoneIndex(signed_bone);r.Skip(5);Mth::Matrix mat;
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)mat[row][col]=r.F32();object.SetSetupMatrix(mat);}
    if(!r.Ok()||r.Remaining())return r.Fail("scene trailing bytes");out=std::move(scene);return true;
}

// PRE LZSS keeps its history in a 4 KiB ring. Retain unfinished matches
// between chunks so the parser never needs a whole decompressed scene.
struct SceneStream {
    static constexpr std::size_t history_size = 4096;
    static constexpr std::size_t initial_cursor = history_size - 18;
    enum { chunk_size = 16 * 1024 };

    std::array<uint8, history_size> history;
    const uint8* compressed = nullptr;
    std::size_t compressed_size = 0, input = 0, remaining = 0;
    unsigned flags = 0, write = initial_cursor, match = 0, match_count = 0;
    void* file = nullptr;
    bool calculate_checksum = false;
    uint32 checksum = 0xffffffff;

    bool Read(uint8* bytes, std::size_t count) {
        if (count > remaining) return false;
        if (file) {
            if (File::Read(bytes, 1, count, file) != count) return false;
        } else {
            for (std::size_t index = 0; index < count; ++index) {
                if (!match_count) {
                    if (((flags >>= 1) & 256) == 0) {
                        if (input == compressed_size) return false;
                        flags = compressed[input++] | 0xff00;
                    }
                    if (input == compressed_size) return false;
                    if (flags & 1) {
                        bytes[index] = compressed[input++];
                        history[write] = bytes[index];
                        advance_write();
                        continue;
                    }
                    if (compressed_size - input < 2) return false;
                    match = compressed[input] | ((compressed[input + 1] & 0xf0) << 4);
                    match_count = (compressed[input + 1] & 15) + 3;
                    input += 2;
                }
                // Before the first wrap, the untouched tail isn't part of the
                // original dictionary. Reject references to those bytes.
                if (uninitialized_tail && match >= write) return false;
                bytes[index] = history[match];
                history[write] = bytes[index];
                match = (match + 1) & (history_size - 1);
                advance_write();
                --match_count;
            }
        }
        remaining -= count;
        if (calculate_checksum)
            checksum = Crc::UpdateCRC(reinterpret_cast<const char*>(bytes), int(count), checksum);
        return remaining || file || (input == compressed_size && !match_count);
    }

private:
    bool uninitialized_tail = true;
    void advance_write() {
        write = (write + 1) & (history_size - 1);
        if (!write) uninitialized_tail = false;
    }
};
}

AssetVertex SectorAsset::Vertex(std::size_t index) const {
    if (!vertices.empty()) return vertices[index];
    AssetVertex result;
    result.position = static_vertices[index].position;
    result.color = static_vertices[index].color;
    if (!static_normals.empty()) result.normal = static_normals[index];
    if (!static_wibble.empty()) result.color_wibble = static_wibble[index];
    return result;
}

std::array<float, 2> SectorAsset::UV(std::size_t index) const {
    if (packed_uv.empty()) return uv[index];
    std::array<float, 2> result{};
    for (uint32 axis=0;axis<2;++axis) {
        const auto scalar = uint32(index * 2 + axis);
        const auto value = packed_uv[scalar];
        if (value == UnusedUVMarker) {
            constexpr uint32 unused = 0xffffffff;
            std::memcpy(&result[axis], &unused, sizeof(unused));
        }
        else if (value != WideUVMarker) result[axis] = value / UVScale;
        else {
            const auto at = std::lower_bound(wide_uv.begin(), wide_uv.end(), scalar,
                [](const auto& item, uint32 key) { return item.index < key; });
            Dbg_Assert(at != wide_uv.end() && at->index == scalar);
            result[axis] = at->value;
        }
    }
    return result;
}

bool ReadScene(const void* bytes, std::size_t size, SceneAsset& out, ResourceError* error, bool compact_static) {
    ResourceReader reader(bytes, size, error);
    return parse_scene(reader, out, compact_static);
}

bool ReadScene(std::shared_ptr<const ResourceBytes> bytes, SceneAsset& out, ResourceError* error) {
    return bytes && ReadScene(bytes->data(), bytes->size(), out, error);
}

bool ReadSceneResource(const char* name, SceneAsset& out, ResourceError* error, SceneSignature* signature, bool compact_static) {
    if (error) *error = {};
    if (!name || !*name || std::strlen(name) >= 128) return false;
    const auto* entry = File::PreMgr::sPreEnabled() ? File::PreMgr::Instance()->FindFileEntry(name) : nullptr;
    const auto close = [](void* handle) { File::Close(handle); };
    std::unique_ptr<void, decltype(close)> file(entry ? nullptr : File::Open(name, "rb"), close);
    if (!entry && !file) return false;
    const auto size = entry ? entry->m_filesize : File::GetFileSize(file.get());
    if (size <= 0 || size > 128 * 1024 * 1024) return false;

    if (entry && (entry->pData || !entry->compressedDataSize)) {
        const auto* data = entry->pData ? entry->pData : entry->pCompressedData;
        if (!ReadScene(data, size, out, error, compact_static)) return false;
        if (signature) *signature = {std::size_t(size), Crc::GenerateCRCCaseSensitive(
            reinterpret_cast<const char*>(data), int(size))};
        SourcePort::Log("SCENE_SOURCE mode=borrowed bytes=%u scratch=0 name=%s", unsigned(size), name);
        if (compact_static) report_compact_scene(name, out);
        return true;
    }

    SceneStream stream;
    stream.remaining = size;
    stream.file = file.get();
    stream.calculate_checksum = signature != nullptr;
    if (entry) {
        stream.compressed = entry->pCompressedData;
        stream.compressed_size = entry->compressedDataSize;
        std::fill_n(stream.history.begin(), SceneStream::initial_cursor, uint8(' '));
    }
    // Large world members are read sequentially from SD. A wider window
    // reduces FS requests without keeping a second copy of the whole scene.
    std::array<uint8, SceneStream::chunk_size> small_buffer;
    ResourceBytes read_ahead;
    if (!entry && compact_static && source_application_os_is_new_3ds() && size >= 512*1024)
        read_ahead.resize(64*1024);
    auto* buffer = read_ahead.empty() ? small_buffer.data() : read_ahead.data();
    const auto buffer_size = read_ahead.empty() ? small_buffer.size() : read_ahead.size();
    const auto fill = [](void* source, uint8* dst, std::size_t bytes) {
        return static_cast<SceneStream*>(source)->Read(dst, bytes);
    };
    SourcePort::Log("SCENE_SOURCE mode=%s bytes=%u scratch=%u name=%s",
        entry ? "lzss" : "disk", unsigned(size), unsigned(sizeof(stream) + buffer_size), name);
    ResourceReader reader(size, error, fill, &stream, buffer, buffer_size);
    if (!parse_scene(reader, out, compact_static)) return false;
    if (compact_static) report_compact_scene(name, out);
    if (signature) *signature = {std::size_t(size), stream.checksum};
    return true;
}
}
