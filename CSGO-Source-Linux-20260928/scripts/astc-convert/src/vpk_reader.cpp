#include "vpk_reader.h"
#include "binary.h"
#include <algorithm>
#include <filesystem>
#include <set>
#include <zlib.h>

namespace fs = std::filesystem;

VpkArchive::~VpkArchive() {
    for (auto& chunk : chunks_) if (chunk.second) fclose(chunk.second);
}

void VpkArchive::Open(const std::string& baseOrDirectory) {
    Require(entries_.empty() && chunks_.empty(), "archive already open");
    base_ = baseOrDirectory;
    const std::string suffix = "_dir.vpk";
    if (base_.size() >= suffix.size() && base_.compare(base_.size() - suffix.size(), suffix.size(), suffix) == 0)
        base_.resize(base_.size() - suffix.size());
    FILE* file = GetChunk(0x7fff);
    uint8_t header[28]{};
    const size_t count = fread(header, 1, sizeof(header), file);
    Require(count >= 12 && LE32(header) == 0x55aa1234, "not a VPK v1/v2 directory");
    const uint32_t version = LE32(header + 4), treeSize = LE32(header + 8);
    Require(version == 1 || version == 2, "unsupported VPK version");
    const uint32_t headerSize = version == 2 ? 28 : 12;
    const uint64_t size = fs::file_size(base_ + suffix);
    Require(count >= headerSize && RangeFits(headerSize, treeSize, size), "truncated VPK tree");
    embeddedOffset_ = uint64_t(headerSize) + treeSize;
    embeddedSize_ = version == 2 ? LE32(header + 12) : size - embeddedOffset_;
    Require(RangeFits(embeddedOffset_, embeddedSize_, size), "truncated VPK embedded data");
    std::vector<uint8_t> tree(treeSize);
    Require(fseeko(file, headerSize, SEEK_SET) == 0 && fread(tree.data(), 1, tree.size(), file) == tree.size(), "cannot read VPK tree");
    size_t pos = 0;
    auto need = [&](size_t bytes) { Require(RangeFits(pos, bytes, tree.size()), "truncated VPK entry"); };
    auto str = [&]() {
        need(1);
        auto end = std::find(tree.begin() + pos, tree.end(), uint8_t(0));
        Require(end != tree.end(), "unterminated VPK path");
        std::string result(tree.begin() + pos, end);
        pos = size_t(end - tree.begin()) + 1;
        return result;
    };
    std::set<std::string> names;
    for (std::string ext; !(ext = str()).empty();) {
        for (std::string dir; !(dir = str()).empty();) {
            for (std::string name; !(name = str()).empty();) {
                VpkEntry entry;
                need(6);
                entry.crc = LE32(&tree[pos]);
                const size_t preload = LE16(&tree[pos + 4]);
                pos += 6;
                for (;;) {
                    need(2);
                    const uint16_t number = LE16(&tree[pos]); pos += 2;
                    if (number == 0xffff) break;
                    need(8);
                    entry.parts.push_back({number, LE32(&tree[pos]), LE32(&tree[pos + 4])}); pos += 8;
                }
                need(preload);
                entry.preload.assign(tree.begin() + pos, tree.begin() + pos + preload); pos += preload;
                entry.path = (dir == " " ? "" : dir + "/") + name + (ext == " " ? "" : "." + ext);
                const fs::path path(entry.path);
                Require(!path.empty() && !path.is_absolute() && entry.path.find('\\') == std::string::npos, "unsafe VPK path");
                for (const auto& component : path)
                    Require(component != ".." && component != ".", "unsafe VPK path: " + entry.path);
                Require(names.insert(entry.path).second, "duplicate VPK path: " + entry.path);
                entries_.push_back(std::move(entry));
            }
        }
    }
    Require(pos == tree.size(), "unexpected bytes at end of VPK tree");
}

FILE* VpkArchive::GetChunk(uint16_t number) {
    auto found = chunks_.find(number);
    if (found != chunks_.end()) return found->second;
    char suffix[32];
    if (number == 0x7fff) snprintf(suffix, sizeof(suffix), "_dir.vpk");
    else snprintf(suffix, sizeof(suffix), "_%03u.vpk", number);
    FILE* file = fopen((base_ + suffix).c_str(), "rb");
    Require(file != nullptr, "cannot open " + base_ + suffix);
    chunks_[number] = file;
    return file;
}

std::vector<uint8_t> VpkArchive::ReadFile(const VpkEntry& entry) {
    auto output = entry.preload;
    for (const auto& part : entry.parts) {
        const uint64_t offset = uint64_t(part.offset) + (part.fileNumber == 0x7fff ? embeddedOffset_ : 0);
        if (part.fileNumber == 0x7fff)
            Require(RangeFits(part.offset, part.size, embeddedSize_), "VPK embedded range exceeds data section: " + entry.path);
        FILE* file = GetChunk(part.fileNumber);
        Require(fseeko(file, 0, SEEK_END) == 0 && ftello(file) >= 0 && RangeFits(offset, part.size, uint64_t(ftello(file))), "VPK part out of range: " + entry.path);
        const size_t old = output.size();
        Require(old <= (1ULL << 31) && part.size <= (1ULL << 31) - old, "VPK entry exceeds 2 GiB: " + entry.path);
        output.resize(old + part.size);
        Require(fseeko(file, offset, SEEK_SET) == 0 && fread(output.data() + old, 1, part.size, file) == part.size, "short VPK read: " + entry.path);
    }
    Require(uint32_t(crc32_z(0, output.data(), output.size())) == entry.crc, "VPK CRC mismatch: " + entry.path);
    return output;
}
