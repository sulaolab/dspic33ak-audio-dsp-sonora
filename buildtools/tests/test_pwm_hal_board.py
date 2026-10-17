"""Host integration of unchanged hal_pwm_audio and the actual Sonora board backend.
Does not simulate electrical PWM edges or DMA timing on the MCU.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
HAL=ROOT/'src/app/hal_pwm_audio'
CC=Path(r'C:\msys64\usr\bin\gcc.exe')
os.environ['PATH']=str(CC.parent)+os.pathsep+os.environ.get('PATH','')
MOCK=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define SONORA_CLOCK_PWM_MCLK_HZ 798720000u
#define SAMPLE_RATE 48000u
#define APP_BLOCK_FRAMES 32u
#define PWM_AUDIO_UPSAMPLE_FACTOR (APP_PWM_OUTPUT == 1 ? TEST_CARRIER_FACTOR : 2u)
#define PWM_AUDIO_PERIOD_COUNT_Q4 ((798720000u/48000u/PWM_AUDIO_UPSAMPLE_FACTOR)<<4)
#include "mpq6612a_board.h"
#define NORA_PWM_GEN_5 5
#define NORA_PWM_GEN_6 6
#define NORA_DMA_CHANNEL_4 4
#define NORA_DMA_CHANNEL_5 5
#define NORA_DMA_TRIGGER_PWM_GEN5 5
#define NORA_DMA_TRMODE_REPEAT_ONESHOT 1
#define NORA_DMA_ADDR_INCREMENT 1
#define NORA_DMA_ADDR_FIXED 0
#define NORA_DMA_SIZE_WORD 4
#define NORA_PWM_MCLK_HIGH_FREQ 1
#define NORA_PWM_MODE_COMPLEMENTARY 1
#define NORA_GPIO_LEVEL_HIGH 1
#define NORA_GPIO_LEVEL_LOW 0
#define SONORA_CLOCK_PWM_OK 0
#define NORA_DMA_HALF_FIRST 1
#define NORA_DMA_HALF_SECOND 2
#define NORA_DMA_DSPIC33AK_STAT_HALF 1u
#define NORA_DMA_DSPIC33AK_STAT_DONE 2u
#define NORA_DMA_DSPIC33AK_STAT_OVERRUN 4u
typedef int nora_gpio_rp_t,nora_dma_channel_t,nora_dma_trigger_t,nora_pwm_generator_id_t,nora_clock_status_t,nora_dma_half_t;
typedef unsigned nora_dma_status_t;
typedef struct {volatile void *src,*dst;unsigned count,src_mode,dst_mode,size,tr_mode,trigger,irq_priority;bool reload_count,reload_src,reload_dst,half_int_en,done_int_en,irq_priority_set,irq_enable;} nora_dma_channel_cfg_t;
typedef struct {unsigned period,duty_init,dead_time_high,dead_time_low,rp_h,rp_l,output_mode,trigger_master;bool pen_h,pen_l,trigger_follows,is_module_master,start_overridden,start_disabled;} nora_pwm_generator_cfg_t;
static struct {unsigned PLL2RDY;} OSCCTRLbits={1};
static struct {unsigned CLKRDY;} CLK5CONbits={1};
static struct {unsigned ON;} PG5CONbits,PG6CONbits;
static struct {unsigned CHEN;} DMA4CHbits,DMA5CHbits;
static volatile uint32_t PG5DC,PG6DC;
static bool pin[128],ovr[8];
static unsigned pin_access[128];
static uintptr_t dma_src[8];
static nora_dma_channel_cfg_t dma_cfg[8];
static nora_pwm_generator_cfg_t pg_cfg[8];
static unsigned fail_at,calls,events[128],event_count;
void pwm_audio_board_service(void);
static nora_dma_status_t irq_status;
static unsigned forced_fault_low_mask;
static bool maybe_fail(void){return ++calls!=fail_at;}
static bool nora_gpio_rp_clear(int p){++pin_access[p];pin[p]=false;return true;}
static bool nora_gpio_rp_set(int p){++pin_access[p];pin[p]=true;return true;}
static bool nora_gpio_rp_config_digital_output(int p,bool v){++pin_access[p];pin[p]=v;return maybe_fail();}
static bool nora_gpio_rp_config_digital_input(int p){++pin_access[p];pin[p]=true;return maybe_fail();}
static int nora_gpio_rp_read(int p){
    ++pin_access[p];
#if APP_PWM_OUTPUT == 1
    if(p==MPQ6612A_FAULT_RP&&(forced_fault_low_mask&1u))return 0;
#if MPQ6612A_STEREO
    if(p==MPQ6612A_RIGHT_FAULT_RP&&(forced_fault_low_mask&2u))return 0;
#endif
#endif
    return pin[p];
}
static int nora_gpio_rp_read_output(int p){++pin_access[p];return pin[p];}
static bool nora_pwm_generator_override(int id,bool en,bool high,bool sync){(void)high;(void)sync;ovr[id]=en;return maybe_fail();}
static bool nora_pwm_generator_run(int id,bool on){if(id==5)PG5CONbits.ON=on;else PG6CONbits.ON=on;if(on)events[event_count++]=100+id;return maybe_fail();}
static uint32_t PG5PER;
static bool nora_pwm_generator_init(int id,const nora_pwm_generator_cfg_t *c){if(id==5)PG5PER=c->period;assert(c->start_disabled&&c->start_overridden);++pin_access[c->rp_h];++pin_access[c->rp_l];pg_cfg[id]=*c;ovr[id]=true;return maybe_fail();}
static bool nora_dma_channel_enable(int id,bool on){if(id==4)DMA4CHbits.CHEN=on;else DMA5CHbits.CHEN=on;return maybe_fail();}
static bool nora_dma_channel_config(int id,const nora_dma_channel_cfg_t *c){dma_cfg[id]=*c;dma_src[id]=(uintptr_t)c->src;return maybe_fail();}
static uintptr_t nora_dma_read_src(int id){return dma_src[id];}
static nora_dma_status_t nora_dma_isr_snapshot(int id){(void)id;return irq_status;}
static nora_dma_status_t nora_dma_read_status(int id){(void)id;return 0;}
static void nora_dma_clear_status(int id){(void)id;}
static bool nora_dma_status_has_overrun(nora_dma_status_t s){return (s&4)!=0;}
static bool nora_dma_status_has_half_done_conflict(nora_dma_status_t s){return (s&3)==3;}
static int nora_dma_half_from_status(nora_dma_status_t s){return (s&2)?2:((s&1)?1:0);}
static int sonora_clock_pwm_prepare(nora_clock_status_t *d){*d=0;return 0;}
static bool nora_pwm_module_init(int c){(void)c;return true;}
static uint32_t GetTicks(void){return 1000;}
static void delay_us(unsigned t){(void)t;}
'''
TEST=r'''
static void trigger(unsigned half){
    irq_status=half;
    unsigned active=half==1?BOARD_HALF_WORDS:0;
    dma_src[4]=(uintptr_t)(s_duty_l+active);
#if BOARD_CHANNELS == 2
    dma_src[5]=(uintptr_t)(s_duty_r+active);
#endif
    _DMA4Interrupt();
}
static void assert_safe(void){
    assert(!PG5CONbits.ON&&!DMA4CHbits.CHEN);
#if BOARD_CHANNELS == 2
    assert(!PG6CONbits.ON&&!DMA5CHbits.CHEN);
#endif
#if APP_PWM_OUTPUT == 1
    assert(pg_cfg[5].rp_h==MPQ6612A_IN1_RP&&pg_cfg[5].rp_l==MPQ6612A_IN2_RP);
    assert(!pin[MPQ6612A_SLEEP_RP]);
    assert(s_route_mask==0u);
#if MPQ6612A_STEREO
    assert(!pin[MPQ6612A_RIGHT_SLEEP_RP]);
#endif
#endif
}
int main(void){
    nora_pwm_audio_config_t c={48000,32,BOARD_CHANNELS};
    int32_t l[32],r[32];
    nora_pwm_audio_block_t b={l,r};
    nora_pwm_audio_status_t st;
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    calls=0;event_count=0;assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    unsigned successful_start_calls=calls;
    assert(pg_cfg[5].period==BOARD_PERIOD_Q4);
    assert(dma_cfg[4].count==2u*32u*BOARD_FACTOR);
    assert(pg_cfg[5].dead_time_high==(APP_PWM_OUTPUT==1?0u:16u));
#if BOARD_CHANNELS == 2
    assert(events[0]==106&&events[1]==105);
    assert(dma_cfg[5].trigger==NORA_DMA_TRIGGER_PWM_GEN5);
    assert(!dma_cfg[5].irq_enable);
#endif
#if APP_PWM_OUTPUT == 1
    assert(pin[MPQ6612A_SLEEP_RP]==!PWM_AUDIO_SCOPE_ONLY);
#if MPQ6612A_STEREO
    assert(pin[MPQ6612A_RIGHT_SLEEP_RP]==!PWM_AUDIO_SCOPE_ONLY);
#endif
#endif
    for(unsigned i=0;i<32;i++){l[i]=INT32_MIN;r[i]=INT32_MAX;}
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
    trigger(1);
    for(unsigned i=0;i<BOARD_HALF_WORDS;i++){
        assert(s_duty_l[i]==BOARD_MIN_DUTY_Q4);
#if BOARD_CHANNELS == 2
        assert(s_duty_r[i]==BOARD_PERIOD_Q4-BOARD_MIN_DUTY_Q4);
#endif
    }
    trigger(2);trigger(1);
    assert(s_duty_l[0]==BOARD_NEUTRAL_Q4);
    nora_pwm_audio_get_status(&st);assert(st.written_blocks==2&&st.underrun_blocks==1&&st.fault_flags==0);
    /* A nonconstant block must keep sample order and repeat each duty for
     * exactly FACTOR carrier cycles, across both DMA halves. */
    for(unsigned i=0;i<32;i++){l[i]=(int32_t)((int64_t)i*100000000-1550000000);r[i]=-l[i];}
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_write(&b)==NORA_PWM_AUDIO_OK);
    for(unsigned half=2;half>=1;--half){
        trigger(half);
        unsigned off=half==1?0:BOARD_HALF_WORDS;
        for(unsigned i=0;i<32;i++){
            if(i)assert(s_duty_l[off+i*BOARD_FACTOR]>s_duty_l[off+(i-1)*BOARD_FACTOR]);
            for(unsigned k=1;k<BOARD_FACTOR;k++){
                assert(s_duty_l[off+i*BOARD_FACTOR+k]==s_duty_l[off+i*BOARD_FACTOR]);
#if BOARD_CHANNELS == 2
                assert(s_duty_r[off+i*BOARD_FACTOR+k]==s_duty_r[off+i*BOARD_FACTOR]);
#endif
            }
        }
    }
    irq_status=4;_DMA4Interrupt();
    nora_pwm_audio_get_status(&st);assert(st.fault_flags==NORA_PWM_AUDIO_FAULT_DMA);
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);
    nora_pwm_audio_stop();assert_safe();
    fail_at=0;assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    calls=0;nora_pwm_audio_stop();assert_safe();
    /* Inject every checked resource operation during start; every failure cleans both streams. */
    for(unsigned f=1;f<=successful_start_calls;f++){
        fail_at=0;forced_fault_low_mask=0u;assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
        calls=0;fail_at=f;
        nora_pwm_audio_result_t res=nora_pwm_audio_start();
        fail_at=0;
        if(res!=NORA_PWM_AUDIO_OK)assert_safe();
        nora_pwm_audio_stop();assert_safe();
    }
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    OSCCTRLbits.PLL2RDY=0;
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);assert_safe();
    OSCCTRLbits.PLL2RDY=1;
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
#if BOARD_CHANNELS == 2
    dma_src[4]=(uintptr_t)(s_duty_l+BOARD_HALF_WORDS);
    dma_src[5]=(uintptr_t)(s_duty_r+BOARD_HALF_WORDS+8);
    irq_status=1;_DMA4Interrupt();nora_pwm_audio_get_status(&st);assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_DMA);
    assert(s_phase_errors>0);nora_pwm_audio_stop();assert_safe();
#endif
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    CLK5CONbits.CLKRDY=0;pwm_audio_board_service();
    nora_pwm_audio_get_status(&st);assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_CLOCK);
    nora_pwm_audio_stop();assert_safe();CLK5CONbits.CLKRDY=1;
    /* A late refill must fault before overwriting the currently active half. */
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    dma_src[4]=(uintptr_t)s_duty_l;
#if BOARD_CHANNELS == 2
    dma_src[5]=(uintptr_t)s_duty_r;
#endif
    irq_status=1;_DMA4Interrupt();nora_pwm_audio_get_status(&st);
    assert(st.fault_flags&NORA_PWM_AUDIO_FAULT_DMA);assert(s_overlaps>0);
    nora_pwm_audio_stop();assert_safe();
#if APP_PWM_OUTPUT == 1 && !PWM_AUDIO_SCOPE_ONLY
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);forced_fault_low_mask=STAGE_ROUTE_ALL;
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);assert_safe();
#endif
#if APP_PWM_OUTPUT == 1 && MPQ6612A_STEREO && !PWM_AUDIO_SCOPE_ONLY
    /* Normal stereo builds run either populated route independently. The
     * inactive route stays asleep with its PWM output overridden. PG5/DMA4
     * still run internally when only the right route is active because PG5
     * is the timing/refill master for PG6/DMA5. */
    forced_fault_low_mask=STAGE_ROUTE_RIGHT;
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    assert(s_route_mask==STAGE_ROUTE_LEFT);
    assert(pin[MPQ6612A_SLEEP_RP]&&!pin[MPQ6612A_RIGHT_SLEEP_RP]);
    assert(!ovr[5]&&ovr[6]);
    nora_pwm_audio_stop();assert_safe();
    forced_fault_low_mask=STAGE_ROUTE_LEFT;
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    assert(s_route_mask==STAGE_ROUTE_RIGHT);
    assert(!pin[MPQ6612A_SLEEP_RP]&&pin[MPQ6612A_RIGHT_SLEEP_RP]);
    assert(ovr[5]&&!ovr[6]&&PG5CONbits.ON&&DMA4CHbits.CHEN);
    nora_pwm_audio_stop();assert_safe();
#endif
#if APP_PWM_OUTPUT == 1
    /* Explicit lab control works with nFAULT Low (absent/unpowered PCB),
     * including a fault latched by normal startup. Scope may wake only here. */
    nora_pwm_audio_stop();pwm_audio_board_set_lab_mode(true);
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
    forced_fault_low_mask=STAGE_ROUTE_ALL;
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_OK);
    assert(pin[MPQ6612A_SLEEP_RP]&&PG5CONbits.ON&&DMA4CHbits.CHEN);
    pwm_audio_board_service();nora_pwm_audio_get_status(&st);
    assert(st.fault_flags==0);
    nora_pwm_audio_stop();assert_safe();pwm_audio_board_set_lab_mode(false);
    assert(nora_pwm_audio_init(&c)==NORA_PWM_AUDIO_OK);
#if !PWM_AUDIO_SCOPE_ONLY
    assert(nora_pwm_audio_start()==NORA_PWM_AUDIO_FAULT);assert_safe();
#endif
    forced_fault_low_mask=0u;
#endif
    /* XPRO1-only must never configure, read or drive any MikroBUS B net,
     * including start/fault cleanup when the PWM PCB is absent. */
#if APP_PWM_OUTPUT == 1 && !MPQ6612A_ENABLE_MIKROB && !defined(APP_TARGET_AK506)
    assert(pg_cfg[5].rp_h==21&&pg_cfg[5].rp_l==18);
    assert(pin_access[98]==0&&pin_access[29]==0&&pin_access[102]==0&&pin_access[82]==0&&pin_access[4]==0&&pin_access[97]==0);
#endif
#if defined(APP_TARGET_AK506)
    /* WM8904 slot-1 SPI pins and its XTAL/RP24 belong to the codec/clock layer.
     * The slot-3 PWM backend must not touch slot-2 control nets reserved for POT. */
    assert(pin_access[24]==0&&pin_access[11]==0&&pin_access[55]==0&&pin_access[40]==0&&pin_access[39]==0);
#if MPQ6612A_NANO_SLOT == 3
    assert(pg_cfg[5].rp_h==23&&pg_cfg[5].rp_l==25);
    assert(pin_access[9]==0&&pin_access[19]==0&&pin_access[42]==0&&pin_access[41]==0&&pin_access[18]==0);
#else
    assert(pg_cfg[5].rp_h==9&&pg_cfg[5].rp_l==19);
    assert(pin_access[23]==0&&pin_access[25]==0&&pin_access[52]==0&&pin_access[51]==0&&pin_access[2]==0);
#endif
#endif
    printf("PASS board channels=%u scope=%u route=%u: L/R mapping, neutral, start order, cleanup, DMA/clock/stage faults\n",BOARD_CHANNELS,PWM_AUDIO_SCOPE_ONLY,APP_PWM_OUTPUT);
}
'''
def main():
    source=(ROOT/'src/app/board/audio/pwm_audio_board_ak.c').read_text()
    source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    source=source.replace('__attribute__((interrupt, context))','')
    source=source.replace('(uint32_t)s_duty_l','(uintptr_t)s_duty_l').replace('(uint32_t)s_duty_r','(uintptr_t)s_duty_r')
    cases = [(1,1,1,1,0),(1,1,1,0,0),(1,1,0,1,0),(1,1,0,0,0),(1,0,1,1,0),(1,0,1,0,0),(0,0,0,0,0)]
    cases += [(1,0,0,scope,slot) for slot in (2,3) for scope in (0,1)]
    cases = [(*case,factor) for case in cases for factor in ((1,2,4) if case[0] else (2,))]
    for output,b,xpro,scope,nano_slot,factor in cases:
        stereo=b and xpro
        with tempfile.TemporaryDirectory(prefix='pwm-hal-board-') as tmp:
            case=Path(tmp)
            ch=1+stereo if output==1 else 2
            conf=f'''#include <stdint.h>
#define NORA_PWM_AUDIO_ENABLE 1
#define NORA_PWM_AUDIO_MAX_CHANNELS {ch}
#define NORA_PWM_AUDIO_SAMPLE_RATE_HZ 48000u
#define NORA_PWM_AUDIO_FRAMES_PER_BLOCK 32u
#define NORA_PWM_AUDIO_FIFO_BLOCKS 4u
#define NORA_PWM_AUDIO_PRIME_BLOCKS 2u
#define NORA_PWM_AUDIO_DUTY_T uint32_t
'''
            (case/'nora_pwm_audio_conf.h').write_text(conf)
            (case/'app_specific_config_defs.h').write_text('/* Config supplied by this host case. */\n')
            nano = f'#define APP_TARGET_AK506 506\n#define APP_TARGET 506\n#define MPQ6612A_NANO_SLOT {nano_slot}\n' if nano_slot else ''
            harness=nano+f'#define TEST_CARRIER_FACTOR {factor}\n#define APP_PWM_OUTPUT {output}\n#define MPQ6612A_ENABLE_MIKROB {b}\n#define MPQ6612A_ENABLE_XPRO1 {xpro}\n#define MPQ6612A_STEREO {int(stereo)}\n#define PWM_AUDIO_SCOPE_ONLY {scope}\n'+MOCK+'\n#include "nora_pwm_audio_backend.h"\n'+source+'\n'+TEST
            (case/'test.c').write_text(harness)
            cmd=[str(CC),'-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I',str(case),'-I',str(ROOT/'src/app/board/audio'),'-I',str(HAL),str(case/'test.c'),str(HAL/'nora_pwm_audio.c'),str(HAL/'nora_pwm_audio_modulator.c'),'-o',str(case/'t.exe')]
            r=subprocess.run(cmd,capture_output=True,text=True)
            if r.returncode:raise RuntimeError(r.stdout+r.stderr)
            r=subprocess.run([str(case/'t.exe')],capture_output=True,text=True)
            if r.returncode:raise RuntimeError(r.stdout+r.stderr+str(r.returncode))
            print(r.stdout.strip())
if __name__=='__main__':main()
