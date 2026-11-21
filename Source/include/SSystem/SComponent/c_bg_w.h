#ifndef C_BG_W_H
#define C_BG_W_H

#include <types.h>

#if defined(DUSK_TARGET_3DS)
class cBgW_BgId;
// Native New 3DS (logic.txt crrskip, d_bg_s.cpp): a collision object's state
// changed (callers note only real value changes); its slot's version moves on
// (no effect when not registered). i_site names the change for the census.
void dBgS_NativeNoteChange(const cBgW_BgId* i_id, const char* i_site);
#endif

class cBgW_BgId {
private:
    /* 0x0 */ u16 m_id;
    /* 0x4 vtable */

public:
    void Release();
    bool ChkUsed() const;
    void Regist(int);

    virtual ~cBgW_BgId() {}

    cBgW_BgId() { Ct(); }
    void Ct() { m_id = 0x100; }
#if defined(DUSK_TARGET_3DS)
    u16 DuskGetId() const { return m_id; }
#endif

    int GetId() const { return m_id; }
};

bool cBgW_CheckBGround(float a1);
bool cBgW_CheckBRoof(float a1);
bool cBgW_CheckBWall(float a1);

#endif /* C_BG_W_H */
