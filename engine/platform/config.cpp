#include <core/defines.h>
#include <sys/config/config.h>
#include <source_port/launch_paths.h>
#include <source_port/diagnostics.h>
#ifdef __3DS__
extern "C" {
#include <3ds/services/cfgu.h>
#include <3ds/result.h>
}
#endif

namespace Config {
void Plat_Init(sint argc, char** argv) {
#ifdef __3DS__
    SourcePort::BootPhase("main_config_enter");
    Dbg_Assert(R_SUCCEEDED(cfguInit()));
    u8 language=0, region=0;
    const auto lr=CFGU_GetSystemLanguage(&language);
    const auto rr=CFGU_SecureInfoGetRegion(&region);
    cfguExit();
    Dbg_Assert(R_SUCCEEDED(lr) && R_SUCCEEDED(rr));
    gHardware=HARDWARE_N3DS;
    gFPS=60;
    gDisplayType=DISPLAY_TYPE_NTSC;
    gCD=true; // Original retail asset layout on SD, not the dev loose-file tree.
    gGotExtraMemory=false; // No PS2 debug RAM at its hardcoded address.
    gTerritory = region==CFG_REGION_USA ? TERRITORY_US
        : (region==CFG_REGION_EUR || region==CFG_REGION_AUS) ? TERRITORY_EUROPE : TERRITORY_ASIA;
    switch (language) {
        case CFG_LANGUAGE_JP: gLanguage=LANGUAGE_JAPANESE; break;
        case CFG_LANGUAGE_FR: gLanguage=LANGUAGE_FRENCH; break;
        case CFG_LANGUAGE_DE: gLanguage=LANGUAGE_GERMAN; break;
        case CFG_LANGUAGE_IT: gLanguage=LANGUAGE_ITALIAN; break;
        case CFG_LANGUAGE_ES: gLanguage=LANGUAGE_SPANISH; break;
        case CFG_LANGUAGE_NL: gLanguage=LANGUAGE_DUTCH; break;
        case CFG_LANGUAGE_PT: gLanguage=LANGUAGE_PORTUGUESE; break;
        default: gLanguage=LANGUAGE_ENGLISH; break; // Languages absent from original THUG1 enum.
    }
    SourcePort::BootPhase("system_config_ready");
    SourcePort::ConfigureGameLaunchPaths(argc,argv);
    SourcePort::BootPhase("launch_paths_ready");
#else
    SourcePort::Assertion(__FILE__,__LINE__,"physical CFGU settings unavailable in host-only tests");
#endif
}
}
