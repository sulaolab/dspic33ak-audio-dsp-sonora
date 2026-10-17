/* SPDX-License-Identifier: MIT-0 */

/*
 * Portable core of nora_pwm_audio: argument checks, the PCM -> duty block
 * FIFO, counters and the fault latch. Hardware lives in the board backend
 * (nora_pwm_audio_backend.h).
 *
 * FIFO: NORA_PWM_AUDIO_FIFO_BLOCKS slots, each holding every channel of one
 * audio block, so a stereo block is published or dropped as one unit and the
 * consumer can never take one channel without the other. One producer
 * (write()) moves s_head, one consumer (the backend's DMA ISR via take()/
 * release()) moves s_tail; both are single bytes, so each side reads the
 * other's index in one access on 16- and 32-bit parts alike.
 */

#include "nora_pwm_audio_backend.h"

#if NORA_PWM_AUDIO_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if (NORA_PWM_AUDIO_MAX_CHANNELS < 1) || (NORA_PWM_AUDIO_MAX_CHANNELS > 2)
#error "NORA_PWM_AUDIO_MAX_CHANNELS must be 1 or 2"
#endif
#if (NORA_PWM_AUDIO_FIFO_BLOCKS < 2) || (NORA_PWM_AUDIO_FIFO_BLOCKS > 255)
#error "NORA_PWM_AUDIO_FIFO_BLOCKS must be 2..255"
#endif
#ifndef NORA_PWM_AUDIO_PRIME_BLOCKS
#define NORA_PWM_AUDIO_PRIME_BLOCKS 1u
#endif
#if (NORA_PWM_AUDIO_PRIME_BLOCKS < 1) || \
    (NORA_PWM_AUDIO_PRIME_BLOCKS >= NORA_PWM_AUDIO_FIFO_BLOCKS)
#error "NORA_PWM_AUDIO_PRIME_BLOCKS must be 1..NORA_PWM_AUDIO_FIFO_BLOCKS-1"
#endif

#define NORA_PWM_AUDIO_BARRIER() __asm__ volatile ("" ::: "memory")

static nora_pwm_audio_duty_t
    s_fifo[NORA_PWM_AUDIO_FIFO_BLOCKS][NORA_PWM_AUDIO_MAX_CHANNELS]
          [NORA_PWM_AUDIO_FRAMES_PER_BLOCK];
static volatile uint8_t s_head;   /* written only by write() */
static volatile uint8_t s_tail;   /* written only by release() */

static nora_pwm_audio_modulator_t s_mod;
static uint8_t s_channels;
static bool s_initialized;
static volatile bool s_running;
/* Set by the first accepted block after start(): neutral output before it is
 * start-up silence, not an underrun. */
static volatile bool s_had_data;
/* Consumer only: false until the FIFO holds NORA_PWM_AUDIO_PRIME_BLOCKS, and
 * again after an underrun, so playback restarts with the same margin. */
static bool s_primed;

static volatile uint32_t s_written;
static volatile uint32_t s_rejected;
static volatile uint32_t s_underrun;
/* One byte per category: a fault ISR at any priority sets its own byte
 * without a read-modify-write of a shared word. */
static volatile uint8_t s_fault_stage;
static volatile uint8_t s_fault_dma;
static volatile uint8_t s_fault_clock;

static uint8_t next_slot(uint8_t i)
{
    ++i;
    return (i == NORA_PWM_AUDIO_FIFO_BLOCKS) ? 0u : i;
}

static uint32_t fault_flags(void)
{
    uint32_t flags = 0u;
    if (s_fault_stage != 0u) flags |= NORA_PWM_AUDIO_FAULT_STAGE;
    if (s_fault_dma != 0u)   flags |= NORA_PWM_AUDIO_FAULT_DMA;
    if (s_fault_clock != 0u) flags |= NORA_PWM_AUDIO_FAULT_CLOCK;
    return flags;
}

/* A 32-bit load is two accesses on a 16-bit part. Two equal reads in a row
 * rule out an update that preempts this reader. They cannot rule out a reader
 * that preempts an update half way: both reads then see the same half-written
 * value. That case is excluded by the get_status() rule in the header. */
static uint32_t read_counter(const volatile uint32_t *counter)
{
    uint32_t a, b;
    do {
        a = *counter;
        b = *counter;
    } while (a != b);
    return a;
}

nora_pwm_audio_result_t nora_pwm_audio_init(const nora_pwm_audio_config_t *cfg)
{
    nora_pwm_audio_result_t result;

    if (cfg == NULL) return NORA_PWM_AUDIO_INVALID_ARGUMENT;
    if (cfg->channel_count == 0u || cfg->frames_per_block == 0u ||
        cfg->sample_rate_hz == 0u) {
        return NORA_PWM_AUDIO_INVALID_ARGUMENT;
    }
    if (cfg->sample_rate_hz != NORA_PWM_AUDIO_SAMPLE_RATE_HZ ||
        cfg->frames_per_block != NORA_PWM_AUDIO_FRAMES_PER_BLOCK ||
        cfg->channel_count > NORA_PWM_AUDIO_MAX_CHANNELS) {
        return NORA_PWM_AUDIO_UNSUPPORTED;
    }

    nora_pwm_audio_stop();
    s_initialized = false;
    result = nora_pwm_audio_backend_init(cfg->channel_count, &s_mod);
    if (result != NORA_PWM_AUDIO_OK) return result;

    s_channels = cfg->channel_count;
    s_head = 0u;
    s_tail = 0u;
    s_written = 0u;
    s_rejected = 0u;
    s_underrun = 0u;
    s_fault_stage = 0u;
    s_fault_dma = 0u;
    s_fault_clock = 0u;
    s_initialized = true;
    return NORA_PWM_AUDIO_OK;
}

nora_pwm_audio_result_t nora_pwm_audio_start(void)
{
    nora_pwm_audio_result_t result;

    if (!s_initialized) return NORA_PWM_AUDIO_NOT_READY;
    if (fault_flags() != 0u) return NORA_PWM_AUDIO_FAULT;
    if (s_running) return NORA_PWM_AUDIO_OK;

    /* The DMA ISR is not running yet (stop() or init() left it off), so the
     * consumer index can be reset from here. */
    s_head = 0u;
    s_tail = 0u;
    s_had_data = false;
    s_primed = false;
    result = nora_pwm_audio_backend_start();
    if (result != NORA_PWM_AUDIO_OK) {
        nora_pwm_audio_backend_stop();
        return result;
    }
    if (fault_flags() != 0u) {
        nora_pwm_audio_backend_stop();
        return NORA_PWM_AUDIO_FAULT;
    }
    NORA_PWM_AUDIO_BARRIER();
    s_running = true;
    return NORA_PWM_AUDIO_OK;
}

void nora_pwm_audio_stop(void)
{
    s_running = false;
    NORA_PWM_AUDIO_BARRIER();
    if (s_initialized) nora_pwm_audio_backend_stop();
}

nora_pwm_audio_result_t nora_pwm_audio_write(const nora_pwm_audio_block_t *block)
{
    uint8_t head, next;
    uint16_t i;

    if (block == NULL || block->left_q31 == NULL) {
        return NORA_PWM_AUDIO_INVALID_ARGUMENT;
    }
    if (s_channels == 2u && block->right_q31 == NULL) {
        return NORA_PWM_AUDIO_INVALID_ARGUMENT;
    }
    if (fault_flags() != 0u) return NORA_PWM_AUDIO_FAULT;
    if (!s_running) return NORA_PWM_AUDIO_NOT_READY;

    head = s_head;
    next = next_slot(head);
    if (next == s_tail) {
        ++s_rejected;
        return NORA_PWM_AUDIO_BUSY;
    }
    for (i = 0u; i < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++i) {
        s_fifo[head][0][i] = (nora_pwm_audio_duty_t)
            nora_pwm_audio_modulate_q31(block->left_q31[i], &s_mod);
    }
#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
    if (s_channels == 2u) {
        for (i = 0u; i < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++i) {
            s_fifo[head][1][i] = (nora_pwm_audio_duty_t)
                nora_pwm_audio_modulate_q31(block->right_q31[i], &s_mod);
        }
    }
#endif
    NORA_PWM_AUDIO_BARRIER();
    s_head = next;
    ++s_written;
    s_had_data = true;
    return NORA_PWM_AUDIO_OK;
}

void nora_pwm_audio_get_status(nora_pwm_audio_status_t *out)
{
    if (out == NULL) return;
    out->written_blocks = read_counter(&s_written);
    out->rejected_blocks = read_counter(&s_rejected);
    out->underrun_blocks = read_counter(&s_underrun);
    out->fault_flags = fault_flags();
}

static uint8_t fifo_level(uint8_t head, uint8_t tail)
{
    return (head >= tail) ? (uint8_t)(head - tail)
                          : (uint8_t)(head + NORA_PWM_AUDIO_FIFO_BLOCKS - tail);
}

bool nora_pwm_audio_core_ready(void)
{
    uint8_t level;

    if (!s_running) return false;
    level = fifo_level(s_head, s_tail);
    return s_primed ? (level != 0u) : (level >= NORA_PWM_AUDIO_PRIME_BLOCKS);
}

bool nora_pwm_audio_core_take(const nora_pwm_audio_duty_t *duty[NORA_PWM_AUDIO_MAX_CHANNELS])
{
    uint8_t tail = s_tail;
    uint8_t head = s_head;
    uint8_t level;
    uint8_t ch;

    if (!s_running) return false;
    level = fifo_level(head, tail);
    if (level == 0u) s_primed = false;
    if (!s_primed && (level < NORA_PWM_AUDIO_PRIME_BLOCKS)) {
        if (s_had_data) ++s_underrun;
        return false;
    }
    s_primed = true;
    NORA_PWM_AUDIO_BARRIER();
    for (ch = 0u; ch < NORA_PWM_AUDIO_MAX_CHANNELS; ++ch) {
        duty[ch] = s_fifo[tail][ch];
    }
    return true;
}

void nora_pwm_audio_core_release(void)
{
    NORA_PWM_AUDIO_BARRIER();
    s_tail = next_slot(s_tail);
}

void nora_pwm_audio_core_fault(uint32_t category)
{
    s_running = false;
    if ((category & NORA_PWM_AUDIO_FAULT_STAGE) != 0u) s_fault_stage = 1u;
    if ((category & NORA_PWM_AUDIO_FAULT_DMA) != 0u)   s_fault_dma = 1u;
    if ((category & NORA_PWM_AUDIO_FAULT_CLOCK) != 0u) s_fault_clock = 1u;
}

#endif /* NORA_PWM_AUDIO_ENABLE */
