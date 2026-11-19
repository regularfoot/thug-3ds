#include <source_port/mesh_assets.h>
#include <source_port/thread_os.h>
#include <gfx/nx.h>
#include <string>
#include <algorithm>
#include <cctype>
namespace {
bool scaling_enabled=false;Nx::SMeshScalingParameters scaling{};
SourcePort::EmbeddedCASResource* embedded_cas=nullptr;
void apply_scaling(SourcePort::SceneAsset& scene,const Nx::SMeshScalingParameters* params){
    if(!params)return;const auto& scaling=*params;
    for(auto& sector:scene.sectors)for(std::size_t i=0;i<sector.vertices.size();++i){auto& p=sector.vertices[i].position;Mth::Vector original(p[0],p[1],p[2],1),scaled(0,0,0,0);
        // Same source head-scaling calculation as DX9 nx/mesh.cpp, including
        // the skeleton's 29..33 head palette and three source float weights.
        for(unsigned j=0;j<3;++j){int bone=static_cast<signed char>(scaling.pWeightIndices[i*3+j]);Mth::Vector origin(0,0,0,1),scale(1,1,1,1);
            if(bone>=29&&bone<=33){origin=scaling.pBonePositions[bone-29];scale=scaling.pBoneScales[bone-29];}
            auto local=original-origin;local.Scale(scale);local+=origin;scaled+=local*scaling.pWeights[i*3+j];}
        for(unsigned c=0;c<3;++c){Dbg_Assert(std::isfinite(scaled[c]));p[c]=scaled[c];}
    }
}
}
namespace SourcePort {
EmbeddedCASResource::EmbeddedCASResource(const void* p,std::size_t size):pointer_(p),size_(size),previous_(embedded_cas){
    Dbg_Assert(source_thread_is_controller()&&size<=128*1024*1024&&((p&&size)||(!p&&!size)));embedded_cas=this;
}
EmbeddedCASResource::~EmbeddedCASResource(){Dbg_Assert(source_thread_is_controller()&&embedded_cas==this);embedded_cas=previous_;}
bool ReadEmbeddedCAS(const void* p,CASAsset& out){
    Dbg_Assert(source_thread_is_controller());for(auto* owner=embedded_cas;owner;owner=owner->previous_)if(owner->pointer_==p&&p){
        const auto* start=static_cast<const uint8*>(p);return ReadCAS(std::make_shared<ResourceBytes>(start,start+owner->size_),out);
    }errno=EINVAL;return false;
}
}
namespace Nx {
CNativeMesh::CNativeMesh(){mp_texDict=nullptr;mp_coll_objects=nullptr;m_num_coll_objects=0;m_coll_filename[0]=0;m_CASRemovalMask=0;}
bool CNativeMesh::Assign(std::shared_ptr<SourcePort::SceneAsset> asset,SourcePort::CASAsset cas,CTexDict* dict,bool shadow){
    Dbg_Assert(source_thread_is_controller());if(!asset||asset_)return false;asset_=std::move(asset);cas_=std::move(cas);mp_texDict=dict;shadow_volume_=shadow;
    mp_hierarchyObjects=asset_->hierarchy.empty()?nullptr:asset_->hierarchy.data();m_numHierarchyObjects=asset_->hierarchy.size();m_CASRemovalMask=cas_.removal_mask;return true;
}
CMesh* CEngine::s_plat_load_mesh(const char* name,CTexDict* dict,uint32,bool skin,bool shadow){
    Dbg_Assert(source_thread_is_controller());if(!name||std::strlen(name)>500)return nullptr;
    const auto params=scaling;bool use_scaling=scaling_enabled;scaling_enabled=false;scaling={};
    auto asset=std::make_shared<SourcePort::SceneAsset>();SourcePort::ResourceError error;
    if(!SourcePort::ReadSceneResource(name,*asset,&error))return nullptr;SourcePort::CASAsset cas;
    if(skin){std::string path=name;std::string lower=path;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});auto ext=lower.rfind(".skin");
        if(ext==std::string::npos)return nullptr;path.replace(ext,5,".cas");if(!SourcePort::ReadCAS(SourcePort::ReadRenderResource(path.c_str()),cas,&error))return nullptr;
        std::replace(lower.begin(),lower.end(),'\\','/');
        asset->pedestrian_skin=lower.rfind("models/peds/",0)==0||
            lower.rfind("models/ped_male/",0)==0||lower.rfind("models/ped_female/",0)==0;
        asset->moscow_guard_skin=lower.substr(0,ext+5)=="models/peds/ped_rguard/ped_rguard_body.skin";
    }
    apply_scaling(*asset,use_scaling?&params:nullptr);auto* mesh=new CNativeMesh;
    if(!mesh->Assign(std::move(asset),std::move(cas),dict,shadow)){delete mesh;return nullptr;}return mesh;
}
CMesh* CEngine::s_plat_load_mesh(uint32 id,uint32* data,int size,uint8* cas_data,CTexDict* dict,uint32,bool,bool shadow){
    Dbg_Assert(source_thread_is_controller());const auto params=scaling;bool use_scaling=scaling_enabled;scaling_enabled=false;scaling={};
    if(!id||!data||size<=0||size>128*1024*1024)return nullptr;const auto* start=reinterpret_cast<const uint8*>(data);
    auto asset=std::make_shared<SourcePort::SceneAsset>();SourcePort::CASAsset cas;
    if(!SourcePort::ReadScene(start,size,*asset)||(cas_data&&!SourcePort::ReadEmbeddedCAS(cas_data,cas)))return nullptr;
    apply_scaling(*asset,use_scaling?&params:nullptr);auto* mesh=new CNativeMesh;
    if(!mesh->Assign(std::move(asset),std::move(cas),dict,shadow)){delete mesh;return nullptr;}return mesh;
}
bool CEngine::s_plat_unload_mesh(CMesh* mesh){if(!mesh)return false;delete mesh;return true;}
void CEngine::s_plat_set_mesh_scaling_parameters(SMeshScalingParameters* params){
    Dbg_Assert(source_thread_is_controller()&&params&&params->pWeightIndices&&params->pWeights&&params->pBonePositions&&params->pBoneScales);scaling=*params;scaling_enabled=true;
}
}
