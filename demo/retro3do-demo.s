/*
 * Retro-3DO original bare-metal demonstration ROM.
 *
 * Copyright (c) 2026 Crown Park Computing.
 * MIT licensed with the rest of Retro-3DO. No Portfolio code or libraries.
 *
 * Assembled big-endian for ARMv4, while deliberately using only the ARMv3
 * instruction set implemented by the 3DO's ARM60 (no BX, no halfword loads).
 */

        .syntax unified
        .arch armv4
        .arm

        .equ VRAM,              0x00200000
        .equ MADAM,             0x03300000
        .equ MADAM_DMA_ENABLE,  0x0008
        .equ MADAM_PBUS_ADDR,   0x0570
        .equ MADAM_PBUS_LENGTH, 0x0574
        .equ MADAM_PBUS_PTR,    0x0578
        .equ MADAM_VDL,         0x0580
        .equ CLIO,              0x03400000
        .equ CLIO_VCOUNT,       0x0034
        .equ CLIO_DSP_PROGRAM2, 0x1800
        .equ CLIO_DSP_RUN,      0x17fc

        .equ VDL_ADDR,          0x00001000
        .equ PBUS_ADDR,         0x00008000
        .equ TEST_ADDR,         0x00009000
        .equ FRAME_WORDS,       41920       /* 320 * 262 / two lines */

        .section .vectors, "ax", %progbits
        .global _start
_start:
        b       reset
        b       hang
        b       hang
        b       hang
        b       hang
        b       hang
        b       hang
        b       hang

        .section .text, "ax", %progbits
reset:
        ldr     sp, =0x001ff000

        /* A first DRAM write removes the reset ROM overlay. Execution remains
         * at the ROM's high mapping, exactly as it does on the console. */
        ldr     r0, =TEST_ADDR
        ldr     r1, =0x55aa33cc
        str     r1, [r0]
        ldr     r2, [r0]
        cmp     r1, r2
        moveq   r10, #1
        movne   r10, #0

        bl      clear_framebuffer
        bl      setup_vdl
        bl      setup_audio
        bl      draw_static_screen

        mov     r8, #152               /* marker x */
        mov     r9, #204               /* marker y */
        ldr     r11, =0x00007fff        /* white RGB555 */

main_loop:
        mov     r0, r8
        mov     r1, r9
        ldr     r2, =0x00000842         /* erase with background */
        bl      draw_marker

        bl      read_pad
        tst     r0, #0x08000000         /* up */
        subne   r9, r9, #2
        tst     r0, #0x10000000         /* down */
        addne   r9, r9, #2
        tst     r0, #0x02000000         /* left */
        subne   r8, r8, #2
        tst     r0, #0x04000000         /* right */
        addne   r8, r8, #2

        cmp     r8, #18
        movlt   r8, #18
        ldr     r7, =294
        cmp     r8, r7
        movgt   r8, r7
        cmp     r9, #190
        movlt   r9, #190
        cmp     r9, #228
        movgt   r9, #228

        ldr     r11, =0x00007fff
        tst     r0, #0x01000000         /* A: red */
        ldrne   r11, =0x00007c00
        tst     r0, #0x00800000         /* B: green */
        ldrne   r11, =0x000003e0
        tst     r0, #0x00400000         /* C: blue */
        movne   r11, #31

        mov     r0, r8
        mov     r1, r9
        mov     r2, r11
        bl      draw_marker
        bl      wait_for_next_field
        b       main_loop

hang:
        b       hang

/* Fill the entire 262-line NTSC field with a dark blue RGB555 value. */
clear_framebuffer:
        stmfd   sp!, {r4-r6, lr}
        ldr     r4, =VRAM
        ldr     r5, =0x08420842
        ldr     r6, =FRAME_WORDS
1:
        str     r5, [r4], #4
        subs    r6, r6, #1
        bne     1b
        ldmfd   sp!, {r4-r6, pc}

/* Test bars and project identification. The text renderer is a small 5x7
 * bitmap font authored below; it writes through the real VRAM layout. */
draw_static_screen:
        stmfd   sp!, {r4, lr}

        mov     r0, #0
        mov     r1, #16
        mov     r2, #320
        mov     r3, #12
        ldr     r4, =0x00007c00
        bl      fill_rect
        mov     r0, #0
        mov     r1, #28
        mov     r2, #320
        mov     r3, #12
        ldr     r4, =0x000003e0
        bl      fill_rect
        mov     r0, #0
        mov     r1, #40
        mov     r2, #320
        mov     r3, #12
        mov     r4, #31
        bl      fill_rect

        ldr     r0, =title_text
        mov     r1, #91
        mov     r2, #62
        ldr     r3, =0x00007fff
        bl      draw_text
        ldr     r0, =subtitle_text
        mov     r1, #40
        mov     r2, #82
        ldr     r3, =0x000057ff
        bl      draw_text

        ldr     r0, =cpu_text
        mov     r1, #34
        mov     r2, #108
        ldr     r3, =0x000003ff
        bl      draw_text
        ldr     r0, =video_text
        mov     r1, #34
        mov     r2, #120
        ldr     r3, =0x000003ff
        bl      draw_text
        ldr     r0, =timer_text
        mov     r1, #34
        mov     r2, #132
        ldr     r3, =0x000003ff
        bl      draw_text
        ldr     r0, =audio_text
        mov     r1, #34
        mov     r2, #144
        ldr     r3, =0x000003ff
        bl      draw_text
        ldr     r0, =input_text
        mov     r1, #34
        mov     r2, #156
        ldr     r3, =0x000003ff
        bl      draw_text
        ldr     r0, =help_text
        mov     r1, #55
        mov     r2, #178
        ldr     r3, =0x00007fe0
        bl      draw_text

        /* Turn the RAM line red if the readback failed. */
        cmp     r10, #0
        bne     1f
        mov     r0, #244
        mov     r1, #108
        mov     r2, #40
        mov     r3, #8
        ldr     r4, =0x00007c00
        bl      fill_rect
1:
        ldmfd   sp!, {r4, pc}

setup_vdl:
        stmfd   sp!, {r4-r6, lr}
        ldr     r4, =VDL_ADDR
        ldr     r5, =0x00210105         /* DMA, current override, 262 lines */
        str     r5, [r4, #0]
        ldr     r5, =VRAM
        str     r5, [r4, #4]
        str     r5, [r4, #8]
        mov     r5, #0
        str     r5, [r4, #12]
        ldr     r6, =MADAM + MADAM_VDL
        str     r4, [r6]
        ldmfd   sp!, {r4-r6, pc}

/* DSP program: keep a 16-bit phase accumulator in DSP data memory, advance it
 * by 0x100 per sample, attenuate it by three bits, send it to both DACs, and
 * sleep. The resulting quiet 172 Hz sawtooth verifies the program window, DSP
 * arithmetic and persistent data, both DAC channels, and host audio output. */
setup_audio:
        ldr     r0, =CLIO + CLIO_DSP_PROGRAM2
        ldr     r1, =0x66208100
        str     r1, [r0], #4
        ldr     r1, =0xc1008900
        str     r1, [r0], #4
        ldr     r1, =0x440d8100
        str     r1, [r0], #4
        ldr     r1, =0x89019bfe
        str     r1, [r0], #4
        ldr     r1, =0x81019bff
        str     r1, [r0], #4
        ldr     r1, =0x81018380
        str     r1, [r0]
        ldr     r0, =CLIO + CLIO_DSP_RUN
        mov     r1, #1
        str     r1, [r0]
        mov     pc, lr

/* Trigger an eight-byte player-bus report and return its first report word. */
read_pad:
        ldr     r1, =MADAM
        ldr     r2, =PBUS_ADDR
        str     r2, [r1, #MADAM_PBUS_ADDR]
        mov     r3, #12
        str     r3, [r1, #MADAM_PBUS_LENGTH]
        mov     r3, #0
        str     r3, [r1, #MADAM_PBUS_PTR]
        mov     r3, #0x8000
        str     r3, [r1, #MADAM_DMA_ENABLE]
        ldr     r0, [r2, #4]
        mov     pc, lr

wait_for_next_field:
        ldr     r0, =CLIO + CLIO_VCOUNT
        ldr     r1, [r0]
1:
        ldr     r2, [r0]
        eor     r3, r1, r2
        tst     r3, #0x800
        beq     1b
        mov     pc, lr

/* r0=x, r1=y, r2=RGB555. Preserves caller registers. */
put_pixel:
        stmfd   sp!, {r3-r6, lr}
        ldr     r3, =VRAM
        mov     r4, r1, lsr #1
        mov     r5, #320
        mul     r4, r5, r4
        add     r4, r4, r0
        add     r3, r3, r4, lsl #2
        ldr     r4, [r3]
        tst     r1, #1
        bne     1f
        ldr     r5, =0x0000ffff
        and     r4, r4, r5
        orr     r4, r4, r2, lsl #16
        b       2f
1:
        ldr     r5, =0xffff0000
        and     r4, r4, r5
        orr     r4, r4, r2
2:
        str     r4, [r3]
        ldmfd   sp!, {r3-r6, pc}

/* r0=x, r1=y, r2=width, r3=height, r4=RGB555. */
fill_rect:
        stmfd   sp!, {r5-r11, lr}
        mov     r5, r0
        mov     r6, r1
        add     r7, r0, r2
        add     r8, r1, r3
1:
        mov     r9, r5
2:
        mov     r0, r9
        mov     r1, r6
        mov     r2, r4
        bl      put_pixel
        add     r9, r9, #1
        cmp     r9, r7
        blt     2b
        add     r6, r6, #1
        cmp     r6, r8
        blt     1b
        ldmfd   sp!, {r5-r11, pc}

/* r0=x, r1=y, r2=colour. */
draw_marker:
        stmfd   sp!, {r4, lr}
        mov     r4, r2
        mov     r2, #10
        mov     r3, #10
        bl      fill_rect
        ldmfd   sp!, {r4, pc}

/* r0=NUL text, r1=x, r2=y, r3=colour. */
draw_text:
        stmfd   sp!, {r4-r11, lr}
        mov     r4, r0                 /* text */
        mov     r5, r1                 /* pen x */
        mov     r6, r2                 /* pen y */
        mov     r11, r3                /* colour */
char_loop:
        ldrb    r7, [r4], #1
        cmp     r7, #0
        beq     text_done
        cmp     r7, #32
        blt     char_advance
        cmp     r7, #90
        bgt     char_advance
        sub     r7, r7, #32
        mov     r8, #7
        mul     r7, r8, r7
        ldr     r8, =font_5x7
        add     r8, r8, r7
        mov     r9, #0                 /* row */
row_loop:
        ldrb    r10, [r8, r9]
        mov     r7, #0                 /* column */
col_loop:
        mov     r0, #0x10
        mov     r0, r0, lsr r7
        tst     r10, r0
        beq     pixel_done
        add     r0, r5, r7
        add     r1, r6, r9
        mov     r2, r11
        bl      put_pixel
pixel_done:
        add     r7, r7, #1
        cmp     r7, #5
        blt     col_loop
        add     r9, r9, #1
        cmp     r9, #7
        blt     row_loop
char_advance:
        add     r5, r5, #6
        b       char_loop
text_done:
        ldmfd   sp!, {r4-r11, pc}

        .section .rodata, "a", %progbits
title_text:     .asciz "RETRO 3DO"
subtitle_text:  .asciz "ORIGINAL HOME-BREW DEMO ROM"
cpu_text:       .asciz "ARM60 CPU AND RAM       PASS"
video_text:     .asciz "MADAM VDLP VIDEO        PASS"
timer_text:     .asciz "CLIO VIDEO TIMER        PASS"
audio_text:     .asciz "DSP AUDIO OUTPUT        PASS"
input_text:     .asciz "PBUS CONTROLLER INPUT   PASS"
help_text:      .asciz "D-PAD MOVES  A B C COLOUR"

/* ASCII 32..90, five low bits per row. Only glyphs used above need ink. */
font_5x7:
        .byte 0,0,0,0,0,0,0             /* space */
        .rept 12                         /* ! through , */
        .byte 0,0,0,0,0,0,0
        .endr
        .byte 0,0,0x1f,0,0,0,0          /* - */
        .byte 0,0,0,0,0,0x0c,0x0c       /* . */
        .byte 1,2,4,8,16,0,0             /* / */
        .byte 14,17,19,21,25,17,14       /* 0 */
        .byte 4,12,4,4,4,4,14            /* 1 */
        .byte 14,17,1,2,4,8,31           /* 2 */
        .byte 30,1,1,14,1,1,30           /* 3 */
        .byte 2,6,10,18,31,2,2           /* 4 */
        .byte 31,16,16,30,1,1,30         /* 5 */
        .byte 14,16,16,30,17,17,14       /* 6 */
        .byte 31,1,2,4,8,8,8             /* 7 */
        .byte 14,17,17,14,17,17,14       /* 8 */
        .byte 14,17,17,15,1,1,14         /* 9 */
        .rept 7                          /* : through @ */
        .byte 0,0,0,0,0,0,0
        .endr
        .byte 14,17,17,31,17,17,17       /* A */
        .byte 30,17,17,30,17,17,30       /* B */
        .byte 14,17,16,16,16,17,14       /* C */
        .byte 30,17,17,17,17,17,30       /* D */
        .byte 31,16,16,30,16,16,31       /* E */
        .byte 31,16,16,30,16,16,16       /* F */
        .byte 14,17,16,23,17,17,15       /* G */
        .byte 17,17,17,31,17,17,17       /* H */
        .byte 14,4,4,4,4,4,14            /* I */
        .byte 7,2,2,2,18,18,12           /* J */
        .byte 17,18,20,24,20,18,17       /* K */
        .byte 16,16,16,16,16,16,31       /* L */
        .byte 17,27,21,21,17,17,17       /* M */
        .byte 17,25,21,19,17,17,17       /* N */
        .byte 14,17,17,17,17,17,14       /* O */
        .byte 30,17,17,30,16,16,16       /* P */
        .byte 14,17,17,17,21,18,13       /* Q */
        .byte 30,17,17,30,20,18,17       /* R */
        .byte 15,16,16,14,1,1,30         /* S */
        .byte 31,4,4,4,4,4,4             /* T */
        .byte 17,17,17,17,17,17,14       /* U */
        .byte 17,17,17,17,17,10,4        /* V */
        .byte 17,17,17,21,21,21,10       /* W */
        .byte 17,17,10,4,10,17,17        /* X */
        .byte 17,17,10,4,4,4,4           /* Y */
        .byte 31,1,2,4,8,16,31           /* Z */

        .ltorg
