#pragma once
namespace Dbg {
void SetUp();
void CloseDown();
void screen_assert(bool on = false);
}
namespace SourcePort {
// Display belongs to the renderer, not this logger. The callback must quiesce
// the GPU before presenting a fatal message; no console/GSP takeover behind
// the renderer's back. Enabling an unbound display fails explicitly.
using AssertionDisplay = void (*)(const char* text, void* owner);
void BindAssertionDisplay(AssertionDisplay display, void* owner);
void UnbindAssertionDisplay(void* owner);
bool DiagnosticSessionReady();
void Log(const char* format, ...);
#ifdef __3DS__
void OpenBootTrace();
void BootPhase(const char* phase);
#endif
// Append-only on disk. Routine messages stay in a rolling memory buffer
// until a terminal diagnostic or a normal exit.
bool OpenDiagnosticLog(const char* path);
bool CheckpointDiagnosticLog(); // after the game loop stops, before original teardown
void RequestDiagnosticCheckpoint();
enum class DiagnosticFramePhase : unsigned { Idle, Logic, Geometry, Upload, HUD, Submit };
void MarkDiagnosticFrame(unsigned frame, DiagnosticFramePhase phase);
bool CloseDiagnosticLog();
}
