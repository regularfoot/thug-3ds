#include <source_port/media_os.h>
#include <source_port/application_os.h>
#include <source_port/clock_os.h>
#include <source_port/diagnostics.h>
#include <source_port/thread_os.h>
#include "audio_file_stream.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

namespace SourcePort {
namespace {
constexpr unsigned block_frames = 1024;
constexpr unsigned blocks_per_stream = 8;

struct Request {
    char path[1024]{};
    std::uint64_t offset = 0, length = 0;
    unsigned generation = 0;
};

struct Slot {
    Request request;
    AudioFileState state = AudioFileState::Idle;
    unsigned rate = 0;
    unsigned first = 0, size = 0;
    float blocks[blocks_per_stream][block_frames * 2];
    unsigned frames[blocks_per_stream]{};
};

Slot slots[audio_file_slots];
SourceThreadOS* worker = nullptr;
SourceThreadOS* music_worker = nullptr;
struct SlotRange { unsigned first, count; };
SlotRange all_slots{1, audio_file_slots};
SlotRange effect_slots{1, audio_file_slots - 1};
SlotRange music_slot{0, 1};
std::atomic<bool> stopping{false};
std::atomic_flag lock = ATOMIC_FLAG_INIT;

struct QueueGuard {
    QueueGuard() {
        while (lock.test_and_set(std::memory_order_acquire)) source_thread_sleep_ms(1);
    }
    ~QueueGuard() { lock.clear(std::memory_order_release); }
};

// Only the worker touches its FILE, decoder and compressed stream position.
struct Decoder {
    FILE* file = nullptr;
    SourceMediaCodec* codec = nullptr;
    std::int64_t start = 0, length = 0, position = 0;
    unsigned generation = 0;
    std::uint64_t last_delay_log = 0, decoded_frames = 0;
    std::uint64_t file_ticks = 0;

    void Close() {
        source_media_close(codec);
        codec = nullptr;
        if (file) std::fclose(file);
        file = nullptr;
    }
    ~Decoder() { Close(); }

    static int Read(void* owner, std::uint8_t* bytes, int requested) {
        auto& decoder = *static_cast<Decoder*>(owner);
        if (requested < 0) return -EINVAL;
        const auto count = std::min<std::int64_t>(requested, decoder.length - decoder.position);
        if (!count) return 0;
        const auto started = source_clock_now();
        const auto read = std::fread(bytes, 1, count, decoder.file);
        decoder.file_ticks += source_clock_now() - started;
        decoder.position += read;
        return read ? static_cast<int>(read) : -EIO;
    }

    static std::int64_t Seek(void* owner, std::int64_t offset, int origin) {
        auto& decoder = *static_cast<Decoder*>(owner);
        origin &= ~SOURCE_MEDIA_SEEK_FORCE;
        if (origin == SOURCE_MEDIA_SEEK_SIZE) return decoder.length;
        std::int64_t base;
        switch (origin) {
            case SEEK_SET: base = 0; break;
            case SEEK_CUR: base = decoder.position; break;
            case SEEK_END: base = decoder.length; break;
            default: return -EINVAL;
        }
        if (offset < -base || offset > decoder.length - base) return -EINVAL;
        const auto position = base + offset;
        const auto started = source_clock_now();
        const int result = std::fseek(decoder.file, long(decoder.start + position), SEEK_SET);
        decoder.file_ticks += source_clock_now() - started;
        if (result) return -EIO;
        return decoder.position = position;
    }

    bool Open(const Request& request) {
        file = std::fopen(request.path, "rb");
        if (!file || std::fseek(file, 0, SEEK_END)) return false;
        const long size = std::ftell(file);
        if (size <= 0 || request.offset >= static_cast<std::uint64_t>(size)) return false;
        const auto remaining = static_cast<std::uint64_t>(size) - request.offset;
        const auto bytes = request.length ? request.length : remaining;
        if (bytes > remaining || bytes > INT_MAX) return false;
        start = request.offset;
        length = bytes;
        position = 0;
        if (std::fseek(file, long(start), SEEK_SET)) return false;
        codec = source_media_open({this, Read, Seek});
        return codec != nullptr;
    }
};

bool Fill(Slot& slot, Decoder& decoder, float* block) {
    Request request;
    bool opening;
    {
        QueueGuard guard;
        opening = slot.request.generation != decoder.generation;
        if (opening) {
            request = slot.request;
            if (request.path[0]) slot.state = AudioFileState::Opening;
        } else if (!decoder.codec || slot.size == blocks_per_stream) return false;
    }
    const auto started=source_clock_now();
    decoder.file_ticks=0;
    if (opening) {
        decoder.Close();
        decoder.generation = request.generation;
        decoder.last_delay_log = decoder.decoded_frames = 0;
        if (!request.path[0]) return true;
        if (!decoder.Open(request)) decoder.Close();
    }

    const unsigned rate = source_media_rate(decoder.codec);
    const unsigned frames = source_media_read(decoder.codec, block, block_frames);
    const bool failed = !decoder.codec || source_media_failed(decoder.codec);
    const bool ended = source_media_ended(decoder.codec);
    if (failed || ended) decoder.Close();
    {
        QueueGuard guard;
        // A track change may have happened while the SD card was serving a read.
        if (slot.request.generation != decoder.generation) return true;
        if (frames) {
            const unsigned at = (slot.first + slot.size) % blocks_per_stream;
            std::memcpy(slot.blocks[at], block, frames * 2 * sizeof(float));
            slot.frames[at] = frames;
            ++slot.size;
        }
        slot.rate = rate;
        slot.state = failed ? AudioFileState::Failed : ended ? AudioFileState::Ended : AudioFileState::Ready;
    }
    if (&slot == &slots[0]) {
        const auto previous = decoder.decoded_frames;
        decoder.decoded_frames += frames;
        const auto checkpoint = std::uint64_t(rate) * 12;
        if (opening || failed || ended || (previous < checkpoint && decoder.decoded_frames >= checkpoint)) {
            Log("MUSIC_DECODE state=%s frames=%llu rate=%u generation=%u",
                failed ? "failed" : ended ? "end" : opening ? "start" : "playing",
                static_cast<unsigned long long>(decoder.decoded_frames), rate, decoder.generation);
        }
    }
    const auto now=source_clock_now();
    const auto ticks=now-started;
    const auto frequency=source_clock_frequency();
    // A busy GPU or SD card can delay consecutive blocks. Keep the evidence
    // without making the diagnostic journal a second streaming workload.
    if(failed || (ticks>=frequency/20 &&
        (!decoder.last_delay_log || now-decoder.last_delay_log>=2*frequency))){
        decoder.last_delay_log=now;
        Log("AUDIO_IO slot=%u phase=%s ms=%llu frames=%u failed=%u read_seek_ms=%llu",
            unsigned(&slot-slots),opening?"open":"decode",static_cast<unsigned long long>(ticks*1000/frequency),frames,unsigned(failed),
            static_cast<unsigned long long>(decoder.file_ticks*1000/frequency));
    }
    return true;
}

void DecodeAudio(void* context, void*) {
    const auto range = *static_cast<SlotRange*>(context);
    Decoder decoders[audio_file_slots];
    float block[block_frames * 2];
    while (!stopping.load(std::memory_order_acquire)) {
        bool work = false;
        for (unsigned i = 0; i < range.count && !stopping.load(std::memory_order_relaxed); ++i) {
            // Give dialog slots the first read each round.
            const unsigned slot = (i + range.first) % audio_file_slots;
            work |= Fill(slots[slot], decoders[slot], block);
        }
        // Yield to the controller, including while filling the startup buffer.
        source_thread_sleep_ms(work ? 1 : 3);
    }
}
SourceThreadOS* StartDecoder(SlotRange& range) {
    std::uint32_t id;
    auto* thread = source_thread_create(DecodeAudio, &range, 128 * 1024, 10, &id);
    if (thread && source_thread_start(thread, nullptr) != 0) {
        source_thread_destroy(thread);
        return nullptr;
    }
    return thread;
}
}

bool StartAudioFileWorker() {
    if (worker) return true;
    stopping.store(false, std::memory_order_relaxed);
    // A slow seek in the dialogue WAD must not hold up the soundtrack.
    // Keep a single stack on Old 3DS and on systems without a spare thread.
    if (!source_application_os_is_old_3ds()) music_worker = StartDecoder(music_slot);
    worker = StartDecoder(music_worker ? effect_slots : all_slots);
    if (!worker && music_worker) {
        stopping.store(true, std::memory_order_release);
        source_thread_destroy(music_worker);
        music_worker = nullptr;
        stopping.store(false, std::memory_order_relaxed);
        worker = StartDecoder(all_slots);
    }
    return worker != nullptr;
}

void StopAudioFileWorker() {
    if (!worker) return;
    stopping.store(true, std::memory_order_release);
    source_thread_destroy(worker);
    worker = nullptr;
    if (music_worker) {
        source_thread_destroy(music_worker);
        music_worker = nullptr;
    }
    for (auto& slot : slots) {
        slot.request = {};
        slot.state = AudioFileState::Idle;
        slot.rate = slot.first = slot.size = 0;
    }
}

void CancelAudioFile(unsigned which) {
    QueueGuard guard;
    auto& slot = slots[which];
    ++slot.request.generation;
    slot.request.path[0] = 0;
    slot.state = AudioFileState::Idle;
    slot.rate = slot.first = slot.size = 0;
}

bool QueueAudioFile(unsigned which, const char* path, std::uint64_t offset, std::uint64_t length) {
    if (!worker || !path || !*path || std::strlen(path) >= sizeof(Request::path)) return false;
    QueueGuard guard;
    auto& slot = slots[which];
    ++slot.request.generation;
    std::strcpy(slot.request.path, path);
    slot.request.offset = offset;
    slot.request.length = length;
    slot.state = AudioFileState::Pending;
    slot.rate = slot.first = slot.size = 0;
    return true;
}

AudioFileStatus GetAudioFileStatus(unsigned which) {
    QueueGuard guard;
    const auto& slot = slots[which];
    return {slot.state, slot.rate, slot.size};
}

unsigned ReadAudioFileBlock(unsigned which, float* output) {
    QueueGuard guard;
    auto& slot = slots[which];
    if (!slot.size) return 0;
    const unsigned frames = slot.frames[slot.first];
    std::memcpy(output, slot.blocks[slot.first], frames * 2 * sizeof(float));
    slot.first = (slot.first + 1) % blocks_per_stream;
    --slot.size;
    return frames;
}
}
