// Deterministic touch ownership and layout geometry, independent of SDL and the engine.
#pragma once
#include "inputsystem/mobileinput.h"
#include "mobile_hud.h"
#include <cmath>
#include <stdint.h>

namespace mobile
{
struct Button { float x, y, size, opacity; };
struct Rect { float x, y, w, h; };
struct Finger { bool active; int64_t id; int control; float x, y; };

inline float Clamp( float n, float lo, float hi ) { return n < lo ? lo : (n > hi ? hi : n); }
inline bool Finite( float n ) { return std::isfinite( n ); }

// Team select and the buy menu are in-game Panorama panels. They need clicks,
// so gameplay must be false or the finger stays a look/move gesture.
inline MobileTouchMode TouchModeForSession( bool gameplay, bool controlsEnabled )
{
    if ( !gameplay ) return MOBILE_TOUCH_UI;
    return controlsEnabled ? MOBILE_TOUCH_GAME : MOBILE_TOUCH_DISABLED;
}
inline Button DefaultButton( int i, float aspect=16.0f/9.0f )
{
    // Distances from the edges are in safe-area heights, so ultrawide phones
    // don't push the thumb controls toward the middle of the screen.
    const float scale=Clamp(aspect/1.70f,.72f,1.f);
    Button b={.5f,.5f,.11f,.72f};
    float offset=0;
    int anchor=0; // -1 left, 0 centre, +1 right
    switch(i)
    {
    case Move: anchor=-1;offset=.185f;b.y=.79f;b.size=.31f;b.opacity=.48f;break;
    case Fire: anchor=1;offset=.125f;b.y=.51f;b.size=.20f;b.opacity=.80f;break;
    case FireLeft: anchor=-1;offset=.11f;b.y=.39f;b.size=.14f;break;
    case Aim: anchor=1;offset=.35f;b.y=.37f;b.size=.14f;break;
    case Jump: anchor=1;offset=.105f;b.y=.74f;b.size=.145f;break;
    case Duck: anchor=1;offset=.255f;b.y=.89f;b.size=.14f;break;
    case Reload: anchor=1;offset=.445f;b.y=.86f;b.size=.13f;break;
    case Use: anchor=1;offset=.555f;b.y=.56f;b.size=.15f;break;
    case Walk: anchor=-1;offset=.355f;b.y=.55f;break;
    case Primary: offset=-.275f;b.y=.925f;b.size=.125f;break;
    case Pistol: offset=-.035f;b.y=.925f;b.size=.125f;break;
    case Knife: offset=.155f;b.y=.925f;b.size=.12f;break;
    case Taser: offset=.315f;b.y=.925f;break;
    case HEGrenade: offset=-.40f;b.y=.74f;break;
    case Flash: offset=-.255f;b.y=.74f;break;
    case Smoke: offset=-.11f;b.y=.74f;break;
    case Molotov: offset=.035f;b.y=.74f;break;
    case Incendiary: offset=.18f;b.y=.74f;break;
    case Decoy: offset=.325f;b.y=.74f;break;
    case Objective: anchor=1;offset=.565f;b.y=.37f;b.size=.15f;break;
    case Drop: anchor=1;offset=.12f;b.y=.25f;b.size=.10f;b.opacity=.60f;break;
    case Buy: anchor=-1;offset=.10f;b.y=.56f;b.size=.115f;break;
    case Menu: anchor=1;offset=.08f;b.y=.085f;b.size=.10f;break;
    case Team: anchor=1;offset=.22f;b.y=.085f;b.size=.10f;break;
    case Score: anchor=1;offset=.36f;b.y=.085f;b.size=.10f;break;
    default:
        // Additional equipment in survival/co-op gets its own stable position.
        // These rows remain absent in ordinary matches without those items.
        offset=-.425f+((i-Healthshot)%6)*.15f;
        b.y=.19f+((i-Healthshot)/6)*.15f;
        if(i>=Tablet)offset=-.50f+((i-Healthshot)%6)*.12f;
        b.size=.105f;
        break;
    }
    b.size*=scale;
    b.x=(anchor<0?0.f:anchor>0?1.f:.5f)+(anchor>0?-offset:offset)*scale/aspect;
    return b;
}

class TouchControls
{
public:
    Button buttons[ControlCount];
    TouchControls() : m_view{ 1920,1080,0,0,1,1 }, m_lookX(0), m_lookY(0), m_pressed(0), m_moveX(0), m_moveY(0),
        m_visible(AllControls()), m_enabled(AllControls())
    {
        for(int i=0;i<ControlCount;++i)m_bindings[i]=0;
        Defaults(); Reset();
    }
    float Aspect() const { return m_view.width*(m_view.right-m_view.left)/(m_view.height*(m_view.bottom-m_view.top)); }
    void Defaults() { for ( int i=0; i<ControlCount; ++i ) buttons[i] = DefaultButton(i,Aspect()); }
    void Reset()
    {
        for ( Finger &f : m_fingers ) f.active = false;
        m_lookX = m_lookY = m_moveX = m_moveY = 0;
        m_pressed = 0;
    }
    void CancelControl(int control)
    {
        m_pressed &= ~ControlBit(control);
        // Keep the finger quarantined until it lifts. Hiding or replacing a
        // button must never turn that same contact into a new action or look.
        for(Finger &f:m_fingers)if(f.active&&f.control==control)f.control=-2;
        if(control==Move)m_moveX=m_moveY=0;
    }
    void SetControls(const HudState &hud)
    {
        const ControlMask visible=hud.Visible(), enabled=hud.Enabled();
        for(int i=0;i<ControlCount;++i)
        {
            const uint64_t binding=hud.controls[i].Binding();
            if(!(enabled&ControlBit(i))||binding!=m_bindings[i])CancelControl(i);
            m_bindings[i]=binding;
        }
        m_visible=visible;m_enabled=enabled;
    }
    void SetViewport( const MobileViewport &v )
    {
        if ( v.width <= 0 || v.height <= 0 || !Finite(v.left) || !Finite(v.top) || !Finite(v.right) || !Finite(v.bottom)
             || v.left < 0 || v.top < 0 || v.right > 1 || v.bottom > 1 || v.right <= v.left || v.bottom <= v.top ) return;
        // Portrait, square and collapsed safe areas show up for a frame when
        // leaving Android freeform. Clamping into them piles every button on
        // one edge, and the next real landscape size does not undo that.
        if ( !PlausibleViewport( v ) ) return;
        const bool changed=v.width != m_view.width || v.height != m_view.height || v.left != m_view.left || v.top != m_view.top
             || v.right != m_view.right || v.bottom != m_view.bottom;
        if ( !changed ) return;
        const bool defaults = MatchesDefaults();
        m_view = v;
        Reset();
        if ( defaults ) Defaults();
        else for ( int i=0; i<ControlCount; ++i ) Normalize(i);
    }
    const MobileViewport &Viewport() const { return m_view; }
    void Normalize( int i )
    {
        Button &b = buttons[i];
        const Button d = DefaultButton(i,Aspect());
        if ( !Finite(b.x) || !Finite(b.y) || !Finite(b.size) || !Finite(b.opacity) ) b=d;
        b.size=Clamp(b.size,.065f,i==Move?.50f:.30f);
        b.opacity=Clamp(b.opacity,.20f,1.0f);
        const float halfX=b.size*ControlWidthRatio(i)*(m_view.bottom-m_view.top)*m_view.height/m_view.width/(m_view.right-m_view.left)*.5f;
        // An inverted clamp range (halfX > 0.5) would pin every control to the
        // same x. Keep the authored position instead of stacking the layout.
        if ( halfX < 0.5f ) b.x=Clamp(b.x,halfX,1-halfX);
        if ( b.size < 1.0f ) b.y=Clamp(b.y,b.size*.5f,1-b.size*.5f);
    }
    Rect Bounds( int i ) const
    {
        const Button &b=buttons[i];
        float h=b.size*(m_view.bottom-m_view.top), w=h*ControlWidthRatio(i)*m_view.height/m_view.width;
        return { m_view.left+b.x*(m_view.right-m_view.left)-w*.5f, m_view.top+b.y*(m_view.bottom-m_view.top)-h*.5f, w,h };
    }
    int Hit( float x, float y ) const
    {
        for ( int i=ControlCount-1; i>=0; --i )
        {
            if(!(m_visible&ControlBit(i)))continue;
            Rect r=Bounds(i);
            float dx=(x-r.x)/r.w-.5f, dy=(y-r.y)/r.h-.5f;
            if ( ControlWidthRatio(i)>1 ? std::fabs(dx)<=.5f&&std::fabs(dy)<=.5f : dx*dx+dy*dy <= .25f ) return i;
        }
        return -1;
    }
    void Drag( int i, float dx, float dy )
    {
        if ( i<0 || i>=ControlCount || !Finite(dx) || !Finite(dy) ) return;
        buttons[i].x += dx/(m_view.right-m_view.left);
        buttons[i].y += dy/(m_view.bottom-m_view.top);
        Normalize(i);
    }
    void Event( const MobileTouchEvent &e )
    {
        if ( e.type==MOBILE_TOUCH_RESET ) { Reset(); return; }
        Finger *f=nullptr;
        for ( Finger &p : m_fingers ) if ( p.active && p.id==e.finger ) { f=&p; break; }
        if ( e.type==MOBILE_TOUCH_UP || e.type==MOBILE_TOUCH_CANCEL )
        {
            if ( f )
            {
                if(e.type==MOBILE_TOUCH_CANCEL && f->control>=0)m_pressed&=~ControlBit(f->control);
                bool move=f->control==Move; f->active=false; if(move) m_moveX=m_moveY=0;
            }
            return;
        }
        if ( !Finite(e.x) || !Finite(e.y) ) return;
        if ( e.type==MOBILE_TOUCH_DOWN )
        {
            // A duplicate down must not steal a held control or leave a stale action.
            if ( f ) return;
            for ( Finger &p : m_fingers ) if ( !p.active ) { f=&p; break; }
            if ( !f ) return;
            int control=Hit(e.x,e.y);
            if(control>=0&&!(m_enabled&ControlBit(control)))control=-2;
            if ( control==Move && Held(Move) ) control=-2;
            if ( control<0 && control!=-2 )
            {
                bool haveLook=false;
                for ( const Finger &p : m_fingers ) if(p.active && p.control==-1) haveLook=true;
                // Empty right-side space owns look; extra fingers remain ignored until lifted.
                control= !haveLook && e.x>m_view.left+(m_view.right-m_view.left)*.36f ? -1 : -2;
            }
            *f={true,e.finger,control,e.x,e.y};
            if ( control>=0 && control!=Move ) m_pressed |= ControlBit(control);
        }
        if ( !f ) return; // Never turn a stale move into a new press after a menu/resize/reset.
        if ( e.type==MOBILE_TOUCH_MOVE && (f->control==-1 || f->control==Fire) )
        {
            m_lookX += (e.x-f->x)*m_view.width/m_view.height;
            m_lookY += e.y-f->y;
        }
        f->x=e.x; f->y=e.y;
        if ( f->control==Move )
        {
            Rect r=Bounds(Move);
            float x=(e.x-r.x-r.w*.5f)/(r.w*.40f), y=(e.y-r.y-r.h*.5f)/(r.h*.40f);
            float length=std::sqrt(x*x+y*y);
            if ( length<.12f ) m_moveX=m_moveY=0;
            else
            {
                float strength=Clamp((length-.12f)/.88f,0,1);
                m_moveX=x/length*strength; m_moveY=-y/length*strength;
            }
        }
    }
    bool Held( int i ) const { for(const Finger &f:m_fingers) if(f.active && f.control==i) return true; return false; }
    ControlMask HeldMask() const { ControlMask m=0; for(const Finger &f:m_fingers) if(f.active && f.control>=0) m|=ControlBit(f.control); return m; }
    ControlMask TakePresses( ControlMask mask=~ControlMask(0) ) { ControlMask p=m_pressed&mask; m_pressed&=~mask; return p; }
    void TakeLook( float &x, float &y ) { x=m_lookX; y=m_lookY; m_lookX=m_lookY=0; }
    float MoveX() const { return m_moveX; }
    float MoveY() const { return m_moveY; }
    bool Collapsed() const
    {
        int pairs=0;
        for(int i=0;i<ControlCount;++i)
            for(int j=i+1;j<ControlCount;++j)
                if(std::fabs(buttons[i].x-buttons[j].x)<0.04f && std::fabs(buttons[i].y-buttons[j].y)<0.04f)
                    ++pairs;
        return pairs>=6;
    }
    bool MatchesDefaults() const
    {
        for ( int i=0; i<ControlCount; ++i )
        {
            const Button d = DefaultButton(i, Aspect());
            const Button &b = buttons[i];
            if ( std::fabs(b.x-d.x) > 0.0001f || std::fabs(b.y-d.y) > 0.0001f
                 || std::fabs(b.size-d.size) > 0.0001f || std::fabs(b.opacity-d.opacity) > 0.0001f )
                return false;
        }
        return true;
    }
private:
    static bool PlausibleViewport( const MobileViewport &v )
    {
        if ( v.width < 320 || v.height < 240 || v.width < v.height ) return false;
        const float safeW = v.right-v.left, safeH = v.bottom-v.top;
        if ( safeW < 0.75f || safeH < 0.75f ) return false;
        const float aspect = v.width*safeW/(v.height*safeH);
        return aspect >= 1.15f && aspect <= 3.2f;
    }
    MobileViewport m_view;
    Finger m_fingers[10];
    float m_lookX,m_lookY;
    ControlMask m_pressed;
    float m_moveX,m_moveY;
    ControlMask m_visible,m_enabled;
    uint64_t m_bindings[ControlCount];
};
}
