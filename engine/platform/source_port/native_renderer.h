#pragma once
namespace SourcePort {
// Owned by the original engine's platform startup/shutdown, after the native
// GPU opens and before its device closes. No replacement simulation loop.
void OpenWorldRenderer();
void CloseWorldRenderer();
bool WorldRendererReady();
bool WorldRendererFrameOpen();
void ReleaseWorldSceneBuffers();
}
