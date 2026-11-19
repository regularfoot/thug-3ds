#pragma once
#include <source_port/graphics_os.h>
#include <core/allmath.h>
#include <vector>
namespace SourcePort {
struct TextureAsset;
// Native device lifetime will be owned by the real Nx renderer. No private
// gameplay loop, synthetic scene, or host software renderer is installed.
void OpenGraphics();
void CloseGraphics();
bool GraphicsReady();
void CameraView(const Mth::Matrix& axes,const Mth::Vector& position,float* rows,bool camera_relative=false);
bool ProjectWorld(const Mth::Matrix& axes,const Mth::Vector& camera,
    const Mth::Vector& world,float hfov,float aspect,float near_clip,float far_clip,
    float& x,float& y,float& depth,float& distance);
bool TilePaddedRGBA(const uint8_t* rgba,unsigned source_width,unsigned source_height,
    unsigned width,unsigned height,std::vector<uint8_t>& tiled);
bool TileCompactTexture(const std::vector<uint8_t>& rgba,unsigned source_width,unsigned source_height,
    unsigned width,unsigned height,SourceTextureFormat& format,std::vector<uint8_t>& tiled);
bool TileRGBA(const uint8_t* rgba,unsigned width,unsigned height,std::vector<uint8_t>& tiled);
bool BuildOpaqueWorldMipChain(const TextureAsset&,unsigned limit,unsigned& width,unsigned& height,
    unsigned& levels,std::vector<uint8_t>& tiled);
bool ReduceTextureRGBA(const std::vector<uint8_t>& rgba,unsigned width,unsigned height,unsigned limit,
    std::vector<uint8_t>& reduced,unsigned& reduced_width,unsigned& reduced_height);
}
