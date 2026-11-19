#pragma once
#include <source_port/texture_cache.h>
#include <source_port/graphics_os.h>
#include <gfx/nxtexture.h>
namespace Nx {
class CNativeTexture final:public CTexture {
public:
    CNativeTexture();~CNativeTexture()override;
    bool Assign(SourcePort::TextureAsset asset);
    SourceGpuTexture* GPU();
    bool GPUResident()const{return gpu_!=nullptr;}
    float UScale()const{return u_scale_;}float VScale()const{return v_scale_;}
    float GPUV(float source_v)const;
    float ModelGPUV(float source_v)const; // original mesh UVs stay top-first after TEX -> IMG replacement
    uint64 Revision()const{return revision_;}
    const SourcePort::TextureAsset& Asset()const{return asset_;}
    bool Pixels(const std::vector<uint8>** pixels);
    // Release only reproducible decode scratch. CPU readers/edits and render
    // targets retain their backing; original compressed asset bytes stay live.
    std::size_t ReleaseUploadScratch();
    bool CreateRenderTarget(unsigned width,unsigned height,unsigned depth,unsigned z_depth);
    bool WriteRenderTarget(const std::vector<uint8>& rgba);
    bool IsRenderTarget()const{return target_;}
    unsigned TargetDepth()const{return target_depth_;}
private:
    SourcePort::TextureAsset asset_;
    std::shared_ptr<const SourcePort::ResourceBytes> compressed_group_;
    SourcePort::TextureCacheEntry compressed_entry_;
    SourcePort::TextureCacheFile disk_cache_=SourcePort::TextureCacheFile::None;
    std::vector<uint8> pixels_,original_;
    SourceGpuTexture* gpu_=nullptr;float u_scale_=1.f,v_scale_=1.f;
    bool transparent_=false,dirty_=false,transparency_known_=false,cpu_read_=false,world_mips_=false;
    uint64 revision_=0;
    bool target_=false;unsigned target_depth_=0;
    bool decode();bool upload();bool changed();
    bool plat_load_texture(const char*,bool,bool)override;
    bool plat_load_texture_from_buffer(uint8*,int,bool,bool)override;
    bool plat_replace_texture(CTexture*)override;
    bool plat_generate_32bit_image(bool,bool)override;
    bool plat_put_32bit_image_into_texture(bool)override;
    bool plat_offset(int,int,bool,Image::RGBA)override;
    bool plat_adjust_region(uint16,uint16,uint16,uint16,int,uint16,uint16)override;
    bool plat_pull_to_edge(uint16,int,int)override;
    bool plat_push_to_point(uint16,int,int,bool,Image::RGBA)override;
    bool plat_adjust_brightness(float,bool)override;
    bool plat_adjust_hsv(float,float,float,bool)override;
    bool plat_add_to_vram()override;bool plat_remove_from_vram()override;
    uint16 plat_get_width()const override{return asset_.actual_width;}
    uint16 plat_get_height()const override{return asset_.actual_height;}
    uint8 plat_get_bitdepth()const override{return dirty_?32:asset_.depth;}
    uint8 plat_get_palette_bitdepth()const override{return dirty_?0:asset_.palette_depth;}
    uint8 plat_get_num_mipmaps()const override{return dirty_?1:asset_.levels.size();}
    bool plat_is_transparent()const override;
    bool plat_combine_textures(CTexture*,bool)override;
    friend class CNativeTexDict;
};
class CNativeTexDict final:public CTexDict {
public:
    explicit CNativeTexDict(uint32 key):CTexDict(key,true){}
    bool Load(std::shared_ptr<const SourcePort::ResourceBytes>);
    bool LoadDisk(const char* name);
    void EnableOpaqueWorldMipmaps(const std::vector<SourcePort::MaterialAsset>&, const char* scene_name);
private:
    std::shared_ptr<const SourcePort::ResourceBytes> dictionary_bytes_;
    bool AddAssets(std::vector<SourcePort::TextureAsset>, uint32 disk_checksum=0);
    CTexture* plat_load_texture(const char*,bool,bool)override;
    CTexture* plat_load_texture_from_buffer(uint8*,int,uint32,bool,bool)override;
    CTexture* plat_reload_texture(const char*)override;
    bool plat_unload_texture(CTexture*)override;
    void plat_add_texture(CTexture*)override;
    bool plat_remove_texture(CTexture*)override;
    CTexture* plat_copy_texture(uint32,CTexture*)override;
};
}
