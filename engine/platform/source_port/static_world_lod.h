#pragma once
#include <source_port/render_assets.h>

namespace SourcePort {
// Sidecars bind to the size and case-sensitive CRC of the original scene.
constexpr uint32 StaticLodVersion = 2;
constexpr std::size_t StaticLodMaxBytes = 2 * 1024 * 1024;
constexpr float StaticLodMaxError = 12.f;

uint32 SceneLodChecksum(const ResourceBytes&);
bool CanSimplifyWorldMesh(const SectorAsset&, const MeshAsset&, const MaterialAsset&);
bool ReadSceneLods(const ResourceBytes& sidecar, const ResourceBytes& scene, SceneAsset&);
void LoadSceneLods(const char* scene_name, SceneSignature scene, SceneAsset&);
}
