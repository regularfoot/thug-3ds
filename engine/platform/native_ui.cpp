#include <source_port/native_ui.h>
#include <source_port/thread_os.h>
#include <source_port/memory.h>
#include <gfx/nx.h>
#include <algorithm>
#include <map>
#include <cmath>
namespace {
std::vector<Nx::CNativeText*> texts,text_pool;
std::vector<Nx::CNativeSprite*> sprites;
std::vector<Nx::CWindow2D*> window_pool;
std::map<Nx::CWindow2D*,std::array<int,4>> clips;
bool constant_z=false;Nx::ZBufferValue sprite_z=0;
void controller(){Dbg_Assert(source_thread_is_controller());}
template<class T>void remove(std::vector<T*>& list,T* value){auto at=std::find(list.begin(),list.end(),value);Dbg_Assert(at!=list.end());list.erase(at);}
std::array<int,4> clip(Nx::CWindow2D* window){
    if(!window)return {0,0,640,448};auto at=clips.find(window);if(at!=clips.end())return at->second;
    return {window->GetXPos(),window->GetYPos(),window->GetWidth(),window->GetHeight()};
}
SourceRenderVertex vertex(float x,float y,float z,float u,float v,Image::RGBA color){
    // Original MODULATE2X saturates AFTER texture multiplication. Preserve
    // source colors here; the PICA texture combiner owns the factor of two.
    return {x,y,z,color.r,color.g,color.b,color.a,u,v};
}
void quad(std::array<SourceRenderVertex,6>& out,const SourceRenderVertex& a,const SourceRenderVertex& b,const SourceRenderVertex& c,const SourceRenderVertex& d){out={a,b,c,a,c,d};}
}
namespace SourcePort {
void ForgetNativeClip(Nx::CWindow2D* window){controller();clips.erase(window);}
void GatherNativeUI(std::vector<UIQuad>& out){
    controller();for(auto* text:texts)text->Gather(out);for(auto* sprite:sprites)sprite->Gather(out);
    std::stable_sort(out.begin(),out.end(),[](const UIQuad& a,const UIQuad& b){return a.priority<b.priority;});
}
void ReleaseNativeUI(){
    controller();Nx::CTextMan::sReleaseNativePool();Nx::CWindow2DManager::sReleaseNativePool();
    Dbg_MsgAssert(texts.empty()&&sprites.empty()&&clips.empty(),("Original UI objects still live at native renderer teardown"));
    decltype(texts)().swap(texts);decltype(sprites)().swap(sprites);decltype(clips)().swap(clips);
}
}
namespace Nx {
CNativeText::CNativeText(){controller();m_zvalue=0;SourcePort::RootHeapScope registry;texts.push_back(this);plat_initialize();}
CNativeText::~CNativeText(){controller();remove(texts,this);}
void CNativeText::plat_initialize(){++revision_;}
void CNativeText::plat_update_hidden(){++revision_;}
void CNativeText::plat_update_engine(){++revision_;}
void CNativeText::plat_update_priority(){++revision_;}
void CNativeText::plat_update_window(){++revision_;}
void CNativeText::Gather(std::vector<SourcePort::UIQuad>& out)const{
    if(m_hidden||!mp_font||!m_string||!*m_string)return;auto* font=dynamic_cast<CNativeFont*>(mp_font);Dbg_Assert(font);
    auto current_clip=clip(mp_window);
    auto layout_generation=SourcePort::FontLayoutGeneration();
    if(cached_revision_==revision_&&cached_font_revision_==font->Revision()&&cached_layout_generation_==layout_generation&&cached_clip_==current_clip){out.insert(out.end(),cached_.begin(),cached_.end());return;}
    cached_.clear();
    std::vector<CNativeFont::Glyph> glyphs;float width;
    bool ok=font->Layout(m_string,m_xpos,m_ypos,m_xscale,m_yscale,m_rgba,m_color_override,&glyphs,width);Dbg_Assert(ok);
    for(const auto& g:glyphs){SourcePort::UIQuad item;item.texture=g.texture;item.priority=m_priority;item.depth=m_use_zbuffer;item.z=m_zvalue;item.clip=current_clip;
        float z=m_use_zbuffer?float(m_zvalue)/65536.f:0;
        quad(item.vertices,vertex(g.x,g.y,z,g.u0,g.v0,g.color),vertex(g.x,g.y+g.height,z,g.u0,g.v1,g.color),
            vertex(g.x+g.width,g.y+g.height,z,g.u1,g.v1,g.color),vertex(g.x+g.width,g.y,z,g.u1,g.v0,g.color));cached_.push_back(std::move(item));}
    cached_revision_=revision_;cached_font_revision_=font->Revision();cached_layout_generation_=layout_generation;cached_clip_=current_clip;out.insert(out.end(),cached_.begin(),cached_.end());
}
void CTextMan::s_plat_alloc_text_pool(){
    SourcePort::RootHeapScope storage;
    controller();Dbg_Assert(text_pool.empty()&&!sp_dynamic_text_list);text_pool.reserve(vMAX_TEXT_INSTANCES);
    for(int i=0;i<vMAX_TEXT_INSTANCES;++i){auto* text=new CNativeText;text->mp_next=sp_dynamic_text_list;sp_dynamic_text_list=text;text_pool.push_back(text);}
}
void CTextMan::sReleaseNativePool(){controller();for(auto* text:text_pool)delete text;decltype(text_pool)().swap(text_pool);sp_dynamic_text_list=nullptr;}
void CWindow2D::plat_update_engine(){
    controller();Dbg_Assert(m_width>=0&&m_height>=0);SourcePort::RootHeapScope registry;clips[this]={m_xpos,m_ypos,m_width,m_height};
}
CWindow2D::CWindow2D(const Mth::Rect& r):CWindow2D(int(r.GetOriginX()),int(r.GetOriginY()),int(r.GetWidth()),int(r.GetHeight())){plat_update_engine();}
void CWindow2DManager::s_plat_alloc_window2d_pool(){
    SourcePort::RootHeapScope storage;
    controller();Dbg_Assert(window_pool.empty()&&!sp_window_list);window_pool.reserve(vMAX_WINDOW_INSTANCES);
    for(int i=0;i<vMAX_WINDOW_INSTANCES;++i){auto* window=new CWindow2D;window->mp_next=sp_window_list;sp_window_list=window;window_pool.push_back(window);}
}
void CWindow2DManager::sReleaseNativePool(){controller();for(auto* window:window_pool)delete window;decltype(window_pool)().swap(window_pool);sp_window_list=nullptr;}
CNativeSprite::CNativeSprite(CWindow2D* window):CSprite(window){controller();m_zvalue=0;SourcePort::RootHeapScope registry;sprites.push_back(this);plat_initialize();}
CNativeSprite::~CNativeSprite(){controller();remove(sprites,this);}
void CNativeSprite::plat_initialize(){++revision_;}
void CNativeSprite::plat_update_hidden(){++revision_;}
void CNativeSprite::plat_update_engine(){++revision_;}
void CNativeSprite::plat_update_priority(){++revision_;}
void CNativeSprite::plat_update_window(){++revision_;}
void CNativeSprite::Gather(std::vector<SourcePort::UIQuad>& out)const{
    if(m_hidden||!m_width||!m_height)return;auto current_clip=clip(mp_window);
    auto* texture=dynamic_cast<CNativeTexture*>(mp_texture);Dbg_Assert(!mp_texture||texture);auto texture_revision=texture?texture->Revision():0;
    if(cached_revision_==revision_&&cached_texture_revision_==texture_revision&&cached_.clip==current_clip&&cached_.depth==(m_use_zbuffer||constant_z)&&(!constant_z||cached_.z==sprite_z)){out.push_back(cached_);return;}
    SourcePort::UIQuad item;
    item.texture=dynamic_cast<CNativeTexture*>(mp_texture);Dbg_Assert(!mp_texture||item.texture);item.priority=m_priority;item.clip=clip(mp_window);
    item.depth=m_use_zbuffer||constant_z;item.z=m_use_zbuffer?m_zvalue:sprite_z;float z=item.depth?float(item.z)/65536.f:0;
    float w=m_width*m_scale_x,h=m_height*m_scale_y,left=-(m_anchor_x+1)*0.5f*w,top=-(m_anchor_y+1)*0.5f*h;
    float u=item.texture?float(m_width)/item.texture->Asset().width:1,v=item.texture?float(m_height)/item.texture->Asset().height:1;
    auto make=[&](float x,float y,float tu,float tv){float c=std::cos(m_rotation),s=std::sin(m_rotation);return vertex(m_pos_x+x*c-y*s,m_pos_y+x*s+y*c,z,tu,tv,m_rgba);};
    quad(item.vertices,make(left,top,0,0),make(left,top+h,0,v),make(left+w,top+h,u,v),make(left+w,top,u,0));cached_=item;cached_revision_=revision_;cached_texture_revision_=texture_revision;out.push_back(std::move(item));
}
void CSprite::plat_enable_constant_z_value(bool enable){controller();constant_z=enable;}
void CSprite::plat_set_constant_z_value(ZBufferValue z){controller();sprite_z=z;}
ZBufferValue CSprite::plat_get_constant_z_value(){controller();return sprite_z;}
CSprite* CEngine::s_plat_create_sprite(CWindow2D* window){return new CNativeSprite(window);}
bool CEngine::s_plat_destroy_sprite(CSprite* sprite){if(!sprite)return false;delete sprite;return true;}
}
