#!/usr/bin/env python3
"""
Decode an encoded mask string and print it to stdout.

The encoded format is: height,width,hex_data
where hex_data is the little-endian byte representation of the mask bitmap.
"""

import sys
import os
import re


def decode_mask(encoded: str) -> list[list[int]]:
    """Decode an encoded mask string into a 2D bitmap."""
    parts = encoded.split(",", 2)
    if len(parts) != 3:
        raise ValueError(
            "Invalid encoded mask format: expected 'height,width,hex_data'"
        )

    height = int(parts[0])
    width = int(parts[1])
    hex_data = parts[2]

    # trim non-hex characters (like newlines)
    hex_data = re.sub(r"[^0-9a-fA-F]", "", hex_data)

    # Convert hex string to bytes
    data_bytes = bytes.fromhex(hex_data)

    # Determine native word size (bytes per word) from the data
    # The C++ code uses sizeof(size_t) which is typically 4 or 8 bytes
    # We can infer it from the data size: x_size = ceil(width / (native_bytes * 8))
    # Total words = x_size * height, total bytes = words * native_bytes
    # Try 8 bytes first (64-bit), then 4 bytes (32-bit)
    native_bytes = 8
    native_bits = native_bytes * 8
    x_size = (width + native_bits - 1) // native_bits
    expected_bytes = x_size * height * native_bytes

    if len(data_bytes) != expected_bytes:
        # Try 4-byte words
        native_bytes = 4
        native_bits = native_bytes * 8
        x_size = (width + native_bits - 1) // native_bits
        expected_bytes = x_size * height * native_bytes

        if len(data_bytes) != expected_bytes:
            raise ValueError(
                f"Data size mismatch: got {len(data_bytes)} bytes, "
                f"expected {expected_bytes} for {height}x{width} mask"
            )

    # Decode the bitmap
    mask = [[0] * width for _ in range(height)]

    for y in range(height):
        for x in range(width):
            word_idx = y * x_size + (x // native_bits)
            bit_idx = x % native_bits

            # Read the word (little-endian)
            byte_offset = word_idx * native_bytes
            word = int.from_bytes(
                data_bytes[byte_offset : byte_offset + native_bytes],
                byteorder="little",
            )

            # Check the bit
            if (word >> bit_idx) & 1:
                mask[y][x] = 1

    return mask


def print_mask(mask: list[list[int]], use_unicode: bool = True) -> None:
    """Print the mask to stdout."""
    if use_unicode:
        chars = {0: "░", 1: "█"}
    else:
        chars = {0: ".", 1: "#"}

    for row in mask:
        print("".join(chars[bit] for bit in row))


def main() -> int:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <encoded_mask_or_file> [--ascii]", file=sys.stderr)
        print(
            "  encoded_mask_or_file: height,width,hex_data OR path to file", file=sys.stderr
        )
        print("  --ascii: use ASCII characters instead of Unicode", file=sys.stderr)
        return 1

    arg = sys.argv[1]
    use_unicode = "--ascii" not in sys.argv

    if os.path.isfile(arg):
        try:
            with open(arg, "r") as f:
                encoded = f.read().strip()
        except OSError as e:
            print(f"Error reading file '{arg}': {e}", file=sys.stderr)
            return 1
    else:
        encoded = arg

    try:
        mask = decode_mask(encoded)
        print_mask(mask, use_unicode)
        return 0
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
