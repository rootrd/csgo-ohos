// Optional SDL touch transport. Kept separate from the desktop launcher ABI.
#pragma once
#include <stdint.h>

#define MOBILE_INPUT_INTERFACE_VERSION "MobileInput001"

enum MobileTouchType { MOBILE_TOUCH_DOWN, MOBILE_TOUCH_MOVE, MOBILE_TOUCH_UP, MOBILE_TOUCH_CANCEL, MOBILE_TOUCH_RESET };
enum MobileTouchMode { MOBILE_TOUCH_UI, MOBILE_TOUCH_GAME, MOBILE_TOUCH_DISABLED };

struct MobileTouchEvent
{
    MobileTouchType type;
    int64_t finger;
    float x, y; // Normalized SDL window coordinates, independent of render resolution.
};

struct MobileViewport
{
    int width, height;
    float left, top, right, bottom; // Safe area in normalized window coordinates.
};

// Phone gyro axes stay in the natural portrait frame; the display rotation is applied here.
// MOBILE_GYRO_LANDSCAPE is the usual grip (portrait's right side up, camera on the left).
enum MobileGyroOrientation
{
    MOBILE_GYRO_LANDSCAPE = 0,
    MOBILE_GYRO_LANDSCAPE_FLIPPED = 1
};

// Device rates are radians/second in the natural portrait frame (right-handed:
// +X right, +Y up, +Z out of the screen). yaw > 0 turns the view left; pitch > 0 looks down.
// Landscape (camera on the left) maps +X to yaw and +Y to pitch. Flipped landscape mirrors both.
inline void MapDeviceGyroToView( int orientation, float deviceX, float deviceY, float &yaw, float &pitch )
{
    if ( orientation == MOBILE_GYRO_LANDSCAPE_FLIPPED )
    {
        yaw = -deviceX;
        pitch = -deviceY;
    }
    else
    {
        yaw = deviceX;
        pitch = deviceY;
    }
}

// mode: 0 off, 1 while scoped, 2 always. Dead and spectating players never take gyro.
inline bool GyroViewActive( int mode, bool alive, bool scoped )
{
    if ( !alive ) return false;
    if ( mode == 2 ) return true;
    if ( mode == 1 ) return scoped;
    return false;
}

class IMobileInputSource
{
public:
    virtual int ReadTouches( MobileTouchEvent *events, int capacity ) = 0;
    virtual void SetTouchMode( MobileTouchMode mode ) = 0;
    virtual MobileViewport GetTouchViewport() = 0;
    // Rotation since the previous call, in radians, already mapped into view space.
    // False means this device has no gyroscope; yaw and pitch are then zero.
    virtual bool ReadGyro( float &yaw, float &pitch ) = 0;
protected:
    virtual ~IMobileInputSource() {}
};
