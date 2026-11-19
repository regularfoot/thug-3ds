#include <source_port/audio_os.h>
#include <source_port/application_os.h>
#include <errno.h>
#include <string.h>
unsigned source_audio_rate(void) {
    return source_application_os_is_old_3ds() ? 32000 : SOURCE_AUDIO_RATE;
}
#ifdef __3DS__
#include <3ds.h>
static int opened;
static int16_t* buffers[SOURCE_AUDIO_BUFFERS];
static ndspWaveBuf waves[SOURCE_AUDIO_BUFFERS];
int source_audio_open(void) {
    if (opened) { errno=EALREADY; return -1; }
    if (R_FAILED(ndspInit())) { errno=ENODEV; return -1; }
    memset(waves,0,sizeof(waves));
    for (unsigned i=0;i<SOURCE_AUDIO_BUFFERS;++i) {
        buffers[i]=linearMemAlign(SOURCE_AUDIO_FRAMES*2*sizeof(int16_t),0x80);
        if (!buffers[i]) {
            ndspExit(); for (unsigned j=0;j<i;++j) linearFree(buffers[j]);
            memset(buffers,0,sizeof(buffers)); errno=ENOMEM; return -1;
        }
    }
    ndspChnReset(0); ndspChnSetFormat(0,NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetInterp(0,NDSP_INTERP_LINEAR); ndspChnSetRate(0,source_audio_rate());
    float mix[12]={0}; mix[0]=mix[1]=1; ndspChnSetMix(0,mix);
    opened=1; return 0;
}
static int free_slot(void) {
    for (unsigned i=0;i<SOURCE_AUDIO_BUFFERS;++i)
        if (waves[i].status==NDSP_WBUF_FREE || waves[i].status==NDSP_WBUF_DONE) return (int)i;
    return -1;
}
int source_audio_available(void) {
    if (!opened) { errno=ENODEV; return -1; }
    int free_count=0;
    for(unsigned i=0;i<SOURCE_AUDIO_BUFFERS;++i)
        if(waves[i].status==NDSP_WBUF_FREE || waves[i].status==NDSP_WBUF_DONE)++free_count;
    return free_count;
}
int source_audio_submit(const int16_t* data,unsigned frames) {
    if (!opened || !data || !frames || frames>SOURCE_AUDIO_FRAMES) { errno=EINVAL; return -1; }
    int index=free_slot(); if (index<0) { errno=EAGAIN; return -1; }
    memcpy(buffers[index],data,frames*2*sizeof(int16_t));
    DSP_FlushDataCache(buffers[index],frames*2*sizeof(int16_t));
    ndspWaveBuf* wave=&waves[index]; memset(wave,0,sizeof(*wave));
    wave->data_vaddr=buffers[index]; wave->nsamples=frames;
    ndspChnWaveBufAdd(0,wave); return 0;
}
void source_audio_discard(void){if(opened)ndspChnWaveBufClear(0);}
void source_audio_close(void) {
    if (!opened) return;
    ndspChnWaveBufClear(0); ndspChnReset(0); ndspExit();
    for (unsigned i=0;i<SOURCE_AUDIO_BUFFERS;++i) { linearFree(buffers[i]); buffers[i]=NULL; }
    memset(waves,0,sizeof(waves)); opened=0;
}
#else
int source_audio_open(void) { errno=ENOTSUP; return -1; }
int source_audio_available(void) { errno=ENOTSUP; return -1; }
int source_audio_submit(const int16_t* p,unsigned n) { (void)p;(void)n;errno=ENOTSUP;return -1; }
void source_audio_discard(void) {}
void source_audio_close(void) {}
#endif
