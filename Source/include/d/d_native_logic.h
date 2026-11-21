#ifndef D_NATIVE_LOGIC_H
#define D_NATIVE_LOGIC_H

// Native New 3DS: cheaper game logic and draw-phase maths with the same
// results. Every rule here gives the original values bit for bit; each one has
// a switch and a verify mode that runs the original next to it, counts the
// differences and keeps the original result.
//
//   procindex  fpcEx_SearchByID (fopAcM_SearchByID and every search of a
//              process by id) reads an id table kept by the layer queues
//              instead of walking every process of every layer. The table
//              mirrors exactly the processes the walk can reach.
//   actorindex fopAcM_SearchByID and fopAcM_SearchByName (fpcSch_JudgeByID and
//              fpcSch_JudgeForPName on the actor list, fopAcIt_Judge) read
//              tables kept where an actor enters and leaves the actor list
//              (f_op/f_op_actor_index.h): the actor with the id, the first
//              actor of the name in list order. Other judges still walk.
//   frsqrte    the GameCube estimate frsqrte (JMAFastSqrt, PSVECMag,
//              PSVECNormalize, PSVECDistance) from 32-bit integer work for
//              positive normal floats (dolphin/ppc_math.h; proven on all of
//              them).
//   fmaf       newlib's fmaf ((float)((double)a * b + c) on the 3DS) inline in
//              PSVECSquareMag and ppc_rsqrte.
//   rnd        the random generators' fmodf(x, 1) as x - trunc(x)
//              (c_math.cpp; proven on every float with |x| < 2^31).
//   math       frsqrte, fmaf and rnd together.
//   kytag12    the twilight mist (KYTAG12, up to 2000 particles per tick)
//              reads the point lights and dark-mist spheres from compact lists
//              made once per tick, and Link's wolf form and the event state
//              once per tick: nothing in its particle loop changes them.
//   split      measurement only (default 0, not changed by none/verify): the
//              profiled pictures time collision corrections (acch.s.*) and
//              the collider checks (cc.s.*) in sections one after another
//              instead of the nested per-query scopes (d_tick_profile.h), and
//              print the cost of one timer (profile.emptyPair).
//   ccpre      push colliders (cCcS::ChkCo): a cylinder/sphere pair that
//              passes the area test is tested first with the function its
//              CrossCo would reach, on the live shapes; "no cross" leaves the
//              pair out before ChkNoHitCo and the virtual dispatch
//              (c_cc_s.cpp). Same pairs, same hits, same order.
//   crrskip    dBgS_Acch::CrrPos of a still door (DOOR20) or carried object
//              (Obj_Carry): a call whose complete input (the acch, its wall
//              circles, the actor's position, old position, speed, angles) is
//              byte-identical to its last call, while no collision object its
//              queries could reach has changed (slot versions, d_bg_s.cpp),
//              gets that call's stored output (d_bg_s_acch.cpp). Default off
//              until the verify tour accepts it; 2 = make every call and
//              compare all output bytes.
//   envelope   skin (envelope) matrices: each joint's world x inverse-bind
//              product is made once per model and calculation instead of
//              once per envelope that uses the joint, and the inverse-bind
//              matrices are read in host order (converted once per model
//              data). Same operations on the same values: bit-identical.
//   gdwrite    display-list commands written by J3D and GD (material diffs and
//              loads, texture, light and matrix commands) as one run of bytes
//              per command with one read of the thread's current list, instead
//              of one read (a call on the ARM11) per byte. Same bytes, same
//              order; a command that does not fit takes the per-byte path.
//              No verify mode (2 = on); the host test gd_native_test compares
//              both writers on every command kind and at every list end.
//
// sdmc:/3ds/DuskLight/logic.txt, read once at the first use: words separated
// by spaces or lines. "none" turns every rule off (Alpha 14), "verify" turns
// every rule on in verify mode; then "key=value" for one rule:
// procindex actorindex envelope frsqrte fmaf rnd math kytag12 ccpre crrskip gdwrite, each =0|1|2
// (0 off: the original code path; 1 on; 2 verify). Every rule's state
// before logic.txt is read (boot, first actors) is off., census=0|1 (one
// log line every 300 drawn pictures, "Native logic ...").

#include <cstring>

namespace dNativeLogic {

enum Mode { Off = 0, On = 1, Verify = 2 };

struct Config {
    unsigned char procIndex = On;
    unsigned char actorIndex = On;
    unsigned char envelope = On;
    unsigned char frsqrte = On;
    unsigned char fmaf = On;
    unsigned char rnd = On;
    unsigned char kytag12 = On;
    unsigned char ccPre = On;
    // On since the lead's acceptance (2026-11-19): 30-zone verify tour, seed 54737253,
    // 259,763 skipped calls compared with the original CrrPos, 0 mismatches.
    unsigned char crrSkip = On;
    unsigned char gdWrite = On;
    bool census = true;
    bool split = false;
};

inline int parseConfig(const char* text, Config& c) {
    int refused = 0;
    char word[64];
    const char* at = text;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n' || *at == ';') {
            ++at;
        }
        unsigned n = 0;
        while (at[n] != '\0' && at[n] != ' ' && at[n] != '\t' && at[n] != '\r' && at[n] != '\n' &&
               at[n] != ';') {
            ++n;
        }
        if (n == 0) {
            break;
        }
        const unsigned kept = n < sizeof(word) - 1 ? n : unsigned(sizeof(word) - 1);
        std::memcpy(word, at, kept);
        word[kept] = '\0';
        at += n;
        if (!std::strcmp(word, "none") || !std::strcmp(word, "off")) {
            c.procIndex = c.actorIndex = c.envelope = c.frsqrte = c.fmaf = c.rnd = c.kytag12 = c.ccPre =
                c.crrSkip = c.gdWrite = Off;
            continue;
        }
        if (!std::strcmp(word, "verify")) {
            c.procIndex = c.actorIndex = c.envelope = c.frsqrte = c.fmaf = c.rnd = c.kytag12 = c.ccPre =
                c.crrSkip = c.gdWrite = Verify;
            continue;
        }
        const char* eq = std::strchr(word, '=');
        if (eq == nullptr || eq[1] < '0' || eq[1] > '2' || eq[2] != '\0') {
            refused++;
            continue;
        }
        const unsigned char value = (unsigned char)(eq[1] - '0');
        const unsigned keyLength = unsigned(eq - word);
        if (keyLength == 9 && !std::memcmp(word, "procindex", 9)) {
            c.procIndex = value;
        } else if (keyLength == 10 && !std::memcmp(word, "actorindex", 10)) {
            c.actorIndex = value;
        } else if (keyLength == 8 && !std::memcmp(word, "envelope", 8)) {
            c.envelope = value;
        } else if (keyLength == 4 && !std::memcmp(word, "math", 4)) {
            c.frsqrte = c.fmaf = c.rnd = value;
        } else if (keyLength == 7 && !std::memcmp(word, "frsqrte", 7)) {
            c.frsqrte = value;
        } else if (keyLength == 4 && !std::memcmp(word, "fmaf", 4)) {
            c.fmaf = value;
        } else if (keyLength == 3 && !std::memcmp(word, "rnd", 3)) {
            c.rnd = value;
        } else if (keyLength == 7 && !std::memcmp(word, "kytag12", 7)) {
            c.kytag12 = value;
        } else if (keyLength == 5 && !std::memcmp(word, "ccpre", 5)) {
            c.ccPre = value;
        } else if (keyLength == 7 && !std::memcmp(word, "crrskip", 7)) {
            c.crrSkip = value;
        } else if (keyLength == 7 && !std::memcmp(word, "gdwrite", 7)) {
            c.gdWrite = value;
        } else if (keyLength == 6 && !std::memcmp(word, "census", 6) && value <= 1) {
            c.census = value != 0;
        } else if (keyLength == 5 && !std::memcmp(word, "split", 5) && value <= 1) {
            c.split = value != 0;
        } else {
            refused++;
        }
    }
    return refused;
}

// Read on the first call (game thread). No thread-safe static guard: the
// first call happens during boot, on the game thread, before any other
// thread runs game logic.
const Config& config();

// Counters of one census window (game thread only).
struct Census {
    unsigned ticks;
    unsigned searchById, searchByIdIndexed, searchByIdMissing;
    unsigned searchByJudge, actorListJudge, createListJudge;
    unsigned nodesVisited;
    unsigned verifyChecks, verifyMismatches;
    unsigned actorById, actorByIdIndexed, actorByName, actorByNameIndexed;
    unsigned actorNodesById, actorNodesByName, actorNodesOther, actorOther;
    unsigned actorVerifyChecks, actorVerifyMismatches;
    unsigned rndCalls;
    unsigned kclCalls[6], kclPrisms[6], kclTriPnt;  // line gnd wallSort wall roof spl
    unsigned ccCoPairs, ccCoArea, ccCoShape, ccAtTgPairs, ccAtTgArea, ccAtTgShape;
    unsigned j3dJoints;
    unsigned ccCoHits, ccPreSkipped, ccPreChecks, ccPreMismatches;
    unsigned bgScanned[5], bgWalks[5];
    unsigned bgChanges;
    unsigned crrEligible, crrHits, crrSkipped, crrChecks, crrMismatches;  // line gnd wall roof spl: slots visited, walks
    unsigned kytagTicks, kytagParticles, kytagChecks, kytagMismatches;
    unsigned envelopeModels, envelopeEntries, envelopeProducts;
    unsigned envelopeChecks, envelopeMismatches;
    // features::farLogic: actors of a reviewed profile asked while culled, and
    // those that rested (f_op_actor.cpp).
    unsigned farCandidates, farRested;
};
extern Census census;

// Actor-list walks by any other judge, by judge function (census line).
void countOtherJudge(void* judge, unsigned nodes);

// Called once per drawn picture from the process manager; prints the census
// every 300 drawn pictures when census=1.
void endPicture();

}  // namespace dNativeLogic

#endif /* D_NATIVE_LOGIC_H */
