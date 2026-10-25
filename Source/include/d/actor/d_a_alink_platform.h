#pragma once

// Alink keeps TARGET_PC resource layouts, relocated pointers and safety fixes
// on native ARM. These selectors are only for individually reviewed optional
// desktop gameplay/presentation changes; they do not change the resource ABI.
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
#define IF_ALINK_DESKTOP(statement) statement
#define ALINK_DESKTOP_ORIGINAL(desktop, original) desktop
#else
#define IF_ALINK_DESKTOP(statement)
#define ALINK_DESKTOP_ORIGINAL(desktop, original) original
#endif
