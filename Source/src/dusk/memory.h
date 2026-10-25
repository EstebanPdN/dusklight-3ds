#pragma once

#if TARGET_PC && !defined(DUSK_TARGET_3DS)
#define HEAP_SIZE(original, dusk) (dusk)
#else
#define HEAP_SIZE(original, dusk) (original)
#endif
