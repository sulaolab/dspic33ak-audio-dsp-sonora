"""Host checks of actual MPQ lifecycle and stereo producer with mocked peripherals.

Does not model PWM edges, DMA arbitration, or physical POP behavior.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def extract(text, name):
    match = re.search(r'^(?:static )?(?:bool|void) ' + name + r'\(', text, re.M)
    assert match, name
    brace = text.index('{', match.start())
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]

def main():
    source = (ROOT / 'src/app/apps/classic/classic_audio_pwm.c').read_text(encoding='utf-8-sig')
    source = source.split('#else /* legacy direct float->duty path */', 1)[-1]
    converter = (ROOT / 'src/app/apps/shared/float_conversion.c').read_text(encoding='utf-8-sig')
    functions = '\n'.join(extract(source, name) for name in (
        'mpq_sleep_all', 'mpq_disable_all', 'classic_audio_pwm_fault_active',
        'classic_audio_pwm_start', 'classic_audio_pwm_stop', 'stereo_commit',
        'classic_audio_pwm_scope_process_pcm', 'classic_audio_pwm_process_left_primary'))
    # MCU addresses are 32 bits. Preserve the algorithm with native host pointers.
    functions = functions.replace('(uint32_t)s_mpq', '(uintptr_t)s_mpq')
    functions = functions.replace('(uint32_t)&s_mpq', '(uintptr_t)&s_mpq')
    functions = functions.replace('uint32_t base =', 'uintptr_t base =')
    functions = functions.replace('uint32_t src =', 'uintptr_t src =')
    functions = functions.replace('uint32_t boundary =', 'uintptr_t boundary =')
    functions = functions.replace('const uint32_t half_pwm_addr =', 'const uintptr_t half_pwm_addr =')
    functions = functions.replace('(uint32_t)&g_pwm', '(uintptr_t)&g_pwm')
    harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#define APP_PWM_OUTPUT 1
#ifndef PWM_AUDIO_SCOPE_ONLY
#define PWM_AUDIO_SCOPE_ONLY 1
#endif
#define MPQ6612A_STEREO 1
#define MPQ_CHANNEL_COUNT 2u
#define APP_BLOCK_FRAMES 32u
#define PWM_AUDIO_UPSAMPLE_FACTOR 1u
#define PWM_HALF_WORDS 32u
#define PWM_AUDIO_DUTY_WORD_COUNT 64u
#define PWM_COMMIT_RESERVE_WORDS 4u
#define PG_PER_COUNT 266240u
#define PG_DC_INIT 133120u
#define PWM_AUDIO_PERIOD_COUNT_Q4 PG_PER_COUNT
#define SONORA_CLOCK_PWM_MCLK_HZ 798720000u
#define ENA_DBG_SANITY_CHK 1
#define NORA_DMA_CHANNEL_4 0
typedef struct {
    unsigned generator, dma, trigger, sleep, fault;
    volatile uint32_t *buffer, *dc;
} mpq_channel_t;
typedef struct {
    uint32_t half, done, dma_errors, pcm_peak, pcm_blocks;
    uint32_t duty_min, duty_max, writes, write_overlap;
} pwm_scope_channel_t;
static volatile uint32_t buffers[2][64], dc[2];
#define g_pwm_audio_pg5_duty buffers[0]
static float Post_Gain_PWM=1.0f;
static const mpq_channel_t s_mpq[2] = {
    {0,0,0,0,0,buffers[0],&dc[0]}, {1,1,1,1,1,buffers[1],&dc[1]}
};
static volatile pwm_scope_channel_t s_scope[2];
static int32_t s_stereo_stage[2][32];
static uint32_t s_stereo_skips, s_stereo_phase_errors;
static bool s_pwm_ready, s_pwm_initialized=true, s_pwm_stopped=true;
static bool s_pwm_stream_fault;
#if PWM_AUDIO_SCOPE_ONLY
static bool s_scope_silence;
#endif
static bool sleep_pin[2], dma_on[2], pg_on[2], override_on[2], fault_high[2]={true,true};
static unsigned order[2], order_count, attempt, fail_at;
static unsigned position[2];
#define NORA_GPIO_LEVEL_HIGH 1
static bool step(void) { return ++attempt != fail_at; }
static void nora_gpio_rp_clear(unsigned pin) { sleep_pin[pin]=false; }
static void nora_gpio_rp_set(unsigned pin) { sleep_pin[pin]=true; }
static unsigned nora_gpio_rp_read(unsigned pin) { return fault_high[pin]; }
static bool nora_dma_channel_enable(unsigned ch, bool on) {
    if (on && !step()) return false;
    dma_on[ch]=on; return true;
}
static bool nora_pwm_generator_enable(unsigned ch, bool on) {
    if (on && !step()) return false;
    pg_on[ch]=on;
    if (on && order_count<2) order[order_count++]=ch;
    return true;
}
static bool nora_pwm_generator_pair_override(unsigned ch, bool on, bool high, bool sync) {
    (void)high; (void)sync;
    if (!on && !step()) return false;
    override_on[ch]=on; return true;
}
static void nora_pwm_generator_set_duty(unsigned ch, uint32_t duty) { dc[ch]=duty; }
static bool pwm_audio_dma_config(unsigned ch, unsigned trigger,
                                  volatile uint32_t *buf, volatile uint32_t *reg) {
    (void)ch; (void)trigger; (void)buf; (void)reg; return step();
}
static uintptr_t nora_dma_read_src(unsigned ch) { return (uintptr_t)&buffers[ch][position[ch]]; }
static void delay_us(unsigned us) { (void)us; }
CONVERTER
FUNCTIONS
static void stopped(void) {
    assert(!s_pwm_ready && s_pwm_stopped);
    for(unsigned ch=0; ch<2; ++ch)
        assert(!sleep_pin[ch] && !dma_on[ch] && !pg_on[ch] && override_on[ch]);
}
int main(void) {
    // Both neutral buffers are rebuilt on every restart, follower before master.
    assert(classic_audio_pwm_start());
    assert(order_count==2 && order[0]==1 && order[1]==0);
    for(unsigned ch=0; ch<2; ++ch) {
        assert(dma_on[ch] && pg_on[ch] && !override_on[ch]);
        assert(sleep_pin[ch] == !PWM_AUDIO_SCOPE_ONLY);
        for(unsigned i=0;i<64;++i) assert(buffers[ch][i]==PG_DC_INIT);
    }
    classic_audio_pwm_stop(); stopped();
    classic_audio_pwm_stop(); stopped();
    // Each configurable/start/release failure cleans up both channels.
    unsigned steps=attempt;
    for(unsigned failure=1; failure<=steps; ++failure) {
        attempt=0; fail_at=failure; order_count=0;
        assert(!classic_audio_pwm_start()); stopped();
    }
    fail_at=0; attempt=0; order_count=0;
#if !PWM_AUDIO_SCOPE_ONLY
    fault_high[1]=false;
    assert(!classic_audio_pwm_start()); stopped();
    fault_high[1]=true;
#endif
    assert(classic_audio_pwm_start());
    // Opposite full-scale L/R values must remain separate in the inactive half.
    position[0]=position[1]=0;
#if PWM_AUDIO_SCOPE_ONLY
    int32_t pcm[64];
    for(unsigned i=0;i<32;++i) { pcm[2*i]=INT32_MIN; pcm[2*i+1]=INT32_MAX; }
    classic_audio_pwm_scope_process_pcm(pcm,2);
    for(unsigned i=0;i<32;++i) {
        assert(buffers[0][i]==PG_DC_INIT && buffers[1][i]==PG_DC_INIT);
        assert(buffers[0][32+i]==256 && buffers[1][32+i]==265984);
    }
    s_scope_silence=true; position[0]=position[1]=32;
    classic_audio_pwm_scope_process_pcm(pcm,2);
    for(unsigned i=0;i<32;++i) assert(buffers[0][i]==PG_DC_INIT && buffers[1][i]==PG_DC_INIT);
    assert(s_scope[0].writes==2 && s_scope[1].writes==2);
#else
    float samples[64];
    for(unsigned i=0;i<32;++i) { samples[i]=-1.0f; samples[32+i]=1.0f; }
    classic_audio_pwm_process_left_primary(samples,2);
    for(unsigned i=0;i<32;++i) {
        assert(buffers[0][i]==PG_DC_INIT && buffers[1][i]==PG_DC_INIT);
        assert(buffers[0][32+i]==6384 && buffers[1][32+i]==259856);
    }
#endif
    position[0]=0; position[1]=32;
    assert(!stereo_commit() && !s_pwm_stream_fault); // different active halves
    position[0]=position[1]=30;
    assert(!stereo_commit() && !s_pwm_stream_fault); // insufficient copy headroom
    position[0]=0; position[1]=2;
    assert(!stereo_commit() && s_pwm_stream_fault && s_stereo_phase_errors==1);
    assert(classic_audio_pwm_fault_active());
    classic_audio_pwm_stop(); stopped();
    fail_at=0;
    assert(classic_audio_pwm_start() && !s_pwm_stream_fault);
    classic_audio_pwm_stop(); stopped();
    printf("PASS: mode=%s, follower-first neutral start, partial failure cleanup, stop/restart, L/R mapping and DMA ownership guards\n",
        PWM_AUDIO_SCOPE_ONLY ? "scope" : "normal");
}
'''.replace('CONVERTER', extract(converter, 'convert_codec_q31_to_pwm_20bit') + '\n' +
            extract(converter, 'convert_float_to_pwm_20bit')).replace('FUNCTIONS', functions)
    gcc = shutil.which('gcc') or (r'C:\msys64\usr\bin\gcc.exe' if os.name == 'nt' else None)
    if not gcc or not Path(gcc).is_file(): raise SystemExit('Host GCC required')
    env = os.environ.copy()
    env['PATH'] = str(Path(gcc).parent) + os.pathsep + env['PATH']
    with tempfile.TemporaryDirectory(prefix='pwm-stereo-') as temp:
        test = Path(temp)/'check.c'
        test.write_text(harness, encoding='utf-8')
        for mode in (0,1):
            binary = Path(temp)/('check.exe' if os.name == 'nt' else 'check')
            subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror',
                '-Wno-unused-function',f'-DPWM_AUDIO_SCOPE_ONLY={mode}',str(test),'-o',str(binary),'-lm'],check=True,env=env)
            subprocess.run([str(binary)],check=True,env=env)

if __name__ == '__main__': main()
