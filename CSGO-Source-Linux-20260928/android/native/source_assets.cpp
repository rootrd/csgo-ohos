#include "source_assets.h"

#include <clocale>
#include <memory>
#include <stdexcept>

#include "tier0/icommandline.h"
#include "tier1/checksum_crc.h"
#include "tier1/utlbuffer.h"
#include "filesystem.h"
#include "bspfile.h"
#include "vtf/vtf.h"
#include "tier0/memdbgoff.h"

namespace {
void require(bool condition, const std::string& error) {
    if (!condition) throw std::runtime_error(error);
}

class EngineFileSystem {
    CSysModule* cvarModule = nullptr;
    CSysModule* module = nullptr;
    IFileSystem* filesystem = nullptr;
    bool connected = false, initialized = false;
public:
    ~EngineFileSystem() {
        if (initialized) filesystem->Shutdown();
        if (connected) filesystem->Disconnect();
        Sys_UnloadModule(module);
        Sys_UnloadModule(cvarModule);
    }
    IFileSystem* open(const char* root) {
        require(std::setlocale(LC_CTYPE, "C.UTF-8") != nullptr, "Cannot select UTF-8 locale");
        CommandLine()->CreateCmdLine("csgo_android -game csgo -nosteam -insecure -novid");
        MathLib_Init();
        cvarModule = Sys_LoadModule("libvstdlib_client.so");
        module = Sys_LoadModule("filesystem_stdio");
        require(cvarModule && module, "Cannot load packaged Source vstdlib/filesystem modules");
        auto cvarFactory = Sys_GetFactory(cvarModule), factory = Sys_GetFactory(module);
        require(cvarFactory && factory, "Source module CreateInterface export is missing");
        filesystem = static_cast<IFileSystem*>(factory(FILESYSTEM_INTERFACE_VERSION, nullptr));
        require(filesystem != nullptr, "Source filesystem interface version does not match");
        connected = filesystem->Connect(cvarFactory);
        require(connected, "Cannot connect Source filesystem dependencies");
        initialized = filesystem->Init() == INIT_OK;
        require(initialized, "Cannot initialize Source filesystem");
        const auto game = std::string(root) + "/csgo";
        filesystem->AddSearchPath(game.c_str(), "GAME");
        filesystem->AddVPKFile((game + "/pak01.vpk").c_str());
        return filesystem;
    }
};

SourceTexture textureForUpload(IVTFTexture& vtf) {
    require(vtf.FrameCount() == 1 && vtf.FaceCount() == 1 && vtf.Depth() == 1,
            "Graphics check requires a single 2D VTF image");
    require(vtf.Width() > 0 && vtf.Height() > 0 && vtf.Width() <= 4096 && vtf.Height() <= 4096,
            "VTF dimensions exceed graphics check bounds");
    SourceTexture result;
    result.width = vtf.Width();
    result.height = vtf.Height();
    switch (vtf.Format()) {
    case IMAGE_FORMAT_DXT1:
    case IMAGE_FORMAT_DXT1_ONEBITALPHA:
        result.encoding = TextureEncoding::BC1; result.blockBytes = 8; break;
    case IMAGE_FORMAT_DXT3:
        result.encoding = TextureEncoding::BC2; result.blockBytes = 16; break;
    case IMAGE_FORMAT_DXT5:
        result.encoding = TextureEncoding::BC3; result.blockBytes = 16; break;
    default:
        throw std::runtime_error("Graphics check requires BC1/BC2/BC3; no texture decode fallback is enabled");
    }
    result.rowBytes = ((result.width + result.blockWidth - 1) / result.blockWidth) * result.blockBytes;
    result.rows = (result.height + result.blockHeight - 1) / result.blockHeight;
    const size_t size = size_t(result.rowBytes) * result.rows;
    require(vtf.ComputeMipSize(0) > 0 && size_t(vtf.ComputeMipSize(0)) == size && vtf.ImageData(0, 0, 0),
            "Source VTF mip size does not match the upload block layout");
    result.pixels.assign(vtf.ImageData(0, 0, 0), vtf.ImageData(0, 0, 0) + size);
    return result;
}
} // namespace

SourceTexture readSourceAssets(const char* root) {
    EngineFileSystem engine;
    IFileSystem* filesystem = engine.open(root);
    CUtlBuffer texture;
    require(filesystem->ReadFile("materials/dev/dev_measuregeneric01b.vtf", "GAME", texture),
            "Cannot read graphics-check texture from the Source VPK filesystem");
    // This fixture is pinned to the validated 2019 resource manifest.
    require(texture.TellPut() == 11128 && CRC32_ProcessSingleBuffer(texture.Base(), texture.TellPut()) == 0xb53a30e6u,
            "Graphics-check VTF CRC differs from the validated resource manifest");
    std::unique_ptr<IVTFTexture, decltype(&DestroyVTFTexture)> vtf(CreateVTFTexture(), DestroyVTFTexture);
    require(vtf && vtf->Unserialize(texture), "Source VTF parser rejected the texture");
    auto result = textureForUpload(*vtf);

    FileHandle_t file = filesystem->Open("maps/de_dust2.bsp", "rb", "GAME");
    require(file != FILESYSTEM_INVALID_HANDLE, "Cannot open Dust II through the Source filesystem");
    static_assert(sizeof(BSPHeader_t) == 1036, "BSP header ABI");
    BSPHeader_t header{};
    const uint64_t bspSize = filesystem->Size(file);
    const int bytesRead = filesystem->Read(&header, sizeof(header), file);
    filesystem->Close(file);
    require(bytesRead == sizeof(header) && header.ident == IDBSPHEADER && header.m_nVersion == 21,
            "Expected the CS:GO Dust II BSP version 21");
    for (const auto& lump : header.lumps)
        require(lump.fileofs >= 0 && lump.filelen >= 0 && uint64_t(lump.fileofs) + lump.filelen <= bspSize,
                "BSP lump outside file");
    result.description = "Source filesystem + VPK + VTF; dev_measuregeneric01b.vtf CRC OK, "
        + std::to_string(result.width) + "x" + std::to_string(result.height)
        + "; de_dust2 BSP v21, 64 lump ranges OK, bytes=" + std::to_string(bspSize);
    return result;
}
