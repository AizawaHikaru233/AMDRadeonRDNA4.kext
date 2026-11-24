//
//  panelrot.hpp
//  AMDRadeonRDNA4
//
//  rdna4-rot180: a panel mounted inverted. One option, off by default:
//    - the scanout is read turned 180 degrees (HUBP DCSURF_SURFACE_CONFIG.ROTATION_ANGLE), and every flip keeps the angle and
//      hands the HUBP the address such a read starts at (flip.cpp takes both through rtScanoutSetRotation, device.cpp passes
//      them here);
//    - the sprite plane is not turned with the picture, so the cursor's image is written turned and its place mirrored
//      (cursor.cpp).
//  Both follow this one flag; nothing else has to be set.
//
//  This is a header variable used by device.cpp alone: a mutable inline variable is NOT shared across this kext's translation
//  units (checked on the card, 2026-11-25), which is why the flip path receives the values as arguments instead.
//

#ifndef PanelRot_hpp
#define PanelRot_hpp

#include <stdint.h>

namespace PanelRot {

inline bool enabled = false;   // rdna4-rot180=1, or debug.rdna4_rot180 at run time

// Which address a turned read starts at: 1 the surface's first pixel -- the one this card was verified with (2026-11-25). 2 (the
// last pixel) and 3 (the last row) exist in flip.cpp for a panel whose hardware reads the other way.
constexpr uint32_t kAddressVariant = 1;

}   // namespace PanelRot

#endif /* PanelRot_hpp */
