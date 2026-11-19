#include "../third_party/meshoptimizer/meshoptimizer.h"
#include <source_port/mesh_assets.h>
#include <algorithm>
#include <source_port/memory.h>
namespace SourcePort {
bool ReadCAS(std::shared_ptr<const ResourceBytes> bytes,CASAsset& out,ResourceError* error){
    if(!bytes)return false;ResourceReader r(bytes->data(),bytes->size(),error);r.Context("CAS polygon removal");CASAsset result;
    auto version=r.U32();if(version<1||version>2)return r.Fail("unsupported CAS version");
    if(version>=2)result.removal_mask=r.U32();auto count=r.U32();if(!r.Count(count,12,1000000))return false;
    result.records.resize(count);
    for(auto& entry:result.records){entry.mask=r.U32();auto a=r.U32(),b=r.U32();entry.mesh=a>>16;entry.indices={uint16(a),uint16(b>>16),uint16(b)};}
    if(!r.Ok()||r.Remaining())return r.Fail("CAS trailing bytes");
    std::stable_sort(result.records.begin(),result.records.end(),[](const auto& a,const auto& b){return a.mesh<b.mesh||(a.mesh==b.mesh&&a.indices[0]<b.indices[0]);});
    out=std::move(result);return true;
}
bool MeshTriangles(const SceneAsset& scene,std::size_t sector,std::size_t mesh,unsigned lod,
    const CASAsset* cas,uint32 mask,std::vector<uint16>& out){
    if(sector>=scene.sectors.size()||mesh>=scene.sectors[sector].meshes.size())return false;
    const auto& m=scene.sectors[sector].meshes[mesh];if(lod>=m.lods.size())return false;
    std::size_t load_order=mesh;
    const auto& strip=m.lods[lod];std::vector<uint16> triangles;
    std::vector<int> cas_index;
    if(cas&&mask&&lod==0&&!cas->records.empty()){
        for(std::size_t s=0;s<sector;++s)load_order+=scene.sectors[s].meshes.size();
        // Original DX9 sMesh::Initialize removes unused vertices, in ascending
        // SOURCE vertex order, before HidePolys compares the CAS triplets.
        // CAS indices address that compact buffer, not the scene's vertices.
        cas_index.assign(scene.sectors[sector].VertexCount(),-1);
        for(auto i:m.lods[0]){if(i>=cas_index.size())return false;cas_index[i]=0;}
        int next=0;for(auto& i:cas_index)if(i==0)i=next++;
    }
    if(strip.size()>2)triangles.reserve((strip.size()-2)*3);
    for(std::size_t i=2;i<strip.size();++i){uint16 a=strip[i-2],b=strip[i-1],c=strip[i];if(a==b||b==c||a==c)continue;
        bool hidden=false;
        if(!cas_index.empty()){
            // ReadCAS orders records by original mesh load order and first
            // strip index. Search that range instead of rescanning every CAS
            // record for every triangle on every shadow/world render pass.
            const std::array<uint16,3> compact{uint16(cas_index[a]),uint16(cas_index[b]),uint16(cas_index[c])};
            auto entry=std::lower_bound(cas->records.begin(),cas->records.end(),std::pair{load_order,compact[0]},[](const CASRecord& r,const auto& key){return r.mesh<key.first||(r.mesh==key.first&&r.indices[0]<key.second);});
            for(;entry!=cas->records.end()&&entry->mesh==load_order&&entry->indices[0]==compact[0];++entry)
                if((entry->mask&mask)&&entry->indices==compact){hidden=true;break;}
        }
        if(hidden)continue;
        // Original D3D triangle strips alternate their winding on EVERY
        // index, including exporter degenerate strip connectors.
        if(i&1)std::swap(a,b);triangles.insert(triangles.end(),{a,b,c});
    }out.swap(triangles);return true;
}
namespace {
Mth::Vector transform(const Mth::Vector& p,const Mth::Matrix& m){
    Mth::Vector result;for(unsigned c=0;c<4;++c)result[c]=p[0]*m[0][c]+p[1]*m[1][c]+p[2]*m[2][c]+p[3]*m[3][c];return result;
}
int signed_bits(uint32 v,unsigned n){unsigned sign=1u<<(n-1);return int(v&(sign-1))-int(v&sign);}
bool finite(const Mth::Vector& p){return std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2])&&std::isfinite(p[3]);}
}
bool PoseSector(const SectorAsset& s,const Mth::Matrix* bones,int count,std::vector<Mth::Vector>& positions,std::vector<Mth::Vector>* normals,const MeshAsset* mesh){
    if(count<0||(!bones&&count))return false;
    if(((s.flags&16)||s.bone>=0)&&(!bones||!count))return false;
    unsigned used=1;auto inspect=[&](const AssetVertex& v){if(v.weights>>22)used=3;else if(((v.weights>>11)&0x7ff)&&used<2)used=2;};
    if(mesh){if(mesh->lods.empty())return false;for(auto i:mesh->lods[0]){if(i>=s.VertexCount())return false;inspect(s.Vertex(i));}}
    else for(std::size_t i=0;i<s.VertexCount();++i)inspect(s.Vertex(i));
    std::vector<Mth::Vector> p,n;p.reserve(s.VertexCount());if(normals)n.reserve(s.VertexCount());
    for(std::size_t i=0;i<s.VertexCount();++i){const auto v=s.Vertex(i);Mth::Vector local(v.position[0],v.position[1],v.position[2],1),normal(v.normal[0],v.normal[1],v.normal[2],0),pos=local,norm=normal;
        if(s.flags&16){
            // The PC exporter uses direct bone indices (DX9 CASE1/2/3),
            // NOT the Xbox shader's three-constant-row palette addresses.
            // D3DDECLTYPE_DEC3N weights are signed 11/11/10.
            float weights[3]={signed_bits(v.weights,11)/1024.f,signed_bits(v.weights>>11,11)/1024.f,signed_bits(v.weights>>22,10)/512.f};
            pos.Set(0,0,0,0);norm.Set(0,0,0,0);
            for(unsigned j=0;j<used;++j){float weight=used==1?1:weights[j];if(weight==0)continue;unsigned index=v.bones[j];if(index>=unsigned(count))return false;
                auto t=transform(local,bones[index]);auto q=transform(normal,bones[index]);
                for(unsigned c=0;c<4;++c){pos[c]+=t[c]*weight;norm[c]+=q[c]*weight;}}
            pos[3]=1;norm[3]=0;
        }else if(s.bone>=0){if(s.bone>=count)return false;pos=transform(local,bones[s.bone]);norm=transform(normal,bones[s.bone]);}
        if(!finite(pos)||!finite(norm))return false;p.push_back(pos);if(normals)n.push_back(norm);
    }positions.swap(p);if(normals)normals->swap(n);return true;
}
namespace {
bool CompactRenderTopology(const SectorAsset& sector, const std::vector<uint16>& indices, RenderTopology& result) {
    std::vector<int> remap(sector.VertexCount(), -1);
    result.triangles.reserve(indices.size());
    for (auto index : indices) {
        if (index >= sector.VertexCount()) return false;
        auto& compact = remap[index];
        if (compact < 0) {
            if (result.vertices.size() > 65535) return false;
            compact = result.vertices.size();
            result.vertices.push_back(index);
        }
        result.triangles.push_back(uint16(compact));
    }
    return true;
}
}
// Build from the already CAS-filtered material. Collapses select original vertices,
// so UV seams, joint assignments and hidden triangles never need reconstruction.
void SimplifyGameplaySkin(const SectorAsset& sector,RenderTopology& topology){
    const auto count=topology.vertices.size();
    const auto elements=topology.triangles.size();
    if(!(sector.flags&16)||elements<120||count<24||!sector.uv_sets)return;
    struct Sample {std::array<float,3> position;std::array<float,5> attributes;};
    std::vector<Sample> samples(count);
    std::vector<unsigned char> locked(count);
    for(size_t i=0;i<count;++i){
        const auto index=topology.vertices[i];
        const auto& v=sector.vertices[index];
        const auto uv=sector.UV(size_t(index)*sector.uv_sets);
        samples[i]={v.position,{uv[0],uv[1],float(signed_bits(v.weights,11))/1024.f,
              float(signed_bits(v.weights>>11,11))/1024.f,float(signed_bits(v.weights>>22,10))/512.f}};
    }
    // Pin both sides of joint palette boundaries; collapsing across them
    // could look fine in bind pose and pull a shoulder apart in animation.
    for(size_t i=0;i<elements;i+=3){
        for(unsigned edge=0;edge<3;++edge){
            auto a=topology.triangles[i+edge],b=topology.triangles[i+(edge+1)%3];
            const auto& va=sector.vertices[topology.vertices[a]];
            const auto& vb=sector.vertices[topology.vertices[b]];
            for(unsigned j=0;j<topology.weights;++j)if(va.bones[j]!=vb.bones[j]){
                locked[a]=locked[b]=1;break;
            }
        }
    }
    const float weights[]={.4f,.4f,.75f,.75f,.75f};
    std::vector<uint16> indices(elements);
    auto kept=meshopt_simplifyWithAttributes(indices.data(),topology.triangles.data(),elements,
        samples[0].position.data(),count,sizeof(Sample),samples[0].attributes.data(),sizeof(Sample),
        weights,5,locked.data(),3*(elements/9),1.0f,
        meshopt_SimplifyLockBorder|meshopt_SimplifyErrorAbsolute,nullptr);
    if(kept<3||kept>=elements)return;
    indices.resize(kept);
    for(auto& index:indices)index=topology.vertices[index];
    RenderTopology compact;
    compact.weights=topology.weights;
    if(CompactRenderTopology(sector,indices,compact))topology=std::move(compact);
}
bool BuildDistantRenderTopology(const SectorAsset& sector, const MeshAsset& mesh, RenderTopology& out) {
    RenderTopology result;
    if (mesh.distant_triangles.empty() ||
        !CompactRenderTopology(sector, mesh.distant_triangles, result)) return false;
    out = std::move(result);
    return true;
}
bool BuildRenderTopology(const SceneAsset& scene,std::size_t sector,std::size_t mesh,unsigned lod,
    const CASAsset* cas,uint32 mask,RenderTopology& out){
    std::vector<uint16> indices;if(!MeshTriangles(scene,sector,mesh,lod,cas,mask,indices))return false;
    const auto& s=scene.sectors[sector];const auto& m=s.meshes[mesh];RenderTopology result;
    // DX9 CASE1/2/3 is selected from the ORIGINAL mesh's base strip, including
    // exporter connector vertices; it is not selected per surviving triangle.
    if(s.flags&16)for(auto index:m.lods[0]){if(index>=s.VertexCount())return false;auto w=s.Vertex(index).weights;
        if(w>>22)result.weights=3;else if(((w>>11)&0x7ff)&&result.weights<2)result.weights=2;}
    if(!CompactRenderTopology(s,indices,result))return false;
    out=std::move(result);return true;
}
bool PoseRenderTopology(const SectorAsset& s,const RenderTopology& topology,const Mth::Matrix* bones,int count,
    std::vector<Mth::Vector>& positions,std::vector<Mth::Vector>* normals){
    if(count<0||(!bones&&count)||!topology.weights||topology.weights>3)return false;
    if(((s.flags&16)||s.bone>=0)&&(!bones||!count))return false;
    std::vector<Mth::Vector> p,n;p.reserve(topology.vertices.size());if(normals)n.reserve(topology.vertices.size());
    for(auto index:topology.vertices){if(index>=s.VertexCount())return false;const auto v=s.Vertex(index);
        Mth::Vector local(v.position[0],v.position[1],v.position[2],1),pos=local,normal(v.normal[0],v.normal[1],v.normal[2],0),norm=normal;
        if(s.flags&16){float weights[3]={signed_bits(v.weights,11)/1024.f,signed_bits(v.weights>>11,11)/1024.f,signed_bits(v.weights>>22,10)/512.f};
            pos.Set(0,0,0,0);if(normals)norm.Set(0,0,0,0);
            for(unsigned j=0;j<topology.weights;++j){float weight=topology.weights==1?1:weights[j];if(weight==0)continue;unsigned bone=v.bones[j];if(bone>=unsigned(count))return false;
                auto t=transform(local,bones[bone]);for(unsigned c=0;c<4;++c)pos[c]+=t[c]*weight;
                if(normals){auto q=transform(normal,bones[bone]);for(unsigned c=0;c<4;++c)norm[c]+=q[c]*weight;}}
            pos[3]=1;norm[3]=0;
        }else if(s.bone>=0){if(s.bone>=count)return false;pos=transform(local,bones[s.bone]);if(normals)norm=transform(normal,bones[s.bone]);}
        if(!finite(pos)||(normals&&!finite(norm)))return false;p.push_back(pos);if(normals)n.push_back(norm);
    }positions.swap(p);if(normals)normals->swap(n);return true;
}
namespace {
using Point3 = std::array<float,3>;
Point3 affine_point(const Point3& p,const Mth::Matrix& m){
    return {p[0]*m[0][0]+p[1]*m[1][0]+p[2]*m[2][0]+m[3][0],
            p[0]*m[0][1]+p[1]*m[1][1]+p[2]*m[2][1]+m[3][1],
            p[0]*m[0][2]+p[1]*m[1][2]+p[2]*m[2][2]+m[3][2]};
}
bool skin_point(const SectorAsset& sector,unsigned index,unsigned weights_count,const std::vector<Mth::Matrix>& bones,Point3& local){
    if(index>=sector.vertices.size())return false;
    const auto& v=sector.vertices[index];
    local={};
    if(weights_count==1){
        if(v.bones[0]>=bones.size())return false;
        local=affine_point(v.position,bones[v.bones[0]]);
    }else{
        const float weights[]={signed_bits(v.weights,11)/1024.f,
            signed_bits(v.weights>>11,11)/1024.f,signed_bits(v.weights>>22,10)/512.f};
        for(unsigned j=0;j<weights_count;++j){
            if(weights[j]==0)continue;
            if(v.bones[j]>=bones.size())return false;
            const auto point=affine_point(v.position,bones[v.bones[j]]);
            for(unsigned axis=0;axis<3;++axis)local[axis]+=point[axis]*weights[j];
        }
    }
    return true;
}
std::size_t ped_pose_bytes=0;

}
SharedSkinPositions::SharedSkinPositions(const SectorAsset& sector){
    const auto n=sector.vertices.size();
    if(!n||n>8192)return;
    std::vector<uint16> order(n),last_mesh(n,0xffff);
    std::vector<bool> shared(n,false);
    for(unsigned m=0;m<sector.meshes.size()&&m<0xffff;++m){
        const auto& lods=sector.meshes[m].lods;
        if(lods.empty())continue;
        for(auto v:lods.front())if(v<n){
            shared[v]=shared[v]||(last_mesh[v]!=0xffff && last_mesh[v]!=m);
            last_mesh[v]=m;
        }
    }
    for(unsigned i=0;i<n;++i)order[i]=i;
    auto compare=[&](unsigned a,unsigned b){
        const auto& x=sector.vertices[a];const auto& y=sector.vertices[b];
        int pos=std::memcmp(x.position.data(),y.position.data(),3*sizeof(float));
        if(pos)return pos;
        if(x.weights!=y.weights)return x.weights<y.weights?-1:1;
        return std::memcmp(x.bones.data(),y.bones.data(),3*sizeof(uint16));
    };
    std::sort(order.begin(),order.end(),[&](auto a,auto b){return compare(a,b)<0;});
    for(std::size_t i=0;i<n;){
        auto end=i+1;
        while(end<n&&compare(order[i],order[end])==0)++end;
        if(end-i>1||shared[order[i]]){
            if(mapping_.empty())mapping_.assign(n,0xffff);
            for(auto j=i;j<end;++j)mapping_[order[j]]=static_cast<uint16>(slots_.size());
            slots_.emplace_back();
        }
        i=end;
    }
}
void SharedSkinPositions::Begin(){
    if(++generation_==0){for(auto& s:slots_)s.generation=0;++generation_;}
    hits=0;
}
std::array<float,3>* SharedSkinPositions::Lookup(unsigned index,unsigned weights,bool& ready){
    ready=false;
    if(index>=mapping_.size()||mapping_[index]==0xffff)return nullptr;
    auto& slot=slots_[mapping_[index]];
    ready=slot.generation==generation_ && slot.weights==weights;
    if(ready)++hits;
    slot.generation=generation_;slot.weights=weights;
    return &slot.point;
}
// The source skin forces w to one after blending, even for signed weights
// whose sum differs from one. Apply the world translation only afterwards.
bool PoseSkinnedDraw(const SectorAsset& sector,const RenderTopology& topology,
    const std::vector<Mth::Matrix>& bones,const Mth::Matrix& world,
    const std::vector<DrawVertexAttributes>& attributes,std::vector<SourceRenderVertex>& out,SharedSkinPositions* reuse){
    if(!(sector.flags&16)||topology.weights<1||topology.weights>3||bones.empty()||
       attributes.size()!=topology.vertices.size()||!out.empty())return false;
    out.reserve(attributes.size());
    for(std::size_t i=0;i<attributes.size();++i){
        bool ready=false;
        auto* slot=reuse?reuse->Lookup(topology.vertices[i],topology.weights,ready):nullptr;
        Point3 p;
        if(ready)p=*slot;
        else{
            Point3 local;
            if(!skin_point(sector,topology.vertices[i],topology.weights,bones,local)){if(reuse)reuse->Begin();out.clear();return false;}
            p=affine_point(local,world);
            if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])){if(reuse)reuse->Begin();out.clear();return false;}
            if(slot)*slot=p;
        }
        const auto& a=attributes[i];
        out.push_back({p[0],p[1],p[2],a.r,a.g,a.b,a.a,a.u,a.v});
    }
    return true;
}
std::unique_ptr<DistantPedPose> DistantPedPose::Create(std::size_t n){
    constexpr std::size_t budget=512*1024;
    const auto bytes_per_vertex=2*sizeof(Point);
    if(!n||n>(budget-ped_pose_bytes)/bytes_per_vertex)return {};
    const auto bytes=n*bytes_per_vertex;
    if(SourcePort::EngineMemoryIsReady()&&Mem::Manager::sHandle().BottomUpHeap()->ParentRegion()->MemAvailable()<int(bytes+2*1024*1024))return {};
    RootHeapScope storage;
    std::unique_ptr<DistantPedPose> pose(new DistantPedPose(n));
    ped_pose_bytes+=bytes;
    pose->current_.reserve(n);pose->previous_.reserve(n);
    return pose;
}
DistantPedPose::~DistantPedPose(){ped_pose_bytes-=capacity_*2*sizeof(Point);}
bool DistantPedPose::Draw(const SectorAsset& sector,const RenderTopology& topology,
    const std::vector<Mth::Matrix>& bones,const Mth::Matrix& world,const std::vector<DrawVertexAttributes>& attributes,
    uint32 now,std::vector<SourceRenderVertex>& out,bool& sampled){
    constexpr uint32 interval=67;
    if(!(sector.flags&16)||topology.weights<1||topology.weights>3||bones.empty()||
       attributes.size()!=capacity_||topology.vertices.size()!=capacity_||!out.empty())return false;
    const bool reset=current_.empty()||now-last_draw_>2*interval||bone_count_!=bones.size();
    const auto age=now-sampled_at_;
    sampled=reset||age>=interval;
    if(sampled){
        current_.swap(previous_);
        if(reset)previous_.clear();
        current_.resize(capacity_);
        for(std::size_t i=0;i<capacity_;++i){
            if(!skin_point(sector,topology.vertices[i],topology.weights,bones,current_[i])){
                current_.clear();previous_.clear();return false;
            }
        }
        span_=reset?0:age;
        sampled_at_=now;bone_count_=bones.size();
    }
    last_draw_=now;
    const float mix=span_?std::clamp(1.f-(interval-float(now-sampled_at_))/span_,0.f,1.f):1.f;
    out.reserve(capacity_);
    for(std::size_t i=0;i<capacity_;++i){
        auto local=current_[i];
        if(mix<1)for(unsigned axis=0;axis<3;++axis)
            local[axis]=previous_[i][axis]+(local[axis]-previous_[i][axis])*mix;
        const auto p=affine_point(local,world);
        if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])){out.clear();return false;}
        const auto& a=attributes[i];out.push_back({p[0],p[1],p[2],a.r,a.g,a.b,a.a,a.u,a.v});
    }
    return true;
}
bool BuildSkinTopology(const SectorAsset& sector,const RenderTopology& topology,unsigned bone_count,SkinTopology& out){
    if(!(sector.flags&16)||topology.vertices.empty()||topology.weights<1||topology.weights>3||!bone_count)return false;
    SkinTopology result;
    result.influences.reserve(topology.vertices.size());
    for(auto vertex_index:topology.vertices){
        if(vertex_index>=sector.VertexCount())return false;
        const auto vertex=sector.Vertex(vertex_index);
        const float weights[]{signed_bits(vertex.weights,11)/1024.f,
            signed_bits(vertex.weights>>11,11)/1024.f,signed_bits(vertex.weights>>22,10)/512.f};
        SourceSkinInfluence influence{};
        for(unsigned slot=0;slot<topology.weights;++slot){
            const float weight=topology.weights==1?1.f:weights[slot];
            if(weight==0)continue;
            const auto bone=vertex.bones[slot];
            if(bone>=bone_count)return false;
            auto at=std::find(result.bones.begin(),result.bones.end(),bone);
            const auto index=std::size_t(at-result.bones.begin());
            if(at==result.bones.end()){
                if(index==SOURCE_SKIN_BONES)return false;
                result.bones.push_back(bone);
            }
            influence.rows[slot]=uint8(index*3);
            influence.weights[slot]=weight;
        }
        result.influences.push_back(influence);
    }
    if(result.bones.empty())return false;
    out=std::move(result);
    return true;
}
}
