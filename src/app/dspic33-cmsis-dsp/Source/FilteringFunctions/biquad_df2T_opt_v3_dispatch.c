/*
 * biquad_df2T_opt_v3_dispatch.c - block-length dispatch for the opt_v3 DF2T
 * kernel.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * biquad_cascade_df2T_f32_dspic33ak_opt_v3 is a two-sample unrolled kernel, so
 * it can only process EVEN block lengths. opt_v1 accepts any length in 1..512,
 * and opt_v3 is meant to be a drop-in replacement for it, so something has to
 * reconcile the two. There were two places to put that reconciliation:
 *
 *   inside the kernel  - a single-sample tail after the pair loop. Correct, but
 *                        it puts a test and a branch inside the STAGE loop,
 *                        which runs 84 times per channel per block, to serve a
 *                        case no caller in this tree produces.
 *   outside the kernel - here. The decision is made ONCE PER CALL, so the
 *                        84-stage production and Tier 1 paths pay one branch per
 *                        block rather than one per stage.
 *
 * The second was chosen. The measured path is then exactly the loop being
 * evaluated, with no dispatch cost inside the region a benchmark times, and the
 * odd-length contract is still honoured because this function routes odd blocks
 * to opt_v1 whole.
 *
 * ROUTING IS WHOLE-BLOCK, NOT SPLIT
 * ---------------------------------
 * An odd block goes to opt_v1 in its entirety - it is NOT split into an even
 * part for opt_v3 plus one sample for opt_v1. Splitting would be faster by one
 * sample's worth of instructions and would introduce a real hazard: each kernel
 * walks pSrc and pDst per stage and reloads the state per stage, so a split
 * would have to run the whole 84-stage cascade twice over two different sample
 * ranges, with the second pass starting from state the first pass left mid-block.
 * That is a different computation, not a faster one. Whole-block routing cannot
 * get it wrong.
 *
 * WHAT THIS MEANS FOR AN ODD-LENGTH CALLER
 * ----------------------------------------
 * It gets opt_v1's speed, not opt_v3's. That is the honest outcome of an
 * even-only optimisation and it is why this is a dispatch rather than a claim of
 * universal improvement. No caller in this tree is affected: APP_BLOCK_FRAMES
 * and IIR_BENCH_FRAMES are both 32.
 *
 * DELIBERATELY NOT gated on a feature macro. The wrapper is a pure addition with
 * no caller of its own until something chooses it, and -ffunction-sections plus
 * --gc-sections drops all three functions from any image that does not reference
 * them - the same reason the other candidate kernels in this directory cost the
 * shipping image nothing.
 */
#include <stdint.h>

#include "dsp/filtering_functions.h"

/*
 * Both kernels are assembly and deliberately outside the library's public
 * header surface, so they are declared here rather than included. Identical
 * signatures by construction: opt_v3 was written as a drop-in for opt_v1, with
 * the same instance, coefficient and state layout.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

/* Even and non-zero blockSize only. Refuses anything else without writing, so
 * this wrapper's test is the thing that keeps that promise rather than a comment
 * asking callers to. */
extern void biquad_cascade_df2T_f32_dspic33ak_opt_v3(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

/*
 * The public entry point: opt_v1's contract, opt_v3's speed whenever the block
 * length allows it.
 *
 * blockSize == 0 goes to opt_v1, which rejects it the same way it always has -
 * this wrapper does not invent a new error behaviour for a case the underlying
 * contract already calls invalid.
 */
void biquad_cascade_df2T_f32_dspic33ak_opt_v3_block(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize )
{
    if( ( blockSize != 0u ) && ( ( blockSize & 1u ) == 0u ) )
    {
        biquad_cascade_df2T_f32_dspic33ak_opt_v3( S, pSrc, pDst, blockSize );
    }
    else
    {
        biquad_cascade_df2T_f32_dspic33ak_opt_v1( S, pSrc, pDst, blockSize );
    }
}
