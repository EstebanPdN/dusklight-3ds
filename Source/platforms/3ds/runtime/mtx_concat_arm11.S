/* Row-major 3x4 affine product, scalar ARMv6K/VFPv2, AAPCS hard-float.
 * Internal contract: nonnull, float-aligned matrices; output must be either
 * identical to or disjoint from EACH input. Input/input overlap is allowed.
 * C_MTXConcat retains assertions and routes partial output overlaps to C.
 * B is loaded before any store; each A row is loaded before its output row,
 * preserving out=A, out=B and out=A=B without matrix scratch or copies.
 *
 * Keep the measured affine_candidate.S operation order. VMLA on VFPv2 is
 * non-fused (two roundings); the explicit +0 on rotation terms is necessary
 * for signed zero. NaNs retain classification, not guaranteed payload/sign.
 * d8-d10 are callee-saved; the 24-byte save keeps SP 8-byte aligned.
 * Prototype evidence: 32768 bit/classification comparisons, 37.53% fewer
 * EMULATED instruction ticks; not physical ARM11 or game-wide performance.
 */
.syntax unified
.arch armv6k
.fpu vfp
.arm
.section .text.DuskMtxConcatArm11,"ax",%progbits
.balign 4
.global DuskMtxConcatArm11
.hidden DuskMtxConcatArm11
.type DuskMtxConcatArm11,%function
DuskMtxConcatArm11:
    .fnstart
    vpush {d8-d10}
    .vsave {d8-d10}
    vldmia r1, {s4-s15}
    mov r3, #0
    vmov s20, r3

    .macro CONCAT_ROW
    vldmia r0!, {s0-s3}
    vmul.f32 s16, s1, s8
    vmul.f32 s17, s1, s9
    vmul.f32 s18, s1, s10
    vmul.f32 s19, s1, s11
    vmla.f32 s16, s0, s4
    vmla.f32 s17, s0, s5
    vmla.f32 s18, s0, s6
    vmla.f32 s19, s0, s7
    vmov.f32 s21, s20
    vmla.f32 s21, s2, s12
    vadd.f32 s16, s16, s21
    vmov.f32 s21, s20
    vmla.f32 s21, s2, s13
    vadd.f32 s17, s17, s21
    vmov.f32 s21, s20
    vmla.f32 s21, s2, s14
    vadd.f32 s18, s18, s21
    vmla.f32 s19, s2, s15
    vadd.f32 s19, s19, s3
    vstmia r2!, {s16-s19}
    .endm

    CONCAT_ROW
    CONCAT_ROW
    CONCAT_ROW
    .purgem CONCAT_ROW
    vpop {d8-d10}
    bx lr
    .fnend
.size DuskMtxConcatArm11, .-DuskMtxConcatArm11
.section .note.GNU-stack,"",%progbits
