"""Use the firmware compiler to verify PWM defaults, pin selection and conflicts."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, default=Path(os.environ.get('ProgramFiles', '/opt'))/'Microchip/xc-dsc/v3.31.01/bin/xc-dsc-gcc.exe')
    parser.add_argument('--dfp', type=Path, default=Path.home()/'.mchp_packs/Microchip/dsPIC33AK-MP_DFP/1.5.269/xc16')
    parser.add_argument('--ak128-dfp', type=Path, default=Path.home()/'.mchp_packs/Microchip/dsPIC33AK-MC_DFP/1.6.272/xc16')
    args = parser.parse_args()
    # Each successful case checks resolved values, not just preprocessing success.
    cases = [
        ('classic1_default', [], {'APP_USE_PWM_AUDIO': 1, 'APP_PWM_AUDIO_HAL': 1, 'MPQ6612A_ENABLE_MIKROB': 0, 'MPQ6612A_ENABLE_XPRO1': 1, 'MPQ6612A_STEREO': 0, 'APP_USE_SPI2_AUDIO': 0, 'MPQ6612A_IN1_RP': 21, 'MPQ6612A_IN2_RP': 18}, None),
        ('codec_b_xpro', ['APP_REQ_MIKROB_WM8904=1'], {'APP_USE_SPI2_AUDIO': 1, 'MPQ6612A_ENABLE_MIKROB': 0, 'MPQ6612A_IN1_RP': 21}, None),
        ('codec_b_pwm_off', ['APP_REQ_MIKROB_WM8904=1', 'MPQ6612A_ENABLE_XPRO1=0'], {'APP_USE_SPI2_AUDIO': 1, 'APP_USE_PWM_AUDIO': 0}, None),
        ('stereo', ['MPQ6612A_ENABLE_MIKROB=1'], {'MPQ6612A_STEREO': 1, 'MPQ6612A_IN1_RP': 98, 'MPQ6612A_RIGHT_IN1_RP': 21}, None),
        ('old_stereo_recipe', ['MPQ6612A_STEREO=1'], {'MPQ6612A_ENABLE_MIKROB': 1, 'MPQ6612A_ENABLE_XPRO1': 1, 'APP_PWM_AUDIO_HAL': 1}, None),
        ('b_only_direct', ['MPQ6612A_ENABLE_MIKROB=1', 'MPQ6612A_ENABLE_XPRO1=0', 'APP_PWM_AUDIO_HAL=0'], {'MPQ6612A_STEREO': 0, 'MPQ6612A_IN2_RP': 29, 'APP_PWM_AUDIO_HAL': 0}, None),
        ('legacy', ['APP_PWM_OUTPUT=0', 'APP_USE_PWM_AUDIO=1'], {'MPQ6612A_ENABLE_XPRO1': 0, 'MPQ6612A_ENABLE_MIKROB': 0, 'APP_PWM_AUDIO_HAL': 0}, None),
        ('classic2', ['APP_BUILD=APP_BUILD_STD_DEMO_2'], {'APP_PWM_OUTPUT': 0, 'APP_USE_PWM_AUDIO': 0, 'APP_USE_SPI2_AUDIO': 1}, None),
        ('classic_drc', ['APP_BUILD=APP_BUILD_DRC_DEMO'], {'APP_PWM_OUTPUT': 0, 'APP_USE_PWM_AUDIO': 0, 'APP_USE_SPI2_AUDIO': 1}, None),
        ('classic_usb48', ['APP_BUILD=APP_BUILD_USB_48'], {'APP_PWM_OUTPUT': 0, 'MPQ6612A_ENABLE_XPRO1': 0, 'APP_USE_USB_AUDIO_IN': 1}, None),
        ('classic_96k', ['APP_BUILD=APP_BUILD_DEMO_96K'], {'APP_PWM_OUTPUT': 0, 'MPQ6612A_ENABLE_XPRO1': 0, 'SAMPLE_RATE': 96000}, None),
        ('asrc', ['APP_BUILD=APP_BUILD_ASRC_CODEC_BIDIR'], {'APP_PWM_OUTPUT': 0, 'APP_USE_PWM_AUDIO': 0, 'APP_USE_SPI2_AUDIO': 1}, None),
        ('ak128', [], {'APP_PWM_OUTPUT': 0, 'APP_USE_PWM_AUDIO': 0, 'MPQ6612A_ENABLE_XPRO1': 0}, None),
        ('codec_b_and_b_pwm', ['APP_REQ_MIKROB_WM8904=1', 'MPQ6612A_ENABLE_MIKROB=1'], {}, 'MikroBUS B conflict'),
        ('codec_b_and_b_pwm_no_xpro', ['APP_REQ_MIKROB_WM8904=1', 'MPQ6612A_ENABLE_MIKROB=1', 'MPQ6612A_ENABLE_XPRO1=0'], {}, 'MikroBUS B conflict'),
        ('usb_xpro_conflict', ['ENA_USB_AUDIO_IN'], {}, 'XPRO1 PWM conflicts with USB'),
        ('direct_xpro_rejected', ['APP_PWM_AUDIO_HAL=0'], {}, 'XPRO1-only PWM requires'),
        ('invalid_route', ['MPQ6612A_ENABLE_MIKROB=2'], {}, 'route enables must be 0 or 1'),
        ('pwm_without_route', ['MPQ6612A_ENABLE_XPRO1=0', 'APP_USE_PWM_AUDIO=1'], {}, 'requires at least one enabled output route'),
        ('scope_without_output', ['MPQ6612A_ENABLE_XPRO1=0', 'PWM_AUDIO_SCOPE_ONLY=1'], {}, 'scope-only requires an enabled PWM output'),
        ('route_with_pwm_disabled', ['APP_USE_PWM_AUDIO=0'], {}, 'Disable MPQ6612A route enables'),
        ('inconsistent_stereo', ['MPQ6612A_STEREO=1', 'MPQ6612A_ENABLE_MIKROB=0'], {}, 'STEREO must match'),
        ('legacy_codec_b_conflict', ['APP_PWM_OUTPUT=0', 'APP_USE_PWM_AUDIO=1', 'APP_REQ_MIKROB_WM8904=1'], {}, 'Legacy MIC4607 PWM conflicts'),
    ]
    with tempfile.TemporaryDirectory(prefix='pwm-config-') as temp:
        source = Path(temp)/'config.c'
        for name, defines, expected, error in cases:
            checks = ''.join(f'\n#if ({key}) != ({value})\n#error "unexpected {key}"\n#endif\n' for key, value in expected.items())
            source.write_text('#include "app_specific_config_defs.h"\n#include "board/audio/mpq6612a_board.h"\n'+checks)
            device = '33AK128MC106' if name == 'ak128' else '33AK512MPS512'
            dfp = args.ak128_dfp if name == 'ak128' else args.dfp
            cmd = [str(args.cc), '-E', '-P', f'-mcpu={device}', f'-mdfp={dfp}', '-I'+str(ROOT/'src/app'), '-x', 'c', *['-D'+d for d in defines], str(source)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if error:
                assert result.returncode != 0 and error in result.stderr, (name, result.stderr)
            else:
                assert result.returncode == 0, (name, result.stderr)
            print(f'PASS {name}: '+('rejected as intended' if error else 'resolved defaults and pins'))


if __name__ == '__main__':
    main()
