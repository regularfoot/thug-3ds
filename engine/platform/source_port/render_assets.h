#pragma once
#include <source_port/resource_reader.h>
#include <array>
#include <vector>
#include <memory>
#include <gfx/nxhierarchy.h>
namespace SourcePort {
using ResourceBytes=std::vector<uint8>;
enum class RenderResourceStorage { OriginalHeap, NativeHeap };
std::shared_ptr<const ResourceBytes> ReadRenderResource(const char* name,
    RenderResourceStorage storage=RenderResourceStorage::OriginalHeap);
struct DiskResourceSlice;
struct TextureLevel { unsigned width=0,height=0;std::size_t offset=0,size=0; };
struct TextureAsset {
    uint32 checksum=0;unsigned width=0,height=0,actual_width=0,actual_height=0,depth=0,palette_depth=0,dxt=0;
    bool image=false,swizzled=true,font_palette=false;
    std::size_t palette_offset=0,palette_size=0;
    std::shared_ptr<const ResourceBytes> bytes;
    std::shared_ptr<const DiskResourceSlice> disk;
    std::vector<TextureLevel> levels;
};
bool ReadTextureDictionary(std::shared_ptr<const ResourceBytes>,std::vector<TextureAsset>&,ResourceError* error=nullptr);
bool ReadDiskTextureDictionary(std::shared_ptr<const DiskResourceSlice>,std::vector<TextureAsset>&,uint32& checksum);
bool ReadTextureImage(std::shared_ptr<const ResourceBytes>,TextureAsset&,ResourceError* error=nullptr);
bool DecodeTextureLevel(const TextureAsset&,unsigned,std::vector<uint8>& rgba,ResourceError* error=nullptr);
struct FontGlyph { uint16 baseline=0;int16_t character=0;uint16 x=0,y=0,width=0,height=0; };
struct FontAsset { uint32 height=0,base=0;std::array<uint8,256> map{};std::array<uint8,32> special{};std::vector<FontGlyph> glyphs;TextureAsset texture; };
bool ReadFont(std::shared_ptr<const ResourceBytes>,FontAsset&,ResourceError* error=nullptr);
struct ColorKey { int time=0;uint32 rgba=0; };
struct TextureKey { uint32 time=0,checksum=0; };
struct ColorSequence { int phase=0;std::vector<ColorKey> keys; };
struct MaterialPass {
    uint32 texture=0,flags=0;bool has_color=false;
    std::array<float,3> color{};uint64 alpha=0;
    uint32 u_address=0,v_address=0,filter=0;std::array<float,2> env_tiling{};
    std::array<float,8> uv_wibble{};
    int anim_period=0,anim_iterations=0,anim_phase=0;std::vector<TextureKey> animation;
    std::array<uint32,4> mip{};
};
struct MaterialAsset {
    uint32 checksum=0,name=0,alpha_cutoff=0;bool sorted=false,single_sided=false,no_backface=false,grass=false;
    float draw_order=0,grass_height=0;int z_bias=0,grass_layers=0;std::array<float,4> specular{};
    std::vector<MaterialPass> passes;std::vector<ColorSequence> color_wibble;
};
struct AssetVertex {
    std::array<float,3> position{},normal{};uint32 color=0xffffffff,weights=0;
    std::array<uint16,4> bones{};uint8 color_wibble=0;
};
struct MeshAsset {
    std::array<float,4> sphere{};std::array<float,6> box{};uint32 flags=0,material=0;
    std::vector<std::vector<uint16>> lods;
    float distant_error=0;
    std::vector<uint16> distant_triangles; // optional 3DS render-only triangle list
};
struct StaticAssetVertex {
    std::array<float, 3> position{};
    uint32 color = 0xffffffff;
};
static_assert(sizeof(StaticAssetVertex) == 16);
struct WideUV { uint32 index; float value; };
struct SectorAsset {
    uint32 checksum=0,flags=0;int bone=-1;std::array<float,6> box{};std::array<float,4> sphere{};
    uint32 billboard_type=0;std::array<float,9> billboard{};unsigned stride=0,uv_sets=0;
    std::vector<AssetVertex> vertices;
    std::vector<std::array<float,2>> uv;
    std::vector<MeshAsset> meshes;
    std::vector<StaticAssetVertex> static_vertices;
    std::vector<std::array<float, 3>> static_normals;
    std::vector<uint8> static_wibble;
    std::vector<int16_t> packed_uv;
    std::vector<WideUV> wide_uv;
    std::size_t VertexCount() const { return vertices.empty() ? static_vertices.size() : vertices.size(); }
    const std::array<float, 3>& Position(std::size_t index) const {
        return vertices.empty() ? static_vertices[index].position : vertices[index].position;
    }
    uint32 Color(std::size_t i) const { return vertices.empty() ? static_vertices[i].color : vertices[i].color; }
    uint8 ColorWibble(std::size_t i) const {
        return vertices.empty() ? (static_wibble.empty() ? 0 : static_wibble[i]) : vertices[i].color_wibble;
    }
    AssetVertex Vertex(std::size_t index) const;
    std::array<float, 2> UV(std::size_t index) const;
};
struct SceneAsset {
    std::array<uint32,3> versions{};std::vector<MaterialAsset> materials;std::vector<SectorAsset> sectors;
    // All scene streams and hierarchy records are decoded into owned storage.
    // Unlike textures, scenes do not need to retain the input file as well.
    std::vector<Nx::CHierarchyObject> hierarchy;
    bool moscow_guard_skin = false;
    bool pedestrian_skin = false;
};
bool ReadScene(std::shared_ptr<const ResourceBytes>,SceneAsset&,ResourceError* error=nullptr);
bool ReadScene(const void*, std::size_t, SceneAsset&, ResourceError* error=nullptr, bool compact_static=false);
struct SceneSignature { std::size_t size; uint32 checksum; };
bool ReadSceneResource(const char*, SceneAsset&, ResourceError* error=nullptr, SceneSignature* signature=nullptr, bool compact_static=false);
}
