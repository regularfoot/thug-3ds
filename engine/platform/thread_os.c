#include <source_port/thread_os.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#ifdef __3DS__
#include <3ds/thread.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
#else
#include <pthread.h>
#include <time.h>
#endif

struct SourceThreadOS {
    SourceThreadEntry entry;
    void* context;
    void* argument;
    int started, cancelled, joined;
#ifdef __3DS__
    Thread thread;
    LightEvent start_event;
#else
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t start_event;
#endif
};

#if !defined(__3DS__) && !defined(__APPLE__)
static pthread_t controller;
__attribute__((constructor(101))) static void remember_controller(void) {
    controller = pthread_self();
}
#endif
int source_thread_is_controller(void) {
#ifdef __3DS__
    return threadGetCurrent() == NULL;
#elif defined(__APPLE__)
    return pthread_main_np();
#else
    return pthread_equal(controller, pthread_self());
#endif
}

static void run_thread(SourceThreadOS* t) {
#ifdef __3DS__
    LightEvent_Wait(&t->start_event);
#else
    pthread_mutex_lock(&t->mutex);
    while (!t->started && !t->cancelled)
        pthread_cond_wait(&t->start_event, &t->mutex);
    pthread_mutex_unlock(&t->mutex);
#endif
    if (!t->cancelled) t->entry(t->context, t->argument);
}
#ifdef __3DS__
static void os_entry(void* value) { run_thread(value); }
#else
static void* os_entry(void* value) { run_thread(value); return NULL; }
#endif

SourceThreadOS* source_thread_create(SourceThreadEntry entry, void* context,
                                    size_t stack_size, int priority, uint32_t* os_id) {
    if (!source_thread_is_controller()) { errno = EPERM; return NULL; }
    if (!entry || !os_id || stack_size < 4096 || stack_size > 1024 * 1024) {
        errno = EINVAL; return NULL;
    }
    SourceThreadOS* t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->entry = entry;
    t->context = context;
#ifdef __3DS__
    LightEvent_Init(&t->start_event, RESET_STICKY);
    // Original PS2 priorities 10/11 map around libctru main priority 0x30,
    // never outside the SDK application range 0x18..0x3f. Use app core 0.
    int sdk_priority = priority < -13 ? 0x18 : priority > 26 ? 0x3f : priority + 37;
    t->thread = threadCreate(os_entry, t, stack_size, sdk_priority, 0, false);
    if (!t->thread) { free(t); errno = EAGAIN; return NULL; }
    *os_id = threadGetHandle(t->thread);
#else
    (void)priority;
    int rc = pthread_mutex_init(&t->mutex, NULL);
    if (rc) { free(t); errno = rc; return NULL; }
    rc = pthread_cond_init(&t->start_event, NULL);
    if (rc) { pthread_mutex_destroy(&t->mutex); free(t); errno = rc; return NULL; }
    pthread_attr_t attr;
    rc = pthread_attr_init(&attr);
    if (!rc) {
        size_t actual = stack_size < PTHREAD_STACK_MIN ? PTHREAD_STACK_MIN : stack_size;
        rc = pthread_attr_setstacksize(&attr, actual);
        if (!rc) rc = pthread_create(&t->thread, &attr, os_entry, t);
        pthread_attr_destroy(&attr);
    }
    if (rc) {
        pthread_cond_destroy(&t->start_event);
        pthread_mutex_destroy(&t->mutex);
        free(t); errno = rc; return NULL;
    }
#ifdef __APPLE__
    *os_id = pthread_mach_thread_np(t->thread);
#else
    // No portable integer pthread ID exists. This field is diagnostic only on
    // POSIX test hosts; the opaque owner retains the actual pthread_t.
    *os_id = 0;
#endif
#endif
    return t;
}

int source_thread_start(SourceThreadOS* t, void* argument) {
    if (!source_thread_is_controller()) { errno = EPERM; return -1; }
    if (!t || t->started || t->cancelled || t->joined) { errno = EINVAL; return -1; }
#ifndef __3DS__
    pthread_mutex_lock(&t->mutex);
#endif
    t->argument = argument;
    t->started = 1;
#ifdef __3DS__
    LightEvent_Signal(&t->start_event);
#else
    pthread_cond_signal(&t->start_event);
    pthread_mutex_unlock(&t->mutex);
#endif
    return 0;
}
int source_thread_join(SourceThreadOS* t) {
    if (!source_thread_is_controller()) { errno = EPERM; return -1; }
    if (!t || (!t->started && !t->cancelled)) { errno = EINVAL; return -1; }
    if (t->joined) return 0;
#ifdef __3DS__
    Result rc = threadJoin(t->thread, UINT64_MAX);
    if (R_FAILED(rc)) { errno = EIO; return -1; }
#else
    int rc = pthread_join(t->thread, NULL);
    if (rc) { errno = rc; return -1; }
#endif
    t->joined = 1;
    return 0;
}
int source_thread_destroy(SourceThreadOS* t) {
    if (!source_thread_is_controller()) { errno = EPERM; return -1; }
    if (!t) { errno = EINVAL; return -1; }
    if (!t->started && !t->cancelled) {
#ifndef __3DS__
        pthread_mutex_lock(&t->mutex);
#endif
        t->cancelled = 1;
#ifdef __3DS__
        LightEvent_Signal(&t->start_event);
#else
        pthread_cond_signal(&t->start_event);
        pthread_mutex_unlock(&t->mutex);
#endif
    }
    if (source_thread_join(t)) return -1;
#ifdef __3DS__
    threadFree(t->thread);
#else
    pthread_cond_destroy(&t->start_event);
    pthread_mutex_destroy(&t->mutex);
#endif
    free(t);
    return 0;
}

void source_thread_sleep_ms(unsigned milliseconds) {
#ifdef __3DS__
    svcSleepThread((int64_t)milliseconds * 1000000);
#else
    struct timespec remaining = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (nanosleep(&remaining, &remaining) && errno == EINTR) {}
#endif
}

/* Single in-flight CPU job. The controller keeps inputs and scratch alive until wait.
 * No engine allocators, ownership changes or graphics calls run on this worker. */
#ifdef __3DS__
typedef LightEvent FrameEvent;
static int frame_event_init(FrameEvent* e) { LightEvent_Init(e, RESET_ONESHOT); return 0; }
static void frame_signal(FrameEvent* e) { LightEvent_Signal(e); }
static void frame_wait(FrameEvent* e) { LightEvent_Wait(e); }
static void frame_event_close(FrameEvent* e) { (void)e; }
#else
typedef struct { pthread_mutex_t lock; pthread_cond_t wake; int set; } FrameEvent;
static int frame_event_init(FrameEvent* e) {
    if (pthread_mutex_init(&e->lock, NULL)) return -1;
    if (!pthread_cond_init(&e->wake, NULL)) return 0;
    pthread_mutex_destroy(&e->lock);
    return -1;
}
static void frame_signal(FrameEvent* e) {
    pthread_mutex_lock(&e->lock);
    e->set=1;
    pthread_cond_signal(&e->wake);
    pthread_mutex_unlock(&e->lock);
}
static void frame_wait(FrameEvent* e) {
    pthread_mutex_lock(&e->lock);
    while (!e->set) pthread_cond_wait(&e->wake, &e->lock);
    e->set=0;
    pthread_mutex_unlock(&e->lock);
}
static void frame_event_close(FrameEvent* e) {
    pthread_cond_destroy(&e->wake);
    pthread_mutex_destroy(&e->lock);
}
#endif
struct SourceFrameWorker {
    FrameEvent work, done;
    void (*job)(void*);
    void* input;
    int core, busy;
#ifdef __3DS__
    Thread thread;
#else
    pthread_t thread;
#endif
};
static void frame_worker_loop(void* arg) {
    SourceFrameWorker* w=arg;
#ifdef __3DS__
    // threadCreate accepted a single fixed affinity; Azahar 2126.2 does
    // not implement the optional GetCurrentProcessorNumber diagnostic SVC.
    w->core=2;
#else
    w->core=-1;
#endif
    frame_signal(&w->done);
    for (;;) {
        frame_wait(&w->work);
        if (!w->job) break;
        w->job(w->input);
        frame_signal(&w->done);
    }
}
#ifndef __3DS__
static void* frame_worker_host(void* arg) { frame_worker_loop(arg); return NULL; }
#endif
SourceFrameWorker* source_frame_worker_create(void) {
    SourceFrameWorker* w=calloc(1,sizeof(*w));
    if (!w) return NULL;
    if (frame_event_init(&w->work)) goto free_worker;
    if (frame_event_init(&w->done)) goto close_work;
#ifdef __3DS__
    w->thread=threadCreate(frame_worker_loop,w,32768,0x30,2,false);
    if (!w->thread) goto close_done;
#else
    if (pthread_create(&w->thread,NULL,frame_worker_host,w)) goto close_done;
#endif
    frame_wait(&w->done);
    return w;
close_done:
    frame_event_close(&w->done);
close_work:
    frame_event_close(&w->work);
free_worker:
    free(w);
    return NULL;
}
int source_frame_worker_core(const SourceFrameWorker* w) { return w->core; }
void source_frame_worker_submit(SourceFrameWorker* w, void (*job)(void*), void* input) {
    w->job=job;
    w->input=input;
    w->busy=1;
    frame_signal(&w->work);
}
void source_frame_worker_wait(SourceFrameWorker* w) {
    if (w->busy) { frame_wait(&w->done); w->busy=0; }
}
void source_frame_worker_destroy(SourceFrameWorker* w) {
    if (!w) return;
    source_frame_worker_wait(w);
    w->job=NULL;
    frame_signal(&w->work);
#ifdef __3DS__
    threadJoin(w->thread,UINT64_MAX);
    threadFree(w->thread);
#else
    pthread_join(w->thread,NULL);
#endif
    frame_event_close(&w->done);
    frame_event_close(&w->work);
    free(w);
}
