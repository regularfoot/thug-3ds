#pragma once
namespace SourcePort {
// Lexical owners inside the actual original Main.cpp heap scope. Device opens
// before original game singletons; engine teardown follows StopAllModules and
// precedes their destruction. Neither class replaces the original game loop.
class GraphicsSession {
public:GraphicsSession();~GraphicsSession();
    GraphicsSession(const GraphicsSession&)=delete;GraphicsSession& operator=(const GraphicsSession&)=delete;
};
class EngineSession {
public:EngineSession();~EngineSession();
    EngineSession(const EngineSession&)=delete;EngineSession& operator=(const EngineSession&)=delete;
};
bool OriginalNativeEngineStarted();
}
