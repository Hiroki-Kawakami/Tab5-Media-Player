/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

    .text

.macro FILL16 q, reg
    esp.movi.16.q \q, \reg, 0
    esp.movi.16.q \q, \reg, 1
    esp.movi.16.q \q, \reg, 2
    esp.movi.16.q \q, \reg, 3
    esp.movi.16.q \q, \reg, 4
    esp.movi.16.q \q, \reg, 5
    esp.movi.16.q \q, \reg, 6
    esp.movi.16.q \q, \reg, 7
.endm

    .balign 4
    .global h264_k_tap6_u8
    .type h264_k_tap6_u8, @function
h264_k_tap6_u8:
    addi sp, sp, -24
    sw s1, 0(sp)
    sw s8, 4(sp)
    sw s9, 8(sp)
    sw s10, 12(sp)
    sw s11, 16(sp)
    li t6, 5
    FILL16 q6, t6
    li t6, -5
    FILL16 q7, t6
    li t5, 0
    li s1, 255
    li t6, 16
    addi a5, a5, -16
    blez a3, 2f
1:
    mv s8, a0
    add s9, s8, a1
    add s10, s9, a1
    add s11, s10, a1
    add t3, s11, a1
    add t4, t3, a1
    esp.vldext.u8.ip q0, q1, s8, 16
    esp.vldext.u8.ip q2, q3, t4, 16
    esp.vadd.s16 q0, q0, q2
    esp.vadd.s16 q1, q1, q3
    esp.vldext.u8.ip q2, q3, s10, 16
    esp.vldext.u8.ip q4, q5, s11, 16
    esp.vadd.s16 q2, q2, q4
    esp.vadd.s16 q3, q3, q5
    esp.vadd.s16 q2, q2, q2
    esp.vadd.s16 q3, q3, q3
    esp.vadd.s16 q2, q2, q2
    esp.vadd.s16 q3, q3, q3
    esp.vldext.u8.ip q4, q5, s9, 16
    esp.vsub.s16 q2, q2, q4
    esp.vsub.s16 q3, q3, q5
    esp.vldext.u8.ip q4, q5, t3, 16
    esp.vsub.s16 q2, q2, q4
    esp.vsub.s16 q3, q3, q5
    esp.vmul.s16 q2, q2, q6
    esp.vmul.s16 q3, q3, q6
    esp.vadd.s16 q2, q2, q0
    esp.vadd.s16 q3, q3, q1
    esp.vsadds.s16 q2, q2, t6
    esp.vsadds.s16 q3, q3, t6
    esp.vsld.16 q2, q2, q7
    esp.vsld.16 q3, q3, q7
    esp.vsat.s16 q2, q2, t5, s1
    esp.vsat.s16 q3, q3, t5, s1
    esp.vunzip.8 q2, q3
    esp.vst.128.ip q2, a2, 16
    add a0, a0, a4
    add a2, a2, a5
    addi a3, a3, -1
    bnez a3, 1b
2:
    lw s1, 0(sp)
    lw s8, 4(sp)
    lw s9, 8(sp)
    lw s10, 12(sp)
    lw s11, 16(sp)
    addi sp, sp, 24
    ret
    .size h264_k_tap6_u8, .-h264_k_tap6_u8

    .balign 4
    .global h264_k_tap6_s16
    .type h264_k_tap6_s16, @function
h264_k_tap6_s16:
    addi sp, sp, -24
    sw s8, 4(sp)
    sw s9, 8(sp)
    sw s10, 12(sp)
    sw s11, 16(sp)
    li t6, 5
    FILL16 q6, t6
    li t6, 16
    FILL16 q7, t6
    blez a2, 2f
1:
    mv s8, a0
    addi s9, a0, 1
    addi s10, a0, 2
    addi s11, a0, 3
    addi t3, a0, 4
    addi t4, a0, 5
    esp.vldext.u8.ip q0, q1, s8, 16
    esp.vldext.u8.ip q2, q3, t4, 16
    esp.vadd.s16 q0, q0, q2
    esp.vadd.s16 q1, q1, q3
    esp.vldext.u8.ip q2, q3, s10, 16
    esp.vldext.u8.ip q4, q5, s11, 16
    esp.vadd.s16 q2, q2, q4
    esp.vadd.s16 q3, q3, q5
    esp.vadd.s16 q2, q2, q2
    esp.vadd.s16 q3, q3, q3
    esp.vadd.s16 q2, q2, q2
    esp.vadd.s16 q3, q3, q3
    esp.vldext.u8.ip q4, q5, s9, 16
    esp.vsub.s16 q2, q2, q4
    esp.vsub.s16 q3, q3, q5
    esp.vldext.u8.ip q4, q5, t3, 16
    esp.vsub.s16 q2, q2, q4
    esp.vsub.s16 q3, q3, q5
    esp.vmul.s16 q2, q2, q6
    esp.vmul.s16 q3, q3, q6
    esp.vadd.s16 q2, q2, q0
    esp.vadd.s16 q3, q3, q1
    esp.vadd.s16 q2, q2, q7
    esp.vadd.s16 q3, q3, q7
    esp.vst.128.ip q2, a1, 16
    esp.vst.128.ip q3, a1, 16
    add a0, a0, a3
    addi a2, a2, -1
    bnez a2, 1b
2:
    lw s8, 4(sp)
    lw s9, 8(sp)
    lw s10, 12(sp)
    lw s11, 16(sp)
    addi sp, sp, 24
    ret
    .size h264_k_tap6_s16, .-h264_k_tap6_s16

    .balign 4
    .global h264_k_tap6v_s16
    .type h264_k_tap6v_s16, @function
h264_k_tap6v_s16:
    addi sp, sp, -4
    sw s1, 0(sp)
    li t5, 0
    li s1, 255
    addi a3, a3, -16
    blez a2, 2f
1:
    li t6, 2
    esp.movx.w.sar t6

    mv t3, a0
    esp.vldext.s16.ip q6, q7, t3, 16
    addi t3, a0, 160
    esp.vldext.s16.ip q4, q5, t3, 16
    esp.vadd.s32 q6, q6, q4
    esp.vadd.s32 q7, q7, q5
    addi t3, a0, 32
    esp.vldext.s16.ip q2, q3, t3, 16
    addi t3, a0, 128
    esp.vldext.s16.ip q4, q5, t3, 16
    esp.vadd.s32 q2, q2, q4
    esp.vadd.s32 q3, q3, q5
    esp.vsub.s32 q6, q6, q2
    esp.vsub.s32 q7, q7, q3
    esp.vsl.32 q2, q2
    esp.vsl.32 q3, q3
    esp.vsub.s32 q6, q6, q2
    esp.vsub.s32 q7, q7, q3
    addi t3, a0, 64
    esp.vldext.s16.ip q2, q3, t3, 16
    addi t3, a0, 96
    esp.vldext.s16.ip q4, q5, t3, 16
    esp.vadd.s32 q2, q2, q4
    esp.vadd.s32 q3, q3, q5
    esp.vsl.32 q2, q2
    esp.vsl.32 q3, q3
    esp.vadd.s32 q6, q6, q2
    esp.vadd.s32 q7, q7, q3
    esp.vsl.32 q2, q2
    esp.vsl.32 q3, q3
    esp.vadd.s32 q6, q6, q2
    esp.vadd.s32 q7, q7, q3

    addi t3, a0, 16
    esp.vldext.s16.ip q0, q1, t3, 16
    addi t3, a0, 176
    esp.vldext.s16.ip q4, q5, t3, 16
    esp.vadd.s32 q0, q0, q4
    esp.vadd.s32 q1, q1, q5
    addi t3, a0, 48
    esp.vldext.s16.ip q2, q3, t3, 16
    addi t3, a0, 144
    esp.vldext.s16.ip q4, q5, t3, 16
    esp.vadd.s32 q2, q2, q4
    esp.vadd.s32 q3, q3, q5
    esp.vsub.s32 q0, q0, q2
    esp.vsub.s32 q1, q1, q3
    esp.vsl.32 q2, q2
    esp.vsl.32 q3, q3
    esp.vsub.s32 q0, q0, q2
    esp.vsub.s32 q1, q1, q3
    addi t3, a0, 80
    esp.vldext.s16.ip q2, q3, t3, 16
    addi t3, a0, 112
    esp.vldext.s16.ip q4, q5, t3, 16
    esp.vadd.s32 q2, q2, q4
    esp.vadd.s32 q3, q3, q5
    esp.vsl.32 q2, q2
    esp.vsl.32 q3, q3
    esp.vadd.s32 q0, q0, q2
    esp.vadd.s32 q1, q1, q3
    esp.vsl.32 q2, q2
    esp.vsl.32 q3, q3
    esp.vadd.s32 q0, q0, q2
    esp.vadd.s32 q1, q1, q3

    li t6, 10
    esp.movx.w.sar t6
    esp.vsr.s32 q6, q6
    esp.vsr.s32 q7, q7
    esp.vsr.s32 q0, q0
    esp.vsr.s32 q1, q1

    esp.vunzip.16 q6, q7
    esp.vunzip.16 q0, q1
    esp.vsat.s16 q6, q6, t5, s1
    esp.vsat.s16 q0, q0, t5, s1
    esp.vunzip.8 q6, q0
    esp.vst.128.ip q6, a1, 16

    addi a0, a0, 32
    add a1, a1, a3
    addi a2, a2, -1
    bnez a2, 1b
2:
    li t6, 0
    esp.movx.w.sar t6
    lw s1, 0(sp)
    addi sp, sp, 4
    ret
    .size h264_k_tap6v_s16, .-h264_k_tap6v_s16

    .balign 4
    .global h264_k_weight_u8
    .type h264_k_weight_u8, @function
h264_k_weight_u8:
    addi sp, sp, -4
    sw s1, 0(sp)
    FILL16 q4, a3
    FILL16 q5, a4
    neg t6, a5
    FILL16 q6, t6
    mv t6, a6
    FILL16 q7, t6
    li t5, 0
    li s1, 255
    addi a1, a1, -16
    blez a2, 2f
1:
    mv t3, a0
    esp.vldext.u8.ip q0, q1, a0, 16
    esp.vmul.s16 q0, q0, q4
    esp.vmul.s16 q1, q1, q4
    esp.vadd.s16 q0, q0, q5
    esp.vadd.s16 q1, q1, q5
    esp.vsld.16 q0, q0, q6
    esp.vsld.16 q1, q1, q6
    esp.vadd.s16 q0, q0, q7
    esp.vadd.s16 q1, q1, q7
    esp.vsat.s16 q0, q0, t5, s1
    esp.vsat.s16 q1, q1, t5, s1
    esp.vunzip.8 q0, q1
    esp.vst.128.ip q0, t3, 16
    add a0, a0, a1
    addi a2, a2, -1
    bnez a2, 1b
2:
    lw s1, 0(sp)
    addi sp, sp, 4
    ret
    .size h264_k_weight_u8, .-h264_k_weight_u8

    .balign 4
    .global h264_k_weight_bi_u8
    .type h264_k_weight_bi_u8, @function
h264_k_weight_bi_u8:
    addi sp, sp, -4
    sw s1, 0(sp)
    mv t6, a6
    FILL16 q4, t6
    li t6, 32
    FILL16 q5, t6
    li t6, -6
    FILL16 q6, t6
    li t5, 0
    li s1, 255
    addi a4, a4, -16
    addi a5, a5, -16
    blez a3, 2f
1:
    esp.vldext.u8.ip q0, q1, a0, 16
    esp.vldext.u8.ip q2, q3, a1, 16
    esp.vsub.s16 q2, q2, q0
    esp.vsub.s16 q3, q3, q1
    esp.vmul.s16 q2, q2, q4
    esp.vmul.s16 q3, q3, q4
    esp.vadd.s16 q2, q2, q5
    esp.vadd.s16 q3, q3, q5
    esp.vsld.16 q2, q2, q6
    esp.vsld.16 q3, q3, q6
    esp.vadd.s16 q2, q2, q0
    esp.vadd.s16 q3, q3, q1
    esp.vsat.s16 q2, q2, t5, s1
    esp.vsat.s16 q3, q3, t5, s1
    esp.vunzip.8 q2, q3
    esp.vst.128.ip q2, a2, 16
    add a0, a0, a4
    add a1, a1, a4
    add a2, a2, a5
    addi a3, a3, -1
    bnez a3, 1b
2:
    lw s1, 0(sp)
    addi sp, sp, 4
    ret
    .size h264_k_weight_bi_u8, .-h264_k_weight_bi_u8
