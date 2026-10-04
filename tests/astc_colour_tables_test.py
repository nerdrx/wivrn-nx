#!/usr/bin/env python3
"""Stdlib ASTC colour-table checks. Optional: pass --decoder /path/to/decode_astc."""
import argparse
import math
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLES = ROOT / "server/shaders/astc_encode_colour_tables.glsl"


def array_body(source, name, glsl_type, count):
    match = re.search(
        rf"const {glsl_type} {name}\[{count}\]\s*=\s*{glsl_type}\[{count}\]\((.*?)\);",
        source,
        re.S,
    )
    assert match, f"missing GLSL array {name}[{count}]"
    return match.group(1)


def integers(body):
    return [int(x) for x in re.findall(r"\b(\d+)u\b", body)]


def vectors(body):
    rows = re.findall(r"uvec4\((\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\)", body)
    assert rows, "no packed uvec4 rows"
    return [[int(x) for x in row] for row in rows]


def interpolation_matrix(rows, grid_size):
    assert len(rows) == 64
    matrix = [[0.0] * grid_size for _ in rows]
    for y, row in enumerate(rows):
        for packed in row:
            weight, index = packed >> 5, packed & 31
            if weight:
                assert index < grid_size, f"row {y}: active coefficient indexes {index} >= {grid_size}"
                matrix[y][index] += weight / 16.0
        assert math.isclose(sum(matrix[y]), 1.0, abs_tol=1e-12), f"row {y}: coefficient sum != 16/16"
    return matrix


def check_tables():
    source = TABLES.read_text()
    inverse = [float(x) for x in re.findall(
        r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?",
        array_body(source, "astc4x4Inverse", "float", 256),
    )]
    assert len(inverse) == 256
    inv = [inverse[i * 16:(i + 1) * 16] for i in range(16)]
    interp4 = vectors(array_body(source, "astcInterp4", "uvec4", 64))
    interp5 = vectors(array_body(source, "astcInterp5", "uvec4", 64))
    a4 = interpolation_matrix(interp4, 16)
    interpolation_matrix(interp5, 25)

    # Verify the 4x4 least-squares inverse against the table-derived A^T A.
    gram = [[sum(row[i] * row[j] for row in a4) for j in range(16)] for i in range(16)]
    for i in range(16):
        for j in range(16):
            product = sum(inv[i][k] * gram[k][j] for k in range(16))
            assert math.isclose(product, float(i == j), rel_tol=1e-6, abs_tol=1e-6), (
                f"inverse4x4 * A^T A [{i},{j}] = {product}"
            )

    ep160 = integers(array_body(source, "ep160", "uint", 160))
    endpoint = integers(array_body(source, "endpoint6ToEp160", "uint", 64))
    assert len(ep160) == 160 and len(endpoint) == 64
    assert all(0 <= x <= 255 for x in ep160)
    for q, code in enumerate(endpoint):
        expanded = (q << 2) | (q >> 4)
        expected = min(range(160), key=lambda i: abs(ep160[i] - expanded))
        assert code == expected, f"q6 endpoint {q}: expected first nearest ep160 code {expected}, got {code}"

    weights = integers(array_body(source, "wt4", "uint", 4))
    assert weights == [0, 21, 43, 64]
    for value in range(65):
        expected = min(range(4), key=lambda i: abs(weights[i] - value))
        actual = 0 if value < 11 else 1 if value < 33 else 2 if value < 54 else 3
        assert actual == expected, f"weight {value}: expected code {expected}, got {actual}"

    quint = integers(array_body(source, "astcQuintEncode", "uint", 125))
    assert len(quint) == 125 and all(0 <= x <= 127 for x in quint)
    assert len(set(quint)) == 125, "quint encoding must map all 125 inputs to unique 7-bit codes"


def put(words, offset, count, value):
    value &= (1 << count) - 1
    bit = offset
    while count:
        word, shift = divmod(bit, 32)
        take = min(count, 32 - shift)
        words[word] |= (value & ((1 << take) - 1)) << shift
        value >>= take
        bit += take
        count -= take


def reverse32(value):
    for shift, mask in ((1, 0x55555555), (2, 0x33333333), (4, 0x0F0F0F0F), (8, 0x00FF00FF)):
        value = ((value >> shift) & mask) | ((value & mask) << shift)
    return ((value >> 16) | (value << 16)) & 0xFFFFFFFF


def pack_cem8(e0, e1, weight_code):
    header = [0x100F3, 0, 0, 0]
    offset = 17
    for channel in range(3):
        put(header, offset, 6, e0[channel]); offset += 6
        put(header, offset, 6, e1[channel]); offset += 6
    assert offset == 53
    weights = [0, 0, 0, 0]
    for i in range(25):
        put(weights, 3 * i, 3, weight_code)
    words = [header[0] | reverse32(weights[3]), header[1] | reverse32(weights[2]),
             header[2] | reverse32(weights[1]), header[3] | reverse32(weights[0])]
    block = b"".join(word.to_bytes(4, "little") for word in words)
    return bytes([0x13, 0xAB, 0xA1, 0x5C, 8, 8, 1, 8, 0, 0, 8, 0, 0, 1, 0, 0]) + block


def check_blue_contraction(decoder):
    def expand6(q):
        return (q << 2) | (q >> 4)

    # Same q6 endpoint sum, but expansion reverses the CEM8 blue-contraction order.
    raw0, raw1 = (32, 0, 0), (15, 15, 2)
    assert sum(raw0) == sum(raw1) == 32
    assert sum(map(expand6, raw0)) == 130 and sum(map(expand6, raw1)) == 128
    expected = tuple(map(expand6, raw0))

    def decode(data, directory):
        astc, rgba = directory / "fixture.astc", directory / "decoded.rgba"
        astc.write_bytes(data)
        subprocess.run([str(decoder), str(astc), str(rgba)], check=True, capture_output=True)
        pixels = rgba.read_bytes()
        assert len(pixels) == 8 * 8 * 4
        return [tuple(pixels[i:i + 3]) for i in range(0, len(pixels), 4)]

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        current_dir, fixed_dir = root / "current", root / "fixed"
        current_dir.mkdir(); fixed_dir.mkdir()
        current = decode(pack_cem8(raw0, raw1, 0), current_dir)
        corrected = decode(pack_cem8(raw1, raw0, 7), fixed_dir)
    assert current != corrected and any(pixel != expected for pixel in current)
    assert all(pixel == expected for pixel in corrected), (expected, sorted(set(corrected)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--decoder", type=Path, help="optional external ASTC decoder for the blue-contraction fixture")
    args = parser.parse_args()
    check_tables()
    if args.decoder:
        assert args.decoder.is_file(), f"decoder not found: {args.decoder}"
        check_blue_contraction(args.decoder)
        print("ASTC colour tables and external blue-contraction fixture: PASS")
    else:
        print("ASTC colour tables: PASS (external blue-contraction fixture skipped; pass --decoder to run it)")


if __name__ == "__main__":
    main()
