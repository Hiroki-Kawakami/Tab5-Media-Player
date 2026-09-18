/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

    .section .rodata
    .balign 16
mpeg2_idct_tables:
    .short 16383, 16383, 16383, 16383, 16383, 16383, 16383, 16383
    .short 22725, 19266, 12873, 4520, -4520, -12873, -19266, -22725
    .short 21407, 8867, -8867, -21407, -21407, -8867, 8867, 21407
    .short 19266, -4520, -22725, -12873, 12873, 22725, 4520, -19266
    .short 16383, -16383, -16383, 16383, 16383, -16383, -16383, 16383
    .short 12873, -22725, 4520, 19266, -19266, -4520, 22725, -12873
    .short 8867, -21407, 21407, -8867, -8867, 21407, -21407, 8867
    .short 4520, -12873, 19266, -22725, 22725, -19266, 12873, -4520
    .short 16383, 22725, 21407, 19266, 16383, 12873, 8867, 4520
    .short 16383, 19266, 8867, -4520, -16383, -22725, -21407, -12873
    .short 16383, 12873, -8867, -22725, -16383, 4520, 21407, 19266
    .short 16383, 4520, -21407, -12873, 16383, 19266, -8867, -22725
    .short 16383, -4520, -21407, 12873, 16383, -19266, -8867, 22725
    .short 16383, -12873, -8867, 22725, -16383, -4520, 21407, -19266
    .short 16383, -19266, 8867, 4520, -16383, 22725, -21407, 12873
    .short 16383, -22725, 21407, -19266, 16383, -12873, 8867, -4520
    .short 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024
    .short 32, 32, 32, 32, 32, 32, 32, 32

    .text

.macro ROW_MAC
    esp.vsmulas.s16.qacc.ld.incp q3, s9, q2, q1, 0
    esp.vsmulas.s16.qacc.ld.incp q2, s9, q3, q1, 1
    esp.vsmulas.s16.qacc.ld.incp q3, s9, q2, q1, 2
    esp.vsmulas.s16.qacc.ld.incp q2, s9, q3, q1, 3
    esp.vsmulas.s16.qacc.ld.incp q3, s9, q2, q1, 4
    esp.vsmulas.s16.qacc.ld.incp q2, s9, q3, q1, 5
    esp.vsmulas.s16.qacc.ld.incp q3, s9, q2, q1, 6
    esp.vsmulas.s16.qacc q3, q1, 7
.endm

.macro COL_ROW out
    esp.vld.128.ip q1, s9, 16
    esp.zero.qacc
    esp.vsmulas.s16.qacc q6, q1, 0
    mv t4, a2
    esp.vld.128.ip q2, t4, 16
    esp.vsmulas.s16.qacc.ld.incp q3, t4, q2, q1, 0
    esp.vsmulas.s16.qacc.ld.incp q2, t4, q3, q1, 1
    esp.vsmulas.s16.qacc.ld.incp q3, t4, q2, q1, 2
    esp.vsmulas.s16.qacc.ld.incp q2, t4, q3, q1, 3
    esp.vsmulas.s16.qacc.ld.incp q3, t4, q2, q1, 4
    esp.vsmulas.s16.qacc.ld.incp q2, t4, q3, q1, 5
    esp.vsmulas.s16.qacc.ld.incp q3, t4, q2, q1, 6
    esp.vsmulas.s16.qacc q3, q1, 7
    esp.srcmb.s16.qacc \out, s8, 0
.endm

    .balign 4
    .global mpeg2_k_idct_put
    .type mpeg2_k_idct_put, @function
mpeg2_k_idct_put:
    li t5, 0
    j mpeg2_k_idct
    .size mpeg2_k_idct_put, .-mpeg2_k_idct_put

    .balign 4
    .global mpeg2_k_idct_add
    .type mpeg2_k_idct_add, @function
mpeg2_k_idct_add:
    li t5, 1
    j mpeg2_k_idct
    .size mpeg2_k_idct_add, .-mpeg2_k_idct_add

    .balign 4
    .type mpeg2_k_idct, @function
mpeg2_k_idct:
    addi sp, sp, -32
    sw s0, 0(sp)
    sw s1, 4(sp)
    sw s8, 8(sp)
    sw s9, 12(sp)
    sw s10, 16(sp)
    sw s11, 20(sp)
    la t3, mpeg2_idct_tables
    andi s0, a3, 0xFF
    srli s1, a3, 8
    addi s10, t3, 256
    esp.vld.128.ip q7, s10, 0
    li s8, 11
    mv t4, a2
    li s11, 8
1:
    andi t6, s0, 1
    beqz t6, 3f
    andi t6, s1, 1
    bnez t6, 2f
    esp.vldbc.16.ip q0, t4, 0
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q0
    esp.vadd.s16 q0, q0, q0
    esp.vst.128.ip q0, t4, 0
    j 3f
2:
    mv s9, t3
    esp.vld.128.ip q1, t4, 0
    esp.vld.128.ip q2, s9, 16
    esp.mov.s16.qacc q7
    ROW_MAC
    esp.srcmb.s16.qacc q0, s8, 0
    esp.vst.128.ip q0, t4, 0
3:
    addi t4, t4, 16
    srli s0, s0, 1
    srli s1, s1, 1
    addi s11, s11, -1
    bnez s11, 1b

    addi s9, t3, 128
    addi s10, t3, 272
    esp.vld.128.ip q6, s10, 0
    li s8, 20
    li s0, 0
    li s1, 255
    mv s10, a0
    li s11, 4
4:
    COL_ROW q4
    COL_ROW q5
    beqz t5, 5f
    esp.vldext.u8.ip q0, q7, s10, 0
    add s10, s10, a1
    esp.vadd.s16 q4, q4, q0
    esp.vldext.u8.ip q0, q7, s10, 0
    add s10, s10, a1
    esp.vadd.s16 q5, q5, q0
5:
    esp.vsat.s16 q4, q4, s0, s1
    esp.vsat.s16 q5, q5, s0, s1
    esp.vunzip.8 q4, q5
    esp.vst.l.64.xp q4, a0, a1
    esp.vst.h.64.xp q4, a0, a1
    addi s11, s11, -1
    bnez s11, 4b

    esp.zero.q q0
    mv t4, a2
    esp.vst.128.ip q0, t4, 16
    esp.vst.128.ip q0, t4, 16
    esp.vst.128.ip q0, t4, 16
    esp.vst.128.ip q0, t4, 16
    esp.vst.128.ip q0, t4, 16
    esp.vst.128.ip q0, t4, 16
    esp.vst.128.ip q0, t4, 16
    esp.vst.128.ip q0, t4, 16

    lw s0, 0(sp)
    lw s1, 4(sp)
    lw s8, 8(sp)
    lw s9, 12(sp)
    lw s10, 16(sp)
    lw s11, 20(sp)
    addi sp, sp, 32
    ret
    .size mpeg2_k_idct, .-mpeg2_k_idct
