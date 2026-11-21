#pragma once

#include <cstring>

namespace {
constexpr int skinMatrixSlots = 10;

// Each contiguous run updates only the slots its shape supplied. Position and
// normal writes occupy disjoint XF ranges, and the position array bindings
// do not read either range. Keep the original binding calls and their order.
void loadSkinMatrixRuns(const BE(u16)* indices, int slots, const u8* scale,
                        MtxP base, const J3DModelData& model, MtxP const (&tables)[2]) {
    u32 positions[skinMatrixSlots * 12];
    u32 normals[skinMatrixSlots * 9];
    for (int slot = 0; slot < slots;) {
        if (indices[slot] == 0xffff) {
            ++slot;
            continue;
        }
        const int first = slot;
        do {
            const u16 index = indices[slot];
            const u16 drawIndex = model.getDrawMtxIndex(index);
            const unsigned table = model.getDrawMtxFlag(index);
            const unsigned count = table ? model.getWEvlpMtxNum() : model.getJointNum();
            j3dSys.setModelDrawMtx(reinterpret_cast<Mtx*>(tables[table]), count);

            Mtx position;
            MTXConcat(base, j3dSys.getModelDrawMtx(drawIndex), position);
            std::memcpy(positions + 12 * (slot - first), position, sizeof(position));
            auto* normal = normals + 9 * (slot - first);
            if (scale[drawIndex] == 1) {
                for (int row = 0; row < 3; ++row)
                    std::memcpy(normal + 3 * row, position[row], 3 * sizeof(u32));
            } else {
                Mtx33 inverseTranspose{};
                J3DPSCalcInverseTranspose(position, inverseTranspose);
                std::memcpy(normal, inverseTranspose, sizeof(inverseTranspose));
            }
            ++slot;
        } while (slot < slots && indices[slot] != 0xffff);

        GXNativeLoadXf(12 * first, positions, 12 * (slot - first));
        GXNativeLoadXf(0x400 + 9 * first, normals, 9 * (slot - first));
    }
}
} // namespace
