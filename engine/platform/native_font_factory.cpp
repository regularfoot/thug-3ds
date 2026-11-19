#include <source_port/native_ui.h>
#include <gfx/nxfontman.h>
namespace Nx {
CFont* CFontManager::s_plat_load_font(const char* name){
    SourcePort::BindOriginalFontMetaMapper(CFontManager::sMapMetaCharacterToButton);
    auto* font=new CNativeFont;bool loaded=font->Load(name);if(!loaded){delete font;Dbg_MsgAssert(false,("Missing/corrupt original font %s",name));return nullptr;}return font;
}
void CFontManager::s_plat_unload_font(CFont* font){delete font;}
void CFontManager::sReleaseNativeFonts(){
    // The font table outlives the native graphics session unless its
    // original unload path is called explicitly after the last UI owner.
    for(auto& entry:s_font_tab)if(entry.mp_font)sUnloadFont(entry.mName);
    SourcePort::BindOriginalFontMetaMapper(nullptr);
}
}
