#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SourceRenderOptions {
    int performance, detailed_shadows, motion_blur, secondary_passes, dynamic_lighting;
    unsigned texture_limit;
    int atmospheric_effects;
} SourceRenderOptions;
const SourceRenderOptions* source_render_options(void);
#ifdef __cplusplus
}
#endif
