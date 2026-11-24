#include <source_port/native_texture.h>
#include <source_port/files.h>
#include <source_port/application_os.h>
#include <source_port/graphics.h>
#include <source_port/world_texture_bank.h>
#include <source_port/native_projection.h>
#include <source_port/thread_os.h>
#include <source_port/diagnostics.h>
#include <source_port/memory.h>
#include <source_port/render_options.h>
#include <gfx/nxtexman.h>
#include <gfx/gfxutils.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <strings.h>
namespace {
uint64 texture_generation=1;
unsigned pot(unsigned n){unsigned p=8;while(p<n&&p<1024)p*=2;return p;}
bool compact_city_dictionary(const char* name) {
    if (!name || !source_application_os_is_old_3ds()) return false;
    std::string path(name);
    std::replace(path.begin(), path.end(), '/', '\\');
    if(strncasecmp(path.c_str(),"levels\\",7)!=0)return false;
    const auto end=path.find('\\',7);
    if(end==std::string::npos)return false;
    const auto level=path.substr(7,end-7);
    return strcasecmp(level.c_str(),"mainmenu") && !strcasecmp(path.c_str()+end+1,(level+".tex.xbx").c_str());
}
void controller(){Dbg_Assert(source_thread_is_controller());}
uint8 byte(float v){return uint8(std::lround(std::clamp(v,0.f,255.f)));}
std::size_t drop_base_mip(SourcePort::TextureCacheEntry& entry) {
    const auto bytes=SourcePort::NativeTextureLevelBytes(entry.format,entry.width,entry.height);
    entry.offset+=bytes;
    entry.bytes-=bytes;
    entry.width/=2;
    entry.height/=2;
    entry.sample_width=std::max(1u,entry.sample_width/2);
    entry.sample_height=std::max(1u,entry.sample_height/2);
    --entry.levels;
    return bytes;
}
}
namespace SourcePort {
uint64 WorldTextureGeneration(){return texture_generation;}
}
namespace Nx {
CNativeTexture::CNativeTexture(){++texture_generation;SetPerm(true);m_checksum=0;}
CNativeTexture::~CNativeTexture(){++texture_generation;SourcePort::ForgetProjectionTexture(this);plat_remove_from_vram();}
bool CNativeTexture::Assign(SourcePort::TextureAsset asset){
    controller();if((!asset.bytes&&!asset.disk)||asset.levels.empty())return false;
    if(!plat_remove_from_vram())return false;asset_=std::move(asset);m_checksum=asset_.checksum;
    compressed_group_.reset();
    disk_cache_ = SourcePort::TextureCacheFile::None;
    std::vector<uint8>().swap(pixels_);std::vector<uint8>().swap(original_);dirty_=false;transparent_=false;transparency_known_=false;cpu_read_=false;target_=false;target_depth_=0;u_scale_=v_scale_=1.f;++revision_;
    if (source_render_options()->performance && (asset_.image || asset_.font_palette)) {
        const auto kind = asset_.font_palette ? SourcePort::TextureSource::Font : SourcePort::TextureSource::Image;
        std::vector<SourcePort::TextureCacheEntry> entries;
        if (source_application_os_is_old_3ds()) {
            const auto cache = SourcePort::FindDiskTextureCacheGroup(SourcePort::WorldTextureChecksum(asset_.bytes->data(), asset_.bytes->size()), asset_.bytes->size(), kind, entries);
            if (cache != SourcePort::TextureCacheFile::None && entries.size() == 1 && entries.front().key == asset_.checksum) {
                disk_cache_ = cache;
                compressed_entry_ = entries.front();
            }
        } else {
            auto group = SourcePort::FindTextureCacheGroup(*asset_.bytes, kind);
            if (group && SourcePort::ReadTextureCacheGroup(*group, entries) &&
                entries.size() == 1 && entries.front().key == asset_.checksum) {
                compressed_entry_ = entries.front();
                compressed_group_ = std::move(group);
            }
        }
    }
    return true;
}
bool CNativeTexture::CreateRenderTarget(unsigned width,unsigned height,unsigned depth,unsigned z_depth){
    if(width<8||height<8||width>1024||height>1024||(width&(width-1))||(height&(height-1))||(depth!=16&&depth!=32)||(z_depth!=0&&z_depth!=16&&z_depth!=24)){errno=EINVAL;return false;}
    SourcePort::RootHeapScope storage; // CPU replacement for PC render-target GPU memory
    SourcePort::TextureAsset asset;asset.width=asset.actual_width=width;asset.height=asset.actual_height=height;asset.depth=32;asset.swizzled=false;
    asset.bytes=std::make_shared<SourcePort::ResourceBytes>(std::size_t(width)*height*4,255);asset.levels.push_back({width,height,0,std::size_t(width)*height*4});
    if(!Assign(std::move(asset)))return false;target_=true;target_depth_=z_depth;return true;
}
bool CNativeTexture::WriteRenderTarget(const std::vector<uint8>& rgba){
    controller();if(!target_||rgba.size()!=std::size_t(asset_.width)*asset_.height*4){errno=EINVAL;return false;}
    SourcePort::RootHeapScope storage;
    // Keep a real CPU backing for Generate32BitImage and a fenced GPU texture.
    // The shadow target rasterizer shares the actual posed Nx geometry.
    if(gpu_){std::vector<uint8> tiled;if(!SourcePort::TileRGBA(rgba.data(),asset_.width,asset_.height,tiled)||source_graphics_texture_update(gpu_,tiled.data(),tiled.size()))return false;}
    pixels_=rgba;dirty_=true;transparent_=false;for(std::size_t i=3;i<rgba.size();i+=4)transparent_|=rgba[i]!=255;transparency_known_=true;++revision_;return true;
}
bool CNativeTexture::decode(){
    controller();if(!pixels_.empty())return true;
    if(!SourcePort::DecodeTextureLevel(asset_,0,pixels_))return false;
    transparent_=false;for(std::size_t i=3;i<pixels_.size();i+=4)transparent_|=pixels_[i]!=255;transparency_known_=true;return true;
}
bool CNativeTexture::Pixels(const std::vector<uint8>** pixels){if(!pixels||!decode())return false;cpu_read_=true;*pixels=&pixels_;return true;}
std::size_t CNativeTexture::ReleaseUploadScratch(){
    controller();if(target_||dirty_||cpu_read_||!original_.empty())return 0;
    auto bytes=pixels_.capacity();std::vector<uint8>().swap(pixels_);return bytes;
}
bool CNativeTexture::upload(){
    controller();if(gpu_)return true;if(!SourcePort::GraphicsReady())return false;
    const auto configured_limit=source_render_options()->texture_limit;
    const bool old_world=world_mips_ && source_application_os_is_old_3ds() && !target_ && !asset_.image && !asset_.font_palette;
    const unsigned limit=old_world && configured_limit ? std::min(64u,configured_limit) : configured_limit;
    if ((compressed_group_ || disk_cache_ != SourcePort::TextureCacheFile::None) && !dirty_ && !target_) {
        SourcePort::ResourceBytes disk_payload;
        auto entry = compressed_entry_;
        while(old_world && limit && entry.levels>1 && std::max(entry.width,entry.height)>limit)
            drop_base_mip(entry);
        const auto* tiled = compressed_group_ ? compressed_group_->data() + entry.offset : nullptr;
        if (!tiled && SourcePort::ReadDiskTexturePayload(disk_cache_, entry, disk_payload)) tiled = disk_payload.data();
        while (tiled) {
            gpu_ = source_graphics_texture_create_mips(entry.width, entry.height, entry.format,
                entry.levels, tiled, entry.bytes);
            if (gpu_) {
                u_scale_=float(entry.sample_width)/entry.width;
                v_scale_=float(entry.sample_height)/entry.height;
                if (asset_.image || asset_.font_palette) compressed_group_.reset();
                ReleaseUploadScratch();
                return true;
            }
            if (errno != ENOMEM) return false;
            if (!source_application_os_is_old_3ds() || entry.levels <= 1 || asset_.image || asset_.font_palette) break;
            tiled += drop_base_mip(entry);
        }
    }
    if(world_mips_&&!dirty_&&!target_){
        unsigned width=0,height=0,levels=0;
        std::vector<uint8_t> tiled;
        if(SourcePort::BuildOpaqueWorldMipChain(asset_,limit,width,height,levels,tiled)){
            gpu_=source_graphics_texture_create_mips(width,height,SOURCE_TEXTURE_RGB565,levels,tiled.data(),tiled.size());
            if(gpu_){
                u_scale_=v_scale_=1.f;
                ReleaseUploadScratch();return true;
            }
            if(errno!=ENOMEM)return false;
        }
    }
    if(!decode())return false;
    const auto* source=&pixels_;std::vector<uint8> reduced;unsigned sample_width=asset_.width,sample_height=asset_.height;
    if(limit&&!target_&&!asset_.image&&!asset_.font_palette&&(asset_.width>limit||asset_.height>limit)){
        if(!SourcePort::ReduceTextureRGBA(pixels_,asset_.width,asset_.height,limit,reduced,sample_width,sample_height))return false;source=&reduced;}
    if(sample_width>1024||sample_height>1024){errno=ENOTSUP;return false;}
    const auto gpu_width=pot(sample_width),gpu_height=pot(sample_height);
    u_scale_=float(sample_width)/gpu_width;v_scale_=float(sample_height)/gpu_height;
    std::vector<uint8> tiled;
    SourceTextureFormat format=SOURCE_TEXTURE_RGBA8;
    if(target_){
        if(!SourcePort::TilePaddedRGBA(source->data(),sample_width,sample_height,gpu_width,gpu_height,tiled))return false;
    }else if(!SourcePort::TileCompactTexture(*source,sample_width,sample_height,gpu_width,gpu_height,format,tiled)){
        return false;
    }
    gpu_=source_graphics_texture_create_format(gpu_width,gpu_height,format,tiled.data(),tiled.size());
    if(gpu_)ReleaseUploadScratch();
    return gpu_!=nullptr;
}
SourceGpuTexture* CNativeTexture::GPU(){return upload()?gpu_:nullptr;}
float CNativeTexture::GPUV(float v)const{
    // Standalone menu/sprite IMG UVs use the existing 2D convention;
    // model/scene TEX dictionaries, FNT and CPU targets use top-first UVs. Treating
    // every retail texture as IMG inverted faces (and dictionary wall signs).
    // Normalised UVs retain their range after reducing resident resolution.
    return (!asset_.image||asset_.font_palette||target_?1.f-v:v)*VScale();
}
float CNativeTexture::ModelGPUV(float v)const{return (1.f-v)*VScale();}
bool CNativeTexture::changed(){
    controller();bool was_resident=gpu_!=nullptr;if(!plat_remove_from_vram())return false;dirty_=true;++revision_;
    transparent_=false;for(std::size_t i=3;i<pixels_.size();i+=4)transparent_|=pixels_[i]!=255;transparency_known_=true;
    return !was_resident||upload();
}
bool CNativeTexture::plat_load_texture(const char* name,bool,bool alloc){
    if(!name||std::strlen(name)>500)return false;
    std::string file=std::string(name)+".img.xbx";SourcePort::TextureAsset asset;
    // Standalone sprite data belongs to the renderer and may load while a
    // whole level is still alive beneath the pause menu.
    auto bytes=SourcePort::ReadRenderResource(file.c_str(),SourcePort::RenderResourceStorage::NativeHeap);
    uint32 key=m_checksum;if(!SourcePort::ReadTextureImage(std::move(bytes),asset)||!Assign(std::move(asset)))return false;
    m_checksum=key;return !alloc||upload();
}
bool CNativeTexture::plat_load_texture_from_buffer(uint8* data,int size,bool,bool alloc){
    if(!data||size<=0)return false;
    auto bytes=std::make_shared<SourcePort::ResourceBytes>(data,data+size);SourcePort::TextureAsset asset;
    uint32 key=m_checksum;if(!SourcePort::ReadTextureImage(bytes,asset)||!Assign(std::move(asset)))return false;m_checksum=key;return !alloc||upload();
}
bool CNativeTexture::plat_replace_texture(CTexture* texture){
    auto* native=dynamic_cast<CNativeTexture*>(texture);if(!native)return false;if(native==this)return true;
    bool resident=gpu_!=nullptr;uint32 key=m_checksum;
    if(!Assign(native->asset_))return false;m_checksum=key;pixels_=native->pixels_;original_=native->original_;dirty_=native->dirty_;transparent_=native->transparent_;transparency_known_=native->transparency_known_;return !resident||upload();
}
bool CNativeTexture::plat_generate_32bit_image(bool renderable,bool store){
    if(renderable){errno=ENOTSUP;return false;}if(!decode())return false;cpu_read_=true;if(store)original_=pixels_;return true;
}
bool CNativeTexture::plat_put_32bit_image_into_texture(bool){return !pixels_.empty()&&changed();}
bool CNativeTexture::plat_add_to_vram(){return upload();}
bool CNativeTexture::plat_remove_from_vram(){
    controller();if(gpu_){if(source_graphics_texture_delete(gpu_))return false;gpu_=nullptr;}return true;
}
bool CNativeTexture::plat_is_transparent()const{auto* self=const_cast<CNativeTexture*>(this);return (transparency_known_||self->decode())&&transparent_;}
bool CNativeTexture::plat_offset(int dx,int dy,bool fill,Image::RGBA color){
    if(!decode())return false;auto old=pixels_;int w=asset_.width,h=asset_.height;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){auto* p=pixels_.data()+(std::size_t(y)*w+x)*4;int sx=x-dx,sy=y-dy;
        if(fill&&(sx<0||sy<0||sx>=w||sy>=h)){p[0]=color.r;p[1]=color.g;p[2]=color.b;p[3]=color.a;}
        else{sx=std::clamp(sx,0,w-1);sy=std::clamp(sy,0,h-1);std::copy_n(old.data()+(std::size_t(sy)*w+sx)*4,4,p);}}
    return changed();
}
bool CNativeTexture::plat_adjust_region(uint16 x,uint16 y,uint16 w,uint16 h,int axis,uint16 start,uint16 end){
    if(!decode()||(axis!=0&&axis!=1)||!w||!h||x+w>asset_.width||y+h>asset_.height)return false;
    unsigned origin=axis?y:x,size=axis?h:w;if(start<origin||start>=origin+size||end<origin||end>=origin+size)return false;
    auto old=pixels_;float a=start-origin,b=end-origin;
    for(unsigned yy=0;yy<h;++yy)for(unsigned xx=0;xx<w;++xx){float d=axis?yy:xx;
        float src=d<=b?(b?d*a/b:0):(b<float(size-1)?a+(d-b)*(size-1-a)/(size-1-b):a);
        unsigned low=std::min<unsigned>(std::max(0.f,std::floor(src)),size-1),high=std::min(low+1,size-1);float f=src-low;
        auto at=[&](unsigned v){return (std::size_t(y+(axis?v:yy))*asset_.width+x+(axis?xx:v))*4;};auto target=(std::size_t(y+yy)*asset_.width+x+xx)*4;
        for(unsigned c=0;c<4;++c)pixels_[target+c]=byte(old[at(low)+c]*(1-f)+old[at(high)+c]*f);}
    return changed();
}
bool CNativeTexture::plat_pull_to_edge(uint16 point,int axis,int amount){
    if(axis!=0&&axis!=1)return false;unsigned n=axis?asset_.height:asset_.width;if(point>=n||n<2)return false;
    return plat_adjust_region(0,0,asset_.width,asset_.height,axis,point,std::clamp<int>(int(point)+amount,0,n-1));
}
bool CNativeTexture::plat_push_to_point(uint16 point,int axis,int amount,bool fill,Image::RGBA color){
    if(!decode()||(axis!=0&&axis!=1))return false;unsigned n=axis?asset_.height:asset_.width;if(point>=n)return false;
    amount=std::clamp<int>(amount,-int(point),int(n-1-point));auto old=pixels_;
    // The edge-to-point region moves/crops; the vacated edge repeats its new
    // border or uses the requested fill, matching the original image operation.
    for(unsigned y=0;y<asset_.height;++y)for(unsigned x=0;x<asset_.width;++x){int d=axis?y:x;if((amount>0&&d>int(point)+amount)||(amount<0&&d<int(point)+amount))continue;
        int src=d-amount;auto* p=pixels_.data()+(std::size_t(y)*asset_.width+x)*4;
        if(fill&&(src<0||src>=int(n))){p[0]=color.r;p[1]=color.g;p[2]=color.b;p[3]=color.a;}
        else{src=std::clamp(src,0,int(n)-1);unsigned sx=axis?x:src,sy=axis?src:y;std::copy_n(old.data()+(std::size_t(sy)*asset_.width+sx)*4,4,p);}}
    return changed();
}
bool CNativeTexture::plat_adjust_brightness(float scale,bool current){
    if(!std::isfinite(scale)||scale<0||!decode())return false;auto source=(!current&&!original_.empty())?original_:pixels_;
    for(std::size_t i=0;i<pixels_.size();i+=4)for(unsigned c=0;c<3;++c)pixels_[i+c]=byte(source[i+c]*scale);return changed();
}
bool CNativeTexture::plat_adjust_hsv(float h,float s,float v,bool current){
    if(!std::isfinite(h)||!std::isfinite(s)||!std::isfinite(v)||h<0||h>360||s<0||v<0||!decode())return false;
    auto source=(!current&&!original_.empty())?original_:pixels_;
    for(std::size_t i=0;i<pixels_.size();i+=4){float ph,ps,pv,red,green,blue;Gfx::inlineRGBtoHSV(source[i]/255.f,source[i+1]/255.f,source[i+2]/255.f,ph,ps,pv);
        ph+=h;if(ph>360)ph-=360;Gfx::inlineHSVtoRGB(red,green,blue,ph,std::min(1.f,ps*s),std::min(1.f,pv*v));
        pixels_[i]=byte(red*255);pixels_[i+1]=byte(green*255);pixels_[i+2]=byte(blue*255);}
    return changed();
}
bool CNativeTexture::plat_combine_textures(CTexture* texture,bool){
    auto* native=dynamic_cast<CNativeTexture*>(texture);if(!native||!decode()||!native->decode()||asset_.width!=native->asset_.width||asset_.height!=native->asset_.height)return false;
    for(std::size_t i=0;i<pixels_.size();i+=4){float a=native->pixels_[i+3]/255.f;
        for(unsigned c=0;c<3;++c)pixels_[i+c]=byte(native->pixels_[i+c]*a+pixels_[i+c]*(1-a));pixels_[i+3]=byte(native->pixels_[i+3]+pixels_[i+3]*(1-a));}
    return changed();
}
bool CNativeTexDict::Load(std::shared_ptr<const SourcePort::ResourceBytes> bytes, const char* name){
    controller();
    std::vector<SourcePort::TextureAsset> assets;
    if (!SourcePort::ReadTextureDictionary(bytes,assets)) return false;
    m_file_size = bytes->size();
    dictionary_bytes_ = std::move(bytes);
    return AddAssets(std::move(assets), 0, name);
}
bool CNativeTexDict::LoadDisk(const char* name) {
    controller();
    SourcePort::DiskResourceSlice slice;
    if (!SourcePort::FindDiskResourceSlice(name, slice)) return false;
    auto disk = std::make_shared<const SourcePort::DiskResourceSlice>(std::move(slice));
    std::vector<SourcePort::TextureAsset> assets;
    uint32 checksum;
    if (!SourcePort::ReadDiskTextureDictionary(disk, assets, checksum)) return false;
    m_file_size = disk->size;
    SourcePort::Log("TEXTURE_SOURCE mode=disk bytes=%u name=%s", unsigned(m_file_size), name);
    return AddAssets(std::move(assets), checksum, name);
}
bool CNativeTexDict::AddAssets(std::vector<SourcePort::TextureAsset> assets, uint32 disk_checksum, const char* name) {
    for(const auto& a:assets)if(mp_texture_lookup->GetItem(a.checksum))return false;
    for(auto& a:assets){auto* texture=new CNativeTexture;Dbg_Assert(texture->Assign(std::move(a)));mp_texture_lookup->PutItem(texture->GetChecksum(),texture);}
    if (source_render_options()->performance) {
        std::shared_ptr<const SourcePort::ResourceBytes> group;
        std::vector<SourcePort::TextureCacheEntry> entries;
        auto cache = SourcePort::TextureCacheFile::None;
        if (source_application_os_is_old_3ds()) {
            const auto checksum = dictionary_bytes_ ?
                SourcePort::WorldTextureChecksum(dictionary_bytes_->data(), dictionary_bytes_->size()) : disk_checksum;
            cache = SourcePort::FindDiskTextureCacheGroup(checksum, m_file_size, SourcePort::TextureSource::Dictionary, entries);
        } else {
            // Keep standard-quality upload payloads resident. Scenes backed
            // by SD only need to read the original dictionary headers/CRC.
            group = dictionary_bytes_ ?
                SourcePort::FindTextureCacheGroup(*dictionary_bytes_, SourcePort::TextureSource::Dictionary) :
                SourcePort::FindTextureCacheGroup(disk_checksum, m_file_size, SourcePort::TextureSource::Dictionary);
            if (group) SourcePort::ReadTextureCacheGroup(*group, entries);
        }
        const bool compact_city = compact_city_dictionary(name);
        unsigned reduced = 0;
        std::size_t skipped_bytes = 0;
        for (auto& entry : entries) {
            auto* texture = dynamic_cast<CNativeTexture*>(mp_texture_lookup->GetItem(entry.key));
            if (!texture) continue;
            if (compact_city && entry.levels > 1 && std::max(entry.width, entry.height) > 64 &&
                std::min(entry.width, entry.height) >= 16 && entry.width == entry.sample_width &&
                entry.height == entry.sample_height &&
                (entry.format == SOURCE_TEXTURE_ETC1 || entry.format == SOURCE_TEXTURE_ETC1A4)) {
                skipped_bytes += drop_base_mip(entry);
                ++reduced;
            }
            texture->compressed_group_ = group;
            texture->disk_cache_ = cache;
            texture->compressed_entry_ = entry;
        }
        if (compact_city)
            SourcePort::Log("OLD_CITY_MIPS name=%s entries=%zu reduced=%u skipped_bytes=%zu", name, entries.size(), reduced, skipped_bytes);
        if (cache == SourcePort::TextureCacheFile::Venice)
            SourcePort::Log("VENICE_TEXTURE_CACHE entries=%zu", entries.size());
    }
    return true;
}
void CNativeTexDict::EnableOpaqueWorldMipmaps(const std::vector<SourcePort::MaterialAsset>& materials, const char* scene_name) {
    controller();
    const auto& options = *source_render_options();
    if (!options.performance) return;
    const auto eligible = SourcePort::OpaqueWorldTextures(materials);
    for (auto key : eligible) {
        if (auto* texture = dynamic_cast<CNativeTexture*>(mp_texture_lookup->GetItem(key)))
            texture->world_mips_ = true;
    }
    if (!scene_name || !dictionary_bytes_) return;

    const auto bank_eligible = SourcePort::OpaqueWorldTextures(materials, true);
    auto legacy_bank_target = [&](uint32 key) -> CNativeTexture* {
        auto* texture = dynamic_cast<CNativeTexture*>(mp_texture_lookup->GetItem(key));
        return texture && !texture->compressed_group_ && texture->disk_cache_ == SourcePort::TextureCacheFile::None && texture->asset_.bytes == dictionary_bytes_ ? texture : nullptr;
    };
    if (std::none_of(bank_eligible.begin(), bank_eligible.end(), legacy_bank_target)) {
        SourcePort::Log("WORLD_ETC1 skipped scene=%s covered=%u", scene_name, unsigned(bank_eligible.size()));
        return;
    }

    SourcePort::RootHeapScope storage;
    auto bank = SourcePort::LoadWorldTextureBank(scene_name);
    if (!bank) return;
    std::vector<SourcePort::WorldTextureEntry> entries;
    if (!SourcePort::ReadWorldTextureBank(*bank, *dictionary_bytes_, options.texture_limit, entries)) {
        SourcePort::Log("WORLD_ETC1 rejected scene=%s", scene_name);
        return;
    }

    unsigned attached = 0;
    for (const auto& entry : entries) {
        if (!bank_eligible.count(entry.checksum)) continue;
        auto* texture = legacy_bank_target(entry.checksum);
        if (!texture) continue;
        const auto& mips = texture->asset_.levels;
        const auto first = std::find_if(mips.begin(), mips.end(), [&](const auto& mip) {
            return std::max(mip.width, mip.height) <= options.texture_limit;
        });
        if (std::size_t(mips.end() - first) < entry.levels ||
            first->width != entry.width || first->height != entry.height) continue;
        texture->compressed_group_ = bank;
        texture->compressed_entry_ = {entry.checksum, entry.width, entry.height, entry.levels,
            entry.width, entry.height, SOURCE_TEXTURE_ETC1, entry.offset, entry.bytes};
        ++attached;
    }
    SourcePort::Log("WORLD_ETC1 loaded scene=%s textures=%u bank_bytes=%u",
        scene_name, attached, unsigned(bank->size()));
}
CTexture* CNativeTexDict::plat_load_texture(const char* name,bool sprite,bool alloc){auto* t=new CNativeTexture;if(!t->LoadTexture(name,sprite,alloc)){delete t;return nullptr;}return t;}
CTexture* CNativeTexDict::plat_load_texture_from_buffer(uint8* data,int size,uint32 key,bool sprite,bool alloc){auto* t=new CNativeTexture;if(!t->LoadTextureFromBuffer(data,size,key,sprite,alloc)){delete t;return nullptr;}t->m_checksum=key;return t;}
CTexture* CNativeTexDict::plat_reload_texture(const char* name){
    auto* fresh=plat_load_texture(name,true,false);if(!fresh)return nullptr;auto* old=GetTexture(fresh->GetChecksum());
    if(old){bool ok=old->ReplaceTexture(fresh);delete fresh;return ok?old:nullptr;}AddTexture(fresh);return fresh;
}
bool CNativeTexDict::plat_unload_texture(CTexture* texture){if(!texture||mp_texture_lookup->GetItem(texture->GetChecksum())!=texture)return false;delete texture;return true;}
void CNativeTexDict::plat_add_texture(CTexture* texture){++texture_generation;Dbg_Assert(dynamic_cast<CNativeTexture*>(texture)&&mp_texture_lookup->GetItem(texture->GetChecksum())==texture);if(SourcePort::GraphicsReady())Dbg_Assert(texture->AddToVram());}
bool CNativeTexDict::plat_remove_texture(CTexture* texture){if(!texture||mp_texture_lookup->GetItem(texture->GetChecksum())!=texture)return false;++texture_generation;mp_texture_lookup->FlushItem(texture->GetChecksum());return true;}
bool CTexDict::RemoveTexture(CTexture* texture){return plat_remove_texture(texture);}
CTexture* CNativeTexDict::plat_copy_texture(uint32 key,CTexture* texture){auto* native=dynamic_cast<CNativeTexture*>(texture);if(!native)return nullptr;auto* copy=new CNativeTexture;
    if(!copy->plat_replace_texture(native)){delete copy;return nullptr;}copy->m_checksum=key;return copy;}
CTexDict* CTexDictManager::s_plat_create_texture_dictionary(uint32 key){return new CNativeTexDict(key);}
CTexDict* CTexDictManager::s_plat_load_texture_dictionary(const char* name,bool,uint32,bool,bool){
    auto* dict=new CNativeTexDict(Crc::GenerateCRCFromString(name));
    if ((source_application_os_is_old_3ds() || source_application_os_is_new_3ds()) && dict->LoadDisk(name)) return dict;
    auto bytes=SourcePort::ReadRenderResource(name);
    if(!bytes || !dict->Load(bytes,name)){delete dict;return nullptr;}return dict;
}
CTexDict* CTexDictManager::s_plat_load_texture_dictionary(uint32 key,uint32* data,int size,bool,uint32,bool,bool){
    if(!data||size<=0)return nullptr;const auto* start=reinterpret_cast<const uint8*>(data);
    auto bytes=std::make_shared<SourcePort::ResourceBytes>(start,start+size);auto* dict=new CNativeTexDict(key);
    if(!dict->Load(bytes)){delete dict;return nullptr;}return dict;
}
bool CTexDictManager::s_plat_unload_texture_dictionary(CTexDict* dict){if(!dict)return false;delete dict;return true;}
}
