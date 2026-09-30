// The touch HUD has exactly two inputs: the local player and their usable target.
// Keep this policy independent of the engine so transitions can be tested on both CPUs.
#pragma once
#include "inputsystem/mobilecontext.h"
#include <stdint.h>

namespace mobile
{
enum Control
{
    Move, Fire, Aim, Jump, Duck, Reload, Use, Walk, FireLeft,
    Primary, Pistol, Knife, HEGrenade, Score, Buy, Menu, Team,
    Smoke, Flash, Molotov, Incendiary, Decoy, Taser, Objective, Drop,
    Healthshot, Shield, TagGrenade, BreachCharge, BumpMine, Snowball,
    Tablet, Fists, Melee, FireBomb, FragGrenade, Diversion, ControlCount
};
static_assert(ControlCount < 64, "Touch controls must fit the input mask");
typedef uint64_t ControlMask;
inline ControlMask ControlBit(int i) { return ControlMask(1) << i; }
inline ControlMask AllControls() { return (ControlBit(ControlCount) - 1); }

struct ControlDefinition { const char *key, *label, *icon; };
inline const ControlDefinition &Definition(int i)
{
    static const ControlDefinition definitions[ControlCount] = {
        {"move", "移动", "move"}, {"fire", "开火", "fire"}, {"aim", "瞄准 / 副攻击", "scope"},
        {"jump", "跳跃", "jump"}, {"duck", "蹲下 / 起身", "crouch"}, {"reload", "换弹", "reload"},
        {"use", "情境交互", "use"}, {"walk", "静步", "walk"}, {"fire_left", "左手开火", "fire"},
        {"primary", "主武器", "equipment/ak47"}, {"pistol", "手枪", "equipment/hkp2000"}, {"knife", "匕首", "equipment/knife"},
        {"grenade", "高爆手雷", "equipment/hegrenade"}, {"score", "战绩", "score"}, {"buy", "购买", "buy"},
        {"menu", "菜单", "menu"}, {"team", "换队", "team"},
        {"smoke", "烟雾弹", "equipment/smokegrenade"}, {"flash", "闪光弹", "equipment/flashbang"}, {"molotov", "燃烧瓶", "equipment/molotov"},
        {"incendiary", "燃烧弹", "equipment/incgrenade"}, {"decoy", "诱饵弹", "equipment/decoy"}, {"taser", "电击枪", "equipment/taser"},
        {"objective", "C4 / 安装 / 拆除", "equipment/c4"}, {"drop", "丢弃", "drop"},
        {"healthshot", "医疗针", "equipment/healthshot"}, {"shield", "盾牌", "ui/shield"}, {"tag", "战术探测弹", "equipment/tagrenade"},
        {"breach", "遥控炸药", "equipment/breachcharge"}, {"bumpmine", "弹射地雷", "equipment/breachcharge"}, {"snowball", "雪球", "equipment/snowball"},
        {"tablet", "平板", "equipment/tablet"}, {"fists", "拳头", "equipment/fists"}, {"melee", "近战武器", "equipment/axe"},
        {"firebomb", "火焰炸弹", "equipment/firebomb"}, {"frag", "破片手雷", "equipment/frag_grenade"}, {"diversion", "干扰装置", "equipment/diversion"}
    };
    return definitions[i >= 0 && i < ControlCount ? i : Move];
}
inline bool IsGameIcon(const char *icon)
{
    // Native icon keys contain a directory (equipment/ or ui/); local action
    // icons are simple names. The renderer resolves both through Panorama.
    for(const char *p=icon;*p;++p)if(*p=='/')return true;
    return false;
}

inline bool IsGrenadeControl(int i)
{
    return i == HEGrenade || i == Smoke || i == Flash || i == Molotov || i == Incendiary || i == Decoy
        || i == TagGrenade || i == Snowball || i == FireBomb || i == FragGrenade || i == Diversion;
}
inline bool IsInventoryControl(int i)
{
    return i == Primary || i == Pistol || i == Knife || i == Taser || i == Objective || IsGrenadeControl(i)
        || (i >= Healthshot && i <= Melee);
}
inline float ControlWidthRatio(int i)
{
    return i == Primary ? 1.85f : i == Pistol ? 1.45f : i == Knife ? 1.15f : 1.0f;
}

enum ActionKind
{
    NoAction, MoveAction, FireAction, SecondaryHoldAction, SecondaryTapAction, JumpAction,
    CrouchAction, ReloadAction, UseAction, WalkAction, EquipAction, PlantAction,
    ScoreAction, BuyAction, MenuAction, TeamAction, DropAction,
    PreviousSpectatorAction, NextSpectatorAction, SpectatorModeAction
};
enum SecondaryKind { NoSecondary, ScopeSecondary, BurstSecondary, SilencerSecondary, StabSecondary, ThrowSecondary, AlternateSecondary, ShieldSecondary, TabletSecondary };
struct WeaponState
{
    uint32_t token; // Full entity handle, including serial; an entity index can be reused.
    int entity, count, clip, reserve;
    bool selected, selectable;
};
struct PlayerState
{
    bool alive, observing, canMove, canAttack, canBuy, canReload, canDrop, canPlant;
    bool crouched, walking, scoped, reloading, planting, defusing;
    int activeControl;
    SecondaryKind secondary;
    float progress;
    WeaponState weapons[ControlCount];
};
struct InteractionState
{
    InteractionKind kind;
    uint32_t token;
    int entity;
    float progress;
};
struct ControlState
{
    // Equipped is inventory selection; active is a toggle or ongoing action.
    // In particular, selecting C4 must remain visible before arming starts.
    bool visible, enabled, active, equipped;
    ActionKind action;
    const char *icon, *label;
    uint32_t token;
    int entity, count, clip, reserve;
    float progress;
    InteractionKind interaction;

    uint64_t Binding() const { return (uint64_t(token) << 16) | (uint64_t(action) << 8) | interaction; }
};
struct HudState
{
    ControlState controls[ControlCount];
    ControlMask Visible() const
    {
        ControlMask result = 0;
        for (int i = 0; i < ControlCount; ++i) if (controls[i].visible) result |= ControlBit(i);
        return result;
    }
    ControlMask Enabled() const
    {
        ControlMask result = 0;
        for (int i = 0; i < ControlCount; ++i) if (controls[i].visible && controls[i].enabled) result |= ControlBit(i);
        return result;
    }
};
inline ControlState &Show(HudState &hud, int control, ActionKind action, bool enabled = true)
{
    ControlState &b = hud.controls[control];
    b.visible = true; b.enabled = enabled; b.action = action;
    return b;
}

inline HudState BuildHud(const PlayerState &p, const InteractionState &target, bool editor = false)
{
    HudState hud = {};
    for (int i = 0; i < ControlCount; ++i)
    {
        ControlState &b = hud.controls[i];
        b.icon = Definition(i).icon; b.label = Definition(i).label;
        b.clip = b.reserve = -1;
        if (editor) { b.visible = b.enabled = true; }
    }
    if (editor) return hud;
    Show(hud, Score, ScoreAction);
    Show(hud, Menu, MenuAction);
    Show(hud, Team, TeamAction);
    if (!p.alive)
    {
        if (p.observing)
        {
            ControlState &prev = Show(hud, FireLeft, PreviousSpectatorAction);
            prev.icon = "previous"; prev.label = "上一位";
            ControlState &next = Show(hud, Fire, NextSpectatorAction);
            next.icon = "next"; next.label = "下一位";
            ControlState &mode = Show(hud, Aim, SpectatorModeAction);
            mode.icon = "spectate"; mode.label = "观战视角";
        }
        return hud;
    }

    Show(hud, Move, MoveAction, p.canMove);
    Show(hud, Jump, JumpAction, p.canMove);
    Show(hud, Duck, CrouchAction).active = p.crouched;
    hud.controls[Duck].label = p.crouched ? "起身" : "蹲下";
    Show(hud, Walk, WalkAction, p.canMove).active = p.walking;
    if (p.canBuy) Show(hud, Buy, BuyAction);
    for (int i = 0; i < ControlCount; ++i)
    {
        const WeaponState &weapon = p.weapons[i];
        if (!IsInventoryControl(i) || !weapon.entity || weapon.count <= 0) continue;
        ControlState &b = Show(hud, i, EquipAction, weapon.selectable);
        b.token = weapon.token; b.entity = weapon.entity; b.equipped = weapon.selected;
        b.count = weapon.count; b.clip = weapon.clip; b.reserve = weapon.reserve;
    }
    if (p.activeControl >= 0 && p.activeControl < ControlCount && p.weapons[p.activeControl].entity)
    {
        const WeaponState &weapon = p.weapons[p.activeControl];
        for (int fireIndex = 0; fireIndex < 2; ++fireIndex)
        {
            const int i = fireIndex == 0 ? Fire : FireLeft;
            if (p.activeControl == Objective && !p.canPlant && !p.planting) continue;
            ControlState &b = Show(hud, i, FireAction, p.canAttack);
            b.token = weapon.token; b.entity = weapon.entity;
            if (IsGrenadeControl(p.activeControl)) { b.icon = "throw"; b.label = "投掷"; }
            else if (p.activeControl == Objective) { b.icon = "equipment/c4"; b.label = "安装"; }
            else if (p.activeControl == Healthshot) { b.icon = "equipment/healthshot"; b.label = "治疗"; }
            else if (p.activeControl == Knife || p.activeControl == Fists || p.activeControl == Melee) { b.icon = "slash"; b.label = "攻击"; }
        }
        if (p.secondary != NoSecondary)
        {
            const bool tap = p.secondary == ScopeSecondary || p.secondary == BurstSecondary || p.secondary == SilencerSecondary || p.secondary == TabletSecondary;
            ControlState &b = Show(hud, Aim, tap ? SecondaryTapAction : SecondaryHoldAction, p.canAttack);
            b.token = weapon.token; b.entity = weapon.entity; b.active = p.scoped;
            switch (p.secondary)
            {
            case ScopeSecondary: b.label = p.scoped ? "切换倍率" : "开镜"; break;
            case BurstSecondary: b.icon = "burst"; b.label = "射击模式"; break;
            case SilencerSecondary: b.icon = "silencer"; b.label = "消音器"; break;
            case StabSecondary: b.icon = "stab"; b.label = "重击"; break;
            case ThrowSecondary: b.icon = "underhand"; b.label = "轻抛"; break;
            case ShieldSecondary: b.icon = "ui/shield"; b.label = "举盾"; break;
            case TabletSecondary: b.icon = "equipment/tablet"; b.label = "切换视图"; break;
            default: b.icon = "fire"; b.label = "副攻击"; break;
            }
        }
        if (weapon.clip >= 0 && !IsGrenadeControl(p.activeControl) && p.activeControl != Objective && p.activeControl != Taser)
        {
            ControlState &b = Show(hud, Reload, ReloadAction, p.canReload && !p.reloading);
            b.token = weapon.token; b.entity = weapon.entity; b.active = p.reloading;
            b.label = p.reloading ? "换弹中" : "换弹";
        }
        if (p.canDrop)
        {
            ControlState &b = Show(hud, Drop, DropAction);
            b.token = weapon.token; b.entity = weapon.entity;
        }
    }

    // Carrying C4, planting and defusing deliberately share one authored position.
    if (p.weapons[Objective].entity)
    {
        ControlState &b = hud.controls[Objective];
        b.label = "C4"; b.icon = "equipment/c4";
        if (p.canPlant || p.planting)
        {
            Show(hud, Objective, PlantAction, p.canAttack || p.planting);
            b.icon = "equipment/c4"; b.label = "按住安装"; b.progress = p.progress; b.active = p.planting;
        }
    }
    if (target.kind != NoInteraction && target.entity)
    {
        const bool defuse = target.kind == DefuseBomb;
        ControlState &b = Show(hud, defuse ? Objective : Use, UseAction);
        b.token = target.token; b.entity = target.entity; b.interaction = target.kind;
        b.equipped = false;
        b.count = 0; b.clip = b.reserve = -1;
        b.progress = target.progress; b.active = b.progress > 0 || (defuse && p.defusing);
        switch (target.kind)
        {
        case PickUpWeapon: b.icon = "pickup"; b.label = "拾取"; break;
        case OpenDoor: b.icon = "door_open"; b.label = "开门"; break;
        case CloseDoor: b.icon = "door_close"; b.label = "关门"; break;
        case FollowChicken: b.icon = "chicken"; b.label = "跟随我"; break;
        case ReleaseChicken: b.icon = "chicken_release"; b.label = "停止跟随"; break;
        case RescueHostage: b.icon = "hostage"; b.label = "按住救援"; break;
        case DefuseBomb: b.icon = "equipment/defuser"; b.label = "按住拆除"; break;
        default: b.icon = "use"; b.label = "使用"; break;
        }
    }
    return hud;
}
inline bool IsFrameAction(ActionKind action)
{
    return action == CrouchAction || action == WalkAction || action == EquipAction || action == ScoreAction
        || action == BuyAction || action == MenuAction || action == TeamAction || action == DropAction
        || action == PreviousSpectatorAction || action == NextSpectatorAction || action == SpectatorModeAction;
}
}
