#include "mobile/mobile_touch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace mobile;

static WeaponState item(int entity, int count=1)
{
    WeaponState w={};w.entity=entity;w.token=uint32_t(0x10000+entity);
    w.count=count;w.selectable=true;w.clip=w.reserve=-1;return w;
}
static void touch(TouchControls &t, MobileTouchType type, int64_t finger, int control)
{
    const Rect r=t.Bounds(control);t.Event({type,finger,r.x+r.w*.5f,r.y+r.h*.5f});
}
int main(int argc,char **argv)
{
    if(argc>1&&!std::strcmp(argv[1],"--layout"))
    {
        TouchControls layout;
        for(int i=0;i<ControlCount;++i)
        {
            const Rect r=layout.Bounds(i);
            std::printf("%s\t%s\t%s\t%.6f\t%.6f\t%.6f\t%.6f\n",Definition(i).key,Definition(i).label,Definition(i).icon,r.x,r.y,r.w,r.h);
        }
        return 0;
    }
    PlayerState p={};p.activeControl=Primary;
    p.alive=p.canMove=p.canAttack=p.canBuy=p.canReload=p.canDrop=true;
    p.weapons[Primary]=item(10);p.weapons[Primary].clip=17;p.weapons[Primary].reserve=90;p.weapons[Primary].selected=true;
    p.weapons[Pistol]=item(11);p.weapons[Knife]=item(12);p.weapons[Taser]=item(13);
    p.weapons[Smoke]=item(14);p.weapons[Flash]=item(15,2);p.weapons[HEGrenade]=item(16);
    p.weapons[Molotov]=item(17);p.weapons[Incendiary]=item(18);p.weapons[Decoy]=item(19);
    InteractionState target={};
    HudState hud=BuildHud(p,target);
    assert(!hud.controls[Use].visible&&!hud.controls[Objective].visible&&!hud.controls[Aim].visible);
    assert(hud.controls[Primary].equipped&&hud.controls[Primary].clip==17&&hud.controls[Primary].reserve==90);
    const int equipment[]={Pistol,Knife,Taser,Smoke,Flash,HEGrenade,Molotov,Incendiary,Decoy};
    for(int i:equipment)assert(hud.controls[i].visible&&hud.controls[i].action==EquipAction&&hud.controls[i].entity==p.weapons[i].entity);
    assert(hud.controls[Flash].count==2);
    // Selection follows the actual equipped item, including while switching is
    // blocked. Action feedback must never masquerade as an equipped weapon.
    assert(!hud.controls[Pistol].equipped&&!hud.controls[Aim].equipped);
    p.weapons[Primary].selected=false;p.weapons[Pistol].selected=true;p.activeControl=Pistol;
    p.weapons[Pistol].selectable=false;hud=BuildHud(p,target);
    assert(!hud.controls[Primary].equipped&&hud.controls[Pistol].equipped&&!hud.controls[Pistol].enabled);
    p.weapons[Pistol].selected=false;p.weapons[Pistol].selectable=true;
    p.weapons[Primary].selected=true;p.activeControl=Primary;hud=BuildHud(p,target);
    assert(std::strcmp(hud.controls[Flash].icon,hud.controls[Smoke].icon));
    assert(std::strcmp(hud.controls[Molotov].icon,hud.controls[Incendiary].icon));
    p.weapons[Smoke].count=0;hud=BuildHud(p,target);
    assert(!hud.controls[Smoke].visible&&hud.controls[Flash].visible);
    p.weapons[Smoke]=item(14);
    p.canBuy=false;hud=BuildHud(p,target);assert(!hud.controls[Buy].visible);

    // A consumed item must cancel its queued tap and release its owned finger.
    TouchControls input;input.SetControls(BuildHud(p,target));
    touch(input,MOBILE_TOUCH_DOWN,1,Move);
    touch(input,MOBILE_TOUCH_DOWN,2,Smoke);
    p.weapons[Smoke].count=0;input.SetControls(BuildHud(p,target));
    assert(!input.Held(Smoke)&&input.Held(Move)&&!(input.TakePresses()&ControlBit(Smoke)));
    p.weapons[Smoke]=item(22);input.SetControls(BuildHud(p,target));
    touch(input,MOBILE_TOUCH_MOVE,2,Smoke);assert(!input.Held(Smoke));
    touch(input,MOBILE_TOUCH_UP,2,Smoke);
    touch(input,MOBILE_TOUCH_DOWN,2,Smoke);assert(input.Held(Smoke));

    // Hidden controls are genuinely empty look space, not invisible hit targets.
    input.Reset();p.weapons[Smoke].count=0;input.SetControls(BuildHud(p,target));
    Rect smoke=input.Bounds(Smoke);
    float sx=smoke.x+smoke.w*.5f,sy=smoke.y+smoke.h*.5f;
    assert(input.Hit(sx,sy)<0);
    input.Event({MOBILE_TOUCH_DOWN,3,sx,sy});input.Event({MOBILE_TOUCH_MOVE,3,sx+.04f,sy});
    float x,y;input.TakeLook(x,y);assert(x>0);

    // Scope switches are taps; grenade underhand and knife heavy attacks are holds.
    p.secondary=ScopeSecondary;hud=BuildHud(p,target);
    assert(hud.controls[Aim].action==SecondaryTapAction);
    p.scoped=true;hud=BuildHud(p,target);assert(hud.controls[Aim].active&&!hud.controls[Aim].equipped);
    p.secondary=StabSecondary;hud=BuildHud(p,target);assert(hud.controls[Aim].action==SecondaryHoldAction);
    p.secondary=ThrowSecondary;p.activeControl=Flash;hud=BuildHud(p,target);
    assert(hud.controls[Aim].action==SecondaryHoldAction&&!std::strcmp(hud.controls[Fire].icon,"throw"));
    p.activeControl=Primary;p.secondary=NoSecondary;p.reloading=true;hud=BuildHud(p,target);
    assert(hud.controls[Reload].visible&&!hud.controls[Reload].enabled&&hud.controls[Reload].active);
    p.reloading=false;

    // C4 carry/plant/defuse share geometry, but changing verbs never retargets a hold.
    p.weapons[Objective]=item(30);hud=BuildHud(p,target);
    assert(hud.controls[Objective].visible&&hud.controls[Objective].action==EquipAction);
    p.canPlant=true;hud=BuildHud(p,target);
    assert(hud.controls[Objective].action==PlantAction&&!hud.controls[Use].visible);
    assert(!hud.controls[Objective].equipped);
    p.weapons[Primary].selected=false;p.weapons[Objective].selected=true;hud=BuildHud(p,target);
    assert(hud.controls[Objective].equipped&&!hud.controls[Objective].active);
    input.Reset();input.SetControls(hud);touch(input,MOBILE_TOUCH_DOWN,4,Objective);
    touch(input,MOBILE_TOUCH_DOWN,5,Fire);
    p.activeControl=Objective;p.planting=true;p.progress=.35f;input.SetControls(BuildHud(p,target));
    assert(input.Held(Objective)&&!input.Held(Fire));
    assert(BuildHud(p,target).controls[Objective].progress==.35f&&BuildHud(p,target).controls[Objective].equipped);
    p.planting=p.canPlant=false;p.weapons[Objective]={};p.activeControl=Primary;p.weapons[Primary].selected=true;
    target={DefuseBomb,0x20042,42,.6f};hud=BuildHud(p,target);input.SetControls(hud);
    assert(hud.controls[Objective].action==UseAction&&hud.controls[Objective].entity==42&&!hud.controls[Use].visible);
    assert(hud.controls[Objective].active&&!hud.controls[Objective].equipped);
    assert(!input.Held(Objective)&&!(input.TakePresses()&ControlBit(Objective)));

    // Target identity includes the handle serial, and a door/chicken verb change
    // cancels only the interaction finger while preserving movement.
    const InteractionKind kinds[]={PickUpWeapon,OpenDoor,CloseDoor,FollowChicken,ReleaseChicken,RescueHostage,UseEntity};
    for(InteractionKind kind:kinds)
    {
        target={kind,0x30042,42,0};hud=BuildHud(p,target);
        assert(hud.controls[Use].visible&&hud.controls[Use].action==UseAction&&!hud.controls[Objective].visible);
        input.Reset();input.SetControls(hud);
        touch(input,MOBILE_TOUCH_DOWN,1,Move);touch(input,MOBILE_TOUCH_DOWN,2,Use);
        target.token+=0x10000;input.SetControls(BuildHud(p,target));
        assert(!input.Held(Use)&&input.Held(Move)&&!(input.TakePresses()&ControlBit(Use)));
    }
    target={OpenDoor,0x40042,42,0};input.Reset();input.SetControls(BuildHud(p,target));
    touch(input,MOBILE_TOUCH_DOWN,2,Use);target.kind=CloseDoor;input.SetControls(BuildHud(p,target));
    assert(!input.Held(Use));
    target={};input.SetControls(BuildHud(p,target));assert(!input.TakePresses());

    // Death/observer mode cannot retain weapons, attack, movement or stale taps.
    input.Reset();input.SetControls(BuildHud(p,target));
    touch(input,MOBILE_TOUCH_DOWN,1,Move);touch(input,MOBILE_TOUCH_DOWN,2,Fire);
    p.alive=false;p.observing=true;hud=BuildHud(p,target);input.SetControls(hud);
    assert(!input.HeldMask()&&!input.TakePresses());
    assert(hud.controls[Fire].action==NextSpectatorAction&&hud.controls[FireLeft].action==PreviousSpectatorAction);
    for(int i=0;i<ControlCount;++i)if(IsInventoryControl(i))assert(!hud.controls[i].visible);
    assert(hud.controls[Menu].visible&&hud.controls[Score].visible);
    assert(BuildHud(p,target,true).Visible()==AllControls());
    for(int i=0;i<ControlCount;++i)if(IsInventoryControl(i))assert(IsGameIcon(Definition(i).icon));

    // Equipment beyond bit 31 remains independent (there are more than 32 controls).
    p.alive=true;p.weapons[Melee]=item(80);p.weapons[Diversion]=item(81);
    input.Reset();input.SetControls(BuildHud(p,target));
    touch(input,MOBILE_TOUCH_DOWN,8,Diversion);
    assert(input.TakePresses()==ControlBit(Diversion));
    input.Event({MOBILE_TOUCH_RESET,0,0,0});assert(!input.HeldMask());
    std::puts("MOBILE_HUD_PASS: inventory, individual grenades/taser, ammo, context identities, plant/defuse, scope modes, hidden hits, death, observer and 64-bit input");
}
