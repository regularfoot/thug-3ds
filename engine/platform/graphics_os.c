// Actual GSP/Citro3D device. This does not replace Nx scenes or game objects.
#include <source_port/stereo.h>
#include <source_port/graphics_os.h>
#include <source_port/pica_commands.h>
#include <source_port/clock_os.h>
#include <source_port/thread_os.h>
#include <source_port/render_options.h>
#include <source_port/application_os.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
uint64_t source_graphics_overlap_ticks(uint64_t phase,uint64_t join,uint64_t irq){
    const uint64_t end=join<irq?join:irq;
    return phase && end>phase?end-phase:0;
}
static int input_capture_probe,single_draw_requested;
void source_graphics_probe_inputs(int enabled){input_capture_probe=enabled!=0;}
void source_graphics_single_draw(int enabled){single_draw_requested=enabled!=0;}
static size_t resource_lower_bound(const SourceResourceIndex* index,uintptr_t key){
    size_t lo=0,hi=index->count;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(index->keys[mid]<key)lo=mid+1;else hi=mid;}return lo;
}
int source_resource_index_contains(const SourceResourceIndex* index,const void* value){
    if(!index||!value)return 0;size_t at=resource_lower_bound(index,(uintptr_t)value);return at<index->count&&index->keys[at]==(uintptr_t)value;
}
static unsigned handle_slot(uintptr_t key){return ((key>>4)^(key>>13))&255u;}
int source_resource_index_cached(SourceResourceIndex* index,const void* value){
    if(!index||!value)return 0;
    const uintptr_t key=(uintptr_t)value;
    uintptr_t* slot=&index->recent[handle_slot(key)];
    if(*slot==key)return 1;
    if(!source_resource_index_contains(index,value))return 0;
    *slot=key;
    return 1;
}
int source_resource_index_insert(SourceResourceIndex* index,const void* value){
    if(!index||!value){errno=EINVAL;return -1;}size_t at=resource_lower_bound(index,(uintptr_t)value);
    if(at<index->count&&index->keys[at]==(uintptr_t)value){errno=EEXIST;return -1;}
    if(index->count==index->capacity){size_t cap=index->capacity?index->capacity*2:16;
        if(cap<index->capacity||cap>SIZE_MAX/sizeof(uintptr_t)){errno=EOVERFLOW;return -1;}
        uintptr_t* keys=realloc(index->keys,cap*sizeof(uintptr_t));if(!keys)return -1;index->keys=keys;index->capacity=cap;}
    memmove(index->keys+at+1,index->keys+at,(index->count-at)*sizeof(uintptr_t));index->keys[at]=(uintptr_t)value;++index->count;return 0;
}
int source_resource_index_remove(SourceResourceIndex* index,const void* value){
    if(!index||!value){errno=EINVAL;return -1;}size_t at=resource_lower_bound(index,(uintptr_t)value);
    if(at==index->count||index->keys[at]!=(uintptr_t)value){errno=ENOENT;return -1;}
    index->recent[handle_slot((uintptr_t)value)]=0;
    memmove(index->keys+at,index->keys+at+1,(index->count-at-1)*sizeof(uintptr_t));--index->count;return 0;
}
void source_resource_index_close(SourceResourceIndex* index){if(index){free(index->keys);memset(index,0,sizeof(*index));}}
// On ARM11 this checks positions and UVs using integer registers. Sign and
// mantissa do not affect whether an IEEE single has an all-ones exponent.
static uint32_t nonfinite_vertex_component(const float* value){
    uint32_t bits;
    memcpy(&bits,value,sizeof bits);
    return ((bits&0x7f800000u)+0x00800000u)&0x80000000u;
}
int source_graphics_vertices_finite(const SourceRenderVertex* data,size_t count){
    if(!data||!count)return 0;
    for(size_t i=0;i<count;++i){
        const SourceRenderVertex* v=data+i;
        if(nonfinite_vertex_component(&v->x)|nonfinite_vertex_component(&v->y)|
           nonfinite_vertex_component(&v->z)|nonfinite_vertex_component(&v->u)|nonfinite_vertex_component(&v->v))return 0;
    }
    return 1;
}
static size_t index_buffer_offset(size_t count){
    return (count*sizeof(SourceRenderVertex)+127)&~(size_t)127;
}
size_t source_graphics_stream_bytes(size_t vertices,size_t indices){
    return indices?index_buffer_offset(vertices)+indices*sizeof(uint16_t):vertices*sizeof(SourceRenderVertex);
}
int source_graphics_validate_index_ranges(size_t vertices,const uint16_t* indices,size_t count,const SourceIndexRange* ranges,size_t range_count){
    if(!vertices||vertices>32u*1024u*1024u/sizeof(SourceRenderVertex)||count>8u*1024u*1024u/sizeof(uint16_t)||range_count>65536
       ||(count&&!indices)||(range_count&&!ranges)||(!range_count&&count)){errno=EINVAL;return -1;}
    size_t end=0;
    for(size_t i=0;i<range_count;++i){const SourceIndexRange* r=&ranges[i];
        if(!r->vertex_count||r->vertex_count>65536||r->vertex_first%4||r->vertex_first>vertices||r->vertex_count>vertices-r->vertex_first
           ||r->vertex_base%4||r->vertex_base>r->vertex_first
           ||r->triangle_strip>1||r->index_count<3||(r->triangle_strip ? r->index_count>SOURCE_DRAW_SLICE_ELEMENTS : r->index_count%3)||r->index_first%8||r->index_first<end||r->index_first>count||r->index_count>count-r->index_first){errno=EINVAL;return -1;}
        const size_t first=r->vertex_first-r->vertex_base;
        if(first>65536-r->vertex_count){errno=EINVAL;return -1;}
        end=(size_t)r->index_first+r->index_count;
        for(size_t j=r->index_first;j<end;++j)if((size_t)indices[j]-first>=r->vertex_count){errno=EINVAL;return -1;}
    }return 0;
}
static unsigned effective_blend(const SourceDrawPacket* p){
    if(!p->original_blend)
        return p->blend==SOURCE_BLEND_ALPHA?5:p->blend==SOURCE_BLEND_ADD?1:0;
    return p->blend_mode==11?0:p->blend_mode==12?5:p->blend_mode;
}
static int uses_constant_alpha(unsigned mode){
    return mode>=2 && mode<=10 && !(mode&1);
}
static unsigned clamped_alpha(unsigned alpha){return alpha<128?alpha:128;}
unsigned source_graphics_packet_changes(const SourceDrawPacket* a,const SourceDrawPacket* b){
    if(!a||!b)return SOURCE_GFX_ALL;unsigned bits=0;
    if(a->x!=b->x||a->y!=b->y||a->width!=b->width||a->height!=b->height||a->clip_enabled!=b->clip_enabled||a->clip_x!=b->clip_x||a->clip_y!=b->clip_y||a->clip_width!=b->clip_width||a->clip_height!=b->clip_height)bits|=SOURCE_GFX_VIEWPORT;
    if(a->orthographic!=b->orthographic||a->ui_depth!=b->ui_depth||a->depth_write!=b->depth_write)bits|=SOURCE_GFX_DEPTH;
    if(a->cull!=b->cull)bits|=SOURCE_GFX_CULL;if(a->alpha_cutoff!=b->alpha_cutoff)bits|=SOURCE_GFX_ALPHA;
    if(a->blend!=b->blend||a->original_blend!=b->original_blend||a->blend_mode!=b->blend_mode||a->fixed_alpha!=b->fixed_alpha){
        const unsigned mode=effective_blend(b);
        if(effective_blend(a)!=mode || (uses_constant_alpha(mode)&&clamped_alpha(a->fixed_alpha)!=clamped_alpha(b->fixed_alpha)))
            bits|=SOURCE_GFX_BLEND;
    }
    if(a->z_bias!=b->z_bias)bits|=SOURCE_GFX_Z;
    if(a->texture!=b->texture||a->u_address!=b->u_address||a->v_address!=b->v_address||a->nearest!=b->nearest)bits|=SOURCE_GFX_SAMPLER;
    if(a->modulate_2x!=b->modulate_2x)bits|=SOURCE_GFX_RGB_SCALE;if(a->alpha_2x!=b->alpha_2x)bits|=SOURCE_GFX_ALPHA_SCALE;
    if(a->fog_color!=b->fog_color)bits|=SOURCE_GFX_FOG_COLOR;return bits;
}
int source_graphics_same_projection(const SourceDrawPacket* a,const SourceDrawPacket* b){
    return a&&b&&a->orthographic==b->orthographic&&(a->orthographic||
        (a->hfov==b->hfov&&a->aspect==b->aspect&&a->width==b->width&&a->height==b->height
         &&a->near_clip==b->near_clip&&a->far_clip==b->far_clip&&a->focal_distance==b->focal_distance));
}
int source_graphics_same_fog_lut(const SourceDrawPacket* a,const SourceDrawPacket* b){
    return a&&b&&a->near_clip==b->near_clip&&a->far_clip==b->far_clip&&a->fog_start==b->fog_start&&a->fog_alpha==b->fog_alpha;
}
unsigned source_graphics_draw_transition(const SourceDrawPacket* previous,const SourceDrawPacket* packet){
    if(!previous)return SOURCE_GFX_FULL_TRANSITION;
    if(previous==packet)return 0;
    unsigned changes=source_graphics_packet_changes(previous,packet);
    if(!source_graphics_same_projection(previous,packet))changes|=SOURCE_GFX_PROJECTION;
    if(memcmp(previous->model_view,packet->model_view,sizeof(packet->model_view)))changes|=SOURCE_GFX_MODEL_VIEW;
    return changes;
}
enum {
    COMMAND_PACKET_RESERVE_WORDS=8192,
    COMMAND_SEGMENT_WORDS=16*1024,
    COMMAND_SEGMENT_ELEMENTS=16*1024,
    COMMAND_SEGMENT_DRAWS=160,
};
_Static_assert(SOURCE_DRAW_SLICE_ELEMENTS%24==0,"A draw slice preserves triangles and aligned index starts");
_Static_assert(SOURCE_DRAW_SLICE_ELEMENTS*sizeof(uint16_t)%128==0,"Every shared index variant remains aligned");
unsigned source_graphics_draw_slice(unsigned remaining){
    return remaining<SOURCE_DRAW_SLICE_ELEMENTS?remaining:SOURCE_DRAW_SLICE_ELEMENTS;
}
int source_graphics_array_index_plan(unsigned first,unsigned count,size_t vertices,SourceArrayIndexDraw* out){
    if(!out || !count || count%3 || count>SOURCE_DRAW_SLICE_ELEMENTS || first>vertices || count>vertices-first){
        errno=EINVAL;return -1;
    }
    out->vertex_base=first&~3u;
    out->index_variant=first&3u;
    return 0;
}
int source_graphics_fill_array_indices(uint16_t* out,size_t words){
    if(!out || words!=SOURCE_ARRAY_INDEX_WORDS){errno=EINVAL;return -1;}
    for(unsigned variant=0;variant<SOURCE_ARRAY_INDEX_VARIANTS;++variant)
        for(unsigned i=0;i<SOURCE_DRAW_SLICE_ELEMENTS;++i)
            out[variant*SOURCE_DRAW_SLICE_ELEMENTS+i]=(uint16_t)(variant+i);
    return 0;
}
int source_graphics_segment_needs_fence(unsigned words,unsigned free_words,unsigned draws,unsigned elements,unsigned next_elements,int boundary){
    return (draws && boundary) || free_words<COMMAND_PACKET_RESERVE_WORDS ||
        (elements && (draws>=COMMAND_SEGMENT_DRAWS || words>=COMMAND_SEGMENT_WORDS || elements>COMMAND_SEGMENT_ELEMENTS ||
                     next_elements>COMMAND_SEGMENT_ELEMENTS-elements));
}
int source_graphics_bounded_needs_fence(unsigned words,unsigned draws,unsigned elements,unsigned next_elements,int boundary){
    if(!draws)return 0;
    return boundary || words>=SOURCE_GFX_BOUNDED_WORDS-SOURCE_GFX_BOUNDED_DRAW_RESERVE ||
        draws>=SOURCE_GFX_BOUNDED_DRAWS || elements>SOURCE_GFX_BOUNDED_ELEMENTS ||
        next_elements>SOURCE_GFX_BOUNDED_ELEMENTS-elements;
}
size_t source_graphics_early_prefix(const SourceDrawPacket* packets,size_t count,unsigned eyes){
    if(!packets||(eyes!=1&&eyes!=2))return 0;
    unsigned elements=0;
    size_t prefix=0;
    // Replay a bounded prefix of the existing sequence against each target.
    // Leave headroom for raster-state changes and the second eye's setup.
    const unsigned limit=48/eyes;
    for(;prefix<count&&prefix<limit;++prefix){
        const SourceDrawPacket* packet=&packets[prefix];
        if(!packet->early_opaque||packet->orthographic||!packet->count||
           packet->count>COMMAND_SEGMENT_ELEMENTS/eyes-elements)break;
        elements+=packet->count;
    }
    return prefix;
}
unsigned source_graphics_pica_barriers(unsigned* pending, uint32_t header, uint32_t value, unsigned count) {
    const unsigned byte_enable = (header >> 16) & 15;
    if (!byte_enable || !count) return 0;
    const unsigned first = header & 0x3ff;
    const unsigned last = (header >> 31) ? first + count - 1 : first;
    enum {
        PICA_TEXTURE_FIRST = 0x080,
        PICA_TEXTURE_LAST = 0x0b7,
        PICA_FOG_TEV_BUFFER = 0x0e0,
        PICA_FRAGMENT_FIRST = 0x100,
        PICA_FRAMEBUFFER_FLUSH = 0x111,
        PICA_FRAGMENT_LAST = 0x130,
        PICA_DRAW_ARRAYS = 0x22e,
        PICA_DRAW_ELEMENTS = 0x22f,
    };
    if ((first == PICA_DRAW_ARRAYS || first == PICA_DRAW_ELEMENTS) && value) {
        *pending = SOURCE_PICA_FLUSH_FRAMEBUFFER | SOURCE_PICA_SYNC_TEXTURE;
        return 0;
    }
    if (first == PICA_FRAMEBUFFER_FLUSH && count == 1 && (byte_enable & 1) && (value & 1)) {
        *pending &= ~SOURCE_PICA_FLUSH_FRAMEBUFFER;
        return 0;
    }

    unsigned barriers = 0;
    if (first <= PICA_FRAGMENT_LAST && last >= PICA_FRAGMENT_FIRST)
        barriers |= *pending & SOURCE_PICA_FLUSH_FRAMEBUFFER;
    if (first <= PICA_TEXTURE_LAST && last >= PICA_TEXTURE_FIRST)
        barriers |= *pending & SOURCE_PICA_SYNC_TEXTURE;
    // Fog mode also affects fragments in flight. Keep the existing guard
    // for the low byte of the shared fog and TEV update register.
    if (first == PICA_FOG_TEV_BUFFER && (byte_enable & 1))
        barriers |= SOURCE_PICA_FLUSH_FRAMEBUFFER;

    *pending &= ~barriers;
    return barriers;
}
int source_graphics_command_list_valid(const uint32_t* words,size_t bytes){
    // GPUCMD_Split emits this complete FINALIZE command, repeated if needed
    // to align the list. Never enqueue a truncated or misaligned list.
    if(!words || (uintptr_t)words%16 || bytes<16 || bytes%16)return 0;
    const size_t count=bytes/sizeof(*words);
    return words[count-2]==0x12345678 && words[count-1]==0x000f0010;
}
int source_graphics_unpack_capture(const uint8_t* linear,size_t input_size,uint8_t* rgb,size_t output_size){
    if(!linear||!rgb||input_size!=400*240*4||output_size!=400*240*3){errno=EINVAL;return -1;}
    // Actual libctru display layout: column x, row 239-y. Reversing the
    // columns as well rotates the captured frame by 180 degrees.
    for(unsigned y=0;y<240;++y)for(unsigned x=0;x<400;++x){size_t from=(x*240+239-y)*4,to=(y*400+x)*3;
        rgb[to]=linear[from+3];rgb[to+1]=linear[from+2];rgb[to+2]=linear[from+1];}
    return 0;
}
int source_graphics_blend_rgb(uint8_t* current,const uint8_t* previous,size_t bytes,unsigned amount){
    if(!current||!previous||amount>255){errno=EINVAL;return -1;}for(size_t i=0;i<bytes;++i)current[i]=(current[i]*(255-amount)+previous[i]*amount+127)/255;return 0;
}
int source_graphics_fog_lut(float near_clip,float far_clip,float start,unsigned alpha,float out[256]){
    if(!out||!isfinite(near_clip)||!isfinite(far_clip)||!isfinite(start)||near_clip<=0||far_clip<=near_clip||alpha>255){errno=EINVAL;return -1;}
    float previous=0;
    for(unsigned k=0;k<=128;++k){float z=(double)far_clip*near_clip/((k/128.)*((double)far_clip-near_clip)+near_clip);
        float amount=fminf(1.f,fmaxf(0.f,(z-start)/7200.f));float value=1.f-fminf(1.f,alpha/128.f)*amount;
        if(k<128)out[k]=value;if(k)out[k+127]=value-previous;previous=value;
    }return 0;
}
static int use_index_sequence,fast_city_cpu,measure_command_cpu;
static uint64_t command_ticks;
void source_graphics_city_cpu(int fast,int measure){fast_city_cpu=fast;measure_command_cpu=measure;}
uint64_t source_graphics_command_ticks(void){return command_ticks;}
void source_graphics_fast_index_commands(int enabled){use_index_sequence=enabled;}
#ifdef __3DS__
#include <3ds.h>
#include <source_port/touch_controls.h>
#include <citro3d.h>
#include <world_shbin.h>
#include <skin_shbin.h>
typedef struct SourceMeshSkin {
    SourceSkinInfluence* influences;
    unsigned bones;
    float rows[SOURCE_SKIN_BONES*3][4];
} SourceMeshSkin;
_Static_assert(sizeof(SourceSkinInfluence)==16,"PICA influence stride");
struct SourceGpuMesh {struct SourceGpuMesh *next,**link;SourceRenderVertex* vertices;u32 vertex_paddr,index_paddr;size_t count,used,bytes;
    uint16_t* indices;size_t index_capacity;SourceIndexRange* ranges;size_t range_count,range_capacity;
    SourceMeshSkin* skin;};
struct SourceGpuTexture { struct SourceGpuTexture* next; C3D_Tex texture; size_t bytes; bool vram; };
enum { CAPTURE_DRAWS=256 };
_Static_assert(CAPTURE_DRAWS<=UINT8_MAX+1u,"snapshot owner indices fit in a byte");
typedef struct SourceGpuInputDraw {
    SourceGpuMesh* mesh;
    const uint16_t* indices;
    C3D_Tex texture;
    unsigned packet, first, elements, vertex_first, vertex_count, index_first, vertex_base;
    uint8_t eye, texture_owner, clip_owner, orthographic, depth_write, alpha_cutoff, cull, strip;
    bool sampler_vram;
    C3D_Mtx clip;
} SourceGpuInputDraw;
enum { FILL_IRQ, DRAW_IRQ, TRANSFER_IRQ, VBLANK_IRQ, IRQ_COUNT };
static struct {
    int open,acquired,program_ready,fence_error;
    C3D_RenderTarget* target[2];
    C3D_RenderTarget* low_target;
    SourceGpuTexture low_color;
    SourceGpuMesh* low_quad;
    bool right_eye_alloc_failed, low_attempted;
    DVLB_s* dvlb;
    shaderProgram_s program;
    int clip_matrix;
    DVLB_s* skin_dvlb;
    shaderProgram_s skin_program;
    int skin_clip,skin_rows,skin_uv,skin_enabled;
    bool skin_tried,skin_ready;
    bool early_open,early_joined,bounded_list;
    float early_stereo;
    unsigned next_present_tag, pending_eyes;
    bool defer_next_present;
    SourcePresentSample presenting, presented[4];
    uint64_t returned_tick, next_cpu_tick, next_geometry_tick;
    C3D_Tex white, sampler;
    int white_ready,console_ready;
    C3D_FogLut fog;
    SourceDrawPacket fog_lut_state;
    bool fog_lut_valid;
    unsigned blur,history_valid[2];
    uint8_t* history[2];SourceGpuTexture* history_texture[2];SourceGpuMesh* history_mesh;
    size_t vertex_bytes,texture_bytes,rgba8_texture_bytes;
    u32* texture_staging;
    unsigned command_buffer_bytes;
    SourceGpuMesh* meshes;
    SourceGpuTexture* textures;
    SourceResourceIndex mesh_index,texture_index;
    unsigned command_recycles,command_peak_words,eyes,gpu_previous_draw_us;
    SourceGraphicsLists lists;
    unsigned pending_pica_barriers, framebuffer_barriers, texture_barriers;
    unsigned draw_segment_us,gpu_complete_frame_us;
    SourceGpuFenceReport submitted;
    const u32* submitted_list;
    SourceGpuInputDraw input_draws[CAPTURE_DRAWS];
    unsigned input_count, segment_saved_words, compact_binds;
    bool flat_offset_only;
    bool capture_probe,submitted_isolated;
    uint16_t* array_indices;
    u32 array_index_paddr;
    unsigned segment_packets,segment_eyes,segment_indexed,segment_overlay;
    unsigned segment_first,segment_last;
    unsigned segment_elements,segment_largest_draw,segment_largest_packet,segment_last_element;
    unsigned irq_start[IRQ_COUNT];
    uint64_t submitted_at;
    unsigned vertex_bindings,local_vertex_bindings;
    uint16_t* transitions;
    size_t transition_capacity;
    unsigned draw_calls,planned_packets,clip_updates;
    uint64_t wait_ticks;
    uint64_t fill_ticks,draw_ticks,transfer_ticks,swap_ticks;
} gpu;
enum { VERTEX_BUDGET=32*1024*1024, TEXTURE_BUDGET=24*1024*1024,
    COMMAND_BUFFER_BYTES=1024*1024, OLD_COMMAND_BUFFER_BYTES=256*1024,
    OLD_TEXTURE_STAGING_BYTES=128*1024, OLD_LINEAR_GEOMETRY_RESERVE=768*1024 };
static int controller(void) { if (source_thread_is_controller()) return 1; errno=EPERM; return 0; }
static unsigned irq_count[IRQ_COUNT];
static LightEvent completion[VBLANK_IRQ];
static int32_t rejected_result;
static unsigned rejected_command;
enum { GX_PICA_LIST=1, GX_FILL=2, GX_TRANSFER=3, GX_TEXTURE_COPY=4, GX_PICA_CACHE_FLUSH=0x101, GX_PICA_INVALID_LIST=0x201 };
static unsigned draw_finished_tick;

static void gpu_irq(void* counter) {
    unsigned* count=counter;
    __atomic_fetch_add(count,1,__ATOMIC_RELEASE);
    if(count!=&irq_count[VBLANK_IRQ])LightEvent_Signal(&completion[count-irq_count]);
}
static void draw_finished(void* counter) {
    // Only the elapsed interval is used. The 32-bit counter wraps well
    // outside the two-second watchdog, so it can be published atomically.
    __atomic_store_n(&draw_finished_tick,(unsigned)source_clock_now(),__ATOMIC_RELAXED);
    gpu_irq(counter);
}
static void vblank(void* unused) {
    (void)unused;
    gpu_irq(&irq_count[VBLANK_IRQ]);
    source_clock_video_notify();
}
static unsigned irq_snapshot(unsigned irq) {
    return __atomic_load_n(&irq_count[irq],__ATOMIC_ACQUIRE);
}

Result __real_gspSubmitGxCommand(const u32 command[8]);
static void report_gx_rejection(unsigned command,Result result) {
    __atomic_store_n(&rejected_command,command,__ATOMIC_RELAXED);
    __atomic_store_n(&rejected_result,result,__ATOMIC_RELEASE);
}
Result __wrap_gspSubmitGxCommand(const u32 command[8]) {
    Result result=__real_gspSubmitGxCommand(command);
    // libctru returns -2 before enqueue when GSP's 15-entry ring is full.
    // The SDK queue otherwise advances past this command and awaits its IRQ.
    for(unsigned attempt=0;result==-2 && attempt<4;++attempt){
        svcSleepThread(100000);
        result=__real_gspSubmitGxCommand(command);
    }
    // Other errors can occur AFTER enqueue. Retrying could double-submit.
    if(R_FAILED(result))report_gx_rejection(command[0]&255,result);
    return result;
}

Result __real_GX_ProcessCommandList(u32* buffer,u32 bytes,u8 flags);
void __real_GPUCMD_Add(u32 header,const u32* parameters,u32 count);
static inline void emit_register(u32 header,u32 value) {
    if(!source_pica_append_register(gpuCmdBuf,gpuCmdBufSize,&gpuCmdBufOffset,header,value))
        __real_GPUCMD_Add(header,&value,1);
}
void __wrap_GPUCMD_Add(u32 header,const u32* parameters,u32 count) {
    // Citro3D defers texture and fragment state until the next draw. Guard
    // the first actual register write in each domain after the previous draw.
    // An existing framebuffer flush at the end of a list already counts.
    // Citro3D sends all 12 vertex streams (36 registers) even for a change
    // to the first address only. Scope this to a flat rebind in encode_packets.
    // This keeps the SDK's shadow state and the first full write of each list.
    if(gpu.flat_offset_only && header==GPUCMD_HEADER(1,15,GPUREG_ATTRIBBUFFER0_OFFSET) && count==36){
        emit_register(header,parameters[0]);
        gpu.segment_saved_words+=36; // 38 aligned words become 2
        ++gpu.compact_binds;
        return;
    }
    const u32 value=parameters && count ? parameters[0] : 0;
    // Single vertex-register writes cannot overlap the fragment or texture
    // domains. Draw triggers still arm the barriers for the next draw.
    const unsigned reg=header&0x3ff;
    if(count==1 && reg>=GPUREG_ATTRIBBUFFERS_LOC &&
       reg!=GPUREG_DRAWARRAYS && reg!=GPUREG_DRAWELEMENTS){
        emit_register(header,value);
        return;
    }
    const unsigned barriers = source_graphics_pica_barriers(
        &gpu.pending_pica_barriers, header, value, count);
    if (barriers & SOURCE_PICA_SYNC_TEXTURE) {
        for (unsigned i = 0; i < 3; ++i)
            emit_register(GPUCMD_HEADER(0, 0, GPUREG_TEXUNIT_CONFIG),0);
        ++gpu.texture_barriers;
    }
    if (barriers & SOURCE_PICA_FLUSH_FRAMEBUFFER) {
        emit_register(GPUCMD_HEADER(0, 15, GPUREG_FRAMEBUFFER_FLUSH),1);
        ++gpu.framebuffer_barriers;
    }
    if(count==1)emit_register(header,value);
    else if(!fast_city_cpu || !source_pica_append_short_run(gpuCmdBuf,gpuCmdBufSize,&gpuCmdBufOffset,header,parameters,count))
        __real_GPUCMD_Add(header,parameters,count);
}
Result __wrap_GX_ProcessCommandList(u32* buffer,u32 bytes,u8 flags) {
    if(!(flags&GX_CMDLIST_FLUSH))return __real_GX_ProcessCommandList(buffer,bytes,flags);
    // Citro3D has now appended the framebuffer flush and end marker. Make
    // that exact range visible before queuing it; a failed flush must not
    // become an unexplained wait for a P3D IRQ.
    gpu.submitted.list_bytes=bytes;
    gpu.submitted_list=buffer;
    if(bytes>gpu.command_buffer_bytes ||
       (gpu.bounded_list && bytes>SOURCE_GFX_BOUNDED_WORDS*sizeof(u32)) ||
       !source_graphics_command_list_valid(buffer,bytes)){
        report_gx_rejection(GX_PICA_INVALID_LIST,-1);
        return -1;
    }
    ++gpu.lists.count;
    const unsigned words=bytes/sizeof(u32);
    if(words>gpu.lists.peak_words)gpu.lists.peak_words=words;
    if(gpu.segment_packets>gpu.lists.peak_draws)gpu.lists.peak_draws=gpu.segment_packets;
    if(gpu.segment_elements>gpu.lists.peak_elements)gpu.lists.peak_elements=gpu.segment_elements;
    const Result flushed=GSPGPU_FlushDataCache(buffer,bytes);
    if(R_FAILED(flushed)){
        report_gx_rejection(GX_PICA_CACHE_FLUSH,flushed);
        return flushed;
    }
    // Keep the SDK's flush flag as well. GSP uses it at dispatch, after
    // dequeuing and just before the GPU fetches this range.
    const Result sent=__real_GX_ProcessCommandList(buffer,bytes,flags);
    if(R_FAILED(sent))report_gx_rejection(GX_PICA_LIST,sent);
    return sent;
}

static int capture_bytes(FILE* file,const void* data,size_t bytes) {
    return !bytes || fwrite(data,1,bytes,file)==bytes ? 0 : -1;
}
static int capture_inputs(FILE* file,void* unused) {
    (void)unused;
    // All words are little-endian u32, followed by 16 clip-matrix floats.
    unsigned version=1;
    for(unsigned i=0;i<gpu.input_count;++i)if(gpu.input_draws[i].strip)version=2;
    if(gpu.fence_error && source_application_os_is_old_3ds())version=3;
    const uint32_t header[4]={0x50474854,version,gpu.submitted.serial,gpu.input_count};
    if(capture_bytes(file,header,sizeof(header)))return -1;
    for(unsigned i=0;i<gpu.input_count;++i){
        const SourceGpuInputDraw* draw=&gpu.input_draws[i];
        const SourceGpuMesh* mesh=draw->mesh;
        const C3D_Tex* texture=&gpu.input_draws[draw->texture_owner].texture;
        const C3D_Mtx* clip=&gpu.input_draws[draw->clip_owner].clip;
        // Keep THGP v1 byte-compatible. Address conversion and sizing are
        // needed only for an explicit capture or a failed fence.
        unsigned m[24]={
            draw->packet,draw->eye,draw->strip?2u:(draw->indices!=NULL),draw->first,draw->elements,
            draw->vertex_first,draw->vertex_count,draw->index_first,draw->indices?draw->elements:0,
            osConvertVirtToPhys(mesh->vertices+draw->vertex_base),
            draw->indices?osConvertVirtToPhys(draw->indices):0,osConvertVirtToPhys(texture->data),
            texture->width,texture->height,texture->fmt,texture->maxLevel,
            C3D_TexCalcTotalSize(texture->size,texture->maxLevel),mesh->skin?mesh->skin->bones:0,
            texture->param,texture->lodParam,draw->orthographic,draw->depth_write,draw->alpha_cutoff,draw->cull
        };
        for(unsigned previous=0;previous<i;++previous){
            unsigned owner=gpu.input_draws[previous].texture_owner;
            if(gpu.input_draws[owner].texture.data==texture->data){m[16]=0;break;}
        }
        // A failed PICA fence leaves VRAM unavailable for a reliable FS write.
        const bool omit_vram=version==3 && gpu.input_draws[draw->texture_owner].sampler_vram;
        if(omit_vram)m[16]=UINT32_MAX;
        if(capture_bytes(file,m,sizeof(m)) ||
           capture_bytes(file,clip,sizeof(*clip)) ||
           capture_bytes(file,mesh->vertices+m[5],m[6]*sizeof(SourceRenderVertex)) ||
           capture_bytes(file,draw->indices,m[8]*sizeof(uint16_t)))return -1;
        // A repeated texture carries its descriptor again but no duplicate payload.
        if(!omit_vram && capture_bytes(file,texture->data,m[16]))return -1;
        if(mesh->skin &&
           (capture_bytes(file,mesh->skin->influences+m[5],m[6]*sizeof(SourceSkinInfluence)) ||
            capture_bytes(file,mesh->skin->rows,m[17]*3*4*sizeof(float))))return -1;
    }
    return 0;
}
static int fence_failure(const char* waiter,int32_t error) {
    SourceGpuFenceReport report=gpu.submitted;
    report.fill_irq=irq_snapshot(FILL_IRQ)-gpu.irq_start[FILL_IRQ];
    report.draw_irq=irq_snapshot(DRAW_IRQ)-gpu.irq_start[DRAW_IRQ];
    report.transfer_irq=irq_snapshot(TRANSFER_IRQ)-gpu.irq_start[TRANSFER_IRQ];
    report.vblank_irq=irq_snapshot(VBLANK_IRQ)-gpu.irq_start[VBLANK_IRQ];
    report.queue_error=error;
    if(error)report.command=__atomic_load_n(&rejected_command,__ATOMIC_RELAXED);
    gpu.fence_error=error?EIO:ETIMEDOUT;
    // On this path FrameBegin has not reset the command arena. Save the
    // finalized stream and avoid GSP register IPC entirely: on hardware a
    // wedged GSP prevented both command capture and terminal error handling.
    source_graphics_report_fence_failure(waiter,&report);
    if(gpu.submitted_list && report.list_bytes && report.list_bytes<=gpu.command_buffer_bytes){
        source_graphics_record_failed_commands(report.serial,gpu.submitted_list,report.list_bytes);
        source_graphics_record_failed_inputs(report.serial,capture_inputs,NULL);
    }
    errno=gpu.fence_error;
    return -1;
}

static void begin_submission(SourceGpuFenceReport report) {
    report.serial=gpu.submitted.serial+1;
    gpu.submitted=report;
    gpu.submitted_list=NULL;
    gpu.submitted_at=source_clock_now();
    source_diagnostics_gpu_command(report.command,report.serial);
    for(unsigned irq=0;irq<IRQ_COUNT;++irq)gpu.irq_start[irq]=irq_snapshot(irq);
}

static int wait_for_irq(const char* waiter,unsigned irq) {
    const uint64_t started=source_clock_now();
    const uint64_t frequency=source_clock_frequency();
    const uint64_t deadline=gpu.submitted_at+2*frequency;
    for(;;){
        const int32_t rejected=__atomic_load_n(&rejected_result,__ATOMIC_ACQUIRE);
        if(rejected)return fence_failure(waiter,rejected);
        if(irq_snapshot(irq)!=gpu.irq_start[irq])break;
        const uint64_t now=source_clock_now();
        if(now>=deadline)return fence_failure(waiter,0);
        const uint64_t remaining_ns=(deadline-now)*1000000000ull/frequency;
        LightEvent_WaitTimeout(&completion[irq],remaining_ns);
    }
    const uint64_t elapsed=source_clock_now()-started;
    gpu.wait_ticks+=elapsed;
    if(irq==FILL_IRQ)gpu.fill_ticks+=elapsed;
    else if(irq==DRAW_IRQ)gpu.draw_ticks+=elapsed;
    else if(irq==TRANSFER_IRQ)gpu.transfer_ticks+=elapsed;
    source_diagnostics_gpu_command(0,gpu.submitted.serial);
    return 0;
}

static int wait_for_draw(const char* waiter){
    if(!gpu.submitted.draw)return 0;
    if(wait_for_irq(waiter,DRAW_IRQ))return -1;
    if(gpu.capture_probe && gpu.submitted_isolated && gpu.input_count){
        gpu.capture_probe=false;
        source_graphics_record_failed_commands(gpu.submitted.serial,gpu.submitted_list,gpu.submitted.list_bytes);
        source_graphics_record_failed_inputs(gpu.submitted.serial,capture_inputs,NULL);
    }
    const unsigned ticks=__atomic_load_n(&draw_finished_tick,__ATOMIC_RELAXED)-(unsigned)gpu.submitted_at;
    gpu.gpu_previous_draw_us=(unsigned)((uint64_t)ticks*1000000/source_clock_frequency());
    return 0;
}

static int acquire(const char* waiter) {
    if (!controller() || !gpu.open) { errno=ENODEV; return -1; }
    if(gpu.fence_error){errno=gpu.fence_error;return -1;}
    if (!gpu.acquired) {
        if(gpu.pending_eyes && source_graphics_drain_present())return -1;
        if(wait_for_draw(waiter))return -1;
        if (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)){errno=EIO;return -1;}
        gpu.input_count=gpu.segment_saved_words=0;
        gpu.segment_packets=gpu.segment_eyes=gpu.segment_indexed=gpu.segment_overlay=0;
        gpu.segment_first=gpu.segment_last=0;
        gpu.segment_elements=gpu.segment_largest_draw=gpu.segment_largest_packet=gpu.segment_last_element=0;
        gpu.acquired=1;
    }
    return 0;
}

static void finish(void) {
    if (!gpu.acquired) return;
    u32 size,offset;
    GPUCMD_GetBuffer(NULL,&size,&offset);
    const unsigned words=gpu.command_buffer_bytes/4-size+offset;
    if(words>gpu.command_peak_words)gpu.command_peak_words=words;
    if(offset){
        gpu.submitted_isolated=gpu.bounded_list;
        begin_submission((SourceGpuFenceReport){
            .packets=gpu.segment_packets,.eyes=(gpu.segment_eyes&1)+(gpu.segment_eyes>>1),
            .indexed=gpu.segment_indexed,.words=words,.draw=1,.command=GX_PICA_LIST,
            .first_packet=gpu.segment_first,.last_packet=gpu.segment_last,.eye_mask=gpu.segment_eyes,
            .elements=gpu.segment_elements,.largest_draw=gpu.segment_largest_draw,
            .largest_packet=gpu.segment_largest_packet,.last_element=gpu.segment_last_element,
            .overlay_packets=gpu.segment_overlay,
        });
    }
    // Mesh and texture writes flush their own ranges. Flush the finalized
    // command bytes in the GX wrapper. Render targets are never auto-presented.
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    gpu.acquired=0;
}

static int transfer(const char* waiter,C3D_RenderTarget* target,u32* output,u32 flags){
    if(!output){errno=ENODEV;return -1;}
    begin_submission((SourceGpuFenceReport){.eyes=gpu.eyes,.transfer=1,.command=GX_TRANSFER});
    const u32 dimensions=GX_BUFFER_DIM(target->frameBuf.width,target->frameBuf.height);
    const Result result=GX_DisplayTransfer(target->frameBuf.colorBuf,dimensions,output,dimensions,flags);
    if(R_FAILED(result)){
        report_gx_rejection(GX_TRANSFER,result);
        return fence_failure(waiter,result);
    }
    return wait_for_irq(waiter,TRANSFER_IRQ);
}

static int complete_present(unsigned eyes){
    const uint64_t join=source_clock_now();
    if(wait_for_draw(__func__))return -1;
    SourcePresentSample* sample=&gpu.presenting;
    if(sample->tag && gpu.submitted.draw){
        const unsigned delta=__atomic_load_n(&draw_finished_tick,__ATOMIC_RELAXED)-(unsigned)gpu.submitted_at;
        const uint64_t irq=gpu.submitted_at+delta;
        sample->cpu_overlap=source_graphics_overlap_ticks(gpu.next_cpu_tick,join,irq);
        sample->geometry_overlap=source_graphics_overlap_ticks(gpu.next_geometry_tick,join,irq);
        sample->draw_tail=source_graphics_overlap_ticks(gpu.returned_tick,irq,irq);
        sample->hold_after_draw=sample->deferred&&join>irq?join-irq:0;
        sample->next_cpu_started=gpu.next_cpu_tick!=0;
        sample->next_geometry_started=gpu.next_geometry_tick!=0;
    }
    gpu.gpu_complete_frame_us=gpu.draw_segment_us+gpu.gpu_previous_draw_us;
    // gfxGetFramebuffer follows the CPU-side swap index immediately. GSP
    // applies that swap at VBlank; until then the next destination may
    // still be on scanout. Wait only while a swap really is outstanding.
    if(gspIsPresentPending(GFX_TOP)){
        const uint64_t started=source_clock_now();
        const uint64_t deadline=started+source_clock_frequency()/4;
        while(gspIsPresentPending(GFX_TOP)){
            if(source_clock_now()>=deadline){
                gpu.fence_error=ETIMEDOUT;
                source_graphics_report_present_timeout(irq_snapshot(VBLANK_IRQ)-gpu.irq_start[VBLANK_IRQ]);
                errno=ETIMEDOUT;return -1;
            }
            svcSleepThread(500000);
        }
        const uint64_t elapsed=source_clock_now()-started;
        gpu.wait_ticks+=elapsed;
        gpu.swap_ticks+=elapsed;
    }
    for(unsigned eye=0;eye<eyes;++eye){
        const unsigned flags=GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8);
        u32* output=(u32*)gfxGetFramebuffer(GFX_TOP,eye?GFX_RIGHT:GFX_LEFT,NULL,NULL);
        if(transfer(__func__,gpu.target[eye],output,flags))return -1;
    }
    gfxScreenSwapBuffers(GFX_TOP,eyes==2);
    // The preceding mono draw and display transfer have both finished. A
    // mono load screen with the slider still up must retain the stereo target.
    if (eyes == 1 && gpu.target[1] && source_application_os_is_old_3ds() && osGet3DSliderState() <= 0.f) {
        C3D_RenderTargetDelete(gpu.target[1]);
        gpu.target[1] = NULL;
        gpu.history_valid[1] = 0;
    }
    if(sample->tag){
        sample->swap_tick=source_clock_now();
        gpu.presented[sample->tag%4]=*sample;
    }
    return 0;
}
void source_graphics_next_present(unsigned tag,int defer){
    gpu.next_present_tag=tag;
    gpu.defer_next_present=defer!=0;
}
int source_graphics_present_sample(unsigned tag,SourcePresentSample* out){
    if(!tag||gpu.presented[tag%4].tag!=tag)return 0;
    *out=gpu.presented[tag%4];
    return 1;
}
int source_graphics_drain_present(void){
    if(!gpu.pending_eyes)return 0;
    if(gpu.fence_error){errno=gpu.fence_error;return -1;}
    // complete_present uses direct GX transfers only; it never reacquires
    // Citro3D or resets the previous frame's diagnostic snapshot.
    if(complete_present(gpu.pending_eyes))return -1;
    gpu.pending_eyes=0;
    return 0;
}
int source_graphics_overlap_phase(int geometry){
    if(!gpu.pending_eyes)return 0;
    uint64_t* tick=geometry?&gpu.next_geometry_tick:&gpu.next_cpu_tick;
    if(!*tick)*tick=source_clock_now();
    // Present a finished picture promptly instead of waiting for a subsequent
    // upload. If still drawing, allow CPU-only game and scene work to continue.
    if(irq_snapshot(DRAW_IRQ)!=gpu.irq_start[DRAW_IRQ])return source_graphics_drain_present();
    return 0;
}
static int present(unsigned eyes){
    finish();
    gpu.presenting=(SourcePresentSample){.tag=gpu.next_present_tag,.draws=gpu.draw_calls};
    const bool defer=gpu.defer_next_present&&eyes==2&&!gpu.blur&&gpu.submitted.draw;
    gpu.next_present_tag=0;gpu.defer_next_present=false;
    gpu.next_cpu_tick=gpu.next_geometry_tick=gpu.returned_tick=0;
    if(!defer)return complete_present(eyes);
    gpu.pending_eyes=eyes;
    gpu.presenting.deferred=1;
    gpu.returned_tick=source_clock_now();
    return 0;
}

static int clear_plane(const char* waiter,C3D_RenderTarget* target,C3D_ClearBits plane,u32 color){
    begin_submission((SourceGpuFenceReport){.eyes=gpu.eyes,.fill=1,.command=GX_FILL});
    C3D_RenderTargetClear(target,plane,color,0);
    return wait_for_irq(waiter,FILL_IRQ);
}
static int clear_target(C3D_RenderTarget* target,C3D_ClearBits bits,u32 color){
    // GSP can report a combined fill as a single PSC0 interrupt. Submit one
    // plane at a time, both using engine 0, so each PSC0 has a unique owner.
    if((bits&C3D_CLEAR_COLOR) && clear_plane("clear_target_color",target,C3D_CLEAR_COLOR,color))return -1;
    if((bits&C3D_CLEAR_DEPTH) && clear_plane("clear_target_depth",target,C3D_CLEAR_DEPTH,0))return -1;
    return 0;
}

static int command_room(C3D_RenderTarget* active,unsigned next_elements,int boundary) {
    u32 size,offset;
    if(fast_city_cpu){size=gpuCmdBufSize;offset=gpuCmdBufOffset;}
    else GPUCMD_GetBuffer(NULL,&size,&offset);
    // Command size alone does not bound PICA work. A short indexed command
    // can submit thousands of triangles. Preserve the render target while
    // fencing by both list size and vertex/index count.
    const unsigned equivalent_words=offset+gpu.segment_saved_words;
    const int native_limit=source_graphics_segment_needs_fence(
        equivalent_words,equivalent_words<=size?size-equivalent_words:0,gpu.segment_packets,gpu.segment_elements,next_elements,boundary);
    if(!native_limit && (!gpu.bounded_list || !source_graphics_bounded_needs_fence(
        equivalent_words,gpu.segment_packets,gpu.segment_elements,next_elements,boundary)))return 0;
    finish(); // submit this segment without presenting a partial eye/frame
    if(acquire(__func__))return -1; // fence the segment before reusing its commands
    gpu.draw_segment_us+=gpu.gpu_previous_draw_us;
    if(!C3D_FrameDrawOn(active)){errno=EIO;return -1;}
    ++gpu.command_recycles;
    return 0; // retain both color/depth buffers and original packet order
}
static int mesh_live(const SourceGpuMesh* m) {return source_resource_index_contains(&gpu.mesh_index,m);}
static int texture_live(const SourceGpuTexture* t) {return source_resource_index_contains(&gpu.texture_index,t);}
static int vertices_valid(const SourceRenderVertex* data,size_t count){
    return count<=VERTEX_BUDGET/sizeof(*data)&&source_graphics_vertices_finite(data,count);
}
static void rollback(void) {
    finish();
    if(gpu.array_indices && !gpu.fence_error)linearFree(gpu.array_indices);
    if(gpu.low_target)C3D_RenderTargetDelete(gpu.low_target);
    if(gpu.low_color.texture.data)C3D_TexDelete(&gpu.low_color.texture);
    if (gpu.white_ready) C3D_TexDelete(&gpu.white);
    if (gpu.skin_ready) shaderProgramFree(&gpu.skin_program);
    if (gpu.skin_dvlb) DVLB_Free(gpu.skin_dvlb);
    if (gpu.program_ready) shaderProgramFree(&gpu.program);
    if (gpu.dvlb) DVLB_Free(gpu.dvlb);
    for (unsigned i=0;i<2;++i) if (gpu.target[i]) C3D_RenderTargetDelete(gpu.target[i]);
    source_resource_index_close(&gpu.mesh_index);source_resource_index_close(&gpu.texture_index);
    free(gpu.transitions);
    if (gpu.texture_staging && !gpu.fence_error) linearFree(gpu.texture_staging);
    C3D_Fini(); gfxExit(); memset(&gpu,0,sizeof(gpu));
}
// Paint once into the lower LCD framebuffer. The top-screen GPU submission
// path and the game loop do not need to draw the controls each frame.
static void draw_touch_controls(void) {
    u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    if (!fb) return;
    const ConsoleFont* font = &consoleGetDefault()->font;
    memset(fb, 0, SOURCE_TOUCH_WIDTH * SOURCE_TOUCH_HEIGHT * sizeof(*fb));
    for (unsigned i = 0; i < SOURCE_TOUCH_ZONES; ++i) {
        const SourceTouchZone* zone = &source_touch_zones[i];
        if (source_application_os_is_new_3ds() && i != SOURCE_TOUCH_ZONES - 1) continue;
        const u16 color = zone->buttons ? RGB8_to_565(36, 55, 67) : RGB8_to_565(17, 26, 32);
        for (int x = zone->x + 1; x < zone->x + zone->width - 1; ++x) {
            for (int y = zone->y + 1; y < zone->y + zone->height - 1; ++y)
                fb[x * SOURCE_TOUCH_HEIGHT + SOURCE_TOUCH_HEIGHT - 1 - y] = color;
        }
        int x = zone->x + (zone->width - (int)strlen(zone->label) * 16) / 2;
        const int y = zone->y + (zone->height - 16) / 2;
        for (const char* p = zone->label; *p; ++p, x += 16) {
            const u8* glyph = font->gfx + ((unsigned char)*p - font->asciiOffset) * 8;
            for (int row = 0; row < 16; ++row) {
                for (int col = 0; col < 16; ++col) {
                    if (glyph[row / 2] & (0x80 >> (col / 2)))
                        fb[(x + col) * SOURCE_TOUCH_HEIGHT + SOURCE_TOUCH_HEIGHT - 1 - y - row] = 0xffff;
                }
            }
        }
    }
    GSPGPU_FlushDataCache(fb, SOURCE_TOUCH_WIDTH * SOURCE_TOUCH_HEIGHT * sizeof(*fb));
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}
void source_graphics_controls_text(const char* text) {
    if (!controller() || !gpu.open || !text) return;
    if (!*text) { draw_touch_controls(); return; }
    u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    if (!fb) return;
    const ConsoleFont* font = &consoleGetDefault()->font;
    memset(fb, 0, SOURCE_TOUCH_WIDTH * SOURCE_TOUCH_HEIGHT * sizeof(*fb));
    int x = 0, y = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p && y < SOURCE_TOUCH_HEIGHT; ++p) {
        if (*p == '\n') { x = 0; y += 8; continue; }
        if (x + 8 > SOURCE_TOUCH_WIDTH) { x = 0; y += 8; }
        if (y + 8 > SOURCE_TOUCH_HEIGHT) break;
        if (*p >= font->asciiOffset && *p < font->asciiOffset + font->numChars) {
            const u8* glyph = font->gfx + (*p - font->asciiOffset) * 8;
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 8; ++col)
                    if (glyph[row] & (0x80 >> col))
                        fb[(x + col) * SOURCE_TOUCH_HEIGHT + SOURCE_TOUCH_HEIGHT - 1 - y - row] = 0xffff;
        }
        x += 8;
    }
    GSPGPU_FlushDataCache(fb, SOURCE_TOUCH_WIDTH * SOURCE_TOUCH_HEIGHT * sizeof(*fb));
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}
static C3D_RenderTarget* create_eye_target(void) {
    return C3D_RenderTargetCreate(240,400,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
}
static bool ensure_right_eye(void) {
    if (!gpu.target[1] && !gpu.right_eye_alloc_failed) {
        gpu.target[1] = create_eye_target();
        gpu.right_eye_alloc_failed = gpu.target[1] == NULL;
        if (gpu.right_eye_alloc_failed)
            fprintf(stderr,"Right eye allocation failed, VRAM free=%lu\n",(unsigned long)vramSpaceFree());
    }
    return gpu.target[1] != NULL;
}
int source_graphics_open(void) {
    if (!controller()) return -1;
    if (gpu.open) { errno=EALREADY; return -1; }
    __atomic_store_n(&rejected_result,0,__ATOMIC_RELEASE);
    gfxInitDefault();
    if (!gfxGetFramebuffer(GFX_TOP,GFX_LEFT,NULL,NULL)) { gfxExit();errno=ENODEV;return -1; }
    gpu.command_buffer_bytes = source_application_os_is_old_3ds() ? OLD_COMMAND_BUFFER_BYTES : COMMAND_BUFFER_BYTES;
    if (!C3D_Init(gpu.command_buffer_bytes)) { gfxExit();errno=ENOMEM;return -1; }
    gpu.open=1;
    {
        // Both handhelds use one draw mode even when an asset has no index buffer.
        const size_t bytes=SOURCE_ARRAY_INDEX_WORDS*sizeof(uint16_t);
        gpu.array_indices=linearMemAlign(bytes,128);
        if(!gpu.array_indices){rollback();errno=ENOMEM;return -1;}
        gpu.array_index_paddr=osConvertVirtToPhys(gpu.array_indices);
        if(source_graphics_fill_array_indices(gpu.array_indices,SOURCE_ARRAY_INDEX_WORDS) ||
           R_FAILED(GSPGPU_FlushDataCache(gpu.array_indices,bytes))){rollback();errno=EIO;return -1;}
        gpu.vertex_bytes+=bytes;
    }
    // Developer-only positive control for the failure-capture path. This
    // marker is never included in a hardware release.
    gpu.capture_probe=input_capture_probe!=0;
    // Use direct GX submissions. Citro3D still encodes PICA state, but its
    // FIFO counts any GPU interrupt as progress. Each submission below waits
    // for its own event type before starting the next hardware operation.
    GX_BindQueue(NULL);
    gpu.target[0] = create_eye_target();
    if (!gpu.target[0] || (source_application_os_is_new_3ds() && !ensure_right_eye())) {
        rollback(); errno=ENOMEM; return -1;
    }
    gpu.dvlb=DVLB_ParseFile((u32*)world_shbin,(u32)(world_shbin_end-world_shbin));
    if (!gpu.dvlb || gpu.dvlb->numDVLE!=1) { rollback();errno=EILSEQ;return -1; }
    shaderProgramInit(&gpu.program);gpu.program_ready=1;
    if (shaderProgramSetVsh(&gpu.program,&gpu.dvlb->DVLE[0])<0) { rollback();errno=EILSEQ;return -1; }
    gpu.clip_matrix=shaderInstanceGetUniformLocation(gpu.program.vertexShader,"clipMatrix");
    if (gpu.clip_matrix<0) { rollback();errno=EILSEQ;return -1; }
    if (!C3D_TexInit(&gpu.white,8,8,GPU_RGBA8)) { rollback();errno=ENOMEM;return -1; }
    gpu.white_ready=1;memset(gpu.white.data,255,8*8*4);C3D_TexFlush(&gpu.white);
    C3D_TexSetFilter(&gpu.white,GPU_NEAREST,GPU_NEAREST);
    gpu.sampler=gpu.white;
    C3D_TexBind(0,&gpu.sampler);
    for(unsigned irq=0;irq<VBLANK_IRQ;++irq)LightEvent_Init(&completion[irq],RESET_ONESHOT);
    gspSetEventCallback(GSPGPU_EVENT_PSC0,gpu_irq,&irq_count[FILL_IRQ],false);
    gspSetEventCallback(GSPGPU_EVENT_P3D,draw_finished,&irq_count[DRAW_IRQ],false);
    gspSetEventCallback(GSPGPU_EVENT_PPF,gpu_irq,&irq_count[TRANSFER_IRQ],false);
    gspSetEventCallback(GSPGPU_EVENT_VBlank0,vblank,NULL,false);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    draw_touch_controls();
    return 0;
}
static void release_blur(void){
    for(unsigned i=0;i<2;++i){if(gpu.history_texture[i])source_graphics_texture_delete(gpu.history_texture[i]);gpu.history_texture[i]=NULL;free(gpu.history[i]);gpu.history[i]=NULL;gpu.history_valid[i]=0;}
    if(gpu.history_mesh)source_graphics_mesh_delete(gpu.history_mesh);gpu.history_mesh=NULL;
}
int source_graphics_set_blur(unsigned amount){
    if(amount>255){errno=EINVAL;return -1;}if(!controller()||!gpu.open){errno=ENODEV;return -1;}if(!source_render_options()->motion_blur)amount=0;gpu.blur=amount;if(!amount)release_blur();return 0;
}
int source_graphics_close(void) {
    if (!controller() || !gpu.open) { errno=ENODEV;return -1; }
    if(gpu.low_quad){
        if(source_graphics_mesh_delete(gpu.low_quad))return -1;
        gpu.low_quad=NULL;
    }
    release_blur();if (gpu.meshes || gpu.textures) { errno=EBUSY;return -1; }
    if (acquire(__func__)) return -1; // complete prior submissions before freeing GPU objects
    gspSetEventCallback(GSPGPU_EVENT_VBlank0,NULL,NULL,false);
    gspSetEventCallback(GSPGPU_EVENT_PPF,NULL,NULL,false);
    gspSetEventCallback(GSPGPU_EVENT_P3D,NULL,NULL,false);
    gspSetEventCallback(GSPGPU_EVENT_PSC0,NULL,NULL,false);
    rollback();return 0;
}
int source_graphics_wait(void) { return acquire(__func__); }
void source_graphics_reset_frame_timing(void){
    command_ticks=0;gpu.wait_ticks=0;gpu.gpu_previous_draw_us=0;
    if(!gpu.pending_eyes)gpu.eyes=0;
    gpu.fill_ticks=gpu.draw_ticks=gpu.transfer_ticks=gpu.swap_ticks=0;
    gpu.vertex_bindings=gpu.local_vertex_bindings=gpu.compact_binds=0;
    gpu.draw_calls=gpu.planned_packets=gpu.clip_updates=0;
    gpu.lists=(SourceGraphicsLists){0};
    gpu.framebuffer_barriers = gpu.texture_barriers = 0;
}
unsigned source_graphics_frame_draw_calls(void) { return gpu.draw_calls; }
SourceGraphicsLists source_graphics_frame_lists(void) { return gpu.lists; }
SourceGraphicsWait source_graphics_frame_wait(void) {
    return (SourceGraphicsWait){gpu.fill_ticks,gpu.draw_ticks,gpu.transfer_ticks,gpu.swap_ticks};
}
size_t source_graphics_linear_free(void){return linearSpaceFree();}
int source_graphics_stats(SourceGraphicsStats* out){
    if(!controller()||!gpu.open||!out){errno=ENODEV;return -1;}memset(out,0,sizeof(*out));
    for(SourceGpuMesh* p=gpu.meshes;p;p=p->next)++out->meshes;
    for(SourceGpuTexture* p=gpu.textures;p;p=p->next){
        ++out->textures;
        if(p->texture.maxLevel){++out->mip_textures;out->mip_bytes+=p->bytes-p->texture.size;}
        if(p->texture.fmt==GPU_ETC1){++out->etc1_textures;out->etc1_bytes+=p->bytes;}
        if(p->texture.fmt==GPU_ETC1A4){++out->etc1a4_textures;out->etc1a4_bytes+=p->bytes;}
    }
    out->vertex_bytes=gpu.vertex_bytes;out->texture_bytes=gpu.texture_bytes;out->rgba8_texture_bytes=gpu.rgba8_texture_bytes;out->vram_free=vramSpaceFree();out->linear_free=linearSpaceFree();
    out->command_recycles=gpu.command_recycles;out->command_peak_words=gpu.command_peak_words;
    out->wait_ticks=gpu.wait_ticks;out->gpu_previous_draw_us=gpu.gpu_previous_draw_us;out->gpu_complete_frame_us=gpu.gpu_complete_frame_us;out->eyes=gpu.eyes;
    out->offset_bindings=gpu.compact_binds;out->vertex_bindings=gpu.vertex_bindings;out->local_vertex_bindings=gpu.local_vertex_bindings;
    out->draw_calls=gpu.draw_calls;out->planned_packets=gpu.planned_packets;out->clip_updates=gpu.clip_updates;
    out->framebuffer_barriers = gpu.framebuffer_barriers;
    out->texture_barriers = gpu.texture_barriers;
    return 0;
}
static int capture_rgb(unsigned eye,uint8_t* rgb,size_t size){
    if(!rgb||size!=400*240*3){errno=EINVAL;return -1;}
    if(acquire(__func__))return -1;
    finish();
    if(wait_for_draw(__func__))return -1;
    uint8_t* linear=linearMemAlign(400*240*4,0x80);if(!linear){errno=ENOMEM;return -1;}
    const unsigned flags=GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8);
    if(transfer(__func__,gpu.target[eye],(u32*)linear,flags))return -1; // GX may still own linear on failure
    if(R_FAILED(GSPGPU_InvalidateDataCache(linear,400*240*4))){linearFree(linear);errno=EIO;return -1;}
    int result=source_graphics_unpack_capture(linear,400*240*4,rgb,size);
    linearFree(linear);return result;
}
int source_graphics_capture_rgb(uint8_t* rgb,size_t size){return capture_rgb(0,rgb,size);}
void source_graphics_debug_text(const char* text){
    if(!controller()||!gpu.open||!text)return;
    if(!*text){draw_touch_controls();return;}
    if(!gpu.console_ready){consoleInit(GFX_BOTTOM,NULL);gpu.console_ready=1;}printf("\x1b[2J\x1b[H%s",text);
    u16 w,h;u8* fb=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);if(fb)GSPGPU_FlushDataCache(fb,(size_t)w*h*sizeof(u16));
}
int source_graphics_stream_write_reserved(SourceGpuMesh** handle,size_t count,size_t index_count,size_t range_count,
                                         SourceStreamWriter write,void* context,SourceStreamCapacity* capacity){
    if(!controller())return -1;
    if(!handle||!write||!count||count>VERTEX_BUDGET/sizeof(SourceRenderVertex)
        ||index_count>VERTEX_BUDGET/sizeof(uint16_t)||range_count>65536
        ||(range_count!=0)!=(index_count!=0)){errno=EINVAL;return -1;}
    SourceGpuMesh* mesh=*handle;
    if(mesh&&(!mesh_live(mesh)||mesh->skin||count>mesh->count||index_count>mesh->index_capacity)){
        errno=EINVAL;return -1;
    }
    if(acquire(__func__))return -1;
    const int creating=mesh==NULL;
    if(creating){
        size_t vc=count,ic=index_count;
        if(capacity&&capacity->vertices>=count&&capacity->indices>=index_count&&
           capacity->vertices<=VERTEX_BUDGET/sizeof(SourceRenderVertex)&&capacity->indices<=VERTEX_BUDGET/sizeof(uint16_t)){
            vc=capacity->vertices;
            ic=capacity->indices;
        }
        const size_t minimum=source_graphics_stream_bytes(count,index_count);
        size_t bytes=source_graphics_stream_bytes(vc,ic);
        if(minimum>VERTEX_BUDGET-gpu.vertex_bytes){errno=ENOMEM;return -1;}
        mesh=calloc(1,sizeof(*mesh));
        if(!mesh)return -1;
        if(bytes<=VERTEX_BUDGET-gpu.vertex_bytes)mesh->vertices=linearMemAlign(bytes,0x80);
        if(!mesh->vertices&&bytes!=minimum){
            vc=count;ic=index_count;bytes=minimum;
            mesh->vertices=linearMemAlign(bytes,0x80);
        }
        if(!mesh->vertices){
            fprintf(stderr,"GPU_VERTEX_OOM bytes=%zu linear_free=%lu vertices=%zu indices=%zu\n",
                bytes,(unsigned long)linearSpaceFree(),count,index_count);
            free(mesh);errno=ENOMEM;return -1;
        }
        // The linear allocation keeps one physical address until mesh destruction.
        mesh->vertex_paddr=osConvertVirtToPhys(mesh->vertices);
        mesh->count=vc;
        mesh->bytes=bytes;
        mesh->index_capacity=ic;
    }
    if(range_count>mesh->range_capacity){
        SourceIndexRange* resized=realloc(mesh->ranges,range_count*sizeof(*mesh->ranges));
        if(!resized)goto failed;
        mesh->ranges=resized;
        mesh->range_capacity=range_count;
    }
    // Keep partially written data out of packet validation if packing fails.
    mesh->used=mesh->range_count=0;
    // The previous draw is fenced. Keep the live index span immediately
    // after this frame's vertices so a shrinking stream flushes no old tail.
    mesh->indices=index_count?(uint16_t*)((uint8_t*)mesh->vertices+index_buffer_offset(count)):NULL;
    mesh->index_paddr=index_count?mesh->vertex_paddr+index_buffer_offset(count):0;
    if(write(context,mesh->vertices,mesh->indices,mesh->ranges))goto failed;
    if(!vertices_valid(mesh->vertices,count)
        ||source_graphics_validate_index_ranges(count,mesh->indices,index_count,mesh->ranges,range_count)){
        errno=EINVAL;goto failed;
    }
    const size_t flush=source_graphics_stream_bytes(count,index_count);
    if(R_FAILED(GSPGPU_FlushDataCache(mesh->vertices,flush))){errno=EIO;goto failed;}
    if(creating){
        if(source_resource_index_insert(&gpu.mesh_index,mesh))goto failed;
        mesh->next=gpu.meshes;
        mesh->link=&gpu.meshes;
        if(mesh->next)mesh->next->link=&mesh->next;
        gpu.meshes=mesh;
        gpu.vertex_bytes+=mesh->bytes;
        *handle=mesh;
    }
    mesh->used=count;
    mesh->range_count=range_count;
    if(capacity)*capacity=(SourceStreamCapacity){mesh->count,mesh->index_capacity};
    return 0;

failed:
    if(creating){
        linearFree(mesh->vertices);
        free(mesh->ranges);
        free(mesh);
    }
    return -1;
}

struct MeshCopy {const SourceRenderVertex* vertices;size_t count;};
static int copy_mesh(void* context,SourceRenderVertex* vertices,uint16_t* indices,SourceIndexRange* ranges){
    (void)indices;
    (void)ranges;
    const struct MeshCopy* copy=context;
    memcpy(vertices,copy->vertices,copy->count*sizeof(*vertices));
    return 0;
}
SourceGpuMesh* source_graphics_mesh_create(const SourceRenderVertex* vertices,size_t count){
    SourceGpuMesh* mesh=NULL;
    if(!vertices){errno=EINVAL;return NULL;}
    struct MeshCopy copy={vertices,count};
    if(source_graphics_stream_write(&mesh,count,0,0,copy_mesh,&copy))return NULL;
    return mesh;
}
static bool prepare_skin_shader(void) {
    if(gpu.skin_tried)return gpu.skin_ready;
    gpu.skin_tried=true;
    gpu.skin_dvlb=DVLB_ParseFile((u32*)skin_shbin,(u32)(skin_shbin_end-skin_shbin));
    if(!gpu.skin_dvlb||gpu.skin_dvlb->numDVLE!=1)return false;
    shaderProgramInit(&gpu.skin_program);
    if(shaderProgramSetVsh(&gpu.skin_program,&gpu.skin_dvlb->DVLE[0])>=0){
        gpu.skin_clip=shaderInstanceGetUniformLocation(gpu.skin_program.vertexShader,"clipMatrix");
        gpu.skin_rows=shaderInstanceGetUniformLocation(gpu.skin_program.vertexShader,"boneRows");
        gpu.skin_uv=shaderInstanceGetUniformLocation(gpu.skin_program.vertexShader,"skinUV");
        gpu.skin_enabled=shaderInstanceGetUniformLocation(gpu.skin_program.vertexShader,"useBones");
        if(gpu.skin_enabled>=0x68&&gpu.skin_enabled<0x78&&gpu.skin_clip>=0&&gpu.skin_rows>=0&&gpu.skin_uv>=0&&gpu.skin_rows+SOURCE_SKIN_BONES*3<=96){
            gpu.skin_ready=true;
            return true;
        }
    }
    shaderProgramFree(&gpu.skin_program);
    return false;
}
struct SkinUpload {
    const SourceRenderVertex* vertices;
    const uint16_t* indices;
    size_t count,index_count;
};
static int copy_skin_mesh(void* context,SourceRenderVertex* vertices,uint16_t* indices,SourceIndexRange* ranges) {
    const struct SkinUpload* upload=context;
    memcpy(vertices,upload->vertices,upload->count*sizeof(*vertices));
    memcpy(indices,upload->indices,upload->index_count*sizeof(*indices));
    ranges[0]=(SourceIndexRange){0,upload->count,0,upload->index_count,0};
    return 0;
}
SourceGpuMesh* source_graphics_skin_mesh_create(const SourceRenderVertex* vertices,const SourceSkinInfluence* influences,size_t count,const uint16_t* indices,size_t index_count) {
    if(!controller()||!gpu.open||!vertices||!influences||!indices||!count||count>65536||
       !index_count||index_count>SOURCE_DRAW_SLICE_ELEMENTS)return NULL;
    if(!prepare_skin_shader())return NULL;
    const size_t bytes=count*sizeof(*influences);
    if(source_graphics_stream_bytes(count,index_count)+bytes>VERTEX_BUDGET-gpu.vertex_bytes)return NULL;
    SourceMeshSkin* skin=calloc(1,sizeof(*skin));
    if(!skin)return NULL;
    skin->influences=linearMemAlign(bytes,0x80);
    if(!skin->influences){free(skin);return NULL;}
    memcpy(skin->influences,influences,bytes);
    SourceGpuMesh* mesh=NULL;
    struct SkinUpload upload={vertices,indices,count,index_count};
    if(R_FAILED(GSPGPU_FlushDataCache(skin->influences,bytes))||
       source_graphics_stream_write(&mesh,count,index_count,1,copy_skin_mesh,&upload)){
        linearFree(skin->influences);
        free(skin);
        return NULL;
    }
    mesh->skin=skin;
    mesh->bytes+=bytes;
    gpu.vertex_bytes+=bytes;
    return mesh;
}
int source_graphics_skin_pose(SourceGpuMesh* mesh,const float* rows,unsigned bones) {
    if(!controller()||!mesh_live(mesh)||!mesh->skin||!rows||!bones||bones>SOURCE_SKIN_BONES)return -1;
    for(unsigned i=0;i<bones*12;++i)if(!isfinite(rows[i]))return -1;
    const SourceSkinInfluence* influence=mesh->skin->influences;
    if(!mesh->skin->bones){
        for(size_t i=0;i<mesh->used;++i)
            for(unsigned s=0;s<3;++s)
                if(influence[i].rows[s]>=bones*3||influence[i].rows[s]%3||
                   !isfinite(influence[i].weights[s]))return -1;
    }else if(mesh->skin->bones!=bones)return -1;
    if(gpu.pending_eyes && source_graphics_drain_present())return -1;
    memcpy(mesh->skin->rows,rows,bones*12*sizeof(float));
    mesh->skin->bones=bones;
    return 0;
}
int source_graphics_mesh_update(SourceGpuMesh* mesh,const SourceRenderVertex* vertices,size_t count){
    if(!mesh){errno=EINVAL;return -1;}
    if(!vertices){errno=EINVAL;return -1;}
    struct MeshCopy copy={vertices,count};
    return source_graphics_stream_write(&mesh,count,0,0,copy_mesh,&copy);
}
int source_graphics_mesh_matches(const SourceGpuMesh* mesh,const SourceRenderVertex* vertices,size_t count){
    if(!controller())return -1;
    if(!mesh_live(mesh)||count>mesh->count||!vertices_valid(vertices,count)){errno=EINVAL;return -1;}
    return memcmp(mesh->vertices,vertices,count*sizeof(*vertices))==0;
}
int source_graphics_mesh_delete(SourceGpuMesh* mesh) {
    if (!controller()) return -1;
    if (!mesh_live(mesh)) { errno=EINVAL;return -1; }
    if (acquire(__func__)) return -1;
    *mesh->link=mesh->next;
    if(mesh->next)mesh->next->link=mesh->link;
    source_resource_index_remove(&gpu.mesh_index,mesh);gpu.vertex_bytes-=mesh->bytes;
    if(mesh->skin){linearFree(mesh->skin->influences);free(mesh->skin);}
    linearFree(mesh->vertices);free(mesh->ranges);free(mesh);return 0;
}
static int upload_texture(SourceGpuTexture* texture,const uint8_t* tiled,size_t bytes){
    if(!texture->vram){
        memcpy(texture->texture.data,tiled,bytes);
        if(R_FAILED(GSPGPU_FlushDataCache(texture->texture.data,bytes))){errno=EIO;return -1;}
        return 0;
    }
    // VRAM is written by GX. The same staging buffer can be reused once PPF
    // has signalled that the complete texture and its mip chain were copied.
    memcpy(gpu.texture_staging,tiled,bytes);
    if(R_FAILED(GSPGPU_FlushDataCache(gpu.texture_staging,bytes))){errno=EIO;return -1;}
    finish();
    if(wait_for_draw(__func__))return -1;
    begin_submission((SourceGpuFenceReport){.eyes=gpu.eyes,.transfer=1,.command=GX_TEXTURE_COPY});
    const Result result=GX_TextureCopy(gpu.texture_staging,0,texture->texture.data,0,bytes,8);
    if(R_FAILED(result)){
        report_gx_rejection(GX_TEXTURE_COPY,result);
        return fence_failure(__func__,result);
    }
    return wait_for_irq(__func__,TRANSFER_IRQ);
}
SourceGpuTexture* source_graphics_texture_create_mips(unsigned width,unsigned height,SourceTextureFormat format,unsigned levels,const uint8_t* tiled,size_t bytes) {
    if (!controller()) return NULL;
    GPU_TEXCOLOR device_format;
    switch(format){
        case SOURCE_TEXTURE_RGBA8: device_format=GPU_RGBA8;break;
        case SOURCE_TEXTURE_RGB565: device_format=GPU_RGB565;break;
        case SOURCE_TEXTURE_RGBA5551: device_format=GPU_RGBA5551;break;
        case SOURCE_TEXTURE_RGBA4: device_format=GPU_RGBA4;break;
        case SOURCE_TEXTURE_LA8: device_format=GPU_LA8;break;
        case SOURCE_TEXTURE_ETC1: device_format=GPU_ETC1;break;
        case SOURCE_TEXTURE_ETC1A4: device_format=GPU_ETC1A4;break;
        default: errno=EINVAL;return NULL;
    }
    const unsigned bits_per_texel = format == SOURCE_TEXTURE_ETC1 ? 4 :
        format == SOURCE_TEXTURE_ETC1A4 ? 8 : format == SOURCE_TEXTURE_RGBA8 ? 32 : 16;
    if(width<8||height<8||width>1024||height>1024||(width&(width-1))||(height&(height-1))
        ||!tiled||!levels||levels>1+(unsigned)C3D_TexCalcMaxLevel(width,height)){errno=EINVAL;return NULL;}
    const unsigned base_bytes=width*height*bits_per_texel/8;
    if(bytes!=C3D_TexCalcTotalSize(base_bytes,levels-1)){errno=EINVAL;return NULL;}
    if (bytes>TEXTURE_BUDGET-gpu.texture_bytes) { errno=ENOMEM;return NULL; }
    if (acquire(__func__)) return NULL;
    SourceGpuTexture* texture=calloc(1,sizeof(*texture));if(!texture)return NULL;
    C3D_TexInitParams params={width,height,levels-1,device_format,GPU_TEX_2D,false};
    const bool old_console = source_application_os_is_old_3ds();
    if(old_console && bytes<=OLD_TEXTURE_STAGING_BYTES){
        if(!gpu.texture_staging)gpu.texture_staging=linearAlloc(OLD_TEXTURE_STAGING_BYTES);
        params.onVram=gpu.texture_staging!=NULL;
    }
    if(params.onVram)texture->vram=C3D_TexInitWithParams(&texture->texture,NULL,params);
    params.onVram=false;
    if(!texture->vram && ((old_console &&
        linearSpaceFree() < bytes + OLD_LINEAR_GEOMETRY_RESERVE) ||
        !C3D_TexInitWithParams(&texture->texture,NULL,params))){
        fprintf(stderr,"GPU_TEXTURE_OOM bytes=%zu linear_free=%lu vram_free=%lu\n",
            bytes,(unsigned long)linearSpaceFree(),(unsigned long)vramSpaceFree());
        free(texture);errno=ENOMEM;return NULL;
    }
    if(upload_texture(texture,tiled,bytes)){
        if(!gpu.fence_error)C3D_TexDelete(&texture->texture);
        free(texture);return NULL;
    }
    C3D_TexSetFilter(&texture->texture,GPU_LINEAR,GPU_LINEAR);C3D_TexSetWrap(&texture->texture,GPU_REPEAT,GPU_REPEAT);
    if(levels>1)C3D_TexSetFilterMipmap(&texture->texture,GPU_NEAREST);
    if(source_resource_index_insert(&gpu.texture_index,texture)){C3D_TexDelete(&texture->texture);free(texture);return NULL;}
    texture->bytes=bytes;texture->next=gpu.textures;gpu.textures=texture;
    gpu.texture_bytes+=bytes;gpu.rgba8_texture_bytes+=(size_t)width*height*4;
    return texture;
}
SourceGpuTexture* source_graphics_texture_create_format(unsigned width,unsigned height,SourceTextureFormat format,const uint8_t* tiled,size_t bytes){
    return source_graphics_texture_create_mips(width,height,format,1,tiled,bytes);
}
SourceGpuTexture* source_graphics_texture_create(unsigned width,unsigned height,const uint8_t* tiled,size_t bytes){
    return source_graphics_texture_create_format(width,height,SOURCE_TEXTURE_RGBA8,tiled,bytes);
}
int source_graphics_texture_delete(SourceGpuTexture* texture) {
    if (!controller()) return -1;
    if (!texture_live(texture)) { errno=EINVAL;return -1; }
    if (acquire(__func__)) return -1;
    SourceGpuTexture** p=&gpu.textures;while (*p!=texture) p=&(*p)->next;*p=texture->next;
    source_resource_index_remove(&gpu.texture_index,texture);
    gpu.texture_bytes-=texture->bytes;
    gpu.rgba8_texture_bytes-=(size_t)texture->texture.width*texture->texture.height*4;
    if(gpu.sampler.data==texture->texture.data){
        gpu.sampler=gpu.white;
        C3D_TexBind(0,&gpu.sampler);
    }
    C3D_TexDelete(&texture->texture);free(texture);return 0;
}
int source_graphics_texture_update(SourceGpuTexture* texture,const uint8_t* tiled,size_t bytes){
    if(!controller())return -1;if(!texture_live(texture)||!tiled||bytes!=texture->bytes){errno=EINVAL;return -1;}
    if(acquire(__func__))return -1;
    if(upload_texture(texture,tiled,bytes))return -1;
    // A CPU cache flush cannot invalidate PICA's texture cache. Rebinding
    // the active sampler lets Citro3D emit the texture-cache clear.
    if(gpu.sampler.data==texture->texture.data)C3D_TexBind(0,&gpu.sampler);
    return 0;
}
static void bind_sampler(const C3D_Tex* texture,GPU_TEXTURE_WRAP_PARAM u,GPU_TEXTURE_WRAP_PARAM v,
                         GPU_TEXTURE_FILTER_PARAM filter){
    gpu.sampler=*texture;
    C3D_TexSetWrap(&gpu.sampler,u,v);
    C3D_TexSetFilter(&gpu.sampler,filter,filter);
    C3D_TexBind(0,&gpu.sampler);
}
static int packet_valid(const SourceDrawPacket* p,const SourceDrawPacket* previous,unsigned changes) {
    // Consecutive draws often share the streaming arena or a material.
    // Every distinct handle still passes the registry before dereferencing.
    if(((!previous||p->mesh!=previous->mesh)&&!(fast_city_cpu?source_resource_index_cached(&gpu.mesh_index,p->mesh):mesh_live(p->mesh)))||
       (p->texture&&(!previous||p->texture!=previous->texture)&&!(fast_city_cpu?source_resource_index_cached(&gpu.texture_index,p->texture):texture_live(p->texture)))||
       !p->count||p->first>p->mesh->used)return 0;
    if(p->index_range){if(p->index_range>p->mesh->range_count)return 0;const SourceIndexRange* r=&p->mesh->ranges[p->index_range-1];
        // A visible prefix of an immutable run starts at the very same validated
        // index address and uses the same vertex base. No page or index rebasing.
        if(p->first!=r->vertex_first||p->count>r->index_count||
           (r->triangle_strip ? p->count!=r->index_count : p->count%3))return 0;
    }else if(p->count%3||p->count>p->mesh->used-p->first)return 0;
    if (!p->width||!p->height||p->x>=400||p->y>=240||p->width>400-p->x||p->height>240-p->y||p->blend>SOURCE_BLEND_ADD||p->depth_write>1||p->cull>1||p->alpha_cutoff>255) return 0;
    if (p->orthographic>1||p->ui_depth>1||p->clip_enabled>1||p->modulate_2x>1||p->alpha_2x>1||p->original_blend>1||p->nearest>1||p->u_address>2||p->v_address>2||!isfinite(p->z_bias)) return 0;
    if(p->fog_enabled>1||p->fog_alpha>255||!isfinite(p->fog_start))return 0;
    if(p->original_blend&&(p->blend_mode>17||p->blend_mode==14||p->blend_mode==16||p->fixed_alpha>255))return 0;
    if(p->mesh->skin&&(!p->index_range||!p->mesh->skin->bones||p->orthographic||
       !isfinite(p->skin_u_scale)||!isfinite(p->skin_v_scale)||!isfinite(p->skin_v_offset)))return 0;
    if (p->clip_enabled&&(!p->clip_width||!p->clip_height||p->clip_x>=400||p->clip_y>=240||p->clip_width>400-p->clip_x||p->clip_height>240-p->clip_y)) return 0;
    if ((changes&SOURCE_GFX_PROJECTION)&&!p->orthographic&&(!isfinite(p->hfov)||p->hfov<=1||p->hfov>=179||!isfinite(p->near_clip)||p->near_clip<=0||!isfinite(p->far_clip)||p->far_clip<=p->near_clip||!isfinite(p->focal_distance)||p->focal_distance<=p->near_clip||!isfinite(p->aspect)||p->aspect<0)) return 0;
    if(changes&SOURCE_GFX_MODEL_VIEW)
        for (unsigned i=0;i<16;++i) if (!isfinite(p->model_view[i])) return 0;
    return 1;
}
static void blend_packet(const SourceDrawPacket* p){
    GPU_BLENDEQUATION op=GPU_BLEND_ADD;GPU_BLENDFACTOR src=GPU_ONE,dst=GPU_ZERO;
    const unsigned mode=effective_blend(p);
    switch(mode){
        case 0:break;
        case 1:src=GPU_SRC_ALPHA;dst=GPU_ONE;break;
        case 2:src=GPU_CONSTANT_ALPHA;dst=GPU_ONE;break;
        case 3:op=GPU_BLEND_REVERSE_SUBTRACT;src=GPU_SRC_ALPHA;dst=GPU_ONE;break;
        case 4:op=GPU_BLEND_REVERSE_SUBTRACT;src=GPU_CONSTANT_ALPHA;dst=GPU_ONE;break;
        case 5:src=GPU_SRC_ALPHA;dst=GPU_ONE_MINUS_SRC_ALPHA;break;
        case 6:src=GPU_CONSTANT_ALPHA;dst=GPU_ONE_MINUS_CONSTANT_ALPHA;break;
        case 7:src=GPU_ZERO;dst=GPU_SRC_ALPHA;break;
        case 8:src=GPU_ZERO;dst=GPU_CONSTANT_ALPHA;break;
        case 9:src=GPU_DST_COLOR;dst=GPU_ONE;break;
        case 10:src=GPU_DST_COLOR;dst=GPU_CONSTANT_ALPHA;break;
        case 13:src=GPU_ONE_MINUS_SRC_ALPHA;dst=GPU_SRC_ALPHA;break;
        case 15:src=GPU_ZERO;dst=GPU_SRC_COLOR;break;
        case 17:src=GPU_ONE;dst=GPU_ONE_MINUS_SRC_ALPHA;break;
    }
    if(uses_constant_alpha(mode)){
        const unsigned alpha=clamped_alpha(p->fixed_alpha)*255/128;
        C3D_BlendingColor(alpha*0x01010101u);
    }
    C3D_AlphaBlend(op,GPU_BLEND_ADD,src,dst,GPU_ONE,GPU_ZERO);
}
static GPU_TEXTURE_WRAP_PARAM wrap(unsigned value){return value==0?GPU_REPEAT:value==1?GPU_CLAMP_TO_EDGE:GPU_CLAMP_TO_BORDER;}
static int blur_frame(float stereo){
    const unsigned eyes=stereo>0?2:1;const size_t bytes=400*240*3;uint8_t* current=malloc(bytes);uint8_t* tiled=calloc(512*256,4);
    if(!current||!tiled){free(current);free(tiled);errno=ENOMEM;return -1;}
    if(eyes==1)gpu.history_valid[1]=0; // a later stereo transition has no stale right eye
    if(!gpu.history_mesh){
        // CPU history is top-first, including the 16 padded bottom rows.
        // Convert its sampler V exactly as for native font/render-target data.
        SourceRenderVertex quad[6]={{0,0,0,255,255,255,255,0,1},{0,448,0,255,255,255,255,0,1-240.f/256},{640,448,0,255,255,255,255,400.f/512,1-240.f/256},{0,0,0,255,255,255,255,0,1},{640,448,0,255,255,255,255,400.f/512,1-240.f/256},{640,0,0,255,255,255,255,400.f/512,1}};
        gpu.history_mesh=source_graphics_mesh_create(quad,6);if(!gpu.history_mesh)goto fail;
    }
    for(unsigned eye=0;eye<eyes;++eye){
        if(capture_rgb(eye,current,bytes))goto fail;
        if(!gpu.history[eye]){gpu.history[eye]=malloc(bytes);if(!gpu.history[eye]){errno=ENOMEM;goto fail;}}
        if(gpu.history_valid[eye])source_graphics_blend_rgb(current,gpu.history[eye],bytes,gpu.blur);
        memcpy(gpu.history[eye],current,bytes);gpu.history_valid[eye]=1;
        for(unsigned y=0;y<256;++y)for(unsigned x=0;x<512;++x){unsigned sx=x<400?x:399,sy=y<240?y:239,morton=0;for(unsigned bit=0;bit<3;++bit)morton|=((x>>bit)&1)<<(2*bit),morton|=((y>>bit)&1)<<(2*bit+1);
            size_t to=(((y/8)*64+x/8)*64+morton)*4,from=(sy*400+sx)*3;tiled[to]=255;tiled[to+1]=current[from+2];tiled[to+2]=current[from+1];tiled[to+3]=current[from];}
        if(!gpu.history_texture[eye])gpu.history_texture[eye]=source_graphics_texture_create(512,256,tiled,512*256*4);
        else if(source_graphics_texture_update(gpu.history_texture[eye],tiled,512*256*4))goto fail;
        if(!gpu.history_texture[eye])goto fail;
    }
    if(acquire(__func__))goto fail;C3D_BindProgram(&gpu.program);
    for(unsigned eye=0;eye<eyes;++eye){
        if(!C3D_FrameDrawOn(gpu.target[eye])){errno=EIO;goto fail;}
        C3D_Mtx clip;Mtx_OrthoTilt(&clip,0,640,448,0,1,0,true);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,gpu.clip_matrix,&clip);
        C3D_SetViewport(0,0,240,400);C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,240,400);C3D_DepthTest(false,GPU_GEQUAL,GPU_WRITE_COLOR);C3D_CullFace(GPU_CULL_NONE);C3D_AlphaTest(false,GPU_ALWAYS,0);
        C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);C3D_DepthMap(true,-1,0);
        bind_sampler(&gpu.history_texture[eye]->texture,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE,GPU_LINEAR);
        C3D_TexEnvScale(C3D_GetTexEnv(0),C3D_Both,GPU_TEVSCALE_1);BufInfo_Init(C3D_GetBufInfo());
        if(BufInfo_Add(C3D_GetBufInfo(),gpu.history_mesh->vertices,sizeof(SourceRenderVertex),3,0x210)<0){errno=EIO;goto fail;}C3D_DrawArrays(GPU_TRIANGLES,0,6);
        gpu.segment_eyes|=1u<<eye;
        ++gpu.segment_packets;
        gpu.segment_elements+=6;
        gpu.segment_largest_draw=6;
        gpu.segment_last_element=5;
    }
    free(current);free(tiled);return present(eyes);
fail:finish();free(current);free(tiled);return -1;
}
float source_graphics_slider(void) {
    if (!controller() || !gpu.open) { errno=ENODEV; return NAN; }
    const float slider = source_stereo_slider(osGet3DSliderState());
    if (slider == 0.f) gpu.right_eye_alloc_failed = false;
    // Camera setup and submission must agree even when VRAM is fragmented.
    // With no right target, stay in mono until the next slider activation.
    if (slider > 0.f && !ensure_right_eye()) return 0.f;
    return slider;
}
int source_graphics_submit(const SourceDrawPacket* packets,size_t count,uint32_t clear,float stereo){return source_graphics_submit_frame(packets,count,clear,stereo,1);}
static int validate_packet_range(const SourceDrawPacket* packets,size_t count,float stereo) {
    if ((!packets&&count)||count>65536||!isfinite(stereo)||stereo<0||stereo>1) { errno=EINVAL;return -1; }
    if (!controller()||!gpu.open) { errno=ENODEV;return -1; }
    if(count>gpu.transition_capacity){
        size_t capacity=256;
        while(capacity<count)capacity*=2;
        uint16_t* transitions=realloc(gpu.transitions,capacity*sizeof(*transitions));
        if(!transitions)return -1;
        gpu.transitions=transitions;
        gpu.transition_capacity=capacity;
    }
    for (size_t i=0;i<count;++i) {
        const SourceDrawPacket* previous=i?&packets[i-1]:NULL;
        const unsigned changes=source_graphics_draw_transition(previous,&packets[i]);
        if (!packet_valid(&packets[i],previous,changes)) { errno=EINVAL;return -1; }
        gpu.transitions[i]=changes;
    }
    if(stereo>0)gpu.planned_packets+=(unsigned)count;
    return 0;
}
static void setup_vertex_input(bool skin,bool combined,bool first) {
    if(!combined||first)C3D_BindProgram(skin||combined?&gpu.skin_program:&gpu.program);
    if(skin||combined)C3D_BoolUnifSet(GPU_VERTEX_SHADER,gpu.skin_enabled,skin);
    AttrInfo_Init(C3D_GetAttrInfo());
    AttrInfo_AddLoader(C3D_GetAttrInfo(),0,GPU_FLOAT,3);
    AttrInfo_AddLoader(C3D_GetAttrInfo(),1,GPU_UNSIGNED_BYTE,4);
    AttrInfo_AddLoader(C3D_GetAttrInfo(),2,GPU_FLOAT,2);
    if(skin){
        AttrInfo_AddLoader(C3D_GetAttrInfo(),3,GPU_UNSIGNED_BYTE,4);
        AttrInfo_AddLoader(C3D_GetAttrInfo(),4,GPU_FLOAT,3);
    }
}
static int start_draw(float stereo,uint32_t clear,int clear_color) {
    if (stereo > 0.f && !ensure_right_eye()) { errno=ENOMEM; return -1; }
    if(acquire(__func__))return -1;
    gpu.eyes=stereo>0?2:1;
    gpu.draw_segment_us=0;
    gfxSet3D(stereo>0);
    C3D_TexEnvInit(C3D_GetTexEnv(0));
    C3D_TexEnvSrc(C3D_GetTexEnv(0),C3D_Both,GPU_TEXTURE0,GPU_PRIMARY_COLOR,0);
    C3D_TexEnvFunc(C3D_GetTexEnv(0),C3D_Both,GPU_MODULATE);
    for (unsigned stage=1;stage<6;++stage) C3D_TexEnvInit(C3D_GetTexEnv(stage));
    // Clear both eye targets before drawing. GX fill and PICA draw commands
    // can otherwise touch the same color/depth storage at the same time.
    for(unsigned eye=0;eye<gpu.eyes;++eye){
        if(clear_target(gpu.target[eye],clear_color?C3D_CLEAR_ALL:C3D_CLEAR_DEPTH,clear)){
            finish();return -1;
        }
    }
    return 0;
}
// A full SDK draw establishes DrawUsed on each newly selected framebuffer
// and after every split. Within that list the only mutations of DrawUsed are
// framebuffer selection and frame split, both of which reset this local path.
extern void C3Di_UpdateContext(void);
static const u32 indexed_triangles[]={
    GPU_GEOMETRY_PRIM,GPUCMD_HEADER(0,2,GPUREG_PRIMITIVE_CONFIG),
    1,GPUCMD_HEADER(0,15,GPUREG_RESTART_PRIMITIVE),
    0,GPUCMD_HEADER(0,15,GPUREG_INDEXBUFFER_CONFIG),
    0,GPUCMD_HEADER(0,15,GPUREG_NUMVERTICES),
    0,GPUCMD_HEADER(0,15,GPUREG_VERTEX_OFFSET),
    0x100,GPUCMD_HEADER(0,2,GPUREG_GEOSTAGE_CONFIG),
    0x100,GPUCMD_HEADER(0,2,GPUREG_GEOSTAGE_CONFIG2),
    0,GPUCMD_HEADER(0,1,GPUREG_START_DRAW_FUNC0),
    1,GPUCMD_HEADER(0,15,GPUREG_DRAWELEMENTS),
    1,GPUCMD_HEADER(0,1,GPUREG_START_DRAW_FUNC0),
    0,GPUCMD_HEADER(0,2,GPUREG_GEOSTAGE_CONFIG),
    0,GPUCMD_HEADER(0,2,GPUREG_GEOSTAGE_CONFIG2),
    1,GPUCMD_HEADER(0,15,GPUREG_VTX_FUNC),
    0,GPUCMD_HEADER(0,8,GPUREG_PRIMITIVE_CONFIG),
    0,GPUCMD_HEADER(0,8,GPUREG_PRIMITIVE_CONFIG),
};
static bool sdk_indexed_sequence(u32 start,u32 address,unsigned count){
    enum { words=sizeof(indexed_triangles)/sizeof(u32) };
    if(start>gpuCmdBufOffset||gpuCmdBufOffset-start<words)return false;
    const u32* actual=gpuCmdBuf+gpuCmdBufOffset-words;
    for(unsigned i=0;i<words;++i){
        const u32 expected=i==4?address:i==6?count:indexed_triangles[i];
        if(actual[i]!=expected)return false;
    }
    return true;
}
static bool emit_indexed_sequence(u32 address,unsigned count,bool clean_context){
    enum { words=sizeof(indexed_triangles)/sizeof(u32) };
    // This emits all deferred texture, matrix, palette and framebuffer state
    // through the same barrier hook as the ordinary SDK draw.
    if(!clean_context)C3Di_UpdateContext();
    if(gpuCmdBufOffset>gpuCmdBufSize||gpuCmdBufSize-gpuCmdBufOffset<words)return false;
    u32* output=gpuCmdBuf+gpuCmdBufOffset;
    memcpy(output,indexed_triangles,sizeof(indexed_triangles));
    output[4]=address;output[6]=count;
    gpuCmdBufOffset+=words;
    gpu.pending_pica_barriers=SOURCE_PICA_FLUSH_FRAMEBUFFER|SOURCE_PICA_SYNC_TEXTURE;
    return true;
}
// The SDK scans 96 float slots for each shader at every draw. During a
// packet the submitter knows exactly which programs and uniforms it wrote.
// List boundaries and program changes always take the complete SDK path.
static unsigned clean_uniform_stages;
// Effects are a pure register emitter in the SDK. Cache the actual SDK
// output, including the pre-write PICA barrier, within one command list.
// Never cache a target change, a texture address, or a draw trigger here.
typedef struct {
    u32 mode,flags,blend_color,z,pending;
} EffectKey;
typedef struct {
    EffectKey key;
    u32 words[64],epoch,count,after,flushes,texture_syncs;
} EffectSequence;
static EffectSequence effects_cache[32];
static u32 effects_epoch,effects_blend_color;
static const SourceDrawPacket* effects_packet;
void __real_C3Di_EffectBind(void*);
void __wrap_C3Di_EffectBind(void* effect){
    const SourceDrawPacket* p=effects_packet;
    if(!p){__real_C3Di_EffectBind(effect);return;}
    EffectKey key={.mode=effective_blend(p),.flags=(unsigned)(!p->orthographic||p->ui_depth)|
        (p->depth_write<<1)|(p->cull<<2)|(p->alpha_cutoff<<8),
        .blend_color=effects_blend_color,.pending=gpu.pending_pica_barriers};
    memcpy(&key.z,&p->z_bias,sizeof key.z);
    const u32 hash=key.mode*17u+key.flags*7u+key.blend_color*23u+key.pending*11u+(key.z>>20);
    EffectSequence* seq=&effects_cache[(hash^(hash>>7))&31];
    const u32 at=gpuCmdBufOffset;
    if(seq->epoch==effects_epoch&&seq->count&&!memcmp(&seq->key,&key,sizeof key)&&
       at<=gpuCmdBufSize&&seq->count<=gpuCmdBufSize-at){
        memcpy(gpuCmdBuf+at,seq->words,seq->count*sizeof(u32));
        gpuCmdBufOffset=at+seq->count;
        gpu.pending_pica_barriers=seq->after;
        gpu.framebuffer_barriers+=seq->flushes;gpu.texture_barriers+=seq->texture_syncs;
        return;
    }
    const u32 flushes=gpu.framebuffer_barriers,syncs=gpu.texture_barriers;
    __real_C3Di_EffectBind(effect);
    if(gpuCmdBufOffset<=at||gpuCmdBufOffset-at>64)return;
    seq->epoch=effects_epoch;seq->key=key;seq->count=gpuCmdBufOffset-at;
    seq->after=gpu.pending_pica_barriers;
    seq->flushes=gpu.framebuffer_barriers-flushes;seq->texture_syncs=gpu.texture_barriers-syncs;
    memcpy(seq->words,gpuCmdBuf+at,seq->count*sizeof(u32));
}
void __real_C3D_UpdateUniforms(GPU_SHADER_TYPE type);
void __wrap_C3D_UpdateUniforms(GPU_SHADER_TYPE type){
    if(!(clean_uniform_stages & (1u<<type)))__real_C3D_UpdateUniforms(type);
}
static int encode_eye(const SourceDrawPacket* packets,size_t first,size_t end,size_t count,float stereo,
                      unsigned eye,C3D_RenderTarget* target,bool reduced) {
    {
        const SourceDrawPacket* fog_state=NULL;
        const SourceDrawPacket* raster_state=NULL;
        int fog_mode=-1;
        int skin_mode=-1;
        const SourceRenderVertex* bound_vertices=NULL;
        const SourceRenderVertex* local_vertices=NULL;
        const SourceDrawPacket* matrix_source=NULL;
        unsigned clip_owner=0,texture_owner=0;
        bool sequence_ready=false;
        u32 vertex_window=0;
        C3D_Mtx projection,view,clip;
        if (!C3D_FrameDrawOn(target)) { finish();errno=EIO;return -1; }
        for (size_t i=first,drawn=0;i<end;) {
            const SourceDrawPacket* p=&packets[i];
            const unsigned slice=source_graphics_draw_slice(p->count-(unsigned)drawn);
            // End the previous eye or 3D pass before rebinding a new eye or
            // starting HUD draws. Reuse the same framebuffers and packet order.
            // Normal submission keeps small batches. The optional diagnostic
            // marker can still isolate any individual draw without a rebuild.
            const int boundary=(single_draw_requested && gpu.segment_packets) ||
                (gpu.segment_eyes && !(gpu.segment_eyes&(1u<<eye))) ||
                (p->orthographic && !gpu.segment_overlay);
            unsigned recycles=gpu.command_recycles;
            if(command_room(target,slice,boundary)){finish();return -1;}
            if(recycles!=gpu.command_recycles){sequence_ready=false;fog_state=raster_state=NULL;fog_mode=skin_mode=-1;bound_vertices=local_vertices=NULL;}
            unsigned changes;
            if(!raster_state)changes=SOURCE_GFX_FULL_TRANSITION;
            else if(drawn)changes=0;
            else changes=gpu.transitions[i];
            const int projection_changed=changes&SOURCE_GFX_PROJECTION;
            const int view_changed=changes&SOURCE_GFX_MODEL_VIEW;
            const bool skin=p->mesh->skin!=NULL;
            const bool combined=use_index_sequence&&gpu.skin_ready;
            const bool program_changed=skin_mode!=(int)skin;
            if(program_changed){
                setup_vertex_input(skin,combined,skin_mode<0);
                skin_mode=skin;
                bound_vertices=NULL;
            }
            // A list fence invalidates PICA state, but the per-eye CPU matrices
            // remain valid. Upload them again without repeating trig/products.
            const bool build_projection=projection_changed&&
                !source_graphics_same_projection(p,matrix_source);
            const bool build_view=view_changed&&(!matrix_source||
                memcmp(p->model_view,matrix_source->model_view,sizeof(p->model_view))!=0);
            if(build_projection){
                if(p->orthographic){
                    // Original 640x448 UI and its D3D 0-near/1-far z values.
                    Mtx_OrthoTilt(&projection,0,640,448,0,1,0,true);
                }else{
                    float aspect=p->aspect>0?p->aspect:(float)p->width/p->height;
                    float vfov=2*atanf(tanf(p->hfov*(float)M_PI/360)/aspect);
                    Mtx_PerspStereoTilt(&projection,vfov,aspect,p->near_clip,p->far_clip,
                        source_stereo_eye_iod(stereo,eye),p->focal_distance,false);
                }
            }
            if(build_view){
                for(unsigned r=0;r<4;++r)
                    view.r[r]=FVec4_New(p->model_view[r*4],p->model_view[r*4+1],p->model_view[r*4+2],p->model_view[r*4+3]);
            }
            if(projection_changed||view_changed||program_changed){
                // Compose in CPU float precision, once per camera state.
                // Both eyes keep their own off-axis projection.
                if(build_projection||build_view)Mtx_Multiply(&clip,&projection,&view);
                matrix_source=p;
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,skin||combined?gpu.skin_clip:gpu.clip_matrix,&clip);
                ++gpu.clip_updates;
            }
            if(skin&&(!drawn||program_changed)){
                const SourceMeshSkin* pose=p->mesh->skin;
                for(unsigned row=0;row<pose->bones*3;++row){
                    const float* values=pose->rows[row];
                    const int slot=gpu.skin_rows+row;
                    const C3D_FVec* current=&C3D_FVUnif[GPU_VERTEX_SHADER][slot];
                    if(combined&&raster_state&&!program_changed&&current->x==values[0]&&current->y==values[1]&&
                       current->z==values[2]&&current->w==values[3])continue;
                    C3D_FVUnifSet(GPU_VERTEX_SHADER,slot,values[0],values[1],values[2],values[3]);
                }
                C3D_FVUnifSet(GPU_VERTEX_SHADER,gpu.skin_uv,p->skin_u_scale,p->skin_v_scale,p->skin_v_offset,0);
            }
            // Top framebuffer is rotated 90 degrees; scissor/viewport follow it.
            if((changes&SOURCE_GFX_VIEWPORT)&&reduced){
                const unsigned width=SOURCE_OLD_WORLD_WIDTH,height=SOURCE_OLD_WORLD_HEIGHT;
                const unsigned x=p->x*width/400,y=p->y*height/240;
                const unsigned right=((p->x+p->width)*width+399)/400,bottom=((p->y+p->height)*height+239)/240;
                C3D_SetViewport(y,width-right,bottom-y,right-x);
                if(p->clip_enabled)C3D_SetScissor(GPU_SCISSOR_NORMAL,p->clip_y*height/240,
                    width-((p->clip_x+p->clip_width)*width+399)/400,((p->clip_y+p->clip_height)*height+239)/240,width-p->clip_x*width/400);
                else C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,height,width);
            }else if(changes&SOURCE_GFX_VIEWPORT){C3D_SetViewport(p->y,400-p->x-p->width,p->height,p->width);
            if(p->clip_enabled)C3D_SetScissor(GPU_SCISSOR_NORMAL,p->clip_y,400-p->clip_x-p->clip_width,p->clip_y+p->clip_height,400-p->clip_x);
            else C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,240,400);}
            if(changes&SOURCE_GFX_DEPTH)C3D_DepthTest(!p->orthographic||p->ui_depth,GPU_GEQUAL,p->depth_write?GPU_WRITE_ALL:GPU_WRITE_COLOR);
            if(changes&SOURCE_GFX_CULL)C3D_CullFace(p->cull?GPU_CULL_BACK_CCW:GPU_CULL_NONE);
            if(changes&SOURCE_GFX_ALPHA)C3D_AlphaTest(p->alpha_cutoff!=0,GPU_GREATER,p->alpha_cutoff);
            if(changes&SOURCE_GFX_BLEND)blend_packet(p);if(changes&SOURCE_GFX_Z)C3D_DepthMap(true,-1.f,p->z_bias*0.00001f);
            if(p->fog_enabled&&!p->orthographic){
                if(!source_graphics_same_fog_lut(p,fog_state)){
                    if(!gpu.fog_lut_valid||!source_graphics_same_fog_lut(p,&gpu.fog_lut_state)){
                        float data[256];
                        if(source_graphics_fog_lut(p->near_clip,p->far_clip,p->fog_start,p->fog_alpha,data)){
                            finish();return -1;
                        }
                        FogLut_FromArray(&gpu.fog,data);
                        gpu.fog_lut_state=*p;gpu.fog_lut_valid=true;
                    }
                    C3D_FogLutBind(&gpu.fog);fog_state=p;}
                if(fog_mode!=1||(changes&SOURCE_GFX_FOG_COLOR))C3D_FogColor(p->fog_color);
                if(fog_mode!=1)C3D_FogGasMode(GPU_FOG,GPU_PLAIN_DENSITY,false);fog_mode=1;
            }else{if(fog_mode!=0)C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);fog_mode=0;}
            if(changes&SOURCE_GFX_SAMPLER)
                bind_sampler(p->texture?&p->texture->texture:&gpu.white,wrap(p->u_address),wrap(p->v_address),
                    p->nearest?GPU_NEAREST:GPU_LINEAR);
            if(changes&SOURCE_GFX_RGB_SCALE)C3D_TexEnvScale(C3D_GetTexEnv(0),C3D_RGB,p->modulate_2x?GPU_TEVSCALE_2:GPU_TEVSCALE_1);
            if(changes&SOURCE_GFX_ALPHA_SCALE)C3D_TexEnvScale(C3D_GetTexEnv(0),C3D_Alpha,p->alpha_2x?GPU_TEVSCALE_2:GPU_TEVSCALE_1);
            const SourceIndexRange* range=p->index_range?&p->mesh->ranges[p->index_range-1]:NULL;
            SourceArrayIndexDraw array={0};
            const bool indexed_array=!range && gpu.array_indices;
            if(indexed_array && source_graphics_array_index_plan(p->first+(unsigned)drawn,slice,p->mesh->used,&array)){
                finish();return -1;
            }
            const uint16_t* draw_indices=range?p->mesh->indices+range->index_first+drawn:
                indexed_array?gpu.array_indices+array.index_variant*SOURCE_DRAW_SLICE_ELEMENTS:NULL;
            const SourceRenderVertex* local_base=p->mesh->vertices+(range?range->vertex_first:0);
            if(local_base!=local_vertices){
                ++gpu.local_vertex_bindings;
                local_vertices=local_base;
            }
            const unsigned base=range?range->vertex_base:indexed_array?array.vertex_base:0;
            const SourceRenderVertex* vertex_base=p->mesh->vertices+base;
            const bool already_bound=vertex_base==bound_vertices;
            bool offset_only=false;
            if(vertex_base!=bound_vertices){
                ++gpu.vertex_bindings;
                if(bound_vertices && !skin){
                    // Only the address changes between flat 24-byte streams.
                    // The SDK initializes the complete layout on every eye,
                    // program change and recycled list before this fast path.
                    C3D_BufInfo* info=C3D_GetBufInfo();
                    info->buffers[0].offset=p->mesh->vertex_paddr+base*sizeof(*vertex_base)-info->base_paddr;
                    offset_only=true;
                }else{
                    BufInfo_Init(C3D_GetBufInfo());
                    vertex_window=C3D_GetBufInfo()->base_paddr;
                    if(BufInfo_Add(C3D_GetBufInfo(),vertex_base,sizeof(SourceRenderVertex),3,0x210)<0 ||
                       (skin&&BufInfo_Add(C3D_GetBufInfo(),p->mesh->skin->influences,sizeof(SourceSkinInfluence),2,0x43)<0)){
                        finish();errno=EIO;return -1;
                    }
                }
                bound_vertices=vertex_base;
            }
            gpu.segment_eyes|=1u<<eye;
            if(!gpu.segment_packets)gpu.segment_first=(unsigned)(eye*count+i);
            gpu.segment_last=(unsigned)(eye*count+i);
            ++gpu.segment_packets;
            ++gpu.draw_calls;
            if(p->orthographic)++gpu.segment_overlay;
            gpu.segment_elements+=slice;
            gpu.segment_last_element=(unsigned)drawn+slice-1;
            if(slice>gpu.segment_largest_draw){
                gpu.segment_largest_draw=slice;
                gpu.segment_largest_packet=gpu.segment_last;
            }
            if(gpu.input_count>=CAPTURE_DRAWS){finish();errno=EOVERFLOW;return -1;}
            SourceGpuInputDraw* input=&gpu.input_draws[gpu.input_count];
            input->mesh=p->mesh;
            input->indices=draw_indices;
            // A snapshot may outlive the caller's packet array. Keep only
            // owned state here, sharing matrix/sampler copies within this list.
            if(!gpu.input_count || (changes&SOURCE_GFX_SAMPLER)){
                texture_owner=gpu.input_count;
                input->texture=gpu.sampler;
                input->sampler_vram=p->texture && p->texture->vram;
            }
            if(!gpu.input_count || projection_changed || view_changed || program_changed){
                clip_owner=gpu.input_count;
                input->clip=clip;
            }
            input->texture_owner=texture_owner;
            input->clip_owner=clip_owner;
            input->packet=gpu.segment_last;
            input->strip=range&&range->triangle_strip;
            input->eye=eye;
            input->first=draw_indices?base:p->first+(unsigned)drawn;
            input->elements=slice;
            input->vertex_first=range?range->vertex_first:p->first+(unsigned)drawn;
            input->vertex_count=range?range->vertex_count:slice;
            input->index_first=range?range->index_first+(unsigned)drawn:0;
            input->vertex_base=base;
            input->orthographic=p->orthographic;
            input->depth_write=p->depth_write;
            input->alpha_cutoff=p->alpha_cutoff;
            input->cull=p->cull;
            ++gpu.input_count;
            if(use_index_sequence){
                if(!raster_state){
                    if(!++effects_epoch){memset(effects_cache,0,sizeof effects_cache);++effects_epoch;}
                    effects_blend_color=256; // unknown entry colour is stable until explicitly replaced
                }
                if((changes&SOURCE_GFX_BLEND)&&uses_constant_alpha(effective_blend(p)))
                    effects_blend_color=clamped_alpha(p->fixed_alpha)*255/128;
                effects_packet=p;
            }
            gpu.flat_offset_only=offset_only;
            if(use_index_sequence&&raster_state&&!program_changed){
                clean_uniform_stages=1u<<GPU_GEOMETRY_SHADER;
                if(!projection_changed&&!view_changed&&!skin)clean_uniform_stages|=1u<<GPU_VERTEX_SHADER;
            }
            if(draw_indices){
                ++gpu.segment_indexed;
                if(range&&range->triangle_strip){
                    C3D_DrawElements(GPU_TRIANGLE_STRIP,slice,C3D_UNSIGNED_SHORT,draw_indices);
                }else if(use_index_sequence&&!single_draw_requested){
                    const u32 pa=range?p->mesh->index_paddr+(range->index_first+drawn)*sizeof(*draw_indices):
                        gpu.array_index_paddr+array.index_variant*SOURCE_DRAW_SLICE_ELEMENTS*sizeof(*draw_indices);
                    if(pa<vertex_window){effects_packet=NULL;clean_uniform_stages=0;finish();errno=EINVAL;return -1;}
                    const u32 address=(pa-vertex_window)|0x80000000u;
                    // The previous SDK update already committed every register for
                    // this flat draw. A new eye/list always has a full transition.
                    const bool clean=fast_city_cpu && !changes && !skin && !program_changed && already_bound &&
                        fog_mode==0 && raster_state && (!raster_state->fog_enabled || raster_state->orthographic);
                    if(!sequence_ready||!emit_indexed_sequence(address,slice,clean)){
                        const u32 before=gpuCmdBufOffset;
                        C3D_DrawElements(GPU_TRIANGLES,slice,C3D_UNSIGNED_SHORT,draw_indices);
                        sequence_ready=sdk_indexed_sequence(before,address,slice);
                    }
                }else C3D_DrawElements(GPU_TRIANGLES,slice,C3D_UNSIGNED_SHORT,draw_indices);
            }else C3D_DrawArrays(GPU_TRIANGLES,p->first+(unsigned)drawn,slice);
            effects_packet=NULL;clean_uniform_stages=0;
            gpu.flat_offset_only=false;
            raster_state=p;
            drawn+=slice;
            if(drawn==p->count){++i;drawn=0;}
        }
    }
    return 0;
}
static int encode_packet_range(const SourceDrawPacket* packets,size_t count,float stereo) {
    for(unsigned eye=0;eye<gpu.eyes;++eye)
        if(encode_eye(packets,0,count,count,stereo,eye,gpu.target[eye],false))return -1;
    return 0;
}
static int validate_packets(const SourceDrawPacket* packets,size_t count,float stereo){
    if(!measure_command_cpu)return validate_packet_range(packets,count,stereo);
    const uint64_t start=source_clock_now();
    const int result=validate_packet_range(packets,count,stereo);
    command_ticks+=source_clock_now()-start;
    return result;
}
static int encode_packets(const SourceDrawPacket* packets,size_t count,float stereo){
    if(!measure_command_cpu)return encode_packet_range(packets,count,stereo);
    const uint64_t start=source_clock_now(),wait=gpu.wait_ticks;
    const int result=encode_packet_range(packets,count,stereo);
    command_ticks+=source_clock_now()-start-(gpu.wait_ticks-wait);
    return result;
}
int source_graphics_submit_frame(const SourceDrawPacket* packets,size_t count,uint32_t clear,float stereo,int clear_color) {
    if(gpu.early_open){errno=EBUSY;return -1;}
    if(validate_packets(packets,count,stereo)||start_draw(stereo,clear,clear_color)||encode_packets(packets,count,stereo))return -1;
    if(gpu.blur){finish();return blur_frame(stereo);}
    return present(gpu.eyes);
}
int source_graphics_submit_bounded(const SourceDrawPacket* packets,size_t count,uint32_t clear,float stereo) {
    gpu.bounded_list=true;
    const int result=source_graphics_submit_frame(packets,count,clear,stereo,1);
    gpu.bounded_list=false;
    return result;
}
int source_graphics_low_target_active(void){return gpu.low_target!=NULL;}
// One RGB565 render texture serves both eyes in turn. HUD and presentation
// retain the native target. Fence each change of owner before clearing or sampling.
static bool ready_low_target(void){
    if(gpu.low_quad)return true;
    if(!C3D_TexInitVRAM(&gpu.low_color.texture,256,512,GPU_RGB565))return false;
    gpu.low_color.vram=true;
    gpu.low_target=C3D_RenderTargetCreateFromTex(&gpu.low_color.texture,GPU_TEXFACE_2D,0,GPU_RB_DEPTH24);
    if(gpu.low_target){
        const float u=SOURCE_OLD_WORLD_HEIGHT/256.f,v=SOURCE_OLD_WORLD_WIDTH/512.f;
        const SourceRenderVertex quad[]={
            {0,0,0,255,255,255,255,u,v},{0,448,0,255,255,255,255,0,v},{640,448,0,255,255,255,255,0,0},
            {0,0,0,255,255,255,255,u,v},{640,448,0,255,255,255,255,0,0},{640,0,0,255,255,255,255,u,0}};
        gpu.low_quad=source_graphics_mesh_create(quad,6);
        if(gpu.low_quad)return true;
        C3D_RenderTargetDelete(gpu.low_target);gpu.low_target=NULL;
    }
    C3D_TexDelete(&gpu.low_color.texture);gpu.low_color=(SourceGpuTexture){0};
    return false;
}
static int join_low_pass(void){
    finish();
    if(acquire(__func__))return -1;
    gpu.draw_segment_us+=gpu.gpu_previous_draw_us;
    return 0;
}
int source_graphics_submit_low(const SourceDrawPacket* packets,size_t count,size_t world_count,uint32_t clear,float stereo,int bounded){
    if(world_count>count||!world_count||gpu.early_open){errno=EINVAL;return -1;}
    if(validate_packets(packets,count,stereo))return -1;
    if(!gpu.low_quad && !gpu.low_attempted){
        gpu.low_attempted=true;
        ready_low_target();
    }
    if(!gpu.low_quad)return bounded?source_graphics_submit_bounded(packets,count,clear,stereo):source_graphics_submit(packets,count,clear,stereo);
    if(start_draw(stereo,clear,0))return -1;
    gpu.bounded_list=bounded!=0;
    SourceDrawPacket quad={.mesh=gpu.low_quad,.texture=&gpu.low_color,.count=6,.width=400,.height=240,.orthographic=1,.nearest=1};
    for(unsigned i=0;i<4;++i)quad.model_view[i*5]=1;
    int result=-1;
    for(unsigned eye=0;eye<gpu.eyes;++eye){
        if(eye && join_low_pass())goto done;
        const uint32_t rgb565=((clear>>16)&0xf800)|((clear>>13)&0x07e0)|((clear>>11)&0x001f);
        if(clear_target(gpu.low_target,C3D_CLEAR_ALL,rgb565*0x10001u))goto done;
        if(encode_eye(packets,0,world_count,count,stereo,eye,gpu.low_target,true)||join_low_pass())goto done;
        if(encode_eye(&quad,0,1,1,stereo,eye,gpu.target[eye],false))goto done;
        if(world_count<count && encode_eye(packets,world_count,count,count,stereo,eye,gpu.target[eye],false))goto done;
    }
    result=present(gpu.eyes);
done:
    gpu.bounded_list=false;
    return result;
}
int source_graphics_submit_early(const SourceDrawPacket* packets,size_t count,uint32_t clear,float stereo,int bounded) {
    if(gpu.early_open||!count||count!=source_graphics_early_prefix(packets,count,stereo>0?2:1)){
        errno=EINVAL;
        return -1;
    }
    gpu.bounded_list=bounded!=0;
    if(validate_packets(packets,count,stereo)||start_draw(stereo,clear,1)||encode_packets(packets,count,stereo)){
        gpu.bounded_list=false;return -1;
    }
    finish();
    gpu.early_stereo=stereo;
    gpu.early_open=true;
    gpu.early_joined=false;
    return 0;
}
int source_graphics_wait_early(void) {
    if(!gpu.early_open||gpu.early_joined){errno=EINVAL;return -1;}
    if(acquire(__func__)){gpu.bounded_list=false;return -1;}
    gpu.draw_segment_us+=gpu.gpu_previous_draw_us;
    gpu.early_joined=true;
    return 0;
}
int source_graphics_submit_remaining(const SourceDrawPacket* packets,size_t count,float stereo) {
    if(!gpu.early_open||!gpu.early_joined||gpu.early_stereo!=stereo){errno=EINVAL;return -1;}
    gpu.early_open=false;
    int result=-1;
    if(!validate_packets(packets,count,stereo)&&!encode_packets(packets,count,stereo)){
        if(gpu.blur){finish();result=blur_frame(stereo);}
        else result=present(gpu.eyes);
    }
    gpu.bounded_list=false;
    return result;
}
void source_graphics_fatal(const char* text) {
    if (!controller()||!gpu.open||!text||gpu.fence_error||
        __atomic_load_n(&rejected_result,__ATOMIC_ACQUIRE)) return;
    // Citro3D's queue does not track the direct GX submissions. Do not
    // reset its list or submit a partial frame on the terminal error path.
    if(gpu.submitted.draw && irq_snapshot(DRAW_IRQ)==gpu.irq_start[DRAW_IRQ])return;
    consoleInit(GFX_BOTTOM,NULL);printf("%s\n",text);gfxFlushBuffers();gfxScreenSwapBuffers(GFX_BOTTOM,false);
}
#else
int source_graphics_open(void) { errno=ENOTSUP;return -1; }
int source_graphics_close(void) { errno=ENOTSUP;return -1; }
int source_graphics_wait(void) { errno=ENOTSUP;return -1; }
void source_graphics_next_present(unsigned tag,int defer){(void)tag;(void)defer;}
int source_graphics_overlap_phase(int geometry){(void)geometry;return 0;}
int source_graphics_drain_present(void){return 0;}
int source_graphics_present_sample(unsigned tag,SourcePresentSample* out){(void)tag;(void)out;return 0;}
int source_graphics_set_blur(unsigned a){(void)a;errno=ENOTSUP;return -1;}
size_t source_graphics_linear_free(void){return 0;}
int source_graphics_stats(SourceGraphicsStats* p){(void)p;errno=ENOTSUP;return -1;}
void source_graphics_reset_frame_timing(void){}
unsigned source_graphics_frame_draw_calls(void){return 0;}
SourceGraphicsLists source_graphics_frame_lists(void){return (SourceGraphicsLists){0};}
SourceGraphicsWait source_graphics_frame_wait(void){return (SourceGraphicsWait){0};}
int source_graphics_capture_rgb(uint8_t* p,size_t s){(void)p;(void)s;errno=ENOTSUP;return -1;}
void source_graphics_debug_text(const char* p){(void)p;}
void source_graphics_controls_text(const char* p){(void)p;}
int source_graphics_stream_write_reserved(SourceGpuMesh** m,size_t n,size_t ni,size_t nr,SourceStreamWriter writer,void* context,SourceStreamCapacity* capacity){
    (void)m;(void)n;(void)ni;(void)nr;(void)writer;(void)context;(void)capacity;errno=ENOTSUP;return -1;
}
SourceGpuMesh* source_graphics_mesh_create(const SourceRenderVertex* p,size_t n) { (void)p;(void)n;errno=ENOTSUP;return NULL; }
SourceGpuMesh* source_graphics_skin_mesh_create(const SourceRenderVertex* v,const SourceSkinInfluence* i,size_t c,const uint16_t* indices,size_t n){
    (void)v;(void)i;(void)c;(void)indices;(void)n;errno=ENOTSUP;return NULL;
}
int source_graphics_skin_pose(SourceGpuMesh* m,const float* r,unsigned b){(void)m;(void)r;(void)b;errno=ENOTSUP;return -1;}
int source_graphics_mesh_update(SourceGpuMesh* m,const SourceRenderVertex* p,size_t n) { (void)m;(void)p;(void)n;errno=ENOTSUP;return -1; }
int source_graphics_mesh_matches(const SourceGpuMesh* m,const SourceRenderVertex* p,size_t n){(void)m;(void)p;(void)n;errno=ENOTSUP;return -1;}
int source_graphics_mesh_delete(SourceGpuMesh* m) { (void)m;errno=ENOTSUP;return -1; }
SourceGpuTexture* source_graphics_texture_create(unsigned w,unsigned h,const uint8_t* p,size_t n) { (void)w;(void)h;(void)p;(void)n;errno=ENOTSUP;return NULL; }
SourceGpuTexture* source_graphics_texture_create_mips(unsigned w,unsigned h,SourceTextureFormat f,unsigned levels,const uint8_t* p,size_t n){
    (void)levels;return source_graphics_texture_create_format(w,h,f,p,n);
}
SourceGpuTexture* source_graphics_texture_create_format(unsigned w,unsigned h,SourceTextureFormat f,const uint8_t* p,size_t n) {
    (void)w;(void)h;(void)f;(void)p;(void)n;errno=ENOTSUP;return NULL;
}
int source_graphics_texture_delete(SourceGpuTexture* t) { (void)t;errno=ENOTSUP;return -1; }
int source_graphics_texture_update(SourceGpuTexture* t,const uint8_t* p,size_t n){(void)t;(void)p;(void)n;errno=ENOTSUP;return -1;}
int source_graphics_submit(const SourceDrawPacket* p,size_t n,uint32_t c,float s) { (void)p;(void)n;(void)c;(void)s;errno=ENOTSUP;return -1; }
int source_graphics_submit_bounded(const SourceDrawPacket* p,size_t n,uint32_t c,float s){(void)p;(void)n;(void)c;(void)s;errno=ENOTSUP;return -1;}
int source_graphics_submit_frame(const SourceDrawPacket* p,size_t n,uint32_t c,float s,int clear) { (void)p;(void)n;(void)c;(void)s;(void)clear;errno=ENOTSUP;return -1; }
int source_graphics_low_target_active(void){return 0;}
int source_graphics_submit_low(const SourceDrawPacket* p,size_t n,size_t w,uint32_t c,float s,int bounded){(void)w;return bounded?source_graphics_submit_bounded(p,n,c,s):source_graphics_submit(p,n,c,s);}
int source_graphics_submit_early(const SourceDrawPacket* p,size_t n,uint32_t c,float s,int bounded){(void)bounded;(void)p;(void)n;(void)c;(void)s;errno=ENOTSUP;return -1;}
int source_graphics_wait_early(void){errno=ENOTSUP;return -1;}
int source_graphics_submit_remaining(const SourceDrawPacket* p,size_t n,float s){(void)p;(void)n;(void)s;errno=ENOTSUP;return -1;}
float source_graphics_slider(void){errno=ENOTSUP;return NAN;}
void source_graphics_fatal(const char* t) { (void)t; }
#endif

int source_graphics_stream_write(SourceGpuMesh** mesh,size_t vertices,size_t indices,size_t ranges,SourceStreamWriter writer,void* context){
    return source_graphics_stream_write_reserved(mesh,vertices,indices,ranges,writer,context,NULL);
}
