#pragma once
#include <source_port/native_world.h>
#include <gfx/nxmiscfx.h>
namespace SourcePort {
// Real source triangle stack and the console subdivision/interpolation rules.
void SubdivideEffectBatches(std::vector<WorldBatch>&,float squared_area);
void ShatterGeometry(std::vector<WorldBatch>);
bool EffectTriangleIntersectsTexture(const SourceRenderVertex*);
bool TextureSplatOnGeometry(const std::vector<WorldBatch>&,Mth::Vector&,Mth::Vector&,float,float,Nx::CTexture*,Nx::sSplatTrailInstanceDetails* =nullptr);
void ReleaseOriginalEffectTables();
}
