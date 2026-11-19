// Hardware/storage implementation of the original Mc classes. The original
// McFuncs serializer, checksums, career/CAS data and menus remain game owners.
#include <core/defines.h>
#include <sys/mcman.h>
#include <source_port/save.h>
#include <source_port/thread_os.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <cerrno>
#include <ctime>
#include <string>
#include <algorithm>
namespace {
constexpr char marker_name[] = ".thug-source-owner";
constexpr char marker_data[] = "THUG1-N3DS-SOURCE-ENGINE-SAVES-v1\n";
char root[1024] = ".thug1-source-saves";
bool active = false, ready = false;
std::string current;
unsigned serial = 0;
struct Handle { int fd = -1; std::string final_path, temporary; bool failed = false; };
Handle handles[32];
void controller() { Dbg_Assert(source_thread_is_controller()); }
bool failure(int error) { errno = error; return false; }
int card_error(int error) {
    switch (error) {
        case ENOSPC: return Mc::Card::vINSUFFICIENT_SPACE;
        case ENOENT: case EINVAL: case ENAMETOOLONG: case ELOOP: return Mc::Card::vINVALID_PATH;
        case ENOTEMPTY: case EEXIST: return Mc::Card::vSUBDIR_NOT_EMPTY;
        case EMFILE: case ENFILE: return Mc::Card::vTOO_MANY_OPEN_FILES;
        default: return Mc::Card::vACCESS_ERROR;
    }
}
bool mount_namespace() {
    struct stat st{};
    bool created = false;
    if (lstat(root, &st)) {
        if (errno != ENOENT || mkdir(root, 0700)) return false;
        created = true;
    } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return failure(EINVAL);
    const std::string marker = std::string(root) + "/" + marker_name;
    if (created) {
        int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { rmdir(root); return false; }
        bool ok = write(fd, marker_data, sizeof(marker_data) - 1) == sizeof(marker_data) - 1;
        if (fsync(fd)) ok = false;
        if (close(fd)) ok = false;
        if (!ok) { unlink(marker.c_str()); rmdir(root); return false; }
    }
    // Never adopt an arbitrary old directory, even if empty.
    if (lstat(marker.c_str(), &st) || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) return failure(EPERM);
    int fd = open(marker.c_str(), O_RDONLY); if (fd < 0) return false;
    char text[sizeof(marker_data)]{};
    auto n = read(fd, text, sizeof(text)); close(fd);
    if (n != sizeof(marker_data) - 1 || std::memcmp(text, marker_data, sizeof(marker_data) - 1)) return failure(EPERM);
    return access(root, R_OK | W_OK | X_OK) == 0;
}
bool path(const char* input, std::string& output, bool pattern = false) {
    if (!ready) return failure(ENODEV);
    if (!input || std::strlen(input) > 255) return failure(ENAMETOOLONG);
    std::string name(input); std::replace(name.begin(), name.end(), '\\', '/');
    bool absolute = !name.empty() && name.front() == '/';
    while (!name.empty() && name.front() == '/') name.erase(0, 1);
    if (!absolute && !current.empty()) name = current + (name.empty() ? "" : "/") + name;
    output = root;
    std::size_t at = 0;
    while (at < name.size()) {
        auto slash = name.find('/', at);
        auto part = name.substr(at, slash == std::string::npos ? slash : slash - at);
        if (part.empty() || part == "." || part == ".." || part.front() == '.' || part.find(':') != std::string::npos) return failure(EINVAL);
        const bool wildcard=part.find_first_of("*?[")!=std::string::npos;
        if(wildcard&&(!pattern||slash!=std::string::npos))return failure(EINVAL);
        if (part.size() > Mc::File::vMAX_FILENAME_LEN) return failure(ENAMETOOLONG);
        output += "/" + part;
        // FAT rejects wildcard characters as literal filenames (EINVAL),
        // unlike host lstat's ENOENT. Validate only the real parent here;
        // GetFileList enumerates it and applies fnmatch to the final mask.
        if(wildcard)break;
        struct stat st{};
        if (!lstat(output.c_str(), &st)) {
            if (S_ISLNK(st.st_mode)) return failure(ELOOP);
            if (slash != std::string::npos && !S_ISDIR(st.st_mode)) return failure(ENOTDIR);
        } else if (errno != ENOENT || slash != std::string::npos) return false;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return output.size() < 1024 || failure(ENAMETOOLONG);
}
Handle* handle(int fd) { for (auto& h : handles) if (h.fd == fd && fd >= 0) return &h; errno = EBADF; return nullptr; }
bool busy(const std::string& target) { for (auto& h : handles) if (h.fd >= 0 && h.final_path == target) return true; return false; }
void release(Handle& h, bool abort) {
    if (h.fd >= 0) close(h.fd);
    if (abort && !h.temporary.empty()) unlink(h.temporary.c_str());
    h.fd = -1; h.failed = false;
    std::string().swap(h.final_path); std::string().swap(h.temporary);
}
void date(Mc::DateTime& out, time_t value) {
    tm result{}; localtime_r(&value, &result);
    out.m_Seconds = result.tm_sec; out.m_Minutes = result.tm_min; out.m_Hour = result.tm_hour;
    out.m_Day = result.tm_mday; out.m_Month = result.tm_mon + 1; out.m_Year = result.tm_year + 1900;
}
}
namespace SourcePort {
bool ConfigureSaveRoot(const char* directory) {
    controller();
    if (active) return failure(EBUSY);
    if (!directory || !*directory || std::strlen(directory) >= sizeof(root)) return failure(EINVAL);
    std::strcpy(root, directory); return true;
}
bool SaveStorageReady() { controller(); return active && ready; }
unsigned OpenSaveTransactions() { controller(); unsigned n = 0; for (auto& h : handles) n += h.fd >= 0; return n; }
}
namespace Mc {
DefineSingletonClass(Manager, "MemCard Manager");
Manager::Manager() {
    controller(); Dbg_Assert(!active); active = true; current.clear();
    ready = mount_namespace();
    for (int p = 0; p < vMAX_PORT; ++p) for (int s = 0; s < vMAX_SLOT; ++s) {
        m_card[p][s].m_port = p; m_card[p][s].m_slot = s;
        m_card[p][s].m_last_error = ready ? 0 : card_error(errno);
    }
}
Manager::~Manager() { controller(); Dbg_Assert(!SourcePort::OpenSaveTransactions()); active = ready = false; std::string().swap(current); }
int Manager::GetMaxSlots(int port) { controller(); return port == 0 ? 1 : 0; }
Card* Manager::GetCard(int port, int slot) { controller(); return ready && port == 0 && slot == 0 && m_card[0][0].IsFormatted() ? &m_card[0][0] : nullptr; }
bool Card::MakeDirectory(const char* name) {
    controller(); std::string p; if (!path(name, p)) { SetError(card_error(errno)); return false; }
    if (!mkdir(p.c_str(), 0700)) return true;
    struct stat st{}; if (errno == EEXIST && !lstat(p.c_str(), &st) && S_ISDIR(st.st_mode)) return true;
    SetError(card_error(errno)); return false;
}
bool Card::DeleteDirectory(const char* name) {
    controller(); std::string p; if (!path(name, p) || p == root) { SetError(vINVALID_PATH); return false; }
    if (!rmdir(p.c_str())) return true; SetError(card_error(errno)); return false;
}
const char* Card::ConvertDirectory(const char* name) {
    controller(); static char relative[256]; std::string p; struct stat st{};
    if (!path(name, p) || stat(p.c_str(), &st) || !S_ISDIR(st.st_mode)) { SetError(vINVALID_PATH); return nullptr; }
    auto offset = std::strlen(root); if (p.size() > offset) ++offset;
    std::strcpy(relative, p.c_str() + offset); return relative;
}
bool Card::ChangeDirectory(const char* name) {
    controller(); const char* converted = ConvertDirectory(name); if (!converted) return false;
    current = converted; return true;
}
bool Card::Format() { controller(); errno = ENOTSUP; SetError(vACCESS_ERROR); return false; } // SD volume format is not exposed to game scripts
bool Card::Unformat() { return Format(); }
bool Card::IsFormatted() { controller(); return ready && access(root, R_OK | W_OK | X_OK) == 0; }
int Card::GetDeviceType() { return IsFormatted() ? vDEV_N3DS_SD : vDEV_NONE; }
int Card::GetNumFreeClusters() {
    controller(); struct statvfs info{};
    if (!ready || statvfs(root, &info)) { SetError(vACCESS_ERROR); return 0; }
    const uint64 bytes = uint64(info.f_bavail) * info.f_frsize;
    return int(std::min<uint64>(bytes / 1024, INT_MAX)); // original non-GC block size is 1 KiB
}
int Card::GetNumFreeEntries(const char* name) {
    controller(); std::string p; if (!path(name, p)) return 0;
    DIR* d = opendir(p.c_str()); if (!d) { SetError(card_error(errno)); return 0; }
    int used = 0; while (auto* e = readdir(d)) if (e->d_name[0] != '.') ++used;
    closedir(d); return std::max(0, vMAX_FILES_PER_DIRECTORY - used);
}
bool Card::Delete(const char* name) {
    controller(); std::string p; if (!path(name, p) || p == root) { SetError(vINVALID_PATH); return false; }
    if (busy(p)) { errno = EBUSY; SetError(vACCESS_ERROR); return false; }
    if (!unlink(p.c_str())) return true; SetError(card_error(errno)); return false;
}
bool Card::Rename(const char* old_name, const char* new_name) {
    controller(); std::string a, b;
    if (!path(old_name, a) || !path(new_name, b) || a == root || b == root) { SetError(vINVALID_PATH); return false; }
    struct stat st{};
    if (busy(a) || busy(b) || !lstat(b.c_str(), &st)) { errno = EEXIST; SetError(vACCESS_ERROR); return false; }
    if (!rename(a.c_str(), b.c_str())) return true; SetError(card_error(errno)); return false;
}
File* Card::Open(const char* name, int mode, int) {
    controller(); std::string p;
    if (!path(name, p) || p == root) { SetError(vINVALID_PATH); return nullptr; }
    if (busy(p)) { errno = EBUSY; SetError(vACCESS_ERROR); return nullptr; }
    bool reading = mode == File::mMODE_READ;
    bool create = mode & File::mMODE_CREATE;
    bool writing = mode == File::mMODE_WRITE || mode == (File::mMODE_READ | File::mMODE_WRITE) || mode == (File::mMODE_WRITE | File::mMODE_CREATE) || mode == File::mMODE_CREATE;
    if (!reading && !writing) { errno = EINVAL; SetError(vINVALID_PATH); return nullptr; }
    Handle* slot = nullptr; for (auto& h : handles) if (h.fd < 0) { slot = &h; break; }
    if (!slot) { errno = EMFILE; SetError(vTOO_MANY_OPEN_FILES); return nullptr; }
    struct stat st{}; bool exists = !lstat(p.c_str(), &st);
    if ((exists && !S_ISREG(st.st_mode)) || (!exists && !create) || (exists && mode == File::mMODE_CREATE)) { SetError(vINVALID_PATH); return nullptr; }
    Handle next; next.final_path = p;
    if (reading) next.fd = open(p.c_str(), O_RDONLY);
    else {
        auto slash = p.find_last_of('/');
        next.temporary = p.substr(0, slash + 1) + ".txn-" + std::to_string(++serial);
        next.fd = open(next.temporary.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (next.fd >= 0 && exists && !create) {
            int from = open(p.c_str(), O_RDONLY); bool ok = from >= 0; char buffer[4096]; ssize_t n;
            while (ok && (n = read(from, buffer, sizeof(buffer))) > 0) ok = write(next.fd, buffer, n) == n;
            if (from >= 0) { if (n < 0) ok = false; close(from); }
            if (!ok || lseek(next.fd, 0, SEEK_SET) < 0) { release(next, true); SetError(vACCESS_ERROR); return nullptr; }
        }
    }
    if (next.fd < 0) { SetError(card_error(errno)); return nullptr; }
    *slot = std::move(next);
    auto* result = new File(slot->fd, this);
    auto base = p.substr(p.find_last_of('/') + 1); std::strcpy(result->m_Filename, base.c_str());
    if (exists) { result->m_Size = st.st_size; date(result->m_Created, st.st_ctime); date(result->m_Modified, st.st_mtime); }
    m_last_error = 0; return result;
}
bool Card::GetFileList(const char* mask, Lst::Head<File>& list) {
    controller(); std::string p;
    if (!path(mask, p, true)) { SetError(vINVALID_PATH); return false; }
    auto slash = p.find_last_of('/'); auto directory = p.substr(0, slash), pattern = p.substr(slash + 1);
    DIR* d = opendir(directory.c_str()); if (!d) { SetError(card_error(errno)); return false; }
    while (auto* e = readdir(d)) {
        if (e->d_name[0] == '.' || fnmatch(pattern.c_str(), e->d_name, 0)) continue;
        if (std::strlen(e->d_name) > File::vMAX_FILENAME_LEN) continue;
        struct stat st{}; auto full = directory + "/" + e->d_name;
        if (lstat(full.c_str(), &st) || S_ISLNK(st.st_mode)) continue;
        auto* item = new File(-1, this); std::strcpy(item->m_Filename, e->d_name);
        item->m_Size = st.st_size; item->m_Attribs = File::mATTRIB_READABLE | File::mATTRIB_WRITEABLE | (S_ISDIR(st.st_mode) ? File::mATTRIB_DIRECTORY : 0);
        date(item->m_Created, st.st_ctime); date(item->m_Modified, st.st_mtime); list.AddToTail(item);
    }
    closedir(d); return true;
}
File::File(int fd, Card* card) : Lst::Node<File>(this), m_Filename{}, m_Created{}, m_Modified{}, m_Size(0), m_Attribs(0), m_fd(fd), m_card(card) {}
File::~File() { controller(); if (auto* h = handle(m_fd)) release(*h, true); }
bool File::Flush() {
    controller(); auto* h = handle(m_fd); if (!h) return false;
    if (fsync(m_fd)) { h->failed = true; m_card->SetError(card_error(errno)); return false; } return true;
}
int File::Write(void* data, int len) {
    controller(); auto* h = handle(m_fd); if (!h || len < 0 || (!data && len)) return -1;
    auto n = write(m_fd, data, len);
    if (n != len) { h->failed = true; m_card->SetError(card_error(errno)); }
    return int(n);
}
int File::Read(void* data, int len) { controller(); if (!handle(m_fd) || len < 0 || (!data && len)) return -1; return int(read(m_fd, data, len)); }
int File::Seek(int offset, FilePointerBase base) {
    controller(); if (!handle(m_fd)) return -1;
    if (base != BASE_START && base != BASE_CURRENT && base != BASE_END) { errno = EINVAL; return -1; }
    auto at = lseek(m_fd, offset, base == BASE_START ? SEEK_SET : base == BASE_CURRENT ? SEEK_CUR : SEEK_END);
    if (at > INT_MAX) { errno = EOVERFLOW; return -1; } return int(at);
}
bool File::Close() {
    controller(); auto* h = handle(m_fd); if (!h) return false;
    bool ok = !h->failed;
    if (!h->temporary.empty() && fsync(m_fd)) ok = false;
    if (close(m_fd)) ok = false; h->fd = -1; m_fd = -1;
    if (!h->temporary.empty()) {
        if (ok && rename(h->temporary.c_str(), h->final_path.c_str())) ok = false;
        if (!ok) unlink(h->temporary.c_str());
    }
    release(*h, false); if (!ok) m_card->SetError(Card::vACCESS_ERROR); return ok;
}
}
