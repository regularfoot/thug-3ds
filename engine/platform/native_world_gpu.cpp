#include <source_port/stereo.h>
#include <source_port/native_world.h>
#include <source_port/batch_sort.h>
#include <source_port/native_fog.h>
#include <source_port/graphics.h>
#include <source_port/thread_os.h>
#include <source_port/application_os.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <tuple>
#include <optional>
#include <source_port/memory.h>
namespace SourcePort {
bool WorldMeshRetention::Remember(std::shared_ptr<const StaticWorldMesh> mesh){
    if(!mesh||mesh->owner.expired())return false;
    const auto bytes=sizeof(StaticWorldMesh)+mesh->vertices.capacity()*sizeof(SourceRenderVertex)+
        (mesh->indices?mesh->indices->capacity()*sizeof(uint16):0)+mesh->source_strip.capacity()*sizeof(uint16);
    if(bytes>budget_)return false;
    while(!entries_.empty()&&(entries_.front().mesh->owner.expired()||bytes>budget_-bytes_)){
        bytes_-=entries_.front().bytes;
        entries_.pop_front();
    }
    NativeHeapScope storage;
    entries_.push_back({std::move(mesh),bytes});
    bytes_+=bytes;
    return true;
}
namespace {
bool base_depth_draw(const WorldBatch& b){return b.depth_write&&!b.sky&&!b.pass&&b.shard_lighting.empty();}
bool sortable(const WorldBatch& b){return b.opaque&&base_depth_draw(b);}
bool batchable(const WorldBatch& b){return !b.retained&&!b.indices&&sortable(b);}
// A key is much smaller than a WorldBatch and owns no geometry. Keep the
// original lexicographic field order, including pointer order and z bias.
struct WorldMaterialKey {
    Nx::CNativeTexture* texture;
    std::array<unsigned,6> sampler;
    unsigned flags;
    float z_bias;
    explicit WorldMaterialKey(const WorldDrawState& b,bool merge_materials=false):texture(b.texture),
        sampler{b.blend,merge_materials&&(b.blend==0||b.blend==5)?0:b.fixed_alpha,
                b.cutoff,b.u_address,b.v_address,merge_materials?0:b.filter},
        flags(unsigned(b.cull)<<6|unsigned(b.depth_write)<<5|unsigned(b.ignore_vertex_alpha)<<4|
              unsigned(!merge_materials&&b.no_skater_shadow)<<3|unsigned(b.fog_disabled)<<2|
              unsigned(b.camera_relative)<<1|unsigned(b.mesh_uv)),z_bias(b.z_bias){}
    bool operator==(const WorldMaterialKey& b)const{
        return texture==b.texture&&sampler==b.sampler&&flags==b.flags&&z_bias==b.z_bias;
    }
    int compare(const WorldMaterialKey& b)const{
        if(texture!=b.texture)return std::less<Nx::CNativeTexture*>{}(texture,b.texture)?-1:1;
        for(unsigned i=0;i<sampler.size();++i)if(sampler[i]!=b.sampler[i])return sampler[i]<b.sampler[i]?-1:1;
        if(flags!=b.flags)return flags<b.flags?-1:1;
        return z_bias<b.z_bias?-1:z_bias>b.z_bias?1:0;
    }
};
bool append_retained(const WorldBatch& first,std::size_t current_elements,const WorldBatch& next,bool merge_materials){
    if(!first.retained||!next.retained||!base_depth_draw(first)||!base_depth_draw(next)
        ||first.opaque!=next.opaque||(!merge_materials&&first.draw_order!=next.draw_order)||first.texture!=next.texture)return false;
    const auto elements=next.ElementCount();
    // Shader sampler filtering is already fixed for world draws, and the
    // shadow receiver flag is consumed before submit. Logical draw bands
    // govern ordering, but adjacent batches still submit in triangle order.
    return elements<=32760&&current_elements<=32760-elements&&
        WorldMaterialKey(first,merge_materials)==WorldMaterialKey(next,merge_materials);
}
uint64 identity_order(const WorldBatch& b){return b.retained?b.retained->serial:~uint64(0);}
bool state_less(const WorldBatch& a,const WorldBatch& b){
    const int compare=WorldMaterialKey(a).compare(WorldMaterialKey(b));
    return compare?compare<0:identity_order(a)<identity_order(b);
}

int compare_material_band(const WorldDrawState& a,const WorldDrawState& b){
    if(a.draw_order!=b.draw_order)return a.draw_order<b.draw_order?-1:1;
    if(a.texture!=b.texture)return std::less<Nx::CNativeTexture*>{}(a.texture,b.texture)?-1:1;
    return WorldMaterialKey(a).compare(WorldMaterialKey(b));
}

bool compare_draw_tail(const WorldBatch& a,const WorldBatch& b,std::size_t i,std::size_t j){
    if(a.distance!=b.distance)return a.opaque?a.distance<b.distance:a.distance>b.distance;
    if(a.group!=b.group)return a.group<b.group;
    if(a.pass!=b.pass)return a.pass<b.pass;
    return i<j;
}

bool sort_material_groups(std::vector<WorldBatch>& batches,WorldDetailStats* detail,SourceFrameWorker* worker){
    const StaticWorldMaterials* plan=nullptr;
    std::size_t planned=0;
    for(const auto& batch:batches){
        if(!plan)plan=batch.static_materials;
        if(plan&&batch.static_materials==plan)++planned;
    }
    if(planned<32||planned<batches.size()/3||plan->GroupCount()>batches.size()*4)return false;
    const auto group_count=plan->GroupCount();
    std::vector<std::size_t> ends(group_count);
    for(const auto& batch:batches)if(batch.static_materials==plan)++ends[batch.material_group];
    std::size_t offset=0;
    for(auto& count:ends){const auto next=offset+count;count=offset;offset=next;}

    std::vector<std::size_t> grouped(batches.size()),other;
    other.reserve(batches.size()-planned);
    for(std::size_t i=0;i<batches.size();++i){
        const auto& batch=batches[i];
        if(batch.static_materials==plan)grouped[ends[batch.material_group]++]=i;
        else other.push_back(i);
    }
    const auto within_group=[&](std::size_t i,std::size_t j){
        const auto& a=batches[i];const auto& b=batches[j];
        const auto first=identity_order(a),second=identity_order(b);
        if(first!=second)return first<second;
        return compare_draw_tail(a,b,i,j);
    };
    const auto compare=[&](std::size_t i,std::size_t j){
        const auto& a=batches[i];const auto& b=batches[j];
        if(a.sky!=b.sky)return a.sky;
        if(a.opaque!=b.opaque)return a.opaque;
        if(a.opaque&&!a.sky){
            const auto band=compare_material_band(a,b);
            return band?band<0:within_group(i,j);
        }
        if(a.draw_order!=b.draw_order)return a.draw_order<b.draw_order;
        return compare_draw_tail(a,b,i,j);
    };
    // Each side sorts its own index array. Both read a frozen world snapshot.
    // No allocations, shared_ptr updates or renderer state writes on core 2.
    auto sort_other=[&]{std::sort(other.begin(),other.end(),compare);};
    const bool parallel=worker && other.size()>=64 && planned>=64;
    if(parallel){
        source_frame_worker_submit(worker,[](void* job){(*static_cast<decltype(sort_other)*>(job))();},&sort_other);
    }
    unsigned visible_groups=0;
    auto start=grouped.begin();
    for(std::size_t group=0;group<group_count;++group){
        const auto end=grouped.begin()+ends[group];
        if(start==end)continue;
        ++visible_groups;
        if(!std::is_sorted(start,end,within_group))std::sort(start,end,within_group);
        start=end;
    }
    if(parallel){
        source_frame_worker_wait(worker);
        if(detail)detail->worker_items+=other.size();
    }else sort_other();
    // Merge from the back into the unused suffix; grouped remains a sorted
    // input even if all dynamic draws come before it.
    auto left=planned,right=other.size(),write=grouped.size();
    while(left&&right){
        if(compare(other[right-1],grouped[left-1]))grouped[--write]=grouped[--left];
        else grouped[--write]=other[--right];
    }
    std::copy_n(other.begin(),right,grouped.begin());
    ApplyPayloadOrder(batches,0,grouped,[](auto& index)->auto&{return index;});
    if(detail){detail->material_meshes+=planned;detail->material_groups+=visible_groups;}
    return true;
}
}

StaticWorldMaterials::StaticWorldMaterials(const SceneAsset& asset,const Nx::CTexDict* dict):materials_(asset.materials.size()){
    material_indices_.reserve(asset.materials.size());
    struct Candidate {WorldDrawState state;std::size_t material;};
    std::vector<Candidate> ordered;
    for(std::size_t i=0;i<asset.materials.size();++i){
        const auto& material=asset.materials[i];
        material_indices_.emplace_back(material.checksum,i);
        const auto passes=FindWorldBasePasses(material);
        if(passes.diffuse>=material.passes.size()||!material.color_wibble.empty()||!std::isfinite(material.draw_order))continue;
        const auto& pass=material.passes[passes.diffuse];
        if(pass.flags&(1|2|(1<<11)))continue;
        WorldDrawState state;
        SetWorldMaterialState(state,material,passes.diffuse,passes.diffuse,passes.opacity,false,true,false);
        if(!state.opaque)continue;
        if(pass.texture){
            if(!dict)continue;
            state.texture=dynamic_cast<Nx::CNativeTexture*>(dict->GetTexture(pass.texture));
            if(!state.texture)continue;
        }
        ordered.push_back({state,i});
        state.no_skater_shadow=true;
        ordered.push_back({state,i});
    }
    std::sort(material_indices_.begin(),material_indices_.end());
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return compare_material_band(a.state,b.state)<0;});
    const WorldDrawState* previous=nullptr;
    for(const auto& item:ordered){
        const auto& state=item.state;
        if(!previous||compare_material_band(*previous,state)!=0)++groups_;
        materials_[item.material][state.no_skater_shadow]={reinterpret_cast<std::uintptr_t>(state.texture),groups_};
        previous=&state;
    }
}
std::size_t StaticWorldMaterials::MaterialIndex(uint32 checksum)const{
    const auto match=std::lower_bound(material_indices_.begin(),material_indices_.end(),std::pair<uint32,uint32>{checksum,0});
    return match!=material_indices_.end()&&match->first==checksum?match->second:materials_.size();
}

void StaticWorldMaterials::Bind(WorldBatch& batch,std::size_t material)const{
    if(batch.sky)return;
    const auto entry=materials_[material][batch.no_skater_shadow];
    // Reloaded dictionaries may give a different pointer order. Such draws
    // participate through the ordinary state comparator for this frame.
    if(entry.group&&entry.texture==reinterpret_cast<std::uintptr_t>(batch.texture)){
        batch.static_materials=this;
        batch.material_group=entry.group-1;
    }
}

bool SortWorldBatches(std::vector<WorldBatch>& batches,bool group_opaque,WorldDetailStats* detail,SourceFrameWorker* worker){
    if(batches.size()<2)return true;
    // The normal fast path has only depth-writing base passes in the
    // opaque world. A non-sortable draw must retain its old position.
    group_opaque=group_opaque&&std::all_of(batches.begin(),batches.end(),[](const auto& b){
        return b.sky||!b.opaque||sortable(b);
    });
    if(!group_opaque){
        StableSortPayloads(batches,0,batches.size(),[](const auto& a,const auto& b){
            if(a.sky!=b.sky)return a.sky;
            if(a.opaque!=b.opaque)return a.opaque;
            if(a.draw_order!=b.draw_order)return a.draw_order<b.draw_order;
            if(a.distance!=b.distance)return a.opaque?a.distance<b.distance:a.distance>b.distance;
            if(a.group!=b.group)return a.group<b.group;
            return a.pass<b.pass;
        });
        return false;
    }
    if(sort_material_groups(batches,detail,worker))return true;
    struct Key {
        WorldMaterialKey material;
        uint64 identity;
        float draw_order,distance;
        std::size_t group,index;
        unsigned pass;
        bool sky,opaque;
        Key(const WorldBatch& b,std::size_t i):material(b),identity(identity_order(b)),
            draw_order(b.draw_order),distance(b.distance),group(b.group),index(i),pass(b.pass),sky(b.sky),opaque(b.opaque){}
    };
    std::vector<Key> keys;keys.reserve(batches.size());
    for(std::size_t i=0;i<batches.size();++i)keys.emplace_back(batches[i],i);
    std::sort(keys.begin(),keys.end(),[](const Key& a,const Key& b){
        if(a.sky!=b.sky)return a.sky;
        if(a.opaque!=b.opaque)return a.opaque;
        if(a.draw_order!=b.draw_order)return a.draw_order<b.draw_order;
        if(a.opaque&&!a.sky){
            const int compare=a.material.compare(b.material);
            if(compare)return compare<0;
            if(a.identity!=b.identity)return a.identity<b.identity;
        }
        if(a.distance!=b.distance)return a.opaque?a.distance<b.distance:a.distance>b.distance;
        if(a.group!=b.group)return a.group<b.group;
        if(a.pass!=b.pass)return a.pass<b.pass;
        return a.index<b.index; // original stable tie order
    });
    ApplyPayloadOrder(batches,0,keys,[](auto& key)->auto&{return key.index;});
    return true;
}
void CoalesceOpaqueWorldBatches(std::vector<WorldBatch>& batches,std::size_t sorted_prefix){
    // Depth-writing opaque base passes can be reordered within the original
    // draw-order band. Keep sky, transparency, projected overlays and shards
    // as barriers, preserving their actual order and blend semantics.
    for(std::size_t begin=0;begin<batches.size();){if(!sortable(batches[begin])){++begin;continue;}
        std::size_t end=begin+1;while(end<batches.size()&&sortable(batches[end])&&batches[end].draw_order==batches[begin].draw_order)++end;
        // Include the boundary pair when later effects extend a sorted band.
        if(end>sorted_prefix&&!std::is_sorted(batches.begin()+std::max(begin,sorted_prefix?sorted_prefix-1:0),batches.begin()+end,state_less))
            StableSortPayloads(batches,begin,end,state_less);
        begin=end;
    }
    std::size_t written=0;
    for(std::size_t read=0;read<batches.size();++read){auto& b=batches[read];
        if(written){auto& previous=batches[written-1];
            if(batchable(previous)&&batchable(b)&&previous.draw_order==b.draw_order&&WorldMaterialKey(previous)==WorldMaterialKey(b)
                &&b.vertices.size()<=32760&&previous.vertices.size()<=32760-b.vertices.size()){
                previous.vertices.insert(previous.vertices.end(),b.vertices.begin(),b.vertices.end());continue;}}
        if(written!=read)batches[written]=std::move(b);++written;
    }batches.resize(written);
}
namespace {
struct ScreenBounds {
    float left=0,right=0,bottom=0,top=0;
    bool valid=false;
    bool disjoint(const ScreenBounds& other)const{
        return valid&&other.valid&&
            (right<other.left||other.right<left||top<other.bottom||other.top<bottom);
    }
};
struct ScreenProjector {
    const WorldView& view;
    float horizontal,vertical;

    ScreenBounds bounds(const StaticWorldMesh& mesh)const{
        const auto& half=mesh.box_half_size;
        const auto delta=mesh.box_center-view.position;
        // Cover roundoff in the composed PICA clip matrix and its float24
        // vertex operations, including cancellation for large world positions.
        float margin=.02f;
        for(unsigned axis=0;axis<3;++axis)
            margin+=.0001f*(std::abs(mesh.box_center[axis])+std::abs(view.position[axis])+half[axis]);
        std::array<float,3> center{},extent{};
        for(unsigned axis=0;axis<3;++axis){
            const auto& normal=view.axes[axis];
            center[axis]=Mth::DotProduct(delta,normal);
            extent[axis]=margin+std::abs(normal[X])*half[X]+std::abs(normal[Y])*half[Y]+std::abs(normal[Z])*half[Z];
        }
        const float near=-center[Z]-extent[Z];
        const float far=-center[Z]+extent[Z];
        // Do not reorder near-plane crossings, where division by depth does
        // not provide finite bounds for the entire source box.
        if(!(near>view.near_clip&&std::isfinite(far)))return {};
        const float inv_near=1/near,inv_far=1/far;
        const float min_x=(center[X]-extent[X])*horizontal,max_x=(center[X]+extent[X])*horizontal;
        const float min_y=(center[Y]-extent[Y])*vertical,max_y=(center[Y]+extent[Y])*vertical;
        // Bound both signs of the full Citro3D stereo shift even when the
        // current frame is mono. Its NDC shift has constant and 1/z terms.
        const float horizontal_pad=.02f+.5f*SOURCE_STEREO_MAX_IOD*(inv_near+horizontal/SOURCE_STEREO_FOCUS);
        return {min_x*(min_x<0?inv_near:inv_far)-horizontal_pad,
                max_x*(max_x>0?inv_near:inv_far)+horizontal_pad,
                min_y*(min_y<0?inv_near:inv_far)-.02f,
                max_y*(max_y>0?inv_near:inv_far)+.02f,
                std::isfinite(min_x)&&std::isfinite(max_x)&&std::isfinite(min_y)&&std::isfinite(max_y)};
    }
};
bool reorderable(const WorldBatch& batch,bool opaque){
    return (!batch.opaque||opaque)&&batch.retained&&base_depth_draw(batch)&&!batch.camera_relative&&!batch.environment
        &&batch.retained->box_half_size[X]>=0;
}
}
// Each node keeps an entire already-batched run. Draws whose conservative
// two-eye rectangles overlap keep their original order, including equal depth.
unsigned GroupStaticMaterialWindows(const WorldView& view,std::vector<WorldBatch>& batches){
    constexpr unsigned limit=24;
    const float horizontal=1/std::tan(view.hfov*float(Mth::PI)/360.f);
    if(!std::isfinite(horizontal)||horizontal<=0||!(view.aspect>0))return 0;
    const ScreenProjector projector{view,horizontal,horizontal*view.aspect};
    struct Node {
        std::size_t first,end,elements;
        unsigned material=0,dependencies=0;
        ScreenBounds rect;
    };
    std::array<Node,limit> nodes;
    std::array<unsigned,limit> order,members;
    std::array<std::optional<WorldMaterialKey>,limit> keys;
    std::array<unsigned,64> slots;
    std::vector<std::size_t> permutation;
    unsigned saved=0;
    for(std::size_t begin=0;begin<batches.size();){
        if(!reorderable(batches[begin],true)){++begin;continue;}
        unsigned count=0,materials=0;
        std::size_t end=begin;
        members.fill(0);
        slots.fill(limit);
        while(count<limit && end<batches.size() && batches[end].opaque==batches[begin].opaque && reorderable(batches[end],true)){
            auto& n=nodes[count];
            n={end,end+1,batches[end].ElementCount()};
            const auto& first=batches[end++];
            while(end<batches.size() && reorderable(batches[end],true) &&
                  append_retained(first,n.elements,batches[end],true)){
                n.elements+=batches[end++].ElementCount();
            }
            n.end=end;
            const WorldMaterialKey key(first,true);
            const auto texture=reinterpret_cast<std::uintptr_t>(first.texture)>>3;
            unsigned slot=(texture^(texture>>11)^(first.blend*37u+first.cutoff*17u+key.flags))&(slots.size()-1);
            while(slots[slot]!=limit && !(*keys[slots[slot]]==key))slot=(slot+1)&(slots.size()-1);
            if(slots[slot]==limit){slots[slot]=materials;keys[materials++]=key;}
            const auto material=slots[slot];
            n.material=material;
            members[material]|=1u<<count;
            ++count;
        }
        if(materials<count && count>=3){
            for(unsigned i=0;i<count;++i){
                auto& n=nodes[i];
                n.rect=projector.bounds(*batches[n.first].retained);
                for(auto k=n.first+1;k<n.end&&n.rect.valid;++k){
                    const auto r=projector.bounds(*batches[k].retained);
                    n.rect.valid=r.valid;
                    n.rect.left=std::min(n.rect.left,r.left);n.rect.right=std::max(n.rect.right,r.right);
                    n.rect.bottom=std::min(n.rect.bottom,r.bottom);n.rect.top=std::max(n.rect.top,r.top);
                }
                for(unsigned j=0;j<i;++j)
                    if(!n.rect.disjoint(nodes[j].rect))n.dependencies|=1u<<j;
            }
            unsigned pending=(1u<<count)-1,ready=0,active=limit;
            for(unsigned pos=0;pos<count;++pos){
                for(unsigned bits=pending&~ready;bits;bits&=bits-1){
                    const unsigned i=__builtin_ctz(bits);
                    if(!(nodes[i].dependencies&pending))ready|=1u<<i;
                }
                // Continue the last material when possible. Otherwise open the
                // material with the largest ready set, breaking ties in source order.
                unsigned choices=active<materials?(ready&members[active]):0;
                if(!choices){
                    unsigned best=0;
                    for(unsigned bits=ready;bits;){
                        const unsigned peers=ready&members[nodes[__builtin_ctz(bits)].material];
                        const unsigned size=__builtin_popcount(peers);
                        if(size>best){best=size;choices=peers;}
                        bits&=~peers;
                    }
                }
                const auto i=unsigned(__builtin_ctz(choices));
                order[pos]=i;
                active=nodes[i].material;
                ready&=~(1u<<i);pending&=~(1u<<i);
            }
            auto calls=[&](bool reordered){
                unsigned draws=0,material=limit;
                std::size_t elements=0;
                auto slices=[](std::size_t n){return unsigned((n+SOURCE_DRAW_SLICE_ELEMENTS-1)/SOURCE_DRAW_SLICE_ELEMENTS);};
                for(unsigned pos=0;pos<count;++pos){
                    const auto& n=nodes[reordered?order[pos]:pos];
                    if(n.material!=material || elements+n.elements>32760){draws+=slices(elements);elements=0;}
                    material=n.material;elements+=n.elements;
                }
                return draws+slices(elements);
            };
            const auto old=calls(false),next=calls(true);
            if(next<old){
                saved+=old-next;
                permutation.clear();
                for(unsigned pos=0;pos<count;++pos){
                    const auto& n=nodes[order[pos]];
                    for(auto i=n.first;i<n.end;++i)permutation.push_back(i-begin);
                }
                ApplyPayloadOrder(batches,begin,permutation,[](auto& i)->auto&{return i;});
            }
        }
        begin=end;
    }
    return saved;
}
unsigned GroupScreenDisjointWorldBatches(const WorldView& view,std::vector<WorldBatch>& batches,bool merge_materials){
    if(batches.size()<2)return 0;
    const float horizontal=1/std::tan(view.hfov*float(Mth::PI)/360.f);
    if(!(std::isfinite(horizontal)&&horizontal>0&&std::isfinite(view.aspect)&&view.aspect>0))return 0;
    const ScreenProjector projector{view,horizontal,horizontal*view.aspect};
    // Only cross draws whose projected bounds are disjoint in both eyes.
    // Opaque trials also keep the source draw-order bands as barriers.
    auto eligible=[&](const WorldBatch& draw){return reorderable(draw,view.group_opaque_disjoint);};
    const auto start=std::find_if(batches.begin(),batches.end(),eligible)-batches.begin();
    if(batches.size()-start<3)return 0;
    struct Entry {
        ScreenBounds bounds;
        std::size_t next,position;
        bool projected=false;
    };
    const auto size=batches.size()-start;
    std::vector<Entry> entries(size);
    std::vector<std::size_t> order(size);
    // The first matching draw never crosses another draw of that material.
    // Its successor is therefore stable even when intervening draws move.
    // Build the chain once instead of scanning the window for every mesh.
    std::size_t slots=1;
    while(slots<size*2)slots*=2;
    std::vector<std::size_t> latest(slots,size);
    std::size_t band=0;
    for(std::size_t i=0;i<size;++i){
        order[i]=i;
        entries[i].position=i;
        entries[i].next=size;
        const auto& draw=batches[start+i];
        if(!eligible(draw)){band=i+1;continue;}
        if(i){
            const auto& previous=batches[start+i-1];
            if(previous.opaque!=draw.opaque ||
               ((!merge_materials||draw.opaque) && draw.draw_order!=previous.draw_order))band=i;
        }
        const WorldMaterialKey material(draw,merge_materials);
        auto hash=reinterpret_cast<std::uintptr_t>(draw.texture)>>3;
        hash^=(hash>>11)^(draw.blend*37u+draw.cutoff*17u+material.flags);
        auto slot=hash&(slots-1);
        for(;;slot=(slot+1)&(slots-1)){
            const auto last=latest[slot];
            if(last==size||last<band)break;
            if(WorldMaterialKey(batches[start+last],merge_materials)==material){
                entries[last].next=i;
                break;
            }
        }
        latest[slot]=i;
    }
    auto bounds=[&](std::size_t index)->const ScreenBounds&{
        auto& entry=entries[index];
        if(!entry.projected){
            entry.bounds=projector.bounds(*batches[start+index].retained);
            entry.projected=true;
        }
        return entry.bounds;
    };
    constexpr std::size_t lookahead=64;
    unsigned moves=0;
    for(std::size_t i=0;i+2<order.size();++i){
        const auto next=entries[order[i]].next;
        if(next==size)continue;
        const auto j=entries[next].position;
        if(j==i+1||j-i>lookahead)continue;
        // Move the later mesh left over a disjoint suffix; if it cannot
        // cross the rest, move the earlier mesh right over that prefix.
        const auto& later=bounds(next);
        auto split=j;
        while(split>i+1&&later.disjoint(bounds(order[split-1])))--split;
        if(split>i+1){
            if(i&&entries[order[i-1]].next==order[i])continue;
            const auto& earlier=bounds(order[i]);
            bool clear=true;
            for(auto k=i+1;k<split;++k){
                if(!earlier.disjoint(bounds(order[k]))){clear=false;break;}
            }
            if(!clear)continue;
            std::rotate(order.begin()+i,order.begin()+i+1,order.begin()+split);
        }
        std::rotate(order.begin()+split,order.begin()+j,order.begin()+j+1);
        for(auto k=i;k<=j;++k)entries[order[k]].position=k;
        ++moves;
    }
    if(!moves)return 0;
    // A partial move can change where the size-limited runs split. Keep the
    // new order only when it saves actual draws under the upload planner's
    // same limits. Count without allocating or packing any vertex data.
    // Include the trailing compatible run from the shared prefix. An opaque
    // or non-retained draw is a hard boundary for the first blended mesh.
    std::size_t begin=start;
    while(begin&&batches[begin-1].retained&&!batches[begin-1].opaque&&base_depth_draw(batches[begin-1]))--begin;
    auto count_runs=[&](bool grouped){
        const WorldBatch* first=nullptr;
        std::size_t elements=0;
        unsigned count=0;
        for(std::size_t i=begin;i<batches.size();++i){
            const auto& batch=batches[grouped&&i>=start?start+order[i-start]:i];
            if(!first||!append_retained(*first,elements,batch,merge_materials)){
                first=&batch;
                elements=0;
                ++count;
            }
            elements+=batch.ElementCount();
        }
        return count;
    };
    const auto original=count_runs(false);
    const auto grouped=count_runs(true);
    if(grouped>=original)return 0;
    ApplyPayloadOrder(batches,start,order,[](auto& index)->auto&{return index;});
    return original-grouped;
}
static void build_world_draw_runs(const std::vector<WorldBatch>& batches,bool merge_materials,bool ordered_streams,
                                 std::vector<WorldDrawRun>& runs){
    runs.clear();
    runs.reserve(batches.size());
    for(const auto& batch:batches){
        const auto count=batch.VertexCount(),elements=batch.ElementCount();
        if(!runs.empty()){
            auto& previous=runs.back();
            // The input is already ordered. Append only adjacent retained
            // meshes with the same state; keep transparent triangles in
            // their original sequence within the combined index stream.
            if(append_retained(*previous.first,previous.element_count,batch,merge_materials)){
                ++previous.retained_count;
                previous.count+=count;
                previous.element_count+=elements;
                previous.indexed|=bool(batch.indices);
                continue;
            }
            // Moving meshes already share the frame stream. Submit compatible
            // neighbours as one draw, keeping their exact triangle order and
            // the same indexed/array mode. Do not cross pass or blend bands.
            const auto& first=*previous.first;
            const bool indexed_pair=previous.indexed&&batch.GpuIndexed();
            const bool array_pair=!previous.indexed&&!batch.GpuIndexed()&&
                (ordered_streams||first.expand_on_upload||batch.expand_on_upload);
            const std::size_t limit=indexed_pair?SOURCE_DRAW_SLICE_ELEMENTS:32760;
            if(merge_materials&&(indexed_pair||array_pair)&&!first.retained&&!batch.retained&&!first.skin_mesh&&!batch.skin_mesh&&
               first.opaque==batch.opaque&&(ordered_streams||first.opaque)&&base_depth_draw(first)&&base_depth_draw(batch)&&
               !first.environment&&!batch.environment&&first.draw_order==batch.draw_order&&
               WorldMaterialKey(first,indexed_pair)==WorldMaterialKey(batch,indexed_pair)&&elements<=limit&&
               previous.element_count<=limit-elements&&count<=65536&&previous.count<=65536-count){
                ++previous.transient_count;
                previous.count+=count;
                previous.element_count+=elements;
                continue;
            }
        }
        runs.push_back({&batch,count,batch.retained?1u:0u,elements,batch.GpuIndexed()});
    }
}
bool MatchesStaticRun(const std::vector<std::shared_ptr<const StaticWorldMesh>>& resident,
    bool resident_indexed,const WorldDrawRun& run,bool allow_prefix){
    if(!run.retained_count||resident_indexed!=run.indexed||resident.size()<run.retained_count)return false;
    if(resident.size()!=run.retained_count&&(!allow_prefix||!sortable(*run.first)))return false;
    for(std::size_t j=0;j<run.retained_count;++j){
        const auto& b=run.first[j];
        if(!b.retained||resident[j]!=b.retained||b.retained->owner.expired()||
           b.indices!=b.retained->indices)return false;
        if(resident.size()!=run.retained_count&&b.material!=run.first->material)return false;
    }
    return true;
}
std::vector<WorldDrawRun> BuildWorldDrawRuns(const std::vector<WorldBatch>& batches,bool merge_materials,bool ordered_streams){
    std::vector<WorldDrawRun> runs;
    build_world_draw_runs(batches,merge_materials,ordered_streams,runs);
    return runs;
}
namespace {
SourceRenderVertex scale_uv(SourceRenderVertex v,float us,float lo,float hi){
    v.u*=us;
    v.v=lo?(1-v.v)*lo:v.v*hi;
    return v;
}
bool WriteRunVertices(const WorldDrawRun& run,float us,float lo,float hi,SourceRenderVertex* dst){
    const bool identity=us==1&&lo==0&&hi==1;
    for(std::size_t i=0;i<run.BatchCount();++i){
        const auto& batch=run.first[i];
        const auto& data=batch.VertexData();
        if(batch.expand_on_upload){
            if(run.indexed||!batch.indices)return false;
            // Texture orientation is uniform over a material run.
            if(identity){
                for(auto vertex:*batch.indices){
                    if(vertex>=data.size())return false;
                    *dst++=data[vertex];
                }
            }else if(us==1.f&&lo==1.f){
                for(auto vertex:*batch.indices){
                    if(vertex>=data.size())return false;
                    auto v=data[vertex];v.v=1.f-v.v;*dst++=v;
                }
            }else for(auto vertex:*batch.indices){
                if(vertex>=data.size())return false;
                *dst++=scale_uv(data[vertex],us,lo,hi);
            }
        }else if(identity)dst=std::copy(data.begin(),data.end(),dst);
        else if(us==1.f&&lo==1.f){
            for(auto v:data){v.v=1.f-v.v;*dst++=v;}
        }else for(auto v:data)*dst++=scale_uv(v,us,lo,hi);
    }
    return true;
}
std::size_t align_stream(std::size_t count,std::size_t alignment){
    return (count+alignment-1)&~(alignment-1);
}
}
void AppendWorldDrawRun(const WorldDrawRun& run,float us,float lo,float hi,std::vector<SourceRenderVertex>& vertices){
    const auto offset=vertices.size();
    vertices.resize(offset+run.count);
    if(run.count)Dbg_Assert(WriteRunVertices(run,us,lo,hi,vertices.data()+offset));
}

bool WorldStreamWriter::Append(const WorldDrawRun& run,float us,float v0,float v1,SourceDrawPacket& packet){
    const auto vertex_first=run.indexed?align_stream(vertex_count,4):vertex_count;
    const auto index_first=run.indexed?align_stream(index_count,8):index_count;
    const auto needed_indices=run.IndexCount();
    if(!run.count||vertex_first>vertex_capacity||run.count>vertex_capacity-vertex_first
        ||index_first>index_capacity||needed_indices>index_capacity-index_first
        ||(run.indexed&&(run.count>65536||range_count>=range_capacity||range_count>=65536))){
        errno=EINVAL;
        return false;
    }
    if(run.source_strip && (!run.indexed || !run.first || run.retained_count!=1 || !run.first->retained ||
                            !run.first->indices || run.first->retained->source_strip.size()!=needed_indices || needed_indices<3)){
        errno=EINVAL;
        return false;
    }
    if(run.indexed){
        std::fill(indices+index_count,indices+index_first,uint16(0));
        auto index=index_first;
        unsigned offset=0;
        for(std::size_t i=0;i<run.BatchCount();++i){
            const auto& batch=run.first[i];
            const auto count=batch.VertexData().size();
            if(batch.indices){
                if(run.BatchCount()==1){
                    // The GPU validates every member of the resulting range
                    // against this exact mesh. There is no sub-mesh boundary.
                    const auto& sequence=run.source_strip?batch.retained->source_strip:*batch.indices;
                    std::copy(sequence.begin(),sequence.end(),indices+index);
                    index+=sequence.size();
                }else{
                    for(auto local:*batch.indices){
                        if(local>=count){errno=EINVAL;return false;}
                        indices[index++]=uint16(offset+local);
                    }
                }
            }else{
                for(std::size_t j=0;j<count;++j)indices[index++]=uint16(offset+j);
            }
            offset+=count;
        }
        // The GPU binds this draw's own vertex base. Its indices stay local
        // even when the combined stream has more than 65536 vertices.
        ranges[range_count++]={unsigned(vertex_first),unsigned(run.count),
            unsigned(index_first),unsigned(needed_indices),unsigned(vertex_first),unsigned(run.source_strip)};
        index_count=index;
    }
    std::fill(vertices+vertex_count,vertices+vertex_first,SourceRenderVertex{});
    if(!WriteRunVertices(run,us,v0,v1,vertices+vertex_first)){errno=EINVAL;return false;}
    vertex_count=vertex_first+run.count;
    packet.first=vertex_first;
    packet.count=run.element_count;
    packet.index_range=run.indexed?range_count:0;
    return true;
}
bool AppendWorldStreamRun(const WorldDrawRun& run,float us,float v0,float v1,std::vector<SourceRenderVertex>& vertices,
    std::vector<uint16>& indices,std::vector<SourceIndexRange>& ranges,SourceDrawPacket& packet){
    const auto v=vertices.size(),i=indices.size(),r=ranges.size();
    vertices.resize((run.indexed?align_stream(v,4):v)+run.count);
    indices.resize((run.indexed?align_stream(i,8):i)+run.IndexCount());
    ranges.resize(r+run.indexed);
    WorldStreamWriter writer{vertices.data(),indices.data(),ranges.data(),vertices.size(),indices.size(),ranges.size(),v,i,r};
    if(writer.Append(run,us,v0,v1,packet))return writer.Full();
    vertices.resize(v);
    indices.resize(i);
    ranges.resize(r);
    return false;
}
std::size_t CityStaticWorldBudget(std::size_t resident_bytes,std::size_t linear_free,std::size_t arena_free,bool keep_turn_buffers){
    // Residents hold both the GPU upload and the original immutable mesh.
    // Leave room in both pools for textures, animation and changing geometry.
    constexpr std::size_t mib=1024*1024;
    const std::size_t maximum=(keep_turn_buffers?10:8)*mib;
    const auto headroom=std::min(std::int64_t(linear_free)-14*1024*1024,
                                 std::int64_t(arena_free)-8*1024*1024);
    const auto target=std::int64_t(std::min(resident_bytes,maximum))+headroom;
    return std::clamp<std::int64_t>(target,StaticWorldVertexBudget,maximum);
}
bool RetainWorldVertexBuffer(std::size_t capacity,std::size_t needed){
    return needed&&capacity>=needed&&capacity-needed<=needed;
}
void PrepareWorldMaterial(const WorldDrawState& b,SourceDrawPacket& packet){
    packet.original_blend=1;packet.blend_mode=b.blend;packet.fixed_alpha=b.fixed_alpha;packet.u_address=b.u_address;packet.v_address=b.v_address;
    packet.nearest=0;packet.z_bias=b.z_bias;packet.alpha_cutoff=b.cutoff;packet.cull=b.cull;packet.depth_write=b.depth_write;packet.modulate_2x=1;
    // Retail PC aPs11TexT0MulR0 doubles vertex alpha * texture alpha.
    // IVA supplies its own constant alpha; its replacement vertex is 255.
    packet.alpha_2x=b.mesh_uv&&b.texture&&!b.ignore_vertex_alpha;
}
NativeWorldBatch::~NativeWorldBatch(){Release();}
void NativeWorldBatch::evict(Residency::iterator at){
    Dbg_Assert(at!=resident_.end());auto bytes=source_graphics_stream_bytes(at->second.capacity,at->second.index_capacity);
    Dbg_Assert(stats_.bytes>=bytes);if(at->second.gpu)Dbg_Assert(source_graphics_mesh_delete(at->second.gpu)==0);
    if(!recent_runs_.empty()){
        auto& hint=recent_runs_[at->first%recent_runs_.size()];
        if(hint==at)hint=resident_.end();
    }
    if(next_owner_check_==at)++next_owner_check_;
    stats_.bytes-=bytes;++stats_.evictions;resident_.erase(at);
}
void NativeWorldBatch::Release(){
    Dbg_Assert(source_thread_is_controller());if(streaming_.gpu)Dbg_Assert(source_graphics_mesh_delete(streaming_.gpu)==0);streaming_={};recent_runs_.clear();while(!resident_.empty())evict(resident_.begin());runs_.clear();choices_.clear();eviction_candidates_.clear();decltype(observed_)().swap(observed_);stats_={};frame_=0;
}
bool NativeWorldBatch::Prepare(const WorldView& view,const std::vector<WorldBatch>& batches,std::vector<SourceDrawPacket>& out){
    Dbg_Assert(source_thread_is_controller());if(!GraphicsReady()){errno=ENODEV;return false;}if(batches.size()>65536){errno=EOVERFLOW;return false;}
    std::size_t static_budget=StaticWorldVertexBudget;
#ifdef __3DS__
    if(!source_application_os_is_new_3ds())static_budget=1024*1024;
    else if((view.local_arrays||view.city_stereo_fast)&&view.city_prop_radius>0&&view.mesh_retention){
        const auto arena_free=Mem::Manager::sHandle().BottomUpHeap()->ParentRegion()->MemAvailable();
        static_budget=CityStaticWorldBudget(stats_.bytes,source_graphics_linear_free(),std::max(0,arena_free),view.keep_turn_buffers);
    }
#endif
    stats_.budget=static_budget;
    ++frame_;
    stats_.prefix_hits=stats_.hits=stats_.uploads=stats_.evictions=stats_.streamed=stats_.indexed_meshes=stats_.static_indexed_runs=stats_.stream_indices=0;
    {
        NativeHeapScope storage;
        build_world_draw_runs(batches,true,view.city_stereo_fast&&view.local_arrays,runs_);
    }
    stats_.strip_runs=stats_.strip_saved_indices=0;
    // Preserve all material batching. Only substitute a strip after grouping,
    // when one retained mesh already constitutes a complete indexed draw.
    if(view.use_source_strips)for(auto& run:runs_){
        if(run.retained_count!=1 || !run.indexed)continue;
        const auto size=run.first->retained->source_strip.size();
        if(!size)continue;
        ++stats_.strip_runs;
        stats_.strip_saved_indices+=run.element_count-size;
        run.element_count=size;
        run.source_strip=true;
    }
    const auto& runs=runs_;
    auto& choices=choices_;
    {NativeHeapScope storage;choices.assign(runs.size(),{resident_.end()});}
    std::size_t wanted=stats_.bytes;
    const bool shrinking=stats_.bytes>static_budget;
    if(view.city_stereo_fast&&recent_runs_.empty()){
        NativeHeapScope storage;
        recent_runs_.assign(2048,resident_.end());
    }
    std::size_t pinned=0, deferred_bytes=0;
    // Only a hint for admission. Mesh identity, ownership, texture scale and
    // GPU span are still validated by the ordinary resident lookup.
    const bool admission=view.defer_cold_runs && stats_.bytes>=static_budget*3/4;
    if(admission && observed_.empty()){
        NativeHeapScope storage;
        observed_.resize(2048);
    }
    for(std::size_t i=0;i<runs.size();++i){
        const auto& run=runs[i];
        if(!run.count||!run.element_count||(!run.source_strip&&run.element_count%3)){errno=EINVAL;return false;}
        if(!run.retained_count)continue;
        auto& choice=choices[i];
        const auto& batch=*run.first;
        if(auto* texture=batch.texture;texture&&texture->GPUResident()){
            choice.u=texture->UScale();
            choice.v0=batch.mesh_uv?texture->ModelGPUV(0):texture->GPUV(0);
            choice.v1=batch.mesh_uv?texture->ModelGPUV(1):texture->GPUV(1);
        }
        // A camera turn may produce A, then A+B, then A again. Keep both
        // immutable buffers if the budget allows it. The hash finds candidates;
        // the ordered ownership and UV checks below decide equality.
        choice.key=batch.retained->serial;
        const bool sampled=!batch.texture||batch.texture->GPUResident();
        auto accepts=[&](const Resident& entry,bool allow_prefix){
            return entry.source_strip==run.source_strip && entry.u_scale==choice.u&&entry.v0==choice.v0&&entry.v1==choice.v1&&
                   MatchesStaticRun(entry.meshes,entry.index_capacity!=0,run,allow_prefix)&&
                   (!shrinking||entry.last_seen==frame_||
                    source_graphics_stream_bytes(entry.capacity,entry.index_capacity)<=static_budget-pinned);
        };
        if(sampled&&view.city_stereo_fast){
            auto at=recent_runs_[choice.key%recent_runs_.size()];
            if(at!=resident_.end()&&at->first==choice.key&&accepts(at->second,false))
                choice.resident=at;
        }
        if(sampled&&choice.resident==resident_.end()){
            const auto [first,last]=resident_.equal_range(choice.key);
            for(auto at=first;at!=last;++at){
                const auto& entry=at->second;
                if(!accepts(entry,view.reuse_static_prefix))continue;
                if(choice.resident==resident_.end()||entry.capacity<choice.resident->second.capacity)
                    choice.resident=at;
                if(entry.meshes.size()==run.retained_count)break;
            }
        }
        if(choice.resident!=resident_.end()){
            auto& entry=choice.resident->second;
            if(shrinking&&entry.last_seen!=frame_)pinned+=source_graphics_stream_bytes(entry.capacity,entry.index_capacity);
            entry.last_seen=frame_;
            if(entry.meshes.size()>run.retained_count)++stats_.prefix_hits;
        }
        if(choice.resident==resident_.end()){
            const auto bytes=source_graphics_stream_bytes(run.count,run.IndexCount());
            if(admission && sortable(batch)){
                const uint64 tail=run.first[run.retained_count-1].retained->serial;
                auto& seen=observed_[(choice.key*2654435761u ^ tail*97u ^ run.retained_count) % observed_.size()];
                const bool warm=seen.serial==choice.key && seen.tail==tail && unsigned(frame_)-seen.frame<=8;
                seen={choice.key,tail,unsigned(frame_)};
                if(!warm && bytes<=65536-deferred_bytes){
                    choice.deferred=true;
                    deferred_bytes+=bytes;
                }
            }
            if(!choice.deferred)wanted+=bytes;
        }
    }
    // Visible entries validated every owner above. With the larger Moscow
    // cache, spread checks of off-screen entries over 16 frames. Memory
    // pressure still reclaims cold buffers in this frame; scene unload
    // releases the whole cache independently of this maintenance cursor.
    const bool stagger=static_budget>StaticWorldVertexBudget&&!shrinking;
    const auto checks=stagger?(resident_.size()+15)/16:resident_.size();
    if(!stagger)next_owner_check_=resident_.begin();
    for(std::size_t n=0;n<checks&&!resident_.empty();++n){
        if(next_owner_check_==resident_.end())next_owner_check_=resident_.begin();
        auto at=next_owner_check_++;
        const auto& entry=at->second;
        // In Manhattan a turn back to a busy street may be more than 120
        // frames away. Keep a still-owned buffer until space is needed.
        const bool aged_out=!view.keep_turn_buffers && frame_-entry.last_seen>120;
        if(entry.last_seen!=frame_&&(aged_out||
           std::any_of(entry.meshes.begin(),entry.meshes.end(),[](const auto& mesh){return mesh->owner.expired();}))){
            wanted-=source_graphics_stream_bytes(entry.capacity,entry.index_capacity);
            evict(at);
        }
    }
    if(wanted>static_budget){
        auto& candidates=eviction_candidates_;
        candidates.clear();
        NativeHeapScope storage;
        for(auto at=resident_.begin();at!=resident_.end();++at)
            if(at->second.last_seen!=frame_)candidates.push_back(at);
        // Usually a turn needs only a few of the hundreds of cold buffers.
        // Heapify once and order just the entries we actually reclaim.
        const auto newer=[](auto a,auto b){
            return std::tie(a->second.last_seen,a->first)>std::tie(b->second.last_seen,b->first);
        };
        std::make_heap(candidates.begin(),candidates.end(),newer);
        while(wanted>static_budget&&!candidates.empty()){
            std::pop_heap(candidates.begin(),candidates.end(),newer);
            const auto at=candidates.back();
            candidates.pop_back();
            wanted-=source_graphics_stream_bytes(at->second.capacity,at->second.index_capacity);
            evict(at);
        }
    }
    std::size_t planned=stats_.bytes;
    for(std::size_t i=0;i<runs.size();++i){
        const auto& run=runs[i];
        if(!run.retained_count||choices[i].deferred)continue;
        if(choices[i].resident!=resident_.end())choices[i].keep=true;
        else if(auto bytes=source_graphics_stream_bytes(run.count,run.IndexCount());planned<=static_budget&&bytes<=static_budget-planned){
            choices[i].keep=true;
            planned+=bytes;
        }
    }
    std::size_t dynamic_vertices=0,dynamic_indices=0,dynamic_ranges=0;
    for(std::size_t i=0;i<runs.size();++i)if(!choices[i].keep&&!runs[i].first->skin_mesh){
        const auto& run=runs[i];
        if(run.indexed){
            dynamic_vertices=align_stream(dynamic_vertices,4);
            dynamic_indices=align_stream(dynamic_indices,8)+run.IndexCount();
            ++dynamic_ranges;
        }
        dynamic_vertices+=run.count;
    }
    auto retain=[](std::size_t capacity,std::size_t needed){
        return needed?RetainWorldVertexBuffer(capacity,needed):capacity==0;
    };
    if(streaming_.gpu&&(!RetainWorldVertexBuffer(streaming_.capacity,dynamic_vertices)||!retain(streaming_.index_capacity,dynamic_indices))){
        Dbg_Assert(source_graphics_mesh_delete(streaming_.gpu)==0);streaming_={};}
    SourceDrawPacket normal{},unfogged{},relative{};normal.width=view.width;normal.height=view.height;normal.x=view.x;normal.y=view.y;
    normal.hfov=view.hfov;normal.aspect=view.aspect;normal.near_clip=view.near_clip;normal.far_clip=view.far_clip;normal.focal_distance=SOURCE_STEREO_FOCUS;
    CameraView(view.axes,view.position,normal.model_view);unfogged=normal;ApplyNativeFog(normal,false);ApplyNativeFog(unfogged,true);relative=unfogged;CameraView(view.axes,view.position,relative.model_view,true);
    auto& packets=packets_;
    packets.clear();
    packets.reserve(runs.size());
    for(std::size_t i=0;i<runs.size();++i){const auto& run=runs[i];const auto& b=*run.first;SourceDrawPacket packet=b.camera_relative?relative:(b.sky||b.fog_disabled?unfogged:normal);
        PrepareWorldMaterial(b,packet);
        // Keep the original sky preamble in the sequence, including any
        // blended sky layers that precede the depth-writing world.
        const bool depth_opaque=b.opaque&&base_depth_draw(b)&&!b.environment;
        packet.early_opaque=b.sky?1:(view.vancouver_early?(depth_opaque&&b.retained?2:0):unsigned(depth_opaque));
        if(b.texture){packet.texture=b.texture->GPU();if(!packet.texture)return false;}
        const auto& choice=choices[i];
        if(b.skin_mesh){
            packet.mesh=b.skin_mesh;
            packet.index_range=1;
            packet.count=run.element_count;
            packet.skin_u_scale=b.texture?b.texture->UScale():1;
            const auto gpu_v=[&](float v){return b.mesh_uv?b.texture->ModelGPUV(v):b.texture->GPUV(v);};
            packet.skin_v_offset=b.texture?gpu_v(0):0;
            packet.skin_v_scale=b.texture?gpu_v(1)-packet.skin_v_offset:1;
            float camera[16];
            std::copy_n(packet.model_view,16,camera);
            const auto& model=*b.skin_transform;
            for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){
                float result=0;
                for(unsigned k=0;k<4;++k)result+=camera[r*4+k]*model[c][k];
                packet.model_view[r*4+c]=result;
            }
        }else if(choice.keep){
            auto at=choice.resident;
            if(at==resident_.end()){
                // A first upload can determine a different padding and UV
                // scale (including a fallback after compressed allocation).
                const auto* texture=b.texture;
                const float u=texture?texture->UScale():1;
                const float v0=texture?(b.mesh_uv?texture->ModelGPUV(0):texture->GPUV(0)):0;
                const float v1=texture?(b.mesh_uv?texture->ModelGPUV(1):texture->GPUV(1)):1;
                struct Upload {
                    const WorldDrawRun& run;
                    float us,v0,v1;
                    SourceDrawPacket& packet;
                } upload{run,u,v0,v1,packet};
                auto write=[](void* context,SourceRenderVertex* vertices,uint16* indices,SourceIndexRange* ranges){
                    const auto& data=*static_cast<Upload*>(context);
                    WorldStreamWriter writer{vertices,indices,ranges,data.run.count,data.run.IndexCount(),data.run.indexed};
                    return writer.Append(data.run,data.us,data.v0,data.v1,data.packet)&&writer.Full()?0:-1;
                };
                SourceGpuMesh* gpu=nullptr;
                if(source_graphics_stream_write(&gpu,run.count,run.IndexCount(),run.indexed,write,&upload))return false;
                NativeHeapScope storage;
                Resident item;
                item.source_strip=run.source_strip;
                item.gpu=gpu;item.capacity=run.count;item.index_capacity=run.IndexCount();
                item.u_scale=u;item.v0=v0;item.v1=v1;
                item.meshes.reserve(run.retained_count);
                for(std::size_t j=0;j<run.retained_count;++j)item.meshes.push_back(run.first[j].retained);
                at=resident_.emplace(choice.key,std::move(item));
                stats_.bytes+=source_graphics_stream_bytes(run.count,run.IndexCount());
                ++stats_.uploads;
                if(view.upload_trace){
                    // Compare ordered immutable mesh IDs and layout, not GPU addresses.
                    uint64 hash=14695981039346656037ull;
                    auto mix=[&](uint64 id){hash=(hash^id)*1099511628211ull;};
                    mix(run.count);mix(run.IndexCount());mix(run.source_strip);mix(run.retained_count);
                    for(const float scale:{u,v0,v1}){
                        uint32 bits;std::memcpy(&bits,&scale,sizeof(bits));mix(bits);
                    }
                    for(const auto& part:at->second.meshes)mix(part->serial);
                    view.upload_trace->push_back(hash);
                }
            }else{
                packet.index_range=run.indexed?1:0;
                packet.count=run.element_count;
                ++stats_.hits;
            }
            if(run.indexed)++stats_.static_indexed_runs;
            if(view.city_stereo_fast&&at->second.meshes.size()==run.retained_count)
                recent_runs_[choice.key%recent_runs_.size()]=at;
            at->second.last_seen=frame_;
            packet.mesh=at->second.gpu;
        }else{
            ++stats_.streamed;
            if(run.indexed)++stats_.indexed_meshes;
        }packets.push_back(packet);
    }
    // Pack changing draws straight into the fenced GPU buffer. The original
    // game heap never holds a second copy of all visible vertices.
    if(dynamic_vertices){
        struct Upload {
            const std::vector<WorldDrawRun>& runs;
            std::vector<SourceDrawPacket>& packets;
            std::size_t vertices,indices,ranges;
        } upload{runs,packets,dynamic_vertices,dynamic_indices,dynamic_ranges};
        auto write=[](void* context,SourceRenderVertex* vertices,uint16* indices,SourceIndexRange* ranges){
            const auto& data=*static_cast<Upload*>(context);
            WorldStreamWriter writer{vertices,indices,ranges,data.vertices,data.indices,data.ranges};
            for(std::size_t i=0;i<data.runs.size();++i){
                auto& packet=data.packets[i];
                if(packet.mesh)continue;
                const auto& run=data.runs[i];
                const auto& b=*run.first;
                auto gpu_v=[&](float v){return b.mesh_uv?b.texture->ModelGPUV(v):b.texture->GPUV(v);};
                const float us=b.texture?b.texture->UScale():1,v0=b.texture?gpu_v(0):0,v1=b.texture?gpu_v(1):1;
                if(!writer.Append(run,us,v0,v1,packet))return -1;
            }
            return writer.Full()?0:-1;
        };
        const bool creating=!streaming_.gpu;
        // Camera motion changes the stream length almost every frame. Give
        // dense stereo scenes bounded spare space without writing or flushing it.
        const auto capacity=[&](std::size_t used){
            return view.city_prop_radius>0?used+std::min(used/8,std::size_t(2048)):used;
        };
        SourceStreamCapacity space{capacity(dynamic_vertices),capacity(dynamic_indices)};
        if(source_graphics_stream_write_reserved(&streaming_.gpu,dynamic_vertices,dynamic_indices,dynamic_ranges,write,&upload,&space))return false;
        if(creating){streaming_.capacity=space.vertices;streaming_.index_capacity=space.indices;}
        for(auto& packet:packets)if(!packet.mesh)packet.mesh=streaming_.gpu;
    }
    stats_.meshes=resident_.size();stats_.draws=packets.size();stats_.stream_vertices=dynamic_vertices;stats_.stream_indices=dynamic_indices;stats_.stream_uploads=dynamic_vertices?1:0;Dbg_Assert(stats_.bytes<=static_budget);out.swap(packets);return true;
}
}
