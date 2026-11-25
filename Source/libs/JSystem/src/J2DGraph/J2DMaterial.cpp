#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J2DGraph/J2DMaterial.h"
#include "JSystem/J3DGraphBase/J3DStruct.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JUtility/JUTPalette.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "os_report.h"
#if defined(DUSK_TARGET_3DS)
#include <cstdio>
#include "../../../../platforms/3ds/runtime/gx_profile.hpp"
#include "../../../../platforms/3ds/runtime/gx_stream.hpp"
#endif

J2DMaterial::J2DMaterial() {
    field_0x4 = 0;
    mIndex = -1;
    mTevBlock = NULL;
    mIndBlock = NULL;
    mAnmPointer = NULL;
    field_0x8 = 1;
    mVisible = true;
}

J2DMaterial::~J2DMaterial() {
#if defined(DUSK_TARGET_3DS)
    // Only the render thread (the one with the native stream) owns the memo.
    if (dusk::native::gx::commandStream() != NULL) {
        dusk::native::gx::materialMemoForget(this);
    }
#endif
    JKR_DELETE(mTevBlock);
    JKR_DELETE(mIndBlock);
    JKR_DELETE(mAnmPointer);
}

#if defined(DUSK_TARGET_3DS)
// Native PICA engine: the typed GX setters of a HUD material cost about 40
// stream round trips per pane (Alpha 6 console: 65-77 us per drawn pane with
// the texture loads and vertex writes). The native stream memoizes them as
// compiled register writes (gx_commands.hpp, material memo); the TEV block's
// texture loads stay live. The original setters still run for anything the
// memo cannot prove unchanged.
namespace {
namespace nativegx = dusk::native::gx;

// Size of the TEV block for the memo key, the texture units its setGX loads
// (first, in unit order) and its stage count.
bool nativeTevShape(J2DTevBlock* block, u32& size, u32& textures, u32& stages) {
    size = 0;
    textures = 0;
    stages = 0;
    if (block == NULL) {
        return true;
    }
    switch (block->getType()) {
    case 'TVB1': size = sizeof(J2DTevBlock1); textures = 1; break;
    case 'TVB2': size = sizeof(J2DTevBlock2); textures = 2; break;
    case 'TVB4': size = sizeof(J2DTevBlock4); textures = 4; break;
    case 'TVB8': size = sizeof(J2DTevBlock8); textures = 8; break;
    case 'TV16': size = sizeof(J2DTevBlock16); textures = 8; break;
    default: return false;
    }
    stages = block->getTevStageNum();
    return stages <= 16;
}

// Development switch sdmc:/3ds/DuskLight/material-memo.txt, read at boot
// (developer_script.cpp): '0' keeps every material on the original setters
// (A/B), 'v' checks each replay and recording against them (register bank
// and recorded writes, like compiled-lists 'v'). Reported on the first call.
void nativeMaterialSwitch() {
    static bool read;
    if (read) {
        return;
    }
    read = true;
    OSReport("Native J2D material memo: %s\n", !nativegx::materialMemoEnabled() ? "off" :
             nativegx::materialMemoVerifying() ? "verified against the original setters" : "on");
}

// Once per profiled frame: the counters since the previous report. hits are
// replays (verified ones with 'v'); misses are calls that ran the setters
// (recorded, refused, given up or left to them as volatile); bypassed calls
// never looked (switch off, indirect block, pending BP mask, automatic SU).
// The second line splits refused recordings by cause (gx_commands.hpp,
// MaterialMemoRefusal); the third reports the typed masked BP writes and the
// incremental state key (state-hash.txt). Each line stays under 255 chars.
void nativeMaterialReport() {
    static bool reported;
    if (!dusk::native::gxSparseVertexProfiling) {
        reported = false;
        return;
    }
    if (reported) {
        return;
    }
    reported = true;
    nativegx::MaterialMemoStatistics s;
    nativegx::materialMemoStatistics(s, true);
    const unsigned long long misses = s.calls - s.replays - s.bypassed;
    OSReport("Native J2D material memo since last report: setGX=%llu hits=%llu misses=%llu (recorded=%llu stored=%llu "
             "refused=%llu volatile=%llu) bypassed=%llu pool-resets=%llu pool-words=%u verified=%llu mismatches=%llu%s\n",
             (unsigned long long)s.calls, (unsigned long long)s.replays, misses, (unsigned long long)s.records,
             (unsigned long long)s.stores, (unsigned long long)s.refused, (unsigned long long)s.volatileSkips,
             (unsigned long long)s.bypassed, (unsigned long long)s.poolResets, s.poolWords,
             (unsigned long long)s.verified, (unsigned long long)s.mismatches,
             s.storageFailed ? " storage=unavailable" : "");
    using R = nativegx::MaterialMemoRefusal;
    auto by = [&](R why) { return (unsigned long long)s.refusedBy[unsigned(why)]; };
    OSReport("Native J2D material memo refusals: execute=%llu list=%llu foreign=%llu texture-order=%llu matrix=%llu "
             "split=%llu mask=%llu capacity=%llu processor=%llu given-up=%llu source-changed=%llu footprint-misses=%llu\n",
             by(R::execute), by(R::list), by(R::foreign), by(R::textureOrder), by(R::matrix), by(R::split), by(R::mask),
             by(R::capacity), by(R::processor), (unsigned long long)s.givenUp, (unsigned long long)s.sourceChanges,
             (unsigned long long)s.footprintMisses);
    nativegx::TypedWriteStatistics typed;
    nativegx::typedWriteStatistics(typed, true);
    nativegx::StateHashStatistics hash;
    nativegx::stateHashStatistics(hash, true);
    OSReport("Native GX typed masked BP writes=%llu unchanged=%llu; state key incremental=%llu checks=%llu mismatches=%llu%s%c\n",
             (unsigned long long)typed.masked, (unsigned long long)typed.unchanged, (unsigned long long)hash.keys,
             (unsigned long long)hash.checks, (unsigned long long)hash.mismatches,
             hash.firstComponent ? " first=" : "", hash.firstComponent ? hash.firstComponent : ' ');
}

// Returns false when the original setters must run unrecorded.
bool nativeMaterialSetGX(J2DMaterial& material) {
    nativegx::CommandStream* stream = nativegx::commandStream();
    if (stream == NULL) {
        return false;
    }
    nativeMaterialSwitch();
    nativeMaterialReport();
    if (!nativegx::materialMemoEnabled()) {
        nativegx::materialMemoBypass();
        return false;
    }
    J2DTevBlock* tev = material.getTevBlock();
    J2DIndBlock* ind = material.getIndBlock();
    u32 tevSize, textures, stages;
    // Indirect stages (IndBlockFull) read back registers outside the memo
    // key; automatic SU tracks texture state the replay would not update.
    // finish() is what the first setter does; on failure the setters report it.
    if (!nativeTevShape(tev, tevSize, textures, stages) || (ind != NULL && ind->getType() != 'IBLN') ||
        stream->automaticSuEnabled() || !stream->finish()) {
        nativegx::materialMemoBypass();
        return false;
    }
    // A material whose recordings keep being refused goes to the setters
    // before paying for the checksum below.
    if (nativegx::materialMemoGivenUp(&material)) {
        return false;
    }
    // Everything the setters read from the material. Texture matrices live in
    // J2DTexMtx objects: J2DTexGenBlock::setGX declares each one to the
    // recorder, and a replay requires the same object bytes (gx_commands.cpp).
    // The TEV block's texture and palette pointers only feed the live loads.
    nativegx::MaterialMemoSum sum;
    const void* identity[3] = {&material, tev, ind};
    nativegx::materialMemoChecksum(sum, identity, sizeof(identity));
    nativegx::materialMemoChecksum(sum, &material.mColorBlock, sizeof(material.mColorBlock));
    nativegx::materialMemoChecksum(sum, &material.mTexGenBlock, sizeof(material.mTexGenBlock));
    if (tev != NULL) {
        nativegx::materialMemoChecksum(sum, tev, tevSize);
    }
    nativegx::materialMemoChecksum(sum, &material.mPEBlock, sizeof(material.mPEBlock));
    const nativegx::MaterialMemoTicket ticket = nativegx::materialMemoLookup(
        stream->registers(), &material, sum, stages, material.getTexGenBlock()->getTexGenNum());
    if (ticket.action == nativegx::MaterialMemoAction::replay) {
        static const auto replayIndex = dusk::native::registerGxNamedProfile("j2d.material.replay");
        dusk::native::GxNamedProfileScope replayScope(replayIndex);
        // Same order as the setters: colour and texgen state, the TEV block's
        // texture loads (live), then TEV, indirect and PE state.
        nativegx::materialMemoReplay(ticket, 0);
        for (u32 i = 0; i < textures; i++) {
            tev->loadTexture(GXTexMapID(i), i);
        }
        nativegx::materialMemoReplay(ticket, 1);
        return true;
    }
    if (ticket.action == nativegx::MaterialMemoAction::record) {
        static const auto recordIndex = dusk::native::registerGxNamedProfile("j2d.material.record");
        dusk::native::GxNamedProfileScope recordScope(recordIndex);
        material.getColorBlock()->setGX();
        material.getTexGenBlock()->setGX();
        nativegx::materialMemoSplit();
        if (tev != NULL) {
            tev->setGX();
        }
        if (ind != NULL) {
            ind->setGX();
        }
        material.getPEBlock()->setGX();
        const nativegx::MaterialMemoVerdict verdict = nativegx::materialMemoFinish(ticket);
        static unsigned reported;
        if (verdict.mismatch && reported < 16 && ++reported) {
            OSReport("Native J2D material memo mismatch: material=%p reason=%s at %c%03x original=%08lx replay=%08lx\n",
                     &material, verdict.reason, verdict.space ? verdict.space : '-', verdict.index,
                     (unsigned long)verdict.original, (unsigned long)verdict.replayed);
        }
        return true;
    }
    return false;
}
}  // namespace
#endif

void J2DMaterial::setGX() {
    if (mVisible) {
#if defined(DUSK_TARGET_3DS)
        static const auto profileIndex = dusk::native::registerGxNamedProfile("j2d.material");
        dusk::native::GxNamedProfileScope profile(profileIndex);
        if (nativeMaterialSetGX(*this)) {
            return;
        }
        // The setters the memo left to run (given up, refused, volatile,
        // bypassed): their own cost, apart from the memo's checks.
        static const auto settersIndex = dusk::native::registerGxNamedProfile("j2d.material.setters");
        dusk::native::GxNamedProfileScope settersScope(settersIndex);
#endif
        getColorBlock()->setGX();
        getTexGenBlock()->setGX();

        if (getTevBlock() != NULL) {
            getTevBlock()->setGX();
        }

        if (getIndBlock() != NULL) {
            getIndBlock()->setGX();
        }

        getPEBlock()->setGX();
    }
}

J2DTevBlock* J2DMaterial::createTevBlock(int block_type, bool noAlign) {
    J2DTevBlock* block = NULL;

    if (noAlign) {
        if (block_type <= 1) {
            block = JKR_NEW J2DTevBlock1();
        } else if (block_type == 2) {
            block = JKR_NEW J2DTevBlock2();
        } else if (block_type <= 4) {
            block = JKR_NEW J2DTevBlock4();
        } else if (block_type <= 8) {
            block = JKR_NEW J2DTevBlock8();
        } else {
            block = JKR_NEW J2DTevBlock16();
        }
    } else {
        if (block_type <= 1) {
            block = JKR_NEW_ARGS (-4) J2DTevBlock1;
        } else if (block_type == 2) {
            block = JKR_NEW_ARGS (-4) J2DTevBlock2;
        } else if (block_type <= 4) {
            block = JKR_NEW_ARGS (-4) J2DTevBlock4;
        } else if (block_type <= 8) {
            block = JKR_NEW_ARGS (-4) J2DTevBlock8;
        } else {
            block = JKR_NEW_ARGS (-4) J2DTevBlock16;
        }
    }
    ASSERTMSGLINE(101, block, "Error : allocate memory.");

    return block;
}

J2DIndBlock* J2DMaterial::createIndBlock(int block_type, bool noAlign) {
    J2DIndBlock* block = NULL;

    if (noAlign) {
        if (block_type != 0) {
            block = JKR_NEW J2DIndBlockFull();
        } else {
            block = JKR_NEW J2DIndBlockNull();
        }
    } else {
        if (block_type != 0) {
            block = JKR_NEW_ARGS (-4) J2DIndBlockFull;
        } else {
            block = JKR_NEW_ARGS (-4) J2DIndBlockNull;
        }
    }
    ASSERTMSGLINE(133, block, "Error : allocate memory.");

    return block;
}

J2DMaterial::J2DMaterialAnmPointer::J2DMaterialAnmPointer() {
    mColorAnm = NULL;
    mColorIds = -1;
    mSRTAnm = NULL;

    for (int i = 0; i < 8; i++) {
        mSRTIds[i] = -1;
    }
    mPatternAnm = NULL;

    for (int i = 0; i < 8; i++) {
        mPatternIds[i] = -1;
    }
    mTevAnm = NULL;

    for (int i = 0; i < 4; i++) {
        mTevCRegIds[i] = -1;
    }

    for (int i = 0; i < 4; i++) {
        mTevKRegIds[i] = -1;
    }
}

void J2DMaterial::makeAnmPointer() {
    int r29;
    int r28;
    if (mAnmPointer == NULL) {
        mAnmPointer = JKR_NEW J2DMaterialAnmPointer();
        r29 = 1;
        if (mAnmPointer == NULL) {
            OS_PANIC(171, "Error : allocate memory.");
            r28 = 0;
            if (!r28) {
                r29 = 0;
            }
        }
    }
}

void J2DMaterial::setAnimation(J2DAnmColor* anm) {
    if (anm != NULL || mAnmPointer != NULL) {
        makeAnmPointer();
        mAnmPointer->mColorAnm = anm;
        mAnmPointer->mColorIds = -1;

        if (anm != NULL) {
            u16 matNum = anm->getUpdateMaterialNum();
            u16 index = getIndex();

            for (u16 i = 0; i < matNum; i++) {
                if (index == anm->getUpdateMaterialID(i)) {
                    mAnmPointer->mColorIds = i;
                    return;
                }
            }
        }
    }
}

void J2DMaterial::setAnimation(J2DAnmTextureSRTKey* anm) {
    if (anm != NULL || mAnmPointer != NULL) {
        makeAnmPointer();
        mAnmPointer->mSRTAnm = anm;

        for (int i = 0; i < 8; i++) {
            mAnmPointer->mSRTIds[i] = -1;
        }

        if (anm != NULL) {
            u16 matNum = anm->getUpdateMaterialNum();
            u16 index = getIndex();

            for (u16 i = 0; i < matNum; i++) {
                if (index == anm->getUpdateMaterialID(i)) {
                    u8 mtxID = anm->getUpdateTexMtxID(i);
                    mAnmPointer->mSRTIds[mtxID] = i;
                }
            }
        }
    }
}

void J2DMaterial::setAnimation(J2DAnmTexPattern* anm) {
    if (anm != NULL || mAnmPointer != NULL) {
        makeAnmPointer();
        mAnmPointer->mPatternAnm = anm;

        for (int i = 0; i < 8; i++) {
            mAnmPointer->mPatternIds[i] = -1;
        }

        if (anm != NULL) {
            u16 matNum = anm->getUpdateMaterialNum();
            u16 index = getIndex();
            J3DAnmTexPatternFullTable* anmTbl = anm->getAnmTable();

            for (u16 i = 0; i < matNum; i++) {
                if (index == anm->getUpdateMaterialID(i)) {
                    mAnmPointer->mPatternIds[anmTbl[i].mTexNo] = i;
                }
            }
        }
    }
}

void J2DMaterial::setAnimation(J2DAnmTevRegKey* anm) {
    if (anm != NULL || mAnmPointer != NULL) {
        makeAnmPointer();
        mAnmPointer->mTevAnm = anm;

        for (int i = 0; i < 4; i++) {
            mAnmPointer->mTevCRegIds[i] = -1;
        }

        if (anm != NULL) {
            u16 matNum = anm->getCRegUpdateMaterialNum();
            u16 index = getIndex();

            for (u16 i = 0; i < matNum; i++) {
                if (index == anm->getCRegUpdateMaterialID(i)) {
                    J3DAnmCRegKeyTable* anmTbl = anm->getAnmCRegKeyTable();
                    mAnmPointer->mTevCRegIds[anmTbl[i].mColorId] = i;
                }
            }
        }

        for (int i = 0; i < 4; i++) {
            mAnmPointer->mTevKRegIds[i] = -1;
        }

        if (anm != NULL) {
            u16 matNum = anm->getKRegUpdateMaterialNum();
            u16 index = getIndex();

            for (u16 i = 0; i < matNum; i++) {
                if (index == anm->getKRegUpdateMaterialID(i)) {
                    J3DAnmKRegKeyTable* anmTbl = anm->getAnmKRegKeyTable();
                    mAnmPointer->mTevKRegIds[anmTbl[i].mColorId] = i;
                }
            }
        }
    }
}

void J2DMaterial::animation() {
    if (!mAnmPointer) {
		return;
	}

	if (mAnmPointer->mColorAnm && mAnmPointer->mColorIds != 0xFFFF) {
		GXColor color;
		mAnmPointer->mColorAnm->getColor(mAnmPointer->mColorIds, &color);
		getColorBlock()->setMatColor(0, color);
	}
    
	if (mAnmPointer->mSRTAnm) {
		for (u8 i = 0; i < 8; i++) {
			if (mAnmPointer->mSRTIds[i] != 0xFFFF) {
				J3DTextureSRTInfo info3D;
				mAnmPointer->mSRTAnm->getTransform(mAnmPointer->mSRTIds[i], &info3D);
				J2DTextureSRTInfo info2D;
				info2D.mScaleX       = info3D.mScaleX;
				info2D.mScaleY       = info3D.mScaleY;
				info2D.mRotationDeg  = (360.0f * f32((u16)info3D.mRotation)) / 65535.0f;
				info2D.mTranslationX = info3D.mTranslationX;
				info2D.mTranslationY = info3D.mTranslationY;

				J2DTexMtx texMtx;
				getTexGenBlock()->getTexMtx(i, texMtx);

				texMtx.getTexMtxInfo().mTexSRTInfo = info2D;
				getTexGenBlock()->setTexMtx(i, texMtx);
				getTexGenBlock()->getTexCoord(i).setTexGenMtx(i * 3 + 30);
			}
		}
	}

	if (getTevBlock() && mAnmPointer->mPatternAnm) {
		for (u8 i = 0; i < 8; i++) {
			if (mAnmPointer->mPatternIds[i] != 0xFFFF) {
				JUTTexture* texture = getTevBlock()->getTexture(i);
				if (!texture) {
					continue;
				}

				u16 idx = mAnmPointer->mPatternIds[i];
				u16 texNo;
				mAnmPointer->mPatternAnm->getTexNo(idx, &texNo);
				getTevBlock()->setTexNo(i, texNo);

                ResTIMG* img = mAnmPointer->mPatternAnm->getResTIMG(idx);
                if (texture->getTexInfo() != img) {
                    JUTPalette* palette = NULL;
                    GXTlut tlut = GX_TLUT0;
                    if (img->indexTexture != 0) {
                        palette = mAnmPointer->mPatternAnm->getPalette(idx);
						if (palette->getNumColors() > 256) {
							tlut = (GXTlut)(i % 4 + 16);
						} else {
							tlut = (GXTlut)i;
						}
                    }

                    texture->storeTIMG(img, palette, tlut);
                }
            }
		}
	}

	if (getTevBlock() && mAnmPointer->mTevAnm) {
		for (u8 i = 0; i < 4; i++) {
			if (mAnmPointer->mTevCRegIds[i] != 0xFFFF) {
				J2DGXColorS10 color;
				mAnmPointer->mTevAnm->getTevColorReg(mAnmPointer->mTevCRegIds[i], &color);
				getTevBlock()->setTevColor(i, color);
			}
		}

		for (u8 i = 0; i < 4; i++) {
			u16 idx = mAnmPointer->mTevKRegIds[i];
			if (idx != 0xFFFF) {
				JUtility::TColor konstColor;
				mAnmPointer->mTevAnm->getTevKonstReg(idx, &konstColor);
				getTevBlock()->setTevKColor(i, konstColor);
			}
		}
	}
}
