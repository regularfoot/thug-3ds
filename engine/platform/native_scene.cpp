#include <source_port/native_world.h>
#include <source_port/application_os.h>
#include <source_port/graphics.h>
#include <source_port/native_projection.h>
#include <source_port/native_renderer.h>
#include <source_port/static_world_lod.h>
#include <source_port/clock_os.h>
#include <source_port/timer_state.h>
#include <source_port/diagnostics.h>
#include <gfx/nx.h>
#include <gfx/nxtexman.h>
#include <gfx/nxmiscfx.h>
#include <gel/collision/collision.h>
#include <string>
#include <strings.h>
namespace {
// Large scenes use the spatial tree by default. Moscow also pays back the
// traversal on New 3DS despite having only 974 static sectors.
constexpr std::size_t WorldPartitionSectorThreshold=1400;
}
namespace Nx {
CNativeSector::CNativeSector(){m_checksum=0;m_scale.Set(1,1,1,1);}
void CNativeSector::plat_set_shatter(bool on){if(on&&mp_geom)Shatter(mp_geom);shatter_=on;}
CSector* CNativeSector::plat_clone(bool,CScene*){auto* clone=new CNativeSector;clone->shatter_=shatter_;clone->m_light_group=m_light_group;return clone;} // original CSector::clone owns geom/collision copying
CNativeScene::CNativeScene(int bits):CScene(bits),majority_(128,128,128,128){mp_tex_dict=nullptr;m_id=0;m_coll_filename[0]=m_add_coll_filename[0]=0;}
CNativeScene::~CNativeScene(){SourcePort::ForgetProjectionScene(this);}
bool CNativeScene::Assign(std::shared_ptr<SourcePort::SceneAsset> asset,CTexDict* dict,bool sky,bool dictionary,const char* scene_name){
    if(asset_||!asset)return false;asset_=std::move(asset);mp_tex_dict=dict;m_sky=sky;
    std::shared_ptr<const SourcePort::StaticWorldMaterials> material_groups;
    if(!sky&&!dictionary){
        if(auto* native=dynamic_cast<CNativeTexDict*>(dict))native->EnableOpaqueWorldMipmaps(asset_->materials,scene_name);
        material_groups=std::make_shared<const SourcePort::StaticWorldMaterials>(*asset_,dict);
    }
    const std::string_view path=scene_name?scene_name:"";
    const auto basename=path.substr(path.find_last_of("/\\")+1);
    partition_world_=asset_->sectors.size()>=WorldPartitionSectorThreshold||
        (!sky&&!dictionary&&source_application_os_is_new_3ds()&&strcasecmp(basename.data(),"RU.scn.xbx")==0);
    for(std::size_t i=0;i<asset_->sectors.size();++i){auto* sector=new CNativeSector;sector->SetChecksum(asset_->sectors[i].checksum);auto* geom=new CNativeGeom;
        Dbg_Assert(geom->Assign(asset_,{i},dict,sky,dictionary));geom->SetScene(this);
        geom->SetStaticWorldMaterials(material_groups);
        if(partition_world_)geom->EnableStaticWorldPartition();
        sector->SetGeom(geom);AddSector(sector);}
    bounds();return true;
}
bool CNativeScene::Add(std::shared_ptr<SourcePort::SceneAsset> asset){
    if(!asset||!additions_.empty())return false;
    std::shared_ptr<const SourcePort::StaticWorldMaterials> material_groups;
    if(!m_sky)material_groups=std::make_shared<const SourcePort::StaticWorldMaterials>(*asset,mp_add_tex_dict);
    for(std::size_t i=0;i<asset->sectors.size();++i){auto* sector=new CNativeSector;sector->SetChecksum(asset->sectors[i].checksum);auto* geom=new CNativeGeom;
        Dbg_Assert(geom->Assign(asset,{i},mp_add_tex_dict,m_sky,false));geom->SetScene(this);sector->SetGeom(geom);
        geom->SetStaticWorldMaterials(material_groups);
        if(partition_world_)geom->EnableStaticWorldPartition();
        if(auto* old=GetSector(sector->GetChecksum())){remove_sector_from_table(old->GetChecksum());mp_orig_sectors->AddToTail(new Lst::Node<CSector>(old));old->SetActive(false);}
        AddSector(sector);mp_add_sectors->AddToTail(new Lst::Node<CSector>(sector));
    }additions_.push_back(std::move(asset));bounds();return true;
}
void CNativeScene::bounds(){
    m_render_bbox.Reset();bool any=false;mp_sector_table->IterateStart();while(auto* sector=mp_sector_table->IterateNext())if(auto* geom=sector->GetGeom()){
        const auto& box=geom->GetBoundingBox();m_render_bbox.AddPoint(box.GetMin());m_render_bbox.AddPoint(box.GetMax());any=true;}
    if(!any)m_render_bbox.Set(Mth::Vector(0,0,0,1),Mth::Vector(0,0,0,1));
}
void CNativeScene::plat_post_load(){bounds();}
void CNativeScene::plat_post_add(){for(auto* node=mp_add_sectors->GetNext();node;node=node->GetNext()){
    auto* geom=dynamic_cast<CNativeGeom*>(node->GetData()->GetGeom());Dbg_Assert(geom);geom->SetTextureDictionary(mp_add_tex_dict);}bounds();}
bool CNativeScene::plat_load_textures(const char* name){
    if(!name||std::strlen(name)>100||mp_tex_dict)return false;mp_tex_dict=CTexDictManager::sLoadTextureDictionary(name,true);return mp_tex_dict!=nullptr;
}
bool CScene::LoadTextures(const char* name){return plat_load_textures(name);}
bool CNativeScene::plat_load_collision(const char*){return mp_coll_sector_data&&(m_num_coll_sectors==0||mp_coll_sectors)&&(!m_in_super_sectors||m_num_coll_sectors==0||mp_sector_man);}
bool CNativeScene::plat_add_collision(const char*){return (m_num_add_coll_sectors==0||mp_add_coll_sectors)&&mp_add_coll_sector_data;}
bool CNativeScene::plat_unload_add_scene(){if(SourcePort::GraphicsReady())Dbg_Assert(source_graphics_wait()==0);additions_.clear();return true;}
CSector* CNativeScene::plat_create_sector(){return new CNativeSector;}
CScene* CEngine::s_plat_create_scene(const char*,CTexDict* dict,bool super){auto* scene=new CNativeScene;scene->SetTexDict(dict);scene->SetInSuperSectors(super);return scene;}
CScene* CEngine::s_plat_load_scene(const char* name,CTexDict* dict,bool super,bool sky,bool dictionary){
    const auto started=source_clock_now();
    auto asset=std::make_shared<SourcePort::SceneAsset>();SourcePort::ResourceError error;
    SourcePort::SceneSignature signature{};
    const bool world=!sky&&!dictionary;
    bool read=SourcePort::ReadSceneResource(name,*asset,&error,world?&signature:nullptr,world&&(source_application_os_is_old_3ds()||source_application_os_is_new_3ds()));
    Dbg_MsgAssert(read,("Original scene %s failed at %u: %s",name,unsigned(error.offset),error.reason?error.reason:"missing resource"));
    const auto parsed=source_clock_now();
    if(world) SourcePort::LoadSceneLods(name,signature,*asset);
    const auto lods=source_clock_now();
    unsigned bits=1;while(bits<12&&(1u<<bits)<asset->sectors.size()*2)++bits;auto* scene=new CNativeScene(bits);
    scene->SetInSuperSectors(super);Dbg_Assert(scene->Assign(std::move(asset),dict,sky,dictionary,name));
    if(world) {
        const auto now=source_clock_now();
        const auto ms=[](uint64 ticks){return (unsigned long long)SourcePort::ScaleClockTicks(ticks,source_clock_frequency(),1000);};
        SourcePort::Log("LEVEL_SCENE parse_ms=%llu lod_ms=%llu bind_ms=%llu name=%s",ms(parsed-started),ms(lods-parsed),ms(now-lods),name);
    }
    return scene;
}
bool CEngine::s_plat_unload_scene(CScene* scene){
    if(!scene)return false;if(SourcePort::GraphicsReady())Dbg_Assert(source_graphics_wait()==0);
    // Release retained mesh data before the next level/menu claims this heap.
    SourcePort::ReleaseWorldSceneBuffers();
    // Original CEngine::sUnloadScene deletes the object and unregisters it
    // AFTER this hook. Native storage is owned by CNativeScene/CNativeGeom
    // destructors: deleting here as well double-frees the sector hash table.
    return true;
}
bool CEngine::s_plat_add_scene(CScene* scene,const char* filename){
    auto* native=dynamic_cast<CNativeScene*>(scene);if(!native)return false;auto asset=std::make_shared<SourcePort::SceneAsset>();
    bool loaded=SourcePort::ReadSceneResource(filename,*asset,nullptr,nullptr,source_application_os_is_old_3ds()||source_application_os_is_new_3ds())&&native->Add(std::move(asset));Dbg_MsgAssert(loaded,("Original scene addition failed: %s",filename));return loaded;
}
}
