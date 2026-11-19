#include <core/defines.h>
#include <gfx/nx.h>
#include <sys/sio/keyboard.h>
#include <source_port/thread_os.h>
// These APIs query optional PHYSICAL devices/services. New 3DS has neither a
// USB keyboard host nor the Xbox dashboard's ripped-CD catalogue. Reporting
// absent hardware does not replace the original pad-operated text editor or
// retail music/playlist owners; both continue through their original code.
namespace SIO {
int KeyboardRead(char* makes){Dbg_Assert(source_thread_is_controller());if(!makes){errno=EINVAL;return 0;}errno=ENODEV;return 0;}
void KeyboardClear(){Dbg_Assert(source_thread_is_controller());errno=ENODEV;}
}
namespace Nx {
int CEngine::s_plat_get_num_soundtracks(){Dbg_Assert(source_thread_is_controller());return 0;}
const char* CEngine::s_plat_get_soundtrack_name(int){Dbg_Assert(source_thread_is_controller());errno=ENOENT;return nullptr;}
}
