#ifndef HELPERS_NATIVE_IMMEDIATE_MERGE_H
#define HELPERS_NATIVE_IMMEDIATE_MERGE_H

// Native 3DS draw merging. Loops that issue one GXBegin/GXEnd per sprite
// (snow flakes, rain drops, cloud layers) cost one complete draw each on the
// 3DS. Inside this scope consecutive fixed-count quad or triangle lists with
// the same vertex format become one draw as long as no GX state is written
// between them; any state change draws what was collected first, so the
// picture and the draw order are the original ones. Open it only around code
// whose drawing all goes through GX calls.
#if defined(DUSK_TARGET_3DS)
extern "C" void GXNativeMergeImmediate(int enabled);
struct NativeImmediateMerge {
    NativeImmediateMerge() { GXNativeMergeImmediate(1); }
    ~NativeImmediateMerge() { GXNativeMergeImmediate(0); }
    NativeImmediateMerge(const NativeImmediateMerge&) = delete;
    NativeImmediateMerge& operator=(const NativeImmediateMerge&) = delete;
};
#else
struct NativeImmediateMerge {};
#endif

#endif
