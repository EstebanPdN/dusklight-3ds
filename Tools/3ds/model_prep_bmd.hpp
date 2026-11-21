// Mac-only: the parts of a J3D model (bmd3/bdl4) that decide how the game
// draws it, read the way J3DModelLoader and J3DJointTree build it. Shared by
// model_prep_survey.cpp (offline candidates for the extractor) and
// model_prep_batch_test.cpp (the runtime's own command processor on every room
// shape). Never linked on the 3DS.
//
// Draw order: J3DJointTree::makeHierarchy adds each INF1 material node to the
// mesh list of the joint whose child scope it is in, at the head
// (J3DJoint::addMesh), and gives it the next shape node. J3DModel::entry walks
// joints 0..n-1 and each mesh list from its head. Room materials are all
// "changed" (dRes_info_c::loaderBasicBmd calls J3DMaterial::change()), so
// J3DDrawBuffer::entryMatSort puts every packet at the head of slot 0, and the
// NonSort lists (XLU BG, dark BG) do the same: the buffer draws a model's
// packets in the reverse of their entry order, one material packet per shape,
// nothing chained.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace modelprep {
using Bytes = std::vector<std::uint8_t>;
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
inline float bef(const std::uint8_t* p) { const auto v = be32(p); float f; std::memcpy(&f, &v, 4); return f; }

// GX primitive opcodes (high five bits of the command byte).
enum : std::uint8_t { gxQuads = 0x80, gxQuads2 = 0x88, gxTriangles = 0x90, gxStrip = 0x98, gxFan = 0xA0,
                      gxLines = 0xA8, gxLineStrip = 0xB0, gxPoints = 0xB8 };

struct Primitive { std::uint8_t opcode; std::uint16_t count; std::uint32_t offset; };
struct Group {                        // one J3DShapeMtx + J3DShapeDraw pair
    std::uint16_t useMtx = 0, useMtxCount = 0;
    std::uint32_t dlOffset = 0, dlSize = 0;     // in the file
    std::vector<Primitive> primitives;
    std::uint32_t dlUsed = 0;                   // bytes up to the end of the last primitive
    bool clean = false;                         // primitives of VAT 0 then only NOP padding
    std::uint32_t vertices = 0, indices = 0;    // drawBatch counts (raw vertices, triangle indices)
    bool triangleKinds = true;                  // every primitive is one drawBatch accepts
};
struct Shape {
    std::uint8_t mtxType = 0;
    std::uint16_t initIndex = 0;
    Bytes vcd;                                  // (attr,type) u32 pairs up to and with GX_VA_NULL
    std::uint32_t stride = 0;                   // packed bytes per vertex (indexed attributes)
    bool directAttributes = false;              // a non-matrix attribute stored in the list
    float min[3]{}, max[3]{}, radius = 0;
    std::vector<Group> groups;
    int material = -1;                          // the material node whose shape this is
};
struct Material {
    std::uint16_t remap = 0;                    // unique init data index (J3DMaterialFactory::getMaterialID)
    std::uint8_t mode = 0;                      // init byte 0: 1 opaque, 2 edge, 4 translucent
    std::string name;
    int joint = -1;
    int shape = -1;
};
struct Model {
    std::string kind;                           // bmd3 / bdl4
    std::uint32_t fileBytes = 0;
    std::vector<Material> materials;
    std::vector<Shape> shapes;
    std::uint16_t joints = 0;
    std::vector<std::vector<int>> meshes;       // per joint, J3D list order (head first)
    std::uint16_t drawMatrices = 0;
    std::vector<std::uint8_t> drawWeighted;     // DRW1: 1 envelope, 0 joint
    std::vector<std::uint16_t> drawIndex;
    // VTX1: per GX attribute (9 POS .. 20 TEX7) the format and the array span in the file.
    struct Format { bool present = false; std::uint32_t count = 0, type = 0; std::uint8_t frac = 0; };
    std::array<Format, 21> formats{};
    std::array<std::uint32_t, 21> arrayOffset{}, arrayBytes{};
    std::string error;                          // why the model could not be read
};

inline const std::uint8_t* blockOf(const Bytes& f, const char* tag, std::uint32_t& size) {
    if (f.size() < 32) return nullptr;
    const auto blocks = be32(f.data() + 12);
    std::size_t at = 32;
    for (std::uint32_t b = 0; b < blocks && at + 8 <= f.size(); ++b) {
        const auto n = be32(f.data() + at + 4);
        if (n < 8 || n > f.size() - at) return nullptr;
        if (!std::memcmp(f.data() + at, tag, 4)) { size = n; return f.data() + at; }
        at += n;
    }
    return nullptr;
}
inline std::string nameAt(const std::uint8_t* tab, std::uint32_t tabBytes, unsigned index) {
    if (tabBytes < 4 || index >= be16(tab) || 4 + index * 4 + 4 > tabBytes) return {};
    const auto off = be16(tab + 4 + index * 4 + 2);
    std::string s;
    for (std::uint32_t i = off; i < tabBytes && tab[i]; ++i) s.push_back(char(tab[i]));
    return s;
}

// Bytes a list stores per vertex for one VCD entry; 0 = not representable here.
inline unsigned attributeBytes(std::uint32_t attr, std::uint32_t type, bool& direct) {
    if (type == 0) return 0;
    if (type == 2) return 1;
    if (type == 3) return 2;
    if (attr <= 8) return 1;                    // PNMTXIDX / TEXnMTXIDX are direct u8
    direct = true;
    return 0;
}

inline bool parseList(const std::uint8_t* dl, std::uint32_t size, std::uint32_t stride, Group& g) {
    std::uint32_t at = 0;
    g.clean = true;
    while (at < size) {
        const std::uint8_t op = dl[at];
        if (op == 0) { ++at; continue; }
        if ((op & 0x80) && (op & 0xF8) <= gxPoints) {
            if (at + 3 > size) return false;
            const std::uint16_t count = be16(dl + at + 1);
            const std::uint64_t bytes = 3 + std::uint64_t(count) * stride;
            if (bytes > size - at) return false;
            // A command after NOP padding, or a VAT other than 0, is not a
            // plain concatenable list.
            if ((op & 7) != 0 || at != g.dlUsed) g.clean = false;
            const std::uint8_t kind = op & 0xF8;
            g.primitives.push_back({kind, count, at});
            g.vertices += count;
            if (kind == gxTriangles && count >= 3 && count % 3 == 0) g.indices += count;
            else if (kind == gxQuads && count >= 4 && count % 4 == 0) g.indices += count / 4 * 6;
            else if ((kind == gxStrip || kind == gxFan) && count >= 3) g.indices += (count - 2) * 3;
            else g.triangleKinds = false;
            at += std::uint32_t(bytes);
            g.dlUsed = at;
            continue;
        }
        g.clean = false;                         // any other command
        return true;
    }
    return true;
}

inline Model parseModel(const Bytes& f) {
    Model m;
    m.fileBytes = std::uint32_t(f.size());
    auto fail = [&](const char* why) { m.error = why; return m; };
    if (f.size() < 32 || std::memcmp(f.data(), "J3D2", 4)) return fail("not J3D2");
    m.kind.assign(reinterpret_cast<const char*>(f.data() + 4), 4);
    std::uint32_t inf1n = 0, jnt1n = 0, mat3n = 0, shp1n = 0, drw1n = 0;
    const auto* inf1 = blockOf(f, "INF1", inf1n);
    const auto* jnt1 = blockOf(f, "JNT1", jnt1n);
    const auto* mat3 = blockOf(f, "MAT3", mat3n);
    const auto* shp1 = blockOf(f, "SHP1", shp1n);
    const auto* drw1 = blockOf(f, "DRW1", drw1n);
    if (!inf1 || !jnt1 || !mat3 || !shp1 || !drw1) return fail("missing block");
    m.joints = be16(jnt1 + 8);
    // VTX1
    {
        std::uint32_t vn = 0;
        const auto* vtx1 = blockOf(f, "VTX1", vn);
        if (vtx1 && vn >= 0x40) {
            const auto fmtOff = be32(vtx1 + 8);
            for (std::uint32_t at = fmtOff; at + 16 <= vn; at += 16) {
                const auto attr = be32(vtx1 + at);
                if (attr == 0xFF) break;
                if (attr < 21) m.formats[attr] = {true, be32(vtx1 + at + 4), be32(vtx1 + at + 8), vtx1[at + 12]};
            }
            // Array slots: POS, NRM, NBT, CLR0, CLR1, TEX0..7 (NBT is not an attribute of its own).
            std::array<std::uint32_t, 13> pointers{};
            for (unsigned i = 0; i < 13; ++i) pointers[i] = be32(vtx1 + 12 + 4 * i);
            const int attrOf[13] = {9, 10, -1, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
            for (unsigned i = 0; i < 13; ++i) {
                if (!pointers[i] || attrOf[i] < 0) continue;
                std::uint32_t end = vn;
                for (const auto q : pointers) if (q > pointers[i] && q < end) end = q;
                m.arrayOffset[attrOf[i]] = std::uint32_t(vtx1 - f.data()) + pointers[i];
                m.arrayBytes[attrOf[i]] = end - pointers[i];
            }
        }
    }
    // DRW1
    m.drawMatrices = be16(drw1 + 8);
    {
        const auto wOff = be32(drw1 + 12), iOff = be32(drw1 + 16);
        if (wOff + m.drawMatrices > drw1n || iOff + 2u * m.drawMatrices > drw1n) return fail("DRW1 bounds");
        for (unsigned i = 0; i < m.drawMatrices; ++i) {
            m.drawWeighted.push_back(drw1[wOff + i]);
            m.drawIndex.push_back(be16(drw1 + iOff + 2 * i));
        }
    }
    // MAT3
    {
        const unsigned count = be16(mat3 + 8);
        const auto initOff = be32(mat3 + 12), remapOff = be32(mat3 + 16), nameOff = be32(mat3 + 20);
        if (remapOff + 2u * count > mat3n) return fail("MAT3 bounds");
        std::uint32_t initSize = 0x14C;
        m.materials.resize(count);
        for (unsigned i = 0; i < count; ++i) {
            auto& mat = m.materials[i];
            mat.remap = be16(mat3 + remapOff + 2 * i);
            const std::uint64_t init = initOff + std::uint64_t(mat.remap) * initSize;
            if (init + 1 > mat3n) return fail("MAT3 init bounds");
            mat.mode = mat3[init];
            if (nameOff && nameOff < mat3n) mat.name = nameAt(mat3 + nameOff, mat3n - nameOff, i);
        }
    }
    // SHP1
    {
        const unsigned count = be16(shp1 + 8);
        const auto initOff = be32(shp1 + 12), indexOff = be32(shp1 + 16), vcdOff = be32(shp1 + 24),
                   mtxTableOff = be32(shp1 + 28), dlOff = be32(shp1 + 32), mtxInitOff = be32(shp1 + 36),
                   drawInitOff = be32(shp1 + 40);
        (void)mtxTableOff;
        if (indexOff + 2u * count > shp1n) return fail("SHP1 index bounds");
        m.shapes.resize(count);
        for (unsigned s = 0; s < count; ++s) {
            auto& shape = m.shapes[s];
            shape.initIndex = be16(shp1 + indexOff + 2 * s);
            const std::uint64_t init = initOff + std::uint64_t(shape.initIndex) * 0x28;
            if (init + 0x28 > shp1n) return fail("SHP1 init bounds");
            const auto* d = shp1 + init;
            shape.mtxType = d[0];
            const unsigned groups = be16(d + 2), vcdIndex = be16(d + 4), mtxInit = be16(d + 6), drawInit = be16(d + 8);
            shape.radius = bef(d + 12);
            for (unsigned c = 0; c < 3; ++c) { shape.min[c] = bef(d + 16 + 4 * c); shape.max[c] = bef(d + 28 + 4 * c); }
            // VCD
            for (std::uint64_t at = std::uint64_t(vcdOff) + vcdIndex;; at += 8) {
                if (at + 8 > shp1n) return fail("VCD bounds");
                const auto attr = be32(shp1 + at), type = be32(shp1 + at + 4);
                shape.vcd.insert(shape.vcd.end(), shp1 + at, shp1 + at + 8);
                if (attr == 0xFF) break;
                shape.stride += attributeBytes(attr, type, shape.directAttributes);
                if (shape.vcd.size() > 8 * 32) return fail("VCD length");
            }
            for (unsigned g = 0; g < groups; ++g) {
                const std::uint64_t mi = mtxInitOff + std::uint64_t(mtxInit + g) * 8;
                const std::uint64_t di = drawInitOff + std::uint64_t(drawInit + g) * 8;
                if (mi + 8 > shp1n || di + 8 > shp1n) return fail("SHP1 group bounds");
                Group grp;
                grp.useMtx = be16(shp1 + mi);
                grp.useMtxCount = be16(shp1 + mi + 2);
                grp.dlSize = be32(shp1 + di);
                const std::uint64_t dl = std::uint64_t(dlOff) + be32(shp1 + di + 4);
                if (dl + grp.dlSize > shp1n) return fail("DL bounds");
                grp.dlOffset = std::uint32_t((shp1 - f.data()) + dl);
                if (!shape.directAttributes && !parseList(f.data() + grp.dlOffset, grp.dlSize, shape.stride, grp))
                    return fail("DL parse");
                shape.groups.push_back(std::move(grp));
            }
        }
    }
    // INF1 hierarchy, as J3DJointTree::makeHierarchy.
    {
        m.meshes.assign(m.joints, {});
        const auto hierOff = be32(inf1 + 20);
        std::uint32_t at = hierOff;
        std::vector<int> scope;      // pJoint of each recursion level
        int pJoint = -1, curJoint = -1;
        for (unsigned guard = 0; guard < 1u << 20; ++guard) {
            if (at + 4 > inf1n) return fail("INF1 bounds");
            const unsigned type = be16(inf1 + at), value = be16(inf1 + at + 2);
            at += 4;
            if (type == 0) break;
            if (type == 1) { scope.push_back(pJoint); pJoint = curJoint; continue; }
            if (type == 2) {
                if (scope.empty()) break;
                // Returning restores the caller's pJoint; its curJoint is the
                // joint that opened the scope.
                curJoint = pJoint; pJoint = scope.back(); scope.pop_back(); continue;
            }
            if (type == 0x10) { if (value >= m.joints) return fail("INF1 joint"); curJoint = int(value); continue; }
            if (type == 0x11) {
                if (pJoint < 0 || value >= m.materials.size()) return fail("INF1 material");
                auto& list = m.meshes[pJoint];
                list.insert(list.begin(), int(value));
                m.materials[value].joint = pJoint;
                continue;
            }
            if (type == 0x12) {
                if (pJoint < 0 || m.meshes[pJoint].empty() || value >= m.shapes.size()) return fail("INF1 shape");
                const int mat = m.meshes[pJoint].front();
                m.materials[mat].shape = int(value);
                m.shapes[value].material = mat;
                continue;
            }
            return fail("INF1 node type");
        }
    }
    return m;
}

// One entry of a model's draw buffers: the material node and its shape.
struct DrawItem { int material; int shape; };
struct DrawOrder { std::vector<DrawItem> opaque, translucent; };
// The order in which the buffers draw the packets of one J3DModel::entry
// with every shape visible: the reverse of the entry order, per buffer.
inline DrawOrder drawOrder(const Model& m) {
    DrawOrder order;
    for (unsigned j = 0; j < m.joints; ++j)
        for (const int mat : m.meshes[j]) {
            const int s = m.materials[mat].shape;
            if (s < 0) continue;
            ((m.materials[mat].mode & 3) ? order.opaque : order.translucent).push_back({mat, s});
        }
    std::reverse(order.opaque.begin(), order.opaque.end());
    std::reverse(order.translucent.begin(), order.translucent.end());
    return order;
}

// The draws one display list becomes in the runtime's command processor
// (runtime/gx_commands.cpp CommandProcessor::drawBatch, the caps of the
// scratch game_startup.cpp gives it): consecutive triangle-kind primitives of
// one VAT are one draw while the raw vertices fit `maxVertices` (and 65535),
// their packed bytes `vertexBytes` and their triangle indices `maxIndices`; a
// run of one primitive is drawn on its own. Checked against the processor
// itself by model_prep_batch_test.cpp on every room shape.
struct BatchCaps { std::uint32_t maxVertices = 4096, vertexBytes = 64 * 1024, maxIndices = 16 * 1024; };
inline std::uint32_t simulateDraws(const Group& g, std::uint32_t stride, const BatchCaps& caps = {}) {
    std::uint32_t draws = 0;
    std::size_t i = 0;
    const auto& p = g.primitives;
    const std::uint32_t vertexCap = std::min<std::uint32_t>(caps.maxVertices, 65535);
    while (i < p.size()) {
        std::size_t vc = 0, ic = 0, packets = 0, j = i;
        while (j < p.size()) {
            const unsigned c = p[j].count, k = p[j].opcode;
            std::size_t idx = 0;
            if (k == gxTriangles && c >= 3 && c % 3 == 0) idx = c;
            else if (k == gxQuads && c >= 4 && c % 4 == 0) idx = std::size_t(c / 4) * 6;
            else if ((k == gxStrip || k == gxFan) && c >= 3) idx = std::size_t(c - 2) * 3;
            else break;
            const std::size_t payload = std::size_t(c) * stride;
            if (c > vertexCap - vc || payload > caps.vertexBytes - vc * stride || idx > caps.maxIndices - ic) break;
            vc += c; ic += idx; ++packets; ++j;
        }
        ++draws;
        i = packets < 2 ? i + 1 : j;
    }
    return draws;
}
}
