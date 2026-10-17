#!/usr/bin/env python3
"""Put the two CPU models and the hardware side by side, without pretending they
are the same quantity.

WHY THIS IS NOT A CALIBRATION SCRIPT.  The planner's `cpu_pct_16ch` is the cost of
the FRONT-END FIR chain only: MACs/s/channel x 16 channels x cycles/MAC / clock.  A
pair whose chain is "(no front end)" reads 0.00 % and that is correct -- it has no
front end.  The board's DSPload is the WHOLE image: both resampler engines, the TDM
push/pull, the LED meter, the console.  Dividing one by the other is not a
cycles/MAC measurement, it is a category error, and it is exactly the kind of
"calibration factor" this study has already had to withdraw once.

So this script reports three columns and refuses to divide them:

  cpu_model_1p90   conservative COMPOSED-chain model (1.90 cyc/MAC).  Provenance:
                   the shipping composed 96 -> 32 front end measured 1.75 and 2.09
                   cyc/MAC on its two stages, so 1.90 already carries composition
                   overhead -- ring push, intermediate buffers, phase schedule,
                   s24-left scatter.
  cpu_model_1p42   measured SINGLE-STAGE anchor (1.42 cyc/MAC, study SS 16: 1:1 LPF,
                   140 us, 28.0 %, calibration factor 0.75).  Applying 1.90 to a
                   single-stage chain charges overhead that chain does not pay.
  hardware         what the board actually did, per pair, with the chain it actually
                   ran -- which is NOT always the chain the planner chose.

The last point is the one that bites.  The image's shipping front-end table only
covers 96 kHz and 48 kHz sources, so at 32 kHz or 44.1 kHz in it runs the rear
resampler DIRECT no matter what the planner would prefer.  A hardware row therefore
qualifies the pair's HARD REAL TIME and its rear spur; it does not qualify the
planner's chain unless hw_chain matches chain_model.  The column says which.

Usage:
  python tools/asrc/asrc_cpu_model_vs_hardware.py \
      --model-1p90 <path to [internal] requalify_45pair_collision_L256_heldchains.csv> \
      --model-1p42 <the same run at --cyc-per-mac 1.42> \
      --hardware   <hardware observations JSON> \
      --csv out.csv
"""
import argparse
import csv
import json
import pathlib
import sys

COLUMNS = [
    'fs_in_hz', 'fs_out_hz', 'rear_step',
    'chain_model', 'macs_per_sec', 'taps',
    'cpu_model_1p90_pct', 'cpu_model_1p42_pct', 'cpu_budget_1p90', 'cpu_budget_1p42',
    'overall_dbc', 'verdict', 'limiting_mechanism',
    'hw_measured', 'hw_chain', 'hw_chain_is_model_chain',
    'hw_rt_status', 'hw_dsp_a_pct', 'hw_dsp_b_pct', 'hw_load_max_pct',
    'hw_max_demand_pct', 'hw_miss', 'hw_starve', 'hw_drop', 'hw_dwell_s',
    'hw_note',
]


def _same(hw_chain, model_chain):
    """'direct' on the board and '(no front end)' in the planner are the same thing:
    the rear resampler alone.  Everything else must match textually."""
    norm = {'direct': '', '(no front end)': ''}
    return norm.get(hw_chain, hw_chain) == norm.get(model_chain, model_chain)


def load_model(path):
    with pathlib.Path(path).open(newline='', encoding='utf-8') as fh:
        return {(r['fs_in_hz'], r['fs_out_hz']): r for r in csv.DictReader(fh)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--model-1p90', required=True)
    ap.add_argument('--model-1p42', required=True)
    ap.add_argument('--hardware', required=True,
                    help='JSON: list of {fs_in_hz, fs_out_hz, chain, rt_status, '
                         'dsp_a_pct, dsp_b_pct, load_max_pct, max_demand_pct, '
                         'miss, starve, drop, dwell_s, note}')
    ap.add_argument('--csv', required=True)
    args = ap.parse_args()

    m90 = load_model(args.model_1p90)
    m42 = load_model(args.model_1p42)
    with pathlib.Path(args.hardware).open(encoding='utf-8') as fh:
        hw = {(str(h['fs_in_hz']), str(h['fs_out_hz'])): h for h in json.load(fh)}

    rows = []
    for key in sorted(m90, key=lambda k: (int(k[0]), int(k[1]))):
        a, b = m90[key], m42.get(key, {})
        h = hw.get(key)
        chain_model = a['chain']
        row = {
            'fs_in_hz': key[0], 'fs_out_hz': key[1], 'rear_step': a['rear_step'],
            'chain_model': chain_model, 'macs_per_sec': a['macs_per_sec'],
            'taps': a['taps'],
            'cpu_model_1p90_pct': a['cpu_pct_16ch'],
            'cpu_model_1p42_pct': b.get('cpu_pct_16ch', ''),
            'cpu_budget_1p90': a['cpu_budget'], 'cpu_budget_1p42': b.get('cpu_budget', ''),
            'overall_dbc': a['overall_dbc'], 'verdict': a['verdict'],
            'limiting_mechanism': a['limiting_mechanism'],
            'hw_measured': 'yes' if h else 'no',
            'hw_chain': h['chain'] if h else '',
            'hw_chain_is_model_chain': ('yes' if h and _same(h['chain'], chain_model)
                                        else ('no' if h else '')),
            'hw_rt_status': h['rt_status'] if h else '',
            'hw_dsp_a_pct': h.get('dsp_a_pct', '') if h else '',
            'hw_dsp_b_pct': h.get('dsp_b_pct', '') if h else '',
            'hw_load_max_pct': h.get('load_max_pct', '') if h else '',
            'hw_max_demand_pct': h.get('max_demand_pct', '') if h else '',
            'hw_miss': h.get('miss', '') if h else '',
            'hw_starve': h.get('starve', '') if h else '',
            'hw_drop': h.get('drop', '') if h else '',
            'hw_dwell_s': h.get('dwell_s', '') if h else '',
            'hw_note': h.get('note', '') if h else '',
        }
        rows.append(row)

    with pathlib.Path(args.csv).open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)

    measured = [r for r in rows if r['hw_measured'] == 'yes']
    print('%d pairs, %d with a hardware row' % (len(rows), len(measured)))
    print()
    print('%-16s %-34s %8s %8s   %-9s %-8s %-9s %s'
          % ('pair', 'chain (model)', '@1.90', '@1.42', 'hw chain', 'hw RT',
             'hw max', 'model chain ran?'))
    for r in measured:
        print('%-16s %-34s %8s %8s   %-9s %-8s %8s%%  %s'
              % ('%s -> %s' % (r['fs_in_hz'], r['fs_out_hz']),
                 r['chain_model'][:34],
                 r['cpu_model_1p90_pct'], r['cpu_model_1p42_pct'],
                 r['hw_chain'], r['hw_rt_status'], r['hw_load_max_pct'],
                 r['hw_chain_is_model_chain']))
    print()
    print('The model columns are FRONT-END cost only; hw max is the whole image.')
    print('They are not the same quantity -- do not divide them.')
    print('csv -> %s' % args.csv)
    return 0


if __name__ == '__main__':
    sys.exit(main())
