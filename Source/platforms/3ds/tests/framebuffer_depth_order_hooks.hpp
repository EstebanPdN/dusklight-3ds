#pragma once
#include <dolphin/gx.h>
// Minimal declarations for compiling the verbatim production hook bodies.
// Full original translation-unit/ABI compilation remains a native-build gate.
class JFWDisplay {
public:
    void clearEfb(int,int,int,int,GXColor);
};
class dDlst_peekZ_c {
public:
    struct Entry {s16 x{},y{};u32* dst{};};
    u8 mCount{};
    Entry mEntries[64]{};
    void peekData();
};
