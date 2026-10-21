// Nameplate and tab-target tuning.
//
// The stock client hard-codes these: 41 yards is 1681 squared, and a nameplate sits 0.6666667 yards
// above GetNamePosition(). awesome_wotlk's nameplateDistance and nameplatePlacement rewrite the same
// immediates in Wow.exe at runtime (the operand of the load at 0x00715739 and the tab-target range
// cap); here a mod writes the variables instead, so nothing in the client has to be patched.

#ifndef WOW_SOURCE_OBJECT_OBJECTCLIENT_NAMEPLATETUNING_H
#define WOW_SOURCE_OBJECT_OBJECTCLIENT_NAMEPLATETUNING_H

// Squared distance at which a nameplate stops being drawn (CGUnit_C::ShouldShowNamePlate)
extern float g_namePlateMaxDistanceSq;
// Squared distance cap of tab target selection (CGGameUI)
extern float g_tabTargetMaxDistanceSq;
// Height of a nameplate above the unit's name position, in yards
extern float g_namePlateHeightOffset;

#endif
