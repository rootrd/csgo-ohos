// Owner-only interaction hints. The server still executes the ordinary +use rules.
#pragma once

namespace mobile
{
enum InteractionKind
{
    NoInteraction = 0,
    PickUpWeapon,
    OpenDoor,
    CloseDoor,
    FollowChicken,
    ReleaseChicken,
    RescueHostage,
    DefuseBomb,
    UseEntity,
    InteractionKindCount
};
}
