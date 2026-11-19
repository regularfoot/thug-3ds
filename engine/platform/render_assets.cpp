// Native wire-data side of the original Nx platform interface. Layouts follow
// original DX9 p_nxtexture.cpp, nx/texture.cpp, nx/chars.cpp and p_nxsector.cpp;
// no converted preview meshes, substituted fonts or gameplay objects.
#include <source_port/render_assets.h>
#include <source_port/files.h>
#include <source_port/memory.h>
#include <source_port/application_os.h>
#include <sys/file/filesys.h>
#include <sys/file/pre.h>
#include <algorithm>
#include <set>
namespace SourcePort {
namespace {
std::shared_ptr<ResourceBytes> allocate_resource(int size,RenderResourceStorage storage){
    if(storage==RenderResourceStorage::NativeHeap){
        try {
            NativeHeapScope native;
            return std::make_shared<ResourceBytes>(size);
        } catch (const std::bad_alloc&) {
            if (!source_application_os_is_old_3ds()) throw;
            // Once a level is unloaded, the original top-down arena can have
            // space even when the small native heap is fragmented returning to UI.
            RootHeapScope fallback(RootHeapDirection::TopDown);
            return std::make_shared<ResourceBytes>(size);
        }
    }
    return std::make_shared<ResourceBytes>(size);
}
}
std::shared_ptr<const ResourceBytes> ReadRenderResource(const char* name,RenderResourceStorage storage){
    constexpr int max_resource_bytes = 128 * 1024 * 1024;
    if (name && std::strlen(name) < 128 && File::PreMgr::sPreEnabled()) {
        struct Request {
            RenderResourceStorage storage;
            std::shared_ptr<ResourceBytes> bytes;
        } request{storage,{}};
        const auto allocate = [](void* context, int size) -> void* {
            if (size <= 0 || size > max_resource_bytes) return nullptr;
            auto& request = *static_cast<Request*>(context);
            request.bytes = allocate_resource(size,request.storage);
            return request.bytes->data();
        };
        int size = 0;
        if (File::PreMgr::Instance()->LoadFile(name, &size, nullptr, allocate, &request))
            return request.bytes;
        if (size != 0) return {};
    }
    void* f=File::Open(name,"rb");if(!f)return {};
    auto size=File::GetFileSize(f);std::shared_ptr<ResourceBytes> bytes;
    if(size>0&&size<=max_resource_bytes){bytes=allocate_resource(size,storage);if(File::Read(bytes->data(),1,bytes->size(),f)!=bytes->size())bytes.reset();}
    File::Close(f);return bytes;
}
namespace {
bool power(unsigned n){return n&&!(n&(n-1));}
bool dimensions(unsigned w,unsigned h){return w&&h&&w<=4096&&h<=4096&&std::size_t(w)*h<=4096*4096;}
bool level_size(ResourceReader& r,TextureAsset& t,unsigned levels){
    if(!levels||levels>13||!dimensions(t.width,t.height))return r.Fail("invalid texture dimensions/levels");
    unsigned w=t.width,h=t.height;
    for(unsigned i=0;i<levels&&r.Ok();++i){
        auto n=r.U32();std::size_t expected=t.dxt?std::size_t((w+3)/4)*((h+3)/4)*(t.dxt==5?16:8):std::size_t(w)*h*(t.depth/8);
        if(n<expected||n>((expected+3)&~std::size_t(3)))return r.Fail("texture mip length mismatch");
        t.levels.push_back({w,h,r.Position(),n});if(!r.Skip(n))return false;w=std::max(1u,w/2);h=std::max(1u,h/2);
    }return r.Ok();
}
std::array<uint8,4> rgb565(unsigned c){return {uint8(((c>>11)&31)*255/31),uint8(((c>>5)&63)*255/63),uint8((c&31)*255/31),255};}
unsigned u16(const uint8* p){return p[0]|(unsigned(p[1])<<8);}
uint32 u32(const uint8* p){return uint32(p[0])|(uint32(p[1])<<8)|(uint32(p[2])<<16)|(uint32(p[3])<<24);}
std::size_t swizzle(unsigned x,unsigned y,unsigned width,unsigned height){
    std::size_t result=0;unsigned target=0;
    for(unsigned bit=0;(width>>bit)>1||(height>>bit)>1;++bit){
        if((width>>bit)>1)result|=std::size_t((x>>bit)&1)<<target++;
        if((height>>bit)>1)result|=std::size_t((y>>bit)&1)<<target++;
    }return result;
}
}
namespace {
bool ParseTextureDictionary(ResourceReader& r,std::vector<TextureAsset>& out){
    std::vector<TextureAsset> result;std::set<uint32> keys;
    if(r.U32()!=1)return r.Fail("unsupported texture dictionary version");
    auto count=r.U32();if(!r.Count(count,36,16384))return false;result.reserve(count);
    for(unsigned i=0;i<count&&r.Ok();++i){
        TextureAsset t;t.checksum=r.U32();t.width=r.U32();t.height=r.U32();auto levels=r.U32();
        t.actual_width=t.width;t.actual_height=t.height;t.depth=r.U32();t.palette_depth=r.U32();t.dxt=r.U32();t.palette_size=r.U32();
        if(!keys.insert(t.checksum).second)return r.Fail("duplicate texture checksum");
        if((t.dxt!=0&&t.dxt!=1&&t.dxt!=2&&t.dxt!=5)||(!t.dxt&&t.depth!=8&&t.depth!=16&&t.depth!=32))return r.Fail("unsupported source texture format");
        if((t.depth==8&&!t.dxt&&(t.palette_depth!=32||!t.palette_size||t.palette_size>1024||t.palette_size%4))||t.palette_size>1024)return r.Fail("invalid texture palette");
        t.palette_offset=r.Position();if(!r.Skip(t.palette_size)||!level_size(r,t,levels))return false;
        result.push_back(std::move(t));
    }
    if(!r.Ok()||r.Remaining())return r.Fail("texture dictionary trailing bytes");out.swap(result);return true;
}
}
bool ReadTextureDictionary(std::shared_ptr<const ResourceBytes> bytes,std::vector<TextureAsset>& out,ResourceError* error){
    if (!bytes) return false;
    ResourceReader reader(bytes->data(),bytes->size(),error);
    if (!ParseTextureDictionary(reader,out)) return false;
    for (auto& asset : out) asset.bytes=bytes;
    return true;
}
bool ReadDiskTextureDictionary(std::shared_ptr<const DiskResourceSlice> disk, std::vector<TextureAsset>& out, uint32& checksum) {
    auto* file = File::Open(disk->archive.c_str(), "rb");
    if (!file) return false;
    struct Input { void* file; uint32 crc=0xffffffff; } input{file};
    const auto fill = [](void* context, uint8* bytes, std::size_t count) {
        auto& in = *static_cast<Input*>(context);
        if (File::Read(bytes,1,count,in.file)!=count) return false;
        in.crc = Crc::UpdateCRC(reinterpret_cast<const char*>(bytes),int(count),in.crc);
        return true;
    };
    std::array<uint8,8192> small_buffer;
    ResourceBytes read_ahead;
    if(source_application_os_is_new_3ds()&&disk->size>=512*1024)read_ahead.resize(64*1024);
    auto* buffer=read_ahead.empty()?small_buffer.data():read_ahead.data();
    const auto size=read_ahead.empty()?small_buffer.size():read_ahead.size();
    ResourceReader reader(disk->size,nullptr,fill,&input,buffer,size);
    const bool parsed = File::Seek(file,disk->offset,SEEK_SET)==0 && ParseTextureDictionary(reader,out);
    File::Close(file);
    if (!parsed) return false;
    for (auto& asset : out) asset.disk=disk;
    checksum=input.crc;
    return true;
}
bool ReadTextureImage(std::shared_ptr<const ResourceBytes> bytes,TextureAsset& out,ResourceError* error){
    if(!bytes)return false;ResourceReader r(bytes->data(),bytes->size(),error);TextureAsset t;t.bytes=bytes;t.image=true;
    if(r.U32()!=2)return r.Fail("unsupported image version");t.checksum=r.U32();t.width=r.U32();t.height=r.U32();
    auto format=r.U32(),palette_format=r.U32();t.actual_width=r.U16();t.actual_height=r.U16();t.palette_size=r.U32();
    t.depth=format==0?32:format==2?16:format==0x13?8:0;t.palette_depth=t.depth==8&&palette_format==0?32:0;
    if(!t.depth||!dimensions(t.width,t.height)||!dimensions(t.actual_width,t.actual_height))return r.Fail("invalid image dimensions/format");
    t.swizzled=power(t.width)&&power(t.height);
    if(!t.swizzled){t.width=t.actual_width;t.height=t.actual_height;}
    if(t.actual_width>t.width||t.actual_height>t.height)return r.Fail("image crop exceeds storage");
    if(t.depth==8&&(!t.palette_depth||!t.palette_size||t.palette_size>1024||t.palette_size%4))return r.Fail("invalid image palette");
    t.palette_offset=r.Position();if(!r.Skip(t.palette_size))return false;
    std::size_t size=(std::size_t(t.width)*t.height*(t.depth/8)+3)&~std::size_t(3);
    t.levels.push_back({t.width,t.height,r.Position(),size});if(!r.Skip(size)||r.Remaining())return r.Fail("image data length mismatch");out=std::move(t);return true;
}
bool DecodeTextureLevel(const TextureAsset& t,unsigned level,std::vector<uint8>& out,ResourceError* error){
    if(error)*error={};auto fail=[&](const char* why){if(error)error->reason=why;return false;};
    if((!t.bytes&&!t.disk)||level>=t.levels.size())return fail("missing mip");const auto& l=t.levels[level];
    const auto source_bytes = t.bytes ? t.bytes->size() : t.disk->size;
    if(!dimensions(l.width,l.height)||l.offset>source_bytes||l.size>source_bytes-l.offset||
        t.palette_offset>source_bytes||t.palette_size>source_bytes-t.palette_offset)return fail("invalid texture span");
    std::vector<uint8> payload;
    std::array<uint8,1024> disk_palette;
    const uint8 *src=nullptr,*palette=nullptr;
    if (t.bytes) {
        src=t.bytes->data()+l.offset;
        palette=t.bytes->data()+t.palette_offset;
    } else {
        payload.resize(l.size);
        auto* file=File::Open(t.disk->archive.c_str(),"rb");
        if (!file) return fail("texture archive unavailable");
        bool read=File::Seek(file,t.disk->offset+l.offset,SEEK_SET)==0 &&
            File::Read(payload.data(),1,payload.size(),file)==payload.size();
        if (read && t.palette_size) read=t.palette_size<=disk_palette.size() &&
            File::Seek(file,t.disk->offset+t.palette_offset,SEEK_SET)==0 &&
            File::Read(disk_palette.data(),1,t.palette_size,file)==t.palette_size;
        File::Close(file);
        if (!read) return fail("texture archive read failed");
        src=payload.data();palette=disk_palette.data();
    }
    std::vector<uint8> rgba(std::size_t(l.width)*l.height*4);
    if(t.dxt){
        if(t.dxt!=1&&t.dxt!=2&&t.dxt!=5)return fail("invalid DXT encoding");unsigned block_size=t.dxt==5?16:8;
        if(std::size_t((l.width+3)/4)*((l.height+3)/4)*block_size>l.size)return fail("truncated DXT blocks");
        for(unsigned by=0;by<l.height;by+=4)for(unsigned bx=0;bx<l.width;bx+=4){
            const uint8* block=src+((std::size_t(by/4)*((l.width+3)/4))+bx/4)*block_size;
            const uint8* colors=block+(t.dxt==5?8:0);unsigned a=u16(colors),b=u16(colors+2);uint32 bits=u32(colors+4);
            std::array<std::array<uint8,4>,4> table{rgb565(a),rgb565(b),{}, {}};
            for(unsigned c=0;c<3;++c){table[2][c]=(a>b||t.dxt==5)?(2*table[0][c]+table[1][c])/3:(table[0][c]+table[1][c])/2;table[3][c]=(a>b||t.dxt==5)?(table[0][c]+2*table[1][c])/3:0;}
            table[2][3]=255;table[3][3]=(a>b||t.dxt==5)?255:0;
            uint8 alpha[8]={255,255,255,255,255,255,255,255};uint64 alpha_bits=0;
            if(t.dxt==5){alpha[0]=block[0];alpha[1]=block[1];for(unsigned i=0;i<6;++i)alpha_bits|=uint64(block[2+i])<<(8*i);
                if(alpha[0]>alpha[1])for(unsigned i=2;i<8;++i)alpha[i]=((8-i)*alpha[0]+(i-1)*alpha[1])/7;
                else{for(unsigned i=2;i<6;++i)alpha[i]=((6-i)*alpha[0]+(i-1)*alpha[1])/5;alpha[6]=0;alpha[7]=255;}}
            for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){unsigned k=y*4+x;if(bx+x>=l.width||by+y>=l.height)continue;
                auto pixel=table[(bits>>(k*2))&3];if(t.dxt==5)pixel[3]=alpha[(alpha_bits>>(k*3))&7];
                std::copy(pixel.begin(),pixel.end(),rgba.begin()+((std::size_t(by+y)*l.width+bx+x)*4));}
        }
    }else{
        unsigned bpp=t.depth/8;if((bpp!=1&&bpp!=2&&bpp!=4)||std::size_t(l.width)*l.height*bpp>l.size)return fail("invalid uncompressed texture");
        bool tiled=t.swizzled;if(tiled&&(!power(l.width)||!power(l.height)))return fail("invalid source swizzle dimensions");
        for(unsigned y=0;y<l.height;++y)for(unsigned x=0;x<l.width;++x){
            auto index=tiled?swizzle(x,y,l.width,l.height):std::size_t(y)*l.width+x;const auto* p=src+index*bpp;auto* to=rgba.data()+(std::size_t(y)*l.width+x)*4;
            if(bpp==1){unsigned entry=*p;if(std::size_t(entry)*4+4>t.palette_size)return fail("palette index outside CLUT");p=palette+entry*4;
                if(t.font_palette){to[0]=p[0];to[1]=p[1];to[2]=p[2];to[3]=p[3]>=128?255:p[3]*2;}
                else{to[0]=p[2];to[1]=p[1];to[2]=p[0];to[3]=p[3];}}
            else if(bpp==2){unsigned c=u16(p);to[0]=((c>>10)&31)*255/31;to[1]=((c>>5)&31)*255/31;to[2]=(c&31)*255/31;to[3]=(c&0x8000)?255:0;}
            else{to[0]=p[2];to[1]=p[1];to[2]=p[0];to[3]=p[3];}
        }
    }out.swap(rgba);return true;
}
bool ReadFont(std::shared_ptr<const ResourceBytes> bytes,FontAsset& out,ResourceError* error){
    if(!bytes)return false;ResourceReader r(bytes->data(),bytes->size(),error);FontAsset f;
    auto declared=r.U32(),count=r.U32();f.height=r.U32();f.base=r.U32();
    if(declared!=bytes->size()||!count||!r.Count(count,4,256)||!f.height||f.height>1024||f.base>1024)return r.Fail("invalid font header");
    f.glyphs.resize(count);
    for(unsigned i=0;i<count&&r.Ok();++i){auto& g=f.glyphs[i];g.baseline=r.U16();auto code=r.U16();std::memcpy(&g.character,&code,2);
        if(g.character>=0&&g.character<256)f.map[g.character]=i;
        else if(g.character>=-32&&g.character<0)f.special[-g.character-1]=i;
        else return r.Fail("invalid font character map");}
    if(f.special[31])for(unsigned i=0;i<256;++i){if(!f.map[i]&&i!='A'&&i!='a')f.map[i]=f.special[31];if(i<31&&!f.special[i])f.special[i]=f.special[31];}
    r.U32();auto& t=f.texture;t.bytes=bytes;t.width=r.U16();t.height=r.U16();t.depth=r.U16();r.Skip(6);
    if(!dimensions(t.width,t.height)||t.depth!=8)return r.Fail("invalid font texture");t.actual_width=t.width;t.actual_height=t.height;t.swizzled=false;t.font_palette=true;t.palette_depth=32;
    auto n=(std::size_t(t.width)*t.height+3)&~std::size_t(3);t.levels.push_back({t.width,t.height,r.Position(),n});if(!r.Skip(n))return false;
    t.palette_offset=r.Position();t.palette_size=1024;if(!r.Skip(1024))return false;
    if(r.U32()!=count)return r.Fail("font subtexture count mismatch");
    for(auto& g:f.glyphs){g.x=r.U16();g.y=r.U16();g.width=r.U16();g.height=r.U16();
        // The retail newtimerfont intentionally has y=1,height=32 in a
        // 32-high atlas. Original PC UVs use WRAP: this is a one-texel border,
        // not an extra CPU image row. Keep metrics/UVs without unbounded reads.
        if(g.x>t.width||g.y>t.height||g.width>t.width||g.height>t.height||unsigned(g.x)+g.width>t.width+1||unsigned(g.y)+g.height>t.height+1)return r.Fail("font glyph outside atlas border");}
    if(!r.Ok()||r.Remaining())return r.Fail("font trailing bytes");out=std::move(f);return true;
}
}
