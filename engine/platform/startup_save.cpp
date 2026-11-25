#include <core/defines.h>
#include <source_port/launch_paths.h>
#include <source_port/diagnostics.h>
#include <source_port/save.h>
#include <source_port/input.h>
#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/symboltable.h>
#include <gfx/2d/screenelemman.h>
#include <sk/modules/skate/skate.h>
#include <sk/scripting/mcfuncs.h>

namespace SourcePort {
void LoadStartupSaveWhenReady() {
    static bool attempted = false;
    const char* name = StartupSaveName();
    if (attempted || !SaveStorageReady()) return;

    // The original card loader reapplies the story skater and refreshes CAS
    // through QB. Both the local skater and the frontend tree must exist.
    if (!Mdl::Skate::Instance()->GetSkater(0) ||
        !Front::CScreenElementManager::Instance()->GetElement(CRCD(0x56a1eae3,"root_window")) ||
        !Script::Resolve("post_load_from_memory_card")) return;
    attempted = true;
    if (!*name) {
#ifdef __3DS__
        // The opt-in automated menu route must also work on a clean profile.
        BeginCityMenuTrial();
#endif
        return;
    }
#ifdef __3DS__
    BootPhase("startup_save_enter");
#endif

    Script::CScript context;
    context.SetScript("NullScript");
    Script::CStruct sections;
    sections.AddChecksum(NONAME, CRCD(0xc4e78e22,"all"));
    CFuncs::ScriptSetSectionsToApplyWhenLoading(&sections, &context);

    Script::CStruct save;
    save.AddString("Name", name);
    save.AddChecksum("Type", CRCD(0xb010f357,"OptionsAndPros"));
    const bool loaded = CFuncs::ScriptLoadFromMemoryCard(&save, &context);
    sections.Clear();
    CFuncs::ScriptSetSectionsToApplyWhenLoading(&sections, &context);
    Log("STARTUP_SAVE name=%s loaded=%d", name, loaded);
#ifdef __3DS__
    if(loaded)BeginCityMenuTrial();
    BootPhase(loaded ? "startup_save_ready" : "startup_save_failed");
#endif
}
}
