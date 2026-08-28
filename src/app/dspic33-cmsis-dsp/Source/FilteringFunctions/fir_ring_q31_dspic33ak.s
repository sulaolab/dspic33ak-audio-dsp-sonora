;*****************************************************************************
; fir_ring_q31_dspic33ak.s
;
; NOT a Microchip file.  Written for this project.  It sits in the vendored
; dspic33-cmsis-dsp tree only so that it assembles with the same include path
; and lands in the same code section as the vendor kernels.  The "mchp_"
; prefix is deliberately NOT used, so the provenance stays readable: the
; upstream library contains no ASRC or multi-channel FIR kernel at all.
;
; _fir_ring_q31 -- Q31 FIR tap loop at the ISA floor of 1 cycle per MAC.
;
;   void fir_ring_q31(const q31_t *coeff,   /* w0 */
;                     const q31_t *hist,    /* w1 */
;                     uint32_t taps,        /* w2, >= 2 */
;                     q31_t *out);          /* w3 */
;
; Why this reaches 1 cycle/MAC where no float kernel in this tree can:
;   MAC Wxp*Wyp,A is 1 word / 1 cycle (DS70005591C Table 39-2, instr #55) and
;   fetches BOTH operands itself -- one through the X RAGU, one through the
;   Y AGU, concurrently (DS70005591C 4.3.16.3).  REPEAT wraps a single
;   instruction with zero loop overhead (4.3.15.1; Table 39-2 instr #73), so
;   the tap loop contains no branch at all.  DTB, the multi-instruction loop
;   primitive, costs 2/3 cycles on every taken iteration (Table 39-2 #39),
;   which is why the vendor's own mchp_fir_q31 DTB loop is ~3x this one.
;   The FPU mac.s takes register operands only, so each float MAC needs its
;   own mov.l loads and can never go below 2 instructions per MAC.
;
; Placement requirements -- functionally invisible, performance critical:
;   * coeff in X data space, hist in Y data space.  If both land in X space
;     the two reads serialise and the datasheet charges "typically one cycle"
;     extra per MAC (4.3.17): the kernel silently halves in speed with no
;     functional symptom.  Y space is 0x6000..0x7FFF on AK128 and
;     0xC000..0x13FFF on AK512 (__YDATA_BASE/__YDATA_END in the DFP
;     p33AK*.gld).  From C: __attribute__((space(ymemory))) / space(xmemory).
;   * hist is a MIRRORED ring -- every sample stored twice, at write and at
;     write+taps -- so the newest `taps` samples are one contiguous ascending
;     run.  That is what lets the Y pointer be a plain [w1]+=4 with no modulo
;     setup, and it is the trick the C front end already uses.
;   * hist is channel-major: one contiguous ring per channel.  MAC-class
;     post-modification is fixed at the operand width, so a frame-interleaved
;     history cannot be walked with a per-channel stride.
;
; The coefficient run must be symmetric (linear phase): the loop walks
; coefficients and history in the same direction rather than convolving.
; Every band-limiting FIR in this project is symmetric.
;
; Accumulator A is clobbered and not saved, matching the vendor Q31 kernels.
;
; Cost: taps + 5 instructions for taps MACs (107 taps -> 1.047 instr/MAC).
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _fir_ring_q31
    .type      _fir_ring_q31, @function
_fir_ring_q31:

    push.l  CORCON
    fractsetup w4                   ; fractional mode: sets sacr.l alignment

    sub.l   #2, w2                  ; tap 0 is the mpy.l below, and
                                    ; REPEAT Wn runs its target Wn+1 times

    mpy.l   [w0]+=4, [w1]+=4, a     ; a  = h[0] * x[0]
    repeat  w2
    mac.l   [w0]+=4, [w1]+=4, a     ; a += h[k] * x[k]   (taps-1 times)
    sacr.l  a, [w3++]               ; rounded Q31 result

    pop.l   CORCON
    return
    .size   _fir_ring_q31, .-_fir_ring_q31
    ; Sized on purpose.  Without it this label owns, as far as any tool that
    ; recovers function boundaries from the image is concerned, every
    ; instruction the linker happens to place after it that belongs to no
    ; sized symbol -- which on 2026-08-22 made tools/asrc/ymod_safety_gate.py
    ; see another function's retfie inside this kernel and report it as an
    ; interrupt handler that inherits a modulo window.  The kernel had not
    ; changed; the code placed after it had.

    .end
