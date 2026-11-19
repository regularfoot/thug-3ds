#include <core/defines.h>
#include <source_port/application.h>
#include <source_port/application_os.h>
#include <source_port/diagnostics.h>
#include <source_port/thread_os.h>
#include <cerrno>

bool gbQuit = false;
namespace { bool ready = false; }
namespace SourcePort {
void StartApplication() {
    Dbg_Assert(source_thread_is_controller() && !ready);
    int result = source_application_os_init();
    Dbg_MsgAssert(result == 0, ("Native application initialization errno=%d", errno));
    gbQuit = false; ready = true;
}
void StopApplication() {
    Dbg_Assert(source_thread_is_controller() && ready);
    int result = source_application_os_exit();
    Dbg_MsgAssert(result == 0, ("Native application shutdown errno=%d", errno));
    ready = false;
}
void RequestApplicationQuit() {
    Dbg_Assert(source_thread_is_controller() && ready);
    gbQuit = true;
}
bool ScriptQuitGame(Script::CStruct*, Script::CScript*) {
    RequestApplicationQuit();
    Log("QB_QUIT_GAME");
    return true;
}
bool PumpApplication() {
    Dbg_Assert(source_thread_is_controller() && ready);
    if (!gbQuit) {
        int result = source_application_os_poll();
        Dbg_MsgAssert(result >= 0, ("Native application event failure errno=%d", errno));
        if (!result) gbQuit = true;
    }
    return !gbQuit;
}
bool ApplicationIsReady() { return ready; }
}
