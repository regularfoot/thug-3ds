// Production links WHOLE Sk/Main.cpp. The isolated adapter test's pre/post
// observers are NEVER linked into the game. libctru retains service ownership.
#include <source_port/application.h>
#include <source_port/diagnostics.h>
#include <source_port/fatal.h>
#include <cstdlib>
#ifdef __3DS__
extern "C" {unsigned int __stacksize__=512*1024;}
#endif
extern "C" void pre_main();
extern "C" void post_main();
namespace {
void close_normal_session() {
    if (SourcePort::FatalDiagnosticInProgress() || !SourcePort::ApplicationIsReady()) return;
#ifdef __3DS__
    SourcePort::Log("NATIVE_CRT_EXIT");
    SourcePort::CheckpointDiagnosticLog();
#endif
    post_main(); // after static destructors, with stdio still open
    SourcePort::StopApplication();
}
}
extern "C" void userAppInit() {
#ifdef __3DS__
    SourcePort::OpenBootTrace();
#endif
    SourcePort::StartApplication();
#ifdef __3DS__
    SourcePort::BootPhase("application_ready");
#endif
    pre_main(); // original Dbg and Mem, BEFORE original dynamic constructors
#ifdef __3DS__
    SourcePort::BootPhase("original_memory_ready");
    // Register before C++ constructors so normal exit runs their destructors
    // first, then closes our journal before newlib closes all FILE streams.
    if (std::atexit(close_normal_session) != 0) std::abort();
#endif
}
extern "C" void userAppExit() {
#ifndef __3DS__
    close_normal_session();
#endif
    // On 3DS this SDK hook is already past stdio cleanup, and abort/_exit
    // may get here on a worker. libctru owns service closure in either case.
}
