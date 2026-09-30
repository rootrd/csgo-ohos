// Exercise the imported physics implementation through the engine's public ABI.
#include "vphysics_interface.h"
#include "vcollide.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "PHYSICS_FAIL: %s\n", message); std::exit(1); }
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.002f * (1 + std::fabs(b)); }

int main() {
    void *module = dlopen("libvphysics_client.so", RTLD_NOW | RTLD_LOCAL);
    if (!module) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto factory = reinterpret_cast<CreateInterfaceFn>(dlsym(module, "CreateInterface"));
    require(factory != nullptr, "physics factory");
    auto *collision = static_cast<IPhysicsCollision *>(factory(VPHYSICS_COLLISION_INTERFACE_VERSION, nullptr));
    require(collision != nullptr, "physics collision interface ABI");
    CPhysCollide *box = collision->BBoxToCollide(Vector(-3, -2, -1), Vector(5, 7, 11));
    require(box != nullptr, "create asymmetric box");
    const Vector fractions(0.25f, 0.5f, 0.75f);
    collision->CollideSetOrthographicAreas(box, fractions);
    Vector initialMin, initialMax, initialCenter;
    collision->CollideGetAABB(&initialMin, &initialMax, box, Vector(0,0,0), QAngle(0,0,0));
    collision->CollideGetMassCenter(box, &initialCenter);
    const float volume = collision->CollideVolume(box);
    require(volume > 0, "nonzero collision volume");
    CPhysCollide *solids[] = {box, nullptr};
    char descriptor[] = {'a', '\0', 'b', '\0'};
    vcollide_t source{};
    source.solidCount = 2;
    source.solids = solids;
    source.pKeyValues = descriptor;
    source.descSize = sizeof(descriptor);
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        vcollide_t result{};
        collision->DuplicateAndScale(&result, &source, scale);
        require(result.solidCount == 2 && result.solids[0] && !result.solids[1], "preserve disabled solids");
        require(result.solids[0] != box && result.pKeyValues != descriptor, "independent ownership");
        require(result.descSize == sizeof(descriptor) && !std::memcmp(result.pKeyValues, descriptor, sizeof(descriptor)),
                "preserve descriptor bytes beyond the first NUL");
        Vector minimum, maximum, center;
        collision->CollideGetAABB(&minimum, &maximum, result.solids[0], Vector(0,0,0), QAngle(0,0,0));
        collision->CollideGetMassCenter(result.solids[0], &center);
        Vector resultFractions = collision->CollideGetOrthographicAreas(result.solids[0]);
        for (int axis = 0; axis < 3; ++axis) {
            require(near(minimum[axis], initialMin[axis] * scale) && near(maximum[axis], initialMax[axis] * scale), "scaled bounds");
            require(near(center[axis], initialCenter[axis] * scale), "scaled mass center");
            require(near(resultFractions[axis], fractions[axis]), "unitless drag coverage must not scale");
        }
        require(near(collision->CollideVolume(result.solids[0]), volume * scale * scale * scale), "cubic volume scaling");
        collision->VCollideUnload(&result);
    }
    require(near(collision->CollideVolume(box), volume), "source collision remains unchanged");
    collision->DestroyCollide(box);
    std::puts("PHYSICS_PASS: collision ABI, bounds, center, volume, drag fractions, descriptors, ownership");
    return 0;
}
