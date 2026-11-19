#include <core/defines.h>
#include <source_port/graphics.h>
#include <source_port/render_assets.h>
#include <source_port/timer.h>
#include <source_port/diagnostics.h>
#include <source_port/thread_os.h>
#include <cmath>
namespace {
bool opened=false;
void fatal(const char* text,void*) { source_graphics_fatal(text); }
}
namespace SourcePort {
void OpenGraphics() {
    Dbg_Assert(source_thread_is_controller() && !opened);
    const int result=source_graphics_open();
    Dbg_MsgAssert(result==0,("Native GPU initialization failed errno=%d",errno));
    opened=true;BindNativeTimerDisplay(&opened);BindAssertionDisplay(fatal,&opened);
}
void CloseGraphics() {
    Dbg_Assert(source_thread_is_controller() && opened);
    UnbindAssertionDisplay(&opened);UnbindTimerVideoSource(&opened);
    const int result=source_graphics_close();
    if(result&&errno==EBUSY){
        SourceGraphicsStats stats{};
        if(source_graphics_stats(&stats)==0)
            Log("GPU_CLOSE_BUSY meshes=%u bytes=%u textures=%u bytes=%u",
                stats.meshes,stats.vertex_bytes,stats.textures,stats.texture_bytes);
        errno=EBUSY;
    }
    Dbg_MsgAssert(result==0,("Native GPU owners still live or shutdown failed errno=%d",errno));
    opened=false;
}
bool GraphicsReady() { Dbg_Assert(source_thread_is_controller());return opened; }
bool ReduceTextureRGBA(const std::vector<uint8_t>& rgba,unsigned width,unsigned height,unsigned limit,
    std::vector<uint8_t>& out,unsigned& reduced_width,unsigned& reduced_height){
    if(!width||!height||width>4096||height>4096||!limit||limit>1024||rgba.size()!=std::size_t(width)*height*4)return false;
    unsigned factor=1;while((width+factor-1)/factor>limit||(height+factor-1)/factor>limit)factor*=2;
    unsigned w=(width+factor-1)/factor,h=(height+factor-1)/factor;std::vector<uint8_t> result(std::size_t(w)*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){uint64 sum[4]{};unsigned count=0;
        for(unsigned yy=y*factor;yy<std::min(height,(y+1)*factor);++yy)for(unsigned xx=x*factor;xx<std::min(width,(x+1)*factor);++xx){
            auto at=(std::size_t(yy)*width+xx)*4;for(unsigned c=0;c<4;++c)sum[c]+=rgba[at+c];++count;}
        for(unsigned c=0;c<4;++c)result[(std::size_t(y)*w+x)*4+c]=(sum[c]+count/2)/count;
    }out.swap(result);reduced_width=w;reduced_height=h;return true;
}
void CameraView(const Mth::Matrix& axes,const Mth::Vector& position,float* rows,bool camera_relative) {
    Dbg_Assert(rows);
    // Original Mth stores right/up/back as rows and uses row-vector transforms.
    // The shader consumes column vectors: the inverse camera uses these axes
    // as rows, with translation -dot(axis,position), not the camera's world Pos.
    for (unsigned r=0;r<3;++r) {
        for (unsigned c=0;c<3;++c) rows[r*4+c]=axes[r][c];
        rows[r*4+3]=camera_relative?0:-(axes[r][0]*position[0]+axes[r][1]*position[1]+axes[r][2]*position[2]);
    }
    rows[12]=rows[13]=rows[14]=0;rows[15]=1;
}
bool ProjectWorld(const Mth::Matrix& axes,const Mth::Vector& camera,const Mth::Vector& world,
    float hfov,float aspect,float near_clip,float far_clip,float& x,float& y,float& depth,float& distance) {
    Dbg_Assert(hfov>0 && hfov<180 && aspect>0 && near_clip>0 && far_clip>near_clip);
    float view[16];CameraView(axes,camera,view);
    float p[3]{};
    for (unsigned r=0;r<3;++r) p[r]=view[r*4]*world[0]+view[r*4+1]*world[1]+view[r*4+2]*world[2]+view[r*4+3];
    distance=-p[2];
    if (!std::isfinite(distance)||distance<near_clip||distance>far_clip) return false;
    float tan_h=std::tan(hfov*3.14159265358979323846f/360.f);
    x=p[0]/(distance*tan_h);y=p[1]*aspect/(distance*tan_h);
    depth=far_clip/(far_clip-near_clip)*(1-near_clip/distance);
    return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(depth);
}
namespace {
template<unsigned Max>
unsigned quantize(unsigned value){return (value*Max+127)/255;}

template<SourceTextureFormat Format>
void store_texel(const uint8_t* rgba,uint8_t* out){
    if constexpr(Format==SOURCE_TEXTURE_RGBA8){
        out[0]=rgba[3];out[1]=rgba[2];out[2]=rgba[1];out[3]=rgba[0];
    }else{
        unsigned packed;
        if constexpr(Format==SOURCE_TEXTURE_RGB565)
            packed=quantize<31>(rgba[0])<<11|quantize<63>(rgba[1])<<5|quantize<31>(rgba[2]);
        else if constexpr(Format==SOURCE_TEXTURE_RGBA5551)
            packed=quantize<31>(rgba[0])<<11|quantize<31>(rgba[1])<<6|quantize<31>(rgba[2])<<1|unsigned(rgba[3]!=0);
        else if constexpr(Format==SOURCE_TEXTURE_RGBA4)
            packed=quantize<15>(rgba[0])<<12|quantize<15>(rgba[1])<<8|quantize<15>(rgba[2])<<4|quantize<15>(rgba[3]);
        else
            packed=unsigned(rgba[0])<<8|rgba[3];
        out[0]=uint8_t(packed);
        out[1]=uint8_t(packed>>8);
    }
}

template<SourceTextureFormat Format>
bool tile_padded(const uint8_t* rgba,unsigned source_width,unsigned source_height,
    unsigned width,unsigned height,std::vector<uint8_t>& tiled) {
    if(!rgba||!source_width||!source_height||source_width>width||source_height>height||
       width<8||height<8||width>1024||height>1024||(width&(width-1))||(height&(height-1))){
        errno=EINVAL;return false;
    }
    constexpr unsigned bytes_per_texel=Format==SOURCE_TEXTURE_RGBA8?4:2;
    std::vector<uint8_t> output(std::size_t(width)*height*bytes_per_texel);
    constexpr unsigned x_offset[]={0,1,4,5,16,17,20,21};
    constexpr unsigned y_offset[]={0,2,8,10,32,34,40,42};
    for(unsigned tile_y=0;tile_y<height;tile_y+=8){
        for(unsigned tile_x=0;tile_x<width;tile_x+=8){
            auto* tile=output.data()+std::size_t(tile_y*width+tile_x*8)*bytes_per_texel;
            for(unsigned y=0;y<8;++y){
                const auto* row=rgba+std::size_t(std::min(tile_y+y,source_height-1))*source_width*4;
                for(unsigned x=0;x<8;++x){
                    const auto* from=row+std::min(tile_x+x,source_width-1)*4;
                    store_texel<Format>(from,tile+(x_offset[x]+y_offset[y])*bytes_per_texel);
                }
            }
        }
    }
    tiled.swap(output);
    return true;
}
}
bool TilePaddedRGBA(const uint8_t* rgba,unsigned source_width,unsigned source_height,
    unsigned width,unsigned height,std::vector<uint8_t>& tiled){
    return tile_padded<SOURCE_TEXTURE_RGBA8>(rgba,source_width,source_height,width,height,tiled);
}
bool TileCompactTexture(const std::vector<uint8_t>& rgba,unsigned source_width,unsigned source_height,
    unsigned width,unsigned height,SourceTextureFormat& format,std::vector<uint8_t>& tiled){
    if(source_width>1024||source_height>1024||rgba.size()!=std::size_t(source_width)*source_height*4){
        errno=EINVAL;
        return false;
    }
    bool grayscale=true,opaque=true,binary_alpha=true;
    for(std::size_t i=0;i<rgba.size();i+=4){
        const auto alpha=rgba[i+3];
        grayscale&=rgba[i]==rgba[i+1]&&rgba[i]==rgba[i+2];
        opaque&=alpha==255;
        binary_alpha&=alpha==0||alpha==255;
        if(!grayscale&&!binary_alpha)break;
    }
    if(grayscale){
        format=SOURCE_TEXTURE_LA8;
        return tile_padded<SOURCE_TEXTURE_LA8>(rgba.data(),source_width,source_height,width,height,tiled);
    }
    if(opaque){
        format=SOURCE_TEXTURE_RGB565;
        return tile_padded<SOURCE_TEXTURE_RGB565>(rgba.data(),source_width,source_height,width,height,tiled);
    }
    if(binary_alpha){
        format=SOURCE_TEXTURE_RGBA5551;
        return tile_padded<SOURCE_TEXTURE_RGBA5551>(rgba.data(),source_width,source_height,width,height,tiled);
    }
    format=SOURCE_TEXTURE_RGBA4;
    return tile_padded<SOURCE_TEXTURE_RGBA4>(rgba.data(),source_width,source_height,width,height,tiled);
}
bool TileRGBA(const uint8_t* rgba,unsigned width,unsigned height,std::vector<uint8_t>& tiled) {
    return TilePaddedRGBA(rgba,width,height,width,height,tiled);
}
bool BuildOpaqueWorldMipChain(const TextureAsset& asset,unsigned limit,unsigned& width,unsigned& height,
    unsigned& levels,std::vector<uint8_t>& tiled){
    const auto pot=[](unsigned size){return size>=8&&!(size&(size-1));};
    if(asset.image||asset.font_palette||!pot(asset.width)||!pot(asset.height)||limit<16||limit>1024)return false;
    unsigned first=0;
    while(first<asset.levels.size()&&std::max(asset.levels[first].width,asset.levels[first].height)>limit)++first;
    if(first+1>=asset.levels.size())return false;
    const auto& base=asset.levels[first];
    if(!pot(base.width)||!pot(base.height))return false;
    std::vector<uint8_t> bytes,rgba,packed;
    unsigned w=base.width,h=base.height,count=0;
    for(unsigned level=first;level<asset.levels.size()&&w>=8&&h>=8;++level,w/=2,h/=2){
        const auto& info=asset.levels[level];
        if(info.width!=w||info.height!=h||!DecodeTextureLevel(asset,level,rgba))return false;
        bool opaque=true;
        for(std::size_t a=3;a<rgba.size();a+=4)if(rgba[a]!=255){opaque=false;break;}
        if(!opaque)break;
        if(!tile_padded<SOURCE_TEXTURE_RGB565>(rgba.data(),w,h,w,h,packed))return false;
        bytes.insert(bytes.end(),packed.begin(),packed.end());
        ++count;
    }
    if(count<2)return false;
    width=base.width;height=base.height;levels=count;tiled.swap(bytes);
    return true;
}
}
