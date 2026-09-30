// VPK v1/v2 reader. Each worker owns its archive and FILE handles.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

struct VpkEntry {
    std::string path;
    uint32_t crc = 0;
    std::vector<uint8_t> preload;
    struct Part { uint16_t fileNumber; uint32_t offset, size; };
    std::vector<Part> parts;
};

class VpkArchive {
public:
    ~VpkArchive();
    VpkArchive() = default;
    VpkArchive(const VpkArchive&) = delete;
    VpkArchive& operator=(const VpkArchive&) = delete;
    void Open(const std::string& baseOrDirectory);
    const std::vector<VpkEntry>& Entries() const { return entries_; }
    std::vector<uint8_t> ReadFile(const VpkEntry& entry);
private:
    std::string base_;
    uint64_t embeddedOffset_ = 0, embeddedSize_ = 0;
    std::vector<VpkEntry> entries_;
    std::map<uint16_t, FILE*> chunks_;
    FILE* GetChunk(uint16_t number);
};
