#include <core/defines.h>
#include <gfx/nx.h>
#include <source_port/graphics_os.h>
#include <source_port/graphics.h>
namespace Nx {
void CEngine::s_plat_set_screen_blur(uint32 amount){Dbg_MsgAssert(source_graphics_set_blur(amount)==0,("Native screen-blur owner failed errno=%d",errno));}
const char* CEngine::s_plat_get_platform_extension() {
    // Resource-format tag, not a hardware identity: original PC retail assets
    // retain the Xbx suffix. Native File resolves their real on-disc spelling.
    return "Xbx";
}
void CEngine::s_plat_finish_rendering() {
    if(!SourcePort::GraphicsReady())return; // no device has acquired/submitted GPU work
    const int result=source_graphics_wait();
    Dbg_MsgAssert(result==0,("Native GPU fence failed errno=%d",errno));
}
}
