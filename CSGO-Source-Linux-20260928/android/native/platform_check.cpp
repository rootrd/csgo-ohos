// Runs against the actual engine libraries on both x86-64 and Android ARM64.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fenv.h>
#include <time.h>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <clocale>

#include "tier0/platform.h"
#include "tier0/icommandline.h"
#include "basetypes.h"
#include "tier0/threadtools.h"
#include "tier0/memalloc.h"
#include "mathlib/mathlib.h"
#include "mathlib/ssemath.h"
#include "tier1/utlbuffer.h"
#include "tier1/strtools.h"
#include "tier1/keyvalues.h"
#include "tier1/checksum_crc.h"
#include "filesystem.h"
#include "bspfile.h"
#include "vtf/vtf.h"
#include "tier0/memdbgoff.h"

namespace {
void require(bool value, const char* label) {
    if (!value) { fprintf(stderr, "PLATFORM_CHECK_FAILED: %s\n", label); std::exit(1); }
}
uint32_t bits(float value) { uint32_t b; memcpy(&b, &value, 4); return b; }
float fromBits(uint32_t b) { float value; memcpy(&value, &b, 4); return value; }

void simdChecks() {
    static_assert(sizeof(fltx4) == 16 && alignof(fltx4) == 16, "SIMD ABI");
    static_assert(sizeof(void*) == 8 && sizeof(int32) == 4, "Platform ABI");
    static_assert(char(-1) < 0, "The engine requires signed char");
    const uint32_t special[] = {0, 0x80000000u, 0x3f800000, 0xbf800000, 0x7f800000,
                                0xff800000u, 0x7fc01234, 0xffc01234u, 1, 0x80000001u};
    for (auto aBits : special) for (auto bBits : special) {
        volatile float a = fromBits(aBits), b = fromBits(bBits);
        const auto av = _mm_set1_ps(a), bv = _mm_set1_ps(b);
        require(bits(_mm_cvtss_f32(MinSIMD(av, bv))) == (a < b ? aBits : bBits), "SIMD min NaN/signed zero/denormal");
        require(bits(_mm_cvtss_f32(MaxSIMD(av, bv))) == (a > b ? aBits : bBits), "SIMD max NaN/signed zero/denormal");
    }
    const float rounding[] = {-3.5f,-2.5f,-1.5f,-0.5f,0.5f,1.5f,2.5f,3.5f};
    for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        require(fesetround(mode) == 0, "Set rounding mode");
        for (volatile float f : rounding)
            require(RoundFloatToInt(f) == int(std::nearbyint(f)), "RoundFloatToInt rounding mode/ties");
    }
    fesetround(FE_TONEAREST);
    for (uint32_t invalid : {0x7fc01234u, 0x7f800000u, 0xff800000u, 0x4f000000u, 0xcf000001u}) {
        volatile float f = fromBits(invalid);
        require(_mm_cvtss_si32(_mm_set_ss(f)) == (-2147483647 - 1), "SSE invalid conversion sentinel");
        require(_mm_cvttss_si32(_mm_set_ss(f)) == (-2147483647 - 1), "SSE invalid truncation sentinel");
    }
    fltx4 x = _mm_setr_ps(1,2,3,4), y = _mm_setr_ps(5,6,7,8);
    fltx4 z = _mm_setr_ps(9,10,11,12), w = _mm_setr_ps(13,14,15,16);
    TransposeSIMD(x,y,z,w);
    require(SubFloat(x,3) == 13 && SubFloat(y,2) == 10 && SubFloat(w,0) == 4, "SIMD transpose lane order");
    float maxRcp = 0, maxRsqrt = 0;
    for (int i = 1; i <= 20000; ++i) {
        volatile float f = float(i) / 113.0f;
        const float rcp = _mm_cvtss_f32(ReciprocalSIMD(_mm_set1_ps(f)));
        const float rsqrt = _mm_cvtss_f32(ReciprocalSqrtSIMD(_mm_set1_ps(f)));
        maxRcp = std::fmax(maxRcp, std::fabs(rcp * f - 1));
        maxRsqrt = std::fmax(maxRsqrt, std::fabs(rsqrt * std::sqrt(f) - 1));
    }
    // SSE estimates are approximate too: compare against its architectural
    // relative-error bound rather than promising identical reciprocal bits.
    require(maxRcp <= 0.0003663f && maxRsqrt <= 0.0003663f, "Reciprocal/rsqrt error bound");
    volatile float tiny = 0x1p-13f;
    const float a = 1 + tiny, b = 1 - tiny;
    const auto separate = MaddSIMD(_mm_set1_ps(a), _mm_set1_ps(b), _mm_set1_ps(-1));
    require(bits(_mm_cvtss_f32(separate)) == 0, "Multiply/add must not contract into FMA");
    MathLib_Init();
    Vector v(3,4,0);
    require(std::fabs(VectorNormalize(v) - 5) < 0.00001f && std::fabs(v.x - 0.6f) < 0.00001f,
            "Linked mathlib vector normalization");
    Vector zero(0,0,0);
    require(VectorNormalize(zero) == 0 && zero == Vector(0,0,0), "Zero vector normalization");

    uint32_t random = 0x31415926;
    uint64_t hash = 14695981039346656037ull;
    auto next = [&] { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    for (int sample = 0; sample < 4096; ++sample) {
        alignas(16) float a[4], b[4], lanes[4];
        for (int lane = 0; lane < 4; ++lane) {
            const uint32_t av = next(), bv = next();
            a[lane] = fromBits((av & 0x807fffffu) | ((70 + av % 100) << 23));
            b[lane] = fromBits((bv & 0x807fffffu) | ((70 + bv % 100) << 23));
        }
        const auto av = _mm_load_ps(a), bv = _mm_load_ps(b);
        for (auto value : {AddSIMD(av,bv), SubSIMD(av,bv), MulSIMD(av,bv), DivSIMD(av,bv),
                           MaddSIMD(av,bv,av), _mm_shuffle_ps(av,bv,_MM_SHUFFLE(1,3,0,2))}) {
            _mm_store_ps(lanes, value);
            for (float lane : lanes) { hash ^= bits(lane); hash *= 1099511628211ull; }
        }
    }
    printf("SIMD_EXACT_HASH: %016llx\n", static_cast<unsigned long long>(hash));
    printf("SIMD_PASS: min/max special values, rounding modes, transpose, separate mul/add; rcp=%g rsqrt=%g\n", maxRcp, maxRsqrt);
}

void foundationChecks() {
    const auto& cpu = GetCPUInformation();
    require(cpu.m_nLogicalProcessors > 0 && cpu.m_nPhysicalProcessors > 0, "CPU topology");
    require(cpu.m_szProcessorID && cpu.m_szProcessorBrand && cpu.m_Speed > 0, "CPU identity and timer frequency");
    timespec start{}, finish{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    const uint64 before = Plat_Rdtsc();
    ThreadSleep(30);
    const uint64 after = Plat_Rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &finish);
    const double elapsed = finish.tv_sec - start.tv_sec + (finish.tv_nsec - start.tv_nsec) * 1e-9;
    require(std::fabs(double(after - before) / cpu.m_Speed - elapsed) < 0.003, "Timer counter/frequency agreement");
    void* allocation = g_pMemAlloc->Alloc(4096);
    require(allocation != nullptr && uintptr_t(allocation) % 16 == 0, "Shared allocator alignment");
    memset(allocation, 0xa5, 4096);
    g_pMemAlloc->Free(allocation);
    CThreadFastMutex mutex;
    int guarded = 0;
    int32 atomicCount = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (int n = 0; n < 25000; ++n) {
            mutex.Lock(); ++guarded; mutex.Unlock();
            ThreadInterlockedIncrement(&atomicCount);
        }
    });
    for (auto& thread : workers) thread.join();
    require(guarded == 100000 && atomicCount == 100000, "Mutex and atomic contention");
    CThreadEvent event;
    int payload = 0;
    std::thread publisher([&] { payload = 73; event.Set(); });
    require(event.Wait(2000) && payload == 73, "Event publishes writes across threads");
    publisher.join();
    CUtlBuffer buffer(0, 0, CUtlBuffer::TEXT_BUFFER);
    buffer.PutString("\"root\" { \"value\" \"73\" }");
    KeyValues* values = new KeyValues("root");
    require(values->LoadFromBuffer("platform-check", static_cast<const char*>(buffer.Base())) && values->GetInt("value") == 73,
            "tier1/vstdlib KeyValues round trip");
    values->deleteThis();
    wchar_t unicode[32]; char utf8[64];
    V_UTF8ToUnicode("CSGO 安卓", unicode, sizeof(unicode));
    V_UnicodeToUTF8(unicode, utf8, sizeof(utf8));
    require(strcmp(utf8, "CSGO 安卓") == 0, "UTF8/wchar conversion");
    printf("FOUNDATION_PASS: cpu=%s cores=%u timer=%lld, allocator, threads, event, KeyValues, Unicode\n",
           cpu.m_szProcessorID, unsigned(cpu.m_nLogicalProcessors), (long long)cpu.m_Speed);
}

void filesystemChecks(const char* resources, const char* temporary) {
    auto* cvarModule = Sys_LoadModule("libvstdlib_client.so");
    require(cvarModule != nullptr, "Load vstdlib by installed soname");
    auto cvarFactory = Sys_GetFactory(cvarModule);
    auto* module = Sys_LoadModule("filesystem_stdio");
    require(module != nullptr, "Load filesystem by engine module name");
    auto factory = Sys_GetFactory(module);
    require(factory && cvarFactory, "Module factory exports");
    auto* fs = static_cast<IFileSystem*>(factory(FILESYSTEM_INTERFACE_VERSION, nullptr));
    require(fs && fs->Connect(cvarFactory) && fs->Init() == INIT_OK, "Initialize real engine filesystem");
    fs->EnableWhitelistFileTracking(true, false, false);

    char root[512], directory[512], original[512], empty[512], large[512];
    snprintf(root, sizeof(root), "%s/csgo-filesystem.XXXXXX", temporary);
    require(mkdtemp(root) != nullptr, "Temporary filesystem fixture");
    snprintf(directory, sizeof(directory), "%s/MixedDir", root);
    require(mkdir(directory, 0700) == 0, "Mixed-case directory");
    snprintf(original, sizeof(original), "%s/HudAnimations.txt", directory);
    snprintf(empty, sizeof(empty), "%s/empty.txt", directory);
    snprintf(large, sizeof(large), "%s/large.bin", directory);
    const char expected[] = "animation fixture\n";
    FILE* out = fopen(original, "wb");
    require(out && fwrite(expected, 1, sizeof(expected)-1, out) == sizeof(expected)-1, "Fixture content");
    fclose(out);
    out = fopen(empty, "wb");
    require(out != nullptr, "Empty fixture");
    fclose(out);
    fs->AddSearchPath(root, "TEST");
    const char* names[] = {"MixedDir/HudAnimations.txt", "mixeddir/hudanimations.txt", "MIXEDDIR/HUDANIMATIONS.TXT"};
    for (const char* name : names) {
        auto file = fs->Open(name, "rb", "TEST");
        require(file != nullptr, name);
        require(fs->Size(file) == sizeof(expected)-1, "Size from open handle");
        char bytes[sizeof(expected)]{};
        require(fs->Read(bytes, sizeof(expected)-1, file) == sizeof(expected)-1 &&
                memcmp(bytes, expected, sizeof(expected)-1) == 0, "Mixed-case file content");
        fs->Close(file);
        require(fs->FileExists(name, "TEST") && fs->Size(name, "TEST") == sizeof(expected)-1,
                "Case-insensitive metadata agrees with open");
    }
    auto file = fs->Open("mixeddir/EMPTY.TXT", "rb", "TEST");
    require(file && fs->Size(file) == 0, "Empty file size");
    fs->Close(file);
    require(!fs->Open("mixeddir/missing.txt", "rb", "TEST") && !fs->FileExists("mixeddir/missing.txt", "TEST"),
            "Missing file returns failure");

    // Sparse fixture: exercise signed 32-bit boundary without copying gigabytes.
    const uint32_t largeSize = 0x80000040u;
    int fd = open(large, O_CREAT | O_RDWR | O_EXCL, 0600);
    require(fd >= 0 && pwrite(fd, "Z", 1, off_t(largeSize)-1) == 1, "Sparse 64-bit-offset fixture");
    close(fd);
    file = fs->Open("MixedDir/large.bin", "rb", "TEST");
    require(file && fs->Size(file) == largeSize, "File size above 2 GiB");
    fs->Seek(file, -1, FILESYSTEM_SEEK_TAIL);
    char tail = 0;
    require(fs->Tell(file) == largeSize-1 && fs->Read(&tail, 1, file) == 1 && tail == 'Z', "Seek/read above 2 GiB");
    fs->Close(file);

    char asyncBytes[sizeof(expected)]{};
    FileAsyncRequest_t request;
    request.pszFilename = names[1]; request.pszPathID = "TEST";
    request.pData = asyncBytes; request.nBytes = sizeof(expected)-1;
    FSAsyncControl_t control{};
    require(fs->AsyncRead(request, &control) >= FSASYNC_OK && control, "Queue asynchronous file read");
    require(fs->AsyncFinish(control, true) == FSASYNC_OK && memcmp(asyncBytes, expected, sizeof(expected)-1) == 0,
            "Asynchronous read completes with correct bytes");
    fs->AsyncRelease(control);

    char game[512], vpk[512];
    snprintf(game, sizeof(game), "%s/csgo", resources);
    snprintf(vpk, sizeof(vpk), "%s/csgo/pak01_dir.vpk", resources);
    fs->AddSearchPath(game, "GAME");
    fs->AddVPKFile(vpk);
    CUtlBuffer texture;
    require(fs->ReadFile("materials/DEV/Dev_MeasureGeneric01b.vtf", "GAME", texture), "Read original VTF through VPK");
    require(texture.TellPut() == 11128 && CRC32_ProcessSingleBuffer(texture.Base(), texture.TellPut()) == 0xb53a30e6u,
            "VPK archive offset, payload and CRC match Linux resource baseline");
    auto* vtf = CreateVTFTexture();
    require(vtf && vtf->Unserialize(texture) && vtf->Width() == 128 && vtf->Height() == 128 && vtf->MipCount() == 8 &&
            vtf->Format() == IMAGE_FORMAT_DXT1, "Original VTF parser and header layout");
    const uint32_t mipCrc[] = {0xc1031ee2u, 0xaf258944u, 0x8f99acf8u, 0xb87f6772u,
                              0x04ff7727u, 0x2c22cb0au, 0x51e7bd49u, 0xd84ee8f5u};
    for (int mip = 0; mip < vtf->MipCount(); ++mip)
        require(CRC32_ProcessSingleBuffer(vtf->ImageData(0,0,mip), vtf->ComputeMipSize(mip)) == mipCrc[mip],
                "VTF mip bytes match the independently parsed resource baseline");
    DestroyVTFTexture(vtf);
#if defined(USE_DXVK_NATIVE)
    require(uint32_t(D3DFMT_DXT1) == 0x31545844u && uint32_t(D3DFMT_DXT5) == 0x35545844u,
            "Native D3D9 uses standard FourCC values");
    for (ImageFormat format : {IMAGE_FORMAT_RGBA8888, IMAGE_FORMAT_RGBX8888, IMAGE_FORMAT_BGRA8888,
            IMAGE_FORMAT_DXT1, IMAGE_FORMAT_DXT3, IMAGE_FORMAT_DXT5, IMAGE_FORMAT_RGBA16161616F,
            IMAGE_FORMAT_RG1616F, IMAGE_FORMAT_RG3232F, IMAGE_FORMAT_RGBA1010102, IMAGE_FORMAT_BGRA1010102,
            IMAGE_FORMAT_R16F, IMAGE_FORMAT_D32})
        require(ImageLoader::D3DFormatToImageFormat(ImageLoader::ImageFormatToD3DFormat(format)) == format,
                "Source/native D3D9 image format round trip");
#endif
    static_assert(sizeof(BSPHeader_t) == 1036, "BSP header ABI");
    BSPHeader_t header{};
    file = fs->Open("maps/de_dust2.bsp", "rb", "GAME");
    require(file && fs->Read(&header, sizeof(header), file) == sizeof(header), "Read real Dust II BSP header");
    const uint64_t mapSize = fs->Size(file);
    require(header.ident == IDBSPHEADER && header.m_nVersion == 21, "BSP identity and version");
    for (const auto& lump : header.lumps)
        require(lump.fileofs >= 0 && lump.filelen >= 0 && uint64_t(lump.fileofs) + lump.filelen <= mapSize,
                "BSP lump layout and file range");
    fs->Close(file);

    fs->Shutdown();
    fs->Disconnect();
    Sys_UnloadModule(module);
    Sys_UnloadModule(cvarModule);
    unlink(original); unlink(empty); unlink(large); rmdir(directory); rmdir(root);
    printf("FILESYSTEM_PASS: factories, mixed-case paths, empty/missing files, 64-bit offsets, async, VPK/VTF mip CRCs, BSP=%llu bytes\n",
           static_cast<unsigned long long>(mapSize));
}
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    require(std::setlocale(LC_CTYPE, "C.UTF-8") != nullptr, "UTF-8 locale for engine text conversion");
    CommandLine()->CreateCmdLine(argc, argv);
    foundationChecks();
    simdChecks();
    if (argc > 2) {
        filesystemChecks(argv[1], argv[2]);
        filesystemChecks(argv[1], argv[2]); // Recreate async workers and module state after shutdown.
    }
    puts("PLATFORM_CHECK_PASS");
    return 0;
}
