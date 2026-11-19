#include <source_port/native_ui.h>
#include <source_port/thread_os.h>
#include <core/string/stringutils.h>
#include <string>
#include <algorithm>
namespace {
Nx::CNativeFont* buttons=nullptr;SourcePort::FontMetaMapper meta_mapper=nullptr;uint64 layout_generation=0;
void controller(){Dbg_Assert(source_thread_is_controller());}
}
namespace SourcePort {
void BindOriginalFontMetaMapper(FontMetaMapper mapper){controller();meta_mapper=mapper;++layout_generation;}
uint64 FontLayoutGeneration(){controller();return layout_generation;}
}
namespace Nx {
CNativeFont::CNativeFont(){m_checksum=0;colors_.fill(Image::RGBA(128,128,128,128));}
CNativeFont::~CNativeFont(){if(buttons==this){buttons=nullptr;++layout_generation;}}
bool CNativeFont::Assign(SourcePort::FontAsset asset){
    controller();if(asset.glyphs.empty()||!texture_.Assign(asset.texture))return false;
    asset_=std::move(asset);space_spacing_=asset_.glyphs[asset_.map['I']].width;++revision_;++layout_generation;return true;
}
bool CNativeFont::plat_load(const char* name){
    if(!name||std::strlen(name)>400)return false;SourcePort::FontAsset asset;
    SourcePort::ResourceError error;auto bytes=SourcePort::ReadRenderResource((std::string("fonts/")+name+".fnt.xbx").c_str());
    if(!SourcePort::ReadFont(bytes,asset,&error)){SourcePort::Log("Original font %s rejected at %u: %s",name,unsigned(error.offset),error.reason?error.reason:"missing file");return false;}
    return Assign(std::move(asset));
}
void CNativeFont::plat_set_spacings(int chars,int spaces){controller();char_spacing_=chars;if(spaces>0)space_spacing_=spaces;++revision_;}
void CNativeFont::plat_set_rgba_table(Image::RGBA* table){controller();Dbg_Assert(table);std::copy_n(table,16,colors_.begin());++revision_;}
void CNativeFont::plat_mark_as_button_font(bool is_button){controller();if(is_button)buttons=this;else if(buttons==this)buttons=nullptr;++revision_;++layout_generation;}
void CNativeFont::plat_unload(){controller();texture_.RemoveFromVram();asset_={};if(buttons==this)buttons=nullptr;++revision_;++layout_generation;}
bool CNativeFont::Layout(const char* text,float x,float y,float sx,float sy,Image::RGBA default_color,bool override_color,std::vector<Glyph>* output,float& width)const{
    controller();if(!text||asset_.glyphs.empty())return false;std::size_t length=strnlen(text,65537);if(length>65536)return false;
    float cursor=x;Image::RGBA color=default_color;const char* p=text;const char* end=text+length;
    while(p<end){const CNativeFont* font=this;const SourcePort::FontGlyph* glyph=nullptr;bool escaped=false;
        while(p<end&&*p=='\\'&&!escaped){
            if(++p==end)return false;char tag=*p++;
            if(tag=='\\'){escaped=true;glyph=&font->asset_.glyphs[font->asset_.map['\\']];break;}
            if(p==end)return false;
            if(tag=='c'||tag=='C'){unsigned digit=Str::DehexifyDigit(p++);if(digit>16)return false;color=(!digit||override_color)?default_color:colors_[digit-1];continue;}
            unsigned digit;
            if(tag=='m'||tag=='M'){if(!meta_mapper)return false;char mapped=meta_mapper(p++);digit=Str::DehexifyDigit(&mapped);font=buttons;}
            else if(tag=='b'||tag=='B'){digit=Str::DehexifyDigit(p++);font=buttons;}
            else if(tag=='s'||tag=='S')digit=Str::DehexifyDigit(p++);
            else return false;
            if(!font||digit>=32||font->asset_.glyphs.empty())return false;glyph=&font->asset_.glyphs[font->asset_.special[digit]];escaped=true;
        }
        if(!glyph){if(p==end)break;unsigned ch=uint8(*p++);if(ch==' '){cursor+=(space_spacing_+char_spacing_)*sx;continue;}glyph=&asset_.glyphs[asset_.map[ch]];}
        if(output&&glyph->width&&glyph->height)output->push_back({const_cast<CNativeTexture*>(&font->texture_),cursor,y+(float(font->asset_.base)-glyph->baseline)*sy,
            glyph->width*sx,glyph->height*sy,float(glyph->x)/font->asset_.texture.width,float(glyph->y)/font->asset_.texture.height,
            float(glyph->x+glyph->width)/font->asset_.texture.width,float(glyph->y+glyph->height)/font->asset_.texture.height,color});
        cursor+=(glyph->width+char_spacing_)*sx;
    }width=cursor-x;return true;
}
void CNativeFont::plat_query_string(char* text,float& width,float& height)const{
    bool ok=Layout(text,0,0,1,1,Image::RGBA(128,128,128,128),false,nullptr,width);Dbg_MsgAssert(ok,("Invalid original font tag/string"));height=asset_.height;
}
}
