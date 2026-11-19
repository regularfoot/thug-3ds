#include <source_port/native_ui.h>
#include <source_port/graphics.h>
#include <source_port/thread_os.h>
#include <algorithm>
#include <limits>
namespace SourcePort {
namespace {
bool same_ui_state(const SourceDrawPacket& a,const SourceDrawPacket& b){
    if(a.orthographic!=1||b.orthographic!=1||a.index_range||b.index_range)return false;
#define SAME(k) (a.k==b.k)
    return SAME(mesh)&&SAME(texture)&&SAME(hfov)&&SAME(near_clip)&&SAME(far_clip)&&SAME(focal_distance)
        &&SAME(x)&&SAME(y)&&SAME(width)&&SAME(height)&&SAME(blend)&&SAME(depth_write)&&SAME(cull)&&SAME(alpha_cutoff)
        &&SAME(ui_depth)&&SAME(clip_enabled)&&SAME(clip_x)&&SAME(clip_y)&&SAME(clip_width)&&SAME(clip_height)
        &&SAME(modulate_2x)&&SAME(alpha_2x)&&SAME(original_blend)&&SAME(blend_mode)&&SAME(fixed_alpha)
        &&SAME(u_address)&&SAME(v_address)&&SAME(nearest)&&SAME(z_bias)&&SAME(aspect)
        &&SAME(fog_enabled)&&SAME(fog_alpha)&&SAME(fog_color)&&SAME(fog_start)
        &&std::equal(std::begin(a.model_view),std::end(a.model_view),std::begin(b.model_view));
#undef SAME
}
}
void CoalesceUIDrawPackets(std::vector<SourceDrawPacket>& packets){
    std::size_t written=0;
    for(std::size_t read=0;read<packets.size();++read){const auto& p=packets[read];
        if(written){auto& previous=packets[written-1];
            if(p.count&&previous.count&&p.first>=previous.first&&p.first-previous.first==previous.count
                &&p.count<=std::numeric_limits<unsigned>::max()-previous.count&&same_ui_state(previous,p)){
                previous.count+=p.count;continue;
            }
        }
        packets[written++]=p;
    }
    packets.resize(written);
}
NativeUIBatch::~NativeUIBatch(){Release();}
void NativeUIBatch::Release(){
    Dbg_Assert(source_thread_is_controller());if(mesh_){Dbg_Assert(source_graphics_mesh_delete(mesh_)==0);mesh_=nullptr;}decltype(vertices_)().swap(vertices_);
}
bool NativeUIBatch::Prepare(const std::vector<UIQuad>& quads,std::vector<SourceDrawPacket>& out){
    Dbg_Assert(source_thread_is_controller());if(!GraphicsReady()){errno=ENODEV;return false;}
    if(quads.size()>65536){errno=EOVERFLOW;return false;}
    std::vector<SourceDrawPacket> packets;std::vector<SourceRenderVertex> active;packets.reserve(quads.size());active.reserve(quads.size()*6);
    for(const auto& q:quads){
        int64 left=std::max<int64>(0,q.clip[0]),top=std::max<int64>(0,q.clip[1]);
        int64 right=std::min<int64>(640,int64(q.clip[0])+q.clip[2]),bottom=std::min<int64>(448,int64(q.clip[1])+q.clip[3]);if(right<=left||bottom<=top)continue;
        SourceDrawPacket p{};p.first=active.size();p.count=6;p.width=400;p.height=240;p.blend=SOURCE_BLEND_ALPHA;p.orthographic=1;p.ui_depth=q.depth;p.depth_write=q.depth;p.modulate_2x=1;p.alpha_2x=1;
        p.model_view[0]=p.model_view[5]=p.model_view[10]=p.model_view[15]=1;
        p.clip_enabled=1;p.clip_x=left*400/640;p.clip_y=top*240/448;
        unsigned end_x=(right*400+639)/640,end_y=(bottom*240+447)/448;p.clip_width=end_x-p.clip_x;p.clip_height=end_y-p.clip_y;
        if(q.texture){p.texture=q.texture->GPU();if(!p.texture)return false;}
        float us=q.texture?q.texture->UScale():1;
        for(auto v:q.vertices){v.u*=us;if(q.texture)v.v=q.texture->GPUV(v.v);active.push_back(v);}packets.push_back(p);
    }
    if(active.empty()){out.clear();return true;}
    CoalesceUIDrawPackets(packets);
    if(active.size()>vertices_.size()){
        std::size_t capacity=6;while(capacity<active.size())capacity*=2;
        std::vector<SourceRenderVertex> replacement(capacity);std::copy(active.begin(),active.end(),replacement.begin());
        // Every mutation is fenced inside the GPU bridge. Never update a
        // live UI buffer between the two eyes of one rendered frame.
        auto* fresh=source_graphics_mesh_create(replacement.data(),replacement.size());if(!fresh)return false;
        if(mesh_)Dbg_Assert(source_graphics_mesh_delete(mesh_)==0);mesh_=fresh;vertices_.swap(replacement);
    }else{
        std::copy(active.begin(),active.end(),vertices_.begin());if(source_graphics_mesh_update(mesh_,vertices_.data(),vertices_.size()))return false;
    }
    for(auto& p:packets)p.mesh=mesh_;out.swap(packets);return true;
}
}
