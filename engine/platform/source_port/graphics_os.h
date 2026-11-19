#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SourceGpuMesh SourceGpuMesh;
typedef struct SourceGpuTexture SourceGpuTexture;
// Sorted live-handle index. Lookups compare integer addresses without ever
// dereferencing an untrusted/stale handle. Inserts/removals happen at ownership
// changes, not once per vertex or submitted packet.
typedef struct SourceResourceIndex {uintptr_t* keys;size_t count,capacity;} SourceResourceIndex;
int source_resource_index_contains(const SourceResourceIndex*,const void*);
int source_resource_index_insert(SourceResourceIndex*,const void*);
int source_resource_index_remove(SourceResourceIndex*,const void*);
void source_resource_index_close(SourceResourceIndex*);
typedef struct SourceRenderVertex {
    float x,y,z;
    uint8_t r,g,b,a;
    float u,v;
} SourceRenderVertex;
enum { SOURCE_SKIN_BONES = 24 };
typedef struct SourceSkinInfluence {
    uint8_t rows[4]; // Three float-uniform rows per bone. Fourth byte is padding.
    float weights[3];
} SourceSkinInfluence;
// Indices are relative to vertex_base. Validation restricts each range to
// its own vertex_first/vertex_count span even when ranges share a binding.
typedef struct SourceIndexRange {
    unsigned vertex_first,vertex_count,index_first,index_count,vertex_base;
} SourceIndexRange;
size_t source_graphics_stream_bytes(size_t vertices,size_t indices);
int source_graphics_vertices_finite(const SourceRenderVertex*,size_t);
int source_graphics_validate_index_ranges(size_t vertices,const uint16_t*,size_t indices,const SourceIndexRange*,size_t ranges);
typedef enum SourceTextureFormat {
    SOURCE_TEXTURE_RGBA8,
    SOURCE_TEXTURE_RGB565,
    SOURCE_TEXTURE_RGBA5551,
    SOURCE_TEXTURE_RGBA4,
    SOURCE_TEXTURE_LA8,
    SOURCE_TEXTURE_ETC1,
    SOURCE_TEXTURE_ETC1A4
} SourceTextureFormat;
enum SourceBlend { SOURCE_BLEND_OPAQUE, SOURCE_BLEND_ALPHA, SOURCE_BLEND_ADD };
typedef struct SourceDrawPacket {
    SourceGpuMesh* mesh;
    SourceGpuTexture* texture; // NULL is the real resident white texture
    unsigned first,count;
    unsigned index_range; // zero: DrawArrays; otherwise 1-based validated range
    float model_view[16]; // conventional row-major, column-vector transform
    float hfov,near_clip,far_clip,focal_distance;
    unsigned x,y,width,height; // unrotated top-screen pixels
    unsigned blend,depth_write,cull,alpha_cutoff;
    unsigned orthographic,ui_depth,clip_enabled;
    unsigned clip_x,clip_y,clip_width,clip_height; // unrotated 400x240 pixels
    unsigned modulate_2x; // original UI: clamp(texture * color * 2), after multiply
    unsigned alpha_2x;
    unsigned original_blend,blend_mode,fixed_alpha; // original Nx blend enum, fixed 0..128
    unsigned u_address,v_address,nearest;
    float z_bias;
    float aspect; // original viewport projection; zero selects physical pixels
    unsigned fog_enabled,fog_alpha;uint32_t fog_color;
    float fog_start;
    unsigned early_opaque;
    float skin_u_scale,skin_v_scale,skin_v_offset;
} SourceDrawPacket;
int source_graphics_same_projection(const SourceDrawPacket*,const SourceDrawPacket*);
int source_graphics_same_fog_lut(const SourceDrawPacket*,const SourceDrawPacket*);
// Native triangle-list submission limits. Count elements (including reused
// indices) and keep each indexed slice's start on a 16-byte boundary.
enum { SOURCE_DRAW_SLICE_ELEMENTS=6144 };
unsigned source_graphics_draw_slice(unsigned remaining);
int source_graphics_segment_needs_fence(unsigned words,unsigned free_words,unsigned draws,unsigned elements,unsigned next_elements,int boundary);
enum { SOURCE_ARRAY_INDEX_VARIANTS=4, SOURCE_ARRAY_INDEX_WORDS=SOURCE_ARRAY_INDEX_VARIANTS*SOURCE_DRAW_SLICE_ELEMENTS };
typedef struct SourceArrayIndexDraw {unsigned vertex_base,index_variant;} SourceArrayIndexDraw;
int source_graphics_array_index_plan(unsigned first,unsigned count,size_t vertices,SourceArrayIndexDraw*);
int source_graphics_fill_array_indices(uint16_t*,size_t words);
enum {
    SOURCE_GFX_BOUNDED_WORDS=8*1024,
    SOURCE_GFX_BOUNDED_DRAW_RESERVE=2*1024,
    SOURCE_GFX_BOUNDED_ELEMENTS=8*1024,
    SOURCE_GFX_BOUNDED_DRAWS=72,
};
int source_graphics_bounded_needs_fence(unsigned words,unsigned draws,unsigned elements,unsigned next_elements,int boundary);
typedef struct SourceGraphicsLists {
    unsigned count, peak_words, peak_draws, peak_elements;
} SourceGraphicsLists;
SourceGraphicsLists source_graphics_frame_lists(void);
enum SourcePicaBarrier {
    SOURCE_PICA_FLUSH_FRAMEBUFFER = 1,
    SOURCE_PICA_SYNC_TEXTURE = 2,
};
// Track the PICA registers written by Citro3D, including deferred state.
unsigned source_graphics_pica_barriers(unsigned* pending, uint32_t header, uint32_t value, unsigned count);
int source_graphics_command_list_valid(const uint32_t*,size_t bytes);
enum SourceGraphicsChange {SOURCE_GFX_VIEWPORT=1,SOURCE_GFX_DEPTH=2,SOURCE_GFX_CULL=4,SOURCE_GFX_ALPHA=8,
    SOURCE_GFX_BLEND=16,SOURCE_GFX_Z=32,SOURCE_GFX_SAMPLER=64,SOURCE_GFX_RGB_SCALE=128,SOURCE_GFX_ALPHA_SCALE=256,
    SOURCE_GFX_FOG_COLOR=512,SOURCE_GFX_ALL=1023};
unsigned source_graphics_packet_changes(const SourceDrawPacket*,const SourceDrawPacket*);
enum SourceGraphicsTransition {SOURCE_GFX_PROJECTION=1024,SOURCE_GFX_MODEL_VIEW=2048,
    SOURCE_GFX_FULL_TRANSITION=SOURCE_GFX_ALL|SOURCE_GFX_PROJECTION|SOURCE_GFX_MODEL_VIEW};
unsigned source_graphics_draw_transition(const SourceDrawPacket*,const SourceDrawPacket*);
int source_graphics_open(void);
int source_graphics_close(void); // EBUSY if resources still have owners
int source_graphics_wait(void);
int source_graphics_fog_lut(float near_clip,float far_clip,float start,unsigned alpha,float out[256]);
typedef struct SourceGraphicsStats {unsigned meshes,textures,vertex_bytes,texture_bytes,rgba8_texture_bytes,vram_free,linear_free,command_recycles,command_peak_words,eyes,gpu_previous_draw_us,gpu_complete_frame_us,vertex_bindings,local_vertex_bindings,offset_bindings,mip_textures,mip_bytes,etc1_textures,etc1_bytes,etc1a4_textures,etc1a4_bytes;uint64_t wait_ticks;unsigned draw_calls,planned_packets,clip_updates,framebuffer_barriers,texture_barriers;} SourceGraphicsStats;
size_t source_graphics_linear_free(void);
int source_graphics_stats(SourceGraphicsStats*);
void source_graphics_reset_frame_timing(void);
typedef struct SourceGraphicsWait {uint64_t fill,draw,transfer,swap;} SourceGraphicsWait;
SourceGraphicsWait source_graphics_frame_wait(void);
unsigned source_graphics_frame_draw_calls(void);
// Native journal report. It never attempts to use the stalled GPU.
typedef struct SourceGpuFenceReport {
    unsigned serial, packets, eyes, indexed, words;
    unsigned fill, draw, transfer;
    unsigned fill_irq, draw_irq, transfer_irq, vblank_irq;
    int32_t queue_error;
    unsigned command;
    unsigned list_bytes, first_packet, last_packet, eye_mask;
    unsigned elements, largest_draw, largest_packet, last_element, overlay_packets;
} SourceGpuFenceReport;
void source_graphics_report_fence_failure(const char* waiter,const SourceGpuFenceReport* report);
void source_diagnostics_gpu_command(unsigned command,unsigned serial);
void source_graphics_report_gpu_status(uint32_t busy,int32_t read_result);
void source_graphics_report_command_engine(uint32_t expected_address,unsigned expected_bytes,
    uint32_t address_units,uint32_t size_units,uint32_t trigger,int32_t read_result);
void source_graphics_report_present_timeout(unsigned vblanks);
void source_graphics_record_failed_commands(unsigned serial,const uint32_t*,size_t bytes);
// Called only after a failed fence, while the submitted resources still live.
// The producer streams existing data without allocating or querying GSP.
void source_graphics_record_failed_inputs(unsigned serial,int (*produce)(FILE*,void*),void*);
void source_graphics_probe_inputs(int enabled);
void source_graphics_single_draw(int enabled);
int source_graphics_capture_rgb(uint8_t*,size_t);
// Linear 240x400 ABGR display-transfer buffer -> top-first 400x240 RGB.
int source_graphics_unpack_capture(const uint8_t*,size_t,uint8_t*,size_t);
void source_graphics_debug_text(const char*);
int source_graphics_set_blur(unsigned amount);
int source_graphics_blend_rgb(uint8_t* current,const uint8_t* previous,size_t bytes,unsigned amount);
SourceGpuMesh* source_graphics_mesh_create(const SourceRenderVertex*,size_t);
SourceGpuMesh* source_graphics_skin_mesh_create(const SourceRenderVertex*,const SourceSkinInfluence*,size_t,const uint16_t*,size_t);
int source_graphics_skin_pose(SourceGpuMesh*,const float*,unsigned bones);
int source_graphics_mesh_update(SourceGpuMesh*,const SourceRenderVertex*,size_t);
// The writer fills all requested elements in fenced, GPU-visible storage.
typedef int (*SourceStreamWriter)(void*,SourceRenderVertex*,uint16_t*,SourceIndexRange*);
typedef struct SourceStreamCapacity {size_t vertices,indices;} SourceStreamCapacity;
int source_graphics_stream_write_reserved(SourceGpuMesh**,size_t vertices,size_t indices,size_t ranges,
    SourceStreamWriter,void*,SourceStreamCapacity*);
int source_graphics_stream_write(SourceGpuMesh**,size_t vertices,size_t indices,size_t ranges,SourceStreamWriter,void*);
// Compare the existing CPU-visible linear vertex storage, without a second
// CPU cache. Reads do not mutate a buffer being replayed by the two eyes.
int source_graphics_mesh_matches(const SourceGpuMesh*,const SourceRenderVertex*,size_t);
int source_graphics_mesh_delete(SourceGpuMesh*);
SourceGpuTexture* source_graphics_texture_create(unsigned,unsigned,const uint8_t*,size_t);
SourceGpuTexture* source_graphics_texture_create_format(unsigned,unsigned,SourceTextureFormat,const uint8_t*,size_t);
SourceGpuTexture* source_graphics_texture_create_mips(unsigned,unsigned,SourceTextureFormat,unsigned,const uint8_t*,size_t);
int source_graphics_texture_delete(SourceGpuTexture*);
int source_graphics_texture_update(SourceGpuTexture*,const uint8_t*,size_t);
// Resource mutation acquires a completed writable frame. Submission reuses
// that frame, then fences the next mutation; UI never rewrites live buffers.
int source_graphics_submit(const SourceDrawPacket*,size_t,uint32_t,float);
int source_graphics_submit_bounded(const SourceDrawPacket*,size_t,uint32_t,float);
void source_graphics_fast_index_commands(int enabled);
int source_graphics_submit_frame(const SourceDrawPacket*,size_t,uint32_t,float,int clear_color);
size_t source_graphics_early_prefix(const SourceDrawPacket*,size_t,unsigned eyes);
int source_graphics_submit_early(const SourceDrawPacket*,size_t,uint32_t,float,int bounded);
int source_graphics_wait_early(void);
int source_graphics_submit_remaining(const SourceDrawPacket*,size_t,float);
float source_graphics_slider(void);
void source_graphics_fatal(const char*);
#ifdef __cplusplus
}
static_assert(sizeof(SourceRenderVertex)==24,"PICA vertex stride must be 24 bytes");
#else
_Static_assert(sizeof(SourceRenderVertex)==24,"PICA vertex stride must be 24 bytes");
#endif
