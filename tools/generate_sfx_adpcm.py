#!/usr/bin/env python3
"""Generate the AK128 button-sound IMA-ADPCM assets.

The canonical source remains ``src/app/apps/classic/tone_data_int16.c``. Each
sound is encoded as independently decodable blocks so firmware can restart or
seek without scanning the whole stream:

    int16_le predictor, uint8 step_index, uint8 reserved, low-nibble-first data

The predictor is the first decoded sample in the block. Every following sample
uses one standard IMA-ADPCM nibble. All blocks except the final block contain
``BLOCK_SAMPLES`` decoded samples and therefore have a fixed byte size.

Each asset also carries its stored sample rate (Hz), read out of the
``Button_Tone_i16`` initializer in the same source file (``Tone_*_rate``), so
the AK128 decoder can SRC-convert per tone exactly like the AK512 SST26 path
does -- sonora-dev stores button clicks and the notification at different
native rates (bandwidth-appropriate, not all 48 kHz), and this generator must
never let the ADPCM table drift from that.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path


BLOCK_SAMPLES = 256
BLOCK_HEADER_BYTES = 4
FULL_BLOCK_BYTES = BLOCK_HEADER_BYTES + ((BLOCK_SAMPLES - 1 + 1) // 2)

STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
)

INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8)

# (label, PCM array symbol, generated ADPCM symbol, Button_Tone_i16 rate field)
ASSETS = (
    ("ON", "Tone_ON_i16", "s_sfx_adpcm_on", "Tone_ON_rate"),
    ("OFF", "Tone_OFF_i16", "s_sfx_adpcm_off", "Tone_OFF_rate"),
    ("NOTIF", "Tone_Notif_i16", "s_sfx_adpcm_notif", "Tone_Notif_rate"),
)


@dataclass(frozen=True)
class EncodedAsset:
    label: str
    c_symbol: str
    rate_Hz: int
    samples: tuple[int, ...]
    encoded: bytes
    decoded: tuple[int, ...]
    block_count: int


def clamp16(value: int) -> int:
    return max(-32768, min(32767, value))


def encode_nibble(sample: int, predictor: int, step_index: int) -> tuple[int, int, int]:
    step = STEP_TABLE[step_index]
    diff = sample - predictor
    code = 0
    if diff < 0:
        code = 8
        diff = -diff

    delta = step >> 3
    if diff >= step:
        code |= 4
        diff -= step
        delta += step
    if diff >= (step >> 1):
        code |= 2
        diff -= step >> 1
        delta += step >> 1
    if diff >= (step >> 2):
        code |= 1
        delta += step >> 2

    predictor = clamp16(predictor - delta if (code & 8) else predictor + delta)
    step_index = max(0, min(88, step_index + INDEX_TABLE[code & 7]))
    return code, predictor, step_index


def decode_nibble(code: int, predictor: int, step_index: int) -> tuple[int, int]:
    step = STEP_TABLE[step_index]
    delta = step >> 3
    if code & 4:
        delta += step
    if code & 2:
        delta += step >> 1
    if code & 1:
        delta += step >> 2
    predictor = clamp16(predictor - delta if (code & 8) else predictor + delta)
    step_index = max(0, min(88, step_index + INDEX_TABLE[code & 7]))
    return predictor, step_index


def encode_block(samples: tuple[int, ...]) -> tuple[bytes, tuple[int, ...]]:
    if not samples:
        raise ValueError("cannot encode an empty block")

    best: tuple[int, int, bytes, tuple[int, ...]] | None = None
    for initial_index in range(89):
        predictor = samples[0]
        step_index = initial_index
        decoded = [predictor]
        nibbles: list[int] = []
        squared_error = 0

        for sample in samples[1:]:
            code, predictor, step_index = encode_nibble(sample, predictor, step_index)
            nibbles.append(code)
            decoded.append(predictor)
            error = sample - predictor
            squared_error += error * error

        payload = bytearray()
        for pos in range(0, len(nibbles), 2):
            low = nibbles[pos]
            high = nibbles[pos + 1] if (pos + 1) < len(nibbles) else 0
            payload.append(low | (high << 4))

        candidate = (
            squared_error,
            initial_index,
            struct.pack("<hBB", samples[0], initial_index, 0) + bytes(payload),
            tuple(decoded),
        )
        if best is None or candidate[:2] < best[:2]:
            best = candidate

    assert best is not None
    return best[2], best[3]


def decode_block(encoded: bytes, sample_count: int) -> tuple[int, ...]:
    if sample_count <= 0 or len(encoded) < BLOCK_HEADER_BYTES:
        raise ValueError("invalid ADPCM block")
    predictor, step_index, reserved = struct.unpack_from("<hBB", encoded)
    if step_index > 88 or reserved != 0:
        raise ValueError("invalid ADPCM block header")

    decoded = [predictor]
    nibble_index = 0
    while len(decoded) < sample_count:
        byte = encoded[BLOCK_HEADER_BYTES + (nibble_index >> 1)]
        code = (byte >> 4) & 0x0F if (nibble_index & 1) else byte & 0x0F
        predictor, step_index = decode_nibble(code, predictor, step_index)
        decoded.append(predictor)
        nibble_index += 1
    return tuple(decoded)


def extract_samples(source: str, symbol: str) -> tuple[int, ...]:
    pattern = re.compile(
        rf"const\s+int16_t\s+{re.escape(symbol)}\s*\[\s*\]\s*=\s*\{{(?P<body>.*?)\}};",
        re.DOTALL,
    )
    match = pattern.search(source)
    if match is None:
        raise ValueError(f"array not found: {symbol}")
    samples = tuple(int(token) for token in re.findall(r"[-+]?\d+", match.group("body")))
    if not samples:
        raise ValueError(f"array is empty: {symbol}")
    if any(sample < -32768 or sample > 32767 for sample in samples):
        raise ValueError(f"array contains a non-int16 value: {symbol}")
    return samples


def extract_rate_Hz(source: str, rate_field: str) -> int:
    # e.g. ".Tone_ON_rate       = 12000u,"  inside the Button_Tone_i16 initializer.
    pattern = re.compile(rf"\.{re.escape(rate_field)}\s*=\s*(\d+)u?\s*,")
    match = pattern.search(source)
    if match is None:
        raise ValueError(f"rate field not found in Button_Tone_i16 initializer: {rate_field}")
    return int(match.group(1))


def encode_asset(label: str, c_symbol: str, rate_Hz: int, samples: tuple[int, ...]) -> EncodedAsset:
    encoded_parts: list[bytes] = []
    decoded_parts: list[int] = []
    for start in range(0, len(samples), BLOCK_SAMPLES):
        block_samples = samples[start : start + BLOCK_SAMPLES]
        block, decoded = encode_block(block_samples)
        expected_bytes = BLOCK_HEADER_BYTES + ((len(block_samples) - 1 + 1) // 2)
        if len(block) != expected_bytes:
            raise AssertionError("encoded block size mismatch")
        if decode_block(block, len(block_samples)) != decoded:
            raise AssertionError("firmware-format decode mismatch")
        encoded_parts.append(block)
        decoded_parts.extend(decoded)

    encoded = b"".join(encoded_parts)
    decoded_tuple = tuple(decoded_parts)
    if len(decoded_tuple) != len(samples):
        raise AssertionError("decoded asset length mismatch")
    return EncodedAsset(
        label=label,
        c_symbol=c_symbol,
        rate_Hz=rate_Hz,
        samples=samples,
        encoded=encoded,
        decoded=decoded_tuple,
        block_count=len(encoded_parts),
    )


def quality(asset: EncodedAsset) -> tuple[float, float, int]:
    noise = sum((a - b) * (a - b) for a, b in zip(asset.samples, asset.decoded))
    signal = sum(a * a for a in asset.samples)
    rmse = math.sqrt(noise / len(asset.samples))
    snr = math.inf if noise == 0 else 10.0 * math.log10(signal / noise)
    peak_error = max(abs(a - b) for a, b in zip(asset.samples, asset.decoded))
    return snr, rmse, peak_error


def format_bytes(data: bytes) -> str:
    rows = []
    for start in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{byte:02X}" for byte in data[start : start + 16]) + ",")
    return "\n".join(rows)


def render_header(assets: tuple[EncodedAsset, ...], source_hash: str) -> str:
    total_encoded = sum(len(asset.encoded) for asset in assets)
    total_samples = sum(len(asset.samples) for asset in assets)
    lines = [
        "// Generated by tools/generate_sfx_adpcm.py; DO NOT EDIT BY HAND.",
        "// Canonical PCM source: src/app/apps/classic/tone_data_int16.c",
        f"// Source SHA-256: {source_hash}",
        "// Format per block: int16_le predictor, uint8 step-index, uint8 reserved,",
        "//                   then low-nibble-first standard IMA-ADPCM codes.",
        f"// Total: {total_samples} decoded samples, {total_encoded} encoded bytes.",
        "",
        "#ifndef TONE_DATA_IMA_ADPCM_H",
        "#define TONE_DATA_IMA_ADPCM_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define SND_EFFECT_ADPCM_BLOCK_SAMPLES      ({BLOCK_SAMPLES}u)",
        f"#define SND_EFFECT_ADPCM_BLOCK_HEADER_BYTES ({BLOCK_HEADER_BYTES}u)",
        f"#define SND_EFFECT_ADPCM_FULL_BLOCK_BYTES   ({FULL_BLOCK_BYTES}u)",
        "",
        "typedef struct snd_effect_adpcm_asset",
        "{",
        "    const uint8_t *data;",
        "    uint32_t       encoded_size;",
        "    uint32_t       sample_count;",
        "    uint32_t       rate_Hz;       // stored sample rate of this tone",
        "    uint16_t       block_count;",
        "    uint16_t       reserved;",
        "} snd_effect_adpcm_asset_t;",
        "",
    ]

    for asset in assets:
        snr, rmse, peak_error = quality(asset)
        lines.extend(
            [
                f"// {asset.label}: {len(asset.samples)} samples @ {asset.rate_Hz} Hz, {len(asset.encoded)} bytes,",
                f"// {asset.block_count} blocks, SNR={snr:.2f} dB, RMSE={rmse:.2f}, peak error={peak_error}.",
                f"// Encoded CRC32: 0x{zlib.crc32(asset.encoded) & 0xFFFFFFFF:08X}",
                f"static const uint8_t {asset.c_symbol}[] __attribute__((aligned(4))) =",
                "{",
                format_bytes(asset.encoded),
                "};",
                "",
            ]
        )

    lines.extend(
        [
            "static const snd_effect_adpcm_asset_t Snd_Effect_Adpcm_Assets[] =",
            "{",
        ]
    )
    for asset in assets:
        lines.append(
            f"    {{ {asset.c_symbol}, (uint32_t)sizeof({asset.c_symbol}), {len(asset.samples)}u, "
            f"{asset.rate_Hz}u, {asset.block_count}u, 0u }},"
        )
    lines.extend(
        [
            "};",
            "",
            f"#define SND_EFFECT_ADPCM_ASSET_COUNT ({len(assets)}u)",
            "",
            "#endif // TONE_DATA_IMA_ADPCM_H",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the checked-in header is stale")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    source_path = root / "src" / "app" / "apps" / "classic" / "tone_data_int16.c"
    output_path = root / "src" / "app" / "apps" / "classic" / "tone_data_ima_adpcm.h"
    source_bytes = source_path.read_bytes()
    source = source_bytes.decode("utf-8")

    encoded_assets = tuple(
        encode_asset(label, c_symbol, extract_rate_Hz(source, rate_field), extract_samples(source, pcm_symbol))
        for label, pcm_symbol, c_symbol, rate_field in ASSETS
    )
    generated = render_header(encoded_assets, hashlib.sha256(source_bytes).hexdigest())

    for asset in encoded_assets:
        snr, rmse, peak_error = quality(asset)
        print(
            f"{asset.label:5s}: rate={asset.rate_Hz:6d}Hz samples={len(asset.samples):5d} bytes={len(asset.encoded):5d} "
            f"blocks={asset.block_count:3d} SNR={snr:6.2f}dB RMSE={rmse:7.2f} peak={peak_error}"
        )
    print(f"TOTAL: {sum(len(asset.encoded) for asset in encoded_assets)} bytes")

    if args.check:
        try:
            current = output_path.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"missing generated file: {output_path}", file=sys.stderr)
            return 1
        if current != generated:
            print(f"stale generated file: {output_path}", file=sys.stderr)
            return 1
        print("generated header is up to date")
        return 0

    output_path.write_text(generated, encoding="utf-8", newline="\n")
    print(f"wrote {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
