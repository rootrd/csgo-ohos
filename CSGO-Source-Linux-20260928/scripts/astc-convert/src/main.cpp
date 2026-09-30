// Offline VPK/VTF -> ASTC KTX2 texture overlay. No engine or game installation changes.
#include "vpk_reader.h"
#include "vtf_decode.h"
#include "ktx2_writer.h"
#include "binary.h"
#include <astcenc.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Json = nlohmann::json;

static std::vector<uint8_t> ReadBytes(const fs::path& path) {
    const auto size = fs::file_size(path);
    Require(size <= (1ULL << 31), "file exceeds 2 GiB: " + path.string());
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> result(size);
    Require(bool(file.read(reinterpret_cast<char*>(result.data()), size)), "cannot read " + path.string());
    return result;
}
static void WriteBytes(const fs::path& path, const uint8_t* data, size_t size) {
    fs::create_directories(path.parent_path());
    const auto temporary = path.string() + ".partial";
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        Require(bool(file.write(reinterpret_cast<const char*>(data), size)), "cannot write " + temporary);
        file.close(); Require(bool(file), "cannot close " + temporary);
        fs::rename(temporary, path);
    } catch (...) {
        std::error_code ignored; fs::remove(temporary, ignored); throw;
    }
}
static void WriteJson(const fs::path& path, const Json& json) {
    const std::string text = json.dump(2) + "\n";
    WriteBytes(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}
static std::string Digest(const std::vector<uint8_t>& data) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length = 0;
    Require(EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr) == 1, "SHA-256 failed");
    const char* digits = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < length; ++i) { result += digits[digest[i] >> 4]; result += digits[digest[i] & 15]; }
    return result;
}
static unsigned Number(const std::string& text, unsigned maximum) {
    Require(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos, "invalid number: " + text);
    const auto number = std::stoull(text);
    Require(number <= maximum, "number out of range: " + text);
    return unsigned(number);
}
struct Options {
    std::string vpk, input, out, filter, mode = "convert", qualityName = "medium";
    unsigned block = 0, limit = 0, jobs = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    float quality = ASTCENC_PRE_MEDIUM;
    bool srgb = false, resume = false, verbose = false, dumpRGBA = false;
};
static void Usage() {
    std::cout << "vtf2astc (--vpk BASE_OR_DIR_VPK | --input FILE.vtf) --out DIR\n"
                 "  [--mode convert|inspect|extract] [--filter SUBSTRING] [--limit N]\n"
                 "  [--block auto|4x4|6x6|8x8] [--quality fastest|fast|medium|thorough|exhaustive]\n"
                 "  [--threads N] [--resume] [--srgb] [--dump-rgba] [-v]\n"
                 "auto: 4x4 for normals, alpha and UI, 6x6 otherwise. Default: medium.\n"
                 "HDR/signed data use lossless native KTX2 formats; volumes use uncompressed KTX2.\n"
                 "--threads is the number of parallel files; one astcenc context per worker.\n"
                 "--srgb is an explicit color sampling override. Normals/data stay linear.\n";
}
static Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() { Require(i + 1 < argc, "missing value for " + arg); return std::string(argv[++i]); };
        if (arg == "--vpk") options.vpk = value();
        else if (arg == "--input") options.input = value();
        else if (arg == "--out") options.out = value();
        else if (arg == "--filter") options.filter = value();
        else if (arg == "--mode") options.mode = value();
        else if (arg == "--limit") options.limit = Number(value(), 10000000);
        else if (arg == "--threads") options.jobs = Number(value(), 64);
        else if (arg == "--block") {
            const auto block = value();
            Require(block == "auto" || block == "4x4" || block == "6x6" || block == "8x8", "unsupported block size");
            options.block = block == "auto" ? 0 : block[0] - '0';
        } else if (arg == "--quality") {
            options.qualityName = value();
            const std::map<std::string, float> qualities = {{"fastest", ASTCENC_PRE_FASTEST}, {"fast", ASTCENC_PRE_FAST},
                {"medium", ASTCENC_PRE_MEDIUM}, {"thorough", ASTCENC_PRE_THOROUGH}, {"exhaustive", ASTCENC_PRE_EXHAUSTIVE}};
            Require(qualities.count(options.qualityName), "unknown quality preset");
            options.quality = qualities.at(options.qualityName);
        } else if (arg == "--srgb") options.srgb = true;
        else if (arg == "--resume") options.resume = true;
        else if (arg == "--dump-rgba") options.dumpRGBA = true;
        else if (arg == "-v") options.verbose = true;
        else throw std::runtime_error("unknown option: " + arg);
    }
    Require(options.vpk.empty() != options.input.empty(), "specify exactly one of --vpk or --input");
    Require(!options.out.empty() && options.jobs, "--out and at least one worker are required");
    Require(options.mode == "convert" || options.mode == "inspect" || options.mode == "extract", "unknown mode");
    return options;
}

class Encoder {
public:
    Encoder(unsigned block, bool srgb, float quality) : block_(block) {
        astcenc_config config{};
        Check(astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, block, block, 1,
                                 quality, 0, &config));
        Check(astcenc_context_alloc(&config, 1, &context_, nullptr));
    }
    ~Encoder() { astcenc_context_free(context_); }
    std::vector<uint8_t> Compress(std::vector<uint8_t>& rgba, unsigned width, unsigned height) {
        void* slices[] = {rgba.data()};
        astcenc_image image{width, height, 1, ASTCENC_TYPE_U8, slices};
        const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        std::vector<uint8_t> output(size_t((width + block_ - 1) / block_) * ((height + block_ - 1) / block_) * 16);
        Check(astcenc_compress_image(context_, &image, &swizzle, output.data(), output.size(), 0));
        Check(astcenc_compress_reset(context_));
        return output;
    }
private:
    static void Check(astcenc_error result) { Require(result == ASTCENC_SUCCESS, astcenc_get_error_string(result)); }
    unsigned block_;
    astcenc_context* context_ = nullptr;
};
using Encoders = std::map<std::pair<unsigned, bool>, std::unique_ptr<Encoder>>;

static Json Describe(const VtfTexture& texture) {
    return {{"version", "7." + std::to_string(texture.versionMinor)}, {"format", texture.format},
        {"width", texture.width}, {"height", texture.height}, {"depth", texture.depth},
        {"frames", texture.frames}, {"faces", texture.faces}, {"stored_faces", texture.storedFaces},
        {"mips", texture.mipCount}, {"flags", texture.flags}, {"start_frame", texture.startFrame},
        {"image_bytes", texture.imageBytes}};
}
static unsigned ChooseBlock(const Options& options, const VtfTexture& texture, const std::string& path) {
    if (options.block) return options.block;
    const bool detailed = (texture.flags & (VTF_FLAG_NORMAL | VTF_FLAG_SSBUMP | 0x3000)) ||
        path.find("/vgui/") != std::string::npos || path.find("/sprites/") != std::string::npos ||
        path.find("/decals/") != std::string::npos;
    return detailed ? 4 : 6;
}
static Json Convert(const Options& options, const Json& recipe, const std::string& name,
                    const std::vector<uint8_t>& bytes, Encoders& encoders, bool& reused) {
    const auto texture = ParseVTF(bytes.data(), bytes.size());
    Json record = {{"path", name}, {"source_sha256", Digest(bytes)}, {"source_bytes", bytes.size()},
                   {"source", Describe(texture)}, {"recipe", recipe}};
    if (options.mode == "inspect") { record["status"] = "inspected"; return record; }
    if (options.mode == "extract") {
        WriteBytes(fs::path(options.out) / name, bytes.data(), bytes.size());
        record["status"] = "extracted"; return record;
    }
    fs::path relative(name); relative.replace_extension(".ktx2");
    const fs::path output = fs::path(options.out) / relative;
    const fs::path sidecar = output.string() + ".json";
    if (options.resume && fs::exists(output) && fs::exists(sidecar) && !options.dumpRGBA) {
        try {
            const auto prior = Json::parse(ReadBytes(sidecar));
            if (prior.at("source_sha256") == record["source_sha256"] && prior.at("recipe") == recipe &&
                prior.at("output_sha256") == Digest(ReadBytes(output))) {
                reused = true; return prior;
            }
        } catch (const std::exception&) { /* Rebuild incomplete or stale pairs. */ }
    }
    const VkFormat lossless = LosslessFormat(texture);
    const bool compressed = lossless == VK_FORMAT_UNDEFINED;
    const unsigned block = compressed ? ChooseBlock(options, texture, name) : 0;
    const bool srgb = compressed && options.srgb && !(texture.flags & (VTF_FLAG_NORMAL | VTF_FLAG_SSBUMP)) &&
                      texture.format != FMT_ATI1N && texture.format != FMT_ATI2N;
    const VkFormat format = compressed ? AstcFormat(block, srgb) : lossless;
    const bool rawSource = !compressed && lossless != VK_FORMAT_R8G8B8A8_UNORM;
    Ktx2Writer writer(texture, format);
    for (const auto& metadata : texture.metadata) writer.Metadata(metadata.first, metadata.second);
    writer.Metadata("KTXorientation", texture.depth > 1 ? "rdi" : "rd");
    // Match libktx's exact version string. In 4.4.2, appendLibId unlinks the old
    // entry without freeing it when it replaces an existing writer value.
    writer.Metadata("KTXwriter", "vtf2astc 2.0 / astcenc 5.7.0 / libktx v4.4.2");
    writer.Metadata("source.vtf.schema", "1");
    writer.Metadata("source.vtf.path", name);
    writer.Metadata("source.vtf.sha256", record["source_sha256"].get<std::string>());
    writer.Metadata("source.vtf.info", record["source"].dump());
    writer.Metadata("source.vtf.recipe", recipe.dump());
    writer.Metadata("source.vtf.sampling", srgb ? "explicit-srgb" : "material-selected; stored values unchanged");
    for (unsigned mip = 0; mip < texture.mipCount; ++mip) {
        const unsigned width = MipDim(texture.width, mip), height = MipDim(texture.height, mip), depth = MipDim(texture.depth, mip);
        for (unsigned frame = 0; frame < texture.frames; ++frame) {
            for (unsigned face = 0; face < texture.faces; ++face) {
                for (unsigned z = 0; z < depth; ++z) {
                    const unsigned faceOrSlice = texture.depth > 1 ? z : face;
                    if (rawSource) {
                        writer.SetImage(mip, frame, faceOrSlice, bytes.data() + texture.Offset(mip, frame, face, z), texture.sliceBytes[mip]);
                    } else {
                        auto rgba = DecodeVtfSlice(bytes.data(), bytes.size(), texture, mip, frame, face, z);
                        if (options.dumpRGBA && !mip && !frame && !face && !z)
                            WriteBytes(output.string() + ".rgba", rgba.data(), rgba.size());
                        if (compressed) {
                            const auto key = std::make_pair(block, srgb);
                            if (!encoders.count(key)) encoders[key] = std::make_unique<Encoder>(block, srgb, options.quality);
                            const auto astc = encoders.at(key)->Compress(rgba, width, height);
                            writer.SetImage(mip, frame, faceOrSlice, astc.data(), astc.size());
                        } else writer.SetImage(mip, frame, faceOrSlice, rgba.data(), rgba.size());
                    }
                }
            }
        }
    }
    writer.Save(output.string());
    record["status"] = "converted";
    record["output"] = relative.generic_string();
    record["output_sha256"] = Digest(ReadBytes(output));
    record["output_bytes"] = fs::file_size(output);
    record["payload_bytes"] = writer.DataSize();
    record["vk_format"] = format;
    record["block"] = block;
    record["encoding"] = compressed ? "astc-ldr" : rawSource ? "lossless-native" : "rgba8-volume";
    record["sampling"] = srgb ? "srgb" : "material-selected";
    WriteJson(sidecar, record);
    return record;
}

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) { Usage(); return argc == 1 ? 2 : 0; }
    try {
        const auto options = ParseOptions(argc, argv);
        fs::create_directories(options.out);
        const int lock = open((fs::path(options.out) / ".vtf2astc.lock").c_str(), O_CREAT | O_WRONLY, 0600);
        Require(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0, "another conversion is using this output directory");
        const Json recipe = {{"schema", 2}, {"tool_sha256", Digest(ReadBytes("/proc/self/exe"))},
            {"astcenc", "5.7.0"}, {"ktx", "4.4.2"}, {"block_policy", options.block ? std::to_string(options.block) + "x" + std::to_string(options.block) : "auto-4x4-detail-6x6-color"},
            {"quality", options.qualityName}, {"srgb_override", options.srgb}, {"normal_channels", "preserve-rgba"}};
        VpkArchive archive;
        std::vector<VpkEntry> entries;
        if (!options.vpk.empty()) {
            archive.Open(options.vpk);
            for (const auto& entry : archive.Entries()) {
                if (fs::path(entry.path).extension() == ".vtf" && (options.filter.empty() || entry.path.find(options.filter) != std::string::npos))
                    entries.push_back(entry);
            }
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
        } else {
            VpkEntry entry; entry.path = fs::path(options.input).filename().string(); entries.push_back(entry);
        }
        if (options.limit && entries.size() > options.limit) entries.resize(options.limit);
        Require(!entries.empty(), "no matching VTF files");
        std::vector<Json> records(entries.size());
        std::atomic<size_t> next{0}, done{0}, failed{0}, resumed{0};
        std::mutex logMutex;
        const auto started = std::chrono::steady_clock::now();
        const unsigned workers = std::min(size_t(options.jobs), entries.size());
        std::cerr << "Selected " << entries.size() << " textures; " << workers << " workers; mode=" << options.mode << "\n";
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < workers; ++worker) threads.emplace_back([&]() {
            Encoders encoders;
            VpkArchive reader;
            std::string openError;
            try { if (!options.vpk.empty()) reader.Open(options.vpk); }
            catch (const std::exception& error) { openError = error.what(); }
            for (;;) {
                const size_t index = next.fetch_add(1);
                if (index >= entries.size()) break;
                const auto& entry = entries[index];
                try {
                    Require(openError.empty(), openError);
                    const auto bytes = options.vpk.empty() ? ReadBytes(options.input) : reader.ReadFile(entry);
                    bool reused = false;
                    records[index] = Convert(options, recipe, entry.path, bytes, encoders, reused);
                    if (reused) ++resumed;
                } catch (const std::exception& error) {
                    ++failed;
                    records[index] = {{"path", entry.path}, {"status", "failed"}, {"error", error.what()}};
                    std::lock_guard<std::mutex> guard(logMutex);
                    std::cerr << "FAIL " << entry.path << ": " << error.what() << "\n";
                }
                const size_t finished = ++done;
                if (options.verbose || finished % 100 == 0 || finished == entries.size()) {
                    std::lock_guard<std::mutex> guard(logMutex);
                    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    std::cerr << "[" << finished << "/" << entries.size() << "] failed=" << failed << " resumed=" << resumed << " elapsed=" << int(seconds) << "s " << entry.path << "\n";
                }
            }
        });
        for (auto& thread : threads) thread.join();
        uint64_t sourceBytes = 0, outputBytes = 0;
        std::map<std::string, size_t> encodings;
        for (const auto& record : records) {
            sourceBytes += record.value("source_bytes", uint64_t(0));
            outputBytes += record.value("output_bytes", uint64_t(0));
            if (record.contains("encoding")) ++encodings[record["encoding"].get<std::string>()];
        }
        Json manifest = {{"schema", 2}, {"recipe", recipe}, {"mode", options.mode},
            {"source", options.vpk.empty() ? options.input : options.vpk},
            {"selection", {{"filter", options.filter}, {"limit", options.limit}}},
            {"complete", failed == 0}, {"selected", entries.size()}, {"failed", failed.load()},
            {"resumed", resumed.load()}, {"source_bytes", sourceBytes}, {"output_bytes", outputBytes},
            {"encodings", encodings}, {"textures", records}};
        WriteJson(fs::path(options.out) / "manifest.json", manifest);
        std::cerr << "done: selected=" << entries.size() << " succeeded=" << entries.size() - failed << " failed=" << failed
                  << " resumed=" << resumed << " source_bytes=" << sourceBytes << " output_bytes=" << outputBytes << "\n";
        close(lock);
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "vtf2astc: " << error.what() << "\n";
        return 1;
    }
}
