/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_PWM_AUDIO_H
#define NORA_PWM_AUDIO_H

/*
 * nora_pwm_audio.h -- public PCM-to-PWM audio output contract.
 *
 * The application hands over finished planar Q1.31 PCM. Generator numbers,
 * DMA channels, pin routing, duty registers and the power stage stay behind
 * this header: one fixed board backend per build owns them. Selection,
 * mixing, rate conversion, mute and gain are done by the caller.
 *
 * v1 scope: 48 kHz, 32 frames per block, one output instance, 1 (L) or
 * 2 (L/R) channels as the built board allows.
 *
 * Context rules:
 *   init(), start(), stop()   foreground only; start() may wait for the
 *                             power stage.
 *   write()                   non-blocking; callable from the audio ISR.
 *                             One producer context only.
 *   get_status()              any context. fault_flags is always exact. Each
 *                             counter is read tear-free when the caller cannot
 *                             preempt the code that updates it -- write() for
 *                             written/rejected, the refill ISR for underrun --
 *                             i.e. from foreground or at the same or a lower
 *                             priority. A caller that does preempt it may see
 *                             that counter mid-update on a core with 16-bit
 *                             stores, and must range-check the value there.
 */

#include <stdint.h>

typedef struct {
    uint32_t sample_rate_hz;       /* v1: 48000 */
    uint16_t frames_per_block;     /* v1: 32 */
    uint8_t channel_count;         /* 1 = L, 2 = L/R */
} nora_pwm_audio_config_t;

typedef struct {
    const int32_t *left_q31;       /* frames_per_block samples */
    const int32_t *right_q31;      /* NULL for mono, required for stereo */
} nora_pwm_audio_block_t;

typedef enum {
    NORA_PWM_AUDIO_OK = 0,
    NORA_PWM_AUDIO_INVALID_ARGUMENT,  /* NULL or malformed argument */
    NORA_PWM_AUDIO_UNSUPPORTED,       /* rate/block/channel count not built */
    NORA_PWM_AUDIO_NOT_READY,         /* not initialized, not started */
    NORA_PWM_AUDIO_BUSY,              /* no room: the whole block was dropped */
    NORA_PWM_AUDIO_FAULT              /* a latched fault; see fault_flags */
} nora_pwm_audio_result_t;

enum {
    NORA_PWM_AUDIO_FAULT_STAGE = 1u << 0,
    NORA_PWM_AUDIO_FAULT_DMA   = 1u << 1,
    NORA_PWM_AUDIO_FAULT_CLOCK = 1u << 2
};

typedef struct {
    uint32_t written_blocks;   /* blocks accepted by write() */
    uint32_t rejected_blocks;  /* blocks dropped whole for lack of room */
    uint32_t underrun_blocks;  /* blocks the core could not supply after the first
                                * block accepted since start() (re-prime included) */
    uint32_t fault_flags;      /* NORA_PWM_AUDIO_FAULT_* */
} nora_pwm_audio_status_t;

/* Validate cfg against the build. On INVALID_ARGUMENT, or UNSUPPORTED from
 * that check, nothing changes: a running output keeps running and a latched
 * fault stays latched. Otherwise a running output is first brought to its
 * stopped safe state and the backend is initialised; if the backend fails,
 * the module is left uninitialised (a fault latched before stays latched).
 * On OK the counters and any latched fault are reset. */
nora_pwm_audio_result_t nora_pwm_audio_init(const nora_pwm_audio_config_t *cfg);

/* Fill every output buffer with neutral duty, bring up clock/generator/DMA,
 * then the power stage, and release the outputs. Fails to the stopped safe
 * state. Returns OK if already running. */
nora_pwm_audio_result_t nora_pwm_audio_start(void);

/* Copy one block. Stereo blocks are accepted or dropped as a whole; the input
 * may be reused as soon as this returns. */
nora_pwm_audio_result_t nora_pwm_audio_write(const nora_pwm_audio_block_t *block);

/* Reach the stopped safe state, then return. Repeatable. Counters are kept. */
void nora_pwm_audio_stop(void);

void nora_pwm_audio_get_status(nora_pwm_audio_status_t *out);

#endif /* NORA_PWM_AUDIO_H */
