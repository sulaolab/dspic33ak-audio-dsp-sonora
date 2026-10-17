"""Exercise actual *ts orchestration, Classic hook and HAL/legacy PWM stops."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = Path(r'C:\msys64\usr\bin\gcc.exe')


def function(text, signature, offset=0):
    start = text.index(signature, offset)
    end, depth = text.index('{', start) + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    transport = (ROOT/'src/app/audio_transport/audio_transport.c').read_text(encoding='utf-8-sig')
    classic = (ROOT/'src/app/apps/classic/classic_demo_app.c').read_text(encoding='utf-8-sig')
    pwm = (ROOT/'src/app/apps/classic/classic_audio_pwm.c').read_text(encoding='utf-8-sig')
    stop = function(transport, 'bool audio_transport_stop_for_flash( void )')
    hook = function(classic, 'static void classic_transport_stop_aux_output( void* user )')
    assert '.stop_aux_output = classic_transport_stop_aux_output' in classic
    hal_stop = function(pwm, 'void classic_audio_pwm_stop(void)')
    legacy_stop = function(pwm, 'void classic_audio_pwm_stop(void)', pwm.index('/* legacy direct'))
    mock = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define I2C_INST_A 1
#define I2C_INST_B 2
#define NORA_TDM_USE_SPI2 1
#define RESOLVED_SAI_TEST_LIVE_ENABLED 0
#define AUDIO_TRANSPORT_ROUTE_HAL_DIRECT 1
#define APP_PWM_OUTPUT 0
#define APP_USE_PWM_AUDIO 1
#define NORA_DMA_CHANNEL_4 4
#define NORA_DMA_CHANNEL_5 5
#define NORA_DMA_CHANNEL_6 6
#define NORA_DMA_CHANNEL_7 7
#define NORA_PWM_GEN_5 5
#define NORA_PWM_GEN_6 6
#define NORA_PWM_GEN_7 7
#define NORA_PWM_GEN_8 8
static bool s_preflash_stop_latched,s_preflash_stop_confirmed;
static bool s_hal_running,s_pwm_ready,pg[9],dma[9],sleep_high;
static bool mute_ok=true,tdm_ok=true;
static unsigned sequence,hal_stops;
typedef struct {void (*stop_aux_output)(void*);void *user;} audio_transport_client_t;
static const audio_transport_client_t *client;
static const audio_transport_client_t* audio_transport_client_get(void){return client;}
static bool wm8904_set_analog_output_mute_verified(unsigned bus,bool mute){assert(mute&&bus>=1&&bus<=2);++sequence;return mute_ok;}
static bool audio_transport_stop_route(int route){assert(route==1&&sequence==2);++sequence;return tdm_ok;}
static void nora_pwm_audio_stop(void){assert(!s_hal_running&&sequence==3);++hal_stops;pg[5]=dma[4]=sleep_high=false;}
static bool nora_dma_channel_enable(unsigned ch,bool on){assert(!s_pwm_ready&&!on);dma[ch]=on;return true;}
static bool nora_pwm_generator_enable(unsigned ch,bool on){assert(!s_pwm_ready&&!on);pg[ch]=on;return true;}
'''
    test = r'''
int main(void){
 audio_transport_client_t bound={classic_transport_stop_aux_output,NULL};
 for(unsigned fail=0;fail<4;fail++){
  client=&bound;mute_ok=!(fail&1);tdm_ok=!(fail&2);sequence=0;
  s_hal_running=s_pwm_ready=pg[5]=dma[4]=sleep_high=true;
  bool ok=audio_transport_stop_for_flash();
  assert(ok==(mute_ok&&tdm_ok)&&s_preflash_stop_latched);
  assert(!pg[5]&&!dma[4]&&!sleep_high);
  sequence=0;assert(audio_transport_stop_for_flash()==ok); // idempotent
 }
 assert(hal_stops==8);
 // No bound app, or app without auxiliary outputs: unchanged transport result.
 mute_ok=tdm_ok=true;client=NULL;sequence=0;assert(audio_transport_stop_for_flash());
 bound.stop_aux_output=NULL;client=&bound;sequence=0;assert(audio_transport_stop_for_flash());
 assert(hal_stops==8);
 puts("PASS *ts: producer/TDM stop precedes PWM, codec/transport failures still stop PWM, repeated stop, NULL/absent optional hook");
}
'''
    legacy_test = r'''
int main(void){
 s_pwm_ready=true;for(unsigned i=0;i<9;i++)pg[i]=dma[i]=true;
 classic_audio_pwm_stop();assert(!s_pwm_ready);
 assert(!dma[4]&&!dma[5]&&!pg[5]&&!pg[6]);
#ifdef PWM_AUDIO_ENABLE_SECONDARY_PAIR
 assert(!dma[6]&&!dma[7]&&!pg[7]&&!pg[8]);
#else
 assert(dma[6]&&dma[7]&&pg[7]&&pg[8]);
#endif
 assert(pg[4]&&dma[3]);classic_audio_pwm_stop();
 puts("PASS legacy *ts stop: selected PWM/DMA pairs off, producer blocked, unrelated generators retained");
}
'''
    env=os.environ.copy()
    env['PATH']=str(CC.parent)+os.pathsep+env.get('PATH','')
    with tempfile.TemporaryDirectory(prefix='pwm-preflash-') as temp:
        directory=Path(temp)
        cases=[('hal',mock+hal_stop+hook+stop+test),
               ('legacy',mock+legacy_stop+legacy_test),
               ('legacy_secondary','#define PWM_AUDIO_ENABLE_SECONDARY_PAIR 1\n'+mock+legacy_stop+legacy_test)]
        for name, text in cases:
            source,binary=directory/f'{name}.c',directory/f'{name}.exe'
            source.write_text(text,encoding='utf-8')
            subprocess.run([str(CC),'-std=c99','-O2','-Wall','-Wextra','-Werror',
                            '-Wno-unused-function','-Wno-unused-variable',str(source),'-o',str(binary)],check=True,env=env)
            subprocess.run([str(binary)],check=True,env=env)


if __name__=='__main__':
    main()
