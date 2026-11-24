// Read-only retail PRE recovery for a NEW package, using the original decoder.
// Some installed loose CAS names are truncated; never replace an existing file.
#include <core/defines.h>
#include <source_port/files.h>
#include <sys/file/filesys.h>
#include <sys/file/pre.h>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <array>
#include <cctype>
namespace fs=std::filesystem;

namespace {
struct PreMember {
    std::array<char, 128> key{};
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::string original_name;
};

void write_disk_pre(const char* archive, const fs::path& target) {
    const std::string source = std::string("pre/") + archive + ".pre";
    auto* raw = static_cast<uint8*>(File::LoadAllocNoPre(source.c_str()));
    Dbg_Assert(raw);
    std::uint32_t byte_count = 0, member_count = 0;
    std::memcpy(&byte_count, raw, 4);
    std::memcpy(&member_count, raw + 8, 4);
    Dbg_Assert(SourcePort::ValidatePre(raw, byte_count) && member_count > 0 && member_count <= 2048);
    File::PreFile pre(raw);
    std::vector<PreMember> members;
    const uint8* at = raw + 12;
    for (std::uint32_t i = 0; i < member_count; ++i) {
        std::uint32_t size = 0, compressed = 0;
        std::uint16_t name_bytes = 0;
        std::memcpy(&size, at, 4);
        std::memcpy(&compressed, at + 4, 4);
        std::memcpy(&name_bytes, at + 8, 2);
        PreMember member;
        member.original_name = reinterpret_cast<const char*>(at + 16);
        auto key = member.original_name;
        std::replace(key.begin(), key.end(), '\\', '/');
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".xbx") == 0) key.resize(key.size() - 4);
        Dbg_Assert(!key.empty() && key.size() < member.key.size());
        std::copy(key.begin(), key.end(), member.key.begin());
        member.size = size;
        members.push_back(std::move(member));
        at += 16 + name_bytes + (((compressed ? compressed : size) + 3) & ~3u);
    }
    std::sort(members.begin(), members.end(), [](const auto& lhs, const auto& rhs) { return lhs.key < rhs.key; });
    std::uint32_t offset = 8 + members.size() * 136;
    for (std::size_t i = 0; i < members.size(); ++i) {
        Dbg_Assert(i == 0 || members[i - 1].key != members[i].key);
        members[i].offset = offset;
        offset += members[i].size;
    }
    FILE* output = std::fopen(target.c_str(), "wbx");
    Dbg_Assert(output);
    const std::uint32_t header[] = {0x314d5044, member_count};
    Dbg_Assert(std::fwrite(header, sizeof(header), 1, output) == 1);
    for (const auto& member : members) {
        Dbg_Assert(std::fwrite(member.key.data(), member.key.size(), 1, output) == 1);
        const std::uint32_t entry[] = {member.offset, member.size};
        Dbg_Assert(std::fwrite(entry, sizeof(entry), 1, output) == 1);
    }
    std::vector<uint8> decoded;
    for (const auto& member : members) {
        if (!member.size) continue;
        decoded.resize(member.size);
        int size = 0;
        Dbg_Assert(pre.LoadContainedFile(member.original_name.c_str(), &size, decoded.data()) == decoded.data());
        Dbg_Assert(size == static_cast<int>(member.size));
        Dbg_Assert(std::fwrite(decoded.data(), decoded.size(), 1, output) == 1);
    }
    Dbg_Assert(!std::fclose(output));
    std::printf("Packed %zu original members into %s (%u bytes)\n", members.size(), target.c_str(), offset);
}

void write_aligned_animations(const std::string& archive, const fs::path& directory) {
    const auto target = directory / (archive + ".apre");
    auto* raw = static_cast<uint8*>(File::LoadAllocNoPre(("pre/" + archive + ".pre").c_str()));
    Dbg_Assert(raw);
    std::uint32_t byte_count = 0, member_count = 0;
    std::memcpy(&byte_count, raw, 4);
    std::memcpy(&member_count, raw + 8, 4);
    Dbg_Assert(SourcePort::ValidatePre(raw, byte_count));
    File::PreFile pre(raw);
    std::vector<uint8> aligned(raw, raw + 12);
    const uint8* source = raw + 12;
    for (std::uint32_t i = 0; i < member_count; ++i) {
        std::uint32_t size = 0, compressed = 0;
        std::uint16_t name_bytes = 0;
        std::memcpy(&size, source, 4);
        std::memcpy(&compressed, source + 4, 4);
        std::memcpy(&name_bytes, source + 8, 2);
        const auto header = aligned.size();
        aligned.insert(aligned.end(), source, source + 16 + name_bytes);
        std::fill_n(aligned.data() + header + 4, 4, uint8(0)); // uncompressed payload
        std::fill_n(aligned.data() + header + 10, 2, uint8(0)); // PIP usage count
        const auto data = (aligned.size() + 15) & ~std::size_t(15);
        aligned.resize(data + ((std::size_t(size) + 3) & ~std::size_t(3)));
        int decoded = 0;
        Dbg_Assert(pre.LoadContainedFile(reinterpret_cast<const char*>(source + 16), &decoded,
                                        aligned.data() + data) == aligned.data() + data);
        Dbg_Assert(decoded == static_cast<int>(size));
        source += 16 + name_bytes + (((compressed ? compressed : size) + 3) & ~3u);
    }
    const std::uint32_t size = aligned.size();
    std::memcpy(aligned.data(), &size, 4);
    FILE* output = std::fopen(target.c_str(), "wbx");
    Dbg_Assert(output);
    Dbg_Assert(std::fwrite(aligned.data(), aligned.size(), 1, output) == 1);
    Dbg_Assert(!std::fclose(output));
    std::printf("Aligned %u original animation members (%u bytes)\n", member_count, size);
}
}

int main(int argc,char** argv){
    if (argc >= 4 && std::string(argv[1]) == "--disk-pre") {
        Mem::Manager::sSetUp();
        {
            Dbg_Assert(SourcePort::ConfigureFileRoots(argv[2]));
            SourcePort::FileSystemSession files;
            fs::create_directories(argv[3]);
            if (argc == 4) {
                for (const char* archive : {"skaterparts", "skater_sounds", "themes", "sc2scn", "sc2", "sc2ped"})
                    write_disk_pre(archive, fs::path(argv[3]) / (std::string(archive) + ".pak"));
                for (const char* archive : {"unloadableanims", "netanims"})
                    write_aligned_animations(archive, argv[3]);
            }
            for (int i = 4; i < argc; ++i) {
                std::string archive = argv[i];
                Dbg_Assert(!archive.empty() && archive.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos);
                write_disk_pre(archive.c_str(), fs::path(argv[3]) / (archive + ".pak"));
            }
        }
        Mem::Manager::sCloseDown();
        return 0;
    }
    if(argc!=3){std::fprintf(stderr,"usage: source_prepare_cas_resources RETAIL_DATA NEW_DATA\n");return 2;}
    if(fs::equivalent(argv[1],argv[2]))return 2;
    Mem::Manager::sSetUp();unsigned added=0;
    {
        Dbg_Assert(SourcePort::ConfigureFileRoots(argv[1]));SourcePort::FileSystemSession files;
        auto* raw=static_cast<uint8*>(File::LoadAllocNoPre("pre/skaterparts.pre"));Dbg_Assert(raw);
        uint32 size,count;std::memcpy(&size,raw,4);std::memcpy(&count,raw+8,4);Dbg_Assert(SourcePort::ValidatePre(raw,size));
        // Enumerate names only; the original PreFile owns and decodes bytes.
        std::vector<std::string> names;const uint8* at=raw+12;
        for(uint32 i=0;i<count;++i){uint32 bytes,packed;uint16 length;std::memcpy(&bytes,at,4);std::memcpy(&packed,at+4,4);std::memcpy(&length,at+8,2);names.emplace_back(reinterpret_cast<const char*>(at+16));at+=16+length+(((packed?packed:bytes)+3)&~3u);}
        File::PreFile pre(raw);
        for(auto name:names){std::replace(name.begin(),name.end(),'\\','/');auto relative=name;
            if(relative.size()>4&&relative.substr(relative.size()-4)==".xbx")relative.resize(relative.size()-4);
            fs::path rel(relative);Dbg_Assert(!rel.is_absolute());for(const auto& part:rel)Dbg_Assert(part!=".."&&part!=".");
            auto target=fs::path(argv[2])/rel;if(fs::exists(target))continue;
            Dbg_Assert(relative.rfind("textures/",0)==0||relative.rfind("images/",0)==0);
            Dbg_Assert(pre.GetContainedFile(name.c_str()));pre.Reset();std::vector<uint8> decoded(pre.GetFileSize());Dbg_Assert(pre.Read(decoded.data(),decoded.size())==int(decoded.size()));pre.Close(false);
            fs::create_directories(target.parent_path());FILE* f=std::fopen(target.c_str(),"wbx");Dbg_Assert(f);Dbg_Assert(std::fwrite(decoded.data(),1,decoded.size(),f)==decoded.size());Dbg_Assert(!std::fclose(f));
            std::printf("recovered %zu bytes %s\n",decoded.size(),relative.c_str());++added;
        }
    }
    Mem::Manager::sCloseDown();std::printf("Recovered %u missing original CAS resources; existing data untouched\n",added);
}
