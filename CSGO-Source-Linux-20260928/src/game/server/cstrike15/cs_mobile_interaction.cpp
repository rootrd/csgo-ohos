#include "cbase.h"
#include "cs_player.h"
#include "cs_gamerules.h"
#include "weapon_csbase.h"
#include "weapon_c4.h"
#include "BasePropDoor.h"
#include "doors.h"
#include "Effects/chicken.h"
#include "inputsystem/mobilecontext.h"
#include "tier0/memdbgon.h"

// The client cannot query ObjectCaps, door locks or chicken ownership. Publish
// only the local player's usable target, using the same search as PlayerUse.
void CCSPlayer::UpdateMobileInteraction()
{
    if (IsBot() || !IsAlive() || IsObserver() || IsPlayerGhost())
    {
        m_hMobileUseEntity = NULL;
        m_iMobileUseAction = mobile::NoInteraction;
        return;
    }
    if (gpGlobals->curtime < m_flNextMobileContextUpdate) return;
    m_flNextMobileContextUpdate = gpGlobals->curtime + .05f;
    const char *enabled = engine->GetClientConVarValue(entindex(), "cl_mobile_context");
    if (!enabled || atoi(enabled) == 0)
    {
        m_hMobileUseEntity = NULL;
        m_iMobileUseAction = mobile::NoInteraction;
        return;
    }

    CBaseEntity *target = NULL;
    if ((m_bIsDefusing || m_bIsGrabbingHostage || m_iBlockingUseActionInProgress) && m_hMobileUseEntity.Get())
    {
        CConfigurationForHighPriorityUseEntity_t cfg;
        if (GetUseConfigurationForHighPriorityUseEntity(m_hMobileUseEntity.Get(), cfg)
            && cfg.m_pEntity && cfg.UseByPlayerNow(this, cfg.k_EPlayerUseType_Progress))
            target = m_hMobileUseEntity.Get();
    }
    // A hint query must not emit pickup-failure events or change last-use state.
    if (!target) target = FindUseEntityInternal(false);
    mobile::InteractionKind kind = mobile::NoInteraction;
    if (target)
    {
        if (CPlantedC4 *bomb = dynamic_cast<CPlantedC4 *>(target))
        {
            if (GetTeamNumber() == TEAM_CT && bomb->IsBombActive() && !bomb->m_bCannotBeDefused
                && (!bomb->GetDefuser() || bomb->GetDefuser() == this) && (GetFlags() & FL_ONGROUND))
                kind = mobile::DefuseBomb;
        }
        else if (CWeaponCSBase *weapon = dynamic_cast<CWeaponCSBase *>(target))
        {
            const AcquireResult::Type result = CanAcquire(weapon->GetEconItemView(), AcquireMethod::PickUp);
            const bool typeAllowed = IsPrimaryOrSecondaryWeapon(weapon->GetWeaponType())
                || weapon->GetWeaponType() == WEAPONTYPE_TASER || CSGameRules()->IsPlayingSurvival();
            if (!weapon->GetOwner() && weapon->CanBePickedUpBy(this, true) && typeAllowed
                && (result == AcquireResult::Allowed || result == AcquireResult::AlreadyOwned))
                kind = mobile::PickUpWeapon;
        }
        else if (CBasePropDoor *door = dynamic_cast<CBasePropDoor *>(target))
        {
            if (!door->IsDoorLocked())
            {
                if ((door->IsDoorClosed() || door->IsDoorClosing() || door->IsDoorAjar()) && door->DoorCanOpen())
                    kind = mobile::OpenDoor;
                else if ((door->IsDoorOpen() || door->IsDoorOpening()) && door->HasSpawnFlags(SF_DOOR_USE_CLOSES)
                    && door->DoorCanClose(false))
                    kind = mobile::CloseDoor;
            }
        }
        else if (CBaseDoor *door = dynamic_cast<CBaseDoor *>(target))
        {
            if (!door->m_bLocked)
            {
                if (door->m_toggle_state == TS_AT_BOTTOM) kind = mobile::OpenDoor;
                else if (door->m_toggle_state == TS_AT_TOP && door->HasSpawnFlags(SF_DOOR_NO_AUTO_RETURN)) kind = mobile::CloseDoor;
            }
        }
        else if (CChicken *chicken = dynamic_cast<CChicken *>(target))
        {
            if (chicken->CanBeUsedBy(this))
                kind = chicken->GetLeader() == this ? mobile::ReleaseChicken : mobile::FollowChicken;
        }
        else if (FClassnameIs(target, "hostage_entity"))
        {
            if (GetTeamNumber() == TEAM_CT && target->IsAlive()) kind = mobile::RescueHostage;
        }
        else kind = mobile::UseEntity;
    }
    m_hMobileUseEntity = kind == mobile::NoInteraction ? NULL : target;
    m_iMobileUseAction = kind;
}
