;*****************************************************************************
; fir_ring_q31_ymod_yonly_dspic33ak.s
;
; NOT a Microchip file.  Written for this project.  See fir_ring_q31_dspic33ak.s
; for why it lives in the vendored tree without the "mchp_" prefix.
;
; _fir_ring_q31_ymod_yonly_block -- Q31 FIR over a hardware-modulo history ring
; with Y MODULO ONLY.  Same 1 instruction per MAC, same single-copy history, but
; it cannot disturb code running in any other interrupt context.
;
;   q31_t *fir_ring_q31_ymod_yonly_block(
;       const q31_t *coeff,     /* w0  taps entries, NO modulo -- X or program  */
;       const q31_t *hist,      /* w1  Y space, oldest sample of this window    */
;       uint32_t taps,          /* w2  >= 2                                     */
;       q31_t *out,             /* w3  outputs written here                      */
;       uint32_t outputs,       /* w4  >= 1                                      */
;       uint32_t decim_bytes,   /* w5  decimation factor * 4                     */
;       const q31_t *ring,      /* w6  YMODSRT (ring base)                       */
;       uint32_t ring_bytes);   /* w7  ring length in bytes                      */
;
;   returns the updated history pointer.
;
; WHY THIS EXISTS -- the X modulo in fir_ring_q31_ymod_dspic33ak.s is unsafe here.
;
; MODCON, XMODSRT/XMODEND and YMODSRT/YMODEND are NOT part of the per-IPL
; register context: DS70005591C Table 4-2 lists W0-W15, W0-W7, ACCA/ACCB, PC, SR,
; SPLIM, RCOUNT and CORCON, and the text above it describes the Modulo Addressing
; control registers as being held *in addition to* the programmer's model.  So a
; modulo setting made in one context is live in every other one.
;
; That matters very differently for the two spaces, and the datasheet is explicit:
;
;   * "The MCU class of instructions operates solely through the X memory AGU"
;     and "The X AGU Circular Addressing can be used with any of the MCU class of
;     instructions" (4.3.2).  So with XMODEN set and XWM naming w0, an ordinary
;     C load through w0 -- in ANY context, including a higher-IPL ISR that
;     preempts this kernel -- is folded into this kernel's coefficient run.  That
;     is not something a coding convention can prevent: it would have to forbid
;     every higher-priority handler from using one particular W register as a
;     pointer.
;   * "The Y AGU supports word and long word-sized data reads for the DSP
;     MAC-class of instructions only" (4.3.16).  So YMODEN can only ever affect
;     the Y-side operand of a MAC-class instruction.  Ordinary C code cannot
;     reach the Y AGU at all, even when the address it touches lies inside Y
;     space -- that access goes through the X RAGU.
;
; So this kernel enables Y modulo and leaves X modulo OFF.  The residual condition
; is narrow and checkable rather than a convention: no handler that can preempt
; this kernel may execute a MAC-class instruction.  Verified 2026-08-21 by
; disassembling the whole image and listing every function containing one -- the
; only hits were the two bench kernels, and __delay32/__data_init/_memset use
; REPEAT only, which no AGU modulo affects.  That check is a grep over the
; disassembly, so it can be run again in CI whenever an ISR changes.
;
; COST OF GIVING UP X MODULO: the coefficient pointer no longer rewinds itself
; after each output, so it is reloaded from w11 -- ONE instruction per output,
; i.e. 1/190 of a tap loop, 0.005 cycles/MAC.  Against that, XMODSRT/XMODEND
; drop out of the save/program/restore sequence, so the per-call fixed cost goes
; DOWN by more than the per-output cost goes up for any realistic block size.
;
; It also lifts a restriction: with X modulo gone the coefficients no longer have
; to satisfy the modulo start-address rules, so they may live in program flash.
; That is what makes "coefficients in flash, history in Y" expressible at all.
;
; Placement requirements are otherwise as fir_ring_q31_dspic33ak.s: coefficients
; and history must be in DIFFERENT spaces or the two reads serialise and the MAC
; costs one extra cycle (4.3.17, measured as exactly 1.012 -> 2.000 cycles/MAC).
;
;-----------------------------------------------------------------------------
; DIAGNOSTIC A/B -- APP_ASRC_REPEAT_IRQ_INHIBIT_AB.  NOT A DESIGN, NOT A FIX.
; OFF BY DEFAULT AND ABSENT FROM EVERY ORDINARY BUILD: with the symbol undefined
; this file assembles to exactly the instructions it did before, so the shipping
; image and every previously linked image contain no DISICTL at all.
;
; WHAT IT ASKS.  Eight STACK ERROR traps under APP_ASRC_RATE_MONOTONIC_ISR=1 all
; report PCTRAP = _fir_ring_q31_ymod_yonly_block + 0x54, which this ELF pins to
; the mac.l IMMEDIATELY AFTER `repeat w8` (report section 19.12.5).  Two
; explanations survive the static audits: REPEAT alone, or REPEAT *while a
; higher-priority interrupt is taken*.  Only RM=1 creates the second one --
; PRIO_TDM_DMA is 4 and rate-monotonic drops the longer-deadline leg to 3, so the
; IPL-4 leg can preempt the IPL-3 leg inside this kernel, and with RM=0 (both at
; 4) it cannot.  That is exactly the observed reproduce/not-reproduce boundary.
;
; So inhibit interrupts at IPL <= APP_ASRC_REPEAT_IPLT (4) across the REPEAT and
; nothing else.  If the trap disappears, "REPEAT active AND a higher-priority
; interrupt entry" is a necessary condition; if it survives, that pairing is
; refuted and REPEAT stands alone.  Either way this is a MEASUREMENT, not a
; product fix: the inhibited window is one tap run (~190 MACs, ~1 us) per output
; against measured worst-case margins of 0.5-1.2 us, so it can plausibly buy the
; trap back as a miss or a starve.  Read miss/starve/sat alongside the verdict.
;
; WHY DISICTL AND NOT SR.IPL OR GIE.  DS70005591C 11.10.1.1: DISICTL selects an
; IPL threshold (IPLT) 0-7, inhibits requests at or below it, and those requests
; "remain pending until such time that the IPLT is lowered" -- nothing is lost.
; `DISICTL #lit3, Wd` sets the threshold AND saves the previous one to Wd in one
; instruction, so entry state is restored exactly rather than assumed to be zero;
; this kernel is called from an ISR, so assuming is not allowed.  It writes no
; SR.IPL, hence no per-IPL register-bank switch.  IPL 5+ and all traps are left
; alone by construction (11.10.1.2 confirms traps ignore even GIE).  The two
; alternatives were checked and are unusable: __builtin_disi does not exist for
; this core, and the SET_CPU_IPL family expands to SRbits.IPL, which
; p33AK512MPS512.h does not define (report section 19.9.2 area).
;
; NOT BLOCKED BY THE REPEAT RESTRICTION LIST.  4.3.15.1.3 forbids DISICTL (and
; DTB, LNK, ULNK, PWRSAV, RESET, flow control, another REPEAT) from being the
; instruction that IMMEDIATELY FOLLOWS a REPEAT.  Here the instruction after
; `repeat` is still `mac.l`; the restoring DISICTL runs after the repeated MAC
; has finished.  The order is asserted in the disassembly before flashing.
;
; w2 (taps) is dead once the setup block has derived the counters from it, and w2
; is banked (W0-W7), so the saved threshold needs no stack slot and cannot leak
; between contexts.  If the trap fires inside the region the restore never runs
; and IPLT stays raised -- which does not matter, because the trap handler resets.
;-----------------------------------------------------------------------------
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

.ifdef APP_ASRC_REPEAT_IRQ_INHIBIT_AB
    ; Threshold for the diagnostic bracket below.  4 = PRIO_TDM_DMA, i.e. inhibit
    ; the rate-monotonic preemptor and everything under it, leaving IPL 5+ (the
    ; ASRC clock-control CCP at 5, UART2 TX at 5 in the stream build) and all
    ; traps free to preempt.  Overridable so a follow-up can try 7 without
    ; touching this file:  -AsDefine APP_ASRC_REPEAT_IPLT=7
.ifndef APP_ASRC_REPEAT_IPLT
.equ APP_ASRC_REPEAT_IPLT, 4
.endif
.endif

    .global    _fir_ring_q31_ymod_yonly_block
_fir_ring_q31_ymod_yonly_block:

    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  CORCON
    push.l  MODCON
    push.l  YMODSRT
    push.l  YMODEND

    fractsetup w8                   ; fractional mode: sets sacr.l alignment

    mov.l   #0x401F, w8             ; YMODEN, YWM = w1, XWM = 1111 (X modulo OFF)
    mov.l   w8, MODCON

    mov.l   w6, YMODSRT             ; history ring
    add.l   w6, w7, w9
    sub.l   #1, w9
    mov.l   w9, YMODEND

    add.l   w6, w7, w9              ; w9 = one past the ring end, for the wrap test
    sub.l   w2, #2, w8              ; w8 = taps-2, the REPEAT count
    mov.l   w1, w10                 ; w10 = start of this output's tap window
    mov.l   w0, w11                 ; w11 = coefficient base, reloaded per output

_yonly_out:
    mov.l   w11, w0                 ; the whole price of dropping X modulo
    mov.l   w10, w1                 ; restart the window.  w1 is NOT reusable across
                                    ; outputs: after taps steps it sits taps*4 further
                                    ; on (mod the ring), which only equals the window
                                    ; start when the ring is exactly taps*4 long.
    mpy.l   [w0]+=4, [w1]+=4, a     ; a  = h[0] * x[0]
.ifdef APP_ASRC_REPEAT_IRQ_INHIBIT_AB
    disictl #APP_ASRC_REPEAT_IPLT, w2   ; DIAGNOSTIC ONLY: save prior IPLT into w2 and
                                        ; inhibit IPL <= IPLT.  Requests stay pending.
.endif
    repeat  w8
    mac.l   [w0]+=4, [w1]+=4, a     ; a += h[k] * x[k]   (taps-1 times, wraps in HW)
.ifdef APP_ASRC_REPEAT_IRQ_INHIBIT_AB
    disictl w2                          ; DIAGNOSTIC ONLY: restore the exact prior IPLT.
                                        ; Straight-line, so it runs on every exit.
.endif
    sacr.l  a, [w3++]               ; rounded Q31 output

    add.l   w10, w5, w10            ; step the window by the decimation factor.
    cp.l    w10, w9                 ; ALU adds do not wrap -- only the AGU does,
    bra     ltu, _yonly_no_wrap     ; so fold the ring by hand here.
    sub.l   w10, w7, w10
_yonly_no_wrap:
    dtb     w4, _yonly_out

    mov.l   w10, w0                 ; return the updated window start

    pop.l   YMODEND
    pop.l   YMODSRT
    pop.l   MODCON
    pop.l   CORCON
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return

    .end
