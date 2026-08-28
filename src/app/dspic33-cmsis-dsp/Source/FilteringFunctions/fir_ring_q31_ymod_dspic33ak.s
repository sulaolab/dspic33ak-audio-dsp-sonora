;*****************************************************************************
; fir_ring_q31_ymod_dspic33ak.s
;
; NOT a Microchip file.  Written for this project.  See fir_ring_q31_dspic33ak.s
; for why it lives in the vendored tree without the "mchp_" prefix.
;
; _fir_ring_q31_ymod_block -- Q31 FIR over a HARDWARE-MODULO history ring,
; still 1 instruction per MAC, with no mirrored copy of the history.
;
;   q31_t *fir_ring_q31_ymod_block(
;       const q31_t *coeff,     /* w0  X space, taps entries, X-modulo        */
;       const q31_t *hist,      /* w1  Y space, oldest sample of this window  */
;       uint32_t taps,          /* w2  >= 2                                   */
;       q31_t *out,             /* w3  outputs written here                    */
;       uint32_t outputs,       /* w4  >= 1                                    */
;       uint32_t decim_bytes,   /* w5  decimation factor * 4                   */
;       const q31_t *ring,      /* w6  YMODSRT (ring base)                     */
;       uint32_t ring_bytes);   /* w7  ring length in bytes                    */
;
;   returns the updated history pointer.
;
; Why this exists next to the mirrored kernel:
;   The mirrored kernel stores every sample twice so the tap run is contiguous
;   and needs no modulo at all.  That costs 2x the history RAM.  The Y AGU
;   however supports Modulo Addressing *for the DSP class of instructions*
;   (DS70005591C 4.3.16.2 / 4.3.16.3), and MAC.l is exactly that class -- so
;   [w1]+=4 can wrap in hardware and the second copy is unnecessary.
;   The vendor's own mchp_fir_q31 already does this, with no dummy mpy.l:
;   the dummy is only needed by the FLOAT kernels, where the sample is fetched
;   with mov.l through the X AGU and so cannot use the Y AGU's DSP modulo.
;   (An earlier report claimed mirroring was required for 1 cycle/MAC.  That
;   generalised the float constraint to Q31 and was wrong.)
;
; The inner loop is byte-identical in cost to the mirrored kernel -- one
; MAC.l inside a REPEAT, no branch.  What differs is per-call setup: six SFRs
; saved and programmed once, then amortised over outputs * taps MACs.
;
; X modulo on the coefficients is what makes the per-output rewind free: after
; taps steps w0 wraps back to coeff[0] on its own, so the loop never reloads it.
;
; The CORCON DSP bits, ACCA/ACCB and RCOUNT are all part of the per-IPL register
; context (Figure 4-2 Note 3, 4.3.9), so an ISR at a distinct IPL cannot have
; them clobbered by another ISR, and the fractsetup below could be hoisted to
; once per context.  It is kept per call so the kernel stays callable from the
; foreground too; drop it if the caller guarantees the mode.  Traps are the one
; exception -- they run in the pre-trap context -- so trap handlers must not use
; DSP instructions, REPEAT or CORCON.
;
; ATTENTION -- start address restriction.  DS70005591C 4.3.18.1 states that an
; incrementing modulo buffer has "certain restrictions on the buffer start
; address" and that power-of-two lengths are the exception that lifts them, but
; it does NOT give the numeric rule, and the vendor init routine enforces
; nothing.  Non-power-of-two LENGTHS are fine and documented -- Figure 4-19 wraps
; a 0x4000..0x40AF buffer -- so the open question is placement, not length.  Until that is settled on hardware, size the ring to a power of two
; (>= taps*4) and align it to that size: legal under either reading, and it
; additionally makes the buffer bidirectional.  A 190-tap ring then occupies
; 1024 B rather than 760 B, i.e. the saving over mirroring is 33% not 50%.
;
; One circular buffer exists per space, so a multi-channel caller reprograms
; YMODSRT/YMODEND per channel.  That is why the ring bounds are parameters.
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _fir_ring_q31_ymod_block
_fir_ring_q31_ymod_block:

    push.l  w8
    push.l  w9
    push.l  w10
    push.l  CORCON
    push.l  MODCON
    push.l  XMODSRT
    push.l  XMODEND
    push.l  YMODSRT
    push.l  YMODEND

    fractsetup w8                   ; fractional mode: sets sacr.l alignment

    mov.l   #0xC010, w8             ; XMODEN|YMODEN, XWM = w0, YWM = w1
    mov.l   w8, MODCON

    mov.l   w0, XMODSRT             ; coefficients: one full pass then wrap
    sl.l    w2, #2, w8              ; w8 = taps * 4
    add.l   w0, w8, w9
    sub.l   #1, w9
    mov.l   w9, XMODEND

    mov.l   w6, YMODSRT             ; history ring
    add.l   w6, w7, w9
    sub.l   #1, w9
    mov.l   w9, YMODEND

    add.l   w6, w7, w9              ; w9 = one past the ring end, for the wrap test
    sub.l   w2, #2, w8              ; w8 = taps-2, the REPEAT count
    mov.l   w1, w10                 ; w10 = start of this output's tap window

_ymod_out:
    mov.l   w10, w1                 ; restart the window.  w1 is NOT reusable
                                    ; across outputs: after taps steps it sits
                                    ; taps*4 further on (mod the ring), which
                                    ; only equals the window start when the ring
                                    ; is exactly taps*4 long -- and the ring is
                                    ; deliberately allowed to be longer.
    mpy.l   [w0]+=4, [w1]+=4, a     ; a  = h[0] * x[0]
    repeat  w8
    mac.l   [w0]+=4, [w1]+=4, a     ; a += h[k] * x[k]   (taps-1 times, wraps in HW)
    sacr.l  a, [w3++]               ; rounded Q31 output

    add.l   w10, w5, w10            ; step the window by the decimation factor.
    cp.l    w10, w9                 ; ALU adds do not wrap -- only the AGU does,
    bra     ltu, _ymod_no_wrap      ; so fold the ring by hand here.
    sub.l   w10, w7, w10
_ymod_no_wrap:
    dtb     w4, _ymod_out

    mov.l   w10, w0                 ; return the updated window start

    pop.l   YMODEND
    pop.l   YMODSRT
    pop.l   XMODEND
    pop.l   XMODSRT
    pop.l   MODCON
    pop.l   CORCON
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return

    .end
