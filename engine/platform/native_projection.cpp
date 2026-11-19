#include <source_port/native_projection.h>
#include <source_port/graphics.h>
#include <source_port/thread_os.h>
#include <source_port/memory.h>
#include <gfx/nx.h>
#include <algorithm>
namespace {
struct Detail {Nx::CNativeTexture* texture;Nx::CModel* model;Nx::CScene* scene;SourcePort::ProjectionCamera camera;bool camera_set=false;};
std::vector<Detail> projections;
struct Vertex {SourceRenderVertex world;float u,v,depth;};
float plane(const Vertex& p,unsigned i){switch(i){case 0:return p.u;case 1:return 1-p.u;case 2:return p.v;case 3:return 1-p.v;case 4:return p.depth-1;default:return 128-p.depth;}}
Vertex mix(const Vertex& a,const Vertex& b,float f){Vertex r=a;r.world.x+=(b.world.x-a.world.x)*f;r.world.y+=(b.world.y-a.world.y)*f;r.world.z+=(b.world.z-a.world.z)*f;
    r.world.u+=(b.world.u-a.world.u)*f;r.world.v+=(b.world.v-a.world.v)*f;r.u+=(b.u-a.u)*f;r.v+=(b.v-a.v)*f;r.depth+=(b.depth-a.depth)*f;return r;}
struct Polygon {
    // A convex triangle gains at most one vertex per clipping plane: 3+6.
    std::array<Vertex,12> vertices{};unsigned count=0;
    void push_back(const Vertex& v){Dbg_Assert(count<vertices.size());vertices[count++]=v;}
    unsigned size()const{return count;}
    Vertex& operator[](unsigned i){return vertices[i];}
};
Polygon clipped(const SourceRenderVertex* triangle,const float* view){
    Polygon a,b;Polygon* polygon=&a;Polygon* output=&b;
    for(unsigned i=0;i<3;++i){const auto& p=triangle[i];float local[3];for(unsigned r=0;r<3;++r)local[r]=view[r*4]*p.x+view[r*4+1]*p.y+view[r*4+2]*p.z+view[r*4+3];polygon->push_back({p,.5f+local[0]/96.f,.5f-local[1]/96.f,-local[2]});}
    for(unsigned p=0;p<6&&polygon->count;++p){output->count=0;auto a=(*polygon)[polygon->count-1];float da=plane(a,p);
        for(unsigned i=0;i<polygon->count;++i){const auto& b=(*polygon)[i];float db=plane(b,p);if((da>=0)!=(db>=0))output->push_back(mix(a,b,da/(da-db)));if(db>=0)output->push_back(b);a=b;da=db;}std::swap(polygon,output);
    }return *polygon;
}
float edge(float ax,float ay,float bx,float by,float x,float y){return (x-ax)*(by-ay)-(y-ay)*(bx-ax);}
}
namespace SourcePort {
ProjectionCamera OriginalProjectionCamera(const Mth::Vector& position,const Mth::Vector& forward){
    Dbg_Assert(forward.LengthSqr()>1e-12f);ProjectionCamera camera;camera.position=position;auto back=-forward;back.Normalize();bool vertical=forward[X]==0&&forward[Z]==0;auto up=Mth::Vector(0,vertical?0:1,vertical?1:0,0);
    auto right=Mth::CrossProduct(up,back);right.Normalize();up=Mth::CrossProduct(back,right);camera.axes[X]=right;camera.axes[Y]=up;camera.axes[Z]=back;camera.axes[3].Set(0,0,0,1);return camera;
}
bool RasterizeOriginalShadow(const ProjectionCamera& camera,unsigned width,unsigned height,unsigned z_depth,const std::vector<WorldBatch>& source,std::vector<uint8>& out){
    if(!width||!height||width>1024||height>1024||(z_depth!=0&&z_depth!=16&&z_depth!=24))return false;
    std::vector<uint8> rgba(std::size_t(width)*height*4,255);std::vector<float> depths(z_depth?std::size_t(width)*height:0,128);
    float view[16];CameraView(camera.axes,camera.position,view);
    for(const auto& batch:source){if(batch.pass)continue;const std::vector<uint8>* texture=nullptr;if(batch.texture&&batch.cutoff&&!batch.texture->Pixels(&texture))return false;
        for(std::size_t t=0;t+2<batch.vertices.size();t+=3){auto polygon=clipped(&batch.vertices[t],view);
            for(std::size_t k=1;k+1<polygon.size();++k){Vertex p[3]={polygon[0],polygon[k],polygon[k+1]};float x[3],y[3];for(unsigned i=0;i<3;++i){x[i]=p[i].u*width;y[i]=p[i].v*height;}
                float area=edge(x[0],y[0],x[1],y[1],x[2],y[2]);if(std::abs(area)<1e-8f)continue;
                int x0=std::max(0,int(std::floor(std::min({x[0],x[1],x[2]})))),x1=std::min(int(width)-1,int(std::ceil(std::max({x[0],x[1],x[2]}))));
                int y0=std::max(0,int(std::floor(std::min({y[0],y[1],y[2]})))),y1=std::min(int(height)-1,int(std::ceil(std::max({y[0],y[1],y[2]}))));
                for(int yy=y0;yy<=y1;++yy)for(int xx=x0;xx<=x1;++xx){float a=edge(x[1],y[1],x[2],y[2],xx+.5f,yy+.5f)/area,b=edge(x[2],y[2],x[0],y[0],xx+.5f,yy+.5f)/area,c=1-a-b;if(a<0||b<0||c<0)continue;
                    auto at=std::size_t(yy)*width+xx;float z=a*p[0].depth+b*p[1].depth+c*p[2].depth;if(z_depth&&z>depths[at])continue;
                    if(texture&&batch.cutoff){float u=a*p[0].world.u+b*p[1].world.u+c*p[2].world.u,v=a*p[0].world.v+b*p[1].world.v+c*p[2].world.v;const auto& asset=batch.texture->Asset();
                        auto coord=[](float value,unsigned size,unsigned mode){if(mode==0)value-=std::floor(value);else value=std::clamp(value,0.f,1.f);return std::min(unsigned(value*size),size-1);};
                        auto sx=coord(u,asset.width,batch.u_address),sy=coord(v,asset.height,batch.v_address);if((*texture)[(std::size_t(sy)*asset.width+sx)*4+3]<=batch.cutoff)continue;}
                    if(z_depth)depths[at]=z;rgba[at*4]=rgba[at*4+1]=rgba[at*4+2]=0;
                }
            }
        }
    }out.swap(rgba);return true;
}
bool ProjectionIntersectsBounds(const ProjectionCamera& camera,const Mth::CBBox& box){
    float view[16];CameraView(camera.axes,camera.position,view);float maximum[6];std::fill_n(maximum,6,-INFINITY);
    for(unsigned corner=0;corner<8;++corner){float world[3],local[3];for(unsigned c=0;c<3;++c)world[c]=(corner&(1u<<c))?box.GetMax()[c]:box.GetMin()[c];
        for(unsigned r=0;r<3;++r)local[r]=view[r*4]*world[0]+view[r*4+1]*world[1]+view[r*4+2]*world[2]+view[r*4+3];
        Vertex p{};p.u=.5f+local[0]/96.f;p.v=.5f-local[1]/96.f;p.depth=-local[2];
        for(unsigned i=0;i<6;++i){float d=plane(p,i);Dbg_Assert(std::isfinite(d));maximum[i]=std::max(maximum[i],d);}
    }
    // Reject only when the COMPLETE world box is outside one projector
    // half-space. Keep touching boxes and a float-error margin. Posed meshes
    // and billboards use the unchanged triangle clip path instead.
    for(float d:maximum)if(d<-.001f)return false;return true;
}
void ProjectOriginalShadow(const ProjectionCamera& camera,Nx::CNativeTexture* texture,const std::vector<WorldBatch>& source,std::vector<WorldBatch>& out){
    float view[16];CameraView(camera.axes,camera.position,view);
    for(const auto& batch:source){if(batch.pass||batch.no_skater_shadow||batch.sky)continue;WorldBatch shadow;shadow.texture=texture;shadow.blend=15;shadow.opaque=false;shadow.depth_write=false;shadow.z_bias=10;shadow.u_address=shadow.v_address=2;shadow.fog_disabled=true;
        for(std::size_t t=0;t+2<batch.vertices.size();t+=3){auto polygon=clipped(&batch.vertices[t],view);for(std::size_t k=1;k+1<polygon.size();++k)for(auto* p:{&polygon[0],&polygon[k],&polygon[k+1]}){auto v=p->world;v.u=p->u;v.v=p->v;v.r=v.g=v.b=128;v.a=255;shadow.vertices.push_back(v);}}
        if(!shadow.vertices.empty())out.push_back(std::move(shadow));
    }
}
void RenderOriginalShadowTargets(uint32 time){
    Dbg_Assert(source_thread_is_controller());for(auto& detail:projections){if(!detail.camera_set||!detail.model||!detail.model->GetActive())continue;std::vector<WorldBatch> posed;WorldView unused;unused.axes.Ident();
        for(int i=0;i<detail.model->GetNumGeoms();++i){auto* geom=dynamic_cast<Nx::CNativeGeom*>(detail.model->GetGeomByIndex(i));Dbg_Assert(geom);if(geom->IsActive()&&geom->CastsShadow())geom->Gather(unused,time,posed,true,true);}
        std::vector<uint8> rgba;Dbg_Assert(RasterizeOriginalShadow(detail.camera,detail.texture->GetWidth(),detail.texture->GetHeight(),detail.texture->TargetDepth(),posed,rgba));Dbg_Assert(detail.texture->WriteRenderTarget(rgba));
    }
}
void GatherOriginalProjectedShadows(const WorldView& view,uint32 time,std::vector<WorldBatch>& out){
    for(const auto& detail:projections)if(detail.camera_set&&detail.model&&detail.model->GetActive())ProjectOriginalSceneShadow(detail.scene,view,time,detail.camera,detail.texture,out);
}
void ProjectOriginalSceneShadow(Nx::CScene* scene,const WorldView& view,uint32 time,const ProjectionCamera& camera,Nx::CNativeTexture* texture,std::vector<WorldBatch>& out){
    if(!scene||scene->IsSky())return;auto* sectors=scene->GetSectorList();sectors->IterateStart();
    while(auto* sector=sectors->IterateNext())if(auto* geom=dynamic_cast<Nx::CNativeGeom*>(sector->GetGeom()))if(geom->IsActive()){
        Mth::CBBox bounds;if(geom->StaticWorldBounds(bounds)&&!ProjectionIntersectsBounds(camera,bounds))continue;
        // Project in the same original sector/mesh order, releasing each
        // receiver before the next. The 96x96 projector must not retain an
        // expanded triangle copy of the entire city alongside the main view.
        std::vector<WorldBatch> receivers;geom->Gather(view,time,receivers,true,true,true);ProjectOriginalShadow(camera,texture,receivers,out);
    }
}
void ForgetProjectionTexture(Nx::CNativeTexture* texture){projections.erase(std::remove_if(projections.begin(),projections.end(),[&](const auto& d){return d.texture==texture;}),projections.end());}
void ForgetProjectionModel(Nx::CModel* model){projections.erase(std::remove_if(projections.begin(),projections.end(),[&](const auto& d){return d.model==model;}),projections.end());}
void ForgetProjectionScene(Nx::CScene* scene){for(auto& d:projections)if(d.scene==scene)d.scene=nullptr;}
void ReleaseNativeProjections(){Dbg_Assert(source_thread_is_controller());decltype(projections)().swap(projections);}
}
namespace Nx {
CTexture* CEngine::s_plat_create_render_target_texture(int width,int height,int depth,int z_depth){auto texture=std::make_unique<CNativeTexture>();if(!texture->CreateRenderTarget(width,height,depth,z_depth))return nullptr;return texture.release();}
void CEngine::s_plat_project_texture_into_scene(CTexture* texture,CModel* model,CScene* scene){
    auto* target=dynamic_cast<CNativeTexture*>(texture);Dbg_Assert(target&&target->IsRenderTarget()&&model);if(!scene)for(auto* loaded:sp_loaded_scenes)if(loaded&&!loaded->IsSky()){scene=loaded;break;}
    SourcePort::RootHeapScope registry;SourcePort::ForgetProjectionTexture(target);projections.push_back({target,model,scene,{},false});
}
void CEngine::s_plat_set_projection_texture_camera(CTexture* texture,Gfx::Camera* camera){Dbg_Assert(camera);for(auto& d:projections)if(d.texture==texture){d.camera=SourcePort::OriginalProjectionCamera(camera->GetPos(),camera->GetMatrix()[Z]);d.camera_set=true;}}
void CEngine::s_plat_stop_projection_texture(CTexture* texture){auto* native=dynamic_cast<CNativeTexture*>(texture);Dbg_Assert(native);SourcePort::ForgetProjectionTexture(native);}
}
