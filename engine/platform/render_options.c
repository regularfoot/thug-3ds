#include <source_port/render_options.h>
const SourceRenderOptions* source_render_options(void) {
#ifdef __3DS__
    static const SourceRenderOptions options={1,0,0,0,0,128,0};
#else
    static const SourceRenderOptions options={0,1,1,1,1,0,1};
#endif
    return &options;
}
