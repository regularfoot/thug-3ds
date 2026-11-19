#pragma once
#include <source_port/native_world.h>
namespace SourcePort {
struct ProjectionCamera {Mth::Matrix axes;Mth::Vector position;ProjectionCamera(){axes.Ident();position.Set(0,0,0,1);}};
// Original shadow projector: right-handed LookAt, 96x96 ortho, near=1/far=128.
ProjectionCamera OriginalProjectionCamera(const Mth::Vector& position,const Mth::Vector& forward);
bool ProjectionIntersectsBounds(const ProjectionCamera&,const Mth::CBBox&);
bool RasterizeOriginalShadow(const ProjectionCamera&,unsigned width,unsigned height,unsigned z_depth,const std::vector<WorldBatch>&,std::vector<uint8>&);
void ProjectOriginalShadow(const ProjectionCamera&,Nx::CNativeTexture*,const std::vector<WorldBatch>&,std::vector<WorldBatch>&);
void ProjectOriginalSceneShadow(Nx::CScene*,const WorldView&,uint32,const ProjectionCamera&,Nx::CNativeTexture*,std::vector<WorldBatch>&);
void RenderOriginalShadowTargets(uint32 time);
void GatherOriginalProjectedShadows(const WorldView&,uint32 time,std::vector<WorldBatch>&);
void ForgetProjectionTexture(Nx::CNativeTexture*);
void ForgetProjectionModel(Nx::CModel*);
void ForgetProjectionScene(Nx::CScene*);
void ReleaseNativeProjections();
}
