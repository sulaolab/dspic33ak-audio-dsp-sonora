"""Test the actual HAL float input adapter with the firmware's fast-math flag."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    text = (ROOT / 'src/app/apps/shared/float_conversion.c').read_text(encoding='utf-8-sig')
    start = text.index('void convert_float_to_pwm_q31(')
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    function = text[start:end]
    harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static float Post_Gain_PWM;
FUNCTION
int main(void) {
    const uint32_t words[] = {0xff800000u, 0xbf800000u, 0xbf000000u,
        0x00000000u, 0x80000000u, 0x3f000000u, 0x3f800000u,
        0x7f800000u, 0x7fc00001u, 0xffc00001u};
    const int32_t expected[] = {INT32_MIN, INT32_MIN, -1073741824,
        0, 0, 1073741824, INT32_MAX, INT32_MAX, 0, 0};
    float input[20]; int32_t result[10];
    for (unsigned i=0;i<10;++i) {
        input[i] = 0.25f; /* other planar channel must not leak */
        memcpy(&input[10+i], &words[i], sizeof(float));
    }
    Post_Gain_PWM=1.0f;
    convert_float_to_pwm_q31(input,1,result,10);
    for (unsigned i=0;i<10;++i) assert(result[i]==expected[i]);
    Post_Gain_PWM=0.5f;
    convert_float_to_pwm_q31(input,0,result,10);
    for (unsigned i=0;i<10;++i) assert(result[i]==268435456);
    input[0]=2.0f; Post_Gain_PWM=0.25f;
    convert_float_to_pwm_q31(input,0,result,1);
    assert(result[0]==1073741824); /* gain precedes clipping */
    input[0]=-2.0f;
    convert_float_to_pwm_q31(input,0,result,1);
    assert(result[0]==-1073741824);
    input[0]=0.25f; Post_Gain_PWM=1.9952623f; // +6 dB, only the PWM copy
    convert_float_to_pwm_q31(input,0,result,1);
    assert(result[0]>1071197000 && result[0]<1071199000);
    assert(input[0]==0.25f); // codec's source buffer must not be amplified in-place
    input[0]=0.125f; Post_Gain_PWM=3.9810717f; // +12 dB
    convert_float_to_pwm_q31(input,0,result,1);
    assert(result[0]>1068659000 && result[0]<1068663000);
    assert(input[0]==0.125f);
    input[0]=0.3f;
    convert_float_to_pwm_q31(input,0,result,1);assert(result[0]==INT32_MAX);
    input[0]=-0.3f;
    convert_float_to_pwm_q31(input,0,result,1);assert(result[0]==INT32_MIN);
    input[0]=0.75f;
    convert_float_to_pwm_q31(input,0,result,1);assert(result[0]==INT32_MAX);
    input[0]=-0.75f;
    convert_float_to_pwm_q31(input,0,result,1);assert(result[0]==INT32_MIN);
    puts("PASS HAL input: planar channel, gain before clipping, Q31 endpoints, NaN/Inf with fast-math");
}
'''.replace('FUNCTION', function)
    cc = Path(r'C:\msys64\usr\bin\gcc.exe')
    env = os.environ.copy()
    env['PATH'] = str(cc.parent) + os.pathsep + env['PATH']
    with tempfile.TemporaryDirectory(prefix='pwm-q31-input-') as temp:
        source, binary = Path(temp) / 'test.c', Path(temp) / 'test.exe'
        source.write_text(harness)
        subprocess.run([str(cc), '-std=c99', '-O3', '-ffast-math', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True, env=env)
        subprocess.run([str(binary)], check=True, env=env)


if __name__ == '__main__':
    main()
