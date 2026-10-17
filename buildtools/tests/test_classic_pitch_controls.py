"""Exercise actual Classic cursor decoder and pitch/POT arbitration on the host."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
CC=Path(r'C:\msys64\usr\bin\gcc.exe')
os.environ['PATH']=str(CC.parent)+os.pathsep+os.environ.get('PATH','')
def body(text,name):
 start=text.index(name);a=text.index('{',start);depth=1;b=a+1
 while depth:
  depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[start:b]
def main():
 controls=(ROOT/'src/app/apps/classic/classic_controls.c').read_text()
 avas_key=body(controls,'void UsrOperate_avas_synth_key(')
 start=controls.index('/* ---------------------------------------------------------------------------\n * POT movement and cursor keys control the sounding AVAS engine pitch.')
 end=controls.index('#endif // defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)',start)
 controls=controls[start:end]
 hotkey=body((ROOT/'src/app/apps/classic/classic_console.c').read_text(),'sonora_hotkey_result_t sonora_app_handle_hotkey(')
 mock=r"""
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define ENA_AVAS_TYPE_TY_SYNTH
#define ENA_AVAS_TYPE_LB_SYNTH
#define APP_TARGET_AK506 3
#define APP_TARGET 3
#define NANO_POT_SPAN_COUNTS 2541u
#define NANO_POT_INVERT 1
#define AVAS_TYPE_TY_PITCH_LIMIT_CENT 200.0f
#define AVAS_TYPE_LB_PITCH_LIMIT_CENT 200.0f
#define AVAS_TYPE_TY_POT_TOP_CENT 200.0f
#define AVAS_TYPE_LB_POT_TOP_CENT 200.0f
static bool s_avas_type_ty_on=true,s_avas_type_lb_on=false,ready=true;
static uint16_t raw=2000;
static uint32_t now;
static float ty,lb;
static unsigned ty_updates,lb_updates,toggles,ty_calls,lb_calls;
static bool ty_tail,lb_tail;
static bool app_avas_type_ty_is_active(void){return s_avas_type_ty_on||ty_tail;}
static bool app_avas_type_lb_is_active(void){return s_avas_type_lb_on||lb_tail;}
static uint32_t GetTicks(void){return now;}
static bool POT_IsReady(void){return ready;}
static uint16_t POT_Read(void){return raw;}
static float clamp(float f){return f>200?200:(f < -200 ? -200:f);}
static void app_avas_type_ty_set_pitch_cent(float f){ty=clamp(f);++ty_updates;}
static void app_avas_type_lb_set_pitch_cent(float f){lb=clamp(f);++lb_updates;}
static float app_avas_type_ty_get_pitch_cent(void){return ty;}
static float app_avas_type_lb_get_pitch_cent(void){return lb;}
static float app_avas_type_ty_get_pitch_ratio(void){return 1;}
static float app_avas_type_lb_get_pitch_ratio(void){return 1;}
static void UsrOperate_avas_synth(void){++toggles;++ty_calls;s_avas_type_ty_on=!s_avas_type_ty_on;}
static void UsrOperate_avas_synth_type_lb(void){++toggles;++lb_calls;s_avas_type_lb_on=!s_avas_type_lb_on;}
static void UsrOperate_kinkon(void){}
static void UsrOperate_pinger(void){}
static void UsrOperate_Bmode(void){}
static void UsrOperate_surround(void){}
static void UsrOperate_avas_pitch_print(void);
typedef enum {SONORA_HOTKEY_IGNORED,SONORA_HOTKEY_HANDLED,SONORA_HOTKEY_HANDLED_FLUSH} sonora_hotkey_result_t;
"""
 test=r"""
static void keys(const char *s){while(*s)sonora_app_handle_hotkey(*s++);}
int main(void){
 keys("\033[A");assert(ty==10&&toggles==0);
 keys("\033OB");assert(ty==0&&toggles==0);
 keys("\033[1;5A");assert(ty==10&&toggles==0);
 keys("\033[C");assert(ty==10&&toggles==0);
 keys("\033[");now+=300;keys("A");assert(toggles==1); /* stale sequence */
 s_avas_type_ty_on=false;s_avas_type_lb_on=true;keys("\033[A");assert(lb==10&&ty==10);
 for(unsigned i=0;i<30;i++){keys("\033[A");} assert(lb==200);
 keys("\033[B");assert(lb==190);
 s_avas_type_ty_on=true;s_avas_type_lb_on=false;
 unsigned n=ty_updates;
 for(unsigned i=0;i<30;i++){now+=100;local_avas_pot_process();}
 assert(ty_updates==n); /* unmoved knob must not undo key pitch */
#if ENA_NANO_POT
 assert(local_avas_pot_to_cent(2541,200)==0);
 assert(local_avas_pot_to_cent(0,200)==200);
 assert(local_avas_pot_to_cent(4095,200)==0);
 raw=1000;for(unsigned i=0;i<40;i++){now+=100;local_avas_pot_process();}
 assert(ty_updates>n&&ty>110&&ty<130);
 keys("\033[A");float at_key=ty;n=ty_updates;
 for(unsigned i=0;i<30;i++){now+=100;local_avas_pot_process();}
 assert(ty==at_key&&ty_updates==n); /* filtered tail must not undo a key */
 ready=false;raw=0;for(unsigned i=0;i<30;i++){now+=100;local_avas_pot_process();}
 assert(ty_updates==n); /* failed ADC must not be interpreted as knob motion */
#endif
 s_avas_type_ty_on=false;s_avas_type_lb_on=false;
 unsigned t=ty_calls,l=lb_calls;
 keys("a");assert(s_avas_type_ty_on&&!s_avas_type_lb_on&&ty_calls==t+1&&lb_calls==l);
 keys("A");assert(!s_avas_type_ty_on&&!s_avas_type_lb_on&&ty_calls==t+2&&lb_calls==l);
 keys("A");assert(s_avas_type_lb_on&&!s_avas_type_ty_on&&lb_calls==l+1);
 keys("a");assert(!s_avas_type_lb_on&&!s_avas_type_ty_on&&lb_calls==l+2);
 keys("aa");assert(!s_avas_type_ty_on&&!s_avas_type_lb_on&&ty_calls==t+4);
 keys("AA");assert(!s_avas_type_ty_on&&!s_avas_type_lb_on&&lb_calls==l+4);
 ty_tail=true;keys("aA");assert(ty_calls==t+4&&lb_calls==l+4);ty_tail=false;
 lb_tail=true;keys("Aa");assert(ty_calls==t+4&&lb_calls==l+4);lb_tail=false;
 puts("PASS cursor CSI/SS3, pitch/POT arbitration, a/A start selection, either-key stop and fade tail");
}
"""
 for enabled in (0,1):
  with tempfile.TemporaryDirectory(prefix='classic-pitch-') as tmp:
   d=Path(tmp);(d/'t.c').write_text(f'#define ENA_NANO_POT {enabled}\n'+mock+controls+avas_key+hotkey+test)
   r=subprocess.run([str(CC),'-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(d/'t.c'),'-o',str(d/'t.exe')],capture_output=True,text=True)
   assert r.returncode==0,r.stderr
   r=subprocess.run([str(d/'t.exe')],capture_output=True,text=True)
   assert r.returncode==0,r.stdout+r.stderr
   print(f'POT enabled={enabled}: '+r.stdout.splitlines()[-1])
if __name__=='__main__':main()
