#include <source_port/native_engine.h>
#include <source_port/native_renderer.h>
#include <source_port/native_effects.h>
#include <source_port/native_weather.h>
#include <source_port/native_loadscreen.h>
#include <source_port/native_projection.h>
#include <source_port/native_ui.h>
#include <source_port/graphics.h>
#include <source_port/diagnostics.h>
#include <source_port/thread_os.h>
#include <gfx/nx.h>
#include <gfx/nxparticlemgr.h>
#include <gfx/nximposter.h>
#include <gfx/nxviewman.h>
#include <gfx/nxfontman.h>
#include <gfx/nxtexman.h>
#include <gfx/nxtexture.h>
namespace {bool started=false;}
namespace SourcePort {
bool OriginalNativeEngineStarted(){Dbg_Assert(source_thread_is_controller());return started;}
GraphicsSession::GraphicsSession(){
#ifdef __3DS__
    BootPhase("graphics_init_enter");
#endif
    OpenGraphics();
#ifdef __3DS__
    BootPhase("graphics_ready");
#endif
}
GraphicsSession::~GraphicsSession(){
    Dbg_Assert(!started);
    ReleaseNativeUI();Nx::CFontManager::sReleaseNativeFonts();
    Nx::CTexDictManager::sp_sprite_tex_dict->FlushTextures(true);
    Nx::CTexDictManager::sp_particle_tex_dict->FlushTextures(true);
    ReleaseNativeWorldRegistry();CloseGraphics();
}
EngineSession::EngineSession(){Dbg_Assert(started&&GraphicsReady()&&WorldRendererReady());}
EngineSession::~EngineSession(){Nx::CEngine::sShutdownNativeEngine();}
}
namespace Nx {
void CEngine::s_plat_start_engine(){
    Dbg_Assert(source_thread_is_controller()&&SourcePort::GraphicsReady()&&!started&&!mp_particle_manager&&!mp_weather);
    mp_particle_manager=new CNativeNewParticleManager;mp_weather=new CNativeWeather;
    SourcePort::OpenWorldRenderer();started=true;
#ifdef __3DS__
    SourcePort::BootPhase("world_renderer_ready");
#endif
}
void CEngine::sShutdownNativeEngine(){
    Dbg_Assert(source_thread_is_controller()&&started&&SourcePort::GraphicsReady());
    Dbg_Assert(source_graphics_wait()==0);
    // Original Main has stopped all game modules before this scope ends.
    // Keep their actual object/QB/singleton owners alive for particle cleanup.
    SourcePort::ReleaseLoadingScreen();destroy_all_particles();mp_particle_manager->Cleanup();
    SourcePort::ReleaseOriginalEffectTables();
    mp_imposter_manager->Cleanup();
    SourcePort::ReleaseNativeProjections();sUnloadAllScenesAndTexDicts();CViewportManager::sCleanup();
    delete mp_weather;mp_weather=nullptr;delete mp_particle_manager;mp_particle_manager=nullptr;
    delete mp_imposter_manager;mp_imposter_manager=nullptr;delete p_particle_table;p_particle_table=nullptr;
    SourcePort::CloseWorldRenderer();started=false;
}
}
