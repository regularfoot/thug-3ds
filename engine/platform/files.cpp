#include <core/defines.h>
#include <sys/file/filesys.h>
#include <sys/file/pre.h>
#include <sys/file/asyncfilesys.h>
#include <source_port/files.h>
#include <source_port/memory.h>
#include <source_port/application_os.h>
#include <source_port/diagnostics.h>
#include <source_port/thread_os.h>
#include <source_port/native_loadscreen.h>
#include <source_port/logic_timing.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <cctype>
#include <string>
#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <vector>
#ifdef __3DS__
extern "C" {
#include <3ds/types.h>
#include <3ds/allocator/linear.h>
}
#endif

namespace {
constexpr std::size_t max_path = 1024;
char data_root[max_path] = "data", write_root[max_path] = "";
bool installed = false;
struct DiskFile {
    FILE* stream;
    std::uintptr_t id;
    bool writing;
    char name[128];
    long slice_start = 0;
    long slice_size = -1;
    char archive[128]{};
};
DiskFile files[64]{};
// Idle streams only. Concurrent logical members still own separate FILEs.
struct IdleArchive { FILE* stream = nullptr; char path[128]{}; };
std::array<IdleArchive, 4> idle_archives{};
struct DiskPreEntry {
    char name[128];
    std::uint32_t offset;
    std::uint32_t size;
};
static_assert(sizeof(DiskPreEntry) == 136);
struct DiskPre {
    std::string name;
    std::string path;
    std::vector<DiskPreEntry> entries;
};
std::vector<DiskPre> disk_pres;
std::uintptr_t next_id = 1;
struct DiskTiming {
    const char* operation;
    const char* name;
    const std::uint64_t started;
    std::size_t bytes;
    DiskTiming(const char* operation,const char* name,std::size_t bytes=0):
        operation(operation),name(name),started(SourcePort::logic_timing_active?source_clock_now():0),bytes(bytes) {}
    ~DiskTiming() {
        if(!started)return;
        const auto elapsed=source_clock_now()-started;
        const auto frequency=source_clock_frequency();
        if(elapsed>=frequency/20) {
            const int saved_errno=errno;
            SourcePort::Log("DISK_STALL operation=%s ms=%llu bytes=%zu name=%s",
                operation,static_cast<unsigned long long>(elapsed*1000/frequency),bytes,name);
            errno=saved_errno;
        }
    }
};
void controller() { Dbg_Assert(source_thread_is_controller()); }
bool fail(int error) { errno = error; return false; }
bool normalized(const char* name, std::string& out) {
    if (!name || !*name || std::strlen(name) >= 128) return fail(ENAMETOOLONG);
    out = name;
    std::replace(out.begin(), out.end(), '\\', '/');
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    constexpr char legacy[] = "c:/skate4/data/";
    if (out.compare(0, sizeof(legacy) - 1, legacy) == 0) out.erase(0, sizeof(legacy) - 1);
    if (out.empty() || out.front() == '/' || out.find(':') != std::string::npos) return fail(EINVAL);
    std::size_t at = 0;
    while (at < out.size()) {
        auto slash = out.find('/', at);
        auto part = out.substr(at, slash == std::string::npos ? slash : slash - at);
        if (part.empty() || part == "." || part == "..") return fail(EINVAL);
        if (slash == std::string::npos) break;
        at = slash + 1;
        if (at == out.size()) return fail(EINVAL);
    }
    // Actual PC retail resource spelling, also used by the original PC owner.
    if (out.size() >= 4 && out.compare(out.size() - 4, 4, ".xbx") == 0) out.resize(out.size() - 4);
    return true;
}
bool resolve(const char* root, const std::string& name, char* out, std::size_t cap, bool writing) {
    std::string path(root);
    std::size_t at = 0;
    while (at < name.size()) {
        auto slash = name.find('/', at);
        auto part = name.substr(at, slash == std::string::npos ? slash : slash - at);
        std::string candidate = path + "/" + part;
        struct stat info{};
        if (stat(candidate.c_str(), &info) != 0) {
            DIR* dir = opendir(path.c_str());
            if (!dir) return false;
            bool found = false;
            while (auto* entry = readdir(dir)) {
                if (strcasecmp(part.c_str(), entry->d_name) == 0) {
                    candidate = path + "/" + entry->d_name; found = true; break;
                }
            }
            closedir(dir);
            if (!found && !(writing && slash == std::string::npos)) return fail(ENOENT);
        }
        path = candidate;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    if (!out || path.size() >= cap) return fail(ENAMETOOLONG);
    std::memcpy(out, path.c_str(), path.size() + 1);
    return true;
}
bool resolve_resource(const std::string& relative,char* out,std::size_t cap){
    if(resolve(data_root,relative,out,cap,false))return true;
    // The installed retail game contains model .tex files. Some PC data sets
    // also use .txx. Prefer the real requested resource; never rewrite every
    // .tex name to .txx before checking the disk.
    if(errno==ENOENT&&relative.size()>=4&&relative.compare(relative.size()-4,4,".tex")==0){
        auto alternate=relative;alternate.replace(alternate.size()-4,4,".txx");return resolve(data_root,alternate,out,cap,false);
    }return false;
}
FILE* open_resource(const std::string& relative,const char* mode){
    // FAT and most installed Data directories resolve the complete relative
    // path directly. Only case-sensitive hosts and legacy aliases need a walk.
    std::string path=std::string(data_root)+"/"+relative;
    if(auto* file=std::fopen(path.c_str(),mode))return file;
    if(errno!=ENOENT&&errno!=ENOTDIR)return nullptr;
    char resolved[max_path];
    if(!resolve_resource(relative,resolved,sizeof(resolved)))return nullptr;
    return std::fopen(resolved,mode);
}
FILE* take_idle_archive(const char* path) {
    for (auto& idle : idle_archives) {
        if (!idle.stream || std::strcmp(path, idle.path) != 0) continue;
        FILE* stream = idle.stream;
        idle = {};
        return stream;
    }
    return nullptr;
}
void close_idle_archives(const char* path = nullptr) {
    for (auto& idle : idle_archives) {
        if (!idle.stream || (path && std::strcmp(path, idle.path) != 0)) continue;
        std::fclose(idle.stream);
        idle = {};
    }
}
DiskPre* disk_pre(const char* archive) {
    if (!archive) return nullptr;
    for (auto& pre : disk_pres)
        if (!strcasecmp(pre.name.c_str(), archive)) return &pre;
    return nullptr;
}
const DiskPreEntry* disk_pre_member(const std::string& name, const char*& archive_path, bool for_streaming=false) {
    for (auto& pre : disk_pres) {
        // Pedestrian assets share an archive for cheaper reads during load.
        // Texture payloads remain in RAM as they did with loose files.
        if(for_streaming&&pre.name=="npcparts.pre")continue;
        const auto at = std::lower_bound(pre.entries.begin(), pre.entries.end(), name,
            [](const auto& entry, const auto& key) { return std::string_view(entry.name) < key; });
        if (at != pre.entries.end() && name == at->name) {
            archive_path = pre.path.c_str();
            return &*at;
        }
    }
    return nullptr;
}
DiskFile* disk(void* handle) {
    auto id = reinterpret_cast<std::uintptr_t>(handle);
    for (auto& slot : files) if (slot.stream && slot.id == id) return &slot;
    return nullptr;
}
bool transfer_size(std::size_t size, std::size_t count, std::size_t& bytes) {
    if (size && count > std::size_t(INT_MAX) / size) return fail(EOVERFLOW);
    bytes = size * count;
    return true;
}
std::size_t read_disk(void* out,std::size_t bytes,FILE* file){
    std::size_t total=0;while(total<bytes){auto part=std::min<std::size_t>(bytes-total,65536);auto got=std::fread(static_cast<uint8*>(out)+total,1,part,file);total+=got;SourcePort::PumpLoadingScreen();if(got!=part)break;}return total;
}
bool valid_pre_name(const char* name) {
    return name && std::strlen(name) < 128;
}
#ifdef __3DS__
uint8* try_linear_archive(std::size_t size) {
    const std::size_t gpu_reserve =
        (source_application_os_is_new_3ds() ? 8u : 3u) * 1024u * 1024u;
    const auto free = linearSpaceFree();
    return free > gpu_reserve && size <= free - gpu_reserve
        ? static_cast<uint8*>(linearAlloc(size)) : nullptr;
}
#endif
File::PreFile::FileHandle* pre(void* handle) {
    if (!handle || !File::PreMgr::sPreEnabled()) { errno = EBADF; return nullptr; }
    auto* value = static_cast<File::PreFile::FileHandle*>(handle);
    (void)File::PreMgr::pre_get_file_size(value);
    if (!File::PreMgr::sPreExecuteSuccess()) { errno = EBADF; return nullptr; }
    return value;
}
}
namespace SourcePort {
bool FindDiskResourceSlice(const char* name, DiskResourceSlice& slice) {
    controller();
    if (File::PreMgr::sPreEnabled() && File::PreMgr::Instance()->FindFileEntry(name)) return false;
    std::string relative;
    const char* path = nullptr;
    if (!normalized(name, relative)) return false;
    if (const auto* member = disk_pre_member(relative, path, true)) {
        slice = {path, member->offset, member->size};
        return true;
    }
    return false;
}

bool MountDiskPre(const char* archive) {
    if (!archive) return false;
    if (disk_pre(archive)) return true;
    RootHeapScope storage;
    std::string name;
    if (!normalized(archive, name) || name.size() <= 4 || name.find('/') != std::string::npos ||
        name.compare(name.size() - 4, 4, ".pre") != 0) return false;
    // New 3DS has enough memory for the ordinary level and sound PREs.
    // Stream the three largest scene PREs to avoid retaining another
    // complete copy of their geometry and texture dictionary.
    if (source_application_os_is_new_3ds() &&
        name != "ruscn.pre" && name != "nyscn.pre" && name != "njscn.pre" && name != "npcparts.pre") return false;
    const auto path = "pre/lowmem/" + name.substr(0, name.size() - 4) + ".pak";
    std::unique_ptr<FILE, decltype(&std::fclose)> file(open_resource(path, "rb"), std::fclose);
    if (!file) return false;
    std::array<std::uint32_t, 2> header{};
    struct stat info{};
    Dbg_MsgAssert(fstat(fileno(file.get()), &info) == 0 && info.st_size > 0 && info.st_size <= INT_MAX &&
        std::fread(header.data(), sizeof(header), 1, file.get()) == 1 &&
        header[0] == 0x314d5044 && header[1] > 0 && header[1] <= 2048,
        ("Invalid low-memory PRE index: %s", path.c_str()));
    std::vector<DiskPreEntry> entries(header[1]);
    const auto data_start = sizeof(header) + entries.size() * sizeof(DiskPreEntry);
    Dbg_MsgAssert(std::fread(entries.data(), sizeof(DiskPreEntry), entries.size(), file.get()) == entries.size(),
                  ("Truncated low-memory PRE index: %s", path.c_str()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        Dbg_MsgAssert(std::memchr(entry.name, 0, sizeof(entry.name)) && entry.name[0] &&
            entry.offset >= data_start && std::uint64_t(entry.offset) + entry.size <= std::uint64_t(info.st_size) &&
            (i == 0 || std::strcmp(entries[i - 1].name, entry.name) < 0),
            ("Invalid low-memory PRE member: %s entry %u", path.c_str(), unsigned(i)));
    }
    Log("DISK_PRE name=%s entries=%zu index_bytes=%zu", archive, entries.size(), data_start);
#ifdef __3DS__
    BootPhase(archive);
#endif
    disk_pres.push_back({std::move(name), path, std::move(entries)});
    return true;
}

bool DiskPreMounted(const char* archive) {
    const auto* pre = disk_pre(archive);
    return pre && !pre->entries.empty();
}

bool UnmountDiskPre(const char* archive) {
    if (!archive) return false;
    const auto at = std::find_if(disk_pres.begin(), disk_pres.end(), [archive](const auto& pre) {
        return !strcasecmp(pre.name.c_str(), archive);
    });
    if (at == disk_pres.end()) return false;
    close_idle_archives(at->path.c_str());
    disk_pres.erase(at);
    return true;
}

unsigned char* AllocatePreBuffer(const char* archive, std::size_t size, bool bottom_up) {
    auto& memory = Mem::Manager::sHandle();
    auto* heap = bottom_up ? memory.BottomUpHeap() : memory.TopDownHeap();
    const bool skater_parts = strcasecmp(archive, "skaterparts.pre") == 0;
    constexpr char scene_suffix[] = "scn.pre";
    const auto name_size = std::strlen(archive);
    const bool large_scene = size >= 2u * 1024u * 1024u && name_size > sizeof(scene_suffix) - 1 &&
        strcasecmp(archive + name_size - (sizeof(scene_suffix) - 1), scene_suffix) == 0;
    bool low_arena_headroom = false;
#ifdef __3DS__
    if (!source_application_os_is_new_3ds()) {
        constexpr std::size_t minimum_spill_size = 1024u * 1024u;
        constexpr std::size_t runtime_headroom = 12u * 1024u * 1024u;
        // Keep room in the original arena for models, textures and scripts
        // expanded later in the load. Use the same lifetime as any other PRE.
        const auto free = static_cast<std::size_t>(std::max(0, heap->ParentRegion()->MemAvailable()));
        low_arena_headroom = size >= minimum_spill_size && free < runtime_headroom + size;
    }
#endif
    if (!skater_parts && !large_scene && !low_arena_headroom) return new (heap) uint8[size];

    if (skater_parts && !low_arena_headroom) {
        if (auto* buffer = memory.New(size, false, heap)) return static_cast<uint8*>(buffer);
    }

    // Give large scene archives their own storage so the original arena
    // can hold the decoded level. Leave room for the GPU's linear buffers.
    uint8* buffer = nullptr;
#ifdef __3DS__
    buffer = try_linear_archive(size);
#else
    buffer = static_cast<uint8*>(std::malloc(size));
#endif
    if (buffer) {
        Log(skater_parts ? "LOAD_PRE_SPILL name=%s bytes=%zu" : "LOAD_PRE_LINEAR name=%s bytes=%zu", archive, size);
        return buffer;
    }
    return new (heap) uint8[size];
}

void* AllocatePipPreBuffer(const char* archive, std::size_t size, bool expanded) {
#ifdef __3DS__
    constexpr std::size_t large_archive = 2u * 1024u * 1024u;
    if (!source_application_os_is_new_3ds() && size >= large_archive) {
        if (auto* buffer = try_linear_archive(size)) {
            Log("PIP_PRE_LINEAR phase=%s bytes=%zu name=%s", expanded ? "expanded" : "input", size, archive);
            return buffer;
        }
    }
#else
    (void)archive;
    (void)expanded;
#endif
    return Mem::Malloc(size);
}

char* LoadPreparedPipPre(const char* archive) {
#ifdef __3DS__
    if (source_application_os_is_new_3ds() || strcasecmp(archive, "unloadableanims.pre")) return nullptr;
    constexpr char path[] = "pre/lowmem/unloadableanims.apre";
    std::unique_ptr<FILE, decltype(&std::fclose)> file(open_resource(path, "rb"), std::fclose);
    if (!file) return nullptr;
    struct stat info{};
    Dbg_MsgAssert(fstat(fileno(file.get()), &info) == 0 &&
        info.st_size > 12 && info.st_size < 4 * 1024 * 1024,
        ("Invalid prepared animation PRE size"));
    const std::size_t length = info.st_size;
    const std::size_t prefix = (std::strlen(archive) + 16) & ~std::size_t(15);
    auto* buffer = static_cast<uint8*>(linearAlloc(prefix + length));
    const bool linear = buffer != nullptr;
    BootPhase(linear ? "aligned_animation_linear" : "aligned_animation_root");
    if (!buffer) buffer = static_cast<uint8*>(Mem::Malloc(prefix + length));
    std::memcpy(buffer, archive, std::strlen(archive) + 1);
    auto* data = buffer + prefix;
    Dbg_MsgAssert(read_disk(data, length, file.get()) == length, ("Truncated prepared animation PRE"));
    const auto word = [data](std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };
    const std::uint32_t entries = word(8);
    Dbg_MsgAssert(word(0) == length && word(4) == 0xabcd0003 && entries > 0 && entries <= 2048,
        ("Invalid prepared animation PRE header"));
    std::size_t at = 12;
    for (std::uint32_t i = 0; i < entries; ++i) {
        Dbg_Assert(at + 16 <= length);
        const std::uint32_t decoded_size = word(at);
        const std::uint32_t name_size = word(at + 8);
        Dbg_Assert(word(at + 4) == 0 && name_size > 0 && name_size < 128 &&
            at + 16 + name_size <= length && std::memchr(data + at + 16, 0, name_size));
        const std::size_t start = (at + 16 + name_size + 15) & ~std::size_t(15);
        Dbg_Assert(start <= length && decoded_size <= length - start);
        at = start + ((std::size_t(decoded_size) + 3) & ~std::size_t(3));
    }
    Dbg_Assert(at == length);
    Log("PIP_PRE_PREPARED name=%s bytes=%zu linear=%u", archive, prefix + length, unsigned(linear));
    BootPhase("aligned_animation_ready");
    CheckpointDiagnosticLog();
    return reinterpret_cast<char*>(buffer);
#else
    (void)archive;
    return nullptr;
#endif
}

void FreePreBuffer(unsigned char* buffer) {
    if (!buffer) return;
    if (IsEngineAddress(buffer)) {
        Mem::Manager::sHandle().Delete(buffer);
        return;
    }
#ifdef __3DS__
    linearFree(buffer);
#else
    std::free(buffer);
#endif
}

bool ConfigureFileRoots(const char* resource, const char* writes) {
    controller();
    if (installed) return fail(EBUSY);
    if (!resource || !*resource || std::strlen(resource) >= max_path || (writes && std::strlen(writes) >= max_path)) return fail(EINVAL);
    std::strcpy(data_root, resource); std::strcpy(write_root, writes ? writes : "");
    return true;
}
bool ResolveResourcePath(const char* name, char* out, std::size_t cap) {
    controller(); std::string relative;
    return normalized(name, relative) && resolve_resource(relative, out, cap);
}
bool FilesInstalled() { controller(); return installed; }
unsigned OpenDiskFiles() { controller(); unsigned count = 0; for (auto& f : files) count += f.stream != nullptr; return count; }
FileSystemSession::FileSystemSession() { File::InstallFileSystem(); }
FileSystemSession::~FileSystemSession() { File::UninstallFileSystem(); }
}
namespace File {
void InstallFileSystem() {
    controller(); Dbg_Assert(!installed);
    struct stat info{};
    Dbg_MsgAssert(stat(data_root, &info) == 0 && S_ISDIR(info.st_mode), ("Missing game Data directory: %s", data_root));
    installed = true;
    CAsyncFileLoader::sInit(); // actual original startup responsibility of p_filesys.cpp
    // Loose pedestrian outfits cost a separate FAT directory traversal
    // for each texture, mesh and CAS. The optional archive stores exact files.
    if(source_application_os_is_new_3ds()) SourcePort::MountDiskPre("npcparts.pre");
}
void UninstallFileSystem() {
    controller(); Dbg_Assert(installed);
    if (PreMgr::sPreEnabled()) PreMgr::Instance()->WaitAllLoadPre();
    CAsyncFileLoader::sCleanup();
    Dbg_Assert(!SourcePort::OpenDiskFiles());
    close_idle_archives();
    std::vector<DiskPre>().swap(disk_pres);
    installed = false;
}
void InitQuickFileSystem() { controller(); Dbg_Assert(installed); }
void ResetQuickFileSystem() { controller(); Dbg_Assert(installed); }
void StopStreaming() { controller(); Dbg_Assert(installed); /* disk owner has no autonomous streaming worker */ }
bool Exist(const char* name) {
    controller(); Dbg_Assert(installed);
    if (!valid_pre_name(name)) return fail(ENAMETOOLONG);
    if (PreMgr::sPreEnabled() && PreMgr::pre_fexist(name)) return true;
    std::string relative;
    const char* archive_path = nullptr;
    char path[max_path];
    return normalized(name, relative) &&
        (disk_pre_member(relative, archive_path) || resolve_resource(relative, path, sizeof(path)));
}
void* Open(const char* name, const char* mode) {
    controller(); Dbg_Assert(installed);
    if (!mode || !valid_pre_name(name)) { errno = EINVAL; return nullptr; }
    bool writing = std::strpbrk(mode, "wa+") != nullptr;
    if (writing && !*write_root) { errno = EROFS; return nullptr; }
    if (!writing && PreMgr::sPreEnabled()) {
        auto* found = PreMgr::pre_fopen(name, mode);
        if (PreMgr::sPreExecuteSuccess()) return found;
    }
    std::string relative; char path[max_path];
    if (!normalized(name, relative))return nullptr;
    DiskTiming timing("open",name);
    if(writing&&!resolve(write_root,relative,path,sizeof(path),true))return nullptr;
    const char* archive_path = nullptr;
    const auto* member = writing ? nullptr : disk_pre_member(relative, archive_path);
    if (!writing && !member) {
        const auto archive=std::find_if(disk_pres.begin(),disk_pres.end(),[&](const auto& pre){return pre.path==relative;});
        if (archive!=disk_pres.end()) archive_path=archive->path.c_str();
    }
    for (auto& slot : files) if (!slot.stream) {
        auto* stream = archive_path ? take_idle_archive(archive_path) : nullptr;
        if (!stream) stream = writing ? std::fopen(path, mode) : open_resource(member ? archive_path : relative, mode);
        if (!stream) return nullptr;
        const long begin = member ? long(member->offset) : 0;
        if (archive_path && std::ftell(stream) != begin && std::fseek(stream, begin, SEEK_SET)) {
            std::fclose(stream);
            return nullptr;
        }
        Dbg_Assert(next_id && next_id < UINTPTR_MAX);
        slot = {stream,next_id++,writing,{}};
        if (member) {
            slot.slice_start = member->offset;
            slot.slice_size = member->size;
        }
        if (archive_path) {
            Dbg_Assert(std::strlen(archive_path) < sizeof(slot.archive));
            std::strcpy(slot.archive, archive_path);
        }
        std::strcpy(slot.name,relative.c_str());
        return reinterpret_cast<void*>(slot.id);
    }
    errno = EMFILE; return nullptr;
}
int Close(void* handle) {
    controller();
    if (auto* f = disk(handle)) {
        if (f->archive[0] && !std::ferror(f->stream)) {
            const bool mounted = std::any_of(disk_pres.begin(), disk_pres.end(),
                [f](const auto& archive) { return archive.path == f->archive; });
            if (mounted) {
                // Replace the older idle stream of this archive, if any.
                close_idle_archives(f->archive);
                for (auto& idle : idle_archives) {
                    if (idle.stream) continue;
                    std::clearerr(f->stream);
                    idle.stream = f->stream;
                    std::strcpy(idle.path, f->archive);
                    *f = {};
                    return 0;
                }
            }
        }
        int result = std::fclose(f->stream);
        *f = {};
        return result;
    }
    auto* p = pre(handle); return p ? PreMgr::pre_fclose(p) : -1;
}
long GetFileSize(void* handle) {
    controller();
    if (auto* f = disk(handle)) {
        if (f->slice_size >= 0) return f->slice_size;
        struct stat info{};
        if(!f->writing&&fstat(fileno(f->stream),&info)==0&&S_ISREG(info.st_mode)){
            if(info.st_size>INT_MAX){errno=EOVERFLOW;return -1;}
            return long(info.st_size);
        }
        long position = std::ftell(f->stream);
        if (position < 0 || std::fseek(f->stream, 0, SEEK_END)) return -1;
        long size = std::ftell(f->stream);
        if (std::fseek(f->stream, position, SEEK_SET)) return -1;
        if (size > INT_MAX) { errno = EOVERFLOW; return -1; }
        return size;
    }
    auto* p = pre(handle); return p ? PreMgr::pre_get_file_size(p) : -1;
}
long GetFilePosition(void* handle) {
    controller();
    if (auto* f = disk(handle)) {
        const long position = std::ftell(f->stream);
        return position < 0 ? position : position - f->slice_start;
    }
    auto* p = pre(handle); return p ? PreMgr::pre_get_file_position(p) : -1;
}
float GetPercentageRead(void* handle) { auto size = GetFileSize(handle); return size > 0 ? float(GetFilePosition(handle)) / float(size) * 100.0f : 0.0f; }
size_t ReadNoPre(void* out, size_t size, size_t count, void* handle) {
    controller(); std::size_t bytes;
    auto* f = disk(handle);
    if (!f) { errno = EBADF; return 0; }
    if (!transfer_size(size, count, bytes) || (!out && bytes)) return 0;
    if (f->slice_size >= 0) {
        const long position = GetFilePosition(handle);
        if (position < 0 || position > f->slice_size) return 0;
        bytes = std::min(bytes, std::size_t(f->slice_size - position));
    }
    DiskTiming timing("read",f->name,bytes);
    return bytes ? read_disk(out,bytes,f->stream) : 0; // original File API returns BYTES
}
size_t Read(void* out, size_t size, size_t count, void* handle) {
    controller(); if (disk(handle)) return ReadNoPre(out, size, count, handle);
    std::size_t bytes;
    if (!transfer_size(size, count, bytes) || (!out && bytes)) return 0;
    auto* p = pre(handle); return p && bytes ? PreMgr::pre_fread(out, 1, bytes, p) : 0;
}
size_t ReadInt(void* out, void* handle) { return Read(out, 4, 1, handle); }
size_t Write(const void* in, size_t size, size_t count, void* handle) {
    controller(); std::size_t bytes; auto* f = disk(handle);
    if (!f) { errno = EBADF; return 0; }
    if (!transfer_size(size, count, bytes) || (!in && bytes)) return 0;
    return bytes ? std::fwrite(in, 1, bytes, f->stream) : 0;
}
char* GetS(char* out, int max, void* handle) {
    controller(); if (!out || max <= 0) { errno = EINVAL; return nullptr; }
    if (auto* f = disk(handle)) {
        if (f->slice_size < 0) return std::fgets(out, max, f->stream);
    } else if (!pre(handle)) return nullptr;
    int at = 0;
    while (at < max - 1) { char c; if (Read(&c, 1, 1, handle) != 1) break; out[at++] = c; if (c == '\n') break; }
    out[at] = 0; return at ? out : nullptr;
}
int PutS(const char* text, void* handle) { controller(); auto* f = disk(handle); if (!f || !text) { errno = EBADF; return EOF; } return std::fputs(text, f->stream); }
int Eof(void* handle) {
    controller();
    if (auto* f = disk(handle)) {
        return f->slice_size >= 0 ? GetFilePosition(handle) >= f->slice_size : std::feof(f->stream);
    }
    auto* p = pre(handle);
    return p ? PreMgr::pre_feof(p) : -1;
}
int Seek(void* handle, long offset, int origin) {
    controller();
    if (auto* f = disk(handle)) {
        DiskTiming timing("seek",f->name);
        if (f->slice_size >= 0) {
            long base = 0;
            switch (origin) {
                case SEEK_SET: break;
                case SEEK_CUR: base = GetFilePosition(handle); break;
                case SEEK_END: base = f->slice_size; break;
                default: return -1;
            }
            if (base < 0 || offset < -base || offset > f->slice_size - base) return -1;
            return std::fseek(f->stream, f->slice_start + base + offset, SEEK_SET);
        }
        return std::fseek(f->stream, offset, origin);
    }
    auto* p = pre(handle); return p ? PreMgr::pre_fseek(p, offset, origin) : -1;
}
int Tell(void* handle) { auto at = GetFilePosition(handle); if (at > INT_MAX) { errno = EOVERFLOW; return -1; } return int(at); }
int Flush(void* handle) { controller(); if (auto* f = disk(handle)) return std::fflush(f->stream); auto* p = pre(handle); return p ? PreMgr::pre_fflush(p) : -1; }
uint32 CanFileBeLoadedQuickly(const char* name) {
    std::string relative;
    const char* archive_path = nullptr;
    if (normalized(name, relative) && disk_pre_member(relative, archive_path)) return 0;
    char path[max_path];
    if (!SourcePort::ResolveResourcePath(name, path, sizeof(path))) return 0;
    struct stat info{};
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > INT_MAX) return 0;
    return uint32(info.st_size);
}
bool LoadFileQuicklyPlease(const char* name, uint8* out) {
    controller(); Dbg_Assert(installed);
    std::string relative;
    if(!out||!normalized(name,relative))return false;
    DiskTiming timing("load",name);
    FILE* f=open_resource(relative,"rb");
    if(!f)return false;
    struct stat info{};
    bool ok=fstat(fileno(f),&info)==0&&S_ISREG(info.st_mode)&&info.st_size>0&&info.st_size<=INT_MAX;
    if(ok)ok=read_disk(out,std::size_t(info.st_size),f)==std::size_t(info.st_size)&&!std::ferror(f);
    if(std::fclose(f)!=0)ok=false;
    if(ok&&relative.size()>=4&&relative.compare(relative.size()-4,4,".pre")==0)
        ok=SourcePort::ValidatePre(out,std::size_t(info.st_size));
    return ok;
}
void* LoadAllocNoPre(const char* name) {
    std::string relative;
    if(!normalized(name,relative))return nullptr;
    DiskTiming timing("load",name);
    FILE* f=open_resource(relative,"rb");
    if(!f)return nullptr;
    struct stat info{};
    uint8* buffer=nullptr;
    if(fstat(fileno(f),&info)==0&&S_ISREG(info.st_mode)&&info.st_size>0&&info.st_size<=INT_MAX){
        const auto bytes=std::size_t(info.st_size);
        buffer=static_cast<uint8*>(Mem::Malloc(bytes));
        bool ok=read_disk(buffer,bytes,f)==bytes&&!std::ferror(f);
        if(ok&&relative.size()>=4&&relative.compare(relative.size()-4,4,".pre")==0)
            ok=SourcePort::ValidatePre(buffer,bytes);
        if(!ok){Mem::Free(buffer);buffer=nullptr;}
    }
    if(std::fclose(f)!=0&&buffer){Mem::Free(buffer);buffer=nullptr;}
    return buffer;
}
}
