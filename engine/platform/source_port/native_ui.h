#pragma once
#include <source_port/native_texture.h>
#include <gfx/nxfont.h>
#include <gfx/nxwin2d.h>
#include <gfx/nxsprite.h>
namespace SourcePort {
struct UIQuad {
    Nx::CNativeTexture* texture=nullptr;
    std::array<SourceRenderVertex,6> vertices{};
    float priority=0;bool depth=false;uint32 z=0;
    std::array<int,4> clip{0,0,640,448};
};
using FontMetaMapper=char(*)(const char*);
void BindOriginalFontMetaMapper(FontMetaMapper);
// Button glyphs and meta mappings are shared by all original fonts.
uint64 FontLayoutGeneration();
void GatherNativeUI(std::vector<UIQuad>&);
void ReleaseNativeUI();
void ForgetNativeClip(Nx::CWindow2D*);
// Merge only adjacent, contiguous UI ranges with identical complete draw state.
// Vertex order, per-vertex color/depth, texture changes and clips stay intact.
void CoalesceUIDrawPackets(std::vector<SourceDrawPacket>&);
class NativeUIBatch {
public:
    NativeUIBatch()=default;~NativeUIBatch();
    NativeUIBatch(const NativeUIBatch&)=delete;NativeUIBatch& operator=(const NativeUIBatch&)=delete;
    bool Prepare(const std::vector<UIQuad>&,std::vector<SourceDrawPacket>&);
    void Release();
private:
    SourceGpuMesh* mesh_=nullptr;std::vector<SourceRenderVertex> vertices_;
};
}
namespace Nx {
class CNativeFont final:public CFont {
public:
    CNativeFont();~CNativeFont()override;
    bool Assign(SourcePort::FontAsset);
    struct Glyph {CNativeTexture* texture;float x,y,width,height,u0,v0,u1,v1;Image::RGBA color;};
    bool Layout(const char*,float,float,float,float,Image::RGBA,bool,std::vector<Glyph>*,float& width)const;
    const SourcePort::FontAsset& Asset()const{return asset_;}
    unsigned Revision()const{return revision_;}
private:
    SourcePort::FontAsset asset_;CNativeTexture texture_;int char_spacing_=0,space_spacing_=0;
    std::array<Image::RGBA,16> colors_{};
    unsigned revision_=0;
    bool plat_load(const char*)override;
    void plat_set_spacings(int,int)override;
    void plat_set_rgba_table(Image::RGBA*)override;
    void plat_mark_as_button_font(bool)override;
    void plat_unload()override;
    uint32 plat_get_default_height()const override{return asset_.height;}
    uint32 plat_get_default_base()const override{return asset_.base;}
    void plat_query_string(char*,float&,float&)const override;
};
class CNativeText final:public CText {
public:
    CNativeText();~CNativeText()override;
    void Gather(std::vector<SourcePort::UIQuad>&)const;
private:
    unsigned revision_=0;
    mutable unsigned cached_revision_=~0u,cached_font_revision_=~0u;
    mutable uint64 cached_layout_generation_=~uint64(0);
    mutable std::array<int,4> cached_clip_{};
    mutable std::vector<SourcePort::UIQuad> cached_;
    void plat_initialize()override;
    void plat_update_hidden()override;
    void plat_update_engine()override;
    void plat_update_priority()override;
    void plat_update_window()override;
};
class CNativeSprite final:public CSprite {
public:
    explicit CNativeSprite(CWindow2D* window);~CNativeSprite()override;
    void Gather(std::vector<SourcePort::UIQuad>&)const;
private:
    unsigned revision_=0;
    mutable unsigned cached_revision_=~0u;
    mutable uint64 cached_texture_revision_=~uint64(0);
    mutable SourcePort::UIQuad cached_;
    void plat_initialize()override;
    void plat_update_hidden()override;
    void plat_update_engine()override;
    void plat_update_priority()override;
    void plat_update_window()override;
};
}
