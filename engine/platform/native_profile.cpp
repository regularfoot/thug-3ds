#include <core/defines.h>
#include <source_port/native_profile.h>
#include <gel/scripting/struct.h>
#include <sk/modules/skate/skate.h>
#include <sk/objects/playerprofilemanager.h>
#include <sk/objects/skaterprofile.h>
namespace SourcePort {
// Retail PC CAS QB calls this command, but the upstream PC function table
// bound it to DummyCommandFalse. Use the same original profile classification
// as CurrentSkaterIsPro and the original custom appearance operations.
bool NativeSkaterIsCustom(Script::CStruct* params,Script::CScript*){
    int player=0;params->GetInteger("player",&player);
    auto* manager=Mdl::Skate::Instance()->GetPlayerProfileManager();Dbg_Assert(manager);
    auto* profile=manager->GetProfile(player);Dbg_Assert(profile);
    return !profile->IsPro();
}
}
