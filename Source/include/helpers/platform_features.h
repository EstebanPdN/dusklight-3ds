#pragma once

// Use only for reviewed optional desktop features. TARGET_PC also controls
// resource relocation/endian adapters and must stay enabled on the native port.
#if TARGET_PC && !defined(DUSK_TARGET_3DS)
#define DUSK_DESKTOP_ONLY(statement) statement
#define DUSK_DESKTOP_ARG(expression) , expression
#define DUSK_DESKTOP_ORIGINAL(desktop, original) desktop
#define DUSK_ORIGINAL_ONLY(statement)
#define DUSK_DESKTOP_BLOCK(condition) if (condition) {
#define DUSK_DESKTOP_BLOCK_END }
#else
#define DUSK_DESKTOP_ONLY(statement)
#define DUSK_DESKTOP_ARG(expression)
#define DUSK_DESKTOP_ORIGINAL(desktop, original) original
#define DUSK_ORIGINAL_ONLY(statement) statement
#define DUSK_DESKTOP_BLOCK(condition)
#define DUSK_DESKTOP_BLOCK_END
#endif
