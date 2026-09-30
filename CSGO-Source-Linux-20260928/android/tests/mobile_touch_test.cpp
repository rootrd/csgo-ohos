#include "mobile/mobile_touch.h"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace mobile;
static void at(TouchControls &t, MobileTouchType type, int64_t id, int control)
{
    Rect r=t.Bounds(control); t.Event({type,id,r.x+r.w*.5f,r.y+r.h*.5f});
}
int main()
{
    TouchControls t;
    at(t,MOBILE_TOUCH_DOWN,1,Move); at(t,MOBILE_TOUCH_DOWN,2,Fire);
    Rect r=t.Bounds(Move);
    t.Event({MOBILE_TOUCH_MOVE,1,r.x+r.w,r.y});
    assert(t.Held(Fire) && t.MoveX()>0 && t.MoveY()>0);
    assert(std::fabs(t.MoveX()*t.MoveX()+t.MoveY()*t.MoveY()-1)<.001f);
    t.Event({MOBILE_TOUCH_DOWN,3,.54f,.49f}); t.Event({MOBILE_TOUCH_MOVE,3,.58f,.47f});
    float x,y; t.TakeLook(x,y); assert(x>0 && y<0);
    at(t,MOBILE_TOUCH_DOWN,4,Fire); at(t,MOBILE_TOUCH_UP,2,Fire); assert(t.Held(Fire));
    at(t,MOBILE_TOUCH_CANCEL,4,Fire); assert(!t.Held(Fire) && t.Held(Move));
    at(t,MOBILE_TOUCH_DOWN,5,Jump); at(t,MOBILE_TOUCH_UP,5,Jump);
    assert(t.TakePresses()&(1u<<Jump)); assert(!t.TakePresses());
    t.Event({MOBILE_TOUCH_RESET,0,0,0});
    at(t,MOBILE_TOUCH_DOWN,6,Fire);at(t,MOBILE_TOUCH_CANCEL,6,Fire);assert(!t.TakePresses());
    assert(!t.HeldMask() && t.MoveX()==0 && t.MoveY()==0);
    t.Event({MOBILE_TOUCH_MOVE,1,.2f,.3f}); assert(!t.HeldMask());
    t.TakeLook(x,y); assert(x==0 && y==0);
    t.SetViewport({2400,1080,.03f,.02f,.97f,.98f});
    t.buttons[Jump]={-100,200,900,-1}; t.Normalize(Jump); r=t.Bounds(Jump);
    assert(r.x>=.03f-.0001f && r.y+r.h<=.9801f && t.buttons[Jump].opacity>=.2f);
    t.buttons[Fire].x=std::numeric_limits<float>::quiet_NaN(); t.Normalize(Fire);
    assert(std::isfinite(t.buttons[Fire].x));
    at(t,MOBILE_TOUCH_DOWN,7,Fire); t.SetViewport({1920,1080,0,0,1,1}); assert(!t.HeldMask());
    t.Event({MOBILE_TOUCH_UP,7,0,0}); assert(!t.HeldMask());
    t.Defaults();
    at(t,MOBILE_TOUCH_DOWN,9,Move); at(t,MOBILE_TOUCH_DOWN,9,Fire); assert(t.Held(Move) && !t.Held(Fire));
    t.Drag(Move,-100,100); r=t.Bounds(Move); assert(r.x>=-.0001f && r.y+r.h<=1.0001f);
    const MobileViewport views[]={{1920,1080,0,0,1,1},{3168,1440,.025f,0,1,1},{1280,800,0,0,1,1},{1280,960,.03f,.02f,.97f,.98f}};
    for(const MobileViewport &view:views)
    {
        TouchControls large;large.SetViewport(view);large.Defaults();
        assert(large.buttons[Fire].size>=.15f&&large.buttons[Move].size>=.23f);
        for(int i=0;i<ControlCount;++i)
        {
            Rect b=large.Bounds(i);
            assert(b.x>=view.left-.0001f&&b.y>=view.top-.0001f);
            assert(b.x+b.w<=view.right+.0001f&&b.y+b.h<=view.bottom+.0001f);
            assert(large.Hit(b.x+b.w*.5f,b.y+b.h*.5f)==i);
        }
        Rect left=large.Bounds(Move),right=large.Bounds(Fire);
        assert((left.x-view.left)*view.width/view.height<.04f);
        assert((view.right-right.x-right.w)*view.width/view.height<.04f);
        large.Drag(Jump,100,100);large.SetViewport({1280,960,.05f,.02f,.95f,.98f});
        Rect resized=large.Bounds(Jump);assert(resized.x+resized.w<=.9501f&&resized.y+resized.h<=.9801f);
    }
    TouchControls layout;
    layout.SetViewport({1920,1080,0,0,1,1});
    layout.Defaults();
    const float wideFire = DefaultButton(Fire, 3168.0f/1440.0f).x;
    layout.SetViewport({3168,1440,0,0,1,1});
    assert(std::fabs(layout.buttons[Fire].x-wideFire)<0.0001f);
    layout.buttons[Jump].x = 0.42f;
    layout.SetViewport({1280,720,0,0,1,1});
    assert(std::fabs(layout.buttons[Jump].x-0.42f)<0.0001f);
    const int keptWidth = layout.Viewport().width;
    const float keptX = layout.buttons[Fire].x;
    layout.SetViewport({1080,2400,0,0,1,1});
    layout.SetViewport({3168,1440,0.45f,0.0f,0.55f,1.0f});
    layout.SetViewport({200,100,0,0,1,1});
    assert(layout.Viewport().width==keptWidth);
    assert(std::fabs(layout.buttons[Fire].x-keptX)<0.0001f);
    assert(TouchModeForSession(true, true)==MOBILE_TOUCH_GAME);
    assert(TouchModeForSession(true, false)==MOBILE_TOUCH_DISABLED);
    assert(TouchModeForSession(false, true)==MOBILE_TOUCH_UI);
    assert(TouchModeForSession(false, false)==MOBILE_TOUCH_UI);
    float yaw=0, pitch=0;
    MapDeviceGyroToView(MOBILE_GYRO_LANDSCAPE, 0.5f, 0.25f, yaw, pitch);
    assert(yaw==0.5f && pitch==0.25f);
    MapDeviceGyroToView(MOBILE_GYRO_LANDSCAPE_FLIPPED, 0.5f, 0.25f, yaw, pitch);
    assert(yaw==-0.5f && pitch==-0.25f);
    assert(!GyroViewActive(0, true, true));
    assert(!GyroViewActive(1, true, false));
    assert(!GyroViewActive(1, false, true));
    assert(GyroViewActive(1, true, true));
    assert(GyroViewActive(2, true, false));
    assert(!GyroViewActive(2, false, false));
    std::puts("MOBILE_TOUCH_PASS: simultaneous move/look/fire, ownership, tap latching, cancel/reset, safe layout geometry, pointer-menu mode and gyro aim");
}
