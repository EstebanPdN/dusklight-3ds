// Host test for the Alpha 15 logic rules (include/d/d_native_logic.h).
//
// 1. procindex: the id table of include/f_pc/f_pc_id_index.h (the code the
//    game runs) against a reference that walks every live process in
//    insertion order, as fpcLyIt_AllJudge does, over random and adversarial
//    queue operations (ids 4096 apart share a home slot, long-lived ids,
//    removal from the middle of probe runs, wrap-around at the table end).
// 2. envelope: the original skin-matrix loop (J3DMtxBuffer::
//    calcWeightEnvelopeMtxOriginal, non-PowerPC path) and the shared-product
//    loop (nativeEnvelopeShared) on random joint trees, compared bit for bit,
//    with the pinned C_MTXConcat. Built with -ffp-contract=off: the 3DS VFPv2
//    has no fused multiply-add, so a*b+c is two roundings there.
//    The device verify mode (logic.txt envelope=2) runs the real code.
#include "f_pc/f_pc_id_index.h"
#include "f_op/f_op_actor_index.h"
#include "d/d_native_logic.h"

#include <dolphin/mtx.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

struct FakeProc {
    unsigned id;
};

using Table = fpcIdIndexTable<FakeProc, unsigned>;

FakeProc* walk(const std::vector<FakeProc*>& live, unsigned id) {
    for (FakeProc* p : live) {
        if (p->id == id) {
            return p;
        }
    }
    return nullptr;
}

unsigned g_finds = 0;

void checkAll(const Table& t, const std::vector<FakeProc*>& live, unsigned nextId, std::mt19937& rng) {
    // Every live id, a few dead or never-made ids, ids that share home slots.
    for (FakeProc* p : live) {
        FakeProc* got = reinterpret_cast<FakeProc*>(1);
        assert(t.find(p->id, &got) == 1);
        assert(got == p);
        g_finds++;
    }
    for (int k = 0; k < 64; k++) {
        unsigned id = rng() % (nextId + 9000);
        if (k & 1) {
            id = (rng() % 8) * 4096u + (rng() % 64);
        }
        FakeProc* got = reinterpret_cast<FakeProc*>(1);
        assert(t.find(id, &got) == 1);
        assert(got == walk(live, id));
        g_finds++;
    }
    assert(t.live == live.size());
}

void testIndex() {
    std::mt19937 rng(12345);
    for (int round = 0; round < 40; round++) {
        static Table t;
        std::memset(&t, 0, sizeof(t));
        std::vector<FakeProc*> live;
        std::vector<FakeProc*> pool;
        unsigned nextId = 1 + (round % 3) * 4090;  // also start near the table end
        const unsigned target = 50 + rng() % 900;
        for (int op = 0; op < 20000; op++) {
            const unsigned r = rng() % 100;
            if (r < 45 && live.size() < 2500) {
                FakeProc* p = new FakeProc{nextId};
                // Ids 4096 apart land on one home slot: long-lived processes
                // (scenes) and later actors collide.
                nextId += (rng() % 50 == 0) ? 4096u * (1 + rng() % 3) : 1u + (rng() % 3 == 0);
                t.add(p);
                live.push_back(p);
                pool.push_back(p);
            } else if (r < 90 && !live.empty() && (live.size() > target || r < 60)) {
                const size_t i = rng() % live.size();
                // Leave a few long-lived processes at the front (scenes).
                if (i < 3 && live.size() > 3) {
                    continue;
                }
                t.remove(live[i]);
                live.erase(live.begin() + i);
            } else {
                checkAll(t, live, nextId, rng);
            }
            assert(t.stopped == 0);
        }
        checkAll(t, live, nextId, rng);
        // Removing everything leaves an empty table.
        while (!live.empty()) {
            t.remove(live.back());
            live.pop_back();
        }
        assert(t.live == 0);
        for (unsigned i = 0; i < Table::kSlots; i++) {
            assert(t.slots[i].proc == nullptr);
        }
        for (FakeProc* p : pool) {
            delete p;
        }
    }
    // Anomalies stop the table (the game then walks the layers).
    {
        static Table t;
        std::memset(&t, 0, sizeof(t));
        FakeProc a{7}, b{7}, c{8};
        t.add(&a);
        t.add(&b);
        assert(t.stopped == 3);
        FakeProc* got;
        assert(t.find(7, &got) == 0);
    }
    {
        static Table t;
        std::memset(&t, 0, sizeof(t));
        FakeProc a{7}, c{8};
        t.add(&a);
        t.remove(&c);
        assert(t.stopped == 4);
    }
    {
        static Table t;
        std::memset(&t, 0, sizeof(t));
        FakeProc a{7}, b{7};
        t.add(&a);
        t.remove(&b);
        assert(t.stopped == 5);
    }
    {
        static Table t;
        std::memset(&t, 0, sizeof(t));
        std::vector<FakeProc> many(Table::kMaxLive + 1);
        for (unsigned i = 0; i < many.size(); i++) {
            many[i].id = i + 1;
            t.add(&many[i]);
        }
        assert(t.stopped == 2);
    }
    std::printf("procindex: %u finds identical to the walk, 0 mismatches\n", g_finds);
}

// ---- actor list by name ----
// The actor list as g_fopAcTg_Queue holds it: appended at the tail, cut
// anywhere. The reference is the walk of fpcSch_JudgeForPName: the first
// actor of the name in list order.
struct FakeActor {
    unsigned id;
    short name;
    FakeActor* next;  // the list node's next
    FakeActor* prev;
};

unsigned g_nameFinds = 0;

void testNames() {
    std::mt19937 rng(4242);
    using Names = fopAcNameFirstTable<FakeActor>;
    for (int round = 0; round < 60; round++) {
        static Names t;
        std::memset(&t, 0, sizeof(t));
        FakeActor* head = nullptr;
        FakeActor* tail = nullptr;
        std::vector<FakeActor*> live;
        const int nameCount = 1 + rng() % (round % 4 == 0 ? 3 : 60);  // few names: long same-name runs
        for (int op = 0; op < 6000; op++) {
            const unsigned r = rng() % 100;
            if (r < 50 && live.size() < 400) {
                FakeActor* a = new FakeActor{unsigned(op), short(rng() % 10 == 0 ? (rng() % 2 ? -5 : 2000)
                                                                               : rng() % nameCount),
                                             nullptr, tail};
                if (tail) {
                    tail->next = a;
                } else {
                    head = a;
                }
                tail = a;
                live.push_back(a);
                t.added(a);
            } else if (r < 90 && !live.empty()) {
                const size_t i = rng() % live.size();
                FakeActor* a = live[i];
                t.removing(a, a, [](FakeActor* n) { return n->next; }, [](FakeActor* n) { return n; });
                if (a->prev) {
                    a->prev->next = a->next;
                } else {
                    head = a->next;
                }
                if (a->next) {
                    a->next->prev = a->prev;
                } else {
                    tail = a->prev;
                }
                live.erase(live.begin() + i);
                delete a;
            }
            for (int name = -6; name < nameCount + 2; name++) {
                FakeActor* walked = nullptr;
                for (FakeActor* n = head; n; n = n->next) {
                    if (n->name == name) {
                        walked = n;
                        break;
                    }
                }
                FakeActor* got = reinterpret_cast<FakeActor*>(1);
                if (t.find(name, &got)) {
                    assert(got == walked);
                    g_nameFinds++;
                } else {
                    assert(!Names::indexed(name));
                }
            }
        }
        for (FakeActor* a : live) {
            delete a;
        }
    }
    std::printf("actor names: %u finds identical to the walk, 0 mismatches\n", g_nameFinds);
}

// ---- envelope ----

uint32_t bswap(uint32_t v) {
    return __builtin_bswap32(v);
}

struct BeMtx {
    uint32_t w[12];
    void toHost(Mtx m) const {
        for (int i = 0; i < 12; i++) {
            const uint32_t v = bswap(w[i]);
            std::memcpy(&m[i / 4][i % 4], &v, 4);
        }
    }
};

struct M {
    Mtx m;
};

struct Tree {
    int jointNum;
    int evlpNum;
    std::vector<uint8_t> mixNum;
    std::vector<uint16_t> indexBe;  // big-endian u16 as stored
    std::vector<uint32_t> weightBe; // big-endian f32 as stored
    std::vector<BeMtx> inv;
    std::vector<uint8_t> scale;
    std::vector<M> anm;
};

uint16_t be16(uint16_t v) {
    return uint16_t((v >> 8) | (v << 8));
}
float readBeF32(uint32_t v) {
    const uint32_t h = bswap(v);
    float f;
    std::memcpy(&f, &h, 4);
    return f;
}

// J3DMtxBuffer::calcWeightEnvelopeMtxOriginal (DEBUG || !__MWERKS__ path).
void original(const Tree& t, std::vector<M>& out, std::vector<uint8_t>& outScale) {
    int i = -1;
    const int max = t.evlpNum;
    const uint16_t* indices = t.indexBe.data() - 1;
    const uint32_t* weights = t.weightBe.data() - 1;
    while (++i < max) {
        uint8_t* pScale = &outScale[i];
        *pScale = 1;
        float (*weightAnmMtx)[4] = out[i].m;
        weightAnmMtx[0][0] = weightAnmMtx[0][1] = weightAnmMtx[0][2] = weightAnmMtx[0][3] =
        weightAnmMtx[1][0] = weightAnmMtx[1][1] = weightAnmMtx[1][2] = weightAnmMtx[1][3] =
        weightAnmMtx[2][0] = weightAnmMtx[2][1] = weightAnmMtx[2][2] = weightAnmMtx[2][3] = 0.0f;
        int j = 0;
        const int mixNum = t.mixNum[i];
        do {
            const int idx = be16(*++indices);
            const Mtx* worldMtx = &t.anm[idx].m;
            Mtx invMtx, mtx;
            t.inv[idx].toHost(invMtx);
            C_MTXConcat(*worldMtx, invMtx, mtx);
            const float weight = readBeF32(*++weights);
            weightAnmMtx[0][0] += mtx[0][0] * weight;
            weightAnmMtx[0][1] += mtx[0][1] * weight;
            weightAnmMtx[0][2] += mtx[0][2] * weight;
            weightAnmMtx[0][3] += mtx[0][3] * weight;
            weightAnmMtx[1][0] += mtx[1][0] * weight;
            weightAnmMtx[1][1] += mtx[1][1] * weight;
            weightAnmMtx[1][2] += mtx[1][2] * weight;
            weightAnmMtx[1][3] += mtx[1][3] * weight;
            weightAnmMtx[2][0] += mtx[2][0] * weight;
            weightAnmMtx[2][1] += mtx[2][1] * weight;
            weightAnmMtx[2][2] += mtx[2][2] * weight;
            weightAnmMtx[2][3] += mtx[2][3] * weight;
            *pScale &= t.scale[idx];
        } while (++j < mixNum);
    }
}

// nativeEnvelopeShared (J3DMtxBuffer.cpp), same statements.
enum { kEnvelopeJoints = 512 };
Mtx g_product[kEnvelopeJoints];
uint32_t g_stamp[kEnvelopeJoints];
uint32_t g_generation;
unsigned g_products, g_entries;

void shared(const Tree& t, std::vector<M>& out, std::vector<uint8_t>& outScale) {
    if (++g_generation == 0) {
        std::memset(g_stamp, 0, sizeof(g_stamp));
        g_generation = 1;
    }
    const uint32_t generation = g_generation;
    const uint16_t* indices = t.indexBe.data();
    const uint32_t* weights = t.weightBe.data();
    for (int i = 0; i < t.evlpNum; i++) {
        float (*weightAnmMtx)[4] = out[i].m;
        float m00 = 0.0f, m01 = 0.0f, m02 = 0.0f, m03 = 0.0f;
        float m10 = 0.0f, m11 = 0.0f, m12 = 0.0f, m13 = 0.0f;
        float m20 = 0.0f, m21 = 0.0f, m22 = 0.0f, m23 = 0.0f;
        uint8_t scale = 1;
        const int mixNum = t.mixNum[i];
        int j = 0;
        do {
            const uint16_t idx = be16(*indices++);
            Mtx local;
            float (*mtx)[4];
            if (idx < kEnvelopeJoints) {
                mtx = g_product[idx];
                if (g_stamp[idx] != generation) {
                    g_stamp[idx] = generation;
                    Mtx invMtx;
                    t.inv[idx].toHost(invMtx);
                    C_MTXConcat(t.anm[idx].m, invMtx, mtx);
                    g_products++;
                }
            } else {
                Mtx invMtx;
                t.inv[idx].toHost(invMtx);
                C_MTXConcat(t.anm[idx].m, invMtx, local);
                mtx = local;
                g_products++;
            }
            const float weight = readBeF32(*weights++);
            m00 += mtx[0][0] * weight;
            m01 += mtx[0][1] * weight;
            m02 += mtx[0][2] * weight;
            m03 += mtx[0][3] * weight;
            m10 += mtx[1][0] * weight;
            m11 += mtx[1][1] * weight;
            m12 += mtx[1][2] * weight;
            m13 += mtx[1][3] * weight;
            m20 += mtx[2][0] * weight;
            m21 += mtx[2][1] * weight;
            m22 += mtx[2][2] * weight;
            m23 += mtx[2][3] * weight;
            scale &= t.scale[idx];
            g_entries++;
        } while (++j < mixNum);
        weightAnmMtx[0][0] = m00;
        weightAnmMtx[0][1] = m01;
        weightAnmMtx[0][2] = m02;
        weightAnmMtx[0][3] = m03;
        weightAnmMtx[1][0] = m10;
        weightAnmMtx[1][1] = m11;
        weightAnmMtx[1][2] = m12;
        weightAnmMtx[1][3] = m13;
        weightAnmMtx[2][0] = m20;
        weightAnmMtx[2][1] = m21;
        weightAnmMtx[2][2] = m22;
        weightAnmMtx[2][3] = m23;
        outScale[i] = scale;
    }
}

float randomFloat(std::mt19937& rng, int kind) {
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    switch (kind) {
    case 0:
        return u(rng);
    case 1:
        return u(rng) * 500.0f;  // translations
    case 2: {  // awkward values: denormal, signed zero, large, exact
        static const float odd[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-39f, -1e-39f, 3.0e38f, 0.5f, 1e-7f};
        return odd[rng() % (sizeof(odd) / sizeof(odd[0]))];
    }
    default:
        return u(rng) * 0.01f;
    }
}

uint32_t beFloat(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return bswap(v);
}

void testEnvelope() {
    std::mt19937 rng(777);
    unsigned matrices = 0, models = 0;
    for (int model = 0; model < 3000; model++) {
        Tree t;
        t.jointNum = 1 + rng() % (model % 50 == 0 ? 700 : 120);  // some over the 512-joint cache
        t.evlpNum = 1 + rng() % 160;
        t.inv.resize(t.jointNum);
        t.anm.resize(t.jointNum);
        t.scale.resize(t.jointNum);
        const int kind = model % 7 == 0 ? 2 : 0;
        for (int j = 0; j < t.jointNum; j++) {
            for (int e = 0; e < 12; e++) {
                t.inv[j].w[e] = beFloat(randomFloat(rng, e % 4 == 3 ? 1 : kind));
                t.anm[j].m[e / 4][e % 4] = randomFloat(rng, e % 4 == 3 ? 1 : kind);
            }
            t.scale[j] = rng() % 4 != 0;
        }
        for (int i = 0; i < t.evlpNum; i++) {
            // 0 happens in the do/while of the original: one entry is read.
            const int mix = (rng() % 40 == 0) ? 0 : 1 + rng() % 4;
            t.mixNum.push_back(uint8_t(mix));
            const int entries = mix == 0 ? 1 : mix;
            float sum = 0.0f;
            for (int k = 0; k < entries; k++) {
                // Joints are reused across envelopes, as in real skins.
                const uint16_t idx = uint16_t(rng() % (t.jointNum < 24 ? t.jointNum : 24) +
                                              (rng() % 3 == 0 ? rng() % t.jointNum : 0)) %
                                     t.jointNum;
                t.indexBe.push_back(be16(idx));
                const float w = k + 1 == entries ? 1.0f - sum : randomFloat(rng, 3) + 0.3f;
                sum += w;
                t.weightBe.push_back(beFloat(w));
            }
        }
        std::vector<M> a(t.evlpNum), b(t.evlpNum);
        std::vector<uint8_t> sa(t.evlpNum), sb(t.evlpNum);
        std::memset((void*)a.data(), 0x5a, a.size() * sizeof(M));
        std::memset((void*)b.data(), 0xa5, b.size() * sizeof(M));
        original(t, a, sa);
        // Twice: the second call must not reuse products of the first.
        shared(t, b, sb);
        for (int j = 0; j < t.jointNum; j++) {
            t.anm[j].m[0][3] += 1.0f;
        }
        original(t, a, sa);
        shared(t, b, sb);
        for (int i = 0; i < t.evlpNum; i++) {
            if (std::memcmp(a[i].m, b[i].m, sizeof(Mtx)) != 0 || sa[i] != sb[i]) {
                std::printf("envelope mismatch model %d envelope %d\n", model, i);
                assert(false);
            }
            matrices++;
        }
        models++;
    }
    std::printf("envelope: %u models, %u skin matrices bit-identical; %u entries used %u joint "
                "products (original: one per entry)\n",
                models, matrices, g_entries, g_products);
}

void testConfig() {
    dNativeLogic::Config c;
    assert(c.procIndex == 1 && c.envelope == 1 && c.census);
    assert(dNativeLogic::parseConfig("none", c) == 0);
    assert(c.procIndex == 0 && c.envelope == 0);
    assert(dNativeLogic::parseConfig("verify\nenvelope=1 census=0", c) == 0);
    assert(c.procIndex == 2 && c.envelope == 1 && !c.census);
    assert(dNativeLogic::parseConfig("procindex=7 bogus envelope=0", c) == 2);
    assert(c.procIndex == 2 && c.envelope == 0);
    assert(c.actorIndex == 2);
    assert(dNativeLogic::parseConfig("actorindex=0", c) == 0 && c.actorIndex == 0);
    assert(dNativeLogic::parseConfig("math=0", c) == 0 && c.frsqrte == 0 && c.fmaf == 0 && c.rnd == 0);
    assert(dNativeLogic::parseConfig("none frsqrte=1", c) == 0 && c.frsqrte == 1 && c.fmaf == 0 &&
           c.rnd == 0 && c.kytag12 == 0 && c.procIndex == 0);
    assert(dNativeLogic::parseConfig("none fmaf=2 rnd=1 kytag12=1", c) == 0 && c.frsqrte == 0 &&
           c.fmaf == 2 && c.rnd == 1 && c.kytag12 == 1);
    assert(dNativeLogic::parseConfig("none ccpre=1", c) == 0 && c.ccPre == 1 && c.kytag12 == 0);
    assert(dNativeLogic::parseConfig("verify", c) == 0 && c.ccPre == 2 && c.crrSkip == 2);
    {
        dNativeLogic::Config d;
        assert(d.crrSkip == 1);  // on since the 30-zone verify tour (0 mismatches)
        assert(dNativeLogic::parseConfig("crrskip=0", d) == 0 && d.crrSkip == 0);
    }
    std::printf("logic.txt parser ok\n");
}

}  // namespace

int main() {
    testConfig();
    testIndex();
    testNames();
    testEnvelope();
    std::printf("native logic tests passed\n");
    return 0;
}
