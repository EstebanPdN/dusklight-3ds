// Runs on 3DS/Azahar, against the original portable body compiled separately
// with the production mtx.c command (only __3DS__ and symbol names differ).
// Non-NaN output bits, including signed zero/infinities, must match exactly;
// NaNs are compared by classification, never by payload or sign.
#include <3ds.h>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/stat.h>

using Mtx = float[3][4];
using Fn = void (*)(const Mtx, const Mtx, Mtx);
extern "C" void C_MTXConcat(const Mtx, const Mtx, Mtx);
extern "C" void DuskMtxConcatReference(const Mtx, const Mtx, Mtx);
extern "C" void DuskMtxConcatArm11(const Mtx, const Mtx, Mtx);
extern "C" unsigned CheckConcatAbi(Fn, const Mtx, const Mtx, Mtx);

// Seed ALL AAPCS callee-saved integer/VFP registers independently of what the
// C++ caller happens to keep live. Save our caller's originals first. A bad SP
// may fault rather than return; the runner requires a complete passing report.
asm(R"(
.syntax unified
.arch armv6k
.fpu vfp
.arm
.section .text.CheckConcatAbi,"ax",%progbits
.balign 4
.global CheckConcatAbi
.type CheckConcatAbi,%function
CheckConcatAbi:
    push {r4-r11, lr}
    sub sp, sp, #4
    vpush {d8-d15}
    sub sp, sp, #8
    mov ip, r0
    mov r0, r1
    mov r1, r2
    mov r2, r3
    mov r3, sp
    str r3, [sp]
    vmrs r3, fpscr
    str r3, [sp, #4]
    .macro SEED_PAIR reg, lo, hi
    mov r3, #\lo
    mov lr, #\hi
    vmov \reg, r3, lr
    .endm
    SEED_PAIR d8, 8, 24
    SEED_PAIR d9, 9, 25
    SEED_PAIR d10, 10, 26
    SEED_PAIR d11, 11, 27
    SEED_PAIR d12, 12, 28
    SEED_PAIR d13, 13, 29
    SEED_PAIR d14, 14, 30
    SEED_PAIR d15, 15, 31
    .purgem SEED_PAIR
    mov r4, #0x44
    mov r5, #0x55
    mov r6, #0x66
    mov r7, #0x77
    mov r8, #0x88
    mov r9, #0x99
    mov r10, #0xaa
    mov r11, #0xbb
    blx ip
    mov r0, #0
    .macro CHECK_R reg, value
    cmp \reg, #\value
    orrne r0, r0, #1
    .endm
    CHECK_R r4, 0x44
    CHECK_R r5, 0x55
    CHECK_R r6, 0x66
    CHECK_R r7, 0x77
    CHECK_R r8, 0x88
    CHECK_R r9, 0x99
    CHECK_R r10, 0xaa
    CHECK_R r11, 0xbb
    .purgem CHECK_R
    .macro CHECK_PAIR reg, lo, hi
    vmov r1, r2, \reg
    cmp r1, #\lo
    orrne r0, r0, #2
    cmp r2, #\hi
    orrne r0, r0, #2
    .endm
    CHECK_PAIR d8, 8, 24
    CHECK_PAIR d9, 9, 25
    CHECK_PAIR d10, 10, 26
    CHECK_PAIR d11, 11, 27
    CHECK_PAIR d12, 12, 28
    CHECK_PAIR d13, 13, 29
    CHECK_PAIR d14, 14, 30
    CHECK_PAIR d15, 15, 31
    .purgem CHECK_PAIR
    ldr r1, [sp]
    cmp r1, sp
    orrne r0, r0, #4
    tst r1, #7
    orrne r0, r0, #4
    vmrs r1, fpscr
    ldr r2, [sp, #4]
    // Arithmetic may set cumulative exception flags, not control bits.
    bic r1, r1, #0x9f
    bic r2, r2, #0x9f
    cmp r1, r2
    orrne r0, r0, #8
    add sp, sp, #8
    vpop {d8-d15}
    add sp, sp, #4
    pop {r4-r11, pc}
.size CheckConcatAbi, .-CheckConcatAbi
.previous
)");

static std::uint32_t seed = 0x19a2b3c4;
static unsigned comparisons;
static std::uint32_t randomWord() {
    seed = seed * 1664525u + 1013904223u;
    return seed;
}
static bool nanBits(std::uint32_t value) {
    return (value & 0x7fffffff) > 0x7f800000;
}
static bool check(Fn fn, unsigned trial, unsigned a, unsigned b, unsigned out) {
    constexpr std::uint32_t edge[]{
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff,
        0x00800000, 0x80800000, 0x3f800000, 0xbf800000,
        0x3f800001, 0xbf800001, 0x7f7fffff, 0xff7fffff,
        0x7f800000, 0xff800000, 0x7fc00001, 0xffc12345, 0x7f800001
    };
    alignas(8) float expected[64], actual[64];
    for (unsigned i = 0; i < 64; ++i) {
        const auto word = randomWord();
        if (trial < 2)
            expected[i] = std::bit_cast<float>(trial == 0 ? 0x80000000u : 0u);
        else if (trial < 1024)
            expected[i] = std::bit_cast<float>(edge[word % (sizeof(edge) / sizeof(edge[0]))]);
        else if (trial < 4096)
            expected[i] = float(int(word & 65535) - 32768) / 256.0f;
        else
            expected[i] = std::bit_cast<float>(word);
    }
    std::memcpy(actual, expected, sizeof(actual));
    DuskMtxConcatReference(reinterpret_cast<const float (*)[4]>(expected + a),
                           reinterpret_cast<const float (*)[4]>(expected + b),
                           reinterpret_cast<float (*)[4]>(expected + out));
    const auto abi = CheckConcatAbi(fn,
        reinterpret_cast<const float (*)[4]>(actual + a),
        reinterpret_cast<const float (*)[4]>(actual + b),
        reinterpret_cast<float (*)[4]>(actual + out));
    if (abi) {
        std::printf("ABI failure trial=%u offsets=%u/%u/%u mask=%u\n", trial, a, b, out, abi);
        return false;
    }
    for (unsigned i = 0; i < 64; ++i) {
        const auto e = std::bit_cast<std::uint32_t>(expected[i]);
        const auto v = std::bit_cast<std::uint32_t>(actual[i]);
        // Outside the output even NaN payloads must remain untouched.
        if (e != v && !(i >= out && i < out + 12 && nanBits(e) && nanBits(v))) {
            std::printf("Numerical/write-boundary failure trial=%u offsets=%u/%u/%u word=%u expected=%08lx actual=%08lx\n",
                        trial, a, b, out, i, static_cast<unsigned long>(e), static_cast<unsigned long>(v));
            return false;
        }
    }
    ++comparisons;
    return true;
}
static bool correctness() {
    constexpr unsigned layouts[][3]{
        {0, 16, 32}, {0, 16, 0}, {0, 16, 16}, {0, 0, 0},
        {0, 0, 32}, {0, 1, 32}, {1, 0, 32}
    };
    for (unsigned trial = 0; trial < 8192; ++trial) {
        for (const auto& layout : layouts) {
            // Identical deterministic input for the public and internal calls.
            const auto saved = seed;
            if (!check(C_MTXConcat, trial, layout[0], layout[1], layout[2])) return false;
            seed = saved;
            if (!check(DuskMtxConcatArm11, trial, layout[0], layout[1], layout[2])) return false;
        }
    }
    // Every float-aligned overlap displacement, including exact output alias
    // with A while partially overlapping B (and conversely). Compare the whole
    // backing store against sequential ORIGINAL C, not mathematical snapshots.
    for (unsigned trial : {2u, 1024u, 4096u})
        for (unsigned b = 0; b <= 32; ++b)
            for (unsigned out = 0; out <= 32; ++out)
                if (!check(C_MTXConcat, trial, 16, b, out)) return false;
    return true;
}
int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/DuskLight", 0777);
    const char* path = "sdmc:/3ds/DuskLight/mtx-concat-arm11.json";
    auto* report = std::fopen(path, "w");
    if (!report) { gfxExit(); return 2; }
    std::fputs("{\"status\":\"running\"}\n", report);
    std::fclose(report);
    const bool passed = correctness();
    report = std::fopen(path, "w");
    if (!report) { gfxExit(); return 2; }
    std::fprintf(report, "{\"status\":\"%s\",\"comparisons\":%u,\"non_nan_bits\":\"exact\",\"nan\":\"classification only\",\"abi\":\"r4-r11,d8-d15,sp,fpscr control\"}\n",
                 passed ? "pass" : "fail", comparisons);
    const bool written = std::fclose(report) == 0;
    std::printf("C_MTXConcat ARM11 %s: %u comparisons\n", passed ? "PASS" : "FAIL", comparisons);
    for (unsigned frame = 0; frame < 120 && aptMainLoop(); ++frame) {
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    gfxExit();
    return passed && written ? 0 : 1;
}
