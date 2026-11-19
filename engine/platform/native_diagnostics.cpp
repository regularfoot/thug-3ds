#include <core/defines.h>
#include <source_port/native_diagnostics.h>
#include <source_port/graphics_os.h>
#include <source_port/memory.h>
#include <source_port/clock_os.h>
#include <source_port/thread_os.h>
#include <source_port/diagnostics.h>
#include <gfx/gfxman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>
namespace {bool memory_view=false;uint64 last_memory=0;std::string screenshot;}
void MemViewToggle(){Dbg_Assert(source_thread_is_controller());memory_view=!memory_view;last_memory=0;if(!memory_view)source_graphics_debug_text("");}
namespace Gfx {
void Manager::DumpVRAMUsage(){SourceGraphicsStats stats{};if(source_graphics_stats(&stats)){SourcePort::Log("GPU allocation report unavailable errno=%d",errno);return;}
    SourcePort::Log("native GPU meshes=%u vertex-bytes=%u textures=%u texture-bytes=%u VRAM-free=%u linear-free=%u",stats.meshes,stats.vertex_bytes,stats.textures,stats.texture_bytes,stats.vram_free,stats.linear_free);
}
void Manager::ScreenShot(const char* root){Dbg_Assert(source_thread_is_controller());if(!root||!*root||std::strlen(root)>40||strspn(root,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::strlen(root)){errno=EINVAL;SourcePort::Log("Rejected invalid capture name");return;}screenshot=root;}
}
namespace SourcePort {
void FinishNativeDiagnostics(){
    Dbg_Assert(source_thread_is_controller());auto now=source_clock_now();
    if(memory_view&&now-last_memory>=source_clock_frequency()){
        last_memory=now;char text[2048];std::size_t at=0;
        for(auto* heap=Mem::Manager::sHandle().FirstHeap();heap&&at<sizeof(text)-160;heap=Mem::Manager::sHandle().NextHeap(heap)){int n=std::snprintf(text+at,sizeof(text)-at,"%s: %d bytes / %d blocks\n",heap->GetName(),heap->mUsedMem.m_count,heap->mUsedBlocks.m_count);if(n<0)break;at+=std::min<std::size_t>(n,sizeof(text)-at-1);}
        text[at]=0;source_graphics_debug_text(text);
    }
    if(screenshot.empty())return;std::string base;std::swap(base,screenshot);std::vector<uint8> rgb(400*240*3);
    if(source_graphics_capture_rgb(rgb.data(),rgb.size())){Log("Capture unavailable errno=%d",errno);return;}
    const char* directory="source-captures";if(mkdir(directory,0700)&&errno!=EEXIST){Log("Cannot create capture directory errno=%d",errno);return;}
    struct stat st{};if(lstat(directory,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)){Log("Capture directory is not owned storage");return;}
    char path[256];int fd=-1;for(unsigned i=0;i<10000;++i){std::snprintf(path,sizeof(path),"%s/%s%04u.ppm",directory,base.c_str(),i);fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd>=0||errno!=EEXIST)break;}
    if(fd<0){Log("Cannot create unique capture errno=%d",errno);return;}
    constexpr char header[]="P6\n400 240\n255\n";bool ok=write(fd,header,sizeof(header)-1)==sizeof(header)-1&&write(fd,rgb.data(),rgb.size())==ssize_t(rgb.size());if(close(fd))ok=false;
    Log("Native screenshot %s: %s",path,ok?"written":"write failed");
}
}
