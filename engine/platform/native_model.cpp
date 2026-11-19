#include <source_port/native_world.h>
#include <source_port/native_projection.h>
#include <source_port/memory.h>
#include <cmath>
#include <cstring>
#include <gfx/nx.h>
#include <gfx/nxquickanim.h>
namespace Nx {
CNativeModel::~CNativeModel(){SourcePort::ForgetProjectionModel(this);}
// The original model submits the same evaluated skeleton to each modular
// piece. Share a checked snapshot, keeping older snapshots immutable for
// detached pieces, clones and cutscene substitutions.
std::shared_ptr<const std::vector<Mth::Matrix>> CNativeModel::CapturePose(const Mth::Matrix* bones,unsigned count){
    Dbg_Assert(bones&&count&&count<=256);
    if(current_pose_&&current_pose_->size()==count&&
       std::memcmp(current_pose_->data(),bones,count*sizeof(*bones))==0)return current_pose_;
    for(unsigned i=0;i<count;++i)for(unsigned row=0;row<4;++row)for(unsigned column=0;column<4;++column)
        Dbg_Assert(std::isfinite(bones[i][row][column]));
    SourcePort::RootHeapScope storage;
    if(!current_pose_.unique())current_pose_.swap(previous_pose_);
    if(!current_pose_.unique())current_pose_=std::make_shared<std::vector<Mth::Matrix>>();
    current_pose_->assign(bones,bones+count);
    return current_pose_;
}
bool CNativeModel::plat_init_skeleton(int count){if(count<=0||count>256){errno=ERANGE;return false;}skeleton_size_=count;return true;}
bool CNativeModel::plat_set_render_mode(ERenderMode mode){
    if(mode!=vTEXTURED&&mode!=vNONE){errno=ENOTSUP;return false;}for(int i=0;i<m_numGeoms;++i)mp_geom[i]->SetActive(mode==vTEXTURED&&m_active&&!m_hidden&&(m_geomActiveMask&(1u<<i)));return true;
}
void CNativeModel::plat_set_bounding_sphere(const Mth::Vector& sphere){for(int i=0;i<m_numGeoms;++i){mp_geom[i]->SetBoundingSphere(sphere);auto bound=sphere;bound[3]*=2;auto* geom=dynamic_cast<CNativeGeom*>(mp_geom[i]);Dbg_Assert(geom);geom->SetModelCullingSphere(bound);}}
Mth::Vector CNativeModel::plat_get_bounding_sphere(){
    if(!m_numGeoms)return Mth::Vector(0,0,0,0);auto sphere=mp_geom[0]->GetBoundingSphere();
    for(int i=1;i<m_numGeoms;++i){auto other=mp_geom[i]->GetBoundingSphere(),delta=other-sphere;float distance=delta.Length();
        if(distance+other[3]<=sphere[3])continue;if(distance+sphere[3]<=other[3]){sphere=other;continue;}
        auto sum=sphere+other;sphere=.5f*(sum+(delta[3]/distance)*delta);sphere[3]=.5f*(distance+sum[3]);}
    sphere[3]*=2; // original DX9 model's animation safety margin
    for(int i=0;i<m_numGeoms;++i){auto* geom=dynamic_cast<CNativeGeom*>(mp_geom[i]);Dbg_Assert(geom);geom->SetModelCullingSphere(sphere);}
    return sphere;
}
CModel* CEngine::s_plat_init_model(){return new CNativeModel;}
bool CEngine::s_plat_uninit_model(CModel* model){if(!model)return false;delete model;return true;}
CQuickAnim* CEngine::s_plat_init_quick_anim(){return new CQuickAnim;} // whole original decompression/interpolation owner
void CEngine::s_plat_uninit_quick_anim(CQuickAnim* anim){delete anim;}
}
