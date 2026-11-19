#pragma once
#include <source_port/render_assets.h>
namespace SourcePort {
struct MaterialPassFrame {uint32 texture=0;std::array<float,2> uv_offset{};};
struct MaterialFrame {std::vector<MaterialPassFrame> passes;std::vector<std::array<uint8,4>> vertex_colors;};
// The original renderer samples Tmr::GetTime for UV/texture wibble and its
// fixed render_start_time for color wibble. Share one evaluated frame per eye.
bool EvaluateMaterial(const MaterialAsset&,uint32 timer_ms,uint32 render_start_ms,MaterialFrame&);
}
