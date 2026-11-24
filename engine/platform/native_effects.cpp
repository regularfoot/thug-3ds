// Original Xbox/NGC MiscFX algorithms over native vertex/material storage.
// Original NxMiscFX owns tables, trail lifetime, pause, expiry and bounce math.
#include <source_port/native_effects.h>
#include <source_port/application_os.h>
#include <source_port/native_particles.h>
#include <source_port/native_renderer.h>
#include <source_port/thread_os.h>
#include <gfx/nxviewman.h>
#include <gfx/nxviewport.h>
#include <sys/timer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace Nx {extern Lst::HashTable<sScreenFlashDetails>* p_screen_flash_details_table;}
namespace {
void controller(){Dbg_Assert(source_thread_is_controller());}
uint32 key(const void* p){return uint32(uintptr_t(p));} // original 32-bit identity hash, never an address to dereference
Mth::Vector point(const SourceRenderVertex& v){return Mth::Vector(v.x,v.y,v.z,1);}
float area2(const SourceRenderVertex* v){return Mth::CrossProduct(point(v[1])-point(v[0]),point(v[2])-point(v[0])).LengthSqr();}
SourceRenderVertex midpoint(const SourceRenderVertex& a,const SourceRenderVertex& b){
    auto half=[](uint8 a,uint8 b){return uint8(int(a)+(int(b)-int(a))/2);};
    return {a.x+(b.x-a.x)*.5f,a.y+(b.y-a.y)*.5f,a.z+(b.z-a.z)*.5f,half(a.r,b.r),half(a.g,b.g),half(a.b,b.b),half(a.a,b.a),a.u+(b.u-a.u)*.5f,a.v+(b.v-a.v)*.5f};
}
struct NativeShards final:Nx::sShatterInstanceDetails{
    explicit NativeShards(std::vector<SourcePort::WorldBatch> data):sShatterInstanceDetails(data[0].vertices.size()/3),batches(std::move(data)){}
    std::vector<SourcePort::WorldBatch> batches;
};
struct Splat final:Nx::sSplatInstanceDetails{
    explicit Splat(Nx::CNativeTexture* tex):texture(tex){std::fill(std::begin(m_lifetimes),std::end(m_lifetimes),0);m_highest_active_splat=-1;}
    Nx::CNativeTexture* texture;
    std::array<SourceRenderVertex,SPLAT_POLYS_PER_MESH*3> vertices{};
};
SourcePort::WorldView actual_view(){
    SourcePort::WorldView view;view.axes.Ident();view.position.Set(0,0,0,1);
    if(Nx::CViewportManager::sGetNumActiveViewports())if(auto* port=Nx::CViewportManager::sGetActiveViewport(0))if(auto* camera=port->GetCamera()){
        view.axes=camera->GetMatrix();view.position=camera->GetPos();view.hfov=camera->GetAdjustedHFOV();view.aspect=port->GetAspectRatio();view.near_clip=camera->GetNearClipPlane();view.far_clip=camera->GetFarClipPlane();}
    return view;
}
Splat* splat_for(Nx::CNativeTexture* texture){
    Dbg_Assert(Nx::p_splat_details_table);Nx::p_splat_details_table->IterateStart();while(auto* base=Nx::p_splat_details_table->IterateNext()){
        auto* s=static_cast<Splat*>(base);if(s->texture==texture)return s;}
    auto* s=new Splat(texture);Nx::p_splat_details_table->PutItem(key(s),s);return s;
}
bool shatter_open=false,splat_open=false;
void shade(SourceRenderVertex& v,const SourcePort::WorldBatch::ShardLighting& s,const SourcePort::LightingFrame& frame){
    auto light=SourcePort::LightNormal(frame,s.normal);auto channel=[&](unsigned c){float value=s.unlit[c]*light[c];Dbg_Assert(!std::isnan(value));return uint8(std::clamp(value,0.f,255.f));};v.r=channel(0);v.g=channel(1);v.b=channel(2);
}
}
namespace SourcePort {
void SubdivideEffectBatches(std::vector<WorldBatch>& batches,float limit){
    controller();Dbg_Assert(!batches.empty()&&batches.size()<=4&&std::isfinite(limit)&&limit>0&&Nx::triSubdivideStack.IsEmpty());
    struct Vertex {SourceRenderVertex pass[4];std::array<float,3> color[4];Mth::Vector normal;};
    auto count=batches[0].vertices.size();for(const auto& b:batches)Dbg_Assert(b.vertices.size()==count);Dbg_Assert(count%3==0);
    std::vector<std::vector<SourceRenderVertex>> output(batches.size());std::vector<std::vector<WorldBatch::ShardLighting>> output_lighting(batches.size());int lit=-1;
    for(unsigned p=0;p<batches.size();++p)if(!batches[p].shard_lighting.empty()){Dbg_Assert(batches[p].shard_lights&&batches[p].shard_lighting.size()==count);lit=p;}
    auto& stack=Nx::triSubdivideStack;stack.SetBlockSize(sizeof(Vertex));
    // Feed one source triangle at a time in reverse order: same LIFO order as
    // the console, without overflowing its 16KiB stack on a large mesh.
    for(std::size_t end=count;end;end-=3){Vertex tri[3]{};
        for(unsigned v=0;v<3;++v){tri[v].normal.Set(0,0,0,0);if(lit>=0)tri[v].normal=batches[lit].shard_lighting[end-3+v].normal;
            for(unsigned p=0;p<batches.size();++p){tri[v].pass[p]=batches[p].vertices[end-3+v];if(!batches[p].shard_lighting.empty())tri[v].color[p]=batches[p].shard_lighting[end-3+v].unlit;}stack.Push(&tri[v]);}
        while(!stack.IsEmpty()){
            for(int v=2;v>=0;--v)stack.Pop(&tri[v]);SourceRenderVertex first[3]={tri[0].pass[0],tri[1].pass[0],tri[2].pass[0]};
            float area=area2(first);Dbg_Assert(std::isfinite(area));
            if(area>limit){Vertex mid[3]{};for(unsigned v=0;v<3;++v){mid[v].normal=(tri[v].normal+tri[(v+1)%3].normal)*.5f;
                    for(unsigned p=0;p<batches.size();++p){mid[v].pass[p]=midpoint(tri[v].pass[p],tri[(v+1)%3].pass[p]);for(unsigned c=0;c<3;++c)mid[v].color[p][c]=tri[v].color[p][c]+(tri[(v+1)%3].color[p][c]-tri[v].color[p][c])*.5f;}}
                const Vertex* children[]={&tri[0],&mid[0],&mid[2], &mid[0],&tri[1],&mid[1], &mid[0],&mid[1],&mid[2], &mid[2],&mid[1],&tri[2]};
                for(auto* vertex:children)stack.Push(const_cast<Vertex*>(vertex));
            }else{
                Dbg_MsgAssert(output[0].size()<3*1024*1024,("Original effect subdivision exceeds native vertex capacity"));
                for(unsigned p=0;p<batches.size();++p)for(const auto& v:tri){auto vertex=v.pass[p];if(!batches[p].shard_lighting.empty()){
                        WorldBatch::ShardLighting s{v.normal,v.color[p]};output_lighting[p].push_back(s);shade(vertex,s,*batches[p].shard_lights);}output[p].push_back(vertex);}
            }
        }
    }
    for(unsigned p=0;p<batches.size();++p){batches[p].vertices.swap(output[p]);batches[p].shard_lighting.swap(output_lighting[p]);}
}
bool EffectTriangleIntersectsTexture(const SourceRenderVertex* v){
    // Same triangle/square overlap as original tri_texture_intersect, expressed
    // as separating axes to handle parallel/degenerate edges without division.
    float x[3],y[3];for(unsigned i=0;i<3;++i){x[i]=v[i].u*2-1;y[i]=v[i].v*2-1;}
    for(unsigned axis=0;axis<5;++axis){float nx=axis==0?1:0,ny=axis==1?1:0;
        if(axis>=2){unsigned i=axis-2,j=(i+1)%3;nx=y[j]-y[i];ny=x[i]-x[j];}
        float a=nx*x[0]+ny*y[0],lo=a,hi=a;for(unsigned i=1;i<3;++i){a=nx*x[i]+ny*y[i];lo=std::min(lo,a);hi=std::max(hi,a);}
        float radius=std::abs(nx)+std::abs(ny);if(lo>radius||hi< -radius)return false;
    }return true;
}
void ReleaseOriginalEffectTables(){
    controller();Nx::MiscFXCleanup();delete Nx::p_splat_details_table;Nx::p_splat_details_table=nullptr;
    delete Nx::p_splat_trail_details_table;Nx::p_splat_trail_details_table=nullptr;delete Nx::p_shatter_details_table;Nx::p_shatter_details_table=nullptr;
    delete Nx::p_screen_flash_details_table;Nx::p_screen_flash_details_table=nullptr;shatter_open=splat_open=false;
}
}
namespace Nx {
void plat_shatter_initialize(){controller();Dbg_Assert(p_shatter_details_table);shatter_open=true;}
void plat_texture_splat_initialize(){controller();Dbg_Assert(p_splat_details_table);splat_open=true;}
void plat_shatter_cleanup(){
    controller();Dbg_Assert(p_shatter_details_table);p_shatter_details_table->IterateStart();auto* item=p_shatter_details_table->IterateNext();
    // Original MiscFXCleanup also runs on level/replay resets. Its table is
    // reusable; only ReleaseOriginalEffectTables ends native ownership.
    while(item){auto* next=p_shatter_details_table->IterateNext();p_shatter_details_table->FlushItem(key(item));delete item;item=next;}
}
void plat_texture_splat_cleanup(){
    controller();Dbg_Assert(p_splat_details_table);p_splat_details_table->IterateStart();auto* item=p_splat_details_table->IterateNext();
    while(item){auto* next=p_splat_details_table->IterateNext();p_splat_details_table->FlushItem(key(item));delete static_cast<Splat*>(item);item=next;}splat_open=false;
}
void plat_shatter(CGeom* geom){
    if(source_application_os_is_old_3ds())return;
    controller();Dbg_Assert(shatter_open&&p_shatter_details_table);auto* native=dynamic_cast<CNativeGeom*>(geom);Dbg_Assert(native);
    std::vector<SourcePort::WorldBatch> geometry;native->Gather(actual_view(),Tmr::GetTime(),geometry,true);
    SourcePort::ShatterGeometry(std::move(geometry));
}
}
namespace SourcePort {
void ShatterGeometry(std::vector<WorldBatch> geometry){
    using namespace Nx;controller();Dbg_Assert(shatter_open&&p_shatter_details_table);
    for(std::size_t start=0;start<geometry.size();){std::size_t end=start+1;while(end<geometry.size()&&geometry[end].group==geometry[start].group)++end;
        std::vector<SourcePort::WorldBatch> group(geometry.begin()+start,geometry.begin()+end);SourcePort::SubdivideEffectBatches(group,shatterAreaTest);
        auto* details=new NativeShards(std::move(group));const auto& verts=details->batches[0].vertices;Mth::CBBox box;box.Reset();for(const auto& v:verts)box.AddPoint(point(v));
        auto center=(box.GetMin()+box.GetMax())*.5f;auto spread=center-shatterVelocity*shatterSpreadFactor;float speed=shatterVelocity.Length();
        for(int i=0;i<details->m_num_triangles;++i){details->mp_positions[i]=(point(verts[i*3])+point(verts[i*3+1])+point(verts[i*3+2]))/3;
            details->mp_velocities[i]=(details->mp_positions[i]-spread).Normalize(speed+speed*shatterVelocityVariance*(float(rand())/RAND_MAX));
            Mth::Vector axis(-1+2.f*rand()/RAND_MAX,-1+2.f*rand()/RAND_MAX,-1+2.f*rand()/RAND_MAX);axis.Normalize();details->mp_matrices[i].Ident();details->mp_matrices[i].Rotate(axis,.1f*rand()/RAND_MAX);
        }p_shatter_details_table->PutItem(key(details),details);start=end;
    }
}
}
namespace Nx {
void plat_shatter_update(sShatterInstanceDetails* base,float dt){
    controller();Dbg_Assert(base&&std::isfinite(dt)&&dt>=0);auto* d=static_cast<NativeShards*>(base);
    for(int i=0;i<d->m_num_triangles;++i){auto previous=d->mp_positions[i];auto rotation=d->mp_matrices[i];d->UpdateParameters(i,dt);
        for(auto& batch:d->batches)for(unsigned k=0;k<3;++k){auto index=i*3+k;auto& vertex=batch.vertices[index];auto p=point(vertex)-previous;p.Rotate(rotation);p+=d->mp_positions[i];vertex.x=p[X];vertex.y=p[Y];vertex.z=p[Z];
            if(!batch.shard_lighting.empty()){auto& lighting=batch.shard_lighting[index];lighting.normal.Rotate(rotation);shade(vertex,lighting,*batch.shard_lights);}}
    }
}
void plat_shatter_render(sShatterInstanceDetails* base){controller();auto* d=static_cast<NativeShards*>(base);for(const auto& b:d->batches)SourcePort::EffectRenderScope::Output().push_back(b);}
void plat_texture_splat_reset_poly(sSplatInstanceDetails* base,int index){
    controller();Dbg_Assert(index>=0&&index<SPLAT_POLYS_PER_MESH);auto* d=static_cast<Splat*>(base);d->vertices[index*3+1]=d->vertices[index*3+2]=d->vertices[index*3];
}
}
namespace SourcePort {
bool TextureSplatOnGeometry(const std::vector<WorldBatch>& batches,Mth::Vector& start,Mth::Vector& end,float size,float lifetime,Nx::CTexture* texture,Nx::sSplatTrailInstanceDetails* trail){
    using namespace Nx;
    controller();Dbg_Assert(p_splat_details_table&&std::isfinite(size)&&size>0&&std::isfinite(lifetime)&&lifetime>=0&&lifetime<std::numeric_limits<int>::max()/1000.f);
    // KillAllTextureSplats is used on scene transitions; creation remains valid
    // afterwards, exactly as with the source table's reusable allocation.
    splat_open=true;auto* native_texture=dynamic_cast<CNativeTexture*>(texture);Dbg_Assert(!texture||native_texture);
    auto direction=end-start;float depth=direction.Length();if(depth<=.1f||lifetime==0)return false;direction/=depth;Mth::Vector up;float height=size;
    if(trail){up=start-trail->m_last_pos;height=up.Length()*.5f;if(height<=1e-6f)return false;start-=up*.5f;end-=up*.5f;up.Normalize();}
    else if(std::abs(direction[Y])>.5f){float angle=float(rand())*2.f*Mth::PI/RAND_MAX;up.Set(std::sin(angle),0,std::cos(angle),0);}else up.Set(0,1,0,0);
    auto right=Mth::CrossProduct(up,-direction);if(right.LengthSqr()<1e-12f)return false;right.Normalize();up=Mth::CrossProduct(-direction,right);bool added=false;Splat* details=nullptr;
        for(const auto& batch:batches){if(batch.pass||batch.no_skater_shadow)continue;
            for(std::size_t i=0;i<batch.vertices.size();i+=3){SourcePort::WorldBatch projection;projection.vertices.assign(batch.vertices.begin()+i,batch.vertices.begin()+i+3);float z[3];
                for(unsigned v=0;v<3;++v){auto delta=point(projection.vertices[v])-start;z[v]=(Mth::DotProduct(delta,direction)-.1f)/(depth-.1f);projection.vertices[v].u=Mth::DotProduct(delta,right)/size+.5f;projection.vertices[v].v=Mth::DotProduct(delta,up)/height+.5f;projection.vertices[v].a=128;}
                if((*std::max_element(z,z+3)<0)||(*std::min_element(z,z+3)>1)||!SourcePort::EffectTriangleIntersectsTexture(projection.vertices.data()))continue;
                float area=area2(projection.vertices.data());if(area<=1e-12f)continue;std::vector<SourcePort::WorldBatch> sub;sub.push_back(std::move(projection));SourcePort::SubdivideEffectBatches(sub,area/128.f);
                for(std::size_t n=0;n<sub[0].vertices.size();n+=3)if(SourcePort::EffectTriangleIntersectsTexture(sub[0].vertices.data()+n)){
                    if(!details)details=splat_for(native_texture);int index=details->GetOldestSplat();details->m_lifetimes[index]=int(lifetime*1000);details->m_highest_active_splat=std::max(details->m_highest_active_splat,index);
                    std::copy_n(sub[0].vertices.data()+n,3,details->vertices.data()+index*3);added=true;
                }
            }
        }
    return added;
}
}
namespace Nx {
bool plat_texture_splat(CSector** sectors,CCollStatic**,Mth::Vector& start,Mth::Vector& end,float size,float lifetime,CTexture* texture,sSplatTrailInstanceDetails* trail){
    if(source_application_os_is_old_3ds())return false;
    controller();Dbg_Assert(sectors);std::vector<SourcePort::WorldBatch> batches;auto view=actual_view();auto time=Tmr::GetTime();
    for(;*sectors;++sectors)if(auto* geom=dynamic_cast<CNativeGeom*>((*sectors)->GetGeom()))geom->Gather(view,time,batches,true,true);
    return SourcePort::TextureSplatOnGeometry(batches,start,end,size,lifetime,texture,trail);
}
void plat_texture_splat_render(){
    controller();if(!p_splat_details_table)return;p_splat_details_table->IterateStart();while(auto* base=p_splat_details_table->IterateNext()){
        auto* d=static_cast<Splat*>(base);SourcePort::WorldBatch b;b.texture=d->texture;b.blend=5;b.cutoff=1;b.u_address=b.v_address=2;b.z_bias=1;b.depth_write=false;
        for(int i=0;i<=d->m_highest_active_splat;++i)if(d->m_lifetimes[i]>0)b.vertices.insert(b.vertices.end(),d->vertices.begin()+i*3,d->vertices.begin()+i*3+3);
        if(!b.vertices.empty())SourcePort::EffectRenderScope::Output().push_back(std::move(b));
    }
}
}
