#pragma once

// G4 offscreen builds keep the native Win32-compatible handle typedefs but
// deliberately provide no platform window type. Surface creation is rejected
// by the headless WSI driver.
