#pragma once
#include <cstddef>
#include <string>
namespace SourcePort {
// Resource and write roots are independent. Null write_root means read-only.
// Configure before original InstallFileSystem, never redirect existing saves.
bool ConfigureFileRoots(const char* resource_root, const char* write_root = nullptr);
bool ResolveResourcePath(const char* name, char* output, std::size_t capacity);
struct DiskResourceSlice { std::string archive; std::size_t offset, size; };
bool FindDiskResourceSlice(const char* name, DiskResourceSlice&);
bool MountDiskPre(const char* archive);
bool DiskPreMounted(const char* archive);
bool UnmountDiskPre(const char* archive);
unsigned char* AllocatePreBuffer(const char* archive, std::size_t size, bool bottom_up);
void* AllocatePipPreBuffer(const char* archive, std::size_t size, bool expanded);
char* LoadPreparedPipPre(const char* archive);
void FreePreBuffer(unsigned char* buffer);
bool ValidatePre(const unsigned char* data, std::size_t length);
bool FilesInstalled();
unsigned OpenDiskFiles();
class FileSystemSession {
public:
    FileSystemSession();
    ~FileSystemSession();
    FileSystemSession(const FileSystemSession&) = delete;
    FileSystemSession& operator=(const FileSystemSession&) = delete;
};
}
