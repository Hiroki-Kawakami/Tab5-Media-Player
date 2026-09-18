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
    .global vdec_k_avg_u8
    .type vdec_k_avg_u8, @function
vdec_k_avg_u8:
    li t6, -1
    FILL16 q7, t6
    li t6, 1
    addi a4, a4, -16
    addi a5, a5, -16
    addi a6, a6, -16
    blez a3, 2f
1:
    esp.vldext.u8.ip q0, q1, a0, 16
    esp.vldext.u8.ip q2, q3, a1, 16
    esp.vadd.s16 q0, q0, q2
    esp.vadd.s16 q1, q1, q3
    esp.vsadds.s16 q0, q0, t6
    esp.vsadds.s16 q1, q1, t6
    esp.vsld.16 q0, q0, q7
    esp.vsld.16 q1, q1, q7
    esp.vunzip.8 q0, q1
    esp.vst.128.ip q0, a2, 16
    add a0, a0, a4
    add a1, a1, a5
    add a2, a2, a6
    addi a3, a3, -1
    bnez a3, 1b
2:
    ret
    .size vdec_k_avg_u8, .-vdec_k_avg_u8

    .balign 4
    .global vdec_k_bilinear
    .type vdec_k_bilinear, @function
vdec_k_bilinear:
    addi sp, sp, -8
    sw s8, 0(sp)
    FILL16 q4, a5
    mv t6, a6
    FILL16 q5, t6
    li t6, -6
    FILL16 q6, t6
    li t6, 32
    addi a3, a3, -16
    blez a4, 2f
1:
    mv t3, a0
    addi t4, a0, 1
    add t5, a0, a1
    addi s8, t5, 1
    esp.vldext.u8.ip q0, q3, t3, 16
    esp.vldext.u8.ip q2, q3, t4, 16
    esp.vsub.s16 q2, q2, q0
    esp.vmul.s16 q2, q2, q4
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q2
    esp.vldext.u8.ip q1, q3, t5, 16
    esp.vldext.u8.ip q2, q3, s8, 16
    esp.vsub.s16 q2, q2, q1
    esp.vmul.s16 q2, q2, q4
    esp.vadd.s16 q1, q1, q1
    esp.vadd.s16 q1, q1, q1
    esp.vadd.s16 q1, q1, q1
    esp.vadd.s16 q1, q1, q2
    esp.vsub.s16 q1, q1, q0
    esp.vmul.s16 q1, q1, q5
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q1
    esp.vsadds.s16 q0, q0, t6
    esp.vsld.16 q0, q0, q6
    esp.vunzip.8 q0, q3
    esp.vst.128.ip q0, a2, 16
    add a0, a0, a1
    add a2, a2, a3
    addi a4, a4, -1
    bnez a4, 1b
2:
    lw s8, 0(sp)
    addi sp, sp, 8
    ret
    .size vdec_k_bilinear, .-vdec_k_bilinear

    .balign 4
    .global vdec_k_copy
    .type vdec_k_copy, @function
vdec_k_copy:
    addi a1, a1, -16
    addi a3, a3, -16
    blez a4, 2f
1:
    esp.vld.128.ip q0, a0, 16
    esp.vst.128.ip q0, a2, 16
    add a0, a0, a1
    add a2, a2, a3
    addi a4, a4, -1
    bnez a4, 1b
2:
    ret
    .size vdec_k_copy, .-vdec_k_copy

    .balign 4
    .global vdec_k_pack
    .type vdec_k_pack, @function
vdec_k_pack:
    blez a3, 2f
1:
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    esp.vld.128.ip q2, a1, 16
    esp.vunzip.8 q1, q2
    esp.vzipt.8 q0, q1, q2
    esp.vst.128.ip q0, a2, 16
    esp.vst.128.ip q1, a2, 16
    esp.vst.128.ip q2, a2, 16
    addi a3, a3, -1
    bnez a3, 1b
2:
    ret
    .size vdec_k_pack, .-vdec_k_pack

    .balign 4
    .global vdec_k_unpack
    .type vdec_k_unpack, @function
vdec_k_unpack:
    blez a3, 2f
1:
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, 16
    esp.vld.128.ip q2, a0, 16
    esp.vunzipt.8 q0, q1, q2
    esp.vzip.8 q1, q2
    esp.vst.128.ip q0, a1, 16
    esp.vst.128.ip q1, a2, 16
    esp.vst.128.ip q2, a2, 16
    addi a3, a3, -1
    bnez a3, 1b
2:
    ret
    .size vdec_k_unpack, .-vdec_k_unpack

    .balign 4
    .global vdec_k_unpack_rows
    .type vdec_k_unpack_rows, @function
vdec_k_unpack_rows:
    addi a1, a1, -48
    addi a3, a3, -16
    addi a5, a5, -32
    blez a6, 2f
1:
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, 16
    esp.vld.128.ip q2, a0, 16
    esp.vunzipt.8 q0, q1, q2
    esp.vzip.8 q1, q2
    esp.vst.128.ip q0, a2, 16
    esp.vst.128.ip q1, a4, 16
    esp.vst.128.ip q2, a4, 16
    add a0, a0, a1
    add a2, a2, a3
    add a4, a4, a5
    addi a6, a6, -1
    bnez a6, 1b
2:
    ret
    .size vdec_k_unpack_rows, .-vdec_k_unpack_rows

    .balign 4
    .global vdec_k_prepare
    .type vdec_k_prepare, @function
vdec_k_prepare:
    esp.movx.r.cfg t6
    ori t6, t6, 3
    esp.movx.w.cfg t6
    li t6, 0
    esp.movx.w.sar t6
    ret
    .size vdec_k_prepare, .-vdec_k_prepare
