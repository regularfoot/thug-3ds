#pragma once
// Original Sk/Main.cpp outer restart loop flag. Main-thread owned: workers
// must not mutate the engine's non-thread-safe lifecycle.
extern bool gbQuit;
namespace Script { class CStruct; class CScript; }
namespace SourcePort {
void StartApplication();
void StopApplication();
void RequestApplicationQuit();
bool ScriptQuitGame(Script::CStruct*, Script::CScript*);
bool PumpApplication();
bool ApplicationIsReady();
}
